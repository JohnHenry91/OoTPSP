#ifndef PSP_AUDIO_PROBE_H
#define PSP_AUDIO_PROBE_H

#include <stdint.h>

/* Catches the exact moment a voice's playback position runs past the end of
 * its sample -- the fault that produces the short bursts of white noise heard
 * over otherwise-correct music.
 *
 * What is already established and must not be re-measured: the audio engine
 * is byte-identical to the pristine decomp (`diff` against reference/oot),
 * the software microcode matches libultraship's line for line, the sample
 * banks are byte-identical to the ROM, and the sample DMA never fails. The
 * finished PCM shows one voice emitting ~112 samples of noise (zero-crossing
 * rate 0.49) on top of music that continues underneath. The cause is that
 * `synthState->samplePosInt` exceeds the sample's `loop->header.end`, after
 * which synthesis.c's decode loop counts `nSamplesProcessed` DOWNWARD and
 * decodes hundreds of frames from beyond the end of the sample.
 *
 * The open question is only WHY the position ends up out of range. An earlier
 * probe suggested the note is handed a different, shorter sample while
 * keeping its position -- but it sampled per voice per chunk and could not
 * see whether `needsInit` was set, nor whether the note's own copy of the
 * state and the per-tick slot disagree. This one records exactly that, at the
 * one line where the overrun becomes visible.
 *
 * Cost: a single comparison per voice per chunk. The previous probe walked
 * every sample of every voice (~12,700 extra iterations per tick) and was
 * audible in its own measurement -- that must not happen again.
 */
typedef struct PspAudioOverrun {
    uint32_t taskCount;      /* gAudioCtx.totalTaskCount, to order events */
    uint32_t noteIndex;
    uint32_t updateIndex;    /* which of the ticksPerUpdate slots */
    uint32_t needsInit;      /* was this chunk an A_INIT? */
    uint32_t samplePosInt;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t loopCount;
    uint32_t sampleAddr;     /* per-tick slot's sample */
    uint32_t sampleSize;
    uint32_t noteSampleAddr; /* the NOTE's own copy -- differs if they got out of step */
    uint32_t prevSampleAddr; /* what this note played on its previous chunk */
    uint32_t prevPos;
    uint32_t prevUpdateIndex;
    uint32_t parentLayer;
    uint32_t instOrWave;
} PspAudioOverrun;

/* Called from AudioSynth_ProcessNote once per voice per chunk. Records only
 * when samplePosInt has already passed loopEnd. */
void PspAudioProbe_CheckOverrun(int32_t noteIndex, int32_t updateIndex, int32_t needsInit, int32_t samplePosInt,
                                const void* sampleStatePtr, const void* samplePtr, const void* loopPtr);

void PspAudioProbe_Flush(void); /* main thread only -- writes ms0:/oot_overrun.bin */
uint32_t PspAudioProbe_StatHits(void);

#endif
