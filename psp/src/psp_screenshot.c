/* See psp/include/psp_screenshot.h for why this exists. */

#include "psp_screenshot.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "psp_blob_assets.h"

static int sPending;
static unsigned int sSeq;
static unsigned int sStatCount;
static unsigned int sStatFails;

int gPspShotOnBgChange;
static const void *sLastBgImg;

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

void PspScreenshot_NoteBgImage(const void *img) {
    if (img != sLastBgImg) {
        sLastBgImg = img;
        if (gPspShotOnBgChange) {
            /* Two, not one: which frame is "the first" depends on where in the
             * room-load sequence the image pointer moves, and a second file
             * costs nothing next to guessing wrong and having to reproduce the
             * whole thing again. */
            PspScreenshot_Request(2);
        }
    }
}

static unsigned int sLastCamSetting = 0xFFFFFFFFu;

void PspScreenshot_NoteCamSetting(unsigned int setting) {
    if (setting != sLastCamSetting) {
        unsigned int was = sLastCamSetting;

        sLastCamSetting = setting;
        /* Not on the first call: at startup every value is "new", and a shot
         * of the boot logo is not what anyone armed this for. */
        if (gPspShotOnBgChange && (was != 0xFFFFFFFFu)) {
            PspScreenshot_Request(2);
        }
    }
}

/* 54-byte BITMAPFILEHEADER + BITMAPINFOHEADER, little-endian, which is also
 * this machine's byte order -- so the fields are written out a byte at a time
 * rather than as structs, and no packing attribute can go wrong. */
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
}
