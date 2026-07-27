/* Unlike libultraship/Shipwright (desktop, x86 — no MIPS-style software-
 * visible cache, so their osInvalDCache/osWritebackDCache are no-ops), the
 * PSP's Allegrex core is MIPS with a real data/instruction cache, same as
 * N64. These forward directly to PSPSDK's cache-management calls so DMA'd
 * memory (once z_std_dma.c is ported) stays coherent. */

#include <psputils.h>

#include "ultra64.h"

void osWritebackDCache(void* vaddr, s32 nbytes) {
    sceKernelDcacheWritebackRange(vaddr, (unsigned int)nbytes);
}

void osWritebackDCacheAll(void) {
    sceKernelDcacheWritebackAll();
}

void osInvalDCache(void* vaddr, s32 nbytes) {
    sceKernelDcacheInvalidateRange(vaddr, (unsigned int)nbytes);
}

void osInvalICache(void* vaddr, s32 nbytes) {
    sceKernelIcacheInvalidateRange(vaddr, (unsigned int)nbytes);
}
