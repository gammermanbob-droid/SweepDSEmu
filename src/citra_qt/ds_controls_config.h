// src/citra_qt/ds_controls_config.h
//
// Keyboard bindings for DS sessions (ds_player_window.cpp). Kept
// separate from Azahar's own Settings::values/Config machinery — that
// system is built around the 3DS's much larger input surface
// (per-profile analog sticks, controllers, per-game overrides) and
// wiring a single set of 12 digital DS buttons through it would mean
// touching settings.h's serialization macros for a feature with none
// of that complexity. This reads/writes the same qt-config.ini file
// Azahar already uses, under its own [DSControls] group, so it still
// lives in one place from the user's perspective.

#pragma once

#include <QMap>

#include "core/button_id_ds.h"

namespace DSControlsConfig {

// Qt::Key values, keyed by DS button. Every MergedCore::DSButton is
// always present — LoadKeyBindings() fills in defaults for anything
// missing from the config file (first run, or a button added later).
using KeyBindings = QMap<MergedCore::DSButton, int>;

KeyBindings DefaultKeyBindings();
KeyBindings LoadKeyBindings();
void SaveKeyBindings(const KeyBindings& bindings);

// Short display name for a DS button ("A", "Start", "Left", ...).
QString ButtonName(MergedCore::DSButton button);

// All twelve buttons, in the order they should be presented/iterated.
const QList<MergedCore::DSButton>& AllButtons();

// The "return to 3DS HOME Menu" hotkey — not a DS button (it closes the
// DS session entirely), so it's stored and exposed separately from
// KeyBindings/AllButtons rather than folded into MergedCore::DSButton,
// which is a real hardware bitmask used to drive the emulated console.
int DefaultReturnToHomeMenuKey();
int LoadReturnToHomeMenuKey();
void SaveReturnToHomeMenuKey(int key);

} // namespace DSControlsConfig
