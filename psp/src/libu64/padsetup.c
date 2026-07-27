/* Ported from reference/oot/src/libu64/padsetup.c, unmodified except
 * status[i].errno -> status[i].errno_ (see include/ultra64/controller.h for
 * why the field is renamed in this shim). The osContStartQuery/
 * osContGetQuery path below is dead in practice: our osContInit
 * (src/libultra/os_cont.c) always resolves *outMask definitively, so it
 * never comes back as the 0xFF "needs a query" sentinel. */

#include "ultra64.h"
#include "libu64/padsetup.h"

s32 PadSetup_Init(OSMesgQueue* mq, u8* outMask, OSContStatus* status) {
    s32 ret;
    s32 i;

    *outMask = 0xFF;
    ret = osContInit(mq, outMask, status);
    if (ret != 0) {
        return ret;
    }

    if (*outMask == 0xFF) {
        if (osContStartQuery(mq) != 0) {
            return 1;
        }
        osRecvMesg(mq, NULL, OS_MESG_BLOCK);
        osContGetQuery(status);

        *outMask = 0;
        for (i = 0; i < MAXCONTROLLERS; i++) {
            switch (status[i].errno) {
                case 0:
                    if (status[i].type == CONT_TYPE_NORMAL) {
                        *outMask |= 1 << i;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return 0;
}
