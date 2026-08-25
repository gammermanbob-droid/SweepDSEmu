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
