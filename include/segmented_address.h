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
/* The "segment slot is populated" test above is NOT sufficient on its own,
 * and this is the same collision gfx_pc.c's seg_addr() was fixed for -- that
 * fix was applied to the display-list interpreter only, leaving this C-side
 * twin with the weaker test.
 *
 * PSP user RAM is 0x08800000..0x09FFFFFF, so EVERY native pointer on this
 * platform carries 0x08 or 0x09 in its top byte -- exactly the bit pattern of
 * a segment-8 / segment-9 reference, and OoT genuinely uses both (Player's
 * eye and mouth textures). On real N64 there is no ambiguity: a native
 * pointer is KSEG0 (0x80......), whose segment nibble reads as 0, and
 * gSegments[0] is 0, so it falls through untouched.
 *
 * Why this bites the C side specifically, and why it looks like a
 * scene-transition bug: gSegments[] is SHARED between this macro and the
 * interpreter, and gfx_sp_moveword() writes gSegments[8]/[9] when it executes
 * Player's gSPSegment(8/9, ...) commands. Play_Draw zeroes them once per
 * frame, but only at draw time -- so after any frame in which Player drew,
 * slots 8 and 9 stay populated. On the FIRST Play_Init they are still zero
 * (Player has never drawn), so every SEGMENTED_TO_VIRTUAL resolves fine; on a
 * SECOND Play_Init they are stale-populated, and any already-native pointer
 * beginning 0x08/0x09 that passes through here is rewritten into garbage.
 * That asymmetry is exactly the observed "first scene loads, changing scenes
 * hangs" behaviour.
 *
 * Discriminator is offset magnitude, identical to seg_addr()'s and validated
 * by the same live measurements: a genuine segment-8/9 reference is a small
 * offset into one small texture (measured at 0..26KB), while a native pointer
 * carries its RAM offset in the low 24 bits (measured >= 1.6MB). Only
 * segments 8 and 9 can collide -- every other segment's reference range lies
 * outside the RAM window entirely, so they keep the plain behaviour. */
#define PSP_SEG89_NATIVE_MIN    0x00100000u /* >= 1MB: certainly a pointer  */
#define PSP_SEG89_AMBIGUOUS_MIN 0x00010000u /* 64KB..1MB: neither is proven */

/* Measurement hooks, same rationale as the interpreter's counters: plain
 * globals, no I/O, read out of the running game with PPSSPP's debugger. A
 * non-zero ambiguous count means the threshold needs revisiting rather than
 * trusting. */
extern unsigned int gPspSegVirtNative8;
extern unsigned int gPspSegVirtNative9;
extern unsigned int gPspSegVirtAmbiguous8;
extern unsigned int gPspSegVirtAmbiguous9;
extern unsigned int gPspSegVirtAmbiguousLast;

static inline void* PspSegmentedToVirtualDefensive(uintptr_t addr) {
    unsigned int segNum = SEGMENT_NUMBER(addr);
    uintptr_t segBase = gSegments[segNum];

    if (segBase == 0) {
        return (void*)addr;
    }

    if (segNum == 8 || segNum == 9) {
        unsigned int off = SEGMENT_OFFSET(addr);

        if (off >= PSP_SEG89_NATIVE_MIN) {
            if (segNum == 8) {
                ++gPspSegVirtNative8;
            } else {
                ++gPspSegVirtNative9;
            }
            return (void*)addr;
        }
        if (off >= PSP_SEG89_AMBIGUOUS_MIN) {
            /* Neither reading is proven here. Resolve as segmented (the
             * conservative choice, matching real hardware) but record it. */
            if (segNum == 8) {
                ++gPspSegVirtAmbiguous8;
            } else {
                ++gPspSegVirtAmbiguous9;
            }
            gPspSegVirtAmbiguousLast = (unsigned int)addr;
        }
    }

    return (void*)(segBase + SEGMENT_OFFSET(addr) + K0BASE);
}
#define SEGMENTED_TO_VIRTUAL(addr) PspSegmentedToVirtualDefensive((uintptr_t)(addr))
#else
#define SEGMENTED_TO_VIRTUAL(addr) (void*)(gSegments[SEGMENT_NUMBER(addr)] + SEGMENT_OFFSET(addr) + K0BASE)
#endif

#endif
