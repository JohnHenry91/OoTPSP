/* Stand-ins for real N64 VI hardware register pokes and a handful of
 * libultra OS globals/functions with no PSP equivalent. None of this is on
 * the Phase 1 hot path (framebuffer swap is handled directly by the PSP
 * main loop via sceGu, not through Sched/VI) -- these exist purely so
 * general-purpose engine code (z_view.c, sched.c, z_sram.c's controller
 * pak probing, etc.) that unconditionally references them still links. */
#include "ultra64.h"
#include "ultra64/motor.h"
#include "ultra64/os_vi.h"
#include "ultra64/os_system.h"
#include "cic6105.h"

void osViExtendVStart(u32 value) {
}

void osViBlack(u8 active) {
}

void osViSetMode(OSViMode* mode) {
}

void osViSetSpecialFeatures(u32 func) {
}

void osViSetYScale(f32 scale) {
}

void osViSetXScale(f32 value) {
}

/* Dummy target -- never dereferenced meaningfully since osViSetMode is a
 * no-op above, only needs to exist as a linkable address. */
OSViMode osViModePalLan1;

s32 osContSetCh(u8 ch) {
    return 0;
}

void CIC6105_EnableAudio(void) {
}

s32 __osMotorAccess(OSPfs* pfs, s32 vibrate) {
    return 0;
}

s32 osMotorInit(OSMesgQueue* ctrlrqueue, OSPfs* pfs, s32 channel) {
    return -1; /* no rumble pak connected */
}

s32 osTvType = 1; /* NTSC-ish 60Hz timing is the better default for a 60fps-target PSP port */
u32 osMemSize = 8 * 1024 * 1024; /* report N64-plausible size; nothing on PSP sizes off physical RAM via this */
s32 osAppNMIBuffer[0x10];
s32 osResetType = 0; /* 0 = cold reset -- no such concept on PSP, always report cold */

/* Real RSP task dispatch (src/libultra/os/sptask*.c) -- only ever called
 * from Sched_RunTask (src/code/sched.c), which is itself dead code on PSP
 * (gScheduler's task queue is never fed; see plan decision #6, graph.c's
 * Graph_TaskSet00). Exists purely so sched.c links. */
void osSpTaskLoad(OSTask* intp) {
}
void osSpTaskStartGo(OSTask* tp) {
}
void osSpTaskYield(void) {
}
OSYieldResult osSpTaskYielded(OSTask* task) {
    return 0;
}

void* osViGetCurrentFramebuffer(void) {
    return NULL;
}
void osViSwapBuffer(void* frameBufPtr) {
}
