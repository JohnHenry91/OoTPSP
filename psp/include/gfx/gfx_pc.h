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
unsigned int gfx_pc_stat_tris_drawn(void);
unsigned int gfx_pc_stat_tex_imports(void);
unsigned int gfx_pc_stat_tex_hits(void);

#ifdef __cplusplus
}
#endif

#endif
