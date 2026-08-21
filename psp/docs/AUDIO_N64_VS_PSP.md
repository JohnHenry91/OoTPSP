# N64 vs. PSP audio: how the OoT sound pipeline actually works

Written during Phase 4 audio session 3, after the port had spent two full
sessions chasing "why is there no sound" against a wrong mental model of the
N64 audio pipeline. Read this before touching anything under `src/audio/**`
or `psp/src/audio/**`.

---

## 1. The N64 side: two processors, not one

The single most important fact, and the one this port got wrong for two
sessions:

> **The N64 audio engine that lives in `src/audio/**` does NOT produce audio
> samples. It produces a *command list* for a second processor (the RSP),
> which produces the samples.**

The split is:

| Stage | Runs on | Code | Output |
|---|---|---|---|
| Sequence interpretation (`.seq` bytecode → note on/off, volume, pitch) | CPU | `src/audio/internal/seqplayer.c` | `SequenceChannel`/`SequenceLayer` state |
| Note bookkeeping (voice allocation, ADSR envelopes, portamento, vibrato) | CPU | `src/audio/internal/playback.c`, `effects.c` | `NoteSampleState[]` — "what each of the N voices should sound like this frame" |
| **Synthesis** (ADPCM decode, resample, envelope-mix, reverb, interleave) | **RSP** | `src/audio/internal/synthesis.c` **only builds the command list**; the actual DSP work is the `aspMain` microcode in ROM | interleaved stereo s16 PCM |
| Playback | AI (Audio Interface) DMA + DAC | `osAiSetNextBuffer` | sound |

`AudioSynth_Update()` looks like it synthesizes — it takes `s16* aiStart` and
walks every active note — but every `a*()` call inside it is a **macro from
`include/ultra64/abi.h` that packs two 32-bit words into an `Acmd` struct**.
Nothing is computed. At the end, `AudioThread_UpdateImpl`
(`src/audio/internal/thread.c`) hands `gAudioCtx.abiCmdBufs[index]` to the
RSP as an `OSTask` of type `M_AUDTASK`, waits for the RSP to finish, and only
*then* is `gAudioCtx.aiBuffers[index]` full of real PCM.

### Consequence for any PC/handheld port

A port has no RSP. It must supply a **software implementation of the audio
microcode**. Every serious port does exactly this, and all of them use the
same trick: `#undef` the `a*` ABI macros and redefine them to call C
functions that execute the operation *immediately*, so `AudioSynth_Update`
turns from a list-builder into a real synthesizer with zero changes to the
decomp file itself.

- SM64 PC ports (incl. `reference/sm64-port-psp`): `src/pc/mixer.c` / `mixer.h`
- Ship of Harkinian / libultraship (`reference/shipwright-vita`):
  `libultraship/src/audio/mixer.c` — this one is the OoT-specific variant
  (OoT's microcode has ~10 opcodes SM64's does not)
- This port: `psp/src/audio/psp_audio_mixer.c` / `psp/include/psp_audio_mixer.h`

### The RSP's memory model, which the mixer must emulate

The RSP has 4 KiB of DMEM (data memory), addressed 0x000–0xFFF. The audio
microcode uses a fixed map, and OoT's `synthesis.c` hardcodes it:

```
0x3C0  DMEM_TEMP            scratch / final interleave target
0x3E0  DMEM_WET_TEMP
0x580  DMEM_UNCOMPRESSED_NOTE
0x5C0  DMEM_HAAS_TEMP
0x720  DMEM_WET_SCRATCH
0x760  DMEM_COMB_TEMP
0x940  DMEM_LEFT_CH  == DMEM_COMPRESSED_ADPCM_DATA
0xAE0  DMEM_RIGHT_CH
0xC80  DMEM_WET_LEFT_CH
0xE20  DMEM_WET_RIGHT_CH   (+ DMEM_1CH_SIZE 0x1A0 = 0xFC0, the exact top)
```

So the software mixer needs one flat byte array standing in for DMEM
0x3C0..0xFC0, plus a little register file (`rspa` in the mixer): current
in/out DMEM addresses and byte count (`aSetBuffer`), the ADPCM codebook
(`aLoadADPCM`), the loop predictor state (`aSetLoop`), and the envelope
volumes/ramps (`aEnvSetup1`/`aEnvSetup2`).

`aLoadBuffer`/`aSaveBuffer` are the DMEM↔RDRAM DMA ops: they take a **real
CPU pointer** on one side and a DMEM address on the other. On a port they are
just `memcpy`. This is why sample data, reverb ring buffers, and the AI
buffers can stay ordinary C memory.

### The endian trap that is NOT in the data: `AudioCmd`

`AudioCmd` (`include/audio.h`) aliases a `u32 opArgs` with a
`{u8 op; u8 arg0; u8 arg1; u8 arg2;}` struct, and `AUDIO_MK_CMD` packs the
opcode into **bits 31..24**. On big-endian that byte is offset 0, so `op`
reads it; on little-endian it is offset 3, so `op` reads the low byte —
always 0. `AudioThread_ProcessCmds` then treats every command as
`AUDIOCMD_OP_NOOP`: sequences never start, fonts never load, and the whole
subsystem looks perfectly healthy from the outside (commands queue, the ring
drains, the reset gate opens, the output backend runs) while doing nothing.

The same applies one level down to the data union: `AudioThread_QueueCmdS8`
and `QueueCmdU16` deliberately store their payload in the **high** bits
(`data << 0x18` / `data << 0x10`), so `asSbyte`/`asUbyte`/`asUShort` must
alias the high end of the word — which needs explicit padding on
little-endian. Both halves are fixed under `#if TARGET_PSP`, matching Ship of
Harkinian's port of the same struct.

**Still open, lower stakes:** several small unions alias a bitfield struct
with a `u8` (`Stereo`, `AdsrState.action`, `SequenceChannel.changes`). GCC
allocates bitfields from the MSB on big-endian and the LSB on little-endian,
so wherever a byte comes from *authored sequence data* and is then read
through the bitfields — `Stereo` is the real case, set from `.seq` bytecode —
the fields are reversed here. That affects stereo panning and headset
effects, not whether sound plays, and SoH does not correct it either. Worth
revisiting if panning sounds wrong once audio is otherwise working.

### Endianness notes that matter on PSP (little-endian)

- **ADPCM sample data is decoded byte-wise** (`*in >> 4`, `*in & 0xF`), so
  raw sample-bank bytes are endian-agnostic. Good: `SampleBank_0.bin` is a
  raw `.incbin` blob of big-endian-era data and needs no swapping.
- **The ADPCM codebook and loop predictor state are `s16` arrays** read from
  the *soundfont*. Those are safe here only because this port compiles the
  extracted soundfont `.c` with `psp-gcc` (little-endian) rather than using
  raw ROM bytes — see `psp/src/psp_audio_tables.c`'s header comment.
- `CODEC_S16`/`CODEC_S16_INMEMORY` samples *would* need byte-swapping, since
  those are raw `s16` straight out of the sample bank. OoT barely uses them;
  if a future sound is loud static, suspect this.

---

## 2. The N64 side: how a sound gets requested

Two threads, and a command queue between them. Getting this wrong produces
"everything looks fine but nothing plays."

```
GAME THREAD (Graph_Update → Audio_Update, src/audio/game/general.c)
  Audio_PlaySfxGeneral / Audio_StartSequence / SEQCMD_* macros
    → sSeqCmdBuf ring buffer          (game-side "sequence command" queue)
  Audio_ProcessSeqCmds()               ← gated behind D_80133418, see below
    → AUDIOCMD_* macros → gAudioCtx.threadCmdBuf  (cross-thread queue)
    → AudioThread_ScheduleProcessCmds() posts the read/write positions

AUDIO THREAD (AudioMgr_ThreadEntry, src/code/audio_thread_manager.c)
  blocks on its IrqMgr-registered retrace queue
  AudioMgr_HandleRetrace → AudioThread_Update → AudioThread_UpdateImpl
    → AudioThread_ProcessCmds()        drains threadCmdBuf
    → AudioSynth_Update()              builds Acmd list (or, here, synthesizes)
    → osAiSetNextBuffer()              hands PCM to hardware
```

Three gates on that path have each cost this port a session:

1. **`IrqMgr_HandleRetrace` must actually be called.** The audio thread has
   no other wake-up source. Phase-1 bring-up called `PadMgr_HandleRetrace`
   directly and skipped the IrqMgr fan-out entirely — audio simply never
   ticked. Fixed in `src/code/graph.c`.
2. **`D_80133418` (the reset gate).** While an audio-heap reset is in
   flight, `Audio_ProcessSeqCmds` refuses to run, so every SEQCMD queues up
   and nothing happens. It is cleared by `func_800FAD34` on reset
   completion — and clearing it is not enough on its own: the same function
   also sends `AUDIOCMD_GLOBAL_UNMUTE`, without which every SequencePlayer
   stays muted forever. See `psp/src/audio/psp_audio_debug.c`.
3. **`gAudioCtx.resetStatus`/`resetTimer`.** `AudioThread_UpdateImpl` skips
   command processing entirely while `resetStatus != 0`, and
   `AudioLoad_SyncInitSeqPlayer` returns early while `resetTimer != 0`.

---

## 3. The N64 side: where the data comes from

Three ROM segments, three tables, one indirection each:

| Segment | Table | Contents |
|---|---|---|
| `Audioseq`   | `gSequenceTable`   | `.seq` bytecode, one entry per sequence id |
| `Audiobank`  | `gSoundFontTable`  | soundfonts: instrument/drum/sfx definitions |
| `Audiotable` | `gSampleBankTable` | raw ADPCM sample data |

Plus `gSequenceFontTable`, a `u8` blob: `u16` offset per seqId into a
`[numFonts, fontId...]` tuple region. Unlike the other three it is **compiled
in directly**, not DMA'd.

Non-obvious rules, all of which this port has already tripped over:

- **`AudioTable` is one struct: a header immediately followed by its
  entries.** Every access is `table->entries[i]` off a single `AudioTable*`.
  Splitting header and entries into two globals puts them in `.data` and
  `.bss` — hundreds of KB apart — and every load silently reads garbage.
- **`size == 0` means "pointer entry":** `AudioLoad_GetRealTableIndex`
  redirects through `entries[romAddr]` as another *index*. But
  `AudioLoad_SyncLoad` reads `medium`/`cachePolicy` from the **original** id
  and only `size`/`romAddr` from the redirected one — so filler entries still
  need real `medium`/`cachePolicy`, or you get `MEDIUM_RAM`(0) and a hang.
- **Soundfont data is relocated after DMA, by design.** `Instrument`/`Drum`/
  `SoundEffect` cross-references are stored as plain integers relative to
  wherever the DMA landed, fixed into pointers by `AudioLoad_RelocateFont`.
  You therefore *cannot* just link a compiled soundfont object in and use its
  addresses; it has to be served as a flat blob through a real DMA path.
  This port does that via `osEPiStartDma` → `PspRom_Read` → blob registry.
- **`cachePolicy 4` (`CACHE_LOAD_EITHER_NOSYNC`) sample banks are never
  loaded whole.** `AudioLoad_TrySyncLoadSampleBank` returns the raw ROM
  address and individual samples are DMA'd per note through
  `gAudioCtx.sampleDmaBuffers`. This is why a 4 MB sample bank works at all.
- `PERMANENT_POOL_SIZE` in `src/audio/game/session_init.c` **hardcodes**
  `Sequence_0_SIZE + Soundfont_0_SIZE + Soundfont_1_SIZE` by literal name.
  Sequence 0 is the SFX meta-sequence; it is not optional.

---

## 4. The PSP side

### Hardware model

The PSP has no DSP the game can program. `sceAudio` takes finished
interleaved stereo s16 PCM and plays it. Two APIs matter:

- **Output2 / SRC channel** (`sceAudioSRCChReserve` +
  `sceAudioOutput2OutputBlocking`): one stereo stream, hardware sample-rate
  conversion from any input rate. This is what the port uses, at 32000 Hz —
  the rate essentially every `AudioSpec` in `src/audio/game/session_config.c`
  asks for, and the rate the engine internally resamples to anyway.
- Plain channels (`sceAudioChReserve` + `sceAudioOutputPannedBlocking`):
  8 channels, but fixed 44100 Hz. Not useful here.

`sceAudioOutput2OutputBlocking` **blocks until the previous buffer has been
consumed**. That is the port's entire pacing mechanism: because it is called
from AudioMgr's own real PSP thread (not the render thread), blocking there
is correct and self-regulating. It is also why `osAiGetLength()` can honestly
return 0 — by the time the caller asks, the previous buffer really has
finished.

Sample count: `sceAudioSRCChReserve(samplecount, freq, channels)` fixes a
buffer length; `sceAudioOutput2ChangeLength(n)` retunes it when the engine's
per-frame `aiBufLengths[index]` changes (it varies frame to frame by design —
`AudioThread_UpdateImpl` computes it from the frame's tick budget).

### Alignment / cache

PSP audio DMA reads physical memory. Buffers must be 64-byte aligned and
written back out of the data cache (`sceKernelDcacheWritebackInvalidateRange`)
before being handed to `sceAudio*`. `gAudioCtx.aiBuffers[]` are engine-heap
allocations with no such guarantee, so `PspAudio_Output` copies into its own
aligned static buffer first.

### Cost

Software synthesis is not free. On PSP at 333 MHz the mixer runs ADPCM decode
+ 4-tap resample + envelope mix per active voice per frame. SM64's PSP port
does the same thing and holds 60 fps, but OoT allocates more voices; if audio
starts costing frames, the lever is `AudioSpec.numNotes` in
`src/audio/game/session_config.c`, not the mixer's inner loops.

---

## 4b. Serving the assets: two traps that both read as "loaded silence"

Both of these produce the same picture — the engine reports
`LOAD_STATUS_PERMANENTLY_LOADED`, `permanentPool` fills, nothing errors, and
every byte is zero.

**Relative paths need a cwd, and only the main thread has one.** A PSP thread
created with `sceKernelCreateThread` inherits no current working directory, so
`sceIoOpen("blobs/x.bin")` from AudioMgr's thread returns
`SCE_KERNEL_ERROR_NOCWD` (0x8002032C). Scene/room loads run on the main thread
and are unaffected, which makes this look like an audio-specific bug when it is
really a threading one. `PspBlob_SetBaseDir` (called from `main` with `argv[0]`)
pins every blob path to the game's own directory.

**A soundfont `.o` is a partial link, not a payload.** `ld -r` leaves
relocations unapplied — the instrument/drum/sfx offset table and every
`SampleBank_0_SAMPLE_*_Off` reference read back as zero, and `objcopy -O binary`
writes a correctly-sized file full of holes. `AudioLoad_RelocateFont` then turns
92 instruments into 92 `NULL`s and no note can ever sound. The payload must come
from a real link that resolves the sample-bank symbols:

```
psp-ld -R SampleBank_0.o -T linker_scripts/soundfont.ld Soundfont_0.o -o Soundfont_0.elf
psp-objcopy -O binary -j .rodata Soundfont_0.elf Soundfont_0.bin
```

`objdump -h` on the object is the quick tell: a `RELOC` flag on the section you
are about to `objcopy` means the bytes are not final. The main Makefile's
`AUDIO_BUILD_DEBUG` path does exactly this link and byte-compares the result
against the audiobank ripped from the ROM — a check worth reusing, and one this
port confirmed by hand (our offset words match `oot-pal-1.0.z64` at 0xD8C0
exactly, allowing for the intended endian flip).

## 5. Diagnostic ladder

When there is no sound, walk this in order — each step distinguishes a whole
class of causes, and the counters for most of it already exist
(`psp/include/psp_audio.h`, HUD line 6 in `psp/src/psp_scene_menu.c`).

1. `PspAudio_StatOutputCalls()` increasing?
   - No → the audio thread is not ticking. Look at `IrqMgr_HandleRetrace`,
     `AudioMgr_Init`, the retrace queue.
2. `PspAudio_StatLastPeakSample()` non-zero?
   - No → PCM is being produced but is all silence. **This is the
     synthesis/microcode layer.** Either no note is active, or the mixer is
     missing/broken. (Two sessions were lost here: the mixer did not exist,
     so this was structurally always 0.)
3. Did real bytes arrive? HUD line 7 (`LOAD`) answers this directly:
   `perm` should settle at 3 (Sequence_0 + Soundfont_0 + Soundfont_1),
   `sq`/`fn` at 5 (`LOAD_STATUS_PERMANENTLY_LOADED`), `b0` (first byte of the
   loaded SFX sequence) non-zero, and `blob` misses not climbing.
   - `perm 0` → `AudioLoad_SyncLoad` was never reached: look upstream at the
     command path, not at the tables.
   - `perm 3` but `b0 0` → the load "succeeded" into zeroes. That is the
     address/registry class of bug, section 3 above.
   - `blob` misses climbing while audio plays → a `romAddr` no registry entry
     covers (remember `AudioLoad_InitTable` rewrites every `romAddr` once).
4. `gAudioCtx.seqPlayers[i].enabled` / `.seqId` sane?
   - No → the load path above, then the table entries themselves.
5. `D_80133418` stuck at 1, or `resetStatus` never reaching 0?
   - → the reset gate, section 2 above.
6. Notes allocated (`gAudioCtx.noteSubEuOffset`, `notes[i].playbackState`)
   but silent → envelope/volume: `AUDIOCMD_GLOBAL_UNMUTE`, `gSfxVolume`,
   `seqPlayer->fadeVolume`.

Live reads of any of these are practical with the PPSSPP WebSocket debugger —
see `reference_oot_psp_toolchains` and
`reference_verify_running_ppsspp_build` (always confirm the running PPSSPP
process is newer than the EBOOT you just built).
