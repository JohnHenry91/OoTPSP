#ifndef PSP_AUDIO_MIXER_H
#define PSP_AUDIO_MIXER_H

/* Software implementation of the N64 RSP audio microcode (aspMain).
 *
 * WHY THIS EXISTS -- read psp/docs/AUDIO_N64_VS_PSP.md section 1 first.
 * src/audio/internal/synthesis.c does NOT synthesize audio: every a*() call
 * in it is a macro from include/ultra64/abi.h that packs two words into an
 * Acmd struct, and AudioThread_UpdateImpl then hands the finished list to
 * the RSP as an M_AUDTASK. The RSP is what actually decodes ADPCM,
 * resamples, envelope-mixes and interleaves into gAudioCtx.aiBuffers[].
 * With no RSP on PSP, that task is never run and the AI buffers stay
 * all-zero -- which is exactly the silence this port had.
 *
 * The fix every N64-decomp port uses: #undef the ABI macros and redefine
 * them to call C functions that perform the operation immediately. That
 * turns AudioSynth_Update from a list-builder into a real synthesizer
 * without editing the decomp's logic at all. `pkt` is still evaluated
 * (usually `cmd++`) so the command count AudioSynth_Update reports stays
 * truthful for debugging, exactly as on N64 -- the buffer it walks is sized
 * for that count by the real engine, so this cannot overrun.
 *
 * The implementations come from reference/shipwright-vita's
 * libultraship/src/audio/mixer.c (Ship of Harkinian's OoT-specific
 * software microcode, itself descended from sm64-port's src/pc/mixer.c --
 * reference/sm64-port-psp has a copy of that lineage too). Only the macro
 * layer below is written for this port, because this decomp's abi.h uses
 * slightly different parameter lists than SoH's (notably aAddMixer, which
 * carries a gain argument here that the microcode ignores).
 *
 * Include this AFTER ultra64.h in any file that emits audio commands.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ultra64/abi.h"

#undef aADPCMdec
#undef aAddMixer
#undef aClearBuffer
#undef aDMEMMove
#undef aDuplicate
#undef aEnvMixer
#undef aEnvSetup1
#undef aEnvSetup2
#undef aFilter
#undef aHiLoGain
#undef aInterl
#undef aInterleave
#undef aLoadADPCM
#undef aLoadBuffer
#undef aMix
#undef aResample
#undef aResampleZoh
#undef aS8Dec
#undef aSaveBuffer
#undef aSegment
#undef aSetBuffer
#undef aSetLoop
#undef aUnkCmd3
#undef aUnkCmd19

void aClearBufferImpl(uint16_t addr, int nbytes);
void aLoadBufferImpl(const void* sourceAddr, uint16_t destAddr, uint16_t nbytes);
void aSaveBufferImpl(uint16_t sourceAddr, int16_t* destAddr, uint16_t nbytes);
void aLoadADPCMImpl(int numEntriesTimes16, const int16_t* bookSourceAddr);
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes);
void aInterleaveImpl(uint16_t dest, uint16_t left, uint16_t right, uint16_t c);
void aDMEMMoveImpl(uint16_t inAddr, uint16_t outAddr, int nbytes);
void aSetLoopImpl(int16_t* adpcmLoopState);
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state);
void aS8DecImpl(uint8_t flags, ADPCM_STATE state);
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state);
void aResampleZohImpl(uint16_t pitch, uint16_t startFract);
void aEnvSetup1Impl(uint8_t initialVolWet, uint16_t rateWet, uint16_t rateLeft, uint16_t rateRight);
void aEnvSetup2Impl(uint16_t initialVolLeft, uint16_t initialVolRight);
void aEnvMixerImpl(uint16_t inAddr, uint16_t nSamples, bool swapReverb, bool neg3, bool neg2, bool negLeft,
                   bool negRight, uint32_t wetDryAddr);
void aMixImpl(uint16_t count, int16_t gain, uint16_t inAddr, uint16_t outAddr);
void aAddMixerImpl(uint16_t count, uint16_t inAddr, uint16_t outAddr);
void aDuplicateImpl(uint16_t count, uint16_t inAddr, uint16_t outAddr);
void aInterlImpl(uint16_t inAddr, uint16_t outAddr, uint16_t nSamples);
void aFilterImpl(uint8_t flags, uint16_t countOrBuf, int16_t* stateOrFilter);
void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t addr);
void aUnkCmd19Impl(uint8_t f, uint16_t count, uint16_t outAddr, uint16_t inAddr);

/* Statistics for the audio HUD / WebSocket debugger: how much work the
 * software microcode actually did last frame. Zero commands with a running
 * PspAudio_StatOutputCalls() means synthesis is being asked for silence
 * (no active notes), which is a different bug class than a broken mixer. */
uint32_t PspAudioMixer_StatCommands(void);
void PspAudioMixer_ResetStats(void);

/* `pkt` is evaluated for its side effect (`cmd++`) and discarded, matching
 * the real macros, which do `Acmd* _a = (Acmd*)pkt;` and never re-evaluate. */
#define PSP_ACMD(pkt, call)      \
    do {                         \
        (void)(pkt);             \
        PspAudioMixer_CountCmd(); \
        call;                    \
    } while (0)

void PspAudioMixer_CountCmd(void);

#define aSegment(pkt, s, b) \
    do {                    \
    } while (0)
/* Lengths go through this. On N64 the abi.h macros pack them into a 16-bit
 * field, so a caller computing a nonsensical (negative or huge) length has it
 * truncated before the microcode ever sees it. Passing the raw C value let a
 * negative count reach memcpy/memmove as a ~4 GB size_t. */
#define PSP_ACMD_LEN(c) ((int)(uint16_t)(c))

#define aClearBuffer(pkt, dmem, size) PSP_ACMD(pkt, aClearBufferImpl(dmem, PSP_ACMD_LEN(size)))
#define aLoadBuffer(pkt, addrSrc, dmemDest, size) PSP_ACMD(pkt, aLoadBufferImpl(addrSrc, dmemDest, PSP_ACMD_LEN(size)))
#define aSaveBuffer(pkt, dmemSrc, addrDest, size) PSP_ACMD(pkt, aSaveBufferImpl(dmemSrc, addrDest, PSP_ACMD_LEN(size)))
#define aLoadADPCM(pkt, c, d) PSP_ACMD(pkt, aLoadADPCMImpl(c, d))
#define aSetBuffer(pkt, f, i, o, c) PSP_ACMD(pkt, aSetBufferImpl(f, i, o, c))
#define aInterleave(pkt, o, l, r, c) PSP_ACMD(pkt, aInterleaveImpl(o, l, r, PSP_ACMD_LEN(c)))
#define aDMEMMove(pkt, i, o, c) PSP_ACMD(pkt, aDMEMMoveImpl(i, o, PSP_ACMD_LEN(c)))
#define aSetLoop(pkt, a) PSP_ACMD(pkt, aSetLoopImpl(a))
#define aADPCMdec(pkt, f, s) PSP_ACMD(pkt, aADPCMdecImpl(f, s))
#define aS8Dec(pkt, f, s) PSP_ACMD(pkt, aS8DecImpl(f, s))
#define aResample(pkt, f, p, s) PSP_ACMD(pkt, aResampleImpl(f, p, s))
#define aResampleZoh(pkt, pitch, pitchAccu) PSP_ACMD(pkt, aResampleZohImpl(pitch, pitchAccu))
#define aEnvSetup1(pkt, a, b, c, d) PSP_ACMD(pkt, aEnvSetup1Impl(a, b, c, d))
#define aEnvSetup2(pkt, volLeft, volRight) PSP_ACMD(pkt, aEnvSetup2Impl(volLeft, volRight))
/* `bits` is the packed opcode word (sEnvMixerOp) the real macro ORs into w0;
 * the operation itself carries no information beyond the opcode, so it is
 * dropped here rather than passed along as an unused argument. */
#define aEnvMixer(pkt, dmemi, count, swapLR, x0, x1, x2, x3, m, bits) \
    PSP_ACMD(pkt, aEnvMixerImpl(dmemi, count, swapLR, x0, x1, x2, x3, m))
#define aMix(pkt, f, g, i, o) PSP_ACMD(pkt, aMixImpl(f, g, i, o))
/* This decomp's aAddMixer carries a gain (`a4`, always 0x7FFF = unity at
 * every call site); A_ADDMIXER is a plain saturating add, so it is unused. */
#define aAddMixer(pkt, count, dmemi, dmemo, a4) PSP_ACMD(pkt, aAddMixerImpl(count, dmemi, dmemo))
#define aDuplicate(pkt, numCopies, dmemSrc, dmemDest) PSP_ACMD(pkt, aDuplicateImpl(numCopies, dmemSrc, dmemDest))
#define aInterl(pkt, dmemi, dmemo, count) PSP_ACMD(pkt, aInterlImpl(dmemi, dmemo, PSP_ACMD_LEN(count)))
#define aFilter(pkt, f, countOrBuf, addr) PSP_ACMD(pkt, aFilterImpl(f, countOrBuf, addr))
#define aHiLoGain(pkt, gain, count, dmem, a4) PSP_ACMD(pkt, aHiLoGainImpl(gain, count, dmem))
#define aUnkCmd3(pkt, a1, a2, a3) \
    do {                          \
        (void)(pkt);              \
    } while (0)
#define aUnkCmd19(pkt, a1, a2, a3, a4) PSP_ACMD(pkt, aUnkCmd19Impl(a1, a2, a3, a4))

#endif /* PSP_AUDIO_MIXER_H */
