#ifndef COLOR_H
#define COLOR_H

#include "ultra64/ultratypes.h"

typedef struct Color_RGB8 {
    u8 r, g, b;
} Color_RGB8;

typedef struct Color_RGBA8 {
    u8 r, g, b, a;
} Color_RGBA8;

// only use when necessary for alignment purposes
typedef union Color_RGBA8_u32 {
    struct {
        u8 r, g, b, a;
    };
    u32 rgba;
} Color_RGBA8_u32;

/* Eine mit RGBA8() gepackte Farbe byteordnungssicher zuweisen.
 *
 * DIE UNION OBEN IST EINE FALLE. RGBA8(r,g,b,a) legt Rot ins HOECHSTE Byte,
 * waehrend `struct { u8 r, g, b, a; }` bei r anfaengt. Auf der big-endian N64
 * liegt das erste Feld an der hoechstwertigen Stelle, also passt beides
 * zufaellig zusammen -- `x.rgba = RGBA8(...)` ist dort richtig. Auf einem
 * little-endian Ziel liest `r` das NIEDRIGSTE Byte, also das Alpha, und die
 * ganze Farbe steht rueckwaerts.
 *
 * Aufgefallen an der weissen Szenenblende: RGBA8(160,160,160,255) ist
 * 0xA0A0A0FF und kam als r=255, g=160, b=160 an -- ein rosa Bildschirm. Der
 * schwarze Uebergang blieb unauffaellig, weil RGBA8(0,0,0,0) in jeder
 * Byteordnung null ist; die Farbe, die den Fehler zeigt, muss erst einmal
 * vorkommen.
 *
 * DIE REGEL LAUTET NICHT "immer umrechnen", sondern "nicht mischen". Wer die
 * Farbe als u32 hineinschreibt und auch wieder als u32 herausliest, ist auf
 * jeder Byteordnung richtig -- das Wort geht gepackt hinein und genauso wieder
 * heraus; dort waere eine Umrechnung die EINFUEHRUNG eines Fehlers, nicht
 * seine Behebung. Genau das gilt fuer z_fbdemo_circle/_triforce/_wipe1, die
 * ueber gDPSetColor mit .rgba zeichnen.
 *
 * Dieses Makro ist fuer den anderen Fall: wenn eine mit RGBA8() gepackte u32
 * hineingeht und spaeter die EINZELFELDER gelesen werden (z_fbdemo_fade liest
 * color->r/g/b/a). Nur diese Mischung ist byteordnungsabhaengig. */
#if TARGET_PSP
#define COLOR_RGBA8_U32_SET(dst, u)          \
    do {                                     \
        u32 color_ = (u32)(u);               \
        (dst).r = (color_ >> 24) & 0xFF;     \
        (dst).g = (color_ >> 16) & 0xFF;     \
        (dst).b = (color_ >> 8) & 0xFF;      \
        (dst).a = (color_ >> 0) & 0xFF;      \
    } while (0)
#else
#define COLOR_RGBA8_U32_SET(dst, u) ((dst).rgba = (u32)(u))
#endif

typedef struct Color_RGBAf {
    f32 r, g, b, a;
} Color_RGBAf;

typedef union Color_RGBA16 {
    struct {
        u16 r : 5;
        u16 g : 5;
        u16 b : 5;
        u16 a : 1;
    };
    u16 rgba;
} Color_RGBA16;

#define RGBA8(r, g, b, a) ((((r) & 0xFF) << 24) | (((g) & 0xFF) << 16) | (((b) & 0xFF) << 8) | (((a) & 0xFF) << 0))

#endif
