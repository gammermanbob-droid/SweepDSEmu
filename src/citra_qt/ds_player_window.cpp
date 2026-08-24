// src/citra_qt/ds_player_window.cpp

#include <algorithm>
#include <chrono>
#include <thread>

#include <QAudioFormat>
#include <QCloseEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMediaDevices>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>

#include "citra_qt/ds_player_window.h"
#include "common/file_util.h"
#include "core/button_id_ds.h"
#include "core/core_factory.h"
#include "core/frontend/emu_window.h"

namespace {

// MelonDSCore::Load() takes a Frontend::EmuWindow& purely to satisfy
// the shared EmulationCore interface (ThreeDSCoreAdapter needs a real
// one to hand to Core::System::Load); MelonDSCore itself never reads
// it. This stub exists so we have something to pass without dragging
// in Azahar's Vulkan/GL window machinery for a DS session that
// doesn't use it.
class NullEmuWindow : public Frontend::EmuWindow {
public:
    void PollEvents() override {}
};

QImage FrameBufferToImage(const MergedCore::ScreenBuffer& buffer) {
    if (buffer.width <= 0 || buffer.height <= 0 ||
        buffer.pixels.size() != static_cast<size_t>(buffer.width) * buffer.height) {
        return QImage();
    }
    // melonDS's own Qt frontend (Screen.cpp) memcpy's this exact same
    // raw framebuffer into a QImage::Format_RGB32 buffer, not ARGB32 —
    // the alpha byte isn't reliably 0xFF, so respecting it (ARGB32)
    // blended frames as transparent over our black background,
    // rendering as solid black despite the emulated frame being valid.
    // RGB32 forces full opacity and matches melonDS's own convention.
    QImage image(reinterpret_cast<const uchar*>(buffer.pixels.data()), buffer.width, buffer.height,
                 buffer.width * 4, QImage::Format_RGB32);
    // Deep copy: `buffer` is about to be destroyed when this frame's
    // MergedCore::FrameOutput goes out of scope back in DSEmuThread::run().
    return image.copy();
}

QString StatePathFor(const QString& rom_path) {
    const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "nds_states";
    FileUtil::CreateFullPath(dir + "/");
    const QFileInfo info(rom_path);
    return QString::fromStdString(dir) + QStringLiteral("/") + info.completeBaseName() +
           QStringLiteral(".dstate");
}

} // namespace

DSEmuThread::DSEmuThread(std::unique_ptr<MergedCore::EmulationCore> core, QString rom_path)
    : core_(std::move(core)), rom_path_(std::move(rom_path)) {}

DSEmuThread::~DSEmuThread() = default;

void DSEmuThread::SetInput(const MergedCore::InputState& input) {
    std::lock_guard lock(input_mutex_);
    input_ = input;
}

void DSEmuThread::RequestStop() {
    stop_requested_ = true;
}

void DSEmuThread::RequestReset() {
    reset_requested_ = true;
}

void DSEmuThread::RequestSaveState(const QString& path) {
    std::lock_guard lock(state_path_mutex_);
    pending_save_path_ = path;
}

void DSEmuThread::RequestLoadState(const QString& path) {
    std::lock_guard lock(state_path_mutex_);
    pending_load_path_ = path;
}

void DSEmuThread::run() {
    NullEmuWindow window;
    const auto load_status = core_->Load(window, rom_path_.toStdString());
    if (load_status != MergedCore::ResultStatus::Success) {
        emit LoadFailed(QStringLiteral("Failed to load DS ROM (status %1)")
                             .arg(static_cast<int>(load_status)));
        return;
    }

    // The DS runs its LCDs at ~59.8261 Hz; pace to that instead of
    // running flat-out, and re-anchor the deadline each iteration so
    // occasional slow frames don't accumulate drift.
    constexpr auto frame_period =
        std::chrono::duration<double>(1.0 / 59.8261);
    auto next_frame = std::chrono::steady_clock::now();
    bool console_powered_off = false;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        {
            std::lock_guard lock(state_path_mutex_);
            if (!pending_save_path_.isEmpty()) {
                core_->SaveState(pending_save_path_.toStdString());
                pending_save_path_.clear();
            }
            if (!pending_load_path_.isEmpty()) {
                core_->LoadState(pending_load_path_.toStdString());
                pending_load_path_.clear();
            }
        }

        if (reset_requested_.exchange(false)) {
            core_->Reset();
        }

        MergedCore::InputState input;
        {
            std::lock_guard lock(input_mutex_);
            input = input_;
        }

        MergedCore::FrameOutput frame;
        core_->RunFrame(input, frame);
        emit FrameReady(FrameBufferToImage(frame.top), FrameBufferToImage(frame.bottom));
        if (!frame.audio_samples.empty()) {
            emit AudioReady(QByteArray(reinterpret_cast<const char*>(frame.audio_samples.data()),
                                        static_cast<int>(frame.audio_samples.size() * sizeof(int16_t))));
        }
        if (frame.powered_off) {
            // nds_->RunFrame() is a permanent no-op from here on (see
            // MelonDSCore::RunFrame) — stop asking for more frames
            // instead of spinning forever on a frozen picture with
            // looping audio.
            console_powered_off = true;
            break;
        }

        next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_period);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now) {
            std::this_thread::sleep_until(next_frame);
        } else {
            // Fell behind (slow host, debugger pause, etc.) — don't
            // try to burst-catch-up, just resync from here.
            next_frame = now;
        }
    }

    core_->Shutdown();

    if (console_powered_off) {
        emit ConsolePoweredOff();
    }
}

DSPlayerWindow::DSPlayerWindow(const QString& rom_path, QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("SweepDS Emu | %1").arg(QFileInfo(rom_path).fileName()));
    resize(kScreenWidth * 2, kScreenHeight * 2 * 2);
    setFocusPolicy(Qt::StrongFocus);

    const auto bindings = DSControlsConfig::LoadKeyBindings();
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        if (it.value() != 0) {
            key_to_button_[it.value()] = it.key();
        }
    }

    auto core = MergedCore::CoreFactory::CreateFor(rom_path.toStdString());
    const int audio_rate = core->GetAudioSampleRate();
    if (audio_rate > 0) {
        QAudioFormat format;
        format.setSampleRate(audio_rate);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        audio_sink_ = std::make_unique<QAudioSink>(QMediaDevices::defaultAudioOutput(), format);
        audio_device_ = audio_sink_->start();
    }

    thread_ = std::make_unique<DSEmuThread>(std::move(core), rom_path);
    connect(thread_.get(), &DSEmuThread::FrameReady, this, &DSPlayerWindow::OnFrameReady);
    connect(thread_.get(), &DSEmuThread::LoadFailed, this, &DSPlayerWindow::OnLoadFailed);
    connect(thread_.get(), &DSEmuThread::AudioReady, this, &DSPlayerWindow::OnAudioReady);
    connect(thread_.get(), &DSEmuThread::ConsolePoweredOff, this,
            &DSPlayerWindow::OnConsolePoweredOff);
    // melonDS's ARMJIT compiler constructor generates ~400 code
    // trampolines with heavy local ARM64Reg/FixupBranch usage — give
    // this thread a generous stack (Qt's platform default may be
    // smaller than what other melonDS embeddings implicitly rely on).
    thread_->setStackSize(16 * 1024 * 1024);
    thread_->start();
}

DSPlayerWindow::~DSPlayerWindow() {
    if (thread_) {
        thread_->RequestStop();
        thread_->wait();
    }
}

void DSPlayerWindow::OnFrameReady(QImage top, QImage bottom) {
    if (!top.isNull())
        top_image_ = std::move(top);
    if (!bottom.isNull())
        bottom_image_ = std::move(bottom);
    update();
}

void DSPlayerWindow::OnAudioReady(QByteArray pcm) {
    if (audio_device_)
        audio_device_->write(pcm);
}

void DSPlayerWindow::OnLoadFailed(QString reason) {
    QMessageBox::critical(this, tr("SweepDS Emu"), reason);
    close();
}

void DSPlayerWindow::OnConsolePoweredOff() {
    // Not an error — matches real DS/DSi hardware, which genuinely
    // powers all the way off after e.g. its firmware settings wizard
    // finishes, rather than restarting itself. Nothing left to do but
    // let the user know and close; they can relaunch to "press power"
    // again.
    QMessageBox::information(this, tr("SweepDS Emu"),
                             tr("The DS console powered itself off."));
    close();
}

void DSPlayerWindow::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (top_image_.isNull() && bottom_image_.isNull())
        return;

    const int half_height = height() / 2;
    if (!top_image_.isNull()) {
        painter.drawImage(QRect(0, 0, width(), half_height), top_image_);
    }
    if (!bottom_image_.isNull()) {
        painter.drawImage(QRect(0, half_height, width(), height() - half_height), bottom_image_);
    }
}

void DSPlayerWindow::UpdateTouch(const QPoint& widget_pos, bool pressed) {
    const int half_height = height() / 2;
    if (widget_pos.y() < half_height) {
        // Touch input only applies to the bottom screen, same as real
        // DS hardware — ignore presses landing on the top half.
        input_state_.touch_pressed = false;
        RebuildKeyMask();
        return;
    }

    if (!pressed) {
        input_state_.touch_pressed = false;
        RebuildKeyMask();
        return;
    }

    const int bottom_height = height() - half_height;
    if (width() <= 0 || bottom_height <= 0)
        return;

    const double scale_x = static_cast<double>(kScreenWidth) / width();
    const double scale_y = static_cast<double>(kScreenHeight) / bottom_height;
    int ds_x = static_cast<int>(widget_pos.x() * scale_x);
    int ds_y = static_cast<int>((widget_pos.y() - half_height) * scale_y);
    ds_x = std::clamp(ds_x, 0, kScreenWidth - 1);
    ds_y = std::clamp(ds_y, 0, kScreenHeight - 1);

    input_state_.touch_pressed = true;
    input_state_.touch_x = static_cast<uint16_t>(ds_x);
    input_state_.touch_y = static_cast<uint16_t>(ds_y);
    RebuildKeyMask();
}

void DSPlayerWindow::RebuildKeyMask() {
    thread_->SetInput(input_state_);
}

void DSPlayerWindow::mousePressEvent(QMouseEvent* event) {
    UpdateTouch(event->pos(), true);
}

void DSPlayerWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton)
        UpdateTouch(event->pos(), true);
}

void DSPlayerWindow::mouseReleaseEvent(QMouseEvent* /*event*/) {
    input_state_.touch_pressed = false;
    RebuildKeyMask();
}

void DSPlayerWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }

    // F1/F5/F9 are fixed emulator hotkeys (reset/save/load state) — not
    // DS buttons, so they're not part of the remappable set in
    // ConfigureDSControls.
    switch (event->key()) {
    case Qt::Key_F5:
        thread_->RequestSaveState(StatePathFor(windowTitle()));
        return;
    case Qt::Key_F9:
        thread_->RequestLoadState(StatePathFor(windowTitle()));
        return;
    case Qt::Key_F1:
        thread_->RequestReset();
        return;
    default:
        break;
    }

    const auto it = key_to_button_.constFind(event->key());
    if (it == key_to_button_.constEnd()) {
        return;
    }

    input_state_.buttons |= static_cast<uint32_t>(it.value());
    RebuildKeyMask();
}

void DSPlayerWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }

    const auto it = key_to_button_.constFind(event->key());
    if (it == key_to_button_.constEnd()) {
        return;
    }

    input_state_.buttons &= ~static_cast<uint32_t>(it.value());
    RebuildKeyMask();
}

void DSPlayerWindow::closeEvent(QCloseEvent* event) {
    if (thread_) {
        thread_->RequestStop();
        thread_->wait();
    }
    QWidget::closeEvent(event);
}
