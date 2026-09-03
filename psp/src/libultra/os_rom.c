/* sceIo-backed "ROM" access — see include/psp_rom.h. */

#include <pspiofilemgr.h>

#include "psp_rom.h"
#include "psp_blob_assets.h"

static SceUID sRomFd = -1;

/* Largest plausible single transfer. The biggest real file in the ROM is a few
 * hundred KB; anything past this is a computed-size bug in the caller, not a
 * genuine load. Deliberately generous so it never rejects real traffic. */
#define PSP_ROM_MAX_READ (4u * 1024u * 1024u)

/* PSP user RAM. A DMA destination outside this is a corrupted pointer. */
#define PSP_RAM_START 0x08800000u
#define PSP_RAM_END   0x0A000000u

/* Diagnostics for rejected reads -- plain globals, read with the debugger.
 * gPspRomBadReadRa records the CALLER, which is the whole point: a bad size
 * here is always someone else's arithmetic. */
unsigned int gPspRomBadReadCount;
unsigned int gPspRomBadReadOffset;
unsigned int gPspRomBadReadSize;
unsigned int gPspRomBadReadDst;
unsigned int gPspRomBadReadRa;
/* Reads that did NOT deliver the bytes the caller asked for. Every one of
 * these leaves the destination holding whatever was there before -- for a
 * sample DMA buffer that is the PREVIOUS note's sample data, which the ADPCM
 * decoder then runs through this note's predictor book. That does not sound
 * like the wrong instrument, it sounds like a burst of noise. Counted here
 * because the failure is otherwise completely silent: neither PspRom_Read nor
 * AudioLoad_Dma has an error channel back to the audio engine. */
unsigned int gPspRomUnservedReads;

/* Kept so the descriptor can be rebuilt later; see PspRom_NotifyResume. */
static char sRomPath[256];

/* Descriptors reopened after a standby, for the HUD. */
unsigned int gPspRomReopens;
unsigned int gPspRomReopenFails;

static volatile int sResumePending;

void PspRom_NotifyResume(void) {
    sResumePending = 1;
}

void PspRom_Init(const char* path) {
    if (path != NULL) {
        unsigned int i = 0;

        while ((path[i] != '\0') && (i < sizeof(sRomPath) - 1)) {
            sRomPath[i] = path[i];
            i++;
        }
        sRomPath[i] = '\0';
    }
    sRomFd = sceIoOpen(path, PSP_O_RDONLY, 0777);
}

/* Standby powers the Memory Stick down and every descriptor open across it
 * comes back invalid. This one is open for the entire session -- PspRom_Init
 * runs once at boot and nothing ever reopens it -- so after a wake every raw
 * .z64 read returns garbage or nothing.
 *
 * That is a much bigger deal than the file's remaining traffic suggests,
 * because of WHAT is still served this way. Almost every asset now comes from
 * the native blobs; the skybox does not (see the block comment in z_vr_box.c),
 * and in prerendered rooms with a PIVOT camera the skybox IS the visible
 * surroundings -- the walls and sky the player is looking at. So a stale
 * descriptor here shows up as exactly one symptom: those rooms, and only those
 * rooms, drawn over in noise after a resume. Rooms with a FIXED camera hide it
 * behind their background image, and shops never leave LOCKED at all.
 *
 * Reopen from the path rather than trying to revive the handle. Done on the
 * reading thread, not in the callback, for the same reason as everywhere else
 * here: a power callback must not do I/O. */
static void PspRomHandleResume(void) {
    sResumePending = 0;

    if (sRomFd >= 0) {
        sceIoClose(sRomFd);
        sRomFd = -1;
    }
    if (sRomPath[0] == '\0') {
        ++gPspRomReopenFails;
        return;
    }
    sRomFd = sceIoOpen(sRomPath, PSP_O_RDONLY, 0777);
    if (sRomFd < 0) {
        ++gPspRomReopenFails;
    } else {
        ++gPspRomReopens;
    }
}

/* A bad `size` here is not a harmless failed read: sceIoRead writes `size`
 * bytes starting at `dst`, so a garbage length silently smashes megabytes of
 * unrelated RAM and then blocks for a very long time doing it. That is
 * indistinguishable from a hang, and it corrupts whatever happens to live
 * after the destination -- observed live as this port's own .bss diagnostic
 * counters changing value on their own while the game appeared frozen inside
 * this function.
 *
 * So validate instead of trusting, the same way os_cache.c had to be taught
 * libultra's real argument semantics: refuse the transfer, record enough to
 * identify the caller, and let the game continue with a bounded glitch rather
 * than an unbounded memory smash. */
void PspRom_Read(uint32_t romOffset, void* dst, size_t size) {
    uintptr_t d = (uintptr_t)dst;

    if (size == 0) {
        return;
    }

    /* Blobs first. This is the single leaf every asset transfer funnels
     * through, which is why the hook lives here rather than at the callers:
     * DmaMgr's queue and thread, the allocation, and Room_RequestNewRoom's
     * buffer paging all stay exactly as the decomp wrote them, and the caller
     * cannot forget to ask. See psp/include/psp_blob_assets.h.
     *
     * Deliberately placed AFTER the size==0 check but BEFORE the destination
     * validation below, so a blob-served transfer still gets its dst range
     * validated by the same rules -- the guard exists because a bad size here
     * smashes RAM regardless of where the bytes come from. */
    if (dst != NULL && size <= PSP_ROM_MAX_READ && d >= PSP_RAM_START && (d + size) <= PSP_RAM_END) {
        if (PspBlob_Read(romOffset, dst, size)) {
            return;
        }
    }

    if (sResumePending) {
        PspRomHandleResume();
    }

    if (sRomFd < 0) {
        ++gPspRomUnservedReads;
        return;
    }

    if (size > PSP_ROM_MAX_READ || dst == NULL || d < PSP_RAM_START || d >= PSP_RAM_END ||
        (d + size) > PSP_RAM_END) {
        ++gPspRomUnservedReads;
        ++gPspRomBadReadCount;
        gPspRomBadReadOffset = romOffset;
        gPspRomBadReadSize = (unsigned int)size;
        gPspRomBadReadDst = (unsigned int)d;
        gPspRomBadReadRa = (unsigned int)(uintptr_t)__builtin_return_address(0);
        return;
    }

    /* Raw big-endian ROM data is about to land here, so any blob range still
     * covering it must stop claiming this memory is native-endian. Without
     * this the arena hands a recycled address to a raw load and the endian
     * decisions keyed on that address go the wrong way -- see the range
     * bookkeeping in psp_blob_assets.c. */
    PspBlob_InvalidateRange(dst, size);

    sceIoLseek(sRomFd, romOffset, PSP_SEEK_SET);
    if ((size_t)sceIoRead(sRomFd, dst, size) != size) {
        ++gPspRomUnservedReads;
    }
}
