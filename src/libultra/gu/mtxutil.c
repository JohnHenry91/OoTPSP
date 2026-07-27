#include "ultra64.h"

/* Both functions below pack/unpack against the SAME real N64 Mtx layout
 * sys_matrix.c's Matrix_MtxFToMtx uses: 16 u16 integer parts followed by 16
 * u16 fractional parts, each in plain row-major order (intPart[row][col] /
 * fracPart[row][col] hold matrix element [row][col] directly -- despite the
 * MtxF struct's xx/xy/.../ww field-name comment in ultratypes.h warning
 * that the *named* fields are transposed from how a matrix is normally
 * written -- e.g. translation lives in [xw,yw,zw], not [wx,wy,wz] -- that
 * naming quirk is already resolved by the time Matrix_MtxFToMtx walks
 * src->xx, src->yx, ... in its written order, which comes out as a
 * straightforward row-major flatten of the mf[4][4] float array with no
 * further transpose needed here).
 *
 * The previous implementation of these two functions instead packed each
 * ROW's two adjacent elements' hi/lo halves together into single 32-bit
 * words via arithmetic -- a scheme that happens to round-trip correctly
 * through gfx_pc.c's OLD int32-reinterpret unpacking (both write and read
 * happen natively on the same little-endian PSP, so no real endianness bug
 * there), but does NOT match Matrix_MtxFToMtx's layout at all. Since
 * guLookAt/guPerspective/guTranslate/etc. (this whole file) and
 * Matrix_Finalize/Matrix_MtxFToMtx (sys_matrix.c, real unmodified decomp
 * code) are both expected to produce the ONE real Mtx format consumed by
 * gfx_pc.c's single gfx_sp_matrix unpacking path, having two different
 * packings here was a real bug -- confirmed this session via the boot
 * logo's camera/projection matrices producing a ~20x too-large clip-space w
 * (hence a much-too-small on-screen size) after gfx_sp_matrix was fixed to
 * correctly read Matrix_MtxFToMtx's layout, which broke what had
 * (coincidentally, via that self-consistent same-machine round-trip) been
 * working for THIS file's matrices. An initial rewrite attempt here
 * mistakenly added a transpose (misreading the MtxF field-name comment
 * above as meaning the STORAGE needs transposing too, not just the
 * *names*) which regressed real gameplay rendering -- corrected to a
 * direct, non-transposed row-major write/read matching Matrix_MtxFToMtx's
 * actual behavior exactly. */
void guMtxF2L(f32 mf[4][4], Mtx* m) {
    s32 row, col;
    u16* intPart = (u16*)&m->m[0][0];
    u16* fracPart = (u16*)&m->m[2][0];

    for (row = 0; row < 4; row++) {
        for (col = 0; col < 4; col++) {
            s32 whole = FTOFIX32(mf[row][col]);
            intPart[row * 4 + col] = (whole >> 16) & 0xFFFF;
            fracPart[row * 4 + col] = whole & 0xFFFF;
        }
    }
}

void guMtxL2F(f32 mf[4][4], Mtx* m) {
    s32 row, col;
    u16* intPart = (u16*)&m->m[0][0];
    u16* fracPart = (u16*)&m->m[2][0];

    for (row = 0; row < 4; row++) {
        for (col = 0; col < 4; col++) {
            s32 whole = ((s32)(s16)intPart[row * 4 + col] << 16) | fracPart[row * 4 + col];
            mf[row][col] = FIX32TOF(whole);
        }
    }
}

void guMtxIdentF(f32 mf[4][4]) {
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (i == j) {
                mf[i][j] = 1.0;
            } else {
                mf[i][j] = 0.0;
            }
        }
    }
}

void guMtxIdent(Mtx* m) {
    f32 mf[4][4];

    guMtxIdentF(mf);

    guMtxF2L(mf, m);
}
