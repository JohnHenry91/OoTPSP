/* Stand-ins for the real N64 PI-bus (cartridge) and 64DD hardware entry
 * points. The DMA path game code actually uses — DmaMgr_RequestSync/Async
 * -> DmaMgr_DmaRomToRam (src/boot/z_std_dma.c) — does NOT go through these;
 * it reads the shipped ROM file directly via sceIo (see os_rom.c). These
 * two only remain because DmaMgr_AudioDmaHandler and
 * DmaMgr_DmaFromDriveRom (audio DMA callback and 64DD disk reads,
 * respectively) reference them unconditionally at the C level even though
 * neither is wired up or called yet on PSP — needed to link, not to work.
 * Revisit when real audio DMA is ported. */

#include <stddef.h>

#include "ultra64.h"

s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) {
    return -1; /* not implemented yet — see file comment */
}

OSPiHandle* osDriveRomInit(void) {
    return NULL; /* no 64DD on PSP */
}
