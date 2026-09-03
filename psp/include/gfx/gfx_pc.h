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
/* GfxPcFrameSnapshot and gfx_pc_stat_snapshot_current live in their own
 * header so non-graphics translation units can reach them without the N64
 * Gfx type. */
#include "gfx/gfx_pc_frame_snapshot.h"

/* Vergiss jede zwischengespeicherte Textur, die aus diesem Speicherbereich
 * dekodiert wurde. Zu rufen, NACHDEM das Spiel einen Puffer ueberschrieben hat,
 * aus dem schon einmal eine Textur geladen wurde -- der Cache ist ueber die
 * Quelladresse geschluesselt und merkt so eine Inhaltsaenderung nicht von
 * selbst. Die lange Begruendung samt Messung steht an der Definition in
 * psp/src/gfx/gfx_pc.c. */
void gfx_texture_cache_invalidate_range(const void *addr, unsigned int size);

unsigned int gfx_pc_stat_tris_drawn(void);
unsigned int gfx_pc_stat_tex_imports(void);
unsigned int gfx_pc_stat_tex_hits(void);

#ifdef __cplusplus
}
#endif

#endif
