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
/* EffectBlure_Destroy/Draw/Init1/Init2/Update PROMOTED to the real
 * src/code/z_eff_blure.c (Phase 3, all actors). Stubs removed -- keeping them
 * would be duplicate symbols. */
void EffectShieldParticle_Destroy(void) {}
void EffectShieldParticle_Draw(void) {}
void EffectShieldParticle_Init(void) {}
void EffectShieldParticle_Update(void) {}
void EffectSpark_Destroy(void) {}
void EffectSpark_Draw(void) {}
void EffectSpark_Init(void) {}
void EffectSpark_Update(void) {}
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
/* Horse_InitPlayerHorse PROMOTED to the real src/code/z_horse.c
 * (Phase 3). Stub removed. */
void Jpeg_Decode(void) {}
void KaleidoManager_Destroy(void) {}
void KaleidoManager_Init(void) {}
void KaleidoScopeCall_Destroy(void) {}
void KaleidoScopeCall_Draw(void) {}
void KaleidoScopeCall_Init(void) {}
void KaleidoScopeCall_Update(void) {}
void KaleidoSetup_Update(void) {}
void Message_ContinueTextbox(void) {}
void Message_Draw(void) {}
/* Message_GetState: nach phase2_stubs.c hochgezogen -- ein void-Stub laesst v0
 * stehen, und der Aufrufer liest das als Textzustand. Siehe dort. */
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
void SfxSource_InitAll(void) {}
void SfxSource_PlaySfxAtFixedWorldPos(void) {}
void SfxSource_UpdateAll(void) {}
void SysUcode_GetUCode(void) {}
void SysUcode_GetUCodeData(void) {}
void TransitionTile_Destroy(void) {}
void TransitionTile_Draw(void) {}
void TransitionTile_Update(void) {}
/* func_80026400/80026608/80026860/80026A6C PROMOTED to the real
 * src/code/z_eff_ss_dead.c (Phase 3). Stubs removed. */
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

/* The 41 scene-texture placeholders that stood here are GONE.
 *
 * They were `char x[64];` while the display lists loaded a FULL texture
 * from that address -- a kilobyte or more read off a 64-byte object, so the
 * GE was handed whatever globals followed it. That is why the symptom was
 * bands of random colour rather than black: Dodongo's Cavern's lava floor,
 * and the confetti pixels on the walls of Kakariko and Lon Lon Ranch (the
 * only two scenes whose draw config binds a window texture).
 *
 * 40 of them now come from psp/build/psp_scene_textures_gen.c, copied out of
 * the scene sources by psp/tools/gen_scene_textures.py. The 41st,
 * gLensOfTruthMaskTex, lives in gameplay_keep and is compiled directly.
 * See Makefile.psp. */

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
/* gBossDoorChainDL / gBossDoorLockDL PROMOTED to the real data in
 * extracted/pal-1.0/assets/objects/object_bdoor (see Makefile.psp): Door_Shutter
 * came into the build and draws the chain and lock over the barred boss doors,
 * so the placeholders would have been the visible thing. */
/* gDoorChainDL / gDoorLockDL PROMOTED to the real data in
 * extracted/pal-1.0/assets/objects/gameplay_dangeon_keep, same reason as the
 * boss-door pair above. */
/* gEffectSsOverlayTable PROMOTED to the real src/code/z_effect_soft_sprite_
 * dlftbls.c (TARGET_PSP branch). It must NOT come back as a `char[64]`: the
 * engine walks it as EffectSsOverlay[EFFECT_SS_TYPE_MAX] (1036 bytes) and a
 * 64-byte stand-in made every scene teardown free a wild pointer. See the
 * long note in that file. */

/* TEN MORE PROMOTED (Phase 3, all actors): gCircleShadowDL, gEffFlash1DL,
 * gFootShadowDL, gGlowCircleDL, gGlowCircleTextureLoadDL, gHorseShadowDL,
 * gLockOnArrowDL, gLockOnReticleTriangleDL, spot00_room_0DL_012B20 and
 * spot16_room_0DL_00AA48.
 *
 * All ten are real display lists in gameplay_keep or in a scene, and all ten
 * are now available -- the object and scene blobs define every one of them
 * (psp/build/psp_object_syms_gen.c, psp/build/psp_scene_syms_gen.c).
 *
 * KEEPING THEM WOULD HAVE BEEN WORSE THAN USELESS, and in a way that is easy
 * to miss: the blob definitions are WEAK, deliberately, so that anything the
 * EBOOT still compiles in wins. A stub here is a strong definition, so each of
 * these ten silently beat the real display list it was standing in for. The
 * game then submitted an immediately-terminating list and drew nothing, with
 * no counter and no error anywhere -- Lights_DrawGlow submitting
 * gGlowCircleTextureLoadDL every frame and getting an empty list back is
 * exactly that.
 *
 * The rule this establishes: when a placeholder's real data becomes reachable,
 * the placeholder is not neutral any more. It outranks it. */
char gspS2DEX2d_fifoDataStart[64];
/* PROMOTED to the real src/code/z_parameter.c / z_lifemeter.c / z_map_exp.c (siehe Makefile.psp):
 *   Health_InitMeter, Interface_ChangeHudVisibilityMode, Interface_Draw, Interface_SetSceneRestrictions, Interface_Update, Inventory_ReplaceItem, Inventory_SwapAgeEquipment, Item_Give, Magic_Reset, Map_Destroy, Map_Init, Map_InitRoomData, Map_SavePlayerInitialInfo, Rupees_ChangeBy */
