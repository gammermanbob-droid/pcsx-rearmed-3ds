// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "settings.h"

#define SETTINGS_DIR "sdmc:/3ds/pcsx_rearmed_3ds"
#define SETTINGS_PATH SETTINGS_DIR "/settings.ini"

static bool s_forceHle;
static bool s_displayOnBottom;
static bool s_skipLines;
static bool s_oldRenderer;

void settingsLoad(void) {
    s_forceHle = false;
    s_displayOnBottom = false;
    s_skipLines = true;
    s_oldRenderer = false;

    FILE* f = fopen(SETTINGS_PATH, "r");
    if (!f) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int value;
        if (sscanf(line, "%63[^=]=%d", key, &value) != 2) {
            continue;
        }
        if (strcmp(key, "force_hle") == 0) {
            s_forceHle = value != 0;
        } else if (strcmp(key, "display_on_bottom") == 0) {
            s_displayOnBottom = value != 0;
        } else if (strcmp(key, "skip_lines") == 0) {
            s_skipLines = value != 0;
        } else if (strcmp(key, "old_renderer") == 0) {
            s_oldRenderer = value != 0;
        }
    }
    fclose(f);
}

static void save(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(SETTINGS_DIR, 0777);
    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) {
        return;
    }
    fprintf(f, "force_hle=%d\n", s_forceHle ? 1 : 0);
    fprintf(f, "display_on_bottom=%d\n", s_displayOnBottom ? 1 : 0);
    fprintf(f, "skip_lines=%d\n", s_skipLines ? 1 : 0);
    fprintf(f, "old_renderer=%d\n", s_oldRenderer ? 1 : 0);
    fclose(f);
}

bool settingsGetForceHle(void) {
    return s_forceHle;
}

void settingsSetForceHle(bool force) {
    s_forceHle = force;
    save();
}

bool settingsGetDisplayOnBottom(void) {
    return s_displayOnBottom;
}

void settingsSetDisplayOnBottom(bool bottom) {
    s_displayOnBottom = bottom;
    save();
}

bool settingsGetSkipLines(void) {
    return s_skipLines;
}

void settingsSetSkipLines(bool skip) {
    s_skipLines = skip;
    save();
}

bool settingsGetOldRenderer(void) {
    return s_oldRenderer;
}

void settingsSetOldRenderer(bool old) {
    s_oldRenderer = old;
    save();
}
