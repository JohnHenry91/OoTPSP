/* TEMPORARY diagnostic for Phase 2's Play_SpawnScene bring-up. Remove once
 * diagnosed -- see src/code/z_play.c's TARGET_PSP block around
 * Play_SpawnScene. Deliberately uses plain C types (not ultra64.h's u8/u32/
 * etc) to avoid conflicting with the PSP SDK headers' own typedefs of the
 * same names. */
#include <stdint.h>
#include <pspiofilemgr.h>
#include <stdio.h>

/* Master switch for every diagnostic in this file (2026-08-02).
 *
 * All of these do synchronous sceIoOpen/sceIoWrite/sceIoClose file I/O, from
 * ~82 call sites scattered through the game code. Two independent reasons to
 * keep this off by default now:
 *
 * 1. A hard crash was traced directly into this file. PPSSPP reported
 *    "Jump to invalid address: 0d9370e8  PC 08a598bc  LR 0886573c"; LR
 *    resolves to the instruction immediately after `jal sceIoOpen` inside
 *    PspDebugLogBgCheck, and the words the JIT then choked on ("Invalid
 *    instruction 7478742e/72726520" = ".txt"/" err") are this file's own
 *    filename/error string constants (module offsets 0xc304a/0xc3800/0xc3834)
 *    being executed as code -- i.e. control flow went through this logging
 *    path and off the rails, rather than any game data being corrupted (the
 *    animation-data bytes at the faulting PC were verified byte-for-byte
 *    against the built ELF and are intact).
 * 2. Since DmaMgr_RequestAsync was made synchronous on TARGET_PSP
 *    (psp/src/boot/z_std_dma.c), the game's own asset streaming issues sceIo
 *    reads on the *same* thread that these log calls run on, so the two can
 *    now interleave/re-enter in ways they never did when DMA had its own
 *    thread.
 *
 * Set to 1 to re-enable when a specific diagnostic is actually needed; the
 * `fd < 0` path every function already has makes them clean no-ops when off. */
#define PSP_DEBUG_LOG_ENABLED 0

/* Collision probe -- NOT file I/O (see the master switch above, which is off
 * for good reason). Plain globals, read live over PPSSPP's WebSocket debugger
 * the same way gPspI4Probe / gPspBgProbe* are; see /home/henry/oot_col.py.
 *
 * Written by Scene_CommandCollisionHeader. "Link falls through the world while
 * the geometry still draws" (Death Mountain Trail) means the collision mesh is
 * empty or was never installed, and these are the numbers that tell which:
 *
 *   [0] colHeader pointer as the scene command gave it (pre-relocation)
 *   [1] numVertices          [2] numPolygons
 *   [3] relocated polyList   [4] relocated vtxList
 *   [5] minBounds.y          [6] maxBounds.y
 *   [7] call count -- more than one per scene load means the command list is
 *       being walked past its end, which is its own bug
 */
unsigned int gPspColProbe[8];

/* Transition-actor (door) probe, same channel as gPspColProbe.
 *   [0] list pointer   [1] count
 *   [2..5] entry 0: id, pos.x, pos.y, pos.z   (sign-extended)
 *   [6] entry 0 rotY   [7] entry 0 params
 * market_day's first entry is the ground truth to compare against:
 * id=ACTOR_EN_DOOR, pos=(-482, 0, 615), rotY=-0x8000, params=0x028D. */
unsigned int gPspTransProbe[8];

static SceUID PspDebugLogOpen(const char* path) {
#if PSP_DEBUG_LOG_ENABLED
    return sceIoOpen(path, PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
#else
    (void)path;
    return -1;
#endif
}

void PspDebugLogPlaySpawn(int sceneId, int spawn, void* sceneSegment, unsigned int checkVal) {
    char msg[96];
    int len = sprintf(msg, "check=%08x sceneId=%d spawn=%d sceneSegment=%p\n", checkVal, sceneId, spawn,
                       sceneSegment);
    SceUID fd = PspDebugLogOpen("ms0:/play_spawn.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogKeepObject(unsigned int vromStart, unsigned int vromEnd, void* slot0Segment, void* spaceEnd) {
    char msg[128];
    int len = sprintf(msg, "keepObj vromStart=%08x vromEnd=%08x slot0Segment=%p spaceEnd=%p\n", vromStart, vromEnd,
                       slot0Segment, spaceEnd);
    SceUID fd = PspDebugLogOpen("ms0:/keep_obj.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogKeepObject2(void* mainKeepSegment, unsigned int gSegments4) {
    char msg[96];
    int len = sprintf(msg, "keepObj2 mainKeepSegment=%p gSegments4=%08x\n", mainKeepSegment, gSegments4);
    SceUID fd = PspDebugLogOpen("ms0:/keep_obj.txt");
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
    SceUID fd = PspDebugLogOpen("ms0:/col_header.txt");
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
    SceUID fd = PspDebugLogOpen("ms0:/scene_table.txt");
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
    SceUID fd = PspDebugLogOpen("ms0:/file_load.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaTest(int ret, unsigned int checksum, int passed) {
    char msg[96];
    int len = sprintf(msg, "dmaTestRet=%d checksum=%08x passed=%d\n", ret, checksum, passed);
    SceUID fd = PspDebugLogOpen("ms0:/dma_test.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogSceneCmd(void* addr, unsigned int code, unsigned int data1, unsigned int data2) {
    char msg[96];
    int len = sprintf(msg, "addr=%p code=%u data1=%02x data2=%04x\n", addr, code, data1, data2);
    SceUID fd = PspDebugLogOpen("ms0:/scene_cmd.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaCaller(void* retAddr) {
    char msg[48];
    int len = sprintf(msg, "  called from %p\n", retAddr);
    SceUID fd = PspDebugLogOpen("ms0:/dma_requests.txt");
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
    SceUID fd = PspDebugLogOpen("ms0:/queue_evict.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogDmaSyncCaller(unsigned int vrom, unsigned int size, void* retAddr) {
    char msg[80];
    int len = sprintf(msg, "sync vrom=%08x size=%u called from %p\n", vrom, size, retAddr);
    SceUID fd = PspDebugLogOpen("ms0:/dma_sync_callers.txt");
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
    fd = PspDebugLogOpen("ms0:/dma_requests.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogCheckpoint(const char* name) {
    char msg[96];
    int len = sprintf(msg, "%s dmaReqCount=%u\n", name, sDmaReqCounter);
    SceUID fd = PspDebugLogOpen("ms0:/checkpoints.txt");
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
    SceUID fd = PspDebugLogOpen("ms0:/dma_align_err.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogVtxFixup(unsigned int marker, unsigned int numv, unsigned int vbidx, unsigned int vp) {
    char msg[128];
    int len = sprintf(msg, "marker=%08x numv=%u vbidx=%u vp=%08x\n", marker, numv, vbidx, vp);
    SceUID fd = PspDebugLogOpen("ms0:/vtx_fixup.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogBgCheck(int stage, unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
    char msg[96];
    int len = sprintf(msg, "stage=%d a=%u b=%u c=%u d=%u\n", stage, a, b, c, d);
    SceUID fd = PspDebugLogOpen("ms0:/bgcheck.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}

void PspDebugLogRaw(const char* msg, int len) {
    SceUID fd = PspDebugLogOpen("ms0:/raw_log.txt");
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
}
