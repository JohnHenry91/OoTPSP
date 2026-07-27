/* osGetTime()/osGetCount() report ticks at the N64's 46.875MHz CPU counter
 * rate (OS_CPU_COUNTER, see ultra64/convert.h) — not real PSP clock ticks —
 * so that OS_CYCLES_TO_USEC()/OS_USEC_TO_CYCLES() conversions used
 * throughout the decomp keep working unmodified. Same approach as
 * Shipwright's libultraship (src/libultraship/libultra/os.cpp), which
 * derives the same rate from std::chrono; here it's derived from PSPSDK's
 * microsecond-resolution system clock instead.
 *
 * osSetTimer/osStopTimer are no-ops (as in libultraship) — nothing currently
 * ported drives the N64 COUNTER-interrupt timer queue they'd need. */

#include <pspkernel.h>

#include "ultra64.h"

static u64 sTimeOffsetUsec = 0;

/* sceKernelGetSystemTimeWide(), not the RTC: monotonic microseconds since
 * boot, unaffected by wall-clock/timezone changes. */
static u64 GetSystemTimeUsec(void) {
    return (u64)sceKernelGetSystemTimeWide();
}

void osSetTime(OSTime time) {
    sTimeOffsetUsec = GetSystemTimeUsec() - OS_CYCLES_TO_USEC(time);
}

OSTime osGetTime(void) {
    return OS_USEC_TO_CYCLES(GetSystemTimeUsec() - sTimeOffsetUsec);
}

u32 osGetCount(void) {
    return (u32)osGetTime();
}

s32 osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    return 0;
}

s32 osStopTimer(OSTimer* timer) {
    return 0;
}
