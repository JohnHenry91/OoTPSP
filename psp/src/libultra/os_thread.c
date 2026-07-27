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

/* One entry per libultra thread OoT actually creates (Idle, Main, Graph,
 * Sched, Audio, PadMgr, IrqMgr, DmaMgr, Fault ~= 8-9) plus headroom. */
#define MAX_TRACKED_THREADS 16

/* N64 stacks are tiny (0x500-0x900 bytes) because RAM was tiny; PSP has far
 * more to spare, and osCreateThread's `sp` argument is just the top of a
 * caller-allocated buffer (via the STACK() macro) that sceKernelCreateThread
 * has no equivalent parameter for — PSP allocates its own thread stacks
 * internally. So the N64 stack buffer becomes dead space and every shim
 * thread gets one fixed, generously-sized real stack instead. */
#define PSP_THREAD_STACK_SIZE (32 * 1024)

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
static int PspPriorityFromOSPri(OSPri pri) {
    int p = 64 - (int)pri;
    if (p < 10) {
        p = 10;
    }
    if (p > 100) {
        p = 100;
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
    e->uid = sceKernelCreateThread("oot_thread", OSThreadTrampoline, PspPriorityFromOSPri(pri), PSP_THREAD_STACK_SIZE,
                                    0, NULL);
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
