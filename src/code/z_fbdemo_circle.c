#include "transition_circle.h"

#include "color.h"
#include "gfx.h"
#include "sfx.h"
#include "tex_len.h"
#include "transition.h"

typedef enum TransitionCircleDirection {
    /* 0 */ TRANS_CIRCLE_DIR_IN,
    /* 1 */ TRANS_CIRCLE_DIR_OUT
} TransitionCircleDirection;

// unused
Gfx sTransCircleEmptyDL[] = {
    gsSPEndDisplayList(),
};

#define sTransCircleNormalTex_WIDTH 16
#define sTransCircleNormalTex_HEIGHT 64
u64 sTransCircleNormalTex[TEX_LEN(u64, sTransCircleNormalTex_WIDTH, sTransCircleNormalTex_HEIGHT, 8)] = {
#include "assets/code/fbdemo_circle/sTransCircleNormalTex.i8.inc.c"
};

#define sTransCircleWaveTex_WIDTH 16
#define sTransCircleWaveTex_HEIGHT 64
u64 sTransCircleWaveTex[TEX_LEN(u64, sTransCircleWaveTex_WIDTH, sTransCircleWaveTex_HEIGHT, 8)] = {
#include "assets/code/fbdemo_circle/sTransCircleWaveTex.i8.inc.c"
};

#define sTransCircleRippleTex_WIDTH 16
#define sTransCircleRippleTex_HEIGHT 64
u64 sTransCircleRippleTex[TEX_LEN(u64, sTransCircleRippleTex_WIDTH, sTransCircleRippleTex_HEIGHT, 8)] = {
#include "assets/code/fbdemo_circle/sTransCircleRippleTex.i8.inc.c"
};

#define sTransCircleStarburstTex_WIDTH 16
#define sTransCircleStarburstTex_HEIGHT 64
u64 sTransCircleStarburstTex[TEX_LEN(u64, sTransCircleStarburstTex_WIDTH, sTransCircleStarburstTex_HEIGHT, 8)] = {
#include "assets/code/fbdemo_circle/sTransCircleStarburstTex.i8.inc.c"
};

Vtx sTransCircleVtx[34] = {
#include "assets/code/fbdemo_circle/sTransCircleVtx.inc.c"
};

Gfx sTransCircleDL[26] = {
#include "assets/code/fbdemo_circle/sTransCircleDL.inc.c"
};

void TransitionCircle_Start(void* thisx) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    this->isDone = false;

    switch (this->appearanceType) {
        case TCA_WAVE:
            this->texture = sTransCircleWaveTex;
            break;

        case TCA_RIPPLE:
            this->texture = sTransCircleRippleTex;
            break;

        case TCA_STARBURST:
            this->texture = sTransCircleStarburstTex;
            break;

        default:
            this->texture = sTransCircleNormalTex;
            break;
    }

    if (this->speedType == TCS_FAST) {
        this->speed = 20;
    } else {
        this->speed = 10;
    }

    if (this->colorType == TCC_BLACK) {
        this->color.rgba = RGBA8(0, 0, 0, 255);
    } else if (this->colorType == TCC_WHITE) {
        this->color.rgba = RGBA8(160, 160, 160, 255);
    } else if (this->colorType == TCC_GRAY) {
        /* Als u32 setzen, nicht feldweise: _Draw liest diese Farbe ueber
         * this->color.rgba. Feldweise geschrieben und als u32 gelesen ist
         * genau die Byteordnungsfalle -- auf little-endian kaeme (100,100,100)
         * mit Alpha 255 als r=255, g=100, b=100 wieder heraus. Die uebrigen
         * drei Zuweisungen in dieser Funktion setzen bereits .rgba; diese eine
         * scherte aus. */
        this->color.rgba = RGBA8(100, 100, 100, 255);
    } else {
        this->speed = 40;
        this->color.rgba = (this->appearanceType == TCA_WAVE) ? RGBA8(0, 0, 0, 255) : RGBA8(160, 160, 160, 255);
    }

    if (this->direction != TRANS_CIRCLE_DIR_IN) {
        this->texY = (s32)(0.0f * (1 << 2));
        if (this->colorType == TCC_SPECIAL) {
            this->texY = (s32)(62.5f * (1 << 2));
        }
    } else {
        this->texY = (s32)(125.0f * (1 << 2));
        if (this->appearanceType == TCA_RIPPLE) {
            SFX_PLAY_CENTERED(NA_SE_OC_SECRET_WARP_OUT);
        }
    }

    guPerspective(&this->projection, &this->normal, 60.0f, (4.0f / 3.0f), 10.0f, 12800.0f, 1.0f);
    guLookAt(&this->lookAt, 0.0f, 0.0f, 400.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
}

void* TransitionCircle_Init(void* thisx) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    bzero(this, sizeof(TransitionCircle));
    return this;
}

void TransitionCircle_Destroy(void* thisx) {
}

void TransitionCircle_Update(void* thisx, s32 updateRate) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    if (this->direction != TRANS_CIRCLE_DIR_IN) {
        if (this->texY == 0) {
            if (this->appearanceType == TCA_RIPPLE) {
                SFX_PLAY_CENTERED(NA_SE_OC_SECRET_WARP_IN);
            }
        }
        this->texY += this->speed * 3 / updateRate;
        if (this->texY >= (s32)(125.0f * (1 << 2))) {
            this->texY = (s32)(125.0f * (1 << 2));
            this->isDone = true;
        }
    } else {
        this->texY -= this->speed * 3 / updateRate;
        if (this->colorType != TCC_SPECIAL) {
            if (this->texY <= (s32)(0.0f * (1 << 2))) {
                this->texY = (s32)(0.0f * (1 << 2));
                this->isDone = true;
            }
        } else {
            if (this->texY <= (s32)(62.5f * (1 << 2))) {
                this->texY = (s32)(62.5f * (1 << 2));
                this->isDone = true;
            }
        }
    }
}

/* 15,6 statt der 14,8 aus der Dekompilierung -- sonst bleiben an allen vier
 * Bildecken dreieckige Schlitze offen, durch die man waehrend der Blende die
 * Szene sieht.
 *
 * DAS SEITENVERHAELTNIS IST NICHT DIE URSACHE, so naheliegend das aussieht.
 * Die Projektion in _Start ist auf 4:3 verdrahtet, der PSP-Schirm ist 480x272.
 * Aber die NDC-Grenze +-1 dieser Projektion landet auf dem Bildrand, egal wie
 * breit der Viewport ist -- das Verhaeltnis "Bildecke zu Kranzradius" ist auf
 * 4:3 und auf 16:9 dasselbe. Die Luecken gibt es auf einem echten N64 genauso.
 *
 * Die Rechnung, gegen die laufende Portierung nachgemessen:
 *
 *   Kranz    16-Eck, Nennradius 25 (sTransCircleVtx), mal scale
 *            -> 370,0 Welteinheiten bei 14,8; in ECKRICHTUNG reicht das
 *               Vieleck wegen der Sehne aber nur bis 368,4
 *   Bildecke tan(30 Grad) * 400 * sqrt((4/3)^2 + 1) = 384,9
 *            (60 Grad Oeffnung, Auge bei z = 400 aus guLookAt)
 *   Fehlbetrag 4,47 % -> noetig sind 15,462; 15,6 gibt knapp 1 Prozent
 *            Reserve gegen die Rundung in den Festkommamatrizen.
 *
 * Nachgemessen wurde die Projektionskette selbst ueber gPspCircleNdcX/Y in
 * gfx_pc.c (groesstes |x/w| bzw. |y/w| der Randvertices, in Tausendsteln):
 * vorhergesagt 1202 / 1602, gemessen 1201 / 1602. An guPerspective, guLookAt
 * und den Matrizen ist also nichts kaputt -- 14,8 ist schlicht zu klein, und
 * in der Dekompilierung steht ueber tPos/rot/scale nicht umsonst ausdruecklich
 * "best guess": die Zeile ist geraten, nicht aus dem ROM belegt.
 *
 * Auf dem N64 hat das niemand gesehen, weil der Fernseher-Overscan genau die
 * Ecken verschluckt hat -- derselbe Grund, aus dem die normale Blende rechts
 * einen Streifen frei liess (siehe gfx_rectangle_covers_width in gfx_pc.c).
 * Ship of Harkinian ist der einzige Referenzport, der hier ueberhaupt etwas
 * tut, und multipliziert mit dem vollen Seitenverhaeltnis; das waere fuer uns
 * 26,1 und wuerde die Blende sichtbar aufblasen, weil die Texturrampe mit der
 * Geometrie mitwaechst.
 *
 * Zur Laufzeit setzbar (Debugger), damit der Wert ohne Neubau nachgezogen
 * werden kann. */
f32 gPspTransCircleScale = 15.6f;

void TransitionCircle_Draw(void* thisx, Gfx** gfxP) {
    Gfx* gfx = *gfxP;
    Mtx* modelView;
    TransitionCircle* this = (TransitionCircle*)thisx;
    Gfx* texScroll;
    // These variables are a best guess based on the other transition types.
    f32 tPos = 0.0f;
    f32 rot = 0.0f;
    f32 scale = gPspTransCircleScale;

    modelView = this->modelView[this->frame];

    this->frame ^= 1;
    gDPPipeSync(gfx++);
    texScroll = Gfx_BranchTexScroll(&gfx, this->texX, this->texY, 16, 64);
    gSPSegment(gfx++, 9, texScroll);
    gSPSegment(gfx++, 8, this->texture);
    gDPSetColor(gfx++, G_SETPRIMCOLOR, this->color.rgba);
    gDPSetColor(gfx++, G_SETENVCOLOR, this->color.rgba);
    gSPMatrix(gfx++, &this->projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPPerspNormalize(gfx++, this->normal);
    gSPMatrix(gfx++, &this->lookAt, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);

    if (scale != 1.0f) {
        guScale(&modelView[0], scale, scale, 1.0f);
        gSPMatrix(gfx++, &modelView[0], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }

    if (rot != 0.0f) {
        guRotate(&modelView[1], rot, 0.0f, 0.0f, 1.0f);
        gSPMatrix(gfx++, &modelView[1], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    }

    if ((tPos != 0.0f) || (tPos != 0.0f)) {
        guTranslate(&modelView[2], tPos, tPos, 0.0f);
        gSPMatrix(gfx++, &modelView[2], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    }
    gSPDisplayList(gfx++, sTransCircleDL);
    gDPPipeSync(gfx++);
    *gfxP = gfx;
}

s32 TransitionCircle_IsDone(void* thisx) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    return this->isDone;
}

void TransitionCircle_SetType(void* thisx, s32 type) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    if (type & TC_SET_PARAMS) {
        // SetType is called twice for circles, the actual direction value will be set on the second call.
        // The direction set here will be overwritten on that second call.
        this->direction = (type >> 5) & 1;

        this->colorType = (type >> 3) & 3;
        this->speedType = type & 1;
        this->appearanceType = (type >> 1) & 3;
    } else if (type == TRANS_INSTANCE_TYPE_FILL_OUT) {
        this->direction = TRANS_CIRCLE_DIR_OUT;
    } else {
        this->direction = TRANS_CIRCLE_DIR_IN;
    }
}

void TransitionCircle_SetColor(void* thisx, u32 color) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    /* Hier ABSICHTLICH ueber die Union: diese Datei schreibt die Farbe als
     * u32 und liest sie in _Draw ebenfalls als u32 (gDPSetColor mit
     * this->color.rgba). Ein solcher Rundlauf ist auf jeder Byteordnung in
     * sich stimmig -- das Wort geht RGBA8-gepackt hinein und genauso wieder
     * heraus. Eine Umrechnung waere hier nicht die Behebung eines Fehlers,
     * sondern seine Einfuehrung. Anders im Fade, der die EINZELFELDER liest;
     * dort ist COLOR_RGBA8_U32_SET noetig. */
    this->color.rgba = color;
}

void TransitionCircle_SetUnkColor(void* thisx, u32 color) {
    TransitionCircle* this = (TransitionCircle*)thisx;

    this->unkColor.rgba = color;
}
