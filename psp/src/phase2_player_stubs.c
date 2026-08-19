/* Phase 2 stubs: engine subsystems that real z_player.c/z_player_lib.c call
 * into but are entirely out of scope for the "stand and walk in a room"
 * milestone -- audio, magic, items, effects (EffectSs*), and cutscene
 * playback. All are safe no-ops; functions with a return value return a
 * harmless default (0/NULL/false) since these paths are dead for a fresh
 * debug save with no combat/items/cutscenes in play. Revisit individually
 * once each subsystem actually comes into scope. */
#include "ultra64.h"

struct PlayState;
struct Actor;
struct Vec3f;
struct Color_RGBA8;
struct EffectBlure;
struct CollisionContext;
struct EnItem00;

/* AudioOcarina_SetInstrument/Audio_PlayFanfare/Audio_SetBaseFilter/
 * Audio_SetBgmEnemyVolume/Audio_SetBgmVolumeOffDuringFanfare/
 * Audio_SetBgmVolumeOnDuringFanfare/Audio_SetCodeReverb/Audio_SetSequenceMode/
 * Audio_StopBgmAndFanfare/Audio_StopSfxById PROMOTED to the real
 * src/audio/game/general.c (Phase 4 audio bring-up, see Makefile.psp).
 *
 * Promoted to the real src/code/z_bg_item.c (Phase 3, enabled by the first
 * dyna-poly actors). Stub removed -- keeping it would be a duplicate symbol. */

void EffectBlure_AddVertex(void* this, void* p1, void* p2) {}
void EffectSsBlast_SpawnWhiteShockwave(void* play, void* pos, void* velocity, void* accel) {}
void EffectSsBubble_Spawn(void* play, void* pos, f32 yPosOffset, f32 yPosRandScale, f32 xzPosRandScale, f32 arg5) {}
void EffectSsFhgFlash_SpawnShock(void* play, void* actor, void* pos, s16 scale, u8 param) {}
void EffectSsFireTail_SpawnFlameOnPlayer(void* play, f32 scale, s16 bodyPart, f32 colorIntensity) {}
void EffectSsGFire_Spawn(void* play, void* pos) {}
void EffectSsGSplash_Spawn(void* play, void* pos, void* primColor, void* envColor, s16 type, s32 arg6) {}
void EffectSsIcePiece_SpawnBurst(void* play, void* refPos, f32 scale) {}
void EffectSsStick_Spawn(void* play, void* pos, s16 yaw) {}

/* Environment_ChangeLightSetting / Environment_WarpSongLeave now come from the
 * real src/code/z_kankyo.c (see Makefile.psp). */

void func_8002836C(void* play, void* pos, void* velocity, void* accel, void* primColor, void* envColor, f32 scale,
                    s32 arg7, s32 arg8, s32 arg9) {}
void func_800849EC(void* play) {}
/* func_800F4138/func_800F4190 PROMOTED to the real src/audio/game/general.c
 * (Phase 4 audio bring-up, see Makefile.psp). */

void GetItem_Draw(void* play, s16 giDrawId) {}

s32 Health_ChangeBy(void* play, s16 amount) {
    return 0;
}

u32 Health_IsCritical(void) {
    return 0;
}

void Interface_SetDoAction(void* play, u16 action) {}
void Interface_SetNaviCall(void* play, u16 naviCallState) {}
void Interface_SetSubTimerToFinalSecond(void* play) {}

void Inventory_ChangeAmmo(s16 item, s16 ammoChange) {}
s32 Inventory_ConsumeFairy(void* play) {
    return 0;
}
void Inventory_UpdateBottleItem(void* play, u8 item, u8 button) {}

u8 Item_CheckObtainability(u8 item) {
    return item;
}
void* Item_DropCollectible(void* play, void* spawnPos, s16 params) {
    return NULL;
}

void Magic_Fill(void* play) {}
s32 Magic_RequestChange(void* play, s16 amount, s16 type) {
    return 0;
}

void Message_StartOcarina(void* play, u16 ocarinaActionId) {}

s16 OnePointCutscene_EndCutscene(void* play, s16 subCamId) {
    return 0;
}
