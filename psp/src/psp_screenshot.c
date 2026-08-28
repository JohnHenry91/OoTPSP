/* See psp/include/psp_screenshot.h for why this exists. */

#include "psp_screenshot.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "psp_blob_assets.h"

/* Declared by hand rather than by including gfx/gfx_pc.h: that header's other
 * prototypes need the N64 Gfx type, and pulling the whole ultra64 headers into
 * a BMP writer to reach one struct is the wrong trade. Keep in step with
 * gfx_pc.h. */
struct GfxPcFrameSnapshot {
    unsigned int frame;
    unsigned int tris_drawn;
    unsigned int tri_calls;
    unsigned int flushes;
    unsigned int tex_imports;
    unsigned int tex_hits;
    unsigned int tex_used;
    unsigned int tex_unused;
    unsigned int settimg;
    unsigned int loadblock;
    unsigned int loadtile;
    unsigned int settile;
    unsigned int sky_tris;
    unsigned int sky_begins;
    unsigned int sky_calls;
    unsigned int sky_id;
    unsigned int sky_drawtype;
    unsigned int tex_unswap_yes;
    unsigned int tex_unswap_no;
    unsigned int sky_tex_imports;
    unsigned int sky_tex_unswap;
    unsigned int sky_tex_hits;
    unsigned int sky_seg0;
    unsigned int sky_seg0_native;
    unsigned int sky_pal;
    unsigned int sky_pal_native;
    unsigned int bind_desyncs;
    unsigned int bind_desyncs_frame;
    unsigned int bind_desyncs_2nd;
    unsigned int lerp2_draws;
};
void gfx_pc_stat_snapshot_current(struct GfxPcFrameSnapshot *out);

static int sPending;
static unsigned int sSeq;
static unsigned int sStatCount;
static unsigned int sStatFails;

/* ON by default. It was off, and had to be armed with a button combo that only
 * someone who had just read this file would know -- so the automatic grab, the
 * one built precisely for frames a human cannot time, reliably did not happen.
 *
 * Being on by default is only safe because of the budget below: without it,
 * a camera change every few steps in a prerendered room would write a 390 KB
 * file each time and fill the Memory Stick. */
int gPspShotOnBgChange = 1;

/* Automatic grabs left this session. The first occurrence is the one worth
 * having; the hundredth is just a full stick. The manual hotkey ignores this
 * and always works, so a deliberate press is never refused. */
static int sAutoBudget = 6;

static const void *sLastBgImg;

/* Automatic triggers that fired, whether or not a file came of it. Without
 * this, "no screenshots appeared" has three causes that look identical from
 * the Memory Stick: the trigger never fired, it fired and the budget was
 * spent, or it fired and every open failed. */
static unsigned int sStatAutoFired;

static void PspScreenshotRequestAuto(int frames) {
    ++sStatAutoFired;
    if (!gPspShotOnBgChange || sAutoBudget <= 0) {
        return;
    }
    --sAutoBudget;
    PspScreenshot_Request(frames);
}

unsigned int PspScreenshot_StatAutoFired(void) {
    return sStatAutoFired;
}

/* Called when a scene is loaded. The camera-setting trigger cannot cover
 * entering a room reliably -- it only runs from the room draw, and the setting
 * on the first drawn frame is not necessarily different from the last
 * prerendered room's. A scene load, by contrast, is exactly the event being
 * investigated. */
void PspScreenshot_NoteSceneLoad(void) {
    /* Three: the first frame of a new scene is not always the first frame
     * DRAWN, and the glitch is reported on entry rather than at a precise
     * frame index. */
    PspScreenshotRequestAuto(3);
}

void PspScreenshot_Request(int frames) {
    if (frames > 0) {
        sPending = frames;
    }
}

unsigned int PspScreenshot_StatCount(void) {
    return sStatCount;
}

unsigned int PspScreenshot_StatFails(void) {
    return sStatFails;
}

int PspScreenshot_StatAutoBudget(void) {
    return sAutoBudget;
}

void PspScreenshot_NoteBgImage(const void *img) {
    if (img != sLastBgImg) {
        sLastBgImg = img;
        /* Two, not one: which frame is "the first" depends on where in the
         * room-load sequence the image pointer moves, and a second file costs
         * nothing next to guessing wrong and having to reproduce the whole
         * thing again. */
        PspScreenshotRequestAuto(2);
    }
}

static unsigned int sLastCamSetting = 0xFFFFFFFFu;

void PspScreenshot_NoteCamSetting(unsigned int setting) {
    if (setting != sLastCamSetting) {
        unsigned int was = sLastCamSetting;

        sLastCamSetting = setting;
        /* Not on the first call: at startup every value is "new", and a shot
         * of the boot logo is not what this is for. */
        if (was != 0xFFFFFFFFu) {
            PspScreenshotRequestAuto(2);
        }
    }
}

/* 54-byte BITMAPFILEHEADER + BITMAPINFOHEADER, little-endian, which is also
 * this machine's byte order -- so the fields are written out a byte at a time
 * rather than as structs, and no packing attribute can go wrong. */
static void PspScreenshotWriteCounters(void);

static void PutU16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void PutU32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

void PspScreenshot_Tick(const void *fb565, int width, int height, int stride) {
    char path[128];
    unsigned char header[54];
    /* One row at a time. A whole 480x272 RGB frame is 383 KB and there is no
     * spare block of that size to stage it in -- and there is no need, since
     * the file is written top to bottom anyway. */
    unsigned char row[480 * 3];
    const unsigned short *src;
    unsigned int rowBytes;
    SceUID fd;
    int y;
    int x;

    if (sPending <= 0 || fb565 == NULL) {
        return;
    }
    --sPending;

    if (width > 480) {
        width = 480;
    }
    rowBytes = (unsigned int)width * 3u;

    snprintf(path, sizeof(path), "%sshot%03u.bmp", PspBlob_GetBaseDir(), sSeq);

    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        ++sStatFails;
        return;
    }
    ++sSeq;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    PutU32(header + 2, 54u + rowBytes * (unsigned int)height);
    PutU32(header + 10, 54u);
    PutU32(header + 14, 40u);
    PutU32(header + 18, (unsigned int)width);
    PutU32(header + 22, (unsigned int)height); /* positive: rows run bottom-up */
    PutU16(header + 26, 1u);
    PutU16(header + 28, 24u);
    PutU32(header + 34, rowBytes * (unsigned int)height);
    sceIoWrite(fd, header, sizeof(header));

    /* BMP stores the bottom row first, so walk the framebuffer upwards. */
    for (y = height - 1; y >= 0; y--) {
        src = (const unsigned short *)fb565 + (size_t)y * (size_t)stride;

        for (x = 0; x < width; x++) {
            unsigned int p = src[x];
            /* GU_PSM_5650 -> 8 bits per channel.
             *
             * RED IS IN THE LOW BITS. The PSP's 5650 is not the PC's RGB565:
             * red occupies bits 0-4 and blue bits 11-15. This file first
             * assumed the PC order, which swapped red and blue in every shot
             * -- and the giveaway was Link coming out with cyan skin, an
             * artefact of the debugging tool that could easily have been
             * reported as a rendering bug and chased for a day. The port's own
             * psp/tools/jfif_to_psp.py settles the order: it packs 5551 as
             * `(b << 10) | (g << 5) | r`.
             *
             * The top bits are replicated into the low ones so full-scale
             * stays full-scale: a plain shift left would cap white at 0xF8 and
             * tint every screenshot slightly dark. */
            unsigned int r = p & 0x1F;
            unsigned int g = (p >> 5) & 0x3F;
            unsigned int b = (p >> 11) & 0x1F;

            row[x * 3 + 0] = (unsigned char)((b << 3) | (b >> 2)); /* BMP is BGR */
            row[x * 3 + 1] = (unsigned char)((g << 2) | (g >> 4));
            row[x * 3 + 2] = (unsigned char)((r << 3) | (r >> 2));
        }
        sceIoWrite(fd, row, (SceSize)rowBytes);
    }

    sceIoClose(fd);
    ++sStatCount;

    PspScreenshotWriteCounters();
}

/* Write the renderer's counters beside the image.
 *
 * This exists because two fixes were built on an association that was never
 * checked. A texture-cache exhaustion was measured once, and the corrupted
 * frames were captured separately with the HUD switched off -- so "the glitch
 * frame is an exhaustion" was an assumption, and both fixes aimed at it
 * changed nothing. A picture without its numbers is an anecdote.
 *
 * Written as plain text next to the BMP, at the same moment, so the two can
 * never be attributed to different frames. */
static void PspScreenshotWriteCounters(void) {
    extern unsigned int gPspTexCacheResetVram;
    extern unsigned int gPspTexCacheResetPool;
    extern unsigned int gPspTexCacheHighWater;
    extern unsigned int psp_tex_overflows;
    extern unsigned int psp_tex_spills;
    extern unsigned int gPspTexSpillBytes;
    extern unsigned int gPspPoolOverflows;
    extern unsigned int gPspBlobShortReads;
    extern unsigned int gPspRomUnservedReads;
    extern unsigned int gPspZeldaAllocFails;
    extern unsigned int gPspGfxBadDlCursors;
    extern unsigned int gPspBgDrawn;
    extern unsigned int gPspBgSkipped;
    extern unsigned int gPspBgLastSkipReason;
    extern unsigned int gPspBgProbeCamSetting;
    extern unsigned int gPspTexSizeVariants;

    char path[128];
    char text[512];
    int len;
    SceUID fd;

    snprintf(path, sizeof(path), "%sshot%03u.txt", PspBlob_GetBaseDir(), sSeq - 1u);
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }

    len = snprintf(text, sizeof(text),
                   "wipeVram %u\nwipePool %u\npoolHigh %u\nsizeVariants %u\n"
                   "texOverflow %u\ntexSpills %u\nspillBytes %u\ndlPool %u\n"
                   "blobShort %u\nromUnserved %u\narenaFail %u\nbadDl %u\n"
                   "bgDrawn %u\nbgSkipped %u\nbgSkipReason %u\ncamSetting %u\n",
                   gPspTexCacheResetVram, gPspTexCacheResetPool, gPspTexCacheHighWater,
                   gPspTexSizeVariants, psp_tex_overflows, psp_tex_spills, gPspTexSpillBytes,
                   gPspPoolOverflows, gPspBlobShortReads, gPspRomUnservedReads, gPspZeldaAllocFails,
                   gPspGfxBadDlCursors, gPspBgDrawn, gPspBgSkipped, gPspBgLastSkipReason,
                   gPspBgProbeCamSetting);
    if (len > 0) {
        sceIoWrite(fd, text, (SceSize)len);
    }

    /* The counters above are all RESOURCE counters, and on the frame this was
     * built for every one of them read zero -- which is a real result, but a
     * purely negative one: it says what the broken frame is not. These say what
     * the frame actually DID.
     *
     * They are what separates the two live explanations for a single corrupted
     * frame on room entry, without another round trip:
     *
     *   tex_imports large   -> this frame decoded and uploaded the room's whole
     *                          texture set, and the corruption rides on the
     *                          upload (stride, swizzle, or a GE that read the
     *                          pool before the CPU's dirty cache lines reached
     *                          RAM). Later frames are all cache hits, which is
     *                          exactly why only the first frame breaks.
     *   tex_imports ~0      -> nothing was uploaded, so the upload path is out.
     *
     *   sky_tris large      -> the skybox is on screen and is a candidate for
     *                          the rectangular blocks (its faces ARE a grid of
     *                          quads, and in a PREREND-PIVOT room it is the
     *                          visible environment, not decoration).
     *   sky_tris 0          -> the skybox drew nothing and is out.
     *
     * tex_used/tex_unused settle the third question the display lists raise:
     * the shrine room's own polygons ask for no texture at all
     * (gsSPTexture(..., G_OFF), a combine with no TEXEL), yet the "no textures"
     * hack removes the corruption. If tex_used is far above the number of
     * genuinely textured surfaces, something untextured is being drawn
     * textured. */
    {
        struct GfxPcFrameSnapshot g;

        gfx_pc_stat_snapshot_current(&g);
        len = snprintf(text, sizeof(text),
                       "frame %u\ntrisDrawn %u\ntriCalls %u\nflushes %u\n"
                       "texImports %u\ntexHits %u\ntexUsed %u\ntexUnused %u\n"
                       "setTimg %u\nloadBlock %u\nloadTile %u\nsetTile %u\n"
                       "skyTris %u\nskyBeginsTotal %u\nskyCallsTotal %u\nskyId %u\nskyDrawType %u\n",
                       g.frame, g.tris_drawn, g.tri_calls, g.flushes,
                       g.tex_imports, g.tex_hits, g.tex_used, g.tex_unused,
                       g.settimg, g.loadblock, g.loadtile, g.settile,
                       g.sky_tris, g.sky_begins, g.sky_calls, g.sky_id, g.sky_drawtype);
        if (len > 0) {
            sceIoWrite(fd, text, (SceSize)len);
        }

        /* The endian verdict, per import and for the skybox's own buffers. */
        len = snprintf(text, sizeof(text),
                       "unswapYes %u\nunswapNo %u\n"
                       "skyTexImports %u\nskyTexUnswap %u\nskyTexHits %u\n"
                       "skySeg0 %08x\nskySeg0Native %u\nskyPal %08x\nskyPalNative %u\n"
                       "bindDesync %u\nbindDesyncFrame %u\nbindDesync2nd %u\nlerp2Draws %u\n",
                       g.tex_unswap_yes, g.tex_unswap_no,
                       g.sky_tex_imports, g.sky_tex_unswap, g.sky_tex_hits,
                       g.sky_seg0, g.sky_seg0_native, g.sky_pal, g.sky_pal_native,
                       g.bind_desyncs, g.bind_desyncs_frame, g.bind_desyncs_2nd, g.lerp2_draws);
        if (len > 0) {
            sceIoWrite(fd, text, (SceSize)len);
        }
    }

    sceIoClose(fd);
}
