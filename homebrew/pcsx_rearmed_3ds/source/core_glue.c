// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Implements the handful of callbacks any libretro frontend must
// provide (video/audio/input/environment) and drives
// pcsx_rearmed_libretro_ctr.a's retro_* entry points -- standing in for
// RetroArch, which is what actually calls these on every other
// platform. We only implement what pcsx_rearmed's frontend/libretro.c
// actually asks for (checked directly against its source rather than
// guessing); everything else returns false/unhandled, which is the
// correct, expected behavior for any environment command a frontend
// doesn't support.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libretro.h"
#include "psx3ds.h"

bool g_psxPad[PSX_MAX_BUTTONS];

static enum retro_pixel_format s_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static const struct retro_core_option_definition* s_coreOptions;
static double s_targetFps = 60.0;
static double s_sampleRate = 44100.0;

#define SYSTEM_DIR "sdmc:/3ds/pcsx_rearmed_3ds/system"
#define SAVE_DIR   "sdmc:/3ds/pcsx_rearmed_3ds/saves"

static void logPrintf(enum retro_log_level level, const char* fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    // No visible console during gameplay -- mirrors ds_native.cpp's
    // LOG_ERROR-to-file approach rather than trying to print over the
    // framebuffer we're also drawing to.
    FILE* f = fopen("sdmc:/3ds/pcsx_rearmed_3ds/log.txt", "a");
    if (f) {
        vfprintf(f, fmt, ap);
        fclose(f);
    }
    va_end(ap);
}

static bool findCoreOptionDefault(const char* key, const char** outValue) {
    if (!s_coreOptions) {
        return false;
    }
    for (const struct retro_core_option_definition* opt = s_coreOptions; opt->key; ++opt) {
        if (strcmp(opt->key, key) == 0) {
            *outValue = opt->default_value;
            return *outValue != NULL;
        }
    }
    return false;
}

static bool environCallback(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool*)data = true;
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        s_pixelFormat = *(const enum retro_pixel_format*)data;
        // video.c only knows how to unpack 0RGB1555/RGB565 (2 bytes) and
        // XRGB8888 (4 bytes) -- both of which pcsx_rearmed can produce,
        // so there's nothing to reject here.
        return true;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/pcsx_rearmed_3ds", 0777);
        mkdir(SYSTEM_DIR, 0777);
        *(const char**)data = SYSTEM_DIR;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/pcsx_rearmed_3ds", 0777);
        mkdir(SAVE_DIR, 0777);
        *(const char**)data = SAVE_DIR;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback*)data)->log = logPrintf;
        return true;

    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        // The v1 (non-_V2, non-_INTL) form -- an array of
        // retro_core_option_definition terminated by a NULL key. This is
        // the one pcsx_rearmed's negotiation cascade falls back to once
        // _V2/_V2_INTL/_INTL are declined below.
        s_coreOptions = (const struct retro_core_option_definition*)data;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable* var = (struct retro_variable*)data;
        if (strcmp(var->key, "pcsx_rearmed_bios") == 0) {
            // Force HLE BIOS: matches the "a ROM just boots, no extra
            // setup" experience this project already committed to for
            // melonDS's FreeBIOS fallback -- users shouldn't need to
            // dump their own PS1 BIOS just to get a game running. A
            // real BIOS dropped in SYSTEM_DIR still works fine later
            // via the in-game options menu once one exists.
            var->value = "HLE";
            return true;
        }
        return findCoreOptionDefault(var->key, &var->value);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool*)data = false;
        return true;

    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;

    default:
        return false;
    }
}

static void videoRefreshCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
    PsxFrame frame = {
        .data = data,
        .width = width,
        .height = height,
        .pitch = pitch,
        .bytesPerPixel = (s_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) ? 4u : 2u,
    };
    videoPresentGameFrame(&frame);
}

static void audioSampleCallback(int16_t left, int16_t right) {
    int16_t frame[2] = {left, right};
    audioSubmitSamples(frame, 1);
}

static size_t audioSampleBatchCallback(const int16_t* data, size_t frames) {
    audioSubmitSamples(data, frames);
    return frames;
}

static void inputPollCallback(void) {
    // Actual hidScanInput() happens once per app frame in main.c's loop
    // (before coreRunFrame()), not here -- retro_run() can call
    // input_poll_cb multiple times in principle, and polling the
    // hardware twice in one frame would be wasteful and could miss a
    // press between the two scans.
}

static int16_t inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= PSX_MAX_BUTTONS) {
        return 0;
    }
    return g_psxPad[id] ? 1 : 0;
}

bool coreLoad(const char* path) {
    retro_set_environment(environCallback);
    retro_init();

    retro_set_video_refresh(videoRefreshCallback);
    retro_set_audio_sample(audioSampleCallback);
    retro_set_audio_sample_batch(audioSampleBatchCallback);
    retro_set_input_poll(inputPollCallback);
    retro_set_input_state(inputStateCallback);

    struct retro_game_info info = {
        .path = path,
        .data = NULL, // PS1 discs are streamed from disk (cdriso.c), not preloaded
        .size = 0,
        .meta = NULL,
    };
    if (!retro_load_game(&info)) {
        retro_deinit();
        return false;
    }

    struct retro_system_av_info avInfo;
    retro_get_system_av_info(&avInfo);
    s_targetFps = avInfo.timing.fps > 0 ? avInfo.timing.fps : 60.0;
    s_sampleRate = avInfo.timing.sample_rate > 0 ? avInfo.timing.sample_rate : 44100.0;
    return true;
}

void coreUnload(void) {
    retro_unload_game();
    retro_deinit();
}

void coreRunFrame(void) {
    retro_run();
}

double coreTargetFps(void) {
    return s_targetFps;
}

double coreSampleRate(void) {
    return s_sampleRate;
}

bool coreSerialize(const char* savestatePath) {
    size_t size = retro_serialize_size();
    if (size == 0) {
        return false;
    }
    void* buf = malloc(size);
    if (!buf) {
        return false;
    }
    bool ok = retro_serialize(buf, size);
    if (ok) {
        FILE* f = fopen(savestatePath, "wb");
        if (f) {
            ok = fwrite(buf, 1, size, f) == size;
            fclose(f);
        } else {
            ok = false;
        }
    }
    free(buf);
    return ok;
}

bool coreUnserialize(const char* savestatePath) {
    FILE* f = fopen(savestatePath, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    void* buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return false;
    }
    bool ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (ok) {
        ok = retro_unserialize(buf, (size_t)size);
    }
    free(buf);
    return ok;
}
