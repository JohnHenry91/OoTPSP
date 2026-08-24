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
 * Originally this closed the file after every single step, on the theory
 * that only an immediately-durable write survives a power cut. It does --
 * but every hardware run also showed the trace stopping after exactly 19
 * entries, no matter what code came after entry 19 (verified three times by
 * placing plain no-op markers right behind the last line and never seeing
 * them appear, even though `strings` on the ELF shows the text is there).
 * A fixed COUNT rather than a fixed CODE LOCATION means the trace itself was
 * the thing setting the clock: at ~50ms per open+write+close, 19 of them is
 * about a second of wall time, and whatever actually kills the console may
 * simply be a timeout that lands around then regardless of where the game
 * has gotten to. The instrument was measuring itself.
 *
 * So durability is now RAM-first: PspDiag_Step appends to a small ring
 * buffer with no I/O at all, and only every PSP_DIAG_FLUSH_EVERY steps (or
 * an explicit PspDiag_Flush() call) does one open+write+close actually
 * happen. That trades a handful of trailing entries (whatever is unflushed
 * when power dies) for the trace no longer dominating the timeline it is
 * trying to measure -- see the note about per-frame debug I/O in the
 * frame-pacing work, which cost far more than it returned for the same
 * reason. */

#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <pspsysmem.h>
#include <pspdebug.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

#include "psp_hw_diag.h"

#if PSP_DIAG_ENABLED
#include "zelda_arena.h"
#include "ultra64.h"

static char sLogPath[256];
static int sEnabled;

/* Pending, not-yet-written trace text. 2 KB comfortably holds
 * PSP_DIAG_FLUSH_EVERY lines (each line is <192 bytes, see PspDiag_Step). */
#define PSP_DIAG_RING_SIZE 2048
#define PSP_DIAG_FLUSH_EVERY 8
static char sRingBuf[PSP_DIAG_RING_SIZE];
static int sRingLen;
static int sStepsSinceFlush;

/* Wall clock, microseconds, captured in PspDiag_Init. Every entry is stamped
 * with the milliseconds elapsed since then, and that stamp is the whole point
 * of this rebuild: the open question is whether the trace stops because
 * execution stops (a fixed CODE point) or because the console dies at a fixed
 * WALL TIME regardless of how far the game got. With per-line I/O costing
 * ~50 ms the two were indistinguishable -- 19 entries was both "the 19th call
 * site" and "one second in". Cheap entries plus a timestamp separate them:
 * if the trace now reaches entry 200 and still stops near the same
 * millisecond, it is time; if it stops at the same NAME with a much smaller
 * timestamp, it is code. */
static u64 sTimeBaseUsec;

/* Bounds of the stack the game actually runs on.
 *
 * The port does NOT spawn a graph thread: main.c's TARGET_PSP branch calls
 * Graph_ThreadEntry() directly, so the whole OoT engine executes on the
 * module's MAIN thread. The earlier "stacks 32 KB -> 128 KB" fix only touched
 * threads created with osCreateThread (padmgr, DmaMgr, audio) -- the main
 * thread was never part of it and still gets whatever the firmware hands a
 * module that defines no sce_newlib_stack_kb_size (psp-nm shows ours as an
 * undefined weak symbol). reference/oot-psp-z2442 runs its game loop on the
 * main thread too and asks for 1 MB explicitly.
 *
 * So "stack ruled out" was never actually measured for the stack that
 * matters. Every trace entry now carries the live stack pointer and how much
 * room is left below it, which turns that from an argument into a number. */
volatile unsigned int gPspDiagBeats[PSP_DIAG_BEAT_COUNT];

static unsigned int sStackBottom;
static unsigned int sStackSize;

static unsigned int DiagStackPointer(void) {
    unsigned int sp;

    __asm__ volatile("move %0, $sp" : "=r"(sp));
    return sp;
}

void PspDiag_Flush(void) {
    SceUID fd;

    if (!sEnabled || sRingLen == 0) {
        return;
    }
    fd = sceIoOpen(sLogPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, sRingBuf, sRingLen);
        sceIoClose(fd);
    }
    sRingLen = 0;
    sStepsSinceFlush = 0;
}

static void DiagAppend(const char* text) {
    size_t len;
    char stamp[16];
    size_t stampLen;
    unsigned int ms;

    if (!sEnabled) {
        return;
    }
    ms = (unsigned int)(((u64)sceKernelGetSystemTimeWide() - sTimeBaseUsec) / 1000);
    stampLen = (size_t)snprintf(stamp, sizeof(stamp), "[%6u] ", ms);
    len = strlen(text) + stampLen;
    /* Flush first if this line would overflow the ring, so no line is ever
     * truncated -- losing a whole entry to save one early flush isn't worth
     * it. */
    if (sRingLen + (int)len > PSP_DIAG_RING_SIZE) {
        PspDiag_Flush();
    }
    if ((int)len > PSP_DIAG_RING_SIZE) {
        return; /* a single line bigger than the whole ring -- give up on it */
    }
    memcpy(sRingBuf + sRingLen, stamp, stampLen);
    sRingLen += (int)stampLen;
    memcpy(sRingBuf + sRingLen, text, len - stampLen);
    sRingLen += (int)(len - stampLen);

    sStepsSinceFlush++;
    if (sStepsSinceFlush >= PSP_DIAG_FLUSH_EVERY) {
        PspDiag_Flush();
    }
}

void PspDiag_Step(const char* step) {
    char line[192];
    unsigned int sysFree = (unsigned int)sceKernelMaxFreeMemSize();
    unsigned int sp = DiagStackPointer();
    unsigned int stackLeft = (sStackBottom != 0 && sp > sStackBottom) ? (sp - sStackBottom) : 0;
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
             "%-26s sp=%08X stkleft=%uK aud=%u/%u pad=%u dma=%u sys=%uK zfree=%uK zalloc=%uK\n", step,
             sp, stackLeft / 1024, gPspDiagBeats[PSP_DIAG_BEAT_AUDIO],
             gPspDiagBeats[PSP_DIAG_BEAT_AUDIO_STAGE], gPspDiagBeats[PSP_DIAG_BEAT_PADMGR],
             gPspDiagBeats[PSP_DIAG_BEAT_DMA], sysFree / 1024, (unsigned int)zFree / 1024,
             (unsigned int)zAlloc / 1024);
    DiagAppend(line);
}

/* Step, then force the ring buffer out to the stick immediately.
 *
 * The periodic flush is what makes ordinary entries cheap, but it also means
 * the last few entries before a power cut are lost -- the previous hardware
 * run stopped on a flush boundary (16 = 2 x PSP_DIAG_FLUSH_EVERY) with up to
 * seven entries still in RAM, which leaves the exact stopping point ambiguous
 * across a seven-call span. Inside that span, and only there, pay the ~50 ms
 * again so every entry is durable and the fault can be pinned to one call. */
/* -DPSP_DIAG_SYNC=0 turns the force-flush back into a plain buffered append.
 *
 * Testing the instrument as the suspect. Every hardware run that died has one
 * property in common that no surviving run has: a burst of back-to-back
 * open/write/close cycles on the Memory Stick, milliseconds apart. The
 * original per-line trace flushed on every entry and died after 19 of them;
 * the force-flushed teardown window flushes five times in 80 ms and dies on
 * the fifth. Meanwhile bring-up levels 0-9 all wrote to the same file on the
 * same stick for six seconds and came back clean -- but spaced 500 ms apart,
 * never in a burst.
 *
 * It also fits the shape of the failure that nothing else has explained: the
 * console does not fault, it loses power, and it does so while executing
 * functions that are literally empty (Setup_Destroy, SpeedMeter_Destroy,
 * VisCvg_Destroy and both phase1_stubs.c stubs all have no body at all). Code
 * that does nothing cannot crash; the I/O between the probes can.
 *
 * This is not yet a conclusion -- the console was switching off before this
 * file existed, so at most this is A killer, not necessarily THE one. But it
 * is one build flag away from being settled either way. */
void PspDiag_StepSync(const char* step) {
    PspDiag_Step(step);
#if !defined(PSP_DIAG_SYNC) || PSP_DIAG_SYNC
    PspDiag_Flush();
#endif
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

/* Idle for `seconds`, logging a heartbeat every ~30 vblanks, then exit to the
 * XMB. Uses sceDisplayWaitVblankStart rather than a busy loop so the console
 * is in a normal running state while it waits -- a spin at full CPU would be
 * a different power profile than the one being tested. */
void PspDiag_Park(const char* stage, int seconds) {
    int frames = seconds * 60;
    int i;

    PspDiag_Step(stage);
    PspDiag_Flush();
    for (i = 0; i < frames; i++) {
        sceDisplayWaitVblankStart();
        if ((i % 30) == 0) {
            char label[32];

            snprintf(label, sizeof(label), "park %d", i / 30);
            PspDiag_Step(label);
            PspDiag_Flush();
        }
    }
    PspDiag_Step("park-done-exiting");
    PspDiag_Flush();
    sceKernelExitGame();
}

void PspDiag_Init(const char* baseDir) {
    SceUID fd;

    sLogPath[0] = '\0';
    if (baseDir != NULL && baseDir[0] != '\0') {
        snprintf(sLogPath, sizeof(sLogPath), "%soot_boot.log", baseDir);
    } else {
        strcpy(sLogPath, "oot_boot.log");
    }
    sEnabled = 1;
    sTimeBaseUsec = (u64)sceKernelGetSystemTimeWide();

    {
        SceKernelThreadInfo info;

        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(0, &info) >= 0) {
            sStackBottom = (unsigned int)(uintptr_t)info.stack;
            sStackSize = (unsigned int)info.stackSize;
        }
    }

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

#endif /* PSP_DIAG_ENABLED */
