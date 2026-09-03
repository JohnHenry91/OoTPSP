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

/* Texture pool.
 *
 * The pool lives in VRAM and is sized at init to whatever EDRAM is left after
 * the two framebuffers and the Z-buffer -- roughly 1.2 MB of the PSP's 2 MB.
 * TEXMAN_BUFFER_SIZE is only the fallback/documentation value now; the real
 * size is computed in gfx_scegu_init and passed to texman_reset.
 *
 * History worth keeping: this was blamed for the speckled corruption that hit
 * the Market, the Dog Lady's house and Bottom of the Well, and was briefly
 * moved to a 4 MB main-RAM allocation. That was treating the symptom. The
 * actual defect was that NOTHING ever reset the pool between scenes, so it
 * accumulated over a whole session and was then wiped in the middle of a frame
 * with the GE still reading it. With gfx_texture_cache_reset() called per
 * scene load, one room's textures fit in VRAM comfortably -- and VRAM is where
 * they want to be, since the GE samples it at full bandwidth.
 */
/* Only an alignment margin. The pool does not need a reserve: the overflow
 * guard in texman_reserve_memory wraps to the start rather than running past
 * the end, and gfx_pc.c already demands 32 KB of headroom before allocating. */
#define TEXMAN_VRAM_SLACK (4 * 1024)

/* Main-RAM spill region, used once VRAM is full.
 *
 * THE PARAGRAPH THAT USED TO BE HERE WAS MEASURED WRONG. It said the region is
 * never touched -- "psp_tex_spills and gPspTexCacheResetVram both stay 0
 * through normal play" -- and concluded it could eventually be deleted. On
 * hardware, 2026-08-27: `wipe=1/0 hw=282`, i.e. a real VRAM exhaustion, with
 * the entry pool barely half full. So ~1.2 MB of VRAM plus 1 MB of spill is
 * NOT enough for every scene, and the wipe that follows throws away all 282
 * live textures in the middle of a frame. That is the single-frame corruption
 * seen while walking through a room: more geometry comes into view, the
 * budget runs out, everything is dropped and re-uploaded.
 *
 * Ask for more, and settle for what is available. The allocation is best
 * effort and a smaller region is strictly better than none, so try in
 * descending order rather than taking one size and falling back to nothing --
 * which is what a single memalign would do the day this no longer fits. */
#define TEXMAN_OVERFLOW_SIZE (4 * 1024 * 1024)
#define TEXMAN_OVERFLOW_MIN (1 * 1024 * 1024)

/* Emergency slot for textures requested after the budget is gone, carved off
 * the tail of the spill region. Sized for the largest texture the importers
 * can produce -- the 256x256 RGBA case this file's own reserve comment names,
 * at 256 KB. See texman_reserve_memory for what it is for. */
#define TEXMAN_SCRATCH_SIZE (256 * 1024)
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
void texman_set_overflow_buffer(void *buf, unsigned int size);
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
/* Which texture id the GE is PHYSICALLY bound to right now. Not the same
 * question as "which id did the renderer select": the upload paths bind
 * directly, so the two can disagree. */
unsigned int texman_get_bound(void);
