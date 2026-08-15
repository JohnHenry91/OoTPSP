#ifndef PSP_FRAME_PACE_H
#define PSP_FRAME_PACE_H

/* Hold the frame for `updateRate` N64 video fields before letting the game
 * loop run again -- the pacing Sched + VI provide on real hardware. Pass
 * R_UPDATE_RATE, the same value graph.c puts in cfb->updateRate for the N64
 * scheduler. See psp/src/psp_frame_pace.c. */
void PspFramePace_Wait(int updateRate);

#endif
