#include "../kernel/heap.h"
#include "../kernel/types.h"

#define STBTT_malloc(x,u)  ((void)(u), KernelHeap::Alloc(x))
#define STBTT_free(x,u)    ((void)(u), KernelHeap::Free(x))
#define STBTT_assert(x)

#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STBTT_strlen strlen

static float my_sqrt(float n) {
    if (n < 0) return 0;
    float x = n;
    float y = 1;
    float e = 0.000001f;
    while(x - y > e) {
        x = (x + y) / 2;
        y = n / x;
    }
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

static stbtt_fontinfo g_font;
static int g_font_ok = 0;

extern "C" int tt_init_from_memory(const unsigned char* data, int size){
    (void)size;
    g_font_ok = stbtt_InitFont(&g_font, data, stbtt_GetFontOffsetForIndex(data, 0));
    return g_font_ok;
}

extern "C" unsigned char* tt_raster(unsigned int codepoint, float pixel_height, int* w, int* h, int* xoff, int* yoff, int* xadvance){
    if(!g_font_ok){ *w=0; *h=0; *xoff=0; *yoff=0; *xadvance=0; return 0; }
    float scale = stbtt_ScaleForPixelHeight(&g_font, pixel_height);
    int advance, lsb; stbtt_GetCodepointHMetrics(&g_font, codepoint, &advance, &lsb);
    int ax, ay, bx, by; stbtt_GetCodepointBitmapBox(&g_font, codepoint, scale, scale, &ax, &ay, &bx, &by);
    int gw = bx - ax; int gh = by - ay;
    unsigned char* bmp = stbtt_GetCodepointBitmap(&g_font, 0, scale, codepoint, &gw, &gh, 0, 0);
    *w = gw; *h = gh; *xoff = ax; *yoff = ay; *xadvance = (int)(advance * scale);
    return bmp;
}

extern "C" void tt_free_bitmap(unsigned char* p){ if(p) stbtt_FreeBitmap(p, 0); }

extern "C" int tt_get_kern(unsigned int c1, unsigned int c2) {
    if (!g_font_ok) return 0;
    return stbtt_GetCodepointKernAdvance(&g_font, c1, c2);
}
