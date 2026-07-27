/* Hand-picked stand-in for reference/oot's src/dmadata/dmadata.c, which
 * generates gDmaDataTable with one entry per game segment (~940 for
 * pal-1.0) from linker symbols produced by a full game link. We aren't
 * linking the whole game yet (see PORTING.md), so those symbols don't
 * exist here.
 *
 * These four entries are real, though: offsets read directly out of
 * reference/oot/build/pal-1.0/oot-pal-1.0.map, for the first few segments
 * of the actual built pal-1.0 ROM (which src/boot/z_std_dma.c's
 * DmaMgr_ProcessRequest reads from via the shipped .z64 — see
 * include/psp_rom.h). Enough to exercise the real DmaMgr thread/queue
 * machinery end-to-end against real data without a full game link.
 *
 * All romEnd fields are 0 (uncompressed) because pal-1.0's dmadata table
 * always is — see z_std_dma.c's top-of-file comment.
 */

#include "dma.h"

/* Phase 2: replaced the 4-entry smoke-test table with a single catch-all
 * entry spanning the entire pal-1.0 ROM. pal-1.0's real dmadata is FULLY
 * UNCOMPRESSED, so for every file virtual ROM == physical ROM; a single
 * entry { {0, ROM_SIZE}, romStart=0 } makes DmaMgr_ProcessRequest resolve
 * any vrom to physical offset `0 + vrom - 0 == vrom`, i.e. a correct
 * identity passthrough for the whole ROM. This lets scenes/rooms/objects
 * DMA by their real vrom offsets (provided as absolute symbols in
 * psp/src/segment_roms_psp.c) without hand-listing ~940 dmadata entries or
 * reading the real serialized dmadata out of the ROM. ROM_SIZE is the
 * shipped oot-pal-1.0.z64 file size (0x34D4000). */
DmaEntry gDmaDataTable[] = {
    { { 0x00000000, 0x034D4000 }, 0x00000000, 0 }, // whole ROM (uncompressed passthrough)
    { { 0, 0 }, 0, 0 },                            // terminator (vromEnd == 0)
};
