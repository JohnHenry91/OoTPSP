#ifndef PSP_RAW_INPUT_H
#define PSP_RAW_INPUT_H

#include "ultra64.h"

/**
 * Raw PSP button state, sampled once per frame by osContGetReadData
 * (psp/src/libultra/os_cont.c) at the same moment it builds the N64 button
 * word.
 *
 * This exists for port-private UI only. The N64 controller has no SELECT, and
 * the game already uses every button that IS mapped, so the warp menu needs a
 * channel that does not go through the N64 button word at all. Reading pspctrl
 * a second time from the menu would work but would sample at a different
 * instant than the game does, which is exactly the kind of thing that produces
 * a "sometimes the press is missed" bug.
 *
 * Values are PSP_CTRL_* bits from <pspctrl.h>. Prev holds last frame's state so
 * callers can do their own edge detection.
 */
extern u32 gPspRawButtons;
extern u32 gPspRawButtonsPrev;

#define PSP_RAW_PRESSED(mask) ((gPspRawButtons & (mask)) && !(gPspRawButtonsPrev & (mask)))

#endif
