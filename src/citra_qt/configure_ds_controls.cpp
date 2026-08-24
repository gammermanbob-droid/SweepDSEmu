// src/citra_qt/configure_ds_controls.cpp

#include "citra_qt/configure_ds_controls.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

using MergedCore::DSButton;

ConfigureDSControls::ConfigureDSControls(QWidget* parent)
    : QDialog(parent), bindings_(DSControlsConfig::LoadKeyBindings()) {
    setWindowTitle(tr("Configure DS Controls"));
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("Click a button below, then press the key you want to use for it."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout();
    for (DSButton button : DSControlsConfig::AllButtons()) {
        auto* key_button = new QPushButton(this);
        key_button->setCheckable(false);
        connect(key_button, &QPushButton::clicked, this,
                [this, button, key_button]() { BeginListening(button, key_button); });
        buttons_by_ds_button_[button] = key_button;
        form->addRow(DSControlsConfig::ButtonName(button) + QStringLiteral(":"), key_button);
    }
    layout->addLayout(form);

    auto* restore_defaults = new QPushButton(tr("Restore Defaults"), this);
    connect(restore_defaults, &QPushButton::clicked, this, &ConfigureDSControls::RestoreDefaults);
    layout->addWidget(restore_defaults);

    auto* button_box =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(button_box, &QDialogButtonBox::accepted, this, &ConfigureDSControls::Accept);
    connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(button_box);

    RebuildButtonLabels();
}

ConfigureDSControls::~ConfigureDSControls() = default;

void ConfigureDSControls::RebuildButtonLabels() {
    for (auto it = buttons_by_ds_button_.constBegin(); it != buttons_by_ds_button_.constEnd();
         ++it) {
        const int key = bindings_.value(it.key(), 0);
        it.value()->setText(key != 0 ? QKeySequence(key).toString() : tr("(unbound)"));
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
    target->setText(tr("Press a key..."));
    setFocus();
}

void ConfigureDSControls::keyPressEvent(QKeyEvent* event) {
    if (!listening_target_) {
        QDialog::keyPressEvent(event);
        return;
    }

    // Escape always cancels listening rather than binding Escape itself
    // — otherwise a mis-click here could make it impossible to
    // press Escape to back out of this exact dialog again.
    if (event->key() != Qt::Key_Escape) {
        bindings_[listening_button_] = event->key();
    }

    listening_target_ = nullptr;
    RebuildButtonLabels();
}

void ConfigureDSControls::RestoreDefaults() {
    listening_target_ = nullptr;
    bindings_ = DSControlsConfig::DefaultKeyBindings();
    RebuildButtonLabels();
}

void ConfigureDSControls::Accept() {
    DSControlsConfig::SaveKeyBindings(bindings_);
    accept();
}
