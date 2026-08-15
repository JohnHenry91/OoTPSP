/*
 * File: z_opening.c
 * Overlay: ovl_opening
 * Description: Initializes the game into the title screen
 */

#include "gfx.h"
#include "regs.h"
#include "sys_matrix.h"
#include "title_setup_state.h"
#include "game.h"
#include "play_state.h"
#include "save.h"
#include "sram.h"
#include "view.h"

void TitleSetup_SetupTitleScreen(TitleSetupState* this) {
    gSaveContext.gameMode = GAMEMODE_TITLE_SCREEN;
    this->state.running = false;
#if TARGET_PSP
    /* Phase 2 milestone target: skip the real intro cutscene (CS_INDEX_3,
     * needs cutscene-command playback + several actors not in scope yet)
     * and boot straight into a normal, controllable Link's House spawn --
     * see the Phase 2 plan's Key Decision 4. Revisit once Play_Init itself
     * is proven and cutscene playback is in scope.
     *
     * IMPORTANT: override gameMode back to GAMEMODE_NORMAL (not
     * TITLE_SCREEN, set above) -- Play_Init's own sceneLayer computation
     * branches on `gameMode != GAMEMODE_NORMAL` to decide between the
     * *cutscene* scene-layer formula (GET_CUTSCENE_LAYER(cutsceneIndex),
     * which we never set here since we're not playing a cutscene) and the
     * normal age/day-night formula. Leaving TITLE_SCREEN mode meant Play_Init
     * took the cutscene branch with a garbage cutsceneIndex, producing a
     * bogus non-zero sceneLayer -- which made Scene_CommandAlternateHeaderList
     * (scene cmd code 24) think this scene had an alternate header to chase,
     * recursing into Scene_ExecuteCommands with an unresolved/garbage
     * segmented pointer. Confirmed via file-log diagnostics in z_scene.c. */
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gSaveContext.save.linkAge = LINK_AGE_CHILD;
    Sram_InitDebugSave();
    /* TEMPORARY (2026-07-26): SCENE_LINKS_HOUSE uses a prerendered JPEG
     * background room shape (ROOM_SHAPE_TYPE_IMAGE), whose draw path
     * (Room_DrawImageSingle) is disabled on this port (see z_room.c --
     * its S2DEX opcodes collide with our F3DEX2-only interpreter). That
     * means Link's House can never show any room geometry on this port
     * yet, independent of any other fix. SCENE_REDEAD_GRAVE is a small
     * grotto using a real 3D mesh (ROOM_SHAPE_TYPE_NORMAL) instead, to
     * validate the room-mesh rendering path while image-room support
     * isn't implemented. Its other actors (EN_BOX, EN_RD, OBJ_SYOKUDAI)
     * all fall back to the shared no-op dummy ActorProfile on this port
     * (see psp/src/z_actor_dlftbls_psp.c), same as everywhere else --
     * only Player has a real profile compiled in. Switch back to
     * ENTR_LINKS_HOUSE_0 once image-room drawing is implemented. */
    /* Test room: SCENE_GRAVE_WITH_FAIRYS_FOUNTAIN (hakaana2).
     *
     * IMPORTANT (2026-08-14): only **single-room** scenes render at all on this
     * port right now. Confirmed 5/5: hakaana (1 room) and hakaana2 (1 room)
     * both draw geometry, while takaraya (7 rooms), tokinoma (2 rooms) and
     * spot04/Kokiri Forest (3 rooms) all come out black or hang. So pick a
     * 1-room scene for any rendering test until multi-room support is fixed.
     * Check `ls extracted/pal-1.0/baserom/ | grep '^<scene>_room_'` first. */
    gSaveContext.save.entranceIndex = ENTR_GRAVE_WITH_FAIRYS_FOUNTAIN_0;
    /* Force a safe midday value -- Play_Init derives IS_DAY (and thus which
     * of SCENE_LAYER_CHILD_DAY/CHILD_NIGHT it picks) from this, and an
     * unset/zero dayTime would land before the 6:30 threshold, i.e. night,
     * which -- same as the gameMode fix above -- would make sceneLayer !=
     * SCENE_LAYER_CHILD_DAY and wrongly re-trigger the alternate-header
     * recursion this milestone deliberately avoids. */
    gSaveContext.save.dayTime = CLOCK_TIME(12, 0);
    gSaveContext.sceneLayer = 0;
#else
    gSaveContext.save.linkAge = LINK_AGE_ADULT;
    Sram_InitDebugSave();
    gSaveContext.save.cutsceneIndex = CS_INDEX_3;
    // assigning scene layer here is redundant, as Play_Init sets it right away
    gSaveContext.sceneLayer = GET_CUTSCENE_LAYER(CS_INDEX_3);
#endif
    SET_NEXT_GAMESTATE(&this->state, Play_Init, PlayState);
}

void func_80803C5C(TitleSetupState* this) {
}

void TitleSetup_Main(GameState* thisx) {
    TitleSetupState* this = (TitleSetupState*)thisx;

    Gfx_SetupFrame(this->state.gfxCtx, 0, 0, 0);
    TitleSetup_SetupTitleScreen(this);
    func_80803C5C(this);
}

void TitleSetup_Destroy(GameState* thisx) {
}

void TitleSetup_Init(GameState* thisx) {
    TitleSetupState* this = (TitleSetupState*)thisx;

    R_UPDATE_RATE = 1;
    Matrix_Init(&this->state);
    View_Init(&this->view, this->state.gfxCtx);
    this->state.main = TitleSetup_Main;
    this->state.destroy = TitleSetup_Destroy;
}
