#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

#ifdef __cplusplus
extern "C" {
#endif

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen);
struct GfxRenderingAPI *gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_end_frame(void);

/* Last completed frame's interpreter counters, for the debug HUD
 * (psp/src/psp_scene_menu.c). Accessors rather than exporting
 * gPspGfxStatsPrev itself: PspGfxFrameStats is debugger-facing and the WebSocket
 * read scripts address it by field offset, so it deliberately stays a private
 * type that nothing else compiles against. */
/* The counters the screenshot writer stamps beside a captured frame.
 *
 * A flat, plain-typed copy on purpose: PspGfxFrameStats itself stays private to
 * gfx_pc.c because the WebSocket read scripts address it by field offset, and
 * anything that compiles against it would freeze that layout. */
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

/* Fills `out` from the frame currently being built -- see the definition for
 * why that, and not the last completed one, is what a screenshot needs. */
void gfx_pc_stat_snapshot_current(struct GfxPcFrameSnapshot *out);

unsigned int gfx_pc_stat_tris_drawn(void);
unsigned int gfx_pc_stat_tex_imports(void);
unsigned int gfx_pc_stat_tex_hits(void);

#ifdef __cplusplus
}
#endif

#endif
