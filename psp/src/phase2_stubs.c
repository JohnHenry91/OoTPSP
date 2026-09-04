/* Phase 2 stubs: cosmetic / out-of-scope subsystems for the first
 * Play_Init milestone (HUD, screen transitions, pause menu, messages,
 * cutscene/demo, game-over, debug, positional sfx). Filled in iteratively
 * as the linker reports what is actually referenced. */
#include "ultra64.h"
#include "play_state.h"
#include "message.h"
#include "camera.h"

/* Real def: src/code/z_demo.c (the cutscene system, not ported). Referenced by
 * the real src/code/z_kankyo.c, which sets it in Environment_Init. Nothing
 * reads it while z_demo.c is out of the build -- the readers are all in
 * z_demo.c and db_camera.c -- so a plain definition with the same type is the
 * whole requirement here. Defining it (rather than --defsym'ing it to an
 * address) keeps the type visible to the compiler: a u8 write through a wrongly
 * sized symbol would smash the three bytes after it. */
u8 gUseCutsceneCam;

/* --- Phase 3: pulled in by the first world actor (En_Kusa) ----------------
 * This used to be a pair: EffectSsKakera_Spawn beside it, so cut grass produced
 * neither shards nor a drop. The shard half is gone -- the real
 * src/code/z_effect_soft_sprite_old_init.c is compiled in now (the Bg_* scenery
 * actors needed it), so the shards are real. Only the drop is still missing.
 *
 * Named here rather than in phase2_stubs_gen.c because that file is
 * regenerated. No prototype on purpose, matching the convention of the
 * generated file: the linker matches by name, and declaring the real signature
 * would drag in the header this stub exists to avoid. */

/* Item_DropCollectibleRandom PROMOTED: src/overlays/actors/ovl_En_Item00/
 * z_en_item00.c is in the build now (Phase 3, all actors). */

/* --- Phase 3: pulled in by Door_Shutter -----------------------------------
 * Real def: src/code/z_onepointdemo.c (the one-point cutscene camera, not in
 * the build). Door_Shutter calls it when a locked door unbars and ignores what
 * comes back, but the signature is written out in full and CAM_ID_NONE is
 * returned anyway: this port has already been bitten once by a `void` stub
 * whose caller evaluated the return value (the bridge glitch, hardware
 * session 3). A stub that returns the "no camera" sentinel is correct for
 * every caller, not just the ones checked today.
 *
 * CAM_ID_NONE is 0 (include/camera.h); spelled as a literal here to keep this
 * file free of the camera headers, matching the convention above. */
s32 OnePointCutscene_Attention(struct PlayState* play, struct Actor* actor) {
    (void)play;
    (void)actor;
    return 0; /* CAM_ID_NONE */
}

/* Real defs: src/code/z_demo.c (not in the build). Bg_Toki_Swd and the Bg_Haka
 * family write them when they hand a cutscene to csCtx. Defined with their real
 * u16 type rather than as a char[] placeholder for the reason given at the top
 * of this file: a write through a wrongly sized symbol takes the storage next
 * to it with it. */
u16 gCamAtSplinePointsAppliedFrame;
u16 gCamEyePointAppliedFrame;
u16 gCamAtPointAppliedFrame;

/* Cutscene scripts that live in scene data.
 *
 * The scenes themselves are native blobs loaded at runtime, so these symbols
 * have no compiled-in definition, yet the actors that trigger them
 * (Bg_Treemouth, Bg_Toki_Swd) reference them directly and assign them to
 * play->csCtx.script. The cutscene interpreter is stubbed out (Cutscene_Update
 * in phase2_stubs_gen.c), so nothing walks them today -- but "nothing reads it
 * today" is exactly the assumption that produced the zeroed display lists of
 * session 9, which were non-terminating and ran off the end of themselves.
 *
 * So each one is a VALID, EMPTY script rather than zeros: CS_HEADER(0 entries,
 * 1 frame) followed by CS_END_OF_SCRIPT. An interpreter that ever does reach
 * one stops at the first command instead of walking into whatever follows.
 * Spelled as literals to keep the cutscene headers out of this file:
 * CS_CMD_END_OF_SCRIPT is -1 (include/cutscene.h). */
/* ALL SEVEN PROMOTED (Phase 3, all actors). These scripts live in the actors'
 * own *_cutscene_data.c files -- ovl_Bg_Treemouth for the four Deku Tree ones,
 * ovl_Bg_Toki_Swd for the three Master Sword ones -- and those files are in
 * the build now, so the real scripts are present and these placeholders would
 * be duplicate symbols. The empty-script reasoning above is kept because it
 * still applies to any future stub of this kind. */

/* Boss_Sst brackets its intro with these two. Real defs: src/code/z_demo.c
 * (not in the build). Both return void, so a no-op is the whole behaviour --
 * the only consequence is that the boss's cutscene camera never runs, which is
 * already true of every cutscene here. Placed in this file rather than in
 * phase2_stubs_gen.c because that one is regenerated. */
void Cutscene_StartManual(struct PlayState* play, CutsceneContext* csCtx) {
    (void)play;
    (void)csCtx;
}
void Cutscene_StopManual(struct PlayState* play, CutsceneContext* csCtx) {
    (void)play;
    (void)csCtx;
}

/* Real def: src/code/z_message.c (das Nachrichtensystem ist nicht im Build).
 *
 * Stand vorher als `void Message_GetState(void) {}` in phase2_stubs_gen.c --
 * und DAS war ein echter Fehler, kein harmloser Platzhalter: ein void-Stub
 * schreibt v0 nicht, der Aufrufer liest also den Restwert des vorigen Aufrufs.
 * Gemessen wurde -1. Die Folge:
 *
 *     if ((Message_GetState(&play->msgCtx) == TEXT_STATE_NONE) && ...)
 *
 * in Player_UpdateInterface war JEDES Frame falsch, der ganze Block wurde
 * uebersprungen, Interface_SetDoAction nie gerufen -- und auf dem A-Knopf stand
 * fuer immer die Beschriftung aus Interface_Init ("Attack").
 *
 * TEXT_STATE_NONE ist die ehrliche Antwort, solange es kein Nachrichtensystem
 * gibt: es ist nie eine Textbox offen. 386 Aufrufstellen im Spiel lesen diesen
 * Wert; sie alle bekamen bisher Muell.
 *
 * Die eigentliche Loesung waere z_message.c im Build. Bis dahin ist das hier
 * die richtige Antwort und nicht nur die bequeme.
 *
 * Dieselbe Falle wie Message_CloseTextbox weiter unten und wie der
 * Bruecken-Glitch aus Hardware-Sitzung 3 -- dort waren es Argumente, hier ist
 * es der Rueckgabewert. Ein `void`-Stub ist nur dann sicher, wenn der Aufrufer
 * WEDER Argumente uebergibt NOCH einen Rueckgabewert liest. */
u8 Message_GetState(MessageContext* msgCtx) {
    (void)msgCtx;
    return TEXT_STATE_NONE;
}

/* Real def: src/code/z_message.c (the message system is not in the build; the
 * generated stub file already carries Message_StartTextbox, Message_GetState
 * and friends). Bg_Mizu_Water calls it when the Water Temple's water level
 * changes while a textbox is open. Returns void, and with no message system
 * there is no textbox to close, so a no-op is the whole behaviour -- but the
 * signature is written out rather than left as `void(void)`, because this file
 * includes play_state.h and a mismatching declaration would not compile, and
 * because a `void` stub whose caller passes arguments is the shape that already
 * cost this port the bridge glitch (hardware session 3). */
void Message_CloseTextbox(struct PlayState* play) {
    (void)play;
}

/* ===================================================================== *
 * Stubs, die einen WERT zurueckgeben muessen
 *
 * Alle folgenden standen in phase2_stubs_gen.c als `void X(void) {}`. Ein
 * void-Stub schreibt v0 nicht -- der Aufrufer liest damit den Restwert des
 * vorigen Aufrufs. Das ist keine Theorie: bei Message_GetState war es -1 und
 * hat Player_UpdateInterface jedes Frame stillgelegt (siehe oben).
 *
 * Jede Funktion hier hat deshalb die echte Signatur und gibt den Wert zurueck,
 * der ohne das jeweilige Subsystem die WAHRHEIT ist -- nicht den bequemsten.
 * Die Begruendung steht jeweils dabei, weil sie pro Fall verschieden ist.
 * ===================================================================== */

/* Real def: src/code/z_frame_advance.c. Der kritischste der Gruppe:
 *
 *     if (FrameAdvance_Update(&this->frameAdvCtx, &input[1])) { ... }
 *
 * in z_play.c umschliesst das GESAMTE Spiel-Update. Mit Muell in v0 lief das
 * Update bisher nur zufaellig jedes Frame. Ohne Frame-Advance-Debugmodus ist
 * die Antwort immer "ja, weiterlaufen". */
s32 FrameAdvance_Update(FrameAdvanceContext* frameAdvCtx, struct Input* input) {
    (void)frameAdvCtx;
    (void)input;
    return 1;
}

/* Real def: src/code/z_message.c (Nachrichtensystem nicht im Build). Fragt, ob
 * der Spieler die Textbox weiterblaettern will. Ohne Textboxen: nein. */
u8 Message_ShouldAdvance(struct PlayState* play) {
    (void)play;
    return 0;
}

/* Real def: src/code/z_onepointdemo.c (Cutscene-Kameras nicht im Build).
 * Der Rueckgabewert wird von den Aufrufern in `this->subCamId` gelegt und
 * spaeter als Index in play->cameraPtrs[] benutzt -- Muell dort ist ein
 * Zugriff ausserhalb des Arrays. SUB_CAM_ID_DONE ist 0, zeigt auf die
 * Hauptkamera und bedeutet den Aufrufern "die Subkamera ist fertig". */
s16 OnePointCutscene_Init(struct PlayState* play, s16 csId, s16 timer, struct Actor* actor, s16 parentCamId) {
    (void)play;
    (void)csId;
    (void)timer;
    (void)actor;
    (void)parentCamId;
    return SUB_CAM_ID_DONE;
}

/* Real def: src/code/z_camera.c-Umfeld (Cutscene-Kamerapfade). z_camera.c sagt
 * im Kommentar an der Aufrufstelle: "function returns 1 if at the end". Ohne
 * Cutscene-Pfade ist der Pfad sofort zu Ende -- 0 hiesse "laeuft noch" und
 * wuerde die Aufrufer endlos interpolieren lassen. */
s32 func_800BB2B4(void* pos, f32* roll, f32* fov, void* point, s16* keyFrame, f32* curFrame) {
    (void)pos;
    (void)roll;
    (void)fov;
    (void)point;
    (void)keyFrame;
    (void)curFrame;
    return 1;
}

/* Real def: src/code/z_jpeg.c. Aufgerufen aus Room_DecodeJpeg:
 *
 *     if (!Jpeg_Decode(data, gZBuffer, ...)) { bcopy(gZBuffer, data, ...); }
 *
 * 0 heisst ERFOLG und loest ein bcopy des Z-Buffers ueber die Raumdaten aus.
 * Mit Muell in v0 passierte das bisher zufaellig -- ein Ueberschreiber der
 * Raumdaten mit Tiefenpuffer-Inhalt. Hier ist die ehrliche Antwort "nein":
 * dieser Port dekodiert kein JPEG, die Praerender-Raeume laufen ueber die
 * Blob-Pipeline. */
s32 Jpeg_Decode(void* data, void* zbuffer, void* work, u32 workSize) {
    (void)data;
    (void)zbuffer;
    (void)work;
    (void)workSize;
    return 1;
}

/* Real def: src/code/z_bgcheck.c-Umfeld. Meldet, ob ein getragener Aktor mit
 * der Plattform mitbewegt wurde. Ohne die Transformation: nein. */
s32 DynaPolyActor_TransformCarriedActor(CollisionContext* colCtx, s32 bgId, struct Actor* carriedActor) {
    (void)colCtx;
    (void)bgId;
    (void)carriedActor;
    return 0;
}

/* Real def: src/libu64/overlay.c. Beide Aufrufer (z_effect_soft_sprite.c,
 * z_map_mark.c) verwerfen den Rueckgabewert, die Groesse ist also folgenlos --
 * die echte Signatur steht trotzdem hier, damit der naechste Leser nicht
 * denselben Weg noch einmal geht. */
size_t Overlay_Load(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd, void* allocatedRamAddr) {
    (void)vromStart;
    (void)vromEnd;
    (void)vramStart;
    (void)vramEnd;
    (void)allocatedRamAddr;
    return 0;
}

/* Real def: src/libultra/gu/... bzw. der Fault-Bildschirm. Gibt die Zahl der
 * ausgegebenen Zeichen zurueck; ohne Fault-Bildschirm sind es null. */
s32 Fault_Printf(const char* fmt, ...) {
    (void)fmt;
    return 0;
}

/* Beide werden im aktuellen Build von NIEMANDEM aufgerufen (0 Sprungziele im
 * Disassembly). Sie stehen hier trotzdem mit echter Signatur, damit sie nicht
 * beim naechsten aufgenommenen Effekt-Overlay stillschweigend zur Falle
 * werden. 0 heisst bei beiden "nicht fertig, nicht loeschen". */
s32 EffectShieldParticle_Update(void* thisx) {
    (void)thisx;
    return 0;
}
s32 EffectSpark_Update(void* thisx) {
    (void)thisx;
    return 0;
}

/* Real def: src/libultra. Liefern die Microcode-Bloecke der RSP -- auf dieser
 * Hardware gibt es keine, und niemand ruft sie derzeit auf. NULL ist die
 * ehrliche Antwort und faellt sofort auf, falls es doch jemand tut. */
u64* SysUcode_GetUCode(void) {
    return NULL;
}
u64* SysUcode_GetUCodeData(void) {
    return NULL;
}
