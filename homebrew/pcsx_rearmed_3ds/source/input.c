// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Maps the 3DS's physical buttons onto both a PSX pad (g_psxPad, read by
// core_glue.c's retro_input_state callback) and simple menu-navigation
// edge triggers, mirroring the button-mapping-table shape
// DsEmulationActivity.kt's keyCodeToDsButton uses on the Android side of
// this project. START+SELECT held together opens the pause menu (see
// main.c) rather than the physical HOME button: 3DS homebrew has to
// explicitly opt in to intercepting HOME (aptSetHomeAllowed), and this
// avoids that extra layer of system-app interaction for a first
// version -- see the project README for taking that on later.

#include <3ds.h>

#include "libretro.h"
#include "psx3ds.h"

static u32 s_held;
static u32 s_down;
static bool s_pauseChordLatched;

void inputPoll(void) {
    hidScanInput();
    s_held = hidKeysHeld();
    s_down = hidKeysDown();

    g_psxPad[RETRO_DEVICE_ID_JOYPAD_UP] = (s_held & KEY_DUP) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_DOWN] = (s_held & KEY_DDOWN) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_LEFT] = (s_held & KEY_DLEFT) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_RIGHT] = (s_held & KEY_DRIGHT) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_A] = (s_held & KEY_A) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_B] = (s_held & KEY_B) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_X] = (s_held & KEY_X) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_Y] = (s_held & KEY_Y) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_L] = (s_held & KEY_L) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_R] = (s_held & KEY_R) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_L2] = (s_held & KEY_ZL) != 0; // New 3DS only
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_R2] = (s_held & KEY_ZR) != 0; // New 3DS only
    // START/SELECT double as the pause chord below -- still passed
    // through to the game normally when not held together, same as any
    // other PSX pad button.
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_START] = (s_held & KEY_START) != 0;
    g_psxPad[RETRO_DEVICE_ID_JOYPAD_SELECT] = (s_held & KEY_SELECT) != 0;

    bool chordHeld = (s_held & KEY_START) && (s_held & KEY_SELECT);
    if (chordHeld && !s_pauseChordLatched) {
        s_pauseChordLatched = true;
    } else if (!chordHeld) {
        s_pauseChordLatched = false;
    }
}

bool inputPausePressed(void) {
    return s_pauseChordLatched && (s_down & (KEY_START | KEY_SELECT)) != 0;
}

bool inputMenuBackPressed(void) {
    return (s_down & KEY_B) != 0;
}

bool inputMenuConfirmPressed(void) {
    return (s_down & KEY_A) != 0;
}

bool inputMenuUpPressed(void) {
    return (s_down & KEY_DUP) != 0;
}

bool inputMenuDownPressed(void) {
    return (s_down & KEY_DDOWN) != 0;
}

bool inputMenuSettingsPressed(void) {
    return (s_down & KEY_X) != 0;
}
