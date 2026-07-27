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

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    MesgQueueMapEntry* e = FindEntry(mq);
    s32 index;

    if (e == NULL) {
        return -1; /* osCreateMesgQueue was never called on this address */
    }

    if (flag == OS_MESG_BLOCK) {
        sceKernelWaitSema(e->semFree, 1, NULL);
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
        sceKernelWaitSema(e->semFull, 1, NULL);
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
