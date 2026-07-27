/* Real (not stubbed) implementation: osSyncPrintf is compiled out entirely
 * on most call sites under our build's DEBUG_FEATURES=0 (PRINTF collapses to
 * (void)0 — see include/printf.h), but src/libu64/stackcheck.c's
 * StackCheck_Check calls it directly and unconditionally in the
 * PLATFORM_N64 branch we build against, so it needs to actually work rather
 * than just link. */

#include <stdarg.h>
#include <stdio.h>

#include <pspdebug.h>

void osSyncPrintf(const char* fmt, ...) {
    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    pspDebugScreenPrintf("%s", buf);
}
