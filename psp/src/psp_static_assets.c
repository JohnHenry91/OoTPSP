#include "psp_static_assets.h"
#include "psp_blob_assets.h"

#include "segment_symbols.h"

unsigned int gPspStaticAssetHits;
unsigned int gPspStaticAssetMisses;

/* The scene's own ROM segment symbols are declared by the scene table's
 * DEFINE_SCENE pass (src/code/z_scene_table.c), not by segment_symbols.h,
 * so declare the one we need here. The room's are already in
 * segment_symbols.h (DECLARE_ROM_SEGMENT(hakaana2_room_0)).
 *
 * These resolve to the real ROM offsets at runtime -- psp/src/segment_roms_psp.c
 * defines them pre-biased by the module load base precisely so that PSP's
 * unconditional relocation cancels back out to the true offset. Using the
 * symbols rather than hardcoded hex keeps this table correct if the ROM or
 * that generated file changes. */

typedef struct {
    const u8* vromStart; /* identity: matches RomFile.vromStart at runtime */
    void* data;          /* compiled-in, native-endian, symbol-referenced */
} PspStaticAsset;

/* EMPTY ON PURPOSE -- superseded by psp_blob_assets.c.
 *
 * Compiling scenes into the EBOOT was always described here as a development
 * mode, not the final architecture, because PSP RAM cannot hold every scene.
 * It did its job: it proved the renderer is sound and that the defects lived in
 * the raw-asset pipeline. The blob loader now gets the same guarantee (native
 * byte order, resolved references) for scenes that are NOT resident, so nothing
 * needs to be linked in any more -- and hakaana2 must go through the blob path
 * for that path to actually be under test.
 *
 * The lookup and the range predicate below are kept: PspStaticAssetIsStatic is
 * still the single "is this already native-endian?" question that all ten
 * PspFixup*Endian guards ask, and it now answers for blob-loaded data too.
 * The sentinel keeps the array a valid C definition while it holds no entries. */
static const PspStaticAsset sStaticAssets[] = {
    { NULL, NULL },
};

#define PSP_STATIC_ASSET_COUNT ((int)(sizeof(sStaticAssets) / sizeof(sStaticAssets[0])))

void* PspStaticAssetLookup(uintptr_t vromStart) {
    int i;

    for (i = 0; i < PSP_STATIC_ASSET_COUNT; i++) {
        if (sStaticAssets[i].data != NULL && (uintptr_t)sStaticAssets[i].vromStart == vromStart) {
            ++gPspStaticAssetHits;
            return sStaticAssets[i].data;
        }
    }

    ++gPspStaticAssetMisses;
    return NULL;
}

/* Deliberately an ADDRESS-RANGE test, not a lookup in the table above.
 *
 * The table only knows the two top-level blobs (the scene and room command
 * lists). But the endian fixups run on the things those lists point AT --
 * hakaana2_scene_02003058_Col, its vtxList and polyList, the room shape, the
 * actor and object lists. Each of those is its own linker symbol, so a
 * table lookup would miss every one of them, and the fixup would byte-reverse
 * data that is already correct. That is exactly what happened on the first
 * attempt: the scene loaded, then PspFixupCollisionHeaderEndian and friends
 * corrupted the compiled-in collision data and the game hung building the
 * collision lookup table (StaticLookup_AddPolyToSSList).
 *
 * The range test catches all of them at once and needs no maintenance:
 * anything carrying an initializer is linked into the module's
 * initialized-data range [_ftext, __bss_start), while every DMA target is
 * .bss / the game arena / the heap, i.e. at or above __bss_start. This is the
 * same discriminator gfx_pc.c already uses to decide whether a texture needs
 * its u64-literal byte order undone -- the two problems are the same problem
 * (compiled-in data vs. raw ROM data), so they use the same test. */
extern char _ftext[];
extern char __bss_start[];

int PspStaticAssetIsStatic(const void* data) {
    const char* p = (const char*)data;

    /* Compiled-in data: carries an initialiser, so it lives in [_ftext, __bss_start). */
    if (p >= _ftext && p < __bss_start) {
        return 1;
    }

    /* Data loaded from a native-endian blob (psp_blob_assets.c). It sits in the
     * arena like any DMA'd file, so the address-range test above cannot see it,
     * but it is just as native and running a fixup over it would byte-reverse
     * correct data exactly the same way.
     *
     * The name of this predicate is now narrower than what it means -- every
     * caller asks it the one question "is this already native-endian, i.e. must
     * I skip the fixup?", and both cases answer yes. Kept as one predicate on
     * purpose: the guard lives INSIDE each of the ten PspFixup*Endian functions
     * rather than at their 22 call sites, precisely so it is impossible to
     * forget one, and that only works while there is a single question to ask. */
    return PspBlob_IsNative(data);
}
