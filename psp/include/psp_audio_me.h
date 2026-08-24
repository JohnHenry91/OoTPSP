#ifndef PSP_AUDIO_ME_H
#define PSP_AUDIO_ME_H

/* Runs the N64 audio microcode on the PSP's Media Engine.
 *
 * The Media Engine is the PSP's second Allegrex core. It is idle in this port
 * and has no FPU -- which suits the audio microcode exactly, since
 * psp/src/audio/psp_audio_mixer.c is pure integer code. Moving the mixer
 * there buys back the largest single block of main-CPU time the audio system
 * costs, and the audio thread stops competing with the renderer for it.
 *
 * HOW IT WORKS. libme-core boots the ME into PspAudioMe_Process (our
 * meLibOnProcess). That function never returns: it spins on a state word and
 * runs a job whenever the main CPU publishes one. Both processors see each
 * other's writes only through UNCACHED memory, so every shared variable
 * lives in the .uncached section and every buffer handed across is flushed
 * or invalidated explicitly. Getting that wrong does not fail loudly -- it
 * produces stale audio or a silent hang -- which is why the ranges are
 * spelled out at each call rather than flushing the whole cache.
 *
 * FALLBACK IS NOT OPTIONAL. PPSSPP does not emulate the Media Engine, and
 * booting it needs the kernel bridge (kcall.prx, embedded in libme-core), so
 * on the emulator every one of these calls has to keep working by running the
 * list on the calling thread instead. PspAudio_RunCommandList picks the path;
 * callers never need to know which one they got. This also means the
 * emulator can no longer prove the ME path works -- that now needs hardware.
 *
 * Modelled on reference/oot-psp-z2442's oot_psp_audio_backend.c, minus its
 * ring buffer and producer threads: this port already has those in
 * psp/src/audio/audio_psp.c.
 */

#include <stdint.h>

#include "ultra64.h"

/* Compile the Media Engine path in at all. Turning this off leaves a working
 * port that mixes on the audio thread, which is the state before this file
 * existed -- useful for isolating whether a sound bug is the ME's doing. */
#ifndef PSP_AUDIO_ME_ENABLED
#define PSP_AUDIO_ME_ENABLED 1
#endif

/* Boot the ME. Call once, early in main, on the main thread -- libme-core
 * writes and loads ./kcall.prx via a relative path, which only resolves
 * there. Returns 0 on success, negative when the ME is unavailable (an
 * emulator, or a kernel that refuses the bridge); in that case everything
 * below silently uses the CPU. */
int32_t PspAudioMe_Init(void);
void PspAudioMe_Shutdown(void);

/* Execute a finished command list. Uses the ME when it is up and idle,
 * otherwise runs it on the calling thread. aiBuffer/aiFrames describe the PCM
 * the list writes, so the result can be made visible to the other processor. */
void PspAudio_RunCommandList(const Acmd* cmdList, int32_t cmdCount, int16_t* aiBuffer, int32_t aiFrames);

/* HUD/debugger counters. meJobs climbing while cpuJobs stays flat is the
 * only positive proof the ME is really doing the work. */
uint32_t PspAudioMe_StatMeJobs(void);
uint32_t PspAudioMe_StatCpuJobs(void);
uint32_t PspAudioMe_StatTimeouts(void);
uint32_t PspAudioMe_StatLastJobUsec(void);
int32_t PspAudioMe_IsActive(void);

#endif
