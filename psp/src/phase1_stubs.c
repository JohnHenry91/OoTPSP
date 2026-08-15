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

/* Environment_FillScreen now comes from the real src/code/z_kankyo.c, which
 * is compiled in as of the environment-lighting promotion (see Makefile.psp).
 * The Phase 1 no-op here existed only so z_title.c's console-logo fade would
 * link without pulling in the whole weather/lighting/skybox system. */

/* Real impl: src/code/z_sram.c -- now built for real (Phase 2, needed for
 * Sram_InitDebugSave in TitleSetup_SetupTitleScreen), see Makefile.psp. It
 * still pulls in two things we don't need yet:
 * - src/boot/ss_sram.c (real N64 cartridge SRAM DMA hardware access) --
 *   no-op for now, we boot straight into Sram_InitDebugSave's default save
 *   every time rather than persisting/loading real save data.
 * - Audio_SetSoundOutputMode (src/audio/...) -- whole audio subsystem is
 *   out of scope (see Audio_Update/AudioMgr_StopAllSfx below). */
void SsSram_ReadWrite(s32 addr, void* dramAddr, size_t size, s32 direction) {
}

void Audio_SetSoundOutputMode(s8 soundSetting) {
}

/* Real impl: src/audio/game/general.c -- ocarina scarecrow-song data, audio
 * subsystem out of scope. Only referenced by src/code/z_sram.c's
 * Sram_OpenSave (real save-file loading, not exercised by our
 * Sram_InitDebugSave-only boot path) -- dummy values are never read. */
u8* gScarecrowSpawnSongPtr = NULL;
OcarinaNote* gScarecrowLongSongPtr = NULL;

/* Real impl: src/code/z_parameter.c (4415 lines, HUD/inventory-UI drawing +
 * item-select logic) -- these two tiny tables are only referenced by
 * src/code/z_sram.c's Sram_OpenSave (see above, unreached), not worth
 * pulling in the whole file for. */
s16 gSpoilingItems[] = { 0 };
s16 gSpoilingItemReverts[] = { 0 };

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

/* Real impl: src/audio/game/sfx.c -- the whole audio subsystem is out of
 * scope for Phase 1 (see plan roadmap; Audio_Update is already stubbed in
 * graph.c). Silence is fine for now. */
Vec3f gSfxDefaultPos;
f32 gSfxDefaultFreqAndVolScale = 1.0f;
s8 gSfxDefaultReverb = 0;

void Audio_PlaySfxGeneral(u16 sfxId, Vec3f* pos, u8 token, f32* freqScale, f32* vol, s8* reverbAdd) {
}

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

/* Real impl: src/audio/game/general.c / src/code/audio_stop_all_sfx.c --
 * whole audio subsystem out of scope for Phase 1 (see graph.c's
 * Audio_Update guard). z_title.c calls these directly too. */
void Audio_Update(void) {
}
void AudioMgr_StopAllSfx(void) {
}

/* Sequence/ambience control, referenced by the real src/code/z_kankyo.c
 * (Environment_PlaySceneSequence, Environment_PlayTimeBasedSequence,
 * Environment_Play/StopStormNatureAmbience). Same subsystem, same reason.
 *
 * These four are the safe kind of no-op stub: they return void and take only
 * scalars by value, so there is no out-parameter left unwritten and no $v0
 * left holding the previous call's garbage. Written with their REAL
 * signatures anyway -- see the note in phase2_stubs_gen.c about why a
 * mismatched signature that "links fine" is the trap, not the shortcut. */
void Audio_PlaySceneSequence(u16 seqId) {
}
void Audio_PlayMorningSceneSequence(u16 seqId) {
}
void Audio_PlayNatureAmbienceSequence(u8 natureAmbienceId) {
}
void Audio_SetNatureAmbienceChannelIO(u8 channelIdxRange, u8 ioPort, u8 ioData) {
}

/* This one returns a value, so it must NOT be a no-op: z_kankyo.c's
 * Environment_StopStormNatureAmbience does
 *
 *     if (Audio_GetActiveSeqId(SEQ_PLAYER_BGM_MAIN) == NA_BGM_NATURE_AMBIENCE)
 *
 * and a garbage $v0 would take that branch at random. With no audio driver
 * nothing is ever playing, and NA_BGM_DISABLED is exactly the value the real
 * function returns for an idle sequence player -- so this is the honest
 * answer rather than a placeholder. */
u16 Audio_GetActiveSeqId(u8 seqPlayerIndex) {
    return NA_BGM_DISABLED;
}

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
