/* See psp/include/psp_blob_assets.h for why this exists and how the blobs are
 * produced. */

#include <pspiofilemgr.h>
#include <string.h>

#include "psp_blob_assets.h"

/* Blob paths in the generated registries are relative ("blobs/x.bin"), which
 * is only meaningful against a current working directory -- and on PSP a
 * thread created with sceKernelCreateThread starts with NO cwd of its own.
 * The main thread has one (the loader sets it), so every load that happens to
 * run there works, and the failure only shows up for loads issued from one of
 * the game's own OS threads. AudioMgr is exactly that case: every soundfont,
 * sequence and sample read came back SCE_KERNEL_ERROR_NOCWD (0x8002032C),
 * sceIoRead was never reached, and the destination kept the audio heap's
 * zeroes -- a "successful" load of silence, with LOAD_STATUS_PERMANENTLY_LOADED
 * set and no error anywhere. So resolve every blob path against the game's own
 * directory instead, captured once from argv[0] on the main thread. */
/* Non-static so the base directory can be read straight out of memory with
 * the PPSSPP debugger when a path question comes up again. */
char gPspBlobBaseDir[192];
#define sBaseDir gPspBlobBaseDir
static char sPathBuf[256];

void PspBlob_SetBaseDir(const char* argv0) {
    size_t i, cut = 0;

    if (argv0 == NULL) {
        return;
    }
    for (i = 0; argv0[i] != '\0' && i < sizeof(sBaseDir) - 1; i++) {
        if (argv0[i] == '/' || argv0[i] == '\\') {
            cut = i + 1; /* keep the separator */
        }
    }
    if (cut == 0) {
        return; /* no directory component -- leave paths relative */
    }
    memcpy(sBaseDir, argv0, cut);
    sBaseDir[cut] = '\0';
}

const char* PspBlob_GetBaseDir(void) {
    return sBaseDir;
}

/* Absolute first, then the original relative path as a fallback. The fallback
 * is not redundant: argv[0] is whatever the loader chose to hand us, and on an
 * emulator it need not be a path the IO layer can reopen. Trying both means
 * this change can only ever fix opens, never break ones that already worked. */
static SceUID PspBlobOpen(const char* rel) {
    SceUID fd;
    size_t n;

    if (sBaseDir[0] != '\0') {
        n = strlen(sBaseDir);
        if (n + strlen(rel) + 1 <= sizeof(sPathBuf)) {
            memcpy(sPathBuf, sBaseDir, n);
            strcpy(sPathBuf + n, rel);
            fd = sceIoOpen(sPathBuf, PSP_O_RDONLY, 0777);
            if (fd >= 0) {
                return fd;
            }
        }
    }
    return sceIoOpen(rel, PSP_O_RDONLY, 0777);
}

unsigned int gPspBlobMagic = 0x50424C42; /* 'PBLB' */
unsigned int gPspBlobHits;
unsigned int gPspBlobMisses;
unsigned int gPspBlobOpenFails;
/* The address and size of the most recent read that matched NO registered
 * blob range. A miss is not a benign statistic: the caller falls through to
 * a raw ROM read at a synthetic address past the end of the ROM file, which
 * transfers nothing and leaves the destination holding whatever the previous
 * note left there -- the "digital squeal" the tail-read comment below
 * describes. Misses are rare (tens, over minutes), so keeping only the last
 * one is enough to identify it on screen: the value sits still long enough
 * to read, and its magnitude alone says which asset family it belongs to
 * (audio blobs live at 0x20000000/0x24000000/0x28000000, scenes elsewhere). */
unsigned int gPspBlobLastMissVrom;
unsigned int gPspBlobLastMissSize;
unsigned int gPspBlobShortReads;
/* WHICH blob came up short, and by how many bytes. The bare count cannot
 * answer the only question that matters about it: a blob built a little
 * smaller than the ROM file it stands in for short-reads on every single load
 * and zero-fills padding nobody looks at, which is harmless noise, while a
 * genuinely truncated transfer leaves real data missing. Same counter, two
 * completely different situations, and the shortfall separates them -- a few
 * bytes is the first, a large round number is the second. */
unsigned int gPspBlobShortLastVrom;
unsigned int gPspBlobShortLastMissing;
/* Sample-bank reads whose window ran past the end of the bank and were served
 * short with a zeroed tail. Expected to be nonzero and harmless -- see the
 * long comment at the tail-read path in PspBlob_Read. Counted because "a lot
 * of these" would mean a stale blob, not just samples near the bank end. */
unsigned int gPspBlobTailReads;
unsigned int gPspBlobLastVrom;

typedef struct {
    uint32_t vromStart;
    const char* path;
} PspBlobEntry;

/* GENERATED, do not hand-edit: build/blobs/registry.inc is concatenated by
 * Makefile.psp from the per-scene .reg fragments that make_scene_blob.sh emits.
 * Hand-maintaining these hex vroms is exactly the kind of transcription that
 * has cost this project time before -- the script derives them from
 * psp/src/segment_roms_psp.c, which is itself generated from the ROM map. */
static const PspBlobEntry sBlobs[] = {
#include "blobs_registry.inc"
};

/* Blobs that may be read at any offset inside them, not only whole.
 *
 * Every blob above is a scene or room: the engine allocates the file's exact
 * vromEnd - vromStart and loads all of it, so an exact vromStart match is the
 * whole lookup. Audio is different, and this is the difference that kept the
 * game silent: OoT deliberately never loads Audiotable (the sample bank) as a
 * unit -- its table entry's cachePolicy is CACHE_LOAD_EITHER_NOSYNC, which
 * makes AudioLoad_TrySyncLoadSampleBank hand back the raw cart address, and
 * every note then DMAs its own few hundred bytes out of the middle of it via
 * AudioLoad_DmaSampleData. That is how a 4 MB sample bank fits in a 229 KB
 * audio heap. Against an exact-match-only registry, every one of those reads
 * missed and fell through to a raw seek past the end of the ROM file, which
 * returns zero bytes: notes played, envelopes ran, and every sample was
 * silence. See psp/docs/AUDIO_N64_VS_PSP.md section 3.
 *
 * Sizes come from the built files (Makefile.psp), so they cannot drift from
 * what is actually shipped. */
typedef struct {
    uint32_t vromStart;
    uint32_t size;
    const char* path;
} PspBlobRangedEntry;

static const PspBlobRangedEntry sRangedBlobs[] = {
#include "blobs_ranged_registry.inc"
};

/* Ranged blobs are held open across reads, but only a few at a time.
 *
 * Holding them open is the right instinct: unlike a scene load -- one open,
 * one read, done -- the sample bank is read several times per audio frame,
 * once per note whose sample window has run out, and a Memory Stick directory
 * lookup in the audio thread's per-frame path costs far more than the read.
 *
 * What was wrong was holding ALL of them open. This used to open lazily and
 * never close, on the stated grounds that "there are five of them and they
 * live for the whole session". That was true when it was written. Full audio
 * coverage then grew the registry to 110 sequences, 38 soundfonts and 6 sample
 * banks -- 153 entries -- and nobody rechecked the sentence. The PSP allows
 * far fewer files open at once, so after enough distinct sequences had been
 * touched, every subsequent sceIoOpen in the whole process failed with
 * SCE_KERNEL_ERROR_MFILE (0x80020320).
 *
 * Measured on hardware, 2026-08-26: entering Hyrule Field (new BGM, hence new
 * sequence and soundfont blobs) pushed it over, and from that moment the boot
 * trace could not open its own log -- which is why four separate runs looked
 * like they "died" a second into the scene while the game was in fact still
 * playing. Anything else that needs a file from then on fails too.
 *
 * So: keep an LRU of at most PSP_BLOB_MAX_OPEN descriptors. The sample bank,
 * being read constantly, simply never becomes least-recently-used, so the hot
 * path keeps its open handle and the audio thread never pays for a reopen. */
#define PSP_BLOB_MAX_OPEN 8

static SceUID sRangedFds[sizeof(sRangedBlobs) / sizeof(sRangedBlobs[0])];
static unsigned int sRangedUse[sizeof(sRangedBlobs) / sizeof(sRangedBlobs[0])];
static unsigned int sRangedClock;
static unsigned int sRangedOpen;
static int sRangedFdsInited;

/* Descriptors closed to stay under the cap, for the HUD. Distinct from
 * gPspBlobOpenFails: this one is healthy housekeeping, not an error. */
unsigned int gPspBlobFdEvictions;

/* Set from the power callback when the console comes back from standby.
 * Volatile and scalar-only on purpose: the callback runs on its own thread and
 * must not do I/O, so it only raises this flag and the next real read acts on
 * it. Same split the reference port uses (OotPsp_AssetNotifyResume in
 * reference/oot-psp-z2442/src/port/psp/oot_psp_asset_loader.c). */
static volatile int sResumePending;

/* Resumes handled, for the HUD -- so "the sound broke after standby" can be
 * told apart from "the callback never fired". */
unsigned int gPspBlobResumes;

void PspBlob_NotifyResume(void) {
    sResumePending = 1;
}

unsigned int PspBlob_OpenFdCount(void) {
    return sRangedOpen;
}

/* Standby powers the Memory Stick down, and every descriptor open across it
 * comes back invalid: reads return errors or garbage rather than failing in
 * any way the callers notice. The audio path is what makes this loud -- the
 * sample bank is read several times per audio frame and is by construction
 * never the LRU victim, so its stale descriptor is the one that survives the
 * cache and feeds every note nonsense until the game is restarted.
 *
 * Dropping the whole cache is the entire fix: the descriptors are pure cache,
 * PspBlobOpenRanged reopens on demand, and the cost is one directory lookup
 * per blob actually touched afterwards. */
static void PspBlobHandleResume(void) {
    unsigned int k;

    sResumePending = 0;
    ++gPspBlobResumes;

    if (!sRangedFdsInited) {
        return;
    }

    for (k = 0; k < sizeof(sRangedFds) / sizeof(sRangedFds[0]); k++) {
        if (sRangedFds[k] >= 0) {
            /* Close even though the descriptor is already dead: leaking it
             * would eat the very budget the LRU exists to protect, and this is
             * exactly the path that ran out of descriptors before. */
            sceIoClose(sRangedFds[k]);
            sRangedFds[k] = -1;
        }
    }
    sRangedOpen = 0;
}

/* Lazily open ranged blob `i`, returning whether it is usable. Shared by both
 * ranged read paths so neither can forget the one-time table init. */
static int PspBlobOpenRanged(unsigned int i) {
    if (!sRangedFdsInited) {
        unsigned int k;

        for (k = 0; k < sizeof(sRangedFds) / sizeof(sRangedFds[0]); k++) {
            sRangedFds[k] = -1;
        }
        sRangedFdsInited = 1;
    }

    if (sRangedFds[i] < 0) {
        if (sRangedOpen >= PSP_BLOB_MAX_OPEN) {
            unsigned int k;
            unsigned int victim = 0;
            unsigned int oldest = 0xFFFFFFFFu;

            for (k = 0; k < sizeof(sRangedFds) / sizeof(sRangedFds[0]); k++) {
                if (sRangedFds[k] >= 0 && sRangedUse[k] < oldest) {
                    oldest = sRangedUse[k];
                    victim = k;
                }
            }
            if (sRangedFds[victim] >= 0) {
                sceIoClose(sRangedFds[victim]);
                sRangedFds[victim] = -1;
                --sRangedOpen;
                ++gPspBlobFdEvictions;
            }
        }

        sRangedFds[i] = PspBlobOpen(sRangedBlobs[i].path);
        if (sRangedFds[i] >= 0) {
            ++sRangedOpen;
        }
    }

    if (sRangedFds[i] < 0) {
        ++gPspBlobOpenFails;
        return 0;
    }

    sRangedUse[i] = ++sRangedClock;
    return 1;
}

/* Ranges most recently filled from a blob, so the endian-fixup passes can tell
 * "already native" data from raw .z64 data.
 *
 * A ring rather than a single range because a scene and its room are both live
 * at once, and the fixup call sites run at different times (Play_SpawnScene vs
 * Room_RequestNewRoom).
 *
 * THE SIZE IS DERIVED, NOT CHOSEN. It used to be 8, with a comment saying that
 * was "far more than the two or three that are ever simultaneously in play" --
 * true while scenes and rooms were the only blob-served files. Once every
 * OBJECT became a blob too (psp/tools/make_object_blobs.py, Phase 3) the live
 * set became:
 *
 *     1  scene
 *     2  rooms (the current one and the previous one, which z_play keeps)
 *    19  object slots  (ARRAY_COUNT(ObjectContext.slots), include/object.h)
 *   ---
 *    22
 *
 * and an 8-entry ring wrapped during the scene load itself, evicting the
 * scene's and the room's ranges. PspBlob_IsNative() then answered "not native"
 * for the room's own textures, the CI decoder byte-reversed data that needed no
 * reversing, and Kokiri Forest came up as full-screen speckle -- the exact
 * symptom the paragraph below describes, arrived at from the opposite end.
 *
 * 40 covers the 22 with room for the object list growing. It is not a guess
 * that has to be trusted, either: gPspBlobRangeEvictions counts every time a
 * live entry is overwritten and is written into shotNNN.txt, so if this number
 * is ever too small again it says so instead of producing speckle.
 *
 * A STALE ENTRY IS NOT HARMLESS -- this comment used to claim it was, on the
 * grounds that "the memory it names is reused only by another blob load, which
 * re-registers it anyway". That is false, and it cost a long hunt. The arena is
 * shared with the RAW .z64 loads, and the skybox is the last asset in the game
 * still served that way (see the block comment in z_vr_box.c). Walk from the
 * Market into a shop and back and the market's skybox buffer can land inside a
 * range some earlier blob registered and then abandoned. PspBlob_IsNative()
 * then answers "native" for raw big-endian ROM data, gfx_pc.c's
 * tex_needs_u64_unswap() believes it, and the CI8 decoder reverses every group
 * of eight bytes of a texture that needed no reversing -- the speckled
 * skybox, with the room's own textures (all blob-served, so correctly
 * classified) still perfectly sharp beside it.
 *
 * So ranges are now retired explicitly, from both ends of their lifetime:
 * PspBlob_InvalidateRange() when a raw ROM read overwrites the memory, and
 * PspBlob_ResetRanges() at scene load, when every range from the previous
 * scene is stale by definition.
 *
 * NB this is an address-range test for the same reason PspStaticAssetIsStatic
 * is: the fixups run on the things a loaded file *points at*, each of which is
 * some interior address, so a table keyed on the file's base would miss them. */
/* ONE RING PER LIFETIME, not one ring for everything.
 *
 * A single ring was correct while scenes and rooms were the only blob-served
 * files. Objects are blob-served too now (psp/tools/make_object_blobs.py), and
 * they have a completely different lifetime: a scene's 19 object slots are
 * refilled on every room change, while the scene's own range has to stay valid
 * for as long as the player is in that scene. Mixing them in one ring means
 * the short-lived entries evict the long-lived one, and simply making the ring
 * bigger only buys a few more room changes before the same thing happens.
 *
 * That is not hypothetical: with a single 8-entry ring, walking into Kokiri
 * Forest evicted the scene's and room's ranges during the scene load itself.
 * PspBlob_IsNative() then answered "not native" for the room's own textures,
 * the CI decoder byte-reversed data that needed no reversing, and the whole
 * room came up as speckle -- the exact symptom the paragraph above describes,
 * reached from the other direction.
 *
 * So each class gets its own table, sized to its own live set:
 *
 *   scene   1  -- exactly one scene is loaded at a time
 *   room    4  -- z_play keeps the current room and the previous one; 4 leaves
 *                 margin for a transition that touches more
 *   keep    3  -- gameplay_keep, gameplay_field_keep, gameplay_dangeon_keep
 *   object 24  -- ARRAY_COUNT(ObjectContext.slots) is 19, plus margin
 *
 * The keeps need their own class and not just a slot in the object ring, even
 * though they are loaded through the very same Object_SpawnPersistent path.
 * Their LIFETIME is different: they are persistent, resident for the whole
 * scene, while the 19 ordinary slots are refilled on every room change. Leaving
 * gameplay_keep in the rotating ring meant that after enough room changes its
 * range was evicted, and the effect textures that other objects borrow from it
 * (gEffFleckTex, gDecorativeFlameTex and friends -- 149 such references across
 * the object set) started getting an endian fixup they must not get. Navi and
 * the particle effects came out in the wrong colours, while the effect textures
 * that happen to be compiled into the EBOOT stayed right, which is what made it
 * look like a per-effect problem rather than a lifetime one.
 *
 * Objects legitimately cycle through their ring as rooms change, and evicting
 * a dead object range is harmless. Evicting a SCENE, ROOM or KEEP range is not,
 * so those are counted separately and must stay at zero. */
#define PSP_BLOB_SCENE_RANGES  1
#define PSP_BLOB_ROOM_RANGES   4
#define PSP_BLOB_KEEP_RANGES   3
#define PSP_BLOB_OBJ_RANGES   24

typedef struct {
    uintptr_t start;
    uintptr_t end;
} PspBlobRange;

typedef enum {
    PSP_BLOB_CLASS_SCENE,
    PSP_BLOB_CLASS_ROOM,
    PSP_BLOB_CLASS_KEEP,
    PSP_BLOB_CLASS_OBJECT
} PspBlobClass;

static PspBlobRange sSceneRanges[PSP_BLOB_SCENE_RANGES];
static PspBlobRange sRoomRanges[PSP_BLOB_ROOM_RANGES];
static PspBlobRange sKeepRanges[PSP_BLOB_KEEP_RANGES];
static PspBlobRange sObjRanges[PSP_BLOB_OBJ_RANGES];
static int sNextScene, sNextRoom, sNextKeep, sNextObj;

/* Evicting a live scene or room range is the failure mode these exist to make
 * visible: it is silent, it corrupts only the graphics, and it reads as a
 * renderer bug. Both must stay 0. The object figure is expected to be nonzero
 * in a dungeon and means nothing on its own. */
unsigned int gPspBlobSceneEvictions;
unsigned int gPspBlobRoomEvictions;
unsigned int gPspBlobKeepEvictions;
unsigned int gPspBlobObjEvictions;

/* Which table a blob belongs in, from its registry path.
 *
 * make_scene_blob.sh names its output "<scene>_scene.bin" and
 * "<scene>_room_<n>.bin"; make_object_blobs.py names its output after the
 * object segment. Matching on those suffixes is safe because both names are
 * generated, never hand-written -- and an object misfiled as a room would only
 * cost it a slot in the wrong ring, not correctness, because
 * PspBlob_IsNative() searches all three. */
static int PspBlobPathHas(const char* path, const char* needle) {
    const char* p;
    const char* a;
    const char* b;

    for (p = path; *p != '\0'; p++) {
        for (a = p, b = needle; *b != '\0' && *a == *b; a++, b++) {
        }
        if (*b == '\0') {
            return 1;
        }
    }
    return 0;
}

static PspBlobClass PspBlobClassify(const char* path) {
    const char* p;

    /* The three persistent keeps, by name. They arrive through the same
     * Object_SpawnPersistent path as any object, so nothing in the read itself
     * distinguishes them -- but their lifetime is the scene's, not the object
     * slot's. Matching the generated blob names is safe: make_object_blobs.py
     * names each blob after its spec segment, and these three names come from
     * the decomp's own spec. */
    if (PspBlobPathHas(path, "gameplay_keep.bin") ||
        PspBlobPathHas(path, "gameplay_field_keep.bin") ||
        PspBlobPathHas(path, "gameplay_dangeon_keep.bin")) {
        return PSP_BLOB_CLASS_KEEP;
    }

    for (p = path; *p != '\0'; p++) {
        if (p[0] == '_' && p[1] == 'r' && p[2] == 'o' && p[3] == 'o' && p[4] == 'm' && p[5] == '_') {
            return PSP_BLOB_CLASS_ROOM;
        }
        if (p[0] == '_' && p[1] == 's' && p[2] == 'c' && p[3] == 'e' && p[4] == 'n' && p[5] == 'e' &&
            p[6] == '.') {
            return PSP_BLOB_CLASS_SCENE;
        }
    }
    return PSP_BLOB_CLASS_OBJECT;
}

static void PspBlobNoteRange(const void* dst, size_t size, PspBlobClass cls) {
    PspBlobRange* tbl;
    int* next;
    int count;
    unsigned int* evictions;

    switch (cls) {
        case PSP_BLOB_CLASS_SCENE:
            tbl = sSceneRanges; next = &sNextScene;
            count = PSP_BLOB_SCENE_RANGES; evictions = &gPspBlobSceneEvictions;
            break;
        case PSP_BLOB_CLASS_ROOM:
            tbl = sRoomRanges; next = &sNextRoom;
            count = PSP_BLOB_ROOM_RANGES; evictions = &gPspBlobRoomEvictions;
            break;
        case PSP_BLOB_CLASS_KEEP:
            tbl = sKeepRanges; next = &sNextKeep;
            count = PSP_BLOB_KEEP_RANGES; evictions = &gPspBlobKeepEvictions;
            break;
        default:
            tbl = sObjRanges; next = &sNextObj;
            count = PSP_BLOB_OBJ_RANGES; evictions = &gPspBlobObjEvictions;
            break;
    }

    /* Re-registering the same memory is a refill, not an eviction -- the object
     * bank hands the same address back constantly. Only a DIFFERENT live range
     * being pushed out counts. */
    if (tbl[*next].end != 0 && tbl[*next].start != (uintptr_t)dst) {
        ++*evictions;
    }
    tbl[*next].start = (uintptr_t)dst;
    tbl[*next].end = (uintptr_t)dst + size;
    *next = (*next + 1) % count;
}

/* A raw .z64 read just landed here, so whatever a blob put here before is gone
 * and must stop claiming to be native-endian.
 *
 * Any overlap retires the whole entry. Both sides allocate and fill WHOLE
 * files, so a partial overlap does not arise in practice; and erring towards
 * "not native" is the safe direction -- it can only cost a redundant fixup
 * decision on data that is about to be overwritten anyway, whereas erring the
 * other way corrupts good data, which is the bug this exists to prevent. */
void PspBlob_InvalidateRange(const void* dst, size_t size) {
    uintptr_t a = (uintptr_t)dst;
    uintptr_t b = a + size;
    int i;

    PspBlobRange* tbls[4];
    int counts[4];
    int t;

    tbls[0] = sSceneRanges; counts[0] = PSP_BLOB_SCENE_RANGES;
    tbls[1] = sRoomRanges;  counts[1] = PSP_BLOB_ROOM_RANGES;
    tbls[2] = sKeepRanges;  counts[2] = PSP_BLOB_KEEP_RANGES;
    tbls[3] = sObjRanges;   counts[3] = PSP_BLOB_OBJ_RANGES;

    for (t = 0; t < 4; t++) {
        for (i = 0; i < counts[t]; i++) {
            if (tbls[t][i].end != 0 && a < tbls[t][i].end && b > tbls[t][i].start) {
                tbls[t][i].start = 0;
                tbls[t][i].end = 0;
            }
        }
    }
}

/* Every range belongs to the scene that loaded it. */
void PspBlob_ResetRanges(void) {
    memset(sSceneRanges, 0, sizeof(sSceneRanges));
    memset(sRoomRanges, 0, sizeof(sRoomRanges));
    memset(sKeepRanges, 0, sizeof(sKeepRanges));
    memset(sObjRanges, 0, sizeof(sObjRanges));
    sNextScene = sNextRoom = sNextKeep = sNextObj = 0;
}

int PspBlob_IsNative(const void* p) {
    uintptr_t a = (uintptr_t)p;
    const PspBlobRange* tbls[4];
    int counts[4];
    int t, i;

    tbls[0] = sSceneRanges; counts[0] = PSP_BLOB_SCENE_RANGES;
    tbls[1] = sRoomRanges;  counts[1] = PSP_BLOB_ROOM_RANGES;
    tbls[2] = sKeepRanges;  counts[2] = PSP_BLOB_KEEP_RANGES;
    tbls[3] = sObjRanges;   counts[3] = PSP_BLOB_OBJ_RANGES;

    for (t = 0; t < 4; t++) {
        for (i = 0; i < counts[t]; i++) {
            if (tbls[t][i].end != 0 && a >= tbls[t][i].start && a < tbls[t][i].end) {
                return 1;
            }
        }
    }
    return 0;
}

int PspBlob_Read(uint32_t romOffset, void* dst, size_t size) {
    const char* path = NULL;
    SceUID fd;
    int got;
    unsigned int i;

    /* The single funnel every asset transfer passes through, so this is the
     * one place a post-standby descriptor flush has to sit. */
    if (sResumePending) {
        PspBlobHandleResume();
    }

    for (i = 0; i < sizeof(sBlobs) / sizeof(sBlobs[0]); i++) {
        if (sBlobs[i].vromStart == romOffset) {
            path = sBlobs[i].path;
            break;
        }
    }

    if (path == NULL) {
        /* Not a whole-file blob -- try the ranged ones (audio; see
         * sRangedBlobs above). Deliberately second: the exact-match loop is
         * the common case and this one only ever runs on a miss. */
        for (i = 0; i < sizeof(sRangedBlobs) / sizeof(sRangedBlobs[0]); i++) {
            uint32_t start = sRangedBlobs[i].vromStart;

            if (romOffset >= start && (romOffset - start) < sRangedBlobs[i].size) {
                uint32_t offset = romOffset - start;

                /* A read that runs off the end of the blob is NORMAL here,
                 * and must not fail the transfer.
                 *
                 * AudioLoad_DmaSampleData always transfers a whole SampleDma
                 * window (`transfer = dma->size`, ~0x300 bytes) starting at
                 * `devAddr & ~0xF`, regardless of how much of it the note
                 * actually needs. For any sample near the end of a sample
                 * bank that window extends past the last byte of the bank. On
                 * N64 that is harmless: the PI bus just reads on into the next
                 * bytes of the cartridge and the note never looks at them.
                 *
                 * Rejecting the read instead meant falling through to the raw
                 * ROM path, which seeks to a synthetic blob address far past
                 * the end of the 55 MB ROM file, reads nothing, and leaves the
                 * DMA buffer holding WHATEVER THE PREVIOUS NOTE PUT THERE. The
                 * ADPCM decoder then runs another instrument's bytes through
                 * this note's predictor book, which does not sound like the
                 * wrong instrument -- it sounds like a digital squeal.
                 *
                 * So serve what exists and zero the tail, matching what the
                 * engine would have ignored anyway. */
                if ((uint64_t)offset + size > sRangedBlobs[i].size) {
                    uint32_t avail = sRangedBlobs[i].size - offset;

                    if (!PspBlobOpenRanged(i)) {
                        return 0;
                    }
                    sceIoLseek(sRangedFds[i], (SceOff)offset, PSP_SEEK_SET);
                    got = sceIoRead(sRangedFds[i], dst, (SceSize)avail);
                    if (got < 0) {
                        got = 0;
                    }
                    memset((char*)dst + got, 0, size - (size_t)got);
                    ++gPspBlobTailReads;
                    gPspBlobLastVrom = romOffset;
                    ++gPspBlobHits;
                    return 1;
                }

                if (!PspBlobOpenRanged(i)) {
                    return 0;
                }
                fd = sRangedFds[i];
                sceIoLseek(fd, (SceOff)offset, PSP_SEEK_SET);
                got = sceIoRead(fd, dst, (SceSize)size);

                if (got < 0 || (size_t)got != size) {
                    ++gPspBlobShortReads;
                    gPspBlobShortLastVrom = romOffset;
                    gPspBlobShortLastMissing =
                        (got > 0) ? (unsigned int)(size - (size_t)got) : (unsigned int)size;
                    if (got > 0 && (size_t)got < size) {
                        memset((char*)dst + got, 0, size - (size_t)got);
                    }
                }

                /* No PspBlobNoteRange here, unlike the whole-file path. That
                 * ring exists so the graphics endian fixups can recognise
                 * native-endian scene data; audio data has no fixup pass and
                 * these reads happen several times a frame, so registering
                 * them would flush every real scene range out of an 8-entry
                 * ring within a frame or two. */
                gPspBlobLastVrom = romOffset;
                ++gPspBlobHits;
                return 1;
            }
        }

        ++gPspBlobMisses;
        gPspBlobLastMissVrom = romOffset;
        gPspBlobLastMissSize = (unsigned int)size;
        return 0;
    }

    gPspBlobLastVrom = romOffset;

    fd = PspBlobOpen(path);
    if (fd < 0) {
        /* Fall back to the ROM rather than leaving dst uninitialised: a missing
         * blob file should degrade to the old (buggy but known) path, not to
         * garbage memory. */
        ++gPspBlobOpenFails;
        return 0;
    }

    got = sceIoRead(fd, dst, (SceSize)size);
    sceIoClose(fd);

    if (got < 0 || (size_t)got != size) {
        /* The blob and the caller disagree about the file's length. The caller's
         * size comes from the ROM's own vromEnd - vromStart, and the blob is
         * built from the same file, so this means a stale blob -- rebuild.
         * Recorded rather than silently tolerated, because a short read leaves
         * the tail of dst holding whatever was there before. */
        ++gPspBlobShortReads;
        gPspBlobShortLastVrom = romOffset;
        gPspBlobShortLastMissing = (got > 0) ? (unsigned int)(size - (size_t)got) : (unsigned int)size;
        if (got > 0 && (size_t)got < size) {
            memset((char*)dst + got, 0, size - (size_t)got);
        }
    }

    PspBlobNoteRange(dst, size, PspBlobClassify(path));
    ++gPspBlobHits;
    return 1;
}
