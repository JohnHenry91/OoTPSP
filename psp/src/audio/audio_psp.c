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
#include "psp_audio_mixer.h"

#define PSP_AUDIO_CHANNELS 2
#define PSP_AUDIO_FREQUENCY 32000

/* sceAudio takes a FIXED block length, and it must be a multiple of 64.
 * The engine's own per-tick buffer length is neither: AudioThread_UpdateImpl
 * sizes it from the audio spec (656 frames here) and it can change from tick
 * to tick. Feeding that straight through meant asking sceAudioOutput2ChangeLength
 * for 656 every frame -- an unaligned length, its result never checked, and a
 * reservation that no longer matched what was actually handed to the hardware.
 *
 * So buffer instead: the engine's variable-size ticks go into a ring, and the
 * hardware is always fed whole aligned blocks. 1024 frames is 32 ms of
 * latency at 32 kHz, small enough not to be felt and large enough that one
 * engine tick never has to be split more than twice. */
#define PSP_AUDIO_BLOCK_FRAMES 1024
#define PSP_AUDIO_RING_FRAMES 8192

static int sChannel = -1;

/* Debug counters -- see psp_audio.h. Plain globals rather than a struct so
 * the existing "read individual fields over the WebSocket debugger" workflow
 * (see gfx_pc.h's gfx_pc_stat_* accessors) works the same way here. */
static uint32_t sStatOutputCalls = 0;
static uint32_t sStatLastNumSamples = 0;
static int32_t sStatLastPeakSample = 0;
static uint32_t sStatReserveFailures = 0;
/* sceAudioOutput2OutputBlocking's result was never inspected. It is the last
 * unverified step in the whole chain: everything upstream can be measured from
 * memory (and has been), but whether the emulator/hardware actually ACCEPTED
 * the block is only visible here. A negative return every call would look
 * exactly like a working audio path from every other vantage point. */
static uint32_t sStatOutputErrors = 0;
static int32_t sStatLastOutputRet = 0;
/* Times the hardware had already drained everything by the moment we came back
 * with the next block -- i.e. it played silence in the gap. This is what
 * "choppy" sounds like, and it is invisible from every other counter: calls,
 * peak and realtime factor all stay healthy while it happens. */
static uint32_t sStatUnderruns = 0;
static uint32_t sStatMinRest = 0xFFFFFFFF;

uint32_t PspAudio_StatOutputCalls(void) {
    return sStatOutputCalls;
}
uint32_t PspAudio_StatLastNumSamples(void) {
    return sStatLastNumSamples;
}
int32_t PspAudio_StatLastPeakSample(void) {
    return sStatLastPeakSample;
}
uint32_t PspAudio_StatReserveFailures(void) {
    return sStatReserveFailures;
}
uint32_t PspAudio_StatOutputErrors(void) {
    return sStatOutputErrors;
}
int32_t PspAudio_StatLastOutputRet(void) {
    return sStatLastOutputRet;
}
uint32_t PspAudio_StatUnderruns(void) {
    return sStatUnderruns;
}
uint32_t PspAudio_StatMinRest(void) {
    return sStatMinRest;
}

static s16 sRing[PSP_AUDIO_RING_FRAMES * PSP_AUDIO_CHANNELS];
static u32 sRingWrite; /* frame index, wraps at PSP_AUDIO_RING_FRAMES */
static u32 sRingFill;  /* frames currently buffered */
static u32 sNoOutputStreak;

/* PSP audio output wants aligned, uncached-safe memory; keep this static
 * rather than pointing directly at gAudioCtx.aiBuffers (real engine memory,
 * not necessarily suitably aligned/sized for direct hardware DMA). */
/* TWO buffers, alternated. sceAudioOutput2OutputBlocking does not copy: it
 * waits for a free slot in the driver's queue, stores the POINTER, and
 * returns while the hardware is still reading that memory. Refilling a single
 * buffer on the next iteration therefore rewrites audio that is still being
 * played. Every PSP homebrew feeding this API double-buffers for exactly this
 * reason, including the port this file came from
 * (reference/sm64-port-psp/src/pc/audio/audio_psp.c, `snd_buffer[2]`).
 *
 * PPSSPP hides the bug -- it reads the samples out of emulated memory
 * synchronously before returning -- so this cannot be observed there. It is
 * real on hardware. */
static s16 sOutputBuffers[2][PSP_AUDIO_BLOCK_FRAMES * PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static u32 sOutputBufferIndex;

void PspAudio_Init(void) {
    sChannel = sceAudioSRCChReserve(PSP_AUDIO_BLOCK_FRAMES, PSP_AUDIO_FREQUENCY, PSP_AUDIO_CHANNELS);
}

/**
 * Output one finished AI buffer. `buf` is real N64-engine-synthesized
 * interleaved stereo s16 PCM (gAudioCtx.aiBuffers[index]);  `numSamples` is
 * frame count (stereo sample pairs), matching aiBufLengths[index].
 */
void PspAudio_Output(const s16* buf, u32 numSamples) {
    u32 i;

    if (numSamples == 0) {
        /* Nothing to queue (happens around audio-heap resets). This thread now
         * outranks the render loop, and its ONLY blocking point is the output
         * call below -- returning straight away here would spin at high
         * priority and freeze the game. Yield instead. */
        sceKernelDelayThread(1000);
        return;
    }
    if (numSamples > PSP_AUDIO_RING_FRAMES) {
        numSamples = PSP_AUDIO_RING_FRAMES; /* cannot happen; never overrun the ring */
    }

    /* Overwriting unplayed audio would be worse than dropping the newest tick,
     * but neither should ever happen: the drain loop below runs until the ring
     * is under one block, so it can only fill up if the hardware stops
     * consuming entirely. */
    if (sRingFill + numSamples > PSP_AUDIO_RING_FRAMES) {
        sStatReserveFailures++;
        return;
    }

    for (i = 0; i < numSamples; i++) {
        sRing[sRingWrite * PSP_AUDIO_CHANNELS + 0] = buf[i * PSP_AUDIO_CHANNELS + 0];
        sRing[sRingWrite * PSP_AUDIO_CHANNELS + 1] = buf[i * PSP_AUDIO_CHANNELS + 1];
        sRingWrite = (sRingWrite + 1) % PSP_AUDIO_RING_FRAMES;
    }
    sRingFill += numSamples;

    if (sChannel < 0) {
        sChannel = sceAudioSRCChReserve(PSP_AUDIO_BLOCK_FRAMES, PSP_AUDIO_FREQUENCY, PSP_AUDIO_CHANNELS);
        if (sChannel < 0) {
            sStatReserveFailures++;
            /* Without a channel sceAudioOutput2OutputBlocking returns instantly,
             * and since it is this thread's only pacer (see AudioMgr_ThreadEntry's
             * TARGET_PSP branch) returning here would spin the CPU. Yield for
             * roughly one block instead. */
            sceKernelDelayThread(1000 * PSP_AUDIO_BLOCK_FRAMES / (PSP_AUDIO_FREQUENCY / 1000));
            sRingFill = 0;
            sRingWrite = 0;
            return;
        }
    }

    if (sRingFill < PSP_AUDIO_BLOCK_FRAMES) {
        /* Same hazard as the numSamples == 0 case: a tick that queues less than
         * one block does not reach the blocking call. Two such ticks in a row
         * cannot happen at the engine's real buffer size, but a short buffer
         * during a spec change could, so bound it rather than assume. */
        sNoOutputStreak++;
        if (sNoOutputStreak > 4) {
            sceKernelDelayThread(1000);
            sNoOutputStreak = 0;
        }
    } else {
        sNoOutputStreak = 0;
    }

    while (sRingFill >= PSP_AUDIO_BLOCK_FRAMES) {
        u32 readPos = (sRingWrite + PSP_AUDIO_RING_FRAMES - sRingFill) % PSP_AUDIO_RING_FRAMES;
        s16* out = sOutputBuffers[sOutputBufferIndex];
        int32_t peak = 0;

        for (i = 0; i < PSP_AUDIO_BLOCK_FRAMES; i++) {
            s16 l = sRing[readPos * PSP_AUDIO_CHANNELS + 0];
            s16 r = sRing[readPos * PSP_AUDIO_CHANNELS + 1];
            int32_t al = l < 0 ? -l : l;
            int32_t ar = r < 0 ? -r : r;

            out[i * PSP_AUDIO_CHANNELS + 0] = l;
            out[i * PSP_AUDIO_CHANNELS + 1] = r;
            if (al > peak) {
                peak = al;
            }
            if (ar > peak) {
                peak = ar;
            }
            readPos = (readPos + 1) % PSP_AUDIO_RING_FRAMES;
        }
        sRingFill -= PSP_AUDIO_BLOCK_FRAMES;

        {
            /* Read BEFORE handing over the next block: zero means the DAC ran
             * dry while we were away, so there is an audible gap. */
            int rest = sceAudioOutput2GetRestSample();

            if (rest <= 0) {
                sStatUnderruns++;
            }
            if (rest >= 0 && (uint32_t)rest < sStatMinRest) {
                sStatMinRest = (uint32_t)rest;
            }
        }

        sceKernelDcacheWritebackInvalidateRange(out, PSP_AUDIO_BLOCK_FRAMES * PSP_AUDIO_CHANNELS * sizeof(s16));

        sStatOutputCalls++;
        sStatLastNumSamples = PSP_AUDIO_BLOCK_FRAMES;
        sStatLastPeakSample = peak;

        /* Blocks until the hardware has room -- this is the audio thread's
         * clock, standing in for the N64's VI retrace interrupt. */
        sStatLastOutputRet = sceAudioOutput2OutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        if (sStatLastOutputRet < 0) {
            sStatOutputErrors++;
        }
        /* The driver keeps reading `out` after this returns, so the next
         * block must go into the other buffer. */
        sOutputBufferIndex ^= 1;
    }

    /* One AI buffer == one synthesis pass, and this call is the last thing
     * that happens in it, so this is where a per-frame command count is
     * complete. See psp_audio_mixer.h. */
    PspAudioMixer_ResetStats();
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
 * It is not a diagnostic: AudioThread_UpdateImpl uses it as the FEEDBACK
 * TERM of the engine's own buffer-size regulator,
 *
 *     aiBufLengths[i] = ((samplesPerFrameTarget - remaining + 0x80) & ~0xF) + 0x10
 *
 * clamped to [minAiBufferLength, maxAiBufferLength] -- a band of only +-0x10
 * around the target.
 *
 * Returning a constant 0 was honest only while PspAudio_Output blocked on
 * every call, and stopped being true once the ring buffer went in (a tick
 * queueing less than one block never reaches the blocking call). A constant 0
 * does not merely lose accuracy: the subtraction always exceeds the maximum,
 * so the engine produced maxAiBufferLength every single tick with its own
 * regulator effectively switched off.
 *
 * Be precise about what reporting the truth does and does not buy, because
 * the measured numbers are less flattering than they first looked. This build
 * runs PAL: gAudioCtx.refreshRate is 50 and ticksPerUpdate is 4 (confirmed
 * live -- the synthesis updateIndex reaches 3), so samplesPerFrameTarget is
 * 640 and the band is [624, 656]. The hardware consumes a fixed 32000
 * frames/s, so production pins the tick rate:
 *
 *     constant 0 -> 656 per tick -> 32000/656 = 48.8 Hz, 2.4% slow
 *     true backlog -> 624 per tick -> 32000/624 = 51.3 Hz, 2.6% fast
 *
 * In steady state that is a wash. The regulator cannot do better here, and it
 * is worth knowing why rather than assuming it now works: on N64 this is read
 * with the AI FIFO nearly empty, so the engine steers a backlog of ~128
 * samples, whereas this port's backlog is one to two sceAudio blocks plus the
 * ring -- held there by the blocking output call, not by how much the engine
 * produces. The feedback term therefore sits far above samplesPerFrameTarget
 * and the expression clamps every tick either way.
 *
 * What it does buy is correct behaviour when the backlog is genuinely small:
 * at startup and after an audio-heap reset the engine now sees a real, low
 * value and produces more to catch up, instead of being told 0 and happening
 * to do the right thing for the wrong reason.
 *
 * Returned in BYTES (the caller divides by 4), matching the AI register.
 */
u32 osAiGetLength(void) {
    int rest = sceAudioOutput2GetRestSample();

    if (rest < 0) {
        rest = 0;
    }
    return (sRingFill + (u32)rest) * 4;
}
