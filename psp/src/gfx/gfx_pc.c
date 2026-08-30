#include <math.h>
#include "psp_screenshot.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "psp_static_assets.h"
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "ultra64.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include "pspmath.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "attributes.h"
#include "segmented_address.h"
#include "psp_bg_rect.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define INFO_MSG(x) printf("%s %s\n", __FILE__ ":" TOSTRING(__LINE__), x)
#define _UNUSED(x) (void)(x)

#define SUPPORT_CHECK(x) assert(x)

// align value to N-byte boundary
#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED (1024)
/* OoT binds up to SEVEN lights plus ambient (z_lights.c's Lights_FindSlot
 * refuses the 8th: `if (lights->numLights >= 7) return NULL`). This was 2,
 * inherited unchanged from sm64-port, where two is genuinely the maximum.
 *
 * Two was not merely a cap -- nothing clamped against it. G_MW_NUMLIGHT
 * assigns current_num_lights straight from the command word, so a scene
 * binding three lights set it to 4 while current_lights[] held 3 entries and
 * current_lights_coeffs[] held 2. The per-vertex lighting path then
 *   - READ current_lights[current_num_lights - 1] as the ambient colour, one
 *     past the end, and
 *   - WROTE current_lights_coeffs[i] for i up to current_num_lights - 2, past
 *     the end of that array and into current_lookat_coeffs behind it.
 * The "ambient" colour was therefore float bit patterns read as u8 RGB, which
 * is why any scene with a third light -- every fairy, torch or glowing actor
 * binds a point light through Lights_BindPoint -- washed the nearby figures
 * out. See auftraege/FEHLERLISTE2.md N36. */
#define MAX_LIGHTS 7
#define MAX_VERTICES 64

/* Pixel Formats */
#define GU_PSM_5650		(0) /* Display, Texture, Palette */
#define GU_PSM_5551		(1) /* Display, Texture, Palette */
#define GU_PSM_4444		(2) /* Display, Texture, Palette */
#define GU_PSM_8888		(3) /* Display, Texture, Palette */
#define GU_PSM_T4		(4) /* Texture */
#define GU_PSM_T8		(5) /* Texture */
#define GU_PSM_T16		(6) /* Texture */
#define GU_PSM_T32		(7) /* Texture */
extern void* getStaticVramTexBuffer(unsigned int width, unsigned int height, unsigned int psm);
extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
/* gfx_scegu.c -- toggle the GU_BLEND fixed-factor overlay used to draw the
 * terrain LERP's second pass (see gPspLerp2SecondPass in gfx_sp_tri1). */
extern void gfx_scegu_lerp2_blend_begin(uint8_t mix);
extern void gfx_scegu_lerp2_blend_end(void);
extern float identity_matrix[4][4];

struct RGBA {
    uint8_t r, g, b, a;
} __attribute__((packed, aligned(4)));

struct XYWidthHeight {
    uint16_t x, y, width, height;
} __attribute__((packed, aligned(4)));

struct LoadedVertex {
    float x, y, z, w;
    float _x, _y, _z, _w;
    float u, v;
    struct RGBA color;
    uint32_t clip_rej;
    /* Which modelview load was in force when this vertex was LOADED. The N64
     * transforms at G_VTX time, so a vertex belongs to the matrix that was
     * current then. This port stores object space and lets the GE transform at
     * DRAW time, so any gap between load and draw silently re-transforms the
     * vertex under a later matrix. Probe for exactly that gap. */
    uint32_t mtx_slot_at_load;
} __attribute__((packed, aligned(16)));

typedef struct VertexColor {
	unsigned short u, v;
	struct RGBA color;
	unsigned short x, y, z;
} VertexColor __attribute__((aligned(16)));

struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    /* TLUT the entry was decoded with. CI4/CI8 texels are palette indices, so
     * the very same texture data yields completely different RGBA depending on
     * which palette was loaded (G_LOADTLUT) at decode time. Keying only on
     * addr/fmt/siz made the first-decoded palette stick for every later draw of
     * that texture -- libultraship hit and documents this exact issue in
     * Interpreter::ImportTexture ("so the same texture drawn with different
     * palettes gets distinct cache entries"). Only compared for G_IM_FMT_CI, so
     * non-paletted formats don't get spurious misses when the TLUT changes. */
    const uint8_t *palette;

    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
    /* PART OF THE CACHE KEY. Every importer derives the uploaded image's
     * width and height from these two, so the same address at a different
     * tile size is a different texture -- see gfx_texture_cache_lookup. */
    uint32_t line_size_bytes;
    uint32_t size_bytes;
    /* PART OF THE CACHE KEY, and needed at draw time.
     *
     * The PSP GE has only GU_REPEAT and GU_CLAMP -- no mirror mode -- so
     * G_TX_MIRROR is emulated by uploading the image next to a mirrored copy
     * of itself and letting REPEAT walk over the pair. That changes both the
     * uploaded pixels (hence part of the key) and the UV scale (hence read
     * back in gfx_sp_tri1). */
    uint8_t mirror_s, mirror_t;
} __attribute__((packed, aligned(4)));
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t shader_input_mapping[2][4];
} __attribute__((packed, aligned(4)));

/* 64 was inherited from sm64-port and is far too small for OoT: SM64 reuses a
 * handful of combine modes, OoT's scenes and objects each bring their own. The
 * recycle path below is a safety net, not a working mode -- if it fires per
 * frame the pool thrashes and every draw pays a gfx_flush(). 256 entries cost
 * 4 KB here and 14 KB for the matching shader pool; the size counters have to
 * grow past uint8_t to match. Watch gPspCcPoolHighWater to see the real
 * requirement. */
#define COLOR_COMBINER_POOL_SIZE 512
static struct ColorCombiner color_combiner_pool[COLOR_COMBINER_POOL_SIZE];
static uint16_t color_combiner_pool_size;

/* N36: how many lights OoT actually binds, and how often that exceeded the
 * THREE slots this renderer used to have (MAX_LIGHTS 2 + ambient). Anything
 * above zero in `lightsOverOld` is a frame that read past the end of
 * current_lights[] for its ambient colour. Reset per frame with the rest of
 * the draw stats. */
uint32_t gPspLightsMax;
uint32_t gPspLightsOverOld;

/* How the two reference ports decide whether a draw's alpha matters.
 *
 * Ours was `(other_mode_l & (G_BL_A_MEM << 18)) == 0` -- a single bit, taken
 * unchanged from sm64-port, and it feeds tcc_for_alpha's opt_alpha guard:
 * false there means the shader is bound GU_TCC_RGB, i.e. alpha comes from the
 * vertex ALONE and the texture's alpha channel never reaches the blender.
 *
 * reference/oot-psp-z2442 (gfx_fast3d.c:3239-3250) asks three questions
 * instead, and reference/daedalus -- an N64 emulator on this same GE --
 * doesn't gate on blending at all (RenderSettings.cpp:155-166: the alpha row
 * using a texel is the whole test). The N64's alpha channel is used for the
 * alpha COMPARE as well as for blending, so "is the blender reading the
 * framebuffer" is the wrong question to hang it on: SETUPDL_65 (Navi's glow)
 * and SETUPDL_20 (the forest motes) both ask for G_AC_THRESHOLD.
 *
 * Switchable so the two can be held against each other in one build --
 * gPspUseAlphaLegacy = 1 restores the single-bit test. */
static inline bool gfx_blend_cycle_uses_framebuffer(uint32_t other_mode_l, uint32_t m2a_shift,
                                                    uint32_t m2b_shift) {
    uint32_t m2a = (other_mode_l >> m2a_shift) & 3;
    uint32_t m2b = (other_mode_l >> m2b_shift) & 3;

    return (m2a == G_BL_CLR_MEM) && ((m2b == G_BL_1MA) || (m2b == G_BL_1));
}

int gPspUseAlphaLegacy;

static inline bool gfx_use_alpha_for(uint32_t other_mode_l) {
    if (gPspUseAlphaLegacy) {
        return (other_mode_l & (G_BL_A_MEM << 18)) == 0;
    }

    const uint32_t alpha_compare = other_mode_l & (3U << G_MDSFT_ALPHACOMPARE);
    const bool alpha_blend = (other_mode_l & FORCE_BL) &&
                             (gfx_blend_cycle_uses_framebuffer(other_mode_l, 22, 18) ||
                              gfx_blend_cycle_uses_framebuffer(other_mode_l, 20, 16));
    const bool texture_edge = (other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;

    return alpha_blend || texture_edge || (alpha_compare != G_AC_NONE);
}

static struct RSP {
    float modelview_matrix_stack[11][4][4]__attribute__((aligned(16)));

    float MP_matrix[4][4] __attribute__((aligned(16)));
    float P_matrix[4][4] __attribute__((aligned(16)));
    uint8_t modelview_matrix_stack_size;
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    /* gsSPTexture's `on` field (G_ON/G_OFF). Recorded rather than discarded so
     * the two counters below can measure what honouring it would change; the
     * draw path does NOT act on it yet -- see tex_off_draws. */
    bool texture_on;
    
    struct VertexColor loaded_vertices_2D[4];
    struct LoadedVertex loaded_vertices[MAX_VERTICES];
} rsp  __attribute__((aligned(16)));

#define LOADED_TEX(tile) (rdp.loaded_texture[rdp.texture_tile[(tile)].tmem_slot])

static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
        /* Row width of the SOURCE image in texels, straight from G_SETTIMG.
         * Only G_LOADTILE needs it -- G_LOADBLOCK always copies a contiguous
         * run, so for that path source stride and tile row length are the same
         * thing. Was discarded (_UNUSED) until the skybox needed it. */
        uint32_t width;
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
    } loaded_texture[2];
    /* Per-tile, NOT a single global -- G_SETTILE/G_SETTILESIZE can target
     * either tile 0 (TEXEL0/render tile) or tile 1 (TEXEL1, e.g. OoT's
     * console-logo text uses a second, simultaneously-loaded tile for its
     * shine overlay via gDPLoadMultiBlock). A single shared instance meant
     * tile 1 loads silently reused whatever line_size_bytes tile 0 had last
     * set, scrambling the texture's width/height (see import_texture_i8 et
     * al., which already take a `tile` param and now correctly index this
     * per-tile instead of reading one shared struct). */
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
        /* Distance between successive SOURCE rows, in bytes. Differs from
         * line_size_bytes only when a G_LOADTILE pulls a sub-rectangle out of a
         * wider image; 0 means "same as line_size_bytes", i.e. contiguous. */
        uint32_t src_stride_bytes;
        /* Which loaded_texture[] slot this tile's texels actually live in,
         * i.e. G_SETTILE's tmem address / 256. NOT the same thing as the tile
         * number: OoT's scrolling-lava/water materials (DMC, Fire Temple, ...)
         * point BOTH tile 0 and tile 1 at tmem 0x0000 -- one G_LOADBLOCK, two
         * tiles reading it with independently scrolled uls/ult, blended by the
         * combine. Indexing loaded_texture[] by the tile NUMBER instead made
         * TEXEL1 read slot 1, which nothing had ever loaded, so the lava came
         * out textured with whatever stale pointer/size was left there. */
        uint8_t tmem_slot;
        /* G_SETTILE's shift_s / shift_t: how many bits the incoming S/T
         * coordinate is shifted BEFORE the tile origin is subtracted.
         * 0 = none, 1..10 = right shift (texture stretched), 11..15 = left
         * shift by 16 - value (texture repeated more often). */
        uint8_t shifts, shiftt;
    } texture_tile[2];
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    /* The colour register cycle 2 multiplies the cycle-1 result by, or CC_0 if
     * cycle 2 is not that shape. Deliberately NOT part of combine_mode/cc_id:
     * folding it in there would change every shader_id in the port. See
     * gfx_dp_set_combine_cycle2_tint. */
    uint8_t combine_cyc2_tint;
    /* Cycle 1's RGB 'c' operand, kept RAW rather than as a CC_* code.
     *
     * color_comb_component() folds G_CCMUX_ENV_ALPHA onto CC_ENV and
     * G_CCMUX_PRIMITIVE_ALPHA onto CC_PRIM, because CC_* has no way to say
     * "the alpha channel of this register as a scalar". That approximation is
     * fine for picking a shader, but not for the two-texture terrain LERP,
     * whose mix factor IS that scalar: reading the parent register's RGB
     * instead silently substitutes OoT's environment TINT, which moves with
     * the scene lighting, for a blend fraction that does not. */
    uint8_t combine_c0_raw;
    /* Low byte of G_SETPRIMCOLOR: the other legal mix source for that LERP. */
    uint8_t prim_lod_frac;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp  __attribute__((aligned(4)));

static struct RenderingState {
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    /* Texenv mode last forced for two-texture combines: 1 == MODULATE,
     * 0 == REPLACE, -1 == unknown / not forced yet. */
    int two_texture_tint;
    /* Last PRIM handed to the GE as the tex-env colour for the PRIM/ENV LERP.
     * Starts at an impossible value so the first draw always issues it. */
    uint32_t lerp_prim_color;
    /* Set by upload_texture_mirrored for the import in progress, then copied
     * onto the cache node so a later cache HIT still knows the UV scale. */
    uint8_t mirror_s, mirror_t;
    /* Distance fog, see psp_fog_apply. -1 == not decided yet. The parameters
     * are cached beside the flag because a CHANGE of range or colour matters
     * exactly as much as a change of the flag: triangles already sitting in
     * buf_vbo would otherwise be drawn under the new fog. */
    int fog_enabled;
    float fog_start, fog_end;
    unsigned int fog_color;
} rendering_state __attribute__((aligned(16)));

/* gfx_scegu.c -- per-draw texenv override; see the call sites in gfx_sp_tri1. */
void gfx_scegu_set_two_texture_tint(int has_tint);
/* The (PRIM - ENV) * TEXEL0 + ENV LERP -- see is_prim_env_lerp_combine in
 * gfx_scegu.c. PRIM rides in the tex-env colour and changes per draw. */
int gfx_scegu_shader_is_prim_env_lerp(void);
void gfx_scegu_set_lerp_prim_color(uint32_t packed);
/* Defined further down this file, at G_SETPRIMCOLOR. */
extern uint32_t gRdpPrimColorPacked;
/* gfx_scegu.c -- the GE's fog unit; see psp_fog_apply below. */
void gfx_scegu_set_fog(int enable, float start, float end, unsigned int color);
/* gfx_scegu.c -- forget which texture each tile has bound. */
void gfx_scegu_invalidate_texture_binding(void);
/* Last texture id gfx_scegu_select_texture actually handed to the GE. */
extern uint32_t gPspCurBoundTex;

/* Exposed to gfx_scegu.c's N64-logo-cube 2-pass hack: it needs to toggle
 * GU_BLEND directly for one extra pass, and must restore it to whatever
 * this cache (not the generic dispatch) currently believes, or a direct
 * sceGuEnable/Disable(GU_BLEND) desyncs this cache from real hardware state
 * -- confirmed by a real regression: an unrelated later draw (the "NINTENDO
 * 64" text quads) came out solid white because their own alpha-blend need
 * was skipped by set_use_alpha() thinking (from this now-stale cache) that
 * GU_BLEND was already in the right state when it wasn't. */
bool gfx_get_alpha_blend_state(void) {
    return rendering_state.alpha_blend;
}

struct GfxDimensions gfx_current_dimensions __attribute__((aligned(4)));

static bool dropped_frame;

#if defined(TARGET_PSP)
typedef struct psp_fast_t {
  float u,v;
  struct RGBA color;
  float x,y,z;
} psp_fast_t;
static psp_fast_t buf_vbo[MAX_BUFFERED  * 3] __attribute__ ((aligned (32))); // 3 vertices in a triangle and 26 floats per vtx

/* Two-texture terrain LERP, (TEXEL1 - TEXEL0) * ENV + TEXEL0. This single-TMU
 * pipeline can only bind one texture at a time, and the rule elsewhere keeps
 * TEXEL0, i.e. always the blurry half; that is the measured cause of "the
 * ground is soft while the walls are crisp" (walls are single-texture
 * materials).
 *
 * Rather than reconstruct a second vertex stream and draw call by hand (that
 * attempt ran but painted the wrong thing -- see gPspLerp2SecondPass usages
 * below for why), the qualifying triangle is sent a SECOND time through this
 * same function, with the TEXEL0/TEXEL1 preference flipped so the normal draw
 * path binds TEXEL1 and computes tile 1's UVs -- exactly what the proven
 * "Prefer TEXEL1" diagnostic already does, just scoped to one triangle instead
 * of the whole frame. GU_BLEND with fixed ENV/(1-ENV) factors turns that
 * second, ordinary draw into the LERP against the first pass already in the
 * framebuffer. */
#else
static float buf_vbo[MAX_BUFFERED * (26 * 3)] // 3 vertices in a triangle and 26 floats per vtx
#endif
static size_t buf_vbo_len;
static size_t buf_num_vert;
static size_t buf_vbo_num_tris;

#if TARGET_PSP
/* ---------------------------------------------------------------------------
 * Per-frame rendering statistics.
 *
 * Deliberately plain globals and NOTHING else: this port's own sceIo debug
 * logging turned out to be a crash cause once already, and any file I/O
 * perturbs exactly the frame-timing-sensitive bugs we are trying to measure.
 * These are meant to be read out of the running game's memory with PPSSPP's
 * WebSocket debugger instead (link-time address from `psp-nm ootpsp.elf`,
 * plus the runtime module base from `hle.module.list`).
 *
 * Three generations are kept because the open bug is an *every other frame*
 * flicker: prev/prev2 hold the two last completed frames, so a single memory
 * read shows the odd and the even frame side by side. `cur` is live and will
 * be mid-update when read.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t magic;         /* 'PGFX', so the block is findable in a raw dump */
    uint32_t frame;         /* monotonic frame index                          */
    uint32_t dl_cmds;       /* display list commands interpreted              */
    uint32_t verts_loaded;  /* vertices through gfx_sp_vertex                 */
    uint32_t tri_calls;     /* gfx_sp_tri1 entered                            */
    uint32_t tri_rej_clip;  /* rejected: fully outside the view volume        */
    uint32_t tri_rej_cull;  /* rejected: backface/frontface culling           */
    uint32_t tris_buffered; /* triangles written into buf_vbo                 */
    uint32_t tris_drawn;    /* triangles actually handed to the GE            */
    uint32_t flushes;       /* gfx_flush() calls that really drew something   */
    uint32_t tex_imports;   /* import_texture() -> real decode+upload         */
    uint32_t tex_hits;      /* import_texture() -> served from the cache      */
    uint32_t dropped;       /* 1 if gfx_wapi->start_frame() refused the frame */
    /* --- appended (keep new fields at the END: the debugger read scripts
     * address this struct by field offset, so inserting anywhere else
     * silently reinterprets every existing counter) ---
     *
     * The room's display lists contain 44 texture loads (29 gsDPLoadTextureBlock
     * + 15 gsDPLoadTextureBlock_4b, counted in the .inc.c sources), yet the
     * frame stats read tex_hits == 2 and tex_imports == 0. These narrow down
     * where the other 42 go: does the interpreter even see the loads
     * (settimg/loadblock/settile), and does the combiner then ask for a
     * texture (tex_used/tex_unused)? */
    uint32_t settimg;       /* G_SETTIMG reached gfx_dp_set_texture_image      */
    uint32_t loadblock;     /* G_LOADBLOCK reached gfx_dp_load_block           */
    uint32_t loadtile;      /* G_LOADTILE reached gfx_dp_load_tile             */
    uint32_t settile;       /* G_SETTILE reached gfx_dp_set_tile               */
    uint32_t tex_used;      /* gfx_sp_tri1: combiner wanted a texture          */
    uint32_t tex_unused;    /* gfx_sp_tri1: combiner wanted none               */

    /* --- shade probe (session 12) -------------------------------------------
     * hakaana2's EnvLightSettings are strongly BLUE (ambient 40,60,90;
     * light1 110,110,250) and 9 of its 18 textures are intensity-only, so they
     * carry no colour of their own and MUST take it from shade. The render is
     * pure greyscale, so the blue is lost somewhere between the light data and
     * the vertex buffer. These fields cut that path at its two joints:
     *
     *   lit_*      what gfx_sp_vertex COMPUTED from rsp.current_lights
     *   vtx_*      what gfx_sp_tri1 actually WROTE into buf_vbo
     *
     * Blue in lit_ but not in vtx_ => the CC input mapping is dropping CC_SHADE
     * (look at shader_input_mapping / the "last input wins" loop).
     * Grey already in lit_ => the lights never reach the interpreter (look at
     * G_MOVEMEM/G_MV_LIGHT and z_lights.c), and the combiner is innocent.
     *
     * Sampled from lit triangles only (G_LIGHTING set, textured), because the
     * question is about the walls and floor. Colours are packed 0x00RRGGBB. */
    uint32_t num_lights;    /* rsp.current_num_lights, incl. ambient           */
    uint32_t amb_color;     /* current_lights[num-1].col -- the ambient        */
    uint32_t light0_color;  /* current_lights[0].col -- the first directional  */
    uint32_t lit_color;     /* d->color computed by the lighting maths         */
    uint32_t lit_samples;   /* how many lit vertices contributed               */
    uint32_t vtx_color;     /* the colour gfx_sp_tri1 chose for the vertex     */
    uint32_t vtx_cc_input;  /* which CC_* won, +1 (0 = loop never matched)     */
    uint32_t vtx_num_inputs;/* comb->num_inputs for that draw                  */

    /* --- matrix/batch coherency probe (session 13) ---------------------------
     * On PSP the vertices in buf_vbo are OBJECT space (gfx_sp_tri1 writes
     * clipped_vertices[i]->x, which gfx_sp_vertex filled from v->ob[]); the
     * transform is applied by the GE from the GU_MODEL/GU_PROJECTION matrices
     * that were last uploaded via sceGuSetMatrix. Those uploads go into the GE
     * display list IMMEDIATELY when the G_MTX command is interpreted, but the
     * triangles sit in buf_vbo until gfx_flush(). So any triangle still pending
     * when a new matrix is uploaded gets drawn by the GE under the NEW matrix.
     *
     * mtx_dirty_* count exactly that situation: how often a matrix upload
     * happened with triangles pending, and how many triangles were affected.
     * If these are ~0, this whole hypothesis is dead. */
    uint32_t mtx_dirty_events;  /* matrix uploads with buf_vbo non-empty      */
    uint32_t mtx_dirty_tris;    /* triangles pending across those uploads     */
    uint32_t mtxpop_dirty_events; /* same, for G_POPMTX                       */

    /* --- depth-state probe (session 13) --------------------------------------
     * The defect is VIEW-DEPENDENT (front view looked fine, side view broken),
     * which a wrong matrix cannot be -- a wrong matrix is wrong from every
     * angle. Wrong depth ordering is exactly view-dependent: it only shows
     * where geometry occludes itself.
     *
     * We enable the depth test from `rsp.geometry_mode & G_ZBUFFER` alone.
     * libultraship additionally requires the Z_CMP bit of other_mode_l
     * (interpreter.cpp:1787) -- a bit this port never reads, and which only
     * started arriving at all with 93dda6f (G_RDPSETOTHERMODE). These count how
     * often that actually matters, so the hypothesis is measurable even if the
     * picture does not visibly change. */
    uint32_t depth_zbuf_set;   /* draws with G_ZBUFFER set                     */
    uint32_t depth_zcmp_clear; /* ...of those, Z_CMP CLEAR = the disagreement  */
    uint32_t depth_zupd_off;   /* draws with Z_UPD clear (no depth WRITE)      */

    /* --- vertex/matrix timing probe (session 13, from the user's "his chest is
     * pasted onto his chin" observation) -- triangles at least one of whose
     * vertices was LOADED under a different modelview than the one in force
     * when the triangle is DRAWN. On N64 that cannot happen (G_VTX transforms
     * immediately); here it silently re-transforms the vertex.
     * vtx_stale_slot packs (load_slot << 16) | draw_slot for the first 8. */
    uint32_t tri_stale_mtx;
    uint32_t vtx_stale_examples;
    uint32_t vtx_stale_slot[8];

    /* --- endian-classification probe (session: PREREND-PIVOT first frame) ----
     * Every texture importer asks tex_needs_u64_unswap() whether its source
     * bytes are already native, and reverses each group of eight if they are
     * not. That predicate is an ADDRESS-RANGE test (PspStaticAssetIsStatic ->
     * PspBlob_IsNative), and psp_blob_assets.c already documents it answering
     * wrongly for exactly one asset: the skybox, the last thing in the game
     * still read raw from the .z64 into the shared arena, where an abandoned
     * blob range can still cover the buffer. The recorded symptom of that
     * misclassification is "the speckled skybox, with the room's own textures
     * still perfectly sharp beside it" -- which is the open bug, in a room type
     * where the skybox IS the visible surroundings.
     *
     * So: split every import by its verdict, and again by whether it happened
     * inside the skybox's BEGIN/END markers. On the corrupted frame,
     * sky_tex_unswap > 0 convicts the predicate; sky_tex_imports > 0 with
     * sky_tex_unswap == 0 clears it and points at the DATA instead; and
     * sky_tex_imports == 0 takes the skybox out of the case entirely. */
    uint32_t tex_unswap_yes;   /* imports that reversed byte octets           */
    uint32_t tex_unswap_no;    /* imports that took the source as-is          */
    uint32_t sky_tex_imports;  /* imports between the skybox markers          */
    uint32_t sky_tex_unswap;   /* ...of which reversed                        */
    uint32_t sky_tex_hits;     /* skybox tiles served from the cache instead  */

    /* --- gsSPTexture(..., G_OFF) probe (Auftrag 09) ---------------------------
     * gfx_sp_texture threw the `on` flag away, and the same command that
     * carries G_OFF also sets the scaling factors to 0. Two separate questions
     * follow, and neither had ever been counted:
     *
     *   tex_off_draws  draws whose COMBINER asked for a texture while the RSP's
     *                  texture unit was switched off. On the N64 those draw
     *                  untextured. Zero means honouring the flag would change
     *                  nothing and the port's behaviour is accidentally right;
     *                  non-zero names the draws that are wrong today.
     *   tex_sc0_draws  draws that used a texture while texture_scaling_factor.s
     *                  was 0, i.e. every texture coordinate collapses to 0 and
     *                  the surface samples a single texel -- a flat coloured
     *                  quad. That is the shape of the reported "yellow box",
     *                  so it is worth knowing whether it ever happens.
     *
     * Measurement only. Do not add behaviour here without a number first. */
    uint32_t tex_off_draws;
    uint32_t tex_sc0_draws;
} PspGfxFrameStats;

PspGfxFrameStats gPspGfxStats;      /* live, currently being built */
PspGfxFrameStats gPspGfxStatsPrev;  /* last completed frame        */
PspGfxFrameStats gPspGfxStatsPrev2; /* the one before that         */

/* First projection and modelview matrix of the frame, three generations like
 * everything else here so the two alternating frame types can be compared in
 * one read. */
#define PSP_MTX_PROJ_SLOTS 4
/* A frame does ~45 modelview loads; 64 leaves headroom without making the
 * block too large to read in a single debugger request. */
#define PSP_MTX_MV_SLOTS 64

typedef struct {
    uint32_t magic;      /* 'PMTX' */
    uint32_t frame;
    uint32_t proj_loads; /* G_MTX loads with G_MTX_PROJECTION this frame */
    uint32_t mv_loads;
    /* Every projection load, not just the first: a frame does 4 of them, and
     * the room's actual view matrix is one of the later ones. */
    float proj[PSP_MTX_PROJ_SLOTS][16];
    float mv_first[16];
    /* The combined modelview-projection actually in force when the frame's
     * first triangle was rejected as outside the view volume. The two
     * bistable frame types submit identical geometry but reject wildly
     * different amounts of it, so this is the value that has to differ. */
    float mp_at_reject[16];
    uint32_t reject_captured;
    /* Per-modelview-load breakdown.
     *
     * Measured: identical geometry (dl_cmds, verts_loaded, tri_calls all
     * constant), identical projection (all four loads), identical mv_first,
     * and an identical MVP at the first rejected triangle -- yet tri_rej_clip
     * swings between ~10 and ~1464 from frame to frame. Since a frame does 45
     * modelview loads and only the first was ever captured, the divergence
     * has to be in one of the other 44, i.e. in a per-object transform rather
     * than the camera.
     *
     * So record, per load index: a hash of the matrix that load produced, and
     * how many triangles were submitted and clip-rejected while it was the
     * one in force. Diffing two consecutive frames by index names the object
     * whose transform is unstable. Hashes rather than the matrices themselves
     * so all 64 slots fit in a block small enough to read in one go. */
    uint32_t mv_hash[PSP_MTX_MV_SLOTS];
    uint32_t mv_tris[PSP_MTX_MV_SLOTS];
    uint32_t mv_rej[PSP_MTX_MV_SLOTS];

    /* --- appended (session 13): where each modelview load actually puts its
     * object. Link's skeleton renders correctly but his ATTACHED items (sword,
     * sheath, shield, hands) are displaced by plausible rigid transforms, and
     * one lands on the floor several units away. A hash says "different", this
     * says "different HOW": if a misplaced item's translation equals Link's
     * root/actor position rather than the limb it hangs off, the item is being
     * drawn under the wrong matrix rather than under a wrong matrix.
     * Row 3 of the (row-vector convention) modelview, i.e. m[3][0..2]. */
    float mv_trans[PSP_MTX_MV_SLOTS][3];

    /* --- appended (session 13, user's hypothesis): the parts may not be
     * mis-POSITIONED but mis-ORIENTED. mv_trans only proves each limb sits in
     * the right place; it says nothing about the upper 3x3.
     *
     * det < 0 means the transform MIRRORS: the mesh turns inside out, back
     * faces point outward, and the result looks like holes and stray shards
     * while every part stays where it belongs -- and it would be strongly
     * view-dependent, which is what the screenshots show.
     * |det| also catches a wrong scale. Link is drawn at scale 0.01, so the
     * expected magnitude is 1e-6; the room's identity slots should read 1.0. */
    float mv_det[PSP_MTX_MV_SLOTS];
} PspGfxMtxTrace;

/* Determinant of the upper 3x3 of a row-vector 4x4. */
static float psp_mtx_det3(const float m[4][4]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

PspGfxMtxTrace gPspGfxMtx;
PspGfxMtxTrace gPspGfxMtxPrev;
PspGfxMtxTrace gPspGfxMtxPrev2;

/* Which modelview-load slot is currently in force, so triangle submissions
 * and clip rejections can be attributed to the transform that produced them.
 * Reset per frame alongside the trace block. */
static uint32_t sPspMtxCurSlot;

/* FNV-1a over the matrix's raw bit patterns. Exact-equality only -- the
 * question is "is this transform identical between the two frame types",
 * which is a bitwise question, so no float tolerance is wanted here. */
static uint32_t psp_mtx_hash(const float m[4][4]) {
    const unsigned char *p = (const unsigned char *)m;
    uint32_t h = 2166136261u;
    unsigned int i;

    for (i = 0; i < sizeof(float) * 16; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

#define PSP_GFX_MTX_MAGIC 0x504D5458u /* 'PMTX' */

#define PSP_GFX_STATS_MAGIC 0x50474658u /* 'PGFX' */
#define GFXSTAT_ADD(field, n) (gPspGfxStats.field += (uint32_t)(n))
#else
#define GFXSTAT_ADD(field, n) ((void)0)
#endif
#define GFXSTAT_INC(field) GFXSTAT_ADD(field, 1)

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#if defined(TARGET_PSP)
#include <pspthreadman.h>
static unsigned long get_time(void) {
    return sceKernelGetSystemTimeWide();
}
#else
#include <time.h>
static unsigned long get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif


//******************* Clipping things

// Bits for clipping
// +-+-+-
// xxyyzz
#define Z_NEG  (0x01)
#define Z_POS  (0x02)
#define Y_NEG  (0x04)
#define Y_POS  (0x08)
#define X_NEG  (0x10)
#define X_POS  (0x20)

// Test all but Z_NEG (for No Near Plane microcodes)
#define CLIP_TEST_FLAGS ( X_POS | X_NEG | Y_POS | Y_NEG | Z_POS | Z_NEG)
//#define CLIP_TEST_FLAGS ( Z_POS | Z_NEG ) /* Faster but worse */

/* --- Near clipping: OoT is a "NoN" (No Nearclipping) game ------------------
 *
 * src/code/sys_ucode.c loads gspF3DZEX2_NoN_fifo. "NoN" means the RSP does NOT
 * clip against the near plane: geometry between the eye and zNear (10.0f, see
 * z_view.c:63) is still drawn. SM64 uses a near-clipping microcode, so the
 * clipper this file inherited from sm64-port-psp cuts away exactly the
 * geometry OoT expects to keep -- worst when the camera turns and walls/actors
 * sweep past the eye.
 *
 * Every mature OoT port handles this explicitly:
 *   - libultraship/Shipwright gfx_pc.cpp: `// if (z < -w) d->clip_rej |= 16;`
 *     (CLIP_NEAR commented out), plus glEnable(GL_DEPTH_CLAMP) in gfx_opengl
 *     and DepthClipEnable = false in gfx_direct3d11.
 *   - DaedalusX64 (PSP) nudges the projection instead
 *     (BaseRenderer::SetProjection, `if (g_ROM.ZELDA_HACK) mProjectionMat[3][2]
 *     += 0.4f;` -- "needed to show heart in OOT & MM, it renders at Z = 0.0f
 *     that gets clipped away").
 *
 * We cannot simply delete the plane the way a GL/D3D port can: the PSP GE has
 * no depth clamp and its perspective divide needs w > 0. So we slide the plane
 * from zNear towards the eye instead. Plane {0,0,-t,-1} is the true near plane
 * at t = 1 and degenerates to the eye plane (w >= 0) at t = 0; a small positive
 * t keeps a hair of margin so w never actually reaches zero.
 * With the software clip guaranteeing w > 0, the GE's own Z = -W clipper (which
 * would still cut at the game's zNear) can be turned off -- see
 * gPspGuClipPlanes in gfx_scegu.c. */
float gPspNearClipT = 0.02f;   /* 1.0f restores the old (sm64) near clipping */

static inline float vec3_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2]);
}

static inline float vec4_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2])+ (lhs[3]*rhs[3]);
}

static inline void vec4_sub(float *out, const float* lhs, const float*rhs){
    out[0] = lhs[0]-rhs[0];
    out[1] = lhs[1]-rhs[1];
    out[2] = lhs[2]-rhs[2];
    out[3] = lhs[3]-rhs[3];
}

void gfx_clip_interpolate_vert(struct LoadedVertex* out, const struct  LoadedVertex* lhs, const struct LoadedVertex* rhs, const float factor )
{
    // projected pos
    out->x = lhs->x + (rhs->x - lhs->x) * factor;
    out->y = lhs->y + (rhs->y - lhs->y) * factor;
    out->z = lhs->z + (rhs->z - lhs->z) * factor;
    //out->w = lhs->w + (rhs->w - lhs->w) * factor;
    // transfomed pos
    out->_x = lhs->_x + (rhs->_x - lhs->_x) * factor;
    out->_y = lhs->_y + (rhs->_y - lhs->_y) * factor;
    out->_z = lhs->_z + (rhs->_z - lhs->_z) * factor;
    out->_w = lhs->_w + (rhs->_w - lhs->_w) * factor;
    // color
    out->color.r = lhs->color.r + (rhs->color.r - lhs->color.r) * factor;
    out->color.g = lhs->color.g + (rhs->color.g - lhs->color.g) * factor;
    out->color.b = lhs->color.b + (rhs->color.b - lhs->color.b) * factor;
    out->color.a = lhs->color.a + (rhs->color.a - lhs->color.a) * factor;
    // texture
    out->u = lhs->u + (rhs->u - lhs->u) * factor;
    out->v = lhs->v + (rhs->v - lhs->v) * factor;
}

//*****************************************************************************
//
//	The following clipping code was taken from The Irrlicht Engine.
//	See http://irrlicht.sourceforge.net/ for more information.
//	Copyright (C) 2002-2006 Nikolaus Gebhardt/Alten Thomas
//
//*****************************************************************************
/* NB: the near/far labels below are the ones inherited from Daedalus/Irrlicht
 * and they are swapped with respect to the actual maths -- plane[0] rejects
 * z > w (that is the FAR plane) and plane[5] rejects z < -w (the NEAR plane).
 * plane[5] is the one the NoN microcode does not have; see gPspNearClipT. */
static const float NDCPlane[6][4] =
{
	{  0.f,  0.f,  1.f, -1.f },	// near
	{  1.f,  0.f,  0.f, -1.f },	// left
	{ -1.f,  0.f,  0.f, -1.f },	// right
	{  0.f,  1.f,  0.f, -1.f },	// bottom
	{  0.f, -1.f,  0.f, -1.f },	// top
	{  0.f,  0.f, -1.f, -1.f }	// far
};

static uint32_t clipToHyperPlane( struct LoadedVertex *dest, const struct LoadedVertex *source, uint32_t inCount, const float plane[4] )
{
	uint32_t outCount;
	struct LoadedVertex *out;

	const struct LoadedVertex *a;
	const struct LoadedVertex *b;

	float aDotPlane;
	float bDotPlane;
    float temp_vec[4];

	out = dest;
	outCount = 0;
	b = source;
	bDotPlane = vec4_dot(&b->_x, plane);
    size_t i;

#define EPSILON 0.00000001
	for(i = 1; i < inCount + 1; ++i)
	{
		a = &source[i%inCount];
		aDotPlane = vec4_dot(&a->_x, plane);

		// current point inside
		if ( aDotPlane <= EPSILON )
		{
			// last point outside
			if ( bDotPlane > EPSILON )
			{
				// intersect line segment with plane
                // Next 2 lines are "(b->ProjectedPos - a->ProjectedPos).Dot( plane )"
                vec4_sub(temp_vec, &b->_x, &a->_x);
                const float dot_projected = vec4_dot(temp_vec, plane);
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );
				out += 1;
				outCount += 1;
			}
			// copy current to out
			*out = *a;
			b = out;

			out += 1;
			outCount += 1;
		}
		else
		{
			// current point outside

			if ( bDotPlane <= EPSILON )
			{
				// previous was inside
				// intersect line segment with plane
                // Next 2 lines are "(b->ProjectedPos - a->ProjectedPos).Dot( plane )"
                vec4_sub(temp_vec, &b->_x, &a->_x);
                const float dot_projected = vec4_dot(temp_vec, plane);
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );

				out += 1;
				outCount += 1;
			}
			b = a;
		}

        bDotPlane = vec4_dot(&b->_x, plane);
	}

	return outCount;
}

/* Session 16 bisection knob for the skybox tilt. Applies ONLY to triangles
 * between PSP_MARK_SKYBOX_BEGIN/END, so the rest of the frame keeps its normal
 * clipping and stays a reference to compare against.
 *
 *   0 = normal, all six planes (ship this)
 *   1 = near plane only; the GE's own clipper handles the sides. Every vertex
 *       still has w > 0, so the picture stays readable and the SHAPE is the
 *       measurement: upright here => the fault is in the five side planes,
 *       still tilted => it is upstream of clip_to_frustum (gfx_sp_vertex's
 *       VFPU transform or the GE submission).
 *   2 = no clipping at all. Vertices behind the eye reach the GE, so expect
 *       garbage at the edges; only the gross orientation means anything.
 *
 * See memory session 15: everything BEFORE the clipper is already eliminated
 * by measurement (skybox rot, eye, the vertex table, the modelview the
 * renderer receives, the late Skybox_UpdateMatrix overwrite, the clip-space
 * aspect mismatch). This splits what is left. */
int gDebugSkyClipMode = 0;

uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn, int near_only )
{
	uint32_t vOut;

	vOut = vIn;

	/* NEAR FIRST, and bail as soon as nothing is left. Both come from
	 * DaedalusX64's clip_tri_to_frustum (reference/daedalus/Source/HLEGraphics/
	 * BaseRenderer.cpp:684), which solves this on the same hardware.
	 *
	 * Order matters, and not for style. A vertex behind the eye has w < 0, and
	 * the side-plane tests are of the form x < -w / x > w -- multiplying
	 * through by a negative w REVERSES them, so those tests are meaningless
	 * until the near plane has removed such vertices. Clipping near first
	 * guarantees every vertex the side planes see has w > 0.
	 *
	 * Ordinary geometry never noticed because it sits in front of the camera.
	 * The skybox is the one thing that SURROUNDS it -- roughly half its
	 * vertices are behind the eye -- which is why it was the only surface
	 * coming out skewed.
	 *
	 * The early-outs matter for their own reason: clipToHyperPlane indexes
	 * source[i % inCount], so once a polygon has been cut below 3 vertices the
	 * remaining planes keep processing it and can grow it back with
	 * invented ones. The caller discards anything under 3, so returning early
	 * is safe regardless of which buffer the result landed in. */
	{
		/* The plane F3DZEX2.NoN does not have. Slid towards the eye by
		 * gPspNearClipT instead of removed, because the GE needs w > 0. */
		const float near_plane[4] = { 0.f, 0.f, -gPspNearClipT, -1.f };

		vOut = clipToHyperPlane( v1, v0, vOut, near_plane );		if (vOut < 3) return vOut; // near
	}
	/* The ping-pong is what makes near_only cheap: after an ODD number of
	 * passes the result sits in v1, after an EVEN one in v0, and the caller
	 * reads v0. One extra copy keeps that contract without duplicating the
	 * retesselation. */
	if (near_only) {
		uint32_t i;
		for (i = 0; i < vOut; i++) {
			v0[i] = v1[i];
		}
		return vOut;
	}
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[0] );		if (vOut < 3) return vOut; // far
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[2] );		if (vOut < 3) return vOut; // right
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[1] );		if (vOut < 3) return vOut; // left
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[4] );		if (vOut < 3) return vOut; // top
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[3] );		// bottom

	return vOut;
}

static struct LoadedVertex temp_a[12];
static struct LoadedVertex temp_b[12];

void gfx_clip_single_vert( struct LoadedVertex *p_p_vertices, size_t *p_num_vertices, struct LoadedVertex *v_arr[3], int near_only )
{
	//
	//	At this point all vertices are lit/projected and have both transformed and projected
	//	vertex positions. For the best results we clip against the projected vertex positions,
	//	but use the resulting intersections to interpolate the transformed positions. 
	//	The clipping is more efficient in normalised device coordinates, but rendering these
	//	directly prevents the PSP performing perspective correction. We could invert the projection
	//	matrix and use this to back-project the clip planes into world coordinates, but this
	//	suffers from various precision issues. Carrying around both sets of coordinates gives
	//	us the best of both worlds :)
	//
    size_t clipped_vertices_num = 0;

    temp_a[ 0 ] = *v_arr[ 0 ];
    temp_a[ 1 ] = *v_arr[ 1 ];
    temp_a[ 2 ] = *v_arr[ 2 ];

    uint32_t out = clip_to_frustum( temp_a, temp_b, 3, near_only );
    if( out < 3 ){
        *p_num_vertices = 0;
        return;
    }

    // Retesselate
    for( uint32_t j = 0; j <= out - 3; ++j )
    {            
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ 0 ] );
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ j + 1 ] );
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ j + 2 ] );
    }

	*p_num_vertices = clipped_vertices_num;
}

//******************* End Clipping things


static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        //int num = buf_vbo_num_tris;
        //unsigned long t0 = get_time();
        GFXSTAT_INC(flushes);
        GFXSTAT_ADD(tris_drawn, buf_vbo_num_tris);
        gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_num_vert = 0;
        buf_vbo_num_tris = 0;
        //unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = (cc_id >> 24) << 24;
    uint8_t shader_input_mapping[2][4] = {{0}};
    for (int i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (int i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (int j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
    }
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

/* Both fixed-size pools that a new combine mode consumes a slot in were filled
 * with an UNCHECKED post-increment (`pool[pool_size++]`), and neither is ever
 * reset -- they accumulate for the whole run, across every scene loaded.
 *
 * What the 65th entry lands on is not abstract. From the ELF (psp-nm -S):
 *
 *   color_combiner_pool  0x495a44 .. 0x495e44   (64 * 16 bytes)
 *   gfx_texture_cache    0x495e44 ..            (hashmap[1024] FIRST)
 *
 * i.e. the overflow writes straight into the texture cache's hash table, whose
 * entries are POINTERS that gfx_texture_cache_lookup then walks. The same holds
 * one layer down for shader_program_pool (0x79a304 + 0xe00 = staticOffset, the
 * VRAM bump allocator's cursor).
 *
 * The pressure is not evenly spread across scenes: cc_id carries SHADER_OPT_FOG
 * (see gDebugFogCombinerBit below), so a scene whose display lists all use a
 * fog render mode needs a SECOND, distinct combiner for every combine mode it
 * shares with the scenes already loaded. link_home is exactly that -- all 7 of
 * its render modes are G_RM_FOG_SHADE_A, against zero in hakaana2.
 *
 * Recycling the whole pool is safe where evicting one entry would not be:
 * combiners only ever point AT ShaderPrograms, so dropping both pools together
 * and letting the next draw re-derive what it needs cannot leave a dangling
 * reference -- provided anything still holding a pointer is cleared too, which
 * is what the three assignments below are for. */
/* 1 = fogged draws get their own combiner/shader IDs (correct). 0 = fold them
 * onto the un-fogged ID.
 *
 * DEFAULT 1, AND CLEARING IT IS NOT SAFE -- kept only as an A/B switch.
 *
 * The bit looks inert at first glance: gfx_cc_get_features copies it into
 * cc_features.opt_fog, whose only reader is a block in gfx_scegu_apply_shader
 * that is #if 0'd out. It was briefly defaulted to 0 on the theory that it
 * therefore could not change a pixel, only double the ID space and overflow the
 * (then 64-entry) pools.
 *
 * That reasoning was wrong, for a reason session 12 had already written down
 * when it deliberately kept the cycle-2 tint OUT of cc_id: **shader_id is not
 * just a cache key, it is dispatched on**. gfx_scegu.c switches on hardcoded
 * ids inherited from sm64-port -- 0x0000038D (mario's eyes), 0x01045A00,
 * 0x01200A00, 0x00000551, 0x01A00045, 0x01200200 -- each forcing a particular
 * texture-environment mode. SHADER_OPT_FOG is bit 25 (0x02000000), so while it
 * is set a fogged material CANNOT collide with any of those. Clearing it drops
 * fogged materials onto ids that can, and a wrongly matched case silently
 * replaces the texture function.
 *
 * That is not hypothetical here: every one of link_home's render modes is
 * G_RM_FOG_SHADE_A, so in that scene it applies to the whole frame -- Link's
 * tunic, his shield, the room's lit surfaces.
 *
 * The pool pressure that motivated clearing it is solved properly instead, by
 * sizing the pools for the real requirement (see COLOR_COMBINER_POOL_SIZE). */
int gDebugFogCombinerBit = 1;

/* Triangles attributed to the skybox: submitted / clip-rejected / cull-rejected.
 * The skybox is the Market's buildings and Link's House's interior walls, and
 * neither shows up -- these say whether its geometry even survives to a draw. */
uint32_t gPspSkyTri[4];
/* Defined in src/code/z_vr_box_draw.c: [0] call count, [1] skyboxId,
 * [2] drawType. Declared here rather than in a header because the skybox probe
 * is diagnostic scaffolding, not an interface. */
extern uint32_t gPspSkyCall[12];
float gPspSkyMtx[4][4];
uint32_t gPspSkyTriMark;

/* Biggest-textured-triangle probe -- see the note at the capture site in
 * gfx_sp_tri1. Answers "how is the ground's texture actually being mapped",
 * which no screenshot can: tile size, the tile rect it was cut from, the
 * shift/clamp modes, and the raw S10.5 vertex texture coordinates.
 *
 * Reset once per frame by gfx_start_frame so the numbers describe the frame on
 * screen rather than the largest triangle ever drawn. */
float gPspBigTriArea2;
/* Last frame's winning area, kept so the highlight can recognise the same
 * triangle on the way past: the winner is only known once the frame is over,
 * which is too late to colour it. Geometry is near-identical frame to frame, so
 * matching against the previous frame's maximum picks out the same surface. */
float gPspBigTriArea2Prev;

/* Why the terrain second pass is or is not happening. Two counters, because
 * "the ground still looks soft" has two quite different causes and guessing
 * between them is what has been expensive:
 *   detected == 0         -> the combine match never fires; the operand test
 *                            in gfx_sp_tri1 is wrong about this material
 *   detected > 0, draws == 0 (or draws << detected) -> it fires but the
 *                            actual overlay draw is being skipped/culled
 *   draws > 0              -> the pass runs and the fault is in what it draws
 *                            (blend factors, texture, coordinates)
 * Cumulative rather than per-frame so one debugger read answers the question. */
uint32_t gPspLerp2Detected;
uint32_t gPspLerp2Draws;
/* The mix factor most recently used, and which RDP operand it came from --
 * the pair that says whether the right source is being read. */
uint32_t gPspLerp2LastMix;
uint32_t gPspLerp2LastMixSrc;

int gPspGfxHackHighlightBigTri;
/* Defined in gfx_scegu.c; read here to pick which tile the single UV set
 * describes, and to force it for the terrain LERP's second pass. */
extern int gPspGfxHackPreferTexel1;
extern int gPspGfxLerp2Enable;
/* Set for the duration of the recursive gfx_sp_tri1 call that draws the
 * terrain LERP's second pass. Read by gfx_scegu_select_texture (same effect
 * as gPspGfxHackPreferTexel1, scoped to one triangle) and by the uv_tile
 * choice below, and doubles as the reentrancy guard that stops the second
 * pass from detecting itself as another LERP and recursing forever. */
int gPspLerp2SecondPass;
uint32_t gPspBigTriTexW, gPspBigTriTexH;
uint32_t gPspBigTriTex01, gPspBigTriCcId;

/* Enough to fetch the probed triangle's texture out of the running game and
 * look at it off-device. A screenshot can say "this looks too soft"; only the
 * bytes can say whether the texture CONTENT is what the ROM holds, which is
 * the question left once the coordinate mapping has been shown correct. */
const uint8_t *gPspBigTriTexAddr;
uint32_t gPspBigTriTexFmt, gPspBigTriTexSiz;
uint32_t gPspBigTriTexLine, gPspBigTriTexBytes;
const uint8_t *gPspBigTriPalAddr;
uint32_t gPspBigTriUls, gPspBigTriUlt, gPspBigTriLrs, gPspBigTriLrt;
uint32_t gPspBigTriShiftS, gPspBigTriShiftT;
uint32_t gPspBigTriCms, gPspBigTriCmt;

/* A probe reserved for triangles the terrain second pass ACTUALLY draws.
 *
 * The general biggest-triangle probe ranks by area alone, so it keeps landing
 * on whatever single-texture surface happens to be largest -- it reported
 * tex01 = 1 while the question was entirely about two-texture materials, which
 * made every number it produced beside the point. This one only ever considers
 * triangles that matched the LERP, so its numbers describe the thing being
 * debugged by construction.
 *
 * Records BOTH tiles, because the failure being chased is a disagreement
 * between them: tile 0 measured a plausible 32x32 while tile 1 came out
 * 16x256, i.e. 8192 bytes -- larger than a TMEM slot can hold, so
 * LOADED_TEX(1) was carrying leftovers from an earlier material rather than
 * this one's detail texture. */
float gPspL2Area2;
uint32_t gPspL2TexW0, gPspL2TexH0, gPspL2TexW1, gPspL2TexH1;
uint32_t gPspL2Line0, gPspL2Bytes0, gPspL2Line1, gPspL2Bytes1;
uint32_t gPspL2Cms0, gPspL2Cmt0, gPspL2Cms1, gPspL2Cmt1;
uint32_t gPspL2Shifts0, gPspL2Shiftt0, gPspL2Shifts1, gPspL2Shiftt1;
uint32_t gPspL2Slot0, gPspL2Slot1;
uint32_t gPspL2CcId;
const uint8_t *gPspL2Addr0;
const uint8_t *gPspL2Addr1;
/* Same address in both slots means the two tiles are reading one image and the
 * detail must come purely from differing tile parameters; different addresses
 * mean there really are two textures and the bind has to pick correctly. */
uint32_t gPspL2SameAddr;
/* Counts LERP triangles whose tile 1 record is impossible (over a TMEM slot,
 * or zero). Non-zero is the smoking gun for stale LOADED_TEX(1). */
uint32_t gPspL2BadTile1;

/* What each of the two passes ACTUALLY put in the vertex buffer, and which
 * texture was bound while it did. Indexed [0] = first pass, [1] = second.
 *
 * Everything upstream now measures clean -- both tiles hold sane 32x32 records
 * at distinct addresses, tile 1 carries shift 14 (a LEFT shift by 2, i.e. four
 * times the repeat rate) and tex_shift_coord handles that range correctly. So
 * the remaining question is no longer what the RDP asked for but whether the
 * second pass carries it through, and that can only be answered where the
 * numbers are written rather than by reading the code that ought to write
 * them. If pass 1 and pass 2 come out with the same UVs, uv_tile never took
 * effect; if the UVs differ but the bound texture id does not, the bind is
 * what is failing. */
float gPspL2UvU[2], gPspL2UvV[2];
uint32_t gPspL2UvW[2], gPspL2UvH[2];
uint32_t gPspL2UvTile[2], gPspL2UvShift[2];
uint32_t gPspL2DrawTex[2];

/* The same four numbers for TILE 1, which is the half the terrain second pass
 * actually draws. Tile 0's values alone cannot answer the question the second
 * pass raises: whether the detail tile is set up to REPEAT across the surface
 * (which is what makes it read as detail) or to CLAMP like the colour map
 * (which would stretch a single band and look like no second pass at all). */
uint32_t gPspBigTriCms1, gPspBigTriCmt1;
uint32_t gPspBigTriShiftS1, gPspBigTriShiftT1;
uint32_t gPspBigTriTexW1, gPspBigTriTexH1;
int32_t gPspBigTriU0, gPspBigTriV0, gPspBigTriU1, gPspBigTriV1, gPspBigTriU2, gPspBigTriV2;

/* Session 16: what the renderer ACTUALLY computes for the skybox's vertices.
 * Everything upstream measures clean -- modelview is a pure translation, the
 * projection (which is where OoT keeps the camera's guLookAt, z_view.c:405)
 * has no roll, and bypassing the clipper changes nothing -- yet the panorama
 * comes out rotated ~30 degrees. So stop reasoning about the transform and
 * read its output: 8 floats per vertex for the first 32 skybox vertices,
 *   [0..2] object space  [3..4] u,v  [5..6] NDC x,y  [7] w.
 * An upright grid in NDC means the geometry is fine and the fault is the
 * texture mapping; a rotated one means it is the transform after all. */
float gPspSkyVtxOut[256][8];
uint32_t gPspSkyVtxOutCount;

uint32_t gPspCcPoolSize;      /* live occupancy, for the debugger */
uint32_t gPspCcPoolRecycles;  /* how often we ran out; 0 == pool is big enough */
uint32_t gPspCcPoolHighWater;

extern void gfx_scegu_reset_shader_pool(void);
extern int gfx_scegu_shader_pool_full(void);

/* Smallest power of two >= v. Mirrors gfx_scegu.c's nextpow2/ispow2 pair, which
 * is what gfx_scegu_upload_texture uses to decide whether a texture gets
 * resampled -- the two must agree or the UV correction below corrects by the
 * wrong factor. Returns v unchanged when it is already a power of two (and for
 * 0, which callers guard against anyway). */
static uint32_t psp_next_pow2(uint32_t v) {
    uint32_t p = 1;

    if (v == 0) {
        return 0;
    }
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }

    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();

    /* Either pool being full forces the recycle: one new combine mode can
     * consume an entry in both, and they must never run out at different
     * times (see gfx_scegu_create_and_load_new_shader). */
    if (color_combiner_pool_size >= (sizeof(color_combiner_pool) / sizeof(color_combiner_pool[0])) ||
        gfx_scegu_shader_pool_full()) {
        /* gfx_flush() above already drew everything pending under the old
         * state, so nothing in flight still refers to the entries being
         * dropped. unload_shader(NULL) is the "forget whatever is bound" case
         * and is handled; load_shader is never called with NULL. */
        gfx_rapi->unload_shader(rendering_state.shader_program);
        rendering_state.shader_program = NULL;
        gfx_scegu_reset_shader_pool();
        color_combiner_pool_size = 0;
        prev_combiner = NULL;
        ++gPspCcPoolRecycles;
    }

    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gPspCcPoolSize = color_combiner_pool_size;
    if (color_combiner_pool_size > gPspCcPoolHighWater) {
        gPspCcPoolHighWater = color_combiner_pool_size;
    }
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

extern int gfx_vram_space_available(void);
extern void texman_clear(void);

/* Drop every cached texture. Safe ONLY between frames -- see the call site in
 * Play_Init.
 *
 * Nothing used to reset these caches at all: texman_clear() was reachable from
 * exactly one place, the mid-frame exhaustion path below. Textures therefore
 * accumulated across every room visited for the whole run, until the pool
 * filled and got wiped in the middle of a frame -- with the GE still holding
 * pointers into memory that a different texture had just been decoded into.
 * That is the speckle corruption, and it explains why it struck rooms far too
 * small to need the pool themselves (the Dog Lady's house): what filled the
 * pool was every room BEFORE it.
 *
 * Enlarging the pool only moves that point later. Resetting per scene load is
 * the actual fix: one room's textures fit comfortably, so the mid-frame wipe
 * stops being reachable in normal play. */
void gfx_texture_cache_reset(void) {
    texman_clear();
    gfx_texture_cache.pool_pos = 0;
    memset(gfx_texture_cache.pool, 0, sizeof(gfx_texture_cache.pool));
    memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));

    /* The renderer caches which texture is bound per tile; a wiped pool makes
     * those ids meaningless, so force the next draw to re-bind. */
    gfx_scegu_invalidate_texture_binding();

    /* And force the next draw to re-IMPORT, which is the subtle half.
     * rendering_state.textures[] points into the pool that was just wiped, but
     * gfx_sp_tri1 only refreshes it when rdp.textures_changed[] says the tile
     * changed -- a flag that survives this reset. Without this the first draw
     * after a scene load reads a dangling entry and, worse, skips the upload
     * entirely, so the GE samples whatever the new scene has since decoded
     * into that memory. (Clearing the pointers to NULL instead is not enough:
     * that same path would then dereference NULL.) */
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

/* How often the whole texture cache had to be thrown away, split by which
 * limit hit first, plus how full the pool was when it happened. Must be read
 * per-scene: these are cumulative. Non-zero while standing still means the
 * working set does not fit and every frame is re-uploading everything. */
uint32_t gPspTexCacheResetVram;
uint32_t gPspTexCacheResetPool;
uint32_t gPspTexCacheHighWater;

/* Set by gfx_texture_cache_lookup below when the cache needs wiping, acted on
 * in gfx_start_frame(). Same variable, same reasoning as sGfxResumePending
 * further down this file -- see the comment there and at the two call sites
 * below for the hardware evidence that made this necessary. */
static int sTexWipePending;


/* How often the size-aware cache key created a SECOND entry for an address
 * that was already cached at different dimensions. Non-zero means the fix is
 * doing real work; it also quantifies the extra pressure on the 512-entry
 * pool, which is the one cost of keying on size.
 *
 * Before the fix this same code path was a mismatch COUNTER (hits returning a
 * wrongly-sized image) and measured: Bottom of the Well 218, Graveyard 182,
 * Market Day 12, Kakariko Village 0, with gPspTexSizeVariantLast = 0x00100020
 * -- cached at line_size_bytes 16, requested 32, exactly 2x. That was the
 * direction-dependent rainbow masonry in Bottom of the Well. */
uint32_t gPspTexSizeVariants;
uint32_t gPspTexSizeVariantLast;

/* Does this tile ask for mirroring that upload_texture_mirrored will act on?
 * G_TX_CLAMP wins, as on hardware. */
static inline uint8_t tile_wants_mirror(uint32_t cm) {
    return ((cm & G_TX_MIRROR) && !(cm & G_TX_CLAMP)) ? 1 : 0;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz) {
    size_t hash = (uintptr_t)orig_addr;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        /* Dimensions are part of the key, not just the payload.
         *
         * Every importer derives width/height from line_size_bytes and
         * size_bytes (see import_texture_i4 and friends), so the SAME texture
         * address loaded under a different G_SETTILE line / G_LOADBLOCK length
         * decodes to a genuinely different image. Keying only on
         * addr/fmt/siz let the first load win forever: later draws got the
         * old image while gfx_sp_tri1 normalised their UVs against the NEW
         * tile_width, and a 2x disagreement samples the texture as garbage.
         *
         * Measured in Bottom of the Well, whose walls load the same I4 stone
         * textures repeatedly at two sizes: 218 such hits per scene load,
         * cached 16 bytes/line vs requested 32. That is the direction-dependent
         * rainbow-mottled masonry -- the texture is 4-bit GRAYSCALE, so the
         * colour could only ever have come from sampling it wrong, never from
         * SHADE (which interpolates smoothly and cannot produce texel-rate
         * speckle) and never from a second texture layer (the combine uses
         * TEXEL0 only).
         *
         * Same reasoning as the palette field above, and the same fix
         * libultraship applies there: distinct decode inputs get distinct
         * entries. */
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz &&
            (*node)->line_size_bytes == rdp.texture_tile[tile].line_size_bytes &&
            (*node)->size_bytes == LOADED_TEX(tile).size_bytes &&
            (*node)->mirror_s == tile_wants_mirror(rdp.texture_tile[tile].cms) &&
            (*node)->mirror_t == tile_wants_mirror(rdp.texture_tile[tile].cmt) &&
            (fmt != G_IM_FMT_CI || (*node)->palette == rdp.palette)) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            gfx_rapi->set_sampler_parameters(0, (*node)->linear_filter, (*node)->cms, (*node)->cmt);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    /* Both conditions below invalidate the SAME logical cache and must stay
     * in lockstep: gfx_texture_cache.pool[512] (this file) and the PSP-side
     * textures[512] array (psp_texture_manager.c, indexed by psp_tex_number,
     * incremented 1:1 with pool_pos every time gfx_rapi->new_texture() /
     * texman_create() runs below). Originally only the first branch (real
     * PSP VRAM/texture-buffer exhaustion) called texman_clear() -- the
     * second branch (this file's pool array hitting its own 512-entry cap,
     * which can and does happen first if many small/distinct textures are
     * referenced before VRAM bytes run out) reset only pool_pos/hashmap,
     * leaving psp_tex_number un-reset. Since both counters had been
     * growing in lockstep up to that point, psp_tex_number was already at
     * 512 too -- so the very next "new" texture after that reset wrote to
     * textures[512], one past the end of that fixed-size array (real
     * out-of-bounds memory corruption), and every texture after that kept
     * pushing psp_tex_number further out of bounds, since the now-empty
     * lookup cache treats everything as new. Found this session while
     * investigating the period-4 wavy/clean render corruption -- fixed by
     * calling texman_clear() here too, keeping both caches synchronized. */
    if(!gfx_vram_space_available() ||
       gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
#if TARGET_PSP
        /* Session 17: this whole-cache wipe was invisible. Bottom of the Well
         * renders correctly until Link walks forward toward the hole -- i.e.
         * until MORE geometry is in view at once -- and then the walls go
         * garish and speckled, reverting when he backs off. That is the
         * signature of the cache thrashing: every texture evicted and
         * re-uploaded every frame, with the GE still bound to whatever the
         * previous occupant of that VRAM was. Count both trigger reasons
         * separately -- VRAM exhaustion and pool exhaustion have different
         * fixes (a bigger/better-packed VRAM budget vs a bigger pool). */
        if (!gfx_vram_space_available()) {
            ++gPspTexCacheResetVram;
        } else {
            ++gPspTexCacheResetPool;
        }
        gPspTexCacheHighWater = gfx_texture_cache.pool_pos;
#endif
        /* Used to wipe right here with texman_clear() + a pool reset. Two
         * consecutive hardware frames captured on room entry (2026-08-28)
         * showed why that was wrong: the glitch frame has correctly-textured
         * geometry sitting next to rectangular blocks of texel-rate
         * green/white noise, at exactly the polygon boundaries where the
         * NEXT frame's (correct) wall/curtain textures land. That is a wipe
         * reassigning VRAM the GE was still reading draws from, mid-list --
         * not a geometry, matrix, or combiner bug. Only note the wipe is due
         * and let the current, overfull frame finish drawing; the actual
         * reset now happens in gfx_start_frame(), the one point where
         * gfx_end_frame's sceGuFinish/sceGuSync already guarantee the GE has
         * gone idle (same guarantee sGfxResumePending relies on there). */
        sTexWipePending = 1;
    }
#if TARGET_PSP
    /* Walk the chain again to see whether this address is already present at
     * some OTHER size -- i.e. whether keying on size is what made this a miss.
     * Cheap: these chains are short, and this runs only on a cache miss. */
    {
        struct TextureHashmapNode *scan = gfx_texture_cache.hashmap[hash];

        while (scan != NULL && scan - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
            if (scan->texture_addr == orig_addr &&
                (scan->line_size_bytes != rdp.texture_tile[tile].line_size_bytes ||
                 scan->size_bytes != LOADED_TEX(tile).size_bytes)) {
                ++gPspTexSizeVariants;
                gPspTexSizeVariantLast = (scan->line_size_bytes << 16) |
                                         (rdp.texture_tile[tile].line_size_bytes & 0xffff);
                break;
            }
            scan = scan->next;
        }
    }
#endif
    if (gfx_texture_cache.pool_pos < sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    } else {
        /* The one real trap in deferring the wipe above: pool_pos used to be
         * guaranteed < 512 because the wipe above reset it to 0 the instant
         * it reached the cap. Now that the wipe waits for gfx_start_frame(),
         * a scene that references more than 512 distinct textures in one
         * frame would walk pool_pos past the end of a fixed 512-entry array
         * -- real memory corruption, the same class this port has already
         * produced once (see the "Both conditions below" comment above on
         * pool[512] vs textures[512] going out of sync). Pin pool_pos at the
         * cap and reuse the last slot for the rest of the frame instead:
         * that one texture thrashes (repeatedly re-decoded into the same
         * VRAM slot), but every other cached entry stays valid until
         * gfx_start_frame() actually clears everything. */
        *node = &gfx_texture_cache.pool[sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode) - 1];
    }
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    /*@Note: unneeded due to sequential GE flow */
    //gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->palette = rdp.palette;
    (*node)->line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
    (*node)->size_bytes = LOADED_TEX(tile).size_bytes;
    *n = *node;
    return false;
}

/* ---------------------------------------------------------------------------
 * Texture source byte order (PSP)
 *
 * Texture pixel data reaches us through two paths with DIFFERENT byte orders,
 * and every per-format hack this file used to carry was really an attempt to
 * paper over that:
 *
 *  1. Compiled-in assets. The extracted assets declare texture data as
 *     `u64 name[TEX_LEN(u64, W, H, siz)] = { 0x..., ... }` (see e.g.
 *     assets/objects/gameplay_keep/link_textures.c). Those are 64-bit
 *     *literals*: on a little-endian target the compiler stores each one with
 *     its 8 bytes reversed relative to the N64's big-endian byte stream. Read
 *     byte-wise (which is what every importer below does) they come out
 *     reversed in 8-byte groups.
 *  2. ROM/DMA-loaded assets. Scene/room/object files are copied verbatim out
 *     of the big-endian .z64 by PspRom_Read; z_endian_fixup_psp.c only ever
 *     swaps *struct fields* (pointers, counts, s16 arrays, display-list
 *     commands), never raw pixel data. Byte-wise reads are already correct.
 *
 * Structured compiled-in data (Vtx, Gfx, s16 tables) is unaffected: it is
 * emitted as native-endian fields and read back as such.
 *
 * The two cases are distinguishable at runtime by address: anything with an
 * initializer lives in the module's initialized-data range [_ftext,
 * __bss_start), while DMA targets are .bss/arena/heap, i.e. at or above
 * __bss_start. So we decide once per texture (and once per TLUT) and index
 * through tex_src_index() everywhere, instead of hardcoding an assumption per
 * pixel format.
 *
 *  3. BLOB-loaded assets (psp/src/psp_blob_assets.c). These are a THIRD case
 *     and they behave like case 1, not case 2: a blob is produced by compiling
 *     the very same `u64 name[] = { 0x..., ... }` sources with the PSP
 *     (little-endian) compiler, so its pixel bytes are reversed in 8-byte
 *     groups exactly like compiled-in data. But a blob is read into the arena,
 *     so the address test below places it with the DMA case and the unswap was
 *     skipped -- which showed up immediately as correct geometry covered in
 *     confetti-coloured noise the first time a scene came from a blob.
 *
 * So the question this predicate really asks is "did a little-endian compiler
 * produce these bytes from u64 literals?", and that is the same question
 * PspStaticAssetIsStatic() answers for the endian fixups. Call it rather than
 * keeping a second copy of the rule here: session 9 already lost time to two
 * copies of the segment discriminator drifting apart, and this is the same
 * trap. */
static inline bool tex_needs_u64_unswap(const void *addr) {
    const bool native = PspStaticAssetIsStatic(addr) != 0;

    /* Counted here rather than at the call sites: there are eight importers and
     * each asks exactly once, so this is the one place that cannot be missed
     * when a ninth is added. See the probe comment on PspGfxFrameStats. */
    if (native) {
        GFXSTAT_INC(tex_unswap_yes);
        if (gPspSkyTriMark) {
            GFXSTAT_INC(sky_tex_unswap);
        }
    } else {
        GFXSTAT_INC(tex_unswap_no);
    }
    return native;
}

/* Undo the compiler's little-endian storage of a u64 literal: byte i of the
 * intended big-endian stream lives at (i & ~7) + (7 - (i & 7)). */
static inline uint32_t tex_src_index(uint32_t i, bool unswap) {
    return unswap ? ((i & ~7u) | (7u - (i & 7u))) : i;
}

/* Read helpers used by every importer below. TEXSRC needs `tile` and
 * `tex_unswap` in scope, PALSRC needs `pal_unswap`. */
/* Map a linear index within the TILE onto its offset in the SOURCE image. They
 * differ only for a G_LOADTILE sub-rectangle; see gfx_dp_load_tile. */
static inline uint32_t tex_row_index(int tile, uint32_t i) {
    uint32_t line = rdp.texture_tile[tile].line_size_bytes;
    uint32_t stride = rdp.texture_tile[tile].src_stride_bytes;

    if (stride == 0 || line == 0 || stride == line) {
        return i;
    }
    return (i / line) * stride + (i % line);
}

#define TEXSRC(i) (LOADED_TEX(tile).addr[tex_src_index(tex_row_index(tile, (i)), tex_unswap)])
#define PALSRC(i) (rdp.palette[tex_src_index((i), pal_unswap)])
/* ------------------------------------------------------------------------ */

/* Scratch for the mirrored copy. Worst case is a 4 KB TMEM tile decoded to
 * 8-bit-per-channel RGBA (32 KB, the size of rgba32_buf below) mirrored on
 * BOTH axes, i.e. four times that. */
static uint8_t mirror_buf[128 * 1024] __attribute__((aligned(16)));

/* Upload, emulating G_TX_MIRROR where the hardware cannot.
 *
 * The RDP mirrors a tile by reflecting the texture coordinate every `mask`
 * texels; the GE cannot, and gfx_cm_to_opengl() has always quietly downgraded
 * mirror to plain REPEAT. The visible result is that the mirrored half repeats
 * instead of reflecting -- e.g. the Market's shop doors, whose leaf is one
 * half-width texture mirrored in S (gFieldDoorLeftDL) with the knob plate
 * mirrored in T, came out with the knob strip reversed.
 *
 * Baking the reflection into the uploaded image reproduces it exactly: the
 * upload becomes [image | mirrored image], REPEAT walks over the pair, and the
 * UV scale halves (gfx_sp_tri1 doubles tex_width/tex_height to match).
 *
 * G_TX_CLAMP wins over mirroring, as it does on hardware, so a clamped axis is
 * left alone. If the doubled image would not fit the scratch buffer the
 * texture is uploaded unmirrored and the flags are cleared, so the UV scale
 * stays consistent with what was actually uploaded. */
static void upload_texture_mirrored(int tile, const uint8_t *buf, uint32_t width, uint32_t height, int psm) {
    const uint32_t cms = rdp.texture_tile[tile].cms;
    const uint32_t cmt = rdp.texture_tile[tile].cmt;
    bool mir_s = (cms & G_TX_MIRROR) && !(cms & G_TX_CLAMP);
    bool mir_t = (cmt & G_TX_MIRROR) && !(cmt & G_TX_CLAMP);
    const uint32_t bpp = (psm == GU_PSM_8888) ? 4 : 2;
    uint32_t out_w = width << (mir_s ? 1 : 0);
    uint32_t out_h = height << (mir_t ? 1 : 0);

    if ((mir_s || mir_t) && (out_w * out_h * bpp) > sizeof(mirror_buf)) {
        mir_s = mir_t = false;
    }

    if (!mir_s && !mir_t) {
        rendering_state.mirror_s = 0;
        rendering_state.mirror_t = 0;
        gfx_rapi->upload_texture(buf, width, height, psm);
        return;
    }

    out_w = width << (mir_s ? 1 : 0);
    out_h = height << (mir_t ? 1 : 0);

    /* Horizontal pass: each source row becomes [row | reversed row]. */
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = buf + (size_t)y * width * bpp;
        uint8_t *dst = mirror_buf + (size_t)y * out_w * bpp;

        memcpy(dst, src, width * bpp);
        if (mir_s) {
            for (uint32_t x = 0; x < width; x++) {
                memcpy(dst + (size_t)(width + x) * bpp, src + (size_t)(width - 1 - x) * bpp, bpp);
            }
        }
    }

    /* Vertical pass: append the rows already written, bottom-up. */
    if (mir_t) {
        for (uint32_t y = 0; y < height; y++) {
            memcpy(mirror_buf + (size_t)(height + y) * out_w * bpp,
                   mirror_buf + (size_t)(height - 1 - y) * out_w * bpp, out_w * bpp);
        }
    }

    rendering_state.mirror_s = mir_s;
    rendering_state.mirror_t = mir_t;
    gfx_rapi->upload_texture(mirror_buf, out_w, out_h, psm);
}

static void import_texture_rgba16(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint16_t rgba16_buf[4096] __attribute__ ((aligned(4)));    
    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes / 2; i++) {
        uint16_t col16 = (TEXSRC(2 * i) << 8) | TEXSRC(2 * i + 1);
        const uint8_t a = col16 & 1;
        const uint8_t r = (col16 >> 11) & 0x1f;
        const uint8_t g = (col16 >> 6) & 0x1f;
        const uint8_t b = (col16 >> 1) & 0x1f;
        rgba16_buf[i] = (a << 15)  | (b << 10)  | (g << 5) | (r);
    }
    
    uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;

    upload_texture_mirrored(tile, (const uint8_t*)rgba16_buf, width, height, GU_PSM_5551);
}

static void import_texture_rgba32(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    uint32_t height = (LOADED_TEX(tile).size_bytes / 2) / rdp.texture_tile[tile].line_size_bytes;

    /* The DMA'd case (by far the common one) still uploads straight from the
     * source with no copy; only compiled-in u64-literal data needs the
     * unswap pass, and then we have to stage it. */
    if (!tex_unswap) {
        upload_texture_mirrored(tile, LOADED_TEX(tile).addr, width, height, GU_PSM_8888);
        return;
    }

    {
        static uint8_t rgba32_buf[16384];
        uint32_t n = LOADED_TEX(tile).size_bytes;
        if (n > sizeof(rgba32_buf)) {
            n = sizeof(rgba32_buf);
        }
        for (uint32_t i = 0; i < n; i++) {
            rgba32_buf[i] = TEXSRC(i);
        }
        upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
    }
}

static void import_texture_ia4(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint8_t rgba32_buf[32768] __attribute__ ((aligned(4)));
    
    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes * 2; i++) {
        uint8_t byte = TEXSRC(i / 2);
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part >> 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_3_8(r);
        rgba32_buf[4*i + 1] = SCALE_3_8(g);
        rgba32_buf[4*i + 2] = SCALE_3_8(b);
        rgba32_buf[4*i + 3] = alpha ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;
    
    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_ia8(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint8_t rgba32_buf[16384]__attribute__ ((aligned(4)));
    
    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes; i++) {
        uint8_t intensity = TEXSRC(i) >> 4;
        uint8_t alpha = TEXSRC(i) & 0xf;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = SCALE_4_8(alpha);
    }
    
    uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;
    
    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_ia16(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint8_t rgba32_buf[8192];
    
    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes / 2; i++) {
        uint8_t intensity = TEXSRC(2 * i);
        uint8_t alpha = TEXSRC(2 * i + 1);
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = alpha;
    }
    
    uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;
    
    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}

/* Session 17 probe for the Bottom of the Well walls. Those are the port's
 * first heavy use of G_IM_FMT_I, and the rainbow mottling can only come from
 * the intensity data being read wrong (I4 is GRAYSCALE -- neither SHADE, which
 * interpolates smoothly, nor a second texture layer, which this combine does
 * not have, can produce texel-rate colour). So record what the importer
 * actually sees, for the LAST I4 texture of each frame:
 *   [0] addr  [1] tex_unswap  [2] line_size_bytes  [3] size_bytes
 *   [4] derived width  [5] derived height  [6] src_stride_bytes
 *   [7..14] first 32 nibbles as read through TEXSRC, packed 8 per word
 * Compare [7..] against the same bytes in the blob on disk: equal means the
 * read path is fine and the fault is downstream, different means it is here. */
int gDebugI4Opaque = 0;
uint32_t gPspI4Probe[16];

static void import_texture_i4(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint8_t rgba32_buf[32768];

#if TARGET_PSP
    gPspI4Probe[0] = (uint32_t)(uintptr_t)LOADED_TEX(tile).addr;
    gPspI4Probe[1] = tex_unswap ? 1u : 0u;
    gPspI4Probe[2] = rdp.texture_tile[tile].line_size_bytes;
    gPspI4Probe[3] = LOADED_TEX(tile).size_bytes;
    gPspI4Probe[4] = rdp.texture_tile[tile].line_size_bytes * 2;
    gPspI4Probe[5] = rdp.texture_tile[tile].line_size_bytes
                         ? LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes
                         : 0;
    gPspI4Probe[6] = rdp.texture_tile[tile].src_stride_bytes;
    {
        int w;

        for (w = 0; w < 8; w++) {
            uint32_t packed = 0;
            int b;

            for (b = 0; b < 4; b++) {
                packed |= (uint32_t)TEXSRC(w * 4 + b) << (b * 8);
            }
            gPspI4Probe[7 + w] = packed;
        }
    }
#endif

    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes * 2; i++) {
        uint8_t byte = TEXSRC(i / 2);
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        /* Real N64 RDP I-format hardware outputs A=I, not a constant --
         * OoT's console-logo text relies on this (its combine samples
         * TEXEL0's alpha directly, gDPSetCombineLERP's alpha cycle in
         * ConsoleLogo_Draw) to fade out the glyph texture's dark/background
         * pixels instead of showing them opaque black. */
        /* I4's alpha = intensity is correct N64 behaviour and the console
         * logo depends on it (its combine samples TEXEL0's alpha directly).
         *
         * TESTED AND REFUTED as the cause of the Bottom of the Well glitch:
         * the theory was that HAKAdan's constant-1 alpha cycle means vanilla
         * discards the texture alpha, so letting it reach the alpha test
         * (G_RM_AA_ZB_TEX_EDGE2 / CVG_X_ALPHA) would punch holes in the walls.
         * The user confirmed the walls were never see-through, which kills it.
         * Left as an A/B switch; default 0 is the correct behaviour. */
        extern int gDebugI4Opaque;
        rgba32_buf[4*i + 3] = gDebugI4Opaque ? 255 : SCALE_4_8(intensity);
    }

    uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;

    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_i8(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes; i++) {
        uint8_t intensity = TEXSRC(i);
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        /* See import_texture_i4's identical comment -- real N64 RDP
         * I-format hardware outputs A=I, not a constant. */
        rgba32_buf[4*i + 3] = intensity;
    }

    uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;

    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}


static void import_texture_ci4(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    const bool pal_unswap = tex_needs_u64_unswap(rdp.palette);
    uint8_t rgba32_buf[32768];

    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes * 2; i++) {
        uint8_t byte = TEXSRC(i / 2);
        uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint16_t col16 = (PALSRC(idx * 2) << 8) | PALSRC(idx * 2 + 1); // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;
    
    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}

/* A/B switch for the CI8 alpha bit, pokeable at runtime with the debugger so a
 * test costs no rebuild (same idea as gDebugDisableCull).
 *
 * CI8 takes its transparency from ONE bit of the 16-bit palette entry, so a
 * palette that is byte-swapped or off by one byte turns alpha into noise and
 * individual texels vanish -- scattered holes across exactly the limbs that use
 * CI8, which is 27 of the textures in Link's object and none at all in the test
 * scene, so nothing before now would have shown it.
 *
 *   set to 1 -> holes fill in  => the palette decode is wrong, look at PALSRC /
 *                                the TLUT unswap
 *   set to 1 -> holes stay     => alpha is innocent, the geometry really is
 *                                missing; look at the flex-limb matrices */
int gDebugCi8Opaque = 0;

static void import_texture_ci8(int tile) {
    const bool tex_unswap = tex_needs_u64_unswap(LOADED_TEX(tile).addr);
    const bool pal_unswap = tex_needs_u64_unswap(rdp.palette);
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < LOADED_TEX(tile).size_bytes; i++) {
        uint8_t idx = TEXSRC(i);
        uint16_t col16 = (PALSRC(idx * 2) << 8) | PALSRC(idx * 2 + 1); // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = (a || gDebugCi8Opaque) ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    uint32_t height = LOADED_TEX(tile).size_bytes / rdp.texture_tile[tile].line_size_bytes;
    
    upload_texture_mirrored(tile, rgba32_buf, width, height, GU_PSM_8888);
}

/* G_SETTILE's shift_s/shift_t, which this port ignored entirely (they sat
 * behind _UNUSED in gfx_dp_set_tile). The RDP's texture-coordinate unit
 * applies them to every incoming S/T before subtracting the tile origin:
 * 0 means no shift, 1..10 shift RIGHT by that many bits, and 11..15 shift
 * LEFT by 16 - value.
 *
 * Dropping them scales the texture wrongly on exactly the surfaces that use
 * them. The Chamber of the Sages water columns are the clearest case
 * (shift_s = 14, shift_t = 15, i.e. S*4 and T*2): at 1x the 32x64 ripple
 * texture stretches to four times its intended width across each face, so the
 * "wobbling mass" of the original degenerates into big drifting rectangles.
 * Death Mountain Crater's lava carries shift_s = 15 for the same reason. Most
 * tiles pass G_TX_NOLOD (0) here and are unaffected. */
static inline int32_t tex_shift_coord(int32_t coord, uint8_t shift) {
    if (shift == 0) {
        return coord;
    }
    if (shift <= 10) {
        return coord >> shift;
    }
    return coord << (16 - shift);
}

static void import_texture(int tile) {
    uint8_t fmt = rdp.texture_tile[tile].fmt;
    uint8_t siz = rdp.texture_tile[tile].siz;

    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], LOADED_TEX(tile).addr, fmt, siz)) {
        GFXSTAT_INC(tex_hits);
        if (gPspSkyTriMark) {
            GFXSTAT_INC(sky_tex_hits);
        }
        return;
    }
    GFXSTAT_INC(tex_imports);
    if (gPspSkyTriMark) {
        GFXSTAT_INC(sky_tex_imports);
    }

    //int t0 = get_time();
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    //int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);

    /* upload_texture_mirrored just told us whether it actually baked a
     * reflection in. Park it on the cache entry so a later HIT (which skips
     * the decode entirely) still scales its UVs the same way. */
    if (rendering_state.textures[tile] != NULL) {
        rendering_state.textures[tile]->mirror_s = rendering_state.mirror_s;
        rendering_state.textures[tile]->mirror_t = rendering_state.mirror_t;
    }
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

static void gfx_normalize_vector(float v[3]) {
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if(dot > 0.00001f){
        const float scale = 1.0f / sqrtf(dot);
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
    }
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

#if !defined(TARGET_PSP)
static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}
#else 
static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
  	__asm__ volatile (
        ".set			push\n"					// save assember option
        ".set			noreorder\n"			// suppress reordering
		"lv.q   R000, 0  + %1\n"
		"lv.q   R001, 16 + %1\n"
		"lv.q   R002, 32 + %1\n"
		"lv.q   R003, 48 + %1\n"

		"lv.q   R100, 0  + %2\n"
		"lv.q   R101, 16 + %2\n"
		"lv.q   R102, 32 + %2\n"
		"lv.q   R103, 48 + %2\n"

		"vmmul.q   M700, M000, M100\n"

		"sv.q   R700, 0  + %0\n"
		"sv.q   R701, 16 + %0\n"
		"sv.q   R702, 32 + %0\n"
		"sv.q   R703, 48 + %0\n"
        ".set			pop\n"					// restore assember option
		: "=m" (*res) : "m" (*a) ,"m" (*b) : "memory" );
}
#endif

/* Runtime-pokeable A/B for the batch-vs-matrix ordering described at
 * mtx_dirty_events above. 1 = flush pending triangles before every matrix
 * upload (correct ordering); 0 = the inherited sm64-port-psp behaviour, which
 * lets a batch straddle matrix changes. Poke this live to compare without a
 * rebuild. */
int gDebugFlushOnMtx = 1;
/* 1 = re-upload GU_MODEL after G_POPMTX. The inherited code updates only the
 * software rsp.MP_matrix on a pop and never tells the GE, so after a pop the
 * hardware still holds the popped child's matrix until the next G_MTX load. */
int gDebugPopUploadMtx = 1;

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4] __attribute__((aligned(16)));

#if TARGET_PSP
    if (buf_vbo_num_tris > 0) {
        GFXSTAT_INC(mtx_dirty_events);
        GFXSTAT_ADD(mtx_dirty_tris, buf_vbo_num_tris);
    }
#endif
    /* Must happen before either sceGuSetMatrix below: the GE consumes the
     * matrix command in list order, so anything still buffered would otherwise
     * be transformed by the matrix that is about to be uploaded. */
    if (gDebugFlushOnMtx) {
        gfx_flush();
    }

#ifndef GBI_FLOATS
    /* Original GBI where fixed point matrices are used. Real N64 Mtx format
     * (see Mtx union in ultra64/gbi.h): 16 u16 integer parts (intPart[4][4],
     * row-major) followed by 16 u16 fractional parts (fracPart[4][4]) --
     * this is a genuine BIG-ENDIAN-shaped layout, since real N64 hardware
     * reads pairs of adjacent u16 values as one 32-bit word with the
     * earlier (lower-address) u16 as the MORE significant half. The
     * original sm64-port-psp code below (kept for reference/GBI_FLOATS
     * builds) does exactly that pairing via int32_t reads -- correct on
     * real (big-endian) N64/its usual PC-port targets, but PSP is
     * LITTLE-ENDIAN, so that same bit-manipulation silently swaps every
     * adjacent pair of matrix components (row (a,b,c,d) becomes (b,a,d,c)).
     * Confirmed this session via direct modelview-matrix logging during
     * ConsoleLogo_Draw (gNintendo64LogoDL): at zero rotation the loaded
     * matrix's row0/row1 showed (0,1,0,0)/(1,0,0,0) instead of the
     * expected identity (1,0,0,0)/(0,1,0,0) -- an X/Y swap matching this
     * exact mechanism, present on EVERY G_MTX load in the entire game, not
     * just this logo. Fixed below by reading each u16 half directly by its
     * own index (matching intPart[4][4]/fracPart[4][4]'s natural row-major
     * layout) instead of reinterpreting adjacent pairs as int32 -- this is
     * endianness-independent. */
    {
        const int16_t *int_parts = (const int16_t *)addr;
        const uint16_t *frac_parts = (const uint16_t *)addr + 16;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                int32_t whole = ((int32_t)int_parts[i * 4 + j] << 16) | frac_parts[i * 4 + j];
                matrix[i][j] = whole / 65536.0f;
            }
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
#if TARGET_PSP
        /* Capture the frame's first projection matrix. The frame stats showed
         * identical geometry (same dl_cmds/verts/tri_calls) producing wildly
         * different clip-rejection counts on alternating frames, which can
         * only be the transform -- so compare the actual matrix across the two
         * frame types instead of reasoning about it. */
        if (gPspGfxMtx.proj_loads < PSP_MTX_PROJ_SLOTS) {
            memcpy(gPspGfxMtx.proj[gPspGfxMtx.proj_loads], rsp.P_matrix,
                   sizeof(gPspGfxMtx.proj[0]));
        }
        ++gPspGfxMtx.proj_loads;
#endif
        /* Allocate space in DL for current proj matrix */
        void *matrix_inline = (void *)ALIGN((unsigned int)sceGuGetMemory(sizeof(rsp.P_matrix)+15), 16);
        memcpy(matrix_inline, rsp.P_matrix, sizeof(rsp.P_matrix));
        sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)matrix_inline);
    } else { // G_MTX_MODELVIEW
        /* The stack starts EMPTY (static -> 0) and OoT never pushes: every limb
         * matrix is an absolute G_MTX_LOAD | G_MTX_NOPUSH, so size stays 0 and
         * every access below indexes modelview_matrix_stack[-1]. That is 64
         * bytes BEFORE rsp -- which the linker fills with the tail of `rdp`,
         * starting exactly at other_mode_l and running through other_mode_h,
         * combine_mode, env/prim/fog/fill_color, viewport and scissor.
         *
         * So "the current modelview matrix" and "the back half of the RDP
         * state" were literally the same 64 bytes: every gDPSetPrimColor,
         * gDPSetEnvColor and gsDPSetOtherMode issued between a matrix load and
         * its triangles overwrote that matrix, and every matrix load trashed
         * the render mode and colours in return. Small ints landing in float
         * slots is why the result was DISPLACED geometry rather than exploded
         * geometry.
         *
         * libultraship guards this (interpreter.cpp:1482, `if (size == 0)
         * ++size` before the load memcpy); sm64-port-psp, which this file is a
         * fork of, does not -- SM64 pushes before it loads, so it never has an
         * empty stack. Bumping the floor here covers the MUL branch and the
         * MP_matrix recompute below as well, not just G_MTX_LOAD. */
        if (rsp.modelview_matrix_stack_size == 0) {
            rsp.modelview_matrix_stack_size = 1;
        }
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
#if TARGET_PSP
            /* Capture the modelview the SKYBOX loads, decoded to floats exactly
             * as the renderer will use it. rot is 0 for both skyboxes and the
             * eye reads (0,64,0), so this must come out as a pure translation;
             * anything else means the Mtx decode or Matrix_MtxFToMtx is the
             * source of the tilt. This is the last link in that chain that was
             * inferred rather than measured. */
            if (gPspSkyTriMark) {
                memcpy(gPspSkyMtx, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                       sizeof(gPspSkyMtx));
            }
#endif
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
#if TARGET_PSP
        ++gPspGfxMtx.mv_loads;
        if (gPspGfxMtx.mv_loads == 1) {
            memcpy(gPspGfxMtx.mv_first,
                   rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                   sizeof(gPspGfxMtx.mv_first));
        }
        /* Attribute everything drawn from here on to this load, and record
         * what transform it actually produced. */
        sPspMtxCurSlot = gPspGfxMtx.mv_loads - 1;
        if (sPspMtxCurSlot < PSP_MTX_MV_SLOTS) {
            const float (*mv)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
            gPspGfxMtx.mv_hash[sPspMtxCurSlot] = psp_mtx_hash(mv);
            gPspGfxMtx.mv_trans[sPspMtxCurSlot][0] = mv[3][0];
            gPspGfxMtx.mv_trans[sPspMtxCurSlot][1] = mv[3][1];
            gPspGfxMtx.mv_trans[sPspMtxCurSlot][2] = mv[3][2];
            gPspGfxMtx.mv_det[sPspMtxCurSlot] = psp_mtx_det3(mv);
        }
#endif
        /* The modelview is applied in software in gfx_sp_vertex now (N64 G_VTX
         * semantics), so the GE must NOT apply it a second time. GU_MODEL is
         * set to identity once at init and deliberately left alone here. */
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
#if TARGET_PSP
    if (buf_vbo_num_tris > 0) {
        GFXSTAT_INC(mtxpop_dirty_events);
    }
#endif
    if (gDebugFlushOnMtx) {
        gfx_flush();
    }
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
        gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
        /* No GU_MODEL upload here: the modelview is applied in software at
         * G_VTX time (see gfx_sp_vertex), so the GE's model matrix stays
         * identity throughout. Restoring the software matrix above is all a
         * pop has to do now. */
        rsp.lights_changed = 1;
    }
}

/* 1 = inherited behaviour (aspect-correct the clip-space x). 0 = clip in the
 * same space the GE rasterises in. See the use site in gfx_sp_vertex. */
int gDebugClipAspectAdjust = 1;

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / ((float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height);
}

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    float temp_vec[4] __attribute__((aligned(16)));
    float world_vec[4] __attribute__((aligned(16)));
    float proj_vec[4] __attribute__((aligned(16)));
    GFXSTAT_ADD(verts_loaded, n_vertices);
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];

        temp_vec[0] = v->ob[0];
        temp_vec[1] = v->ob[1];
        temp_vec[2] = v->ob[2];
        temp_vec[3] = 1.0f;

        __asm__ volatile (
            ".set			push\n"					// save assember option
            ".set			noreorder\n"			// suppress reordering
            "lv.q			c700,  0 + %1\n"
            "lv.q			c710, 16 + %1\n"
            "lv.q			c720, 32 + %1\n"
            "lv.q			c730, 48 + %1\n"
            "lv.q			c200, %2\n"
            "vtfm4.q		c000, e700, c200\n"
            "sv.q			c000, %0\n"
            ".set			pop\n"
            : "=m"(*world_vec)
            : "m"(*rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]), "m"(*temp_vec)
        );

        __asm__ volatile (
            ".set			push\n"
            ".set			noreorder\n"
            "lv.q			c700,  0 + %1\n"
            "lv.q			c710, 16 + %1\n"
            "lv.q			c720, 32 + %1\n"
            "lv.q			c730, 48 + %1\n"
            "lv.q			c200, %2\n"
            "vtfm4.q		c000, e700, c200\n"
            "sv.q			c000, %0\n"
            ".set			pop\n"
            : "=m"(*proj_vec)
            : "m"(*rsp.P_matrix), "m"(*world_vec)
        );

        /* N64 semantics: G_VTX transforms IMMEDIATELY, so a vertex belongs to
         * the matrix in force when it was loaded. This port used to store raw
         * object space and let the GE apply GU_MODEL at DRAW time -- so any
         * display list that loads a matrix, loads vertices, then loads a
         * DIFFERENT matrix before drawing (measured: 138 of ~550 triangles per
         * frame, load slot 7 -> draw slot 8) silently re-transformed those
         * vertices onto the neighbouring limb. That is what pasted Link's chest
         * onto his chin.
         *
         * Now the modelview is applied HERE, at load time, and GU_MODEL is kept
         * at identity so the GE only applies the projection. */
        //const float x = proj_vec[0];
        /* CLIP SPACE vs RENDER SPACE, and they disagree.
         *
         * _x below feeds the trivial-reject test and clip_to_frustum; d->x/y/z
         * feed the GE, which transforms them with P_matrix alone. Applying the
         * aspect correction to only the first means the CPU clips against a
         * frustum 1/0.7556 wider in x than the one actually rasterised
         * (gfx_calc_and_set_viewport uses RATIO_X = width/320 = 1.5, i.e. the
         * full 480, so nothing is pillarboxed).
         *
         * Runtime switch because the consequence is only visible on geometry
         * that is HEAVILY clipped -- ordinary room and actor geometry sits
         * inside the frustum and never notices. The skybox does: it is a box
         * around the camera, so 196 of its 256 triangles get clipped.
         *
         * TESTED AND REFUTED as the cause of the skybox's tilt: with this off,
         * i.e. clipping in exactly the space the GE rasterises in, the tilt is
         * unchanged. Default is therefore left at the inherited behaviour. The
         * inconsistency above is real and worth revisiting, but it is not that
         * bug. */
        extern int gDebugClipAspectAdjust;
        const float x = gDebugClipAspectAdjust ? gfx_adjust_x_for_aspect_ratio(proj_vec[0])
                                               : proj_vec[0];
        const float y = proj_vec[1];
        const float z = proj_vec[2];
        float w = proj_vec[3];

        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        
        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lights_changed = false;
            }
            
            unsigned int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            unsigned int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            unsigned int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
            
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }
            
            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            /* Shade probe -- see PspGfxFrameStats. Records the LAST lit vertex
             * of the frame rather than the first: the first vertices of a frame
             * are Link and the fountain (rgba16, coloured anyway), the room
             * geometry comes later, and the room is the thing in question. */
            gPspGfxStats.num_lights = rsp.current_num_lights;
            gPspGfxStats.amb_color =
                ((uint32_t)rsp.current_lights[rsp.current_num_lights - 1].col[0] << 16) |
                ((uint32_t)rsp.current_lights[rsp.current_num_lights - 1].col[1] << 8) |
                ((uint32_t)rsp.current_lights[rsp.current_num_lights - 1].col[2]);
            gPspGfxStats.light0_color = ((uint32_t)rsp.current_lights[0].col[0] << 16) |
                                        ((uint32_t)rsp.current_lights[0].col[1] << 8) |
                                        ((uint32_t)rsp.current_lights[0].col[2]);
            gPspGfxStats.lit_color = ((uint32_t)d->color.r << 16) | ((uint32_t)d->color.g << 8) |
                                     ((uint32_t)d->color.b);
            gPspGfxStats.lit_samples++;


            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;

        /* TEMPORARY diagnostic (see plan/memory notes): force solid white
         * to isolate whether garbled rendering is a geometry/matrix
         * problem (silhouette should look like a recognizable logo shape)
         * vs a color/lighting/texture problem (silhouette already garbled
         * even with color forced off). Remove once diagnosed. */
        extern int gDebugForceWhiteVerts;
        if (gDebugForceWhiteVerts) {
            d->color.r = d->color.g = d->color.b = 255;
        }

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= X_POS;
        if (x > w) d->clip_rej |= X_NEG;
        if (y < -w) d->clip_rej |= Y_POS;
        if (y > w) d->clip_rej |= Y_NEG;
        /* Z_POS is the near plane. `z < -w` is the plane at zNear; the
         * generalised form `w < -t*z` is the same plane at t == 1 and slides
         * towards the eye as t -> 0, matching F3DZEX2.NoN (see gPspNearClipT).
         * This has to match near_plane[] in clip_to_frustum exactly, otherwise
         * triangles get trivially rejected that the clipper would have kept. */
        if (w < -gPspNearClipT * z) d->clip_rej |= Z_POS;
        if (z > w) d->clip_rej |= Z_NEG;

        d->x = world_vec[0];
        d->y = world_vec[1];
        d->z = world_vec[2];
#if TARGET_PSP
        d->mtx_slot_at_load = sPspMtxCurSlot;
#endif

        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;

#if TARGET_PSP
        if (gPspSkyTriMark && gPspSkyVtxOutCount < 256) {
            float *o = gPspSkyVtxOut[gPspSkyVtxOutCount++];

            o[0] = v->ob[0]; o[1] = v->ob[1]; o[2] = v->ob[2];
            o[3] = (float)d->clip_rej;
            o[4] = (float)dest_index;
            o[5] = (w != 0.0f) ? x / w : 0.0f;
            o[6] = (w != 0.0f) ? y / w : 0.0f;
            o[7] = w;
        }
#endif

        /*@Note: this is a trainwreck*/
        /*if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }
            
            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }
            
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
            //d->color.r = d->color.r + (rdp.fog_color.r - d->color.r) * (fog_z/255);
            //d->color.g = d->color.g + (rdp.fog_color.g - d->color.g) * (fog_z/255);
            //d->color.b = d->color.b + (rdp.fog_color.b - d->color.b) * (fog_z/255);
            
            d->color.r = d->color.r + (255 - d->color.r) * (fog_z/255);
            d->color.g = d->color.g + (0 - d->color.g) * (fog_z/255);
            d->color.b = d->color.b + (0 - d->color.b) * (fog_z/255);
            //d->color.r = 255-fog_z;
            //d->color.g = 255-fog_z;
            //d->color.b = 255-fog_z;
            d->color.a = 255;
        } else {
            d->color.a = v->cn[3];
        }*/
        d->color.a = v->cn[3];
    }
}

/* The colour register a CC_* operand names, or NULL if it is not a register
 * (a texel, or the constant 0). Used by the two-input product below. */
static inline const struct RGBA *cc_operand_color(uint8_t cc, const struct LoadedVertex *v) {
    switch (cc) {
        case CC_PRIM:
            return &rdp.prim_color;
        case CC_SHADE:
            return &v->color;
        case CC_ENV:
            return &rdp.env_color;
        default:
            return NULL;
    }
}

/* 1 = also require Z_CMP from other_mode_l before enabling the depth test,
 * matching libultraship. 0 = the inherited sm64-port behaviour (G_ZBUFFER
 * only). Runtime-pokeable so the two can be compared without a rebuild. */
int gDebugZCmpMode = 1;

/* Shared by gfx_sp_tri1 and its 2D twin -- the two copies of this logic had
 * already drifted once in this file's history. */
static inline bool psp_depth_test_enabled(void) {
    bool zbuf = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    bool zcmp = (rdp.other_mode_l & Z_CMP) == Z_CMP;

    if (zbuf) {
        GFXSTAT_INC(depth_zbuf_set);
        if (!zcmp) {
            GFXSTAT_INC(depth_zcmp_clear);
        }
    }
    if ((rdp.other_mode_l & Z_UPD) != Z_UPD) {
        GFXSTAT_INC(depth_zupd_off);
    }

    return gDebugZCmpMode ? (zbuf && zcmp) : zbuf;
}

/* --- Distance fog ---------------------------------------------------------
 *
 * This was dead code for the whole life of the port: the G_FOG block in
 * gfx_sp_vertex is commented out and gfx_scegu.c's sceGuFog call with it. The
 * consequence is not subtle, and it is not a fog-only consequence -- in OoT the
 * scene's ATMOSPHERE is fog. Measured in the scene data (Auftrag 04):
 *
 *   Zora's Domain   fogColor (25,100,100), fogNear 990  -- reported as
 *                   "the bluish cast is missing entirely, the scene looks neutral"
 *   Skulltula House fogColor (10,0,10),   fogNear 930, zFar 2000 -- reported
 *                   as "too bright"
 *
 * Both reports are this one gap, not a lighting bug.
 *
 * WHAT THE N64 DOES. G_MOVEWORD/G_MW_FOG carries (fog_mul, fog_offset), and the
 * RSP computes per vertex
 *     fog = clamp(z_ndc * fog_mul + fog_offset, 0, 255)
 * which the blender then uses to lerp the finished pixel towards fog_color.
 * gSPFogPosition(near, far) generates fog_mul = 128000/(far-near) and
 * fog_offset = (500-near)*256/(far-near), with near/far on a 0..1000 scale.
 * Substituting shows what that scale IS: the fog factor is 0 exactly where
 * 500*(z_ndc+1) == near and 255 where it == far. So "N64 fog depth" is simply
 * the normalised device depth mapped onto 0..1000. That identity is what makes
 * the conversion below possible, and it is worth stating because it is not
 * written down anywhere in the GBI headers.
 *
 * WHAT THE GE DOES. sceGuFog(start, end, colour) fogs linearly in EYE-SPACE
 * DISTANCE, and applies it after texturing -- the same place in the pipeline as
 * the N64's blender. Since session 13 this port hands the GE eye-space vertices
 * with GU_MODEL/GU_VIEW at identity, so the GE has exactly the z it needs.
 *
 * THE CONVERSION. Undo the gSPFogPosition arithmetic to recover near/far, then
 * ask the current projection which eye distance produces a given z_ndc:
 *   z_ndc(e) = (a2*(-e) + b2) / (a3*(-e) + b3)   for eye distance e > 0
 * taken straight from P_matrix, so it holds for whatever projection is loaded
 * rather than assuming a particular guPerspective form.
 *
 * THE APPROXIMATION, stated plainly because it is the one thing here that is
 * not a faithful port: the N64 ramps linearly in z_ndc, i.e. in 1/e, while the
 * GE ramps linearly in e. Matching both endpoints would put the GE ramp far
 * below the N64's across the whole middle of the range -- with OoT's typical
 * fogNear of 990 the visible result would be almost no fog at all, which is the
 * bug this is meant to fix. So the ramp is fitted at the 0% and 50% points
 * instead: start is the exact distance where the N64 fog begins, and end is
 * placed so the GE reaches 50% where the N64 does. Perceptually the half-way
 * point is what reads as "how foggy is it", and the endpoints clamp anyway.
 *
 * A second pass per fogged triangle would be exact (that is what Daedalus does
 * on this hardware, reference/daedalus RendererPSP::RenderFog, because its
 * vertices reach the GE already projected). It also doubles the triangle count
 * on exactly the geometry that is already the heaviest. Start with the free
 * one; gPspFogMode makes the comparison possible without a rebuild.
 */

/* 0 = off (the behaviour up to now), 1 = GE hardware fog. Runtime-switchable
 * from the hack menu, because this changes the look of every outdoor scene. */
int gPspFogMode = 1;

/* Draws that asked for fog, and draws where the conversion could not produce a
 * usable range (a degenerate projection, or fog_mul == 0). The second being
 * non-zero means the maths below, not the GE, is what to look at. */
uint32_t gPspFogDraws;
uint32_t gPspFogBadRange;

static void psp_fog_disable(void) {
    if (rendering_state.fog_enabled != 0) {
        gfx_flush();
        gfx_scegu_set_fog(0, 0.0f, 0.0f, 0);
        rendering_state.fog_enabled = 0;
    }
}

static void psp_fog_set(float start, float end, unsigned int color) {
    if (rendering_state.fog_enabled != 1 || start != rendering_state.fog_start ||
        end != rendering_state.fog_end || color != rendering_state.fog_color) {
        gfx_flush();
        gfx_scegu_set_fog(1, start, end, color);
        rendering_state.fog_enabled = 1;
        rendering_state.fog_start = start;
        rendering_state.fog_end = end;
        rendering_state.fog_color = color;
    }
}

static void psp_fog_apply(void) {
    if (!gPspFogMode || (rsp.geometry_mode & G_FOG) != G_FOG) {
        psp_fog_disable();
        return;
    }

    /* The GE takes 0x00BBGGRR and ignores alpha. */
    const unsigned int color = ((unsigned int)rdp.fog_color.b << 16) |
                               ((unsigned int)rdp.fog_color.g << 8) |
                               ((unsigned int)rdp.fog_color.r);

    /* Gfx_SetFog's three special cases never go through gSPFogPosition, so they
     * arrive here with a zero multiplier and have to be read from the offset:
     * (0, 0) is "no fog at all" and (0, 255) is "everything fully fogged" (see
     * src/code/z_rcp.c). Treating both as "no fog" -- which a bare
     * `fog_mul == 0` test does -- would silently drop the whiteout case. */
    if (rsp.fog_mul == 0) {
        if (rsp.fog_offset >= 255) {
            ++gPspFogDraws;
            /* Everything at or beyond `end` is fully fogged, so a range that
             * closes immediately in front of the camera fogs the whole scene.
             * Not 0/0: sceGuFog divides by (far - near). */
            psp_fog_set(0.0f, 0.001f, color);
        } else {
            psp_fog_disable();
        }
        return;
    }

    /* The four projection terms the conversion needs. P_matrix is stored the
     * way sceGuSetMatrix and the VFPU transform above both read it, so P[i][j]
     * is row i of the N64's row-vector matrix and column i of the GE's: with
     * v = (0,0,z,1), clip_z = z*P[2][2] + P[3][2] and clip_w = z*P[2][3] +
     * P[3][3]. No particular guPerspective form is assumed. */
    const float a2 = rsp.P_matrix[2][2], b2 = rsp.P_matrix[3][2];
    const float a3 = rsp.P_matrix[2][3], b3 = rsp.P_matrix[3][3];

    /* This runs per TRIANGLE, and the answer changes per material at most --
     * the fog factors come from a display-list command and the projection from
     * a matrix load. Two divisions per triangle is real money in a frame budget
     * measured at 7.2 ms, so memoise on the six inputs. */
    static int16_t memo_mul, memo_off;
    static float memo_a2, memo_b2, memo_a3, memo_b3;
    static float memo_start, memo_end;
    static int memo_valid;

    float start, end;

    if (memo_valid && memo_mul == rsp.fog_mul && memo_off == rsp.fog_offset &&
        memo_a2 == a2 && memo_b2 == b2 && memo_a3 == a3 && memo_b3 == b3) {
        start = memo_start;
        end = memo_end;
    } else {
        /* Recover the 0..1000 near/far the display list asked for. */
        const float span = 128000.0f / (float)rsp.fog_mul;      /* far - near */
        const float n64_near = 500.0f - (float)rsp.fog_offset * 500.0f / (float)rsp.fog_mul;
        const float n64_half = n64_near + span * 0.5f;

        float e[2];
        for (int i = 0; i < 2; i++) {
            const float t = ((i == 0 ? n64_near : n64_half) / 500.0f) - 1.0f; /* target z_ndc */
            /* (a2*z + b2) = t*(a3*z + b3), with z = -e */
            const float denom = a2 - t * a3;
            if (denom == 0.0f) {
                ++gPspFogBadRange;
                return;
            }
            e[i] = -((t * b3 - b2) / denom);
        }

        start = e[0];
        end = start + 2.0f * (e[1] - start);
        if (!(end > start) || !(start >= 0.0f)) {
            ++gPspFogBadRange;
            return;
        }

        memo_mul = rsp.fog_mul;
        memo_off = rsp.fog_offset;
        memo_a2 = a2; memo_b2 = b2; memo_a3 = a3; memo_b3 = b3;
        memo_start = start;
        memo_end = end;
        memo_valid = 1;
    }

    ++gPspFogDraws;
    /* sceGuFog's own contract (pspsdk, sceGuFog.c): near >= 0, far > near,
     * far <= 65535. The first two are checked above; clamp the third rather
     * than hand the GE a value its register cannot hold. */
    psp_fog_set(start, end > 65535.0f ? 65535.0f : end, color);
}

/* Dimensions of the texture a tile actually had UPLOADED, which is what its
 * texture coordinates must be normalised by. Derived from the load's line size
 * and byte count -- the same arithmetic every import_texture_* uses to decide
 * what it hands the GE, so the two cannot disagree.
 *
 * Deliberately NOT the tile rectangle ((lrs - uls + 4) / 4): those are the same
 * number only when the tile covers the whole loaded image, and dividing by the
 * rectangle scaled every coordinate by their ratio. Ship of Harkinian's Fast3D
 * keeps both and uses the tile rectangle solely to decide whether G_TX_CLAMP
 * needs emulating; this port inherited sm64-port's conflation of the two. */
static void gfx_tile_dimensions(int tile, uint32_t *out_w, uint32_t *out_h) {
    uint32_t line_size = rdp.texture_tile[tile].line_size_bytes;
    uint32_t height;

    if (line_size == 0) {
        line_size = 1;
    }
    height = LOADED_TEX(tile).size_bytes / line_size;

    switch (rdp.texture_tile[tile].siz) {
        case G_IM_SIZ_4b:
            line_size <<= 1; /* two texels per byte */
            break;
        case G_IM_SIZ_8b:
            break;
        case G_IM_SIZ_16b:
            line_size /= 2;
            break;
        case G_IM_SIZ_32b:
            line_size /= 2;
            height /= 2;
            break;
    }

    /* Never hand a zero to the callers' divisions. */
    *out_w = (line_size != 0) ? line_size : 1;
    *out_h = (height != 0) ? height : 1;
}

#if TARGET_PSP
/* Put the texture pipeline into the state the terrain LERP's next pass needs,
 * by ASSERTING it outright rather than by nudging the change-detection that
 * normally maintains it.
 *
 * Called on both edges of the second pass: entering, tile 1 must win the
 * single-TMU bind; leaving, tile 0 must win it again. Neither transition is
 * something the ordinary paths can notice, and all three of them are
 * event-driven:
 *
 *   - the import loop only rebinds when rdp.textures_changed[i] is set, and a
 *     repeat of the same triangle changes nothing;
 *   - gfx_scegu_select_texture only rebinds when tmu_state[tile].tex differs,
 *     so from the SECOND lerp triangle onward it short-circuits and the GE
 *     keeps the previous pass's sampler modes;
 *   - the loop only re-applies filter/clamp when they differ from what the
 *     cached texture node claims, and here they never do.
 *
 * The middle one is what made the second pass invisible: with tile 0's
 * CLAMP/CLAMP left in the GE, the detail tile -- which has to repeat several
 * times across the surface -- came out as a single stretched band, i.e. no
 * detail. This is the same trap the two earlier attempts fell into: never take
 * the state a render pass depends on from a path that only runs on a change. */
/* Old behaviour: reach the bind by forcing the whole import loop to run again.
 * Kept as a runtime switch rather than deleted, so both halves of the A/B come
 * from ONE build -- the rule this port adopted after two screenshots taken
 * under uncertain build states produced a wrong conclusion. Default off. */
int gPspLerp2ForceReimport = 0;

static void gfx_lerp2_assert_texture_state(void) {
    const int tile = gPspLerp2SecondPass ? 1 : 0;
    const bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;

    /* Ask for the BIND, not for a re-import.
     *
     * This used to set rdp.textures_changed[0] and [1], purely so that the
     * import loop in gfx_sp_tri1 would run and reach its select_texture call.
     * That works, but textures_changed is the display list's flag for "a new
     * texture was loaded", and setting it drags import_texture along with the
     * bind. On the ONE frame per room where the cache is cold, that turns every
     * LERP triangle into a second full cache lookup for BOTH tiles.
     *
     * Measured on hardware, same room, same first frame (2026-08-28):
     *
     *   second pass on   79 texture imports, 60 of them the skybox's -> broken
     *   second pass off  50 texture imports, 31 of them the skybox's -> clean
     *   frame after      0 imports, second pass still running        -> clean
     *
     * So the fault needs the second pass AND an upload, and the second pass's
     * own contribution is that it roughly DOUBLES the uploads on precisely the
     * frame that comes out wrong. Neither half is guilty alone; this is where
     * they meet.
     *
     * gfx_scegu_invalidate_texture_binding() below already guarantees that
     * select_texture will not short-circuit, so the flags were never what made
     * the bind happen -- they only added the imports. Bind directly instead.
     *
     * (A different mechanism was suspected first and is now ruled out by
     * measurement: that the upload's own texman_bind_tex left the GE bound to
     * tile 0 while the second pass wanted tile 1. bindDesync2nd measured 0 on
     * the corrupted frame, i.e. the binding never desyncs inside the second
     * pass. Do not re-chase it.) */
    if (gPspLerp2ForceReimport) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    /* Force the bind itself to happen: without this, select_texture's own
     * "same id, nothing to do" test skips it and the wrong tile stays bound. */
    gfx_scegu_invalidate_texture_binding();
    if (!gPspLerp2ForceReimport && rendering_state.textures[tile] != NULL) {
        gfx_rapi->select_texture(tile, rendering_state.textures[tile]->texture_id);
    }

    gfx_rapi->set_sampler_parameters(tile, linear_filter,
                                     rdp.texture_tile[tile].cms,
                                     rdp.texture_tile[tile].cmt);
    /* Keep gfx_pc's per-texture belief in step with what was just written to
     * the GE, so it does not later skip a re-apply it actually owes. */
    if (rendering_state.textures[tile] != NULL) {
        rendering_state.textures[tile]->linear_filter = linear_filter;
        rendering_state.textures[tile]->cms = rdp.texture_tile[tile].cms;
        rendering_state.textures[tile]->cmt = rdp.texture_tile[tile].cmt;
    }
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
#if TARGET_PSP
    if (gPspSkyTriMark) {
        gPspSkyTri[0]++;
    }
#endif
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};

    GFXSTAT_INC(tri_calls);
#if TARGET_PSP
    if (sPspMtxCurSlot < PSP_MTX_MV_SLOTS) {
        ++gPspGfxMtx.mv_tris[sPspMtxCurSlot];
    }
    /* THE test for "loaded under one matrix, drawn under another". */
    {
        int k;
        int stale = 0;
        for (k = 0; k < 3; k++) {
            if (v_arr[k]->mtx_slot_at_load != sPspMtxCurSlot) {
                stale = 1;
                if (gPspGfxStats.vtx_stale_examples < 8) {
                    gPspGfxStats.vtx_stale_slot[gPspGfxStats.vtx_stale_examples] =
                        (v_arr[k]->mtx_slot_at_load << 16) | (sPspMtxCurSlot & 0xffff);
                    gPspGfxStats.vtx_stale_examples++;
                }
            }
        }
        if (stale) {
            GFXSTAT_INC(tri_stale_mtx);
        }
    }
#endif

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        GFXSTAT_INC(tri_rej_clip);
#if TARGET_PSP
        if (sPspMtxCurSlot < PSP_MTX_MV_SLOTS) {
            ++gPspGfxMtx.mv_rej[sPspMtxCurSlot];
        }
        if (!gPspGfxMtx.reject_captured) {
            gPspGfxMtx.reject_captured = 1;
            memcpy(gPspGfxMtx.mp_at_reject, rsp.MP_matrix,
                   sizeof(gPspGfxMtx.mp_at_reject));
        }
#endif
        return;
    }
#if TARGET_PSP
    extern int gDebugDisableCull;
#endif
    if ((rsp.geometry_mode & G_CULL_BOTH) != 0
#if TARGET_PSP
        && !gDebugDisableCull
#endif
    ) {
        float dx1 = v1->_x / (v1->_w) - v2->_x / (v2->_w);
        float dy1 = v1->_y / (v1->_w) - v2->_y / (v2->_w);
        float dx2 = v3->_x / (v3->_w) - v2->_x / (v2->_w);
        float dy2 = v3->_y / (v3->_w) - v2->_y / (v2->_w);
        float cross = dx1 * dy2 - dy1 * dx2;
        
        if ((v1->_w < 0) ^ (v2->_w < 0) ^ (v3->_w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }

        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) { GFXSTAT_INC(tri_rej_cull); return; }
                break;
            case G_CULL_BACK:
                if (cross >= 0) { GFXSTAT_INC(tri_rej_cull); return; }
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                GFXSTAT_INC(tri_rej_cull);
                return;
        }
    }

    /* Setup to clip but if we dont, we preload correct values and fix up pointers; */
    struct LoadedVertex **clipped_vertices = v_arr;
    size_t clipped_vertices_num = 3;
    /* 24, not 18. Six planes can each add one vertex, so a clipped triangle
     * carries up to 9, and the fan below emits (9 - 2) * 3 = 21 vertices.
     * The inherited 18 was a stack overwrite waiting for a polygon that hit
     * every plane -- which is exactly what the skybox, surrounding the eye,
     * finally supplies. */
    struct LoadedVertex _clipped_vertices[24];
    struct LoadedVertex *ptr_clipped_vertices[24];

#if TARGET_PSP
    extern int gDebugSkyClipMode;
    const int sky_clip = gPspSkyTriMark ? gDebugSkyClipMode : 0;
#else
    const int sky_clip = 0;
#endif

    if (sky_clip != 2 &&
        ((v1->clip_rej || v2->clip_rej || v3->clip_rej) & CLIP_TEST_FLAGS)) {
        gfx_clip_single_vert(_clipped_vertices, &clipped_vertices_num, v_arr, sky_clip == 1);

        if(!clipped_vertices_num){
            /* No idea if this is possible */
            return;
        }
        size_t i;
        for(i = 0;i < clipped_vertices_num;i++){
            ptr_clipped_vertices[i] = &_clipped_vertices[i];
        }
        clipped_vertices = ptr_clipped_vertices;
    }

    psp_fog_apply();

    bool depth_test = psp_depth_test_enabled();
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    uint32_t cc_id = rdp.combine_mode;
    
    bool use_alpha = gfx_use_alpha_for(rdp.other_mode_l);
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    
    if (texture_edge) {
        use_alpha = true;
    }
    
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog && gDebugFogCombinerBit) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    
    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }
    
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
        /* Applying a shader re-issues sceGuTexFunc, so the override below has
         * to be re-decided rather than assumed still in force. Same for the
         * LERP's tex-env colour: other paths (the boot logo, the 2D blit)
         * write sceGuTexEnvColor directly, so a remembered value cannot be
         * trusted across a shader change. */
        rendering_state.two_texture_tint = -1;
        rendering_state.lerp_prim_color = 0xFFFFFFFFu ^ gRdpPrimColorPacked;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    /* A two-texture combine renders as REPLACE (texel only) or MODULATE
     * (texel * vertex colour) depending on whether its SECOND cycle multiplies
     * the result by a colour register -- RDP state that is not part of cc_id,
     * so the renderer's per-shader texenv cache cannot decide it alone. See
     * gfx_scegu_set_two_texture_tint and texenv_set_texture_texture.
     *
     * OoT's skybox (SETUPDL_40, cycle 2 = passthrough) needs REPLACE: its only
     * colour register is cycle 1's interpolation factor, PRIM = RGB(0,0,0),
     * which under MODULATE blacks out the entire market. The Chamber of the
     * Sages waterfalls (cycle 2 = COMBINED * SHADE) need MODULATE or they lose
     * the blue and render as white pillars. */
    if (used_textures[0] && used_textures[1]) {
        const int tint = (rdp.combine_cyc2_tint != CC_0 &&
                          (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) ? 1 : 0;

        if (tint != rendering_state.two_texture_tint) {
            gfx_flush();
            gfx_scegu_set_two_texture_tint(tint);
            rendering_state.two_texture_tint = tint;
        }
    }

    /* The PRIM/ENV LERP carries PRIM in the tex-env colour, and PRIM changes
     * per draw (each dust mote sets its own). Same shape as the tint above:
     * only when it actually changed, and behind a flush, since the buffered
     * triangles were built against the previous value. */
    if (gfx_scegu_shader_is_prim_env_lerp() &&
        gRdpPrimColorPacked != rendering_state.lerp_prim_color) {
        gfx_flush();
        gfx_scegu_set_lerp_prim_color(gRdpPrimColorPacked);
        rendering_state.lerp_prim_color = gRdpPrimColorPacked;
    }

    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
                /* A cache MISS uploads, and the upload binds whatever it just
                 * decoded -- for tile 1 that means tile 1's texture wins the
                 * single GE texture unit, the opposite of the TEXEL0-wins rule
                 * gfx_scegu_select_texture applies on every other frame (see
                 * its comment and the Chamber of the Sages water it cites).
                 * Ask for the binding explicitly, so a miss ends up in the
                 * same state a hit would instead of in whatever order the
                 * uploads happened to finish. */
                if (rendering_state.textures[i] != NULL) {
                    gfx_rapi->select_texture(i, rendering_state.textures[i]->texture_id);
                }
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            if (linear_filter != rendering_state.textures[i]->linear_filter || rdp.texture_tile[i].cms != rendering_state.textures[i]->cms || rdp.texture_tile[i].cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile[i].cms, rdp.texture_tile[i].cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile[i].cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile[i].cmt;
            }
        }
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    if (use_texture) { GFXSTAT_INC(tex_used); } else { GFXSTAT_INC(tex_unused); }
    if (use_texture && !rsp.texture_on) { GFXSTAT_INC(tex_off_draws); }
    if (use_texture && rsp.texture_scaling_factor.s == 0) { GFXSTAT_INC(tex_sc0_draws); }

    /* Texture coordinates are normalised by the size of the texture that was
     * actually UPLOADED, which is derived from the load's line size and byte
     * count -- exactly the same arithmetic every import_texture_* above uses to
     * decide the dimensions it hands the GE.
     *
     * This used to divide by the TILE RECTANGLE instead
     * ((lrs - uls + 4) / 4). Those two are the same number only when the tile
     * covers the whole loaded image; when they differ, every texture
     * coordinate is scaled by their ratio and the material comes out stretched
     * or squashed. Ship of Harkinian's Fast3D (reference/shipwright-vita,
     * gfx_pc.cpp ~1400) keeps both quantities and is explicit about the split:
     * the loaded size normalises the coordinates, while the tile rectangle is
     * used ONLY to decide whether G_TX_CLAMP has to be emulated. Our copy comes
     * from sm64-port, where SM64's own materials happen to make the two agree
     * often enough for the difference never to have shown up.
     *
     * NOTE the 2D path (gfx_sp_tri1_2d) still measures the tile rectangle. It
     * is left alone deliberately: texture rectangles address texels within the
     * tile directly, and that path is currently correct for the HUD and the
     * pre-rendered backgrounds. */
    /* Which tile's mapping the single set of vertex texture coordinates
     * describes. Zero except under the prefer-TEXEL1 diagnostic or the terrain
     * LERP's own second pass (gPspLerp2SecondPass, set just below this
     * function's own vertex loop) -- both force tile 1, and for the same
     * reason: binding tile 1's TEXELS while still measuring tile 0's TILE made
     * an earlier experiment meaningless, since it drew TEXEL1's content at
     * TEXEL0's scale and the very thing being tested could not show up.
     *
     * That single UV set is also why this port cannot draw both halves of a
     * two-texture material in ONE draw call: the two tiles have independent
     * origins, scales and clamp modes. Sending the triangle through twice,
     * once per tile, sidesteps that instead of growing a second UV pair. */
    const int uv_tile = ((gPspGfxHackPreferTexel1 || gPspLerp2SecondPass) &&
                          used_textures[0] && used_textures[1]) ? 1 : 0;

    /* Is this the terrain LERP, (TEXEL1 - TEXEL0) * factor + TEXEL0? Matched on
     * the combine's own operands rather than on a shader id, so it covers every
     * material built this way rather than one scene's.
     *
     * Only ENV and PRIM are accepted as the factor: both are constant across the
     * draw, which is what lets the second pass express the mix with fixed GU
     * blend factors. A per-vertex factor would need per-vertex alpha instead and
     * is left for when a material that uses one actually turns up.
     *
     * Skipped entirely while gPspLerp2SecondPass is set: that is THIS triangle,
     * on its way through a second time under the tile-1 override, and the
     * combine id is unchanged, so without this guard it would qualify again and
     * recurse forever. */
    bool lerp2 = false;
    uint8_t lerp2_mix = 0;
    if (!gPspLerp2SecondPass && used_textures[0] && used_textures[1] && gPspGfxLerp2Enable) {
        const uint8_t a = (cc_id >> 0) & 7;
        const uint8_t b = (cc_id >> 3) & 7;
        const uint8_t d = (cc_id >> 9) & 7;

        if (a == CC_TEXEL1 && b == CC_TEXEL0 && d == CC_TEXEL0) {
            /* The mix factor comes from the RAW operand, not from cc_id.
             *
             * This used to test cc_id's c field for CC_ENV/CC_PRIM and then
             * use that register's RGB. But color_comb_component() maps BOTH
             * G_CCMUX_ENVIRONMENT and G_CCMUX_ENV_ALPHA onto CC_ENV, and OoT's
             * ground combine is the ENV_ALPHA one -- so the blend fraction was
             * being taken from env_color.rgb, which is the scene's environment
             * TINT. That tint tracks the lighting, so the detail layer faded in
             * and out with where the player stood: bright spots happened to sit
             * near 128 and looked right, shaded spots drove the mix toward zero
             * and the ground went flat. The alpha channel, which is what the
             * RDP actually multiplies by here, does not move with lighting.
             *
             * Ship of Harkinian's PSP-side counterpart (reference/oot-psp-z2442,
             * gfx_fast3d.c:5062) accepts exactly G_CCMUX_ENV_ALPHA and
             * G_CCMUX_PRIM_LOD_FRAC for this shape and feeds rdp.env_color.a or
             * the LOD fraction. */
            bool have_mix = true;

            switch (rdp.combine_c0_raw) {
                case G_CCMUX_ENV_ALPHA:
                    lerp2_mix = rdp.env_color.a;
                    break;
                case G_CCMUX_PRIM_LOD_FRAC:
                    lerp2_mix = rdp.prim_lod_frac;
                    break;
                case G_CCMUX_PRIMITIVE_ALPHA:
                    lerp2_mix = rdp.prim_color.a;
                    break;
                /* Genuine RGB operands stay on the old reading: the factor is a
                 * colour, and with one scalar to spend the luminance-free green
                 * channel is the closest single number to it. */
                case G_CCMUX_ENVIRONMENT:
                    lerp2_mix = rdp.env_color.g;
                    break;
                case G_CCMUX_PRIMITIVE:
                    lerp2_mix = rdp.prim_color.g;
                    break;
                default:
                    have_mix = false;
                    break;
            }

            if (have_mix) {
                lerp2 = true;
                gPspLerp2Detected++;
                gPspLerp2LastMix = lerp2_mix;
                gPspLerp2LastMixSrc = rdp.combine_c0_raw;
            }
        }
    }

    uint32_t tex_width;
    uint32_t tex_height;
    gfx_tile_dimensions(uv_tile, &tex_width, &tex_height);

    /* A mirrored axis was uploaded as [image | reflection], so one tile now
     * spans half the texture. See upload_texture_mirrored. */
    if (use_texture && rendering_state.textures[0] != NULL) {
        if (rendering_state.textures[0]->mirror_s) {
            tex_width *= 2;
        }
        if (rendering_state.textures[0]->mirror_t) {
            tex_height *= 2;
        }
    }

#if TARGET_PSP
    /* Biggest-triangle probe. The outdoor ground is reliably the largest
     * textured surface in a scene by a wide margin, so "the triangle with the
     * greatest area this frame" is a dependable handle on it without needing
     * any way to point at a specific draw.
     *
     * Area is computed from the vertex positions as they are here, which on
     * this port is object space (the GE transforms at draw time -- see
     * mtx_slot_at_load). That is fine for ranking: room geometry shares one
     * matrix, so relative sizes within a room are meaningful even though the
     * absolute number is not in world units. Cross product magnitude squared,
     * to keep a sqrt out of the per-triangle path. */
    bool psp_is_probe_tri = false;
    bool psp_l2_capture = false;
    if (use_texture) {
        const float ax = v2->x - v1->x, ay = v2->y - v1->y, az = v2->z - v1->z;
        const float bx = v3->x - v1->x, by = v3->y - v1->y, bz = v3->z - v1->z;
        const float cx = ay * bz - az * by;
        const float cy = az * bx - ax * bz;
        const float cz = ax * by - ay * bx;
        const float area2 = cx * cx + cy * cy + cz * cz;

        /* Only HORIZONTAL surfaces qualify. Ranking by area alone kept
         * selecting the Kokiri Forest wall -- a dumped texture that turned out
         * to be tree trunks, which is exactly what that wall should be, so two
         * rounds of measurement described a surface nobody was asking about.
         * The ground is the thing with a mostly-vertical normal; the cross
         * product needed for the area already carries it, so this costs one
         * comparison. */
        const float upness = cy * cy;
        const bool horizontal = upness > (cx * cx + cz * cz);

        /* The measurement is only worth anything if it is known WHICH surface
         * it describes -- "the biggest triangle is the ground" is an assumption,
         * and reading tile numbers off the wrong surface sends the search into
         * the wrong part of the renderer. This paints the measured triangle so
         * the assumption can be checked instead of trusted. */
        psp_is_probe_tri = gPspGfxHackHighlightBigTri && horizontal &&
                           gPspBigTriArea2Prev > 0.0f && area2 >= gPspBigTriArea2Prev * 0.999f;

        /* '>=' not '>': on the second pass gPspL2Area2 already holds this very
         * triangle's area from the first, so a strict test would never match
         * the half we are trying to observe. */
        psp_l2_capture = (lerp2 || gPspLerp2SecondPass) && area2 >= gPspL2Area2;

        if (lerp2) {
            uint32_t w1, h1;
            gfx_tile_dimensions(1, &w1, &h1);
            if (LOADED_TEX(1).size_bytes == 0 || LOADED_TEX(1).size_bytes > 4096) {
                gPspL2BadTile1++;
            }
            if (area2 > gPspL2Area2) {
                uint32_t w0, h0;
                gfx_tile_dimensions(0, &w0, &h0);
                gPspL2Area2 = area2;
                gPspL2TexW0 = w0; gPspL2TexH0 = h0;
                gPspL2TexW1 = w1; gPspL2TexH1 = h1;
                gPspL2Line0 = rdp.texture_tile[0].line_size_bytes;
                gPspL2Line1 = rdp.texture_tile[1].line_size_bytes;
                gPspL2Bytes0 = LOADED_TEX(0).size_bytes;
                gPspL2Bytes1 = LOADED_TEX(1).size_bytes;
                gPspL2Cms0 = rdp.texture_tile[0].cms; gPspL2Cmt0 = rdp.texture_tile[0].cmt;
                gPspL2Cms1 = rdp.texture_tile[1].cms; gPspL2Cmt1 = rdp.texture_tile[1].cmt;
                gPspL2Shifts0 = rdp.texture_tile[0].shifts; gPspL2Shiftt0 = rdp.texture_tile[0].shiftt;
                gPspL2Shifts1 = rdp.texture_tile[1].shifts; gPspL2Shiftt1 = rdp.texture_tile[1].shiftt;
                gPspL2Slot0 = rdp.texture_tile[0].tmem_slot;
                gPspL2Slot1 = rdp.texture_tile[1].tmem_slot;
                gPspL2Addr0 = LOADED_TEX(0).addr;
                gPspL2Addr1 = LOADED_TEX(1).addr;
                gPspL2SameAddr = (LOADED_TEX(0).addr == LOADED_TEX(1).addr);
                gPspL2CcId = cc_id;
            }
        }

        if (horizontal && area2 > gPspBigTriArea2) {
            gPspBigTriArea2 = area2;
            gPspBigTriTexW = tex_width;
            gPspBigTriTexH = tex_height;
            gPspBigTriUls = rdp.texture_tile[0].uls;
            gPspBigTriUlt = rdp.texture_tile[0].ult;
            gPspBigTriLrs = rdp.texture_tile[0].lrs;
            gPspBigTriLrt = rdp.texture_tile[0].lrt;
            gPspBigTriShiftS = rdp.texture_tile[0].shifts;
            gPspBigTriShiftT = rdp.texture_tile[0].shiftt;
            gPspBigTriCms = rdp.texture_tile[0].cms;
            gPspBigTriCmt = rdp.texture_tile[0].cmt;
            gPspBigTriCms1 = rdp.texture_tile[1].cms;
            gPspBigTriCmt1 = rdp.texture_tile[1].cmt;
            gPspBigTriShiftS1 = rdp.texture_tile[1].shifts;
            gPspBigTriShiftT1 = rdp.texture_tile[1].shiftt;
            gfx_tile_dimensions(1, &gPspBigTriTexW1, &gPspBigTriTexH1);
            /* Does this material want a SECOND texture? OoT terrain often
             * blends two, and this port has one usable TMU, so TEXEL1 is
             * dropped -- which looks exactly like "the detail is missing".
             * Recording the combine id as well makes the material
             * identifiable against the gsDPSetCombineMode macros. */
            gPspBigTriTex01 = (used_textures[0] ? 1 : 0) | (used_textures[1] ? 2 : 0);
            gPspBigTriCcId = cc_id;
            gPspBigTriTexAddr = LOADED_TEX(0).addr;
            gPspBigTriTexFmt = rdp.texture_tile[0].fmt;
            gPspBigTriTexSiz = rdp.texture_tile[0].siz;
            gPspBigTriTexLine = rdp.texture_tile[0].line_size_bytes;
            gPspBigTriTexBytes = LOADED_TEX(0).size_bytes;
            gPspBigTriPalAddr = rdp.palette;
            /* Raw vertex texture coordinates, S10.5 texels as the N64 stores
             * them. The span across the triangle divided by tex_width is how
             * many times the tile SHOULD repeat over it -- which is the number
             * the stretched-ground symptom is really about. */
            gPspBigTriU0 = (s32)v1->u;
            gPspBigTriV0 = (s32)v1->v;
            gPspBigTriU1 = (s32)v2->u;
            gPspBigTriV1 = (s32)v2->v;
            gPspBigTriU2 = (s32)v3->u;
            gPspBigTriV2 = (s32)v3->v;
        }
    }
#endif

    /* Make room for the *whole* primitive before writing any of it. Clipping
     * expands one triangle into up to 6 (_clipped_vertices[18]), so checking
     * only after the fact (see the flush at the end of this function) still
     * lets a single call run off the end of buf_vbo[MAX_BUFFERED * 3]. */
    if (buf_vbo_num_tris + (clipped_vertices_num / 3) > MAX_BUFFERED) {
        gfx_flush();
    }

    size_t i;
    for (i = 0; i < clipped_vertices_num; i++) {
        buf_vbo[buf_num_vert].x = clipped_vertices[i]->x;
        buf_vbo[buf_num_vert].y = clipped_vertices[i]->y;
        buf_vbo[buf_num_vert].z = clipped_vertices[i]->z;
        
        if (use_texture) {
            float u = (tex_shift_coord(clipped_vertices[i]->u, rdp.texture_tile[uv_tile].shifts) -
                       rdp.texture_tile[uv_tile].uls * 8) / 32.0f;
            float v = (tex_shift_coord(clipped_vertices[i]->v, rdp.texture_tile[uv_tile].shiftt) -
                       rdp.texture_tile[uv_tile].ult * 8) / 32.0f;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            buf_vbo[buf_num_vert].u = u / tex_width;
            buf_vbo[buf_num_vert].v = v / tex_height;
#if TARGET_PSP
            if (psp_l2_capture && i == 0) {
                const int pass = gPspLerp2SecondPass ? 1 : 0;
                gPspL2UvU[pass] = u / tex_width;
                gPspL2UvV[pass] = v / tex_height;
                gPspL2UvW[pass] = tex_width;
                gPspL2UvH[pass] = tex_height;
                gPspL2UvTile[pass] = uv_tile;
                gPspL2UvShift[pass] = rdp.texture_tile[uv_tile].shifts;
                gPspL2DrawTex[pass] = gPspCurBoundTex;
            }
#endif
        } else {
            buf_vbo[buf_num_vert].u = 0;
            buf_vbo[buf_num_vert].v = 0;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = clipped_vertices[i].color.a / 255.0f; // fog factor (not alpha)
        }
        */
        struct RGBA white = (struct RGBA){0xff, 0xff, 0xff, 0xff};
        struct RGBA tmp = (struct RGBA){0x00, 0x00, 0x00, 0x00};
        /* comb->shader_input_mapping's FIRST index is [0] = RGB, [1] = ALPHA
         * (see gfx_dp_set_combine_mode: `rgb | (alpha << 12)`), so the k loop
         * below walks two independent channels and they need two independent
         * results.
         *
         * This used to be a single `color` that both iterations wrote, with one
         * memcpy of the whole RGBA after the loop -- which meant the ALPHA row
         * silently decided the RGB. For the room geometry that is fatal: the
         * walls' combine is (TEXEL0 - 0) * SHADE + 0 with a constant-1 alpha,
         * so k == 0 correctly picked CC_SHADE and then k == 1 hit `default:`
         * on the constant and overwrote it with white. Every intensity-only
         * texture (i4/i8/ia8 carry no colour of their own) therefore rendered
         * as flat greyscale no matter how correct the lighting was.
         *
         * sm64-port, which this is a fork of, does not have the bug: its k == 0
         * branch writes r/g/b and its k == 1 branch writes ONLY the alpha
         * component, into separate float slots (the original is still here,
         * commented out, a few lines below). The defect was introduced in
         * collapsing that into one packed PSP vertex colour. */
        struct RGBA *color = &white;       /* RGB   channel, k == 0 */
        struct RGBA *alpha_src = &white;   /* ALPHA channel, k == 1 */
        uint32_t probe_cc_input = 0; /* shade probe: 0 == the loop matched nothing */

        for (int j = 0; j < num_inputs; j++) {
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                struct RGBA **dst = (k == 0) ? &color : &alpha_src;

                if (k == 0) {
                    probe_cc_input = (uint32_t)comb->shader_input_mapping[k][j] + 1;
                }
                switch (comb->shader_input_mapping[k][j]) {
                    case CC_PRIM:
                        *dst = &rdp.prim_color;
                        break;
                    case CC_SHADE:
                        *dst = &clipped_vertices[i]->color;
                        break;
                    case CC_ENV:
                        *dst = &rdp.env_color;
                        break;
                    case CC_LOD:
                    {
                        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                        if (distance_frac < 0.0f) distance_frac = 0.0f;
                        if (distance_frac > 1.0f) distance_frac = 1.0f;
                        tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                        *dst = &tmp;
                        break;
                    }
                    default:
                        *dst = &white;
                        break;
                }
                /*@Note: should this be here ? */
                //memcpy(&buf_vbo[buf_num_vert].color, color, sizeof(struct RGBA));

                /*
                //Ignore for now
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = color->r / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->g / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->b / 255.0f;
                } else {
                    if (use_fog && color == &clipped_vertices[i]->color) {
                        // Shade alpha is 100% for fog
                        buf_vbo[buf_vbo_len++] = 1.0f;
                    } else {
                        buf_vbo[buf_vbo_len++] = color->a / 255.0f;
                    }
                }*/

            }
        }
        /* "Last matched input wins" is only right when there IS one input. A
         * combine of the form (X - 0) * Y + 0 where BOTH X and Y are colour
         * registers is a product of two of them, and picking one silently drops
         * the other.
         *
         * hakaana2's ceiling is exactly this: (SHADE - 0) * PRIMITIVE + 0,
         * untextured, with prim = 255,255,255. PRIM is the later input, so it
         * won and the surface rendered pure white -- the same shade loss as the
         * alpha-row clobber above, by a different route.
         *
         * Only the both-registers case is folded here. When one operand is a
         * texel the GE already multiplies the texture in for us, so leaving the
         * remaining register in the vertex colour is correct as it stands.
         * Operands are decoded straight out of cc_id the same way
         * gfx_generate_cc does it (3 bits each, a/b/c/d). */
        struct RGBA prod;
        if (num_inputs >= 2) {
            uint8_t cc_a = (comb->cc_id >> 0) & 7;
            uint8_t cc_b = (comb->cc_id >> 3) & 7;
            uint8_t cc_c = (comb->cc_id >> 6) & 7;
            uint8_t cc_d = (comb->cc_id >> 9) & 7;

            if (cc_b == CC_0 && cc_d == CC_0) {
                const struct RGBA *xa = cc_operand_color(cc_a, clipped_vertices[i]);
                const struct RGBA *xc = cc_operand_color(cc_c, clipped_vertices[i]);

                if (xa != NULL && xc != NULL) {
                    prod.r = (uint8_t)((xa->r * xc->r) / 255);
                    prod.g = (uint8_t)((xa->g * xc->g) / 255);
                    prod.b = (uint8_t)((xa->b * xc->b) / 255);
                    prod.a = color->a;
                    color = &prod;
                }
            }
        }

        /* Two-texture combines: the cycle-1 colour register is an
         * INTERPOLATION FACTOR, not a tint, and must not reach the vertex
         * colour.
         *
         * The shape is (A - B) * C + B with A/B the two texels -- OoT's
         * skybox (SETUPDL_40), the Chamber of the Sages waterfalls and every
         * scrolling lava/water surface are all built this way. C selects
         * BETWEEN the two texels; on a single TMU only one texel survives, so
         * multiplying it by C is simply wrong. gfx_generate_cc has no concept
         * of a factor and hands C over as the vertex colour, which is why this
         * used to be papered over by forcing GU_TFX_REPLACE for every
         * two-texture material (see texenv_set_texture_texture) -- correct for
         * the skybox, but it also threw away the SHADE that the SAGES
         * waterfalls get their blue from, leaving grey/white pillars.
         *
         * Neutralising the factor to white makes MODULATE behave exactly like
         * the old REPLACE wherever nothing else contributes, while letting a
         * genuine cycle-2 tint (applied just below) through. Restricted to
         * two-texture combines: for a single texture the register is a real
         * multiplier and the existing handling is right. */
        if (used_textures[0] && used_textures[1]) {
            const uint8_t cc_b = (comb->cc_id >> 3) & 7;
            const uint8_t cc_d = (comb->cc_id >> 9) & 7;

            if (cc_b != CC_0 && cc_b == cc_d) {
                color = &white;
            }
        }

        /* Cycle 2's tint (see combine_cycle2_tint). Folding it into the vertex
         * colour is exact for the shape we accept: the GE computes
         * texture * vertexColour, cycle 1 is texture * X, and cycle 2 is
         * REG * that -- so vertexColour = X * REG reproduces it. Only applies
         * in 2-cycle mode; in 1-cycle the second set of operands is unused
         * (the gsDPSetCombineMode macros just repeat cycle 1 there). */
        if (rdp.combine_cyc2_tint != CC_0 &&
            (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) {
            const struct RGBA *t = cc_operand_color(rdp.combine_cyc2_tint, clipped_vertices[i]);

            if (t != NULL) {
                prod.r = (uint8_t)((color->r * t->r) / 255);
                prod.g = (uint8_t)((color->g * t->g) / 255);
                prod.b = (uint8_t)((color->b * t->b) / 255);
                prod.a = color->a;
                color = &prod;
            }
        }

#if TARGET_PSP
        if (psp_is_probe_tri) {
            /* Magenta: nothing in OoT's palette is near it, so the highlighted
             * triangle cannot be mistaken for real scenery. */
            color = &(struct RGBA){0xFF, 0x00, 0xFF, 0xFF};
        }
#endif
        buf_vbo[buf_num_vert].color.r = color->r;
        buf_vbo[buf_num_vert].color.g = color->g;
        buf_vbo[buf_num_vert].color.b = color->b;
        /* Alpha comes from the ALPHA row, not from whatever supplied the RGB.
         * When that row is a constant (the common `(0-0)*0+1` case) nothing
         * matches and alpha_src stays white, i.e. fully opaque -- which is what
         * a constant 1 means. Note this also stops the vertex alpha picking up
         * gfx_sp_vertex's fog factor, which it stores in color.a when G_FOG is
         * set (the room's display lists do set it) and which is not an alpha at
         * all; that is the likely source of the walls' half-transparent look. */
        buf_vbo[buf_num_vert].color.a = alpha_src->a;

        /* Shade probe -- the other half of the cut, see PspGfxFrameStats.
         * Only textured, lit draws: that is the walls and the floor. */
        if (use_texture) {
            gPspGfxStats.vtx_color = ((uint32_t)color->r << 16) | ((uint32_t)color->g << 8) |
                                     ((uint32_t)color->b);
            gPspGfxStats.vtx_cc_input = probe_cc_input;
            gPspGfxStats.vtx_num_inputs = (uint32_t)num_inputs;
        }

        /*@Note: Blue Star color */
        if((rendering_state.shader_program->shader_id == 0x01200200)){
            memcpy(&buf_vbo[buf_num_vert].color, &clipped_vertices[0]->color, sizeof(struct RGBA));
            if(rdp.env_color.a != 255){
                buf_vbo[buf_num_vert].color.a = rdp.env_color.a;
            }
        }
        if((rendering_state.shader_program->shader_id == 0x01A00045)){
            color = &tmp;
        }
        buf_num_vert++;
        buf_vbo_len += sizeof(psp_fast_t);
    }
    buf_vbo_num_tris += clipped_vertices_num/3;
    GFXSTAT_ADD(tris_buffered, clipped_vertices_num / 3);
    /* MUST be >=, not ==. Clipping turns one input triangle into a fan of up to
     * ~7, so this counter grows in steps of 1..7 and can step straight *over*
     * MAX_BUFFERED (e.g. 1023 -> 1026) without ever being equal to it. When
     * that happens the flush below never runs and the loop above keeps writing
     * past the end of buf_vbo[MAX_BUFFERED * 3] into whatever static data
     * follows it -- a real out-of-bounds write, which matches this port's
     * long-standing "renders for a while, then geometry degrades and it
     * eventually dies on a wild jump" behaviour. Inherited as `==` from
     * sm64-port-psp (gfx_pc.c:1308 there); SM64 evidently never hit the skip,
     * OoT's much heavier, camera-inside-the-room clipping load does. */
    if (buf_vbo_num_tris >= MAX_BUFFERED) {
        gfx_flush();
    }

#if TARGET_PSP
    /* Terrain LERP second pass: send this SAME triangle through this SAME
     * function again, tile-1-preferred, blended over the first pass with fixed
     * GU factors ENV and (1 - ENV) -- the identity src*ENV + dst*(1-ENV) ==
     * (TEXEL1-TEXEL0)*ENV + TEXEL0 makes that an exact reproduction of the N64
     * combine, provided ENV is constant across the draw (checked above).
     *
     * This replaces an earlier attempt that hand-built a second vertex buffer
     * and a dedicated draw call: it ran (measured: draws > 0, no crashes) but
     * painted the wrong thing anyway, because keeping two buffers and two code
     * paths in lockstep is exactly the kind of duplication that silently drifts.
     * Reusing this function instead means the second pass literally cannot
     * disagree with the first about vertex positions, clipping, colour or
     * texture-coordinate math -- it IS the first pass's code, run once more
     * with one flag flipped. gPspLerp2SecondPass is that flag: it forces
     * uv_tile to 1 (above) and, in gfx_scegu_select_texture, forces TEXEL1 to
     * be the one that survives the single-TMU bind instead of TEXEL0.
     *
     * gfx_lerp2_assert_texture_state() handles both edges of that flip; see its
     * own comment for why the bind and the sampler modes have to be asserted
     * outright instead of left to the pipeline's change-detection. */
    if (lerp2 && !gPspLerp2SecondPass) {
        gfx_flush(); /* the first pass must be in the framebuffer as the LERP's destination */
        gfx_scegu_lerp2_blend_begin(lerp2_mix);
        gPspLerp2SecondPass = 1;
        gfx_lerp2_assert_texture_state();
        gfx_sp_tri1(vtx1_idx, vtx2_idx, vtx3_idx);
        gfx_flush();
        gPspLerp2SecondPass = 0;
        gfx_lerp2_assert_texture_state();
        gfx_scegu_lerp2_blend_end();
        gPspLerp2Draws++;
    }
#endif
}

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
static void gfx_sp_tri1_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, UNUSED uint8_t vtx3_idx) {
    struct VertexColor *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    struct VertexColor *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    struct VertexColor *v_arr[2] = {v1, v2};

    /* Never fog the 2D path: the HUD, the texture rectangles and the
     * pre-rendered backgrounds are screen-space quads whose eye-space Z means
     * nothing. Their vertices would land at whatever depth the fog range
     * happens to cover, and the HUD would fade with the scenery. */
    psp_fog_disable();

    bool depth_test = psp_depth_test_enabled();
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    uint32_t cc_id = rdp.combine_mode;
    
    bool use_alpha = gfx_use_alpha_for(rdp.other_mode_l);
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    
    if (texture_edge) {
        use_alpha = true;
    }
    
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog && gDebugFogCombinerBit) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    
    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }
    
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
        /* Applying a shader re-issues sceGuTexFunc, so the override below has
         * to be re-decided rather than assumed still in force. Same for the
         * LERP's tex-env colour: other paths (the boot logo, the 2D blit)
         * write sceGuTexEnvColor directly, so a remembered value cannot be
         * trusted across a shader change. */
        rendering_state.two_texture_tint = -1;
        rendering_state.lerp_prim_color = 0xFFFFFFFFu ^ gRdpPrimColorPacked;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    /* A two-texture combine renders as REPLACE (texel only) or MODULATE
     * (texel * vertex colour) depending on whether its SECOND cycle multiplies
     * the result by a colour register -- RDP state that is not part of cc_id,
     * so the renderer's per-shader texenv cache cannot decide it alone. See
     * gfx_scegu_set_two_texture_tint and texenv_set_texture_texture.
     *
     * OoT's skybox (SETUPDL_40, cycle 2 = passthrough) needs REPLACE: its only
     * colour register is cycle 1's interpolation factor, PRIM = RGB(0,0,0),
     * which under MODULATE blacks out the entire market. The Chamber of the
     * Sages waterfalls (cycle 2 = COMBINED * SHADE) need MODULATE or they lose
     * the blue and render as white pillars. */
    if (used_textures[0] && used_textures[1]) {
        const int tint = (rdp.combine_cyc2_tint != CC_0 &&
                          (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) ? 1 : 0;

        if (tint != rendering_state.two_texture_tint) {
            gfx_flush();
            gfx_scegu_set_two_texture_tint(tint);
            rendering_state.two_texture_tint = tint;
        }
    }

    /* The PRIM/ENV LERP carries PRIM in the tex-env colour, and PRIM changes
     * per draw (each dust mote sets its own). Same shape as the tint above:
     * only when it actually changed, and behind a flush, since the buffered
     * triangles were built against the previous value. */
    if (gfx_scegu_shader_is_prim_env_lerp() &&
        gRdpPrimColorPacked != rendering_state.lerp_prim_color) {
        gfx_flush();
        gfx_scegu_set_lerp_prim_color(gRdpPrimColorPacked);
        rendering_state.lerp_prim_color = gRdpPrimColorPacked;
    }
    
    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
                /* A cache MISS uploads, and the upload binds whatever it just
                 * decoded -- for tile 1 that means tile 1's texture wins the
                 * single GE texture unit, the opposite of the TEXEL0-wins rule
                 * gfx_scegu_select_texture applies on every other frame (see
                 * its comment and the Chamber of the Sages water it cites).
                 * Ask for the binding explicitly, so a miss ends up in the
                 * same state a hit would instead of in whatever order the
                 * uploads happened to finish. */
                if (rendering_state.textures[i] != NULL) {
                    gfx_rapi->select_texture(i, rendering_state.textures[i]->texture_id);
                }
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            if (linear_filter != rendering_state.textures[i]->linear_filter || rdp.texture_tile[i].cms != rendering_state.textures[i]->cms || rdp.texture_tile[i].cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile[i].cms, rdp.texture_tile[i].cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile[i].cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile[i].cmt;
            }
        }
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    uint32_t tex_width = (rdp.texture_tile[0].lrs - rdp.texture_tile[0].uls + 4) / 4;
    uint32_t tex_height = (rdp.texture_tile[0].lrt - rdp.texture_tile[0].ult + 4) / 4;

    /* A mirrored axis was uploaded as [image | reflection], so one tile now
     * spans half the texture. See upload_texture_mirrored. */
    if (use_texture && rendering_state.textures[0] != NULL) {
        if (rendering_state.textures[0]->mirror_s) {
            tex_width *= 2;
        }
        if (rendering_state.textures[0]->mirror_t) {
            tex_height *= 2;
        }
    }

    /* NON-POWER-OF-TWO CORRECTION -- the reason the boot logo read "NINTENDC"
     * with no "64".
     *
     * The GE only samples power-of-two textures, so gfx_scegu_upload_texture
     * RESAMPLES anything else up to the next power of two: the console logo's
     * 192x2 glyph strip is stretched to 256x2, content and all. The 3D path
     * survives that because it hands the GE NORMALISED coordinates (u / tex_width,
     * GU_TEXTURE_32BITF) -- 0..1 still spans the whole stretched image.
     *
     * This 2D path instead hands over RAW TEXELS (GU_TEXTURE_16BIT), computed
     * from the tile's original size. So it asked for texels 0..192 of an image
     * that had been stretched across 256, i.e. the leftmost 75% of the artwork
     * blown up to fill the rectangle -- which is exactly "NINTENDO" without the
     * "(R)64", with the final O clipped mid-glyph.
     *
     * Scaling by padded/original puts the request back on the stretched grid.
     * General, not logo-specific: it applies to every non-pow2 texture rectangle
     * in the game. */
    uint32_t tex_pad_w = psp_next_pow2(tex_width);
    uint32_t tex_pad_h = psp_next_pow2(tex_height);

    VertexColor tri_buf[2] = {{0}};
    int tri_num_vert = 0;
    
    for (int i = 0; i < 2; i++) {
        tri_buf[tri_num_vert].x = v_arr[i]->x;
        tri_buf[tri_num_vert].y = v_arr[i]->y;
        tri_buf[tri_num_vert].z = 0;
        
        if (use_texture) {
            int32_t u = (tex_shift_coord(v_arr[i]->u, rdp.texture_tile[0].shifts) -
                         rdp.texture_tile[0].uls * 8) / 32;
            int32_t v = (tex_shift_coord(v_arr[i]->v, rdp.texture_tile[0].shiftt) -
                         rdp.texture_tile[0].ult * 8) / 32;

            /* See the tex_pad_* note above. Done in 32-bit: 192 -> 256 already
             * exceeds what the intermediate would hold comfortably in a short
             * once multiplied. */
            if (tex_width != 0 && tex_pad_w != tex_width) {
                u = (int32_t)((int64_t)u * tex_pad_w / tex_width);
            }
            if (tex_height != 0 && tex_pad_h != tex_height) {
                v = (int32_t)((int64_t)v * tex_pad_h / tex_height);
            }
            /*
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            */
            tri_buf[tri_num_vert].u = u;
            tri_buf[tri_num_vert].v = v;
        } else {
            tri_buf[tri_num_vert].u = 0;
            tri_buf[tri_num_vert].v = 0;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            tri_buf[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            tri_buf[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }
        */
        struct RGBA white = (struct RGBA){0xff, 0xff, 0xff, 0xff};
        struct RGBA *color = &white;
        
        //const int hack = (num_inputs > 1) * ((int)used_textures[0]);
        for (int j = 0; j < num_inputs; j++) {
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                    case CC_PRIM:
                        color = &rdp.prim_color;
                        break;
                    case CC_SHADE:
                        color = &v_arr[i]->color;
                        break;
                    case CC_ENV:
                        color = &rdp.env_color;
                        break;
                    /*
                    case CC_LOD:
                    {
                        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                        if (distance_frac < 0.0f) distance_frac = 0.0f;
                        if (distance_frac > 1.0f) distance_frac = 1.0f;
                        tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                        color = &tmp;
                        break;
                    }*/
                    default:
                        color = &white;
                        break;
                }
                /*@Note: should this be here ? */
                //memcpy(&tri_buf[buf_num_vert].color, color, sizeof(struct RGBA));

                /*
                //Ignore for now
                if (k == 0) {
                    tri_buf[buf_vbo_len++] = color->r / 255.0f;
                    tri_buf[buf_vbo_len++] = color->g / 255.0f;
                    tri_buf[buf_vbo_len++] = color->b / 255.0f;
                } else {
                    if (use_fog && color == &v_arr[i]->color) {
                        // Shade alpha is 100% for fog
                        tri_buf[buf_vbo_len++] = 1.0f;
                    } else {
                        tri_buf[buf_vbo_len++] = color->a / 255.0f;
                    }
                }*/
            }
        }
        memcpy(&tri_buf[tri_num_vert].color, color, sizeof(struct RGBA));
        tri_num_vert++;
    }
    gfx_scegu_draw_triangles_2d((float*)&tri_buf[0],0,1);
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);
    
    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
            /* Measure BEFORE the clamp -- afterwards every frame reads 8 and
             * the question "did OoT ever ask for more than three?" is gone. */
            if (rsp.current_num_lights > gPspLightsMax) {
                gPspLightsMax = rsp.current_num_lights;
            }
            if (rsp.current_num_lights > 3) {
                gPspLightsOverOld++;
            }
            /* Clamp: the count comes straight off the command stream and
             * indexes current_lights[] / current_lights_coeffs[] unchecked.
             * See MAX_LIGHTS. */
            if (rsp.current_num_lights > MAX_LIGHTS + 1) {
                rsp.current_num_lights = MAX_LIGHTS + 1;
            } else if (rsp.current_num_lights < 1) {
                rsp.current_num_lights = 1;
            }
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
            /* gSPSegment(pkt, segment, base) (include/ultra64/gbi.h) emits
             * exactly this command with offset = segment*4, data = base --
             * on real N64 hardware the RSP's own segment table is what
             * matters for subsequent seg_addr() resolution in this same
             * display list, so update gSegments[] here rather than relying
             * on whatever a CPU-side C assignment (used elsewhere, e.g.
             * z_actor.c's gSegments[6] = ...) may or may not have set --
             * see seg_addr() above for why this matters (e.g. z_title.c's
             * console-logo asset segment, set only via gSPSegment). */
            gSegments[offset / 4] = data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    _UNUSED(level);
    _UNUSED(tile);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    rsp.texture_on = (on != 0);
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);

    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    _UNUSED(format);

    GFXSTAT_INC(settimg);
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
    /* The COMMAND carries width - 1 (gbi.h:3496, `_SHIFTL((width) - 1, 0, 12)`),
     * so the field has to be un-biased here. Storing the raw 255 for a
     * 256-texel-wide image made gfx_dp_load_tile read every source row one
     * byte early -- a 1-texel-per-row shear, i.e. ~45 degrees across a 32-row
     * tile, applied identically to all 32 tiles of a skybox face. That is what
     * the "tilted skybox" was: the Market panorama sheared, not rotated.
     *
     * Nothing else noticed because only G_LOADTILE reads this field, and until
     * the skybox nothing in the port used G_LOADTILE. */
    rdp.texture_to_load.width = width + 1;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    _UNUSED(maskt);
    _UNUSED(masks);

    GFXSTAT_INC(settile);
    if (tile < 2) {
        SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
        rdp.texture_tile[tile].fmt = fmt;
        rdp.texture_tile[tile].siz = siz;
        rdp.texture_tile[tile].cms = cms;
        rdp.texture_tile[tile].cmt = cmt;
        rdp.texture_tile[tile].line_size_bytes = line * 8;
        rdp.texture_tile[tile].tmem_slot = (tmem / 256) & 1;
        rdp.texture_tile[tile].shifts = shifts & 0xf;
        rdp.texture_tile[tile].shiftt = shiftt & 0xf;
        rdp.textures_changed[tile] = true;
    }

    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile < 2) {
        rdp.texture_tile[tile].uls = uls;
        rdp.texture_tile[tile].ult = ult;
        rdp.texture_tile[tile].lrs = lrs;
        rdp.texture_tile[tile].lrt = lrt;
        rdp.textures_changed[tile] = true;
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index) {
    _UNUSED(high_index);

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
}

/* Which of the two TMEM slots a load command writes into.
 *
 * The destination is a property of the LOAD TILE, not a constant.
 * rdp.texture_to_load.tile_number answers it only for tile G_TX_LOADTILE (7),
 * because that is the only tile gfx_dp_set_tile records it for. A display list
 * that loads through tile 1 would therefore have written over whatever slot the
 * last tile-7 load happened to name, and both load commands avoided that by
 * DROPPING the load outright (`if (tile == 1) return;`) -- leaving the slot
 * holding the previous texture's address and size, so every draw referencing it
 * got stale pixels.
 *
 * Ship of Harkinian has no such special case; it indexes by
 * rdp.texture_tile[tile].tmem_index (reference/shipwright-vita, gfx_pc.cpp,
 * gfx_dp_load_block). tmem_slot is this port's equivalent field and
 * gfx_dp_set_tile already maintains it for tiles 0 and 1, so tile 1 can simply
 * be asked directly.
 *
 * Deliberately narrow: only tile 1 takes the new route. Tile 7 (and tile 0,
 * which some lists do use as a load tile) keep the path that has been drawing
 * correctly all along, so this cannot regress them.
 *
 * The & 1 is a real bounds fix, not cosmetics: loaded_texture[] and
 * texture_tile[] have two entries, while tile_number is tmem/256 and is not
 * bounded by anything. */
static inline uint32_t gfx_load_dest_slot(uint8_t tile) {
    if (tile == 1) {
        return rdp.texture_tile[1].tmem_slot & 1;
    }
    return rdp.texture_to_load.tile_number & 1;
}

/* How many loads arrive through tile 1, i.e. the ones the old guard threw away.
 * Zero would mean the guard was harmless all along and this change is inert;
 * anything else is that many stale textures. Measure before believing either. */
uint32_t gPspTile1Loads;

/* A/B switch for the above, since it touches the texture path every scene
 * depends on: 0 restores the old behaviour of dropping tile-1 loads. */
int gPspGfxTile1LoadsEnable = 1;

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, uint32_t dxt) {
    _UNUSED(dxt);

    GFXSTAT_INC(loadblock);
    if (tile == 1) {
        gPspTile1Loads++;
        if (!gPspGfxTile1LoadsEnable) return;
    }
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    const uint32_t dest = gfx_load_dest_slot(tile);

    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
    rdp.loaded_texture[dest].size_bytes = size_bytes;
    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[dest].addr = rdp.texture_to_load.addr;
    
    /* Contiguous by definition -- make sure a previous G_LOADTILE's stride does
     * not leak into this tile. */
    rdp.texture_tile[dest].src_stride_bytes = 0;

    rdp.textures_changed[dest] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    GFXSTAT_INC(loadtile);
    if (tile == 1) {
        gPspTile1Loads++;
        if (!gPspGfxTile1LoadsEnable) return;
    }

    const uint32_t dest = gfx_load_dest_slot(tile);

    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    /* G_LOADTILE pulls a SUB-RECTANGLE out of a wider image; the old maths here
     * assumed the rectangle always started at (0,0) -- which the two
     * SUPPORT_CHECKs above asserted, and which is true for everything the port
     * had drawn until the skybox.
     *
     * The skybox is not that: Skybox_CalculateFace256 (z_vr_box.c:182) loads a
     * 256-texel-wide CI8 face as a 4x4 grid of 64x32 tiles, stepping uls by 63
     * and ult by 31. Taking lrs/lrt as the size ignores the origin, and reading
     * the source linearly ignores that successive rows of a 64-wide tile sit
     * 256 bytes apart -- which is exactly a shear, and exactly what the skybox
     * looked like: diagonal bands. */
    uint32_t tile_w = (lrs >> G_TEXTURE_IMAGE_FRAC) - (uls >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_h = (lrt >> G_TEXTURE_IMAGE_FRAC) - (ult >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t row_bytes = tile_w << word_size_shift;
    uint32_t size_bytes = row_bytes * tile_h;
    /* texture_to_load.width is the true source width (see
     * gfx_dp_set_texture_image); it is only 0 before the first G_SETTIMG. */
    uint32_t src_stride = (rdp.texture_to_load.width > 1 ? rdp.texture_to_load.width : tile_w)
                          << word_size_shift;

    rdp.loaded_texture[dest].size_bytes = size_bytes;

    assert(size_bytes <= 4096 && "bug: too big texture");
    /* Point at the sub-rectangle's own first texel. The UVs stay in SOURCE
     * space and gfx_sp_tri1 subtracts texture_tile[].uls/ult from them, so the
     * two together land on tile-local coordinates. */
    rdp.loaded_texture[dest].addr =
        rdp.texture_to_load.addr + (ult >> G_TEXTURE_IMAGE_FRAC) * src_stride +
        (((uls >> G_TEXTURE_IMAGE_FRAC) << word_size_shift));
    rdp.texture_tile[dest].uls = uls;
    rdp.texture_tile[dest].ult = ult;
    rdp.texture_tile[dest].lrs = lrs;
    rdp.texture_tile[dest].lrt = lrt;
    rdp.texture_tile[dest].line_size_bytes = row_bytes;
    rdp.texture_tile[dest].src_stride_bytes = src_stride;

    rdp.textures_changed[dest] = true;
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        /* PRIMITIVE_ALPHA/SHADE_ALPHA/ENV_ALPHA select the alpha channel of
         * these colors as an RGB multiplier -- CC_* has no "alpha-as-scalar"
         * concept, so approximate by using the parent color's RGB instead of
         * silently collapsing to CC_0 (which was discarding the entire
         * (A-B)*C term below whenever C was one of these, e.g. OoT's N64
         * logo cube combine `(TEXEL0-PRIMITIVE)*ENV_ALPHA+TEXEL0` was
         * reducing to a flat, untinted TEXEL0 passthrough). */
        case G_CCMUX_PRIMITIVE_ALPHA:
            return CC_PRIM;
        case G_CCMUX_SHADE_ALPHA:
            return CC_SHADE;
        case G_CCMUX_ENV_ALPHA:
            return CC_ENV;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

/* OoT tints almost everything in the SECOND combiner cycle, and this port only
 * ever decoded the first (see G_SETCOMBINE, where the cycle-2 operands sit
 * commented out). Link's tunic is the clearest case:
 *
 *   gsDPSetCombineLERP(TEXEL0, 0, SHADE, 0,  0,0,0,TEXEL0,
 *                      ENVIRONMENT, 0, COMBINED, 0,  0,0,0,COMBINED)
 *
 * cycle 1 is TEXEL0 * SHADE and cycle 2 is ENV * COMBINED -- and ENV is where
 * Player_DrawImpl puts the tunic colour (sTunicColors, {30,105,27} for Kokiri).
 * Dropping cycle 2 left the tunic untinted, i.e. white. 49 of Link's combines
 * multiply by PRIMITIVE in cycle 2 and 18 by ENVIRONMENT, so this is the rule
 * for this game rather than a special case.
 *
 * Full 2-cycle emulation is not needed for the dominant shape. When cycle 2 is
 * just "multiply the whole cycle-1 result by one colour register", and cycle 1
 * already resolves to texture * vertexColour, the register folds straight into
 * the vertex colour -- which the GE then multiplies by the texture exactly as
 * before. So all this has to recover is WHICH register.
 *
 * Returns CC_PRIM / CC_ENV / CC_SHADE, or CC_0 for "not this shape, do
 * nothing". Slot encodings differ (a/b are 4 bits, c is 5, d is 3) and a
 * written 0 is G_CCMUX_0 == 31 truncated to the slot width, which
 * color_comb_component already maps to CC_0. Raw 0 means COMBINED in every
 * slot, which is the one thing color_comb_component cannot express, so it is
 * tested directly. */
static uint8_t combine_cycle2_tint(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint8_t reg;

    /* b and d must be genuine zero, not COMBINED: anything else is a subtract
     * or an add and no longer a plain multiply. */
    if (b == G_CCMUX_COMBINED || color_comb_component(b) != CC_0) {
        return CC_0;
    }
    if (d == G_CCMUX_COMBINED || color_comb_component(d) != CC_0) {
        return CC_0;
    }

    if (a == G_CCMUX_COMBINED) {
        reg = color_comb_component(c); /* (COMBINED - 0) * REG + 0 */
    } else if (c == G_CCMUX_COMBINED) {
        reg = color_comb_component(a); /* (REG - 0) * COMBINED + 0 */
    } else {
        return CC_0;
    }

    return (reg == CC_PRIM || reg == CC_ENV || reg == CC_SHADE) ? reg : CC_0;
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha) {
    rdp.combine_mode = rgb | (alpha << 12);
}

/* Exposed to gfx_scegu.c for the N64-logo-cube 2-pass combine emulation
 * (see gfx_scegu.c's gfx_scegu_draw_n64_logo_cube_2pass) -- same packing as
 * gRdpPrimColorPacked. */
uint32_t gRdpEnvColorPacked = 0xFFFFFFFF;

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    /* Multiple consecutive triangle groups sharing the same shader+texture
     * (e.g. the OoT boot logo cube's 5 face groups, all one combine/texture,
     * only PRIM/ENV differing) get batched into a single buffered draw call
     * by gfx_flush()'s own triggers (shader/texture change only) -- without
     * flushing here first, an already-buffered-but-not-yet-drawn group would
     * retroactively pick up this NEW env color instead of the one that was
     * active when its vertices were actually processed. Real hardware has no
     * such batching, so every group's color is always correct there. */
    if (rdp.env_color.r != r || rdp.env_color.g != g || rdp.env_color.b != b || rdp.env_color.a != a) {
        gfx_flush();
    }
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
    gRdpEnvColorPacked = (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
}

/* Exposed to gfx_scegu.c for the 2-cycle "N64 logo cube" combine hack (see
 * gfx_scegu.c's texenv_set_texture_color) -- PSP GU_COLOR_8888 byte order
 * matches struct RGBA{r,g,b,a} packed already. */
uint32_t gRdpPrimColorPacked = 0xFFFFFFFF;

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    /* Same batching hazard as gfx_dp_set_env_color -- flush before adopting
     * the new PRIM color/gRdpPrimColorPacked so any already-buffered group
     * still gets drawn (and, for the logo-cube hack, gets sceGuTexEnvColor()
     * refreshed) with the color that was actually active when it was built. */
    if (rdp.prim_color.r != r || rdp.prim_color.g != g || rdp.prim_color.b != b || rdp.prim_color.a != a) {
        gfx_flush();
    }
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
    gRdpPrimColorPacked = (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

/* 1 = the old behaviour, aspect-correct 2D rectangles (and pillarbox them).
 * 0 = the fix. A switch rather than a deletion because this path also carries
 * the HUD and the screen fades, so the two can be compared in place. */
int gPspRect2dPillarbox = 0;

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = (ulyf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = (lryf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;

    /* NOT aspect-corrected, and that is the fix rather than an oversight.
     *
     * gfx_adjust_x_for_aspect_ratio multiplies x by (4/3)/(480/272) = 0.7556.
     * Applied here it PILLARBOXES every screen-space rectangle: a full-width
     * N64 rect (0..319) came out spanning pixels 59..421 of 480, with a 59-pixel
     * black bar down each side.
     *
     * The 3D scene has no such bar. gfx_calc_and_set_viewport scales the
     * viewport by RATIO_X = 480/320 = 1.5, i.e. the full width, and the only
     * place the 3D path applies the aspect factor is the CLIP test in
     * gfx_sp_vertex -- never the coordinates the GE rasterises (see the long
     * comment there, which flags exactly this inconsistency and says it is real
     * and worth revisiting). So 2D rectangles were pillarboxed onto a world
     * that is not.
     *
     * What made it visible: OoT paints a full-screen rectangle in the scene's
     * FOG COLOUR whenever skyboxId is SKYBOX_UNSET_1D -- that is how a scene
     * with no skybox gets a sky (Environment_DrawSkyboxFilters, z_kankyo.c,
     * where UNSET_1D also forces alpha to 1.0). Every scene the user reported
     * as "a coloured box with black bars left and right" is SKYBOX_UNSET_1D and
     * no other: Kokiri Forest, Sacred Forest Meadow, Lost Woods, Zora's
     * Fountain, Lord Jabu-Jabu's boss room, the Windmill/Dampe's Grave scene and
     * the Water Temple. It also explains the colours -- olive in Kokiri Forest
     * (76,83,60), yellow-green in the Windmill (150,170,120), dark blue in the
     * Lost Woods at night -- because they ARE those scenes' fog colours, and why
     * two of them are interiors with no sky at all, which is what made the
     * report look like two unrelated bugs.
     *
     * The HUD uses this same path and moves with it. That is the point: it has
     * to line up with a world that fills the width. Nobody noticed it was inset
     * because almost all of Interface_* is still stubbed. */
    if (gPspRect2dPillarbox) {
        ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
        lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
    }

    ulxf = (ulxf*240)+240;
    lrxf = (lrxf*240)+240;

    ulyf = (ulyf*136)+136;
    lryf = (lryf*136)+136;
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    
    ul->x = (unsigned short)ulxf;
    ul->y = (unsigned short)ulyf;

    lr->x = (unsigned short)lrxf;
    lr->y = (unsigned short)lryf;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;
    
    gfx_sp_tri1_2d(0, 1, 2);
    
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    _UNUSED(tile);

    uint32_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0));
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    /*@Note: fix this */
    #if 0
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    #endif
    
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    uint32_t saved_combine_mode = rdp.combine_mode;

    if (mode == G_CYC_FILL || mode == G_CYC_COPY) {
        // Real hardware: FILL/COPY cycle bypasses the color combiner
        // entirely and paints the raw fill_color register directly.
        for (int i = 0; i < 2; i++) {
            rsp.loaded_vertices_2D[i].color = rdp.fill_color;
        }
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE));
    } else {
        // 1-cycle/2-cycle mode: real hardware runs FILLRECT through
        // whatever combiner the caller already configured, NOT fill_color.
        // z_fbdemo_fade.c's TransitionFade (the OoT scene-transition fade
        // used e.g. entering Redead Grave) sets up a PRIMITIVE-based
        // 1-cycle combine and a per-frame ramping alpha via gDPSetPrimColor
        // before its gDPFillRectangle call -- unconditionally forcing
        // SHADE/fill_color here, as this code used to do, silently
        // discarded that real per-frame PRIM alpha ramp, which manifested
        // as the fade only ever showing its two extreme colors (solid
        // black/solid white flicker) instead of a smooth cross-fade.
        // CC_PRIM and CC_SHADE both map to the vertex-color shader input in
        // this port's simplified combiner (see gfx_generate_cc's CC_PRIM/
        // CC_SHADE case), so feeding prim_color through the vertex color
        // here keeps the caller's already-set combine_mode intact and
        // correct without needing a dedicated PRIM shader path.
        for (int i = 0; i < 2; i++) {
            rsp.loaded_vertices_2D[i].color = rdp.prim_color;
        }
    }

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    _UNUSED(format);
    _UNUSED(size);
    _UNUSED(width);

    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

/* sm64-port-psp's original seg_addr() was pure identity -- SM64's own PC
 * port already resolved every N64 segmented address to a real pointer
 * years earlier, across its whole codebase, so its gfx_pc.c never needed
 * to. OoT's decomp still actively uses real segmented addressing elsewhere
 * (e.g. z_actor.c's gSegments[6] = ... for actor object data), so this
 * needs a real implementation to work correctly once scenes/actors are in
 * scope.
 *
 * Earlier in this session, enabling this coincided with what looked like a
 * rendering regression (visible-but-garbled -> solid black) -- turned out
 * to be a false signal: automated screenshots taken while the PPSSPP
 * window was unfocused come back solid black regardless of what's actually
 * rendering (PPSSPP appears to pause/blank when not focused). With a
 * focused screenshot this is confirmed NOT a regression -- see reference
 * memory notes for this gotcha.
 *
 * Heuristic: treat w1 as segmented (resolve via gSegments[]) only if its
 * upper byte looks like a small segment number (< NUM_SEGMENTS) AND that
 * segment slot has actually been populated (non-NULL) -- otherwise treat
 * it as an already-real pointer (e.g. static compiled-in data passed
 * directly into a gSP* macro without ever going through
 * SEGMENTED_TO_VIRTUAL). This mirrors real N64 convention: segment 0 is
 * conventionally left unset/NULL for "not relocated" content, and code
 * explicitly sets gSegments[N] only for segments it actually uses (see
 * src/code/main.c's gSegments[NUM_SEGMENTS] and SEGMENT_NUMBER/
 * SEGMENT_OFFSET in include/ultra64/mbi.h for the real encoding this
 * matches). Known limitation: a real PSP pointer whose own upper byte
 * happens to be < NUM_SEGMENTS (16) AND collides with a populated segment
 * slot would be mis-resolved -- not ruled out yet, revisit if a future
 * scene's geometry looks wrong in a way this could explain (see
 * libultraship's low-bit marker convention, SegAddr() in
 * reference/libultraship/src/fast/interpreter.cpp, for a collision-proof
 * alternative discriminator if this becomes a real problem). */
#if TARGET_PSP
/* PSP user RAM is 0x08800000..0x09FFFFFF -- that window is why segments 8 and
 * 9 collide with native pointers at all; see the long note in seg_addr(). */

/* Genuine segment-8/9 references carry a tiny offset -- they name one small
 * texture or one runtime-built display list, in practice at offset 0 and
 * measured never above a few tens of KB. Native pointers that merely start
 * with 0x08/0x09 sit far higher: the graphics pools are at offset ~0x189000
 * and everything reached through the 0x08 page is at offset >= 0x800000. */
/* PSP_SEG89_NATIVE_MIN / PSP_SEG89_AMBIGUOUS_MIN come from
 * include/segmented_address.h (included above). They are deliberately shared
 * with SEGMENTED_TO_VIRTUAL's C-side resolver rather than duplicated here:
 * the two resolvers must classify the same value the same way, and this
 * threshold was previously defined twice, which is exactly how they drifted
 * apart in the first place (the interpreter got the offset-magnitude fix, the
 * C side was left on the weaker "is the slot populated" test). */

/* How often a value that looked like a segment-8/9 reference was passed
 * through as a native pointer instead, split by segment number. */
unsigned int gPspSegAddrNative8 = 0;
unsigned int gPspSegAddrNative9 = 0;
/* Values landing in the unproven middle band, so the threshold above can be
 * checked against reality instead of trusted. Should stay at 0. */
unsigned int gPspSegAmbiguous8 = 0;
unsigned int gPspSegAmbiguous9 = 0;
unsigned int gPspSegAmbiguousLast = 0;
#endif

static inline void *seg_addr(uintptr_t w1) {
    uint32_t segNum = SEGMENT_NUMBER(w1);
    if (segNum < NUM_SEGMENTS && gSegments[segNum] != 0) {
#if TARGET_PSP
        /* THE segment/pointer collision on this platform.
         *
         * PSP user RAM is 0x08800000..0x09FFFFFF, so *every* native pointer
         * here carries 0x08 or 0x09 in its top byte -- which is exactly the
         * bit pattern an N64 segmented address uses for segments 8 and 9, and
         * OoT genuinely uses both (Player's eye and mouth textures).
         *
         * On real hardware there is no ambiguity: a native N64 pointer is
         * KSEG0 (0x80......), whose segment nibble reads as 0, and
         * gSegments[0] is 0, so the check above lets it fall through
         * untouched. On PSP that discriminator does not exist. The moment a
         * frame's display list executes gSPSegment(8, ...), every subsequent
         * native pointer beginning 0x08 gets "resolved" into garbage --
         * measured live: gLinkChildWaistNearDL (0x088e5fa8) was rewritten to
         * 0x091ebc40, inside gZBuffer, and the interpreter then walked
         * linearly through ~1.6MB of zeroes until the runaway guard stopped
         * it, every single frame.
         *
         * A genuine segment-8/9 reference is a small offset into one small
         * texture and therefore sits below the start of RAM; a native
         * pointer's low 24 bits are its offset inside the RAM window. So
         * anything landing inside the RAM window is a pointer, not a
         * reference.
         *
         * Only segments 8 and 9 can collide -- every other segment's
         * reference range (0x00xxxxxx-0x07xxxxxx, 0x0Axxxxxx+) lies outside
         * the RAM window entirely. For those two, offset magnitude is the
         * discriminator, and it separates cleanly in measured traffic:
         * genuine references sit at offset 0 (e.g. the segment-9 reference
         * 0x09000000 that a room display list really does make), while native
         * pointers are megabytes up.
         *
         * An earlier version of this check used "inside the RAM window" as
         * the test. That is exact for segment 8, whose references stay below
         * the start of RAM, but it silently misread every segment-9 reference
         * as a pointer -- which is what sent the interpreter wandering
         * through 0x0900xxxx once the segment-8 half was fixed. */
        if ((segNum == 8 || segNum == 9)
            && SEGMENT_OFFSET(w1) >= PSP_SEG89_NATIVE_MIN) {
            if (segNum == 8) {
                ++gPspSegAddrNative8;
            } else {
                ++gPspSegAddrNative9;
            }
            return (void *)w1;
        }
        if ((segNum == 8 || segNum == 9)
            && SEGMENT_OFFSET(w1) >= PSP_SEG89_AMBIGUOUS_MIN) {
            /* Neither interpretation is proven here. Resolve as segmented
             * (the conservative choice -- it matches real hardware) but
             * record it, because a non-zero count means this threshold needs
             * revisiting rather than trusting. */
            if (segNum == 8) {
                ++gPspSegAmbiguous8;
            } else {
                ++gPspSegAmbiguous9;
            }
            gPspSegAmbiguousLast = (uint32_t)w1;
        }
#endif
        return (void *)(gSegments[segNum] + SEGMENT_OFFSET(w1));
    }
    return (void *) w1;
}

#if TARGET_PSP
/* Pre-rendered room background -- see psp/include/gfx/psp_bg_rect.h for why
 * this is a port-private command instead of the S2DEX display list the N64
 * uses, and gfx_scegu_draw_background() for how the blit itself works. */
extern void gfx_scegu_draw_background(const void *img, int width, int height, int offset_x, int offset_y);
extern void gfx_scegu_invalidate_texture_binding(void);

uint32_t gPspBgDrawn = 0;
uint32_t gPspBgSkipped = 0;
uint32_t gPspBgLastSkipReason = 0;

static void gfx_psp_bg_rect(const PspBgRect *bg) {
    if (bg == NULL) {
        return;
    }

    const uint8_t *img = seg_addr((uintptr_t)bg->source);

    /* Vanilla only ever uses RGBA16 for these (every RoomShapeImage in the
     * game sets fmt=G_IM_FMT_RGBA, siz=G_IM_SIZ_16b); the CI branch of
     * Room_DrawBackground2D is dead code. Refuse anything else rather than
     * blit it as 5551 garbage. */
    if (img == NULL || bg->width <= 0 || bg->height <= 0 ||
        bg->fmt != G_IM_FMT_RGBA || bg->siz != G_IM_SIZ_16b) {
        gPspBgLastSkipReason = 1;
        ++gPspBgSkipped;
        return;
    }

    /* Still-undecoded JPEG data: this room's blob was not built by the PSP
     * asset pipeline (psp/tools/jfif_to_psp.py decodes it at build time, in
     * place). Drawing it would paint compressed bytes on the screen, so skip
     * -- the counter says the pipeline, not the renderer, is what to fix.
     * Tested in both byte orders because the check costs nothing and the two
     * asset paths (compiled-in u64 literals vs. raw ROM bytes) differ. */
    {
        uint32_t magic = *(const uint32_t *)img;
        if (magic == 0xFFD8FFE0 || magic == 0xE0FFD8FF) {
            gPspBgLastSkipReason = 2;
            ++gPspBgSkipped;
            return;
        }
    }

    /* Tell the grabber which background is on screen. Rooms with a fixed
     * camera can hold several images, one per camera angle, and switching
     * angle swaps the image without reloading the room -- which is exactly
     * the moment being investigated. */
    PspScreenshot_NoteBgImage(img);

    gfx_flush();
    gfx_scegu_draw_background(img, bg->width, bg->height, bg->offsetX, bg->offsetY);
    ++gPspBgDrawn;

    /* Put back everything the blit changed. Depth state is restored from what
     * gfx_pc believes is current; the shader and the texture binding are
     * invalidated instead, so the next draw re-applies them through the normal
     * paths rather than trusting a cache the blit went behind the back of. */
    gfx_rapi->set_depth_test(rendering_state.depth_test);
    gfx_rapi->set_depth_mask(rendering_state.depth_mask);
    rendering_state.shader_program = NULL;
    gfx_scegu_invalidate_texture_binding();
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}
#endif

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

#if TARGET_PSP
/* Safety net against microcode-confusion bugs (e.g. an S2DEX opcode
 * colliding with a real F3DEX2 opcode number and producing a bad G_DL
 * branch) sending this interpreter wandering through unrelated memory
 * hunting for a G_ENDDL that isn't there -- caps the damage to one
 * slow-but-bounded pass instead of a true hang. Shared across the whole
 * gfx_run() call (reset there, not per gfx_run_dl invocation) because
 * gfx_run_dl recurses on G_DL "push" branches -- a per-call-local counter
 * would never trip on an infinite/very-deep recursive branch cycle, since
 * each individual recursive call could stay under the cap while the total
 * work across all of them never terminates. */
unsigned int gPspGfxOpcodeGuardCount = 0;

/* ---------------------------------------------------------------------------
 * Display-list walk trace.
 *
 * The frame statistics showed dl_cmds pinned at exactly the guard limit on
 * EVERY frame while only ~650 triangles were ever submitted -- i.e. the
 * interpreter is not drawing 200k commands, it is *wandering* through
 * something that is not a display list and never yields a G_ENDDL. This
 * records where that happens so it can be identified from the address alone.
 *
 * Same three-generation layout and the same no-file-I/O rule as
 * PspGfxFrameStats above.
 * ------------------------------------------------------------------------- */
#define PSP_DL_PROBES 4
#define PSP_DL_PROBE_EVERY 25000
#define PSP_DL_JUMPS 16
/* A healthy room frame measured max_depth 4; the crashing one hit 47. */
#define PSP_DL_SNAP_LEVELS 24
#define PSP_DL_SNAP_DEPTH  PSP_DL_SNAP_LEVELS

typedef struct {
    uint32_t magic;      /* 'PDLT'                                            */
    uint32_t frame;
    uint32_t guard_hit;  /* 1 if the runaway guard tripped this frame         */
    uint32_t guard_cmd;  /* address of the command it tripped on              */
    uint32_t guard_w0;   /* that command's words, to see what it thinks it is */
    uint32_t guard_w1;
    uint32_t guard_depth;/* gfx_run_dl recursion depth at the trip            */
    uint32_t dl_calls;   /* G_DL, push variant (recurses)                     */
    uint32_t dl_branches;/* G_DL, branch variant (gSPBranchList)              */
    uint32_t max_depth;
    uint32_t dl_top;     /* the Gfx* gfx_run() itself was handed              */
    uint32_t njumps;     /* jumps recorded (may exceed PSP_DL_JUMPS)          */
    /* cmd address sampled every PSP_DL_PROBE_EVERY commands: if the walk is
     * stuck in a small region these cluster, if it is marching linearly
     * through memory they climb by a steady ~200KB per probe. */
    uint32_t probe[PSP_DL_PROBES];
    /* G_DL jumps as a RING of the most recent PSP_DL_JUMPS: the command's own
     * address, the raw w1 before segment resolution, the address actually
     * jumped to, and the recursion depth. A healthy frame now makes ~400
     * jumps, so the interesting ones are at the end, not the start -- and a
     * cycle shows up immediately as repeating addresses. Slot for jump i is
     * i % PSP_DL_JUMPS; the reader reorders using njumps. */
    uint32_t jump_from[PSP_DL_JUMPS];
    uint32_t jump_w1[PSP_DL_JUMPS];
    uint32_t jump_to[PSP_DL_JUMPS];
    uint32_t jump_depth[PSP_DL_JUMPS];
    /* Snapshot of the whole display-list call stack (the cmd address each
     * level was entered at) taken the first time the recursion depth reaches
     * PSP_DL_SNAP_DEPTH. A runaway nesting shows up here directly: a cycle
     * repeats the same few addresses down the levels, whereas a legitimately
     * deep scene shows all-different ones. */
    uint32_t snap_taken;
    uint32_t depth_aborts; /* recursions refused by PSP_DL_MAX_DEPTH */
    uint32_t snap[PSP_DL_SNAP_LEVELS];
} PspGfxDlTrace;

PspGfxDlTrace gPspGfxDlTrace;
PspGfxDlTrace gPspGfxDlTracePrev;
PspGfxDlTrace gPspGfxDlTracePrev2;

#define PSP_DL_TRACE_MAGIC 0x50444C54u /* 'PDLT' */

static unsigned int sPspGfxDlDepth = 0;
/* cmd address each recursion level was entered at, for the snapshot above. */
static uint32_t sPspDlEntry[PSP_DL_SNAP_LEVELS];

static void PspGfxDlRecordJump(const void *from, uint32_t w1, const void *to) {
    unsigned int i = gPspGfxDlTrace.njumps++ % PSP_DL_JUMPS;
    gPspGfxDlTrace.jump_from[i] = (uint32_t)(uintptr_t)from;
    gPspGfxDlTrace.jump_w1[i] = w1;
    gPspGfxDlTrace.jump_to[i] = (uint32_t)(uintptr_t)to;
    gPspGfxDlTrace.jump_depth[i] = sPspGfxDlDepth;
}
#endif

/* Companion to the command-count guard below: cap nesting too. A display list
 * that never yields a G_ENDDL makes the interpreter recurse without ever
 * unwinding, which blows the real PSP thread stack long before the command
 * counter runs out -- that is a hard crash rather than a bounded slow frame.
 * Measured healthy room frames peak at depth 4, so 64 is far above anything
 * legitimate. */
#define PSP_DL_MAX_DEPTH 64

#if TARGET_PSP
/* Rejected display-list cursors, so the HUD can distinguish "the renderer
 * refused to walk into nowhere" from "the bug is gone". */
unsigned int gPspGfxBadDlCursors = 0;
unsigned int gPspGfxBadDlLast = 0;

/* Is this address a Gfx command we may legally dereference?
 *
 * The PSP user partition starts at 0x08800000 -- everything below is kernel
 * memory, and a user-mode read there does not return garbage, it takes an
 * exception and the console loses power. A display list built from stale or
 * half-loaded data routinely points somewhere like that, and the interpreter
 * would follow it: PPSSPP maps guest RAM as one flat host block and happily
 * reads whatever is there, so the whole class is invisible under emulation
 * and instantly fatal on hardware.
 *
 * reference/oot-psp-z2442 guards the same thing (gfx_validate_dl_cursor in
 * src/port/psp/gfx/gfx_fast3d.c) and additionally proves provenance against a
 * registry of known asset ranges. This is the cheap half of that: partition
 * bounds plus the 8-byte alignment every Gfx has by construction. It cannot
 * catch a wild pointer that happens to land in valid RAM, but it does catch
 * every pointer that would kill the console outright, and it costs two
 * compares per display list rather than a lookup. */
static inline int PspGfxDlCursorIsSafe(const void* p) {
    uintptr_t a = (uintptr_t)p;

    return (a >= 0x08800000U) && (a < 0x0C000000U) && ((a & 7u) == 0);
}
#endif

static void gfx_run_dl(Gfx* cmd) {
#if TARGET_PSP
    if (!PspGfxDlCursorIsSafe(cmd)) {
        ++gPspGfxBadDlCursors;
        gPspGfxBadDlLast = (uint32_t)(uintptr_t)cmd;
        return; /* nothing pushed yet, so nothing to unwind */
    }
#endif
#if TARGET_PSP
    if (sPspGfxDlDepth >= PSP_DL_MAX_DEPTH) {
        ++gPspGfxDlTrace.depth_aborts;
        return; /* nothing pushed yet, so nothing to unwind */
    }
    if (++sPspGfxDlDepth > gPspGfxDlTrace.max_depth) {
        gPspGfxDlTrace.max_depth = sPspGfxDlDepth;
    }
    if (sPspGfxDlDepth <= PSP_DL_SNAP_LEVELS) {
        sPspDlEntry[sPspGfxDlDepth - 1] = (uint32_t)(uintptr_t)cmd;
        if (sPspGfxDlDepth == PSP_DL_SNAP_DEPTH && !gPspGfxDlTrace.snap_taken) {
            gPspGfxDlTrace.snap_taken = 1;
            memcpy(gPspGfxDlTrace.snap, sPspDlEntry, sizeof(sPspDlEntry));
        }
    }
#define PSP_DL_RETURN() do { --sPspGfxDlDepth; return; } while (0)
#else
#define PSP_DL_RETURN() return
#endif
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;

#if TARGET_PSP
        ++gPspGfxOpcodeGuardCount;
        if (gPspGfxOpcodeGuardCount % PSP_DL_PROBE_EVERY == 0) {
            unsigned int slot = gPspGfxOpcodeGuardCount / PSP_DL_PROBE_EVERY - 1;
            if (slot < PSP_DL_PROBES) {
                gPspGfxDlTrace.probe[slot] = (uint32_t)(uintptr_t)cmd;
            }
        }
        if (gPspGfxOpcodeGuardCount > 200000) {
            if (!gPspGfxDlTrace.guard_hit) {
                gPspGfxDlTrace.guard_hit = 1;
                gPspGfxDlTrace.guard_cmd = (uint32_t)(uintptr_t)cmd;
                gPspGfxDlTrace.guard_w0 = cmd->words.w0;
                gPspGfxDlTrace.guard_w1 = cmd->words.w1;
                gPspGfxDlTrace.guard_depth = sPspGfxDlDepth;
            }
            PSP_DL_RETURN();
        }
#endif
        switch (opcode) {
            // RSP commands:
            case G_MTX:
                gfx_flush();
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
#if TARGET_PSP
                    ++gPspGfxDlTrace.dl_calls;
                    PspGfxDlRecordJump(cmd, cmd->words.w1, seg_addr(cmd->words.w1));
#endif
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
#if TARGET_PSP
                    ++gPspGfxDlTrace.dl_branches;
                    PspGfxDlRecordJump(cmd, cmd->words.w1, seg_addr(cmd->words.w1));
#endif
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                PSP_DL_RETURN();
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
            /* G_QUAD shares G_TRI2's decode -- verified by preprocessing
             * gSP1Quadrangle with this build's own flags: under F3DEX_GBI_2 it
             * emits opcode 0x07 with w0 = (v0,v1,v2) and w1 = (v0,v2,v3), i.e.
             * the quad already split into two triangles.
             *
             * Session 13 found this case missing and recorded it as "harmless:
             * neither Link's objects nor spot02 emit a single gsSPQuadrangle".
             * That stopped being true the moment the skybox could run: the
             * skybox is built ENTIRELY from gSP1Quadrangle
             * (Skybox_CalculateFace256, z_vr_box.c:186), so every one of its
             * faces fell through this switch -- which has no default -- and was
             * silently dropped. Measured: 890 skybox display-list sections
             * interpreted per run, 0 triangles submitted.
             *
             * That is why the Market had no buildings and Link's House in pivot
             * no walls: both are skyboxes (SKYBOX_MARKET_CHILD_DAY /
             * SKYBOX_HOUSE_LINK). */
            case (uint8_t)G_QUAD:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
            /* The COMBINED othermode write. sm64-port never needed this case:
             * SM64 only ever emits the split G_SETOTHERMODE_H/_L pair below.
             * OoT uses gsDPSetOtherMode -- which expands to this opcode -- in
             * every one of z_rcp.c's setup display lists, i.e. before nearly
             * every draw in the game, so all of othermode was arriving stale.
             *
             * That is a lot of state: other_mode_h carries the CYCLE TYPE and
             * the texture filter, and other_mode_l carries the render mode,
             * which is where gfx_sp_tri1 reads use_alpha, use_fog, z_upd,
             * zmode_decal and texture_edge from. Concretely it is why Link's
             * tunic stayed untinted -- the cycle-2 tint is only applied in
             * G_CYC_2CYCLE, and the cycle type never got set.
             *
             * Unlike the split writes this is a full assignment, not a masked
             * merge: mode0 is the whole H word (24 significant bits, packed
             * into w0) and mode1 the whole L word. */
            case (uint8_t)G_RDPSETOTHERMODE:
                rdp.other_mode_h = C0(0, 24);
                rdp.other_mode_l = cmd->words.w1;
                break;
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 12), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                rdp.prim_lod_frac = C0(0, 8);
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(
                    color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)));
                /* Cycle 2's RGB row -- the operands that used to be dropped
                 * here (they are the commented-out line this replaces). Only
                 * the "multiply everything by one colour register" shape is
                 * recovered; see combine_cycle2_tint. Kept out of cc_id on
                 * purpose so shader ids are unaffected. */
                rdp.combine_cyc2_tint = combine_cycle2_tint(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3));
                rdp.combine_c0_raw = C0(15, 5);
                /* alpha cycle 2 would be color_comb(C1(21,3), C1(3,3), C1(18,3), C1(0,3)) */
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
#if TARGET_PSP
            /* Port-private, not an F3DEX2 command. The argument block is a
             * native pointer into the display buffer (like the S2DEX uObjBg it
             * replaces), so it is deliberately NOT run through seg_addr. */
            case G_PSP_BGRECT:
                gfx_psp_bg_rect((const PspBgRect *)(uintptr_t)cmd->words.w1);
                break;
            /* Diagnostic marker; see psp_bg_rect.h. Attributes the triangles
             * between BEGIN and END to a named part of the frame. */
            case G_PSP_MARK:
                if (cmd->words.w1 == PSP_MARK_SKYBOX_BEGIN) {
                    gPspSkyTriMark = 1;
                    gPspSkyTri[0] = 0;
                    gPspSkyVtxOutCount = 0; /* one frame's worth, not cumulative */
                    gPspSkyTri[1]++; /* BEGIN markers actually interpreted */
                } else {
                    gPspSkyTriMark = 0;
                    gPspSkyTri[2]++; /* END markers */
                }
                gPspSkyTri[3] = gPspGfxDlTrace.dl_calls;
                break;
#endif
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}
#undef PSP_DL_RETURN

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
    /* Default ON. A frame that draws before its first gsSPTexture must not be
     * counted as "texturing switched off" -- that would be a measurement
     * artefact, not the condition tex_off_draws is asking about. */
    rsp.texture_on = true;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}


float times[30];
float time_avg;
float time_first_200;
int total_frame_counter;
int frame_counter;

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();

    int i;
    for(i=0;i<30;i++){
        times[i] = 0.0f;
    }
    frame_counter = 0;
    time_avg = 0.0f;
    time_first_200 = 0;
    total_frame_counter = 0;

    /* sm64-port-psp precompiled a hardcoded list of 26 SM64-specific
     * color-combiner shader IDs here ("used in the 120 star TAS") -- with
     * OoT's shader table starting empty (plan step 7), those IDs mean
     * nothing here and would just spam 26 "shader not known" fallback
     * hits at startup. Removed; OoT's own shader IDs get created lazily
     * as gfx_run() encounters them during real display-list interpretation. */

    memcpy(rsp.P_matrix, identity_matrix, sizeof(identity_matrix));
    memcpy(rsp.modelview_matrix_stack[0], identity_matrix, sizeof(identity_matrix));
    /* Slot 0 now holds a real identity, so make it the live one. Without this
     * the stack is still logically empty until the first G_MTX, and the
     * lighting path (calculate_normal_dir, which reads
     * modelview_matrix_stack[size - 1]) can run first -- same [-1] aliasing
     * onto rdp's tail as in gfx_sp_matrix. */
    rsp.modelview_matrix_stack_size = 1;

    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

unsigned int total_t0, total_t1;

#if TARGET_PSP
/* Raised by the power callback after standby. Acted on here rather than in the
 * callback because this is the one point in the frame where the GE is provably
 * idle: gfx_end_frame has already run its sceGuFinish/sceGuSync, and nothing
 * of this frame has been queued yet. Wiping a texture cache the GE is still
 * reading from is the speckled corruption this file fights elsewhere. */
static volatile int sGfxResumePending;
unsigned int gPspGfxResumes;

/* Bumped by whoever loads a room; see gfx_scegu_draw_background. */
extern unsigned int gPspBgCacheGeneration;
void gfx_texture_cache_reset(void);

void PspGfx_NotifyResume(void) {
    sGfxResumePending = 1;
}
#endif

void gfx_start_frame(void) {
    //sceIoWrite(1, "----START FRAME!\n", 18);
    total_t0 = sceKernelLibcClock();
#if TARGET_PSP
    if (sGfxResumePending) {
        sGfxResumePending = 0;
        ++gPspGfxResumes;

        /* Standby powers the GE's eDRAM down, so every texture the cache
         * still claims to have in VRAM is gone -- the entries are valid, the
         * pixels behind them are not. Throw the cache away so the next draws
         * re-upload instead of sampling whatever survived. */
        gfx_texture_cache_reset();

        /* And force the fixed-camera background to be flushed to RAM again.
         * gfx_scegu_draw_background skips its 150 KB writeback when the image
         * pointer AND the generation both match the last blit -- an
         * optimisation that is exactly wrong across a resume, because the
         * room buffer is reused at the same address and the generation only
         * moves when a room loads. Walking back into the room you slept in
         * would otherwise blit whatever the GE finds in RAM. */
        ++gPspBgCacheGeneration;
    }
    if (sTexWipePending) {
        sTexWipePending = 0;

        /* gfx_texture_cache_lookup set this when the cache/VRAM was full,
         * instead of wiping right there while the GE could still be mid-list
         * over draws pointing into the memory a wipe hands out next -- see
         * the comment at that call site for the hardware capture that showed
         * this happening. This is the same idle point sGfxResumePending
         * above relies on: gfx_end_frame has already run its
         * sceGuFinish/sceGuSync for the PREVIOUS frame, and nothing of this
         * one has been queued yet, so it is safe to hand this frame's
         * textures out at addresses the last frame's draws used. */
        gfx_texture_cache_reset();
    }
    /* Without this the probe would latch onto the largest triangle ever drawn
     * in the session -- typically something from a long-gone scene -- instead
     * of the largest one in the picture being looked at. */
    gPspBigTriArea2Prev = gPspBigTriArea2;
    gPspBigTriArea2 = 0.0f;
    gPspL2Area2 = 0.0f;
#endif
    gfx_wapi->handle_events();
}

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
#if TARGET_PSP
    /* The GE's fog state does not survive whatever else touched the hardware
     * between frames, and neither does the cache in gfx_scegu_set_fog once the
     * display list is rebuilt. Start every frame not knowing, so the first draw
     * that cares establishes it. */
    rendering_state.fog_enabled = -1;
    gPspFogDraws = 0;
    gPspFogBadRange = 0;
    /* N36 lighting probe -- per frame, like the fog counters above. */
    gPspLightsMax = 0;
    gPspLightsOverOld = 0;
#endif
#if TARGET_PSP
    /* Rotate the stat generations *before* building this frame, so prev/prev2
     * always describe two fully completed, consecutive frames while the game
     * keeps running -- that is what makes an every-other-frame difference
     * readable from a single debugger memory read. */
    {
        uint32_t next_frame = gPspGfxStats.frame + 1;
        gPspGfxStatsPrev2 = gPspGfxStatsPrev;
        gPspGfxStatsPrev = gPspGfxStats;
        memset(&gPspGfxStats, 0, sizeof(gPspGfxStats));
        gPspGfxStats.magic = PSP_GFX_STATS_MAGIC;
        gPspGfxStats.frame = next_frame;

        gPspGfxDlTracePrev2 = gPspGfxDlTracePrev;
        gPspGfxDlTracePrev = gPspGfxDlTrace;
        memset(&gPspGfxDlTrace, 0, sizeof(gPspGfxDlTrace));
        gPspGfxDlTrace.magic = PSP_DL_TRACE_MAGIC;
        gPspGfxDlTrace.frame = next_frame;
        gPspGfxDlTrace.dl_top = (uint32_t)(uintptr_t)commands;

        /* gPspTexBindDesyncs lives in gfx_scegu.c next to the draw it guards and
         * so cannot be reset by the rotation above. Remember where it stood at
         * the start of this frame instead, and report the difference -- a
         * cumulative counter beside a single screenshot cannot say whether that
         * frame contributed anything, which cost a reading already. */
        {
            extern uint32_t gPspTexBindDesyncs;
            extern uint32_t gPspTexBindDesyncsFrameBase;

            gPspTexBindDesyncsFrameBase = gPspTexBindDesyncs;
        }

        gPspGfxMtxPrev2 = gPspGfxMtxPrev;
        gPspGfxMtxPrev = gPspGfxMtx;
        memset(&gPspGfxMtx, 0, sizeof(gPspGfxMtx));
        gPspGfxMtx.magic = PSP_GFX_MTX_MAGIC;
        gPspGfxMtx.frame = next_frame;
        /* Out of range until the frame's first modelview load, so anything
         * drawn before it is attributed to no slot rather than to slot 0. */
        sPspMtxCurSlot = PSP_MTX_MV_SLOTS;
    }

    sPspGfxDlDepth = 0;
    gPspGfxOpcodeGuardCount = 0;
    /* Same fix as Play_Draw's gSegments[8]/[9] reset (see project memory),
     * but for the INTERPRETER's own view of gSegments[], not the C-side
     * build-time view -- these are two separate points in time. Player's
     * eye/mouth gSPSegment(8/9,...) commands only actually update
     * gSegments[] when THIS interpreter walk reaches and executes them,
     * partway through the frame -- so at the very start of a frame's
     * interpretation, gSegments[8]/[9] still hold whatever value the
     * PREVIOUS frame's interpretation last left them at. If that stale
     * value's segment number collides with a real native pointer used
     * elsewhere early in THIS frame's walk (e.g. gfxCtx->polyOpaBuffer
     * itself, a plain static native pointer that starts the whole
     * WORK_DISP -> polyOpaBuffer -> polyXluBuffer -> overlayBuffer chain
     * via the very first gSPBranchList), seg_addr() misresolves it and the
     * interpreter branches into unrelated memory instead of the real
     * buffer -- explaining why Player's own (correctly-written) draw
     * commands were never reached despite everything upstream being
     * correct. */
    gSegments[8] = 0;
    gSegments[9] = 0;
#endif

    //INFO_MSG("New frame");
    
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        GFXSTAT_INC(dropped);
        return;
    }
    dropped_frame = false;
    //double t0 = gfx_wapi->get_time();
    unsigned int t0 = sceKernelLibcClock();
    gfx_rapi->start_frame();
    gfx_run_dl(commands);
    gfx_flush();
#if TARGET_PSP
    /* gfx_run_dl's runaway guard already counts every interpreted command. */
    gPspGfxStats.dl_cmds = gPspGfxOpcodeGuardCount;
#endif
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
    //double t1 = gfx_wapi->get_time();
    unsigned int t1 = sceKernelLibcClock();
    //printf("Process %f %f\n", t1, t1 - t0);
    //printf("Process %d microsec, %f sec\n", t1 - t0, (t1 - t0)/1000000.0f);
    times[frame_counter] = (t1 - t0)/1000.0f;
    frame_counter++;
    time_first_200  += (t1 - t0)/1000.0f;
    total_frame_counter++;
    if(frame_counter>=30){
        frame_counter = 0;
        int i;
        for(i=0;i<30;i++)
            time_avg += times[i];
        time_avg /= 30;
        //printf("GFX AVG: %2.3f ms FPS %2.3f\n", time_avg, 1000/time_avg);
    }
    if(total_frame_counter == 200){
        printf("GFX FRAME 250 TIME TAKEN: %2.3f ms FPS %2.3f, AVG: %2.3f ms \n",  time_first_200, (250*1000)/time_first_200, 1000/(250/time_first_200));
    }
}

unsigned int gfx_pc_stat_tris_drawn(void) {
    return gPspGfxStatsPrev.tris_drawn;
}

unsigned int gfx_pc_stat_tex_imports(void) {
    return gPspGfxStatsPrev.tex_imports;
}

unsigned int gfx_pc_stat_tex_hits(void) {
    return gPspGfxStatsPrev.tex_hits;
}

/* The CURRENT frame's counters, for the screenshot writer.
 *
 * Not Prev: the grab happens in gfx_scegu's end-of-frame path, after the GE
 * has gone idle and before the next gfx_run rotates the generations -- so the
 * frame in the picture is the one still sitting in gPspGfxStats. Reading Prev
 * there would label the image with the previous frame's numbers, which is
 * exactly the picture-and-counters mismatch this whole mechanism exists to
 * prevent.
 *
 * One out-parameter block rather than eleven accessors: the caller wants all
 * of them at the same instant anyway. */
void gfx_pc_stat_snapshot_current(struct GfxPcFrameSnapshot *out) {
    if (out == NULL) {
        return;
    }
    out->frame        = gPspGfxStats.frame;
    out->tris_drawn   = gPspGfxStats.tris_drawn;
    out->tri_calls    = gPspGfxStats.tri_calls;
    out->flushes      = gPspGfxStats.flushes;
    out->tex_imports  = gPspGfxStats.tex_imports;
    out->tex_hits     = gPspGfxStats.tex_hits;
    out->tex_used     = gPspGfxStats.tex_used;
    out->tex_unused   = gPspGfxStats.tex_unused;
    out->settimg      = gPspGfxStats.settimg;
    out->loadblock    = gPspGfxStats.loadblock;
    out->loadtile     = gPspGfxStats.loadtile;
    out->settile      = gPspGfxStats.settile;
    /* sky_tris is per-frame (reset at the BEGIN marker); sky_begins and
     * sky_calls are running totals since boot, hence the "Total" suffix on
     * their labels in the file. Mixing the two silently would invite reading a
     * cumulative number as this frame's. */
    out->sky_tris     = gPspSkyTri[0];
    out->sky_begins   = gPspSkyTri[1];
    out->sky_calls    = gPspSkyCall[0];
    out->sky_id       = gPspSkyCall[1];
    out->sky_drawtype = gPspSkyCall[2];
    out->tex_unswap_yes  = gPspGfxStats.tex_unswap_yes;
    out->tex_unswap_no   = gPspGfxStats.tex_unswap_no;
    out->sky_tex_imports = gPspGfxStats.sky_tex_imports;
    out->sky_tex_unswap  = gPspGfxStats.sky_tex_unswap;
    out->sky_tex_hits    = gPspGfxStats.sky_tex_hits;
    out->tex_off_draws   = gPspGfxStats.tex_off_draws;
    out->tex_sc0_draws   = gPspGfxStats.tex_sc0_draws;
    {
        extern uint32_t gPspTccRgbNoAlphaOpt, gPspTccRgbNoTexelRow, gPspTccRgbaOk;
        out->tcc_rgb_no_alpha_opt = gPspTccRgbNoAlphaOpt;
        out->tcc_rgb_no_texel_row = gPspTccRgbNoTexelRow;
        out->tcc_rgba_ok          = gPspTccRgbaOk;
    }
    out->lights_max      = gPspLightsMax;
    out->lights_over_old = gPspLightsOverOld;
    out->amb_color       = gPspGfxStats.amb_color;
    out->lit_color       = gPspGfxStats.lit_color;
    out->fog_draws       = gPspFogDraws;
    out->fog_bad_range   = gPspFogBadRange;
    out->sky_seg0        = gPspSkyCall[8];
    out->sky_seg0_native = (unsigned int)(gPspSkyCall[8] != 0 &&
        PspStaticAssetIsStatic((const void *)(uintptr_t)gPspSkyCall[8]) != 0);
    out->sky_pal         = gPspSkyCall[9];
    out->sky_pal_native  = (unsigned int)(gPspSkyCall[9] != 0 &&
        PspStaticAssetIsStatic((const void *)(uintptr_t)gPspSkyCall[9]) != 0);
    {
        /* Cumulative since boot, not per-frame: the counter lives in gfx_scegu.c
         * next to the draw it guards, and a per-frame reset there would have to
         * reach across into this file's generation rotation. Two consecutive
         * shots give the per-frame figure by subtraction, and the automatic grab
         * always takes three. */
        extern uint32_t gPspTexBindDesyncs;
        extern uint32_t gPspTexBindDesyncs2nd;

        extern uint32_t gPspTexBindDesyncsFrameBase;

        out->bind_desyncs       = gPspTexBindDesyncs;
        out->bind_desyncs_frame = gPspTexBindDesyncs - gPspTexBindDesyncsFrameBase;
        out->bind_desyncs_2nd = gPspTexBindDesyncs2nd;
    }
    out->lerp2_draws = gPspLerp2Draws;
}

void gfx_end_frame(void) {
    
    //sceIoWrite(1, "----END FRAME!\n", 16);
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
    total_t1 = sceKernelLibcClock();
    float delta = (total_t1 - total_t0)/1000.0f;
    (void)delta;
    if(frame_counter>=29){
        //printf("TOTAL TIME FRAME: %2.3f ms FPS %2.3f\n", delta, 1000/delta);
    }
}
