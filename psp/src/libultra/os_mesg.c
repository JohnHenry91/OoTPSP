/* Message queues backed by real PSP semaphores, so OS_MESG_BLOCK actually
 * blocks the calling thread instead of returning -1.
 *
 * The previous version of this file (ported from Shipwright's
 * libultraship/os_mesg.cpp) treated every queue as non-blocking, which
 * relied on nothing ever really needing to block — true for Pad polling,
 * false for src/boot/z_std_dma.c's DmaMgr: DmaMgr_RequestSync sends a
 * request then blocks on osRecvMesg waiting for the DMA thread's reply. Now
 * that os_thread.c uses real PSP threads (see that file's header comment),
 * this needs to be real blocking too, or the DMA thread and its caller can
 * never actually hand off to each other.
 *
 * Classic bounded-buffer producer/consumer, one triplet of PSP kernel
 * objects per OSMesgQueue:
 *   - semFree: counts empty slots (senders wait on this when full)
 *   - semFull: counts filled slots (receivers wait on this when empty)
 *   - mutex:   protects the ring buffer indices during the actual copy
 *
 * libultra has no osDestroyMesgQueue — queues just stop being used once
 * their backing memory (often a stack local, re-created on every call, e.g.
 * every DmaMgr_RequestSync) goes out of scope. Since PSP semaphores are
 * kernel objects that must be explicitly deleted or they leak, this table
 * treats a second osCreateMesgQueue on the same address as "this queue was
 * torn down and rebuilt", which is exactly what it means in practice: the
 * same stack slot getting reinitialized call after call.
 */

#include <pspthreadman.h>
#include <stddef.h>

#include "ultra64.h"

/* ---------------------------------------------------------------------------
 * Main-thread deadlock guard.
 *
 * This port's single most common failure mode is a silent stall: the game stops
 * responding with no crash, no assert and no CPU load, because the main thread
 * is parked in a blocking queue operation whose counterpart no longer exists.
 * That happens because the N64 thread topology was collapsed into one loop, so
 * several libultra send/receive pairs lost one half (GameState_Init/Destroy's
 * gfxCtx->queue token is one example that cost two debugging rounds).
 *
 * reference/libultraship/src/libultraship/libultra/os_mesg.cpp solves this by
 * making the queues NEVER block -- it ignores the OS_MESG_BLOCK flag entirely
 * and returns -1 when a queue is full or empty. That is safe there because
 * that port has no libultra worker threads at all.
 *
 * We cannot copy it wholesale: padmgr.c and z_std_dma.c really do create PSP
 * threads here (they are the two `oot_thread` entries visible in PPSSPP's
 * thread list), and their entry functions are `for(;;) osRecvMesg(..., BLOCK)`
 * loops. Making those non-blocking would turn them into busy-wait spins.
 *
 * So the guard is applied where the problem actually is: only the MAIN thread
 * gets a bounded wait. Worker threads keep waiting forever, which is precisely
 * what they are for. On timeout the call returns -1 like libultraship's would,
 * leaving the caller's own error handling to deal with it -- a bounded glitch
 * instead of a dead console, the same philosophy as the os_cache argument
 * clamping and the display-list depth cap.
 * ------------------------------------------------------------------------- */
#define PSP_MESG_MAIN_TIMEOUT_US 2000000 /* generous: no legitimate wait is this long */

static SceUID sPspMainThreadId = -1;

/* Counts waits that timed out rather than being satisfied. Non-zero means a
 * real send/receive imbalance was hit and survived -- pair it with
 * gPspMesgBlockedSendRa/RecvRa to find the call site. */
unsigned int gPspMesgSendTimeouts;
unsigned int gPspMesgRecvTimeouts;

void PspOsMesgSetMainThread(void) {
    sPspMainThreadId = sceKernelGetThreadId();
}

/* Wait on `sema`, bounded if we are the main thread. Returns 0 on success. */
static int PspMesgWait(SceUID sema) {
    if (sPspMainThreadId >= 0 && sceKernelGetThreadId() == sPspMainThreadId) {
        SceUInt timeout = PSP_MESG_MAIN_TIMEOUT_US;

        return sceKernelWaitSema(sema, 1, &timeout) < 0 ? -1 : 0;
    }
    return sceKernelWaitSema(sema, 1, NULL) < 0 ? -1 : 0;
}

#define MAX_TRACKED_QUEUES 64

typedef struct {
    OSMesgQueue* mq; /* NULL = free slot */
    SceUID semFree;
    SceUID semFull;
    SceUID mutex;
} MesgQueueMapEntry;

static MesgQueueMapEntry sQueueMap[MAX_TRACKED_QUEUES];
static int sNextEvictIdx = 0;

typedef struct {
    OSMesgQueue* queue;
    OSMesg msg;
} __OSEventState;

static __OSEventState sEventStateTab[OS_NUM_EVENTS];

static void DeleteEntrySemas(MesgQueueMapEntry* e) {
    if (e->semFree >= 0) {
        sceKernelDeleteSema(e->semFree);
    }
    if (e->semFull >= 0) {
        sceKernelDeleteSema(e->semFull);
    }
    if (e->mutex >= 0) {
        sceKernelDeleteSema(e->mutex);
    }
}

static MesgQueueMapEntry* FindEntry(OSMesgQueue* mq) {
    for (int i = 0; i < MAX_TRACKED_QUEUES; i++) {
        if (sQueueMap[i].mq == mq) {
            return &sQueueMap[i];
        }
    }
    return NULL;
}

static MesgQueueMapEntry* AllocEntry(OSMesgQueue* mq) {
    MesgQueueMapEntry* e = FindEntry(mq);
    if (e != NULL) {
        DeleteEntrySemas(e);
        return e;
    }

    for (int i = 0; i < MAX_TRACKED_QUEUES; i++) {
        if (sQueueMap[i].mq == NULL) {
            return &sQueueMap[i];
        }
    }

    /* Table exhausted (shouldn't happen in practice — see MAX_TRACKED_QUEUES
     * comment) — evict round-robin rather than leaking silently forever. */
#if TARGET_PSP
    {
        extern void PspDebugLogQueueEvict(void* evictedMq, void* newMq);
        PspDebugLogQueueEvict((void*)sQueueMap[sNextEvictIdx].mq, (void*)mq);
    }
#endif
    e = &sQueueMap[sNextEvictIdx];
    sNextEvictIdx = (sNextEvictIdx + 1) % MAX_TRACKED_QUEUES;
    DeleteEntrySemas(e);
    return e;
}

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msgBuf, s32 count) {
    MesgQueueMapEntry* e = AllocEntry(mq);

    e->mq = mq;
    e->semFree = sceKernelCreateSema("mqFree", 0, count, count, NULL);
    e->semFull = sceKernelCreateSema("mqFull", 0, 0, count, NULL);
    e->mutex = sceKernelCreateSema("mqMutex", 0, 1, 1, NULL);

    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
}

/* Who is parked in a blocking queue operation right now.
 *
 * This port's characteristic failure is not a crash but a silent stall: the
 * game stops responding with no fault, no assert and no CPU load, because a
 * blocking osSendMesg/osRecvMesg is waiting on a queue whose counterpart no
 * longer exists on this platform (the N64 thread topology was collapsed into a
 * single loop, so several send/receive pairs lost one half). Resolving the PC
 * only ever says "osSendMesg" -- useless, since every queue in the game funnels
 * through here. What is actually needed is the CALLER and the QUEUE.
 *
 * So record both immediately before parking, and clear on wake. Non-zero
 * gPspMesgBlockedSendRa while the game is stalled names the exact call site;
 * feed it to the psp-nm nearest-preceding-symbol lookup. Costs two stores on
 * the blocking path only. */
unsigned int gPspMesgBlockedSendRa;
unsigned int gPspMesgBlockedSendQueue;
unsigned int gPspMesgBlockedRecvRa;
unsigned int gPspMesgBlockedRecvQueue;
unsigned int gPspMesgSendWaits;
unsigned int gPspMesgRecvWaits;

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    MesgQueueMapEntry* e = FindEntry(mq);
    s32 index;

    if (e == NULL) {
        return -1; /* osCreateMesgQueue was never called on this address */
    }

    if (flag == OS_MESG_BLOCK) {
        /* Only record when the queue is actually full, i.e. when this call is
         * about to park. A poll that succeeds is the normal case and must stay
         * free of bookkeeping. */
        if (sceKernelPollSema(e->semFree, 1) != 0) {
            int waited;

            ++gPspMesgSendWaits;
            gPspMesgBlockedSendRa = (unsigned int)(uintptr_t)__builtin_return_address(0);
            gPspMesgBlockedSendQueue = (unsigned int)(uintptr_t)mq;
            waited = PspMesgWait(e->semFree);
            if (waited != 0) {
                /* Main-thread timeout: the queue is full and nothing is
                 * draining it. Leave the diagnostics set so the stalled call
                 * site stays identifiable, and fail like libultraship does. */
                ++gPspMesgSendTimeouts;
                return -1;
            }
            gPspMesgBlockedSendRa = 0;
            gPspMesgBlockedSendQueue = 0;
        }
    } else {
        if (sceKernelPollSema(e->semFree, 1) != 0) {
            return -1;
        }
    }

    sceKernelWaitSema(e->mutex, 1, NULL);
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
    sceKernelSignalSema(e->mutex, 1);

    sceKernelSignalSema(e->semFull, 1);
    return 0;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    MesgQueueMapEntry* e = FindEntry(mq);

    if (e == NULL) {
        return -1;
    }

    if (flag == OS_MESG_BLOCK) {
        sceKernelWaitSema(e->semFree, 1, NULL);
    } else {
        if (sceKernelPollSema(e->semFree, 1) != 0) {
            return -1;
        }
    }

    sceKernelWaitSema(e->mutex, 1, NULL);
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    sceKernelSignalSema(e->mutex, 1);

    sceKernelSignalSema(e->semFull, 1);
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    MesgQueueMapEntry* e = FindEntry(mq);

    if (e == NULL) {
        return -1;
    }

    if (flag == OS_MESG_BLOCK) {
        /* See the note above osSendMesg: record caller and queue only when
         * this call is actually about to park. */
        if (sceKernelPollSema(e->semFull, 1) != 0) {
            int waited;

            ++gPspMesgRecvWaits;
            gPspMesgBlockedRecvRa = (unsigned int)(uintptr_t)__builtin_return_address(0);
            gPspMesgBlockedRecvQueue = (unsigned int)(uintptr_t)mq;
            waited = PspMesgWait(e->semFull);
            if (waited != 0) {
                /* Main-thread timeout: nothing is going to post to this queue.
                 * Diagnostics stay set on purpose, see the send side. */
                ++gPspMesgRecvTimeouts;
                return -1;
            }
            gPspMesgBlockedRecvRa = 0;
            gPspMesgBlockedRecvQueue = 0;
        }
    } else {
        if (sceKernelPollSema(e->semFull, 1) != 0) {
            return -1;
        }
    }

    sceKernelWaitSema(e->mutex, 1, NULL);
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    sceKernelSignalSema(e->mutex, 1);

    sceKernelSignalSema(e->semFree, 1);
    return 0;
}

void osSetEventMesg(OSEvent event, OSMesgQueue* mq, OSMesg msg) {
    sEventStateTab[event].queue = mq;
    sEventStateTab[event].msg = msg;
}
