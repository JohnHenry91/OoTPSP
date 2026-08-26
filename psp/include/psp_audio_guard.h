#ifndef PSP_AUDIO_GUARD_H
#define PSP_AUDIO_GUARD_H

/* Pointer sanity checks for the audio engine, ported from the reference PSP
 * port (reference/oot-psp-z2442, `#if defined(TARGET_PSP)` blocks in
 * src/audio/internal/{playback,seqplayer,load,heap,synthesis}.c).
 *
 * Why the engine needs them here and not on N64: teardown queues audio
 * commands that the audio thread executes one or two ticks LATER, by which
 * time the pointers they carry can already be freed or half-rebuilt. On N64
 * a garbage pointer still lands somewhere in RDRAM, so the note merely sounds
 * wrong; under PPSSPP guest RAM is one flat host block, equally harmless. On
 * real PSP hardware it leaves the user partition and the console loses power.
 * See psp/docs/PORTING_PITFALLS.md and the hardware bring-up notes.
 *
 * A "native" pointer is one inside the PSP user partition and 4-byte aligned.
 */

#include "ultra64.h"

/* The PSP user partition starts at 0x08800000, NOT at 0x08000000.
 *
 * 0x08000000..0x087FFFFF is kernel memory. A user-mode thread that reads it
 * takes an exception and the console loses power -- which is the exact death
 * this guard exists to prevent, so accepting that range left an 8 MB hole in
 * the middle of the safety net. Every "guarded" audio path could still walk
 * straight into it.
 *
 * The upper bound is the top of a 64 MB model's user partition (PSP-2000 and
 * later). A PSP-1000 has only 32 MB and its user RAM ends at 0x0A000000, so
 * this stays permissive by 32 MB on that model -- still infinitely better than
 * the old lower bound, and the tighter value would have to be probed at
 * runtime rather than assumed. */
#define PSP_AUDIO_RAM_START 0x08800000U
#define PSP_AUDIO_RAM_END   0x0C000000U

/* Bumped whenever a guard rejects something, so the HUD can show that the
 * engine was saved from a wild pointer rather than the bug being absent. */
extern u32 gPspAudioBadPtrDrops;

static inline s32 PspAudio_IsAlignedNativePtr(const void* ptr) {
    u32 addr = (u32)ptr;

    return (addr >= PSP_AUDIO_RAM_START) && (addr < PSP_AUDIO_RAM_END) && ((addr & 3) == 0);
}

/* Records a rejected pointer. Kept as a call (not a macro) so the counter has
 * exactly one definition and probes can breakpoint one address. */
void PspAudio_NoteBadPtr(const char* where);

#endif
