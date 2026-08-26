/* Media Engine offload for the audio microcode. See psp/include/psp_audio_me.h
 * for what this is and why the CPU fallback is mandatory.
 *
 * The protocol is one state word plus a job description, all uncached and all
 * written in a fixed order. Submitting and collecting are two separate steps:
 *
 *   CPU submit : collect the previous job -> fill job fields -> sync
 *                -> state = RUN -> RETURN IMMEDIATELY
 *   ME         : see RUN -> run the list -> sync -> state = IDLE
 *                -> raise the completion interrupt
 *   CPU collect: sleep on the completion semaphore until state leaves RUN
 *
 * The split is the whole point. The first version published RUN and then spun
 * on the state word until the ME was done, so the main core paid for the mix
 * anyway -- it just paid it in a busy-wait instead of in the mixer. Now the
 * collect for tick N happens at the submit for tick N+1, so the ME mixes tick
 * N while the main core runs the sequence player, the sample DMAs and the
 * command building for tick N+1. That overlap is the entire gain.
 *
 * The sync before publishing RUN is what makes the job fields visible; the
 * one after the list is what makes the PCM visible. Neither can be dropped,
 * and neither shows up as a crash when it is -- only as stale or missing
 * sound.
 *
 * WHY ONE TICK OF DEFERRAL IS SAFE. Between submit N and collect N the main
 * core runs AudioSynth_Update for tick N+1, which touches note and reverb
 * state that the ME's mixer never reads: the command list is a closed
 * description, and the mixer only ever touches DMEM, the sample data the list
 * names, the reverb ring buffers and the AI buffer. Note synthesis state,
 * ADPCM and resampler state blocks are written by the mixer but only ever
 * read back by the mixer, so the two cores never look at the same word.
 * The AI buffers give three ticks of slack: AudioThread_Update hands
 * aiBuffers[curAiBufIndex - 2] to the DAC, two full ticks after the mix that
 * filled it, and the collect that invalidates it happens one tick after.
 * The one genuinely shared resource is the sample DMA cache, which tick N+1
 * may recycle while the ME still reads it for tick N -- only for a buffer
 * whose TTL had already expired, i.e. one no note used last tick. The
 * reference port (reference/oot-psp-z2442) takes the same window.
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
static uint32_t sStatLastWaitUsec;
static uint32_t sStatMaxWaitUsec;
static uint32_t sStatFreeCollects;

#if TARGET_PSP && PSP_AUDIO_ME_ENABLED

#include <me-core-mapper/me-core.h>
#include <pspintrman.h>
#include <pspkernel.h>
#include <pspsdk.h>

#define PSP_AUDIO_ME_STATE_BOOTING 0
#define PSP_AUDIO_ME_STATE_IDLE    1
#define PSP_AUDIO_ME_STATE_RUN     2
#define PSP_AUDIO_ME_STATE_STOP    3
#define PSP_AUDIO_ME_STATE_HALTED  4
#define PSP_AUDIO_ME_STATE_FAULT   5

/* One audio frame is ~16 ms of work at most; anything past this means the ME
 * is wedged, and the job is redone on the CPU rather than dropped. After a
 * timeout the ME is never used again -- a wedged core does not recover, and
 * silently retrying it would cost a stall every single frame. */
#define PSP_AUDIO_ME_TIMEOUT_USEC 40000
#define PSP_AUDIO_ME_READY_USEC   250000

/* Longest single sleep on the completion semaphore. The state word, not the
 * semaphore, is the authority on whether the job finished; the semaphore only
 * decides how fast we notice. Capping each sleep means a signal that never
 * arrives -- an interrupt lost to a race, or a ME that faulted before it
 * could raise one -- costs this much extra latency instead of the full 40 ms
 * timeout followed by a needless fallback to CPU mixing. */
#define PSP_AUDIO_ME_WAIT_SLICE_USEC 2000

/* Poll interval for the paths that have no semaphore to sleep on. */
#define PSP_AUDIO_ME_POLL_USEC 100

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
static volatile uint32_t sMeShared[7 * 16] __attribute__((aligned(64)));

#define ME_SHARED(i) (((volatile uint32_t*)((uintptr_t)sMeShared | 0x40000000u))[(i) * 16])

#define sMeState     ME_SHARED(0)
#define sMeCmdList   ME_SHARED(1)
#define sMeCmdCount  ME_SHARED(2)
#define sMeAiBuffer  ME_SHARED(3)
#define sMeAiBytes   ME_SHARED(4)
#define sMeProgress  ME_SHARED(5)
/* Read by the ME after every job and on both fault paths. Kept in shared
 * memory rather than as a ME-side constant so the CPU can turn the interrupt
 * off again without stopping the core. */
#define sMeIntrArmed ME_SHARED(6)

static int32_t sMeAvailable;
static int32_t sMeBootAttempted;

/* The job the ME is working on right now, owned by the submitting thread.
 * Kept on the CPU side (not in shared memory): only this core reads it, and
 * the fallback path needs the original pointer after the ME has been given
 * up on. */
static const Acmd* sPendingCmdList;
static int32_t sPendingCmdCount;
static uint32_t sPendingStartUsec;
static int32_t sPendingJob;

static SceUID sCompletionSema = -1;
static int32_t sCompletionIntrReady;

static int32_t PspAudioMe_JobIsRunning(void) {
    return sMeState == PSP_AUDIO_ME_STATE_RUN;
}

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

            /* Wake the collector. Without this the main core would have to
             * poll, and polling at a useful resolution costs exactly the CPU
             * time this whole file exists to save. */
            if (sMeIntrArmed) {
                meLibSendExternalSoftInterrupt();
            }
        } else {
            meLibDelayPipeline();
        }
    }

    sMeState = PSP_AUDIO_ME_STATE_HALTED;
    meLibSync();
    meLibHalt();
}

/* libme-core defines both of these weakly as bare halts. Overriding them
 * matters now that the main core sleeps instead of spinning: a ME that dies
 * mid-job would otherwise never signal anything, and the collector would wait
 * out the full 40 ms timeout on every remaining tick before giving up. Report
 * the fault, ring the bell, then halt. */
__attribute__((noinline, aligned(4))) void meLibOnException(void) {
    sMeState = PSP_AUDIO_ME_STATE_FAULT;
    meLibSync();
    if (sMeIntrArmed) {
        meLibSendExternalSoftInterrupt();
    }
    meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnExternalInterrupt(void) {
    sMeState = PSP_AUDIO_ME_STATE_FAULT;
    meLibSync();
    if (sMeIntrArmed) {
        meLibSendExternalSoftInterrupt();
    }
    meLibHalt();
}

/* Runs in interrupt context: signalling a semaphore is the only thing that is
 * legal and the only thing needed. Everything about the completed job is
 * already in shared memory. */
static void PspAudioMe_CompletionHandler(int subIntr, void* arg) {
    (void)subIntr;
    (void)arg;
    if (sCompletionSema >= 0) {
        sceKernelSignalSema(sCompletionSema, 1);
    }
}

/* Failure here is not fatal: sMeIntrArmed stays false, the ME skips the
 * interrupt, and the collector falls back to polling with a short delay. That
 * costs main-CPU time but still overlaps the mix with the sequence player, so
 * it is strictly better than the busy-wait this replaced. */
static int32_t PspAudioMe_EnsureCompletionInterrupt(void) {
    int32_t ret;

    if (sCompletionIntrReady) {
        return 0;
    }

    if (sCompletionSema < 0) {
        SceUID sema = sceKernelCreateSema("OoTPspAudioMeDone", 0, 0, 1, NULL);

        if (sema < 0) {
            return (int32_t)sema;
        }
        sCompletionSema = sema;
    }

    ret = sceKernelRegisterSubIntrHandler(PSP_MECODEC_INT, 0, (void*)PspAudioMe_CompletionHandler, NULL);
    if (ret < 0) {
        return ret;
    }

    ret = sceKernelEnableSubIntr(PSP_MECODEC_INT, 0);
    if (ret < 0) {
        sceKernelReleaseSubIntrHandler(PSP_MECODEC_INT, 0);
        return ret;
    }

    sMeIntrArmed = 1;
    meLibSync();
    sCompletionIntrReady = 1;
    return 0;
}

/* A completion signalled for a job that was already collected (or one the ME
 * raised while we were not looking) would otherwise satisfy the NEXT wait
 * immediately and let the collector read a half-written buffer. Drain before
 * every submit so the semaphore only ever carries signals for the job in
 * flight. */
static void PspAudioMe_DrainCompletion(void) {
    if (!sCompletionIntrReady) {
        return;
    }
    while (sceKernelPollSema(sCompletionSema, 1) == 0) {
    }
}

int32_t PspAudioMe_Init(void) {
    uint32_t start;

    if (sMeBootAttempted) {
        return sMeAvailable ? 0 : -1;
    }
    sMeBootAttempted = 1;

    /* sMeShared lives in .bss, which the loader cleared through the CACHED
     * mapping -- so this core can still be holding dirty lines for a block
     * that is only ever meant to be touched uncached. A later
     * sceKernelDcacheWritebackAll on the submit path would then flush those
     * stale zeroes over whatever the ME had written. Retire them once, here,
     * before the block carries anything worth keeping. */
    sceKernelDcacheWritebackInvalidateAll();

    sMeCmdList = 0;
    sMeCmdCount = 0;
    sMeAiBuffer = 0;
    sMeAiBytes = 0;
    sMeProgress = 0;
    sMeIntrArmed = 0;
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

    /* Arm the interrupt while the ME is still parked in BOOTING, so the flag
     * is already visible to it before it can finish a first job. */
    PspAudioMe_EnsureCompletionInterrupt();

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
        sceKernelDelayThread(PSP_AUDIO_ME_POLL_USEC);
    }

    sMeAvailable = 1;
    return 0;
}

void PspAudioMe_Shutdown(void) {
    if (!sMeAvailable) {
        return;
    }
    /* Never leave a job in flight: the ME would keep writing into audio heap
     * buffers after this core believes the offload is gone. */
    PspAudio_WaitForCommandList();
    sMeAvailable = 0;
    sMeState = PSP_AUDIO_ME_STATE_STOP;
    meLibSync();
}

/* Give up on the ME and redo the outstanding job here. Safe to re-run the
 * list only because the ME never published anything: it clears the state word
 * as its last act, so a job still marked RUN produced no output. */
static void PspAudioMe_FallBackFromPending(void) {
    const Acmd* cmdList = sPendingCmdList;
    int32_t cmdCount = sPendingCmdCount;

    sMeAvailable = 0;
    sPendingJob = 0;
    sPendingCmdList = NULL;
    sPendingCmdCount = 0;

    sStatTimeouts++;
    sStatCpuJobs++;
    if ((cmdList != NULL) && (cmdCount > 0)) {
        PspAudioMixer_ExecuteCommandList(cmdList, cmdCount);
    }
}

void PspAudio_WaitForCommandList(void) {
    uint32_t waitStart;
    uint32_t elapsed;

    if (!sPendingJob) {
        return;
    }

    waitStart = sceKernelGetSystemTimeLow();

    while (PspAudioMe_JobIsRunning()) {
        elapsed = sceKernelGetSystemTimeLow() - sPendingStartUsec;
        if (elapsed >= PSP_AUDIO_ME_TIMEOUT_USEC) {
            break;
        }

        if (sCompletionIntrReady) {
            SceUInt timeout = PSP_AUDIO_ME_TIMEOUT_USEC - elapsed;

            /* Sleep in slices and re-read the state word each time. See the
             * comment on PSP_AUDIO_ME_WAIT_SLICE_USEC: the semaphore is a
             * hint, the state word is the truth. */
            if (timeout > PSP_AUDIO_ME_WAIT_SLICE_USEC) {
                timeout = PSP_AUDIO_ME_WAIT_SLICE_USEC;
            }
            sceKernelWaitSema(sCompletionSema, 1, &timeout);
        } else {
            sceKernelDelayThread(PSP_AUDIO_ME_POLL_USEC);
        }
    }

    elapsed = sceKernelGetSystemTimeLow() - waitStart;
    sStatLastWaitUsec = elapsed;
    if (elapsed > sStatMaxWaitUsec) {
        sStatMaxWaitUsec = elapsed;
    }
    if (elapsed < PSP_AUDIO_ME_POLL_USEC) {
        /* The job was already done when we came to collect it -- the whole
         * mix rode along inside the tick's other work and cost this core
         * nothing. This counter climbing in step with meJobs is what "the
         * offload is actually buying us time" looks like. */
        sStatFreeCollects++;
    }

    /* Anything other than a clean IDLE means the ME is wedged or faulted. */
    if (sMeState != PSP_AUDIO_ME_STATE_IDLE) {
        PspAudioMe_FallBackFromPending();
        return;
    }

    sPendingJob = 0;
    sPendingCmdList = NULL;
    sPendingCmdCount = 0;
    sStatLastJobUsec = sceKernelGetSystemTimeLow() - sPendingStartUsec;

    /* The ME wrote the PCM (and the mixer state blocks) straight to RAM. This
     * core may still hold stale cache lines for them. */
    /* Writeback-invalidate rather than a plain invalidate: the SDK offers no
     * invalidate-all, and dropping dirty lines wholesale would discard writes
     * other threads made while this one waited. The ME's output regions are
     * touched by nobody else, so the writeback half is a no-op for them. */
    sceKernelDcacheWritebackInvalidateAll();
    sStatMeJobs++;
}

void PspAudio_RunCommandList(const Acmd* cmdList, int32_t cmdCount, int16_t* aiBuffer, int32_t aiFrames) {
    uint32_t aiBytes;

    /* Collect the previous tick's job first. On the ME path this is where the
     * main core finally blocks -- by now the mix has usually been finished
     * for most of a tick and this returns straight away. */
    PspAudio_WaitForCommandList();

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

    PspAudioMe_DrainCompletion();

    sMeCmdList = (uint32_t)(uintptr_t)cmdList;
    sMeCmdCount = (uint32_t)cmdCount;
    sMeAiBuffer = (uint32_t)(uintptr_t)aiBuffer;
    sMeAiBytes = aiBytes;

    sPendingCmdList = cmdList;
    sPendingCmdCount = cmdCount;
    sPendingStartUsec = sceKernelGetSystemTimeLow();
    sPendingJob = 1;

    meLibSync();
    sMeState = PSP_AUDIO_ME_STATE_RUN;
    meLibSync();
    /* Deliberately no wait here. The collect happens at the next call. */
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

void PspAudio_WaitForCommandList(void) {
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

uint32_t PspAudioMe_StatLastWaitUsec(void) {
    return sStatLastWaitUsec;
}

uint32_t PspAudioMe_StatMaxWaitUsec(void) {
    return sStatMaxWaitUsec;
}

uint32_t PspAudioMe_StatFreeCollects(void) {
    return sStatFreeCollects;
}
