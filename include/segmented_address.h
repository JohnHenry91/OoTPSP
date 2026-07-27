#ifndef SEGMENTED_ADDRESS_H
#define SEGMENTED_ADDRESS_H

#include "ultra64.h"
#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

#if TARGET_PSP
/* Most callers pass genuine N64 segmented addresses (segment number + offset
 * into whatever gSegments[N] currently points at), which resolve correctly
 * here exactly as on real hardware. But some pointers reaching this macro
 * (e.g. gPlayerSkelHeaders[] in z_player_lib.c, real native pointers to
 * statically-linked objects like gLinkChildSkel/gLinkAdultSkel -- these
 * never needed DMA/relocation in this port, see project memory on static
 * asset linking) are ALREADY real, resolved PSP pointers, not segmented
 * values at all. Blindly re-resolving one of those misreads its own upper
 * byte as a segment number -- if that number happens to collide with some
 * OTHER, currently-populated gSegments[] slot, the result is a garbage
 * pointer into unrelated memory (root-caused via a real symptom: Player's
 * FlexSkeletonHeader resolving to garbage, its 1-byte limbCount field
 * reading ~160 instead of ~21, causing a large out-of-bounds animation-
 * frame DMA that corrupted unrelated Player struct fields hundreds of bytes
 * away). Mirrors gfx_pc.c's seg_addr() heuristic: only resolve via
 * gSegments[] if that segment slot is actually populated right now;
 * otherwise the address must already be a real pointer, so return it
 * unchanged. Every currently-working real segmented resolution already
 * relies on its target segment being populated (that's why it resolves
 * correctly today), so this fallback only changes behavior for the
 * previously-broken "already a real pointer" case. */
static inline void* PspSegmentedToVirtualDefensive(uintptr_t addr) {
    uintptr_t segBase = gSegments[SEGMENT_NUMBER(addr)];
    if (segBase == 0) {
        return (void*)addr;
    }
    return (void*)(segBase + SEGMENT_OFFSET(addr) + K0BASE);
}
#define SEGMENTED_TO_VIRTUAL(addr) PspSegmentedToVirtualDefensive((uintptr_t)(addr))
#else
#define SEGMENTED_TO_VIRTUAL(addr) (void*)(gSegments[SEGMENT_NUMBER(addr)] + SEGMENT_OFFSET(addr) + K0BASE)
#endif

#endif
