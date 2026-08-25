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
#include "settings.h"

bool g_psxPad[PSX_MAX_BUTTONS];

static enum retro_pixel_format s_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static const struct retro_core_option_definition* s_coreOptions;
static retro_audio_buffer_status_callback_t s_audioBufferStatusCb;
static double s_targetFps = 60.0;
static double s_sampleRate = 44100.0;
static char s_currentGameName[128];

#define SYSTEM_DIR "sdmc:/3ds/pcsx_rearmed_3ds/system"
#define SAVE_DIR   "sdmc:/3ds/pcsx_rearmed_3ds/saves"

// Not in libretro.h itself -- pcsx_rearmed's frontend/libretro.c
// defines these locally as RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG,
// N) and switches on the exact value in retro_set_controller_port_device,
// so replicating the same macro application here is enough without
// needing pcsx_rearmed's own header.
#define RETRO_DEVICE_PSE_DUALSHOCK RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)

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

    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        // Without this, pcsx_rearmed's own version-negotiation defaults
        // to 0 (treating GET_CORE_OPTIONS_VERSION returning false the
        // same as "no core-options API at all") and never calls
        // SET_CORE_OPTIONS below, even for the plain v1 form -- s_coreOptions
        // stayed permanently NULL, silently breaking every
        // GET_VARIABLE lookup that isn't hardcoded like BIOS mode is
        // (this is exactly why the memory card never got configured:
        // pcsx_rearmed's own default_value for that option was never
        // reachable, so it landed on "no config" and disabled it).
        *(unsigned*)data = 1;
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
            // pcsx_rearmed's own "auto" default already does the right
            // thing with zero setup -- tries a real BIOS dump from
            // SYSTEM_DIR first, silently falls back to HLE if none is
            // there -- so just let it through unless the user has
            // explicitly forced HLE from the settings screen (skips
            // the real-BIOS lookup and its boot logo even when a real
            // dump is present, matching what "HLE" always meant here).
            var->value = settingsGetForceHle() ? "HLE" : "auto";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_memcard1") == 0 ||
            strcmp(var->key, "pcsx_rearmed_memcard2") == 0) {
            // pcsx_rearmed's own default for card 1 is "libretro"
            // (hands the card's raw bytes to the frontend via
            // retro_get_memory_data/retro_get_memory_size for us to
            // persist ourselves) -- we don't implement that side of
            // the libretro API, so a card in that mode never actually
            // got backed by anything on disk and games reported
            // "memory card is not inserted". "shared" writes directly
            // to a real .mcd file via SAVE_DIR (see
            // RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY above) -- pcsx_rearmed
            // handles that file I/O entirely itself, nothing further
            // needed on our end. One real card per slot, shared across
            // every game, same as a real physical memory card.
            var->value = "shared";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_analog_combo") == 0) {
            // Which button combo pcsx_rearmed's own update_input()
            // watches for to flip analog mode (see padToggleAnalog in
            // libpcsxcore/pad.c) -- L3+R3 specifically because nothing
            // on our overlay maps to either normally (see input.c),
            // so coreTriggerAnalogToggle() can inject just these two
            // for one frame without ever colliding with a real button
            // press.
            var->value = "l3+r3";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_gpu_thread_rendering") == 0 ||
            strcmp(var->key, "pcsx_rearmed_drc_thread") == 0) {
            // Both default to "auto" (enable if >=2 CPU cores detected),
            // but the device log showed pcsx_rearmed's own core-count
            // detection reporting exactly 1 -- on a New3DS with
            // CanAccessCore2 set in this app's own exheader (see
            // package_cia.py), that's wrong, and it means GPU rendering
            // and the dynarec compiler were both silently staying on
            // the same thread as everything else instead of actually
            // using the second core. frontend/3ds/pthread.h's own
            // pthread_create() already does the right thing when asked
            // to run a thread at all -- it checks APT_CheckNew3DS
            // itself and pins new threads to CPU 2 -- so forcing these
            // on sidesteps the broken "auto" check entirely rather
            // than needing any threading code of our own.
            var->value = "enabled";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_frameskip_type") == 0) {
            // Defaults to "disabled" -- fine on real hardware or a
            // beefy retro frontend, but on a 3DS's own ARM11 core
            // reported stutter is far more likely a frame occasionally
            // missing its budget than anything actually wrong with the
            // emulation itself. "auto" skips a video frame instead of
            // letting the audio ring buffer run dry when that happens
            // (pcsx_rearmed's own frontend-advised skip, not a fixed
            // interval), trading an occasional dropped frame for not
            // being able to hear the difference. (This was previously
            // shipped alongside a New3DS hardware-video-decode boot
            // intro that hung the app outright -- reverted together
            // with it out of caution, then re-confirmed on its own:
            // this option and its SET_AUDIO_BUFFER_STATUS_CALLBACK
            // support below don't touch any 3DS system service beyond
            // what audioInit() already uses, unlike the video path.)
            var->value = "auto";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_gpu_unai_skipline") == 0) {
            // Live toggle (see settings.h) rather than a fixed value --
            // this halves the software renderer's pixel-fill cost, a
            // real perf win, but visibly lowers output quality (skips
            // every 2nd scanline), so it needs to be switchable from
            // the settings screen without a new build if the tradeoff
            // isn't worth it for a given game/player.
            var->value = settingsGetSkipLines() ? "enabled" : "disabled";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_cd_readahead") == 0) {
            // Default is 12 sectors (~28KB) -- a 3DS's SD card has real
            // seek latency, and a bigger read-ahead buffer means fewer
            // individual seeks for the same amount of disc data, which
            // should help both the initial load (reading the boot
            // executable/BIOS data) and in-game stalls alike. 64 sectors
            // is ~150KB, trivial against this app's own 96/178MB
            // memory budget (see package_cia.py) -- nowhere near the
            // 333000 "read the whole disc" option this core disables
            // outright on 3DS for exactly that RAM-cost reason.
            var->value = "64";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_scale_hires") == 0) {
            // Defaults to disabled upstream -- only affects the small
            // subset of games that switch into a 480i/512i hi-res video
            // mode, downscaling those to 320x240 by skipping lines/
            // columns instead of rendering the full resolution. Unlike
            // gpu_unai_skipline (all games, all the time), this only
            // does anything for games that already use a hi-res mode,
            // so there's no real downside for the majority that don't.
            var->value = "enabled";
            return true;
        }
        if (strcmp(var->key, "pcsx_rearmed_gpu_unai_old_renderer") == 0) {
            // Live toggle (see settings.h), off by default -- unlike
            // skipline's well-understood "every 2nd line" effect,
            // upstream only documents this as "faster, but less
            // accurate" without saying how, so it needs to be
            // switchable per-preference rather than assumed safe to
            // force on for everyone.
            var->value = settingsGetOldRenderer() ? "enabled" : "disabled";
            return true;
        }
        return findCoreOptionDefault(var->key, &var->value);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool*)data = false;
        return true;

    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;

    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        // Required for the "auto" pcsx_rearmed_frameskip_type forced
        // above to do anything at all -- pcsx_rearmed's own frameskip
        // logic only ever sees retro_audio_buff_active as true if this
        // callback was successfully registered (frontend/libretro.c's
        // update_variables()), and skip_frame is gated on that flag.
        // coreRunFrame() below calls it once per emulated frame, right
        // before retro_run(), matching this environment call's own doc
        // comment for when a frontend should invoke it.
        const struct retro_audio_buffer_status_callback* cb =
            (const struct retro_audio_buffer_status_callback*)data;
        s_audioBufferStatusCb = cb ? cb->callback : NULL;
        return true;
    }

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
        .pixelFormat = (int)s_pixelFormat,
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

// Set for exactly one retro_run()'s worth of polling by
// coreTriggerAnalogToggle() -- see its own comment for why this,
// rather than reconfiguring the pad type, is how the ANALOG button
// actually flips analog mode.
static bool s_analogComboPending;

static int16_t inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) {
        return 0;
    }
    if (device == RETRO_DEVICE_JOYPAD) {
        if (s_analogComboPending && (id == RETRO_DEVICE_ID_JOYPAD_L3 || id == RETRO_DEVICE_ID_JOYPAD_R3)) {
            return 1;
        }
        return (id < PSX_MAX_BUTTONS && g_psxPad[id]) ? 1 : 0;
    }
    if (device == RETRO_DEVICE_ANALOG) {
        // index selects which stick (LEFT/RIGHT), id selects X/Y --
        // g_psxAnalog is laid out as [LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y],
        // exactly index*2 + id.
        unsigned axis = index * 2 + id;
        return axis < 4 ? g_psxAnalog[axis] : 0;
    }
    return 0;
}

static void setCurrentGameNameFromPath(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(s_currentGameName)) {
        len = sizeof(s_currentGameName) - 1;
    }
    memcpy(s_currentGameName, base, len);
    s_currentGameName[len] = '\0';
}

bool coreLoad(const char* path) {
    setCurrentGameNameFromPath(path);

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

    // Declared once, unconditionally, and never switched again --
    // matches how a player would actually use a real DualShock (always
    // plugged in as a DualShock; digital vs analog is the pad's own
    // internal mode, not a different pad). Reconfiguring the pad
    // *type* every time the ANALOG button was tapped -- this file's
    // previous approach -- is what caused the reported delay: it makes
    // pcsx_rearmed re-run its own pad-change/reset logic
    // (retro_set_controller_port_device -> padChanged() -> padReset()
    // in libpcsxcore/pad.c), which real analog-mode toggling never
    // does; padToggleAnalog() is a plain instant flag flip. Real
    // toggling now happens via coreTriggerAnalogToggle() instead,
    // simulating the L3+R3 combo pcsx_rearmed's own analog_combo core
    // option (forced to "l3+r3" below) already watches for -- the
    // same mechanism a real DualShock's own ANALOG button uses,
    // nothing to reconfigure. Starts in digital mode every session,
    // same as a real DualShock does on power-on.
    retro_set_controller_port_device(0, RETRO_DEVICE_PSE_DUALSHOCK);
    return true;
}

void coreUnload(void) {
    retro_unload_game();
    retro_deinit();
}

void coreRunFrame(void) {
    if (s_audioBufferStatusCb) {
        unsigned occupancy = audioBufferOccupancyPercent();
        // "Likely underrun" is meant to be a predictive judgment from
        // actual playback-rate history, which we don't track -- "the
        // buffer is nearly empty right now" is a coarser proxy, but
        // it's exactly the state an underrun is about to happen from,
        // so it's a reasonable stand-in given what audio.c exposes.
        s_audioBufferStatusCb(true, occupancy, occupancy < 25);
    }
    retro_run();
    // Only pending for the retro_run() call it was set before -- pad.c's
    // own toggle-on-edge detection (in_dualshock_toggling) only needs
    // the combo held for a single input poll to register one flip.
    s_analogComboPending = false;
}

void coreTriggerAnalogToggle(void) {
    s_analogComboPending = true;
}

double coreTargetFps(void) {
    return s_targetFps;
}

double coreSampleRate(void) {
    return s_sampleRate;
}

const char* coreCurrentGameName(void) {
    return s_currentGameName;
}

void coreSaveStatePath(char* buf, size_t bufSize) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/pcsx_rearmed_3ds", 0777);
    mkdir(SAVE_DIR, 0777);
    snprintf(buf, bufSize, "%s/%s.state", SAVE_DIR, s_currentGameName);
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
