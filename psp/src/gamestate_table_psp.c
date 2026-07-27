/* Hand-built replacement for src/code/z_game_dlftbls.c, used only for
 * TARGET_PSP. The real file builds gGameStateOverlayTable from
 * tables/gamestate_table.h via macros that reference every game state's
 * Init/Destroy AND real linker-placed overlay segment symbols
 * (_ovl_xxxSegmentStart/End) for ALL states -- including GAMESTATE_PLAY,
 * whose Init/Destroy live in the actor/collision/camera engine (z_play.c),
 * hugely out of scope for Phase 1 (see plan: static-link-everything means
 * no overlay relocation is needed, so those segment symbols would only
 * ever be dummy placeholders anyway).
 *
 * Overlay_LoadGameState (src/code/z_DLF.c) already treats vramStart == NULL
 * as "not an overlay, nothing to relocate" and returns immediately without
 * touching init/destroy -- so setting vramStart/vramEnd/file to NULL/unset
 * for every entry here (matching how GAMESTATE_SETUP, an internal
 * non-overlay state, already works in the real table) makes overlay
 * loading a true no-op for all of them, real segment symbols or not.
 *
 * Only Setup and ConsoleLogo (Phase 1's boot target) have real Init/Destroy
 * wired up; the rest are stubs that exist purely so this table links and so
 * Graph_GetNextGameState's linear Init-function-pointer search (src/code/
 * graph.c) has something to compare against -- they are never reached. */
#include "z_game_dlftbls.h"
#include "setup_state.h"
#include "console_logo_state.h"
#include "map_select_state.h"
#include "play_state.h"
#include "title_setup_state.h"
#include "file_select_state.h"

void Setup_Init(GameState* thisx);
void Setup_Destroy(GameState* thisx);
void ConsoleLogo_Init(GameState* thisx);
void ConsoleLogo_Destroy(GameState* thisx);
/* Real impl: src/overlays/gamestates/ovl_opening/z_opening.c (Phase 2). */
void TitleSetup_Init(GameState* thisx);
void TitleSetup_Destroy(GameState* thisx);
/* Real impl: src/code/z_play.c (Phase 2 -- the actual gameplay engine). */
void Play_Init(GameState* thisx);
void Play_Destroy(GameState* thisx);

/* GameState_Init (src/code/game.c) unconditionally sets gameState->running
 * = 1 BEFORE calling a state's init function, so Graph_ThreadEntry's
 * `while (GameState_IsRunning(gameState)) Graph_Update(...)` loop runs
 * regardless of whether init actually set up a real main/destroy -- and
 * Graph_Update calls `gameState->main(gameState)` unconditionally.
 * Without this, transitioning into any of these stub states (e.g. once
 * ConsoleLogo's fade timer expires and it transitions onward) calls
 * through a NULL function pointer and halts PPSSPP with "Bad execution
 * access ... 00000000". A shared no-op main/destroy makes an unported
 * stub state a harmless infinite idle instead of a crash. */
static void StubState_Main(GameState* thisx) {
}
static void StubState_Destroy(GameState* thisx) {
}

void MapSelect_Init(GameState* thisx) {
    thisx->main = StubState_Main;
    thisx->destroy = StubState_Destroy;
}
void MapSelect_Destroy(GameState* thisx) {
}
void FileSelect_Init(GameState* thisx) {
    thisx->main = StubState_Main;
    thisx->destroy = StubState_Destroy;
}
void FileSelect_Destroy(GameState* thisx) {
}

GameStateOverlay gGameStateOverlayTable[GAMESTATE_ID_MAX] = {
    [GAMESTATE_SETUP] = {
        .init = Setup_Init,
        .destroy = Setup_Destroy,
        .instanceSize = sizeof(SetupState),
    },
    [GAMESTATE_MAP_SELECT] = {
        .init = MapSelect_Init,
        .destroy = MapSelect_Destroy,
        .instanceSize = sizeof(MapSelectState),
    },
    [GAMESTATE_CONSOLE_LOGO] = {
        .init = ConsoleLogo_Init,
        .destroy = ConsoleLogo_Destroy,
        .instanceSize = sizeof(ConsoleLogoState),
    },
    [GAMESTATE_PLAY] = {
        .init = Play_Init,
        .destroy = Play_Destroy,
        .instanceSize = sizeof(PlayState),
    },
    [GAMESTATE_TITLE_SETUP] = {
        .init = TitleSetup_Init,
        .destroy = TitleSetup_Destroy,
        .instanceSize = sizeof(TitleSetupState),
    },
    [GAMESTATE_FILE_SELECT] = {
        .init = FileSelect_Init,
        .destroy = FileSelect_Destroy,
        .instanceSize = sizeof(FileSelectState),
    },
};
