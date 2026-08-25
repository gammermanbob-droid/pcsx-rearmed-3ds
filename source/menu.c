// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// A deliberately simple file browser + pause menu, scoped down from
// what mGBA's mGUI-based menu system offers (no theming, no on-screen
// keyboard search, no per-game settings) -- this project doesn't have
// mGBA's shared cross-platform GUI library to build on, so this is a
// hand-rolled equivalent covering just what's needed: list games,
// pick one, and a pause screen with the same "confirm before closing"
// shape as this project's Android DS player (see
// DsEmulationActivity.kt's confirmReturnToThreeDsHomeMenu).

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "menu.h"
#include "psx3ds.h"
#include "settings.h"

#define ROMS_ROOT "sdmc:/roms/psx"
#define MAX_ENTRIES 512
#define MAX_PATH_LEN 512

typedef struct {
    char path[MAX_PATH_LEN]; // full path, for loading
    char label[128];         // what's shown in the list -- relative to
                              // ROMS_ROOT, so a deeply-nested game still
                              // shows a recognizable path, not just a
                              // bare filename that could collide with
                              // another game's
} MenuEntry;

static MenuEntry s_entries[MAX_ENTRIES];
static int s_entryCount;

static bool hasDiscExtension(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    static const char* const exts[] = {".cue", ".bin", ".chd", ".pbp", ".iso", ".img"};
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

// A .cue is a plain-text sheet listing the .bin(s) that make up its
// disc (often more than one, for multi-track games) -- if a directory
// has one, its .bin files are tracks meant to be loaded *through* the
// .cue, not picked directly, so they shouldn't show up as their own
// separate entries.
static bool dirHasCueFile(const char* dirPath) {
    DIR* dir = opendir(dirPath);
    if (!dir) {
        return false;
    }
    bool found = false;
    struct dirent* ent;
    while (!found && (ent = readdir(dir)) != NULL) {
        const char* dot = strrchr(ent->d_name, '.');
        found = dot && strcasecmp(dot, ".cue") == 0;
    }
    closedir(dir);
    return found;
}

static void addEntry(const char* fullPath, const char* label) {
    if (s_entryCount >= MAX_ENTRIES) {
        return;
    }
    MenuEntry* e = &s_entries[s_entryCount++];
    snprintf(e->path, sizeof(e->path), "%s", fullPath);
    snprintf(e->label, sizeof(e->label), "%s", label);
}

// How many directory levels deep under ROMS_ROOT to look for discs --
// covers a flat dump directly in ROMS_ROOT, the common "one folder per
// game" layout, and a couple of extra levels on top of that for
// however a user has their collection organized (by console/region/
// series/game, for instance).
#define MAX_SCAN_DEPTH 4

static void scanDir(const char* dirPath, const char* relLabel, int depthRemaining) {
    if (depthRemaining <= 0 || s_entryCount >= MAX_ENTRIES) {
        return;
    }
    DIR* dir = opendir(dirPath);
    if (!dir) {
        return;
    }
    bool skipBareBins = dirHasCueFile(dirPath);
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && s_entryCount < MAX_ENTRIES) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char fullPath[MAX_PATH_LEN];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, ent->d_name);

        char label[128];
        if (relLabel[0]) {
            snprintf(label, sizeof(label), "%s/%s", relLabel, ent->d_name);
        } else {
            snprintf(label, sizeof(label), "%s", ent->d_name);
        }

        struct stat st;
        if (stat(fullPath, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scanDir(fullPath, label, depthRemaining - 1);
        } else if (hasDiscExtension(ent->d_name)) {
            const char* dot = strrchr(ent->d_name, '.');
            bool isBareBin = dot && strcasecmp(dot, ".bin") == 0;
            if (!(isBareBin && skipBareBins)) {
                addEntry(fullPath, label);
            }
        }
    }
    closedir(dir);
}

static void scanGames(void) {
    s_entryCount = 0;
    mkdir("sdmc:/roms", 0777);
    mkdir(ROMS_ROOT, 0777);
    scanDir(ROMS_ROOT, "", MAX_SCAN_DEPTH);
}

// Every list/menu screen in this file uses the same row layout: a
// touch tap anywhere in [rowY, rowY+kRowHeight) selects that row --
// full screen width, no need to match text extents exactly.
#define kRowHeight 18.0f

char* menuBrowseForGame(void) {
    scanGames();

    // Loops continuously (see audioPlayClip's looping arg) for as long
    // as the player is on this screen -- including while menuSettings()
    // below is open, since that's still "on the menu" from the
    // player's perspective, not a separate context that should
    // interrupt the music. Stopped on every way out of this function
    // (a game gets picked, or the player quits the app) so it never
    // bleeds into gameplay or keeps playing after the app closes.
    audioPlayClip("romfs:/menu_music.pcm", 32000.0f, true);

    int selected = 0;
    while (aptMainLoop()) {
        inputPoll();

        if (inputMenuUpPressed() && s_entryCount > 0) {
            selected = (selected - 1 + s_entryCount) % s_entryCount;
        }
        if (inputMenuDownPressed() && s_entryCount > 0) {
            selected = (selected + 1) % s_entryCount;
        }
        if (inputMenuBackPressed()) {
            audioStopClip();
            return NULL; // quit the app
        }
        if (inputMenuSettingsPressed()) {
            menuSettings();
            continue;
        }

        // Keep the selected entry roughly centered rather than
        // scrolling the whole list from the top -- simplest way to
        // keep a long library navigable on a 240px-tall screen.
        int firstVisible = selected - 5;
        if (firstVisible < 0) {
            firstVisible = 0;
        }
        int lastVisible = firstVisible + 11;
        if (lastVisible >= s_entryCount) {
            lastVisible = s_entryCount - 1;
            firstVisible = lastVisible - 11;
            if (firstVisible < 0) {
                firstVisible = 0;
            }
        }

        bool confirmed = inputMenuConfirmPressed() && s_entryCount > 0;
        int tx, ty;
        if (inputTouchTapped(&tx, &ty)) {
            if (ty >= 204) {
                menuSettings(); // tapping the hint row doubles as the X shortcut
                continue;
            }
            int row = (int)(ty / kRowHeight);
            int tapped = firstVisible + row;
            if (tapped >= firstVisible && tapped <= lastVisible) {
                selected = tapped;
                confirmed = true; // tap-to-play, standard touch-list behavior
            }
        }
        if (confirmed) {
            audioStopClip();
            return strdup(s_entries[selected].path);
        }

        videoBeginFrame(false);
        if (s_entryCount == 0) {
            videoDrawMenuText("No PS1 discs found.", 8, 8, 0.5f);
            videoDrawMenuText("Put .cue/.bin/.chd/.pbp/.iso files in", 8, 28, 0.42f);
            videoDrawMenuText(ROMS_ROOT, 8, 44, 0.42f);
            videoDrawMenuText("(one folder per game is fine too)", 8, 60, 0.42f);
        } else {
            float y = 8;
            for (int i = firstVisible; i <= lastVisible; ++i) {
                char line[80];
                snprintf(line, sizeof(line), "%s %s", i == selected ? ">" : " ", s_entries[i].label);
                videoDrawMenuText(line, 8, y, 0.45f);
                y += kRowHeight;
            }
        }
        videoDrawMenuText("Tap a game to play, or:", 8, 204, 0.38f);
        videoDrawMenuText("A: Play   B: Quit App   X: Settings", 8, 220, 0.4f);
        videoEndFrame();
    }
    audioStopClip(); // aptMainLoop() went false -- the whole app is closing
    return NULL;
}

bool menuPause(void) {
    enum { OPT_RESUME, OPT_SAVE_STATE, OPT_LOAD_STATE, OPT_CHANGE_DISC, OPT_COUNT };
    static const char* const kOptions[OPT_COUNT] = {
        "Resume", "Save State", "Load State", "Change Disc",
    };

    const float kFirstOptionY = 40.0f, kOptionSpacing = 20.0f;

    int selected = OPT_RESUME;
    bool confirmingQuit = false;
    char status[64] = "";

    while (aptMainLoop()) {
        inputPoll();
        int tx, ty;
        bool tapped = inputTouchTapped(&tx, &ty);

        if (confirmingQuit) {
            // Tap either half of the screen for the matching choice --
            // simplest touch equivalent of a two-button confirm dialog
            // without needing to draw and hit-test two separate boxes.
            if (inputMenuConfirmPressed() || (tapped && tx < 160)) {
                return true; // confirmed: caller unloads and returns to the browser
            }
            if (inputMenuBackPressed() || (tapped && tx >= 160)) {
                confirmingQuit = false;
            }
        } else {
            if (inputMenuUpPressed()) {
                selected = (selected - 1 + OPT_COUNT) % OPT_COUNT;
            }
            if (inputMenuDownPressed()) {
                selected = (selected + 1) % OPT_COUNT;
            }
            if (inputMenuBackPressed()) {
                return false; // B also just resumes, matching most pause menus
            }
            bool confirmed = inputMenuConfirmPressed();
            if (tapped) {
                int row = (int)((ty - kFirstOptionY) / kOptionSpacing);
                if (row >= 0 && row < OPT_COUNT) {
                    selected = row;
                    confirmed = true; // tap an option to select and activate it in one go
                }
            }
            if (confirmed) {
                char path[512];
                switch (selected) {
                case OPT_RESUME:
                    return false;
                case OPT_SAVE_STATE:
                    coreSaveStatePath(path, sizeof(path));
                    snprintf(status, sizeof(status), "%s",
                        coreSerialize(path) ? "State saved." : "Save failed.");
                    break;
                case OPT_LOAD_STATE:
                    coreSaveStatePath(path, sizeof(path));
                    snprintf(status, sizeof(status), "%s",
                        coreUnserialize(path) ? "State loaded." : "No state to load.");
                    break;
                case OPT_CHANGE_DISC:
                    confirmingQuit = true;
                    break;
                }
            }
        }

        videoBeginFrame(false);
        videoDrawMenuText("Paused", 8, 8, 0.6f);
        if (confirmingQuit) {
            videoDrawMenuText("Return to the game list?", 8, 40, 0.5f);
            videoDrawMenuText("Unsaved progress will be lost --", 8, 58, 0.42f);
            videoDrawMenuText("use Save State first if you need it.", 8, 74, 0.42f);
            videoDrawMenuText("Tap left: Confirm   Tap right: Cancel", 8, 204, 0.38f);
            videoDrawMenuText("A: Confirm   B: Cancel", 8, 220, 0.42f);
        } else {
            float y = kFirstOptionY;
            for (int i = 0; i < OPT_COUNT; ++i) {
                char line[64];
                snprintf(line, sizeof(line), "%s %s", i == selected ? ">" : " ", kOptions[i]);
                videoDrawMenuText(line, 8, y, 0.5f);
                y += kOptionSpacing;
            }
            if (status[0]) {
                videoDrawMenuText(status, 8, y + 12, 0.42f);
            }
            videoDrawMenuText("Tap an option, or:", 8, 204, 0.38f);
            videoDrawMenuText("A: Select   B: Resume", 8, 220, 0.42f);
        }
        videoEndFrame();
    }
    return true;
}

void menuSettings(void) {
    enum { ROW_BIOS, ROW_DISPLAY, ROW_SKIPLINE, ROW_OLDRENDERER, ROW_COUNT };
    const float kBiosRowY = 44.0f, kDisplayRowY = 84.0f, kSkipLineRowY = 124.0f, kOldRendererRowY = 164.0f;

    int selected = ROW_BIOS;

    while (aptMainLoop()) {
        inputPoll();

        if (inputMenuUpPressed()) {
            selected = (selected - 1 + ROW_COUNT) % ROW_COUNT;
        }
        if (inputMenuDownPressed()) {
            selected = (selected + 1) % ROW_COUNT;
        }
        bool toggle = inputMenuConfirmPressed();

        int tx, ty;
        if (inputTouchTapped(&tx, &ty)) {
            if (ty >= kBiosRowY && ty < kBiosRowY + kRowHeight) {
                selected = ROW_BIOS;
                toggle = true;
            } else if (ty >= kDisplayRowY && ty < kDisplayRowY + kRowHeight) {
                selected = ROW_DISPLAY;
                toggle = true;
            } else if (ty >= kSkipLineRowY && ty < kSkipLineRowY + kRowHeight) {
                selected = ROW_SKIPLINE;
                toggle = true;
            } else if (ty >= kOldRendererRowY && ty < kOldRendererRowY + kRowHeight) {
                selected = ROW_OLDRENDERER;
                toggle = true;
            }
        }
        if (toggle) {
            if (selected == ROW_BIOS) {
                settingsSetForceHle(!settingsGetForceHle());
            } else if (selected == ROW_DISPLAY) {
                settingsSetDisplayOnBottom(!settingsGetDisplayOnBottom());
            } else if (selected == ROW_SKIPLINE) {
                settingsSetSkipLines(!settingsGetSkipLines());
            } else {
                settingsSetOldRenderer(!settingsGetOldRenderer());
            }
        }
        if (inputMenuBackPressed()) {
            return;
        }

        videoBeginFrame(false);
        videoDrawMenuText("Settings", 8, 8, 0.6f);

        videoDrawMenuText(selected == ROW_BIOS ? "> BIOS mode:" : "  BIOS mode:", 8, 28, 0.45f);
        videoDrawMenuText(settingsGetForceHle() ? "   Force HLE (no real BIOS)" :
            "   Auto (use a real BIOS if present)", 8, kBiosRowY, 0.42f);

        videoDrawMenuText(selected == ROW_DISPLAY ? "> Game display:" : "  Game display:", 8, 68, 0.45f);
        videoDrawMenuText(settingsGetDisplayOnBottom() ? "   Bottom screen" : "   Top screen",
            8, kDisplayRowY, 0.42f);

        videoDrawMenuText(selected == ROW_SKIPLINE ? "> Performance mode:" : "  Performance mode:", 8, 108, 0.45f);
        videoDrawMenuText(settingsGetSkipLines() ? "   Faster (skips every 2nd line)" :
            "   Full quality", 8, kSkipLineRowY, 0.42f);

        videoDrawMenuText(selected == ROW_OLDRENDERER ? "> Renderer (experimental):" : "  Renderer (experimental):",
            8, 148, 0.45f);
        videoDrawMenuText(settingsGetOldRenderer() ? "   Old (faster, may glitch)" : "   Normal",
            8, kOldRendererRowY, 0.42f);

        videoDrawMenuText("BIOS dump goes in sdmc:/3ds/pcsx_rearmed_3ds/system/", 8, 188, 0.32f);

        videoDrawMenuText("Tap a setting, or:", 8, 204, 0.38f);
        videoDrawMenuText("A/Up/Down: Select+Toggle   B: Back", 8, 220, 0.4f);
        videoEndFrame();
    }
}
