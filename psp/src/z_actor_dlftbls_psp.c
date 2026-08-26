/* TARGET_PSP replacement for src/code/z_actor_dlftbls.c (that real file is
 * excluded from the PSP build). Phase 2, Key Decision 1+2 of the plan.
 *
 * Real N64 hardware DMAs each actor's overlay code to a runtime RAM address
 * and relocates it; the real table therefore carries per-actor ROM/VRAM
 * segment addresses (_ovl_xxxSegmentStart/End linker symbols). On PSP we
 * statically link every actor we support directly into the binary -- no DMA,
 * no relocation -- so every entry here sets vramStart == NULL, which
 * Actor_AddToCategory/Actor_Spawn's overlay path (src/code/z_actor.c:3199)
 * already treats as "not an overlay, already resident: just use ->profile"
 * (the exact same NULL-vramStart fast path DEFINE_ACTOR_INTERNAL uses in
 * stock decomp for Player/En_Item00, and that gGameStateOverlayTable already
 * uses for every game state, see psp/src/gamestate_table_psp.c).
 *
 * Allowlist mechanism (Key Decision 2): we don't want to compile all ~450
 * actor .c files (each drags in its own object/asset dependency web) just to
 * satisfy the &name##_Profile references in this table. Instead every
 * name##_Profile is declared as a WEAK alias to a single shared
 * gDummyActorProfile below. Actors whose real .c file IS compiled into
 * Makefile.psp define name##_Profile as a strong symbol, which the linker
 * uses in preference to the weak alias; actors we haven't ported yet fall
 * back to the dummy (a harmless no-op actor that spawns, does nothing, and
 * never crashes). To "enable" a real actor: add its .c (and its object/asset
 * deps) to Makefile.psp -- no change needed here. */
#include "z_actor_dlftbls.h"
#include "actor.h"
#include "object.h"

/* A no-op actor used for every not-yet-ported actor id.
 *
 * instanceSize carries a margin over sizeof(Actor) on purpose: if engine code
 * touches actor-subclass fields on a dummy instance, it should write inside
 * its own allocation rather than stomp the arena. But the margin used to be
 * 0x2000 -- 8 KB, twenty-three times the 0x14C of a real Actor -- and every
 * unported actor in a scene paid it out of the Zelda arena.
 *
 * That is not free space, it is the SCENE's space. Play_Init hands the arena
 * whatever the 1.83 MB THA has left after the 1000 KB object bank and the room
 * buffer, which for a big overworld scene is only about 406 KB -- the same
 * figure the N64 works with. Hardware trace oot_boot_pd5.log: the scene loads,
 * fills the arena to 401K of 406K in its first frame, creeps to zfree=0K on
 * the second, and the console hangs on the third. 403 KB divided by 8 KB is
 * roughly fifty actors, which is an ordinary population for such a scene.
 *
 * 0x200 keeps the safety property -- it still covers DynaPolyActor (0x164),
 * the largest subclass the engine itself casts to, with room to spare -- while
 * costing one sixteenth as much. Raise it again only with a named actor that
 * needs it, not on principle. */
static void DummyActor_Noop(struct Actor* thisx, struct PlayState* play) {
}

ActorProfile gDummyActorProfile = {
    /* id            */ ACTOR_ID_MAX, // sentinel; never matches a real id
    /* category      */ ACTORCAT_MISC,
    /* flags         */ 0,
    /* objectId      */ OBJECT_GAMEPLAY_KEEP,
    /* instanceSize  */ 0x200,
    /* init          */ DummyActor_Noop,
    /* destroy       */ DummyActor_Noop,
    /* update        */ DummyActor_Noop,
    /* draw          */ DummyActor_Noop,
};

/* Weak-alias every actor profile to the dummy (overridden by the real
 * strong symbol when that actor's .c is linked). */
#define DEFINE_ACTOR(name, _1, _2, _3) \
    extern ActorProfile name##_Profile __attribute__((weak, alias("gDummyActorProfile")));
#define DEFINE_ACTOR_INTERNAL(name, _1, _2, _3) \
    extern ActorProfile name##_Profile __attribute__((weak, alias("gDummyActorProfile")));
#define DEFINE_ACTOR_UNSET(_0)

#include "tables/actor_table.h"

#undef DEFINE_ACTOR
#undef DEFINE_ACTOR_INTERNAL
#undef DEFINE_ACTOR_UNSET

/* Build the table itself: every entry vramStart == NULL (resident). */
#define DEFINE_ACTOR(name, _1, allocType, _3) \
    { ROM_FILE_UNSET, NULL, NULL, NULL, &name##_Profile, NULL, allocType, 0 },
#define DEFINE_ACTOR_INTERNAL(name, _1, allocType, _3) \
    { ROM_FILE_UNSET, NULL, NULL, NULL, &name##_Profile, NULL, allocType, 0 },
#define DEFINE_ACTOR_UNSET(_0) { 0 },

ActorOverlay gActorOverlayTable[] = {
#include "tables/actor_table.h"
};

#undef DEFINE_ACTOR
#undef DEFINE_ACTOR_INTERNAL
#undef DEFINE_ACTOR_UNSET

s32 gMaxActorId = 0;

/* Init/Cleanup are platform-agnostic in the real file (they only set
 * gMaxActorId and register a fault-handler client). Fault_AddClient/
 * RemoveClient are already stubbed (psp/src/phase1_stubs.c), so skip the
 * fault client entirely and just set the id bound. */
void ActorOverlayTable_Init(void) {
    gMaxActorId = ACTOR_ID_MAX;
}

void ActorOverlayTable_Cleanup(void) {
    gMaxActorId = 0;
}

void ActorOverlayTable_LogPrint(void) {
}
