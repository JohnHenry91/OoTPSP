#ifndef PSP_AUDIO_H
#define PSP_AUDIO_H

#include <stdint.h>

void PspAudio_Init(void);

/* Debug-facing counters for the WebSocket debugger / a HUD, same convention
 * as gfx_pc.h's gfx_pc_stat_* accessors -- read to tell "PspAudio_Output is
 * never called" (synthesis/retrace path is dead) apart from "it's called
 * with real but silent (all-zero) PCM" (a mixing/volume bug further up). */
uint32_t PspAudio_StatOutputCalls(void);
uint32_t PspAudio_StatLastNumSamples(void);
int32_t PspAudio_StatLastPeakSample(void);
uint32_t PspAudio_StatReserveFailures(void);
/* Whether the hardware/emulator actually accepted the blocks we handed it --
 * the one link in the chain that memory reads upstream cannot prove. */
uint32_t PspAudio_StatOutputErrors(void);
int32_t PspAudio_StatLastOutputRet(void);
/* Dropout counters: underruns is how often the DAC had already gone dry when
 * the next block arrived (what choppy audio actually is); minRest is the
 * closest it ever came to that. */
uint32_t PspAudio_StatUnderruns(void);
uint32_t PspAudio_StatMinRest(void);

#endif
