/* N64 controller -> PSP pad shim. Backed directly by pspctrl instead of
 * emulating the PIF-RAM protocol that src/libultra/io talks to real N64
 * controller hardware — that whole layer (contreaddata.c, controller.c,
 * pfs*.c memory-pak/rumble-pak polling) is superseded by this file and
 * won't be ported.
 *
 * Button mapping (user-confirmed 2026-07-22, see PORTING.md): PSP has no
 * second stick or C-buttons, but does have a D-Pad that's otherwise idle
 * once the analog stick drives movement, so:
 *   Analog stick -> N64 analog stick
 *   D-Pad Up/Down/Left/Right -> C-Up/C-Down/C-Left/C-Right
 *   Cross -> A, Circle -> B, L -> Z (target), R -> R (camera), Start -> Start
 * Triangle/Square/Select are unmapped for now.
 *
 * Only port 0 is real; ports 1-3 report CONT_ERR_NO_CONTROLLER, matching a
 * single-player N64 setup with controllers 2-4 unplugged. */

#include <pspctrl.h>
#include <string.h>

#include "ultra64.h"
#include "ultra64/controller.h"
#include "ultra64/rcp.h"
#include "controller.h"

s32 osContInit(OSMesgQueue* mq, u8* ctlBitfield, OSContStatus* status) {
    s32 i;

    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    *ctlBitfield = 0x1; /* only controller 0 present */

    status[0].type = CONT_TYPE_NORMAL;
    status[0].status = 0;
    status[0].errno = 0;
    for (i = 1; i < MAXCONTROLLERS; i++) {
        status[i].type = 0;
        status[i].status = 0;
        status[i].errno = CONT_ERR_NO_CONTROLLER;
    }

    return 0;
}

/* Real N64 hardware kicks off an async joybus DMA read here and signals
 * completion later via an SI interrupt message. PSP pad reads are already
 * synchronous/instant (sceCtrlReadBufferPositive, called from
 * osContGetReadData below), so "start" and "done" collapse to the same
 * instant -- post the completion message immediately so callers like
 * PadMgr_HandleRetrace (src/code/padmgr.c), which blocks on this queue for
 * real N64 async completion, don't hang. Mirrors osContStartQuery below,
 * which already did this. */
s32 osContStartReadData(OSMesgQueue* mq) {
    if (mq != NULL) {
        osJamMesg(mq, NULL, OS_MESG_NOBLOCK);
    }
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    SceCtrlData sceData;
    u16 button;
    s32 i;

    memset(pad, 0, sizeof(OSContPad) * MAXCONTROLLERS);
    /* PadMgr_UpdateInputs (src/code/padmgr.c) switches on this field
     * expecting real controller-read RX error codes (CHNL_ERR_OVERRUN>>4=4,
     * CHNL_ERR_NORESP>>4=8, from include/ultra64/rcp.h) with an unhandled-
     * default case that calls Fault_AddHungupAndCrash. CONT_ERR_NO_CONTROLLER
     * (=1, from PFS_ERR_NOPACK -- a controller-PAK/rumble error code, a
     * different namespace entirely) doesn't match any of those and used to
     * hit that default case, hanging every frame in what looked like a
     * deadlock but was actually a real (if PSP-shim-only) crash-handler
     * infinite loop. CHNL_ERR_NORESP is the correct "nothing responded on
     * this port" code, matching real N64 behavior for unplugged controllers. */
    for (i = 1; i < MAXCONTROLLERS; i++) {
        pad[i].errno = (CHNL_ERR_NORESP >> 4);
    }

    sceCtrlReadBufferPositive(&sceData, 1);

    button = 0;
    if (sceData.Buttons & PSP_CTRL_CROSS) button |= BTN_A;
    if (sceData.Buttons & PSP_CTRL_CIRCLE) button |= BTN_B;
    if (sceData.Buttons & PSP_CTRL_LTRIGGER) button |= BTN_Z;
    if (sceData.Buttons & PSP_CTRL_RTRIGGER) button |= BTN_R;
    if (sceData.Buttons & PSP_CTRL_START) button |= BTN_START;
    if (sceData.Buttons & PSP_CTRL_UP) button |= BTN_CUP;
    if (sceData.Buttons & PSP_CTRL_DOWN) button |= BTN_CDOWN;
    if (sceData.Buttons & PSP_CTRL_LEFT) button |= BTN_CLEFT;
    if (sceData.Buttons & PSP_CTRL_RIGHT) button |= BTN_CRIGHT;

    pad[0].button = button;
    pad[0].stick_x = (s8)((s32)sceData.Lx - 128);
    /* PSP analog Y grows downward; N64 stick_y is positive when pushed
     * away from the player (forward/up), so invert. */
    pad[0].stick_y = (s8)(128 - (s32)sceData.Ly);
}

/* Real N64 hardware uses these to auto-detect which of the 4 ports have a
 * controller plugged in when osContInit's initial probe is inconclusive.
 * Our osContInit above always reports port 0 definitively, so callers
 * (e.g. libu64/padsetup.c) never actually take the branch that reaches
 * these — kept only so such call sites still link. Still jams a dummy
 * reply: os_mesg.c's osRecvMesg genuinely blocks now (real PSP semaphores),
 * so if this dead path is ever accidentally exercised it degrades to an
 * instant no-op instead of hanging the calling thread forever. */
s32 osContStartQuery(OSMesgQueue* mq) {
    osJamMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetQuery(OSContStatus* data) {
}
