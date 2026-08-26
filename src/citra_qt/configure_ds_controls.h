// src/citra_qt/configure_ds_controls.h
//
// Standalone "click a button, press a key" remapper for DS controls
// (see ds_controls_config.h). Deliberately not part of Azahar's main
// ConfigureDialog tab set — that dialog's tabs are all wired through
// Settings::values, and DS control bindings live in their own small
// QSettings-backed store instead (see ds_controls_config.h for why).

#pragma once

#include <memory>
#include <vector>
#include <QDialog>
#include <QMap>
#include <QTimer>

#include "citra_qt/ds_controls_config.h"
#include "common/param_package.h"

namespace InputCommon::Polling {
class DevicePoller;
}

class QPushButton;

class ConfigureDSControls : public QDialog {
    Q_OBJECT

public:
    explicit ConfigureDSControls(QWidget* parent = nullptr);
    ~ConfigureDSControls() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void RebuildButtonLabels();
    void BeginListening(MergedCore::DSButton button, QPushButton* target);
    void BeginListeningForHomeMenuKey(QPushButton* target);
    void BeginListeningForController(MergedCore::DSButton button, QPushButton* target);
    void ClearControllerBinding(MergedCore::DSButton button);
    void BeginListeningForHomeMenuController(QPushButton* target);
    void ClearHomeMenuControllerBinding();
    void SetControllerPollingResult(const Common::ParamPackage& params, bool abort);
    void RestoreDefaults();
    void Accept();

    DSControlsConfig::KeyBindings bindings_;
    QMap<MergedCore::DSButton, QPushButton*> buttons_by_ds_button_;

    DSControlsConfig::ControllerBindings controller_bindings_;
    QMap<MergedCore::DSButton, QPushButton*> controller_buttons_by_ds_button_;

    // The "return to 3DS HOME Menu" hotkey — not a DS button, so it's
    // tracked separately from bindings_/buttons_by_ds_button_ (see
    // ds_controls_config.h for why).
    int home_menu_key_;
    QPushButton* home_menu_key_button_ = nullptr;

    // Optional controller binding for the same hotkey — see
    // ds_controls_config.h's LoadHomeMenuControllerBinding() for why
    // this matters more here than for ordinary DS buttons.
    QString home_menu_controller_binding_;
    QPushButton* home_menu_controller_button_ = nullptr;

    // Non-null while waiting for the next key press to bind to either
    // listening_button_ or (if listening_for_home_menu_key_) the HOME
    // Menu hotkey; keyPressEvent() only intercepts input while this is
    // set, so the dialog's own Tab/Enter/etc. navigation isn't
    // swallowed the rest of the time.
    QPushButton* listening_target_ = nullptr;
    MergedCore::DSButton listening_button_{};
    bool listening_for_home_menu_key_ = false;

    // Set instead of controller_listening_button_ while listening for
    // the HOME Menu hotkey's controller binding specifically — mirrors
    // listening_for_home_menu_key_'s role for the keyboard side.
    bool listening_for_home_menu_controller_ = false;

    // Controller listening uses Azahar's existing InputCommon::Polling
    // machinery (the same one the 3DS side's Configure Controls dialog
    // uses) instead of keyPressEvent, since a controller press has no
    // Qt event to hook — device_pollers_ get checked on poll_timer_'s
    // tick rather than driven by an event callback. Escape still works
    // to cancel, via keyPressEvent, since it's delivered as a normal
    // key event regardless of what's being listened for.
    QPushButton* controller_listening_target_ = nullptr;
    MergedCore::DSButton controller_listening_button_{};
    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> device_pollers_;
    QTimer poll_timer_;
    QTimer timeout_timer_;
};
