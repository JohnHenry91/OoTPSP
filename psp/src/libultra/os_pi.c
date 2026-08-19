/* Stand-ins for the real N64 PI-bus (cartridge) and 64DD hardware entry
 * points. The main DMA path game code uses — DmaMgr_RequestSync/Async ->
 * DmaMgr_DmaRomToRam (src/boot/z_std_dma.c) — does NOT go through these; it
 * reads the shipped ROM file directly via sceIo (see os_rom.c). osDriveRomInit
 * (64DD disk reads) is unreachable the same way DmaMgr_DmaFromDriveRom's own
 * caller is (see n64dd_stub.c) — kept only so it links.
 *
 * osEPiStartDma is different: it's the real leaf DmaMgr_AudioDmaHandler
 * (src/boot/z_std_dma.c, real ported decomp code, unmodified) calls for every
 * audio DMA (AudioLoad_SyncDma/AudioLoad_Dma, src/audio/internal/load.c).
 * Real N64 hardware queues the transfer onto a PI-manager thread and returns
 * immediately, with that thread posting mb->hdr.retQueue once the hardware
 * transfer completes. This port has no such thread (single-loop collapse,
 * see project plan decision #4) and doesn't need one: PspRom_Read already
 * knows how to serve an audio blob's fake "ROM address" the exact same way
 * it already serves scene/room blobs (see psp_blob_assets.c) -- so just do
 * the read synchronously right here and post the completion immediately.
 * This is safe because it only ever runs on AudioMgr's own real thread
 * (src/code/audio_thread_manager.c), never the main game/render thread.
 */

#include <stddef.h>

#include "ultra64.h"
#include "psp_rom.h"

s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) {
    if (direction != OS_READ) {
        return -1;
    }

    PspRom_Read(mb->devAddr, mb->dramAddr, mb->size);

    /* AudioLoad_SyncDma (the only caller that ever waits on this) discards
     * the message payload -- it only cares that ONE arrives. OS_MESG_BLOCK
     * per the same reasoning as DmaMgr_ThreadEntry's own notify fix (see
     * that file's comment): NOBLOCK against a queue backed by a real PSP
     * semaphore can lose the wakeup on a race, hanging the waiter forever
     * with no fault -- and unlike that queue, this one really can be raced,
     * since a soundfont/sample-bank/sequence load can be requested from
     * more than one place. */
    osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_BLOCK);

    return 0;
}

OSPiHandle* osDriveRomInit(void) {
    return NULL; /* no 64DD on PSP */
}
