/* Real PSP entry point for the OoT-PSP single-loop engine (plan steps 5-7).
 * Boots the ported libultra/libu64 shim + DmaMgr, runs the same DMA smoke
 * test attempt 1 validated (still useful as a fast pre-flight check), then
 * hands off to the real decomp boot chain: Main() (src/code/main.c) inits
 * Sched/PadMgr/IrqMgr (all collapsed to non-threaded no-ops or direct
 * per-frame calls on TARGET_PSP, see plan decision #4) and calls
 * Graph_ThreadEntry() directly -- which never returns in practice, driving
 * every game state (Setup -> ConsoleLogo for Phase 1) from here on. Each
 * frame's display list now goes through the ported gfx_pc.c/gfx_scegu.c
 * F3DEX2 interpreter (plan steps 6-7) via Graph_TaskSet00's TARGET_PSP hook
 * (src/code/graph.c), so real rendering happens via gfx_init() below
 * instead of the hand-rolled sceGu bring-up this file used before. */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>

#include "ultra64.h"
#include "libu64/pad.h"
#include "libu64/padsetup.h"
#include "psp_rom.h"
#include "dma.h"
#include "gfx_pc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"

extern void DmaMgr_InitForTest(void);
extern void Main(void* arg);
extern struct GfxWindowManagerAPI gfx_wm_psp;
extern struct GfxRenderingAPI gfx_opengl_api; /* gfx_scegu.c's PSP backend -- see gfx_scegu.c */

PSP_MODULE_INFO("OOT_PSP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-1024);

/* Own display list buffer for the pre-Main() DMA test flash, separate from
 * gfx_scegu.c's internal one -- sceGu only cares that whatever buffer is
 * passed to sceGuStart is valid/aligned, not who owns it. */
static unsigned int __attribute__((aligned(16))) sDmaTestList[16384];

int main(void) {
    extern void PspOsMesgSetMainThread(void);

    pspDebugScreenInit();

    /* Register this thread as the one that must never park forever in a
     * libultra message queue. Worker threads (padmgr, DmaMgr) still block
     * indefinitely, which is correct for them -- see the long note in
     * psp/src/libultra/os_mesg.c. Must run before any osCreateMesgQueue. */
    PspOsMesgSetMainThread();

    /* gfx_init() runs the real sceGu bring-up (gfx_scegu_init, via the
     * rendering API's .init callback) and exit-callback registration (via
     * gfx_wm_psp's .init callback) -- see psp/src/gfx/gfx_scegu.c /
     * gfx_wm_psp.c. Must happen before any sceGu* call, including the DMA
     * test flash below. */
    gfx_init(&gfx_wm_psp, &gfx_opengl_api, "OoT PSP", false);

    OSMesgQueue contMesgQ;
    OSMesg contMesgBuf[1];
    OSContStatus contStatus[MAXCONTROLLERS];
    u8 contPadMask;
    osCreateMesgQueue(&contMesgQ, contMesgBuf, 1);
    PadSetup_Init(&contMesgQ, &contPadMask, contStatus);

    /* DmaMgr smoke test (see file header): verify the ported thread/queue
     * machinery against real ROM data, same "dmadata" segment and expected
     * checksum attempt 1 already validated byte-correct. */
    PspRom_Init("oot-pal-1.0.z64");
    DmaMgr_InitForTest();

    static u8 sDmaTestBuf[0x6000];
    s32 dmaTestSize = 0x0000D8C0 - 0x00007950;
    s32 dmaTestRet = DmaMgr_RequestSync(sDmaTestBuf, 0x00007950, dmaTestSize);

    u32 dmaTestChecksum = 0;
    for (s32 i = 0; i < dmaTestSize; i++) {
        dmaTestChecksum = dmaTestChecksum * 31 + sDmaTestBuf[i];
    }
    int dmaTestPassed = (dmaTestRet == 0) && (dmaTestChecksum == 0x036b70b9);
    {
        extern void PspDebugLogDmaTest(int ret, unsigned int checksum, int passed);
        PspDebugLogDmaTest(dmaTestRet, dmaTestChecksum, dmaTestPassed);
    }

    /* Show the DMA pre-flight result ONLY when it fails (blue for ~1 second).
     *
     * This used to flash green for a second on every successful boot as well.
     * That was worth having back when reaching Main() at all was in question,
     * but it now costs a second of every launch to report the expected case,
     * and a coloured screen before the game appears is indistinguishable from
     * the port having hung -- which is exactly the signal it was meant to
     * provide. A failure still stops and says so, because if the ROM cannot be
     * read then everything after this point misbehaves in confusing ways. */
    if (!dmaTestPassed) {
        for (int i = 0; i < 60; i++) {
            sceGuStart(GU_DIRECT, sDmaTestList);
            sceGuClearColor(0xff0000ff);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
            sceGuFinish();
            sceGuSync(0, 0);
            sceDisplayWaitVblankStart();
            sceGuSwapBuffers();
        }
    }

    /* Hand off to the real single-loop engine (see file header). Never
     * returns in practice -- Graph_ThreadEntry loops through game states
     * until the game truly exits. */
    Main(NULL);
 
    return 0;
}
