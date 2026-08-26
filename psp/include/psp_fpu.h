/* Per-thread FPU control-register setup (FCR31).
 *
 * This exists because FCR31 is the single biggest source of faults that PPSSPP
 * cannot show you. The emulator computes floating point with host doubles and
 * never raises a MIPS FPU exception; real silicon does, and an unhandled one on
 * a user thread takes the console down instantly -- indistinguishable, from the
 * outside, from a wild pointer write.
 *
 * Two settings, for two different mechanisms:
 *
 *  - Enable = 0 clears the five exception-enable bits, so division by zero,
 *    overflow and invalid operations produce inf/NaN quietly instead of
 *    trapping. This is what Daedalus does at startup, and what
 *    reference/oot-psp-z2442 copied from it (src/port/psp/oot_psp_probe.c).
 *
 *  - FS = 1 flushes denormalized results and operands to zero. This one is NOT
 *    covered by the enable bits: a denormal raises "Unimplemented Operation",
 *    whose enable bit does not exist (see pspfpu.h -- UNIMPOP is a cause bit
 *    only), so it always traps. The audio path generates denormals as a matter
 *    of course, because envelope and volume ramps decay asymptotically toward
 *    zero, which is precisely the arithmetic that walks a float down through
 *    1e-38 on its way out.
 *
 * FCR31 is part of the per-thread context, saved and restored by the kernel, so
 * setting it on the main thread does NOT configure any thread created later.
 * It has to be applied on each thread's own entry -- hence the call in
 * OSThreadTrampoline (psp/src/libultra/os_thread.c), which every engine thread
 * (Graph, AudioMgr, PadMgr, DmaMgr, Sched, Irq) passes through.
 */
#ifndef PSP_FPU_H
#define PSP_FPU_H

#include <pspfpu.h>

static inline void PspFpu_ConfigureThread(void) {
    pspFpuSetEnable(0);
    pspFpuSetFS(1);
    pspFpuClearFlags(PSP_FPU_EXCEPTION_ALL);
    pspFpuClearCause(PSP_FPU_EXCEPTION_ALL);
}

#endif
