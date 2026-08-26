/* Debug-only accessors into the real audio engine's live state, for the
 * WebSocket debugger / the debug HUD (psp_scene_menu.c). PspAudio_Output
 * (audio_psp.c) already proved real, correctly-sized buffers reach the PSP
 * hardware every retrace but with peak==0 (real silent PCM) -- these
 * accessors narrow down WHERE upstream of that the signal goes to zero:
 * no sequence player enabled at all (command never reached AudioThread) vs.
 * a player enabled but its fade/channel volume is zero vs. no notes actually
 * playing under an enabled player. */

#include "ultra64.h"
#include "audio.h"
#include "audiothread_cmd.h"
#include "sequence.h"
#include "sfx.h"

/* Temporary diagnostic counter, see the use site in z_play.c's Play_Init. */
int gPspPlayInitCount = 0;

int PspAudioDebug_ActiveNoteCount(void) {
    int count = 0;
    int i;

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        if (gAudioCtx.notes[i].sampleState.bitField0.enabled) {
            count++;
        }
    }
    return count;
}

/* How many sounding voices are playing a SYNTHETIC WAVE (a sawtooth/square/
 * sine from gWaveSamples) rather than a recorded instrument sample.
 *
 * This exists to test one specific description of the remaining artefact --
 * that individual notes sound synthetic instead of like their instrument.
 * OoT does use synthetic waves deliberately, so a small nonzero count is
 * normal and proves nothing on its own. What would be diagnostic is the
 * count tracking the artefact: rising exactly when the wrong-sounding note
 * is heard means notes are being pushed onto the synthetic path (an
 * instrument that failed to resolve, leaving instOrWave in the 0x80..0xBF
 * synthetic range -- cross-check the DROP line's `ins` counter). Staying
 * flat while the artefact sounds rules the synthetic path out entirely and
 * moves the search into the sample decode. */
int PspAudioDebug_SyntheticNoteCount(void) {
    int count = 0;
    int i;

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        const NoteSampleState* st = &gAudioCtx.notes[i].sampleState;

        if (st->bitField0.enabled && st->bitField1.isSyntheticWave) {
            count++;
        }
    }
    return count;
}

void PspAudioDebug_PlayerInfo(int playerIdx, int* enabled, int* seqId, int* state, int* fadeVolumeX1000) {
    SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[playerIdx];

    *enabled = seqPlayer->enabled;
    *seqId = seqPlayer->seqId;
    *state = seqPlayer->state;
    *fadeVolumeX1000 = (int)(seqPlayer->fadeVolume * 1000.0f);
}

extern u8 D_80133418;

/* Diagnoses the func_800FAD34()/D_80133418 gate that skips Audio_Update's
 * entire body (including AudioThread_ScheduleProcessCmds, which is what
 * hands SEQCMD_* commands like Audio_PlayFanfare's to the audio thread at
 * all) while a heap reset is thought to be in progress -- see general.c's
 * Audio_Update and sequence.c's func_800FAD34. */
void PspAudioDebug_ResetGateInfo(int* d80133418, int* resetStatus, int* specId) {
    *d80133418 = D_80133418;
    *resetStatus = gAudioCtx.resetStatus;
    *specId = gAudioCtx.specId;
}

/* Real bug, not a port artifact: sequence.c's func_800FAD34 gates the whole
 * body of Audio_Update (general.c) -- including Audio_ProcessSeqCmds and
 * AudioThread_ScheduleProcessCmds, i.e. every future SEQCMD -- behind
 * D_80133418 clearing back to 0, which only happens when func_800E5EDC()
 * drains a completion message from gAudioCtx.audioResetQueueP whose payload
 * specId matches the CURRENT gAudioCtx.specId (thread.c). That queue
 * (audio.h's audioResetMsgBuf) has exactly ONE slot and every send uses
 * OS_MESG_NOBLOCK (thread.c ~L115) -- if two SEQCMD_RESET_AUDIO_HEAP
 * requests (z_scene.c's per-room sound-settings command is one real source)
 * land close enough together, the first reset's completion message can sit
 * unread while a second reset finishes and its own NOBLOCK send onto the
 * still-full queue is silently dropped. The message that DOES survive can
 * then carry a specId that no longer matches gAudioCtx.specId by the time
 * func_800E5EDC finally reads it, so it returns -1 forever and D_80133418
 * never clears again -- confirmed live on this port: resetStatus reads 0
 * (the underlying reset genuinely finished) while D_80133418 stays stuck at
 * 1 indefinitely, silencing all audio from that point on.
 *
 * Self-heal rather than patch the real (shared, byte-ported) engine files:
 * resetStatus==0 already proves no reset is actually in flight, which is
 * the only fact D_80133418 exists to confirm, so it's always safe to clear
 * it once resetStatus has read 0 for a few consecutive frames (a short
 * debounce, since resetStatus legitimately passes through 0 for one frame
 * between case-1 finishing and a next reset's case-5 starting). Also drains
 * the queue defensively in case a stale mismatched message is still sitting
 * in it. */
/* The fanfare-command trace that lived here (PspAudioDebug_FanfareQueueInfo /
 * PspAudioDebug_SeqCmdRaw) is gone with its HUD line. It existed to answer
 * "does a hand-fired test fanfare's SEQCMD ever reach the audio thread",
 * back when that fanfare was the only sound this port could make. Real
 * sequences play now, the hotkey is gone, and the line it fed has been
 * repurposed for the dropped-note diagnosis -- so the accessors, and their
 * externs into general.c's file-static fanfare state, were dead weight
 * reaching across a module boundary for nothing. */

static u32 sHealCount = 0;
static u32 sResetStartCount = 0;

void PspAudioDebug_HealResetGate(void) {
    static int sZeroStreak = 0;
    static int sPrevResetStatus = 0;

    /* Rising edge into 5 (AudioHeap_ResetStep's "just started" state) --
     * counts how many distinct heap-reset cycles have actually run, to tell
     * "one reset happened, something else is stuck after it" apart from
     * "resets keep re-firing continuously and never let Audio_Update's body
     * run for more than a frame or two at a time". */
    if (gAudioCtx.resetStatus == 5 && sPrevResetStatus != 5) {
        sResetStartCount++;
    }
    sPrevResetStatus = gAudioCtx.resetStatus;

    if (gAudioCtx.resetStatus != 0) {
        sZeroStreak = 0;
        return;
    }

    if (D_80133418 == 0) {
        sZeroStreak = 0;
        return;
    }

    sZeroStreak++;
    if (sZeroStreak >= 3) {
        OSMesg msg;

        while (osRecvMesg(gAudioCtx.audioResetQueueP, &msg, OS_MESG_NOBLOCK) != -1) {
        }
        D_80133418 = 0;
        sZeroStreak = 0;
        sHealCount++;

        /* Real (func_800FAD34) side effects of a successful gate clear --
         * NOT just clearing D_80133418. Missing these was a second, separate
         * bug from the gate itself: even with the gate open, every
         * SequencePlayer stayed muted forever (AUDIOCMD_OP_GLOBAL_UNMUTE,
         * thread.c, is what flips SequencePlayer.muted back to false), so
         * PspAudio_Output kept running (calls/n advancing) but every mixed
         * sample was forced to 0 -- confirmed live: AUD peak stayed 0 while
         * calls climbed steadily, and FANFARE's own SEQCMD never drained
         * (gSeqCmdWritePos/ReadPos stayed 1 apart) because
         * AudioThread_ScheduleProcessCmds is only invoked from this same
         * path, not from the heal's plain D_80133418 write. */
        AUDIOCMD_SEQPLAYER_SET_IO(SEQ_PLAYER_SFX, 0, gSfxChannelLayout);
        func_800F7170();
    }
}

void PspAudioDebug_HealStats(unsigned int* healCount, unsigned int* resetStartCount) {
    *healCount = sHealCount;
    *resetStartCount = sResetStartCount;
}

/* The load path, which is upstream of everything the lines above show: no
 * matter how healthy the gate, the queue and the output backend look, a
 * SequencePlayer with no sequence data and a font whose instruments all
 * relocated to NULL produces exactly the same "calls climbing, peak 0"
 * picture as a muting bug.
 *
 * permEntries is gAudioCtx.permanentPool.numEntries: AudioHeap_AllocPermanent
 * bumps it once per permanently-cached sequence/soundfont, so the expected
 * steady state for this bring-up's scope is 3 (Sequence_0 + Soundfont_0 +
 * Soundfont_1). 0 means AudioLoad_SyncLoad was never reached at all -- look
 * upstream at the command path, not at the tables.
 *
 * seqStatus0/fontStatus0 are LOAD_STATUS_* for Sequence_0/Soundfont_0 (5 =
 * PERMANENTLY_LOADED is what cachePolicy 0 produces on success, 0 =
 * NOT_LOADED means the load never completed).
 *
 * seqDataByte is the first byte of the loaded sequence 0 -- the single most
 * direct "did real bytes actually arrive" test there is, because the failure
 * mode this port hit twice reads as a successful load of all zeroes. */
void PspAudioDebug_LoadInfo(int* permEntries, int* seqStatus0, int* fontStatus0, int* seqDataByte) {
    SequencePlayer* sfxPlayer = &gAudioCtx.seqPlayers[SEQ_PLAYER_SFX];

    *permEntries = gAudioCtx.permanentPool.numEntries;
    *seqStatus0 = gAudioCtx.seqLoadStatus[0];
    *fontStatus0 = gAudioCtx.fontLoadStatus[0];
    *seqDataByte = (sfxPlayer->seqData != NULL) ? sfxPlayer->seqData[0] : -1;
}

/* ---------------------------------------------------------------------- */
/* Why notes go missing.
 *
 * The engine already keeps its own account of every note it refused to
 * start: gAudioCtx.audioErrorFlags, written at six places in playback.c
 * (Audio_GetInstrumentInner / Audio_GetDrum / Audio_GetSoundEffect). On N64
 * the debug menu reads it; this port never looked at it, so every dropped
 * note was silent in both senses.
 *
 * It is a single u32 that each new error overwrites, so it has to be drained
 * faster than errors are produced. Polling from PspAudio_Output (once per
 * output block) undercounts when several errors land in one block -- that is
 * accepted deliberately: the question here is WHICH failures happen at all
 * and roughly how often, not an exact tally. Draining it is safe because
 * nothing else in this port reads the field.
 *
 * Note allocation failure is the one cause of a missing note the engine does
 * NOT record this way -- Audio_AllocNote just returns NULL and the layer
 * quietly gives up -- so playback.c bumps gPspAudioNoteAllocFails at that
 * one spot. A climbing count there means voice starvation (too few notes for
 * what the sequence asks for, or notes not being freed), which is a
 * completely different fix from a missing instrument. */
u32 gPspAudioNoteAllocFails = 0;

static u32 sErrInstrument;  /* instrument missing or out of range */
static u32 sErrDrumSfx;     /* drum / sound effect missing or out of range */
static u32 sErrFontLoad;    /* font not finished loading when a note wanted it */
static u32 sErrOther;
static u32 sErrTotal;
static u32 sErrLast;

void PspAudioDebug_PollErrorFlags(void) {
    u32 code = gAudioCtx.audioErrorFlags;

    if (code == 0) {
        return;
    }
    gAudioCtx.audioErrorFlags = 0;
    sErrLast = code;
    sErrTotal++;

    /* The high byte is the class; the low half is (fontId << 8) | id. */
    switch (code >> 24) {
        case 0x01: /* Audio_GetInstrumentInner: instrument pointer is NULL */
        case 0x03: /* Audio_GetInstrumentInner: instId >= numInstruments */
            sErrInstrument++;
            break;
        case 0x04: /* drum/sfx id out of range */
        case 0x05: /* drum/sfx pointer is NULL */
            sErrDrumSfx++;
            break;
        case 0x10: /* AudioLoad_IsFontLoadComplete said no */
            sErrFontLoad++;
            break;
        default:
            sErrOther++;
            break;
    }
}

void PspAudioDebug_ErrorSummary(u32* total, u32* last, u32* instrument, u32* drumSfx, u32* fontLoad,
                                u32* allocFails) {
    *total = sErrTotal;
    *last = sErrLast;
    *instrument = sErrInstrument;
    *drumSfx = sErrDrumSfx;
    *fontLoad = sErrFontLoad;
    *allocFails = gPspAudioNoteAllocFails;
}

/* Guard-layer bookkeeping, see psp/include/psp_audio_guard.h. Every increment
 * is one pointer that would have been dereferenced outside the user partition
 * -- silent corruption on N64/PPSSPP, loss of power on real hardware. */
u32 gPspAudioBadPtrDrops = 0;
const char* gPspAudioBadPtrLastWhere = "";

void PspAudio_NoteBadPtr(const char* where) {
    gPspAudioBadPtrDrops++;
    gPspAudioBadPtrLastWhere = where;
}

/* See the probe in Audio_SetSfxProperties (src/audio/game/general.c): the
 * distance and volume the engine last computed for a BANK_PLAYER sound. */
f32 gPspSfxProbeDist;
f32 gPspSfxProbeVol;
f32 gPspSfxProbeEntryVol;
u32 gPspSfxProbeCount;

/* The two sequence players' applied fade volumes, for the HUD's SFX line.
 * They are faded independently, so SFX sitting well below BGM is a
 * whole-player gain problem rather than anything to do with a single sound. */
f32 PspAudioDebug_SfxPlayerVolume(void) {
    return gAudioCtx.seqPlayers[SEQ_PLAYER_SFX].appliedFadeVolume;
}

f32 PspAudioDebug_BgmPlayerVolume(void) {
    return gAudioCtx.seqPlayers[SEQ_PLAYER_BGM_MAIN].appliedFadeVolume;
}

/* Written by the probe in Audio_InitSampleState: the velocity and resulting
 * target volume of the last note belonging to the SFX sequence player. */
f32 gPspNoteProbeVel;
s32 gPspNoteProbeTargetVol;
u32 gPspNoteProbeCount;

/* Written by the envelope probe in Audio_ProcessNotes. */
f32 gPspAdsrProbeCur;
f32 gPspAdsrProbeTarget;
s32 gPspAdsrProbeD0;
s32 gPspAdsrProbeA0;
u32 gPspAdsrProbeCount;

s32 gPspAdsrProbeDelay;
f32 gPspAdsrProbeVel;
s32 gPspAdsrProbeTicks;

s32 gPspAdsrProbeIndex;
s32 gPspAdsrProbeDN;
s32 gPspAdsrProbeAN;
