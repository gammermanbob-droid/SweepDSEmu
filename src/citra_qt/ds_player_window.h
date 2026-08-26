// src/citra_qt/ds_player_window.h
//
// Standalone window for DS titles, driven by MergedCore::MelonDSCore.
// Deliberately does not reuse Azahar's existing bootmanager.cpp /
// RendererVulkan pipeline — that pipeline is built around Core::System
// and the Pica3D GPU emulation, none of which applies to a DS session.
// MelonDSCore renders in software and hands back plain RGBA8888
// buffers, so this window just blits those with QPainter each frame
// instead of standing up a second GPU-backed renderer for a simple
// 256x192x2 image.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <QAudioSink>
#include <QByteArray>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include "citra_qt/ds_controls_config.h"
#include "core/emulation_core.h"
#include "core/frontend/input.h"

namespace MergedCore {
class EmulationCore;
}

// Drives MelonDSCore::RunFrame() on its own thread at the DS's native
// ~59.826 Hz, decoupled from the Qt GUI thread. Owns the core for its
// entire lifetime — DSPlayerWindow only ever touches it indirectly,
// through thread-safe accessors here.
class DSEmuThread : public QThread {
    Q_OBJECT

public:
    explicit DSEmuThread(std::unique_ptr<MergedCore::EmulationCore> core, QString rom_path);
    ~DSEmuThread() override;

    // Thread-safe; call from the GUI thread as input state changes.
    void SetInput(const MergedCore::InputState& input);

    void RequestStop();

    // Thread-safe; queued through Qt's event loop internally.
    void RequestSaveState(const QString& path);
    void RequestLoadState(const QString& path);
    void RequestReset();

signals:
    void FrameReady(QImage top, QImage bottom);
    void LoadFailed(QString reason);
    // Interleaved stereo S16 PCM for one frame, at whatever rate
    // core_->GetAudioSampleRate() reports; empty frames aren't emitted.
    void AudioReady(QByteArray pcm);
    // The emulated system shut itself down (see FrameOutput::powered_off) —
    // the run loop has already stopped by the time this fires.
    void ConsolePoweredOff();

protected:
    void run() override;

private:
    std::unique_ptr<MergedCore::EmulationCore> core_;
    QString rom_path_;

    std::mutex input_mutex_;
    MergedCore::InputState input_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reset_requested_{false};

    std::mutex state_path_mutex_;
    QString pending_save_path_;
    QString pending_load_path_;
};

class DSPlayerWindow : public QWidget {
    Q_OBJECT

public:
    explicit DSPlayerWindow(const QString& rom_path, QWidget* parent = nullptr);
    ~DSPlayerWindow() override;

signals:
    // Emitted on the "return to 3DS HOME Menu" hotkey (F12) — a
    // deliberate user action distinct from just closing the window, so
    // it fires regardless of whether this session was launched from a
    // forwarder or picked directly from Azahar's own list.
    void RequestReturnToHomeMenu();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void OnFrameReady(QImage top, QImage bottom);
    void OnLoadFailed(QString reason);
    void OnAudioReady(QByteArray pcm);
    void OnConsolePoweredOff();

private:
    void UpdateTouch(const QPoint& widget_pos, bool pressed);
    void RebuildKeyMask();
    void PollController();

    // Largest kScreenWidth:kScreenHeight-aspect rect that fits inside
    // `area`, centered within it (letterboxed/pillarboxed as needed).
    // paintEvent() and UpdateTouch() both call this so the drawn
    // screen and the touch hit-test always agree on where the screen
    // actually is — previously each computed its own independent
    // mapping (drawImage() stretching to the full half-window rect,
    // UpdateTouch() scaling linearly off the full widget width), which
    // is what caused the squash/stretch-on-resize behavior.
    static QRect AspectFitRect(const QRect& area);

    std::unique_ptr<DSEmuThread> thread_;
    QImage top_image_;
    QImage bottom_image_;

    // Owned on the GUI thread (this object's thread) since QAudioSink
    // needs a running Qt event loop for its internal state machine;
    // DSEmuThread just emits raw PCM via AudioReady and never touches
    // this directly.
    std::unique_ptr<QAudioSink> audio_sink_;
    QIODevice* audio_device_ = nullptr; // owned by audio_sink_

    MergedCore::InputState input_state_;

    // Built once at construction from DSControlsConfig::LoadKeyBindings()
    // — Qt::Key -> DSButton, the direction keyPressEvent/keyReleaseEvent
    // actually need. Control remapping takes effect on the next DS
    // session rather than live, so this doesn't need to watch for
    // changes made in ConfigureDSControls while a session is running.
    QMap<int, MergedCore::DSButton> key_to_button_;

    // Built once at construction from
    // DSControlsConfig::LoadReturnToHomeMenuKey() — same
    // load-once-at-construction rationale as key_to_button_ above.
    int return_to_home_menu_key_;

    // Controller (SDL joystick/gamepad) bindings, built once at
    // construction from DSControlsConfig::LoadControllerBindings().
    // Unlike key_to_button_, which reacts to Qt key press/release
    // events, controller state has no equivalent Qt event to hook —
    // controller_poll_timer_ polls each bound device's GetStatus()
    // every ~16ms (roughly one DS frame) instead.
    std::map<MergedCore::DSButton, std::unique_ptr<Input::ButtonDevice>> controller_devices_;
    QTimer* controller_poll_timer_ = nullptr;

    // Optional controller binding for the "return to 3DS HOME Menu"
    // hotkey, built once at construction from
    // DSControlsConfig::LoadHomeMenuControllerBinding() — polled
    // alongside controller_devices_ in PollController() rather than
    // folded into that map, since it isn't a MergedCore::DSButton and
    // triggers RequestReturnToHomeMenu()/close() instead of feeding
    // controller_buttons_.
    std::unique_ptr<Input::ButtonDevice> home_menu_controller_device_;
    // Currently-held controller buttons, OR'd with input_state_.buttons
    // (which tracks only keyboard-held buttons) in RebuildKeyMask()
    // rather than merged into input_state_.buttons directly — keeping
    // the two sources separate means a stuck/misread controller button
    // can never mask out what the keyboard is actually doing or vice
    // versa.
    uint32_t controller_buttons_ = 0;

    static constexpr int kScreenWidth = 256;
    static constexpr int kScreenHeight = 192;
};
