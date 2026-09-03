#include "gameplay_keep_0x4D160.h"
#include "sun_textures.h"
#include "sun_evening_textures.h"
#include "gfx.h"

Gfx gKokiriDustMoteMaterialDL[9] = {
#include "assets/objects/gameplay_keep/gKokiriDustMoteMaterialDL.inc.c"
};

Gfx gKokiriDustMoteModelDL[3] = {
#include "assets/objects/gameplay_keep/gKokiriDustMoteModelDL.inc.c"
};

#if TARGET_PSP
/* Die Sonne, aus dem ROM neu geschrieben -- der einzige Ort in diesem Port, an
 * dem eine Anzeigeliste von der ROM-Vorlage abweichen MUSS.
 *
 * gSunDL im ROM (pal-1.0, VROM 0xF501C0) laedt seine drei Streifen mit
 * `gDPLoadTextureBlock`, Render-Tile `G_IM_SIZ_8b`, line 8 (= 64 Byte/Zeile),
 * Kachel 64x32 -- nachgemessen an den Rohbytes, nicht vermutet:
 *   F5881000 00094260  SETTILE fmt=I siz=8b line=8 tmem=0 masks=6 maskt=5
 *   F2000000 000FC07C  SETTILESIZE lrs=63 lrt=31
 * Die DATEN dort sind aber I4: 64 breit, 31+16+16 = 63 Zeilen, drei Streifen
 * hintereinander (0x4C160 / 0x4C540 / 0x4C740, je 992/512/512 Byte). Als I8
 * gelesen fasst jede Zeile ZWEI I4-Zeilen zu je halber Breite zusammen -- und
 * genau das war der Fehler auf dem Schirm: die Sonne vier Mal, 2x2.
 * Nachgeprueft, indem dieselben ROM-Bytes einmal als I8 64x32 und einmal als
 * I4 64x63 dekodiert wurden; nur die I4-Lesart ergibt eine Sonne.
 *
 * Das ist also `gDPLoadTextureBlock` dort, wo `gDPLoadTextureBlock_4b`
 * hingehoert -- ein Fehler in der ROM-Anzeigeliste selbst, den die echte RDP
 * offenbar nicht so zeigt wie unser Sampler. Wir koennen ihn nicht im Renderer
 * heilen, ohne pro Textur ein Format-Metadatum mitzufuehren (so loest es Ship
 * of Harkinian, `RawTexMetadata`); bis es das hier gibt, ist die Anzeigeliste
 * der ehrlichste Ort fuer die Korrektur. Reihenfolge und Laenge (49 Gfx)
 * bleiben exakt wie im ROM, nur die beiden Lade-Makros werden zu ihren
 * 4-Bit-Varianten. */
Gfx gSunDL[49] = {
    gsSPMatrix(0x01000000, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW),
    gsDPPipeSync(),

    gsDPLoadTextureBlock_4b(gSun1Tex, G_IM_FMT_I, gSun1Tex_WIDTH, gSun1Tex_HEIGHT, 0,
                            G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsDPLoadMultiBlock_4b(gSunEvening1Tex, 0x0100, 1, G_IM_FMT_I, gSunEvening1Tex_WIDTH, gSunEvening1Tex_HEIGHT, 0,
                          G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsSPVertex(&gSunVtx[0], 12, 0),
    gsSP2Triangles(0, 1, 2, 0, 2, 1, 3, 0),

    gsDPLoadTextureBlock_4b(gSun2Tex, G_IM_FMT_I, gSun2Tex_WIDTH, gSun2Tex_HEIGHT, 0,
                            G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsDPLoadMultiBlock_4b(gSunEvening2Tex, 0x0100, 1, G_IM_FMT_I, gSunEvening2Tex_WIDTH, gSunEvening2Tex_HEIGHT, 0,
                          G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsSP2Triangles(4, 5, 6, 0, 6, 5, 7, 0),

    gsDPLoadTextureBlock_4b(gSun3Tex, G_IM_FMT_I, gSun3Tex_WIDTH, gSun3Tex_HEIGHT, 0,
                            G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsDPLoadMultiBlock_4b(gSunEvening3Tex, 0x0100, 1, G_IM_FMT_I, gSunEvening3Tex_WIDTH, gSunEvening3Tex_HEIGHT, 0,
                          G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsSP2Triangles(8, 9, 10, 0, 10, 9, 11, 0),

    gsSPEndDisplayList(),
};
#else
Gfx gSunDL[49] = {
#include "assets/objects/gameplay_keep/gSunDL.inc.c"
};
#endif

Vtx gSunVtx[] = {
#include "assets/objects/gameplay_keep/gSunVtx.inc.c"
};

Vtx gKokiriDustMoteModelVtx[] = {
#include "assets/objects/gameplay_keep/gKokiriDustMoteModelVtx.inc.c"
};
