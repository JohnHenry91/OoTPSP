#ifndef PSP_SCENE_MENU_H
#define PSP_SCENE_MENU_H

#include "ultra64.h"

struct PlayState;

/* Non-zero while the warp menu is on screen. os_cont.c reads this to blank the
 * N64 button word and stick, so the D-Pad presses driving the menu do not also
 * reach the game as C-buttons. */
extern s32 gPspSceneMenuOpen;

void PspSceneMenu_Update(struct PlayState* play);
void PspSceneMenu_DrawBackdrop(void);

/* Frame-pacing HUD, toggled with TRIANGLE. Drawn from the same place as the
 * backdrop and independently of whether the warp menu is open. */
void PspSceneMenu_DrawHud(void);

#endif
