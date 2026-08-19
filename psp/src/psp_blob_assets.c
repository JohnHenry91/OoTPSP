/* See psp/include/psp_blob_assets.h for why this exists and how the blobs are
 * produced. */

#include <pspiofilemgr.h>
#include <string.h>

#include "psp_blob_assets.h"

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
        ++gPspBlobMisses;
        return 0;
    }

    gPspBlobLastVrom = romOffset;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
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
