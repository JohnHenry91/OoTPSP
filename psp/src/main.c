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
#include <psppower.h>

#include "ultra64.h"
#include "libu64/pad.h"
#include "libu64/padsetup.h"
#include "psp_rom.h"
#include "psp_blob_assets.h"
#include "dma.h"
#include "gfx_pc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "psp_hw_diag.h"
#include "psp_blob_assets.h"

extern void DmaMgr_InitForTest(void);
extern void Main(void* arg);
extern struct GfxWindowManagerAPI gfx_wm_psp;
extern struct GfxRenderingAPI gfx_opengl_api; /* gfx_scegu.c's PSP backend -- see gfx_scegu.c */

PSP_MODULE_INFO("OOT_PSP", 0, 1, 0);
/* THREAD_ATTR_VFPU is not optional here.
 *
 * Without it the vector unit is disabled for the thread, and any VFPU
 * instruction raises a Coprocessor Unusable exception. This port links
 * -lpspgum and -lpspmath, both of which ARE VFPU code, so that is not a
 * hypothetical. PPSSPP does not enforce the attribute and executes the
 * instructions regardless, which is why months of emulator testing never
 * showed it while a PSP-1000 and a PSP-2000 both died on real silicon. */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

/* Own display list buffer for the pre-Main() DMA test flash, separate from
 * gfx_scegu.c's internal one -- sceGu only cares that whatever buffer is
 * passed to sceGuStart is valid/aligned, not who owns it. */
static unsigned int __attribute__((aligned(16))) sDmaTestList[16384];

int main(int argc, char* argv[]) {
    extern void PspOsMesgSetMainThread(void);

    pspDebugScreenInit();

    /* Pin blob paths to the game's own directory FIRST. This used to happen
     * two hundred lines further down, after gfx_init, PspAudio_Init and
     * PspAudioTables_Init had already run -- all three of which can touch the
     * asset registry. Under an emulator the process cwd happens to be the game
     * directory so the relative paths resolve anyway; on hardware that is not
     * something to rely on. It also has to precede PspDiag_Init, which writes
     * its log to the same directory. */
    PspBlob_SetBaseDir(argc > 0 ? argv[0] : NULL);
    PspDiag_Init(PspBlob_GetBaseDir());
    PspDiag_Step("boot");

    /* CPU clock.
     *
     * This ran at 333/333/166 unconditionally, with a comment claiming
     * "battery life is the cost; there is no downside for correctness". That
     * held for every test so far because every test was PPSSPP, which does not
     * model power draw at all. On real hardware a PSP-1000 and a PSP-2000 both
     * switch OFF -- not freeze, not fault, but lose power -- and the full
     * overclock is the obvious suspect: it raises current draw sharply, and an
     * aged battery can brown out under it.
     *
     * So the clock is now a knob rather than a constant, defaulting to the
     * 222/111 the PSP boots applications at. Build with -DPSP_CPU_MHZ=333 to
     * get the old behaviour back once the hardware failure is understood. The
     * chosen value is written to the boot log, so a log from the console says
     * which clock produced it. */
#ifndef PSP_CPU_MHZ
#define PSP_CPU_MHZ 222
#endif
#if PSP_CPU_MHZ >= 333
    scePowerSetClockFrequency(333, 333, 166);
#else
    scePowerSetClockFrequency(PSP_CPU_MHZ, PSP_CPU_MHZ, PSP_CPU_MHZ / 2);
#endif
    PspDiag_Step("clock-set");

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
    PspDiag_Step("gfx-init");

    /* PspAudio_Init reserves the sceAudio output channel -- must happen
     * before Main() spawns AudioMgr's real thread (src/code/main.c), whose
     * first retrace tick can call osAiSetNextBuffer -> PspAudio_Output
     * almost immediately. See psp/src/audio/audio_psp.c. */
    extern void PspAudio_Init(void);
#if !PSP_DISABLE_AUDIO
    PspAudio_Init();
#endif
    PspDiag_Step("audio-init");

    /* Fills gSequenceTable/gSoundFontTable/gSampleBankTable -- must happen
     * before Main() spawns AudioMgr's real thread, which reads them almost
     * immediately (Audio_Init -> AudioLoad_Init). See psp_audio_tables.c. */
    extern void PspAudioTables_Init(void);
#if !PSP_DISABLE_AUDIO
    PspAudioTables_Init();
#endif
    PspDiag_Step("audio-tables");

    OSMesgQueue contMesgQ;
    OSMesg contMesgBuf[1];
    OSContStatus contStatus[MAXCONTROLLERS];
    u8 contPadMask;
    osCreateMesgQueue(&contMesgQ, contMesgBuf, 1);
    PadSetup_Init(&contMesgQ, &contPadMask, contStatus);
    PspDiag_Step("pad-init");

    /* DmaMgr smoke test (see file header): verify the ported thread/queue
     * machinery against real ROM data, same "dmadata" segment and expected
     * checksum attempt 1 already validated byte-correct. */
    /* Blob paths are relative, and only the main thread has a cwd to resolve
     * them against -- pin them to the game's own directory before any thread
     * that might load one exists. See psp/include/psp_blob_assets.h. */
    /* (base dir is pinned at the top of main now) */

    PspDiag_Step("rom-init");
    PspRom_Init("oot-pal-1.0.z64");
    PspDiag_Step("dma-init");
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
