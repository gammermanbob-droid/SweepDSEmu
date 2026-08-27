// src/citra_qt/configure_ds_controls.cpp

#include "citra_qt/configure_ds_controls.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "input_common/main.h"

using MergedCore::DSButton;

ConfigureDSControls::ConfigureDSControls(QWidget* parent)
    : QDialog(parent), bindings_(DSControlsConfig::LoadKeyBindings()),
      controller_bindings_(DSControlsConfig::LoadControllerBindings()),
      home_menu_key_(DSControlsConfig::LoadReturnToHomeMenuKey()),
      home_menu_controller_binding_(DSControlsConfig::LoadHomeMenuControllerBinding()) {
    setWindowTitle(tr("Configure DS Controls"));
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("Click a button below, then press the key (or, for the Controller column, the "
           "gamepad button) you want to use for it. Controller bindings are optional — "
           "right-click one to clear it."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout();
    for (DSButton button : DSControlsConfig::AllButtons()) {
        auto* key_button = new QPushButton(this);
        key_button->setCheckable(false);
        connect(key_button, &QPushButton::clicked, this,
                [this, button, key_button]() { BeginListening(button, key_button); });
        buttons_by_ds_button_[button] = key_button;

        auto* controller_button = new QPushButton(this);
        controller_button->setCheckable(false);
        controller_button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(controller_button, &QPushButton::clicked, this,
                [this, button, controller_button]() {
                    BeginListeningForController(button, controller_button);
                });
        connect(controller_button, &QPushButton::customContextMenuRequested, this,
                [this, button]() { ClearControllerBinding(button); });
        controller_buttons_by_ds_button_[button] = controller_button;

        auto* row = new QHBoxLayout();
        row->addWidget(key_button);
        row->addWidget(controller_button);
        form->addRow(DSControlsConfig::ButtonName(button) + QStringLiteral(":"), row);
    }
    layout->addLayout(form);

    auto* hotkeys_form = new QFormLayout();
    home_menu_key_button_ = new QPushButton(this);
    home_menu_key_button_->setCheckable(false);
    connect(home_menu_key_button_, &QPushButton::clicked, this,
            [this]() { BeginListeningForHomeMenuKey(home_menu_key_button_); });

    home_menu_controller_button_ = new QPushButton(this);
    home_menu_controller_button_->setCheckable(false);
    home_menu_controller_button_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(home_menu_controller_button_, &QPushButton::clicked, this,
            [this]() { BeginListeningForHomeMenuController(home_menu_controller_button_); });
    connect(home_menu_controller_button_, &QPushButton::customContextMenuRequested, this,
            [this]() { ClearHomeMenuControllerBinding(); });

    auto* home_menu_row = new QHBoxLayout();
    home_menu_row->addWidget(home_menu_key_button_);
    home_menu_row->addWidget(home_menu_controller_button_);
    hotkeys_form->addRow(tr("Return to 3DS HOME Menu:"), home_menu_row);
    layout->addLayout(hotkeys_form);

    auto* restore_defaults = new QPushButton(tr("Restore Defaults"), this);
    connect(restore_defaults, &QPushButton::clicked, this, &ConfigureDSControls::RestoreDefaults);
    layout->addWidget(restore_defaults);

    auto_savestate_checkbox_ = new QCheckBox(tr("Auto save/load state"), this);
    auto_savestate_checkbox_->setChecked(DSControlsConfig::LoadAutoSaveState());
    auto_savestate_checkbox_->setToolTip(
        tr("Automatically save your progress when closing a DS game and resume from it next "
           "time, regardless of the game's own save data."));
    layout->addWidget(auto_savestate_checkbox_);

    auto* button_box =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(button_box, &QDialogButtonBox::accepted, this, &ConfigureDSControls::Accept);
    connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(button_box);

    timeout_timer_.setSingleShot(true);
    connect(&timeout_timer_, &QTimer::timeout, this,
            [this]() { SetControllerPollingResult({}, true); });
    connect(&poll_timer_, &QTimer::timeout, this, [this]() {
        for (auto& poller : device_pollers_) {
            const Common::ParamPackage params = poller->GetNextInput();
            if (params.Has("engine") && !params.Has("down")) {
                SetControllerPollingResult(params, false);
                return;
            }
        }
    });

    RebuildButtonLabels();
}

ConfigureDSControls::~ConfigureDSControls() = default;

void ConfigureDSControls::RebuildButtonLabels() {
    for (auto it = buttons_by_ds_button_.constBegin(); it != buttons_by_ds_button_.constEnd();
         ++it) {
        const int key = bindings_.value(it.key(), 0);
        it.value()->setText(key != 0 ? QKeySequence(key).toString() : tr("(unbound)"));
    }
    for (auto it = controller_buttons_by_ds_button_.constBegin();
         it != controller_buttons_by_ds_button_.constEnd(); ++it) {
        const QString serialized = controller_bindings_.value(it.key());
        if (serialized.isEmpty()) {
            it.value()->setText(tr("(none)"));
        } else {
            const Common::ParamPackage params(serialized.toStdString());
            it.value()->setText(QString::fromStdString(InputCommon::ButtonToText(params)));
        }
    }
    home_menu_key_button_->setText(
        home_menu_key_ != 0 ? QKeySequence(home_menu_key_).toString() : tr("(unbound)"));

    if (home_menu_controller_binding_.isEmpty()) {
        home_menu_controller_button_->setText(tr("(none)"));
    } else {
        const Common::ParamPackage params(home_menu_controller_binding_.toStdString());
        home_menu_controller_button_->setText(
            QString::fromStdString(InputCommon::ButtonToText(params)));
    }
}

void ConfigureDSControls::BeginListening(DSButton button, QPushButton* target) {
    // Re-clicking the button currently being listened for cancels
    // listening instead of getting stuck waiting on itself.
    if (listening_target_ == target) {
        listening_target_ = nullptr;
        RebuildButtonLabels();
        return;
    }

    listening_target_ = target;
    listening_button_ = button;
    listening_for_home_menu_key_ = false;
    target->setText(tr("Press a key..."));
    setFocus();
}

void ConfigureDSControls::BeginListeningForHomeMenuKey(QPushButton* target) {
    if (listening_target_ == target) {
        listening_target_ = nullptr;
        RebuildButtonLabels();
        return;
    }

    listening_target_ = target;
    listening_for_home_menu_key_ = true;
    target->setText(tr("Press a key..."));
    setFocus();
}

void ConfigureDSControls::BeginListeningForController(DSButton button, QPushButton* target) {
    if (controller_listening_target_ == target) {
        SetControllerPollingResult({}, true);
        return;
    }
    // Switching from listening for one controller binding straight to
    // another — cancel the first cleanly (stop its pollers/timers)
    // rather than leaking device_pollers_ from the abandoned attempt.
    if (controller_listening_target_) {
        SetControllerPollingResult({}, true);
    }

    controller_listening_target_ = target;
    controller_listening_button_ = button;
    target->setText(tr("Press a button..."));
    setFocus();

    device_pollers_ = InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Button);
    for (auto& poller : device_pollers_) {
        poller->Start();
    }
    timeout_timer_.start(5000);
    poll_timer_.start(200);
}

void ConfigureDSControls::ClearControllerBinding(DSButton button) {
    if (controller_listening_target_ == controller_buttons_by_ds_button_.value(button)) {
        SetControllerPollingResult({}, true);
    }
    controller_bindings_[button] = QString();
    RebuildButtonLabels();
}

void ConfigureDSControls::BeginListeningForHomeMenuController(QPushButton* target) {
    if (controller_listening_target_ == target) {
        SetControllerPollingResult({}, true);
        return;
    }
    if (controller_listening_target_) {
        SetControllerPollingResult({}, true);
    }

    controller_listening_target_ = target;
    listening_for_home_menu_controller_ = true;
    target->setText(tr("Press a button..."));
    setFocus();

    device_pollers_ = InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Button);
    for (auto& poller : device_pollers_) {
        poller->Start();
    }
    timeout_timer_.start(5000);
    poll_timer_.start(200);
}

void ConfigureDSControls::ClearHomeMenuControllerBinding() {
    if (controller_listening_target_ == home_menu_controller_button_) {
        SetControllerPollingResult({}, true);
    }
    home_menu_controller_binding_.clear();
    RebuildButtonLabels();
}

void ConfigureDSControls::SetControllerPollingResult(const Common::ParamPackage& params,
                                                      bool abort) {
    timeout_timer_.stop();
    poll_timer_.stop();
    for (auto& poller : device_pollers_) {
        poller->Stop();
    }
    device_pollers_.clear();

    if (!abort && controller_listening_target_) {
        if (listening_for_home_menu_controller_) {
            home_menu_controller_binding_ = QString::fromStdString(params.Serialize());
        } else {
            controller_bindings_[controller_listening_button_] =
                QString::fromStdString(params.Serialize());
        }
    }

    controller_listening_target_ = nullptr;
    listening_for_home_menu_controller_ = false;
    RebuildButtonLabels();
}

void ConfigureDSControls::keyPressEvent(QKeyEvent* event) {
    // Escape cancels whichever kind of listening is in progress, if
    // any, rather than binding Escape itself as a key or falling
    // through to the dialog's own shortcut handling (which would
    // otherwise close the whole dialog mid-listen).
    if (controller_listening_target_) {
        if (event->key() == Qt::Key_Escape) {
            SetControllerPollingResult({}, true);
        }
        // Any other key is ignored — this column only accepts
        // controller input, delivered via poll_timer_ rather than here.
        return;
    }

    if (!listening_target_) {
        QDialog::keyPressEvent(event);
        return;
    }

    // Escape always cancels listening rather than binding Escape itself
    // — otherwise a mis-click here could make it impossible to
    // press Escape to back out of this exact dialog again.
    if (event->key() != Qt::Key_Escape) {
        if (listening_for_home_menu_key_) {
            home_menu_key_ = event->key();
        } else {
            bindings_[listening_button_] = event->key();
        }
    }

    listening_target_ = nullptr;
    listening_for_home_menu_key_ = false;
    RebuildButtonLabels();
}

void ConfigureDSControls::RestoreDefaults() {
    if (controller_listening_target_) {
        SetControllerPollingResult({}, true);
    }
    listening_target_ = nullptr;
    listening_for_home_menu_key_ = false;
    bindings_ = DSControlsConfig::DefaultKeyBindings();
    home_menu_key_ = DSControlsConfig::DefaultReturnToHomeMenuKey();
    // Controller bindings have no non-empty default (see
    // ds_controls_config.h) — "restore defaults" just clears them.
    controller_bindings_.clear();
    listening_for_home_menu_controller_ = false;
    home_menu_controller_binding_.clear();
    RebuildButtonLabels();
}

void ConfigureDSControls::Accept() {
    DSControlsConfig::SaveKeyBindings(bindings_);
    DSControlsConfig::SaveControllerBindings(controller_bindings_);
    DSControlsConfig::SaveReturnToHomeMenuKey(home_menu_key_);
    DSControlsConfig::SaveHomeMenuControllerBinding(home_menu_controller_binding_);
    DSControlsConfig::SaveAutoSaveState(auto_savestate_checkbox_->isChecked());
    accept();
}
