// src/citra_qt/ds_controls_config.cpp

#include "citra_qt/ds_controls_config.h"

#include <QKeySequence>
#include <QSettings>

#include "common/file_util.h"

using MergedCore::DSButton;

namespace DSControlsConfig {

namespace {

QSettings OpenSettings() {
    const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "qt-config.ini";
    return QSettings(QString::fromStdString(path), QSettings::IniFormat);
}

// Config-file key each button is stored under, e.g. "key_A".
QString SettingsKey(DSButton button) {
    return QStringLiteral("key_") + ButtonName(button);
}

} // namespace

const QList<DSButton>& AllButtons() {
    // Declaration order here becomes the row order in the remap dialog.
    static const QList<DSButton> buttons{
        DSButton::DS_BTN_A,     DSButton::DS_BTN_B,    DSButton::DS_BTN_X,
        DSButton::DS_BTN_Y,     DSButton::DS_BTN_L,    DSButton::DS_BTN_R,
        DSButton::DS_BTN_START, DSButton::DS_BTN_SELECT,
        DSButton::DS_BTN_UP,    DSButton::DS_BTN_DOWN, DSButton::DS_BTN_LEFT,
        DSButton::DS_BTN_RIGHT,
    };
    return buttons;
}

QString ButtonName(DSButton button) {
    switch (button) {
    case DSButton::DS_BTN_A:
        return QStringLiteral("A");
    case DSButton::DS_BTN_B:
        return QStringLiteral("B");
    case DSButton::DS_BTN_X:
        return QStringLiteral("X");
    case DSButton::DS_BTN_Y:
        return QStringLiteral("Y");
    case DSButton::DS_BTN_L:
        return QStringLiteral("L");
    case DSButton::DS_BTN_R:
        return QStringLiteral("R");
    case DSButton::DS_BTN_START:
        return QStringLiteral("Start");
    case DSButton::DS_BTN_SELECT:
        return QStringLiteral("Select");
    case DSButton::DS_BTN_UP:
        return QStringLiteral("Up");
    case DSButton::DS_BTN_DOWN:
        return QStringLiteral("Down");
    case DSButton::DS_BTN_LEFT:
        return QStringLiteral("Left");
    case DSButton::DS_BTN_RIGHT:
        return QStringLiteral("Right");
    }
    return QStringLiteral("?");
}

KeyBindings DefaultKeyBindings() {
    // Matches the bindings ds_player_window.cpp originally hardcoded —
    // changing these would silently remap existing users' muscle memory,
    // so they're carried over exactly rather than picked fresh.
    return KeyBindings{
        {DSButton::DS_BTN_A, Qt::Key_X},
        {DSButton::DS_BTN_B, Qt::Key_Z},
        {DSButton::DS_BTN_X, Qt::Key_S},
        {DSButton::DS_BTN_Y, Qt::Key_A},
        {DSButton::DS_BTN_L, Qt::Key_Q},
        {DSButton::DS_BTN_R, Qt::Key_W},
        {DSButton::DS_BTN_START, Qt::Key_Return},
        {DSButton::DS_BTN_SELECT, Qt::Key_Backspace},
        {DSButton::DS_BTN_UP, Qt::Key_Up},
        {DSButton::DS_BTN_DOWN, Qt::Key_Down},
        {DSButton::DS_BTN_LEFT, Qt::Key_Left},
        {DSButton::DS_BTN_RIGHT, Qt::Key_Right},
    };
}

KeyBindings LoadKeyBindings() {
    QSettings settings = OpenSettings();
    KeyBindings bindings = DefaultKeyBindings();

    settings.beginGroup(QStringLiteral("DSControls"));
    for (DSButton button : AllButtons()) {
        const QVariant stored = settings.value(SettingsKey(button));
        if (stored.isValid()) {
            bool ok = false;
            const int key = stored.toInt(&ok);
            if (ok) {
                bindings[button] = key;
            }
        }
    }
    settings.endGroup();

    return bindings;
}

void SaveKeyBindings(const KeyBindings& bindings) {
    QSettings settings = OpenSettings();

    settings.beginGroup(QStringLiteral("DSControls"));
    for (DSButton button : AllButtons()) {
        settings.setValue(SettingsKey(button), bindings.value(button, 0));
    }
    settings.endGroup();
    settings.sync();
}

int DefaultReturnToHomeMenuKey() {
    return Qt::Key_F12;
}

int LoadReturnToHomeMenuKey() {
    QSettings settings = OpenSettings();
    settings.beginGroup(QStringLiteral("DSControls"));
    const QVariant stored = settings.value(QStringLiteral("key_ReturnToHomeMenu"));
    settings.endGroup();

    if (stored.isValid()) {
        bool ok = false;
        const int key = stored.toInt(&ok);
        if (ok) {
            return key;
        }
    }
    return DefaultReturnToHomeMenuKey();
}

void SaveReturnToHomeMenuKey(int key) {
    QSettings settings = OpenSettings();
    settings.beginGroup(QStringLiteral("DSControls"));
    settings.setValue(QStringLiteral("key_ReturnToHomeMenu"), key);
    settings.endGroup();
    settings.sync();
}

} // namespace DSControlsConfig
