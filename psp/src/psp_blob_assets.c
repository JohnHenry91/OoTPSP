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

    PspBlobNoteRange(dst, size);
    ++gPspBlobHits;
    return 1;
}
