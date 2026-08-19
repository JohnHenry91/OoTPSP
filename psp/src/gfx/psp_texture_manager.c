/*
 * File: psp_texture_manager.c
 * Project: gfx
 * File Created: Friday, 7th August 2020 9:11:50 pm
 * Author: HaydenKow
 * -----
 * Copyright (c) 2020 Hayden Kowalchuk, Hayden Kowalchuk
 * License: BSD 3-clause "New" or "Revised" License, http://www.opensource.org/licenses/BSD-3-Clause
 */

#include "psp_texture_manager.h"
#include <string.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct PSP_Texture textures[512];

/* Two-region bump allocator: VRAM first, then main RAM.
 *
 * VRAM is where textures want to be -- the GE samples it at full bandwidth --
 * but after two framebuffers and the Z-buffer only ~1.2 MB of the PSP's 2 MB
 * is left, and a busy scene like the Market needs more than that on its own.
 * A single-region pool therefore had to choose between "fast but overflows"
 * and "big but slow", and overflowing is not a graceful failure: it wipes both
 * texture caches in the middle of a frame while the GE is still reading them,
 * which is the speckled corruption.
 *
 * So: fill VRAM, then spill into a much larger RAM region instead of wiping.
 * A scene's first (and most reused) textures stay in fast memory and only the
 * tail pays the slower fetch. */
#define TEX_REGIONS 2

static struct TexRegion {
    void *start;
    void *cur;
    void *end;
} regions[TEX_REGIONS];

static int region_cur = 0;
static unsigned int psp_tex_number = 0;
unsigned int psp_tex_bound = 0;
unsigned int psp_tex_overflows = 0;   /* both regions full -> wrapped */
unsigned int psp_tex_spills = 0;      /* fell back from VRAM to RAM */

static inline unsigned int getMemorySize(int width, int height, unsigned int psm) {
    switch (psm) {
        case GU_PSM_T4:
            return (width * height) >> 1;

        case GU_PSM_T8:
            return width * height;

        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            return 2 * width * height;

        case GU_PSM_8888:
        case GU_PSM_T32:
            return 4 * width * height;

        default:
            return 0;
    }
}

static inline unsigned int getTexWidthBytes(int width, unsigned int psm) {
    switch (psm) {
        case GU_PSM_T4:
            return (width >> 1);

        case GU_PSM_T8:
            return width;

        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            return 2 * width;

        case GU_PSM_8888:
        case GU_PSM_T32:
            return 4 * width;

        default:
            return 0;
    }
}

static void swizzle_fast(unsigned char *out, const unsigned char *in, unsigned int width,
                         unsigned int height) {
    unsigned int blockx, blocky;
    unsigned int j;

    unsigned int width_blocks = (width / 16);
    unsigned int height_blocks = (height / 8);

    unsigned int src_pitch = (width - 16) / 4;
    unsigned int src_row = width * 8;

    const unsigned char *ysrc = in;
    unsigned int *dst = (unsigned int *) out;

    for (blocky = 0; blocky < height_blocks; ++blocky) {
        const unsigned char *xsrc = ysrc;
        for (blockx = 0; blockx < width_blocks; ++blockx) {
            const unsigned int *src = (unsigned int *) xsrc;
            for (j = 0; j < 8; ++j) {
                *(dst++) = *(src++);
                *(dst++) = *(src++);
                *(dst++) = *(src++);
                *(dst++) = *(src++);
                src += src_pitch;
            }
            xsrc += 16;
        }
        ysrc += src_row;
    }
}

int texman_inited(void) {
    return regions[0].start != NULL;
}

void texman_reset(void *buf, unsigned int size) {
    memset(textures, 0, sizeof(textures));
    memset(regions, 0, sizeof(regions));
    psp_tex_number = 0;
    region_cur = 0;
    regions[0].start = regions[0].cur = buf;
    regions[0].end = (char *)buf + size;
#ifdef DEBUG
    char msg[64];
    sprintf(msg, "TEXMAN reset @ %p size %d bytes\n", buf, size);
    sceIoWrite(1, msg, strlen(msg));
#endif
}

/* Register the spill region. Call after texman_reset. */
void texman_set_overflow_buffer(void *buf, unsigned int size) {
    regions[1].start = regions[1].cur = buf;
    regions[1].end = (char *)buf + size;
}

void texman_clear(void) {
    int i;

    memset(textures, 0, sizeof(textures));
    psp_tex_number = 0;
    region_cur = 0;
    for (i = 0; i < TEX_REGIONS; i++) {
        regions[i].cur = regions[i].start;
    }
#ifdef DEBUG
    char msg[64];
    sprintf(msg, "TEXMAN clear %p!\n", regions[0].start);
    sceIoWrite(1, msg, strlen(msg));
#endif
}

void texman_set_buffer(void *buf, unsigned int size) {
    regions[region_cur].start = regions[region_cur].cur = buf;
    regions[region_cur].end = (char *)buf + size;
}

/* "Can another texture be allocated at all", i.e. does ANY region still have
 * room -- not just the one currently being filled. Answering only for the
 * current region is what used to trigger the whole-cache wipe the moment VRAM
 * ran out, with a completely empty spill region sitting right there. */
int gfx_vram_space_available(void) {
    int i;

    for (i = 0; i < TEX_REGIONS; i++) {
        if (regions[i].start != NULL &&
            ((char *)regions[i].end - (char *)regions[i].cur) > (32 * 1024)) {
            return 1;
        }
    }
    return 0;
}

unsigned char *texman_get_tex_data(unsigned int num) {
    return textures[num].location;
}

unsigned char texman_get_tex_type(unsigned int num) {
    return textures[num].type;
}

struct PSP_Texture *texman_reserve_memory(int width, int height, unsigned int type) {
    unsigned int tex_size = getMemorySize(width, height, type);
    int i;

    /* Take the first region with room, starting at the one in use. Nothing
     * here used to check the end of the buffer at all: the caller only
     * guarantees 32 KB of headroom, but a 256x256 RGBA texture is 256 KB, so a
     * big enough texture walked straight past the end into unrelated memory. */
    for (i = 0; i < TEX_REGIONS; i++) {
        const int idx = (region_cur + i) % TEX_REGIONS;
        struct TexRegion *r = &regions[idx];
        void *next;

        if (r->start == NULL) {
            continue;
        }

        next = (void *) ((((unsigned int) r->cur + tex_size + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT)
                         * TEX_ALIGNMENT);
        if (next > r->end) {
            continue;
        }

        if (idx != region_cur) {
            ++psp_tex_spills;
            region_cur = idx;
        }
        textures[psp_tex_number].location = r->cur;
        r->cur = next;
        return &textures[psp_tex_number];
    }

    /* Every region full. Wrap the first one rather than scribbling past its
     * end: that corrupts the OLDEST texture instead of unrelated memory, and
     * the caches are about to be wiped anyway. */
    ++psp_tex_overflows;
    region_cur = 0;
    regions[0].cur = regions[0].start;
    textures[psp_tex_number].location = regions[0].cur;
    regions[0].cur = (void *) ((((unsigned int) regions[0].cur + tex_size + TEX_ALIGNMENT - 1) /
                                TEX_ALIGNMENT) * TEX_ALIGNMENT);
    return &textures[psp_tex_number];
}

unsigned int texman_create(void) {
    /* textures[] is a fixed 512 entries. gfx_pc.c keeps its own pool in
     * lockstep and wipes both when either fills, so this should not be
     * reachable -- but an unchecked ++ here writes out of bounds if it ever
     * is, which is exactly the corruption class this file already caused once
     * (see the comment on the pool wipe in gfx_texture_cache_lookup). */
    if (psp_tex_number + 1 >= (unsigned int) (sizeof(textures) / sizeof(textures[0]))) {
        texman_clear();
    }

    psp_tex_number++;
    textures[psp_tex_number] = (struct PSP_Texture){
        location : regions[region_cur].cur,
        width : 0,
        height : 0,
        type : 0,
        swizzled : 0
    };
    psp_tex_bound = psp_tex_number;

#ifdef DEBUG
    printf("TEX_MAN new tex [%d] @ %x\n", psp_tex_number, (unsigned int)regions[region_cur].cur);
#endif
    return psp_tex_number;
}

void texman_upload_swizzle(int width, int height, unsigned int type, const void *buffer) {
    struct PSP_Texture *current = texman_reserve_memory(width, height, type);
    sceKernelDcacheWritebackRange(buffer, getMemorySize(width, height, type));
    current->width = width;
    current->height = height;
    current->type = type;
    /* 32bpp = 4 bytes, width is in bytes */
    swizzle_fast(current->location, buffer, getTexWidthBytes(width, type), height);
    current->swizzled = GU_TRUE;
#ifdef DEBUG
    printf("TEX_MAN upload swizzled [%d]\n", psp_tex_number);
#endif
    sceKernelDcacheWritebackRange(current->location, getMemorySize(width, height, type));
    sceKernelDcacheInvalidateRange(current->location, getMemorySize(width, height, type));
    texman_bind_tex(psp_tex_number);
}

void texman_upload(int width, int height, unsigned int type, const void *buffer) {
    struct PSP_Texture *current = texman_reserve_memory(width, height, type);
    sceKernelDcacheWritebackRange(buffer, getMemorySize(width, height, type));
    current->width = width;
    current->height = height;
    current->type = type;
    current->swizzled = GU_FALSE;
    memcpy(current->location, buffer, getMemorySize(width, height, type));
#ifdef DEBUG
    // printf("TEX_MAN upload plain [%d]\n", psp_tex_number);
#endif
    sceKernelDcacheWritebackRange(current->location, getMemorySize(width, height, type));
    sceKernelDcacheInvalidateRange(current->location, getMemorySize(width, height, type));
    texman_bind_tex(psp_tex_number);
}

void texman_bind_tex(unsigned int num) {
    const struct PSP_Texture *current = &textures[num];
#ifdef DEBUG
    /* Note this will SPAM if you enable */
    // if (psp_tex_bound != num)
    //    printf("TEX_MAN bind tex [%d]\n", num);
#endif
    sceGuTexMode(current->type, 0, 0, current->swizzled);
    sceGuTexImage(0, current->width, current->height, current->width, current->location);
    psp_tex_bound = num;
}

unsigned int texman_get_bound(void) {
    return psp_tex_bound;
}