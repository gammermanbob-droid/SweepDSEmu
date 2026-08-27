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

// Serialized Common::ParamPackage strings (as produced by Azahar's
// existing InputCommon::Polling machinery — same format the 3DS side's
// Configure Controls dialog uses), keyed by DS button. Unlike
// KeyBindings, a button missing here (empty string) just means
// "no controller binding" — a controller is optional, so there's no
// sensible non-empty default the way there is for the keyboard.
using ControllerBindings = QMap<MergedCore::DSButton, QString>;

ControllerBindings LoadControllerBindings();
void SaveControllerBindings(const ControllerBindings& bindings);

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

// Optional controller binding for the same hotkey (serialized
// Common::ParamPackage, same format as ControllerBindings above).
// Kept as a plain string rather than folded into ControllerBindings
// for the same reason the keyboard version is separate from
// KeyBindings — this isn't a MergedCore::DSButton. A controller
// binding matters more here than it does for ordinary DS buttons: a
// physical controller's Home/Guide button is frequently intercepted
// by the OS before a *keyboard*-style binding would ever see it (see
// ds_player_window.cpp), so this is often the only way to actually
// use it from a controller.
QString LoadHomeMenuControllerBinding();
void SaveHomeMenuControllerBinding(const QString& serialized);

// Not a control binding (stored under its own [DSGameplay] group, not
// [DSControls]) but kept in this file rather than a new one for a single
// boolean. Auto-saves a savestate when a DS session closes and
// auto-loads it the next time that same ROM opens, independent of the
// game's own cart save data. On by default.
bool LoadAutoSaveState();
void SaveAutoSaveState(bool enabled);

} // namespace DSControlsConfig
