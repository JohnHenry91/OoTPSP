/* N64 ROMs are big-endian; the PSP CPU (MIPS Allegrex) is little-endian.
 * Scene/room command streams (SceneCmd, include/scene.h) are DMA'd raw from
 * the .z64 file with no byte-swap anywhere in the pipeline, so every 32-bit
 * scalar/pointer field inside them reads back byte-reversed on PSP.
 *
 * The 8-byte SceneCmd layout is NOT uniformly swappable, though: word0
 * (bytes 0-3) is always `u8 code; u8 data1;` plus 2 padding bytes -- these
 * are read via single-byte loads and must be left alone (byte-swapping them
 * would move the `code`/`data1` bytes to the wrong offset, not fix them).
 * word1 (bytes 4-7) is a genuine 32-bit scalar or segmented pointer for most
 * commands (CMD_PTR/CMD_W in include/command_macros_base.h) and DOES need
 * swapping -- except for the handful of commands built with CMD_BBBB, which
 * pack 4 independent u8 sub-fields into word1 (e.g. SCmdWindSettings.x/y/z);
 * those are also single-byte reads and must NOT be swapped. See
 * project_oot_psp_port_v2_phase2 memory for the full diagnosis. */
#include "z_endian_fixup_psp.h"

static void PspSwapU16At(unsigned char* p, unsigned int off) {
    unsigned char t = p[off];
    p[off] = p[off + 1];
    p[off + 1] = t;
}

static void PspSwapU32At(unsigned char* p, unsigned int off) {
    unsigned char t0 = p[off];
    unsigned char t1 = p[off + 1];
    p[off] = p[off + 3];
    p[off + 1] = p[off + 2];
    p[off + 2] = t1;
    p[off + 3] = t0;
}

/* Field offsets match include/bgcheck.h's CollisionHeader (0x2C bytes total):
 *   0x00 Vec3s minBounds (3x s16)   0x06 Vec3s maxBounds (3x s16)
 *   0x0C u16 numVertices            0x0E pad
 *   0x10 Vec3s* vtxList             0x14 u16 numPolygons   0x16 pad
 *   0x18 CollisionPoly* polyList    0x1C SurfaceType* surfaceTypeList
 *   0x20 BgCamInfo* bgCamList       0x24 u16 numWaterBoxes 0x26 pad
 *   0x28 WaterBox* waterBoxes */
void PspFixupCollisionHeaderEndian(void* colHeader) {
    unsigned char* p = (unsigned char*)colHeader;

    PspSwapU16At(p, 0x00);
    PspSwapU16At(p, 0x02);
    PspSwapU16At(p, 0x04);
    PspSwapU16At(p, 0x06);
    PspSwapU16At(p, 0x08);
    PspSwapU16At(p, 0x0A);
    PspSwapU16At(p, 0x0C);
    PspSwapU32At(p, 0x10);
    PspSwapU16At(p, 0x14);
    PspSwapU32At(p, 0x18);
    PspSwapU32At(p, 0x1C);
    PspSwapU32At(p, 0x20);
    PspSwapU16At(p, 0x24);
    PspSwapU32At(p, 0x28);
}

void PspFixupVtxListEndian(void* vtxList, unsigned int count) {
    unsigned char* p = (unsigned char*)vtxList;
    unsigned int i;

    for (i = 0; i < count; i++) {
        PspSwapU16At(p, 0);
        PspSwapU16At(p, 2);
        PspSwapU16At(p, 4);
        p += 6;
    }
}

void PspFixupPolyListEndian(void* polyList, unsigned int count) {
    unsigned char* p = (unsigned char*)polyList;
    unsigned int i;

    for (i = 0; i < count; i++) {
        PspSwapU16At(p, 0x00);  // type
        PspSwapU16At(p, 0x02);  // flags_vIA / vtxData[0]
        PspSwapU16At(p, 0x04);  // flags_vIB / vtxData[1]
        PspSwapU16At(p, 0x06);  // vIC / vtxData[2]
        PspSwapU16At(p, 0x08);  // normal.x
        PspSwapU16At(p, 0x0A);  // normal.y
        PspSwapU16At(p, 0x0C);  // normal.z
        PspSwapU16At(p, 0x0E);  // dist
        p += 0x10;
    }
}

void PspFixupActorEntryListEndian(void* actorEntryList, unsigned int count) {
    unsigned char* p = (unsigned char*)actorEntryList;
    unsigned int i;
    unsigned int j;

    for (i = 0; i < count; i++) {
        for (j = 0; j < 8; j++) {
            PspSwapU16At(p, j * 2);
        }
        p += 0x10;
    }
}

void PspFixupTransitionActorEntryListEndian(void* transitionActorList, unsigned int count) {
    unsigned char* p = (unsigned char*)transitionActorList;
    unsigned int i;

    for (i = 0; i < count; i++) {
        // bytes 0-1 (sides[2]) are s8 fields, left alone
        PspSwapU16At(p, 0x04); // id
        PspSwapU16At(p, 0x06); // pos.x
        PspSwapU16At(p, 0x08); // pos.y
        PspSwapU16At(p, 0x0A); // pos.z
        PspSwapU16At(p, 0x0C); // rotY
        PspSwapU16At(p, 0x0E); // params
        p += 0x10;
    }
}

void PspFixupRomFileListEndian(void* romFileList, unsigned int count) {
    unsigned char* p = (unsigned char*)romFileList;
    unsigned int i;

    for (i = 0; i < count; i++) {
        PspSwapU32At(p, 0x00); // vromStart
        PspSwapU32At(p, 0x04); // vromEnd
        p += 0x08;
    }
}

void PspFixupS16ArrayEndian(void* data, unsigned int count) {
    unsigned char* p = (unsigned char*)data;
    unsigned int i;

    for (i = 0; i < count; i++) {
        PspSwapU16At(p, 0);
        p += 2;
    }
}

extern unsigned int gSegments[16];
#define PSP_K0BASE 0x80000000U

/* Returns NULL if the segment isn't populated yet, instead of blindly
 * dereferencing garbage (defensive -- this whole fixup pass runs on raw,
 * not-yet-fully-validated ROM data). */
static unsigned char* PspSegmentedToVirtual(unsigned int segAddr) {
    unsigned int segNum = (segAddr << 4) >> 28;
    unsigned int segOff = segAddr & 0x00FFFFFF;
    if (gSegments[segNum] == 0) {
        return (unsigned char*)0;
    }
    return (unsigned char*)(gSegments[segNum] + segOff + PSP_K0BASE);
}

static unsigned int PspReadU32(unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static unsigned short PspReadU16(unsigned char* p) {
    return (unsigned short)(((unsigned int)p[0] << 8) | p[1]);
}

/* BgCamInfo (include/bgcheck.h): 0x0 u16 setting, 0x2 s16 count, 0x4 Vec3s*
 * bgCamFuncData -- see header comment for why these read on demand instead
 * of doing a bulk in-place swap like the other fixups. */
unsigned short PspReadBgCamSettingRaw(void* bgCamInfoEntry) {
    if (bgCamInfoEntry == 0) {
        return 0;
    }
    return PspReadU16((unsigned char*)bgCamInfoEntry + 0x0);
}

void* PspReadBgCamFuncDataRaw(void* bgCamInfoEntry) {
    if (bgCamInfoEntry == 0) {
        return 0;
    }
    return PspSegmentedToVirtual(PspReadU32((unsigned char*)bgCamInfoEntry + 0x4));
}

short PspReadBgCamCountRaw(void* bgCamInfoEntry) {
    if (bgCamInfoEntry == 0) {
        return 0;
    }
    return (short)PspReadU16((unsigned char*)bgCamInfoEntry + 0x2);
}

/* BgCamFuncData (include/bgcheck.h, 0x12 bytes: Vec3s pos, Vec3s rot, s16 fov,
 * s16 union{roomImageOverrideBgCamIndex,timer,flags}, s16 unk_10 -- all 9
 * fields are plain s16/u16 scalars) is raw-DMA'd data too, never
 * byte-swapped -- real unmodified `Camera_Fixed3` (src/code/z_camera.c) reads
 * its fields directly as native. Rather than an in-place swap (this same
 * struct gets re-resolved every frame a fixed camera is active, and an
 * in-place swap has no cheap way to avoid double-swapping on the 2nd+ call
 * without extra bookkeeping), fill a caller-provided native-layout copy on
 * demand -- safe to call every frame. `out` must point at a real
 * BgCamFuncData-sized (0x12-byte) buffer; only the byte layout is assumed,
 * not the real struct definition (this file avoids depending on game
 * headers), so the caller passes `sizeof(BgCamFuncData)` implicitly by using
 * a real `BgCamFuncData` local. */
void PspReadBgCamFuncDataStruct(void* raw, void* out) {
    unsigned char* p = (unsigned char*)raw;
    unsigned char* o = (unsigned char*)out;
    int i;

    if (raw == 0) {
        return;
    }
    for (i = 0; i < 0x12; i += 2) {
        unsigned short v = PspReadU16(p + i);
        o[i] = (unsigned char)(v >> 8);
        o[i + 1] = (unsigned char)v;
    }
}

static void PspFixupDisplayListEndianImpl(unsigned char* p, int depth);

/* Resolve+fix the display lists referenced by two RAW (still-big-endian)
 * segmented Gfx* pointer values. Must be given the pre-swap values -- reading
 * the fields after they've been byte-swapped would re-interpret native-order
 * bytes as big-endian and yield a garbage address. */
static void PspFixupEntryDisplayLists(unsigned int opaRaw, unsigned int xluRaw) {
    unsigned char* dl;

    dl = PspSegmentedToVirtual(opaRaw);
    if (dl != 0) {
        PspFixupDisplayListEndianImpl(dl, 0);
    }
    dl = PspSegmentedToVirtual(xluRaw);
    if (dl != 0) {
        PspFixupDisplayListEndianImpl(dl, 0);
    }
}

/* include/room.h's RoomShapeBase/Normal/Cullable, ROOM_SHAPE_TYPE_NORMAL=0,
 * ROOM_SHAPE_TYPE_CULLABLE=2. See that header for exact field offsets. */
void PspFixupRoomShapeEndian(void* roomShape) {
    unsigned char* p = (unsigned char*)roomShape;
    unsigned char type = p[0];

    if (type == 0) { /* ROOM_SHAPE_TYPE_NORMAL */
        unsigned char* entryP;
        unsigned char* entryEndP;

        /* Resolve the segmented pointers from their still-big-endian bytes
         * FIRST, then swap the fields -- reading after the swap would
         * re-interpret native-order bytes as big-endian and yield garbage. */
        entryP = PspSegmentedToVirtual(PspReadU32(p + 0x04));
        entryEndP = PspSegmentedToVirtual(PspReadU32(p + 0x08));
        PspSwapU32At(p, 0x04); /* entries */
        PspSwapU32At(p, 0x08); /* entriesEnd */

        while (entryP != 0 && entryP < entryEndP) {
            unsigned int opaRaw = PspReadU32(entryP + 0x00);
            unsigned int xluRaw = PspReadU32(entryP + 0x04);
            PspSwapU32At(entryP, 0x00); /* opa */
            PspSwapU32At(entryP, 0x04); /* xlu */
            PspFixupEntryDisplayLists(opaRaw, xluRaw);
            entryP += 0x08;
        }
    } else if (type == 2) { /* ROOM_SHAPE_TYPE_CULLABLE */
        unsigned char* entryP;
        unsigned char* entryEndP;

        entryP = PspSegmentedToVirtual(PspReadU32(p + 0x04));
        entryEndP = PspSegmentedToVirtual(PspReadU32(p + 0x08));
        PspSwapU32At(p, 0x04); /* entries */
        PspSwapU32At(p, 0x08); /* entriesEnd */

        while (entryP != 0 && entryP < entryEndP) {
            unsigned int opaRaw = PspReadU32(entryP + 0x08);
            unsigned int xluRaw = PspReadU32(entryP + 0x0C);
            PspSwapU16At(entryP, 0x00); /* boundsSphereCenter.x */
            PspSwapU16At(entryP, 0x02); /* boundsSphereCenter.y */
            PspSwapU16At(entryP, 0x04); /* boundsSphereCenter.z */
            PspSwapU16At(entryP, 0x06); /* boundsSphereRadius */
            PspSwapU32At(entryP, 0x08); /* opa */
            PspSwapU32At(entryP, 0x0C); /* xlu */
            PspFixupEntryDisplayLists(opaRaw, xluRaw);
            entryP += 0x10;
        }
    } else if (type == 1) { /* ROOM_SHAPE_TYPE_IMAGE */
        unsigned char amountType = p[1];

        {
            unsigned int entrySeg = PspReadU32(p + 0x04);
            unsigned char* entryP = PspSegmentedToVirtual(entrySeg);
#if 1 /* TEMP diagnostic: dump raw roomShape words 0x00,0x04,0x08,0x0C */
            {
                extern void PspDebugLogDmaAlignErr(unsigned int, unsigned int, unsigned int, unsigned int);
                PspDebugLogDmaAlignErr(0xEE000000 | p[0], PspReadU32(p + 0x00), PspReadU32(p + 0x04),
                                       PspReadU32(p + 0x08));
                PspDebugLogDmaAlignErr(0xEE000001 | (p[1] << 8), PspReadU32(p + 0x0C),
                                       (unsigned int)(unsigned long)p, (unsigned int)(unsigned long)entryP);
            }
#endif
            PspSwapU32At(p, 0x04); /* entry (RoomShapeDListsEntry*) */
            if (entryP != 0) {
                unsigned int opaRaw = PspReadU32(entryP + 0x00);
                unsigned int xluRaw = PspReadU32(entryP + 0x04);
                PspSwapU32At(entryP, 0x00); /* opa */
                PspSwapU32At(entryP, 0x04); /* xlu */
                PspFixupEntryDisplayLists(opaRaw, xluRaw);
            }
        }

        if (amountType == 1) { /* ROOM_SHAPE_IMAGE_AMOUNT_SINGLE */
            PspSwapU32At(p, 0x08); /* source */
            PspSwapU32At(p, 0x0C); /* unk_0C */
            PspSwapU32At(p, 0x10); /* tlut */
            PspSwapU16At(p, 0x14); /* width */
            PspSwapU16At(p, 0x16); /* height */
            PspSwapU16At(p, 0x1A); /* tlutMode */
            PspSwapU16At(p, 0x1C); /* tlutCount */
        } else if (amountType == 2) { /* ROOM_SHAPE_IMAGE_AMOUNT_MULTI */
            unsigned char numBackgrounds = p[0x08];
            unsigned char* bgP;
            unsigned char* bgEndP;
            unsigned int i;

            bgP = PspSegmentedToVirtual(PspReadU32(p + 0x0C));
            PspSwapU32At(p, 0x0C); /* backgrounds */
            bgEndP = (bgP != 0) ? bgP + (unsigned int)numBackgrounds * 0x1C : 0;
            for (i = 0; bgP != 0 && bgP < bgEndP; i++, bgP += 0x1C) {
                PspSwapU16At(bgP, 0x00); /* unk_00 */
                PspSwapU32At(bgP, 0x04); /* source */
                PspSwapU32At(bgP, 0x08); /* unk_0C */
                PspSwapU32At(bgP, 0x0C); /* tlut */
                PspSwapU16At(bgP, 0x10); /* width */
                PspSwapU16At(bgP, 0x12); /* height */
                PspSwapU16At(bgP, 0x16); /* tlutMode */
                PspSwapU16At(bgP, 0x18); /* tlutCount */
            }
        }
    }
}

#define PSP_G_ENDDL 0xDF
#define PSP_G_DL 0xDE
#define PSP_G_VTX 0x01
#define PSP_DL_MAX_CMDS 4096
#define PSP_DL_MAX_DEPTH 8
#define PSP_VTX_MAX_COUNT 256

/* F3DEX2 `Vtx` (include/ultra64/gbi.h): ob[3] (3x s16), flag (u16), tc[2] (2x
 * s16) -- all need swapping -- followed by cn[4]/n[3]+a, which are all
 * single-byte fields (color/alpha or normal+alpha) and must be left alone,
 * same reasoning as SceneCmd's code/data1. 16 bytes total per vertex. */
static void PspFixupGfxVtxArrayEndian(unsigned char* vp, unsigned int count) {
    unsigned int i;

    if (vp == 0) {
        return;
    }
    if (count > PSP_VTX_MAX_COUNT) {
        count = PSP_VTX_MAX_COUNT;
    }
    for (i = 0; i < count; i++) {
        PspSwapU16At(vp, 0x00); /* ob[0] x */
        PspSwapU16At(vp, 0x02); /* ob[1] y */
        PspSwapU16At(vp, 0x04); /* ob[2] z */
        PspSwapU16At(vp, 0x06); /* flag */
        PspSwapU16At(vp, 0x08); /* tc[0] s */
        PspSwapU16At(vp, 0x0A); /* tc[1] t */
        vp += 0x10;
    }
}

static void PspFixupDisplayListEndianImpl(unsigned char* p, int depth) {
    int i;

    if (depth > PSP_DL_MAX_DEPTH || p == 0) {
        return;
    }

    for (i = 0; i < PSP_DL_MAX_CMDS; i++) {
        unsigned char opcode = p[0];

        if (opcode == PSP_G_DL) {
            unsigned int rawW1 = PspReadU32(p + 4);
            unsigned char* subDl;

            PspSwapU32At(p, 0x00);
            PspSwapU32At(p, 0x04);

            subDl = PspSegmentedToVirtual(rawW1);
            if (subDl != 0) {
                PspFixupDisplayListEndianImpl(subDl, depth + 1);
            }

            /* bit 0 of the (pre-swap) w0 low byte distinguishes G_DL_PUSH
             * (branch-and-link, execution returns and continues here) from
             * G_DL_NOPUSH (branch, does not return) -- for OUR sweep we
             * always keep walking this buffer regardless, since we're just
             * finding every command physically present, not simulating
             * real display-list execution flow. */
            p += 8;
        } else if (opcode == PSP_G_VTX) {
            /* F3DEX2 encoding: w0 = (0x01<<24) | (numv<<12) | (vbidx<<1) | ...,
             * w1 = segmented Vtx* -- read both from the still-big-endian raw
             * bytes first (PspReadU32 converts to the correct native-order
             * scalar), matching gfx_pc.c's C0/C1(w0)/w1 decode exactly since
             * that decode is just shift+mask on the whole word, order-
             * independent of when the swap happens. */
            unsigned int rawW0 = PspReadU32(p + 0);
            unsigned int rawW1 = PspReadU32(p + 4);
            unsigned int vbidx = (rawW0 >> 12) & 0xFF;
            unsigned int numv = ((rawW0 >> 1) & 0x7F) - vbidx;
            unsigned char* vp = PspSegmentedToVirtual(rawW1);

            PspSwapU32At(p, 0x00);
            PspSwapU32At(p, 0x04);

            PspFixupGfxVtxArrayEndian(vp, numv);

            p += 8;
        } else if (opcode == PSP_G_ENDDL) {
            PspSwapU32At(p, 0x00);
            PspSwapU32At(p, 0x04);
            return;
        } else {
            PspSwapU32At(p, 0x00);
            PspSwapU32At(p, 0x04);
            p += 8;
        }
    }
}

/* See header comment. `dl` must already be a resolved (non-segmented)
 * pointer to raw-DMA'd display list data. */
void PspFixupDisplayListEndian(void* dl) {
    if (dl == 0) {
        return;
    }
    PspFixupDisplayListEndianImpl((unsigned char*)dl, 0);
}

static int PspSceneCmdWord1IsPackedBytes(unsigned int code) {
    switch (code) {
        case 5:  /* SCENE_CMD_ID_WIND_SETTINGS */
        case 16: /* SCENE_CMD_ID_TIME_SETTINGS */
        case 17: /* SCENE_CMD_ID_SKYBOX_SETTINGS */
        case 18: /* SCENE_CMD_ID_SKYBOX_DISABLES */
        case 21: /* SCENE_CMD_ID_SOUND_SETTINGS */
        case 22: /* SCENE_CMD_ID_ECHO_SETTINGS */
            return 1;
        default:
            return 0;
    }
}

/* The scene/room file blob is NOT purely a command array -- the commands are
 * only a leading run terminated by SCENE_CMD_ID_END (20); the rest of the
 * blob holds whatever data those commands point into (collision header,
 * path lists, etc.), which must NOT be reinterpreted as more 8-byte
 * commands. `size` is only an upper safety bound in case END is never
 * found (corrupt/unexpected data) -- normal termination is via the END
 * command itself. */
void PspFixupCommandStreamEndian(void* data, unsigned int size) {
    unsigned char* base = (unsigned char*)data;
    unsigned int maxCmds = size / 8;
    unsigned int i;

    for (i = 0; i < maxCmds; i++) {
        unsigned char* cmd = base + (i * 8);
        unsigned int code = cmd[0];

        if (code > 0x19) {
            /* Not a valid SCENE_CMD_ID -- stop, this is no longer command
             * data (or our assumptions about this stream are wrong). */
            break;
        }

        if (!PspSceneCmdWord1IsPackedBytes(code)) {
            unsigned char t0 = cmd[4];
            unsigned char t1 = cmd[5];
            unsigned char t2 = cmd[6];
            unsigned char t3 = cmd[7];
            cmd[4] = t3;
            cmd[5] = t2;
            cmd[6] = t1;
            cmd[7] = t0;
        }

        if (code == 20 /* SCENE_CMD_ID_END */) {
            break;
        }
    }
}
