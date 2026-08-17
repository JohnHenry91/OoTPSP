#ifndef PSP_BG_RECT_H
#define PSP_BG_RECT_H

/* A port-private display-list command for the pre-rendered room backgrounds.
 *
 * The N64 draws these with the S2DEX microcode (gSPBgRect1Cyc / gSPBgRectCopy
 * on a uObjBg, after gSPLoadUcodeL swaps the microcode). This interpreter has
 * one opcode table, and S2DEX's numbers collide with F3DEX2's -- G_BG_1CYC is
 * 0x01, i.e. G_VTX here -- so interpreting that display list corrupted the rest
 * of the frame, and the whole path was disabled (z_room.c).
 *
 * Since the port controls both the emitter (Room_DrawBackground2D's PSP branch)
 * and the interpreter, express it as one command of our own instead of
 * emulating a second microcode. Opcode 0x0F is unassigned in both F3DEX2 and
 * F3D, so it cannot be confused with a real one in either direction.
 *
 * The parameters do not fit in a command's two words, so the command carries a
 * pointer to a PspBgRect built in the display buffer just ahead of it -- the
 * same trick the original does with its uObjBg, and it is branched over the
 * same way, so nothing ever tries to interpret it as commands.
 */
#include <stdint.h>

#define G_PSP_BGRECT 0x0F

/* Port-private marker, opcode 0x0E (unassigned in F3DEX2 and F3D, same
 * reasoning as G_PSP_BGRECT above). w1 carries an arbitrary tag; gfx_pc.c uses
 * it to attribute triangles to whatever part of the frame emitted them, which
 * is otherwise impossible: the per-frame counters are filled while the display
 * list is INTERPRETED, long after the code that built it has returned, so
 * bracketing a draw in Play_Draw cannot work. Emitted only by diagnostics. */
#define G_PSP_MARK 0x0E
#define PSP_MARK_SKYBOX_BEGIN 1
#define PSP_MARK_SKYBOX_END   2

typedef struct {
    /* Segmented (0x03xxxxxx) address of the image, resolved by the
     * interpreter -- NOT resolved here, because the emitter runs before the
     * frame's segment table is necessarily in force. */
    void* source;
    uint16_t width;
    uint16_t height;
    int16_t offsetX;
    int16_t offsetY;
    uint8_t fmt;
    uint8_t siz;
    uint16_t pad;
} PspBgRect; // size = 0x10

#endif
