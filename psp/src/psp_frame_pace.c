/* Game-loop pacing -- the job Sched + the VI retrace do on real hardware.
 *
 * OoT does not run its logic once per displayed field. Graph_Update builds one
 * frame and hands it to Sched with `cfb->updateRate = R_UPDATE_RATE` (3, set in
 * game.c), and the scheduler then holds that framebuffer on screen for that
 * many retraces before letting the next task start. So the whole engine --
 * animation, actor updates, camera -- advances at VI/3: 20 Hz on NTSC, 16.67 Hz
 * on PAL, which is what this build targets (Makefile.psp sets
 * OOT_VERSION=PAL_1_0).
 *
 * This port bypasses Sched entirely (see the TARGET_PSP block in graph.c), so
 * nothing consumed updateRate and the loop ran the game once per rendered
 * frame. Measured live at 30.1 Hz, i.e. 1.81x too fast -- which is exactly what
 * "the walk animation is much too fast" looks like.
 *
 * Note the measurement is also why this paces on TIME rather than on counted
 * vblanks: the port renders at 30 Hz, not 60, so "wait 3 vblanks" would have
 * been both wrong (20 Hz) and fragile (a frame that overruns would stack extra
 * waits on top of the overrun). A deadline clock degrades correctly instead --
 * if a frame takes longer than its budget it simply does not wait.
 */

#include <pspkernel.h>
#include <pspthreadman.h>

#include "psp_frame_pace.h"

/* Field rate of the N64 video mode this build targets. 50 for PAL, 60 for
 * NTSC -- set this to 60 if you would rather have the NTSC game's 20 Hz feel
 * than PAL's authentic 16.67 Hz. */
#define PSP_N64_VI_HZ 50

/* How far behind the deadline we tolerate before giving up on catching up.
 * Without this, any long stall (a scene load, a blob read off the memory
 * stick) is followed by a burst of frames that skip their delay entirely to
 * "make up" the lost time -- which reads on screen as fast-forward, the very
 * artefact this file exists to remove. */
#define PSP_PACE_RESYNC_PERIODS 4

static u64 sNextDueUsec;

void PspFramePace_Wait(int updateRate) {
    u64 now = (u64)sceKernelGetSystemTimeWide();
    u64 period;

    if (updateRate < 1) {
        updateRate = 1; /* transitions set R_UPDATE_RATE = 1 (z_play.c) */
    }
    period = (u64)((unsigned int)updateRate) * 1000000u / PSP_N64_VI_HZ;

    if (sNextDueUsec == 0 || now > sNextDueUsec + PSP_PACE_RESYNC_PERIODS * period) {
        sNextDueUsec = now + period;
        return;
    }

    if (now < sNextDueUsec) {
        sceKernelDelayThread((SceUInt)(sNextDueUsec - now));
    }
    sNextDueUsec += period;
}
