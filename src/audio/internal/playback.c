/**
 * original filename: channel.c
 */
#include "ultra64.h"
#include "audio.h"
#if TARGET_PSP
#include "psp_audio_guard.h"

/* A soundfont index is only usable if it can index a table that is itself in
 * native RAM. Ported from reference/oot-psp-z2442 (OotPspAudio_IsSafeFontId);
 * 0x30 is one past the highest font id the game ever asks for. */
static s32 PspAudio_IsSafeFontId(s32 fontId) {
    return (fontId >= 0) && (fontId < 0x30) && PspAudio_IsAlignedNativePtr(gAudioCtx.soundFontList);
}
#endif

/**
 * original name: Nas_smzSetParam
 */
void Audio_InitSampleState(Note* note, NoteSampleState* sampleState, NoteSampleStateAttributes* attrs) {
    f32 volLeft;
    f32 volRight;
    s32 halfPanIndex;
    u64 pad;
    u8 strongLeft;
    u8 strongRight;
    f32 vel;
    u8 pan;
    u8 reverbVol;
    StereoData stereoData;
    s32 stereoHeadsetEffects = note->playbackState.stereoHeadsetEffects;

    vel = attrs->velocity;
    pan = attrs->pan;
    reverbVol = attrs->reverbVol;
    stereoData = attrs->stereo.s;

    sampleState->bitField0 = note->sampleState.bitField0;
    sampleState->bitField1 = note->sampleState.bitField1;
    sampleState->waveSampleAddr = note->sampleState.waveSampleAddr;
    sampleState->harmonicIndexCurAndPrev = note->sampleState.harmonicIndexCurAndPrev;

    Audio_NoteSetResamplingRate(sampleState, attrs->frequency);

    pan &= 0x7F;

    sampleState->bitField0.stereoStrongRight = false;
    sampleState->bitField0.stereoStrongLeft = false;
    sampleState->bitField0.stereoHeadsetEffects = stereoData.stereoHeadsetEffects;
    sampleState->bitField0.usesHeadsetPanEffects = stereoData.usesHeadsetPanEffects;
    if (stereoHeadsetEffects && (gAudioCtx.soundOutputMode == SOUND_OUTPUT_HEADSET)) {
        halfPanIndex = pan >> 1;
        if (halfPanIndex > 0x3F) {
            halfPanIndex = 0x3F;
        }

        sampleState->haasEffectRightDelaySize = gHaasEffectDelaySizes[halfPanIndex];
        sampleState->haasEffectLeftDelaySize = gHaasEffectDelaySizes[0x3F - halfPanIndex];
        sampleState->bitField1.useHaasEffect = true;

        volLeft = gHeadsetPanVolume[pan];
        volRight = gHeadsetPanVolume[0x7F - pan];
    } else if (stereoHeadsetEffects && (gAudioCtx.soundOutputMode == SOUND_OUTPUT_STEREO)) {
        strongLeft = strongRight = 0;
        sampleState->haasEffectLeftDelaySize = 0;
        sampleState->haasEffectRightDelaySize = 0;
        sampleState->bitField1.useHaasEffect = false;

        volLeft = gStereoPanVolume[pan];
        volRight = gStereoPanVolume[0x7F - pan];
        if (pan < 0x20) {
            strongLeft = 1;
        } else if (pan > 0x60) {
            strongRight = 1;
        }

        sampleState->bitField0.stereoStrongRight = strongRight;
        sampleState->bitField0.stereoStrongLeft = strongLeft;

        switch (stereoData.bit2) {
            case 0:
                break;

            case 1:
                sampleState->bitField0.stereoStrongRight = stereoData.strongRight;
                sampleState->bitField0.stereoStrongLeft = stereoData.strongLeft;
                break;

            case 2:
                sampleState->bitField0.stereoStrongRight = stereoData.strongRight | strongRight;
                sampleState->bitField0.stereoStrongLeft = stereoData.strongLeft | strongLeft;
                break;

            case 3:
                sampleState->bitField0.stereoStrongRight = stereoData.strongRight ^ strongRight;
                sampleState->bitField0.stereoStrongLeft = stereoData.strongLeft ^ strongLeft;
                break;
        }

    } else if (gAudioCtx.soundOutputMode == SOUND_OUTPUT_MONO) {
        sampleState->bitField0.stereoHeadsetEffects = false;
        sampleState->bitField0.usesHeadsetPanEffects = false;
        volLeft = 0.707f; // approx 1/sqrt(2)
        volRight = 0.707f;
    } else {
        sampleState->bitField0.stereoStrongRight = stereoData.strongRight;
        sampleState->bitField0.stereoStrongLeft = stereoData.strongLeft;
        volLeft = gDefaultPanVolume[pan];
        volRight = gDefaultPanVolume[0x7F - pan];
    }

    vel = 0.0f > vel ? 0.0f : vel;
    vel = 1.0f < vel ? 1.0f : vel;

    sampleState->targetVolLeft = (s32)((vel * volLeft) * (0x1000 - 0.001f));
    sampleState->targetVolRight = (s32)((vel * volRight) * (0x1000 - 0.001f));

#if TARGET_PSP
    /* Last unmeasured step in the "Link's sounds are quiet" chain. Everything
     * upstream is now known to be near full (see the SFX HUD line), so if a
     * note still ends up quiet, it happens right here: vel carries the whole
     * channel gain, volLeft/volRight only the pan split. Recorded for notes
     * belonging to the SFX player, whose sounds are the ones in question.
     *
     * Reading it: tgt is targetVolLeft out of 0x1000 (4096). A close,
     * centred, full-volume sound should land in the low thousands. tgt in
     * the hundreds while the HUD's vol/ent are near full means vel arrived
     * small -- the channel gain never reached the note. */
    /* NOT a plain != NULL test. OoT parks a note's layer pointer at
     * NO_LAYER == (SequenceLayer*)-1 (audio.h), which is non-NULL and sails
     * straight through a null check; the chain below then reads 0xFFFFFFFF+0x50
     * and dereferences address 0x0000004F. PPSSPP logs that and carries on,
     * hardware takes an address error and the console dies -- and this probe
     * runs for every SFX note, so it fires constantly. Use the same range
     * guard as the rest of the audio path (psp_audio_guard.h). */
    if (PspAudio_IsAlignedNativePtr(note->playbackState.parentLayer) &&
        PspAudio_IsAlignedNativePtr(note->playbackState.parentLayer->channel) &&
        (note->playbackState.parentLayer->channel->seqPlayer ==
         &gAudioCtx.seqPlayers[SEQ_PLAYER_SFX])) {
        extern f32 gPspNoteProbeVel;
        extern s32 gPspNoteProbeTargetVol;
        extern u32 gPspNoteProbeCount;

        gPspNoteProbeVel = vel;
        gPspNoteProbeTargetVol = sampleState->targetVolLeft;
        gPspNoteProbeCount++;
    }
#endif

    sampleState->gain = attrs->gain;
    sampleState->filter = attrs->filter;
    sampleState->combFilterSize = attrs->combFilterSize;
    sampleState->combFilterGain = attrs->combFilterGain;
    sampleState->reverbVol = reverbVol;
}

/**
 * original name: Nas_smzSetPitch
 */
void Audio_NoteSetResamplingRate(NoteSampleState* sampleState, f32 resamplingRateInput) {
    f32 resamplingRate = 0.0f;

    if (resamplingRateInput < 2.0f) {
        sampleState->bitField1.hasTwoParts = false;
        resamplingRate = CLAMP_MAX(resamplingRateInput, 1.99998f);

    } else {
        sampleState->bitField1.hasTwoParts = true;
        if (resamplingRateInput > 3.99996f) {
            resamplingRate = 1.99998f;
        } else {
            resamplingRate = resamplingRateInput * 0.5f;
        }
    }
    sampleState->resamplingRateFixedPoint = (s32)(resamplingRate * 32768.0f);
}

/**
 * original name: Nas_StartVoice
 */
void Audio_NoteInit(Note* note) {
    if (note->playbackState.parentLayer->adsr.decayIndex == 0) {
        Audio_AdsrInit(&note->playbackState.adsr, note->playbackState.parentLayer->channel->adsr.envelope,
                       &note->playbackState.adsrVolScaleUnused);
    } else {
        Audio_AdsrInit(&note->playbackState.adsr, note->playbackState.parentLayer->adsr.envelope,
                       &note->playbackState.adsrVolScaleUnused);
    }

    note->playbackState.unk_04 = 0;
    note->playbackState.adsr.action.s.state = ADSR_STATE_INITIAL;
    note->sampleState = gDefaultNoteSampleState;
}

/**
 * original name: Nas_StopVoice
 */
void Audio_NoteDisable(Note* note) {
    if (note->sampleState.bitField0.needsInit == true) {
        note->sampleState.bitField0.needsInit = false;
    }
    note->playbackState.priority = 0;
    note->sampleState.bitField0.enabled = false;
    note->playbackState.unk_04 = 0;
    note->sampleState.bitField0.finished = false;
    note->playbackState.parentLayer = NO_LAYER;
    note->playbackState.prevParentLayer = NO_LAYER;
    note->playbackState.adsr.action.s.state = ADSR_STATE_DISABLED;
    note->playbackState.adsr.current = 0;
}

/**
 * original name: Nas_UpdateChannel
 */
void Audio_ProcessNotes(void) {
    s32 pad[2];
    NoteAttributes* attrs;
    NoteSampleState* sampleState2;
    NoteSampleState* sampleState;
    Note* note;
    NotePlaybackState* playbackState;
    NoteSampleStateAttributes sampleStateAttrs;
    u8 bookOffset;
    f32 scale;
    s32 i;

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        note = &gAudioCtx.notes[i];
        sampleState2 = &gAudioCtx.sampleStates[gAudioCtx.sampleStateOffset + i];
        playbackState = &note->playbackState;
        if (playbackState->parentLayer != NO_LAYER) {
            // The literal here is K0BASE in all but name: on N64 every real
            // SequenceLayer* is a KSEG0 address, so this only ever fires on a
            // corrupt pointer. Spelled as the shared constant so it keeps that
            // meaning on a target whose RAM sits below 0x80000000 -- see
            // AUDIO_RELOCATED_ADDRESS_START in audio.h. No behaviour change on
            // N64: no pointer is ever between 0x7FFFFFFF and 0x80000000.
#if TARGET_PSP
            /* A lower bound alone is not enough here: freed layers routinely
             * leave a high-but-wild value behind, which passes the N64 test
             * and then faults outside the user partition. Check the whole
             * range and the alignment, as reference/oot-psp-z2442 does. */
            if (!PspAudio_IsAlignedNativePtr(playbackState->parentLayer)) {
                PspAudio_NoteBadPtr("process-notes-layer");
                continue;
            }
#else
            if ((u32)playbackState->parentLayer < AUDIO_RELOCATED_ADDRESS_START) {
                continue;
            }
#endif

            if (note != playbackState->parentLayer->note && playbackState->unk_04 == 0) {
                playbackState->adsr.action.s.release = true;
                playbackState->adsr.fadeOutVel = gAudioCtx.audioBufferParameters.ticksPerUpdateInv;
                playbackState->priority = 1;
                playbackState->unk_04 = 2;
                goto out;
            } else if (!playbackState->parentLayer->enabled && playbackState->unk_04 == 0 &&
                       playbackState->priority >= 1) {
                // do nothing
            } else if (playbackState->parentLayer->channel->seqPlayer == NULL) {
                AudioSeq_SequenceChannelDisable(playbackState->parentLayer->channel);
                playbackState->priority = 1;
                playbackState->unk_04 = 1;
                continue;
            } else if (playbackState->parentLayer->channel->seqPlayer->muted &&
                       (playbackState->parentLayer->channel->muteBehavior & MUTE_BEHAVIOR_STOP_NOTES)) {
                // do nothing
            } else {
                goto out;
            }

            Audio_SeqLayerNoteRelease(playbackState->parentLayer);
            Audio_AudioListRemove(&note->listItem);
            Audio_AudioListPushFront(&note->listItem.pool->decaying, &note->listItem);
            playbackState->priority = 1;
            playbackState->unk_04 = 2;
        } else if (playbackState->unk_04 == 0 && playbackState->priority >= 1) {
            continue;
        }

    out:
        if (playbackState->priority != 0) {
            if (1) {}
            sampleState = &note->sampleState;
            if (playbackState->unk_04 >= 1 || sampleState->bitField0.finished) {
                if (playbackState->adsr.action.s.state == ADSR_STATE_DISABLED || sampleState->bitField0.finished) {
                    if (playbackState->wantedParentLayer != NO_LAYER) {
                        Audio_NoteDisable(note);
                        if (playbackState->wantedParentLayer->channel != NULL) {
                            Audio_NoteInitForLayer(note, playbackState->wantedParentLayer);
                            Audio_NoteVibratoInit(note);
                            Audio_NotePortamentoInit(note);
                            Audio_AudioListRemove(&note->listItem);
                            AudioSeq_AudioListPushBack(&note->listItem.pool->active, &note->listItem);
                            playbackState->wantedParentLayer = NO_LAYER;
                            // don't skip
                        } else {
                            Audio_NoteDisable(note);
                            Audio_AudioListRemove(&note->listItem);
                            AudioSeq_AudioListPushBack(&note->listItem.pool->disabled, &note->listItem);
                            playbackState->wantedParentLayer = NO_LAYER;
                            goto skip;
                        }
                    } else {
                        if (playbackState->parentLayer != NO_LAYER) {
                            playbackState->parentLayer->bit1 = true;
                        }
                        Audio_NoteDisable(note);
                        Audio_AudioListRemove(&note->listItem);
                        AudioSeq_AudioListPushBack(&note->listItem.pool->disabled, &note->listItem);
                        continue;
                    }
                }
            } else if (playbackState->adsr.action.s.state == ADSR_STATE_DISABLED) {
                if (playbackState->parentLayer != NO_LAYER) {
                    playbackState->parentLayer->bit1 = true;
                }
                Audio_NoteDisable(note);
                Audio_AudioListRemove(&note->listItem);
                AudioSeq_AudioListPushBack(&note->listItem.pool->disabled, &note->listItem);
                continue;
            }

            scale = Audio_AdsrUpdate(&playbackState->adsr);
#if TARGET_PSP
            /* The envelope is the last unexplained factor: the SFX side is
             * measured at full volume (VOICE vol/ent 1000) while notes arrive
             * at a few percent, and this scale is the only multiplier in
             * between.
             *
             * cur/tgt are the envelope's live and target amplitude x1000.
             * d0/a0 are the RAW first envelope point straight out of the
             * soundfont. That pair decides the byte-order question in one
             * look: a real OoT attack point is a small positive delay with a
             * target up to 32767, so d0=1 a0=32767 means the data is being
             * read correctly, while d0=256 a0=-129 is exactly that same point
             * with its bytes swapped -- and would explain a near-silent note
             * while every other volume upstream reads full. */
            if (scale < 0.2f) {
                extern f32 gPspAdsrProbeCur;
                extern f32 gPspAdsrProbeTarget;
                extern s32 gPspAdsrProbeD0;
                extern s32 gPspAdsrProbeA0;
                extern u32 gPspAdsrProbeCount;

                {
                    extern s32 gPspAdsrProbeDelay;
                    extern f32 gPspAdsrProbeVel;
                    extern s32 gPspAdsrProbeTicks;
                    extern s32 gPspAdsrProbeIndex;
                    extern s32 gPspAdsrProbeDN;
                    extern s32 gPspAdsrProbeAN;

                    /* delay is the point's tick count AFTER scaling, vel the
                     * per-tick step it produces. A correct attack covers most
                     * of the distance to target within a couple of ticks; a
                     * delay inflated by a wrong ticksPerUpdateScaled makes vel
                     * tiny, so a short sound ends while current is still near
                     * zero -- which is what cur 0 against tgt 806 looks like.
                     * ticks is the scale factor itself, so the two can be
                     * told apart: a large delay with a small factor means the
                     * envelope data asked for it. */
                    /* Report the ACTIVE point, not envelope[0]: the long
                     * delay that stalls the attack comes from a later index,
                     * and index 0 was measured short (d0 2). */
                    gPspAdsrProbeIndex = playbackState->adsr.envIndex;
                    if (PspAudio_IsAlignedNativePtr(playbackState->adsr.envelope)) {
                        gPspAdsrProbeDN = playbackState->adsr.envelope[playbackState->adsr.envIndex].delay;
                        gPspAdsrProbeAN = playbackState->adsr.envelope[playbackState->adsr.envIndex].arg;
                    }
                    gPspAdsrProbeDelay = playbackState->adsr.delay;
                    gPspAdsrProbeVel = playbackState->adsr.velocity;
                    gPspAdsrProbeTicks = gAudioCtx.audioBufferParameters.ticksPerUpdateScaled;
                }
                gPspAdsrProbeCur = playbackState->adsr.current;
                gPspAdsrProbeTarget = playbackState->adsr.target;
                if (PspAudio_IsAlignedNativePtr(playbackState->adsr.envelope)) {
                    gPspAdsrProbeD0 = playbackState->adsr.envelope[0].delay;
                    gPspAdsrProbeA0 = playbackState->adsr.envelope[0].arg;
                }
                gPspAdsrProbeCount++;
            }
#endif
            Audio_NoteVibratoUpdate(note);
            attrs = &playbackState->attributes;
            if (playbackState->unk_04 == 1 || playbackState->unk_04 == 2) {
                sampleStateAttrs.frequency = attrs->freqScale;
                sampleStateAttrs.velocity = attrs->velocity;
                sampleStateAttrs.pan = attrs->pan;
                sampleStateAttrs.reverbVol = attrs->reverb;
                sampleStateAttrs.stereo = attrs->stereo;
                sampleStateAttrs.gain = attrs->gain;
                sampleStateAttrs.filter = attrs->filter;
                sampleStateAttrs.combFilterSize = attrs->combFilterSize;
                sampleStateAttrs.combFilterGain = attrs->combFilterGain;
                bookOffset = sampleState->bitField1.bookOffset;
            } else {
                SequenceLayer* layer = playbackState->parentLayer;
                SequenceChannel* channel = layer->channel;

                sampleStateAttrs.frequency = layer->noteFreqScale;
                sampleStateAttrs.velocity = layer->noteVelocity;
                sampleStateAttrs.pan = layer->notePan;
                if (layer->stereo.asByte == 0) {
                    sampleStateAttrs.stereo = channel->stereo;
                } else {
                    sampleStateAttrs.stereo = layer->stereo;
                }
                sampleStateAttrs.reverbVol = channel->targetReverbVol;
                sampleStateAttrs.gain = channel->gain;
                sampleStateAttrs.filter = channel->filter;
                sampleStateAttrs.combFilterSize = channel->combFilterSize;
                sampleStateAttrs.combFilterGain = channel->combFilterGain;
                bookOffset = channel->bookOffset & 0x7;

                if (channel->seqPlayer->muted && (channel->muteBehavior & MUTE_BEHAVIOR_3)) {
                    sampleStateAttrs.frequency = 0.0f;
                    sampleStateAttrs.velocity = 0.0f;
                }
            }

            sampleStateAttrs.frequency *= playbackState->vibratoFreqScale * playbackState->portamentoFreqScale;
            sampleStateAttrs.frequency *= gAudioCtx.audioBufferParameters.resampleRate;
            sampleStateAttrs.velocity *= scale;
            Audio_InitSampleState(note, sampleState2, &sampleStateAttrs);
            sampleState->bitField1.bookOffset = bookOffset;
        skip:;
        }
    }
}

/**
 * original name: NoteToVoice
 */
TunedSample* Audio_GetInstrumentTunedSample(Instrument* instrument, s32 semitone) {
    TunedSample* tunedSample;

    if (semitone < instrument->normalRangeLo) {
        tunedSample = &instrument->lowPitchTunedSample;
    } else if (semitone <= instrument->normalRangeHi) {
        tunedSample = &instrument->normalPitchTunedSample;
    } else {
        tunedSample = &instrument->highPitchTunedSample;
    }
    return tunedSample;
}

/**
 * original name: ProgToVp
 */
Instrument* Audio_GetInstrumentInner(s32 fontId, s32 instId) {
    Instrument* inst;

    if (fontId == 0xFF) {
        return NULL;
    }

#if TARGET_PSP
    if (!PspAudio_IsSafeFontId(fontId) || (instId < 0)) {
        PspAudio_NoteBadPtr("inst-range");
        return NULL;
    }
#endif

    if (!AudioLoad_IsFontLoadComplete(fontId)) {
        gAudioCtx.audioErrorFlags = fontId + 0x10000000;
        return NULL;
    }

    if (instId >= gAudioCtx.soundFontList[fontId].numInstruments) {
        gAudioCtx.audioErrorFlags = ((fontId << 8) + instId) + 0x3000000;
        return NULL;
    }

#if TARGET_PSP
    if (!PspAudio_IsAlignedNativePtr(gAudioCtx.soundFontList[fontId].instruments)) {
        PspAudio_NoteBadPtr("inst-table");
        return NULL;
    }
#endif

    inst = gAudioCtx.soundFontList[fontId].instruments[instId];
#if TARGET_PSP
    if ((inst != NULL) && !PspAudio_IsAlignedNativePtr(inst)) {
        PspAudio_NoteBadPtr("inst");
        return NULL;
    }
#endif
    if (inst == NULL) {
        gAudioCtx.audioErrorFlags = ((fontId << 8) + instId) + 0x1000000;
        return inst;
    }

    return inst;
}

/**
 * original name: PercToPp
 */
Drum* Audio_GetDrum(s32 fontId, s32 drumId) {
    Drum* drum;

    if (fontId == 0xFF) {
        return NULL;
    }

#if TARGET_PSP
    if (!PspAudio_IsSafeFontId(fontId)) {
        PspAudio_NoteBadPtr("drum-range");
        return NULL;
    }
#endif

    if (!AudioLoad_IsFontLoadComplete(fontId)) {
        gAudioCtx.audioErrorFlags = fontId + 0x10000000;
        return NULL;
    }

    if (drumId >= gAudioCtx.soundFontList[fontId].numDrums) {
        gAudioCtx.audioErrorFlags = ((fontId << 8) + drumId) + 0x4000000;
        return NULL;
    }
#if TARGET_PSP
    if (!PspAudio_IsAlignedNativePtr(gAudioCtx.soundFontList[fontId].drums)) {
        PspAudio_NoteBadPtr("drum-table");
        return NULL;
    }
#else
    if ((u32)gAudioCtx.soundFontList[fontId].drums < AUDIO_RELOCATED_ADDRESS_START) {
        return NULL;
    }
#endif
    drum = gAudioCtx.soundFontList[fontId].drums[drumId];
#if TARGET_PSP
    if ((drum != NULL) && !PspAudio_IsAlignedNativePtr(drum)) {
        PspAudio_NoteBadPtr("drum");
        return NULL;
    }
#endif

    if (drum == NULL) {
        gAudioCtx.audioErrorFlags = ((fontId << 8) + drumId) + 0x5000000;
    }

    return drum;
}

/**
 * original name: VpercToVep
 */
SoundEffect* Audio_GetSoundEffect(s32 fontId, s32 sfxId) {
    SoundEffect* soundEffect;

    if (fontId == 0xFF) {
        return NULL;
    }

#if TARGET_PSP
    if (!PspAudio_IsSafeFontId(fontId)) {
        PspAudio_NoteBadPtr("sfx-range");
        return NULL;
    }
#endif

    if (!AudioLoad_IsFontLoadComplete(fontId)) {
        gAudioCtx.audioErrorFlags = fontId + 0x10000000;
        return NULL;
    }

    if (sfxId >= gAudioCtx.soundFontList[fontId].numSfx) {
        gAudioCtx.audioErrorFlags = ((fontId << 8) + sfxId) + 0x4000000;
        return NULL;
    }

#if TARGET_PSP
    if (!PspAudio_IsAlignedNativePtr(gAudioCtx.soundFontList[fontId].soundEffects)) {
        PspAudio_NoteBadPtr("sfx-table");
        return NULL;
    }
#else
    if ((u32)gAudioCtx.soundFontList[fontId].soundEffects < AUDIO_RELOCATED_ADDRESS_START) {
        return NULL;
    }
#endif

    soundEffect = &gAudioCtx.soundFontList[fontId].soundEffects[sfxId];

    if (soundEffect == NULL) {
        gAudioCtx.audioErrorFlags = ((fontId << 8) + sfxId) + 0x5000000;
    }

    if (soundEffect->tunedSample.sample == NULL) {
        return NULL;
    }

    return soundEffect;
}

/**
 * original name: OverwriteBank
 */
s32 Audio_SetFontInstrument(s32 instrumentType, s32 fontId, s32 index, void* value) {
    if (fontId == 0xFF) {
        return -1;
    }

    if (!AudioLoad_IsFontLoadComplete(fontId)) {
        return -2;
    }

    switch (instrumentType) {
        case 0:
            if (index >= gAudioCtx.soundFontList[fontId].numDrums) {
                return -3;
            }
            gAudioCtx.soundFontList[fontId].drums[index] = value;
            break;

        case 1:
            if (index >= gAudioCtx.soundFontList[fontId].numSfx) {
                return -3;
            }
            gAudioCtx.soundFontList[fontId].soundEffects[index] = *(SoundEffect*)value;
            break;

        default:
            if (index >= gAudioCtx.soundFontList[fontId].numInstruments) {
                return -3;
            }
            gAudioCtx.soundFontList[fontId].instruments[index] = value;
            break;
    }

    return 0;
}

/**
 * original name: __Nas_Release_Channel_Main
 */
void Audio_SeqLayerDecayRelease(SequenceLayer* layer, s32 target) {
    Note* note;
    NoteAttributes* attrs;
    SequenceChannel* channel;
    s32 i;

    if (layer == NO_LAYER) {
        return;
    }

    layer->bit3 = false;

    if (layer->note == NULL) {
        return;
    }

    note = layer->note;
    attrs = &note->playbackState.attributes;

    if (note->playbackState.wantedParentLayer == layer) {
        note->playbackState.wantedParentLayer = NO_LAYER;
    }

    if (note->playbackState.parentLayer != layer) {
        if (note->playbackState.parentLayer == NO_LAYER && note->playbackState.wantedParentLayer == NO_LAYER &&
            note->playbackState.prevParentLayer == layer && target != ADSR_STATE_DECAY) {
            note->playbackState.adsr.fadeOutVel = gAudioCtx.audioBufferParameters.ticksPerUpdateInv;
            note->playbackState.adsr.action.s.release = true;
        }
        return;
    }

    if (note->playbackState.adsr.action.s.state != ADSR_STATE_DECAY) {
        attrs->freqScale = layer->noteFreqScale;
        attrs->velocity = layer->noteVelocity;
        attrs->pan = layer->notePan;

        if (layer->channel != NULL) {
            channel = layer->channel;
            attrs->reverb = channel->targetReverbVol;
            attrs->gain = channel->gain;
            attrs->filter = channel->filter;

            if (attrs->filter != NULL) {
                for (i = 0; i < 8; i++) {
                    attrs->filterBuf[i] = attrs->filter[i];
                }
                attrs->filter = attrs->filterBuf;
            }

            attrs->combFilterGain = channel->combFilterGain;
            attrs->combFilterSize = channel->combFilterSize;
            if (channel->seqPlayer->muted && (channel->muteBehavior & MUTE_BEHAVIOR_3)) {
                note->sampleState.bitField0.finished = true;
            }

            if (layer->stereo.asByte == 0) {
                attrs->stereo = channel->stereo;
            } else {
                attrs->stereo = layer->stereo;
            }
            note->playbackState.priority = channel->someOtherPriority;
        } else {
            attrs->stereo = layer->stereo;
            note->playbackState.priority = 1;
        }

        note->playbackState.prevParentLayer = note->playbackState.parentLayer;
        note->playbackState.parentLayer = NO_LAYER;
        if (target == ADSR_STATE_RELEASE) {
            note->playbackState.adsr.fadeOutVel = gAudioCtx.audioBufferParameters.ticksPerUpdateInv;
            note->playbackState.adsr.action.s.release = true;
            note->playbackState.unk_04 = 2;
        } else {
            note->playbackState.unk_04 = 1;
            note->playbackState.adsr.action.s.decay = true;
            if (layer->adsr.decayIndex == 0) {
                note->playbackState.adsr.fadeOutVel = gAudioCtx.adsrDecayTable[layer->channel->adsr.decayIndex];
            } else {
                note->playbackState.adsr.fadeOutVel = gAudioCtx.adsrDecayTable[layer->adsr.decayIndex];
            }
            note->playbackState.adsr.sustain =
                ((f32)(s32)(layer->channel->adsr.sustain) * note->playbackState.adsr.current) / 256.0f;
        }
    }

    if (target == ADSR_STATE_DECAY) {
        Audio_AudioListRemove(&note->listItem);
        Audio_AudioListPushFront(&note->listItem.pool->decaying, &note->listItem);
    }
}

/**
 * original name: Nas_Release_Channel
 */
void Audio_SeqLayerNoteDecay(SequenceLayer* layer) {
    Audio_SeqLayerDecayRelease(layer, ADSR_STATE_DECAY);
}

/**
 * original name: Nas_Release_Channel_Force
 */
void Audio_SeqLayerNoteRelease(SequenceLayer* layer) {
    Audio_SeqLayerDecayRelease(layer, ADSR_STATE_RELEASE);
}

/**
 * Extract the synthetic wave to use from gWaveSamples and update corresponding frequencies
 *
 * @param note
 * @param layer
 * @param waveId the index of the type of synthetic wave to use, offset by 128
 * @return harmonicIndex, the index of the harmonic for the synthetic wave contained in gWaveSamples
 */
s32 Audio_BuildSyntheticWave(Note* note, SequenceLayer* layer, s32 waveId) {
    f32 freqScale;
    f32 freqRatio;
    u8 harmonicIndex;

    if (waveId < 128) {
        waveId = 128;
    }

    freqScale = layer->freqScale;
    if (layer->portamento.mode != 0 && 0.0f < layer->portamento.extent) {
        freqScale *= (layer->portamento.extent + 1.0f);
    }

    // Map frequency to the harmonic to use from gWaveSamples
    if (freqScale < 0.99999f) {
        harmonicIndex = 0;
        freqRatio = 1.0465f;
    } else if (freqScale < 1.99999f) {
        harmonicIndex = 1;
        freqRatio = 1.0465f / 2;
    } else if (freqScale < 3.99999f) {
        harmonicIndex = 2;
        freqRatio = 1.0465f / 4 + 1.005E-3;
    } else {
        harmonicIndex = 3;
        freqRatio = 1.0465f / 8 - 2.5E-6;
    }

    // Update results
    layer->freqScale *= freqRatio;
    note->playbackState.waveId = waveId;
    note->playbackState.harmonicIndex = harmonicIndex;

    // Save the pointer to the synthethic wave
    // waveId index starts at 128, there are WAVE_SAMPLE_COUNT samples to read from
    note->sampleState.waveSampleAddr = &gWaveSamples[waveId - 128][harmonicIndex * WAVE_SAMPLE_COUNT];

    return harmonicIndex;
}

void Audio_InitSyntheticWave(Note* note, SequenceLayer* layer) {
    s32 prevHarmonicIndex;
    s32 curHarmonicIndex;
    s32 waveId = layer->instOrWave;

    if (waveId == 0xFF) {
        waveId = layer->channel->instOrWave;
    }

    prevHarmonicIndex = note->playbackState.harmonicIndex;
    curHarmonicIndex = Audio_BuildSyntheticWave(note, layer, waveId);

    if (curHarmonicIndex != prevHarmonicIndex) {
        note->sampleState.harmonicIndexCurAndPrev = (curHarmonicIndex << 2) + prevHarmonicIndex;
    }
}

/**
 * original name: __Nas_InitList
 */
void Audio_InitNoteList(AudioListItem* list) {
    list->prev = list;
    list->next = list;
    list->u.count = 0;
}

/**
 * original name: Nas_InitChNode
 */
void Audio_InitNoteLists(NotePool* pool) {
    Audio_InitNoteList(&pool->disabled);
    Audio_InitNoteList(&pool->decaying);
    Audio_InitNoteList(&pool->releasing);
    Audio_InitNoteList(&pool->active);
    pool->disabled.pool = pool;
    pool->decaying.pool = pool;
    pool->releasing.pool = pool;
    pool->active.pool = pool;
}

/**
 * original name: Nas_InitChannelList
 */
void Audio_InitNoteFreeList(void) {
    s32 i;

    Audio_InitNoteLists(&gAudioCtx.noteFreeLists);
    for (i = 0; i < gAudioCtx.numNotes; i++) {
        gAudioCtx.notes[i].listItem.u.value = &gAudioCtx.notes[i];
        gAudioCtx.notes[i].listItem.prev = NULL;
        AudioSeq_AudioListPushBack(&gAudioCtx.noteFreeLists.disabled, &gAudioCtx.notes[i].listItem);
    }
}

/**
 * original name: Nas_DeAllocAllVoices
 */
void Audio_NotePoolClear(NotePool* pool) {
    s32 i;
    AudioListItem* source;
    AudioListItem* cur;
    AudioListItem* dest;

    for (i = 0; i < 4; i++) {
        switch (i) {
            case 0:
                source = &pool->disabled;
                dest = &gAudioCtx.noteFreeLists.disabled;
                break;

            case 1:
                source = &pool->decaying;
                dest = &gAudioCtx.noteFreeLists.decaying;
                break;

            case 2:
                source = &pool->releasing;
                dest = &gAudioCtx.noteFreeLists.releasing;
                break;

            case 3:
                source = &pool->active;
                dest = &gAudioCtx.noteFreeLists.active;
                break;
        }

        while (true) {
            cur = source->next;
            if (cur == source || cur == NULL) {
                break;
            }
            Audio_AudioListRemove(cur);
            AudioSeq_AudioListPushBack(dest, cur);
        }
    }
}

/**
 * original name: Nas_AllocVoices
 */
void Audio_NotePoolFill(NotePool* pool, s32 count) {
    s32 i;
    s32 j;
    Note* note;
    AudioListItem* source;
    AudioListItem* dest;

    Audio_NotePoolClear(pool);

    for (i = 0, j = 0; j < count; i++) {
        if (i == 4) {
            return;
        }

        switch (i) {
            case 0:
                source = &gAudioCtx.noteFreeLists.disabled;
                dest = &pool->disabled;
                break;

            case 1:
                source = &gAudioCtx.noteFreeLists.decaying;
                dest = &pool->decaying;
                break;

            case 2:
                source = &gAudioCtx.noteFreeLists.releasing;
                dest = &pool->releasing;
                break;

            case 3:
                source = &gAudioCtx.noteFreeLists.active;
                dest = &pool->active;
                break;
        }

        while (j < count) {
            note = AudioSeq_AudioListPopBack(source);
            if (note == NULL) {
                break;
            }
            AudioSeq_AudioListPushBack(dest, &note->listItem);
            j++;
        }
    }
}

/**
 * original name: Nas_AddListHead
 */
void Audio_AudioListPushFront(AudioListItem* list, AudioListItem* item) {
#if TARGET_PSP
    /* Teardown can hand us a list head or an item that has already been freed;
     * the splice below writes through both. Ported from
     * reference/oot-psp-z2442. A head whose links are wild is re-emptied
     * rather than dropped, so the pool stays usable. */
    if (!PspAudio_IsAlignedNativePtr(list) || !PspAudio_IsAlignedNativePtr(item)) {
        PspAudio_NoteBadPtr("push-front");
        return;
    }

    if (!PspAudio_IsAlignedNativePtr(list->next)) {
        PspAudio_NoteBadPtr("push-front-head");
        list->prev = list;
        list->next = list;
        list->u.count = 0;
    }
#endif

    // add 'item' to the front of the list given by 'list', if it's not in any list
    if (item->prev == NULL) {
        item->prev = list;
        item->next = list->next;
        list->next->prev = item;
        list->next = item;
        list->u.count++;
        item->pool = list->pool;
    }
}

/**
 * original name: Nas_CutList
 */
void Audio_AudioListRemove(AudioListItem* item) {
    // remove 'item' from the list it's in, if any
    if (item->prev != NULL) {
#if TARGET_PSP
        if (!PspAudio_IsAlignedNativePtr(item) || !PspAudio_IsAlignedNativePtr(item->prev) ||
            !PspAudio_IsAlignedNativePtr(item->next)) {
            PspAudio_NoteBadPtr("list-remove");
            if (PspAudio_IsAlignedNativePtr(item)) {
                item->prev = NULL;
            }
            return;
        }
#endif
        item->prev->next = item->next;
        item->next->prev = item->prev;
        item->prev = NULL;
    }
}

/**
 * original name: __Nas_GetLowerPrio
 */
Note* Audio_FindNodeWithPrioLessThan(AudioListItem* list, s32 limit) {
    AudioListItem* cur = list->next;
    AudioListItem* best;

    if (cur == list) {
        return NULL;
    }

    for (best = cur; cur != list; cur = cur->next) {
        if (((Note*)best->u.value)->playbackState.priority >= ((Note*)cur->u.value)->playbackState.priority) {
            best = cur;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    if (limit <= ((Note*)best->u.value)->playbackState.priority) {
        return NULL;
    }

    return best->u.value;
}

/**
 * original name: Nas_EntryTrack
 */
void Audio_NoteInitForLayer(Note* note, SequenceLayer* layer) {
    s32 pad[3];
    s16 instId;
    NotePlaybackState* playbackState = &note->playbackState;
    NoteSampleState* sampleState = &note->sampleState;

    note->playbackState.prevParentLayer = NO_LAYER;
    note->playbackState.parentLayer = layer;
    playbackState->priority = layer->channel->notePriority;
    layer->notePropertiesNeedInit = true;
    layer->bit3 = true;
    layer->note = note;
    layer->channel->noteUnused = note;
    layer->channel->layerUnused = layer;
    layer->noteVelocity = 0.0f;
    Audio_NoteInit(note);
    instId = layer->instOrWave;

    if (instId == 0xFF) {
        instId = layer->channel->instOrWave;
    }
    sampleState->tunedSample = layer->tunedSample;

    if (instId >= 0x80 && instId < 0xC0) {
        sampleState->bitField1.isSyntheticWave = true;
    } else {
        sampleState->bitField1.isSyntheticWave = false;
    }

    if (sampleState->bitField1.isSyntheticWave) {
        Audio_BuildSyntheticWave(note, layer, instId);
    }

    playbackState->fontId = layer->channel->fontId;
    playbackState->stereoHeadsetEffects = layer->channel->stereoHeadsetEffects;
    sampleState->bitField1.reverbIndex = layer->channel->reverbIndex & 3;
}

/**
 * original name: __Nas_InterTrack
 */
void func_800E82C0(Note* note, SequenceLayer* layer) {
    // similar to Audio_NoteReleaseAndTakeOwnership, hard to say what the difference is
    Audio_SeqLayerNoteRelease(note->playbackState.parentLayer);
    note->playbackState.wantedParentLayer = layer;
}

/**
 * original name: __Nas_InterReleaseTrack
 */
void Audio_NoteReleaseAndTakeOwnership(Note* note, SequenceLayer* layer) {
    note->playbackState.wantedParentLayer = layer;
    note->playbackState.priority = layer->channel->notePriority;

    note->playbackState.adsr.fadeOutVel = gAudioCtx.audioBufferParameters.ticksPerUpdateInv;
    note->playbackState.adsr.action.s.release = true;
}

Note* Audio_AllocNoteFromDisabled(NotePool* pool, SequenceLayer* layer) {
    Note* note = AudioSeq_AudioListPopBack(&pool->disabled);
    if (note != NULL) {
        Audio_NoteInitForLayer(note, layer);
        Audio_AudioListPushFront(&pool->active, &note->listItem);
    }
    return note;
}

Note* Audio_AllocNoteFromDecaying(NotePool* pool, SequenceLayer* layer) {
    Note* note = AudioSeq_AudioListPopBack(&pool->decaying);
    if (note != NULL) {
        Audio_NoteReleaseAndTakeOwnership(note, layer);
        AudioSeq_AudioListPushBack(&pool->releasing, &note->listItem);
    }
    return note;
}

/**
 * original name: __Nas_ChLookRelWait
 */
Note* Audio_AllocNoteFromActive(NotePool* pool, SequenceLayer* layer) {
    Note* rNote;
    Note* aNote;
    s32 rPriority;
    s32 aPriority;

    rPriority = aPriority = 0x10;
    rNote = Audio_FindNodeWithPrioLessThan(&pool->releasing, layer->channel->notePriority);

    if (rNote != NULL) {
        rPriority = rNote->playbackState.priority;
    }

    aNote = Audio_FindNodeWithPrioLessThan(&pool->active, layer->channel->notePriority);

    if (aNote != NULL) {
        aPriority = aNote->playbackState.priority;
    }

    if (rNote == NULL && aNote == NULL) {
        return NULL;
    }

    if (aPriority < rPriority) {
        Audio_AudioListRemove(&aNote->listItem);
        func_800E82C0(aNote, layer);
        AudioSeq_AudioListPushBack(&pool->releasing, &aNote->listItem);
        aNote->playbackState.priority = layer->channel->notePriority;
        return aNote;
    }
    rNote->playbackState.wantedParentLayer = layer;
    rNote->playbackState.priority = layer->channel->notePriority;
    return rNote;
}

/**
 * original name: Nas_AllocationOnRequest
 */
Note* Audio_AllocNote(SequenceLayer* layer) {
    Note* note;
    u32 policy = layer->channel->noteAllocPolicy;

    if (policy & 1) {
        note = layer->note;
        if (note != NULL && note->playbackState.prevParentLayer == layer &&
            note->playbackState.wantedParentLayer == NO_LAYER) {
            Audio_NoteReleaseAndTakeOwnership(note, layer);
            Audio_AudioListRemove(&note->listItem);
            AudioSeq_AudioListPushBack(&note->listItem.pool->releasing, &note->listItem);
            return note;
        }
    }

    if (policy & 2) {
        if (!(note = Audio_AllocNoteFromDisabled(&layer->channel->notePool, layer)) &&
            !(note = Audio_AllocNoteFromDecaying(&layer->channel->notePool, layer)) &&
            !(note = Audio_AllocNoteFromActive(&layer->channel->notePool, layer))) {
            goto null_return;
        }
        return note;
    }

    if (policy & 4) {
        if (!(note = Audio_AllocNoteFromDisabled(&layer->channel->notePool, layer)) &&
            !(note = Audio_AllocNoteFromDisabled(&layer->channel->seqPlayer->notePool, layer)) &&
            !(note = Audio_AllocNoteFromDecaying(&layer->channel->notePool, layer)) &&
            !(note = Audio_AllocNoteFromDecaying(&layer->channel->seqPlayer->notePool, layer)) &&
            !(note = Audio_AllocNoteFromActive(&layer->channel->notePool, layer)) &&
            !(note = Audio_AllocNoteFromActive(&layer->channel->seqPlayer->notePool, layer))) {
            goto null_return;
        }
        return note;
    }

    if (policy & 8) {
        if (!(note = Audio_AllocNoteFromDisabled(&gAudioCtx.noteFreeLists, layer)) &&
            !(note = Audio_AllocNoteFromDecaying(&gAudioCtx.noteFreeLists, layer)) &&
            !(note = Audio_AllocNoteFromActive(&gAudioCtx.noteFreeLists, layer))) {
            goto null_return;
        }
        return note;
    }

    if (!(note = Audio_AllocNoteFromDisabled(&layer->channel->notePool, layer)) &&
        !(note = Audio_AllocNoteFromDisabled(&layer->channel->seqPlayer->notePool, layer)) &&
        !(note = Audio_AllocNoteFromDisabled(&gAudioCtx.noteFreeLists, layer)) &&
        !(note = Audio_AllocNoteFromDecaying(&layer->channel->notePool, layer)) &&
        !(note = Audio_AllocNoteFromDecaying(&layer->channel->seqPlayer->notePool, layer)) &&
        !(note = Audio_AllocNoteFromDecaying(&gAudioCtx.noteFreeLists, layer)) &&
        !(note = Audio_AllocNoteFromActive(&layer->channel->notePool, layer)) &&
        !(note = Audio_AllocNoteFromActive(&layer->channel->seqPlayer->notePool, layer)) &&
        !(note = Audio_AllocNoteFromActive(&gAudioCtx.noteFreeLists, layer))) {
        goto null_return;
    }
    return note;

null_return:
#if TARGET_PSP
    /* The one cause of a missing note the engine does not record in
     * gAudioCtx.audioErrorFlags: every voice is busy and none could be
     * stolen, so the layer silently gives up. See psp_audio_debug.c. */
    {
        extern u32 gPspAudioNoteAllocFails;

        gPspAudioNoteAllocFails++;
    }
#endif
    layer->bit3 = true;
    return NULL;
}

/**
 * original name: Nas_ChannelInit
 */
void Audio_NoteInitAll(void) {
    Note* note;
    s32 i;

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        note = &gAudioCtx.notes[i];
        note->sampleState = gZeroNoteSampleState;
        note->playbackState.priority = 0;
        note->playbackState.unk_04 = 0;
        note->playbackState.parentLayer = NO_LAYER;
        note->playbackState.wantedParentLayer = NO_LAYER;
        note->playbackState.prevParentLayer = NO_LAYER;
        note->playbackState.waveId = 0;
        note->playbackState.attributes.velocity = 0.0f;
        note->playbackState.adsrVolScaleUnused = 0;
        note->playbackState.adsr.action.asByte = 0;
        note->playbackState.vibratoState.active = 0;
        note->playbackState.portamento.cur = 0;
        note->playbackState.portamento.speed = 0;
        note->playbackState.stereoHeadsetEffects = false;
        note->startSamplePos = 0;
        note->synthesisState.synthesisBuffers =
            AudioHeap_AllocDmaMemory(&gAudioCtx.miscPool, sizeof(NoteSynthesisBuffers));
    }
}
