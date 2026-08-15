#ifndef PSP_STATIC_ASSETS_H
#define PSP_STATIC_ASSETS_H

#include "stdint.h"

/* Compiled-in ("static") scene and room assets.
 *
 * WHY THIS EXISTS -- see reference/libultraship/docs/PORTING.md, "Phase 3:
 * Assets (The Fork in the Road)". A port either converts its assets at build
 * time (Option A, recommended) or keeps the raw N64 data and must "manually
 * massage the assets to handle endianness and bit-width differences"
 * (Option B). This port has been on Option B: scene/room files are DMA'd raw
 * out of the big-endian .z64 and then patched in place by the hand-written
 * PspFixup*Endian passes in psp/src/z_endian_fixup_psp.c -- ten functions
 * listing 82 individual struct field offsets by hand. Every field nobody
 * thought of stays byte-reversed, which never faults: it just yields a
 * garbage count or pointer that hangs or renders wrong much later, somewhere
 * unrelated. That is the exact failure signature this port has been chasing.
 *
 * The decomp already contains every asset as ordinary typed C source
 * (extracted/pal-1.0/assets/scenes/...), and it is written entirely in terms
 * of real C symbols -- SCENE_CMD_ROOM_SHAPE(&..._RoomShapeNormal),
 * gsSPVertex(&..._Vtx_fused_[667], ...), CollisionHeader fields naming their
 * vtxList/polyList arrays directly. Compiling that in gets both problems
 * solved by the toolchain instead of by hand:
 *
 *   - byte order: the compiler emits every field in native little-endian, so
 *     NO PspFixup*Endian pass is needed (and none may be applied -- running
 *     one over this data would corrupt correct data).
 *   - segmented addressing: references are linker-resolved pointers, not
 *     segment+offset values, so nothing has to be resolved through
 *     gSegments[] at all.
 *
 * This is a development mode, not the final architecture: PSP RAM cannot hold
 * every scene at once. Its purpose is to split the remaining bug space in
 * two. If a room that is compiled in renders correctly, then the renderer is
 * sound and every remaining defect lives in the raw-asset pipeline (endian
 * fixups + segment resolution). If it still renders wrong, the fault is in
 * the renderer and the asset pipeline is exonerated. Either answer is worth
 * more than another round of bisecting.
 *
 * Registration is keyed on the asset's VROM start address, because that is
 * the one identifier both load paths already have in hand (Play_LoadFile
 * takes a RomFile*, Room_RequestNewRoom indexes play->roomList.romFiles[]).
 */

/* Returns the compiled-in, native-endian data for the file starting at this
 * VROM address, or NULL if that file is not compiled in and must be DMA'd
 * from the ROM as usual. */
void* PspStaticAssetLookup(uintptr_t vromStart);

/* True if this pointer is compiled-in asset data. Both load paths use it to
 * decide whether to SKIP the endian fixup: static data is already correct and
 * a fixup pass would actively corrupt it. */
int PspStaticAssetIsStatic(const void* data);

/* Measurement, read via the debugger like every other counter in this port
 * (never file I/O -- that was itself a crash cause once). */
extern unsigned int gPspStaticAssetHits;
extern unsigned int gPspStaticAssetMisses;

#endif
