/* Storage for the dead 64DD-expansion globals declared in include/n64dd.h —
 * see that file's comment. Always NULL/0: PSP has no 64DD, and neither did
 * any pal-1.0 N64 cartridge (Japan-only add-on), so the branches in
 * src/boot/z_std_dma.c guarded by these are unreachable either way. */

#include "n64dd.h"

n64ddStruct_80121220* B_80121220 = NULL;
u8 D_80121212 = 0;
vu8 D_80121214 = 0;
