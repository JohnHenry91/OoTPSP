/* Real z_player_call.c is an N64-overlay indirection wrapper (PlayerCall_Init
 * etc. dynamically load a separate relocatable "player_actor" overlay via
 * KaleidoManager_GetRamAddr/Overlay_Load before calling through). We haven't
 * ported that real-relocation overlay loader to PSP -- Phase 2's flattening
 * strategy links z_player.c's Player_Init/Destroy/Update/Draw directly
 * instead (matching DEFINE_ACTOR_INTERNAL's "already resident" fast path,
 * the same trick used for gActorOverlayTable/gGameStateOverlayTable), so
 * this profile calls them with no indirection. Values (flags, alloc type,
 * etc.) copied from the real src/code/z_player_call.c's Player_Profile. */
#include "actor.h"
#include "actor_profile.h"
#include "play_state.h"
#include "player.h"

#define FLAGS                                                                                \
    (ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE | ACTOR_FLAG_UPDATE_CULLING_DISABLED | \
     ACTOR_FLAG_DRAW_CULLING_DISABLED | ACTOR_FLAG_UPDATE_DURING_OCARINA | ACTOR_FLAG_CAN_PRESS_SWITCHES)

void Player_Init(Actor* thisx, PlayState* play);
void Player_Destroy(Actor* thisx, PlayState* play);
void Player_Update(Actor* thisx, PlayState* play);
void Player_Draw(Actor* thisx, PlayState* play);

ActorProfile Player_Profile = {
    /**/ ACTOR_PLAYER,
    /**/ ACTORCAT_PLAYER,
    /**/ FLAGS,
    /**/ OBJECT_GAMEPLAY_KEEP,
    /**/ sizeof(Player),
    /**/ Player_Init,
    /**/ Player_Destroy,
    /**/ Player_Update,
    /**/ Player_Draw,
};
