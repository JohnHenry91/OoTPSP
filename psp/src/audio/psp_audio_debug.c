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
#include "sequence.h"

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
extern u8 sFanfareStartTimer;
extern u16 sFanfareSeqId;
extern u8 gSeqCmdWritePos;
extern u8 gSeqCmdReadPos;

void PspAudioDebug_FanfareQueueInfo(int* fanfareTimer, int* fanfareSeqId, int* seqCmdPending) {
    *fanfareTimer = sFanfareStartTimer;
    *fanfareSeqId = sFanfareSeqId;
    *seqCmdPending = (u8)(gSeqCmdWritePos - gSeqCmdReadPos);
}

/* Raw absolute positions rather than the diff -- two readings a few seconds
 * apart tell "write advances, read frozen" (Audio_ProcessSeqCmds truly never
 * runs) apart from "both advance together, always 1 apart" (a real drain
 * loop with an off-by-one, a different bug) apart from "both frozen" (
 * nothing is even queuing anything new, e.g. this reading predates the
 * fanfare firing). */
void PspAudioDebug_SeqCmdRaw(int* writePos, int* readPos) {
    *writePos = gSeqCmdWritePos;
    *readPos = gSeqCmdReadPos;
}

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
    }
}

void PspAudioDebug_HealStats(unsigned int* healCount, unsigned int* resetStartCount) {
    *healCount = sHealCount;
    *resetStartCount = sResetStartCount;
}
