#include "sys_cfb.h"
#include "ultra64.h"
#include "versions.h"

#pragma increment_block_number "gc-eu:128 gc-eu-mq:128 gc-jp:128 gc-jp-ce:128 gc-jp-mq:128 gc-us:128 gc-us-mq:128" \
                               "ique-cn:0 ntsc-1.0:0 ntsc-1.1:0 ntsc-1.2:0 pal-1.0:0 pal-1.1:0"

// Declared before including other headers for BSS ordering
extern uintptr_t gSegments[NUM_SEGMENTS];

#pragma increment_block_number "gc-eu:252 gc-eu-mq:252 gc-jp:252 gc-jp-ce:252 gc-jp-mq:252 gc-us:252 gc-us-mq:252" \
                               "ique-cn:252 ntsc-1.0:128 ntsc-1.1:128 ntsc-1.2:128 pal-1.0:128 pal-1.1:128"

extern struct PreNmiBuff* gAppNmiBufferPtr;
extern struct Scheduler gScheduler;
extern struct PadMgr gPadMgr;
extern struct IrqMgr gIrqMgr;

#include "libc64/malloc.h"
#include "libc64/sleep.h"
#include "libu64/rcp_utils.h"
#include "libu64/runtime.h"
#include "array_count.h"
#include "audiomgr.h"
#include "debug_arena.h"
#include "fault.h"
#include "gfx.h"
#include "idle.h"
#include "padmgr.h"
#include "prenmi_buff.h"
#include "printf.h"
#include "regs.h"
#include "segment_symbols.h"
#include "segmented_address.h"
#include "stack.h"
#include "stackcheck.h"
#include "terminal.h"
#include "translation.h"
#include "versions.h"
#if PLATFORM_N64
#include "cic6105.h"
#include "n64dd.h"
#endif
#include "debug.h"
#include "thread.h"

#pragma increment_block_number "gc-eu:128 gc-eu-mq:128 gc-jp:128 gc-jp-ce:128 gc-jp-mq:128 gc-us:128 gc-us-mq:128" \
                               "ique-cn:0 ntsc-1.0:51 ntsc-1.1:51 ntsc-1.2:51 pal-1.0:49 pal-1.1:49"

extern u8 _buffersSegmentEnd[];

s32 gScreenWidth = SCREEN_WIDTH;
s32 gScreenHeight = SCREEN_HEIGHT;
u32 gSystemHeapSize = 0;

PreNmiBuff* gAppNmiBufferPtr;
Scheduler gScheduler;
PadMgr gPadMgr;
IrqMgr gIrqMgr;
uintptr_t gSegments[NUM_SEGMENTS];

OSThread sGraphThread;
STACK(sGraphStack, 0x1800);
#if OOT_VERSION < PAL_1_0
STACK(sSchedStack, 0x400);
#else
STACK(sSchedStack, 0x600);
#endif
STACK(sAudioStack, 0x800);
STACK(sPadMgrStack, 0x500);
STACK(sIrqMgrStack, 0x500);
StackEntry sGraphStackInfo;
StackEntry sSchedStackInfo;
StackEntry sAudioStackInfo;
StackEntry sPadMgrStackInfo;
StackEntry sIrqMgrStackInfo;
AudioMgr sAudioMgr;
OSMesgQueue sSerialEventQueue;
OSMesg sSerialMsgBuf[1];

#if DEBUG_FEATURES
void Main_LogSystemHeap(void) {
    PRINTF_COLOR_GREEN();
    PRINTF(
        T("システムヒープサイズ %08x(%dKB) 開始アドレス %08x\n", "System heap size %08x (%dKB) Start address %08x\n"),
        gSystemHeapSize, gSystemHeapSize / 1024, _buffersSegmentEnd);
    PRINTF_RST();
}
#endif

void Main(void* arg) {
    IrqMgrClient irqClient;
    OSMesgQueue irqMgrMsgQueue;
    OSMesg irqMgrMsgBuf[60];
    uintptr_t systemHeapStart;
    uintptr_t fb;

    PRINTF(T("mainproc 実行開始\n", "mainproc Start running\n"));
    gScreenWidth = SCREEN_WIDTH;
    gScreenHeight = SCREEN_HEIGHT;
    gAppNmiBufferPtr = (PreNmiBuff*)osAppNMIBuffer;
    PreNmiBuff_Init(gAppNmiBufferPtr);
    Fault_Init();
#if PLATFORM_N64 && !TARGET_PSP
    func_800AD410();
    if (D_80121211 != 0) {
        systemHeapStart = (uintptr_t)_n64ddSegmentEnd;
        SysCfb_Init(1);
    } else {
        func_800AD488();
        systemHeapStart = (uintptr_t)_buffersSegmentEnd;
        SysCfb_Init(0);
    }
#else
    /* On PSP, same as any real pal-1.0 N64 cart (no 64DD hardware exists to
     * probe): always take the no-64DD path. */
    SysCfb_Init(0);
    systemHeapStart = (uintptr_t)_buffersSegmentEnd;
#endif
#if TARGET_PSP
    /* _buffersSegmentEnd/SysCfb_GetFbPtr's addresses only mean something
     * inside a real linker-placed N64 ROM segment layout, which Phase 1
     * doesn't have (no full game link yet -- see plan step 4's note on
     * segment_symbols_stub.c). Use a plain static PSP heap instead, same
     * pattern as sm64-port-psp's static main_pool. Size is a tunable
     * starting point, not final -- revisit once real scenes/actors load. */
    static u8 sPspSystemHeap[2 * 1024 * 1024] __attribute__((aligned(16)));

    systemHeapStart = (uintptr_t)sPspSystemHeap;
    fb = systemHeapStart + sizeof(sPspSystemHeap);
#else
    fb = (uintptr_t)SysCfb_GetFbPtr(0);
#endif
    gSystemHeapSize = fb - systemHeapStart;
    PRINTF(T("システムヒープ初期化 %08x-%08x %08x\n", "System heap initialization %08x-%08x %08x\n"), systemHeapStart,
           fb, gSystemHeapSize);
    Runtime_Init((void*)systemHeapStart, gSystemHeapSize);

#if DEBUG_FEATURES
    {
        void* debugHeapStart;
        u32 debugHeapSize;

        if (osMemSize >= 0x800000) {
            debugHeapStart = SysCfb_GetFbEnd();
            debugHeapSize = PHYS_TO_K0(0x600000) - (uintptr_t)debugHeapStart;
        } else {
            debugHeapSize = 0x400;
            debugHeapStart = SYSTEM_ARENA_MALLOC(debugHeapSize, "../main.c", 565);
        }

        PRINTF("debug_InitArena(%08x, %08x)\n", debugHeapStart, debugHeapSize);
        DebugArena_Init(debugHeapStart, debugHeapSize);
    }
#endif

    Regs_Init();

    R_ENABLE_ARENA_DBG = 0;

    osCreateMesgQueue(&sSerialEventQueue, sSerialMsgBuf, ARRAY_COUNT(sSerialMsgBuf));
    osSetEventMesg(OS_EVENT_SI, &sSerialEventQueue, NULL);

#if DEBUG_FEATURES
    Main_LogSystemHeap();
#endif

    osCreateMesgQueue(&irqMgrMsgQueue, irqMgrMsgBuf, ARRAY_COUNT(irqMgrMsgBuf));
    StackCheck_Init(&sIrqMgrStackInfo, sIrqMgrStack, STACK_TOP(sIrqMgrStack), 0, 0x100, "irqmgr");
    IrqMgr_Init(&gIrqMgr, STACK_TOP(sIrqMgrStack), THREAD_PRI_IRQMGR, 1);

    PRINTF(T("タスクスケジューラの初期化\n", "Initialize the task scheduler\n"));
    StackCheck_Init(&sSchedStackInfo, sSchedStack, STACK_TOP(sSchedStack), 0, 0x100, "sched");
    Sched_Init(&gScheduler, STACK_TOP(sSchedStack), THREAD_PRI_SCHED, gViConfigModeType, 1, &gIrqMgr);

#if PLATFORM_N64 && !TARGET_PSP
    /* Real cartridge CIC security-chip boot handshake -- no such hardware
     * on PSP. */
    CIC6105_AddFaultClient();
    CIC6105_RunBootTask();
#endif

    IrqMgr_AddClient(&gIrqMgr, &irqClient, &irqMgrMsgQueue);

    /* Audio (Phase 1 bring-up, see project plan): AudioMgr_Init spawns a
     * real PSP thread (same osCreateThread/osStartThread path DmaMgr
     * already uses) that registers itself with IrqMgr and is driven by
     * Graph_Update's new IrqMgr_HandleRetrace call (src/code/graph.c). */
    StackCheck_Init(&sAudioStackInfo, sAudioStack, STACK_TOP(sAudioStack), 0, 0x100, "audio");
    AudioMgr_Init(&sAudioMgr, STACK_TOP(sAudioStack), THREAD_PRI_AUDIOMGR, THREAD_ID_AUDIOMGR, &gScheduler, &gIrqMgr);

    StackCheck_Init(&sPadMgrStackInfo, sPadMgrStack, STACK_TOP(sPadMgrStack), 0, 0x100, "padmgr");
    PadMgr_Init(&gPadMgr, &sSerialEventQueue, &gIrqMgr, THREAD_ID_PADMGR, THREAD_PRI_PADMGR, STACK_TOP(sPadMgrStack));

    AudioMgr_WaitForInit(&sAudioMgr);

#if TARGET_PSP
    /* Close a boot-time double audio-heap-reset race ("kein Ton"
     * investigation): AudioMgr_WaitForInit only proves Audio_Init()/
     * Audio_InitSound() were CALLED -- the heap reset they kick off
     * (AudioHeap_ResetStep, one state per AudioThread_Update tick) only
     * actually SETTLES once retrace messages start flowing, which only
     * happens once Graph_Update begins ticking, just below. On real N64 the
     * title screen guarantees many real frames pass before any scene's own
     * SEQCMD_RESET_AUDIO_HEAP (z_scene.c's Scene_CommandSoundSettings,
     * unmodified/real) can fire; this port's dev boot skips straight to a
     * scene, so that second reset used to land while the first was still in
     * flight and permanently stomp the very first permanent-pool load
     * (Sequence_0/Soundfont_0/1 queued by Audio_InitSound never actually
     * finished allocating -- confirmed live over the PPSSPP debugger,
     * gAudioCtx.permanentPool stayed at numEntries 0 forever, silencing all
     * audio). Pump retrace manually here, before any game state (and
     * therefore any scene) can run, until the boot-time reset has genuinely
     * finished -- same self-heal spirit as PspAudioDebug_HealResetGate,
     * PSP-side only, real engine files untouched. Bounded so a genuinely
     * stuck reset can't hang boot forever; falls through and proceeds
     * exactly as before if that cap is ever hit. */
    {
        extern void IrqMgr_HandleRetrace(IrqMgr * irqMgr);
        extern AudioContext gAudioCtx;
        s32 i;

        for (i = 0; i < 64 && gAudioCtx.resetStatus != 0; i++) {
            IrqMgr_HandleRetrace(&gIrqMgr);
            Sleep_Msec(2);
        }
    }
#endif

    StackCheck_Init(&sGraphStackInfo, sGraphStack, STACK_TOP(sGraphStack), 0, 0x100, "graph");
#if TARGET_PSP
    /* Single-loop collapse (plan decision #4): Graph_ThreadEntry's own
     * while(nextOvl != NULL) loop already IS the per-game-state engine
     * loop once Graph_Update/Graph_TaskSet00's blocking waits are removed
     * (see src/code/graph.c) -- call it directly instead of spawning a
     * thread. It returns once the game truly exits (no game state left),
     * same as the real thread would terminate. */
    Graph_ThreadEntry(arg);
#else
    osCreateThread(&sGraphThread, THREAD_ID_GRAPH, Graph_ThreadEntry, arg, STACK_TOP(sGraphStack), THREAD_PRI_GRAPH);
    osStartThread(&sGraphThread);
#endif

#if OOT_VERSION >= PAL_1_0
    osSetThreadPri(NULL, THREAD_PRI_MAIN);
#endif

#if !TARGET_PSP
    while (true) {
        s16* msg = NULL;

        osRecvMesg(&irqMgrMsgQueue, (OSMesg*)&msg, OS_MESG_BLOCK);
        if (msg == NULL) {
            break;
        }
        switch (*msg) {
            case OS_SC_PRE_NMI_MSG:
                PRINTF(T("main.c: リセットされたみたいだよ\n", "main.c: Looks like it's been reset\n"));
#if OOT_VERSION < PAL_1_0
                StackCheck_Check(NULL);
#endif
                PreNmiBuff_SetReset(gAppNmiBufferPtr);
                break;
        }
    }

    PRINTF(T("mainproc 後始末\n", "mainproc Cleanup\n"));
    osDestroyThread(&sGraphThread);
    RcpUtils_Reset();
#endif
#if PLATFORM_N64 && !TARGET_PSP
    CIC6105_RemoveFaultClient();
#endif
    PRINTF(T("mainproc 実行終了\n", "mainproc End of execution\n"));
}
