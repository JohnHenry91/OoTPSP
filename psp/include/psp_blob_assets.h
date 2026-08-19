#ifndef PSP_BLOB_ASSETS_H
#define PSP_BLOB_ASSETS_H

#include <stddef.h>
#include <stdint.h>

/* Native-endian, segment-addressed asset blobs -- the port's asset strategy.
 *
 * BACKGROUND. reference/libultraship/docs/PORTING.md, "Phase 3: Assets (The
 * Fork in the Road)", gives two options. Option B keeps the raw N64 data and
 * requires you to "manually massage the assets to handle endianness and
 * bit-width differences"; this port did that for a long time, via ten
 * PspFixup*Endian passes listing 82 struct field offsets by hand, and it
 * produced a bug every session -- most recently a scene command list that never
 * reached SCENE_CMD_ID_END and spun Object_SpawnPersistent ~8.4 million times.
 * Option A converts assets at build time and is recommended, but as documented
 * it means OTR/Torch plus libultraship, which does not fit here: every LUS
 * Fast3D backend is shader-based while the PSP GE is fixed-function, so LUS
 * would not remove the hard part, and its C++ resource system assumes far more
 * RAM than a PSP has.
 *
 * What Option A actually *is*, though, is just: convert at build time to native
 * byte order with references resolved. The decomp hands us that for free --
 * every asset already exists as ordinary typed C with real symbol references.
 * So psp/tools/make_scene_blob.sh links each scene at 0x02000000 and each room
 * at 0x03000000 (their N64 segment bases) and objcopies a flat binary. The
 * compiler emits native little-endian; the linker resolves every reference; and
 * because the link is based at the segment address, every internal pointer in
 * the blob is a genuine 0x02xxxxxx / 0x03xxxxxx segment address, which
 * SEGMENTED_TO_VIRTUAL already handles and which is position independent.
 *
 * Result: no endian fixups, no hand-listed field offsets, no segment
 * arithmetic -- and it generalises, because all 32k assets are C.
 *
 * INTERCEPTION POINT. The hook sits in PspRom_Read (psp/src/libultra/os_rom.c),
 * the single leaf that every asset transfer funnels through. Everything above
 * it -- DmaMgr's request queue and thread, allocation, the room buffer paging in
 * Room_RequestNewRoom -- stays exactly as the decomp wrote it. Keying works
 * because this port's gDmaDataTable is an identity passthrough over the whole
 * uncompressed pal-1.0 ROM, so the physical offset handed to PspRom_Read is the
 * file's vromStart, the same id the callers already hold.
 */

/* Serve this transfer from a blob if one is registered for `romOffset`.
 * Returns 1 if handled (dst is filled), 0 if the caller should fall back to
 * reading the .z64. */
int PspBlob_Read(uint32_t romOffset, void* dst, size_t size);

/* True if `p` points into memory most recently filled from a blob, i.e. data
 * that is ALREADY native-endian and must NOT be run through any
 * PspFixup*Endian pass -- doing so would byte-reverse correct data. */
int PspBlob_IsNative(const void* p);

/* Retire any registered range the given memory overlaps -- call from the raw
 * .z64 read path, which is about to overwrite it with big-endian data. */
void PspBlob_InvalidateRange(const void* dst, size_t size);

/* Retire every range. Call at scene load. */
void PspBlob_ResetRanges(void);

/* Diagnostics, read with the WebSocket debugger like every other counter here
 * (never file I/O -- that was itself a crash cause once). gPspBlobMagic is a
 * magic word so a stale symbol address after a rebuild is caught instead of
 * being read as plausible garbage. */
extern unsigned int gPspBlobMagic; /* 'PBLB' */
extern unsigned int gPspBlobHits;
extern unsigned int gPspBlobMisses;
extern unsigned int gPspBlobOpenFails;
extern unsigned int gPspBlobShortReads;
extern unsigned int gPspBlobLastVrom;

#endif
