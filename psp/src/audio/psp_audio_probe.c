/* See psp/include/psp_audio_probe.h for what this is looking for. */
#include <pspiofilemgr.h>
#include <string.h>

#include "ultra64.h"
#include "audio.h"
#include "psp_audio_probe.h"

#define PSP_PROBE_MAX_HITS 256
#define PSP_PROBE_MAX_NOTES 32

static PspAudioOverrun sRecords[PSP_PROBE_MAX_HITS];
static uint32_t sRecordCount;
static uint32_t sHits;

/* What each note played on its previous chunk, updated for EVERY voice --
 * the chunk before the fault is the interesting one, and it is by definition
 * not itself a hit. */
static uint32_t sPrevSampleAddr[PSP_PROBE_MAX_NOTES];
static uint32_t sPrevPos[PSP_PROBE_MAX_NOTES];
static uint32_t sPrevUpdateIndex[PSP_PROBE_MAX_NOTES];

uint32_t PspAudioProbe_StatHits(void) {
    return sHits;
}

void PspAudioProbe_CheckOverrun(int32_t noteIndex, int32_t updateIndex, int32_t needsInit, int32_t samplePosInt,
                                const void* sampleStatePtr, const void* samplePtr, const void* loopPtr) {
    const NoteSampleState* sampleState = (const NoteSampleState*)sampleStatePtr;
    const Sample* sample = (const Sample*)samplePtr;
    const AdpcmLoop* loop = (const AdpcmLoop*)loopPtr;
    uint32_t ni = (uint32_t)noteIndex < PSP_PROBE_MAX_NOTES ? (uint32_t)noteIndex : 0;

    if (sample != NULL && loop != NULL && samplePosInt > (int32_t)loop->header.end) {
        sHits++;
        if (sRecordCount < PSP_PROBE_MAX_HITS) {
            PspAudioOverrun* r = &sRecords[sRecordCount++];
            const Note* note = &gAudioCtx.notes[noteIndex];

            r->taskCount = gAudioCtx.totalTaskCount;
            r->noteIndex = (uint32_t)noteIndex;
            r->updateIndex = (uint32_t)updateIndex;
            r->needsInit = (uint32_t)needsInit;
            r->samplePosInt = (uint32_t)samplePosInt;
            r->loopStart = loop->header.start;
            r->loopEnd = loop->header.end;
            r->loopCount = loop->header.count;
            r->sampleAddr = (uint32_t)sample->sampleAddr;
            r->sampleSize = sample->size;
            /* The note's OWN copy of the state, not the per-tick slot. If the
             * two disagree the note and its slot got out of step. */
            r->noteSampleAddr = (note->sampleState.tunedSample != NULL && !note->sampleState.bitField1.isSyntheticWave &&
                                 note->sampleState.tunedSample->sample != NULL)
                                    ? (uint32_t)note->sampleState.tunedSample->sample->sampleAddr
                                    : 0;
            r->prevSampleAddr = sPrevSampleAddr[ni];
            r->prevPos = sPrevPos[ni];
            r->prevUpdateIndex = sPrevUpdateIndex[ni];
            r->parentLayer = (uint32_t)note->playbackState.parentLayer;
            r->instOrWave = (note->playbackState.parentLayer != NULL &&
                             (uint32_t)note->playbackState.parentLayer > 0x7FFFFFFFu)
                                ? 0xFFFFFFFFu
                                : 0;
            (void)sampleState;
        }
    }

    if (sample != NULL) {
        sPrevSampleAddr[ni] = (uint32_t)sample->sampleAddr;
    }
    sPrevPos[ni] = (uint32_t)samplePosInt;
    sPrevUpdateIndex[ni] = (uint32_t)updateIndex;
}

void PspAudioProbe_Flush(void) {
    SceUID fd;

    if (sRecordCount == 0) {
        return;
    }
    fd = sceIoOpen("ms0:/oot_overrun.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    sceIoWrite(fd, sRecords, (int)(sRecordCount * sizeof(sRecords[0])));
    sceIoClose(fd);
}
