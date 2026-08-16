/**
 * Test-scene cycler (development aid, PSP only).
 *
 * The pre-rendered-background rooms ("JPEG rooms") are 28 separate scenes and
 * every one of them is its own asset: a different image, a different room
 * shape, and for five of them several backgrounds selected by camera angle
 * (ROOM_SHAPE_IMAGE_AMOUNT_MULTI). Rebuilding and relaunching to look at each
 * one costs minutes; stepping through them with a button costs seconds, and it
 * exercises scene transitions at the same time -- which is itself a path this
 * port has had trouble with (see the session notes on PspRom_Read hangs).
 *
 * Every scene listed here must also be in Makefile.psp's BLOB_SCENES, or the
 * loader has nothing to read and the transition lands in an empty scene.
 *
 * Controls: hold L (PSP left trigger), then PSP D-Pad Right / Left to step
 * forward / back. Those reach the game as the N64 C-Right / C-Left buttons --
 * see the note at the button test below; holding L keeps them out of the way of
 * what C-Left/C-Right normally do, and D-Pad Up (C-Up) stays free because it is
 * the game's own viewpoint toggle, which these rooms need.
 */
#include "play_state.h"

#include "controller.h"
#include "scene.h"
#include "save.h"

/* All 28 ROOM_SHAPE_TYPE_IMAGE scenes in the game, grouped the way the asset
 * tree groups them. All are single-room, which matters: multi-room scenes still
 * do not render on this port. The five marked MULTI carry more than one
 * background image and pick between them by camera angle -- those are the ones
 * that exercise Room_DrawImageMulti. */
static const s16 sPspTestEntrances[] = {
    /* indoors */
    ENTR_LINKS_HOUSE_0,
    ENTR_KNOW_IT_ALL_BROS_HOUSE_0,
    ENTR_TWINS_HOUSE_0,
    ENTR_MIDOS_HOUSE_0,
    ENTR_SARIAS_HOUSE_0,
    ENTR_KAKARIKO_CENTER_GUEST_HOUSE_0,
    ENTR_BACK_ALLEY_HOUSE_0,
    ENTR_IMPAS_HOUSE_0,
    ENTR_DOG_LADY_HOUSE_0,
    ENTR_GRAVEKEEPERS_HUT_0,
    ENTR_STABLE_0,
    ENTR_CARPENTERS_TENT_0,
    /* shops */
    ENTR_KOKIRI_SHOP_0,
    ENTR_BAZAAR_0,
    ENTR_POTION_SHOP_MARKET_0,
    ENTR_POTION_SHOP_KAKARIKO_0,
    ENTR_BOMBCHU_SHOP_0,
    ENTR_HAPPY_MASK_SHOP_0,
    ENTR_GORON_SHOP_0,
    ENTR_ZORA_SHOP_0,
    /* market / Temple of Time exterior */
    ENTR_MARKET_ENTRANCE_DAY_0,
    ENTR_MARKET_ENTRANCE_NIGHT_0_1,
    ENTR_MARKET_ENTRANCE_RUINS_0_2,
    ENTR_BACK_ALLEY_DAY_3,        /* MULTI: 3 backgrounds */
    ENTR_BACK_ALLEY_NIGHT_3_1,    /* MULTI: 3 backgrounds */
    ENTR_TEMPLE_OF_TIME_EXTERIOR_DAY_0,     /* MULTI: 2 backgrounds */
    ENTR_TEMPLE_OF_TIME_EXTERIOR_NIGHT_0_1, /* MULTI: 2 backgrounds */
    ENTR_TEMPLE_OF_TIME_EXTERIOR_RUINS_0_2, /* MULTI: 2 backgrounds */
};

#define PSP_TEST_ENTRANCE_COUNT ((s32)(sizeof(sPspTestEntrances) / sizeof(sPspTestEntrances[0])))

/* Read from the debugger to see which scene is on screen without having to
 * recognise it. */
s32 gPspTestSceneIndex = 0;

void PspTestSceneCycle(PlayState* play) {
    Input* input = &play->state.input[0];
    s32 dir;

    if (!CHECK_BTN_ALL(input->cur.button, BTN_L)) {
        return;
    }

    /* BTN_CRIGHT/BTN_CLEFT, not BTN_DRIGHT/BTN_DLEFT: this port maps the PSP's
     * physical D-Pad to the N64 C-buttons (psp/src/libultra/os_cont.c), and
     * nothing is mapped to the N64 D-Pad at all -- so the D-Pad constants can
     * never be pressed and this cycler silently did nothing. */
    if (CHECK_BTN_ALL(input->press.button, BTN_CRIGHT)) {
        dir = 1;
    } else if (CHECK_BTN_ALL(input->press.button, BTN_CLEFT)) {
        dir = -1;
    } else {
        return;
    }

    /* A transition already in flight owns nextEntranceIndex; starting a second
     * one on top of it would load two scenes over each other. */
    if (play->transitionTrigger != TRANS_TRIGGER_OFF) {
        return;
    }

    gPspTestSceneIndex = (gPspTestSceneIndex + dir + PSP_TEST_ENTRANCE_COUNT) % PSP_TEST_ENTRANCE_COUNT;

    play->nextEntranceIndex = sPspTestEntrances[gPspTestSceneIndex];
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
}
