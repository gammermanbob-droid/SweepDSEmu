// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// App entry point. Owns the top-level state machine (browse -> play ->
// pause -> back to browse), same shape as mGBA-3ds's main() driving its
// mGUIRunner, just without that shared library backing it -- see
// menu.c's doc comment for why this project has its own small
// menu/pause implementation instead.

#include <3ds.h>
#include <math.h>
#include <stdlib.h>

#include "menu.h"
#include "psx3ds.h"
#include "settings.h"

// devkitARM's default main-thread stack for a 3dsx/CIA homebrew app is
// only 32KB (__stacksize__'s weak default from libctru's own startup
// code) -- pcsx_rearmed's own log_mem_usage() checks this at startup
// and logs "past OOM detected, expect instability" if it's under 1MB,
// which is exactly what showed up in this app's own log after
// installing. A plain, non-weak global of the same name overrides
// that default (standard ELF weak-symbol resolution; matches
// DSEmulationActivity.kt's setStackSize() comment for the same class
// of problem on the Android/DS side of this project -- deep JIT
// compiler call chains need more than the platform default). 2MB is
// comfortably above pcsx_rearmed's own 1MB floor.
u32 __stacksize__ = 2 * 1024 * 1024;

static void runGame(const char* path) {
    if (!coreLoad(path)) {
        return;
    }

    bool analogEnabled = settingsGetAnalogMode();
    osSetSpeedupEnable(true); // New 3DS CPU clock boost while actually playing
    bool quit = false;
    while (!quit && aptMainLoop()) {
        inputPoll();

        if (inputPausePressed()) {
            quit = menuPause();
            continue;
        }

        // The ANALOG toggle is always live during gameplay (not tucked
        // away in the settings screen) -- matches a real DualShock's
        // own physical ANALOG button, which works the same way whether
        // or not a menu is open.
        int tx, ty;
        if (inputTouchTapped(&tx, &ty)) {
            float dx = tx - kAnalogToggleX, dy = ty - kAnalogToggleY;
            if (sqrtf(dx * dx + dy * dy) <= kAnalogToggleRadius) {
                analogEnabled = !analogEnabled;
                settingsSetAnalogMode(analogEnabled);
                coreSetAnalogMode(analogEnabled);
            }
        }

        // coreRunFrame() -> retro_run() -> the retro_video_refresh
        // callback -> videoPresentGameFrame() runs synchronously inside
        // this call, uploading the new frame into s_gameTex before we
        // draw it below.
        coreRunFrame();

        videoBeginFrame(true);
        videoDrawAnalogToggle(analogEnabled);
        videoEndFrame();
    }
    osSetSpeedupEnable(false);

    coreUnload();
}

int main(void) {
    settingsLoad();

    if (!videoInit()) {
        return 1;
    }
    if (!audioInit()) {
        videoExit();
        return 1;
    }

    while (aptMainLoop()) {
        char* gamePath = menuBrowseForGame();
        if (!gamePath) {
            break; // user chose to quit from the browser
        }
        runGame(gamePath);
        free(gamePath);
    }

    audioExit();
    videoExit();
    return 0;
}
