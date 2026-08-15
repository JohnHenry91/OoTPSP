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
