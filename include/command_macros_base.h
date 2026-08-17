#ifndef COMMAND_MACROS_BASE_H
#define COMMAND_MACROS_BASE_H

/**
 * Command Base macros intended for use in designing of more specific command macros
 * Each macro packs bytes (B), halfwords (H) and words (W, for consistency) into a single word
 */

#if TARGET_PSP

/* THESE MACROS ARE BYTE-ORDER SENSITIVE, and the port is little-endian.
 *
 * The name of each macro describes how the command's STRUCT reads the word --
 * CMD_BBBB means four u8 fields, CMD_BBH a u8, a u8 and a u16, and so on. The
 * value itself is built as a single u32 and stored as a u32. Those two views
 * only agree on a big-endian machine: there, (a << 24) | (b << 16) | (c << 8)
 * | d lands in memory as the bytes a, b, c, d, which is exactly what fields at
 * +0, +1, +2, +3 of that word expect. On the PSP the same u32 lands as
 * d, c, b, a and every such field reads its neighbour's value.
 *
 * MEASURED, 2026-08-17, link_home: SCENE_CMD_SKYBOX_SETTINGS(SKYBOX_HOUSE_LINK,
 * 0, LIGHT_MODE_SETTINGS) was read back as envLightMode == 0 == LIGHT_MODE_TIME
 * -- it had picked up skyboxConfig instead. Environment_Update therefore never
 * consulted the scene's EnvLightSettings at all: lightCtx.ambientColor stayed
 * (0,0,0) while the scene's own list plainly held (70,60,40), every lit vertex
 * came out black, and Link -- whose materials are TEXEL0 * SHADE under
 * GU_TFX_MODULATE -- rendered as a solid black silhouette. Also explains why
 * only the INDOOR scenes were affected: hakaana2 and spot02 genuinely want
 * LIGHT_MODE_TIME, so reading 0 happened to be right for them.
 *
 * Fixed here, at the point where the word is BUILT, rather than by swapping at
 * every read site: this is compile-time data, the macro name already states the
 * intended field layout, and the scene/room blobs are compiled with these very
 * headers, so they come out correct for free. (That is the same reasoning as
 * psp/tools/make_scene_blob.sh -- let the compiler be the byte-order converter.)
 *
 * Reversed per macro so that the FIRST argument still lands at the LOWEST
 * address, which is what the struct fields say.
 *
 * NOT reversed: CMD_W and CMD_PTR, whose consumers read the whole word as a
 * word (e.g. SCmdMiscSettings.area, SCmdRoomBehavior.gpFlag2), so their byte
 * order never matters.
 *
 * NOTE for whoever promotes z_demo.c: include/cutscene_commands.h uses these
 * same macros, and it is currently compiled into scene data that NOTHING reads
 * (z_demo.c is not in Makefile.psp). Once the cutscene interpreter is live,
 * check how it consumes these words -- if it reads them as u32 and shifts,
 * rather than through byte/halfword struct fields, it will need the ORIGINAL
 * big-endian packing and these macros will have to be split per consumer.
 */

/* a -> +0, b -> +1, c -> +2, d -> +3 */
#define CMD_BBBB(a, b, c, d) (_SHIFTL(d, 24, 8) | _SHIFTL(c, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8))

/* a -> +0, b -> +1, c -> +2..+3 (u16) */
#define CMD_BBH(a, b, c) (_SHIFTL(c, 16, 16) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8))

/* a -> +0..+1 (u16), b -> +2, c -> +3 */
#define CMD_HBB(a, b, c) (_SHIFTL(c, 24, 8) | _SHIFTL(b, 16, 8) | _SHIFTL(a, 0, 16))

/* a -> +0..+1 (u16), b -> +2..+3 (u16) */
#define CMD_HH(a, b) (_SHIFTL(b, 16, 16) | _SHIFTL(a, 0, 16))

#else

#define CMD_BBBB(a, b, c, d) (_SHIFTL(a, 24, 8) | _SHIFTL(b, 16, 8) | _SHIFTL(c, 8, 8) | _SHIFTL(d, 0, 8))

#define CMD_BBH(a, b, c) (_SHIFTL(a, 24, 8) | _SHIFTL(b, 16, 8) | _SHIFTL(c, 0, 16))

#define CMD_HBB(a, b, c) (_SHIFTL(a, 16, 16) | _SHIFTL(b, 8, 8) | _SHIFTL(c, 0, 8))

#define CMD_HH(a, b) (_SHIFTL(a, 16, 16) | _SHIFTL(b, 0, 16))

#endif

#define CMD_W(a) (a)

#define CMD_PTR(a) (u32)(a)

#endif
