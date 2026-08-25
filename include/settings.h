// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Tiny persisted settings file (sdmc:/3ds/pcsx_rearmed_3ds/settings.ini)
// -- just the one BIOS preference for now. Loaded once at startup,
// written back out whenever menu.c's settings screen changes it.

#pragma once

#include <stdbool.h>

void settingsLoad(void);

// false (default): pcsx_rearmed's own "auto" BIOS mode -- tries a real
// BIOS dump from SYSTEM_DIR first, silently falls back to HLE if none
// is found. true: always use HLE, skipping the real-BIOS lookup (and
// its boot logo/splash) even if a real dump is present.
bool settingsGetForceHle(void);
void settingsSetForceHle(bool force);

// Which screen the game's picture renders on -- false (default): top
// screen, matching every other emulator on this project (DS, mGBA).
// true: bottom (touch) screen, for players who'd rather have the
// bigger/centered screen free for something else or just prefer it.
bool settingsGetDisplayOnBottom(void);
void settingsSetDisplayOnBottom(bool bottom);

// true (default): skip every 2nd scanline in the GPU_UNAI software
// renderer, roughly halving pixel-fill cost -- a real, meaningful
// performance win at the cost of a visibly lower-res/"interlaced"
// look. Exposed as a live toggle specifically so it can be turned back
// off from the settings screen without a new build if the tradeoff
// doesn't feel worth it.
bool settingsGetSkipLines(void);
void settingsSetSkipLines(bool skip);

// false (default): pcsx_rearmed's normal GPU_UNAI renderer. true:
// switches to its alternate "old renderer" code path -- upstream docs
// call it "faster, but less accurate" without saying exactly how, so
// unlike skip-lines this one's visual impact isn't well understood
// ahead of time. Off by default for that reason; a live toggle so it
// can be tried and reverted per-preference without a new build either
// way.
bool settingsGetOldRenderer(void);
void settingsSetOldRenderer(bool old);
