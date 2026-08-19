/* Game-loop pacing -- the job Sched + the VI retrace do on real hardware.
 *
 * OoT does not run its logic once per displayed field. Graph_Update builds one
 * frame and hands it to Sched with `cfb->updateRate = R_UPDATE_RATE` (3, set in
 * game.c), and the scheduler then holds that framebuffer on screen for that
 * many retraces before letting the next task start. So the whole engine --
 * animation, actor updates, camera -- advances at VI/3.
 *
 * This port bypasses Sched entirely (see the TARGET_PSP block in graph.c), so
 * nothing consumed updateRate and the loop ran the game once per rendered
 * frame. Measured live at 30.1 Hz, i.e. 1.81x too fast -- which is exactly what
 * "the walk animation is much too fast" looks like.
 *
 * ## Why this counts VBLANKS and not microseconds
 *
 * The first version of this file paced on a wall-clock deadline: `updateRate *
 * 1s / 50 Hz` = 60 ms, PAL's field rate, because Makefile.psp builds
 * OOT_VERSION=PAL_1_0. That produced the right average rate and a permanently
 * uneven picture, because the PSP panel refreshes at 59.94 Hz and 60 ms is
 * 3.596 of its 16.68 ms vblanks. gfx_scegu_end_frame waits for a vblank before
 * swapping, so every frame got rounded up to a whole number of refreshes and
 * the hold time alternated 3, 4, 4, 3, 4, 4 ... -- a 25% swing in frame
 * interval on every single frame, at perfectly constant load. That reads as
 * judder no matter how stable the framerate underneath it is.
 *
 * So: the PSP's own refresh IS the video field this port paces on. `updateRate`
 * fields means exactly that many vblanks, counted with sceDisplayGetVcount, and
 * the interval is uniform by construction with no drift to accumulate.
 *
 * The consequence worth naming: 59.94 Hz is NTSC's field rate, so the game now
 * runs at NTSC timing (R_UPDATE_RATE 3 -> 19.98 Hz) rather than the PAL ROM's
 * 16.67 Hz. That is 20% faster than the PAL original -- and it is the same 20%
 * the PAL release gave up, i.e. this is the speed the game was designed at, not
 * a speed-up on top of it. Restoring PAL timing would mean going back to a
 * fractional-vblank deadline and accepting the judder; there is no way to have
 * both on a 59.94 Hz panel.
 *
 * Counting vblanks rather than sleeping also degrades correctly: a frame that
 * overruns its budget simply misses its target and the next one is due
 * immediately, and a long stall (scene load, blob read off the memory stick)
 * trips the resync below instead of being "made up" with a burst of
 * zero-delay frames, which would read as fast-forward.
 */

#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "psp_frame_pace.h"

/* How far behind the target we tolerate before giving up on catching up and
 * restarting the cadence from now. In vblanks. */
#define PSP_PACE_RESYNC_FIELDS 12

/* Frames per rolling average published in gPspFramePace. */
#define PSP_PACE_SAMPLES 32

s32 gPspPaceOverride = 0;
PspFramePaceStats gPspFramePace;
u32 gPspVblankWaitUsec = 0;

/* The vblank at which the next frame may start. sceDisplayGetVcount counts
 * vblanks since boot and wraps at 2^32; every comparison below is done on the
 * signed difference so the wrap is a non-event. */
static u32 sNextDueVcount;
static s32 sHavePace;

static u64 sReleaseUsec; /* when the previous frame was let go */
static u64 sWorkAccum;
static u64 sFrameAccum;
static u32 sSamples;

void PspFramePace_Wait(int updateRate) {
    u64 enter = (u64)sceKernelGetSystemTimeWide();
    u64 release;
    u32 now;
    s32 resync;

    if (updateRate < 1) {
        updateRate = 1; /* transitions set R_UPDATE_RATE = 1 (z_play.c) */
    }

    /* Everything since the previous release is this frame's real work: game
     * logic, display-list interpretation, GE submission and the swap's own
     * vblank wait. Sampled before the wait below so the HUD can show work and
     * interval separately -- the gap between them is the headroom, which is
     * the number that decides whether a lower updateRate is affordable. */
    if (sReleaseUsec != 0) {
        u64 elapsed = enter - sReleaseUsec;

        /* The swap's vblank wait is idle time, not work -- see the comment on
         * gPspVblankWaitUsec. Clamped rather than trusted: the two are sampled
         * by different code on different clocks. */
        if (elapsed > (u64)gPspVblankWaitUsec) {
            elapsed -= (u64)gPspVblankWaitUsec;
        } else {
            elapsed = 0;
        }
        sWorkAccum += elapsed;
    }

    now = sceDisplayGetVcount();

    resync = (!sHavePace || (s32)(now - sNextDueVcount) > PSP_PACE_RESYNC_FIELDS);

    if (resync) {
        sNextDueVcount = now + (u32)updateRate;
        sHavePace = 1;
    } else {
        while ((s32)(sceDisplayGetVcount() - sNextDueVcount) < 0) {
            sceDisplayWaitVblankStart();
        }
        sNextDueVcount += (u32)updateRate;
    }

    release = (u64)sceKernelGetSystemTimeWide();

    if (resync) {
        /* Throw the averaging window away rather than closing it. A resync
         * means a multi-second stall just landed in it, and one scene load
         * would otherwise poison the HUD's work/headroom numbers for the next
         * 32 frames -- exactly the frames someone is looking at to judge
         * whether a rate change fits. */
        sWorkAccum = 0;
        sFrameAccum = 0;
        sSamples = 0;
        sReleaseUsec = release;
        return;
    }

    if (sReleaseUsec != 0) {
        sFrameAccum += release - sReleaseUsec;
    }
    sReleaseUsec = release;

    if (++sSamples >= PSP_PACE_SAMPLES) {
        gPspFramePace.work_usec = (u32)(sWorkAccum / PSP_PACE_SAMPLES);
        gPspFramePace.frame_usec = (u32)(sFrameAccum / PSP_PACE_SAMPLES);
        gPspFramePace.update_rate = updateRate;
        sWorkAccum = 0;
        sFrameAccum = 0;
        sSamples = 0;
    }
}
