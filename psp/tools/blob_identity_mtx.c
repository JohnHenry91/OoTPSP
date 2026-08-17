/* Blob-private gIdentityMtx.
 *
 * Some room display lists (Jabu-Jabu's, bdan/bdan_boss) do
 * `gSPMatrix(&gIdentityMtx, ...)`. That symbol lives in the main binary
 * (src/code/sys_matrix.c), which the blob link cannot see -- and even if it
 * could, a PRX is relocated at load time so its address is not knowable when
 * the blob is linked. Linking a copy INTO the blob makes the reference a plain
 * segment-3 address like every other pointer in there.
 *
 * THE LAYOUT IS THE WHOLE POINT -- getting it wrong here is a bug this port has
 * already paid for once (session 9: the room was drawn through a permutation
 * matrix because gIdentityMtx was in the N64's own packing).
 *
 * The main binary's copy is declared with gdSPDefMtx(), which packs two matrix
 * elements per s32 as `(IPART(xx) << 16) | IPART(yx)`. That is big-endian
 * shaped, so on the little-endian PSP every adjacent pair reads out swapped --
 * which is exactly why Matrix_Init REBUILDS it at runtime via
 * Matrix_MtxFToMtx. A copy sitting in a blob gets no such fixup, so it has to
 * be written in the finished layout directly.
 *
 * Matrix_MtxFToMtx (src/code/sys_matrix.c) writes, as a flat u16 array:
 *     [0 .. 15]  integer parts,  in MtxF field order xx,yx,zx,wx, xy,yy,...
 *     [16 .. 31] fraction parts, same order
 * For the identity every element is 0.0 or 1.0, and 1.0f * 0x10000 == 0x10000,
 * so the integer part is 1 and the fraction 0. The diagonal lands on flat
 * indices 0, 5, 10 and 15.
 *
 * Deliberately typed as u16[32] rather than Mtx: it is a separate translation
 * unit, so only the symbol name and its 64-byte size matter to the linker, and
 * a flat array is the one spelling that cannot silently re-pack itself.
 */
unsigned short gIdentityMtx[32] = {
    /* integer parts */
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
    /* fraction parts */
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
};
