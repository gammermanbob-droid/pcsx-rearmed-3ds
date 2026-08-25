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
#include <stdio.h>
#include <stdlib.h>
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

// Boot chime / menu music playback -- a whole-file-at-once alternative
// to the streaming path above, used only outside gameplay (see
// audioPlayClip's own comment for why sharing AUDIO_CHANNEL is safe).
static ndspWaveBuf s_clipWaveBuf;
static int16_t* s_clipData;

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
    audioStopClip();
    ndspChnWaveBufClear(AUDIO_CHANNEL);
    ndspExit();
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (s_bufferData[i]) {
            linearFree(s_bufferData[i]);
            s_bufferData[i] = NULL;
        }
    }
}

void audioResetForGameplay(void) {
    // A menu clip may have left the channel at its own sample rate
    // (see audioPlayClip) -- PS1 audio is always 44100Hz, but reading
    // it back from the core rather than hardcoding keeps this in sync
    // with coreSampleRate() if that's ever wrong.
    ndspChnSetRate(AUDIO_CHANNEL, (float)coreSampleRate());
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

// Boot chime (main.c, once) and looping menu music (menu.c, while
// browsing) both play on AUDIO_CHANNEL -- the same channel game audio
// streams to, but never at the same time as a game: nothing calls
// audioSubmitSamples() outside runGame()'s loop, and runGame() only
// starts after menu.c's browser screen (where the music plays) has
// already returned a ROM to load, so there's no real overlap to guard
// against.
bool audioPlayClip(const char* romfsPath, float sampleRate, bool looping) {
    audioStopClip();

    FILE* f = fopen(romfsPath, "rb");
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

    s_clipData = (int16_t*)linearAlloc((size_t)size);
    if (!s_clipData) {
        fclose(f);
        return false;
    }
    size_t read = fread(s_clipData, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        linearFree(s_clipData);
        s_clipData = NULL;
        return false;
    }

    ndspChnSetRate(AUDIO_CHANNEL, sampleRate);

    memset(&s_clipWaveBuf, 0, sizeof(s_clipWaveBuf));
    s_clipWaveBuf.data_vaddr = s_clipData;
    s_clipWaveBuf.nsamples = (u32)(size / (2 * sizeof(int16_t))); // stereo S16
    s_clipWaveBuf.looping = looping;
    DSP_FlushDataCache(s_clipData, (u32)size);
    ndspChnWaveBufAdd(AUDIO_CHANNEL, &s_clipWaveBuf);
    return true;
}

void audioStopClip(void) {
    if (!s_clipData) {
        return;
    }
    // A full channel reset rather than just ndspChnWaveBufClear() --
    // still documented to "stop playback" on its own, but a looping
    // clip (see audioPlayClip) was still reportedly audible after a
    // game got picked, so this reaches for the heavier, unambiguous
    // stop instead of trusting the lighter one a second time.
    // ndspChnReset() also clears format/rate/interpolation, so those
    // get re-applied here rather than left for the next caller
    // (audioResetForGameplay or another audioPlayClip) to discover
    // they're missing.
    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    linearFree(s_clipData);
    s_clipData = NULL;
}

bool audioClipFinished(void) {
    return !s_clipData || s_clipWaveBuf.status == NDSP_WBUF_DONE;
}

unsigned audioBufferOccupancyPercent(void) {
    int queued = 0;
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (s_waveBufs[i].status != NDSP_WBUF_DONE) {
            ++queued;
        }
    }
    return (unsigned)(queued * 100 / NUM_BUFFERS);
}
