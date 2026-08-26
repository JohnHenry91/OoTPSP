#include "segment_symbols.h"
#include "effect.h"

#if TARGET_PSP

/* The table has to keep its real SHAPE on this port, not just its name.
 *
 * It used to be a 64-byte `char gEffectSsOverlayTable[64]` in
 * psp/src/phase2_stubs_gen.c, while every caller in z_effect_soft_sprite.c
 * still saw the header's `EffectSsOverlay[EFFECT_SS_TYPE_MAX]` -- 37 entries
 * of 0x1C bytes, 1036 bytes. So EffectSs_InitInfo and EffectSs_ClearAll each
 * walked 972 bytes PAST the object, reading neighbouring globals as if they
 * were overlay records and writing NULL over them; ClearAll additionally
 * handed whatever it read in the loadedRamAddr slot straight to
 * ZELDA_ARENA_FREE, i.e. __osFree walked the arena from a wild node header.
 *
 * That is what killed the console on every scene transition: the Kokiri
 * Forest hardware run played 900 frames and then froze inside ClearAll and
 * powered off (trace oot_boot_pd.log, last probe "pd-effects"). PPSSPP
 * survived it because neither the stray free nor the trampled neighbours
 * necessarily fault there.
 *
 * Shape taken from reference/oot-psp-z2442, which does the same thing for the
 * same reason: on PSP the effect code is linked in rather than DMA'd, so
 * vromStart/vramStart stay unset and only `profile` matters. Its entries point
 * at real profiles because it has them all; ours are all NULL until an effect
 * is actually promoted into the build, which EffectSs_Spawn's TARGET_PSP guard
 * treats as "effect not supported yet" instead of dereferencing NULL.
 *
 * To promote one effect: build its ovl_Effect_Ss_* source, and change its line
 * here from DEFINE_EFFECT_SS_UNSET-shaped to `&name##_Profile`. */
#define DEFINE_EFFECT_SS(name, _1)                 \
    {                                              \
        ROM_FILE_UNSET, NULL, NULL, NULL, NULL, 0, \
    },
#define DEFINE_EFFECT_SS_UNSET(_0)                 \
    {                                              \
        ROM_FILE_UNSET, NULL, NULL, NULL, NULL, 0, \
    },

EffectSsOverlay gEffectSsOverlayTable[] = {
#include "tables/effect_ss_table.h"
};

#undef DEFINE_EFFECT_SS
#undef DEFINE_EFFECT_SS_UNSET

#else

// Linker symbol declarations (used in the table below)
#define DEFINE_EFFECT_SS(name, _1) DECLARE_OVERLAY_SEGMENT(name)
#define DEFINE_EFFECT_SS_UNSET(_0)

#include "tables/effect_ss_table.h"

#undef DEFINE_EFFECT_SS
#undef DEFINE_EFFECT_SS_UNSET

// Profile declarations (also used in the table below)
#define DEFINE_EFFECT_SS(name, _1) extern EffectSsProfile name##_Profile;
#define DEFINE_EFFECT_SS_UNSET(_0)

#include "tables/effect_ss_table.h"

#undef DEFINE_EFFECT_SS
#undef DEFINE_EFFECT_SS_UNSET

// Effect SS Overlay Table definition
#define DEFINE_EFFECT_SS(name, _1)                                                                          \
    {                                                                                                       \
        ROM_FILE(ovl_##name), _ovl_##name##SegmentStart, _ovl_##name##SegmentEnd, NULL, &name##_Profile, 1, \
    },

#define DEFINE_EFFECT_SS_UNSET(_0)                 \
    {                                              \
        ROM_FILE_UNSET, NULL, NULL, NULL, NULL, 0, \
    },

EffectSsOverlay gEffectSsOverlayTable[] = {
#include "tables/effect_ss_table.h"
};

#undef DEFINE_EFFECT_SS
#undef DEFINE_EFFECT_SS_UNSET

#endif
