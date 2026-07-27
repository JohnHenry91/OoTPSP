/* Real (not stubbed) implementation of the one libc64/sleep.h function
 * actually linked in: Sleep_Msec, referenced (but never called — dead 64DD
 * code path, see include/n64dd.h) by DmaMgr_AudioDmaHandler. */

#include <pspthreadman.h>

#include "libc64/sleep.h"

void Sleep_Msec(u32 ms) {
    sceKernelDelayThread(ms * 1000);
}
