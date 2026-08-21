#ifndef PSP_ROM_H
#define PSP_ROM_H

/* PSP-specific (not libultra-shaped) helper backing DmaMgr_DmaRomToRam
 * (src/boot/z_std_dma.c): reads directly out of the built .z64 shipped
 * alongside EBOOT.PBP, since PSP has no cartridge/PI-bus to DMA from. See
 * PORTING.md for why this is the chosen replacement for N64 PI-bus DMA. */

#include <stddef.h>
#include <stdint.h>

void PspRom_Init(const char* path);
void PspRom_Read(uint32_t romOffset, void* dst, size_t size);

/* Reads that left the destination holding stale bytes -- see os_rom.c. */
extern unsigned int gPspRomUnservedReads;

#endif
