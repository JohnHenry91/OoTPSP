/* Symbols the full actor set needs that no ported subsystem defines yet.
 *
 * HOW THIS LIST WAS ARRIVED AT, and why it is this short: every one of the 426
 * actor sources was compiled, the union of their undefined symbols was taken,
 * and everything already provided by the EBOOT, by the object blobs
 * (psp/build/psp_object_syms_gen.c) or by the scene blobs
 * (psp/build/psp_scene_syms_gen.c) was subtracted. What remained was 35
 * symbols. Nine engine files covered most of them and are compiled for real in
 * Makefile.psp; this file is the rest -- functions belonging to subsystems that
 * are still stubbed wholesale (Interface, Message, OnePointCutscene, Cutscene)
 * plus three odds and ends that have no PSP equivalent at all.
 *
 * Keeping them here rather than in psp/src/phase2_stubs_gen.c is deliberate:
 * that file is generated, and these have hand-chosen return values that a
 * regeneration would silently discard.
 *
 * EVERY return value below is a decision, not a default. Where a caller acts on
 * the result, the value that makes the actor behave as if the missing feature
 * simply did not fire was chosen, and the reasoning is written next to it.
 */

#include "z_lib.h"
#include "play_state.h"
#include "actor.h"
#include "ultra64.h"

struct PlayState;
struct Actor;

/* -------------------------------------------------------------------------
 * Cutscene system (src/code/z_demo.c is not ported)
 * ------------------------------------------------------------------------- */

/* Actors call this to hand the cutscene system a script -- with no cutscene
 * system there is nothing to hand it to. Dropping the script is the behaviour
 * that matches the rest of the port: PlayState's cutscene fields stay at their
 * "no cutscene running" values, so the actor's own state machine sees the
 * cutscene never start, rather than start and never end. */
void Cutscene_SetScript(struct PlayState* play, void* script) {
}

/* -------------------------------------------------------------------------
 * Interface / HUD (src/code/z_parameter.c is not ported)
 * ------------------------------------------------------------------------- */

void Interface_SetTimer(s16 seconds) {
}

void Interface_SetSubTimer(s16 seconds) {
}

void Interface_InitHorsebackArchery(struct PlayState* play) {
}

/* Both bottle queries answer "no bottle".
 *
 * This is the conservative direction and not an arbitrary one: callers use the
 * answer to decide whether to offer a bottle interaction (catching a fish, a
 * fairy, milk). Answering "yes" would start an exchange that the unported
 * inventory code cannot complete, leaving the actor waiting. Answering "no"
 * simply means the interaction is not offered. Revisit when z_parameter.c is
 * ported -- the real function reads gSaveContext.save.info.inventory.items. */
s32 Inventory_HasEmptyBottle(void) {
    return 0;
}

s32 Inventory_HasSpecificBottle(u8 bottleItem) {
    return 0;
}

/* -------------------------------------------------------------------------
 * Message system (src/code/z_message.c is not ported)
 * ------------------------------------------------------------------------- */

void Message_StartOcarinaSunsSongDisabled(struct PlayState* play, u16 ocarinaActionId) {
}

void Message_UpdateOcarinaMemoryGame(struct PlayState* play) {
}

/* -------------------------------------------------------------------------
 * One-point cutscenes (src/code/z_onepointdemo.c is not ported)
 * ------------------------------------------------------------------------- */

/* The real function returns the index of the camera it started, or a negative
 * value when it started none. Callers store the result and later use it to stop
 * that camera again. Returning a negative value is therefore the only safe
 * answer: a fake non-negative index would be used to tear down a camera that
 * was never created. -1 is what the real code returns on its own failure path. */
s32 OnePointCutscene_AttentionSetSfx(struct PlayState* play, struct Actor* actor, s32 sfxId) {
    return -1;
}

s32 OnePointCutscene_CheckForCategory(struct PlayState* play, s32 actorCategory) {
    return -1;
}

/* -------------------------------------------------------------------------
 * Three things with no PSP equivalent
 * ------------------------------------------------------------------------- */

/* The N64 boot ROM leaves the cartridge's CIC chip id at 0x80000310, and
 * src/libultra/os/parameters.s makes osCicId an absolute symbol pointing there.
 * There is no such address here, so it has to be a real variable.
 *
 * 6105 is not a placeholder. En_Zl2 (Zelda's adult model) carries the game's
 * anti-piracy check -- `if (osCicId != 6105) Matrix_Scale(2.0f, 0.5f, 2.0f)`,
 * which deliberately misshapes her hair -- and that check is inside
 * `#if PLATFORM_N64`, which this build defines. A genuine PAL cartridge uses
 * CIC 6105, so this is the value the console reports, and any other value
 * would reproduce the anti-piracy artefact on a legitimate copy. */
s32 osCicId = 6105;

/* Written by the CIC 6105 boot path (src/boot/cic6105.c) from an IO_READ of a
 * cartridge register. That file is not in the build -- it reads N64 hardware --
 * so the variable exists here and stays 0, which is what the read yields when
 * the check is not running. */
u32 gCICBootMagic0 = 0;

/* 64DD presence check (src/n64dd/z_n64dd.c). There is no 64DD, and there never
 * will be one on a PSP; 0 is the real function's "not present" answer. */
s32 func_801C70FC(void) {
    return 0;
}
