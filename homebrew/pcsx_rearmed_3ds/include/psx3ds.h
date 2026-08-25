// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Shared types passed between core_glue.c (the libretro frontend
// implementation driving pcsx_rearmed_libretro_ctr.a), video.c, audio.c
// and input.c. Kept in one header since these four files are the whole
// "frontend" -- there's no need for each to have its own tiny header.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PSX pad bits, indexed by RETRO_DEVICE_ID_JOYPAD_* (see libretro.h) --
// input.c fills this in from hidKeysHeld() once per frame, core_glue.c's
// retro_input_state callback reads it.
#define PSX_MAX_BUTTONS 16
extern bool g_psxPad[PSX_MAX_BUTTONS];

// Left stick X/Y, right stick X/Y -- indices match libretro.h's
// RETRO_DEVICE_INDEX_ANALOG_LEFT/RIGHT * 2 + RETRO_DEVICE_ID_ANALOG_X/Y,
// full int16 range (matches retro_input_state_t's own return range).
// Only meaningful once analog mode is on (see coreTriggerAnalogToggle) --
// pcsx_rearmed ignores these entirely in plain digital-pad mode.
#define PSX_ANALOG_LEFT_X 0
#define PSX_ANALOG_LEFT_Y 1
#define PSX_ANALOG_RIGHT_X 2
#define PSX_ANALOG_RIGHT_Y 3
extern int16_t g_psxAnalog[4];

// Set by core_glue.c's retro_video_refresh callback each frame the core
// actually renders one (data may be NULL on a "skip this frame" call,
// per libretro.h's retro_video_refresh_t doc comment -- video.c must
// leave the previous frame on screen in that case, not blank it).
// Matches libretro.h's enum retro_pixel_format values exactly (0RGB1555=0,
// XRGB8888=1, RGB565=2) -- video.c needs to know which of the two 16-bit
// layouts it's looking at, not just that it's 2 bytes wide; RGB565 and
// 0RGB1555 pack their bits completely differently and mixing them up
// scrambles every pixel's colors.
typedef struct {
    const void* data;   // NULL if this frame was skipped
    unsigned width;
    unsigned height;
    size_t pitch;       // bytes per row; may exceed width * bytes-per-pixel
    int pixelFormat;     // 0 = 0RGB1555, 1 = XRGB8888, 2 = RGB565
} PsxFrame;

// video.c
bool videoInit(void);
void videoExit(void);
// gameplayActive: true only for main.c's active-play loop (see
// settingsGetDisplayOnBottom) -- always false from the file browser,
// pause, and settings screens.
void videoBeginFrame(bool gameplayActive);
void videoPresentGameFrame(const PsxFrame* frame);
void videoDrawMenuText(const char* text, float x, float y, float scale);
void videoDrawAnalogToggle(bool enabled);
// Boot screen animation, drawn on the top screen while main.c's boot
// chime plays -- frame is just a monotonically increasing counter (one
// full videoBeginFrame/videoEndFrame cycle apart, so effectively a
// ~60fps clock) the caller keeps, not a wall-clock timestamp.
void videoDrawBootLogo(int frame);
void videoEndFrame(void);

// Bottom-screen (320x240) position of the always-visible in-game
// ANALOG toggle -- bottom-center, matching where the physical HOME
// button sits just below a real 3DS's bottom screen. Shared between
// video.c (drawing) and main.c (touch hit-testing) so they can't drift
// apart.
#define kAnalogToggleX 160.0f
#define kAnalogToggleY 208.0f
#define kAnalogToggleRadius 30.0f

// audio.c
bool audioInit(void);
void audioExit(void);
void audioSubmitSamples(const int16_t* interleavedStereo, size_t frames);
// Restores the game-audio sample rate after a menu clip (see
// audioPlayClip) may have left the shared channel at its own rate --
// call once gameplay actually starts.
void audioResetForGameplay(void);
// Loads a whole romfs-embedded raw S16LE stereo PCM file and starts it
// playing immediately, replacing whatever clip (if any) was already
// playing. Returns false (and plays nothing) if the file can't be
// opened/read/allocated.
bool audioPlayClip(const char* romfsPath, float sampleRate, bool looping);
void audioStopClip(void);
// True once a non-looping clip has finished, or if nothing is playing;
// always false for a looping clip (see audioPlayClip).
bool audioClipFinished(void);

// input.c
void inputPoll(void);
// True the frame the pause chord (START+SELECT) first becomes held.
bool inputPausePressed(void);
// True the frame the physical B button is pressed (menu "back").
bool inputMenuBackPressed(void);
// True the frame the physical A button is pressed (menu "select").
bool inputMenuConfirmPressed(void);
// Raw physical-button d-pad edge triggers, for menu navigation.
bool inputMenuUpPressed(void);
bool inputMenuDownPressed(void);
// X button -- opens the settings screen from the file browser.
bool inputMenuSettingsPressed(void);

// True the frame the touch screen was first pressed this frame (not
// held from a previous frame), with the tap position in raw bottom-
// screen pixel coordinates (0,0 top-left, 320x240).
bool inputTouchTapped(int* x, int* y);

// core_glue.c
bool coreLoad(const char* path);
void coreUnload(void);
// Runs exactly one emulated frame; video/audio callbacks fire from
// inside this call.
void coreRunFrame(void);
bool coreSerialize(const char* savestatePath);
bool coreUnserialize(const char* savestatePath);
double coreTargetFps(void);
double coreSampleRate(void);
const char* coreCurrentGameName(void);
void coreSaveStatePath(char* buf, size_t bufSize);
// Flips analog mode by simulating the L3+R3 combo pcsx_rearmed's own
// analog_combo option is forced to watch for, held for exactly one
// input poll -- an instant, in-engine flag flip (see padToggleAnalog
// in libpcsxcore/pad.c), matching how a real DualShock's own ANALOG
// button works. Deliberately *not* implemented by reconfiguring the
// pad type -- see coreLoad's own comment for why that caused a
// noticeable delay instead.
void coreTriggerAnalogToggle(void);
