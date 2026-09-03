/* Minimal PSP GfxWindowManagerAPI implementation, written for this port
 * rather than reusing sm64-port-psp's gfx_psp.c: that file bundles Media
 * Engine audio-offload plumbing (melib.h, psp_audio_stack.h) tightly
 * together with the window-manager bring-up, and audio is out of scope for
 * Phase 1 (see plan roadmap). gfx_scegu.c's own .init callback
 * (gfx_scegu_init) already does the actual sceGuInit/buffer setup, so this
 * file only needs to handle what's left: exit-callback registration (same
 * pattern already proven in psp/src/main.c), frame pacing, and vblank-
 * synced swap. */
#include <stdbool.h>
#include <stdint.h>

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psprtc.h>

#include "gfx_window_manager_api.h"
#include "psp_audio.h"
#include "psp_audio_me.h"
#include "psp_blob_assets.h"

static int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

/* Standby is not a pause: the Memory Stick and the audio hardware are powered
 * down, and everything the port was holding on to across them comes back dead.
 * Nothing noticed, because the port registered an exit callback and no power
 * callback at all -- so flipping the power switch in-game and back left every
 * cached blob descriptor stale (the sample bank is read several times per audio
 * frame and never ages out of the LRU, so every note was fed nonsense) and the
 * reserved SRC channel gone, which also costs the audio thread its only
 * blocking point.
 *
 * This runs on its own callback thread and does no work itself. Each of the
 * three notify calls sets a flag that the owning thread acts on at its next
 * natural point -- closing files or touching sceAudio from here would be doing
 * I/O in a callback, and reference/oot-psp-z2442 splits it the same way
 * (OotPsp_AssetNotifyResume). */
static int power_callback(int unknown, int power_info, void *common) {
    if (power_info & PSP_POWER_CB_RESUME_COMPLETE) {
        extern void PspGfx_NotifyResume(void);
        extern void PspRom_NotifyResume(void);

        PspBlob_NotifyResume();
        /* The .z64 descriptor, which is the one that has been open since boot
         * and is still what serves the skybox. */
        PspRom_NotifyResume();
        PspAudio_NotifyResume();
        PspAudioMe_NotifyResume();
        PspGfx_NotifyResume();
    }
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    int pwid = sceKernelCreateCallback("Power Callback", power_callback, NULL);

    sceKernelRegisterExitCallback(cbid);
    if (pwid >= 0) {
        scePowerRegisterCallback(0, pwid);
    }
    sceKernelSleepThreadCB();
    return 0;
}

static void gfx_wm_psp_init(const char *game_name, bool start_in_fullscreen) {
    int thid = sceKernelCreateThread("gfx_wm_psp_exit_cb", callback_thread, 0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

static void gfx_wm_psp_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode), void (*on_all_keys_up)(void)) {
}

static void gfx_wm_psp_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_wm_psp_set_fullscreen(bool enable) {
}

static void gfx_wm_psp_main_loop(void (*run_one_game_iter)(void)) {
    /* Never used -- this port drives frames from Graph_Update
     * (src/code/graph.c), not from a gfx_pc.c-owned main loop. */
}

static void gfx_wm_psp_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = 480;
    *height = 272;
}

static void gfx_wm_psp_handle_events(void) {
}

static bool gfx_wm_psp_start_frame(void) {
    return true;
}

static void gfx_wm_psp_swap_buffers_begin(void) {
}

static void gfx_wm_psp_swap_buffers_end(void) {
    /* Deliberately empty. gfx_scegu_end_frame already does the full swap --
     * sceGuSync, sceDisplayWaitVblankStart, sceGuSwapBuffers -- so the
     * WaitVblankStart that used to sit here was a SECOND wait on the same
     * frame. It cost up to one refresh (16.7 ms) of pure idle per frame and,
     * worse, made two vblanks the floor for any frame at all, capping the
     * renderer at 30 Hz however cheap the scene was. Pacing is
     * psp_frame_pace.c's job and it counts vblanks itself. */
}

static double gfx_wm_psp_get_time(void) {
    return 0.0;
}

struct GfxWindowManagerAPI gfx_wm_psp = {
    gfx_wm_psp_init,
    gfx_wm_psp_set_keyboard_callbacks,
    gfx_wm_psp_set_fullscreen_changed_callback,
    gfx_wm_psp_set_fullscreen,
    gfx_wm_psp_main_loop,
    gfx_wm_psp_get_dimensions,
    gfx_wm_psp_handle_events,
    gfx_wm_psp_start_frame,
    gfx_wm_psp_swap_buffers_begin,
    gfx_wm_psp_swap_buffers_end,
    gfx_wm_psp_get_time,
};
