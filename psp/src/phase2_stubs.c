/* Phase 2 stubs: cosmetic / out-of-scope subsystems for the first
 * Play_Init milestone (HUD, screen transitions, pause menu, messages,
 * cutscene/demo, game-over, debug, positional sfx). Filled in iteratively
 * as the linker reports what is actually referenced. */
#include "ultra64.h"
#include "play_state.h"

/* Real def: src/code/z_demo.c (the cutscene system, not ported). Referenced by
 * the real src/code/z_kankyo.c, which sets it in Environment_Init. Nothing
 * reads it while z_demo.c is out of the build -- the readers are all in
 * z_demo.c and db_camera.c -- so a plain definition with the same type is the
 * whole requirement here. Defining it (rather than --defsym'ing it to an
 * address) keeps the type visible to the compiler: a u8 write through a wrongly
 * sized symbol would smash the three bytes after it. */
u8 gUseCutsceneCam;

/* --- Phase 3: pulled in by the first world actor (En_Kusa) ----------------
 * This used to be a pair: EffectSsKakera_Spawn beside it, so cut grass produced
 * neither shards nor a drop. The shard half is gone -- the real
 * src/code/z_effect_soft_sprite_old_init.c is compiled in now (the Bg_* scenery
 * actors needed it), so the shards are real. Only the drop is still missing.
 *
 * Named here rather than in phase2_stubs_gen.c because that file is
 * regenerated. No prototype on purpose, matching the convention of the
 * generated file: the linker matches by name, and declaring the real signature
 * would drag in the header this stub exists to avoid. */

/* Item_DropCollectibleRandom PROMOTED: src/overlays/actors/ovl_En_Item00/
 * z_en_item00.c is in the build now (Phase 3, all actors). */

/* --- Phase 3: pulled in by Door_Shutter -----------------------------------
 * Real def: src/code/z_onepointdemo.c (the one-point cutscene camera, not in
 * the build). Door_Shutter calls it when a locked door unbars and ignores what
 * comes back, but the signature is written out in full and CAM_ID_NONE is
 * returned anyway: this port has already been bitten once by a `void` stub
 * whose caller evaluated the return value (the bridge glitch, hardware
 * session 3). A stub that returns the "no camera" sentinel is correct for
 * every caller, not just the ones checked today.
 *
 * CAM_ID_NONE is 0 (include/camera.h); spelled as a literal here to keep this
 * file free of the camera headers, matching the convention above. */
s32 OnePointCutscene_Attention(struct PlayState* play, struct Actor* actor) {
    (void)play;
    (void)actor;
    return 0; /* CAM_ID_NONE */
}

/* Real defs: src/code/z_demo.c (not in the build). Bg_Toki_Swd and the Bg_Haka
 * family write them when they hand a cutscene to csCtx. Defined with their real
 * u16 type rather than as a char[] placeholder for the reason given at the top
 * of this file: a write through a wrongly sized symbol takes the storage next
 * to it with it. */
u16 gCamAtSplinePointsAppliedFrame;
u16 gCamEyePointAppliedFrame;
u16 gCamAtPointAppliedFrame;

/* Cutscene scripts that live in scene data.
 *
 * The scenes themselves are native blobs loaded at runtime, so these symbols
 * have no compiled-in definition, yet the actors that trigger them
 * (Bg_Treemouth, Bg_Toki_Swd) reference them directly and assign them to
 * play->csCtx.script. The cutscene interpreter is stubbed out (Cutscene_Update
 * in phase2_stubs_gen.c), so nothing walks them today -- but "nothing reads it
 * today" is exactly the assumption that produced the zeroed display lists of
 * session 9, which were non-terminating and ran off the end of themselves.
 *
 * So each one is a VALID, EMPTY script rather than zeros: CS_HEADER(0 entries,
 * 1 frame) followed by CS_END_OF_SCRIPT. An interpreter that ever does reach
 * one stops at the first command instead of walking into whatever follows.
 * Spelled as literals to keep the cutscene headers out of this file:
 * CS_CMD_END_OF_SCRIPT is -1 (include/cutscene.h). */
/* ALL SEVEN PROMOTED (Phase 3, all actors). These scripts live in the actors'
 * own *_cutscene_data.c files -- ovl_Bg_Treemouth for the four Deku Tree ones,
 * ovl_Bg_Toki_Swd for the three Master Sword ones -- and those files are in
 * the build now, so the real scripts are present and these placeholders would
 * be duplicate symbols. The empty-script reasoning above is kept because it
 * still applies to any future stub of this kind. */

/* Boss_Sst brackets its intro with these two. Real defs: src/code/z_demo.c
 * (not in the build). Both return void, so a no-op is the whole behaviour --
 * the only consequence is that the boss's cutscene camera never runs, which is
 * already true of every cutscene here. Placed in this file rather than in
 * phase2_stubs_gen.c because that one is regenerated. */
void Cutscene_StartManual(struct PlayState* play, CutsceneContext* csCtx) {
    (void)play;
    (void)csCtx;
}
void Cutscene_StopManual(struct PlayState* play, CutsceneContext* csCtx) {
    (void)play;
    (void)csCtx;
}

/* Real def: src/code/z_message.c (the message system is not in the build; the
 * generated stub file already carries Message_StartTextbox, Message_GetState
 * and friends). Bg_Mizu_Water calls it when the Water Temple's water level
 * changes while a textbox is open. Returns void, and with no message system
 * there is no textbox to close, so a no-op is the whole behaviour -- but the
 * signature is written out rather than left as `void(void)`, because this file
 * includes play_state.h and a mismatching declaration would not compile, and
 * because a `void` stub whose caller passes arguments is the shape that already
 * cost this port the bridge glitch (hardware session 3). */
void Message_CloseTextbox(struct PlayState* play) {
    (void)play;
}
