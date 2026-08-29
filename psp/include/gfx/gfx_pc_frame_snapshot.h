#ifndef GFX_PC_FRAME_SNAPSHOT_H
#define GFX_PC_FRAME_SNAPSHOT_H

/* The counters the screenshot writer stamps beside a captured frame.
 *
 * A flat, plain-typed copy on purpose: PspGfxFrameStats itself stays private to
 * gfx_pc.c because the WebSocket read scripts address it by field offset, and
 * anything that compiles against it would freeze that layout.
 *
 * It lives in its own header so it can be reached WITHOUT gfx/gfx_pc.h, whose
 * other prototypes need the N64 Gfx type -- pulling the whole ultra64 headers
 * into a BMP writer to reach one struct is the wrong trade. It used to be
 * hand-copied into psp/src/psp_screenshot.c for that reason, which is this
 * port's most expensive failure shape: two declarations of the same layout,
 * one of them quietly out of date. Do not copy it again; include this. */
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
    /* Auftrag 09: draws that used a texture while gsSPTexture said G_OFF, and
     * draws that used a texture with a zero S scaling factor. See the probe
     * comment on PspGfxFrameStats in gfx_pc.c. */
    unsigned int tex_off_draws;
    unsigned int tex_sc0_draws;
    /* Distance fog (Auftrag 04): draws that were fogged this frame, and draws
     * where the N64->GE range conversion refused to produce a range. The second
     * being non-zero points at psp_fog_apply's maths, not at the GE. */
    unsigned int fog_draws;
    unsigned int fog_bad_range;
};

/* Fills `out` from the frame currently being built -- see the definition for
 * why that, and not the last completed one, is what a screenshot needs. */
void gfx_pc_stat_snapshot_current(struct GfxPcFrameSnapshot *out);

#endif
