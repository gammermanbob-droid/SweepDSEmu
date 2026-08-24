// src/citra_qt/configure_ds_controls.h
//
// Standalone "click a button, press a key" remapper for DS controls
// (see ds_controls_config.h). Deliberately not part of Azahar's main
// ConfigureDialog tab set — that dialog's tabs are all wired through
// Settings::values, and DS control bindings live in their own small
// QSettings-backed store instead (see ds_controls_config.h for why).

#pragma once

#include <QDialog>
#include <QMap>

#include "citra_qt/ds_controls_config.h"

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
    void RestoreDefaults();
    void Accept();

    DSControlsConfig::KeyBindings bindings_;
    QMap<MergedCore::DSButton, QPushButton*> buttons_by_ds_button_;

    // Non-null while waiting for the next key press to bind to
    // listening_button_; keyPressEvent() only intercepts input while
    // this is set, so the dialog's own Tab/Enter/etc. navigation isn't
    // swallowed the rest of the time.
    QPushButton* listening_target_ = nullptr;
    MergedCore::DSButton listening_button_{};
};
