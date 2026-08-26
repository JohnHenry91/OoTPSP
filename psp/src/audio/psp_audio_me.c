/* Media Engine offload for the audio microcode. See psp/include/psp_audio_me.h
 * for what this is and why the CPU fallback is mandatory.
 *
 * The whole protocol is one state word plus four job fields, all uncached and
 * all written in a fixed order:
 *
 *   CPU: fill job fields -> sync -> state = RUN -> poll for IDLE
 *   ME : see RUN -> run the list -> sync -> state = IDLE
 *
 * The sync before publishing RUN is what makes the job fields visible; the
 * one after the list is what makes the PCM visible. Neither can be dropped,
 * and neither shows up as a crash when it is -- only as stale or missing
 * sound.
 */

#include "psp_audio_me.h"

#include <string.h>

#include "psp_audio_mixer.h"

/* Shared by both builds so the getters below have exactly one definition. */
/* Result of meLibDefaultInit(), shown on the HUD. 1 means "never attempted". */
int32_t gPspAudioMeInitResult = 1;

static uint32_t sStatMeJobs;
static uint32_t sStatCpuJobs;
static uint32_t sStatTimeouts;
static uint32_t sStatLastJobUsec;

#if TARGET_PSP && PSP_AUDIO_ME_ENABLED

#include <me-core-mapper/me-core.h>
#include <pspkernel.h>
#include <pspsdk.h>

#define PSP_AUDIO_ME_STATE_BOOTING 0
#define PSP_AUDIO_ME_STATE_IDLE    1
#define PSP_AUDIO_ME_STATE_RUN     2
#define PSP_AUDIO_ME_STATE_STOP    3
#define PSP_AUDIO_ME_STATE_HALTED  4

/* One audio frame is ~16 ms of work at most; anything past this means the ME
 * is wedged, and the job is redone on the CPU rather than dropped. After a
 * timeout the ME is never used again -- a wedged core does not recover, and
 * silently retrying it would cost a stall every single frame. */
#define PSP_AUDIO_ME_TIMEOUT_USEC 40000
#define PSP_AUDIO_ME_READY_USEC   250000

/* Shared with the ME: every access must bypass both data caches. Writing these
 * through a cached mapping is the classic way to make the two processors
 * disagree forever.
 *
 * That is exactly what used to happen here. These were declared
 * `section(".uncached")`, but the PSP link script defines no such section, so
 * the attribute quietly did nothing and the words were placed in ordinary
 * cached .data (psp-nm: `004de680 d sMeState`). The intent was written down
 * and never took effect.
 *
 * The consequence was the whole handshake failing in a way that reads like
 * "the Media Engine is not supported": PspAudioMe_Init sets sMeState = IDLE to
 * release the ME from its BOOTING wait, but that write sat in this core's
 * cache, the ME kept polling its own stale copy, sMeProgress never reached 2,
 * and the 250 ms readiness wait timed out. Measured on hardware:
 * `ME 0/4820 to1 i2` -- meLibDefaultInit succeeded (i2), one timeout (to1),
 * and every job since then mixed on the main core.
 *
 * The fix is an address, not a section. 0x40000000 is the uncached mirror of
 * user RAM, the same trick gfx_scegu.c already uses for the GE list. One
 * cache-line-aligned block, accessed only through that alias, so neither core
 * can cache any of it. The macros keep the original names, so every use site
 * below is unchanged. */
static volatile uint32_t sMeShared[6 * 16] __attribute__((aligned(64)));

#define ME_SHARED(i) (((volatile uint32_t*)((uintptr_t)sMeShared | 0x40000000u))[(i) * 16])

#define sMeState    ME_SHARED(0)
#define sMeCmdList  ME_SHARED(1)
#define sMeCmdCount ME_SHARED(2)
#define sMeAiBuffer ME_SHARED(3)
#define sMeAiBytes  ME_SHARED(4)
#define sMeProgress ME_SHARED(5)

static int32_t sMeAvailable;
static int32_t sMeBootAttempted;

/* Entry point for the Media Engine. libme-core jumps here after reset and
 * never expects a return. Must not call anything that traps into the kernel:
 * no sce* calls, no allocation, no I/O. The mixer qualifies -- it is integer
 * C over memcpy/memset only, which is also why it suits a core with no FPU. */
__attribute__((noinline, aligned(4))) void meLibOnProcess(void) {
    sMeProgress = 1;
    meLibSync();

    while (sMeState == PSP_AUDIO_ME_STATE_BOOTING) {
        meLibDelayPipeline();
    }

    sMeProgress = 2;
    meLibSync();

    while (sMeState != PSP_AUDIO_ME_STATE_STOP) {
        if (sMeState == PSP_AUDIO_ME_STATE_RUN) {
            const Acmd* cmdList = (const Acmd*)(uintptr_t)sMeCmdList;
            int32_t cmdCount = (int32_t)sMeCmdCount;

            /* The CPU wrote the command list, the sample data and every
             * mixer state block through ITS cache. Drop everything this core
             * still believes about RAM before reading any of it. Coarse on
             * purpose: the inputs are scattered across the whole audio heap,
             * and enumerating them (as the reference port does) buys speed at
             * the price of a bug class that is invisible until it is a
             * wrong-sounding note weeks later. */
            meLibDcacheWritebackInvalidateAll();

            PspAudioMixer_ExecuteCommandList(cmdList, cmdCount);

            /* Push the PCM and every state block the list just updated back
             * to RAM before announcing completion. */
            meLibDcacheWritebackInvalidateAll();
            meLibSync();
            sMeState = PSP_AUDIO_ME_STATE_IDLE;
            meLibSync();
        } else {
            meLibDelayPipeline();
        }
    }

    sMeState = PSP_AUDIO_ME_STATE_HALTED;
    meLibSync();
    meLibHalt();
}

int32_t PspAudioMe_Init(void) {
    uint32_t start;

    if (sMeBootAttempted) {
        return sMeAvailable ? 0 : -1;
    }
    sMeBootAttempted = 1;

    sMeCmdList = 0;
    sMeCmdCount = 0;
    sMeAiBuffer = 0;
    sMeAiBytes = 0;
    sMeProgress = 0;
    sMeState = PSP_AUDIO_ME_STATE_BOOTING;
    meLibSync();

    /* Loads the embedded kcall.prx through a RELATIVE path, so this only
     * works from the main thread, which is the only one with a cwd -- see the
     * NOCWD note in psp/docs/AUDIO_N64_VS_PSP.md. A negative result here is
     * the normal outcome under PPSSPP and is not an error. */
    /* Keep the exact result. "The offload is not running" has two very
     * different causes and the counters cannot tell them apart: a readiness
     * timeout bumps sStatTimeouts, but a failure here bumps nothing at all, so
     * it looks identical to never having enabled the offload. And the reason
     * matters -- kcall.prx missing is a packaging problem, while a privilege
     * refusal means the EBOOT would have to run in kernel mode, which is a
     * much larger decision. */
    gPspAudioMeInitResult = meLibDefaultInit();
    if (gPspAudioMeInitResult < 0) {
        sMeState = PSP_AUDIO_ME_STATE_STOP;
        meLibSync();
        return -1;
    }

    meLibSync();
    sMeState = PSP_AUDIO_ME_STATE_IDLE;
    meLibSync();

    /* Do not declare the ME usable until it has actually reached its loop.
     * Submitting into a core still in startup produces one timeout and then
     * permanent CPU mixing, which looks exactly like the ME never working. */
    start = sceKernelGetSystemTimeLow();
    while (sMeProgress < 2) {
        if ((sceKernelGetSystemTimeLow() - start) >= PSP_AUDIO_ME_READY_USEC) {
            sMeState = PSP_AUDIO_ME_STATE_STOP;
            meLibSync();
            sStatTimeouts++;
            return -1;
        }
        sceKernelDelayThread(100);
    }

    sMeAvailable = 1;
    return 0;
}

void PspAudioMe_Shutdown(void) {
    if (!sMeAvailable) {
        return;
    }
    sMeAvailable = 0;
    sMeState = PSP_AUDIO_ME_STATE_STOP;
    meLibSync();
}

void PspAudio_RunCommandList(const Acmd* cmdList, int32_t cmdCount, int16_t* aiBuffer, int32_t aiFrames) {
    uint32_t start;
    uint32_t aiBytes;

    if ((cmdList == NULL) || (cmdCount <= 0)) {
        return;
    }

    if (!sMeAvailable || (sMeState != PSP_AUDIO_ME_STATE_IDLE)) {
        sStatCpuJobs++;
        PspAudioMixer_ExecuteCommandList(cmdList, cmdCount);
        return;
    }

    aiBytes = (aiBuffer != NULL) ? (uint32_t)aiFrames * 2u * sizeof(int16_t) : 0u;

    /* Publish this core's writes -- the command list itself, and everything
     * the sequencer touched this tick -- before the ME is allowed to look. */
    sceKernelDcacheWritebackAll();

    sMeCmdList = (uint32_t)(uintptr_t)cmdList;
    sMeCmdCount = (uint32_t)cmdCount;
    sMeAiBuffer = (uint32_t)(uintptr_t)aiBuffer;
    sMeAiBytes = aiBytes;
    meLibSync();
    sMeState = PSP_AUDIO_ME_STATE_RUN;
    meLibSync();

    start = sceKernelGetSystemTimeLow();
    while (sMeState == PSP_AUDIO_ME_STATE_RUN) {
        if ((sceKernelGetSystemTimeLow() - start) >= PSP_AUDIO_ME_TIMEOUT_USEC) {
            /* Give up on the ME permanently and redo the work here. The list
             * is idempotent only because the ME never got far enough to
             * publish anything -- if it had, it would have cleared the state
             * word. */
            sMeAvailable = 0;
            sStatTimeouts++;
            sStatCpuJobs++;
            PspAudioMixer_ExecuteCommandList(cmdList, cmdCount);
            return;
        }
    }

    sStatLastJobUsec = sceKernelGetSystemTimeLow() - start;

    /* The ME wrote the PCM (and the mixer state blocks) straight to RAM. This
     * core may still hold stale cache lines for them. */
    /* Writeback-invalidate rather than a plain invalidate: the SDK offers no
     * invalidate-all, and dropping dirty lines wholesale would discard writes
     * other threads made while this one waited. The ME's output regions are
     * touched by nobody else, so the writeback half is a no-op for them. */
    sceKernelDcacheWritebackInvalidateAll();
    sStatMeJobs++;
}

int32_t PspAudioMe_IsActive(void) {
    return sMeAvailable;
}

#else /* no ME support compiled in */

int32_t PspAudioMe_Init(void) {
    return -1;
}

void PspAudioMe_Shutdown(void) {
}

void PspAudio_RunCommandList(const Acmd* cmdList, int32_t cmdCount, int16_t* aiBuffer, int32_t aiFrames) {
    (void)aiBuffer;
    (void)aiFrames;
    sStatCpuJobs++;
    PspAudioMixer_ExecuteCommandList(cmdList, cmdCount);
}

int32_t PspAudioMe_IsActive(void) {
    return 0;
}

#endif

uint32_t PspAudioMe_StatMeJobs(void) {
    return sStatMeJobs;
}

uint32_t PspAudioMe_StatCpuJobs(void) {
    return sStatCpuJobs;
}

uint32_t PspAudioMe_StatTimeouts(void) {
    return sStatTimeouts;
}

uint32_t PspAudioMe_StatLastJobUsec(void) {
    return sStatLastJobUsec;
}
