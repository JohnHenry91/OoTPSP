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
    /* Test room: SCENE_LINKS_HOUSE (link_home), a ROOM_SHAPE_TYPE_IMAGE room --
     * i.e. one of the "JPEG rooms" whose pre-rendered background the N64 draws
     * with the S2DEX microcode.
     *
     * That path used to be disabled on this port (S2DEX opcode numbers collide
     * with F3DEX2's), which is why the test scene was SCENE_GRAVE_WITH_FAIRYS_
     * FOUNTAIN (hakaana2) instead. It is implemented now: the background is
     * decoded to the GE's pixel format at build time (psp/tools/jfif_to_psp.py)
     * and blitted by one port-private command (see psp/include/gfx/psp_bg_rect.h).
     *
     * Still true, and still what limits the choice of test scene: only
     * **single-room** scenes render (confirmed 5/5 on 2026-08-14 -- hakaana,
     * hakaana2 draw; takaraya, tokinoma, spot04 come out black or hang).
     * link_home has exactly one room. Check
     * `ls extracted/pal-1.0/baserom/ | grep '^<scene>_room_'` before picking
     * another. The scene must also be listed in Makefile.psp's BLOB_SCENES.
     *
     * Its actors all fall back to the shared no-op dummy ActorProfile (see
     * psp/src/z_actor_dlftbls_psp.c) -- only Player has a real profile.
     *
     * Retarget by changing PSP_BOOT_ENTRANCE alone -- and note that SELECT now
     * opens a warp menu (psp/src/psp_scene_menu.c) that reaches every scene at
     * runtime, so this only decides where a fresh launch lands. */
#ifndef PSP_BOOT_ENTRANCE
#define PSP_BOOT_ENTRANCE ENTR_LINKS_HOUSE_0
#endif
    gSaveContext.save.entranceIndex = PSP_BOOT_ENTRANCE;
    /* No intro cutscene: link_home's alternate headers include the "Link asleep
     * in bed" opening (gLinkHouseIntroSleepCs), and the alternate-header path is
     * exactly what the gameMode fix above exists to stay out of. */
    gSaveContext.save.cutsceneIndex = CS_INDEX_NONE;
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
