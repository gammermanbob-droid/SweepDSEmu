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
#include <stdlib.h>

#include "menu.h"
#include "psx3ds.h"
#include "settings.h"

static void runGame(const char* path) {
    if (!coreLoad(path)) {
        return;
    }

    osSetSpeedupEnable(true); // New 3DS CPU clock boost while actually playing
    bool quit = false;
    while (!quit && aptMainLoop()) {
        inputPoll();

        if (inputPausePressed()) {
            quit = menuPause();
            continue;
        }

        // coreRunFrame() -> retro_run() -> the retro_video_refresh
        // callback -> videoPresentGameFrame() runs synchronously inside
        // this call, uploading the new frame into s_gameTex before we
        // draw it below.
        coreRunFrame();

        videoBeginFrame();
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
