/* Unlike libultraship/Shipwright (desktop, x86 — no MIPS-style software-
 * visible cache, so their osInvalDCache/osWritebackDCache are no-ops), the
 * PSP's Allegrex core is MIPS with a real data/instruction cache, same as
 * N64. These forward directly to PSPSDK's cache-management calls so DMA'd
 * memory (once z_std_dma.c is ported) stays coherent.
 *
 * They must also reproduce libultra's *argument* semantics, which an earlier
 * version of this file did not. Real osInvalICache/osInvalDCache/
 * osWritebackDCache treat a non-positive nbytes as "do nothing" and a size at
 * or above the cache size as "operate on the whole cache". Forwarding the
 * value raw instead meant a negative size was reinterpreted as unsigned:
 * PPSSPP logged `Bad InvalidateICache: 088781b8 with len=-16` repeatedly
 * during gameplay, i.e. we were handing the kernel a ~4GB range. Callers
 * really do pass sizes like this -- the N64 code computes them as
 * end-minus-start and libultra absorbs the degenerate cases silently. */

#include <psputils.h>

#include "ultra64.h"

/* Allegrex: 16KB instruction cache, 16KB data cache. */
#define PSP_ICACHE_SIZE (16 * 1024)
#define PSP_DCACHE_SIZE (16 * 1024)

#if TARGET_PSP
/* Records degenerate calls so the *caller* can still be tracked down rather
 * than merely tolerated -- read with the WebSocket debugger, no file I/O. */
u32 gPspCacheBadCallCount = 0;
void* gPspCacheBadCallAddr = NULL;
s32 gPspCacheBadCallSize = 0;

static void PspCacheNoteBadCall(void* vaddr, s32 nbytes) {
    gPspCacheBadCallCount++;
    gPspCacheBadCallAddr = vaddr;
    gPspCacheBadCallSize = nbytes;
}
#else
#define PspCacheNoteBadCall(vaddr, nbytes) ((void)0)
#endif

void osWritebackDCache(void* vaddr, s32 nbytes) {
    if (nbytes <= 0) {
        PspCacheNoteBadCall(vaddr, nbytes);
        return;
    }
    if (nbytes >= PSP_DCACHE_SIZE) {
        sceKernelDcacheWritebackAll();
        return;
    }
    sceKernelDcacheWritebackRange(vaddr, (unsigned int)nbytes);
}

void osWritebackDCacheAll(void) {
    sceKernelDcacheWritebackAll();
}

void osInvalDCache(void* vaddr, s32 nbytes) {
    if (nbytes <= 0) {
        PspCacheNoteBadCall(vaddr, nbytes);
        return;
    }
    if (nbytes >= PSP_DCACHE_SIZE) {
        sceKernelDcacheWritebackInvalidateAll();
        return;
    }
    sceKernelDcacheInvalidateRange(vaddr, (unsigned int)nbytes);
}

void osInvalICache(void* vaddr, s32 nbytes) {
    if (nbytes <= 0) {
        PspCacheNoteBadCall(vaddr, nbytes);
        return;
    }
    if (nbytes >= PSP_ICACHE_SIZE) {
        sceKernelIcacheInvalidateAll();
        return;
    }
    sceKernelIcacheInvalidateRange(vaddr, (unsigned int)nbytes);
}
