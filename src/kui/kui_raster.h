// kui_raster.h: the shared software rasterizer behind kui::Canvas, built into both backends (satoru)
// every routine is a port of today's kernel drawing (Graphics, MDraw, WMMotion::SoftShadow) with the (satoru)
// screen bounds replaced by a clip box, so the kernel and ring 3 paint the same pixels and nothing (satoru)
// spills outside a window. coordinates here are absolute target pixels; kui::Canvas adds the origin. (satoru)
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace kui {
namespace raster {

struct Target {
    uint32_t* px;                 // 32bpp xrgb pixels, nullptr = draw nothing (satoru)
    int pitch;                    // row length in pixels (satoru)
    int w, h;                     // target size (satoru)
    int x0, y0, x1, y1;           // clip box, half open, already inside [0,w) x [0,h) (satoru)
};

// Graphics::BlendColors: src over dst at alpha, rounded /255 (satoru)
uint32_t BlendColors(uint32_t src, uint32_t dst, uint8_t alpha);
// MDraw::Mix, t 0..255 (satoru)
uint32_t Mix(uint32_t a, uint32_t b, int t);

// Graphics:: primitives (satoru)
void FillRect(Target& t, int x, int y, int w, int h, uint32_t c);
void FillRectAlpha(Target& t, int x, int y, int w, int h, uint8_t a, uint32_t c);
void FillRectRounded(Target& t, int x, int y, int w, int h, int r, uint32_t c);
void DrawPixel(Target& t, int x, int y, uint32_t c);                 // alpha aware, like Graphics::DrawPixel (satoru)
void BlendPixel(Target& t, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);   // Graphics::BlendPixel (satoru)
void Line(Target& t, int x0, int y0, int x1, int y1, uint32_t c, int thickness);
void CircleMid(Target& t, int cx, int cy, int r, uint32_t c, bool filled);   // Graphics::DrawCircle (satoru)

// MDraw:: anti-aliased primitives (satoru)
void RoundRect(Target& t, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha);
void RoundOutline(Target& t, int x, int y, int w, int h, int r, uint32_t c, int alpha);
void Shadow(Target& t, int x, int y, int w, int h, int r, int spread, int dy, int alpha);
// WMMotion::SoftShadow (satoru)
void SoftShadow(Target& t, float x, float y, float w, float h, float r, float blur, float dy, int alpha);

// images (satoru)
void Blit(Target& t, int x, int y, const uint32_t* src, int w, int h, int stride);
void BlitAlpha(Target& t, int x, int y, const uint32_t* src, int w, int h, int stride, int alpha);
void BlitPremul(Target& t, int x, int y, const uint32_t* src, int w, int h, int stride, int alpha);
void BlitScaled(Target& t, int dx, int dy, int dw, int dh, const uint32_t* src, int w, int h, int stride);
void BlitNearest(Target& t, int dx, int dy, int dw, int dh, const uint32_t* src, int w, int h, int stride);

// a glyph coverage bitmap at (x, y) in colour c, blended like FontTTF's BlitGlyphRow (satoru)
void Glyph(Target& t, int x, int y, const uint8_t* cov, int w, int h, uint32_t c);
// the vga 8x16 font at pxh, like FontTTF's DrawStringFallback; returns the advance (satoru)
int  Bitmap8x16(Target& t, int x, int y, float pxh, const char* s, int n, uint32_t c);
int  Bitmap8x16Width(float pxh, const char* s, int n);

// MDraw::AppTile's box-filter resample of a premultiplied square (satoru)
void AreaScale(const uint32_t* src, int n, uint32_t* dst, int s);

}  // namespace raster
}  // namespace kui

// end (satoru)
