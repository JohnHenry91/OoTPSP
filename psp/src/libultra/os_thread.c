/* libultra thread lifecycle backed by real PSP threads (pspthreadman).
 *
 * Earlier version of this file made osCreateThread/osStartThread no-ops,
 * mirroring Shipwright's soh/soh/stubs.c. That works for Shipwright because
 * it rewrote asset loading around its own resource manager and never runs
 * N64 OoT's actual DmaMgr thread. We're porting the real src/boot/z_std_dma.c
 * unmodified, and DmaMgr_RequestSync's blocking request/reply pattern (send
 * to sDmaMgrMsgQueue, block on osRecvMesg for the reply) only terminates if
 * something is genuinely, concurrently draining that queue. A no-op thread
 * body never runs, so the request hangs forever. PSP has real preemptive
 * threads (unlike a bare desktop OpenGL/SDL loop), so the fix is to actually
 * use them instead of building a per-frame pump for every subsystem.
 *
 * OSThread's layout (thread.h) is kept N64-compatible for decomp code that
 * embeds one directly; the real PSP thread id (SceUID) lives in a side
 * table here, keyed by the OSThread* the decomp code already owns.
 */

#include <pspthreadman.h>
#include <stddef.h>
#include <string.h>

#include "ultra64.h"
#include "thread.h"
#include "psp_fpu.h"

/* One entry per libultra thread OoT actually creates (Idle, Main, Graph,
 * Sched, Audio, PadMgr, IrqMgr, DmaMgr, Fault ~= 8-9) plus headroom. */
#define MAX_TRACKED_THREADS 16

/* N64 stacks are tiny (0x500-0x900 bytes) because RAM was tiny; PSP has far
 * more to spare, and osCreateThread's `sp` argument is just the top of a
 * caller-allocated buffer (via the STACK() macro) that sceKernelCreateThread
 * has no equivalent parameter for — PSP allocates its own thread stacks
 * internally. So the N64 stack buffer becomes dead space and every shim
 * thread gets one fixed, generously-sized real stack instead. */
/* 128 KB, matching what the other PSP port of this game gives its game
 * threads (reference/oot-psp-z2442, libultra_psp.c: 0x20000).
 *
 * This was 32 KB. The Graph thread is not a worker -- it runs the WHOLE game:
 * Graph_Update, every actor's update and draw, and gfx_pc.c's display list
 * interpreter, which keeps vertex and clipping arrays on the stack. On N64 that
 * thread gets a correspondingly large stack from the STACK() macro; the PSP
 * side sizes it here instead, and 32 KB was simply inherited without anyone
 * checking it against that call depth.
 *
 * Overflowing a thread stack is another failure PPSSPP hides: it writes past
 * the allocation into whatever happens to follow, which under an emulator is
 * often unused and harmless, and on hardware is somebody else's data. */
#define PSP_THREAD_STACK_SIZE (128 * 1024)

typedef struct {
    OSThread* thread; /* NULL = free slot */
    SceUID uid;
    void (*entry)(void*);
    void* arg;
} ThreadMapEntry;

static ThreadMapEntry sThreadMap[MAX_TRACKED_THREADS];

static ThreadMapEntry* FindEntry(OSThread* thread) {
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (sThreadMap[i].thread == thread) {
            return &sThreadMap[i];
        }
    }
    return NULL;
}

static ThreadMapEntry* FindEntryByUid(SceUID uid) {
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (sThreadMap[i].thread != NULL && sThreadMap[i].uid == uid) {
            return &sThreadMap[i];
        }
    }
    return NULL;
}

static ThreadMapEntry* AllocEntry(OSThread* thread) {
    ThreadMapEntry* e = FindEntry(thread);
    if (e != NULL) {
        return e;
    }
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (sThreadMap[i].thread == NULL) {
            sThreadMap[i].thread = thread;
            sThreadMap[i].uid = -1;
            return &sThreadMap[i];
        }
    }
    return NULL;
}

/* N64 priority: higher = more urgent (0-255). PSP priority: lower number =
 * more urgent. Invert onto a safe user-mode range; OoT only ever uses
 * 10-17 plus OS_PRIORITY_APPMAX(127)-ish for Fault, so this doesn't need to
 * be more than monotonic and clamped. */
/* N64 and PSP order priorities in opposite directions: on N64 a HIGHER OSPri
 * wins, on PSP a LOWER number wins. So the mapping has to invert -- but it
 * also has to land the whole band ABOVE the PSP main thread, and that part was
 * missed for a long time.
 *
 * PSPSDK gives `main` priority 32 (0x20). The old mapping (64 - pri) put every
 * one of the game's threads in the 48-54 range, i.e. strictly BELOW main, and
 * PSP scheduling is strict priority: a thread at 52 runs only when nothing at
 * 32 or above is runnable. AudioMgr therefore only got whatever CPU the render
 * loop happened to leave -- fine while it merely had to service one retrace
 * message per frame, but once it became the thread that actually synthesizes
 * audio (see AudioMgr_ThreadEntry's TARGET_PSP branch) that starvation is
 * audible directly as choppy, dropout-ridden sound.
 *
 * The game's own priorities span THREAD_PRI_IDLE_INIT(10)..THREAD_PRI_DMAMGR(16)
 * (include/thread.h), so map that band onto 31..25 -- all above main, and with
 * N64's relative ordering preserved: DmaMgr over Main over PadMgr over
 * AudioMgr over Graph, exactly as on hardware. Audio above the render loop is
 * both what N64 does (AUDIOMGR 12 > GRAPH 11) and what PSP audio needs, since
 * an audio thread spends nearly all its time asleep inside a blocking output
 * call and only needs to be serviced promptly when it wakes. */
#define PSP_MAIN_THREAD_PRIORITY 32

static int PspPriorityFromOSPri(OSPri pri) {
    int p = (PSP_MAIN_THREAD_PRIORITY - 1) - ((int)pri - THREAD_PRI_IDLE_INIT);

    /* Never reach the system's own high-priority range, and never fall back to
     * or below main -- the whole point is that these outrank the render loop. */
    if (p < 16) {
        p = 16;
    }
    if (p > PSP_MAIN_THREAD_PRIORITY - 1) {
        p = PSP_MAIN_THREAD_PRIORITY - 1;
    }
    return p;
}

typedef struct {
    void (*entry)(void*);
    void* arg;
} ThreadTrampolineArgs;

static int OSThreadTrampoline(SceSize argSize, void* argp) {
    /* sceKernelStartThread copies arglen bytes onto the new thread's own
     * stack and hands us a pointer to that copy, so this is safe even
     * though the original argp (a stack local in osStartThread) is long
     * gone by the time this actually runs. */
    ThreadTrampolineArgs args = *(ThreadTrampolineArgs*)argp;

    /* FCR31 is per-thread context: whatever main() set does not carry over
     * here. Every engine thread that runs float code -- Graph for the renderer,
     * AudioMgr for the whole synthesis path -- needs its own call, or a
     * denormal envelope value traps and kills the console. See psp_fpu.h. */
    PspFpu_ConfigureThread();

    args.entry(args.arg);
    return 0;
}

void osCreateThread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    ThreadMapEntry* e = AllocEntry(thread);

    memset(thread, 0, sizeof(*thread));
    thread->id = id;
    thread->priority = pri;
    thread->state = OS_STATE_STOPPED;

    if (e == NULL) {
        return; /* thread map exhausted — shouldn't happen, OoT creates ~8 */
    }

    e->entry = entry;
    e->arg = arg;
    /* THREAD_ATTR_USER | THREAD_ATTR_VFPU, not 0.
     *
     * These are the game's own threads -- Graph, AudioMgr, PadMgr -- and the
     * Graph one runs the entire renderer, including sceGum* and pspmath, which
     * are VFPU code. An attribute of 0 left the vector unit disabled, so the
     * first such instruction raised a Coprocessor Unusable exception on real
     * hardware. It never surfaced under PPSSPP, which does not enforce the
     * attribute; the boot trace from a PSP-2000 showed all of main() completing
     * and the failure landing immediately afterwards, which is exactly where
     * Graph_ThreadEntry starts running on this thread. */
    e->uid = sceKernelCreateThread("oot_thread", OSThreadTrampoline, PspPriorityFromOSPri(pri), PSP_THREAD_STACK_SIZE,
                                    THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
}

void osStartThread(OSThread* thread) {
    ThreadMapEntry* e = FindEntry(thread);
    ThreadTrampolineArgs targs;

    if (e == NULL || e->uid < 0) {
        return;
    }

    thread->state = OS_STATE_RUNNABLE;
    targs.entry = e->entry;
    targs.arg = e->arg;
    sceKernelStartThread(e->uid, sizeof(targs), &targs);
}

void osStopThread(OSThread* thread) {
    ThreadMapEntry* e = FindEntry(thread);

    if (e == NULL || e->uid < 0) {
        return;
    }

    thread->state = OS_STATE_STOPPED;
    sceKernelSuspendThread(e->uid);
}

void osDestroyThread(OSThread* thread) {
    ThreadMapEntry* e = FindEntry(thread);

    if (e == NULL) {
        return;
    }

    if (e->uid >= 0) {
        sceKernelTerminateDeleteThread(e->uid);
    }
    e->thread = NULL;
    e->uid = -1;
}

void osSetThreadPri(OSThread* thread, OSPri pri) {
    if (thread == NULL) {
        /* NULL means "the calling thread" in libultra */
        sceKernelChangeThreadPriority(0, PspPriorityFromOSPri(pri));
        return;
    }

    ThreadMapEntry* e = FindEntry(thread);
    thread->priority = pri;
    if (e != NULL && e->uid >= 0) {
        sceKernelChangeThreadPriority(e->uid, PspPriorityFromOSPri(pri));
    }
}

OSPri osGetThreadPri(OSThread* thread) {
    if (thread == NULL) {
        /* Inverse of PspPriorityFromOSPri isn't exact (clamped), but this
         * is only ever used for informational/debug purposes in OoT. */
        return 64 - sceKernelGetThreadCurrentPriority();
    }
    return thread->priority;
}

OSId osGetThreadId(OSThread* thread) {
    if (thread != NULL) {
        return thread->id;
    }

    ThreadMapEntry* e = FindEntryByUid(sceKernelGetThreadId());
    return (e != NULL) ? e->thread->id : 0;
}

void osYieldThread(void) {
    sceKernelDelayThread(0);
}
