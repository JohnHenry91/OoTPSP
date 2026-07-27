/* TEMPORARY diagnostic for Phase 2's Play_SpawnScene bring-up. Remove once
 * diagnosed -- see src/code/z_play.c's TARGET_PSP block around
 * Play_SpawnScene. Deliberately uses plain C types (not ultra64.h's u8/u32/
 * etc) to avoid conflicting with the PSP SDK headers' own typedefs of the
 * same names. */
#include <stdint.h>
#include <pspiofilemgr.h>
#include <stdio.h>

void PspDebugLogPlaySpawn(int sceneId, int spawn, void* sceneSegment, unsigned int checkVal) {
    char msg[96];
    int len = sprintf(msg, "check=%08x sceneId=%d spawn=%d sceneSegment=%p\n", checkVal, sceneId, spawn,
                       sceneSegment);
    SceUID fd = sceIoOpen("ms0:/play_spawn.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogKeepObject(unsigned int vromStart, unsigned int vromEnd, void* slot0Segment, void* spaceEnd) {
    char msg[128];
    int len = sprintf(msg, "keepObj vromStart=%08x vromEnd=%08x slot0Segment=%p spaceEnd=%p\n", vromStart, vromEnd,
                       slot0Segment, spaceEnd);
    SceUID fd = sceIoOpen("ms0:/keep_obj.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogKeepObject2(void* mainKeepSegment, unsigned int gSegments4) {
    char msg[96];
    int len = sprintf(msg, "keepObj2 mainKeepSegment=%p gSegments4=%08x\n", mainKeepSegment, gSegments4);
    SceUID fd = sceIoOpen("ms0:/keep_obj.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogColHeader(void* cmd, int stage, void* colHeader, void* vtxList, void* polyList, void* surfaceTypeList,
                           void* bgCamList, void* waterBoxes) {
    char msg[192];
    int len = sprintf(msg,
                       "stage=%d cmd=%p colHeader=%p vtx=%p poly=%p surf=%p bgCam=%p wbox=%p\n", stage, cmd,
                       colHeader, vtxList, polyList, surfaceTypeList, bgCamList, waterBoxes);
    SceUID fd = sceIoOpen("ms0:/col_header.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogGSceneTable(void* gSceneTableAddr, void* sceneAddr, int sceneId, unsigned int vromStart,
                             unsigned int vromEnd, unsigned int structSize) {
    char msg[128];
    int len = sprintf(msg, "gSceneTable=%p scene=%p sceneId=%d vromStart=%08x vromEnd=%08x structSize=%u\n",
                       gSceneTableAddr, sceneAddr, sceneId, vromStart, vromEnd, structSize);
    SceUID fd = sceIoOpen("ms0:/scene_table.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogFileLoad(void* allocp, unsigned int vromStart, unsigned int size, unsigned int b0, unsigned int b1,
                          unsigned int b2, unsigned int b3, int stage) {
    char msg[128];
    int len = sprintf(msg, "stage=%d allocp=%p vromStart=%08x size=%u bytes=%02x %02x %02x %02x\n", stage, allocp,
                       vromStart, size, b0, b1, b2, b3);
    SceUID fd = sceIoOpen("ms0:/file_load.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaTest(int ret, unsigned int checksum, int passed) {
    char msg[96];
    int len = sprintf(msg, "dmaTestRet=%d checksum=%08x passed=%d\n", ret, checksum, passed);
    SceUID fd = sceIoOpen("ms0:/dma_test.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogSceneCmd(void* addr, unsigned int code, unsigned int data1, unsigned int data2) {
    char msg[96];
    int len = sprintf(msg, "addr=%p code=%u data1=%02x data2=%04x\n", addr, code, data1, data2);
    SceUID fd = sceIoOpen("ms0:/scene_cmd.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaCaller(void* retAddr) {
    char msg[48];
    int len = sprintf(msg, "  called from %p\n", retAddr);
    SceUID fd = sceIoOpen("ms0:/dma_requests.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

/* TEMPORARY diagnostic: DmaMgr_RequestSync's own immediate caller (its
 * __builtin_return_address(0), reliable since it's captured in the same
 * frame, unlike trying to unwind through DmaMgr_RequestAsync which only
 * ever sees DmaMgr_RequestSync itself as its caller under this build's
 * DMA_REQUEST_SYNC macro). Written to a separate file so it lines up
 * 1:1 with dma_requests.txt's request numbering without needing to change
 * that function's existing log format. See project memory (DMA-thread
 * hang investigation) -- remove once the bad vrom=0x088d7a9c-class
 * request's real caller is found. */
/* TEMPORARY diagnostic: fires whenever os_mesg.c's MesgQueueMapEntry table
 * (MAX_TRACKED_QUEUES slots) is full and has to evict a still-possibly-live
 * entry's semaphores to make room for a new OSMesgQueue address. See project
 * memory (DMA-thread hang investigation) -- if this ever fires, it's a
 * strong candidate root cause for the hang (a live queue's semaphores get
 * deleted out from under it, corrupting whichever DmaMgr_RequestSync call is
 * still using that stack address). Remove once resolved. */
void PspDebugLogQueueEvict(void* evictedMq, void* newMq) {
    char msg[64];
    int len = sprintf(msg, "EVICT evicted=%p new=%p\n", evictedMq, newMq);
    SceUID fd = sceIoOpen("ms0:/queue_evict.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaSyncCaller(unsigned int vrom, unsigned int size, void* retAddr) {
    char msg[80];
    int len = sprintf(msg, "sync vrom=%08x size=%u called from %p\n", vrom, size, retAddr);
    SceUID fd = sceIoOpen("ms0:/dma_sync_callers.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

static unsigned int sDmaReqCounter = 0;

void PspDebugLogDmaRequest(unsigned int vrom, unsigned int size, void* ram) {
    char msg[96];
    int len;
    SceUID fd;

    sDmaReqCounter++;
    len = sprintf(msg, "#%u vrom=%08x size=%u ram=%p\n", sDmaReqCounter, vrom, size, ram);
    fd = sceIoOpen("ms0:/dma_requests.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogCheckpoint(const char* name) {
    char msg[96];
    int len = sprintf(msg, "%s dmaReqCount=%u\n", name, sDmaReqCounter);
    SceUID fd = sceIoOpen("ms0:/checkpoints.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaAlignErr(unsigned int vrom, unsigned int size, unsigned int iterVromStart,
                             unsigned int iterVromEnd) {
    char msg[128];
    int len = sprintf(msg, "vrom=%08x size=%u vrom+size=%08x iterStart=%08x iterEnd=%08x\n", vrom, size,
                       vrom + size, iterVromStart, iterVromEnd);
    SceUID fd = sceIoOpen("ms0:/dma_align_err.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogVtxFixup(unsigned int marker, unsigned int numv, unsigned int vbidx, unsigned int vp) {
    char msg[128];
    int len = sprintf(msg, "marker=%08x numv=%u vbidx=%u vp=%08x\n", marker, numv, vbidx, vp);
    SceUID fd = sceIoOpen("ms0:/vtx_fixup.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogBgCheck(int stage, unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
    char msg[96];
    int len = sprintf(msg, "stage=%d a=%u b=%u c=%u d=%u\n", stage, a, b, c, d);
    SceUID fd = sceIoOpen("ms0:/bgcheck.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogRaw(const char* msg, int len) {
    SceUID fd = sceIoOpen("ms0:/raw_log.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}
