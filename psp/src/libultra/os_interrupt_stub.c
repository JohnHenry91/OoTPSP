/* osSetIntMask/osAfterPreNMI stand-ins -- see ultra64.h for why these are
 * no-ops on PSP for Phase 1 (IrqMgr not spawned as a real thread). */
#include "ultra64.h"

OSIntMask osSetIntMask(OSIntMask mask) {
    return OS_IM_NONE;
}

s32 osAfterPreNMI(void) {
    return 0;
}
