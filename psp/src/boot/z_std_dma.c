/**
 * Ported from reference/oot/src/boot/z_std_dma.c. Unmodified except
 * DmaMgr_DmaRomToRam's body: real N64 hardware queues chunked transfers with
 * the OS PI Manager over the cartridge PI-bus (osEPiStartDma against
 * gCartHandle) specifically to avoid stalling audio DMA for too long. PSP
 * has no PI-bus and no cartridge — DmaMgr_RequestSync/Async's request/reply
 * message-queue handoff and DmaMgr_ThreadEntry's background thread (the
 * actual point of porting this file — see PORTING.md's libultra threading
 * section) are all unchanged; only the leaf hardware-transfer call is
 * replaced with a direct sceIo read against the .z64 shipped alongside
 * EBOOT.PBP (see include/psp_rom.h). The PI-congestion chunking loop is
 * dropped along with it — there's no real PI bus left to congest.
 *
 * pal-1.0's dmadata table is fully uncompressed (see src/dmadata/dmadata.c:
 * romEnd is hardcoded 0 for every entry in the retail/spec-generated table),
 * so DmaMgr_ProcessRequest's Yaz0-decompression branch is dead code for us —
 * ported unmodified anyway since it costs nothing extra once
 * DmaMgr_DmaRomToRam exists (src/boot/yaz0.c depends only on that).
 *
 * This file's original header comment about compressed-vs-uncompressed
 * "Virtual ROM" addressing still applies to how DmaMgr_ProcessRequest reads
 * gDmaDataTable: it's just that for pal-1.0, virtual and physical ROM
 * addresses always coincide.
 */
#include "libc/assert.h"
#include "libc64/sleep.h"
#include "libc64/sprintf.h"
#include "libu64/debug.h"
#include "array_count.h"
#include "attributes.h"
#include "carthandle.h"
#include "fault.h"
#include "idle.h"
#if PLATFORM_IQUE
#include "inflate.h"
#endif
#include "line_numbers.h"
#if PLATFORM_N64
#include "n64dd.h"
#endif
#include "printf.h"
#include "segment_symbols.h"
#include "stack.h"
#include "stackcheck.h"
#include "terminal.h"
#include "translation.h"
#if !PLATFORM_IQUE
#include "yaz0.h"
#endif
#include "dma.h"
#include "thread.h"
#include "psp_rom.h"

#pragma increment_block_number "gc-eu:0 gc-eu-mq:0 gc-jp:0 gc-jp-ce:0 gc-jp-mq:0 gc-us:0 gc-us-mq:0 ntsc-1.2:82" \
                               "pal-1.0:80 pal-1.1:80"

StackEntry sDmaMgrStackInfo;
OSMesgQueue sDmaMgrMsgQueue;
OSMesg sDmaMgrMsgBuf[32];

u32 gDmaMgrVerbose = 0;
size_t gDmaMgrDmaBuffSize = DMAMGR_DEFAULT_BUFSIZE;
u32 sDmaMgrIsRomCompressed = false;

OSThread sDmaMgrThread;
STACK(sDmaMgrStack, 0x500);

#if DEBUG_FEATURES

const char* sDmaMgrCurFileName;
s32 sDmaMgrCurFileLine;

// dmadata filenames
#define DEFINE_DMA_ENTRY(_0, nameString) nameString,

const char* sDmaMgrFileNames[] = {
#include "tables/dmadata_table.h"
};

#undef DEFINE_DMA_ENTRY

#endif

#define SET_IOMSG(ioMsg, queue, rom, ram, buffSize) \
    do {                                            \
        (ioMsg).hdr.pri = OS_MESG_PRI_NORMAL;       \
        (ioMsg).hdr.retQueue = (queue);             \
        (ioMsg).devAddr = (rom);                    \
        (ioMsg).dramAddr = (ram);                   \
        (ioMsg).size = (buffSize);                  \
    } while (0)

#if !PLATFORM_GC || DEBUG_FEATURES
/**
 * Compares `str1` and `str2`.
 *
 * @return
 *  0 if str1 and str2 are the same,
 *  -1 if the first character that does not match has a smaller value in str1 than str2,
 *  +1 if the first character that does not match has a greater value in str1 than str2
 */
s32 DmaMgr_StrCmp(const u8* str1, const u8* str2) {
    while (*str1 != '\0') {
        if (*str1 > *str2) {
            return 1;
        }
        if (*str1 < *str2) {
            return -1;
        }
        str1++;
        str2++;
    }

    if (*str2 > '\0') {
        return -1;
    }
    return 0;
}
#endif

/**
 * Transfer `size` bytes from physical ROM address `rom` to `ram`.
 *
 * PSP REWRITE: real N64 hardware queues this as one-or-more OSIoMesg PI
 * transfers via osEPiStartDma(gCartHandle, ...), chunked to
 * gDmaMgrDmaBuffSize so a large transfer can't stall the PI bus long enough
 * to starve audio DMA. PSP has no PI bus and no cartridge — there's nothing
 * for a large transfer to congest — so this just reads directly out of the
 * shipped ROM file. DmaMgr_RequestSync/Async and DmaMgr_ThreadEntry (the
 * actual message-queue handoff and background thread this file exists to
 * exercise) are unchanged and still call this exactly as before.
 */
s32 DmaMgr_DmaRomToRam(uintptr_t rom, void* ram, size_t size) {
    osInvalICache(ram, size);
    osInvalDCache(ram, size);

    PspRom_Read((uint32_t)rom, ram, size);

    osInvalICache(ram, size);
    osInvalDCache(ram, size);

    return 0;
}

/**
 * Callback function to facilitate audio DMA. Audio DMA does not use the request queue as audio data is often needed
 * very soon after the request is sent, requiring a higher priority method for enqueueing a DMA on the OS PI command
 * queue.
 *
 * @param pihandle Cartridge ROM PI Handle.
 * @param mb IO Message describing the transfer.
 * @param direction Read or write. (Only read is allowed)
 * @return 0 if the IO Message was successfully put on the OS PI command queue, < 0 otherwise
 */
s32 DmaMgr_AudioDmaHandler(OSPiHandle* pihandle, OSIoMesg* mb, s32 direction) {
    s32 ret;

    ASSERT(pihandle == gCartHandle, "pihandle == carthandle", "../z_std_dma.c", 530);
    ASSERT(direction == OS_READ, "direction == OS_READ", "../z_std_dma.c", 531);
    ASSERT(mb != NULL, "mb != NULL", "../z_std_dma.c", 532);

#if PLATFORM_N64
    if (D_80121212) {
        while (D_80121214) {
            Sleep_Msec(1000);
        }
    }
#endif

    if (gDmaMgrVerbose == 10) {
        PRINTF(T("%10lld サウンドＤＭＡ %08x %08x %08x (%d)\n", "%10lld Sound DMA %08x %08x %08x (%d)\n"),
               OS_CYCLES_TO_USEC(osGetTime()), mb->dramAddr, mb->devAddr, mb->size, MQ_GET_COUNT(&gPiMgrCmdQueue));
    }

    ret = osEPiStartDma(pihandle, mb, direction);
    if (ret != 0) {
        PRINTF("OOPS!!\n");
    }
    return ret;
}

/**
 * DMA read from disk drive. Blocks the current thread until DMA completes.
 *
 * @param ram RAM address to write data to.
 * @param rom ROM address to read from.
 * @param size Size of transfer.
 */
void DmaMgr_DmaFromDriveRom(void* ram, uintptr_t rom, size_t size) {
#if PLATFORM_N64
    s32 pad;
#endif
    OSPiHandle* handle = osDriveRomInit();
    OSMesgQueue queue;
    OSMesg msg;
    OSIoMesg ioMsg;

    osInvalICache(ram, size);
    osInvalDCache(ram, size);
    osCreateMesgQueue(&queue, &msg, 1);

    SET_IOMSG(ioMsg, &queue, rom, ram, size);
    handle->transferInfo.cmdType = 2;

    osEPiStartDma(handle, &ioMsg, OS_READ);
    osRecvMesg(&queue, NULL, OS_MESG_BLOCK);
}

#if DEBUG_FEATURES
/**
 * DMA error encountered, print error messages and bring up the crash screen.
 *
 * @param req DMA Request causing the error.
 * @param filename DMA data filename associated with the operation that errored.
 * @param errorName Error name string.
 * @param errorDesc Error description string.
 *
 * This function does not return.
 */
NORETURN void DmaMgr_Error(DmaRequest* req, const char* filename, const char* errorName, const char* errorDesc) {
    uintptr_t vrom = req->vromAddr;
    void* ram = req->dramAddr;
    size_t size = req->size;
    char buff1[80];
    char buff2[80];

    PRINTF("%c", BEL);
    PRINTF_COLOR_RED();
    PRINTF(T("DMA致命的エラー(%s)\nROM:%X RAM:%X SIZE:%X %s\n", "DMA Fatal Error (%s)\nROM:%X RAM:%X SIZE:%X %s\n"),
           errorDesc != NULL ? errorDesc : (errorName != NULL ? errorName : "???"), vrom, ram, size,
           filename != NULL ? filename : "???");

    if (req->filename != NULL) { // Source file name that issued the DMA request
        PRINTF("DMA ERROR: %s %d", req->filename, req->line);
    } else if (sDmaMgrCurFileName != NULL) {
        PRINTF("DMA ERROR: %s %d", sDmaMgrCurFileName, sDmaMgrCurFileLine);
    }

    PRINTF_RST();

    if (req->filename != NULL) {
        sprintf(buff1, "DMA ERROR: %s %d", req->filename, req->line);
    } else if (sDmaMgrCurFileName != NULL) {
        sprintf(buff1, "DMA ERROR: %s %d", sDmaMgrCurFileName, sDmaMgrCurFileLine);
    } else {
        sprintf(buff1, "DMA ERROR: %s", errorName != NULL ? errorName : "???");
    }

    sprintf(buff2, "%07X %08X %X %s", vrom, ram, size, filename != NULL ? filename : "???");
    Fault_AddHungupAndCrashImpl(buff1, buff2);
}

#define DMA_ERROR(req, filename, errorName, errorDesc, file, line) DmaMgr_Error(req, filename, errorName, errorDesc)
#else
#define DMA_ERROR(req, filename, errorName, errorDesc, file, line) Fault_AddHungupAndCrash(file, line)
#endif

#if PLATFORM_GC
/**
 * Searches the filesystem for the entry containing the address `vrom`. Retrieves the name of this entry from
 * the array of file names.
 *
 * @param vrom Virtual ROM location
 * @return Pointer to associated filename
 */
const char* DmaMgr_FindFileName(uintptr_t vrom) {
#if DEBUG_FEATURES
    DmaEntry* iter = gDmaDataTable;
    const char** name = sDmaMgrFileNames;

    while (iter->file.vromEnd != 0) {
        if (vrom >= iter->file.vromStart && vrom < iter->file.vromEnd) {
            return *name;
        }

        iter++;
        name++;
    }
    //! @bug Since there is no return, in case the file isn't found, the return value will be a pointer to the end
    // of gDmaDataTable
#ifdef AVOID_UB
    return "";
#endif
#else
    return NULL;
#endif
}
#endif

const char* DmaMgr_GetFileName(uintptr_t vrom) {
#if PLATFORM_GC && DEBUG_FEATURES
    const char* ret = DmaMgr_FindFileName(vrom);

    if (ret == NULL) {
        return "(unknown)";
    }

    if (DmaMgr_StrCmp((const u8*)ret, (const u8*)"kanji") == 0 ||
        DmaMgr_StrCmp((const u8*)ret, (const u8*)"link_animetion") == 0) {
        // This check may be related to these files being too large to be loaded all at once, however a NULL filename
        // does not prevent them from being loaded.
        return NULL;
    }
    return ret;
#elif PLATFORM_GC
    return "";
#else
    return "??";
#endif
}

void DmaMgr_ProcessRequest(DmaRequest* req) {
    uintptr_t vrom = req->vromAddr;
    void* ram = req->dramAddr;
    size_t size = req->size;
    uintptr_t romStart;
    size_t romSize;
    u8 found = false;
    DmaEntry* iter;
    UNUSED_NDEBUG const char* filename;
    s32 i = 0;

#if DEBUG_FEATURES
    // Get the filename (for debugging)
    filename = DmaMgr_GetFileName(vrom);
#elif PLATFORM_GC
    // An unused empty string is defined in .rodata of GameCube retail builds, suggesting it was used near here.
    filename = "";
#endif

    // Iterate through the DMA data table until the region containing the vrom address for this request is found
    iter = gDmaDataTable;
    while (iter->file.vromEnd != 0) {
        if (vrom >= iter->file.vromStart && vrom < iter->file.vromEnd) {
            // Found the region this request falls into

            if (0) {
#if !PLATFORM_GC
                // Based on the MM Debug ROM, these strings are part of the condition for the empty if statement below
                if (DmaMgr_StrCmp((const u8*)"", (const u8*)"kanji") != 0 &&
                    DmaMgr_StrCmp((const u8*)"", (const u8*)"link_animetion") != 0)
#endif
                {
                    // The string is defined in .rodata of debug builds but not used, suggesting a debug print is here
                    // but was optimized out in some way.
                    PRINTF("DMA ROM:%08X RAM:%08X SIZE:%08X %s\n", vrom, ram, size, filename);
                }
            }

            if (iter->romEnd == 0) {
                // romEnd of 0 indicates that the file is uncompressed. Files that are stored uncompressed can have
                // only part of their content loaded into RAM, so DMA only the requested region.

                if (iter->file.vromEnd < vrom + size) {
                    // Error, vrom + size ends up in a different file than it started in
#if TARGET_PSP
                    {
                        extern void PspDebugLogDmaAlignErr(unsigned int vrom, unsigned int size,
                                                            unsigned int iterVromStart, unsigned int iterVromEnd);
                        PspDebugLogDmaAlignErr((unsigned int)vrom, (unsigned int)size,
                                               (unsigned int)iter->file.vromStart, (unsigned int)iter->file.vromEnd);
                    }
#endif

                    DMA_ERROR(req, filename, "Segment Alignment Error",
                              T("セグメント境界をまたがってＤＭＡ転送することはできません",
                                "DMA transfers cannot cross segment boundaries"),
                              "../z_std_dma.c", LN3(575, 578, 595, 726));
                }

                DmaMgr_DmaRomToRam(iter->romStart + vrom - iter->file.vromStart, ram, size);
                found = true;

                if (0) {
                    PRINTF("No Press ROM:%08X RAM:%08X SIZE:%08X\n", vrom, ram, size);
                }
            } else {
                // File is compressed. Files that are stored compressed must be loaded into RAM all at once.

                romSize = iter->romEnd - iter->romStart;
                romStart = iter->romStart;

                if (iter->file.vromStart != vrom) {
                    // Error, requested vrom is not the start of a file

                    DMA_ERROR(req, filename, "Can't Transfer Segment",
                              T("圧縮されたセグメントの途中からはＤＭＡ転送することはできません",
                                "DMA transfer cannot be performed from the middle of a compressed segment"),
                              "../z_std_dma.c", LN3(595, 598, 615, 746));
                }

                if (size != iter->file.vromEnd - iter->file.vromStart) {
                    // Error, only part of the file was requested

                    DMA_ERROR(req, filename, "Can't Transfer Segment",
                              T("圧縮されたセグメントの一部だけをＤＭＡ転送することはできません",
                                "It is not possible to DMA only part of a compressed segment"),
                              "../z_std_dma.c", LN3(601, 604, 621, 752));
                }

                // Reduce the thread priority and decompress the file, the decompression routine handles the DMA
                // in chunks. Restores the thread priority when done.
                osSetThreadPri(NULL, THREAD_PRI_DMAMGR_LOW);

#if !PLATFORM_IQUE
                Yaz0_Decompress(romStart, ram, romSize);
#else
                gzip_decompress(romStart, ram, romSize);
#endif

                osSetThreadPri(NULL, THREAD_PRI_DMAMGR);
                found = true;

                if (0) {
                    PRINTF("   Press ROM:%X RAM:%X SIZE:%X\n", vrom, ram, size);
                }
            }
            break;
        }

#if !PLATFORM_GC
        if (i != 0) {
            i += 4;
        }
#endif

        iter++;
    }

    if (!found) {
        // Requested region was not found in the filesystem

        if (sDmaMgrIsRomCompressed) {
            // Error, rom is compressed so DMA may only be requested within the filesystem bounds

            DMA_ERROR(req, NULL, "DATA DON'T EXIST",
                      T("該当するデータが存在しません", "Corresponding data does not exist"), "../z_std_dma.c",
                      LN3(621, 624, 641, 771));
            return;
        } else {
            // ROM is uncompressed, allow arbitrary DMA even if the region is not marked in the filesystem
            romStart = vrom;
            DmaMgr_DmaRomToRam(romStart, ram, size);

            if (0) {
                PRINTF(T("No Press ROM:%08X RAM:%08X SIZE:%08X (非公式)\n",
                         "No Press ROM:%08X RAM:%08X SIZE:%08X (informal)\n"),
                       vrom, ram, size);
            }
        }
    }
}

void DmaMgr_ThreadEntry(void* arg) {
    OSMesg msg;
    DmaRequest* req;

    PRINTF(T("ＤＭＡマネージャスレッド実行開始\n", "DMA manager thread execution start\n"));

    while (true) {
        // Wait for DMA Requests to arrive from other threads
        osRecvMesg(&sDmaMgrMsgQueue, &msg, OS_MESG_BLOCK);
        req = (DmaRequest*)msg;
        if (req == NULL) {
            break;
        }

        if (0) {
            PRINTF(T("ＤＭＡ登録受付 dmap=%08x\n", "DMA registration acceptance dmap=%08x\n"), req);
        }

        // Process the DMA request
        DmaMgr_ProcessRequest(req);

        // Notify the sender that the request has been processed
        if (req->notifyQueue != NULL) {
#if TARGET_PSP
            /* Real N64 libultra uses OS_MESG_NOBLOCK here because on real
             * hardware the receiver's queue (almost always freshly created,
             * capacity 1, for a synchronous DmaMgr_RequestSync wait) can
             * never legitimately be full at this point. On this port,
             * os_mesg.c's OSMesgQueue is backed by real PSP semaphores keyed
             * by the OSMesgQueue's address (see that file's header comment)
             * -- if this notify's semFree poll ever loses a race (e.g. right
             * after a queue at a reused stack address gets torn down and
             * recreated), OS_MESG_NOBLOCK silently drops the completion
             * message entirely, and a synchronous caller blocked in
             * osRecvMesg(..., OS_MESG_BLOCK) then hangs forever with no
             * fault -- a strong match for this port's still-unresolved
             * "low-CPU hang, no crash" heisenbug (see project memory).
             * Blocking here is safe: this is the only sender for `req->
             * notifyQueue`, and the receiver's whole point is to drain it
             * right after, so there's no deadlock risk, just occasionally
             * waiting a negligible instant instead of silently losing the
             * wakeup. */
            osSendMesg(req->notifyQueue, req->notifyMsg, OS_MESG_BLOCK);
#else
            osSendMesg(req->notifyQueue, req->notifyMsg, OS_MESG_NOBLOCK);
#endif
            if (0) {
                PRINTF("osSendMesg: dmap=%08x, mq=%08x, m=%08x \n", req, req->notifyQueue, req->notifyMsg);
            }
        }
    }

    PRINTF(T("ＤＭＡマネージャスレッド実行終了\n", "DMA manager thread execution end\n"));
}

/**
 * Submit an asynchronous DMA request. Unlike other DMA requests, this will not block the current thread. Data arrival
 * is not immediate however, ensure that the request has completed by awaiting a message sent to `queue` when the DMA
 * operation has completed.
 *
 * @param req DMA request structure, filled out internally.
 * @param ram Location in DRAM for data to be written.
 * @param vrom Virtual ROM location for data to be read.
 * @param size Transfer size.
 * @param queue Message queue to notify with `msg` once the transfer is complete.
 * @param msg Message to send to `queue` once the transfer is complete.
 * @return 0
 */
s32 DmaMgr_RequestAsync(DmaRequest* req, void* ram, uintptr_t vrom, size_t size, u32 unk, OSMesgQueue* queue,
                        OSMesg msg) {
    static s32 sDmaMgrQueueFullLogged = 0;

#if PLATFORM_IQUE
    PRINTF("dmacopy_bg(%x, %x, %x, %x, %x, %x, %x)\n", req, ram, vrom, size, unk, queue, msg);
#endif

#if DEBUG_FEATURES
    if ((ram == NULL) || (osMemSize < OS_K0_TO_PHYSICAL(ram) + size) || (vrom & 1) || (vrom > 0x4000000) ||
        (size == 0) || (size & 1)) {
        //! @bug `req` is passed to `DMA_ERROR` without rom, ram and size being set
        DMA_ERROR(req, NULL, "ILLIGAL DMA-FUNCTION CALL", T("パラメータ異常です", "Parameter error"), "../z_std_dma.c",
                  UNK_LINE);
    }
#endif

#if PLATFORM_N64
    if ((B_80121220 != NULL) && (B_80121220->unk_70 != NULL)) {
        if (B_80121220->unk_70(req, ram, vrom, size, unk, queue, msg) != 0) {
            return 0;
        }
    }
#endif

    req->vromAddr = vrom;
    req->dramAddr = ram;
    req->size = size;
    req->unk_14 = 0;
    req->notifyQueue = queue;
    req->notifyMsg = msg;

#if DEBUG_FEATURES
    if (1 && (sDmaMgrQueueFullLogged == 0) && MQ_IS_FULL(&sDmaMgrMsgQueue)) {
        sDmaMgrQueueFullLogged++;
        PRINTF("%c", BEL);
        PRINTF_COLOR_RED();
        PRINTF(T("dmaEntryMsgQが一杯です。キューサイズの再検討をおすすめします。",
                 "dmaEntryMsgQ is full. Reconsider your queue size."));
        LOG_NUM("(sizeof(dmaEntryMsgBufs) / sizeof(dmaEntryMsgBufs[0]))", ARRAY_COUNT(sDmaMgrMsgBuf), "../z_std_dma.c",
                952);
        PRINTF_RST();
    }
#endif

    osSendMesg(&sDmaMgrMsgQueue, (OSMesg)req, OS_MESG_BLOCK);
    return 0;
}

/**
 * Submit a synchronous DMA request. This will block the current thread until the requested transfer is complete. Data
 * is immediately available as soon as this function returns.
 *
 * @param ram Location in DRAM for data to be written.
 * @param vrom Virtual ROM location for data to be read.
 * @param size Transfer size.
 * @return 0
 */
s32 DmaMgr_RequestSync(void* ram, uintptr_t vrom, size_t size) {
    DmaRequest req;
    OSMesgQueue queue;
    OSMesg msg;
    s32 ret;

    osCreateMesgQueue(&queue, &msg, 1);
    ret = DmaMgr_RequestAsync(&req, ram, vrom, size, 0, &queue, NULL);
    if (ret == -1) { // DmaMgr_RequestAsync only returns 0
        return ret;
    }

    osRecvMesg(&queue, NULL, OS_MESG_BLOCK);
    return 0;
}

void DmaMgr_Init(void) {
    const char** name;
    s32 idx;
    DmaEntry* iter;

    // DMA the dma data table to RAM
    DmaMgr_DmaRomToRam((uintptr_t)_dmadataSegmentRomStart, _dmadataSegmentStart,
                       (u32)(_dmadataSegmentRomEnd - _dmadataSegmentRomStart));

#if DEBUG_FEATURES
    PRINTF("dma_rom_ad[]\n");

    name = sDmaMgrFileNames;
    iter = gDmaDataTable;
    idx = 0;

    // Check if the ROM is compressed (romEnd not 0)
    sDmaMgrIsRomCompressed = false;
    while (iter->file.vromEnd != 0) {
        if (iter->romEnd != 0) {
            sDmaMgrIsRomCompressed = true;
        }

        PRINTF("%3d %08x %08x %08x %08x %08x %c %s\n", idx, iter->file.vromStart, iter->file.vromEnd, iter->romStart,
               iter->romEnd,
               (iter->romEnd != 0) ? iter->romEnd - iter->romStart : iter->file.vromEnd - iter->file.vromStart,
               (((iter->romEnd != 0) ? iter->romEnd - iter->romStart : 0) > 0x10000) ? '*' : ' ', name ? *name : "");

        idx++;
        iter++;

        if (name != NULL) {
            name++;
        }
    }
#endif

    // Ensure that the boot segment always follows after the makerom segment.
    if ((uintptr_t)_bootSegmentRomStart != gDmaDataTable[0].file.vromEnd) {
        PRINTF("_bootSegmentRomStart(%08x) != dma_rom_ad[0].rom_b(%08x)\n", _bootSegmentRomStart,
               gDmaDataTable[0].file.vromEnd);
        //! @bug The main code file where fault.c resides is not yet loaded
        Fault_AddHungupAndCrash("../z_std_dma.c", LN3(837, 840, 859, 1055));
    }

    // Start the DMA manager
    osCreateMesgQueue(&sDmaMgrMsgQueue, sDmaMgrMsgBuf, ARRAY_COUNT(sDmaMgrMsgBuf));
    StackCheck_Init(&sDmaMgrStackInfo, sDmaMgrStack, STACK_TOP(sDmaMgrStack), 0, 0x100, "dmamgr");
    osCreateThread(&sDmaMgrThread, THREAD_ID_DMAMGR, DmaMgr_ThreadEntry, NULL, STACK_TOP(sDmaMgrStack),
                   THREAD_PRI_DMAMGR);
    osStartThread(&sDmaMgrThread);
}

/* Test-only addition (not part of reference/oot): creates the message queue
 * and starts the real, unmodified DmaMgr_ThreadEntry, skipping the parts of
 * DmaMgr_Init above that depend on a full game link's segment symbols (the
 * dmadata-table-reload-from-ROM and the boot-segment sanity check — see
 * include/segment_symbols.h). Everything this calls into
 * (DmaMgr_RequestSync/Async, DmaMgr_ThreadEntry, DmaMgr_ProcessRequest,
 * DmaMgr_DmaRomToRam) is the real decomp code, unmodified except for
 * DmaMgr_DmaRomToRam's hardware-transfer leaf (see that function's
 * comment). Exists so psp-port/src/main.c can exercise the real DMA
 * thread/queue machinery against src/dmadata_test_table.c's hand-built
 * table before the whole game is linked. */
void DmaMgr_InitForTest(void) {
    osCreateMesgQueue(&sDmaMgrMsgQueue, sDmaMgrMsgBuf, ARRAY_COUNT(sDmaMgrMsgBuf));
    StackCheck_Init(&sDmaMgrStackInfo, sDmaMgrStack, STACK_TOP(sDmaMgrStack), 0, 0x100, "dmamgr");
    osCreateThread(&sDmaMgrThread, THREAD_ID_DMAMGR, DmaMgr_ThreadEntry, NULL, STACK_TOP(sDmaMgrStack),
                   THREAD_PRI_DMAMGR);
    osStartThread(&sDmaMgrThread);
}

#if DEBUG_FEATURES
/**
 * Asynchronous DMA Request with source file and line info for debugging.
 *
 * @see DmaMgr_RequestAsync
 */
s32 DmaMgr_RequestAsyncDebug(DmaRequest* req, void* ram, uintptr_t vrom, size_t size, u32 unk5, OSMesgQueue* queue,
                             OSMesg msg, const char* file, int line) {
    req->filename = file;
    req->line = line;
    return DmaMgr_RequestAsync(req, ram, vrom, size, unk5, queue, msg);
}

/**
 * Synchronous DMA Request with source file and line info for debugging.
 *
 * @see DmaMgr_RequestSync
 */
s32 DmaMgr_RequestSyncDebug(void* ram, uintptr_t vrom, size_t size, const char* file, int line) {
    DmaRequest req;
    s32 ret;
    OSMesgQueue queue;
    OSMesg msg;
    s32 pad;

    req.filename = file;
    req.line = line;
    osCreateMesgQueue(&queue, &msg, 1);
    ret = DmaMgr_RequestAsync(&req, ram, vrom, size, 0, &queue, NULL);
    if (ret == -1) { // DmaMgr_RequestAsync only returns 0
        return ret;
    }

    osRecvMesg(&queue, NULL, OS_MESG_BLOCK);
    return 0;
}
#endif
