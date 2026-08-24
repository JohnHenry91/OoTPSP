/* Boot tracing and crash reporting for REAL hardware.
 *
 * PPSSPP has run this port for months; a PSP-1000 and a PSP-2000 both switch
 * off outright. Nothing in the port could say where, because there was no
 * exception handler and no persistent trace -- on hardware there is no
 * debugger to attach, so anything not written to the memory stick before the
 * failure is lost.
 *
 * Two instruments, because the two candidate failures leave different
 * evidence:
 *
 *   - A CPU exception (bad pointer, unaligned access, unmapped read) is
 *     catchable, and PspDiagExceptionHandler records the registers.
 *   - A power collapse is NOT catchable: the console is simply gone, mid
 *     instruction, with no chance to run anything. Only data already on the
 *     stick survives it.
 *
 * So the trace closes the file after every step. That is deliberately the
 * slow way to write a log -- it is also the only way that survives losing
 * power between two steps, and it costs a few dozen writes once per boot
 * rather than per frame (see the note about per-frame debug I/O in the
 * frame-pacing work, which cost far more than it returned). */

#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <pspdebug.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

#include "psp_hw_diag.h"
#include "zelda_arena.h"
#include "ultra64.h"

static char sLogPath[256];
static int sEnabled;

static void DiagAppend(const char* text) {
    SceUID fd;

    if (!sEnabled) {
        return;
    }
    fd = sceIoOpen(sLogPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) {
        return;
    }
    sceIoWrite(fd, text, strlen(text));
    sceIoClose(fd);
}

void PspDiag_Step(const char* step) {
    char line[192];
    unsigned int sysFree = (unsigned int)sceKernelMaxFreeMemSize();
    u32 zMaxFree = 0, zFree = 0, zAlloc = 0;

    /* sceKernelMaxFreeMemSize() alone was useless here: it read a flat 512 KB
     * on every line of the first hardware log. PSP_HEAP_SIZE_KB(-1024) claims
     * the whole partition into the application heap up front, so what this
     * reports afterwards is the sliver left in the SYSTEM partition -- a
     * constant, not a measure of what the port is consuming.
     *
     * ZeldaArena is the allocator the game itself draws scenes, actors and
     * objects from, so that is the number that actually moves. Kept alongside
     * the system figure rather than replacing it, because a failure to grow
     * the heap in the first place would show up in the system one. */
    ZeldaArena_GetSizes(&zMaxFree, &zFree, &zAlloc);
    snprintf(line, sizeof(line),
             "%-30s sys=%uK zfree=%uK zalloc=%uK zmax=%uK cpu=%dMHz\n", step,
             sysFree / 1024, (unsigned int)zFree / 1024, (unsigned int)zAlloc / 1024,
             (unsigned int)zMaxFree / 1024, scePowerGetCpuClockFrequency());
    DiagAppend(line);
}

/* Formatting lives here rather than at the call site: graph.c is decomp code
 * with a small stack, and an earlier session already lost time to a sprintf
 * stack overflow on a game thread. */
void PspDiag_Frame(unsigned int n) {
    char label[32];

    snprintf(label, sizeof(label), "frame %u", n);
    PspDiag_Step(label);
}

/* Dump raw words around a pointer.
 *
 * Deliberately untyped: the question is whether an ArenaNode header still
 * looks like one, and pulling in the arena's private struct just to read it
 * would make this probe depend on the very thing under suspicion. Raw words
 * can be decoded off-device against the header layout instead. */
void PspDiag_Hex(const char* label, const void* addr, int words) {
    char line[256];
    const unsigned int* p = (const unsigned int*)addr;
    int i;
    int n;

    n = snprintf(line, sizeof(line), "%s @%08X:", label, (unsigned int)(uintptr_t)addr);
    for (i = 0; i < words && n < (int)sizeof(line) - 12; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %08X", p[i]);
    }
    snprintf(line + n, sizeof(line) - n, "\n");
    DiagAppend(line);
}

void PspDiag_Note(const char* fmt, unsigned int a, unsigned int b) {
    char line[160];

    snprintf(line, sizeof(line), fmt, a, b);
    DiagAppend(line);
}

/* NOTE: no exception handler.
 *
 * pspDebugInstallErrorHandler() was tried and does not link: it resolves to
 * sceKernelRegisterDefaultExceptionHandler, which is KERNEL mode, while this
 * module runs in user mode (PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER)). Catching
 * exceptions would mean shipping a separate kernel PRX.
 *
 * That is a smaller loss than it looks. The symptom being chased is the
 * console switching OFF, and no handler can run after power is gone -- only
 * data already written to the stick survives, which is exactly what the trace
 * below provides. If the failure turns out to be a catchable exception after
 * all (the trace will show it stopping at a repeatable step), a kernel PRX is
 * worth building then. */

void PspDiag_Init(const char* baseDir) {
    SceUID fd;

    sLogPath[0] = '\0';
    if (baseDir != NULL && baseDir[0] != '\0') {
        snprintf(sLogPath, sizeof(sLogPath), "%soot_boot.log", baseDir);
    } else {
        strcpy(sLogPath, "oot_boot.log");
    }
    sEnabled = 1;

    /* Truncate: the log must describe THIS boot, not be an ever-growing pile
     * in which the interesting last line is impossible to find. */
    fd = sceIoOpen(sLogPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        sEnabled = 0; /* read-only medium or bad path -- stay silent */
        return;
    }
    sceIoClose(fd);

    PspDiag_Step("diag-init");
}
