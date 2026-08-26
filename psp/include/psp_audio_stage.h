/* Bring-up ladder for the inside of the audio thread, selected at RUNTIME.
 *
 * The whole-program ladder (PSP_BRINGUP_LEVEL, psp_hw_diag.h) narrowed the
 * hardware fault to "the audio thread" and then stopped being able to help,
 * because its finest step is "start AudioMgr or don't". This ladder continues
 * the same idea one level down, splitting AudioThread_UpdateImpl into its four
 * separable jobs.
 *
 * Runtime, not compile-time, for two reasons. Makefile.psp does not track
 * CFLAGS changes and has no clean target, so every compile-time knob has cost
 * a wasted test run at least once (see PORTING_PITFALLS). And a hardware test
 * cycle is minutes of copying to a stick: the user should be able to walk the
 * whole ladder from one build by editing a text file, not wait on four builds.
 *
 * The stage is read from `audiostage.txt` next to EBOOT.PBP -- a plain decimal
 * number, nothing else. A missing file means the full game (stage 4), so a
 * normal stick is unaffected.
 *
 * Pass criterion, same as the outer ladder: does the console come back to the
 * XMB instead of dying. Walk down from 4 until it survives; the first stage
 * that survives names the job that kills it.
 */
#ifndef PSP_AUDIO_STAGE_H
#define PSP_AUDIO_STAGE_H

/* audio thread runs, drains its queue, and does nothing else */
#define PSP_AUDIO_STAGE_IDLE 0
/* + AudioThread_ProcessCmds: sequence starts, font loads, heap resets */
#define PSP_AUDIO_STAGE_CMDS 1
/* + AudioSynth_Update: the sequence player, note allocation, ABI list build */
#define PSP_AUDIO_STAGE_SYNTH 2
/* + the software microcode, executed on THIS core */
#define PSP_AUDIO_STAGE_MIX_CPU 3
/* + the Media Engine is allowed to take the mixing job */
#define PSP_AUDIO_STAGE_MIX_ME 4

/* Default is MIX_CPU, not MIX_ME. The Media Engine is opt-in because it is
 * proven on neither platform: PPSSPP has no second core at all, and
 * meLibDefaultInit's kernel bridge faults there and can wedge the boot (0 diag
 * lines in 50 s, against 134 frames at stage 3); on hardware the path has
 * simply never been reached alive. Write 4 into audiostage.txt to turn it on
 * and watch the HUD's `ME me/cpu` counters -- me climbing with cpu still is
 * the only proof it is really mixing. */
#define PSP_AUDIO_STAGE_FULL PSP_AUDIO_STAGE_MIX_CPU

/* The highest stage a file may select, as opposed to the default. */
#define PSP_AUDIO_STAGE_MAX PSP_AUDIO_STAGE_MIX_ME

#ifdef __cplusplus
extern "C" {
#endif

/* Non-static and read straight from C: this has to be cheap enough to sit in
 * the audio thread's inner loop without reshaping the timing it measures. */
extern int gPspAudioStage;

/* Reads audiostage.txt. MUST be called on the main thread, after
 * PspBlob_SetBaseDir -- threads created later have no cwd (the NOCWD trap). */
void PspAudioStage_Init(void);

#define PSP_AUDIO_STAGE_AT_LEAST(stage) (gPspAudioStage >= (stage))

#ifdef __cplusplus
}
#endif

#endif
