#ifndef PSP_HW_DIAG_H
#define PSP_HW_DIAG_H

/* Master switch. -DPSP_DIAG_ENABLED=0 (make -f Makefile.psp DIAG=0) compiles
 * the entire boot trace, the bring-up ladder and every probe out of the
 * build: no memory-stick log is opened, no entry is written, no call site
 * survives. Everything below turns into empty inline functions, so no call
 * site needs an #if of its own.
 *
 * Kept as a switch rather than deleted. The instrumentation is what found the
 * failures this port had on real hardware -- per-thread heartbeats, the stack
 * high-water mark, the millisecond stamps that disproved the "timed out"
 * theory, and the ladder whose pass criterion needs no log at all. Throwing
 * that away to get a clean binary would mean rebuilding it from scratch the
 * next time, which is exactly the cost this switch avoids.
 *
 * It also makes the trace itself testable: the log is memory-stick I/O in the
 * middle of the failing path, so "does it still fail with no logging at all?"
 * is a question only a build with this set to 0 can answer. */
#ifndef PSP_DIAG_ENABLED
#define PSP_DIAG_ENABLED 1
#endif

/* How many frames the in-frame probes (graph.c, gfx_scegu.c, general.c,
 * thread.c) stay active for. Three is enough to see a boot go wrong and cheap
 * enough to leave on; widen it with -DPSP_DIAG_FRAMES=24 when localising a
 * failure inside a frame, as the audio investigation needed. */
#ifndef PSP_DIAG_FRAMES
#define PSP_DIAG_FRAMES 3
#endif

/* See psp/src/psp_hw_diag.c. Writes a durable boot trace and a crash report
 * next to EBOOT.PBP, because on real hardware there is no debugger to attach
 * and a console that switches itself off leaves nothing else behind. */
#if PSP_DIAG_ENABLED

void PspDiag_Init(const char* baseDir);
void PspDiag_Step(const char* step);
void PspDiag_StepSync(const char* step);
void PspDiag_Frame(unsigned int n);
void PspDiag_Hex(const char* label, const void* addr, int words);
void PspDiag_Note(const char* fmt, unsigned int a, unsigned int b);

/* One line per scene change, forced durable immediately (see PspDiag_StepSync
 * -- this is exactly the "fires once per transition, can afford it" case).
 * Auftrag 01: the frame-level counters in PspDiag_Step cannot tell an actor or
 * object-bank leak apart from ordinary per-frame churn, because nothing in
 * that line is scoped to "one scene lifetime". This adds the numbers that
 * are: which entrance, how full the object bank and actor list are right
 * after Play_Init, how many blob descriptors the ranged LRU is holding open,
 * and a running count of scene changes so a monotonic column can be told from
 * a flat one. */
void PspDiag_Scene(unsigned int changeCount, unsigned int entranceIndex, unsigned int objEntries,
                    unsigned int objBytesUsed, unsigned int objBytesTotal, unsigned int actorTotal,
                    unsigned int blobOpenFds);

/* Force whatever is still sitting in the RAM ring buffer out to the memory
 * stick right now. Normal appends only do this every PSP_DIAG_FLUSH_EVERY
 * steps -- see the file header in psp_hw_diag.c for why per-line I/O was
 * itself burning enough wall-clock time to be a suspect. Call this at a
 * point that must not be lost even though it falls between two periodic
 * flushes (e.g. right before a risky call). */
void PspDiag_Flush(void);

/* Per-thread heartbeats.
 *
 * The trace is written by the main thread, which is where the whole engine
 * runs -- so it can only ever say where the MAIN thread was. Hardware run 3
 * stopped between two adjacent probes with nothing but no-ops between them
 * (Rumble_Destroy is two pointer stores, SpeedMeter/VisCvg are empty, and
 * VisZBuf/VisMono resolve to the stubs in phase1_stubs.c). A main thread that
 * dies where it is doing nothing points away from the main thread: the port
 * also runs AudioMgr, PadMgr and DmaMgr as real PSP threads, and a fault in
 * any of them takes the whole module down without the main thread's trace
 * showing anything unusual.
 *
 * These counters make the other threads visible without them touching the log
 * (and without the file races that would bring). Each worker bumps its slot;
 * every main-thread entry prints all of them. A slot that stops advancing
 * while the timestamps keep moving is a dead thread, and the last value of
 * PSP_DIAG_BEAT_AUDIO_STAGE says how far audio initialisation got. */
enum {
    PSP_DIAG_BEAT_AUDIO,
    PSP_DIAG_BEAT_AUDIO_STAGE,
    PSP_DIAG_BEAT_PADMGR,
    PSP_DIAG_BEAT_DMA,
    PSP_DIAG_BEAT_COUNT
};

extern volatile unsigned int gPspDiagBeats[PSP_DIAG_BEAT_COUNT];

#define PSP_DIAG_BEAT(slot) (gPspDiagBeats[slot]++)
#define PSP_DIAG_BEAT_SET(slot, value) (gPspDiagBeats[slot] = (value))

/* Bring-up ladder: -DPSP_BRINGUP_LEVEL=<n>, default 99 (the whole game).
 *
 * Four hardware runs have localised the failure to a moving target: the trace
 * stops around 220 ms every time, but in run 3 that was after
 * "gsd-destroy-cb" and in run 4 one probe earlier, after "gsd-audioupd" --
 * and the five calls in between are empty functions. Probing further inside
 * the engine is chasing a fault that is very likely not the main thread's at
 * all.
 *
 * The deeper problem is that this port has NEVER been seen to survive on
 * hardware, so there is no known-good anchor to bisect against. Every run so
 * far compared one failing configuration with another failing configuration.
 * This ladder supplies the anchor: stop after stage n, idle visibly for a few
 * seconds, then leave through sceKernelExitGame.
 *
 * The pass criterion needs no log at all -- if the console returns to the
 * XMB, that stage is clean; if it switches off, the fault is in the stage
 * just added and nothing below it. Levels:
 *
 *   0  nothing at all: module loaded, heap claimed, trace open
 *   1  + gfx_init  (sceGu bring-up, exit-callback thread)
 *   2  + PspAudio_Init and the audio tables
 *   3  + PadSetup, PspRom_Init, DmaMgr and the ROM read smoke test
 *   99 + Main(): the real engine, all worker threads
 */
#ifndef PSP_BRINGUP_LEVEL
#define PSP_BRINGUP_LEVEL 99
#endif

/* Idle visibly for `seconds` at the named stage, then leave through
 * sceKernelExitGame. See the ladder note above: the point is a pass criterion
 * that needs no log -- back at the XMB means the stage is clean. */
void PspDiag_Park(const char* stage, int seconds);

#define PSP_BRINGUP_STOP_AFTER(level, name)          \
    do {                                             \
        if (PSP_BRINGUP_LEVEL <= (level)) {          \
            PspDiag_Park(name, 6);                   \
        }                                            \
    } while (0)

#else /* !PSP_DIAG_ENABLED */

static inline void PspDiag_Init(const char* baseDir) { (void)baseDir; }
static inline void PspDiag_Step(const char* step) { (void)step; }
static inline void PspDiag_StepSync(const char* step) { (void)step; }
static inline void PspDiag_Frame(unsigned int n) { (void)n; }
static inline void PspDiag_Hex(const char* label, const void* addr, int words) {
    (void)label;
    (void)addr;
    (void)words;
}
static inline void PspDiag_Note(const char* fmt, unsigned int a, unsigned int b) {
    (void)fmt;
    (void)a;
    (void)b;
}
static inline void PspDiag_Scene(unsigned int changeCount, unsigned int entranceIndex, unsigned int objEntries,
                                  unsigned int objBytesUsed, unsigned int objBytesTotal, unsigned int actorTotal,
                                  unsigned int blobOpenFds) {
    (void)changeCount;
    (void)entranceIndex;
    (void)objEntries;
    (void)objBytesUsed;
    (void)objBytesTotal;
    (void)actorTotal;
    (void)blobOpenFds;
}
static inline void PspDiag_Flush(void) {
}
static inline void PspDiag_Park(const char* stage, int seconds) {
    (void)stage;
    (void)seconds;
}

#define PSP_DIAG_BEAT(slot) ((void)0)
#define PSP_DIAG_BEAT_SET(slot, value) ((void)0)
#define PSP_BRINGUP_STOP_AFTER(level, name) ((void)0)

#endif /* PSP_DIAG_ENABLED */

#endif
