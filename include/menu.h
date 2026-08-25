// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <stdbool.h>

// Renders and drives the SD-card file browser (bottom screen) until the
// user picks a disc image or asks to quit the app. Returns a
// heap-allocated absolute path (caller frees it) on selection, or NULL
// if the user chose to quit.
char* menuBrowseForGame(void);

// Renders and drives the pause menu opened by the START+SELECT chord
// during gameplay (see input.c). Returns true if the caller should
// unload the game and return to the file browser (the user picked
// "Change Disc" and confirmed), or false to just resume play (covers
// both "Resume" and after a save/load state, which stay in this
// screen rather than falling through to gameplay automatically).
bool menuPause(void);

// Renders and drives the BIOS settings screen, reachable from the file
// browser via X. Self-contained -- reads/writes settings.h directly,
// nothing for the caller to do with the result.
void menuSettings(void);
