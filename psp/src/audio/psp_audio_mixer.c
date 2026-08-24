/* Software implementation of the N64 RSP audio microcode (aspMain).
 *
 * See psp/include/psp_audio_mixer.h for why this file exists, and
 * psp/docs/AUDIO_N64_VS_PSP.md section 1 for the full N64 audio-pipeline
 * background. Short version: src/audio/internal/synthesis.c only *emits*
 * DSP commands; without an RSP to run them, gAudioCtx.aiBuffers[] is silence.
 *
 * The operations below are ported from reference/shipwright-vita's
 * libultraship/src/audio/mixer.c (Ship of Harkinian's OoT software
 * microcode; the same file is vendored at soh/soh/mixer.c). That lineage
 * traces back to sm64-port's src/pc/mixer.c, a copy of which is also in
 * reference/sm64-port-psp -- but SM64's variant lacks roughly ten opcodes
 * OoT's microcode uses (aEnvSetup1/2, aS8Dec, aAddMixer, aDuplicate,
 * aResampleZoh, aFilter, aInterl, ...), so the OoT-specific one is the
 * correct reference here.
 *
 * Deliberately kept as close to that reference as possible: this is a
 * bit-exact emulation of fixed-point DSP hardware, and "tidying" the
 * saturation, rounding or accumulator widths silently changes how the game
 * sounds. Only naming, the DMEM window, and the stats counter are ours.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ultra64.h"
#include "psp_audio_mixer.h"

#ifndef __clang__
#pragma GCC optimize("unroll-loops")
#endif

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xF)

/* DMEM, and what happens when an access falls outside it.
 *
 * Real DMEM is a flat 4 KiB and the hardware masks every address to 12 bits,
 * so an out-of-range access wraps inside the RSP's own scratchpad and can
 * never reach anything else. This port has to reproduce that CONTAINMENT,
 * because the addresses do go out of range here.
 *
 * Measured, not assumed: when a voice's samplePosInt has run past its
 * sample's loop end, synthesis.c's decode loop drives `nSamplesProcessed`
 * NEGATIVE, and the noteFinished path then issues
 *
 *     AudioSynth_ClearBuffer(cmd++, DMEM_UNCOMPRESSED_NOTE + s5,
 *                            (samplesLenAdjusted - nSamplesProcessed) * SAMPLE_SIZE)
 *
 * with a length of up to 55114 bytes -- 54 KiB of memset. Overruns of that
 * shape were logged 87 times in 30 seconds of play. Against the old 3648-byte
 * window that wiped ~50 KiB of neighbouring globals every time, including
 * other voices' state, which is almost certainly why the fault looked
 * self-sustaining and why a note could end up playing a sample its own
 * NoteSampleState disagreed with.
 *
 * Two containment strategies were tried; only one is right:
 *
 *   - Masking to 12 bits, as the hardware does, stops the corruption but
 *     makes the fallout AUDIBLE. During the same overrun `aligned` grows past
 *     0x940, so `addr = DMEM_COMPRESSED_ADPCM_DATA - aligned` goes negative
 *     and arrives as a large u16 such as 0xFF00; masking folds that onto
 *     0x0F00, a live DMEM address. Measured: peak rose from 4130 to full
 *     scale with single-sample swings of 65533, and the artefacts turned from
 *     noise bursts into hard cracks. Masking is only correct when the
 *     addresses are correct, which on N64 they always are.
 *   - Giving every in-ABI address and length somewhere harmless to land.
 *     The real 4 KiB sits at DMEM_LEAD; addresses are u16 and lengths are
 *     u16, so a span of 0x10000 on either side means nothing can escape the
 *     allocation, and everything outside the real 4 KiB is silent dead space.
 *
 * DMEM_LEAD also covers aResampleImpl, which deliberately steps its input
 * pointer up to 8 samples backwards to reach the previous chunk's filter
 * tail.
 *
 * This contains the damage; it does not fix the overrun itself, which is
 * still open.
 */
#define DMEM_LEAD 0x40
#define DMEM_ADDR_SPAN 0x10000 /* any u16 address */
#define DMEM_TAIL 0x10000      /* any u16 length from any of those addresses */
#define DMEM_BUF_SIZE (DMEM_LEAD + DMEM_ADDR_SPAN + DMEM_TAIL)

#define BUF_U8(a) (sRspa.buf.asU8 + DMEM_LEAD + (uint16_t)(a))
#define BUF_S16(a) (sRspa.buf.asS16 + (DMEM_LEAD + (uint16_t)(a)) / (int)sizeof(int16_t))

/* The microcode's register file: everything an a*() command sets up for a
 * later command to consume. */
static struct {
    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    uint16_t vol[2];
    uint16_t rate[2];
    uint16_t volWet;
    uint16_t rateWet;

    int16_t* adpcmLoopState;

    int16_t adpcmTable[8][2][8];

    uint16_t filterCount;
    int16_t filter[8];

    union {
        int16_t asS16[DMEM_BUF_SIZE / sizeof(int16_t)];
        uint8_t asU8[DMEM_BUF_SIZE];
    } buf;
} sRspa;

static uint32_t sStatCommands = 0;
static uint32_t sStatCommandsPrev = 0;

void PspAudioMixer_CountCmd(void) {
    sStatCommands++;
}

uint32_t PspAudioMixer_StatCommands(void) {
    return sStatCommandsPrev;
}

void PspAudioMixer_ResetStats(void) {
    sStatCommandsPrev = sStatCommands;
    sStatCommands = 0;
}

/* The RSP's 4-tap polyphase resampling filter table, 64 phases. */
static const int16_t sResampleTable[64][4] = {
    { 0x0C39, 0x66AD, 0x0D46, 0xFFDF }, { 0x0B39, 0x6696, 0x0E5F, 0xFFD8 }, { 0x0A44, 0x6669, 0x0F83, 0xFFD0 },
    { 0x095A, 0x6626, 0x10B4, 0xFFC8 }, { 0x087D, 0x65CD, 0x11F0, 0xFFBF }, { 0x07AB, 0x655E, 0x1338, 0xFFB6 },
    { 0x06E4, 0x64D9, 0x148C, 0xFFAC }, { 0x0628, 0x643F, 0x15EB, 0xFFA1 }, { 0x0577, 0x638F, 0x1756, 0xFF96 },
    { 0x04D1, 0x62CB, 0x18CB, 0xFF8A }, { 0x0435, 0x61F3, 0x1A4C, 0xFF7E }, { 0x03A4, 0x6106, 0x1BD7, 0xFF71 },
    { 0x031C, 0x6007, 0x1D6C, 0xFF64 }, { 0x029F, 0x5EF5, 0x1F0B, 0xFF56 }, { 0x022A, 0x5DD0, 0x20B3, 0xFF48 },
    { 0x01BE, 0x5C9A, 0x2264, 0xFF3A }, { 0x015B, 0x5B53, 0x241E, 0xFF2C }, { 0x0101, 0x59FC, 0x25E0, 0xFF1E },
    { 0x00AE, 0x5896, 0x27A9, 0xFF10 }, { 0x0063, 0x5720, 0x297A, 0xFF02 }, { 0x001F, 0x559D, 0x2B50, 0xFEF4 },
    { 0xFFE2, 0x540D, 0x2D2C, 0xFEE8 }, { 0xFFAC, 0x5270, 0x2F0D, 0xFEDB }, { 0xFF7C, 0x50C7, 0x30F3, 0xFED0 },
    { 0xFF53, 0x4F14, 0x32DC, 0xFEC6 }, { 0xFF2E, 0x4D57, 0x34C8, 0xFEBD }, { 0xFF0F, 0x4B91, 0x36B6, 0xFEB6 },
    { 0xFEF5, 0x49C2, 0x38A5, 0xFEB0 }, { 0xFEDF, 0x47ED, 0x3A95, 0xFEAC }, { 0xFECE, 0x4611, 0x3C85, 0xFEAB },
    { 0xFEC0, 0x4430, 0x3E74, 0xFEAC }, { 0xFEB6, 0x424A, 0x4060, 0xFEAF }, { 0xFEAF, 0x4060, 0x424A, 0xFEB6 },
    { 0xFEAC, 0x3E74, 0x4430, 0xFEC0 }, { 0xFEAB, 0x3C85, 0x4611, 0xFECE }, { 0xFEAC, 0x3A95, 0x47ED, 0xFEDF },
    { 0xFEB0, 0x38A5, 0x49C2, 0xFEF5 }, { 0xFEB6, 0x36B6, 0x4B91, 0xFF0F }, { 0xFEBD, 0x34C8, 0x4D57, 0xFF2E },
    { 0xFEC6, 0x32DC, 0x4F14, 0xFF53 }, { 0xFED0, 0x30F3, 0x50C7, 0xFF7C }, { 0xFEDB, 0x2F0D, 0x5270, 0xFFAC },
    { 0xFEE8, 0x2D2C, 0x540D, 0xFFE2 }, { 0xFEF4, 0x2B50, 0x559D, 0x001F }, { 0xFF02, 0x297A, 0x5720, 0x0063 },
    { 0xFF10, 0x27A9, 0x5896, 0x00AE }, { 0xFF1E, 0x25E0, 0x59FC, 0x0101 }, { 0xFF2C, 0x241E, 0x5B53, 0x015B },
    { 0xFF3A, 0x2264, 0x5C9A, 0x01BE }, { 0xFF48, 0x20B3, 0x5DD0, 0x022A }, { 0xFF56, 0x1F0B, 0x5EF5, 0x029F },
    { 0xFF64, 0x1D6C, 0x6007, 0x031C }, { 0xFF71, 0x1BD7, 0x6106, 0x03A4 }, { 0xFF7E, 0x1A4C, 0x61F3, 0x0435 },
    { 0xFF8A, 0x18CB, 0x62CB, 0x04D1 }, { 0xFF96, 0x1756, 0x638F, 0x0577 }, { 0xFFA1, 0x15EB, 0x643F, 0x0628 },
    { 0xFFAC, 0x148C, 0x64D9, 0x06E4 }, { 0xFFB6, 0x1338, 0x655E, 0x07AB }, { 0xFFBF, 0x11F0, 0x65CD, 0x087D },
    { 0xFFC8, 0x10B4, 0x6626, 0x095A }, { 0xFFD0, 0x0F83, 0x6669, 0x0A44 }, { 0xFFD8, 0x0E5F, 0x6696, 0x0B39 },
    { 0xFFDF, 0x0D46, 0x66AD, 0x0C39 },
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7FFF) {
        return 0x7FFF;
    }
    return (int16_t)v;
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(BUF_U8(addr), 0, nbytes);
}

/* Containment: these three functions are the ONLY places this microcode
 * touches memory outside its own DMEM window, and every one of them is a raw
 * memcpy against an address the engine computed.
 *
 * On N64 that is safe by construction -- the RSP DMAs within RDRAM, and an
 * address that has run past the end of a sample still lands on other RAM. It
 * reads nonsense and the note sounds wrong, which is exactly the "sound
 * glitches, Ganon's Castle is completely broken" symptom this port has had
 * for months. PPSSPP reproduces the harmless half for the same reason: guest
 * RAM is one flat host allocation with nothing to fault against.
 *
 * On real PSP hardware the same address leaves the user partition and the
 * console loses power outright -- no exception screen, no freeze. Guarding
 * the sample POINTERS in synthesis.c narrowed the window (the bring-up
 * ladder's level 10 went from dying to returning to the XMB) but did not
 * close it, because the fatal read is not always a bad pointer: a valid
 * pointer plus an overrun LENGTH walks off the end just as well.
 *
 * So bound it here instead of trying to predict it upstream. Whatever cannot
 * be read is zero-filled, which is silence -- the worst case is a note that
 * goes quiet, never a console that switches off. gPspMixerBadRanges says how
 * often it fired, so this stays a diagnosis and not just a bandage. */
#define PSP_MIXER_RAM_START 0x08000000U
#define PSP_MIXER_RAM_END 0x0C000000U

uint32_t gPspMixerBadRanges;

/* How many of `nbytes` from `addr` are actually inside PSP user RAM. */
static int MixerReadableBytes(const void* addr, int nbytes) {
    uint32_t a = (uint32_t)(uintptr_t)addr;

    if (nbytes <= 0) {
        return 0;
    }
    if ((a < PSP_MIXER_RAM_START) || (a >= PSP_MIXER_RAM_END)) {
        gPspMixerBadRanges++;
        return 0;
    }
    if ((a + (uint32_t)nbytes) > PSP_MIXER_RAM_END) {
        gPspMixerBadRanges++;
        return (int)(PSP_MIXER_RAM_END - a);
    }
    return nbytes;
}

void aLoadBufferImpl(const void* sourceAddr, uint16_t destAddr, uint16_t nbytes) {
    int want = ROUND_DOWN_16(nbytes);
    int have = MixerReadableBytes(sourceAddr, want);

    if (have > 0) {
        memcpy(BUF_U8(destAddr), sourceAddr, have);
    }
    if (have < want) {
        memset(BUF_U8(destAddr) + have, 0, want - have);
    }
}

void aSaveBufferImpl(uint16_t sourceAddr, int16_t* destAddr, uint16_t nbytes) {
    int want = ROUND_DOWN_16(nbytes);
    int have = MixerReadableBytes(destAddr, want);

    /* A write, so a clamped range is simply not written -- there is nothing
     * safe to put outside RAM. */
    if (have > 0) {
        memcpy(destAddr, BUF_S16(sourceAddr), have);
    }
}

void aLoadADPCMImpl(int numEntriesTimes16, const int16_t* bookSourceAddr) {
    int have = MixerReadableBytes(bookSourceAddr, numEntriesTimes16);

    if (have > 0) {
        memcpy(sRspa.adpcmTable, bookSourceAddr, have);
    }
    if (have < numEntriesTimes16) {
        memset((uint8_t*)sRspa.adpcmTable + have, 0, numEntriesTimes16 - have);
    }
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    sRspa.in = in;
    sRspa.out = out;
    sRspa.nbytes = nbytes;
}

void aInterleaveImpl(uint16_t dest, uint16_t left, uint16_t right, uint16_t c) {
    int count = ROUND_UP_8(c) / (int)sizeof(int16_t) / 4;
    int16_t* l = BUF_S16(left);
    int16_t* r = BUF_S16(right);
    int16_t* d = BUF_S16(dest);

    while (count > 0) {
        int16_t l0 = *l++;
        int16_t l1 = *l++;
        int16_t l2 = *l++;
        int16_t l3 = *l++;
        int16_t r0 = *r++;
        int16_t r1 = *r++;
        int16_t r2 = *r++;
        int16_t r3 = *r++;

        *d++ = l0;
        *d++ = r0;
        *d++ = l1;
        *d++ = r1;
        *d++ = l2;
        *d++ = r2;
        *d++ = l3;
        *d++ = r3;
        count--;
    }
}

void aDMEMMoveImpl(uint16_t inAddr, uint16_t outAddr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memmove(BUF_U8(outAddr), BUF_U8(inAddr), nbytes);
}

void aSetLoopImpl(int16_t* adpcmLoopState) {
    sRspa.adpcmLoopState = adpcmLoopState;
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
    uint8_t* in = BUF_U8(sRspa.in);
    int16_t* out = BUF_S16(sRspa.out);
    int nbytes = ROUND_UP_32(sRspa.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, sRspa.adpcmLoopState, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;

    while (nbytes > 0) {
        int shift = *in >> 4;          // 0..12 (or 0..14 for small ADPCM)
        int tableIndex = *in++ & 0xF;  // 0..7
        const int16_t(*tbl)[8] = sRspa.adpcmTable[tableIndex];
        int i;

        for (i = 0; i < 2; i++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];
            int j;
            int k;

            /* Bit 2 selects CODEC_SMALL_ADPCM (2 bits/sample) over the
             * normal 4 bits/sample -- see synthesis.c's `flags | 4`. */
            if (flags & 4) {
                for (j = 0; j < 2; j++) {
                    ins[j * 4 + 0] = (((*in >> 6) << 30) >> 30) << shift;
                    ins[j * 4 + 1] = ((((*in >> 4) & 0x3) << 30) >> 30) << shift;
                    ins[j * 4 + 2] = ((((*in >> 2) & 0x3) << 30) >> 30) << shift;
                    ins[j * 4 + 3] = (((*in++ & 0x3) << 30) >> 30) << shift;
                }
            } else {
                for (j = 0; j < 4; j++) {
                    ins[j * 2 + 0] = (((*in >> 4) << 28) >> 28) << shift;
                    ins[j * 2 + 1] = (((*in++ & 0xF) << 28) >> 28) << shift;
                }
            }

            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);

                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
        nbytes -= 16 * (int)sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

void aS8DecImpl(uint8_t flags, ADPCM_STATE state) {
    uint8_t* in = BUF_U8(sRspa.in);
    int16_t* out = BUF_S16(sRspa.out);
    int nbytes = ROUND_UP_32(sRspa.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, sRspa.adpcmLoopState, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;

    while (nbytes > 0) {
        int i;

        for (i = 0; i < 16; i++) {
            *out++ = (int16_t)(*in++ << 8);
        }
        nbytes -= 16 * (int)sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t tmp[16];
    int16_t* inInitial = BUF_S16(sRspa.in);
    int16_t* in = inInitial;
    int16_t* out = BUF_S16(sRspa.out);
    int nbytes = ROUND_UP_16(sRspa.nbytes);
    uint32_t pitchAccumulator;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / (int)sizeof(int16_t);
    }
    in -= 4;
    pitchAccumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

    do {
        for (i = 0; i < 8; i++) {
            const int16_t* tbl = sResampleTable[pitchAccumulator * 64 >> 16];
            int32_t sample = ((in[0] * tbl[0] + 0x4000) >> 15) + ((in[1] * tbl[1] + 0x4000) >> 15) +
                             ((in[2] * tbl[2] + 0x4000) >> 15) + ((in[3] * tbl[3] + 0x4000) >> 15);

            *out++ = clamp16(sample);

            pitchAccumulator += (pitch << 1);
            in += pitchAccumulator >> 16;
            pitchAccumulator %= 0x10000;
        }
        nbytes -= 8 * (int)sizeof(int16_t);
    } while (nbytes > 0);

    state[4] = (int16_t)pitchAccumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - inInitial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}

void aResampleZohImpl(uint16_t pitch, uint16_t startFract) {
    int16_t* in = BUF_S16(sRspa.in);
    int16_t* out = BUF_S16(sRspa.out);
    int nbytes = ROUND_UP_8(sRspa.nbytes);
    uint32_t pos = startFract;
    uint32_t pitchAdd = pitch << 2;

    do {
        *out++ = in[pos >> 17];
        pos += pitchAdd;
        *out++ = in[pos >> 17];
        pos += pitchAdd;
        *out++ = in[pos >> 17];
        pos += pitchAdd;
        *out++ = in[pos >> 17];
        pos += pitchAdd;

        nbytes -= 4 * (int)sizeof(int16_t);
    } while (nbytes > 0);
}

void aEnvSetup1Impl(uint8_t initialVolWet, uint16_t rateWet, uint16_t rateLeft, uint16_t rateRight) {
    sRspa.volWet = (uint16_t)(initialVolWet << 8);
    sRspa.rateWet = rateWet;
    sRspa.rate[0] = rateLeft;
    sRspa.rate[1] = rateRight;
}

void aEnvSetup2Impl(uint16_t initialVolLeft, uint16_t initialVolRight) {
    sRspa.vol[0] = initialVolLeft;
    sRspa.vol[1] = initialVolRight;
}

void aEnvMixerImpl(uint16_t inAddr, uint16_t nSamples, bool swapReverb, bool neg3, bool neg2, bool negLeft,
                   bool negRight, uint32_t wetDryAddr) {
    int16_t* in = BUF_S16(inAddr);
    int16_t* dry[2] = { BUF_S16(((wetDryAddr >> 24) & 0xFF) << 4), BUF_S16(((wetDryAddr >> 16) & 0xFF) << 4) };
    int16_t* wet[2] = { BUF_S16(((wetDryAddr >> 8) & 0xFF) << 4), BUF_S16((wetDryAddr & 0xFF) << 4) };
    int16_t negs[4] = { negLeft ? -1 : 0, negRight ? -1 : 0, neg3 ? -4 : 0, neg2 ? -2 : 0 };
    int swapped[2] = { swapReverb ? 1 : 0, swapReverb ? 0 : 1 };
    int n = ROUND_UP_16(nSamples);

    uint16_t vols[2] = { sRspa.vol[0], sRspa.vol[1] };
    uint16_t rates[2] = { sRspa.rate[0], sRspa.rate[1] };
    uint16_t volWet = sRspa.volWet;
    uint16_t rateWet = sRspa.rateWet;

    do {
        int i;

        for (i = 0; i < 8; i++) {
            int16_t samples[2] = { *in, *in };
            int j;

            in++;
            for (j = 0; j < 2; j++) {
                samples[j] = (samples[j] * vols[j] >> 16) ^ negs[j];
            }
            for (j = 0; j < 2; j++) {
                *dry[j] = clamp16(*dry[j] + samples[j]);
                dry[j]++;
                *wet[j] = clamp16(*wet[j] + ((samples[swapped[j]] * volWet >> 16) ^ negs[2 + j]));
                wet[j]++;
            }
        }
        vols[0] += rates[0];
        vols[1] += rates[1];
        volWet += rateWet;

        n -= 8;
    } while (n > 0);
}

void aMixImpl(uint16_t count, int16_t gain, uint16_t inAddr, uint16_t outAddr) {
    int nbytes = ROUND_UP_32(ROUND_DOWN_16(count << 4));
    int16_t* in = BUF_S16(inAddr);
    int16_t* out = BUF_S16(outAddr);
    int i;

    if (gain == -0x8000) {
        while (nbytes > 0) {
            for (i = 0; i < 16; i++) {
                int32_t sample = *out - *in++;

                *out++ = clamp16(sample);
            }
            nbytes -= 16 * (int)sizeof(int16_t);
        }
        return;
    }

    while (nbytes > 0) {
        for (i = 0; i < 16; i++) {
            int32_t sample = ((*out * 0x7FFF + *in++ * gain) + 0x4000) >> 15;

            *out++ = clamp16(sample);
        }
        nbytes -= 16 * (int)sizeof(int16_t);
    }
}

void aAddMixerImpl(uint16_t count, uint16_t inAddr, uint16_t outAddr) {
    int16_t* in = BUF_S16(inAddr);
    int16_t* out = BUF_S16(outAddr);
    int nbytes = ROUND_UP_64(ROUND_DOWN_16(count));

    do {
        int i;

        for (i = 0; i < 16; i++) {
            *out = clamp16(*out + *in++);
            out++;
        }
        nbytes -= 16 * (int)sizeof(int16_t);
    } while (nbytes > 0);
}

void aDuplicateImpl(uint16_t count, uint16_t inAddr, uint16_t outAddr) {
    uint8_t* in = BUF_U8(inAddr);
    uint8_t* out = BUF_U8(outAddr);
    uint8_t tmp[128];

    memcpy(tmp, in, 128);
    do {
        memcpy(out, tmp, 128);
        out += 128;
    } while (count-- > 0);
}

void aInterlImpl(uint16_t inAddr, uint16_t outAddr, uint16_t nSamples) {
    int16_t* in = BUF_S16(inAddr);
    int16_t* out = BUF_S16(outAddr);
    int n = ROUND_UP_8(nSamples);

    do {
        int i;

        for (i = 0; i < 8; i++) {
            *out++ = *in++;
            in++;
        }
        n -= 8;
    } while (n > 0);
}

void aFilterImpl(uint8_t flags, uint16_t countOrBuf, int16_t* stateOrFilter) {
    if (flags > A_INIT) {
        sRspa.filterCount = ROUND_UP_16(countOrBuf);
        memcpy(sRspa.filter, stateOrFilter, sizeof(sRspa.filter));
    } else {
        int16_t tmp[16];
        int16_t tmp2[8];
        int count = sRspa.filterCount;
        int16_t* buf = BUF_S16(countOrBuf);
        int i;

        if (flags == A_INIT) {
            memset(tmp, 0, 8 * sizeof(int16_t));
            memset(tmp2, 0, 8 * sizeof(int16_t));
        } else {
            memcpy(tmp, stateOrFilter, 8 * sizeof(int16_t));
            memcpy(tmp2, stateOrFilter + 8, 8 * sizeof(int16_t));
        }

        for (i = 0; i < 8; i++) {
            sRspa.filter[i] = (tmp2[i] + sRspa.filter[i]) / 2;
        }

        do {
            memcpy(tmp + 8, buf, 8 * sizeof(int16_t));
            for (i = 0; i < 8; i++) {
                int64_t sample = 0x4000; // rounding term
                int j;

                for (j = 0; j < 8; j++) {
                    sample += tmp[i + j] * sRspa.filter[7 - j];
                }
                buf[i] = clamp16((int32_t)(sample >> 15));
            }
            memcpy(tmp, tmp + 8, 8 * sizeof(int16_t));

            buf += 8;
            count -= 8 * (int)sizeof(int16_t);
        } while (count > 0);

        memcpy(stateOrFilter, tmp, 8 * sizeof(int16_t));
        memcpy(stateOrFilter + 8, sRspa.filter, 8 * sizeof(int16_t));
    }
}

void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t addr) {
    int16_t* samples = BUF_S16(addr);
    int nbytes = ROUND_UP_32(count);

    do {
        int i;

        for (i = 0; i < 8; i++) {
            *samples = clamp16((*samples * g) >> 4);
            samples++;
        }
        nbytes -= 8;
    } while (nbytes > 0);
}

void aUnkCmd19Impl(uint8_t f, uint16_t count, uint16_t outAddr, uint16_t inAddr) {
    int nbytes = ROUND_UP_64(count);
    int16_t* in = BUF_S16(inAddr + f);
    int16_t* out = BUF_S16(outAddr);
    int16_t tbl[32];

    memcpy(tbl, in, 32 * sizeof(int16_t));
    do {
        int i;

        for (i = 0; i < 32; i++) {
            out[i] = clamp16(out[i] * tbl[i]);
        }
        out += 32;
        nbytes -= 32 * (int)sizeof(int16_t);
    } while (nbytes > 0);
}
