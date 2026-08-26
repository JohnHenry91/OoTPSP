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
#include "psp_blob_assets.h"

/* Refused Zelda-arena allocations; defined in src/code/z_malloc.c. */
extern unsigned int gPspZeldaAllocFails;

/* Renderer health, for the class of bug that shows as a wrecked picture with a
 * perfectly healthy heap. Hyrule Field (ENTR_HYRULE_FIELD_3) rendered with
 * zfail=0, blob=71/0 and every GE stage completing, and still came out
 * garbled -- so the next question is whether the renderer is silently
 * discarding state. psp_tex_overflows counts "both texture regions full, wrap
 * region 0 and corrupt its oldest texture" (psp_texture_manager.c);
 * gPspGfxBadDlCursors counts display-list cursors refused by the guard added
 * in 965d76c0b. Both are silent by design and neither has ever been in a log. */
extern unsigned int psp_tex_overflows;
extern unsigned int psp_tex_spills;
extern unsigned int gPspGfxBadDlCursors;

/* Whole-cache throwaways that happen MID-FRAME because the working set does
 * not fit. gfx_texture_cache_reset()'s own comment calls this path unreachable
 * in normal play "because one room's textures fit comfortably" -- an
 * assumption that Hyrule Field, the largest scene in the game, is the obvious
 * candidate to break. When it is reached, the GE is left holding pointers into
 * memory that a different texture is being decoded into, which is exactly what
 * a wrecked picture looks like. Logged as rst=<vram>/<pool>. */
extern unsigned int gPspTexCacheResetVram;
extern unsigned int gPspTexCacheResetPool;

/* Peak bytes of the 2 MB GE display list used by any frame so far, and the
 * triangles the interpreter emitted last frame. See the long note beside
 * `list` in gfx_scegu.c: with every other counter reading zero on the stalling
 * run, overrunning that buffer is the remaining way we could stall the GE
 * ourselves, and pspsdk bounds-checks it nowhere. Logged as ge=<peakKB> and
 * tri=<count>. */
extern unsigned int gPspGeListPeak;
unsigned int gfx_pc_stat_tris_drawn(void);

/* Display-list pool overruns.
 *
 * This is the one remaining way we could hand the GE a garbage command, and
 * the build cannot notice it: with DEBUG_FEATURES=0, OPEN_DISPS/CLOSE_DISPS
 * compile to nothing and GRAPH_ALLOC is a bare pointer decrement with no
 * bounds check at all. The TwoHeadGfxArena grows display list commands up from
 * `p` and allocations (every matrix and viewport) down from `d`; when they
 * cross, one silently overwrites the other.
 *
 * graph.c has measured this for a long time -- but only into globals meant to
 * be read over the WebSocket debugger, which does not exist on hardware. So
 * the numbers have never appeared in a log. `over` counting up is the overrun
 * itself; `hr` is the smallest gap ever seen between the two heads, and a
 * negative value means they have crossed. */
extern unsigned int gPspPoolOverflows;
extern int gPspPoolOpaHeadroomMin;
#include "libc64/malloc.h"
#include "ultra64.h"

static char sLogPath[256];
static int sEnabled;

/* Pending, not-yet-written trace text. 2 KB comfortably holds
 * PSP_DIAG_FLUSH_EVERY lines (each line is <192 bytes, see PspDiag_Step). */
#define PSP_DIAG_RING_SIZE 2048
#define PSP_DIAG_FLUSH_EVERY 8
static char sRingBuf[PSP_DIAG_RING_SIZE];
static int sRingLen;

/* The ring is appended to by the MAIN thread (frame and teardown probes) and
 * by the AUDIO thread (at-synth/at-mix) with nothing between them, so two
 * appends could interleave inside memcpy or race on sRingLen. That is not
 * theoretical: oot_boot_pd4.log line 2335 is the bare fragment "01K", the tail
 * of a zalloc= field whose head was overwritten, and the Play_Init probes for
 * the scene that then hung ("gamestate alloc", "hyrule want") are missing from
 * that log entirely. An instrument that drops the entries you are hunting for
 * is worse than no instrument. */
static SceUID sLogSema = -1;

/* Flushes that could not open the log, and the last error code. Read on the
 * HUD (psp/src/psp_scene_menu.c), because by definition these cannot be
 * reported through the log itself. */
unsigned int gPspDiagWriteFails;
int gPspDiagWriteLastErr;

static void DiagLock(void) {
    if (sLogSema >= 0) {
        sceKernelWaitSema(sLogSema, 1, NULL);
    }
}

static void DiagUnlock(void) {
    if (sLogSema >= 0) {
        sceKernelSignalSema(sLogSema, 1);
    }
}
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

static void DiagFlushLocked(void) {
    SceUID fd;

    if (!sEnabled || sRingLen == 0) {
        return;
    }
    fd = sceIoOpen(sLogPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, sRingBuf, sRingLen);
        sceIoClose(fd);
    } else {
        /* The ring is emptied either way (below), so a failing open discards
         * the lines in complete silence while the game carries on. That is not
         * hypothetical: on 2026-08-26 four consecutive runs stopped logging
         * about three frames into Hyrule Field and then played on for another
         * thirty seconds, which reads exactly like a crash in the trace and is
         * nothing of the kind. Count it, and put the count somewhere that does
         * not depend on the file working -- the on-screen HUD. */
        ++gPspDiagWriteFails;
        gPspDiagWriteLastErr = (int)fd;
    }
    sRingLen = 0;
    sStepsSinceFlush = 0;
}

void PspDiag_Flush(void) {
    DiagLock();
    DiagFlushLocked();
    DiagUnlock();
}

static void DiagAppend(const char* text) {
    size_t len;
    char stamp[16];
    size_t stampLen;
    unsigned int ms;

    if (!sEnabled) {
        return;
    }
    DiagLock();
    ms = (unsigned int)(((u64)sceKernelGetSystemTimeWide() - sTimeBaseUsec) / 1000);
    stampLen = (size_t)snprintf(stamp, sizeof(stamp), "[%6u] ", ms);
    len = strlen(text) + stampLen;
    /* Flush first if this line would overflow the ring, so no line is ever
     * truncated -- losing a whole entry to save one early flush isn't worth
     * it. */
    if (sRingLen + (int)len > PSP_DIAG_RING_SIZE) {
        DiagFlushLocked();
    }
    if ((int)len > PSP_DIAG_RING_SIZE) {
        DiagUnlock();
        return; /* a single line bigger than the whole ring -- give up on it */
    }
    memcpy(sRingBuf + sRingLen, stamp, stampLen);
    sRingLen += (int)stampLen;
    memcpy(sRingBuf + sRingLen, text, len - stampLen);
    sRingLen += (int)(len - stampLen);

    sStepsSinceFlush++;
    if (sStepsSinceFlush >= PSP_DIAG_FLUSH_EVERY) {
        DiagFlushLocked();
    }
    DiagUnlock();
}

void PspDiag_Step(const char* step) {
    char line[256];
    unsigned int sysFree = (unsigned int)sceKernelMaxFreeMemSize();
    unsigned int sp = DiagStackPointer();
    /* Query the CURRENT thread's stack, every line.
     *
     * These bounds used to be captured once, in PspDiag_Init, on the main
     * thread -- so every line logged from the audio or graphics thread
     * compared that thread's sp against the MAIN thread's stack, fell through
     * the `sp > sStackBottom` test, and printed a flat "stkleft=0K". That
     * reads exactly like a thread with an exhausted stack, and cost real time
     * chasing a stack overflow that was never there. sStackBottom/sStackSize
     * survive only as the fallback for when the query fails. */
    unsigned int stackBottom = sStackBottom;
    unsigned int stackLeft;

    {
        SceKernelThreadInfo info;

        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(0, &info) >= 0) {
            stackBottom = (unsigned int)(uintptr_t)info.stack;
        }
    }
    stackLeft = (stackBottom != 0 && sp > stackBottom) ? (sp - stackBottom) : 0;
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

    /* The SYSTEM arena, not sceKernelMaxFreeMemSize(), is the number that
     * decides how big the Zelda arena gets: Play_Init asks GameState_Realloc
     * for 0x1D4790 (1.83 MB), the PSP never has that much, and game.c then
     * SILENTLY settles for `systemMaxFree - 0x10` instead. So the Zelda arena
     * on this port is simply "the biggest free block in the system arena at
     * the moment the scene loads".
     *
     * Measured across one hardware run (oot_boot_pd3.log): Kokiri Forest got
     * 541K, Hyrule Field 638K, and Kokiri again -- same scene, one round trip
     * later -- only 403K, at which point it ran dry (zfree=0K) and the picture
     * fell apart. smax vs salloc is what separates the two possible causes: if
     * salloc climbs across transitions the system arena is LEAKING, if salloc
     * holds steady while smax falls it is FRAGMENTING, and the fixes for those
     * are not the same. */
    {
        u32 sysMaxFree = 0, sysArenaFree = 0, sysArenaAlloc = 0;

        SystemArena_GetSizes(&sysMaxFree, &sysArenaFree, &sysArenaAlloc);
        snprintf(line, sizeof(line),
                 "%-26s sp=%08X stk=%uK aud=%u/%u pad=%u blob=%u/%u zfail=%u tex=%u/%u rst=%u/%u dl=%u ge=%uK tri=%u pool=%u hr=%d zfree=%uK zalloc=%uK\n",
                 step, sp, stackLeft / 1024, gPspDiagBeats[PSP_DIAG_BEAT_AUDIO],
                 gPspDiagBeats[PSP_DIAG_BEAT_AUDIO_STAGE], gPspDiagBeats[PSP_DIAG_BEAT_PADMGR],
                 gPspBlobMisses, gPspBlobOpenFails, gPspZeldaAllocFails, psp_tex_overflows, psp_tex_spills,
                 gPspTexCacheResetVram, gPspTexCacheResetPool, gPspGfxBadDlCursors,
                 gPspGeListPeak / 1024, gfx_pc_stat_tris_drawn(), gPspPoolOverflows,
                 gPspPoolOpaHeadroomMin, (unsigned int)zFree / 1024,
                 (unsigned int)zAlloc / 1024);
        (void)sysFree;
        (void)sysMaxFree;
        (void)sysArenaFree;
        (void)sysArenaAlloc;
    }
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
    /* Before the first append, and on the main thread, so no worker can be
     * inside DiagAppend while this runs. */
    if (sLogSema < 0) {
        sLogSema = sceKernelCreateSema("ootDiagLog", 0, 1, 1, NULL);
    }

    {
        SceKernelThreadInfo info;

        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(0, &info) >= 0) {
            sStackBottom = (unsigned int)(uintptr_t)info.stack;
            sStackSize = (unsigned int)info.stackSize;
        }
    }

    /* Keep the PREVIOUS boot as oot_boot_prev.log before truncating.
     *
     * Truncating alone is right for readability -- an ever-growing pile makes
     * the interesting last line impossible to find -- but it destroys exactly
     * the evidence this file exists to capture. A console that dies is a
     * console you then switch back on, and that next boot wiped the crash. It
     * cost a whole analysis round on 2026-08-25: a run was read as "died in
     * Hyrule Field" when the real crash had happened during warp-menu scene
     * changes in an earlier boot whose log no longer existed.
     *
     * Rename, do not copy: it is one directory operation, no read-back, no
     * second buffer, and it cannot half-succeed and leave a truncated file
     * that reads like a crash. A missing previous log (first ever run) simply
     * makes this fail harmlessly. */
    {
        char prevPath[sizeof(sLogPath) + 8];
        const char* suffix = strstr(sLogPath, "oot_boot.log");

        if (suffix != NULL) {
            size_t headLen = (size_t)(suffix - sLogPath);

            memcpy(prevPath, sLogPath, headLen);
            strcpy(prevPath + headLen, "oot_boot_prev.log");
            sceIoRemove(prevPath);
            sceIoRename(sLogPath, prevPath);
        }
    }

    fd = sceIoOpen(sLogPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        sEnabled = 0; /* read-only medium or bad path -- stay silent */
        return;
    }
    sceIoClose(fd);

    PspDiag_Step("diag-init");
}

#endif /* PSP_DIAG_ENABLED */
