// src/citra_qt/ds_player_window.cpp

#include <algorithm>
#include <chrono>
#include <thread>

#include <QAudioFormat>
#include <QCloseEvent>
#include <QFileInfo>
#include <QFont>
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

// Distinct from StatePathFor's manual-save-slot files so the auto-save
// feature never collides with (or gets confused for) a save the user
// made deliberately.
QString AutoStatePathFor(const QString& rom_path) {
    const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "nds_states";
    FileUtil::CreateFullPath(dir + "/");
    const QFileInfo info(rom_path);
    return QString::fromStdString(dir) + QStringLiteral("/") + info.completeBaseName() +
           QStringLiteral(".auto.dstate");
}

} // namespace

DSEmuThread::DSEmuThread(std::unique_ptr<MergedCore::EmulationCore> core, QString rom_path)
    : core_(std::move(core)), rom_path_(std::move(rom_path)) {}

DSEmuThread::~DSEmuThread() = default;

void DSEmuThread::SetInput(const MergedCore::InputState& input) {
    std::lock_guard lock(input_mutex_);
    input_ = input;
}

void DSEmuThread::RequestStop(const QString& auto_save_path) {
    if (!auto_save_path.isEmpty()) {
        RequestSaveState(auto_save_path);
        // run()'s loop only consumes a pending save/load path once per
        // frame, in between other work -- if stop_requested_ were set
        // immediately, a thread currently asleep in its frame-pacing wait
        // could wake up, see stop_requested_ already true, and exit
        // without ever reaching that check, silently dropping this save.
        // Wait for it to actually be consumed first. Bounded so a wedged
        // emulation thread can't hang shutdown forever; one real frame at
        // ~60fps is ~16ms, so 2s is generous headroom.
        for (int i = 0; i < 200; ++i) {
            {
                std::lock_guard lock(state_path_mutex_);
                if (pending_save_path_.isEmpty()) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
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

void DSEmuThread::RequestInsertCart(const QString& path) {
    std::lock_guard lock(state_path_mutex_);
    pending_insert_cart_path_ = path;
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
            if (!pending_insert_cart_path_.isEmpty()) {
                core_->InsertCart(pending_insert_cart_path_.toStdString());
                pending_insert_cart_path_.clear();
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
    // Empty rom_path is MelonDSCore::Load's "boot straight to the DSi
    // Menu, no cart" convention (see CoreFactory::Detect's own doc
    // comment on the same thing) -- QFileInfo("").fileName() is just an
    // empty string, so name the window sensibly instead of "SweepDS Emu | ".
    const QString display_name =
        rom_path.isEmpty() ? QStringLiteral("DSi Menu") : QFileInfo(rom_path).fileName();
    setWindowTitle(QStringLiteral("SweepDS Emu | %1").arg(display_name));
    is_dsi_menu_session_ = rom_path.isEmpty();
    resize(kScreenWidth * 2, kScreenHeight * 2 * 2);
    setFocusPolicy(Qt::StrongFocus);

    const auto bindings = DSControlsConfig::LoadKeyBindings();
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        if (it.value() != 0) {
            key_to_button_[it.value()] = it.key();
        }
    }
    return_to_home_menu_key_ = DSControlsConfig::LoadReturnToHomeMenuKey();

    const auto controller_bindings = DSControlsConfig::LoadControllerBindings();
    for (auto it = controller_bindings.constBegin(); it != controller_bindings.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            controller_devices_[it.key()] =
                Input::CreateDevice<Input::ButtonDevice>(it.value().toStdString());
        }
    }
    const QString home_menu_controller_binding = DSControlsConfig::LoadHomeMenuControllerBinding();
    if (!home_menu_controller_binding.isEmpty()) {
        home_menu_controller_device_ =
            Input::CreateDevice<Input::ButtonDevice>(home_menu_controller_binding.toStdString());
    }

    if (!controller_devices_.empty() || home_menu_controller_device_) {
        controller_poll_timer_ = new QTimer(this);
        connect(controller_poll_timer_, &QTimer::timeout, this, &DSPlayerWindow::PollController);
        controller_poll_timer_->start(16);
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

    if (DSControlsConfig::LoadAutoSaveState() && !is_dsi_menu_session_) {
        // Must match closeEvent()'s AutoStatePathFor(windowTitle()) below
        // (itself matching the pre-existing manual save/load convention
        // at F5/F9) rather than rom_path directly -- those are different
        // strings (windowTitle() is "SweepDS Emu | <filename>"), so using
        // rom_path here would derive a different filename and the
        // auto-load could never find what the auto-save actually wrote.
        const QString auto_path = AutoStatePathFor(windowTitle());
        if (QFileInfo::exists(auto_path)) {
            thread_->RequestLoadState(auto_path);
        }
    }

    loading_spinner_timer_ = new QTimer(this);
    connect(loading_spinner_timer_, &QTimer::timeout, this, [this]() {
        loading_spinner_angle_ = (loading_spinner_angle_ + 8) % 360;
        update();
    });
    loading_spinner_timer_->start(16);
}

DSPlayerWindow::~DSPlayerWindow() {
    // The normal user-initiated close path (closeEvent(), below) already
    // stops the thread -- including the auto-save, when enabled -- before
    // this destructor ever runs, via Qt::WA_DeleteOnClose. This is just a
    // safety net for the (rare) case of programmatic deletion without a
    // close event; a plain stop here, not another auto-save attempt, since
    // by the time closeEvent() has already run the thread's loop has
    // exited and nothing would be left to consume a second save request
    // (RequestStop's bounded wait would just burn its full timeout).
    if (thread_) {
        thread_->RequestStop();
        thread_->wait();
    }
}

void DSPlayerWindow::OnFrameReady(QImage top, QImage bottom) {
    if (!first_frame_received_) {
        first_frame_received_ = true;
        loading_spinner_timer_->stop();
    }
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

QRect DSPlayerWindow::AspectFitRect(const QRect& area) {
    if (area.width() <= 0 || area.height() <= 0) {
        return area;
    }
    // Compare width-if-full-height against the area's own width rather
    // than dividing to compare ratios directly, so this works exactly
    // the same regardless of int/float precision quirks at extreme
    // (very wide or very tall) window shapes.
    const int width_at_full_height = area.height() * kScreenWidth / kScreenHeight;
    if (width_at_full_height <= area.width()) {
        // Height is the limiting dimension -- pillarbox (bars on the sides).
        const int x = area.left() + (area.width() - width_at_full_height) / 2;
        return QRect(x, area.top(), width_at_full_height, area.height());
    }
    // Width is the limiting dimension -- letterbox (bars top/bottom).
    const int height_at_full_width = area.width() * kScreenHeight / kScreenWidth;
    const int y = area.top() + (area.height() - height_at_full_width) / 2;
    return QRect(area.left(), y, area.width(), height_at_full_width);
}

void DSPlayerWindow::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (top_image_.isNull() && bottom_image_.isNull()) {
        // No real frame has arrived yet -- the ROM (and, for homebrew,
        // its SD card image) is still loading, which can take a
        // noticeable moment. Without this the window is just plain black
        // the whole time, indistinguishable from a hang. loading_spinner_timer_
        // (started in the constructor, stopped for good on the first real
        // frame in OnFrameReady) keeps repainting this at ~60fps to
        // animate the spinner below.
        const int spinner_diameter = std::min(width(), height()) / 6;
        const QRect spinner_rect(width() / 2 - spinner_diameter / 2,
                                  height() / 2 - spinner_diameter / 2 - spinner_diameter,
                                  spinner_diameter, spinner_diameter);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor(255, 255, 255, 60));
        pen.setWidth(spinner_diameter / 10);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawArc(spinner_rect, 0, 360 * 16);
        pen.setColor(Qt::white);
        painter.setPen(pen);
        // Angles in Qt's drawArc are in 1/16ths of a degree and increase
        // counter-clockwise from 3 o'clock -- negate loading_spinner_angle_
        // so it visibly spins clockwise, the more familiar direction for
        // this kind of indicator.
        painter.drawArc(spinner_rect, -loading_spinner_angle_ * 16, 90 * 16);

        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPointSize(14);
        painter.setFont(font);
        const QRect text_rect(0, height() / 2 + spinner_diameter / 2, width(),
                               height() / 2 - spinner_diameter / 2);
        painter.drawText(text_rect, Qt::AlignHCenter | Qt::AlignTop, tr("Loading…"));
        return;
    }

    const int half_height = height() / 2;
    if (!top_image_.isNull()) {
        painter.drawImage(AspectFitRect(QRect(0, 0, width(), half_height)), top_image_);
    }
    if (!bottom_image_.isNull()) {
        painter.drawImage(
            AspectFitRect(QRect(0, half_height, width(), height() - half_height)), bottom_image_);
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

    // Must match paintEvent()'s own AspectFitRect() call exactly, or
    // touch position drifts from what's actually drawn on screen the
    // moment the window isn't a perfect 4:3-per-half shape (i.e.
    // almost always) — this is the same letterboxed/pillarboxed rect
    // the bottom screen image is drawn into, not the full bottom half.
    const QRect bottom_rect =
        AspectFitRect(QRect(0, half_height, width(), height() - half_height));
    if (!bottom_rect.contains(widget_pos)) {
        // Inside the bottom half but landed in a letterbox/pillarbox
        // bar rather than on the screen image itself.
        input_state_.touch_pressed = false;
        RebuildKeyMask();
        return;
    }

    const double scale_x = static_cast<double>(kScreenWidth) / bottom_rect.width();
    const double scale_y = static_cast<double>(kScreenHeight) / bottom_rect.height();
    int ds_x = static_cast<int>((widget_pos.x() - bottom_rect.left()) * scale_x);
    int ds_y = static_cast<int>((widget_pos.y() - bottom_rect.top()) * scale_y);
    ds_x = std::clamp(ds_x, 0, kScreenWidth - 1);
    ds_y = std::clamp(ds_y, 0, kScreenHeight - 1);

    input_state_.touch_pressed = true;
    input_state_.touch_x = static_cast<uint16_t>(ds_x);
    input_state_.touch_y = static_cast<uint16_t>(ds_y);
    RebuildKeyMask();
}

void DSPlayerWindow::RebuildKeyMask() {
    MergedCore::InputState combined = input_state_;
    combined.buttons |= controller_buttons_;
    thread_->SetInput(combined);
}

void DSPlayerWindow::PollController() {
    // Checked ahead of the regular DS-button poll below since a hit
    // here closes the window immediately (same as the keyboard hotkey
    // in keyPressEvent) — no point updating controller_buttons_ for a
    // frame that's about to end anyway.
    if (home_menu_controller_device_ && home_menu_controller_device_->GetStatus()) {
        emit RequestReturnToHomeMenu();
        close();
        return;
    }

    uint32_t buttons = 0;
    for (const auto& [button, device] : controller_devices_) {
        if (device->GetStatus()) {
            buttons |= static_cast<uint32_t>(button);
        }
    }
    if (buttons != controller_buttons_) {
        controller_buttons_ = buttons;
        RebuildKeyMask();
    }
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

void DSPlayerWindow::RequestReset() {
    thread_->RequestReset();
}

void DSPlayerWindow::InsertCart(const QString& path) {
    thread_->RequestInsertCart(path);
}

void DSPlayerWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }

    // The return-to-HOME-Menu hotkey is user-remappable (ConfigureDSControls);
    // check it before the fixed F1/F5/F9 emulator hotkeys below, in case
    // it's ever rebound to collide with one of them.
    if (event->key() == return_to_home_menu_key_) {
        emit RequestReturnToHomeMenu();
        close();
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
        RequestReset();
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
        thread_->RequestStop(DSControlsConfig::LoadAutoSaveState() && !is_dsi_menu_session_
                                  ? AutoStatePathFor(windowTitle())
                                  : QString());
        thread_->wait();
    }
    QWidget::closeEvent(event);
}
