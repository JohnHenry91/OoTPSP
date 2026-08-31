#include "gfx.h"
#include "gfx_setupdl.h"
#include "sys_matrix.h"
#include "skybox.h"
#if TARGET_PSP
#include "gfx/psp_bg_rect.h"
#endif

Mtx* sSkyboxDrawMatrix;
#if TARGET_PSP
/* Session 16: draw only the selected face display lists, bit k == dListBuf[k].
 * The four side faces are drawn -z, +x, +z, -x with no depth write, so a face
 * that projects badly simply paints over the one that projected correctly.
 * Poke 0x30 for the +z face alone, etc.
 *
 * THE DEFAULT WAS 0xFF AND THAT WAS A BUG, not a setting. The 128-type skybox
 * -- every outdoor scene -- draws faces 0, 2, 4, 6 and 8, and 8 is the TOP.
 * 1 << 8 is 0x100, outside an 0xFF mask, so the ceiling of the sky was never
 * drawn: a black rectangle straight overhead, which is what the user reported
 * in Hyrule Field ("a square black area at the top"), Gerudo's Fortress,
 * Kakariko Village, Lon Lon Ranch, Lake Hylia and Outside Ganon's Castle. Face
 * 10 (the cutscene map's floor) was dropped the same way.
 *
 * A diagnostic default that silently does not cover its own range -- this
 * port's most expensive recurring mistake, and the reason the range is now
 * spelt out rather than left at "all the bits I happened to think of". */
int gDebugSkyFaceMask = 0xFFF;
/* Bisect-Schalter, definiert in psp/src/gfx/gfx_scegu.c. */
extern int gPspSkyDecalNoBlend;
#define SKY_DL(k)                                             \
    do {                                                      \
        if (gDebugSkyFaceMask & (1 << (k))) {                 \
            gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[k]); \
        }                                                     \
    } while (0)
#else
#define SKY_DL(k) gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[k])
#endif
#if TARGET_PSP
/* [8] and [9] carry the two addresses the CI8 decoder's endian verdict is taken
 * from -- see the endian-classification probe on PspGfxFrameStats in gfx_pc.c.
 * These are raw .z64 buffers in the shared arena, so whether an abandoned blob
 * range still covers them is a runtime fact, not a static one. */
u32 gPspSkyCall[12] = { 0 };
u32 gPspSkyVtx[8] = { 0 };
u32 gPspSkyVtxDump[96] = { 0 };
#endif

Mtx* Skybox_UpdateMatrix(SkyboxContext* skyboxCtx, f32 x, f32 y, f32 z) {
    Matrix_Translate(x, y, z, MTXMODE_NEW);
    Matrix_Scale(1.0f, 1.0f, 1.0f, MTXMODE_APPLY);
    Matrix_RotateX(skyboxCtx->rot.x, MTXMODE_APPLY);
    Matrix_RotateY(skyboxCtx->rot.y, MTXMODE_APPLY);
    Matrix_RotateZ(skyboxCtx->rot.z, MTXMODE_APPLY);
    return MATRIX_TO_MTX(sSkyboxDrawMatrix, "../z_vr_box_draw.c", 42);
}

void Skybox_Draw(SkyboxContext* skyboxCtx, GraphicsContext* gfxCtx, s16 skyboxId, s16 blend, f32 x, f32 y, f32 z) {
    OPEN_DISPS(gfxCtx, "../z_vr_box_draw.c", 52);

#if TARGET_PSP
    /* Was this function even reached, and with what? Answers the question the
     * triangle attribution alone cannot: zero skybox triangles means either
     * "not called" or "called but emitted nothing", and those need different
     * fixes. */
    gPspSkyCall[0]++;
    gPspSkyCall[1] = (u32)skyboxId;
    gPspSkyCall[2] = (u32)skyboxCtx->drawType;
    gPspSkyCall[3] = (u32)(uintptr_t)skyboxCtx->dListBuf;
    gPspSkyCall[4] = (u32)(uintptr_t)skyboxCtx->roomVtx;
    /* The skybox renders but is TILTED, and Link is upright -- so the camera is
     * innocent and the fault is the skybox's own transform or its vertices.
     * rot is 0 for both SKYBOX_MARKET_CHILD_DAY and SKYBOX_HOUSE_LINK, so the
     * matrix should be a PURE TRANSLATION to the eye, which cannot tilt
     * anything. These two groups separate the remaining possibilities:
     * eye position wrong -> matrix; vertex positions off the expected grid ->
     * Skybox_CalculateFace256's output. Expected first face corner is
     * (xStart, yStart, zStart) = (-126, 124, -126), steps 63 / -31. */
    gPspSkyCall[5] = (u32)(s32)x;
    gPspSkyCall[6] = (u32)(s32)y;
    gPspSkyCall[7] = (u32)(s32)z;
    gPspSkyCall[8] = (u32)(uintptr_t)skyboxCtx->staticSegments[0];
    gPspSkyCall[9] = (u32)(uintptr_t)skyboxCtx->palettes;
    if (skyboxCtx->roomVtx != NULL) {
        /* All 32 vertices of face 0, not just the first two. z_vr_box.c is
         * byte-identical to the decomp (tables included), so the grid SHOULD be
         * right -- this is what turns "should" into "is", and separates a
         * runtime data problem from a renderer one. Expected: a 5x9 grid at
         * z = -126, x stepping by 63 from -126, y stepping by -31 from 124. */
        {
            s32 vi;

            for (vi = 0; vi < 32; vi++) {
                gPspSkyVtxDump[vi * 3 + 0] = (u32)(s32)skyboxCtx->roomVtx[vi].v.ob[0];
                gPspSkyVtxDump[vi * 3 + 1] = (u32)(s32)skyboxCtx->roomVtx[vi].v.ob[1];
                gPspSkyVtxDump[vi * 3 + 2] = (u32)(s32)skyboxCtx->roomVtx[vi].v.ob[2];
            }
        }
        gPspSkyVtx[0] = (u32)(s32)skyboxCtx->roomVtx[0].v.ob[0];
        gPspSkyVtx[1] = (u32)(s32)skyboxCtx->roomVtx[0].v.ob[1];
        gPspSkyVtx[2] = (u32)(s32)skyboxCtx->roomVtx[0].v.ob[2];
        gPspSkyVtx[3] = (u32)(s32)skyboxCtx->roomVtx[1].v.ob[0];
        gPspSkyVtx[4] = (u32)(s32)skyboxCtx->roomVtx[1].v.ob[1];
        gPspSkyVtx[5] = (u32)(s32)skyboxCtx->roomVtx[1].v.ob[2];
        gPspSkyVtx[6] = (u32)(s32)skyboxCtx->roomVtx[0].v.tc[0];
        gPspSkyVtx[7] = (u32)(s32)skyboxCtx->roomVtx[0].v.tc[1];
    }

    /* Bracket the skybox so its triangles can be told apart from the room's and
     * Link's in the per-frame counters -- see G_PSP_MARK in psp_bg_rect.h. */
    /* NOT gSPNoOp + [-1]: gDma0p writes at the pointer WITHOUT advancing it
     * (include/ultra64/gbi.h:2046), so that pattern overwrites the PREVIOUS
     * command and corrupts the list -- which is exactly what it did. */
    {
        Gfx* mark = POLY_OPA_DISP++;

        mark->words.w0 = _SHIFTL(G_PSP_MARK, 24, 8);
        mark->words.w1 = PSP_MARK_SKYBOX_BEGIN;
    }
#endif

    Gfx_SetupDL_40Opa(gfxCtx);

    gSPSegment(POLY_OPA_DISP++, 0x7, skyboxCtx->staticSegments[0]);
    gSPSegment(POLY_OPA_DISP++, 0x8, skyboxCtx->staticSegments[1]);
    gSPSegment(POLY_OPA_DISP++, 0x9, skyboxCtx->palettes);

    gDPSetPrimColor(POLY_OPA_DISP++, 0x00, 0x00, 0, 0, 0, blend);
    gSPTexture(POLY_OPA_DISP++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
#if TARGET_PSP
    /* Wenn nicht ueberblendet wird, den Himmel gar nicht erst als
     * Zwei-Textur-Material zeichnen.
     *
     * SETUPDL_40 ist (TEXEL1 - TEXEL0) * PRIM_ALPHA + TEXEL0 in zwei Zyklen.
     * Bei blend == 0 ist das Ergebnis exakt TEXEL0 -- die zweite Textur traegt
     * NICHTS bei, kostet aber alles: einen zweiten Kachel-Ladevorgang je Quad
     * (32 pro Flaeche), den Kampf um die eine Textureinheit der GE, und den
     * LERP-Zweitdurchgang samt Flush pro Dreieck. Jeder dieser Schritte kann
     * einzeln schiefgehen, und wenn er schiefgeht, sieht man eine Flaeche des
     * Himmels in der falschen Textur oder als Muell.
     *
     * gTimeBasedSkyboxConfigs setzt skyboxBlend fuer jeden Eintrag mit
     * changeSkybox == false auf 0, also fuer alle stabilen Tagesabschnitte;
     * nur waehrend der Uebergaenge ist er ungleich 0. Damit faellt der
     * schwierige Pfad die meiste Zeit komplett weg statt repariert zu werden.
     *
     * Uebernommen aus reference/oot-psp-z2442, z_vr_box_draw.c (Commit
     * 3f7c9cf3c "Improve skybox!") -- dort exakt dieselben zwei Zeilen. */
    /* Auf einem Schalter, solange der Bisect ueber 34ab82e0b laeuft -- diese
     * Zeilen sind Aenderung 3 der vier, die den Renderpfad des Himmels in
     * jenem Commit angefasst haben. Siehe gPspSkyDecalNoBlend in gfx_scegu.c. */
    if (blend == 0 && gPspSkyDecalNoBlend) {
        gDPSetCombineMode(POLY_OPA_DISP++, G_CC_DECALRGBA, G_CC_DECALRGBA);
        gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    }
#endif

    // Prepare matrix
    sSkyboxDrawMatrix = GRAPH_ALLOC(gfxCtx, sizeof(Mtx));
    Matrix_Translate(x, y, z, MTXMODE_NEW);
    Matrix_Scale(1.0f, 1.0f, 1.0f, MTXMODE_APPLY);
    Matrix_RotateX(skyboxCtx->rot.x, MTXMODE_APPLY);
    Matrix_RotateY(skyboxCtx->rot.y, MTXMODE_APPLY);
    Matrix_RotateZ(skyboxCtx->rot.z, MTXMODE_APPLY);
    MATRIX_TO_MTX(sSkyboxDrawMatrix, "../z_vr_box_draw.c", 76);
    gSPMatrix(POLY_OPA_DISP++, sSkyboxDrawMatrix, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    // Enable magic square RGB dithering and bilinear filtering
    gDPSetColorDither(POLY_OPA_DISP++, G_CD_MAGICSQ);
    gDPSetTextureFilter(POLY_OPA_DISP++, G_TF_BILERP);

    // All skyboxes use CI8 textures with an RGBA16 palette
    gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[0]);
    gDPSetTextureLUT(POLY_OPA_DISP++, G_TT_RGBA16);

    // Enable texture filtering RDP pipeline stages for bilinear filtering
    gDPSetTextureConvert(POLY_OPA_DISP++, G_TC_FILT);

    if (skyboxCtx->drawType != SKYBOX_DRAW_128) {
        // 256x256 textures, per-face palettes
        // 2, 3 or 4 faces

        SKY_DL(0); // -z face upper
        SKY_DL(1); // -z face lower

        gDPPipeSync(POLY_OPA_DISP++);
        gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[1]);
        SKY_DL(2); // +x face upper
        SKY_DL(3); // +x face lower

        if (skyboxId != SKYBOX_BAZAAR) {
            if (skyboxId < SKYBOX_KOKIRI_SHOP || skyboxId > SKYBOX_BOMBCHU_SHOP) {
                // Skip remaining faces for most shop skyboxes

                gDPPipeSync(POLY_OPA_DISP++);
                gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[2]);
                SKY_DL(4); // +z face upper
                SKY_DL(5); // +z face lower

                // Note this pipesync is slightly misplaced and would be better off inside the condition
                gDPPipeSync(POLY_OPA_DISP++);

                if (skyboxCtx->drawType != SKYBOX_DRAW_256_3FACE) {
                    gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[3]);
                    SKY_DL(6); // -x face upper
                    SKY_DL(7); // -x face lower
                }
            }
        }
    } else {
        // 128x128 and 128x64 textures
        // 5 or 6 faces

        // Draw each face
        SKY_DL(0); // -z face
        SKY_DL(2); // +z face
        SKY_DL(4); // -x face
        SKY_DL(6); // +x face
        SKY_DL(8); // +y face
        if (skyboxId == SKYBOX_CUTSCENE_MAP) {
            // Skip the bottom face in the cutscene map
            SKY_DL(10); // -y face
        }
    }

    gDPPipeSync(POLY_OPA_DISP++);

    #if TARGET_PSP
    {
        Gfx* mark = POLY_OPA_DISP++;

        mark->words.w0 = _SHIFTL(G_PSP_MARK, 24, 8);
        mark->words.w1 = PSP_MARK_SKYBOX_END;
    }
#endif

    CLOSE_DISPS(gfxCtx, "../z_vr_box_draw.c", 125);
}

void Skybox_Update(SkyboxContext* skyboxCtx) {
}
