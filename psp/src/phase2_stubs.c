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
 * Both are real subsystems that simply are not in the build yet, so the grass
 * renders and can be cut but produces neither shards nor a drop. Promote when
 * the effect system and En_Item00 come into scope; these are named here rather
 * than in phase2_stubs_gen.c because that file is regenerated.
 *
 * No prototypes on purpose, matching the convention of the generated file: the
 * linker matches by name, and declaring the real signatures would drag in the
 * headers these stubs exist to avoid. */
void EffectSsKakera_Spawn(void) {
}

/* Real def: src/overlays/actors/ovl_En_Item00/z_en_item00.c. Returns nothing
 * useful; En_Kusa ignores the return value. */
void Item_DropCollectibleRandom(void) {
}
