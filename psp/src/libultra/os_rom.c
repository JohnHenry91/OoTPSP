/* sceIo-backed "ROM" access — see include/psp_rom.h. */

#include <pspiofilemgr.h>

#include "psp_rom.h"

static SceUID sRomFd = -1;

void PspRom_Init(const char* path) {
    sRomFd = sceIoOpen(path, PSP_O_RDONLY, 0777);
}

void PspRom_Read(uint32_t romOffset, void* dst, size_t size) {
    if (sRomFd < 0) {
        return;
    }
    sceIoLseek(sRomFd, romOffset, PSP_SEEK_SET);
    sceIoRead(sRomFd, dst, size);
}
