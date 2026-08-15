#include "psp_static_assets.h"

#include "segment_symbols.h"
#include "assets/scenes/misc/hakaana2/hakaana2_scene.h"
#include "assets/scenes/misc/hakaana2/hakaana2_room_0.h"

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
DECLARE_ROM_SEGMENT(hakaana2_scene)

typedef struct {
    const u8* vromStart; /* identity: matches RomFile.vromStart at runtime */
    void* data;          /* compiled-in, native-endian, symbol-referenced */
} PspStaticAsset;

/* hakaana2 = "Grave with Fairy's Fountain" (ENTR_GRAVE_WITH_FAIRYS_FOUNTAIN_0),
 * this port's standing test scene: one room, ROOM_SHAPE_TYPE_NORMAL, no
 * prerendered JPEG background, and known to boot and render. Both halves must
 * be registered together -- the room's display lists reference the scene's
 * textures by symbol across the file boundary. */
static const PspStaticAsset sStaticAssets[] = {
    { _hakaana2_sceneSegmentRomStart, hakaana2_scene },
    { _hakaana2_room_0SegmentRomStart, hakaana2_room_0 },
};

#define PSP_STATIC_ASSET_COUNT ((int)(sizeof(sStaticAssets) / sizeof(sStaticAssets[0])))

void* PspStaticAssetLookup(uintptr_t vromStart) {
    int i;

    for (i = 0; i < PSP_STATIC_ASSET_COUNT; i++) {
        if ((uintptr_t)sStaticAssets[i].vromStart == vromStart) {
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

    return p >= _ftext && p < __bss_start;
}
