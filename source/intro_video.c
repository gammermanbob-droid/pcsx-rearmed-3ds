// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Plays romfs:/intro_video.h264 (a small Annex-B H.264 elementary
// stream produced offline via ffmpeg -- see audio_src/README.md) using
// the New3DS's own hardware MVD video decoder (3ds/services/mvd.h),
// rather than a software H.264 decoder: at ARM11/268-804MHz there's no
// realistic way to software-decode video in real time, but the actual
// New3DS Internet Browser leans on this exact same hardware block for
// its own video playback, so it's a real, proven decode path -- just
// one devkitPro's own shipped example (examples/3ds/mvd/source/main.c)
// was the only concrete reference available for its NAL-unit-splitting
// and render-loop conventions, there being no wrapper library for it.
// Every entry point here fails closed (returns false / sets "done") on
// anything unexpected, since main.c's only fallback on failure is the
// already-working audio+logo boot screen -- there's no in-between
// "partially working video" state worth trying to patch together.

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx3ds.h"

#define INTRO_WIDTH 400
#define INTRO_HEIGHT 224
// Matches the encode's own -r 20 (see audio_src/README.md) -- held for
// this many ~60Hz vsync ticks per decoded picture so playback runs at
// the source's actual speed instead of however fast MVD can chew
// through NAL units.
#define INTRO_TICKS_PER_FRAME 3

// A single baseline-profile slice at 400x224 is at most a few KB; this
// is generous headroom over that for the occasional larger keyframe.
#define NALU_SCRATCH_SIZE (512 * 1024)
#define MAX_NALUS 4096

typedef struct {
    u32 offset;
    u32 length;
} NaluRange;

static u8* s_fileData;
static size_t s_fileSize;
static u16* s_outBuf;      // linearmem, MVD's RGB565 render target
static u8* s_naluScratch;  // linearmem staging buffer: 3-byte start code + one NAL's payload

static NaluRange s_nalus[MAX_NALUS];
static int s_naluCount;
static int s_naluCursor;

static bool s_mvdReady;
static bool s_done;
static int s_holdTicks;

static bool findStartCode(size_t from, size_t* outPos) {
    if (from + 3 > s_fileSize) {
        return false;
    }
    for (size_t i = from; i + 3 <= s_fileSize; ++i) {
        if (s_fileData[i] == 0 && s_fileData[i + 1] == 0 && s_fileData[i + 2] == 1) {
            *outPos = i;
            return true;
        }
    }
    return false;
}

// Splits the whole file into NAL units up front (a few hundred for a
// clip this short, trivial to hold in one static array) rather than
// re-scanning from the current position on every step -- simpler than
// a streaming parser and this file is only ever loaded whole anyway.
static void scanNalus(void) {
    s_naluCount = 0;
    size_t pos;
    if (!findStartCode(0, &pos)) {
        return;
    }
    pos += 3;
    while (s_naluCount < MAX_NALUS) {
        size_t nextPos;
        size_t end;
        if (!findStartCode(pos, &nextPos)) {
            end = s_fileSize;
        } else {
            // A 4-byte "00 00 00 01" start code is just a 3-byte one
            // with an extra leading zero -- that zero belongs to the
            // start code, not to this NAL's payload.
            end = (nextPos > pos && s_fileData[nextPos - 1] == 0) ? nextPos - 1 : nextPos;
        }
        if (end > pos) {
            s_nalus[s_naluCount].offset = (u32)pos;
            s_nalus[s_naluCount].length = (u32)(end - pos);
            ++s_naluCount;
        }
        if (!findStartCode(pos, &nextPos)) {
            break;
        }
        pos = nextPos + 3;
    }
}

// Feeds NAL units to the decoder until one produces a rendered
// picture, the stream runs out, or a call fails outright. Returns
// false in the latter two cases -- the caller (introVideoStep) treats
// "no picture, no error" (end of stream) and "hard failure" the same
// way, since there's nothing more this file can do in either case.
static bool decodeUntilFrame(void) {
    while (s_naluCursor < s_naluCount) {
        NaluRange r = s_nalus[s_naluCursor++];
        if (r.length + 3 > NALU_SCRATCH_SIZE) {
            continue; // shouldn't happen at this resolution; skip rather than abort the whole clip over one bad unit
        }
        s_naluScratch[0] = 0x00;
        s_naluScratch[1] = 0x00;
        s_naluScratch[2] = 0x01;
        memcpy(s_naluScratch + 3, s_fileData + r.offset, r.length);
        GSPGPU_FlushDataCache(s_naluScratch, r.length + 3);

        MVDSTD_ProcessNALUnitOut out;
        Result res = mvdstdProcessVideoFrame(s_naluScratch, r.length + 3, 0, &out);
        if (!MVD_CHECKNALUPROC_SUCCESS((u32)res)) {
            return false;
        }
        if ((u32)res == MVD_STATUS_PARAMSET || (u32)res == MVD_STATUS_INCOMPLETEPROCESSING) {
            continue; // SPS/PPS unit or a partial unit -- no picture yet
        }

        Result renderRes = mvdstdRenderVideoFrame(NULL, true);
        if (R_FAILED(renderRes)) {
            return false;
        }
        return true;
    }
    return false;
}

bool introVideoInit(void) {
    // Disabled for now -- this hung the whole app on real hardware
    // (not just a failed/skipped intro: nothing loaded at all, not
    // even the fallback boot screen), and an exheader fix for the
    // most likely cause (missing mvd:STD service access -- see
    // package_cia.py) made no observed difference, meaning either
    // that wasn't the actual cause or there's a second problem behind
    // it. Bailing out before anything below runs (no APT_CheckNew3DS,
    // no romfs read, no MVD calls at all) rules this file out entirely
    // as a variable while that gets root-caused for real, rather than
    // shipping another guess against hardware nobody in this
    // conversation can directly test on.
    return false;

    bool isNew3ds = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew3ds)) || !isNew3ds) {
        return false;
    }

    FILE* f = fopen("romfs:/intro_video.h264", "rb");
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
    s_fileData = (u8*)malloc((size_t)size);
    if (!s_fileData) {
        fclose(f);
        return false;
    }
    size_t read = fread(s_fileData, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(s_fileData);
        s_fileData = NULL;
        return false;
    }
    s_fileSize = (size_t)size;

    s_outBuf = (u16*)linearAlloc((size_t)INTRO_WIDTH * INTRO_HEIGHT * sizeof(u16));
    s_naluScratch = (u8*)linearAlloc(NALU_SCRATCH_SIZE);
    if (!s_outBuf || !s_naluScratch) {
        introVideoExit();
        return false;
    }

    if (R_FAILED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565,
            MVD_DEFAULT_WORKBUF_SIZE, NULL))) {
        introVideoExit();
        return false;
    }
    s_mvdReady = true;

    MVDSTD_Config config;
    mvdstdGenerateDefaultConfig(&config, INTRO_WIDTH, INTRO_HEIGHT, INTRO_WIDTH, INTRO_HEIGHT,
        NULL, (u32*)s_outBuf, NULL);
    if (R_FAILED(MVDSTD_SetConfig(&config))) {
        introVideoExit();
        return false;
    }

    scanNalus();
    if (s_naluCount == 0) {
        introVideoExit();
        return false;
    }
    s_naluCursor = 0;
    s_done = false;
    s_holdTicks = 0;

    audioPlayClip("romfs:/boot_chime.pcm", 44100.0f, false);
    return true;
}

void introVideoStep(void) {
    if (s_done) {
        return;
    }
    if (s_holdTicks > 0) {
        --s_holdTicks;
        return;
    }
    if (!decodeUntilFrame()) {
        s_done = true;
        return;
    }

    PsxFrame frame = {
        .data = s_outBuf,
        .width = INTRO_WIDTH,
        .height = INTRO_HEIGHT,
        .pitch = (size_t)INTRO_WIDTH * sizeof(u16),
        .pixelFormat = 2, // RGB565 -- matches MVD_OUTPUT_RGB565 above
    };
    videoPresentGameFrame(&frame);
    s_holdTicks = INTRO_TICKS_PER_FRAME - 1; // this tick already presented the new picture
}

bool introVideoDone(void) {
    return s_done;
}

void introVideoExit(void) {
    if (s_mvdReady) {
        mvdstdExit();
        s_mvdReady = false;
    }
    if (s_naluScratch) {
        linearFree(s_naluScratch);
        s_naluScratch = NULL;
    }
    if (s_outBuf) {
        linearFree(s_outBuf);
        s_outBuf = NULL;
    }
    if (s_fileData) {
        free(s_fileData);
        s_fileData = NULL;
    }
}
