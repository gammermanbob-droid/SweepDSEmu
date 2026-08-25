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
#include <stdint.h>

// PSX pad bits, indexed by RETRO_DEVICE_ID_JOYPAD_* (see libretro.h) --
// input.c fills this in from hidKeysHeld() once per frame, core_glue.c's
// retro_input_state callback reads it.
#define PSX_MAX_BUTTONS 16
extern bool g_psxPad[PSX_MAX_BUTTONS];

// Set by core_glue.c's retro_video_refresh callback each frame the core
// actually renders one (data may be NULL on a "skip this frame" call,
// per libretro.h's retro_video_refresh_t doc comment -- video.c must
// leave the previous frame on screen in that case, not blank it).
typedef struct {
    const void* data;   // NULL if this frame was skipped
    unsigned width;
    unsigned height;
    size_t pitch;       // bytes per row; may exceed width * bytes-per-pixel
    unsigned bytesPerPixel; // 2 (RGB565/0RGB1555) or 4 (XRGB8888)
} PsxFrame;

// video.c
bool videoInit(void);
void videoExit(void);
void videoBeginFrame(void);
void videoPresentGameFrame(const PsxFrame* frame);
void videoDrawMenuText(const char* text, float x, float y, float scale);
void videoEndFrame(void);

// audio.c
bool audioInit(void);
void audioExit(void);
void audioSubmitSamples(const int16_t* interleavedStereo, size_t frames);

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
