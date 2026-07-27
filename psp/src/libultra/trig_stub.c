/* Real N64 SDK binary-angle sine/cosine lookup tables (declared in
 * include/ultra64/gu.h) reimplemented via libm -- these aren't decomp
 * source, they ship as a prebuilt libultra archive on real N64 builds.
 * angle is in [0,0x10000) = [0,360deg); result is a fixed-point s16 in
 * [-SHT_MAX, SHT_MAX], matching what z_lib.c's Math_SinS/Math_CosS
 * (coss(angle)*SHT_MINV) expect to scale back down to a [-1,1] float. */
#include "ultra64.h"
#include "libc/math.h"
#include <math.h>

#define BINANG_TO_RAD(angle) ((f32)(angle) * (2.0f * M_PI / 65536.0f))

s16 sins(u16 angle) {
    return (s16)(sinf(BINANG_TO_RAD(angle)) * SHT_MAX);
}

s16 coss(u16 angle) {
    return (s16)(cosf(BINANG_TO_RAD(angle)) * SHT_MAX);
}
