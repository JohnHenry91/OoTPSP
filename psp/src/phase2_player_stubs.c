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

/* EffectBlure_AddVertex PROMOTED to src/code/z_eff_blure.c (Phase 3). */

/* The nine EffectSs*_Spawn stubs that stood here are PROMOTED to the real
 * src/code/z_effect_soft_sprite_old_init.c, which came into the build with the
 * Bg_* scenery actors (see Makefile.psp). Player's water bubbles, ice shards
 * and stick fragments are real effects now rather than no-ops. */

/* Environment_ChangeLightSetting / Environment_WarpSongLeave now come from the
 * real src/code/z_kankyo.c (see Makefile.psp). */

/* func_8002836C promoted with the block above. */
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
/* Item_DropCollectible PROMOTED to the real src/code/z_en_item00.c
 * (Phase 3) -- it now really does drop the collectible instead of
 * returning NULL. */

void Magic_Fill(void* play) {}
s32 Magic_RequestChange(void* play, s16 amount, s16 type) {
    return 0;
}

void Message_StartOcarina(void* play, u16 ocarinaActionId) {}

s16 OnePointCutscene_EndCutscene(void* play, s16 subCamId) {
    return 0;
}
