// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Double-buffered NDSP output for the interleaved stereo PCM16 samples
// pcsx_rearmed hands us via retro_audio_sample_batch. Parallel to
// ds_native.cpp's AAudio path on the Android side of this project --
// same "one ring of a couple hardware buffers, non-blocking submit"
// shape, just on 3DS's NDSP instead.

#include <3ds.h>
#include <string.h>

#include "psx3ds.h"

#define AUDIO_CHANNEL 0
#define NUM_BUFFERS 4
// A little over one PS1 audio frame's worth (44100Hz / ~60fps ~= 735
// frames) per hardware buffer -- small enough to keep latency low,
// large enough that a single ndspChnWaveBufAdd per emulated frame is
// enough (no mid-frame buffer starvation from an odd sample count).
#define SAMPLES_PER_BUFFER 1024

static ndspWaveBuf s_waveBufs[NUM_BUFFERS];
static int16_t* s_bufferData[NUM_BUFFERS];
static int s_nextBuffer;
static size_t s_writeOffsetFrames; // frames (L+R pairs) already filled in the current buffer

bool audioInit(void) {
    if (R_FAILED(ndspInit())) {
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(AUDIO_CHANNEL, (float)coreSampleRate());

    memset(s_waveBufs, 0, sizeof(s_waveBufs));
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        s_bufferData[i] = (int16_t*)linearAlloc(SAMPLES_PER_BUFFER * 2 * sizeof(int16_t));
        if (!s_bufferData[i]) {
            return false;
        }
        s_waveBufs[i].data_vaddr = s_bufferData[i];
        s_waveBufs[i].nsamples = 0;
        s_waveBufs[i].looping = false;
        s_waveBufs[i].status = NDSP_WBUF_DONE;
    }
    s_nextBuffer = 0;
    s_writeOffsetFrames = 0;
    return true;
}

void audioExit(void) {
    ndspChnWaveBufClear(AUDIO_CHANNEL);
    ndspExit();
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (s_bufferData[i]) {
            linearFree(s_bufferData[i]);
            s_bufferData[i] = NULL;
        }
    }
}

static void flushCurrentBuffer(void) {
    if (s_writeOffsetFrames == 0) {
        return;
    }
    ndspWaveBuf* buf = &s_waveBufs[s_nextBuffer];
    buf->nsamples = s_writeOffsetFrames;
    buf->status = NDSP_WBUF_FREE;
    DSP_FlushDataCache(buf->data_vaddr, s_writeOffsetFrames * 2 * sizeof(int16_t));
    ndspChnWaveBufAdd(AUDIO_CHANNEL, buf);

    s_nextBuffer = (s_nextBuffer + 1) % NUM_BUFFERS;
    s_writeOffsetFrames = 0;
}

void audioSubmitSamples(const int16_t* interleavedStereo, size_t frames) {
    size_t srcOffset = 0;
    while (srcOffset < frames) {
        // Skip a buffer that's still queued for playback rather than
        // stall the emulation thread waiting on it -- an occasional
        // dropped chunk of audio is far less disruptive than blocking
        // the whole emulated frame on the DSP catching up (same
        // tradeoff ds_native.cpp's AAudio path makes).
        if (s_waveBufs[s_nextBuffer].status != NDSP_WBUF_DONE &&
            s_waveBufs[s_nextBuffer].status != NDSP_WBUF_FREE) {
            return;
        }

        size_t room = SAMPLES_PER_BUFFER - s_writeOffsetFrames;
        size_t copyFrames = frames - srcOffset;
        if (copyFrames > room) {
            copyFrames = room;
        }

        int16_t* dst = s_bufferData[s_nextBuffer] + s_writeOffsetFrames * 2;
        memcpy(dst, interleavedStereo + srcOffset * 2, copyFrames * 2 * sizeof(int16_t));
        s_writeOffsetFrames += copyFrames;
        srcOffset += copyFrames;

        if (s_writeOffsetFrames == SAMPLES_PER_BUFFER) {
            flushCurrentBuffer();
        }
    }
}
