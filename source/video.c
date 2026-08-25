// Copyright SweepDS Emu Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Blits the PS1 framebuffer pcsx_rearmed hands us (via
// core_glue.c's retro_video_refresh callback) to the top screen, and
// draws simple citro2d text for the file browser / pause menu on the
// bottom screen. Uses citro2d rather than hand-written PICA200 shaders
// (unlike mGBA-3ds's ctr-gpu.c, which is heavily hand-tuned) -- a
// deliberate simplicity-over-performance tradeoff for this first
// working version; PS1 output tops out around 640x480, comfortably
// within what a straightforward textured-quad blit can keep up with.

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <string.h>

#include "psx3ds.h"
#include "settings.h"

// Covers every PS1 output resolution pcsx_rearmed can produce
// (max effectively 640x480) with headroom to spare; must be a
// power-of-two per PICA200's texture requirements.
#define TEX_W 1024
#define TEX_H 512

static C3D_RenderTarget* s_top;
static C3D_RenderTarget* s_bottom;
static C3D_Tex s_gameTex;
static u16* s_scratch; // linear-heap RGB565 conversion buffer, TEX_W x TEX_H
static Tex3DS_SubTexture s_subtex;
static C2D_Image s_gameImage;
static bool s_haveFrame;
static C2D_TextBuf s_textBuf;
static C2D_Font s_font; // NULL == fall back to the citro2d system font
static C2D_SpriteSheet s_analogSheet;
static C2D_Image s_analogLogo;

bool videoInit(void) {
    gfxInitDefault();
    gfxSetWide(false);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return false;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        return false;
    }
    C2D_Prepare();

    if (R_FAILED(romfsInit())) {
        return false;
    }
    s_analogSheet = C2D_SpriteSheetLoad("romfs:/ps1_logo.t3x");
    if (!s_analogSheet) {
        return false;
    }
    s_analogLogo = C2D_SpriteSheetGetImage(s_analogSheet, 0);

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!s_top || !s_bottom) {
        return false;
    }

    if (!C3D_TexInit(&s_gameTex, TEX_W, TEX_H, GPU_RGB565)) {
        return false;
    }
    C3D_TexSetFilter(&s_gameTex, GPU_LINEAR, GPU_LINEAR);

    s_scratch = (u16*)linearAlloc((size_t)TEX_W * TEX_H * sizeof(u16));
    if (!s_scratch) {
        return false;
    }
    memset(s_scratch, 0, (size_t)TEX_W * TEX_H * sizeof(u16));

    s_gameImage.tex = &s_gameTex;
    s_gameImage.subtex = &s_subtex;

    s_textBuf = C2D_TextBufNew(4096);
    return true;
}

void videoExit(void) {
    if (s_textBuf) {
        C2D_TextBufDelete(s_textBuf);
    }
    if (s_analogSheet) {
        C2D_SpriteSheetFree(s_analogSheet);
    }
    romfsExit();
    if (s_scratch) {
        linearFree(s_scratch);
    }
    C3D_TexDelete(&s_gameTex);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

static inline u16 to565_from1555(u16 px) {
    u16 r = (px >> 10) & 0x1F, g = (px >> 5) & 0x1F, b = px & 0x1F;
    return (u16)((r << 11) | ((g << 1) << 5) | b);
}

static inline u16 to565_from8888(u32 px) {
    u16 r = (u16)((px >> 16) & 0xFF) >> 3;
    u16 g = (u16)((px >> 8) & 0xFF) >> 2;
    u16 b = (u16)(px & 0xFF) >> 3;
    return (u16)((r << 11) | (g << 5) | b);
}

void videoPresentGameFrame(const PsxFrame* frame) {
    if (!frame->data) {
        // Core asked us to just repeat the previous frame -- nothing to
        // upload, s_haveFrame/s_subtex from last time are still valid.
        return;
    }

    unsigned w = frame->width < TEX_W ? frame->width : TEX_W;
    unsigned h = frame->height < TEX_H ? frame->height : TEX_H;

    const uint8_t* src = (const uint8_t*)frame->data;
    for (unsigned y = 0; y < h; ++y) {
        u16* dstRow = s_scratch + (size_t)y * TEX_W;
        const uint8_t* srcRow = src + (size_t)y * frame->pitch;
        switch (frame->pixelFormat) {
        case 1: { // XRGB8888
            const u32* srcPx = (const u32*)srcRow;
            for (unsigned x = 0; x < w; ++x) {
                dstRow[x] = to565_from8888(srcPx[x]);
            }
            break;
        }
        case 2: { // RGB565 -- already our target layout, straight copy
            memcpy(dstRow, srcRow, (size_t)w * sizeof(u16));
            break;
        }
        default: { // 0RGB1555
            const u16* srcPx = (const u16*)srcRow;
            for (unsigned x = 0; x < w; ++x) {
                dstRow[x] = to565_from1555(srcPx[x]);
            }
            break;
        }
        }
    }

    C3D_SyncDisplayTransfer(
        (u32*)s_scratch, GX_BUFFER_DIM(TEX_W, h),
        (u32*)s_gameTex.data, GX_BUFFER_DIM(TEX_W, h),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
            GX_TRANSFER_RAW_COPY(0) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
            GX_TRANSFER_SCALING(0));

    // NOTE: citro3d textures are stored bottom-to-top -- if this comes
    // out upside down on real hardware, swap top/bottom below (can't
    // verify orientation without a real 3DS/display to test against;
    // see the project README for how to check this).
    s_subtex.width = (u16)w;
    s_subtex.height = (u16)h;
    s_subtex.left = 0.0f;
    s_subtex.right = (float)w / TEX_W;
    s_subtex.top = 1.0f;
    s_subtex.bottom = 1.0f - (float)h / TEX_H;
    s_haveFrame = true;
}

static void drawGameImage(float screenWidth) {
    if (!s_haveFrame) {
        return;
    }
    // Scale the PS1's (up to) 640x480 4:3 image to fill the target
    // screen's width, letterboxed to preserve aspect ratio -- the top
    // screen is 400px wide, the bottom 320px, so this has to take the
    // actual target width rather than assuming top.
    float scale = screenWidth / s_subtex.width;
    float drawH = s_subtex.height * scale;
    float y = (240.0f - drawH) / 2.0f;
    C2D_DrawImageAt(s_gameImage, 0.0f, y, 0.5f, NULL, scale, scale);
}

// gameplayActive selects which of the two screens the live game image
// draws to (see settingsGetDisplayOnBottom) -- irrelevant for the file
// browser/pause/settings screens (pass false there), which always show
// menu text on the bottom screen and, if set, the last game frame as a
// static backdrop on top, regardless of this setting: that setting is
// specifically about where actively-playing gameplay goes, and mixing
// it into the menu screens would fight with the menu text this same
// bottom screen needs to show there.
void videoBeginFrame(bool gameplayActive) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    if (gameplayActive && settingsGetDisplayOnBottom()) {
        C2D_TargetClear(s_top, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(s_top);

        C2D_TargetClear(s_bottom, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(s_bottom);
        drawGameImage(320.0f);
    } else {
        C2D_TargetClear(s_top, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(s_top);
        drawGameImage(400.0f);

        C2D_TargetClear(s_bottom, C2D_Color32(20, 20, 30, 255));
        C2D_SceneBegin(s_bottom);
    }
}

void videoDrawMenuText(const char* text, float x, float y, float scale) {
    C2D_Text c2dText;
    C2D_TextFontParse(&c2dText, s_font, s_textBuf, text);
    C2D_TextOptimize(&c2dText);
    C2D_DrawText(&c2dText, C2D_WithColor, x, y, 0.5f, scale, scale, C2D_Color32(255, 255, 255, 255));
}

// The touch-screen "ANALOG" toggle shown during gameplay -- always on
// the bottom (only touch-capable) screen, positioned bottom-center to
// match where the physical HOME button sits just below a real 3DS's
// bottom screen. Caller (main.c) must have already selected the bottom
// screen's scene via videoBeginFrame(true) before calling this.
void videoDrawAnalogToggle(bool enabled) {
    C2D_SceneBegin(s_bottom); // in case drawGameImage() above left the top screen selected

    // Switched from concentric circles to concentric squares: circles
    // use a separate internal citro2d draw mode from images ("Switching
    // to and from circle mode internally requires an expensive state
    // change", per C2D_DrawCircle's own doc comment), and even with an
    // explicit C2D_Flush() between the two, the ring never actually
    // showed up on real hardware -- only the logo did. Squares use the
    // exact same draw path as C2D_DrawImageAt (both are plain textured/
    // colored quads), so there's no mode switch involved at all here.
    //
    // The PlayStation logo itself is a large, solid, multi-colored
    // graphic (not a small transparent-background icon), so the red/
    // gray state distinction is a dedicated border around it rather
    // than a fill color the logo would mostly cover: white outline,
    // then a thick state-color square, then a fixed dark square behind
    // the logo so it reads the same regardless of state.
    const float half = kAnalogToggleRadius;
    C2D_DrawRectSolid(kAnalogToggleX - half - 10, kAnalogToggleY - half - 10,
        0.2f, (half + 10) * 2, (half + 10) * 2, C2D_Color32(255, 255, 255, 255));
    C2D_DrawRectSolid(kAnalogToggleX - half - 6, kAnalogToggleY - half - 6,
        0.3f, (half + 6) * 2, (half + 6) * 2,
        enabled ? C2D_Color32(200, 20, 20, 255) : C2D_Color32(80, 80, 90, 200));
    C2D_DrawRectSolid(kAnalogToggleX - half + 10, kAnalogToggleY - half + 10,
        0.4f, (half - 10) * 2, (half - 10) * 2, C2D_Color32(20, 20, 25, 255));

    // PlayStation logo badge, centered inside the dark square -- small
    // enough that the border around it stays clearly visible.
    const float logoSize = (kAnalogToggleRadius - 10) * 1.3f;
    float scale = logoSize / s_analogLogo.subtex->width;
    C2D_DrawImageAt(s_analogLogo, kAnalogToggleX - logoSize / 2.0f, kAnalogToggleY - logoSize / 2.0f,
        0.5f, NULL, scale, scale);

    videoDrawMenuText("ANALOG", kAnalogToggleX - 26, kAnalogToggleY - kAnalogToggleRadius - 20, 0.42f);
}

// Reuses the same PS1 logo sprite as the in-game ANALOG toggle -- an
// eased grow-in on the top screen, held at full size, while main.c's
// boot chime plays. No real video decode (see the boot-chime design
// discussion): this is deliberately just an animated logo synced to
// the audio's own runtime, not a frame-accurate playback of the
// source clip.
void videoDrawBootLogo(int frame) {
    C2D_TargetClear(s_top, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(s_top);

    float t = frame / 90.0f; // ~1.5s ramp at ~60fps
    if (t > 1.0f) {
        t = 1.0f;
    }
    float eased = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
    float logoSize = 90.0f + 110.0f * eased;       // grows 90px -> 200px, then holds

    float scale = logoSize / s_analogLogo.subtex->width;
    float x = (400.0f - logoSize) / 2.0f;
    float y = (240.0f - logoSize) / 2.0f;
    C2D_DrawImageAt(s_analogLogo, x, y, 0.5f, NULL, scale, scale);

    C2D_TargetClear(s_bottom, C2D_Color32(20, 20, 30, 255));
    C2D_SceneBegin(s_bottom);
    videoDrawMenuText("Tap, A, or B to skip", 100, 210, 0.42f);
}

// The real-video intro (see intro_video.c) feeds decoded pictures in
// through the exact same videoPresentGameFrame()/drawGameImage() path
// a PS1 game's own frame does (MVD's RGB565 output lines up with the
// existing case-2 passthrough in videoPresentGameFrame with zero new
// texture/blit code needed) -- this just sets up the top-screen scene
// for that, always on top regardless of settingsGetDisplayOnBottom()
// (see this function's own header comment for why that setting
// doesn't apply here).
void videoBeginIntroFrame(void) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(s_top);
    drawGameImage(400.0f);

    C2D_TargetClear(s_bottom, C2D_Color32(20, 20, 30, 255));
    C2D_SceneBegin(s_bottom);
}

void videoEndFrame(void) {
    C3D_FrameEnd(0);
    C2D_TextBufClear(s_textBuf);
}
