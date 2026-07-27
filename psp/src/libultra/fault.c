/* Minimal stand-in for the real N64 fault.c (hundreds of lines: a whole
 * framebuffer-based crash screen with register dumps, stack traces, a
 * client-registration system for per-subsystem crash callbacks). All we
 * need right now is that Fault_AddHungupAndCrash exists and stops the
 * system visibly instead of corrupting memory further or silently hanging —
 * DmaMgr_Init's boot-segment sanity check and the (currently unreachable,
 * since our DMA table is always uncompressed) DMA_ERROR path in
 * src/boot/z_std_dma.c both call it. Revisit with a real crash screen once
 * something can actually hit this in practice. */

#include <pspdebug.h>
#include <pspiofilemgr.h>

#include "ultra64.h"
#include "fault.h"

NORETURN void Fault_AddHungupAndCrash(const char* file, int line) {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("FAULT: %s:%d\n", file, line);

    /* pspDebugScreenPrintf renders nothing in our current PPSSPP setup (see
     * reference_oot_psp_toolchains memory) -- without this, a real fault
     * looks pixel-identical to a benign black screen / still-loading state.
     * Mirror to the same ms0:/*.txt file-log channel every other diagnostic
     * in this bring-up uses, so a fault is unambiguous. */
    {
        char msg[96];
        int len;
        SceUID fd;

        len = 0;
        {
            const char* p = "FAULT: ";
            while (*p) {
                msg[len++] = *p++;
            }
            p = file;
            while (*p && len < 60) {
                msg[len++] = *p++;
            }
            msg[len++] = ':';
            {
                char numBuf[12];
                int n = line;
                int i = 0;
                if (n == 0) {
                    numBuf[i++] = '0';
                }
                while (n > 0 && i < 11) {
                    numBuf[i++] = '0' + (n % 10);
                    n /= 10;
                }
                while (i > 0) {
                    msg[len++] = numBuf[--i];
                }
            }
            msg[len++] = '\n';
        }

        fd = sceIoOpen("ms0:/fault.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, msg, len);
            sceIoClose(fd);
        }
    }

    for (;;) {
    }
}
