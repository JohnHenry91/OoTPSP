#ifndef Z_ENDIAN_FIXUP_PSP_H
#define Z_ENDIAN_FIXUP_PSP_H

/* Byte-swaps the 32-bit scalar/pointer word of each 8-byte SceneCmd entry in
 * a raw-DMA'd scene or room command buffer (`data`, `size` bytes) from
 * big-endian (N64 ROM order) to little-endian (PSP native). Safe to call on
 * both scene and room command streams -- see z_endian_fixup_psp.c. */
void PspFixupCommandStreamEndian(void* data, unsigned int size);

/* Byte-swaps the fixed-layout fields of a raw-DMA'd CollisionHeader struct
 * (include/bgcheck.h) in place -- its u16/Vec3s and pointer fields, from
 * big-endian to little-endian. Call once, right after the header pointer is
 * resolved and before reading any of its fields. Does NOT touch the vertex/
 * polygon/surface-type/camera/waterbox arrays it points to -- those need
 * their own (not yet implemented) fixups once reached. */
void PspFixupCollisionHeaderEndian(void* colHeader);

/* Byte-swaps a raw-DMA'd Vec3s vertex array (`count` entries, 3x s16/6 bytes
 * each) in place. */
void PspFixupVtxListEndian(void* vtxList, unsigned int count);

/* Byte-swaps a raw-DMA'd CollisionPoly array (`count` entries, 8x u16/16
 * bytes each -- every field in CollisionPoly is a 16-bit scalar, see
 * include/bgcheck.h) in place. */
void PspFixupPolyListEndian(void* polyList, unsigned int count);

/* Byte-swaps a raw-DMA'd ActorEntry array (`count` entries, 8x s16/16 bytes
 * each -- id, pos, rot, params, see include/scene.h) in place. */
void PspFixupActorEntryListEndian(void* actorEntryList, unsigned int count);

/* Byte-swaps a raw-DMA'd TransitionActorEntry array (`count` entries, 0x10
 * bytes each) in place. sides[2] (2x s8) is left untouched -- single-byte
 * fields, same reasoning as SceneCmd's code/data1. */
void PspFixupTransitionActorEntryListEndian(void* transitionActorList, unsigned int count);

/* Byte-swaps a raw-DMA'd RomFile array (`count` entries, 2x u32/8 bytes
 * each: vromStart, vromEnd -- include/romfile.h) in place. Used for the
 * scene's room list. */
void PspFixupRomFileListEndian(void* romFileList, unsigned int count);

/* Byte-swaps a raw-DMA'd array of `count` s16/u16 values in place (object
 * list, exit list, etc: any list that's just a flat array of 16-bit
 * scalars). */
void PspFixupS16ArrayEndian(void* data, unsigned int count);

/* Byte-swaps a raw-DMA'd RoomShape union in place (include/room.h) --
 * handles ROOM_SHAPE_TYPE_NORMAL and ROOM_SHAPE_TYPE_CULLABLE (the two
 * types actually used in retail rooms); ROOM_SHAPE_TYPE_IMAGE (title
 * screen/credits backgrounds) is not handled yet. */
void PspFixupRoomShapeEndian(void* roomShape);

/* Byte-swaps a raw-DMA'd F3DEX2 display list in place, recursing into
 * G_DL-referenced sub-lists, stopping at G_ENDDL. Every Gfx command is a
 * uniform 2x 32-bit-word pair (see include/ultra64/gbi.h's Gfx union) whose
 * sub-fields are always extracted via shift+mask (e.g. gfx_pc.c's `C0`
 * macro: `(cmd->words.w0 >> pos) & mask`), never raw byte access -- so
 * unlike SceneCmd/RoomShape, a blanket word-swap of both w0 and w1 is
 * correct for every opcode, no per-opcode exceptions needed. G_VTX commands
 * additionally get their referenced Vtx array byte-swapped (see
 * PspFixupGfxVtxArrayEndian in the .c file) -- texture/TLUT data the DL
 * loads via G_SETTIMG/G_LOADBLOCK is still not handled, separate, not yet
 * implemented. Statically-compiled display lists (compiled fresh by
 * psp-gcc, e.g. Player's own object files) must NOT be passed here -- their
 * bytes are already in native PSP order. */
void PspFixupDisplayListEndian(void* dl);

/* BgCamInfo (include/bgcheck.h, 0x8 bytes: u16 setting, s16 count, Vec3s*
 * bgCamFuncData) lives inside the raw-DMA'd CollisionHeader's bgCamList
 * array, whose length isn't stored anywhere (real N64 code just trusts
 * whatever index scene data hands it) -- so instead of a bulk fixup pass
 * like the other arrays, these read ONE entry's field on demand, correctly
 * interpreting the still-big-endian bytes without mutating them (safe to
 * call every frame, from multiple call sites, for the same or different
 * entries, with no "already fixed" bookkeeping needed). */
unsigned short PspReadBgCamSettingRaw(void* bgCamInfoEntry);
void* PspReadBgCamFuncDataRaw(void* bgCamInfoEntry);
short PspReadBgCamCountRaw(void* bgCamInfoEntry);

/* Fills `out` (must point at a real, native BgCamFuncData local -- see the
 * .c file's doc comment) with `raw`'s fields correctly byte-order-corrected.
 * Non-destructive (does not touch `raw`), safe to call every frame. */
void PspReadBgCamFuncDataStruct(void* raw, void* out);

#endif
