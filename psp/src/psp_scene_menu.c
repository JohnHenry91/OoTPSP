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
#include "psp_audio.h"

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

/* Implemented in psp/src/gfx/gfx_scegu.c -- the font atlas is bound behind the
 * texture manager's back, same as the pre-rendered background blit does. */
extern void gfx_scegu_invalidate_texture_binding(void);

typedef struct {
    const char* name;
    s16 entrance;
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

/* Held-direction auto-repeat: 110 entries is far too many to step through one
 * press at a time. */
static s32 sRepeatTimer = 0;
static s32 sFontReady = 0;
/* On by default: this build exists to measure the frame budget, and an
 * overlay nobody knows to press TRIANGLE for measures nothing. */
static s32 sHudOpen = 1;

static void PspSceneMenu_Move(s32 delta) {
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
    if (sHudOpen && PSP_RAW_PRESSED(PSP_CTRL_SQUARE)) {
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
        PspSceneMenu_Move(-PAGE_STEP);
    } else if (PSP_RAW_PRESSED(PSP_CTRL_RIGHT)) {
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
        sPendingEntrance = sEntries[sCursor].entrance;
        gPspSceneMenuOpen = 0;
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

    PspSceneMenu_PutString(&v, 8, 6, 0xFF80FFFF, "SCENE WARP  UP/DN MOVE  L/R +-10  X LOAD  O CLOSE");

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
        sprintf(line1, "HEAL %u  RESETS %u", healCount, resetStartCount);
    }

    /* Cullable rooms only. A ROOM_SHAPE_TYPE_NORMAL room draws every entry
     * unconditionally, so the counters would be stale leftovers from the last
     * cullable room and worse than no line at all. */
    /* SHAPE is the room shape type actually in force (0 normal, 1 image,
     * 2 cullable). It disambiguates a 0/0 cull line: shape 2 with 0 entries is
     * a data problem, any other shape means there is nothing to cull and the
     * zeroes are expected. */
    /* Temporarily repurposed from the room-cull line (SHAPE/CULL) to trace
     * the fanfare command's path: fanfareTimer is sFanfareStartTimer
     * (nonzero and stuck means Audio_UpdateFanfare itself isn't being
     * reached -- Audio_Update gate or Play_Update cadence -- 0 with no
     * effect means it already ran and fired SEQCMD_PLAY_SEQUENCE); seqId is
     * the pending sFanfareSeqId; pend is how many queued SEQCMDs
     * Audio_ProcessSeqCmds hasn't drained yet (gSeqCmdWritePos-ReadPos). */
    {
        extern void PspAudioDebug_FanfareQueueInfo(int* fanfareTimer, int* fanfareSeqId, int* seqCmdPending);
        int fanTimer, fanSeqId, pend;

        PspAudioDebug_FanfareQueueInfo(&fanTimer, &fanSeqId, &pend);
        {
            extern void PspAudioDebug_SeqCmdRaw(int* writePos, int* readPos);
            int wpos, rpos;

            PspAudioDebug_SeqCmdRaw(&wpos, &rpos);
            sprintf(line2, "FANQ t %d id %d pend %d  W %d R %d", fanTimer, fanSeqId, pend, wpos, rpos);
        }
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

    /* SEQ_PLAYER_FANFARE (1) is what R+TRIANGLE's Audio_PlayFanfare targets.
     * en/st/id show whether the command ever reached AudioThread at all;
     * fadeVol (x1000) whether an enabled player's volume envelope is
     * actually nonzero; notes is the engine-wide active-note count -- 0
     * notes with the player enabled means the sequence data itself (or its
     * font) isn't producing playable notes. */
    {
        extern int PspAudioDebug_ActiveNoteCount(void);
        extern void PspAudioDebug_PlayerInfo(int playerIdx, int* enabled, int* seqId, int* state,
                                              int* fadeVolumeX1000);
        int en, seqId, state, fadeX1000;

        PspAudioDebug_PlayerInfo(1, &en, &seqId, &state, &fadeX1000);
        sprintf(line4, "FANFARE en %d id %d st %d fadeVol %d notes %d", en, seqId, state, fadeX1000,
                PspAudioDebug_ActiveNoteCount());
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
        extern unsigned int PspAudioMixer_StatCommands(void);
        int perm, sq, fn, b0;

        PspAudioDebug_LoadInfo(&perm, &sq, &fn, &b0);
        sprintf(line6, "LOAD perm %d sq %d fn %d b0 %d blob %u/%u acmd %u", perm, sq, fn, b0,
                gPspBlobHits, gPspBlobMisses, PspAudioMixer_StatCommands());
    }

    PspSceneMenu_FillPanel(HUD_X - 4, HUD_Y - 3, HUD_X + 4 + 54 * GLYPH_W, HUD_Y + 3 + 7 * (GLYPH_H + 2),
                           0xB0000000);

    verts = sceGuGetMemory(sizeof(MenuVertex) * 2 * 7 * (MENU_SCR_W / GLYPH_W));
    v = verts;

    /* Red once work no longer fits the interval: the pacer is no longer the
     * thing setting the framerate, the renderer is. */
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y, headroom10 < 0 ? 0xFF4040FF : 0xFF80FFFF, line0);
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y + GLYPH_H + 2, 0xFFFFFFFF, line1);

    /* Yellow whenever anything was culled away -- that is the state worth
     * noticing, and it is invisible in the picture by definition. */
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y + 2 * (GLYPH_H + 2),
                           (gPspRoomCullRejNear + gPspRoomCullRejFar) != 0 ? 0xFF00FFFF : 0xFFFFFFFF, line2);

    /* Red until at least one real, non-silent buffer has actually gone out --
     * that is the one line this HUD exists for right now. */
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y + 3 * (GLYPH_H + 2),
                           PspAudio_StatLastPeakSample() != 0 ? 0xFF80FF80 : 0xFF4040FF, line3);
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y + 4 * (GLYPH_H + 2), 0xFFFFFFFF, line4);
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y + 5 * (GLYPH_H + 2), gateVal != 0 ? 0xFF4040FF : 0xFF80FF80, line5);
    PspSceneMenu_PutString(&v, HUD_X, HUD_Y + 6 * (GLYPH_H + 2), 0xFFFFFFFF, line6);

    PspSceneMenu_SubmitText(verts, v);
}
