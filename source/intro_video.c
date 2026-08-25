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
#include <sys/stat.h>

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

// mvdstdInit() is the one call in this whole file that reaches an
// actual 3DS system service (mvd:STD) rather than just our own
// buffers -- exactly the kind of call that hung the app outright on
// real hardware once already (see introVideoInit's own comment). A
// service-permission fix was applied since, but it was never verified
// in isolation, so this still runs it on a throwaway thread with a
// hard timeout rather than trusting it can't hang again: if it
// doesn't finish in time, the thread (and its small stack) is simply
// abandoned -- there's no safe way to force-cancel a blocked syscall/
// IPC call on 3DS -- and introVideoInit() falls back exactly like any
// other failure. A one-time few-KB leak in the timeout case is a
// trivial cost next to "the app doesn't boot at all."
static volatile bool s_mvdInitThreadDone;
static volatile Result s_mvdInitResult;

static void mvdInitThreadFunc(void* arg) {
    (void)arg;
    s_mvdInitResult = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
        MVD_OUTPUT_RGB565, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    s_mvdInitThreadDone = true;
}

// Returns false if mvdstdInit() failed OR didn't finish within the
// timeout -- caller can't tell which, but doesn't need to: either way
// MVD isn't usable and introVideoInit() should fall back.
static bool mvdInitWithTimeout(void) {
    s_mvdInitThreadDone = false;
    Thread t = threadCreate(mvdInitThreadFunc, NULL, 4 * 1024, 0x3F, -2, true);
    if (!t) {
        return false;
    }
    // ~3 seconds at ~16ms/tick -- generous for a real service call
    // under normal conditions, short enough that a genuine hang still
    // resolves to the fallback boot screen quickly rather than the
    // player staring at a black screen wondering if the app is dead.
    for (int i = 0; i < 180 && !s_mvdInitThreadDone; ++i) {
        svcSleepThread(16 * 1000 * 1000);
    }
    if (!s_mvdInitThreadDone) {
        return false; // still stuck -- abandon it, thread was created detached
    }
    return R_SUCCEEDED(s_mvdInitResult);
}

// Diagnostic-only: introVideoInit() has many independent bail-out
// points (New3DS check, romfs read, allocation, MVD service init,
// config, NAL scan) and every one of them looks identical from the
// outside -- "no real intro, fell back to the logo" -- so there's no
// way to tell which one is actually firing on a given device without
// this. Appends rather than truncates so a full boot's story survives
// even if something after this crashes outright.
static void introLog(const char* msg) {
    FILE* f = fopen("sdmc:/3ds/pcsx_rearmed_3ds/intro_debug.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

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
            char buf[128];
            snprintf(buf, sizeof(buf), "bail: mvdstdProcessVideoFrame failed at NAL #%d, res=0x%08lx",
                s_naluCursor - 1, (unsigned long)res);
            introLog(buf);
            return false;
        }
        if ((u32)res == MVD_STATUS_PARAMSET || (u32)res == MVD_STATUS_INCOMPLETEPROCESSING) {
            continue; // SPS/PPS unit or a partial unit -- no picture yet
        }

        Result renderRes = mvdstdRenderVideoFrame(NULL, true);
        if (R_FAILED(renderRes)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "bail: mvdstdRenderVideoFrame failed at NAL #%d, res=0x%08lx",
                s_naluCursor - 1, (unsigned long)renderRes);
            introLog(buf);
            return false;
        }
        return true;
    }
    introLog("decodeUntilFrame: reached end of NAL list (normal end of clip)");
    return false;
}

bool introVideoInit(void) {
    // Re-enabled: was shipped once alongside auto-frameskip in the
    // same build, hung the app, and a first exheader fix (missing
    // mvd:STD service access -- see package_cia.py) made no observed
    // difference -- but that test never isolated this file from
    // frameskip. Frameskip has since been re-confirmed working fine on
    // its own (see core_glue.c), which was the other prime suspect in
    // that build, so this is the first time the video path is being
    // tested alone with the RSF fix in place.
    // This runs before any game has been loaded, so unlike
    // core_glue.c's SYSTEM_DIR/SAVE_DIR (created lazily on first game
    // load) or settings.c's own dir (created on first settings
    // change), sdmc:/3ds/pcsx_rearmed_3ds/ may not exist yet on a
    // fresh install -- without this, the log below would silently
    // fail to open every single time and produce nothing to look at.
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/pcsx_rearmed_3ds", 0777);

    // Truncate rather than append here (introVideoInit() only ever
    // runs once per app launch, at boot) so each test run's log is a
    // clean, unambiguous record instead of piling onto whatever a
    // previous session already wrote.
    FILE* truncateLog = fopen("sdmc:/3ds/pcsx_rearmed_3ds/intro_debug.log", "w");
    if (truncateLog) {
        fclose(truncateLog);
    }
    introLog("introVideoInit: start");

    bool isNew3ds = false;
    Result checkRes = APT_CheckNew3DS(&isNew3ds);
    char buf[128];
    snprintf(buf, sizeof(buf), "APT_CheckNew3DS: res=0x%08lx isNew3ds=%d",
        (unsigned long)checkRes, (int)isNew3ds);
    introLog(buf);
    if (R_FAILED(checkRes) || !isNew3ds) {
        introLog("bail: not New3DS (or check failed)");
        return false;
    }

    FILE* f = fopen("romfs:/intro_video.h264", "rb");
    if (!f) {
        introLog("bail: fopen romfs:/intro_video.h264 failed");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        introLog("bail: intro_video.h264 empty/ftell failed");
        fclose(f);
        return false;
    }
    s_fileData = (u8*)malloc((size_t)size);
    if (!s_fileData) {
        introLog("bail: malloc(s_fileData) failed");
        fclose(f);
        return false;
    }
    size_t read = fread(s_fileData, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        snprintf(buf, sizeof(buf), "bail: fread got %zu of %ld bytes", read, size);
        introLog(buf);
        free(s_fileData);
        s_fileData = NULL;
        return false;
    }
    s_fileSize = (size_t)size;
    snprintf(buf, sizeof(buf), "loaded intro_video.h264: %zu bytes", s_fileSize);
    introLog(buf);

    s_outBuf = (u16*)linearAlloc((size_t)INTRO_WIDTH * INTRO_HEIGHT * sizeof(u16));
    s_naluScratch = (u8*)linearAlloc(NALU_SCRATCH_SIZE);
    if (!s_outBuf || !s_naluScratch) {
        introLog("bail: linearAlloc(s_outBuf/s_naluScratch) failed");
        introVideoExit();
        return false;
    }

    introLog("calling mvdInitWithTimeout()...");
    bool mvdOk = mvdInitWithTimeout();
    snprintf(buf, sizeof(buf), "mvdInitWithTimeout: %s (res=0x%08lx, timed_out=%d)",
        mvdOk ? "ok" : "FAILED", (unsigned long)s_mvdInitResult, (int)!s_mvdInitThreadDone);
    introLog(buf);
    if (!mvdOk) {
        // Not marking s_mvdReady here even on the timeout path -- if
        // mvdstdInit() really is still stuck in the abandoned thread,
        // calling mvdstdExit() concurrently from here would race
        // against it instead of cleanly tearing down.
        introVideoExit();
        return false;
    }
    s_mvdReady = true;

    MVDSTD_Config config;
    mvdstdGenerateDefaultConfig(&config, INTRO_WIDTH, INTRO_HEIGHT, INTRO_WIDTH, INTRO_HEIGHT,
        NULL, (u32*)s_outBuf, NULL);
    Result setConfigRes = MVDSTD_SetConfig(&config);
    if (R_FAILED(setConfigRes)) {
        snprintf(buf, sizeof(buf), "bail: MVDSTD_SetConfig failed, res=0x%08lx", (unsigned long)setConfigRes);
        introLog(buf);
        introVideoExit();
        return false;
    }

    scanNalus();
    snprintf(buf, sizeof(buf), "scanNalus: found %d NAL units", s_naluCount);
    introLog(buf);
    if (s_naluCount == 0) {
        introLog("bail: 0 NAL units found (bad/truncated intro_video.h264?)");
        introVideoExit();
        return false;
    }
    s_naluCursor = 0;
    s_done = false;
    s_holdTicks = 0;

    audioPlayClip("romfs:/boot_chime.pcm", 44100.0f, false);
    introLog("introVideoInit: success, playback starting");
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
