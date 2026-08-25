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
#include <math.h>
#include <stdlib.h>

#include "menu.h"
#include "psx3ds.h"
#include "settings.h"

// devkitARM's default main-thread stack for a 3dsx/CIA homebrew app is
// only 32KB (__stacksize__'s weak default from libctru's own startup
// code) -- pcsx_rearmed's own log_mem_usage() checks this at startup
// and logs "past OOM detected, expect instability" if it's under 1MB,
// which is exactly what showed up in this app's own log after
// installing. A plain, non-weak global of the same name overrides
// that default (standard ELF weak-symbol resolution; matches
// DSEmulationActivity.kt's setStackSize() comment for the same class
// of problem on the Android/DS side of this project -- deep JIT
// compiler call chains need more than the platform default). 2MB is
// comfortably above pcsx_rearmed's own 1MB floor.
u32 __stacksize__ = 2 * 1024 * 1024;

// Plays once on app launch, since the "ideal"/HLE BIOS this project
// falls back to when no real BIOS dump is present skips the real
// console's own startup intro entirely. On a New3DS this is the actual
// source video, hardware-decoded via intro_video.c; everywhere else
// (Old 3DS, or if the New3DS decoder fails to start for any reason)
// it's the audio + animated-logo fallback. Skippable via any of the
// same inputs the file browser itself uses either way, so it never
// gets in the way of someone who's already sat through it once.
static void showBootScreen(void) {
    if (introVideoInit()) {
        while (aptMainLoop() && !introVideoDone()) {
            inputPoll();
            if (inputMenuConfirmPressed() || inputMenuBackPressed()) {
                break;
            }
            int tx, ty;
            if (inputTouchTapped(&tx, &ty)) {
                break;
            }
            introVideoStep();
            videoBeginIntroFrame();
            videoDrawMenuText("Tap, A, or B to skip", 100, 210, 0.42f);
            videoEndFrame();
        }
        introVideoExit();
        audioStopClip();
        return;
    }

    if (!audioPlayClip("romfs:/boot_chime.pcm", 44100.0f, false)) {
        return; // missing/corrupt clip -- just go straight to the browser
    }
    int frame = 0;
    while (aptMainLoop()) {
        inputPoll();
        if (inputMenuConfirmPressed() || inputMenuBackPressed()) {
            break;
        }
        int tx, ty;
        if (inputTouchTapped(&tx, &ty)) {
            break;
        }
        if (audioClipFinished()) {
            break;
        }
        videoBeginFrame(false); // just for the C3D_FrameBegin -- videoDrawBootLogo does its own clears
        videoDrawBootLogo(frame);
        videoEndFrame();
        ++frame;
    }
    audioStopClip();
}

static void runGame(const char* path) {
    if (!coreLoad(path)) {
        return;
    }
    audioResetForGameplay(); // menu music (see menuBrowseForGame) may have left the channel at 32kHz

    // Starts false every session -- matches a real DualShock, which
    // resets to digital mode on power-on too; see coreLoad's own
    // comment for why the engine's internal analog state can't be
    // restored from a previous session anyway (only ever flipped via
    // the same one-frame combo injection a real ANALOG button press
    // triggers, nothing to read back from).
    bool analogEnabled = false;
    osSetSpeedupEnable(true); // New 3DS CPU clock boost while actually playing
    bool quit = false;
    while (!quit && aptMainLoop()) {
        inputPoll();

        if (inputPausePressed()) {
            quit = menuPause();
            continue;
        }

        // The ANALOG toggle is always live during gameplay (not tucked
        // away in the settings screen) -- matches a real DualShock's
        // own physical ANALOG button, which works the same way whether
        // or not a menu is open.
        int tx, ty;
        if (inputTouchTapped(&tx, &ty)) {
            float dx = tx - kAnalogToggleX, dy = ty - kAnalogToggleY;
            if (sqrtf(dx * dx + dy * dy) <= kAnalogToggleRadius) {
                analogEnabled = !analogEnabled;
                coreTriggerAnalogToggle();
            }
        }

        // coreRunFrame() -> retro_run() -> the retro_video_refresh
        // callback -> videoPresentGameFrame() runs synchronously inside
        // this call, uploading the new frame into s_gameTex before we
        // draw it below.
        coreRunFrame();

        videoBeginFrame(true);
        videoDrawAnalogToggle(analogEnabled);
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

    showBootScreen();

    while (aptMainLoop()) {
        char* gamePath = menuBrowseForGame();
        if (!gamePath) {
            break; // user chose to quit from the browser
        }

        // coreLoad() below is a single big blocking call (disc image
        // I/O, plugin setup, BIOS boot) with nothing drawn until the
        // first emulated frame -- without this, the screen just
        // freezes on the last browser frame the instant a game is
        // tapped, which reads as the app hanging rather than loading.
        videoBeginFrame(false);
        videoDrawMenuText("Loading...", 128, 108, 0.6f);
        videoEndFrame();

        runGame(gamePath);
        free(gamePath);
    }

    audioExit();
    videoExit();
    return 0;
}
