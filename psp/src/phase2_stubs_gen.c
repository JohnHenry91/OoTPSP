/* AUTO-GENERATED Phase 2 stubs (psp/src/phase2_stubs_gen.c).
 * No-op / dummy definitions for cosmetic and out-of-scope subsystems
 * (HUD/interface, screen transitions, pause/kaleido menu, message system,
 * cutscene/demo, game-over, weather/environment draws, particle effects,
 * positional SFX, camera quake, framebuffer post-effects, map, jpeg).
 * Deliberately declares NO prototypes/headers: the C linker matches these
 * by name only, so signature mismatches vs the real decl are irrelevant for
 * linking. These exist so z_play.c + the real engine core link; the code
 * paths that call them are not exercised by a static Link's-House room
 * render. Promote any of these to a real source file in Makefile.psp when
 * its subsystem comes into scope. Regenerate via the same script if the
 * linked file set changes.
 *
 * WHEN REGENERATING: every stub that is used as a display list must keep its
 * PSP_STUB_ENDDL initialiser (see the note above the data section). A plain
 * zeroed stub is not an empty display list, it is a non-terminating one, and
 * it crashes the game. */

/* --- functions: 166 --- */
/* Audio_QueueSeqCmd/Audio_SetEnvReverb/Audio_SetExtraFilter/Audio_StopSfxByPos
 * PROMOTED to the real src/audio/game/{general,sequence,sfx}.c (Phase 4 audio
 * bring-up, see Makefile.psp). Stubs removed -- keeping them would be
 * duplicate symbols. */
/* Screen transitions: these five MUST NOT be `void` stubs.
 *
 * z_play.c drives the whole scene change through a function-pointer table and
 * branches on what these RETURN:
 *
 *     case TRANS_MODE_INSTANCE_RUNNING:
 *         if (this->transitionCtx.isDone(&this->transitionCtx.instanceData)) {
 *
 * A `void X_IsDone(void) {}` stub returns nothing, so the caller reads whatever
 * happened to be left in $v0. When that is zero the transition is never done:
 * transitionMode stays TRANS_MODE_INSTANCE_RUNNING forever, input stays
 * blocked, the next scene is never loaded, and the screen keeps whatever
 * half-drawn state the (equally stubbed) Draw left behind -- while the audio
 * thread and the port's own menu, which bypass the N64 display list entirely,
 * carry on working. That is exactly the reported failure on the walk back over
 * the Kokiri bridge, including its intermittence: whether it hangs depends on
 * a leftover register value.
 *
 * The stub file's own header says signature mismatches "are irrelevant for
 * linking". True -- and irrelevant only for linking. Calling one is undefined.
 *
 * Returning 1 is the honest answer here rather than a fudge: Update and Draw
 * really are no-ops on this port, so the transition genuinely has nothing left
 * to do. The scene change happens without a fade until the real transitions
 * are ported. Init returns the instance pointer it was handed, same as the
 * real ones. */
void* TransitionFade_Init(void* thisx) { return thisx; }
int TransitionFade_IsDone(void* thisx) { (void)thisx; return 1; }
void* TransitionCircle_Init(void* thisx) { return thisx; }
int TransitionCircle_IsDone(void* thisx) { (void)thisx; return 1; }
void* TransitionWipe_Init(void* thisx) { return thisx; }
int TransitionWipe_IsDone(void* thisx) { (void)thisx; return 1; }
void* TransitionTriforce_Init(void* thisx) { return thisx; }
int TransitionTriforce_IsDone(void* thisx) { (void)thisx; return 1; }
void* TransitionTile_Init(void* thisx) { return thisx; }

void Cutscene_HandleConditionalTriggers(void) {}
void Cutscene_HandleEntranceTriggers(void) {}
void Cutscene_InitContext(void) {}
void Cutscene_UpdateManual(void) {}
void Cutscene_UpdateScripted(void) {}
void DamageTable_Get(void) {}
void DebugDisplay_DrawObjects(void) {}
void DebugDisplay_Init(void) {}
void DynaPolyActor_TransformCarriedActor(void) {}
/* Promoted to the real src/code/z_bg_item.c (Phase 3, enabled by the first
 * dyna-poly actors). Stub removed -- keeping it would be a duplicate symbol. */
void EffectBlure_Destroy(void) {}
void EffectBlure_Draw(void) {}
void EffectBlure_Init1(void) {}
void EffectBlure_Init2(void) {}
void EffectBlure_Update(void) {}
void EffectShieldParticle_Destroy(void) {}
void EffectShieldParticle_Draw(void) {}
void EffectShieldParticle_Init(void) {}
void EffectShieldParticle_Update(void) {}
void EffectSpark_Destroy(void) {}
void EffectSpark_Draw(void) {}
void EffectSpark_Init(void) {}
void EffectSpark_Update(void) {}
void EffectSsGRipple_Spawn(void) {}
void EffectSsHitMark_SpawnFixedScale(void) {}
void EffectSsKiraKira_SpawnDispersed(void) {}
void EffectSsKiraKira_SpawnSmall(void) {}
void EffectSsSibuki_SpawnBurst(void) {}
/* Environment_* PROMOTED to the real src/code/z_kankyo.c (see Makefile.psp).
 *
 * Second instance of the same failure mode as the Quake_* block below, and the
 * one behind the port's long-standing "walls and floor are white-grey".
 * Environment_Update's real signature is
 *
 *     void Environment_Update(PlayState*, EnvironmentContext*, LightContext*,
 *                             PauseContext*, MessageContext*, GameOverContext*,
 *                             GraphicsContext*)
 *
 * i.e. the "void, but writes through pointer arguments" class -- of the 154
 * no-op stubs in this file, 113 fall into it. A no-op therefore left the
 * scene's ENV_LIGHT_SETTINGS never becoming actual light state, so shade sat
 * at its default. That is visible on exactly the surfaces it should be:
 * hakaana2's 18 textures are 9 intensity-only formats (5x i4, 3x i8, 1x ia8),
 * which carry no colour at all and take it entirely from the combiner (shade /
 * prim / env), and 8 rgba16 ones that carry their own -- which is why the
 * fountain water and Link looked right while the walls and floor did not.
 *
 * Environment_ZBufValToFixedPoint, Environment_LerpWeight and
 * Environment_IsForcedSequenceDisabled additionally return values, so they were
 * in the 29-strong "caller reads garbage $v0" class as well. */
void Fault_Printf(void) {}
void Fault_SetCursor(void) {}
void Font_LoadOrderedFont(void) {}
void FrameAdvance_Init(void) {}
void FrameAdvance_Update(void) {}
void GameOver_FadeInLights(void) {}
void GameOver_Init(void) {}
void GameOver_Update(void) {}
void Health_InitMeter(void) {}
void Horse_InitPlayerHorse(void) {}
void Interface_ChangeHudVisibilityMode(void) {}
void Interface_Draw(void) {}
void Interface_SetSceneRestrictions(void) {}
void Interface_Update(void) {}
void Inventory_ReplaceItem(void) {}
void Inventory_SwapAgeEquipment(void) {}
void Item_Give(void) {}
void Jpeg_Decode(void) {}
void KaleidoManager_Destroy(void) {}
void KaleidoManager_Init(void) {}
void KaleidoScopeCall_Destroy(void) {}
void KaleidoScopeCall_Draw(void) {}
void KaleidoScopeCall_Init(void) {}
void KaleidoScopeCall_Update(void) {}
void KaleidoSetup_Update(void) {}
void Magic_Reset(void) {}
void Map_Destroy(void) {}
void Map_Init(void) {}
void Map_InitRoomData(void) {}
void Map_SavePlayerInitialInfo(void) {}
void Message_ContinueTextbox(void) {}
void Message_Draw(void) {}
void Message_GetState(void) {}
void Message_SetTables(void) {}
void Message_ShouldAdvance(void) {}
void Message_StartTextbox(void) {}
void Message_Update(void) {}
void OnePointCutscene_Init(void) {}
void Overlay_Load(void) {}
void PreRender_ApplyFilters(void) {}
void PreRender_Destroy(void) {}
void PreRender_DrawCoverage(void) {}
void PreRender_Init(void) {}
void PreRender_RestoreFramebuffer(void) {}
void PreRender_SaveFramebuffer(void) {}
void PreRender_SetValues(void) {}
void PreRender_SetValuesSave(void) {}
/* Quake_* PROMOTED to the real src/code/z_quake.c (see Makefile.psp).
 *
 * These stubs caused the port's long-standing "the perspective flips back and
 * forth" bug, and the mechanism is worth reading before adding stubs like
 * this again. z_camera.c:8231 does:
 *
 *     numQuakesApplied = Quake_Update(camera, &camShake);
 *     if ((numQuakesApplied != 0) && (camera->setting != CAM_SET_TURN_AROUND)) {
 *         viewAt.x = camera->at.x + camShake.atOffset.x;   // ...etc
 *
 * `void Quake_Update(void) {}` standing in for
 * `s16 Quake_Update(Camera*, ShakeInfo*)` means: the stub never writes
 * `camShake` (a caller stack local, so it holds whatever was there before),
 * and never sets $v0, so `numQuakesApplied` is leftover garbage from the
 * previous call -- reliably non-zero. The quake branch is therefore taken on
 * every single frame and adds uninitialised stack to the camera's `at`.
 *
 * Measured symptom: `camera->at` constant and correct, `play->view.at` equal
 * to it plus a garbage offset that settles into a couple of stable values --
 * i.e. the view snapping between two orientations.
 *
 * NOTE FOR WHOEVER REGENERATES THIS FILE: the header above says signature
 * mismatches "are irrelevant for linking". That is true, and it is also
 * exactly the trap -- it is irrelevant for *linking* and disastrous at
 * runtime for any function that returns a value or fills an out-parameter.
 * A no-op stub is only safe for a function that returns void AND writes
 * nothing through its arguments. */
void Rupees_ChangeBy(void) {}
void SfxSource_InitAll(void) {}
void SfxSource_PlaySfxAtFixedWorldPos(void) {}
void SfxSource_UpdateAll(void) {}
void SysUcode_GetUCode(void) {}
void SysUcode_GetUCodeData(void) {}
void TransitionCircle_Destroy(void) {}
void TransitionCircle_Draw(void) {}
void TransitionCircle_SetColor(void) {}
void TransitionCircle_SetType(void) {}
void TransitionCircle_SetUnkColor(void) {}
void TransitionCircle_Start(void) {}
void TransitionCircle_Update(void) {}
void TransitionFade_Destroy(void) {}
void TransitionFade_Draw(void) {}
void TransitionFade_SetColor(void) {}
void TransitionFade_SetType(void) {}
void TransitionFade_Start(void) {}
void TransitionFade_Update(void) {}
void TransitionTile_Destroy(void) {}
void TransitionTile_Draw(void) {}
void TransitionTile_Update(void) {}
void TransitionTriforce_Destroy(void) {}
void TransitionTriforce_Draw(void) {}
void TransitionTriforce_SetColor(void) {}
void TransitionTriforce_SetType(void) {}
void TransitionTriforce_Start(void) {}
void TransitionTriforce_Update(void) {}
void TransitionWipe_Destroy(void) {}
void TransitionWipe_Draw(void) {}
void TransitionWipe_SetColor(void) {}
void TransitionWipe_SetType(void) {}
void TransitionWipe_Start(void) {}
void TransitionWipe_Update(void) {}
void func_80026400(void) {}
void func_80026608(void) {}
void func_80026860(void) {}
void func_80026A6C(void) {}
void func_8002857C(void) {}
void func_8002865C(void) {}
void func_800286CC(void) {}
void func_80043334(void) {}
void func_800BB2B4(void) {}
/* func_800F4010/func_800F4C58/func_800F6964 PROMOTED to the real
 * src/audio/game/general.c (Phase 4 audio bring-up, see Makefile.psp). */
void func_801C7C1C(void) {}
void gspS2DEX2d_fifoTextStart(void) {}
void guS2DInitBg(void) {}

/* A stubbed display list must still be a VALID display list. A zeroed buffer
 * decodes as an endless run of G_NOOP (opcode 0x00) with no
 * gsSPEndDisplayList() anywhere, so the interpreter never returns from it: it
 * walks off the end into neighbouring memory and keeps going. Measured live
 * (2026-08-15): gGlowCircleTextureLoadDL, which z_lights.c:375 submits every
 * single frame, drove the display-list interpreter 47 levels deep and took
 * the game down with it.
 *
 * Byte 3 = 0xDF makes w0 read as 0xDF000000 (G_ENDDL) when loaded as a
 * little-endian u32, which is how the interpreter reads compiled-in lists.
 * So each of these now draws nothing and returns immediately -- the same
 * intent the zeroed stub had, but actually expressed. */
#define PSP_STUB_ENDDL { [3] = (char)0xDF }

/* NOTE: nine `char x[64]` placeholders were removed here -- gWeatherMode,
 * gTimeSpeed, gTimeBasedSkyboxConfigs, gNormalSkyFiles, gLensFlareScale,
 * gLensFlareGlareStrength, gLensFlareColorIntensity, gCustomLensFlarePos and
 * gCustomLensFlareOn. They are real definitions in src/code/z_kankyo.c, which
 * is now compiled in.
 *
 * They are worth naming rather than just deleting, because they are the data
 * counterpart of the signature-mismatch trap documented above: a `char[64]`
 * standing in for an `f32` or a `SkyboxFile[]` links fine and then reads as
 * whatever the neighbouring bytes happen to be, and a WRITE through one of the
 * smaller ones (gTimeSpeed is a u16) leaves 62 bytes of unrelated storage
 * inside the same object. Same rule as for functions: a placeholder is only
 * safe when nothing reads or writes it. */

/* --- data: 57 --- */
char gBossDoorChainDL[64] = PSP_STUB_ENDDL;
char gBossDoorLockDL[64] = PSP_STUB_ENDDL;
char gCircleShadowDL[64] = PSP_STUB_ENDDL;
char gDCDayEntranceTex[64];
char gDCLavaFloor1Tex[64];
char gDCLavaFloor2Tex[64];
char gDCLavaFloor3Tex[64];
char gDCLavaFloor4Tex[64];
char gDCLavaFloor5Tex[64];
char gDCLavaFloor6Tex[64];
char gDCLavaFloor7Tex[64];
char gDCLavaFloor8Tex[64];
char gDCNightEntranceTex[64];
char gDekuTreeDayEntranceTex[64];
char gDekuTreeNightEntranceTex[64];
char gDoorChainDL[64] = PSP_STUB_ENDDL;
char gDoorLockDL[64] = PSP_STUB_ENDDL;
char gEffFlash1DL[64] = PSP_STUB_ENDDL;
/* gEffectSsOverlayTable PROMOTED to the real src/code/z_effect_soft_sprite_
 * dlftbls.c (TARGET_PSP branch). It must NOT come back as a `char[64]`: the
 * engine walks it as EffectSsOverlay[EFFECT_SS_TYPE_MAX] (1036 bytes) and a
 * 64-byte stand-in made every scene teardown free a wild pointer. See the
 * long note in that file. */
char gFootShadowDL[64] = PSP_STUB_ENDDL;
char gForestTempleDayEntranceTex[64];
char gForestTempleNightEntranceTex[64];
char gGTGDayEntranceTex[64];
char gGTGNightEntranceTex[64];
char gGerudoFortressDayWallTex[64];
char gGerudoFortressNightWallTex[64];
char gGlowCircleDL[64] = PSP_STUB_ENDDL;
char gGlowCircleTextureLoadDL[64] = PSP_STUB_ENDDL;
char gGoronCityDayEntranceTex[64];
char gGoronCityNightEntranceTex[64];
char gGuardHouseOutSideView1DayTex[64];
char gGuardHouseOutSideView1NightTex[64];
char gGuardHouseOutSideView2DayTex[64];
char gGuardHouseOutSideView2NightTex[64];
char gHorseShadowDL[64] = PSP_STUB_ENDDL;
char gIceCavernDayEntranceTex[64];
char gIceCavernNightEntranceTex[64];
char gKakarikoVillageDayWindowTex[64];
char gKakarikoVillageNightWindowTex[64];
char gLensOfTruthMaskTex[64];
char gLockOnArrowDL[64] = PSP_STUB_ENDDL;
char gLockOnReticleTriangleDL[64] = PSP_STUB_ENDDL;
char gLonLonHouseDayEntranceTex[64];
char gLonLonHouseNightEntranceTex[64];
char gLonLonRanchDayWindowTex[64];
char gLonLonRangeNightWindowsTex[64];
char gSpiritTempleDayEntranceTex[64];
char gSpiritTempleNightEntranceTex[64];
char gThievesHideoutDayEntranceTex[64];
char gThievesHideoutNightEntranceTex[64];
char gWaterTempleDayEntranceTex[64];
char gWaterTempleNightEntranceTex[64];
char gZorasDomainDayEntranceTex[64];
char gZorasDomainNightEntranceTex[64];
char gspS2DEX2d_fifoDataStart[64];
char spot00_room_0DL_012B20[64] = PSP_STUB_ENDDL;
char spot16_room_0DL_00AA48[64] = PSP_STUB_ENDDL;
