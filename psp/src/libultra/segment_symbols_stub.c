/* _dmadataSegmentRomStart/RomEnd and _bootSegmentRomStart now come from
 * psp/src/segment_roms_psp.c (Phase 2, real ROM offsets generated from the
 * build map, same as every other segment). _dmadataSegmentStart has no real
 * RAM equivalent on PSP (nothing loads the dmadata blob into RAM -- see
 * dmadata_test_table.c's whole-ROM DmaMgr passthrough) so it stays a dummy
 * here. */

#include "ultra64/ultratypes.h"

u8 _dmadataSegmentStart[1];
