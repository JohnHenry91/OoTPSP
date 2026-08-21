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
unsigned int gPspBlobShortReads;
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

/* Ranged blobs stay open. Unlike a scene load -- one open, one read, done --
 * the sample bank is read several times per audio frame, once per note whose
 * sample window has run out. Opening and closing the file each time would put
 * a Memory Stick directory lookup in the audio thread's per-frame path, which
 * on real hardware is far more expensive than the read itself. Opened lazily
 * on first use and never closed; there are five of them and they live for the
 * whole session. */
static SceUID sRangedFds[sizeof(sRangedBlobs) / sizeof(sRangedBlobs[0])];
static int sRangedFdsInited;

/* Ranges most recently filled from a blob, so the endian-fixup passes can tell
 * "already native" data from raw .z64 data.
 *
 * A ring rather than a single range because a scene and its room are both live
 * at once, and the fixup call sites run at different times (Play_SpawnScene vs
 * Room_RequestNewRoom). Eight is far more than the two or three that are ever
 * simultaneously in play.
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
#define PSP_BLOB_RANGES 8

typedef struct {
    uintptr_t start;
    uintptr_t end;
} PspBlobRange;

static PspBlobRange sRanges[PSP_BLOB_RANGES];
static int sNextRange;

static void PspBlobNoteRange(const void* dst, size_t size) {
    sRanges[sNextRange].start = (uintptr_t)dst;
    sRanges[sNextRange].end = (uintptr_t)dst + size;
    sNextRange = (sNextRange + 1) % PSP_BLOB_RANGES;
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

    for (i = 0; i < PSP_BLOB_RANGES; i++) {
        if (sRanges[i].end != 0 && a < sRanges[i].end && b > sRanges[i].start) {
            sRanges[i].start = 0;
            sRanges[i].end = 0;
        }
    }
}

/* Every range belongs to the scene that loaded it. */
void PspBlob_ResetRanges(void) {
    int i;

    for (i = 0; i < PSP_BLOB_RANGES; i++) {
        sRanges[i].start = 0;
        sRanges[i].end = 0;
    }
    sNextRange = 0;
}

int PspBlob_IsNative(const void* p) {
    uintptr_t a = (uintptr_t)p;
    int i;

    for (i = 0; i < PSP_BLOB_RANGES; i++) {
        if (sRanges[i].end != 0 && a >= sRanges[i].start && a < sRanges[i].end) {
            return 1;
        }
    }
    return 0;
}

int PspBlob_Read(uint32_t romOffset, void* dst, size_t size) {
    const char* path = NULL;
    SceUID fd;
    int got;
    unsigned int i;

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

                if ((uint64_t)offset + size > sRangedBlobs[i].size) {
                    /* Reading off the end of the blob means the table entry
                     * and the built file disagree -- same class of stale-blob
                     * bug as the short read below, and worth counting rather
                     * than silently truncating. */
                    ++gPspBlobShortReads;
                    break;
                }

                if (!sRangedFdsInited) {
                    unsigned int k;

                    for (k = 0; k < sizeof(sRangedFds) / sizeof(sRangedFds[0]); k++) {
                        sRangedFds[k] = -1;
                    }
                    sRangedFdsInited = 1;
                }
                if (sRangedFds[i] < 0) {
                    sRangedFds[i] = PspBlobOpen(sRangedBlobs[i].path);
                }
                fd = sRangedFds[i];
                if (fd < 0) {
                    ++gPspBlobOpenFails;
                    return 0;
                }
                sceIoLseek(fd, (SceOff)offset, PSP_SEEK_SET);
                got = sceIoRead(fd, dst, (SceSize)size);

                if (got < 0 || (size_t)got != size) {
                    ++gPspBlobShortReads;
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
        if (got > 0 && (size_t)got < size) {
            memset((char*)dst + got, 0, size - (size_t)got);
        }
    }

    PspBlobNoteRange(dst, size);
    ++gPspBlobHits;
    return 1;
}
