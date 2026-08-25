// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <stdbool.h>

// Renders and drives the SD-card file browser (bottom screen) until the
// user picks a disc image or asks to quit the app. Returns a
// heap-allocated absolute path (caller frees it) on selection, or NULL
// if the user chose to quit.
char* menuBrowseForGame(void);

// Renders and drives the pause menu opened by the START+SELECT chord
// during gameplay (see input.c). Returns true if the caller should
// return to the file browser (either the user chose "Quit to Menu" and
// confirmed, or "Quit App" -- both unload the current game first), or
// false to just resume play.
bool menuPause(void);
