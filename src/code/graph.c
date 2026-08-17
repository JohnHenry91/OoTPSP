#include "libc64/malloc.h"
#include "libc64/sprintf.h"
#include "libu64/debug.h"
#include "array_count.h"
#include "buffers.h"
#include "console_logo_state.h"
#include "controller.h"
#include "gfx.h"
#include "fault.h"
#include "file_select_state.h"
#include "line_numbers.h"
#include "map_select_state.h"
#include "prenmi_buff.h"
#include "prenmi_state.h"
#include "printf.h"
#include "regs.h"
#include "setup_state.h"
#include "speed_meter.h"
#include "sys_cfb.h"
#include "sys_debug_controller.h"
#include "sys_ucode.h"
#include "terminal.h"
#include "title_setup_state.h"
#include "translation.h"
#include "ucode_disas.h"
#include "versions.h"
#include "vi_mode.h"
#include "z_game_dlftbls.h"
#include "audio.h"
#include "save.h"
#include "play_state.h"
#if TARGET_PSP
#include "gfx_pc.h"
#include "psp_frame_pace.h"
#include "padmgr.h"

#if TARGET_PSP
/* Display-buffer overrun probe; see the use site in Graph_Update. */
u32 gPspGfxArenaProbe[8] = { 0 };
#endif
/* Not part of padmgr.h's public API (only used internally by
 * PadMgr_ThreadEntry there) -- forward-declare for the direct per-frame
 * call this port needs instead. */
void PadMgr_HandleRetrace(PadMgr* padMgr);
#endif

#define GFXPOOL_HEAD_MAGIC 0x1234
#define GFXPOOL_TAIL_MAGIC 0x5678

#if TARGET_PSP
/* Graphics pool usage counters -- see the sampling site in Graph_Update.
 * Plain globals, read with the debugger, never file I/O. */
u32 gPspPoolOpaUsed;
u32 gPspPoolXluUsed;
u32 gPspPoolOvlUsed;
u32 gPspPoolWrkUsed;
u32 gPspPoolOpaMax;
u32 gPspPoolXluMax;
u32 gPspPoolOvlMax;
u32 gPspPoolWrkMax;
u32 gPspPoolOpaCap;
u32 gPspPoolXluCap;
u32 gPspPoolOvlCap;
u32 gPspPoolWrkCap;
u32 gPspPoolOverflows;
/* Tail side of polyOpa -- where GRAPH_ALLOC puts matrices/viewports. */
u32 gPspPoolOpaTailUsed;
u32 gPspPoolOpaTailMax;
s32 gPspPoolOpaHeadroom;
s32 gPspPoolOpaHeadroomMin = 0x7FFFFFFF;
/* Matrix stack depth at end of frame -- see the sampling site in Graph_Update.
 * min != max means Matrix_Push/Matrix_Pop are unbalanced across frames. */
s32 gPspMtxStackDepth;
s32 gPspMtxStackMax;
s32 gPspMtxStackMin = 0x7FFFFFFF;
#endif

#pragma increment_block_number "gc-eu:0 gc-eu-mq:0 gc-jp:0 gc-jp-ce:0 gc-jp-mq:0 gc-us:0 gc-us-mq:0 ique-cn:128" \
                               "ntsc-1.0:224 ntsc-1.1:224 ntsc-1.2:224 pal-1.0:224 pal-1.1:224"

/**
 * The time at which the previous `Graph_Update` ended.
 */
OSTime sGraphPrevUpdateEndTime;

/**
 * The time at which the previous graphics task was scheduled to run.
 */
OSTime sGraphPrevTaskTimeStart;

#if DEBUG_FEATURES
FaultClient sGraphFaultClient;

UCodeInfo D_8012D230[3] = {
#ifndef F3DEX_GBI_PL
    { UCODE_TYPE_F3DZEX, gspF3DZEX2_NoN_fifoTextStart },
#else
    { UCODE_TYPE_F3DZEX, gspF3DZEX2_NoN_PosLight_fifoTextStart },
#endif
    { UCODE_TYPE_UNK, NULL },
    { UCODE_TYPE_S2DEX, gspS2DEX2d_fifoTextStart },
};

UCodeInfo D_8012D248[3] = {
#ifndef F3DEX_GBI_PL
    { UCODE_TYPE_F3DZEX, gspF3DZEX2_NoN_fifoTextStart },
#else
    { UCODE_TYPE_F3DZEX, gspF3DZEX2_NoN_PosLight_fifoTextStart },
#endif
    { UCODE_TYPE_UNK, NULL },
    { UCODE_TYPE_S2DEX, gspS2DEX2d_fifoTextStart },
};

void Graph_FaultClient(void) {
    void* nextFb = osViGetNextFramebuffer();
    void* newFb = (SysCfb_GetFbPtr(0) != nextFb) ? SysCfb_GetFbPtr(0) : SysCfb_GetFbPtr(1);

    osViSwapBuffer(newFb);
    Fault_WaitForInput();
    osViSwapBuffer(nextFb);
}

// TODO: merge Gfx and GfxMod to make this function's arguments consistent
void UCodeDisas_Disassemble(UCodeDisas*, Gfx*);

void Graph_DisassembleUCode(Gfx* workBuf) {
    UCodeDisas disassembler;

    if (R_HREG_MODE == HREG_MODE_UCODE_DISAS && R_UCODE_DISAS_TOGGLE != 0) {
        UCodeDisas_Init(&disassembler);
        disassembler.enableLog = R_UCODE_DISAS_LOG_LEVEL;

        UCodeDisas_RegisterUCode(&disassembler, ARRAY_COUNT(D_8012D230), D_8012D230);
#ifndef F3DEX_GBI_PL
        UCodeDisas_SetCurUCode(&disassembler, gspF3DZEX2_NoN_fifoTextStart);
#else
        UCodeDisas_SetCurUCode(&disassembler, gspF3DZEX2_NoN_PosLight_fifoTextStart);
#endif

        UCodeDisas_Disassemble(&disassembler, workBuf);

        R_UCODE_DISAS_DL_COUNT = disassembler.dlCnt;
        R_UCODE_DISAS_TOTAL_COUNT =
            disassembler.tri2Cnt * 2 + disassembler.tri1Cnt + (disassembler.quadCnt * 2) + disassembler.lineCnt;
        R_UCODE_DISAS_VTX_COUNT = disassembler.vtxCnt;
        R_UCODE_DISAS_SPVTX_COUNT = disassembler.spvtxCnt;
        R_UCODE_DISAS_TRI1_COUNT = disassembler.tri1Cnt;
        R_UCODE_DISAS_TRI2_COUNT = disassembler.tri2Cnt;
        R_UCODE_DISAS_QUAD_COUNT = disassembler.quadCnt;
        R_UCODE_DISAS_LINE_COUNT = disassembler.lineCnt;
        R_UCODE_DISAS_SYNC_ERROR_COUNT = disassembler.syncErr;
        R_UCODE_DISAS_LOAD_COUNT = disassembler.loaducodeCnt;

        if (R_UCODE_DISAS_LOG_MODE == 1 || R_UCODE_DISAS_LOG_MODE == 2) {
            PRINTF("vtx_cnt=%d\n", disassembler.vtxCnt);
            PRINTF("spvtx_cnt=%d\n", disassembler.spvtxCnt);
            PRINTF("tri1_cnt=%d\n", disassembler.tri1Cnt);
            PRINTF("tri2_cnt=%d\n", disassembler.tri2Cnt);
            PRINTF("quad_cnt=%d\n", disassembler.quadCnt);
            PRINTF("line_cnt=%d\n", disassembler.lineCnt);
            PRINTF("sync_err=%d\n", disassembler.syncErr);
            PRINTF("loaducode_cnt=%d\n", disassembler.loaducodeCnt);
            PRINTF("dl_depth=%d\n", disassembler.dlDepth);
            PRINTF("dl_cnt=%d\n", disassembler.dlCnt);
        }

        UCodeDisas_Destroy(&disassembler);
    }
}

void Graph_UCodeFaultClient(Gfx* workBuf) {
    UCodeDisas disassembler;

    UCodeDisas_Init(&disassembler);
    disassembler.enableLog = true;
    UCodeDisas_RegisterUCode(&disassembler, ARRAY_COUNT(D_8012D248), D_8012D248);
#ifndef F3DEX_GBI_PL
    UCodeDisas_SetCurUCode(&disassembler, gspF3DZEX2_NoN_fifoTextStart);
#else
    UCodeDisas_SetCurUCode(&disassembler, gspF3DZEX2_NoN_PosLight_fifoTextStart);
#endif
    UCodeDisas_Disassemble(&disassembler, workBuf);
    UCodeDisas_Destroy(&disassembler);
}
#endif

/* TEMPORARY DIAGNOSTIC (2026-08-14): set to 1 to pin the engine to a single
 * graphics pool instead of alternating between gGfxPools[0] and [1] each
 * frame. The remaining symptom is a hard every-other-frame flicker, and the
 * two pools are the main thing in the pipeline that alternates at exactly that
 * rate. Safe to force on this port specifically: unlike real N64 hardware, the
 * display list here is interpreted synchronously inside Graph_ExecuteAndDraw
 * (see the TARGET_PSP block below), so the previous frame's list is fully
 * consumed before this one is built -- the double-buffering the two pools
 * exist for is unnecessary.
 *   - flicker gone   => the fault is pool-related (memory layout/overlap or
 *                       something not re-initialised per pool)
 *   - flicker stays  => pools are innocent, look elsewhere
 * Set back to 0 once the answer is known. */
#define PSP_DIAG_SINGLE_GFX_POOL 0

void Graph_InitTHGA(GraphicsContext* gfxCtx) {
#if PSP_DIAG_SINGLE_GFX_POOL && TARGET_PSP
    GfxPool* pool = &gGfxPools[0];
#else
    GfxPool* pool = &gGfxPools[gfxCtx->gfxPoolIdx & 1];
#endif

    pool->headMagic = GFXPOOL_HEAD_MAGIC;
    pool->tailMagic = GFXPOOL_TAIL_MAGIC;
    THGA_Init(&gfxCtx->polyOpa, pool->polyOpaBuffer, sizeof(pool->polyOpaBuffer));
    THGA_Init(&gfxCtx->polyXlu, pool->polyXluBuffer, sizeof(pool->polyXluBuffer));
    THGA_Init(&gfxCtx->overlay, pool->overlayBuffer, sizeof(pool->overlayBuffer));
    THGA_Init(&gfxCtx->work, pool->workBuffer, sizeof(pool->workBuffer));

    gfxCtx->polyOpaBuffer = pool->polyOpaBuffer;
    gfxCtx->polyXluBuffer = pool->polyXluBuffer;
    gfxCtx->overlayBuffer = pool->overlayBuffer;
    gfxCtx->workBuffer = pool->workBuffer;

    //! @bug fbIdx is a signed integer that can overflow into the negatives. When compiled with a C99+ compiler or IDO,
    //! the remainder operator will yield -1 for odd negative values of fbIdx.
    //! This causes SysCfb_GetFbPtr to read beyond the bounds of an array when retrieving the framebuffer pointer, which
    //! will likely crash the game.
    //!
    //! This isn't an issue in practice. In the worst case scenario with the game operating at a consistent 60 FPS,
    //! it would take approximately 414.25 days of continuous operation for fbIdx to overflow.
    gfxCtx->curFrameBuffer = SysCfb_GetFbPtr(gfxCtx->fbIdx % 2);
    gfxCtx->unk_014 = 0;
}

GameStateOverlay* Graph_GetNextGameState(GameState* gameState) {
    void* gameStateInitFunc = GameState_GetInit(gameState);

    // Generates code to match gameStateInitFunc to a gamestate entry and returns it if found
#define DEFINE_GAMESTATE_INTERNAL(typeName, enumName) \
    if (gameStateInitFunc == typeName##_Init) {       \
        return &gGameStateOverlayTable[enumName];     \
    }
#define DEFINE_GAMESTATE(typeName, enumName, name) DEFINE_GAMESTATE_INTERNAL(typeName, enumName)
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL

    LOG_ADDRESS("game_init_func", gameStateInitFunc, "../graph.c", 696);
    return NULL;
}

void Graph_Init(GraphicsContext* gfxCtx) {
    bzero(gfxCtx, sizeof(GraphicsContext));
    gfxCtx->gfxPoolIdx = 0;
    gfxCtx->fbIdx = 0;
    gfxCtx->viMode = NULL;

#if OOT_VERSION < PAL_1_0
    gfxCtx->viFeatures = 0;
#else
    gfxCtx->viFeatures = gViConfigFeatures;
    gfxCtx->xScale = gViConfigXScale;
    gfxCtx->yScale = gViConfigYScale;
#endif

    osCreateMesgQueue(&gfxCtx->queue, gfxCtx->msgBuff, ARRAY_COUNT(gfxCtx->msgBuff));

#if DEBUG_FEATURES
    func_800D31F0();
    Fault_AddClient(&sGraphFaultClient, Graph_FaultClient, NULL, NULL);
#endif
}

void Graph_Destroy(GraphicsContext* gfxCtx) {
#if DEBUG_FEATURES
    func_800D3210();
    Fault_RemoveClient(&sGraphFaultClient);
#endif
}

void Graph_TaskSet00(GraphicsContext* gfxCtx) {
#if DEBUG_FEATURES
    static Gfx* sPrevTaskWorkBuffer = NULL;
#endif
    OSTask_t* task = &gfxCtx->task.list.t;
    OSScTask* scTask = &gfxCtx->task;

    gGfxTaskSentToNextReadyMinusAudioThreadUpdateTime =
        osGetTime() - sGraphPrevTaskTimeStart - gAudioThreadUpdateTimeAcc;

#if TARGET_PSP
    /* No real RCP hardware to wait on -- the PSP gfx dispatch below (see the
     * matching #if TARGET_PSP further down this function) is fully
     * synchronous, so by construction the "previous task" is always already
     * done by the time we get here. Nothing to wait for. See plan decision
     * #6. */
#else
    {
        OSTimer timer;
        OSMesg msg;

        // Schedule a message to be handled in 3 seconds, for RCP timeout
        osSetTimer(&timer, OS_USEC_TO_CYCLES(3000000), 0, &gfxCtx->queue, (OSMesg)666);

        osRecvMesg(&gfxCtx->queue, &msg, OS_MESG_BLOCK);
        osStopTimer(&timer);

        if (msg == (OSMesg)666) {
#if DEBUG_FEATURES
            PRINTF_COLOR_RED();
            PRINTF(T("RCPが帰ってきませんでした。", "RCP did not return."));
            PRINTF_RST();

            LogUtils_LogHexDump((void*)PHYS_TO_K1(SP_BASE_REG), 0x20);
            LogUtils_LogHexDump((void*)PHYS_TO_K1(DPC_BASE_REG), 0x20);
            LogUtils_LogHexDump(gGfxSPTaskYieldBuffer, sizeof(gGfxSPTaskYieldBuffer));

            SREG(6) = -1;
            if (sPrevTaskWorkBuffer != NULL) {
                R_HREG_MODE = HREG_MODE_UCODE_DISAS;
                R_UCODE_DISAS_TOGGLE = 1;
                R_UCODE_DISAS_LOG_LEVEL = 2;
                Graph_DisassembleUCode(sPrevTaskWorkBuffer);
            }
#endif

            Fault_AddHungupAndCrashImpl("RCP is HUNG UP!!", "Oh! MY GOD!!");
        }

        osRecvMesg(&gfxCtx->queue, &msg, OS_MESG_NOBLOCK);

#if DEBUG_FEATURES
        sPrevTaskWorkBuffer = gfxCtx->workBuffer;
#endif
    }
#endif

    if (gfxCtx->callback != NULL) {
        gfxCtx->callback(gfxCtx, gfxCtx->callbackParam);
    }

    {
        OSTime timeNow = osGetTime();

        if (gAudioThreadUpdateTimeStart != 0) {
            // The audio thread update is running
            // Add the time already spent to the accumulator and leave the rest for the next cycle

            gAudioThreadUpdateTimeAcc += timeNow - gAudioThreadUpdateTimeStart;
            gAudioThreadUpdateTimeStart = timeNow;
        }
        gAudioThreadUpdateTimeTotalPerGfxTask = gAudioThreadUpdateTimeAcc;
        gAudioThreadUpdateTimeAcc = 0;

        sGraphPrevTaskTimeStart = osGetTime();
    }

    task->type = M_GFXTASK;
#if TARGET_PSP
    /* Only data_ptr/data_size are ever read on PSP (by gfx_run() below) --
     * everything else here configures a real RSP microcode task
     * (boot ucode, F3DEX2 ucode blob, RSP-local stacks/output buffers) that
     * never gets dispatched to real hardware, so skip it entirely. Avoids
     * needing to link the RSP microcode blobs at all (see sys_ucode.c). */
    task->data_ptr = (u64*)gfxCtx->workBuffer;

    OPEN_DISPS(gfxCtx, "../graph.c", 828);
    task->data_size = (uintptr_t)WORK_DISP - (uintptr_t)gfxCtx->workBuffer;
    CLOSE_DISPS(gfxCtx, "../graph.c", 830);
#else
    task->flags = OS_SC_DRAM_DLIST;
    task->ucode_boot = SysUcode_GetUCodeBoot();
    task->ucode_boot_size = SysUcode_GetUCodeBootSize();
    task->ucode = SysUcode_GetUCode();
    task->ucode_data = SysUcode_GetUCodeData();
    task->ucode_size = SP_UCODE_SIZE;
    task->ucode_data_size = SP_UCODE_DATA_SIZE;
    task->dram_stack = gGfxSPTaskStack;
    task->dram_stack_size = sizeof(gGfxSPTaskStack);
    task->output_buff = gGfxSPTaskOutputBuffer;
    task->output_buff_size = gGfxSPTaskOutputBuffer + ARRAY_COUNT(gGfxSPTaskOutputBuffer);
    task->data_ptr = (u64*)gfxCtx->workBuffer;

    OPEN_DISPS(gfxCtx, "../graph.c", 828);
    task->data_size = (uintptr_t)WORK_DISP - (uintptr_t)gfxCtx->workBuffer;
    CLOSE_DISPS(gfxCtx, "../graph.c", 830);

    task->yield_data_ptr = gGfxSPTaskYieldBuffer;
    task->yield_data_size = sizeof(gGfxSPTaskYieldBuffer);
#endif

    scTask->next = NULL;
    scTask->flags = OS_SC_NEEDS_RSP | OS_SC_NEEDS_RDP | OS_SC_SWAPBUFFER | OS_SC_LAST_TASK;
    if (R_GRAPH_TASKSET00_FLAGS & 1) {
        R_GRAPH_TASKSET00_FLAGS &= ~1;
        scTask->flags &= ~OS_SC_SWAPBUFFER;
        gfxCtx->fbIdx--;
    }

    scTask->msgQueue = &gfxCtx->queue;
    scTask->msg = NULL;

    {
        static CfbInfo sGraphCfbInfos[3];
        static s32 sGraphCfbInfoIdx = 0;
        CfbInfo* cfb;

        cfb = &sGraphCfbInfos[sGraphCfbInfoIdx];

        sGraphCfbInfoIdx = (sGraphCfbInfoIdx + 1) % ARRAY_COUNT(sGraphCfbInfos);
        cfb->framebuffer = gfxCtx->curFrameBuffer;
        cfb->swapBuffer = gfxCtx->curFrameBuffer;

        cfb->viMode = gfxCtx->viMode;
        cfb->viFeatures = gfxCtx->viFeatures;
#if OOT_VERSION >= PAL_1_0
        cfb->xScale = gfxCtx->xScale;
        cfb->yScale = gfxCtx->yScale;
#endif
        cfb->unk_10 = 0;
        cfb->updateRate = R_UPDATE_RATE;

        scTask->framebuffer = cfb;
    }

    gfxCtx->schedMsgQueue = &gScheduler.cmdQueue;

#if TARGET_PSP
    /* Direct hook per plan decision #6: interpret the just-built display
     * list immediately instead of enqueuing it for real RCP hardware via
     * Sched. gfxCtx->curFrameBuffer (set up by Graph_InitTHGA below via
     * SysCfb_GetFbPtr) tracks which double-buffered target this frame
     * intended to swap to -- the PSP-side main loop reads that after this
     * call to drive its own sceGu buffer swap (see psp/src/main.c). */
    gfx_start_frame();
    gfx_run((Gfx*)task->data_ptr);
    gfx_end_frame();

    /* The other half of what the skipped Sched submission did: hold this frame
     * for cfb->updateRate video fields before the loop runs the game again.
     * Without it the engine advanced once per rendered frame -- measured 30.1
     * Hz against PAL's 50/3 = 16.67 Hz, i.e. everything ran 1.81x too fast.
     * See psp/src/psp_frame_pace.c. */
    PspFramePace_Wait(R_UPDATE_RATE);
#else
    osSendMesg(&gScheduler.cmdQueue, (OSMesg)scTask, OS_MESG_BLOCK);
    Sched_Notify(&gScheduler);
#endif
}

void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState) {
    u32 problem;

    gameState->inPreNMIState = false;
    Graph_InitTHGA(gfxCtx);

#if TARGET_PSP
    /* IrqMgr's vsync-tick fan-out is bypassed for Phase 1 (Sched is a
     * no-op here, AudioMgr is stubbed out) -- PadMgr is the one subsystem
     * that still genuinely needs a once-per-frame tick, so call it
     * directly. See plan decision #4. */
    PadMgr_HandleRetrace(&gPadMgr);
#endif

#if DEBUG_FEATURES
    OPEN_DISPS(gfxCtx, "../graph.c", 966);

    gDPNoOpString(WORK_DISP++, T("WORK_DISP 開始", "WORK_DISP start"), 0);
    gDPNoOpString(POLY_OPA_DISP++, T("POLY_OPA_DISP 開始", "POLY_OPA_DISP start"), 0);
    gDPNoOpString(POLY_XLU_DISP++, T("POLY_XLU_DISP 開始", "POLY_XLU_DISP start"), 0);
    gDPNoOpString(OVERLAY_DISP++, T("OVERLAY_DISP 開始", "OVERLAY_DISP start"), 0);

    CLOSE_DISPS(gfxCtx, "../graph.c", 975);
#endif

    GameState_ReqPadData(gameState);
    GameState_Update(gameState);

#if DEBUG_FEATURES
    OPEN_DISPS(gfxCtx, "../graph.c", 987);

    gDPNoOpString(WORK_DISP++, T("WORK_DISP 終了", "WORK_DISP end"), 0);
    gDPNoOpString(POLY_OPA_DISP++, T("POLY_OPA_DISP 終了", "POLY_OPA_DISP end"), 0);
    gDPNoOpString(POLY_XLU_DISP++, T("POLY_XLU_DISP 終了", "POLY_XLU_DISP end"), 0);
    gDPNoOpString(OVERLAY_DISP++, T("OVERLAY_DISP 終了", "OVERLAY_DISP end"), 0);

    CLOSE_DISPS(gfxCtx, "../graph.c", 996);
#endif

    OPEN_DISPS(gfxCtx, "../graph.c", 999);

    gSPBranchList(WORK_DISP++, gfxCtx->polyOpaBuffer);
    gSPBranchList(POLY_OPA_DISP++, gfxCtx->polyXluBuffer);
    gSPBranchList(POLY_XLU_DISP++, gfxCtx->overlayBuffer);
    gDPPipeSync(OVERLAY_DISP++);
    gDPFullSync(OVERLAY_DISP++);
    gSPEndDisplayList(OVERLAY_DISP++);

    CLOSE_DISPS(gfxCtx, "../graph.c", 1028);

#if DEBUG_FEATURES
    if (R_HREG_MODE == HREG_MODE_PLAY && R_PLAY_ENABLE_UCODE_DISAS == 2) {
        R_HREG_MODE = HREG_MODE_UCODE_DISAS;
        R_UCODE_DISAS_TOGGLE = -1;
        R_UCODE_DISAS_LOG_LEVEL = R_PLAY_UCODE_DISAS_LOG_LEVEL;
    }

    if (R_HREG_MODE == HREG_MODE_UCODE_DISAS && R_UCODE_DISAS_TOGGLE != 0) {
        static FaultClient sGraphUcodeFaultClient;

        if (R_UCODE_DISAS_LOG_MODE == 3) {
            Fault_AddClient(&sGraphUcodeFaultClient, Graph_UCodeFaultClient, gfxCtx->workBuffer, "do_count_fault");
        }

        Graph_DisassembleUCode(gfxCtx->workBuffer);

        if (R_UCODE_DISAS_LOG_MODE == 3) {
            Fault_RemoveClient(&sGraphUcodeFaultClient);
        }

        if (R_UCODE_DISAS_TOGGLE < 0) {
            LogUtils_LogHexDump((void*)PHYS_TO_K1(SP_BASE_REG), 0x20);
            LogUtils_LogHexDump((void*)PHYS_TO_K1(DPC_BASE_REG), 0x20);
        }

        if (R_UCODE_DISAS_TOGGLE < 0) {
            R_UCODE_DISAS_TOGGLE = 0;
        }
    }
#endif

    problem = false;

    {
#if PSP_DIAG_SINGLE_GFX_POOL && TARGET_PSP
        /* Must match Graph_InitTHGA's choice. Pinning only the init site (as
         * the first version of this diagnostic did) makes the guard word get
         * written to pool 0 and then read back from pool 1 on alternate
         * frames, which fails `../graph.c:951` immediately and looks exactly
         * like real pool corruption. That false positive is what produced the
         * earlier conclusion that "the two pools are load-bearing" -- they may
         * well be, but this experiment never showed it. */
        GfxPool* pool = &gGfxPools[0];
#else
        GfxPool* pool = &gGfxPools[gfxCtx->gfxPoolIdx & 1];
#endif

#if TARGET_PSP
        /* Matrix stack depth at end of frame.
         *
         * Last hypothesis standing for the alternating-view bug. Everything
         * else has been measured and cleared: Link's position, the camera
         * (play->view is byte-stable), all four projection loads, the geometry
         * and display list, both ends of the graphics pool (6% used, no
         * overflow), stray writes to the pool guard words (a PPSSPP memory
         * watchpoint saw only Graph_InitTHGA), the Mtx fixed-point packing,
         * and pool alternation itself (the flicker survives a single pool once
         * the diagnostic is applied consistently at BOTH sites).
         *
         * What remains is that 43 of 45 modelview loads produce a different
         * matrix every frame. Matrix_NewMtx converts whatever sCurrentMatrix
         * points at, so an unbalanced Matrix_Push/Matrix_Pop leaves the stack
         * pointer somewhere else at the same point in the next frame, and
         * every matrix built after that comes from a different stack level --
         * which is exactly the observed signature. Matrix_Init resets the
         * pointer only once, at scene load, not per frame, so drift persists.
         *
         * depth should be identical at the same point in every frame. Anything
         * else, and this is the bug. */
        {
            extern MtxF* sMatrixStack;
            extern MtxF* sCurrentMatrix;

            if (sMatrixStack != NULL) {
                s32 depth = (s32)(sCurrentMatrix - sMatrixStack);

                gPspMtxStackDepth = depth;
                if (depth > gPspMtxStackMax) {
                    gPspMtxStackMax = depth;
                }
                if (depth < gPspMtxStackMin) {
                    gPspMtxStackMin = depth;
                }
            }
        }

        /* Graphics pool usage, sampled at the point OoT itself checks the
         * pool's guard words.
         *
         * Why this matters: forcing a SINGLE pool
         * (PSP_DIAG_SINGLE_GFX_POOL 1) makes the game die immediately on
         * `../graph.c:951`, i.e. this very headMagic check -- the display
         * list overruns its buffer inside one frame. With the normal TWO
         * pools that same overrun lands in the *neighbouring* pool instead of
         * its own guard word, so the check stays quiet and the damage is
         * silent. But the neighbouring pool is exactly where the NEXT frame's
         * matrices live, which is the mechanism behind the every-other-frame
         * flicker: identical camera and geometry, yet corrupted modelviews on
         * alternate frames.
         *
         * So record how far each arena's head actually advanced, plus the
         * high-water mark, and compare against the buffer's real capacity.
         * `over` counting up proves the overrun directly. */
        {
            u32 opaUsed = (u32)((uintptr_t)gfxCtx->polyOpa.p - (uintptr_t)pool->polyOpaBuffer);
            u32 xluUsed = (u32)((uintptr_t)gfxCtx->polyXlu.p - (uintptr_t)pool->polyXluBuffer);
            u32 ovlUsed = (u32)((uintptr_t)gfxCtx->overlay.p - (uintptr_t)pool->overlayBuffer);
            u32 wrkUsed = (u32)((uintptr_t)gfxCtx->work.p - (uintptr_t)pool->workBuffer);

            /* THGA is TWO-headed: `p` grows up with display list commands,
             * while `d` grows DOWN from the end. GRAPH_ALLOC (include/gfx.h)
             * takes every matrix and viewport off polyOpa.d specifically:
             *
             *   gfxCtx->polyOpa.d = polyOpa.d - ALIGN16(size)
             *
             * Measuring only `p` (as the first version of this did) therefore
             * misses the allocation direction that can actually reach the
             * pool's headMagic, which sits *below* polyOpaBuffer. `d` running
             * past the buffer start is the only way to corrupt that guard
             * word from this side -- and corrupting it is exactly what the
             * single-pool experiment showed happening.
             *
             * tailUsed = how far d has descended from the buffer end.
             * headroom = bytes still between the two heads; it reaching 0 is
             * the real collision condition (THGA_IsCrash). */
            {
                uintptr_t opaEnd = (uintptr_t)pool->polyOpaBuffer + sizeof(pool->polyOpaBuffer);

                gPspPoolOpaTailUsed = (u32)(opaEnd - (uintptr_t)gfxCtx->polyOpa.d);
                if (gPspPoolOpaTailUsed > gPspPoolOpaTailMax) {
                    gPspPoolOpaTailMax = gPspPoolOpaTailUsed;
                }
                /* Signed on purpose: negative means the heads have crossed. */
                gPspPoolOpaHeadroom =
                    (s32)((uintptr_t)gfxCtx->polyOpa.d - (uintptr_t)gfxCtx->polyOpa.p);
                if (gPspPoolOpaHeadroom < gPspPoolOpaHeadroomMin) {
                    gPspPoolOpaHeadroomMin = gPspPoolOpaHeadroom;
                }
                if (gPspPoolOpaHeadroom < 0) {
                    ++gPspPoolOverflows;
                }
            }

            gPspPoolOpaUsed = opaUsed;
            gPspPoolXluUsed = xluUsed;
            gPspPoolOvlUsed = ovlUsed;
            gPspPoolWrkUsed = wrkUsed;
            gPspPoolOpaCap = (u32)sizeof(pool->polyOpaBuffer);
            gPspPoolXluCap = (u32)sizeof(pool->polyXluBuffer);
            gPspPoolOvlCap = (u32)sizeof(pool->overlayBuffer);
            gPspPoolWrkCap = (u32)sizeof(pool->workBuffer);

            if (opaUsed > gPspPoolOpaMax) {
                gPspPoolOpaMax = opaUsed;
            }
            if (xluUsed > gPspPoolXluMax) {
                gPspPoolXluMax = xluUsed;
            }
            if (ovlUsed > gPspPoolOvlMax) {
                gPspPoolOvlMax = ovlUsed;
            }
            if (wrkUsed > gPspPoolWrkMax) {
                gPspPoolWrkMax = wrkUsed;
            }
            if (opaUsed > gPspPoolOpaCap || xluUsed > gPspPoolXluCap ||
                ovlUsed > gPspPoolOvlCap || wrkUsed > gPspPoolWrkCap) {
                ++gPspPoolOverflows;
            }
        }
#endif

        if (pool->headMagic != GFXPOOL_HEAD_MAGIC) {
            //! @bug (?) : "problem = true;" may be missing
            PRINTF("%c", BEL);
            PRINTF(VT_COL(RED, WHITE) T("ダイナミック領域先頭が破壊されています\n", "Dynamic area head is destroyed\n")
                       VT_RST);
            Fault_AddHungupAndCrash("../graph.c", LN4(937, 940, 951, 1067, 1070));
        }

        if (pool->tailMagic != GFXPOOL_TAIL_MAGIC) {
            problem = true;
            PRINTF("%c", BEL);
            PRINTF(VT_COL(RED, WHITE)
                       T("ダイナミック領域末尾が破壊されています\n", "Dynamic region tail is destroyed\n") VT_RST);
            Fault_AddHungupAndCrash("../graph.c", LN4(943, 946, 957, 1073, 1076));
        }
    }

#if TARGET_PSP
    /* The pivot view faults with a wild jump AFTER every actor has drawn, and a
     * global that only ever receives small constants (gPspDrawStage) reads back
     * as 0xFFFFFFFF -- i.e. something is writing outside its buffer. These are
     * exactly the buffers that would do it: the checks below already detect the
     * overrun, but only PRINTF about it (and PRINTF goes nowhere in this port),
     * so the corruption has always been silent. Recorded here so it can be read
     * with the debugger instead. */
    gPspGfxArenaProbe[0] = THGA_IsCrash(&gfxCtx->polyOpa) ? 1 : 0;
    gPspGfxArenaProbe[1] = THGA_IsCrash(&gfxCtx->polyXlu) ? 1 : 0;
    gPspGfxArenaProbe[2] = THGA_IsCrash(&gfxCtx->overlay) ? 1 : 0;
    if (gPspGfxArenaProbe[0] | gPspGfxArenaProbe[1] | gPspGfxArenaProbe[2]) {
        gPspGfxArenaProbe[3]++;
    }
    gPspGfxArenaProbe[4] = (u32)THGA_GetRemaining(&gfxCtx->polyOpa);
    gPspGfxArenaProbe[5] = (u32)THGA_GetRemaining(&gfxCtx->polyXlu);
    gPspGfxArenaProbe[6] = (u32)THGA_GetRemaining(&gfxCtx->overlay);
#endif

    if (THGA_IsCrash(&gfxCtx->polyOpa)) {
        problem = true;
        PRINTF("%c", BEL);
        PRINTF(VT_COL(RED, WHITE) T("ゼルダ0は死んでしまった(graph_alloc is empty)\n",
                                    "Zelda 0 is dead (graph_alloc is empty)\n") VT_RST);
    }
    if (THGA_IsCrash(&gfxCtx->polyXlu)) {
        problem = true;
        PRINTF("%c", BEL);
        PRINTF(VT_COL(RED, WHITE) T("ゼルダ1は死んでしまった(graph_alloc is empty)\n",
                                    "Zelda 1 is dead (graph_alloc is empty)\n") VT_RST);
    }
    if (THGA_IsCrash(&gfxCtx->overlay)) {
        problem = true;
        PRINTF("%c", BEL);
        PRINTF(VT_COL(RED, WHITE) T("ゼルダ4は死んでしまった(graph_alloc is empty)\n",
                                    "Zelda 4 is dead (graph_alloc is empty)\n") VT_RST);
    }

    if (!problem) {
        Graph_TaskSet00(gfxCtx);
        gfxCtx->gfxPoolIdx++;
        gfxCtx->fbIdx++;
    }

#if !TARGET_PSP
    /* Audio is out of scope for Phase 1 (see plan roadmap) -- avoid pulling
     * in the whole audio subsystem just to link the engine skeleton. */
    Audio_Update();
#endif

    {
        OSTime timeNow = osGetTime();
        s32 pad;

        gRSPGfxTimeTotal = gRSPGfxTimeAcc;
        gRSPAudioTimeTotal = gRSPAudioTimeAcc;
        gRDPTimeTotal = gRDPTimeAcc;
        gRSPGfxTimeAcc = 0;
        gRSPAudioTimeAcc = 0;
        gRDPTimeAcc = 0;

        if (sGraphPrevUpdateEndTime != 0) {
            gGraphUpdatePeriod = timeNow - sGraphPrevUpdateEndTime;
        }
        sGraphPrevUpdateEndTime = timeNow;
    }

#if DEBUG_FEATURES
    if (gIsCtrlr2Valid && CHECK_BTN_ALL(gameState->input[0].press.button, BTN_Z) &&
        CHECK_BTN_ALL(gameState->input[0].cur.button, BTN_L | BTN_R)) {
        gSaveContext.gameMode = GAMEMODE_NORMAL;
        SET_NEXT_GAMESTATE(gameState, MapSelect_Init, MapSelectState);
        gameState->running = false;
    }

    if (gIsCtrlr2Valid && PreNmiBuff_IsResetting(gAppNmiBufferPtr) && !gameState->inPreNMIState) {
        PRINTF(VT_COL(YELLOW, BLACK) T("PRE-NMIによりリセットモードに移行します\n",
                                       "PRE-NMI causes the system to transition to reset mode\n") VT_RST);
        SET_NEXT_GAMESTATE(gameState, PreNMI_Init, PreNMIState);
        gameState->running = false;
    }
#endif
}

void Graph_ThreadEntry(void* arg0) {
    GraphicsContext gfxCtx;
    GameState* gameState;
    u32 size;
    GameStateOverlay* nextOvl = &gGameStateOverlayTable[GAMESTATE_SETUP];
    GameStateOverlay* ovl;

    PRINTF(T("グラフィックスレッド実行開始\n", "Start graphic thread execution\n"));
    Graph_Init(&gfxCtx);

    while (nextOvl != NULL) {
        ovl = nextOvl;
        Overlay_LoadGameState(ovl);

        size = ovl->instanceSize;
        PRINTF(T("クラスサイズ＝%dバイト\n", "Class size = %d bytes\n"), size);

        gameState = SYSTEM_ARENA_MALLOC(size, "../graph.c", 1196);

        if (gameState == NULL) {
#if DEBUG_FEATURES
            char faultMsg[0x50];

            PRINTF(T("確保失敗\n", "Failure to secure\n"));

            sprintf(faultMsg, "CLASS SIZE= %d bytes", size);
            Fault_AddHungupAndCrashImpl("GAME CLASS MALLOC FAILED", faultMsg);
#else
            Fault_AddHungupAndCrash("../graph.c", LN4(1067, 1070, 1081, 1197, 1200));
#endif
        }

        GameState_Init(gameState, ovl->init, &gfxCtx);

        while (GameState_IsRunning(gameState)) {
            Graph_Update(&gfxCtx, gameState);
        }

        nextOvl = Graph_GetNextGameState(gameState);
        GameState_Destroy(gameState);
        SYSTEM_ARENA_FREE(gameState, "../graph.c", 1227);
        Overlay_FreeGameState(ovl);
    }
    Graph_Destroy(&gfxCtx);
    PRINTF(T("グラフィックスレッド実行終了\n", "End of graphic thread execution\n"));
}

void* Graph_Alloc(GraphicsContext* gfxCtx, size_t size) {
    TwoHeadGfxArena* thga = &gfxCtx->polyOpa;

    if (HREG(59) == 1) {
        PRINTF("graph_alloc siz=%d thga size=%08x bufp=%08x head=%08x tail=%08x\n", size, thga->size, thga->start,
               thga->p, thga->d);
    }
    return THGA_AllocTail(&gfxCtx->polyOpa, ALIGN16(size));
}

void* Graph_Alloc2(GraphicsContext* gfxCtx, size_t size) {
    TwoHeadGfxArena* thga = &gfxCtx->polyOpa;

    if (HREG(59) == 1) {
        PRINTF("graph_alloc siz=%d thga size=%08x bufp=%08x head=%08x tail=%08x\n", size, thga->size, thga->start,
               thga->p, thga->d);
    }
    return THGA_AllocTail(&gfxCtx->polyOpa, ALIGN16(size));
}

#if DEBUG_FEATURES
void Graph_OpenDisps(Gfx** dispRefs, GraphicsContext* gfxCtx, const char* file, int line) {
    if (R_HREG_MODE == HREG_MODE_UCODE_DISAS && R_UCODE_DISAS_LOG_MODE != 4) {
        dispRefs[0] = gfxCtx->polyOpa.p;
        dispRefs[1] = gfxCtx->polyXlu.p;
        dispRefs[2] = gfxCtx->overlay.p;

        gDPNoOpOpenDisp(gfxCtx->polyOpa.p++, file, line);
        gDPNoOpOpenDisp(gfxCtx->polyXlu.p++, file, line);
        gDPNoOpOpenDisp(gfxCtx->overlay.p++, file, line);
    }
}

void Graph_CloseDisps(Gfx** dispRefs, GraphicsContext* gfxCtx, const char* file, int line) {
    if (R_HREG_MODE == HREG_MODE_UCODE_DISAS && R_UCODE_DISAS_LOG_MODE != 4) {
        if (dispRefs[0] + 1 == gfxCtx->polyOpa.p) {
            gfxCtx->polyOpa.p = dispRefs[0];
        } else {
            gDPNoOpCloseDisp(gfxCtx->polyOpa.p++, file, line);
        }

        if (dispRefs[1] + 1 == gfxCtx->polyXlu.p) {
            gfxCtx->polyXlu.p = dispRefs[1];
        } else {
            gDPNoOpCloseDisp(gfxCtx->polyXlu.p++, file, line);
        }

        if (dispRefs[2] + 1 == gfxCtx->overlay.p) {
            gfxCtx->overlay.p = dispRefs[2];
        } else {
            gDPNoOpCloseDisp(gfxCtx->overlay.p++, file, line);
        }
    }
}
#endif
