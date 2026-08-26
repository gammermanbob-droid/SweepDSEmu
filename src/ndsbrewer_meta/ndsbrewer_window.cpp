// src/ndsbrewer_meta/ndsbrewer_window.cpp

#include "ndsbrewer_meta/ndsbrewer_window.h"

#include <cryptopp/sha.h>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "citra_qt/ds_forwarder_registry.h"
#include "citra_qt/util/nds_icon.h"
#include "common/file_util.h"
#include "core/hle/service/am/am.h"
#include "core/hw/ds_forwarder_stub.h"
#include "ndsbrewer_meta/martini_progress_widget.h"

namespace {

constexpr int kIconSize = 48; // SMDH icon size bannertool expects.

QString ForwarderToolsDir() {
#ifdef Q_OS_MAC
    // Matches the CMakeLists.txt install location: Contents/Resources/,
    // not Contents/MacOS/ (see the comment there for why codesign needs
    // this split). applicationDirPath() is Contents/MacOS/, so this
    // steps up one level to Contents/ first.
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/../Resources/ds_forwarder_tools/");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/ds_forwarder_tools/");
#endif
}

QString ProfileRomDir(const QString& subfolder) {
    return QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::UserDir)) +
           QStringLiteral("sdmc/roms/") + subfolder;
}

// Deterministic identity derived from the ROM's own absolute path, so
// re-running NDSBrewer on the same ROM reuses the same title ID instead
// of registering a duplicate -- matches
// tools/make_ds_forwarder.py's identical derivation exactly (same SHA-256
// input, same 16-bit truncation/zero guard, same product-code format),
// so a forwarder built by either tool is replaceable by the other.
struct ForwarderIdentity {
    uint64_t program_id;
    uint16_t unique_id;
    QString product_code;
};

ForwarderIdentity ComputeIdentity(const QString& absolute_rom_path) {
    const QByteArray path_utf8 = absolute_rom_path.toUtf8();

    CryptoPP::SHA256 hasher;
    unsigned char digest[CryptoPP::SHA256::DIGESTSIZE];
    hasher.CalculateDigest(digest, reinterpret_cast<const unsigned char*>(path_utf8.constData()),
                           static_cast<size_t>(path_utf8.size()));

    uint16_t unique_id = static_cast<uint16_t>((digest[0] << 8) | digest[1]);
    if (unique_id == 0) {
        unique_id = 1;
    }

    ForwarderIdentity identity;
    identity.unique_id = unique_id;
    identity.product_code = QStringLiteral("DSF%1").arg(digest[0], 2, 16, QLatin1Char('0')).toUpper();
    // makerom builds the real title ID's low 32 bits as (UniqueId << 8) |
    // variation (variation 0x00 for a plain application) -- the registry
    // has to record that same combined value, since that's what
    // ReadProgramId() actually reports back for the installed CIA.
    identity.program_id = 0x0004000000000000ULL | (static_cast<uint64_t>(unique_id) << 8);
    return identity;
}

QString SafeFileName(const QString& title) {
    QString safe;
    for (const QChar& c : title) {
        safe += (c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-') ||
                 c == QLatin1Char('_'))
                    ? c
                    : QLatin1Char('_');
    }
    return safe.trimmed();
}

// A zero-length WAV data chunk is technically valid, but bannertool's
// banner audio is meant to loop for as long as an icon stays highlighted
// on the HOME Menu -- a real (if short) run of true-zero 16-bit PCM
// samples is unambiguous silence either way a decoder handles an empty
// one. Mirrors tools/make_ds_forwarder.py's _build_silent_wav() exactly.
QByteArray BuildSilentWav() {
    constexpr int kSampleRate = 32728;
    constexpr double kDurationSeconds = 1.0;
    const int num_samples = static_cast<int>(kDurationSeconds * kSampleRate);
    const QByteArray pcm(num_samples * 2, '\0');

    QByteArray fmt_chunk;
    auto append_u16 = [&fmt_chunk](uint16_t v) {
        fmt_chunk.append(static_cast<char>(v & 0xFF));
        fmt_chunk.append(static_cast<char>((v >> 8) & 0xFF));
    };
    auto append_u32 = [](QByteArray& buf, uint32_t v) {
        buf.append(static_cast<char>(v & 0xFF));
        buf.append(static_cast<char>((v >> 8) & 0xFF));
        buf.append(static_cast<char>((v >> 16) & 0xFF));
        buf.append(static_cast<char>((v >> 24) & 0xFF));
    };
    append_u16(1);           // PCM
    append_u16(1);           // mono
    append_u32(fmt_chunk, kSampleRate);
    append_u32(fmt_chunk, kSampleRate * 2); // byte rate
    append_u16(2);           // block align
    append_u16(16);          // bits per sample

    const uint32_t riff_size =
        4 + (8 + static_cast<uint32_t>(fmt_chunk.size())) + (8 + static_cast<uint32_t>(pcm.size()));

    QByteArray wav;
    wav.append("RIFF");
    append_u32(wav, riff_size);
    wav.append("WAVE");
    wav.append("fmt ");
    append_u32(wav, static_cast<uint32_t>(fmt_chunk.size()));
    wav.append(fmt_chunk);
    wav.append("data");
    append_u32(wav, static_cast<uint32_t>(pcm.size()));
    wav.append(pcm);
    return wav;
}

bool RunTool(const QString& program, const QStringList& args, QString* out_error) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
        *out_error = QStringLiteral("%1 did not start or finish: %2")
                         .arg(QFileInfo(program).fileName(), process.errorString());
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *out_error = QStringLiteral("%1 failed (exit %2):\n%3")
                         .arg(QFileInfo(program).fileName())
                         .arg(process.exitCode())
                         .arg(QString::fromUtf8(process.readAllStandardError()));
        return false;
    }
    return true;
}

} // namespace

NDSBrewerWindow::NDSBrewerWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("SweepDSEmuNDSBrewer"));
    resize(560, 640);

    auto* layout = new QVBoxLayout(this);

    // --- ROM picker ---
    auto* picker_group = new QGroupBox(tr("Build forwarders"), this);
    auto* picker_layout = new QVBoxLayout(picker_group);

    select_all_ = new QCheckBox(tr("Select All"), picker_group);
    connect(select_all_, &QCheckBox::toggled, this, &NDSBrewerWindow::OnSelectAllToggled);
    picker_layout->addWidget(select_all_);

    rom_list_ = new QListWidget(picker_group);
    picker_layout->addWidget(rom_list_);

    install_after_build_ =
        new QCheckBox(tr("Install after building (deletes the .cia once installed)"), picker_group);
    install_after_build_->setChecked(true);
    picker_layout->addWidget(install_after_build_);

    build_button_ = new QPushButton(tr("Build Forwarders"), picker_group);
    connect(build_button_, &QPushButton::clicked, this, &NDSBrewerWindow::OnBuildClicked);
    picker_layout->addWidget(build_button_);

    layout->addWidget(picker_group);

    // --- Manage existing forwarders ---
    auto* manage_group = new QGroupBox(tr("Manage existing forwarders"), this);
    auto* manage_layout = new QVBoxLayout(manage_group);

    forwarder_list_ = new QListWidget(manage_group);
    manage_layout->addWidget(forwarder_list_);

    delete_forwarder_button_ = new QPushButton(tr("Delete Selected"), manage_group);
    connect(delete_forwarder_button_, &QPushButton::clicked, this,
            &NDSBrewerWindow::OnDeleteForwarderClicked);
    manage_layout->addWidget(delete_forwarder_button_);

    layout->addWidget(manage_group);

    RefreshRomList();
    RefreshForwarderList();
}

NDSBrewerWindow::~NDSBrewerWindow() = default;

void NDSBrewerWindow::RefreshRomList() {
    rom_list_->clear();

    for (const QString& subfolder : {QStringLiteral("nds"), QStringLiteral("dsi")}) {
        QDir dir(ProfileRomDir(subfolder));
        if (!dir.exists()) {
            continue;
        }
        const QStringList filters{QStringLiteral("*.nds"), QStringLiteral("*.dsi")};
        for (const QFileInfo& info : dir.entryInfoList(filters, QDir::Files, QDir::Name)) {
            auto* item = new QListWidgetItem(info.fileName(), rom_list_);
            item->setData(Qt::UserRole, info.absoluteFilePath());
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
        }
    }
}

void NDSBrewerWindow::RefreshForwarderList() {
    forwarder_list_->clear();

    for (const auto& forwarder : DSForwarderRegistry::ListForwarders()) {
        const QString label = QStringLiteral("%1  (%2)")
                                   .arg(QFileInfo(forwarder.rom_path).fileName(),
                                        QStringLiteral("%1").arg(forwarder.program_id, 16, 16,
                                                                 QLatin1Char('0')));
        auto* item = new QListWidgetItem(label, forwarder_list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(forwarder.program_id));
    }
}

void NDSBrewerWindow::OnSelectAllToggled(bool checked) {
    for (int i = 0; i < rom_list_->count(); i++) {
        rom_list_->item(i)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

QString NDSBrewerWindow::BuildForwarderCia(const QString& rom_path, QString* out_error) {
    const QFileInfo rom_info(rom_path);
    const QString absolute_rom_path = rom_info.absoluteFilePath();

    const QPixmap icon_pixmap = NdsIcon::Decode(rom_path.toStdString());
    if (icon_pixmap.isNull()) {
        *out_error = tr("This ROM has no icon/banner block -- can't build a forwarder for it.");
        return QString();
    }

    QTemporaryDir work_dir;
    if (!work_dir.isValid()) {
        *out_error = tr("Couldn't create a temporary work directory.");
        return QString();
    }

    const QString icon_png = work_dir.filePath(QStringLiteral("icon.png"));
    // Nearest-neighbor upscale from the native 32x32 keeps the pixelated
    // look instead of blurring it, matching make_ds_forwarder.py.
    icon_pixmap.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::FastTransformation)
        .save(icon_png, "PNG");

    const QString stub_elf = work_dir.filePath(QStringLiteral("forwarder.elf"));
    {
        QFile f(stub_elf);
        if (!f.open(QIODevice::WriteOnly)) {
            *out_error = tr("Couldn't write the stub app to a temp file.");
            return QString();
        }
        f.write(reinterpret_cast<const char*>(HW::ds_forwarder_stub_elf),
                static_cast<qint64>(HW::ds_forwarder_stub_elf_size));
    }

    const QString title = rom_info.completeBaseName();
    const QString tools_dir = ForwarderToolsDir();
#ifdef Q_OS_WIN
    const QString bannertool = tools_dir + QStringLiteral("bannertool.exe");
    const QString makerom = tools_dir + QStringLiteral("makerom.exe");
#else
    const QString bannertool = tools_dir + QStringLiteral("bannertool");
    const QString makerom = tools_dir + QStringLiteral("makerom");
#endif
    const QString rsf_template = tools_dir + QStringLiteral("template.rsf");
    if (!QFileInfo::exists(bannertool) || !QFileInfo::exists(makerom) ||
        !QFileInfo::exists(rsf_template)) {
        *out_error = tr("bannertool/makerom weren't found next to this app "
                        "(expected under %1) -- reinstall SweepDSEmuNDSBrewer.")
                         .arg(tools_dir);
        return QString();
    }

    const QString smdh_path = work_dir.filePath(QStringLiteral("forwarder.smdh"));
    if (!RunTool(bannertool,
                {QStringLiteral("makesmdh"), QStringLiteral("-s"), title, QStringLiteral("-l"),
                 title, QStringLiteral("-p"), QStringLiteral("Azahar"), QStringLiteral("-i"),
                 icon_png, QStringLiteral("-o"), smdh_path, QStringLiteral("-f"),
                 QStringLiteral("visible,extendedbanner")},
                out_error)) {
        return QString();
    }

    // 256x128 fixed canvas: no cover art input in this first version, so
    // just center the icon on a plain background (matches
    // make_ds_forwarder.py's own no-cover-art fallback).
    QImage banner_image(256, 128, QImage::Format_ARGB32);
    banner_image.fill(qRgb(32, 32, 48));
    {
        QPainter painter(&banner_image);
        const QImage banner_icon =
            icon_pixmap.toImage().scaled(96, 96, Qt::KeepAspectRatio, Qt::FastTransformation);
        painter.drawImage((256 - banner_icon.width()) / 2, (128 - banner_icon.height()) / 2,
                          banner_icon);
    }
    const QString banner_png = work_dir.filePath(QStringLiteral("banner.png"));
    banner_image.save(banner_png, "PNG");

    const QString silence_wav = work_dir.filePath(QStringLiteral("silence.wav"));
    {
        QFile f(silence_wav);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(BuildSilentWav());
        }
    }

    const QString banner_bnr = work_dir.filePath(QStringLiteral("forwarder.bnr"));
    if (!RunTool(bannertool,
                {QStringLiteral("makebanner"), QStringLiteral("-i"), banner_png,
                 QStringLiteral("-a"), silence_wav, QStringLiteral("-o"), banner_bnr},
                out_error)) {
        return QString();
    }

    const ForwarderIdentity identity = ComputeIdentity(absolute_rom_path);
    const QString empty_romfs = work_dir.filePath(QStringLiteral("empty_romfs"));
    QDir().mkpath(empty_romfs);

    const QString output_cia_dir =
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::UserDir)) +
        QStringLiteral("ndsbrewer_output/");
    QDir().mkpath(output_cia_dir);
    const QString output_cia = output_cia_dir + SafeFileName(title) + QStringLiteral(".cia");

    if (!RunTool(makerom,
                {QStringLiteral("-f"), QStringLiteral("cia"), QStringLiteral("-o"), output_cia,
                 QStringLiteral("-rsf"), rsf_template, QStringLiteral("-elf"), stub_elf,
                 QStringLiteral("-icon"), smdh_path, QStringLiteral("-banner"), banner_bnr,
                 QStringLiteral("-target"), QStringLiteral("t"), QStringLiteral("-exefslogo"),
                 QStringLiteral("-DAPP_TITLE=%1").arg(title),
                 QStringLiteral("-DAPP_PRODUCT_CODE=%1").arg(identity.product_code),
                 QStringLiteral("-DAPP_ROMFS=%1").arg(empty_romfs),
                 QStringLiteral("-DAPP_CATEGORY=Application"),
                 QStringLiteral("-DAPP_UNIQUE_ID=%1").arg(identity.unique_id),
                 QStringLiteral("-DAPP_USE_ON_SD=true"), QStringLiteral("-DAPP_ENCRYPTED=false"),
                 QStringLiteral("-DAPP_MEMORY_TYPE=Application"),
                 QStringLiteral("-DAPP_SYSTEM_MODE=64MB"),
                 QStringLiteral("-DAPP_SYSTEM_MODE_EXT=Legacy"),
                 QStringLiteral("-DAPP_CPU_SPEED=268MHz"),
                 QStringLiteral("-DAPP_ENABLE_L2_CACHE=false"),
                 QStringLiteral("-DAPP_VERSION_MAJOR=1")},
                out_error)) {
        return QString();
    }

    DSForwarderRegistry::RegisterForwarder(identity.program_id, absolute_rom_path);
    return output_cia;
}

void NDSBrewerWindow::OnBuildClicked() {
    QStringList selected_rom_paths;
    for (int i = 0; i < rom_list_->count(); i++) {
        const QListWidgetItem* item = rom_list_->item(i);
        if (item->checkState() == Qt::Checked) {
            selected_rom_paths.append(item->data(Qt::UserRole).toString());
        }
    }
    if (selected_rom_paths.isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("No ROMs selected."));
        return;
    }

    QDialog progress_dialog(this);
    progress_dialog.setWindowTitle(tr("Building Forwarders"));
    progress_dialog.setWindowModality(Qt::WindowModal);
    auto* progress_layout = new QVBoxLayout(&progress_dialog);
    auto* martini = new MartiniProgressWidget(&progress_dialog);
    progress_layout->addWidget(martini);
    auto* cancel_button = new QPushButton(tr("Cancel"), &progress_dialog);
    progress_layout->addWidget(cancel_button);
    bool canceled = false;
    connect(cancel_button, &QPushButton::clicked, &progress_dialog,
            [&canceled, &progress_dialog]() {
                canceled = true;
                progress_dialog.close();
            });
    progress_dialog.show();

    QStringList failures;
    for (int i = 0; i < selected_rom_paths.size(); i++) {
        if (canceled) {
            break;
        }
        const QString& rom_path = selected_rom_paths[i];
        martini->SetLabelText(tr("Building %1...").arg(QFileInfo(rom_path).fileName()));
        martini->SetProgress(i * 100 / selected_rom_paths.size());
        QApplication::processEvents();

        QString error;
        const QString cia_path = BuildForwarderCia(rom_path, &error);
        if (cia_path.isEmpty()) {
            failures.append(QFileInfo(rom_path).fileName() + QStringLiteral(": ") + error);
            continue;
        }

        if (install_after_build_->isChecked()) {
            const auto status = Service::AM::InstallCIA(cia_path.toStdString());
            if (status == Service::AM::InstallStatus::Success) {
                QFile::remove(cia_path);
            } else {
                failures.append(QFileInfo(rom_path).fileName() +
                                tr(": built but failed to install (cia kept at %1)").arg(cia_path));
            }
        }
    }
    martini->SetProgress(100);
    progress_dialog.close();

    if (!failures.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Some forwarders had problems:\n\n%1").arg(failures.join('\n')));
    }

    RefreshForwarderList();
}

void NDSBrewerWindow::OnDeleteForwarderClicked() {
    QListWidgetItem* item = forwarder_list_->currentItem();
    if (!item) {
        QMessageBox::information(this, windowTitle(), tr("No forwarder selected."));
        return;
    }
    const uint64_t program_id = item->data(Qt::UserRole).toULongLong();

    if (QMessageBox::question(this, windowTitle(),
                              tr("Delete this forwarder? This removes it from the HOME Menu and "
                                 "cannot be undone (the original .nds ROM itself is untouched).")) !=
        QMessageBox::Yes) {
        return;
    }

    DSForwarderRegistry::RemoveForwarder(program_id);
    RefreshForwarderList();
}
