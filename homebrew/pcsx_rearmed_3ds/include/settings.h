// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Tiny persisted settings file (sdmc:/3ds/pcsx_rearmed_3ds/settings.ini)
// -- just the one BIOS preference for now. Loaded once at startup,
// written back out whenever menu.c's settings screen changes it.

#pragma once

#include <stdbool.h>

void settingsLoad(void);

// false (default): pcsx_rearmed's own "auto" BIOS mode -- tries a real
// BIOS dump from SYSTEM_DIR first, silently falls back to HLE if none
// is found. true: always use HLE, skipping the real-BIOS lookup (and
// its boot logo/splash) even if a real dump is present.
bool settingsGetForceHle(void);
void settingsSetForceHle(bool force);

// Which screen the game's picture renders on -- false (default): top
// screen, matching every other emulator on this project (DS, mGBA).
// true: bottom (touch) screen, for players who'd rather have the
// bigger/centered screen free for something else or just prefer it.
bool settingsGetDisplayOnBottom(void);
void settingsSetDisplayOnBottom(bool bottom);

// Whether the emulated pad reports as a DualShock (analog sticks, PSX's
// own analog-mode games) instead of a plain digital pad. Off by
// default -- most PS1 games work fine digital-only, and turning this on
// changes how some games read input entirely.
bool settingsGetAnalogMode(void);
void settingsSetAnalogMode(bool enabled);
