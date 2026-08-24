/**
 * Scene warp menu (development aid, PSP only).
 *
 * SELECT opens a scrollable list of every scene in the game; Cross loads the
 * selected one. Replaces the older L + D-Pad cycler (psp/src/psp_test_scenes.c),
 * which could only reach the 28 pre-rendered rooms and only in list order.
 *
 * Three deliberate choices:
 *
 * 1. SELECT, read RAW. The N64 controller has no SELECT and the port maps every
 *    button it does have to something the game uses, so routing this through the
 *    N64 button word would mean stealing a button. psp_raw_input.h publishes the
 *    raw pad state that os_cont.c already samples once per frame.
 *
 * 2. The list is GENERATED, not written out here -- see psp/tools/gen_scene_menu.py.
 *    Names come from the SCENE_* enum rather than the asset file names, because
 *    the enum is the readable one: SCENE_KOKIRI_FOREST for a file called `spot04`.
 *
 * 3. Everything is drawn with RAW GE CALLS -- its own 8x8 font atlas, its own
 *    sprites -- not through the N64 display-list path. This menu has to stay
 *    usable precisely when the N64 renderer is misbehaving, which is the
 *    situation it gets used in; going through GfxPrint would make the debug
 *    tool depend on the thing being debugged.
 *
 *    It also must not use pspDebugScreen, which was the first attempt. That
 *    writes pixels into the framebuffer with the CPU: correct on real hardware,
 *    but invisible under PPSSPP, which renders into a GPU texture and does not
 *    read CPU framebuffer writes back by default. Confirmed on the device that
 *    matters here -- the GE-drawn backdrop appeared, the CPU-drawn text did not.
 *
 * The frame-pacing HUD at the bottom of this file (TRIANGLE) lives here for
 * reason 3: this is the file that owns the raw-GE font and the raw-input
 * channel, and duplicating either to give the HUD its own would mean two
 * debug overlays that can disagree about the pipeline state they hand back.
 */
#include <pspctrl.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "play_state.h"

#include "controller.h"
#include "scene.h"
#include "save.h"
#include "psp_raw_input.h"
#include "psp_scene_menu.h"
#include "psp_frame_pace.h"
#include "gfx_pc.h"
#include <pspiofilemgr.h>

#include "psp_audio.h"
#include "psp_audio_probe.h"

/* Cullable-room probe, defined in src/code/z_room.c -- see the comment on
 * gPspRoomCullEntries there for what the counters distinguish. */
extern u32 gPspRoomCullEntries;
extern u32 gPspRoomCullDrawn;
extern u32 gPspRoomCullRejNear;
extern u32 gPspRoomCullRejFar;
extern s32 gPspRoomCullZFar;
extern s32 gPspRoomCullType;
extern s32 gPspRoomCullDisable;

#include <stdio.h>
#include <string.h>

/* Implemented in psp/src/gfx/gfx_scegu.c -- the font atlas is bound behind the
 * texture manager's back, same as the pre-rendered background blit does. */
extern void gfx_scegu_invalidate_texture_binding(void);

/* `entrance` is always a group BASE entrance, never one of the three layer
 * members that follow it. Play_Init indexes gEntranceTable[entranceIndex +
 * sceneLayer] and derives sceneLayer itself from age + time of day, so handing
 * it a member entrance applies the offset twice -- see the long note in
 * psp/tools/gen_scene_menu.py. `layer` records which of the four variants this
 * menu row wants; PspSceneMenu_ApplyLayer sets the world state that makes the
 * engine land on it. */
typedef struct {
    const char* name;
    s16 entrance;
    u8 layer;
} PspSceneMenuEntry;

static const PspSceneMenuEntry sEntries[] = {
#include "psp_scene_menu.inc"
};

#define ENTRY_COUNT ((s32)(sizeof(sEntries) / sizeof(sEntries[0])))
#define VISIBLE_ROWS 24
#define PAGE_STEP 10

/* Read by os_cont.c to freeze the game's own input while the menu is up --
 * otherwise the D-Pad presses used to navigate also reach the game as C-buttons
 * (that is what the PSP D-Pad is mapped to) and toggle the viewpoint underneath
 * the menu. */
s32 gPspSceneMenuOpen = 0;

static s32 sCursor = 0;
static s32 sScroll = 0;
static s16 sPendingEntrance = -1;
/* Layer wanted by the pending warp, latched alongside it. -1 == leave the
 * world state alone, which is what a debugger poke of sPendingEntrance gets. */
static s8 sPendingLayer = -1;

/* Establish the world state from which Play_Init recomputes sceneLayer, so the
 * variant the menu row names is the one that loads.
 *
 * Play_Init derives the layer from LINK_IS_ADULT and IS_DAY, and IS_DAY reads
 * nightFlag, which Play_Init itself recomputes from dayTime a few lines
 * earlier. Setting dayTime (and skyboxTime with it, or the sky would disagree
 * with the clock) is therefore the way in; writing nightFlag directly would be
 * overwritten before it was read. Noon and midnight are picked as values
 * comfortably inside each half rather than near the 6:30/18:00 boundaries. */
static void PspSceneMenu_ApplyLayer(s32 layer) {
    if (layer < 0) {
        return;
    }

    gSaveContext.save.linkAge = (layer >= SCENE_LAYER_ADULT_DAY) ? LINK_AGE_ADULT : LINK_AGE_CHILD;

    if (layer == SCENE_LAYER_CHILD_NIGHT || layer == SCENE_LAYER_ADULT_NIGHT) {
        gSaveContext.save.dayTime = CLOCK_TIME(0, 0);
    } else {
        gSaveContext.save.dayTime = CLOCK_TIME(12, 0);
    }
    gSaveContext.skyboxTime = gSaveContext.save.dayTime;

    /* A cutscene layer outranks the age/time computation entirely (Play_Init
     * checks it first), so a stale one would silently ignore everything set
     * above. The menu never wants a cutscene layer. */
    gSaveContext.save.cutsceneIndex = CS_INDEX_NONE;
    gSaveContext.nextCutsceneIndex = NEXT_CS_INDEX_NONE;
}

/* ---------------------------------------------------------------------------
 * Render hacks page (SQUARE while the warp menu is open).
 *
 * Every switch here used to be a #define in gfx_scegu.c, flipped by editing,
 * rebuilding and relaunching. That is slow, but the real cost was worse: a
 * screenshot does not say which build produced it, so two shots taken to be an
 * A/B of one hack were in fact two different scenes under two different builds,
 * and the conclusion drawn from them was wrong. Live switches remove both
 * problems -- both halves of a comparison come from one build, one scene and
 * one camera position, and PspSceneMenu_DrawHud names whatever is active.
 *
 * Kept as a table of int* rather than a switch so adding the next probe is one
 * line here plus the global it points at.
 * ------------------------------------------------------------------------- */
extern int gPspGfxHackNoTexture;
extern int gPspGfxHackPointFilter;
extern int gPspGfxHackPreferTexel1;
extern int gPspGfxLerp2Enable;
extern int gPspLerp2Force;
extern int gPspGfxTile1LoadsEnable;
extern int gDebugSkyFaceMask;
extern int gPspGfxHackHighlightBigTri;
extern const unsigned char *gPspBigTriTexAddr, *gPspBigTriPalAddr;
extern unsigned int gPspBigTriTexFmt, gPspBigTriTexSiz, gPspBigTriTexLine, gPspBigTriTexBytes;

/* Request flag for the texture dump below; cleared as soon as it has run. */
static int sDumpProbeTexture;

/* Biggest-textured-triangle probe, filled in gfx_pc.c's gfx_sp_tri1. */
extern unsigned int gPspBigTriTexW, gPspBigTriTexH;
extern unsigned int gPspBigTriTex01, gPspBigTriCcId;
extern unsigned int gPspBigTriUls, gPspBigTriUlt, gPspBigTriLrs, gPspBigTriLrt;
extern unsigned int gPspBigTriShiftS, gPspBigTriShiftT;
extern unsigned int gPspBigTriCms, gPspBigTriCmt;
/* Declared `int`, not `s32`: gfx_pc.c defines these as int32_t, and s32 is
 * `long int` here. Same width, but the two are distinct types and nothing
 * diagnoses a mismatch across translation units. */
extern int gPspBigTriU0, gPspBigTriV0, gPspBigTriU1, gPspBigTriV1, gPspBigTriU2, gPspBigTriV2;

/* Defined in gfx_scegu.c: drops every cached texture binding, which is what
 * makes a sampler-state change take effect on already-bound textures. */
extern void gfx_scegu_invalidate_texture_binding(void);

typedef struct {
    const char* name;
    int* value;
    int onValue;  /* what "on" means; the off state is always 0 */
} PspRenderHack;

static const PspRenderHack sHacks[] = {
    { "No textures (vertex colour only)", &gPspGfxHackNoTexture, 1 },
    { "Point filter (show texel size)", &gPspGfxHackPointFilter, 1 },
    { "Prefer TEXEL1 (detail layer, diagnostic)", &gPspGfxHackPreferTexel1, 1 },
    { "Disable two-pass terrain detail", &gPspGfxLerp2Enable, 0 },
    { "Second pass at FULL strength (diag)", &gPspLerp2Force, 1 },
    { "Drop tile-1 texture loads (old behaviour)", &gPspGfxTile1LoadsEnable, 0 },
    /* gPspRoomCullDisable is s32 (long int) while the renderer's own switches
     * are plain int. Both are 32 bits on this ABI, but they are distinct types
     * to the compiler, so one cast is needed to keep the table homogeneous. */
    { "Disable room culling", (int*)&gPspRoomCullDisable, 1 },
    { "Skybox: side faces only", &gDebugSkyFaceMask, 0x0F },
    { "Highlight probed triangle (magenta)", &gPspGfxHackHighlightBigTri, 1 },
    /* Not really a toggle -- flipping it on performs the dump and it is turned
     * straight back off. Living in the same list keeps one place to look. */
    { "Dump probed texture to ms0:/bigtex.bin", &sDumpProbeTexture, 1 },
};

#define HACK_COUNT ((s32)(sizeof(sHacks) / sizeof(sHacks[0])))

/* gDebugSkyFaceMask's "off" is 0xFF (all faces), not 0 (no faces), so the
 * default has to be recorded rather than assumed. */
static int sHackDefault[HACK_COUNT];
static s32 sHackDefaultsCaptured;

/* ---------------------------------------------------------------------------
 * HUD sections.
 *
 * The HUD grew one line per investigation and never lost any, so by now it
 * covers the top third of the screen -- including, during the renderer work,
 * exactly the part of the scene being looked at. Most of those lines belong to
 * the audio work and are dead weight while chasing a texture bug.
 *
 * So each block is switchable, and the default is "what the current
 * investigation needs" rather than "everything". Nothing is lost: the audio
 * lines are one tab and one press away when audio is the subject again.
 *
 * The BUILD line is deliberately NOT in this table. It is always drawn: which
 * build a screenshot came from is not a preference, and getting that wrong has
 * already cost this project two separate false conclusions.
 * ------------------------------------------------------------------------- */
enum {
    HUD_SEC_FPS,
    HUD_SEC_PATH,
    HUD_SEC_DROP,
    HUD_SEC_AUD,
    HUD_SEC_BGM,
    HUD_SEC_GATE,
    HUD_SEC_LOAD,
    HUD_SEC_BIG,
    HUD_SEC_COUNT
};

static int sHudSection[HUD_SEC_COUNT] = {
    /* FPS  */ 1, /* frame budget -- cheap and always worth seeing */
    /* PATH */ 0, /* audio decode census */
    /* DROP */ 0, /* audio note drops */
    /* AUD  */ 0, /* audio output backend */
    /* BGM  */ 0, /* sequence player */
    /* GATE */ 0, /* audio reset gate */
    /* LOAD */ 0, /* asset/blob loading */
    /* BIG  */ 1, /* biggest-triangle texture probe -- the current subject */
};

static const char* const sHudSectionName[HUD_SEC_COUNT] = {
    "FPS / frame budget", "PATH  audio decode paths", "DROP  audio note drops",
    "AUD   audio output",  "BGM   sequence player",    "GATE  audio reset gate",
    "LOAD  asset loading", "BIG   texture probe",
};

/* 0 = scene list, 1 = render hacks, 2 = HUD sections. */
enum { PAGE_MAPS, PAGE_HACKS, PAGE_HUD, PAGE_COUNT };
static s32 sPage = PAGE_MAPS;
static s32 sHackCursor = 0;
static s32 sHudCursor = 0;

/* Write the probed triangle's texture, exactly as the display list handed it
 * over, to the memory stick for off-device inspection.
 *
 * This exists because the remaining question about the soft ground cannot be
 * answered from a screenshot: the coordinate mapping has been measured correct,
 * so what is left is whether the TEXELS are right -- and those can be compared
 * byte for byte against the ROM's own asset, which no picture allows.
 *
 * Raw source bytes plus the metadata needed to decode them, rather than the
 * decoded result: this way a decoding bug in the port cannot hide itself in its
 * own output, and the same file can be checked against the extracted asset.
 * ms0:/ is the memory stick root, which is ~/.config/ppsspp/ on this host. */
static void PspSceneMenu_DumpProbeTexture(void) {
    SceUID fd;
    u32 header[8];

    if (gPspBigTriTexAddr == NULL || gPspBigTriTexBytes == 0 ||
        gPspBigTriTexBytes > 64 * 1024) {
        return;
    }

    fd = sceIoOpen("ms0:/bigtex.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }

    header[0] = 0x42494754; /* "BIGT" -- so a stale file is recognisable */
    header[1] = gPspBigTriTexFmt;
    header[2] = gPspBigTriTexSiz;
    header[3] = gPspBigTriTexLine;
    header[4] = gPspBigTriTexBytes;
    header[5] = gPspBigTriTexW;
    header[6] = gPspBigTriTexH;
    header[7] = gPspBigTriCcId;
    sceIoWrite(fd, header, sizeof(header));
    sceIoWrite(fd, gPspBigTriTexAddr, gPspBigTriTexBytes);
    /* Always 1 KB, whether or not this format uses it: a fixed layout is easier
     * to parse than a conditional one, and 1 KB costs nothing. */
    if (gPspBigTriPalAddr != NULL) {
        sceIoWrite(fd, gPspBigTriPalAddr, 1024);
    }
    sceIoClose(fd);
}

static void PspSceneMenu_ToggleHack(s32 i) {
    const PspRenderHack* h = &sHacks[i];

    if (h->value == &sDumpProbeTexture) {
        PspSceneMenu_DumpProbeTexture();
        return;
    }

    *h->value = (*h->value == h->onValue) ? sHackDefault[i] : h->onValue;

    /* A sampler-mode change only reaches the hardware when a texture is
     * (re)bound, and gfx_scegu caches those bindings. Without this, toggling
     * the point filter appeared to do nothing until enough textures had been
     * evicted naturally. */
    gfx_scegu_invalidate_texture_binding();
}

/* Held-direction auto-repeat: 110 entries is far too many to step through one
 * press at a time. */
static s32 sRepeatTimer = 0;
static s32 sFontReady = 0;
/* On by default: this build exists to measure the frame budget, and an
 * overlay nobody knows to press TRIANGLE for measures nothing. */
static s32 sHudOpen = 1;

/* 0 = unchecked, 1 = armed, 2 = done. */
static s32 sAutoCaptureState;
static s32 sAutoCaptureTimer;

static void PspSceneMenu_Move(s32 delta) {
    if (sPage == PAGE_HACKS) {
        sHackCursor += delta;
        if (sHackCursor < 0) {
            sHackCursor = HACK_COUNT - 1;
        } else if (sHackCursor >= HACK_COUNT) {
            sHackCursor = 0;
        }
        return;
    }
    if (sPage == PAGE_HUD) {
        sHudCursor += delta;
        if (sHudCursor < 0) {
            sHudCursor = HUD_SEC_COUNT - 1;
        } else if (sHudCursor >= HUD_SEC_COUNT) {
            sHudCursor = 0;
        }
        return;
    }

    sCursor += delta;
    if (sCursor < 0) {
        sCursor = ENTRY_COUNT - 1;
    } else if (sCursor >= ENTRY_COUNT) {
        sCursor = 0;
    }

    if (sCursor < sScroll) {
        sScroll = sCursor;
    } else if (sCursor >= sScroll + VISIBLE_ROWS) {
        sScroll = sCursor - VISIBLE_ROWS + 1;
    }
}

/**
 * Runs every frame from Play_Update. Only the actual scene load needs a
 * PlayState; opening, closing and moving the cursor do not, which is why the
 * pending entrance is latched into a static and consumed here rather than the
 * menu reaching for a PlayState it may not have.
 */
void PspSceneMenu_Update(PlayState* play) {
    s32 held;

    /* Self-heals the func_800FAD34()/D_80133418 audio reset-gate deadlock --
     * see psp_audio_debug.c's PspAudioDebug_HealResetGate for why this is
     * safe and necessary. Runs every frame regardless of menu/HUD state. */
    {
        extern void PspAudioDebug_HealResetGate(void);
        PspAudioDebug_HealResetGate();
    }

    if (!sHackDefaultsCaptured) {
        s32 i;

        for (i = 0; i < HACK_COUNT; i++) {
            sHackDefault[i] = *sHacks[i].value;
        }
        sHackDefaultsCaptured = 1;
    }

    if (PSP_RAW_PRESSED(PSP_CTRL_SELECT)) {
        gPspSceneMenuOpen = !gPspSceneMenuOpen;
        sRepeatTimer = 0;
    }

    /* Consumed BEFORE the "menu closed" early-out, and on every frame until it
     * takes. It used to sit at the end of this function, i.e. after that
     * return, which made it reachable only on the single frame the Cross press
     * cleared gPspSceneMenuOpen and fell through. If that one frame happened to
     * land on a transition already in flight, the guard below refused it and
     * the warp was lost for good -- while sPendingEntrance stayed set, so it
     * fired unbidden the next time the menu was opened.
     *
     * The guard itself is right and stays: a transition already in flight owns
     * nextEntranceIndex, and starting a second one on top of it loads two
     * scenes over each other. Retrying next frame is what it wanted all along.
     *
     * Being frame-driven rather than press-driven also makes the menu warp
     * usable from the debugger -- poke sPendingEntrance and the game goes,
     * which is what drives the automated scene sweep. */
    if (sPendingEntrance >= 0 && play != NULL && play->transitionTrigger == TRANS_TRIGGER_OFF) {
        PspSceneMenu_ApplyLayer(sPendingLayer);
        sPendingLayer = -1;
        play->nextEntranceIndex = sPendingEntrance;
        play->transitionTrigger = TRANS_TRIGGER_START;
        play->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
        gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
        sPendingEntrance = -1;
    }

    /* HUD and pace override sit OUTSIDE the "menu closed" early-out on
     * purpose: the whole point of the framerate knob is to feel the difference
     * while playing, not while a full-screen list covers the game. Neither
     * button is mapped to anything in os_cont.c, so nothing is being stolen. */
    /* Unattended measurement, armed only by ms0:/oot_autocapture.txt so an
     * ordinary session is untouched. The probe itself is always collecting
     * (it costs one comparison per voice); this just decides when to write
     * what it has. File IO stays on the main thread -- doing it on the audio
     * thread was audible. */
    if (sAutoCaptureState == 0) {
        SceUID marker = sceIoOpen("ms0:/oot_autocapture.txt", PSP_O_RDONLY, 0777);

        if (marker >= 0) {
            sceIoClose(marker);
            sAutoCaptureState = 1;
            sAutoCaptureTimer = 0;
        } else {
            sAutoCaptureState = 2;
        }
    } else if (sAutoCaptureState == 1) {
        if (++sAutoCaptureTimer > 900) { /* ~30 s of gameplay frames */
            PspAudioProbe_Flush();
            sAutoCaptureState = 2;
        }
    }

    if (PSP_RAW_PRESSED(PSP_CTRL_TRIANGLE)) {
        /* L is the modifier rather than a button of its own: TRIANGLE and
         * SQUARE are the only two the N64 pad mapping leaves free, and both
         * are already spoken for. L doubles as BTN_Z in game, but a Z press
         * that also lands on TRIANGLE is not something that happens by
         * accident. */
        if (gPspRawButtons & PSP_CTRL_LTRIGGER) {
            gPspRoomCullDisable = !gPspRoomCullDisable;
        /* The R+TRIANGLE fanfare hotkey that lived here is gone. It was only
         * ever a bring-up probe for "does any sequence play at all", and once
         * real sequences play it is actively harmful: unlike L (which doubles
         * as BTN_Z, a deliberate press), R is used constantly in normal play,
         * so the combo fired the Song of Time at random during gameplay. */
        } else {
            sHudOpen = !sHudOpen;
        }
    }
    if (sHudOpen && !gPspSceneMenuOpen && PSP_RAW_PRESSED(PSP_CTRL_SQUARE)) {
        /* off -> 3 -> 2 -> 1 -> off. "off" is not the same as 3: it hands
         * R_UPDATE_RATE back to the engine, which drives it to 1 during
         * transitions and 2 in the pause menu. */
        switch (gPspPaceOverride) {
            case 0:  gPspPaceOverride = 3; break;
            case 3:  gPspPaceOverride = 2; break;
            case 2:  gPspPaceOverride = 1; break;
            default: gPspPaceOverride = 0; break;
        }
    }

    if (!gPspSceneMenuOpen) {
        return;
    }

    if (PSP_RAW_PRESSED(PSP_CTRL_UP)) {
        PspSceneMenu_Move(-1);
        sRepeatTimer = -18; /* longer delay before the first repeat */
    } else if (PSP_RAW_PRESSED(PSP_CTRL_DOWN)) {
        PspSceneMenu_Move(1);
        sRepeatTimer = -18;
    } else if (PSP_RAW_PRESSED(PSP_CTRL_LEFT)) {
        /* Left/Right switch tabs -- the obvious gesture for a tab strip, and
         * worth taking even though it displaced the list's +-10 jump, which
         * moved onto the shoulder buttons (free inside the menu). */
        sPage = (sPage + PAGE_COUNT - 1) % PAGE_COUNT;
        sRepeatTimer = 0;
    } else if (PSP_RAW_PRESSED(PSP_CTRL_RIGHT)) {
        sPage = (sPage + 1) % PAGE_COUNT;
        sRepeatTimer = 0;
    } else if (PSP_RAW_PRESSED(PSP_CTRL_LTRIGGER)) {
        PspSceneMenu_Move(-PAGE_STEP);
    } else if (PSP_RAW_PRESSED(PSP_CTRL_RTRIGGER)) {
        PspSceneMenu_Move(PAGE_STEP);
    }

    held = (gPspRawButtons & (PSP_CTRL_UP | PSP_CTRL_DOWN)) ? 1 : 0;
    if (held) {
        if (++sRepeatTimer >= 3) {
            sRepeatTimer = 0;
            PspSceneMenu_Move((gPspRawButtons & PSP_CTRL_DOWN) ? 1 : -1);
        }
    } else {
        sRepeatTimer = 0;
    }

    if (PSP_RAW_PRESSED(PSP_CTRL_CROSS)) {
        if (sPage == PAGE_HUD) {
            sHudSection[sHudCursor] = !sHudSection[sHudCursor];
        } else if (sPage == PAGE_HACKS) {
            /* Deliberately leaves the menu open: a hack is something you flip
             * back and forth, unlike a warp, which is done once. */
            PspSceneMenu_ToggleHack(sHackCursor);
        } else {
            sPendingEntrance = sEntries[sCursor].entrance;
            sPendingLayer = (s8)sEntries[sCursor].layer;
            gPspSceneMenuOpen = 0;
        }
    } else if (PSP_RAW_PRESSED(PSP_CTRL_CIRCLE)) {
        gPspSceneMenuOpen = 0;
    }

}

#define MENU_SCR_W 480
#define MENU_SCR_H 272
#define GLYPH_W 8
#define GLYPH_H 8
#define FONT_FIRST 32
#define FONT_LAST 126
#define ATLAS_W 128
#define ATLAS_H 64
#define ATLAS_COLS (ATLAS_W / GLYPH_W)

/* 1 bit per pixel, 8 bytes per glyph -- see psp/tools/gen_menu_font.py. */
static const u8 sFontBits[] = {
#include "psp_menu_font.inc"
};

/* Expanded once into a GE-samplable atlas. 8888 rather than a paletted format
 * so no CLUT state has to be set up and torn down around a draw that sits in
 * the middle of the game's own rendering. 16-byte aligned because the GE reads
 * it directly. */
static u32 sFontAtlas[ATLAS_W * ATLAS_H] __attribute__((aligned(16)));

typedef struct {
    u16 u, v;
    u32 color;
    s16 x, y, z;
} MenuVertex;

static void PspSceneMenu_BuildFont(void) {
    s32 count = FONT_LAST - FONT_FIRST + 1;
    s32 i;

    for (i = 0; i < ATLAS_W * ATLAS_H; i++) {
        sFontAtlas[i] = 0;
    }

    for (i = 0; i < count; i++) {
        s32 cx = (i % ATLAS_COLS) * GLYPH_W;
        s32 cy = (i / ATLAS_COLS) * GLYPH_H;
        s32 row;

        for (row = 0; row < GLYPH_H; row++) {
            u8 bits = sFontBits[i * GLYPH_H + row];
            s32 col;

            for (col = 0; col < GLYPH_W; col++) {
                if (bits & (0x80 >> col)) {
                    /* White, opaque. Everything else stays fully transparent,
                     * so one blended draw puts text over the backdrop. */
                    sFontAtlas[(cy + row) * ATLAS_W + (cx + col)] = 0xFFFFFFFFu;
                }
            }
        }
    }

    /* The GE reads main memory and is not coherent with the CPU's data cache. */
    sceKernelDcacheWritebackRange(sFontAtlas, sizeof(sFontAtlas));
}

/* Appends one string's sprites to *vp. Two vertices per character; blanks are
 * skipped rather than drawn as empty quads. */
static void PspSceneMenu_PutString(MenuVertex** vp, s32 x, s32 y, u32 color, const char* str) {
    MenuVertex* v = *vp;

    for (; *str != '\0'; str++, x += GLYPH_W) {
        u8 c = (u8)*str;
        s32 idx;
        s32 ax;
        s32 ay;

        if (c == ' ' || c < FONT_FIRST || c > FONT_LAST) {
            continue;
        }

        idx = c - FONT_FIRST;
        ax = (idx % ATLAS_COLS) * GLYPH_W;
        ay = (idx / ATLAS_COLS) * GLYPH_H;

        v[0].u = ax;
        v[0].v = ay;
        v[0].color = color;
        v[0].x = x;
        v[0].y = y;
        v[0].z = 0;

        v[1].u = ax + GLYPH_W;
        v[1].v = ay + GLYPH_H;
        v[1].color = color;
        v[1].x = x + GLYPH_W;
        v[1].y = y + GLYPH_H;
        v[1].z = 0;

        v += 2;
    }

    *vp = v;
}

/* Draws one dark panel. Both overlays want a readable ground under their text
 * and neither wants to depend on what the game left in the pipeline. */
static void PspSceneMenu_FillPanel(s32 x0, s32 y0, s32 x1, s32 y1, u32 color) {
    struct {
        u32 color;
        s16 x, y, z;
    }* bg = sceGuGetMemory(sizeof(*bg) * 2);

    bg[0].color = color;
    bg[0].x = x0;
    bg[0].y = y0;
    bg[0].z = 0;
    bg[1].color = color;
    bg[1].x = x1;
    bg[1].y = y1;
    bg[1].z = 0;

    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, bg);
}

/* Binds the font atlas, draws everything PspSceneMenu_PutString appended
 * between `verts` and `v`, then hands the pipeline back. */
static void PspSceneMenu_SubmitText(MenuVertex* verts, MenuVertex* v) {
    if (v != verts) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, sFontAtlas);
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       (int)(v - verts), 0, verts);
    }

    /* Hand the pipeline back roughly as found. Texturing stays ENABLED because
     * gfx_pc.c's rendering_state assumes it is; the texture BINDING is
     * invalidated so the next textured draw re-binds instead of trusting a
     * cache entry that now points at our font atlas. Depth and alpha test are
     * re-established per frame in gfx_scegu_start_frame. */
    sceGuEnable(GU_TEXTURE_2D);
    gfx_scegu_invalidate_texture_binding();
}

/**
 * Called from gfx_scegu_end_frame while the GE list is still open (before
 * sceGuFinish), so this is ordinary GE geometry submitted into the same frame
 * the game just drew.
 */
void PspSceneMenu_DrawBackdrop(void) {
    MenuVertex* verts;
    MenuVertex* v;
    s32 last;
    s32 i;
    s32 maxChars;

    if (!gPspSceneMenuOpen) {
        return;
    }

    if (!sFontReady) {
        PspSceneMenu_BuildFont();
        sFontReady = 1;
    }

    /* ABGR, near-opaque dark blue */
    PspSceneMenu_FillPanel(0, 0, MENU_SCR_W, MENU_SCR_H, 0xE0301808);

    /* --- text ------------------------------------------------------------ */
    last = sScroll + VISIBLE_ROWS;
    if (last > ENTRY_COUNT) {
        last = ENTRY_COUNT;
    }

    /* Worst case: the header plus every visible row, each capped at the screen
     * width. Allocating for the cap rather than measuring keeps this a single
     * sceGuGetMemory with no chance of running past the end. */
    maxChars = (MENU_SCR_W / GLYPH_W) * (VISIBLE_ROWS + 2);
    verts = sceGuGetMemory(sizeof(MenuVertex) * 2 * maxChars);
    v = verts;

    /* Tab strip. Drawn on every page, with the active one highlighted, so the
     * other pages are discoverable rather than something you have to be told
     * about. */
    {
        static const char* const kTabs[PAGE_COUNT] = { "MAPS", "HACKS", "HUD" };
        s32 x = 8;

        for (i = 0; i < PAGE_COUNT; i++) {
            PspSceneMenu_PutString(&v, x, 6, (i == sPage) ? 0xFF00FFFF : 0xFF808080, kTabs[i]);
            x += (s32)strlen(kTabs[i]) * GLYPH_W + 16;
        }
        PspSceneMenu_PutString(&v, x + 8, 6, 0xFF80FFFF,
                               (sPage == PAGE_MAPS) ? "<> TAB  L/R +-10  X LOAD  O CLOSE"
                                                    : "<> TAB  X TOGGLE  O CLOSE");
    }

    if (sPage == PAGE_HACKS) {
        for (i = 0; i < HACK_COUNT; i++) {
            s32 y = 24 + i * (GLYPH_H + 2);
            s32 selected = (i == sHackCursor);
            s32 on = (*sHacks[i].value == sHacks[i].onValue);

            if (selected) {
                PspSceneMenu_PutString(&v, 8, y, 0xFF00FFFF, ">");
            }
            PspSceneMenu_PutString(&v, 24, y, on ? 0xFF00FF00 : 0xFF808080, on ? "[X]" : "[ ]");
            PspSceneMenu_PutString(&v, 56, y, selected ? 0xFF00FFFF : 0xFFFFFFFF, sHacks[i].name);
        }

        PspSceneMenu_SubmitText(verts, v);
        return;
    }

    if (sPage == PAGE_HUD) {
        for (i = 0; i < HUD_SEC_COUNT; i++) {
            s32 y = 24 + i * (GLYPH_H + 2);
            s32 selected = (i == sHudCursor);

            if (selected) {
                PspSceneMenu_PutString(&v, 8, y, 0xFF00FFFF, ">");
            }
            PspSceneMenu_PutString(&v, 24, y, sHudSection[i] ? 0xFF00FF00 : 0xFF808080,
                                   sHudSection[i] ? "[X]" : "[ ]");
            PspSceneMenu_PutString(&v, 56, y, selected ? 0xFF00FFFF : 0xFFFFFFFF, sHudSectionName[i]);
        }

        PspSceneMenu_SubmitText(verts, v);
        return;
    }

    for (i = sScroll; i < last; i++) {
        s32 y = 24 + (i - sScroll) * (GLYPH_H + 2);
        s32 selected = (i == sCursor);

        if (selected) {
            PspSceneMenu_PutString(&v, 8, y, 0xFF00FFFF, ">");
        }
        PspSceneMenu_PutString(&v, 24, y, selected ? 0xFF00FFFF : 0xFFFFFFFF, sEntries[i].name);
    }

    PspSceneMenu_SubmitText(verts, v);
}

/* ---------------------------------------------------------------------------
 * Frame-pacing HUD (TRIANGLE toggles, SQUARE cycles the rate override).
 *
 * The one number worth reading here is HEADROOM: interval minus work. The
 * pacer holds each frame for R_UPDATE_RATE PSP vblanks, so the budget is
 * 50.0 / 33.4 / 16.7 ms at rate 3 / 2 / 1. Dropping a step is affordable
 * exactly when WORK already fits in the next step down -- and WORK is measured
 * with the pacer's sleep excluded, so it answers that directly instead of
 * making it inferable from a framerate that the pacer pins anyway.
 *
 * Deliberately no floats through sprintf: newlib's %f drags in a large
 * formatter, and everything here is naturally fixed-point in tenths.
 * ------------------------------------------------------------------------- */

/* Which build is actually running.
 *
 * Two separate hours were lost to not knowing this: once comparing two
 * screenshots that turned out to come from different builds, and once reading
 * probe globals over the debugger at symbol addresses that had shifted under a
 * rebuild the running game had never loaded -- which yields plausible-looking
 * numbers, not an error. Both failure modes are silent, so the build has to say
 * who it is. Non-static and non-const so the debugger can find it by symbol and
 * a screenshot can show it. */
/* Defined in the generated psp/build/build_stamp.c, which the Makefile
 * regenerates on every build. Deliberately not __DATE__/__TIME__ here: that
 * expands when THIS file is compiled, so a build that only touched the renderer
 * left the stamp naming an older build -- authoritative-looking and wrong. */
extern char gPspBuildId[24];

/* Set by the DROP line below, read by its colour decision further down. */
static u32 sDropTotal;

/* The "which hacks are forced" line, built early because the HUD panel has to
 * be sized before any text is drawn into it. Length <= 4 means "just the HACK
 * prefix", i.e. nothing is active and the line is suppressed. */
static char sHackLine[80];
static s32 sHackLineLen;

#define HUD_X 6
#define HUD_Y 6

void PspSceneMenu_DrawHud(void) {
    MenuVertex* verts;
    MenuVertex* v;
    char line0[64];
    char line1[64];
    char line2[64];
    char line3[64];
    char line4[64];
    char line5[64];
    char line6[72];
    int gateVal;
    u32 workUsec;
    u32 frameUsec;
    u32 fps10;
    s32 headroom10;

    /* Suppressed under the warp menu: its list runs the full height of the
     * screen behind an opaque backdrop, so the two would overlap, and a
     * framerate measured while a full-screen debug list is being drawn is not
     * the framerate anyone wants to read. */
    if (!sHudOpen || gPspSceneMenuOpen) {
        return;
    }

    if (!sFontReady) {
        PspSceneMenu_BuildFont();
        sFontReady = 1;
    }

    workUsec = gPspFramePace.work_usec;
    frameUsec = gPspFramePace.frame_usec;

    /* Zero until the first averaging window completes -- show the labels with
     * blank numbers rather than dividing by it. */
    fps10 = (frameUsec != 0) ? (u32)((10000000u + frameUsec / 2) / frameUsec) : 0;
    headroom10 = (s32)((frameUsec + 50) / 100) - (s32)((workUsec + 50) / 100);

    {
        s32 work10 = (s32)((workUsec + 50) / 100);
        s32 head = (headroom10 < 0) ? -headroom10 : headroom10;

        sprintf(line0, "FPS %d.%d  WORK %d.%dms  HEAD %s%d.%dms", (int)(fps10 / 10), (int)(fps10 % 10),
                (int)(work10 / 10), (int)(work10 % 10), (headroom10 < 0) ? "-" : "", (int)(head / 10),
                (int)(head % 10));
    }

    /* Temporarily repurposed from RATE/TRI/TEX: heal = how many times the
     * gate watchdog has had to force-clear D_80133418 (>1 confirms resets
     * keep re-arming the gate, not just the one we already knew about);
     * resets = how many distinct heap-reset cycles (resetStatus hitting 5)
     * have run at all -- tells us whether Audio_Update's body is truly
     * stuck vs. resets are continuously restarting and never leaving it
     * more than a frame or two of open gate to work with. */
    {
        extern void PspAudioDebug_HealStats(unsigned int* healCount, unsigned int* resetStartCount);
        unsigned int healCount, resetStartCount;

        PspAudioDebug_HealStats(&healCount, &resetStartCount);
        {
            /* Decode-path census -- which branches of the software microcode
             * are actually exercised. 2p (two-part notes), bk (non-zero book
             * offset) and hs (Haas delay) are the rare ones and the ones a
             * hand-ported microcode is most likely to have wrong; a count
             * that grows at the rate the artefact is heard names the
             * suspect, one that stays 0 eliminates it. See
             * psp/src/audio/psp_audio_probe.c. */
            extern u32 gPspPathAdpcm, gPspPathSmallAdpcm, gPspPathS8, gPspPathS16;
            extern u32 gPspPathTwoParts, gPspPathBookOffset, gPspPathHaas;

            sprintf(line1, "PATH ad %u sm %u s8 %u 16 %u 2p %u bk %u hs %u", (unsigned)gPspPathAdpcm,
                    (unsigned)gPspPathSmallAdpcm, (unsigned)gPspPathS8, (unsigned)gPspPathS16,
                    (unsigned)gPspPathTwoParts, (unsigned)gPspPathBookOffset, (unsigned)gPspPathHaas);
        }
        (void)healCount;
        (void)resetStartCount;
    }

    /* Cullable rooms only. A ROOM_SHAPE_TYPE_NORMAL room draws every entry
     * unconditionally, so the counters would be stale leftovers from the last
     * cullable room and worse than no line at all. */
    /* SHAPE is the room shape type actually in force (0 normal, 1 image,
     * 2 cullable). It disambiguates a 0/0 cull line: shape 2 with 0 entries is
     * a data problem, any other shape means there is nothing to cull and the
     * zeroes are expected. */
    /* DROP: why notes go missing, straight from the engine's own account
     * (gAudioCtx.audioErrorFlags, drained on the audio thread -- see
     * psp_audio_debug.c). Each counter points at a different fix:
     *   ins  an instrument was missing or out of range -> soundfont data
     *   drm  a drum or sound effect was missing        -> soundfont data
     *   fnt  the font had not finished loading yet     -> load/DMA timing
     *   alc  no free voice could be allocated or stolen -> voice starvation,
     *        nothing to do with the data at all
     * last is the raw code of the most recent error: high byte = class,
     * low half = (fontId << 8) | id, so it names the exact culprit.
     * All zero while music plays means missing notes are NOT being dropped
     * at the sequencer level, and the fault is further down in synthesis. */
    {
        extern void PspAudioDebug_ErrorSummary(u32* total, u32* last, u32* instrument, u32* drumSfx,
                                               u32* fontLoad, u32* allocFails);
        u32 eTot, eLast, eIns, eDrm, eFnt, eAlc;

        PspAudioDebug_ErrorSummary(&eTot, &eLast, &eIns, &eDrm, &eFnt, &eAlc);
        /* ovr is the session-4 overrun probe's live hit count (it was only
         * ever dumped to a file after the fact). On screen it separates the
         * two competing explanations for a burst of noise WHILE it is being
         * heard: ovr climbing means samplePosInt is still running past the
         * end of a sample; ovr frozen at 0 while the artefact is audible
         * rules that mechanism out on the spot and points somewhere else. */
        sprintf(line2, "DROP ins %u drm %u fnt %u alc %u ovr %u last %08x", (unsigned)eIns, (unsigned)eDrm,
                (unsigned)eFnt, (unsigned)eAlc, (unsigned)PspAudioProbe_StatHits(), (unsigned)eLast);
        sDropTotal = eTot + PspAudioProbe_StatHits();
    }

    /* AUD: calls = how many times PspAudio_Output has run at all (0 forever
     * means synthesis/retrace never reaches the output backend -- a thread/
     * IrqMgr wiring bug, not a mixing bug). PEAK is the last buffer's max
     * abs(sample); calls>0 but PEAK always 0 means real silent PCM reached
     * the hardware (a volume/mixing bug upstream, not this backend). RSVF
     * counts sceAudioSRCChReserve failures. */
    {
        sprintf(line3, "AUD calls %u n %u peak %d rsvf %u", (unsigned)PspAudio_StatOutputCalls(),
                (unsigned)PspAudio_StatLastNumSamples(), (int)PspAudio_StatLastPeakSample(),
                (unsigned)PspAudio_StatReserveFailures());
    }

    /* SEQ_PLAYER_BGM_MAIN (0) -- the player that actually carries the game's
     * music. This line used to watch SEQ_PLAYER_FANFARE (1) because the only
     * sound this port could make was a hand-fired test fanfare; that hotkey
     * is long gone and the fanfare player is idle in normal play, so the
     * line was reporting on nothing.
     *
     * en/st/id say whether a sequence is running at all; fadeVol (x1000)
     * whether an enabled player's volume envelope is actually nonzero; notes
     * is the engine-wide active-note count. Notes dropping to 0 (or dipping)
     * while music should be playing is the direct read-out for "a tone went
     * missing", and pairs with the DROP line above: DROP says whether the
     * note was refused on the way in, this says whether it is sounding. */
    {
        extern int PspAudioDebug_ActiveNoteCount(void);
        extern int PspAudioDebug_SyntheticNoteCount(void);
        extern void PspAudioDebug_PlayerInfo(int playerIdx, int* enabled, int* seqId, int* state,
                                              int* fadeVolumeX1000);
        int en, seqId, state, fadeX1000;

        PspAudioDebug_PlayerInfo(0, &en, &seqId, &state, &fadeX1000);
        sprintf(line4, "BGM en %d id %d st %d vol %d notes %d syn %d", en, seqId, state, fadeX1000,
                PspAudioDebug_ActiveNoteCount(), PspAudioDebug_SyntheticNoteCount());
    }

    /* func_800FAD34()'s gate: while D_80133418 != 0, Audio_Update's entire
     * body (including AudioThread_ScheduleProcessCmds, which is what hands
     * queued SEQCMD_*s to the audio thread at all) is skipped every frame.
     * gate!=0 forever is the smoking gun for "nothing ever plays". */
    {
        extern void PspAudioDebug_ResetGateInfo(int* d80133418, int* resetStatus, int* specId);
        int resetStatus, specId;

        PspAudioDebug_ResetGateInfo(&gateVal, &resetStatus, &specId);
        sprintf(line5, "GATE d418 %d resetSt %d specId %d", gateVal, resetStatus, specId);
    }

    /* LOAD is the one thing every earlier line takes for granted: that real
     * sequence and soundfont bytes actually arrived. perm counts permanent-
     * pool allocations (3 expected here: Sequence_0 + Soundfont_0/1); sq/fn
     * are their LOAD_STATUS_* values; b0 is the first byte of the loaded SFX
     * sequence, which is 0 when the blob lookup silently returned nothing.
     * blob H/M are PspBlob_Read's hit/miss counters -- a climbing miss count
     * while audio plays means a romAddr the registry does not cover. acmd is
     * how many DSP commands the software microcode executed last frame;
     * 0 there with notes active means synthesis is not running at all. */
    {
        extern void PspAudioDebug_LoadInfo(int* permEntries, int* seqStatus0, int* fontStatus0, int* seqDataByte);
        extern unsigned int gPspBlobHits;
        extern unsigned int gPspBlobMisses;
        extern unsigned int gPspBlobLastMissVrom;
        extern unsigned int PspAudioMixer_StatCommands(void);
        int perm, sq, fn, b0;

        PspAudioDebug_LoadInfo(&perm, &sq, &fn, &b0);
        sprintf(line6, "LOAD perm %d fn %d blob %u/%u miss@%08x acmd %u", perm, fn, gPspBlobHits,
                gPspBlobMisses, gPspBlobLastMissVrom, PspAudioMixer_StatCommands());
        (void)sq;
        (void)b0;
    }

    /* Built before the panel is sized, because whether any hack is active
     * decides whether there is an eighth row to make room for. */
    {
        s32 i;

        sHackLineLen = sprintf(sHackLine, "HACK");
        for (i = 0; i < HACK_COUNT; i++) {
            if (*sHacks[i].value == sHacks[i].onValue && *sHacks[i].value != sHackDefault[i]) {
                /* Short tags, not the menu's full sentences: this line has to
                 * survive next to six others on a 480px screen. */
                static const char* const kTags[] = { " notex", " point", " texel1", " no2pass", " full2", " nocull", " sky4",
                                                     " hilite", " dump" };

                sHackLineLen += sprintf(sHackLine + sHackLineLen, "%s", kTags[i]);
            }
        }
    }

    /* Lines are collected first and drawn afterwards, so that switching a
     * section off closes the gap instead of leaving a hole -- and so the panel
     * can be sized to what is actually going to be shown. The BUILD line is
     * pushed unconditionally and first; see the note on sHudSection.
     *
     * Note the numbers above are still COMPUTED for switched-off sections. The
     * probes are a handful of counter reads, far cheaper than the sprintf and
     * the text drawing this now skips, and leaving them running means a section
     * switched on mid-scene shows real values immediately rather than stale
     * ones. */
    {
        const char* text[HUD_SEC_COUNT + 4];
        u32 colour[HUD_SEC_COUNT + 4];
        s32 n = 0;
        s32 k;
        char buildLine[40];
        char big0[80];
        char big1[80];

        sprintf(buildLine, "BUILD %s", gPspBuildId);
        text[n] = buildLine;
        colour[n++] = 0xFF808080;

        if (sHudSection[HUD_SEC_FPS]) {
            /* Red once work no longer fits the interval: the pacer is no longer
             * the thing setting the framerate, the renderer is. */
            text[n] = line0;
            colour[n++] = (headroom10 < 0) ? 0xFF4040FF : 0xFF80FFFF;
        }
        if (sHudSection[HUD_SEC_PATH]) {
            text[n] = line1;
            colour[n++] = 0xFFFFFFFF;
        }
        if (sHudSection[HUD_SEC_DROP]) {
            /* Red whenever notes were refused -- a state that is invisible in
             * the picture by definition. */
            text[n] = line2;
            colour[n++] = (sDropTotal != 0) ? 0xFF4040FF : 0xFF80FF80;
        }
        if (sHudSection[HUD_SEC_AUD]) {
            /* Red until at least one real, non-silent buffer has gone out. */
            text[n] = line3;
            colour[n++] = (PspAudio_StatLastPeakSample() != 0) ? 0xFF80FF80 : 0xFF4040FF;
        }
        if (sHudSection[HUD_SEC_BGM]) {
            text[n] = line4;
            colour[n++] = 0xFFFFFFFF;
        }
        if (sHudSection[HUD_SEC_GATE]) {
            text[n] = line5;
            colour[n++] = (gateVal != 0) ? 0xFF4040FF : 0xFF80FF80;
        }
        if (sHudSection[HUD_SEC_LOAD]) {
            text[n] = line6;
            colour[n++] = 0xFFFFFFFF;
        }

        /* Biggest horizontal textured triangle this frame -- the ground,
         * outdoors. `rep` is the derived number worth reading: how many times
         * the tile repeats across it. `tex` is 3 when the material wants a
         * second texture this single-TMU pipeline cannot show. */
        if (sHudSection[HUD_SEC_BIG]) {
            s32 du = gPspBigTriU1 - gPspBigTriU0;
            s32 dv = gPspBigTriV1 - gPspBigTriV0;
            s32 repU10 = 0;
            s32 repV10 = 0;

            if (du < 0) { du = -du; }
            if (dv < 0) { dv = -dv; }
            /* u is S10.5 texels, so u/32 is texels; x10 for one decimal. */
            if (gPspBigTriTexW) { repU10 = (du * 10) / (32 * (s32)gPspBigTriTexW); }
            if (gPspBigTriTexH) { repV10 = (dv * 10) / (32 * (s32)gPspBigTriTexH); }

            /* Kept under ~50 characters each: the panel is 54 glyphs wide and
             * an overrun runs off the edge of a screenshot, which cost a round
             * trip once already. */
            sprintf(big0, "BIG %ux%u sh%u,%u cm%u,%u tex%u cc%08x", gPspBigTriTexW, gPspBigTriTexH,
                    gPspBigTriShiftS, gPspBigTriShiftT, gPspBigTriCms, gPspBigTriCmt,
                    gPspBigTriTex01, gPspBigTriCcId);
            sprintf(big1, "    rep %d.%d/%d.%d tile %u,%u-%u,%u", repU10 / 10, repU10 % 10,
                    repV10 / 10, repV10 % 10, gPspBigTriUls, gPspBigTriUlt, gPspBigTriLrs,
                    gPspBigTriLrt);
            text[n] = big0;
            colour[n++] = 0xFFFFFF80;
            text[n] = big1;
            colour[n++] = 0xFFFFFF80;
        }

        /* Always last, and only when something is forced: a screenshot must
         * state which hacks produced it. */
        if (sHackLineLen > 4) {
            text[n] = sHackLine;
            colour[n++] = 0xFF00FFFF;
        }

        PspSceneMenu_FillPanel(HUD_X - 4, HUD_Y - 3, HUD_X + 4 + 54 * GLYPH_W,
                               HUD_Y + 3 + n * (GLYPH_H + 2), 0xB0000000);

        verts = sceGuGetMemory(sizeof(MenuVertex) * 2 * n * (MENU_SCR_W / GLYPH_W));
        v = verts;

        for (k = 0; k < n; k++) {
            PspSceneMenu_PutString(&v, HUD_X, HUD_Y + k * (GLYPH_H + 2), colour[k], text[k]);
        }
    }

    PspSceneMenu_SubmitText(verts, v);
}
