#include "../kernel/heap.h"
#include "../kernel/types.h"

#define STBTT_malloc(x,u)  ((void)(u), KernelHeap::Alloc(x))
#define STBTT_free(x,u)    ((void)(u), KernelHeap::Free(x))
#define STBTT_assert(x)

#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STBTT_strlen strlen

// bounded sqrt: exponent-halving first guess, then fixed newton steps. the old
// loop ran until two floats agreed to 1e-6, which rounding can make impossible;
// truetype outlines never reached it, but cff (.otf) cubic tessellation calls
// sqrt for every curve and hung the first text draw. (satoru)
static float my_sqrt(float n) {
    if (!(n > 0.0f)) return 0.0f;
    union { float f; uint32_t u; } v;
    v.f = n;
    v.u = 0x1fbd1df5u + (v.u >> 1);
    float x = v.f;
    for (int i = 0; i < 4; i++) x = 0.5f * (x + n / x);
    return x;
}
static float my_pow(float x, float y) { (void)y; return x; } // stub

#define STBTT_ifloor(x)   ((int)(x))
#define STBTT_iceil(x)    ((int)((x)+0.9999f))
#define STBTT_sqrt(x)     my_sqrt(x)
#define STBTT_pow(x,y)    my_pow(x,y)
#define STBTT_fmod(x,y)   ((x) - (int)((x)/(y)) * (y))
#define STBTT_cos(x)      0.0f
#define STBTT_acos(x)     0.0f
#define STBTT_fabs(x)     ((x)<0?-(x):(x))

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// up to three faces (0 = regular, 1 = semibold, 2 = monospace). advances and kerning come
// back in 26.6 fixed point so text can be laid out at sub-pixel precision
// instead of truncating every advance to a whole pixel. (satoru)
static stbtt_fontinfo g_font[3];
static int g_font_ok[3] = { 0, 0, 0 };

static inline int face_ok(int face) { return face >= 0 && face < 3 && g_font_ok[face]; }

extern "C" int tt_init_from_memory(const unsigned char* data, int size){
    (void)size;
    g_font_ok[0] = stbtt_InitFont(&g_font[0], data, stbtt_GetFontOffsetForIndex(data, 0));
    return g_font_ok[0];
}

extern "C" int tt_init_face(int face, const unsigned char* data, int size){
    (void)size;
    if (face < 0 || face >= 3) return 0;
    g_font_ok[face] = stbtt_InitFont(&g_font[face], data, stbtt_GetFontOffsetForIndex(data, 0));
    return g_font_ok[face];
}

extern "C" unsigned char* tt_raster_face(int face, unsigned int codepoint, float pixel_height,
                                         int* w, int* h, int* xoff, int* yoff, int* xadvance_26_6){
    if (!face_ok(face)) face = 0;
    if (!face_ok(face)) { *w=0; *h=0; *xoff=0; *yoff=0; *xadvance_26_6=0; return 0; }
    stbtt_fontinfo* f = &g_font[face];
    float scale = stbtt_ScaleForPixelHeight(f, pixel_height);
    int advance, lsb; stbtt_GetCodepointHMetrics(f, codepoint, &advance, &lsb);
    int ax, ay, bx, by; stbtt_GetCodepointBitmapBox(f, codepoint, scale, scale, &ax, &ay, &bx, &by);
    int gw = bx - ax; int gh = by - ay;
    unsigned char* bmp = stbtt_GetCodepointBitmap(f, 0, scale, codepoint, &gw, &gh, 0, 0);
    *w = gw; *h = gh; *xoff = ax; *yoff = ay;
    *xadvance_26_6 = (int)(advance * scale * 64.0f + 0.5f);
    return bmp;
}

extern "C" unsigned char* tt_raster(unsigned int codepoint, float pixel_height, int* w, int* h, int* xoff, int* yoff, int* xadvance){
    int adv = 0;
    unsigned char* bmp = tt_raster_face(0, codepoint, pixel_height, w, h, xoff, yoff, &adv);
    *xadvance = (adv + 32) >> 6;
    return bmp;
}

extern "C" void tt_free_bitmap(unsigned char* p){ if(p) stbtt_FreeBitmap(p, 0); }

extern "C" int tt_get_kern(unsigned int c1, unsigned int c2) {
    if (!g_font_ok[0]) return 0;
    return stbtt_GetCodepointKernAdvance(&g_font[0], c1, c2);
}

// kerning between two codepoints at pixel_height, 26.6 fixed point.
extern "C" int tt_kern_face(int face, unsigned int c1, unsigned int c2, float pixel_height) {
    if (!face_ok(face)) face = 0;
    if (!face_ok(face)) return 0;
    int k = stbtt_GetCodepointKernAdvance(&g_font[face], c1, c2);
    if (!k) return 0;
    float scale = stbtt_ScaleForPixelHeight(&g_font[face], pixel_height);
    float v = k * scale * 64.0f;
    return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

// ascent above the baseline at pixel_height, in whole pixels.
extern "C" int tt_ascent_face(int face, float pixel_height) {
    if (!face_ok(face)) face = 0;
    if (!face_ok(face)) return (int)(pixel_height * 0.8f);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font[face], &asc, &desc, &gap);
    return (int)(asc * stbtt_ScaleForPixelHeight(&g_font[face], pixel_height) + 0.5f);
}

// true when the face has an outline for codepoint (glyph index != 0). a space
// has a glyph but no bitmap, so "no bitmap" alone does not mean "missing".
extern "C" int tt_has_glyph(int face, unsigned int codepoint) {
    if (!face_ok(face)) face = 0;
    if (!face_ok(face)) return 0;
    return stbtt_FindGlyphIndex(&g_font[face], (int)codepoint) != 0;
}
