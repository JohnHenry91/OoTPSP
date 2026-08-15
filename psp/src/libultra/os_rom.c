/* sceIo-backed "ROM" access — see include/psp_rom.h. */

#include <pspiofilemgr.h>

#include "psp_rom.h"

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

void PspRom_Init(const char* path) {
    sRomFd = sceIoOpen(path, PSP_O_RDONLY, 0777);
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

    if (sRomFd < 0) {
        return;
    }

    if (size == 0) {
        return;
    }

    if (size > PSP_ROM_MAX_READ || dst == NULL || d < PSP_RAM_START || d >= PSP_RAM_END ||
        (d + size) > PSP_RAM_END) {
        ++gPspRomBadReadCount;
        gPspRomBadReadOffset = romOffset;
        gPspRomBadReadSize = (unsigned int)size;
        gPspRomBadReadDst = (unsigned int)d;
        gPspRomBadReadRa = (unsigned int)(uintptr_t)__builtin_return_address(0);
        return;
    }

    sceIoLseek(sRomFd, romOffset, PSP_SEEK_SET);
    sceIoRead(sRomFd, dst, size);
}
