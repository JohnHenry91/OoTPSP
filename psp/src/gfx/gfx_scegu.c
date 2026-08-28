#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#include <stdint.h>
#include "psp_screenshot.h"
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "ultra64.h"

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <string.h>

#include <pspge.h>

#include "psp_texture_manager.h"
#include "psp_scene_menu.h"
#include "psp_frame_pace.h"

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

static void *sFbp0;
static void *sFbp1;

float identity_matrix[4][4] __attribute__((aligned(16))) = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

/* Shader IDs
id        alp fog edg nse ut0 ut1 num sin0 sin1 mul0 mul1 mix0 mix1 cas
-----------------------------------------------------------------------
69        0   0   0   0   1   0   1   0    1    1    1    1    1    0
512       0   0   0   0   0   0   1   1    1    0    1    0    1    0
909       0   0   0   0   1   0   1   0    1    0    1    1    1    0
1361      0   0   0   0   1   0   2   0    1    0    1    1    1    0
2560      0   0   0   0   1   0   0   1    1    0    1    0    1    0
17059909  1   0   0   0   1   0   1   0    0    1    1    1    1    1
17062400  1   0   0   0   1   0   1   1    0    0    1    0    1    0
17305729  1   0   0   0   0   0   2   0    0    1    1    1    1    1
18092101  1   0   0   0   1   0   1   0    0    1    1    1    1    0
18874437  1   0   0   0   1   0   1   0    1    1    0    1    0    0
18874880  1   0   0   0   0   0   1   1    1    0    0    0    0    1
18875277  1   0   0   0   1   0   1   0    1    0    0    1    0    0
18876928  1   0   0   0   1   0   1   1    1    0    0    0    0    0
27263045  1   0   0   0   1   0   1   0    1    1    0    1    0    0
27265536  1   0   0   0   1   0   0   1    1    0    0    0    0    1
27265647  1   0   0   0   1   1   1   0    1    0    0    1    0    0
52428869  1   1   0   0   1   0   1   0    1    1    0    1    0    0
52429312  1   1   0   0   0   0   1   1    1    0    0    0    0    1
52431360  1   1   0   0   1   0   1   1    1    0    0    0    0    0
84168773  1   0   1   0   1   0   1   0    0    1    1    1    1    1
85983744  1   0   1   0   0   0   1   1    1    0    0    0    0    1
94374400  1   0   1   0   1   0   0   1    1    0    0    0    0    1
127928832 1   1   1   0   1   0   0   1    1    0    0    0    0    1
153092165 1   0   0   1   1   0   1   0    1    1    0    1    0    0
153092608 1   0   0   1   0   0   1   1    1    0    0    0    0    1
153093005 1   0   0   1   1   0   1   0    1    0    0    1    0    0
153094656 1   0   0   1   1   0   1   1    1    0    0    0    0    0

printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", shader_id,
    cc_features.opt_alpha,
    cc_features.opt_fog,
    cc_features.opt_texture_edge,
    cc_features.opt_noise,
    cc_features.used_textures[0],
    cc_features.used_textures[1],
    cc_features.num_inputs,
    cc_features.do_single[0],
    cc_features.do_single[1],
    cc_features.do_multiply[0],
    cc_features.do_multiply[1],
    cc_features.do_mix[0],
    cc_features.do_mix[1],
    cc_features.color_alpha_same
);
*/

/* Shader Working List:
84168773    - Menu Overlays
*/

/* Shader Broken List:
153092165   - Noise
153092608   - Noise
153093005   - Noise
153094656   - Noise
*/

/* Started EMPTY for OoT (plan step 7) -- SM64's 27-entry table above encoded
 * SM64's own material set (see comment block above: "id -> alp fog edg nse
 * ut0 ut1 num sin0 sin1 mul0 mul1 mix0 mix1 cas" bit-pattern classification
 * of its color-combiner modes into a PSP GU_TFX_* mode + special-case hacks
 * further down this file, e.g. "mario's eyes", "peach letter").
 *
 * IMPORTANT finding while building this table for real: this array (and
 * shader_remap/shader_broken) is NOT what does the actual CC-mode -> PSP
 * GU_TFX_* classification -- that happens generically in
 * gfx_scegu_create_and_load_new_shader/gfx_scegu_apply_shader below, driven
 * by gfx_cc_get_features(shader_id) for ANY id, known or not. This table is
 * only consulted for the "known broken, remap to a working substitute"
 * override path (is_shader_enabled/get_shader_remap) -- i.e. it's for
 * *hand-tuned exceptions* discovered by actually looking at what renders
 * wrong, not a prerequisite for basic rendering. So: an empty table doesn't
 * block rendering, it just means no exceptions have been hand-tuned yet.
 *
 * Built empirically via a temporary file-logging hook in
 * gfx_scegu_create_and_load_new_shader (see LogUnknownShaderId below) --
 * PPSSPP homebrew can't write to the game's own directory, had to use the
 * "ms0:/" virtual memstick path, which maps to
 * ~/.var/app/org.ppsspp.PPSSPP/config/ppsspp/ on this host (see reference
 * memory notes). Only 3 distinct shader IDs occur across the entire
 * Setup->ConsoleLogo boot sequence -- OoT's actual material variety will
 * grow this list a lot once more scenes are in scope, but for Phase 1 this
 * is the complete set. None needed remapping/marked broken yet (nothing
 * empirically identified as rendering wrong enough to need a hack) --
 * revisit shader_broken/shader_remap once a specific texture is visibly
 * incorrect. */
#define NUM_SHADER_IDS 3

static uint32_t shader_ids[NUM_SHADER_IDS] = {
    18874880,
    16779776,
    27265536,
};
static uint32_t shader_remap[NUM_SHADER_IDS * 2] = {
    18874880, 18874880,
    16779776, 16779776,
    27265536, 27265536,
};
static uint32_t shader_broken[NUM_SHADER_IDS] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };

unsigned int __attribute__((aligned(64))) list[262144 * 2];

/* How much of `list` a frame actually uses, and the worst frame so far.
 *
 * Everything else came back clean on the run that stalled: tex=0/0 (texture
 * pool never wrapped), rst=0/0 (no mid-frame cache wipe), dl=0 (no display
 * list cursor refused), zfail=0, blob=72/0. The trace ends durably at
 * "ge-finish", i.e. immediately before sceGuSync -- the one call that blocks
 * on the graphics hardware -- and nothing, not even the audio thread, logged
 * another line. That is a stalled GE.
 *
 * This buffer is the remaining way to stall one from our side. It runs in
 * GU_DIRECT, so the GE executes the list as the CPU writes it, and
 * sceGuGetMemory carves the per-draw vertex arrays out of the SAME buffer
 * with no bounds check anywhere in pspsdk. Overrun it and the GE walks off
 * the end into whatever follows. Hyrule Field is the largest scene in the
 * game, which makes it the one most likely to get there.
 *
 * sceGuFinish() returns the finished list's size, so this costs one store per
 * frame and needs no instrumentation of the draw path at all. */
unsigned int gPspGeListBytes;
unsigned int gPspGeListPeak;

static unsigned int staticOffset = 0;
unsigned int scegu_fog_color = 0;

static unsigned int getMemorySize(unsigned int width, unsigned int height, unsigned int psm) {
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

#define TEX_ALIGNMENT (16)
/* How much spill actually got reserved, for the HUD -- the difference between
 * "the budget is too small" and "the region was never allocated" is not
 * visible from any other number. */
unsigned int gPspTexSpillBytes;

void *getStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm) {
    unsigned int memSize = getMemorySize(width, height, psm);
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return result;
}

void *getStaticVramBufferBytes(size_t bytes) {
    unsigned int memSize = bytes;
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return (void *) (((unsigned int) result) + ((unsigned int) sceGeEdramGetAddr()));
}

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "attributes.h"

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

struct SamplerState {
    int min_filter;
    int mag_filter;
    int wrap_s;
    int wrap_t;
    uint32_t tex;
};

typedef struct Vertex {
    float u, v;
    unsigned int color;
    float x, y, z;
} Vertex;

typedef struct VertexColor {
    unsigned short a, b;
    unsigned long color;
    unsigned short x, y, z;
} VertexColor;

/* Sized to match COLOR_COMBINER_POOL_SIZE in gfx_pc.c -- one shader per
 * distinct combiner is the worst case, and the two pools are recycled together
 * so they must not run out at different times. See the comment there. */
#define SHADER_PROGRAM_POOL_SIZE 512
static struct ShaderProgram shader_program_pool[SHADER_PROGRAM_POOL_SIZE];
static uint16_t shader_program_pool_size;
uint32_t gPspShaderPoolHighWater; /* read with the debugger; 64 == we hit the cap */
uint32_t gPspShaderPoolClamped;
static struct ShaderProgram *cur_shader = NULL;
static struct SamplerState tmu_state[2];
static bool gl_blend = false;

/* Logs distinct never-seen shader IDs to a real file (not fd 2 -- that
 * turned out to not reach any log we can read in this dev environment, see
 * reference memory notes) so we can build OoT's real shader_ids/
 * shader_remap/shader_broken table empirically (plan step 7 / roadmap: "run
 * the game, log every unrecognized shader ID, then classify"). Opened once
 * lazily, kept open for the process lifetime (PPSSPP flushes on exit/close
 * is unreliable, so this reopens in append mode each write instead, cheap
 * enough at the rate distinct new IDs actually appear). TEMPORARY -- remove
 * once the real table is built. */
#define MAX_LOGGED_SHADER_IDS 256
static uint32_t sLoggedShaderIds[MAX_LOGGED_SHADER_IDS];
static int sNumLoggedShaderIds = 0;

static void LogUnknownShaderId(uint32_t id) {
    for (int i = 0; i < sNumLoggedShaderIds; i++) {
        if (sLoggedShaderIds[i] == id) {
            return; /* already logged */
        }
    }
    if (sNumLoggedShaderIds < MAX_LOGGED_SHADER_IDS) {
        sLoggedShaderIds[sNumLoggedShaderIds++] = id;
    }

    /* Off by default, same reasoning as phase2_debug_log.c's
     * PSP_DEBUG_LOG_ENABLED switch. Note this one had a latent trap: the
     * de-dupe array above stops *recording* once it is full, but the write
     * below is not inside that `if`, so past 256 distinct combiner IDs every
     * single call would hit the filesystem again -- a per-draw-call I/O storm
     * in a busy scene. Set to 1 only when collecting IDs on purpose. */
#if 0
    char msg[32];
    int len = sprintf(msg, "%u\n", id);
    SceUID fd = sceIoOpen("ms0:/shader_log.txt", PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, len);
        sceIoClose(fd);
    }
#endif
}

static inline uint32_t get_shader_index(uint32_t id) {
    size_t i;
    for (i = 0; i < NUM_SHADER_IDS; i++) {
        if (shader_ids[i] == id) {
            return i;
        }
    }
    LogUnknownShaderId(id);
    return 0;
}

static inline uint32_t get_shader_remap(uint32_t id) {
    size_t index = get_shader_index(id);
    return shader_remap[index * 2 + 1];
}

static inline bool is_shader_enabled(uint32_t id) {
    size_t i;
    for (i = 0; i < NUM_SHADER_IDS; i++) {
        if (shader_broken[i] == id) {
            return false;
        }
    }
    return true;
}

static struct ShaderProgram *get_shader_from_id(uint32_t id) {
    size_t i;
    for (i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static bool gfx_scegu_z_is_from_0_to_1(void) {
    return true;
}

/* Does this combine's ALPHA actually come from the texture?
 *
 * cc.c[0] is the RGB row and cc.c[1] the ALPHA row of the reduced combine.
 * sceGuTexFunc's second argument decides whether the texture's alpha channel
 * participates at all, and we were passing GU_TCC_RGBA unconditionally -- i.e.
 * every draw multiplied by the texture's alpha whether its combine asked for
 * it or not.
 *
 * That is what made the walls half-transparent. hakaana2's walls are i4/i8
 * intensity textures, and real RDP I-format hardware outputs A = I (alpha
 * equals intensity, which import_texture_i4/i8 correctly reproduce). Their
 * combine's alpha row is the constant 1, so on N64 the wall is opaque and the
 * intensity only ever drives colour. Here the intensity was leaking into alpha,
 * so every dark masonry texel became see-through -- darker stone, more
 * transparent, which is exactly the observed look.
 *
 * GU_TCC_RGB takes RGB from the texture and alpha from the vertex alone, which
 * is what a constant alpha row means. */
static inline int tcc_for_alpha(const struct ShaderProgram *prg) {
    if (prg->cc.opt_alpha) {
        for (int i = 0; i < 4; i++) {
            uint8_t v = prg->cc.c[1][i];

            if (v == SHADER_TEXEL0 || v == SHADER_TEXEL0A || v == SHADER_TEXEL1) {
                return GU_TCC_RGBA;
            }
        }
    }
    return GU_TCC_RGB;
}

static inline int texenv_set_color(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

extern uint32_t gRdpPrimColorPacked;
extern uint32_t gRdpEnvColorPacked;
extern bool gfx_get_alpha_blend_state(void);

/* OoT N64-logo cube: real combine is 2-cycle
 * (TEXEL0-PRIMITIVE)*ENV_ALPHA+TEXEL0 then (PRIMITIVE-ENVIRONMENT)*COMBINED+ENVIRONMENT
 * -- PSP's single-stage fixed-function GE can't do genuine 2-cycle math, so
 * approximate cycle 2 the same way DaedalusX64's PSP HLE graphics backend
 * does for its own hand-identified "Zelda OoT logo / flames" blend mode
 * (Source/SysPSP/HLEGraphics/BlendModes.cpp, BlendMode_0x00272c60350ce37fLL):
 * force the vertex/material color to ENV (already happens for free here --
 * see the generic per-vertex color-input loop in gfx_pc.c, ENV is the last
 * matched input for this shader's reduced pattern) and blend it against the
 * texture using PRIM as the fixed texture-environment color via
 * GU_TFX_BLEND. Detected structurally (SHADER_TEXEL0/INPUT_1/INPUT_2/TEXEL0)
 * rather than by hardcoded shader_id, since the id shifts if opt flags
 * (fog/alpha) change. */
extern int gPspBootLogoActive;

static inline bool is_n64_logo_cube_combine(struct ShaderProgram *prg) {
    /* Gated on gPspBootLogoActive: this structural shape (TEXEL0, IN1, IN2,
     * TEXEL0) doesn't actually identify the logo cube specifically -- it's
     * the generic "(TEXEL0-X)*Y+TEXEL0" idiom, which real OoT gameplay
     * materials (e.g. shade/env-modulated stone walls) also collapse to.
     * Restricting this hack to the boot-logo phase avoids misapplying it
     * (and the stale/never-set gRdpPrimColorPacked tex-env-color it forces)
     * to unrelated real geometry once gameplay starts. */
    return gPspBootLogoActive && prg->cc.c[0][0] == SHADER_TEXEL0 && prg->cc.c[0][1] == SHADER_INPUT_1 &&
           prg->cc.c[0][2] == SHADER_INPUT_2 && prg->cc.c[0][3] == SHADER_TEXEL0;
}

/* Same technique, "NINTENDO 64" text quads (ConsoleLogo_Draw,
 * src/overlays/gamestates/ovl_title/z_title.c:141): combine is
 * (TEXEL1-PRIMITIVE)*ENV_ALPHA+TEXEL0 then (PRIMITIVE-ENVIRONMENT)*COMBINED+ENVIRONMENT
 * -- same PRIM/ENV cycle-2 shape as the cube, just cycle-1's first operand is
 * TEXEL1 instead of TEXEL0 (a second, simultaneously-bound glyph-shine
 * texture our single-TMU pipeline can't really represent either). Classified
 * SH_MT_TEXTURE_TEXTURE (both used_textures[0] and [1] true) rather than
 * SH_MT_TEXTURE_COLOR, so it never reached texenv_set_texture_color's
 * dispatch at all -- was falling into the generic "Bowser/Peach Paintings"
 * GU_TFX_DECAL hack, which ignores PRIM/ENV entirely (raw texture only,
 * explaining the flat white blowout: whichever texture ends up bound here
 * is mostly high-intensity). Whichever single texture our TMU actually has
 * bound (TEXEL0, the per-character glyph row) still benefits from the same
 * PRIM-as-texenv-color/ENV-as-vertex-color blend as the cube, even without
 * true dual-texture blending. */
static inline bool is_n64_logo_text_combine(struct ShaderProgram *prg) {
    /* Same gPspBootLogoActive gating as is_n64_logo_cube_combine, same reason. */
    return gPspBootLogoActive && prg->cc.c[0][0] == SHADER_TEXEL1 && prg->cc.c[0][1] == SHADER_INPUT_1 &&
           prg->cc.c[0][2] == SHADER_INPUT_2 && prg->cc.c[0][3] == SHADER_TEXEL0;
}

static inline int texenv_set_texture_color(struct ShaderProgram *prg) {
    int mode;
    /*@Hack: lord forgive me for this, but this is easier */
    switch (prg->shader_id) {
        case 0x0000038D: // mario's eyes
        case 0x01045A00: // peach letter
        case 0x01200A00: // intro copyright fade in
            mode = GU_TFX_DECAL;
            break;
        case 0x00000551: // goddard
            mode = GU_TFX_BLEND;
            break;
        default:
            mode = is_n64_logo_cube_combine(prg) ? GU_TFX_BLEND : GU_TFX_MODULATE;
            break;
    }

    return mode;
}

static inline int texenv_set_texture_texture(struct ShaderProgram *prg) {
    if (is_n64_logo_text_combine(prg)) {
        return GU_TFX_BLEND;
    }

    /* MODULATE, not DECAL -- this follows DaedalusX64, which solves the same
     * problem on the same hardware (it is an N64 emulator for the PSP, so its
     * GE has exactly one texture unit too).
     *
     * Source/SysPSP/HLEGraphics/RendererPSP.cpp, in the render-state loop:
     *
     *     // NB if install_texture0 and install_texture1 are both set,
     *     // 0 wins out
     *     texture_idx = install_texture0 ? 0 : 1;
     *
     * i.e. when a combine wants two textures, pick TEXEL0 and treat it as an
     * ordinary single-texture draw. (Daedalus keeps a per-ROM T1_HACK flag for
     * the handful of games where TEXEL1 is the one that matters, and decomposes
     * genuinely two-stage combines into multiple passes -- worth copying if we
     * ever need it.)
     *
     * DECAL was actively wrong here: it mixes by the TEXTURE's alpha and falls
     * back to the vertex colour where that alpha is 0, so a draw whose texture
     * has no alpha renders as flat vertex colour -- invisible against a black
     * background. That is what hid the skybox: every skybox face uses
     * SETUPDL_40, whose combine is
     *     (TEXEL1 - TEXEL0) * PRIMITIVE_ALPHA + TEXEL0
     * and whose PRIMITIVE alpha is the time-of-day blend, 0 outside a
     * transition -- so the correct result is exactly TEXEL0, which is precisely
     * what Daedalus's rule produces.
     *
     * The DECAL default came from sm64-port's Bowser/Peach painting hack
     * (shader 0x1A00A6F), which no OoT content reaches.
     *
     * REPLACE rather than MODULATE, and that distinction is the whole point:
     * in (A - B) * X + B the X operand is an INTERPOLATION FACTOR, not a tint,
     * so nothing may multiply the surviving texel. gfx_pc.c's reduced combiner
     * has no way to express that -- it picks the one colour register it sees
     * (here CC_PRIM) and hands it over as the vertex colour, and Skybox_Draw
     * sets that register to gDPSetPrimColor(..., 0, 0, 0, blend): RGB (0,0,0),
     * with only the ALPHA carrying the time-of-day blend. MODULATE therefore
     * multiplied every skybox texel by black. REPLACE took the texel alone,
     * which is exactly the N64 result while the blend factor is 0.
     *
     * This stays the DEFAULT, but it is no longer the whole story: it is only
     * right when the cycle-1 register is the sole non-texel contribution.
     * The Chamber of the Sages waterfalls are the counter-example -- two I8
     * (intensity-only, i.e. grey) textures mixed in cycle 1, with
     * cycle 2 = COMBINED * SHADE supplying the blue. REPLACE threw that shade
     * away and the pillars rendered white. Those draws get MODULATE instead,
     * switched per draw by gfx_scegu_set_two_texture_tint() below, because
     * whether a cycle-2 tint exists is RDP state that cc_id (and therefore
     * this per-shader decision) does not capture. */
    return GU_TFX_REPLACE;
}

/* Render hacks, toggled live from the scene menu's hack page (SQUARE) rather
 * than rebuilt each time. These used to be #defines, which cost a full
 * rebuild-and-relaunch per experiment and -- worse -- made screenshots
 * ambiguous: a picture does not say which build produced it, and comparing two
 * shots taken under uncertain build states produced a wrong conclusion once
 * already. As runtime switches, both halves of an A/B come from one build, one
 * scene and one camera position, and the HUD names the active hack.
 *
 * gPspGfxHackNoTexture: draw every material with texturing off, so geometry
 * comes out in pure vertex/shade colour. Splits "is this the texture stage or
 * the vertex/lighting/combiner path" in one look.
 *
 * gPspGfxHackPointFilter: force GU_NEAREST everywhere. Makes texel size
 * directly visible, which separates "the texture is correctly tiled and merely
 * filtered soft" (fine detail appears) from "a handful of texels is being
 * stretched over the whole surface" (big hard-edged blocks). */
int gPspGfxHackNoTexture = 0;
int gPspGfxHackPointFilter = 0;

/* gPspGfxHackPreferTexel1: on a two-texture combine, keep TEXEL1 instead of
 * TEXEL0.
 *
 * Measured on Kokiri Forest's ground (2026-08-23): it is a two-texture material
 * whose TEXEL0 is a 32x32 RGBA16 colour map, CLAMPed on both axes, of which one
 * triangle uses about 8x20 texels stretched across the whole surface -- i.e.
 * deliberately low frequency. That is the N64's detail-texture idiom: TEXEL0
 * carries broad colour, TEXEL1 carries the fine grain, tiled many times over.
 *
 * The single-TMU rule below keeps TEXEL0 for every two-texture combine, so this
 * port systematically keeps the blurry half and discards the sharp one -- which
 * is exactly what "the ground is soft while the walls are crisp" looks like
 * (the walls are single-texture materials and are unaffected).
 *
 * This switch is a DIAGNOSTIC, not the fix: it trades the colour map away for
 * the detail rather than combining them. The real answer is a second pass that
 * multiplies the two, which is what the note in gfx_scegu_select_texture
 * describes an abandoned attempt at. */
int gPspGfxHackPreferTexel1 = 0;

/* Master switch for the two-texture terrain second pass (gfx_pc.c decides which
 * materials qualify). On by default -- it is the fix, not a diagnostic -- but
 * switchable so its cost and its correctness can be compared side by side in
 * one build. */
int gPspGfxLerp2Enable = 1;

/* Defined in gfx_pc.c, which is where the second pass is actually issued
 * (gfx_sp_tri1's own recursive call); see gPspLerp2Detected there. */
extern uint32_t gPspLerp2Draws;
extern int gPspLerp2SecondPass;

/* Draw the second pass at full strength instead of at the material's mix
 * factor. A diagnostic with one job: the ENV factor measured 128/128/128, i.e.
 * a plain 50/50 blend, yet the picture does not change -- so either the pass
 * draws the right thing and half of it is simply hard to see against a base of
 * the same colour, or it draws the wrong thing entirely. At full strength the
 * result must look exactly like the (known-good) Prefer-TEXEL1 diagnostic; if
 * it does not, the pass is not drawing what it is supposed to. */
int gPspLerp2Force;

/* Last texenv mode forced by gfx_scegu_set_two_texture_tint(); -1 == none. */
static int mode_override = -1;

static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
    if (gPspGfxHackNoTexture) {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }
    // If we have textures, Enable otherwise Disable
    if (prg->texture_used[0] || prg->texture_used[1]) {
        sceGuEnable(GU_TEXTURE_2D);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }
/*@Note: Revisit one day! */
#if 0
    if (prg->shader_id & SHADER_OPT_FOG) {
        // Yea this doesnt work at all */
        //sceGuFog(scegu_fog_near, scegu_fog_far, 0x00FF0000);//scegu_fog_color); // color is the same for all verts, only intensity is different
        //sceGuEnable(GU_FOG);
        sceGuEnable(GU_BLEND);
    }
#endif

    if (prg->num_inputs) {
        // have colors
        // TODO: more than one color (maybe glSecondaryColorPointer?)
        // HACK: if there's a texture and two colors, one of them is likely for speculars or some shit
        // (see mario head)
        //       if there's two colors but no texture, the real color is likely the second one
        /*
        const int hack = (prg->num_inputs > 1) * (4 - (int)prg->texture_used[0]);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, cur_buf_stride, ofs + hack);
        ofs += 4 * prg->num_inputs;
        */
    }

    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        // (horrible) alpha discard
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    if (!prg->enabled) {
        // configure formulae, we only need to do this once
        prg->enabled = true;

        int mode;
        switch (prg->mix) {
            case SH_MT_TEXTURE:
                mode = texenv_set_texture(prg);
                break;
            case SH_MT_TEXTURE_TEXTURE:
                mode = texenv_set_texture_texture(prg);
                break;
            case SH_MT_TEXTURE_COLOR:
                mode = texenv_set_texture_color(prg);
                break;
            default:
                mode = texenv_set_color(prg);
                break;
        }

        /* Transition Screens */
        if (prg->shader_id == 0x01A00045) {
            mode = GU_TFX_REPLACE;
        }
        sceGuTexFunc(mode, tcc_for_alpha(prg));
        mode_override = -1;
    }
}

/* Per-draw texenv override for two-texture combines.
 *
 * texenv_set_texture_texture() runs once per shader program and caches its
 * answer in prg->enabled, but the thing that decides between REPLACE and
 * MODULATE -- whether the combine's SECOND cycle multiplies the result by a
 * colour register -- is not part of cc_id (see gfx_pc.c's combine_cyc2_tint,
 * deliberately kept out of shader ids). The same shader program can therefore
 * be used both with and without a tint, so gfx_pc.c calls this on the draws
 * that care.
 *
 * `mode_override` is reset whenever a shader is (re)applied, since that path
 * issues its own sceGuTexFunc and would otherwise leave this cache stale. */
void gfx_scegu_set_two_texture_tint(int has_tint) {
    if (cur_shader == NULL || cur_shader->mix != SH_MT_TEXTURE_TEXTURE) {
        return;
    }
    /* The boot logo has its own approximation, don't disturb it. */
    if (is_n64_logo_text_combine(cur_shader) || is_n64_logo_cube_combine(cur_shader)) {
        return;
    }

    const int mode = has_tint ? GU_TFX_MODULATE : GU_TFX_REPLACE;

    if (mode != mode_override) {
        mode_override = mode;
        sceGuTexFunc(mode, tcc_for_alpha(cur_shader));
    }
}

static void gfx_scegu_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg)) {
        cur_shader->enabled = false;
        cur_shader = NULL;
    }
}

static void gfx_scegu_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    gfx_scegu_apply_shader(cur_shader);
    if (cur_shader)
        cur_shader->enabled = false;
}

/* Called by gfx_pc.c's gfx_lookup_or_create_color_combiner when it recycles the
 * combiner pool -- see the long comment there for why the two pools have to be
 * dropped together rather than one at a time. Safe only immediately after a
 * gfx_flush(): it invalidates every ShaderProgram pointer handed out so far. */
/* Is the shader pool out of room? gfx_pc.c checks this before creating a
 * combiner, because a new combine mode can need a new entry in BOTH pools and
 * they have to be recycled together. */
int gfx_scegu_shader_pool_full(void) {
    return shader_program_pool_size >= SHADER_PROGRAM_POOL_SIZE;
}

void gfx_scegu_reset_shader_pool(void) {
    shader_program_pool_size = 0;
    cur_shader = NULL;
}

static struct ShaderProgram *gfx_scegu_create_and_load_new_shader(uint32_t shader_id) {
    LogUnknownShaderId(shader_id); /* every distinct ID actually used, not just remap misses */

    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    /* Belt and braces against the same unchecked post-increment gfx_pc.c's
     * combiner pool had: this array is immediately followed in .bss by
     * staticOffset, the VRAM bump allocator's cursor, so running off the end
     * corrupts framebuffer allocation.
     *
     * This must NOT reset the pool on its own. ColorCombiners in gfx_pc.c hold
     * raw ShaderProgram pointers, so handing slot 0 back out while those are
     * still live makes two different combiners share (and overwrite) one
     * program -- silently wrong colours, which is worse than the overflow it
     * was guarding. Recycling is gfx_lookup_or_create_color_combiner's job
     * because only it can drop BOTH pools together; gfx_scegu_shader_pool_full
     * below is how it knows to. Reaching here at all means that proactive check
     * failed, so degrade to reusing the last slot -- no out-of-bounds write --
     * and say so in the counter. */
    if (shader_program_pool_size >= SHADER_PROGRAM_POOL_SIZE) {
        ++gPspShaderPoolClamped;
        shader_program_pool_size = SHADER_PROGRAM_POOL_SIZE - 1;
    }

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    if (shader_program_pool_size > gPspShaderPoolHighWater) {
        gPspShaderPoolHighWater = shader_program_pool_size;
    }

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0];
    prg->texture_used[1] = ccf.used_textures[1];

    if (ccf.used_textures[0] && ccf.used_textures[1]) {
        prg->mix = SH_MT_TEXTURE_TEXTURE;
        if (ccf.do_single[1]) {
            prg->texture_ord[0] = 1;
            prg->texture_ord[1] = 0;
        } else {
            prg->texture_ord[0] = 0;
            prg->texture_ord[1] = 1;
        }
    } else if (ccf.used_textures[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_textures[0]) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    }

    prg->enabled = false;

    gfx_scegu_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_scegu_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_scegu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

static uint32_t gfx_scegu_new_texture(void) {
    return texman_create();
}

/* The PSP GE's sceGuTexWrap only has REPEAT and CLAMP -- no per-axis mirror --
 * so G_TX_MIRROR silently falls through to plain REPEAT below. That IS a real
 * gap: HAKAdan's walls use G_TX_MIRROR heavily, and per the RDP docs mirroring
 * flips every 2^mask texels (the mask'th bit of the coordinate selects the
 * mirrored copy), which plain REPEAT cannot reproduce. The real fix is a
 * doubled, pre-mirrored texture upload, plus honouring masks/maskt (currently
 * discarded in gfx_dp_set_tile).
 *
 * TESTED AND REFUTED as the cause of the Bottom of the Well wall glitch:
 * forcing mirror axes to CLAMP left that artifact pixel-identical, across a
 * fresh scene load so the sampler state was genuinely re-bound. Kept as an A/B
 * switch for whoever implements the real mirror support. */
int gDebugMirrorAsClamp = 0;
static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP)
        return GU_CLAMP;
    if ((val & G_TX_MIRROR) && gDebugMirrorAsClamp)
        return GU_CLAMP;
    return GU_REPEAT;
}

static inline int ispow2(uint32_t x) {
    return (x & (x - 1)) == 0;
}

// compute the next highest power of 2 of 32-bit v
static inline int nextpow2(int v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v++;
    return v;
}

uint32_t gPspWrapSamplerSets;

/* Last texture id actually handed to the GE, so gfx_pc.c can record which
 * texture each pass of the terrain LERP drew with. */
uint32_t gPspCurBoundTex;

static inline void gfx_scegu_apply_tmu_state(const int tile) {
    sceGuTexFilter(tmu_state[tile].min_filter, tmu_state[tile].mag_filter);
    sceGuTexWrap(tmu_state[tile].wrap_s, tmu_state[tile].wrap_t);
}

static void gfx_scegu_set_sampler_parameters(const int tile, const bool linear_filter, const uint32_t cms, const uint32_t cmt) {
    const int filter = (linear_filter && !gPspGfxHackPointFilter) ? GU_LINEAR : GU_NEAREST;

    const int wrap_s = gfx_cm_to_opengl(cms);
    const int wrap_t = gfx_cm_to_opengl(cmt);

    /* Does anything actually ask a non-pow2 texture to repeat? If this stays
     * zero, padding is safe everywhere and the stretch can go away outright. */
    if (wrap_s == GU_REPEAT || wrap_t == GU_REPEAT) {
        gPspWrapSamplerSets++;
    }

    tmu_state[tile].min_filter = filter;
    tmu_state[tile].mag_filter = filter;
    tmu_state[tile].wrap_s = wrap_s;
    tmu_state[tile].wrap_t = wrap_t;

    /* One physical TMU, so only ONE tile's sampler modes can be in the GE at a
     * time -- and it has to be the tile whose bind survives
     * gfx_scegu_select_texture. Normally that is tile 0; under the
     * prefer-TEXEL1 diagnostic and inside the terrain LERP's second pass it is
     * tile 1.
     *
     * This used to be a flat `if (!tile)`, i.e. tile 1's modes were recorded in
     * tmu_state[1] but never reached the hardware. The second pass then drew
     * the detail texture with tile 0's modes -- CLAMP/CLAMP on Kokiri Forest's
     * ground, where the detail tile needs to REPEAT about eight times. A
     * clamped detail tile is one stretched band, which on screen is
     * indistinguishable from "the second pass did nothing at all". */
    const int bound_tile = (gPspGfxHackPreferTexel1 || gPspLerp2SecondPass) ? 1 : 0;
    if (tile == bound_tile) {
        gfx_scegu_apply_tmu_state(tile);
    }
}

static void gfx_scegu_select_texture(int tile, uint32_t texture_id) {
    /* PSP GE has one real texture unit -- gfx_pc.c's texture-import loop
     * always processes tile 0 (TEXEL0) then tile 1 (TEXEL1), so tile 1's
     * bind physically wins regardless of which the RDP combine actually
     * wants visible. For the "NINTENDO 64" text (is_n64_logo_text_combine),
     * TEXEL1 is a static shine/gradient overlay loaded once
     * (gDPLoadMultiBlock in ConsoleLogo_Draw) while TEXEL0 is the real,
     * per-character glyph bitmap (gDPLoadTextureBlock, changes every draw)
     * -- keep TEXEL0 bound instead, since it's the one that actually
     * carries the visible text shape; the gradient TEXEL1 detail is lost,
     * an acceptable approximation given true dual-texture blending isn't
     * representable on this single-TMU pipeline at all.
     *
     * A real two-pass dual-texture implementation was attempted this
     * session (remember each tile's desired texture id, explicitly bind
     * tile 1 for a genuine second draw pass in gfx_scegu_draw_triangles/
     * _2d) and did technically work for the immediate boot-logo case, but
     * caused a real crash later in Play state: a completely unrelated
     * gameplay shader also got classified SH_MT_TEXTURE_TEXTURE, and
     * `tmu_state[1].tex` still held a stale texture id from the long-gone
     * boot-logo phase (nothing invalidates it when texman_clear() resets
     * the underlying texture pool) -- binding that stale id fed a garbage
     * VRAM address into sceGuTexImage, producing wild/sequential
     * "Bad memory access" reads and a hard exit. Reverted for safety.
     * Revisit with proper staleness tracking (e.g. invalidate tmu_state on
     * texman_clear(), or only trust a same-draw-sequence id) before
     * attempting real dual-texture support again. */
    /* GENERALISED (see below): keep TEXEL0 bound for EVERY two-texture
     * combine, which is the rule texenv_set_texture_texture already assumes
     * and quotes from Daedalus ("NB if install_texture0 and install_texture1
     * are both set, 0 wins out"). The code contradicted its own comment: tile
     * 1 is processed second, so its bind physically won.
     *
     * The Chamber of the Sages water columns show why TEXEL0 is the right
     * half to keep. Their combine is (TEXEL1 - TEXEL0) * ENV_ALPHA + TEXEL0,
     * where TEXEL0 (kenjyanoma 0x00012508) is the fine 32x64 ripple pattern
     * and TEXEL1 (0x000114E8) is a coarse blocky mask. The N64 scrolls the two
     * against each other into the wobbling mass; with one TMU, binding TEXEL1
     * rendered the bare blocks -- the drifting rectangles. TEXEL0 alone still
     * reads as water.
     *
     * Same argument as the boot logo's "NINTENDO 64" text, which this used to
     * special-case: TEXEL0 is the per-character glyph bitmap, TEXEL1 a static
     * shine overlay. The general rule covers it.
     *
     * gPspLerp2SecondPass (gfx_pc.c) flips this rule the same way
     * gPspGfxHackPreferTexel1 does, but scoped to the single recursive
     * gfx_sp_tri1 call that draws a terrain LERP's detail half: TEXEL1 needs
     * to be the one that survives for exactly that one triangle, twice
     * (entering and leaving the second pass), which is why gfx_pc.c forces
     * rdp.textures_changed on both edges to make sure this function actually
     * runs instead of being skipped as "nothing changed". */
    if (tile == 1 && cur_shader != NULL && cur_shader->texture_used[0] &&
        !(gPspGfxHackPreferTexel1 || gPspLerp2SecondPass)) {
        return;
    }
    /* The mirror image of the rule above: with either switch on, it is TEXEL0
     * that gets dropped, so that tile 1 -- processed second -- is the bind
     * that survives. */
    if (tile == 0 && cur_shader != NULL && cur_shader->texture_used[0] &&
        cur_shader->texture_used[1] && (gPspGfxHackPreferTexel1 || gPspLerp2SecondPass)) {
        return;
    }
    if (tmu_state[tile].tex != texture_id) {
        tmu_state[tile].tex = texture_id;
        gPspCurBoundTex = texture_id;
        texman_bind_tex(texture_id);
        /* Re-assert the sampler state this tile already holds, because
         * texman_bind_tex's sceGuTexMode/sceGuTexImage disturb it -- do NOT
         * overwrite it. This line used to call
         * gfx_scegu_set_sampler_parameters(tile, false, 0, 0), i.e. it forced
         * GU_NEAREST and wrap mode 0 on every texture change.
         *
         * That desynced two caches. gfx_pc.c remembers the applied filter and
         * clamp modes PER CACHED TEXTURE (rendering_state.textures[i]->
         * linear_filter/cms/cmt) and only re-applies them when its own
         * remembered value differs from what the RDP now asks for. It never
         * learned about the (false, 0, 0) written behind its back, so any
         * texture rebound after a previous use kept whatever the last binding
         * happened to leave in the hardware instead of its own modes. Only a
         * freshly cached texture (whose node starts at linear_filter = false)
         * reliably got its real settings. */
        gfx_scegu_apply_tmu_state(tile);
    }
}

/* Used for rescaling textures ROUGHLY into pow2 dims */
static unsigned int __attribute__((aligned(16))) scaled[256 * 256 * sizeof(unsigned int)]; /* 16kb */
static void gfx_scegu_resample_32bit(const unsigned int *in, int inwidth, int inheight, unsigned int *out, int outwidth, int outheight) {
    int i, j;
    const unsigned int *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_resample_16bit(const unsigned short *in, int inwidth, int inheight, unsigned short *out, int outwidth, int outheight) {
    int i, j;
    const unsigned short *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_resample_8bit(const unsigned char *in, int inwidth, int inheight, unsigned char *out, int outwidth, int outheight) {
    int i, j;
    const unsigned char *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

/* swizzle_fast (psp_texture_manager.c) organizes data in 8-row blocks
 * (height_blocks = height/8) -- for height < 8 that's 0, so its whole outer
 * loop never runs and NOT A SINGLE BYTE of the texture actually gets
 * written, leaving whatever was previously in that VRAM slot (effectively
 * garbage). Confirmed as the real cause of OoT's "NINTENDO 64" boot-logo
 * text rendering as a dashed/garbled mess: its per-character glyph texture
 * (ConsoleLogo_Draw, z_title.c) is only 2 texels tall. Real PSP homebrew
 * generally only swizzles textures tall enough for it to matter; fall back
 * to the plain (non-swizzled, already fully supported via texman_upload/
 * sceGuTexMode's own swizzle flag) upload path for anything shorter. */
static inline void texman_upload_swizzle_or_plain(int width, int height, unsigned int type, void *buf) {
    /* swizzle_fast() works in 16-byte x 8-row blocks and computes
     * width_blocks = width_bytes/16, height_blocks = height/8 -- for anything
     * smaller than one full block in either axis that count is 0, so it writes
     * *nothing at all* and the texture keeps whatever stale bytes the previous
     * texture left in that buffer. The height<8 half of this guard was already
     * here; the width side was missing, which matters for OoT specifically
     * (lots of tiny 1-2 texel wide gradient/ramp textures that SM64, whose
     * renderer this is derived from, never had). Fall back to a plain
     * (unswizzled) upload in both cases. */
    if (height < 8 || getMemorySize(width, 1, type) < 16) { /* getMemorySize(w,1,psm) == bytes per row */
        texman_upload(width, height, type, buf);
    } else {
        texman_upload_swizzle(width, height, type, buf);
    }
}

/* How much non-power-of-two texture traffic there actually is, and what it
 * looks like.
 *
 * The GE can only sample power-of-two textures, so anything else is currently
 * RESAMPLED up to the next power of two with a nearest-neighbour stretch --
 * which duplicates columns and rows and costs real sharpness. Daedalus, on the
 * same hardware, PADS instead (Source/SysPSP/Graphics/NativeTexturePSP.cpp:
 * mCorrectedWidth/mCorrectedHeight, with mScale = 1/mCorrectedWidth so the
 * coordinates are normalised by the padded size). Padding is texel-exact.
 *
 * It is not a free swap, though: padding only works where the texture does not
 * REPEAT, because repeating a padded image tiles the padding as well. So the
 * question that decides whether this is worth changing is not "how many
 * non-pow2 textures are there" but "how many of them wrap" -- hence the second
 * counter. Measure before rewriting the upload path. */
uint32_t gPspNonPow2Uploads;
uint32_t gPspPow2Uploads;
uint32_t gPspNonPow2LastDims; /* width << 16 | height */

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    if (ispow2(width) && ispow2(height)) {
        gPspPow2Uploads++;
    } else {
        gPspNonPow2Uploads++;
        gPspNonPow2LastDims = ((uint32_t)width << 16) | (uint32_t)(height & 0xFFFF);
    }
    if (ispow2(width) && ispow2(height)) {
        texman_upload_swizzle_or_plain(width, height, type, (void *) rgba32_buf);
    } else {
        int scaled_width = nextpow2(width);
        int scaled_height = nextpow2(height);

        if (type == GU_PSM_8888) {
            gfx_scegu_resample_32bit((const unsigned int *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
        } else if (type == GU_PSM_5551) {
            gfx_scegu_resample_16bit((const unsigned short *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
        } else {
            gfx_scegu_resample_8bit((const unsigned char *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
        }
        texman_upload_swizzle_or_plain(scaled_width, scaled_height, type, (void *) scaled);
    }
}

static void gfx_scegu_set_depth_test(bool depth_test) {
    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
}

static void gfx_scegu_set_depth_mask(bool z_upd) {
    sceGuDepthMask(z_upd ? GU_FALSE : GU_TRUE);
}

static void gfx_scegu_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        sceGuDepthOffset(32); /* I think we need a little more on psp because of 16bit depth buffer */
    } else {
        sceGuDepthOffset(0);
    }
}

static void gfx_scegu_set_viewport(int x, int y, int width, int height) {
    sceGuViewport(2048 - (SCR_WIDTH / 2) + x + (width / 2), 2048 + (SCR_HEIGHT / 2) - y - (height / 2), width, height);
    sceGuScissor(x, SCR_HEIGHT - y - height, x + width, SCR_HEIGHT - y);
}

static void gfx_scegu_set_scissor(int x, int y, int width, int height) {
    sceGuScissor(x, SCR_HEIGHT - y - height, x + width, SCR_HEIGHT - y);
}

static void gfx_scegu_set_use_alpha(bool use_alpha) {
    gl_blend = use_alpha;
    if (use_alpha) {
        sceGuEnable(GU_BLEND);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

// draws the same triangles as plain fog color + fog intensity as alpha
// on top of the normal tris and blends them to achieve sort of the same effect
// as fog would
static inline void gfx_scegu_blend_fog_tris(void) {
    /*@Todo: figure this out! */
    return;
#if 0
    // if a texture was used, replace it with fog color instead, but still keep the alpha
    if (cur_shader->texture_used[0]) {
        glActiveTexture(GL_TEXTURE0);
        TEXENV_COMBINE_ON();
        // out.rgb = input0.rgb
        TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_PRIMARY_COLOR);
        // out.a = texel0.a * input0.a
        TEXENV_COMBINE_SET2(ALPHA, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
    }

    glEnableClientState(GL_COLOR_ARRAY); // enable color array temporarily
    glColorPointer(4, GL_FLOAT, cur_buf_stride, cur_fog_ofs); // set fog colors as primary colors
    if (!gl_blend) glEnable(GL_BLEND); // enable blending temporarily
    glDepthFunc(GL_LEQUAL); // Z is the same as the base triangles

    glDrawArrays(GL_TRIANGLES, 0, 3 * cur_buf_num_tris);

    glDepthFunc(GL_LESS); // set back to default
    if (!gl_blend) glDisable(GL_BLEND); // disable blending if it was disabled
    glDisableClientState(GL_COLOR_ARRAY); // will get reenabled later anyway
#endif
}

/* sm64-port-psp's bundled libpspmath.a doesn't actually contain
 * memcpy_vfpu (confirmed via psp-nm) -- plain memcpy is functionally
 * identical, just not VFPU-accelerated; a reasonable simplification to
 * revisit if vertex upload turns out to be a real bottleneck. */

/* Two-pass emulation of the OoT N64-logo cube's real 2-cycle RDP combine
 * (see is_n64_logo_cube_combine above for the exact formula and why the
 * single-stage GU_TFX_BLEND hack there is only a rough approximation --
 * confirmed by the user against real reference footage that hues were
 * noticeably off, e.g. showing cyan/pink instead of the real logo's
 * green/blue/red faces).
 *
 * Cycle 1 = (TEXEL0-PRIM)*ENV_ALPHA + TEXEL0 = TEXEL0*(1+ENV_ALPHA) - PRIM*ENV_ALPHA
 * Cycle 2 = (PRIM-ENV)*COMBINED + ENV
 * Substituting cycle 1 into cycle 2 and expanding (PRIM/ENV/ENV_ALPHA are all
 * flat per-face-group constants -- the only thing that varies per-pixel is
 * TEXEL0, and it appears exactly once after expansion) collapses the whole
 * 2-cycle formula to a plain affine function of TEXEL0:
 *   final = TEXEL0 * K1 + K2
 *   K1 = (PRIM-ENV) * (1+ENV_ALPHA)
 *   K2 = ENV - (PRIM-ENV) * PRIM * ENV_ALPHA
 * (all channels normalized to 0..1, K1/K2 independently clamped back to
 * 0..1 afterward -- real values can occasionally fall slightly outside that
 * range, e.g. K1 up to ~2.0, and PSP vertex colors can't represent that
 * directly; clamping loses a bit of dynamic range at the extremes but keeps
 * every group's hue correct across the bulk of the texture's intensity
 * range, which is what was actually wrong before this fix).
 *
 * Rendered as: pass 1 draws TEXEL0*K1 via GU_TFX_MODULATE with the vertex
 * color forced to K1 (opaque, replaces the framebuffer); pass 2 re-draws
 * the exact same buffered geometry with texturing disabled, vertex color
 * forced to K2, and additive blending, adding the flat K2 term on top.
 * Doesn't apply to the "NINTENDO 64" text (is_n64_logo_text_combine): that
 * combine's cycle 1 mixes TWO different textures (TEXEL0 and TEXEL1) with
 * no repeated single term, so it can't be collapsed the same way. */
static inline uint8_t clamp_u8_from_unit(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

static void gfx_scegu_draw_n64_logo_cube_2pass(void *buf, size_t num_verts) {
    float pr = (gRdpPrimColorPacked & 0xFF) / 255.0f;
    float pg = ((gRdpPrimColorPacked >> 8) & 0xFF) / 255.0f;
    float pb = ((gRdpPrimColorPacked >> 16) & 0xFF) / 255.0f;
    float er = (gRdpEnvColorPacked & 0xFF) / 255.0f;
    float eg = ((gRdpEnvColorPacked >> 8) & 0xFF) / 255.0f;
    float eb = ((gRdpEnvColorPacked >> 16) & 0xFF) / 255.0f;
    float ea = ((gRdpEnvColorPacked >> 24) & 0xFF) / 255.0f;

    float k1r = (pr - er) * (1.0f + ea), k1g = (pg - eg) * (1.0f + ea), k1b = (pb - eb) * (1.0f + ea);
    float k2r = er - (pr - er) * pr * ea, k2g = eg - (pg - eg) * pg * ea, k2b = eb - (pb - eb) * pb * ea;

    uint32_t k1_packed = 0xFF000000u | (uint32_t)clamp_u8_from_unit(k1b) << 16 | (uint32_t)clamp_u8_from_unit(k1g) << 8 | clamp_u8_from_unit(k1r);
    uint32_t k2_packed = 0xFF000000u | (uint32_t)clamp_u8_from_unit(k2b) << 16 | (uint32_t)clamp_u8_from_unit(k2g) << 8 | clamp_u8_from_unit(k2r);

    /* sceGuDrawArray's buffer is consumed by the GE asynchronously (only
     * guaranteed processed by the next sceGuFinish/frame sync) -- mutating
     * the SAME memory in place for a second draw risks the GE seeing pass
     * 2's color for pass 1 too. Use two independent buffers. */
    Vertex *verts1 = (Vertex *)buf;
    void *buf2 = sceGuGetMemory(sizeof(Vertex) * num_verts);
    Vertex *verts2 = (Vertex *)buf2;
    memcpy(buf2, buf, sizeof(Vertex) * num_verts);
    size_t i;
    for (i = 0; i < num_verts; i++) verts1[i].color = k1_packed;
    for (i = 0; i < num_verts; i++) verts2[i].color = k2_packed;

    /* real GU_BLEND state going in, per gfx_pc.c's own cache -- restore to
     * exactly this afterward, NOT an assumed constant. A direct
     * sceGuEnable/Disable(GU_BLEND) that diverges from this cache (which
     * only gfx_pc.c's set_use_alpha() normally updates) desyncs it from real
     * hardware state -- confirmed via a real regression: doing exactly that
     * here made an unrelated later draw (the "NINTENDO 64" text quads, which
     * need GU_BLEND enabled) come out solid white, because set_use_alpha()
     * saw its cached belief already matched the (actually wrong) hardware
     * state and skipped re-enabling it. */
    bool prev_alpha_blend = gfx_get_alpha_blend_state();

    /* Pass 1: TEXEL0 * K1, opaque -- this cube's own render mode
     * (G_RM_AA_ZB_OPA_SURF2) is already opaque, so GU_BLEND is already
     * correctly disabled going in; only the texture function needs
     * switching away from this shader's normal GU_TFX_BLEND. */
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)num_verts, 0, buf);

    /* Pass 2: + K2 flat, additive, no texture. */
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00ffffff, 0x00ffffff);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)num_verts, 0, buf2);

    /* Restore the shared GU state this shader (and the rest of the renderer)
     * normally relies on. */
    sceGuEnable(GU_TEXTURE_2D);
    if (prev_alpha_blend) {
        sceGuEnable(GU_BLEND);
    } else {
        sceGuDisable(GU_BLEND);
    }
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
    sceGuTexEnvColor(gRdpPrimColorPacked);
}

/* Tex-env colour for the "NINTENDO 64" text (is_n64_logo_text_combine).
 *
 * GU_TFX_BLEND computes  out = Cvertex*(1-Ct) + Ctexenv*Ct , i.e. it LERPs
 * between two constants by the texture. The N64's two cycles here are
 *
 *     COMBINED = TEXEL0 + (TEXEL1 - PRIM) * ENV_ALPHA
 *     out      = ENV + (PRIM - ENV) * COMBINED
 *
 * which is LINEAR in TEXEL0 -- so a lerp reproduces it EXACTLY, provided the
 * two endpoints are the real combine evaluated at TEXEL0 = 1 and TEXEL0 = 0.
 * The only approximation left is TEXEL1, the 32x32 shine texture that sweeps
 * across the letters and that a single-TMU GE cannot sample at the same time.
 *
 * Feeding raw PRIM as the tex-env colour -- what this used to do -- is the
 * TEXEL0 = 1 endpoint with TEXEL1 forced to WHITE, which saturates COMBINED to
 * 1 and collapses out to PRIM = (170,255,255). That is why the letters came out
 * flat cyan instead of the blue they should be.
 *
 * TEXEL1 is held at the shine texture's mean intensity (105/255, measured over
 * nintendo_rogo_static_Tex_001800) rather than 0 or 1: it is the value that
 * makes a static approximation of a moving highlight land in the middle of the
 * range it actually sweeps through. With the stock PRIM/ENV this yields roughly
 * (148,180,255) -- the light blue the console shows. The endpoint at TEXEL0 = 0
 * works out to ENV itself once clamped, which the vertex colour already is, so
 * only this end needs correcting.
 *
 * Computed from the live registers, not baked in, because ConsoleLogo_Draw
 * ramps PRIM/ENV during the fade. */
#define N64_LOGO_SHINE_MEAN (105.0f / 255.0f)

static uint32_t n64_logo_text_env_color(void) {
    float pr = (gRdpPrimColorPacked & 0xFF) / 255.0f;
    float pg = ((gRdpPrimColorPacked >> 8) & 0xFF) / 255.0f;
    float pb = ((gRdpPrimColorPacked >> 16) & 0xFF) / 255.0f;
    float er = (gRdpEnvColorPacked & 0xFF) / 255.0f;
    float eg = ((gRdpEnvColorPacked >> 8) & 0xFF) / 255.0f;
    float eb = ((gRdpEnvColorPacked >> 16) & 0xFF) / 255.0f;
    float ea = ((gRdpEnvColorPacked >> 24) & 0xFF) / 255.0f;
    const float t = N64_LOGO_SHINE_MEAN;

    /* COMBINED at TEXEL0 == 1, then cycle 2. */
    float cr = 1.0f + (t - pr) * ea;
    float cg = 1.0f + (t - pg) * ea;
    float cb = 1.0f + (t - pb) * ea;

    float outr = er + (pr - er) * cr;
    float outg = eg + (pg - eg) * cg;
    float outb = eb + (pb - eb) * cb;

    return (gRdpPrimColorPacked & 0xFF000000u) | (uint32_t)clamp_u8_from_unit(outb) << 16 |
           (uint32_t)clamp_u8_from_unit(outg) << 8 | clamp_u8_from_unit(outr);
}

/* Draws issued while the GE's PHYSICAL texture binding is not the one
 * gfx_scegu_select_texture chose.
 *
 * The two can only disagree by way of an upload: texman_upload/_swizzle end
 * with a direct texman_bind_tex(), which changes the binding without going
 * through the tile-priority rule above and without re-asserting the sampler
 * state (see the comment in gfx_scegu_select_texture about exactly that). A
 * frame that uploads nothing therefore cannot desync -- and the one frame
 * after a room load uploads EVERYTHING, which is precisely the frame that comes
 * out corrupted. This counts whether that coincidence is the mechanism or just
 * a coincidence. Split by pass, because the terrain LERP's second pass is the
 * confirmed trigger (turning it off makes the corruption go away, measured on
 * hardware 2026-08-28) and it is also the one path where select_texture
 * deliberately DECLINES to bind tile 0 -- leaving whatever the upload bound. */
uint32_t gPspTexBindDesyncs;
uint32_t gPspTexBindDesyncs2nd;
/* Value at the start of the current frame; see gfx_run in gfx_pc.c. */
uint32_t gPspTexBindDesyncsFrameBase;

static void gfx_scegu_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (cur_shader != NULL && (cur_shader->texture_used[0] || cur_shader->texture_used[1])) {
        const int want_tile = ((gPspGfxHackPreferTexel1 || gPspLerp2SecondPass) &&
                               cur_shader->texture_used[0] && cur_shader->texture_used[1])
                                  ? 1
                                  : (cur_shader->texture_used[0] ? 0 : 1);

        if (tmu_state[want_tile].tex != (uint32_t)texman_get_bound()) {
            ++gPspTexBindDesyncs;
            if (gPspLerp2SecondPass) {
                ++gPspTexBindDesyncs2nd;
            }
        }
    }
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    /* Per-draw refresh (not just per-shader-enable) since PRIMITIVE color
     * changes between face groups that share this same shader_id -- see
     * is_n64_logo_cube_combine in texenv_set_texture_color. */
    if (cur_shader->mix == SH_MT_TEXTURE_COLOR && is_n64_logo_cube_combine(cur_shader)) {
        sceGuTexEnvColor(gRdpPrimColorPacked);
    }

    void *buf = sceGuGetMemory(sizeof(Vertex) * 3 * buf_vbo_num_tris);
    memcpy(buf, buf_vbo, sizeof(Vertex) * 3 * buf_vbo_num_tris);

    if (cur_shader->mix == SH_MT_TEXTURE_COLOR && is_n64_logo_cube_combine(cur_shader)) {
        gfx_scegu_draw_n64_logo_cube_2pass(buf, 3 * buf_vbo_num_tris);
    } else {
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3 * buf_vbo_num_tris, 0, buf);
    }

    // cur_fog_ofs is only set if GL_EXT_fog_coord isn't used
    // if (cur_fog_ofs) gfx_scegu_blend_fog_tris();
}

/* Second pass of a two-texture terrain LERP: (TEXEL1 - TEXEL0) * ENV + TEXEL0.
 *
 * The first pass has already drawn TEXEL0 into the framebuffer, so it is the
 * destination here, and the identity
 *
 *     src * ENV + dst * (1 - ENV)  ==  (TEXEL1 - TEXEL0) * ENV + TEXEL0
 *
 * makes this an exact reproduction rather than an approximation -- provided the
 * mix factor is constant across the draw, which is why gfx_sp_tri1 only accepts
 * ENV and PRIM as the factor. GU_FIX supplies both factors as literal colours,
 * so no per-vertex alpha is needed.
 *
 * Unlike the earlier version of this second pass, these two calls do nothing
 * but toggle GU_BLEND: the actual draw is the ordinary gfx_flush() ->
 * gfx_rapi->draw_triangles() path, driven by an ordinary recursive gfx_sp_tri1
 * call in gfx_pc.c (see gPspLerp2SecondPass there). Texture bind, sampler
 * state and tex-env mode all come from that same normal path, so they cannot
 * drift out of step with what the first pass did for TEXEL0.
 *
 * Depth writes stay ON with the default LEQUAL test: the geometry is bitwise
 * the same as the first pass, so it compares equal and passes. */
void gfx_scegu_lerp2_blend_begin(uint8_t mix) {
    /* One scalar, not three channels. The RDP multiplies the (TEXEL1 - TEXEL0)
     * term by a single value here -- ENV_ALPHA or the LOD fraction -- so the
     * two GU_FIX factors are that value and its complement, applied equally to
     * R, G and B. Feeding three different per-channel factors (which is what
     * passing env_color.rgb amounted to) tinted the crossfade instead of just
     * weighting it. */
    const unsigned int m = gPspLerp2Force ? 255u : (unsigned int)mix;
    const unsigned int inv = 255u - m;
    const unsigned int src_fix = m | (m << 8) | (m << 16);
    const unsigned int dst_fix = inv | (inv << 8) | (inv << 16);

    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, src_fix, dst_fix);
}

/* Hand the pipeline back exactly as gfx_pc.c's own cache (gl_blend, mirroring
 * rendering_state.alpha_blend) believes it left it -- a direct
 * sceGuEnable/Disable(GU_BLEND) that diverges from that cache desyncs it from
 * real hardware state. Confirmed via a real regression elsewhere in this file
 * (see gfx_scegu_draw_n64_logo_cube_2pass): getting this wrong made an
 * unrelated later draw come out solid white because gfx_pc.c's set_use_alpha()
 * saw its cached belief already matched the (actually wrong) hardware state
 * and skipped re-enabling it. */
void gfx_scegu_lerp2_blend_end(void) {
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    if (!gl_blend) {
        sceGuDisable(GU_BLEND);
    }
}

void gfx_scegu_draw_triangles_2d(float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    /* See gfx_scegu_draw_triangles' identical refresh -- "NINTENDO 64" text
     * quads (ConsoleLogo_Draw) go through this 2D path instead. */
    if (cur_shader->mix == SH_MT_TEXTURE_TEXTURE && is_n64_logo_text_combine(cur_shader)) {
        sceGuTexEnvColor(n64_logo_text_env_color());
    }

    void *quad_buf = sceGuGetMemory(sizeof(VertexColor) * 2);
    memcpy(quad_buf, buf_vbo, sizeof(VertexColor) * 2);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, quad_buf);
}

/* ---------------------------------------------------------------------------
 * Pre-rendered room background (the "JPEG rooms": houses, shops, the market).
 *
 * On the N64 this is drawn by a SECOND microcode: Room_DrawBackground2D builds
 * an S2DEX uObjBg and issues gSPBgRect1Cyc/gSPBgRectCopy, after a
 * gSPLoadUcodeL swaps F3DZEX2 out for S2DEX2. This interpreter has a single
 * opcode table, and S2DEX's opcode numbers collide with F3DEX2's (G_BG_1CYC is
 * 0x01, which is G_VTX here), so those commands used to be misread as geometry
 * and sent the walk into unrelated memory -- which is why the whole background
 * path was disabled on this port (see z_room.c).
 *
 * Nothing about that display list is worth emulating, though: it draws ONE
 * screen-sized, screen-aligned, opaque image with the combiner set to pass the
 * texel straight through. That is a blit, and the GE does it in one sprite.
 *
 * The image comes out of the blob pipeline already in GU_PSM_5551 (see
 * psp/tools/jfif_to_psp.py -- the JPEG is decoded at build time, in place), so
 * there is no decode, no format conversion, no staging copy and no texture-cache
 * entry: sceGuTexImage points the GE straight at the room asset in RAM.
 *
 * The 320-pixel-wide image is fed to the GE as a 512x256 texture with a texture
 * BUFFER WIDTH of 320. The GE requires power-of-two texture dimensions but the
 * buffer width is independent of them, and since the drawn u range stops at 320
 * the out-of-image part of that 512 is never sampled.
 * ------------------------------------------------------------------------- */
typedef struct {
    short u, v;
    short x, y, z;
} BgVertex;

/* Set by gfx_pc.c after the blit so the next draw re-binds its own texture:
 * sceGuTexImage above overwrites the GE's texture pointer behind the texture
 * manager's back, and tmu_state[] would otherwise still claim that texture is
 * bound and skip the re-bind. (This is the same class of staleness that the
 * dual-texture attempt in gfx_scegu_select_texture tripped over.) */
void gfx_scegu_invalidate_texture_binding(void) {
    tmu_state[0].tex = (uint32_t)-1;
    tmu_state[1].tex = (uint32_t)-1;
}

/* Bumped whenever a room finishes loading (Room_ProcessRoomRequest). See the
 * cache note in gfx_scegu_draw_background: a pointer comparison alone cannot
 * tell "same image" from "different image at the same address". */
unsigned int gPspBgCacheGeneration = 0;

void gfx_scegu_draw_background(const void *img, int width, int height, int offset_x, int offset_y) {
    static const void *last_img = NULL;
    static unsigned int last_generation = (unsigned int)-1;

    if (img == NULL || width <= 0 || height <= 0) {
        return;
    }

    /* Reject implausible dimensions before they become a memory range. These
     * backgrounds are always 320x240; anything far past that means the
     * RoomShapeImage was read from data that is not (yet) a room, and both the
     * cache writeback below and the GE's own fetch would then run off the end
     * of the buffer -- fatal on hardware, silently tolerated by PPSSPP. */
    if (width > 1024 || height > 1024) {
        return;
    }

    /* The GE reads main memory directly and is not coherent with the CPU's
     * data cache. A blob is written by ordinary file I/O, so write it back
     * once -- when the room changes, not per frame (150 KB of cache
     * maintenance every frame would cost more than the draw).
     *
     * "When the room changes" is NOT the same as "when this pointer changes",
     * which is what this used to test. OoT alternates between two fixed room
     * buffers (roomCtx->bufPtrs[activeBufPage], z_room.c), so a DIFFERENT room
     * routinely arrives at the SAME address -- the pointer compares equal, the
     * writeback is skipped, and the GE reads whatever the last flush left in
     * RAM while the new image is still sitting in this core's dirty cache
     * lines. The result is an image torn into horizontal bands, some rows new
     * and some stale, which stays that way until the lines happen to evict.
     * PPSSPP models no cache at all and so cannot reproduce it. Hence the
     * generation counter, bumped by whoever loads a room.
     *
     * The range is aligned outward to 64-byte cache lines: the hardware call
     * operates on whole lines, and an unaligned base or length can leave the
     * first and last line of the image unflushed -- which looks like a thin
     * band of corruption at the top and bottom and is easy to misread as a
     * geometry problem. */
    if (img != last_img || gPspBgCacheGeneration != last_generation) {
        uintptr_t start = (uintptr_t)img & ~63u;
        uintptr_t end = ((uintptr_t)img + (unsigned int)(width * height * 2) + 63u) & ~63u;

        sceKernelDcacheWritebackRange((const void *)start, (unsigned int)(end - start));
        last_img = img;
        last_generation = gPspBgCacheGeneration;
    }

    const float sx = (float)SCR_WIDTH / (float)width;
    const float sy = (float)SCR_HEIGHT / (float)height;

    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE); /* GU_TRUE == "don't write depth" here, see set_depth_mask */
    /* This is meant to be an unconditional opaque paint, like the N64's COPY-mode
     * combiner bypass -- but two pieces of GE state that gfx_scegu_init leaves
     * enabled by default silently disagree with that unless told otherwise:
     *  - GU_ALPHA_TEST (GU_GREATER, 0x55) discards any fragment whose alpha is
     *    <= 0x55, and
     *  - GU_TCC_RGB (see the shader-combiner comment elsewhere in this file)
     *    takes alpha from the STICKY vertex-color register, not the texture, and
     *    BgVertex carries no color at all, so that register holds whatever the
     *    previous draw (e.g. the room's own opaque geometry) last left it at.
     * Together, an unlucky leftover vertex alpha silently discarded the entire
     * blit -- not a blend-transparency issue, an actual zero pixels drawn, which
     * is indistinguishable from "nothing rendered" and was mistaken for exactly
     * that. GU_TCC_RGBA sources alpha from the texture's own alpha bit (always
     * set opaque by psp/tools/jfif_to_psp.py) instead, and alpha test is turned
     * off outright since this draw has no use for it either way. */
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexImage(0, 512, 256, width, img);

    BgVertex *v = (BgVertex *)sceGuGetMemory(sizeof(BgVertex) * 2);
    v[0].u = 0;
    v[0].v = 0;
    v[0].x = (short)(offset_x * sx);
    v[0].y = (short)(offset_y * sy);
    v[0].z = 0;
    v[1].u = (short)width;
    v[1].v = (short)height;
    v[1].x = (short)(SCR_WIDTH + offset_x * sx);
    v[1].y = (short)(SCR_HEIGHT + offset_y * sy);
    v[1].z = 0;

    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);

    /* GU_ALPHA_TEST is not part of gfx_pc.c's rendering_state cache -- unlike
     * depth test/mask, nothing re-applies it before the next triangle, so
     * leaving it off here would silently disable alpha testing for the rest of
     * the frame (it is normally set once per frame, in gfx_scegu_start_frame
     * below, and never touched again until the next frame). gDebugAlphaTest is
     * that function's own source of truth for what it should be. */
    extern int gDebugAlphaTest;
    if (gDebugAlphaTest) {
        sceGuEnable(GU_ALPHA_TEST);
    }

    /* Leave the sampler the way the rest of the pipeline expects to find it.
     * Everything else the blit touched (texture enable/mode/func, depth test,
     * depth mask, texture binding) is restored by the caller in gfx_pc.c,
     * which is the side that knows what the current state is supposed to be. */
    gfx_scegu_apply_tmu_state(0);
}

static void gfx_scegu_init(void) {
    sceGuInit();

    sFbp0 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    sFbp1 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp0 = sFbp0;
    void *fbp1 = sFbp1;
    void *zbp = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);

    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp1, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2), SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0xffff, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CCW);
    sceGuDepthMask(GU_FALSE);
    sceGuTexEnvColor(0xffffffff);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);

    gPspGeListBytes = (unsigned int)sceGuFinish();
    if (gPspGeListBytes > gPspGeListPeak) {
        gPspGeListPeak = gPspGeListBytes;
    }
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    /* Back in VRAM, and now sized to whatever is ACTUALLY left rather than a
     * hardcoded 1 MB: the GE samples VRAM at full bandwidth, RAM at a fraction
     * of it, so textures belong here as long as they fit. What made the old
     * VRAM pool unusable was never its location -- it was that nothing ever
     * reset it between scenes (see gfx_texture_cache_reset), so it filled up
     * over a whole play session and then got wiped mid-frame.
     *
     * The bump allocator has handed out the two framebuffers and the Z-buffer
     * by now, so staticOffset is exactly what they cost; everything past it to
     * the end of EDRAM is ours. Leave one texture's worth of slack so the
     * overflow guard in texman_reserve_memory has somewhere to land. */
    unsigned int vram_total = sceGeEdramGetSize();
    unsigned int vram_left = (vram_total > staticOffset) ? (vram_total - staticOffset) : 0;
    unsigned int texman_size = (vram_left > TEXMAN_VRAM_SLACK) ? (vram_left - TEXMAN_VRAM_SLACK) : 0;

    texman_size &= ~(TEX_ALIGNMENT - 1);

    void *texman_buffer = texman_size ? getStaticVramBufferBytes(texman_size) : NULL;
    void *texman_aligned = (void *) ((((unsigned int) texman_buffer + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT);
    texman_reset(texman_aligned, texman_size);

    /* Spill region in main RAM. VRAM alone is not enough for a busy scene --
     * the Market overflows ~1.2 MB on its own -- and overflowing means wiping
     * both texture caches mid-frame (the speckled corruption). With a fallback
     * the hot textures still land in VRAM and only the tail is slower.
     *
     * Take the largest that fits. One fixed request would mean that the day it
     * no longer fits, the region silently disappears entirely and every busy
     * scene starts wiping again -- a failure that looks like a rendering
     * regression and has nothing to do with rendering. */
    {
        unsigned int want = TEXMAN_OVERFLOW_SIZE;
        void *texman_overflow = NULL;

        while (want >= TEXMAN_OVERFLOW_MIN) {
            texman_overflow = memalign(TEX_ALIGNMENT, want);
            if (texman_overflow != NULL) {
                break;
            }
            want /= 2;
        }
        if (texman_overflow != NULL) {
            texman_set_overflow_buffer(texman_overflow, want);
            gPspTexSpillBytes = want;
        }
    }
    if (!texman_buffer) {
        char msg[32];
        sprintf(msg, "OUT OF MEMORY!\n");
        sceIoWrite(1, msg, strlen(msg));

        sceKernelExitGame();
    }
}

/* The GE's only clipper is a single Z = -W near plane, and it clips against
 * the projection we upload -- i.e. at OoT's zNear (10.0f), which the
 * F3DZEX2.NoN microcode this game ships does NOT clip against. gfx_pc.c's
 * software clipper already guarantees w > 0 (see gPspNearClipT) and clips the
 * four sides, so the hardware plane only ever removes geometry that should
 * have been drawn. Set to 1 to restore the old behaviour for an A/B test. */
int gPspGuClipPlanes = 0;

/* --- two runtime A/B switches for the open "Link's geometry is displaced" bug.
 * Measured so far: matrices are all correct, adult Link breaks identically to
 * child (so it is the renderer, not model data), and Z_CMP is set on every
 * single depth-tested draw (so the libultraship depth-test divergence never
 * fires here). What is left is HOW the depth comparison and the alpha test are
 * configured, both of which are inherited from sm64-port-psp unexamined.
 *
 * gDebugDepthMode -- NOTE: this one is a NO-OP by construction and was a badly
 * designed experiment; it is kept only so both conventions stay expressible.
 * Flipping range, function AND clear together cancels out exactly (reversed
 * range + GEQUAL is the SAME test as normal range + LEQUAL), so it can never
 * change the picture, and measuring "no change" with it proves nothing.
 * 0 = inherited: reversed range (near = 0xffff) + GU_GEQUAL,
 * cleared to 0 (far). Self-consistent, but only if the z the GE derives from
 * OoT's projection maps the way SM64's did. 1 = conventional: range
 * (0 .. 0xffff) + GU_LEQUAL, cleared to 0xffff. If the ordering is inverted,
 * flipping this fixes or dramatically worsens the picture instantly -- either
 * outcome is an answer.
 *
 * gDebugAlphaTest -- the inherited setup enables GU_ALPHA_TEST unconditionally
 * with GU_GREATER, 0x55, i.e. every fragment with alpha <= 0x55 is discarded,
 * regardless of what other_mode_l's alpha compare actually asks for. That is a
 * standalone candidate for the HOLES in the mesh. 0 = disable the test.
 *
 * Both are applied per frame, so a poke takes effect on the next frame -- no
 * rebuild, no scene reload. */
int gDebugDepthMode = 0;
int gDebugAlphaTest = 1;
/* Flip ONLY the depth comparison (range and clear untouched) -> the far surface
 * wins instead of the near one. This is the actual inversion test. */
int gDebugDepthFuncFlip = 0;
/* Force the depth test off entirely. If the picture does not change, depth
 * testing was already having no effect, which would itself be the bug. */
int gDebugDepthTestOff = 0;

static void gfx_scegu_start_frame(void) {
    sceGuStart(GU_DIRECT, list);
    if (gPspGuClipPlanes) {
        sceGuEnable(GU_CLIP_PLANES);
    } else {
        sceGuDisable(GU_CLIP_PLANES);
    }

    /* NOTE: gDebugDepthMode flips range, function AND clear together -- those
     * three changes cancel out exactly (reversed range + GEQUAL is the SAME
     * test as normal range + LEQUAL), so it can never change the picture. It is
     * kept only so the two conventions stay switchable; it is NOT a test of
     * depth ordering. Measured: no visual change, as the maths requires.
     *
     * gDebugDepthFuncFlip is the real test: flip ONLY the comparison and leave
     * range and clear alone, which genuinely inverts which surface wins.
     * gDebugDepthTestOff answers the prior question -- is the depth buffer
     * doing anything at all? If disabling it changes nothing, depth testing is
     * already inert and THAT is the bug. */
    if (gDebugDepthMode) {
        sceGuDepthRange(0, 0xffff);
        sceGuDepthFunc(gDebugDepthFuncFlip ? GU_GEQUAL : GU_LEQUAL);
    } else {
        sceGuDepthRange(0xffff, 0);
        sceGuDepthFunc(gDebugDepthFuncFlip ? GU_LEQUAL : GU_GEQUAL);
    }
    if (gDebugDepthTestOff) {
        sceGuDisable(GU_DEPTH_TEST);
    }
    if (gDebugAlphaTest) {
        sceGuEnable(GU_ALPHA_TEST);
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    sceGuClearColor(0xFF000000);
    /* Must match the range above: clear to the FAR end, or nothing passes. */
    sceGuClearDepth(gDebugDepthMode ? 0xffff : 0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_FALSE);

    // Identity every frame? unsure.
    //sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *) identity_matrix);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *) identity_matrix);
    /* MUST be identity and stay identity: gfx_pc.c's gfx_sp_vertex applies the
     * modelview in software at vertex-load time (N64 G_VTX semantics), so the
     * GE only ever applies the projection. */
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *) identity_matrix);

#if 0
    const int DitherMatrix[2][16] = { { 0, 8, 0, 8,
                         8, 0, 8, 0,
                         0, 8, 0, 8,
                         8, 0, 8, 0 },
                        { 8, 8, 8, 8,
                          0, 8, 0, 8,
                          8, 8, 8, 8,
                          0, 8, 0, 8 } };

    extern int gDoDither;
    extern int gFrame;

    sceGuDisable(GU_DITHER);
    if(gDoDither){
        // every frame
        sceGuSetDither((const ScePspIMatrix4 *)DitherMatrix[(gFrame&1)]);
        sceGuEnable(GU_DITHER);
    }
#endif
}

void gfx_scegu_on_resize(void) {
}

/* Which of the two buffers the GE is currently DRAWING into. sceGuSwapBuffers
 * exchanges them, so this toggles in lockstep with it. Tracked because
 * PspSceneMenu_DrawOverlay writes to the framebuffer with the CPU and has to be
 * given the buffer that is about to be displayed, not the one on screen now. */
#include "psp_hw_diag.h"

/* Mirrors graph.c's PSP_DIAG_FIRST window without pulling in its private
 * frame counter's definition. */
extern u32 gPspDiagFrameCount;
#define PSP_DIAG_GFX(name)                 \
    do {                                   \
        if (gPspDiagFrameCount < PSP_DIAG_FRAMES) {     \
            PspDiag_Step(name);        \
        }                                  \
    } while (0)

static int sDrawBufferIsFbp0 = 1;

static void gfx_scegu_end_frame(void) {
    /* Menu backdrop goes in BEFORE sceGuFinish, while the GE list is still
     * open -- it is ordinary GE geometry, unlike the text below which the CPU
     * pokes into the finished framebuffer. Splitting it this way also makes the
     * overlay self-diagnosing: a visible panel with no text isolates the
     * failure to the pspDebugScreen half, no panel at all to the input half. */
    /* Stage probes across the GE handover.
     *
     * On hardware the image freezes on the last completed frame and the
     * console loses power a few seconds later, while PPSSPP runs the
     * byte-identical trace onwards -- which is what a stalled GE looks like,
     * not what a crashed CPU looks like. sceGuSync is the one blocking call
     * in this port that waits on the graphics hardware, so it is the one that
     * would park the main thread forever with the last swapped frame still on
     * screen. These four say which side of it we are on, and separate the
     * menu overlay (drawn into the still-open list) from the engine's own
     * geometry. First 24 frames only, same window as the rest. */
    PSP_DIAG_GFX("ge-menu-begin");
    PspSceneMenu_DrawBackdrop();
    PspSceneMenu_DrawHud();
    PSP_DIAG_GFX("ge-menu-done");

    gPspGeListBytes = (unsigned int)sceGuFinish();
    if (gPspGeListBytes > gPspGeListPeak) {
        gPspGeListPeak = gPspGeListBytes;
    }
    PSP_DIAG_GFX("ge-finish");
    /* One durable write per frame, and only inside the probe window (the first
     * PSP_DIAG_FRAMES frames of each scene, which is exactly when a freshly
     * loaded scene falls over).
     *
     * The tension this resolves: force-flushing every probe costs ~240 ms a
     * frame, which slows the game enough to make the fault disappear -- a run
     * instrumented that way rendered the scene that otherwise dies. Leaving
     * everything buffered keeps the timing honest but loses up to seven
     * entries, and a run ended with "ge-sync" as its last line with no way to
     * tell whether the GE had actually stalled there or the tail was simply
     * still in RAM.
     *
     * Flushing here, immediately BEFORE the one blocking call that waits on
     * the graphics hardware, means a stall inside sceGuSync leaves a log that
     * durably ends at "ge-finish". One write per frame is ~24 ms and only for
     * the first few frames of a scene. */
    if (gPspDiagFrameCount < PSP_DIAG_FRAMES) {
        PspDiag_Flush();
    }
    sceGuSync(0, 0);
    PSP_DIAG_GFX("ge-sync");

    /* Grab the finished picture here, between the GE going idle and the swap:
     * the buffer just drawn into is complete and nothing is reading it. After
     * the swap it would be the buffer the DISPLAY owns and the next frame's
     * target, i.e. one frame stale and being overwritten while it is read.
     *
     * The address arithmetic mirrors getStaticVramBufferBytes: sFbp0/sFbp1
     * hold VRAM-relative offsets with the uncached bit, which is what sceGu
     * wants and not something the CPU can dereference. */
    {
        void *drawn = sDrawBufferIsFbp0 ? sFbp0 : sFbp1;

        PspScreenshot_Tick((const void *)((unsigned int)drawn + (unsigned int)sceGeEdramGetAddr()),
                           SCR_WIDTH, SCR_HEIGHT, BUF_WIDTH);
    }

    {
        /* Timed so the pacer can charge this to idle rather than to the
         * frame's work -- see gPspVblankWaitUsec in psp_frame_pace.h. */
        u64 waitStart = (u64)sceKernelGetSystemTimeWide();

        sceDisplayWaitVblankStart();
        gPspVblankWaitUsec = (u32)((u64)sceKernelGetSystemTimeWide() - waitStart);
    }
    PSP_DIAG_GFX("ge-vblank");

    sceGuSwapBuffers();
    PSP_DIAG_GFX("ge-swap");
    sDrawBufferIsFbp0 = !sDrawBufferIsFbp0;
}

static void gfx_scegu_finish_render(void) {
    /* There should be something here! */
}

// clang-format off
struct GfxRenderingAPI gfx_opengl_api = {
    gfx_scegu_z_is_from_0_to_1,
    gfx_scegu_unload_shader,
    gfx_scegu_load_shader,
    gfx_scegu_create_and_load_new_shader,
    gfx_scegu_lookup_shader,
    gfx_scegu_shader_get_info,
    gfx_scegu_new_texture,
    gfx_scegu_select_texture,
    gfx_scegu_upload_texture,
    gfx_scegu_set_sampler_parameters,
    gfx_scegu_set_depth_test,
    gfx_scegu_set_depth_mask,
    gfx_scegu_set_zmode_decal,
    gfx_scegu_set_viewport,
    gfx_scegu_set_scissor,
    gfx_scegu_set_use_alpha,
    gfx_scegu_draw_triangles,
    gfx_scegu_init,
    gfx_scegu_on_resize,
    gfx_scegu_start_frame,
    gfx_scegu_end_frame,
    gfx_scegu_finish_render
};

#endif // RAPI_GL_LEGACY
