#ifndef PSP_FRAME_PACE_H
#define PSP_FRAME_PACE_H

#include "ultra64.h"

/* Hold the frame for `updateRate` video fields before letting the game loop run
 * again -- the pacing Sched + the VI provide on real hardware. Pass
 * R_UPDATE_RATE, the same value graph.c puts in cfb->updateRate for the N64
 * scheduler. One "field" here is one PSP vblank. See psp/src/psp_frame_pace.c. */
void PspFramePace_Wait(int updateRate);

/* Development override for R_UPDATE_RATE, applied in Graph_Update just before
 * the game state updates (src/code/graph.c). 0 = leave the engine's own value
 * alone; 1/2/3 = force ~60 / ~30 / ~20 Hz.
 *
 * This is a real engine knob, not a pacer hack: z_skelanime, Actor_UpdatePos,
 * Math_ScaledStepToS and the camera all scale their per-update deltas by
 * R_UPDATE_RATE, so the game runs at the same SPEED whichever value is in
 * force -- it just samples that motion more or less often. (The engine uses
 * this itself: the pause menu runs at 2, transitions at 1.) What does NOT
 * scale is per-update counting -- actor timers, unscaled Math_StepToF, cutscene
 * frame counters -- so those run 1.5x/3x fast at 2/1. That is the same
 * trade-off the N64 60fps codes make. */
extern s32 gPspPaceOverride;

/* Rolling averages over the last PACE_SAMPLES frames, published for the debug
 * HUD (psp/src/psp_scene_menu.c). Zero until the first window completes. */
typedef struct {
    u32 work_usec;   /* game logic + display-list interpretation + GE submit */
    u32 frame_usec;  /* wall time between consecutive frames, i.e. the real rate */
    s32 update_rate; /* the updateRate the last window was paced at          */
} PspFramePaceStats;

extern PspFramePaceStats gPspFramePace;

/* Microseconds the last frame spent parked in gfx_scegu_end_frame's
 * sceDisplayWaitVblankStart. Written there, subtracted from work_usec here.
 *
 * Without this, WORK is quantised to whole refreshes: a frame doing 4 ms of
 * real work waits ~12 ms for the vblank and reports 16.7 ms, and so does a
 * frame doing 15 ms. The first measurement on the device read exactly 16.7 ms,
 * i.e. fully saturated and unable to say anything about how much room is left
 * -- which is the one question the HUD exists to answer. */
extern u32 gPspVblankWaitUsec;

#endif
