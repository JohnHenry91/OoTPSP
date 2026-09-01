/* Deliberate Phase 1 simplifications -- functions whose real implementation
 * lives in a file with heavy PlayState/scene dependencies far out of scope
 * for booting to the console-logo state, stubbed out rather than pulling
 * the whole subsystem in. Revisit once actual scene rendering is in scope
 * (see plan roadmap). */
#include "ultra64.h"
#include "gfx.h"
#include "game.h"
#include "sram.h"
#include "vis.h"
#include "fault.h"
#include "z_lib.h"
#include "z_math.h"
#include "sfx.h"
#include "ss_sram.h"
#include "audio.h"
#include "ocarina.h"
#include "player.h"

/* Real RSP audio microcode blob (sys_ucode.c-adjacent -- same category as the
 * graphics RSP microcode already never linked, see Graph_TaskSet00's own
 * TARGET_PSP branch/that file's note). AudioThread_UpdateImpl (src/audio/
 * internal/thread.c) builds an OSTask referencing these, but never actually
 * submits it to anything on PSP -- AudioMgr_HandleRetrace's TARGET_PSP branch
 * (src/code/audio_thread_manager.c) skips the real-RSP Sched handoff entirely,
 * since AudioSynth_Update already did the real synthesis in portable C.
 * Dummy 1-entry arrays only so the (dead, but still computed) pointer/size
 * arithmetic links. */
u64 aspMainTextStart[1];
u64 aspMainTextEnd[1];
u64 aspMainDataStart[1];
u64 aspMainDataEnd[1];

/* Real N64 cartridge PI handle init -- no such hardware on PSP. Our own
 * osEPiStartDma (psp/src/libultra/os_pi.c) ignores the handle entirely, so
 * this used to return NULL.
 *
 * It must not. AudioLoad_Dma (src/audio/internal/load.c) does not merely pass
 * the handle along -- it WRITES through it first:
 *
 *     handle->transferInfo.cmdType = 2;   // OSPiHandle + 0x14
 *     sDmaHandler(handle, mesg, direction);
 *
 * With a NULL handle that is a word store to address 0x00000014, on the audio
 * thread, roughly twice per frame. PPSSPP catches it, logs "Bad memory access
 * detected and ignored", and runs on; real hardware takes an address error and
 * the console dies. That is why the fault only ever appeared on the console,
 * why it landed just after AudioThread_ScheduleProcessCmds handed the command
 * stack over (AudioThread_ProcessGlobalCmd -> AudioLoad_SyncInitSeqPlayerInternal
 * -> AudioLoad_SyncLoadFont -> AudioLoad_SyncLoad -> AudioLoad_SyncDma), and
 * why disabling the audio thread made it go away.
 *
 * So hand out a real, static handle. Writable memory is the entire
 * requirement -- nothing reads the field back, and the DMA itself is served by
 * our own osEPiStartDma. Keeping the decomp's store legal is much safer than
 * editing load.c to skip it, because AudioLoad_Dma is reached from a dozen
 * call sites and the same store would come back with the next one. */
static OSPiHandle sPspCartHandle;

OSPiHandle* osCartRomInit(void) {
    return &sPspCartHandle;
}

/* Environment_FillScreen now comes from the real src/code/z_kankyo.c, which
 * is compiled in as of the environment-lighting promotion (see Makefile.psp).
 * The Phase 1 no-op here existed only so z_title.c's console-logo fade would
 * link without pulling in the whole weather/lighting/skybox system. */

/* Real impl: src/code/z_sram.c -- now built for real (Phase 2, needed for
 * Sram_InitDebugSave in TitleSetup_SetupTitleScreen), see Makefile.psp. It
 * still pulls in one thing we don't need yet:
 * - src/boot/ss_sram.c (real N64 cartridge SRAM DMA hardware access) --
 *   no-op for now, we boot straight into Sram_InitDebugSave's default save
 *   every time rather than persisting/loading real save data.
 * Audio_SetSoundOutputMode used to be stubbed here too (whole audio
 * subsystem out of scope for Phase 1) -- now comes from the real
 * src/audio/game/general.c, see Makefile.psp's audio-assets block. */
void SsSram_ReadWrite(s32 addr, void* dramAddr, size_t size, s32 direction) {
}

/* gScarecrowSpawnSongPtr/gScarecrowLongSongPtr PROMOTED to the real
 * src/audio/game/general.c (Phase 4 audio bring-up, see Makefile.psp). */

/* Real impl: src/code/z_parameter.c (4415 lines, HUD/inventory-UI drawing +
 * item-select logic) -- these two tiny tables are only referenced by
 * src/code/z_sram.c's Sram_OpenSave (see above, unreached), not worth
 * pulling in the whole file for. */
/* gSpoilingItems / gSpoilingItemReverts: PROMOTED to the real
 * src/code/z_parameter.c (siehe Makefile.psp). Die Platzhalter waren
 * einelementige Nullfelder -- z_parameter.c liest sie ueber den Index der
 * verderblichen Flaschenware, was auf einem solchen Feld daneben gegriffen
 * haette, sobald das Interface wirklich laeuft. */

/* Real impls (src/code/z_vismono.c, z_viszbuf.c) hardcode real N64 KSEG1
 * physical addresses (D_0E000000/D_0F000000) with no PSP equivalent --
 * debug-hotkey-only framebuffer visualizers, never exercised by Phase 1's
 * boot path. */
void VisMono_Init(VisMono* this) {
}
void VisMono_Destroy(VisMono* this) {
}
void VisMono_Draw(VisMono* this, Gfx** gfxP) {
}

void VisZBuf_Init(VisZBuf* this) {
}
void VisZBuf_Destroy(VisZBuf* this) {
}
void VisZBuf_Draw(VisZBuf* this, Gfx** gfxP) {
}

/* func_8002EABC now comes from the real src/code/z_actor.c (Phase 2). */

/* Real impl: src/n64dd/z_n64dd.c -- 64DD (N64 disk drive expansion,
 * Japan-only add-on) hardware probing/init. pal-1.0 never has this
 * attached (same as real N64 hardware -- see src/code/main.c's
 * D_80121211 check, always 0), so these are all safely "not present". */
void func_801C6EA0(Gfx** gfxP) {
}
void func_801C7268(void) {
}
s32 func_801C7658(void) {
    return 0;
}
s32 func_801C7818(void) {
    return 0;
}
void func_801C7E78(void) {
}

/* Real impl: src/code/fault_n64.c (856 lines) -- the N64 crash/debugger
 * screen, full of real hardware register dumps with no PSP equivalent.
 * No-ops; Fault_AddHungupAndCrash(Impl) intentionally still halts (matches
 * its NORETURN contract) so a real bug doesn't silently continue running. */
vs32 gFaultMsgId = 0;

void Fault_Init(void) {
}
void Fault_AddClient(FaultClient* client, void* callback, void* arg0, void* arg1) {
}
void Fault_RemoveClient(FaultClient* client) {
}
void Fault_SetFrameBuffer(void* fb, u16 w, u16 h) {
}
NORETURN void Fault_AddHungupAndCrashImpl(const char* exp1, const char* exp2) {
    for (;;) {
    }
}
/* Fault_AddHungupAndCrash itself already exists in
 * psp/src/libultra/fault.c (attempt 1's version, which also prints
 * file:line via pspDebugScreenPrintf -- kept as the one definition). */

/* Real impl: src/libu64/loadfragment2_n64.c (212 lines, MIPS ELF-style
 * relocation of runtime-DMA'd overlay blobs) -- dead code on PSP, since
 * every gGameStateOverlayTable entry has vramStart == NULL (see
 * psp/src/gamestate_table_psp.c), meaning Overlay_LoadGameState
 * (src/code/z_DLF.c) already never takes the branch that would call this.
 * Exists purely so that dead branch still links. */
void* Overlay_AllocateAndLoad(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd) {
    return NULL;
}

/* Real N64DD probe globals (src/code/code_n64dd_800AD410.c, not linked --
 * see src/code/main.c's #if PLATFORM_N64 && !TARGET_PSP guard). Always 0,
 * matching real pal-1.0 hardware (no 64DD attached). */
u8 D_80121210 = 0;
u8 D_80121211 = 0;

/* Real def: src/boot/idle.c (Idle_ThreadEntry -- not used at all on PSP,
 * see plan decision #4; psp/src/main.c replaces the whole Idle/Main
 * bootstrap). Only the two globals are needed, for game.c/graph.c's VI
 * mode bookkeeping (never meaningfully read since Sched/VI is stubbed). */
OSViMode gViConfigMode;
u8 gViConfigModeType = OS_VI_NTSC_LAN1;

/* gTransitionTileState now comes from the real src/code/z_play.c (Phase 2). */

/* Audio_Update/AudioMgr_StopAllSfx/Audio_PlaySceneSequence/
 * Audio_PlayMorningSceneSequence/Audio_PlayNatureAmbienceSequence/
 * Audio_SetNatureAmbienceChannelIO/Audio_GetActiveSeqId used to be stubbed
 * here (whole audio subsystem out of scope for Phase 1) -- now come from the
 * real src/audio/game/{general,sequence}.c and src/code/audio_stop_all_sfx.c,
 * see Makefile.psp's audio-assets block. */

/* _nintendo_rogo_staticSegmentRomStart/End dummies removed: z_title.c now
 * uses real ROM offsets (NINTENDO_ROGO_STATIC_ROM_START/SIZE, read from
 * reference/oot's own build map) directly under TARGET_PSP instead of these
 * symbols -- see src/overlays/gamestates/ovl_title/z_title.c. */

/* TEMPORARY diagnostic flag -- see psp/src/gfx/gfx_pc.c's gfx_sp_vertex.
 * 1 = force solid white vertex color, to isolate whether garbled logo
 * rendering is a geometry/matrix problem or a color/lighting/texture
 * problem. Remove once diagnosed. */
int gDebugForceWhiteVerts = 0;

/* TEMPORARY diagnostic flag -- see psp/src/gfx/gfx_pc.c's cull test in the
 * triangle-drawing code. 1 = disable backface culling entirely, to test
 * whether the period-3 sky/ground flip seen after the matrix-unpacking fix
 * is caused by inconsistent winding/culling. Tested this session: flip
 * persisted identically with culling disabled, so culling is NOT the cause
 * -- left at 0 (real culling behavior) pending the next lead. Remove once
 * diagnosed. */
int gDebugDisableCull = 0;

/* Which Link to spawn -- see the override in z_scene.c's Play_SpawnScene.
 * -1 = leave the save's own value alone, 0 = LINK_AGE_ADULT, 1 = LINK_AGE_CHILD.
 * Kept after it did its job: forcing ADULT proved the displaced-geometry bug
 * was in the renderer and not in the child model data, because adult is a
 * different object, skeleton and set of limb display lists yet broke
 * identically. Default -1 = leave the save's own value alone. Read at every
 * scene load, so poking it and crossing a scene transition switches age live. */
int gDebugForceLinkAge = -1;

/* 1 while the boot logo (ConsoleLogo/TitleSetup) is on screen, 0 once real
 * gameplay (Play_Init) starts -- see gfx_scegu.c's is_n64_logo_cube_combine/
 * is_n64_logo_text_combine, which gate the boot-logo-only 2-cycle-blend
 * approximation hack on this flag (that hack's structural shader-shape
 * detection collides with common real N64 combine formulas once real scenes
 * start rendering, see project memory). Set to 0 in Play_Init (z_play.c). */
int gPspBootLogoActive = 1;

/* "Pressing forward walks backward" probe -- see the sample site in
 * z_player.c (Player_ProcessControlStick, right after sControlStickWorldYaw is
 * computed). Defined here rather than in z_player.c so the probe block there
 * stays a pure read with no storage of its own. */
short gPspInpCurX, gPspInpCurY, gPspInpRelX, gPspInpRelY;
short gPspInpStickAngle, gPspInpWorldYaw, gPspInpCamYaw, gPspInpShapeYaw;
unsigned int gPspInpMagic, gPspInpSamples;
float gPspInpMagnitude;
