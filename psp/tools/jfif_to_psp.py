#!/usr/bin/env python3
"""Decode a room's pre-rendered JFIF background into a PSP-native 5551 image.

WHY THIS EXISTS
---------------
OoT stores the pre-rendered backgrounds of its "image" rooms (all house
interiors, shops, the market, ...) as a JPEG blob inside the room file, and
decodes it ON THE CONSOLE: Room_DecodeJpeg() -> Jpeg_Decode(), whose entropy
decode runs on the CPU but whose dequantise + IDCT + YCbCr->RGBA16 stage runs
as an RSP microcode task (njpgdsp).  There is no RSP here, so that task would
have to be re-implemented in software and then run ~150 ms per room load.

There is no reason to do any of that at runtime.  The decoded image is exactly
as big as the JPEG's storage slot (SCREEN_WIDTH * SCREEN_HEIGHT * 2 = 153600
bytes -- the array is declared at that fixed size), so the decode fits in place
and can happen at BUILD time, which is exactly what psp/tools/make_scene_blob.sh
already does for every other asset ("convert assets at build time to native byte
order, with references resolved").  SoH/ZAPD reach the same conclusion: their
Jpeg_ScheduleDecoderTask is #if 0'd out because ZAPD decodes the background at
extraction time.

OUTPUT FORMAT
-------------
Not N64 RGBA5551, but the PSP GE's own GU_PSM_5551 (bit 0-4 R, 5-9 G, 10-14 B,
15 A), so gfx_scegu_draw_background() can hand the pointer straight to
sceGuTexImage with no conversion, no copy and no texture-cache round trip.

The emitted file mirrors what tools/assets/build_jfif produces -- one
`0x...,`-per-u64 initialiser list for a `u64 name[]` array -- but the words are
written so that the LITTLE-endian compiler stores the intended byte stream, i.e.
the pixels are already in memory order on the target.  That deliberately breaks
the "compiled u64 literal needs an 8-byte unswap" rule that gfx_pc.c's
tex_needs_u64_unswap() applies to ordinary textures: this data never goes
through that path, and un-swapping 76800 pixels per frame (or even once per room
load) would be pure waste.

Usage: jfif_to_psp.py <in.jpg> <out.jpg.inc.c>
"""

import sys

from PIL import Image

WIDTH = 320
HEIGHT = 240


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: jfif_to_psp.py <in.jpg> <out.jpg.inc.c>\n")
        return 1

    src, dst = argv[1], argv[2]

    with Image.open(src) as im:
        if im.size != (WIDTH, HEIGHT):
            sys.stderr.write(
                "jfif_to_psp.py: %s is %dx%d, expected %dx%d\n"
                % (src, im.size[0], im.size[1], WIDTH, HEIGHT)
            )
            return 1
        rgb = im.convert("RGB").tobytes()

    # RGB888 -> GU_PSM_5551, one u16 per pixel, alpha always opaque.
    px = bytearray(WIDTH * HEIGHT * 2)
    for i in range(WIDTH * HEIGHT):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        v = 0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)
        px[2 * i] = v & 0xFF
        px[2 * i + 1] = v >> 8

    # Group into u64 literals such that a little-endian store reproduces `px`
    # byte for byte: the first byte of the group is the LOW byte of the word.
    out = []
    for off in range(0, len(px), 8):
        w = 0
        for k in range(8):
            w |= px[off + k] << (8 * k)
        out.append("0x%016X," % w)

    with open(dst, "w") as f:
        for i in range(0, len(out), 4):
            f.write(" ".join(out[i : i + 4]) + "\n")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
