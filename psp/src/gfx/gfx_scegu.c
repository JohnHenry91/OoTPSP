#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#include <stdint.h>
#include <stdlib.h>
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

#include "psp_texture_manager.h"

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

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

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
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
    /*@Note: hack shader 0x1A00A6F for Bowser/Peach Paintings (still broken, but just fixed on peach)*/
    return GU_TFX_DECAL;
}

/* TEMPORARY DIAGNOSTIC (2026-08-14). Set to 1 to render every material with
 * texturing switched off, so geometry comes out in pure vertex/shade colour.
 * This splits the remaining "everything is white with diagonal hatching"
 * problem cleanly in two, in a single test run:
 *   - if the room then shows plausible, solid, *coloured* shaded geometry, the
 *     vertex/lighting/combiner path is fine and the fault is entirely in the
 *     texture stage (upload -> texman binding -> GU_TFX mode);
 *   - if it still comes out white/hatched, texturing is NOT the culprit and the
 *     problem is upstream in vertex colours, lighting or the combiner.
 * Texture *decoding* itself has already been verified correct offline (room
 * hakaana2's RGBA16 tiles decode byte-for-byte to the reference PNGs, modulo
 * +-1 rounding in the 5->8 bit scale), so this is the right next split.
 * Set back to 0 once the answer is known. */
#define PSP_DIAG_DISABLE_TEXTURING 0

static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
#if PSP_DIAG_DISABLE_TEXTURING
    sceGuDisable(GU_TEXTURE_2D);
    return;
#endif
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

static struct ShaderProgram *gfx_scegu_create_and_load_new_shader(uint32_t shader_id) {
    LogUnknownShaderId(shader_id); /* every distinct ID actually used, not just remap misses */

    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

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

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP)
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

static inline void gfx_scegu_apply_tmu_state(const int tile) {
    sceGuTexFilter(tmu_state[tile].min_filter, tmu_state[tile].mag_filter);
    sceGuTexWrap(tmu_state[tile].wrap_s, tmu_state[tile].wrap_t);
}

static void gfx_scegu_set_sampler_parameters(const int tile, const bool linear_filter, const uint32_t cms, const uint32_t cmt) {
    const int filter = linear_filter ? GU_LINEAR : GU_NEAREST;

    const int wrap_s = gfx_cm_to_opengl(cms);
    const int wrap_t = gfx_cm_to_opengl(cmt);

    tmu_state[tile].min_filter = filter;
    tmu_state[tile].mag_filter = filter;
    tmu_state[tile].wrap_s = wrap_s;
    tmu_state[tile].wrap_t = wrap_t;

    // set state for the first texture right away
    if (!tile)
        gfx_scegu_apply_tmu_state(tile);
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
    if (tile == 1 && cur_shader != NULL && is_n64_logo_text_combine(cur_shader)) {
        return;
    }
    if (tmu_state[tile].tex != texture_id) {
        tmu_state[tile].tex = texture_id;
        texman_bind_tex(texture_id);
        gfx_scegu_set_sampler_parameters(tile, false, 0, 0);
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

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
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

static void gfx_scegu_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
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

void gfx_scegu_draw_triangles_2d(float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    /* See gfx_scegu_draw_triangles' identical refresh -- "NINTENDO 64" text
     * quads (ConsoleLogo_Draw) go through this 2D path instead. */
    if (cur_shader->mix == SH_MT_TEXTURE_TEXTURE && is_n64_logo_text_combine(cur_shader)) {
        sceGuTexEnvColor(gRdpPrimColorPacked);
    }

    void *quad_buf = sceGuGetMemory(sizeof(VertexColor) * 2);
    memcpy(quad_buf, buf_vbo, sizeof(VertexColor) * 2);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, quad_buf);
}

static void gfx_scegu_init(void) {
    sceGuInit();

    void *fbp0 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
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

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    void *texman_buffer = getStaticVramBufferBytes(TEXMAN_BUFFER_SIZE);
    void *texman_aligned = (void *) ((((unsigned int) texman_buffer + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT);
    texman_reset(texman_aligned, TEXMAN_BUFFER_SIZE);
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

static void gfx_scegu_start_frame(void) {
    sceGuStart(GU_DIRECT, list);
    if (gPspGuClipPlanes) {
        sceGuEnable(GU_CLIP_PLANES);
    } else {
        sceGuDisable(GU_CLIP_PLANES);
    }
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_FALSE);

    // Identity every frame? unsure.
    //sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *) identity_matrix);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *) identity_matrix);
    //sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *) identity_matrix);

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

static void gfx_scegu_end_frame(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
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
