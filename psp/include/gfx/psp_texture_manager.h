/*
 * File: psp_texture_manager.h
 * Project: gfx
 * File Created: Friday, 7th August 2020 9:11:56 pm
 * Author: HaydenKow
 * -----
 * Copyright (c) 2020 Hayden Kowalchuk, Hayden Kowalchuk
 * License: BSD 3-clause "New" or "Revised" License, http://www.opensource.org/licenses/BSD-3-Clause
 */

#pragma once
#define TEX_ALIGNMENT (16)

/* Texture pool size.
 *
 * This used to be 1 MB carved out of VRAM (getStaticVramBufferBytes), which is
 * all that is left of the PSP's 2 MB after two 512x272 framebuffers and the
 * Z-buffer. Every texture is decoded to 32-bit RGBA before upload, so 1 MB is
 * roughly 64 textures of 64x64 -- less than a single OoT room needs. Running
 * out makes gfx_texture_cache_lookup wipe BOTH caches mid-frame
 * (texman_clear), while the GE stays bound to VRAM that now belongs to a
 * different texture: the speckled/garish corruption that shows up in the
 * Market, Back Alley House and Bottom of the Well, and that comes and goes
 * depending on how full the pool happened to be when the room loaded.
 *
 * The pool now lives in main RAM instead (the GE can sample textures straight
 * out of RAM, just with less bandwidth than VRAM -- this is what DaedalusX64
 * does on the same hardware), so it can be sized for the content rather than
 * for what VRAM has left over. */
#define TEXMAN_BUFFER_SIZE (4 * 1024 * 1024)

struct PSP_Texture {
    unsigned char *location;
    int width, height;
    unsigned int type;
    unsigned int swizzled;
};

/* used for initialization */
int texman_inited(void);
void texman_reset(void *buf, unsigned int size);
void texman_set_buffer(void *buf, unsigned int size);

/* management funcs for clients
Steps:
1. create
2. reserve memory & upload by pointer OR upload_swizzle

note: texture will be bound
*/
unsigned int texman_create(void);
void texman_clear(void);
int gfx_vram_space_available(void);
unsigned char *texman_get_tex_data(unsigned int num);
struct PSP_Texture *texman_reserve_memory(int width, int height, unsigned int type);
void texman_upload_swizzle(int width, int height, unsigned int type, const void *buffer);
void texman_upload(int width, int height, unsigned int type, const void *buffer);
void texman_bind_tex(unsigned int num);
