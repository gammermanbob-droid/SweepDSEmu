// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// A deliberately simple file browser + pause menu, scoped down from
// what mGBA's mGUI-based menu system offers (no theming, no on-screen
// keyboard search, no per-game settings) -- this project doesn't have
// mGBA's shared cross-platform GUI library to build on, so this is a
// hand-rolled equivalent covering just what's needed: list games,
// pick one, and a pause screen with the same "confirm before closing"
// shape as this project's Android DS player (see
// DsEmulationActivity.kt's confirmReturnToThreeDsHomeMenu).

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "menu.h"
#include "psx3ds.h"
#include "settings.h"

#define ROMS_ROOT "sdmc:/roms/psx"
#define MAX_ENTRIES 512
#define MAX_PATH_LEN 512

typedef struct {
    char path[MAX_PATH_LEN]; // full path, for loading
    char label[64];          // what's shown in the list
} MenuEntry;

static MenuEntry s_entries[MAX_ENTRIES];
static int s_entryCount;

static bool hasDiscExtension(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    static const char* const exts[] = {".cue", ".bin", ".chd", ".pbp", ".iso", ".img"};
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void addEntry(const char* fullPath, const char* label) {
    if (s_entryCount >= MAX_ENTRIES) {
        return;
    }
    MenuEntry* e = &s_entries[s_entryCount++];
    snprintf(e->path, sizeof(e->path), "%s", fullPath);
    snprintf(e->label, sizeof(e->label), "%s", label);
}

// Scans ROMS_ROOT plus exactly one level of subdirectories -- covers
// both a flat dump of images directly in ROMS_ROOT and the very common
// "one folder per game, containing its .cue/.bin" layout, without the
// complexity of full interactive directory navigation (a reasonable
// scope cut for a first version -- see the project checklist).
static void scanGames(void) {
    s_entryCount = 0;
    mkdir("sdmc:/roms", 0777);
    mkdir(ROMS_ROOT, 0777);

    DIR* root = opendir(ROMS_ROOT);
    if (!root) {
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(root)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char fullPath[MAX_PATH_LEN];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", ROMS_ROOT, ent->d_name);

        struct stat st;
        if (stat(fullPath, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            DIR* sub = opendir(fullPath);
            if (!sub) {
                continue;
            }
            struct dirent* subEnt;
            while ((subEnt = readdir(sub)) != NULL) {
                if (subEnt->d_name[0] == '.' || !hasDiscExtension(subEnt->d_name)) {
                    continue;
                }
                char subPath[MAX_PATH_LEN];
                snprintf(subPath, sizeof(subPath), "%s/%s", fullPath, subEnt->d_name);
                char label[64];
                snprintf(label, sizeof(label), "%s/%s", ent->d_name, subEnt->d_name);
                addEntry(subPath, label);
            }
            closedir(sub);
        } else if (hasDiscExtension(ent->d_name)) {
            addEntry(fullPath, ent->d_name);
        }
    }
    closedir(root);
}

char* menuBrowseForGame(void) {
    scanGames();

    int selected = 0;
    while (aptMainLoop()) {
        inputPoll();

        if (inputMenuUpPressed() && s_entryCount > 0) {
            selected = (selected - 1 + s_entryCount) % s_entryCount;
        }
        if (inputMenuDownPressed() && s_entryCount > 0) {
            selected = (selected + 1) % s_entryCount;
        }
        if (inputMenuConfirmPressed() && s_entryCount > 0) {
            char* result = strdup(s_entries[selected].path);
            return result;
        }
        if (inputMenuBackPressed()) {
            return NULL; // quit the app
        }
        if (inputMenuSettingsPressed()) {
            menuSettings();
            continue;
        }

        videoBeginFrame();
        if (s_entryCount == 0) {
            videoDrawMenuText("No PS1 discs found.", 8, 8, 0.5f);
            videoDrawMenuText("Put .cue/.bin/.chd/.pbp/.iso files in", 8, 28, 0.42f);
            videoDrawMenuText(ROMS_ROOT, 8, 44, 0.42f);
            videoDrawMenuText("(one folder per game is fine too)", 8, 60, 0.42f);
        } else {
            float y = 8;
            // Keep the selected entry roughly centered rather than
            // scrolling the whole list from the top -- simplest way to
            // keep a long library navigable on a 240px-tall screen.
            int firstVisible = selected - 5;
            if (firstVisible < 0) {
                firstVisible = 0;
            }
            int lastVisible = firstVisible + 11;
            if (lastVisible >= s_entryCount) {
                lastVisible = s_entryCount - 1;
                firstVisible = lastVisible - 11;
                if (firstVisible < 0) {
                    firstVisible = 0;
                }
            }
            for (int i = firstVisible; i <= lastVisible; ++i) {
                char line[80];
                snprintf(line, sizeof(line), "%s %s", i == selected ? ">" : " ", s_entries[i].label);
                videoDrawMenuText(line, 8, y, 0.45f);
                y += 18;
            }
        }
        videoDrawMenuText("A: Play   B: Quit App   X: Settings", 8, 220, 0.4f);
        videoEndFrame();
    }
    return NULL;
}

bool menuPause(void) {
    enum { OPT_RESUME, OPT_SAVE_STATE, OPT_LOAD_STATE, OPT_CHANGE_DISC, OPT_COUNT };
    static const char* const kOptions[OPT_COUNT] = {
        "Resume", "Save State", "Load State", "Change Disc",
    };

    int selected = OPT_RESUME;
    bool confirmingQuit = false;
    char status[64] = "";

    while (aptMainLoop()) {
        inputPoll();

        if (confirmingQuit) {
            if (inputMenuConfirmPressed()) {
                return true; // confirmed: caller unloads and returns to the browser
            }
            if (inputMenuBackPressed()) {
                confirmingQuit = false;
            }
        } else {
            if (inputMenuUpPressed()) {
                selected = (selected - 1 + OPT_COUNT) % OPT_COUNT;
            }
            if (inputMenuDownPressed()) {
                selected = (selected + 1) % OPT_COUNT;
            }
            if (inputMenuBackPressed()) {
                return false; // B also just resumes, matching most pause menus
            }
            if (inputMenuConfirmPressed()) {
                char path[512];
                switch (selected) {
                case OPT_RESUME:
                    return false;
                case OPT_SAVE_STATE:
                    coreSaveStatePath(path, sizeof(path));
                    snprintf(status, sizeof(status), "%s",
                        coreSerialize(path) ? "State saved." : "Save failed.");
                    break;
                case OPT_LOAD_STATE:
                    coreSaveStatePath(path, sizeof(path));
                    snprintf(status, sizeof(status), "%s",
                        coreUnserialize(path) ? "State loaded." : "No state to load.");
                    break;
                case OPT_CHANGE_DISC:
                    confirmingQuit = true;
                    break;
                }
            }
        }

        videoBeginFrame();
        videoDrawMenuText("Paused", 8, 8, 0.6f);
        if (confirmingQuit) {
            videoDrawMenuText("Return to the game list?", 8, 40, 0.5f);
            videoDrawMenuText("Unsaved progress will be lost --", 8, 58, 0.42f);
            videoDrawMenuText("use Save State first if you need it.", 8, 74, 0.42f);
            videoDrawMenuText("A: Confirm   B: Cancel", 8, 220, 0.42f);
        } else {
            float y = 40;
            for (int i = 0; i < OPT_COUNT; ++i) {
                char line[64];
                snprintf(line, sizeof(line), "%s %s", i == selected ? ">" : " ", kOptions[i]);
                videoDrawMenuText(line, 8, y, 0.5f);
                y += 20;
            }
            if (status[0]) {
                videoDrawMenuText(status, 8, y + 12, 0.42f);
            }
            videoDrawMenuText("A: Select   B: Resume", 8, 220, 0.42f);
        }
        videoEndFrame();
    }
    return true;
}

void menuSettings(void) {
    while (aptMainLoop()) {
        inputPoll();

        if (inputMenuConfirmPressed() || inputMenuUpPressed() || inputMenuDownPressed()) {
            settingsSetForceHle(!settingsGetForceHle());
        }
        if (inputMenuBackPressed()) {
            return;
        }

        videoBeginFrame();
        videoDrawMenuText("Settings", 8, 8, 0.6f);
        videoDrawMenuText("BIOS mode:", 8, 44, 0.5f);
        videoDrawMenuText(settingsGetForceHle() ? "> Force HLE (no real BIOS)" :
            "> Auto (use a real BIOS if present)", 8, 64, 0.45f);
        videoDrawMenuText("A real BIOS dump goes in:", 8, 96, 0.42f);
        videoDrawMenuText("sdmc:/3ds/pcsx_rearmed_3ds/system/", 8, 112, 0.42f);
        videoDrawMenuText("(e.g. scph1001.bin) -- Auto finds it there", 8, 128, 0.38f);
        videoDrawMenuText("and falls back to HLE if it's missing.", 8, 142, 0.38f);
        videoDrawMenuText("A/Up/Down: Toggle   B: Back", 8, 220, 0.42f);
        videoEndFrame();
    }
}
