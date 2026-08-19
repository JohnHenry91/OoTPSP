/* PSP audio output backend for the real N64 audio engine (src/audio/**).
 *
 * Ported from reference/sm64-port-psp's src/pc/audio/audio_psp.c (a working
 * PSP homebrew port of a sibling N64 decomp using the same libultra audio-
 * driver lineage), simplified: that file exists to feed a separate PC-style
 * audio thread from SM64's PC port architecture (see its own
 * psp_audio_stack.c). This port doesn't need that -- AudioMgr already runs
 * as its own real PSP thread (src/code/audio_thread_manager.c's
 * AudioMgr_Init, enabled in src/code/main.c), same as DmaMgr, and
 * AudioSynth_Update (src/audio/internal/synthesis.c) already produces
 * finished s16 PCM directly -- no separate RSP task/ucode to run. So this
 * file only needs to be the two real N64 AI (Audio Interface) hardware
 * touchpoints' PSP replacement: see osAiSetFrequency/osAiGetLength below,
 * and PspAudio_Output, called from src/audio/internal/os.c's TARGET_PSP
 * osAiSetNextBuffer.
 *
 * sceAudioOutput2OutputBlocking blocks until the PSP audio hardware is
 * ready for a new buffer -- since this all runs on AudioMgr's own real
 * thread (not the main game/render thread), that's the whole pacing
 * mechanism: no separate output thread or queue needed, and no need to
 * track real "samples remaining" for osAiGetLength (see below).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pspaudio.h>
#include <pspkernel.h>

#include "ultra64.h"

#define PSP_AUDIO_CHANNELS 2
#define PSP_AUDIO_FREQUENCY 32000

/* AIBUF_LEN (include/audio.h) is the real engine's own defined ceiling on
 * how many samples one AI buffer can ever hold (88 * ADPCMFSIZE = 1408) --
 * size our buffers with margin above that rather than re-deriving it here. */
#define PSP_AUDIO_MAX_SAMPLES 2048

static int sChannel = -1;
static int sReservedSamples = 0;

/* PSP audio output wants aligned, uncached-safe memory; keep this static
 * rather than pointing directly at gAudioCtx.aiBuffers (real engine memory,
 * not necessarily suitably aligned/sized for direct hardware DMA). */
static s16 sOutputBuffer[PSP_AUDIO_MAX_SAMPLES * PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));

void PspAudio_Init(void) {
    sChannel = sceAudioSRCChReserve(PSP_AUDIO_MAX_SAMPLES / 2, PSP_AUDIO_FREQUENCY, PSP_AUDIO_CHANNELS);
    sReservedSamples = PSP_AUDIO_MAX_SAMPLES / 2;
}

/**
 * Output one finished AI buffer. `buf` is real N64-engine-synthesized
 * interleaved stereo s16 PCM (gAudioCtx.aiBuffers[index]);  `numSamples` is
 * frame count (stereo sample pairs), matching aiBufLengths[index].
 */
void PspAudio_Output(const s16* buf, u32 numSamples) {
    if (numSamples == 0) {
        return;
    }
    if (numSamples > PSP_AUDIO_MAX_SAMPLES) {
        /* Should never happen (see AIBUF_LEN above) -- clamp defensively
         * rather than overrun sOutputBuffer. */
        numSamples = PSP_AUDIO_MAX_SAMPLES;
    }

    memcpy(sOutputBuffer, buf, numSamples * PSP_AUDIO_CHANNELS * sizeof(s16));
    sceKernelDcacheWritebackInvalidateRange(sOutputBuffer, numSamples * PSP_AUDIO_CHANNELS * sizeof(s16));

    if (sChannel < 0) {
        sChannel = sceAudioSRCChReserve(numSamples, PSP_AUDIO_FREQUENCY, PSP_AUDIO_CHANNELS);
        sReservedSamples = numSamples;
    }
    if ((s32)numSamples != sReservedSamples) {
        sceAudioOutput2ChangeLength(numSamples);
        sReservedSamples = numSamples;
    }

    sceAudioOutput2OutputBlocking(PSP_AUDIO_VOLUME_MAX, sOutputBuffer);
}

/**
 * Real osAiSetFrequency (src/libultra/io/aisetfreq.c) programs real N64 AI
 * hardware DAC registers from osViClock -- not compiled for TARGET_PSP (see
 * Makefile.psp), this is its replacement. sceAudio can output at exactly
 * the requested rate (32000, the sampling frequency essentially every
 * AudioSpec in gAudioSpecs uses, see session_config.c; the engine also
 * always internally resamples to a 32000 base -- see
 * audioBufferParameters.resampleRate in src/audio/internal/heap.c), so
 * unlike real hardware there's no rounding to report back: just return the
 * requested frequency as the "true" one.
 */
s32 osAiSetFrequency(u32 frequency) {
    return (s32)frequency;
}

/**
 * Real osAiGetLength (src/libultra/io/aigetlen.c) reads how many bytes are
 * left in the real AI hardware's in-progress DMA, to size the next
 * synthesis pass (src/audio/internal/thread.c's AudioThread_UpdateImpl).
 * PspAudio_Output above is fully blocking -- by the time AudioMgr's thread
 * is back here asking, the previous buffer has already finished playing on
 * real PSP hardware, so 0 remaining is both simple and accurate.
 */
u32 osAiGetLength(void) {
    return 0;
}
