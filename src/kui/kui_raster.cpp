// kui_raster.cpp: ports of the kernel's drawing code onto a clipped pixel target (satoru)
// sources: drivers/graphics.cpp (FillRect, FillRectRounded, FillRectAlpha, DrawPixel, BlendPixel, (satoru)
// lines, DrawCircle), ui/modern_draw.cpp (round_rect, RoundOutline, Shadow, Mix, area_scale), (satoru)
// ui/wm_motion.cpp (SoftShadow) and ui/font.cpp (glyph rows, the 8x16 fallback). the arithmetic is (satoru)
// kept exactly, only "inside the screen" became "inside the clip box". (satoru)
#include "kui_raster.h"
#include "kui.h"
#include "../ui/vga_font.h"

namespace kui {
namespace raster {

static inline bool in_clip(const Target& t, int x, int y) {
    return x >= t.x0 && x < t.x1 && y >= t.y0 && y < t.y1;
}

uint32_t BlendColors(uint32_t src, uint32_t dst, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    uint32_t a = alpha;
    uint32_t inv = 255u - a;
    uint32_t r = (((src >> 16) & 0xFFu) * a + ((dst >> 16) & 0xFFu) * inv + 127u) / 255u;
    uint32_t g = (((src >> 8)  & 0xFFu) * a + ((dst >> 8)  & 0xFFu) * inv + 127u) / 255u;
    uint32_t b = (( src        & 0xFFu) * a + ( dst        & 0xFFu) * inv + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// ---- Graphics:: (satoru)

void DrawPixel(Target& t, int x, int y, uint32_t c) {
    if (!t.px || !in_clip(t, x, y)) return;
    uint8_t a = (uint8_t)(c >> 24);
    if (a == 0) return;
    uint32_t* d = t.px + (size_t)y * t.pitch + x;
    if (a >= 0xF0) *d = c | 0xFF000000u;
    else *d = BlendColors(c, *d, a);
}

void BlendPixel(Target& t, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    uint32_t rgb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (a == 255) { DrawPixel(t, x, y, rgb); return; }
    if (!t.px || !in_clip(t, x, y)) return;
    uint32_t* d = t.px + (size_t)y * t.pitch + x;
    uint32_t src = ((uint32_t)a << 24) | (rgb & 0xFFFFFFu);
    *d = BlendColors(src, *d, a);
}

void FillRect(Target& t, int x, int y, int w, int h, uint32_t c) {
    if (!t.px) return;
    int r = x + w, b = y + h;
    if (x < t.x0) x = t.x0;
    if (y < t.y0) y = t.y0;
    if (r > t.x1) r = t.x1;
    if (b > t.y1) b = t.y1;
    w = r - x; h = b - y;
    if (w <= 0 || h <= 0) return;
    uint8_t a = (uint8_t)(c >> 24);
    if (a >= 0xF0) {
        uint32_t op = c | 0xFF000000u;
        for (int j = 0; j < h; j++) {
            uint32_t* row = t.px + (size_t)(y + j) * t.pitch + x;
            for (int i = 0; i < w; i++) row[i] = op;
        }
    } else if (a == 0) {
        return;
    } else {
        for (int j = 0; j < h; j++) {
            uint32_t* row = t.px + (size_t)(y + j) * t.pitch + x;
            for (int i = 0; i < w; i++) row[i] = BlendColors(c, row[i], a);
        }
    }
}

void FillRectAlpha(Target& t, int x, int y, int w, int h, uint8_t a, uint32_t c) {
    if (a == 0 || w <= 0 || h <= 0 || !t.px) return;
    if (a == 255) { FillRect(t, x, y, w, h, c | 0xFF000000u); return; }
    int r = x + w, b = y + h;
    if (x < t.x0) x = t.x0;
    if (y < t.y0) y = t.y0;
    if (r > t.x1) r = t.x1;
    if (b > t.y1) b = t.y1;
    w = r - x; h = b - y;
    if (w <= 0 || h <= 0) return;
    const uint32_t av = a, inv = 255u - av;
    const uint32_t cr = ((c >> 16) & 0xFFu) * av + 127u;
    const uint32_t cg = ((c >> 8)  & 0xFFu) * av + 127u;
    const uint32_t cb = ( c        & 0xFFu) * av + 127u;
    for (int j = 0; j < h; j++) {
        uint32_t* p = t.px + (size_t)(y + j) * t.pitch + x;
        for (int i = 0; i < w; i++) {
            uint32_t d = p[i];
            uint32_t rr = (cr + ((d >> 16) & 0xFFu) * inv) / 255u;
            uint32_t gg = (cg + ((d >> 8)  & 0xFFu) * inv) / 255u;
            uint32_t bb = (cb + ( d        & 0xFFu) * inv) / 255u;
            p[i] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        }
    }
}

void FillRectRounded(Target& t, int x, int y, int w, int h, int r, uint32_t c) {
    if (r <= 0 || r > h / 2 || r > w / 2) { FillRect(t, x, y, w, h, c); return; }
    FillRect(t, x + r, y, w - 2 * r, h, c);
    FillRect(t, x, y + r, r, h - 2 * r, c);
    FillRect(t, x + w - r, y + r, r, h - 2 * r, c);
    uint8_t a = (uint8_t)(c >> 24);
    uint32_t col = a >= 0xF0 ? (c | 0xFF000000u) : c;
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        int dy2 = dy * dy;
        for (int dx = 0; dx < r; dx++) {
            if (dx * dx + dy2 <= r2) {
                DrawPixel(t, x + r - 1 - dx, y + r - 1 - dy, col);
                DrawPixel(t, x + w - r + dx, y + r - 1 - dy, col);
                DrawPixel(t, x + r - 1 - dx, y + h - r + dy, col);
                DrawPixel(t, x + w - r + dx, y + h - r + dy, col);
            }
        }
    }
}

static void bresenham(Target& t, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (int guard = 0; guard < 1 << 20; guard++) {
        DrawPixel(t, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void Line(Target& t, int x0, int y0, int x1, int y1, uint32_t c, int thickness) {
    if (thickness <= 1) { bresenham(t, x0, y0, x1, y1, c); return; }
    for (int i = -thickness / 2; i <= thickness / 2; i++)
        for (int j = -thickness / 2; j <= thickness / 2; j++)
            bresenham(t, x0 + i, y0 + j, x1 + i, y1 + j, c);
}

void CircleMid(Target& t, int cx, int cy, int radius, uint32_t c, bool filled) {
    if (radius <= 0) { DrawPixel(t, cx, cy, c); return; }
    int x = 0, y = radius, d = 1 - radius;
    while (x <= y) {
        if (filled) {
            FillRect(t, cx - x, cy + y, 2 * x + 1, 1, c);
            FillRect(t, cx - x, cy - y, 2 * x + 1, 1, c);
            FillRect(t, cx - y, cy + x, 2 * y + 1, 1, c);
            FillRect(t, cx - y, cy - x, 2 * y + 1, 1, c);
        } else {
            DrawPixel(t, cx + x, cy + y, c); DrawPixel(t, cx - x, cy + y, c);
            DrawPixel(t, cx + x, cy - y, c); DrawPixel(t, cx - x, cy - y, c);
            DrawPixel(t, cx + y, cy + x, c); DrawPixel(t, cx - y, cy + x, c);
            DrawPixel(t, cx + y, cy - x, c); DrawPixel(t, cx - y, cy - x, c);
        }
        if (d < 0) d += 2 * x + 3;
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }
}

// ---- MDraw:: (satoru)

static inline uint32_t blend256(uint32_t src, uint32_t dst, int a256) {
    if (a256 >= 256) return src | 0xFF000000u;
    uint32_t rb = (((src & 0xFF00FFu) * a256 + (dst & 0xFF00FFu) * (256 - a256)) >> 8) & 0xFF00FFu;
    uint32_t g  = (((src & 0x00FF00u) * a256 + (dst & 0x00FF00u) * (256 - a256)) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

static inline uint32_t mix256(uint32_t a, uint32_t b, int t256) {
    uint32_t rb = (((a & 0xFF00FFu) * (256 - t256) + (b & 0xFF00FFu) * t256) >> 8) & 0xFF00FFu;
    uint32_t g  = (((a & 0x00FF00u) * (256 - t256) + (b & 0x00FF00u) * t256) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

uint32_t Mix(uint32_t a, uint32_t b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    return mix256(a, b, t + (t >> 7));
}

static const int MASK_MAX_R = 160;
static uint8_t* g_mask[MASK_MAX_R + 1];

static const uint8_t* corner_mask(int r) {
    if (r <= 0 || r > MASK_MAX_R) return nullptr;
    if (g_mask[r]) return g_mask[r];
    uint8_t* m = (uint8_t*)kui::Alloc((size_t)r * r);
    if (!m) return nullptr;
    float r2 = (float)r * r;
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int n = 0;
            for (int b = 0; b < 4; b++) {
                float dy = j + (b + 0.5f) * 0.25f - r;
                for (int a = 0; a < 4; a++) {
                    float dx = i + (a + 0.5f) * 0.25f - r;
                    if (dx * dx + dy * dy <= r2) n++;
                }
            }
            m[j * r + i] = (uint8_t)n;
        }
    g_mask[r] = m;
    return m;
}

static int corner_cov(int i, int j, int r) {
    if (r <= 0 || i < 0 || j < 0 || i >= r || j >= r) return 0;
    if (const uint8_t* m = corner_mask(r)) return m[j * r + i];
    int n = 0;
    float r2 = (float)r * r;
    for (int b = 0; b < 4; b++) {
        float dy = j + (b + 0.5f) * 0.25f - r;
        for (int a = 0; a < 4; a++) {
            float dx = i + (a + 0.5f) * 0.25f - r;
            if (dx * dx + dy * dy <= r2) n++;
        }
    }
    return n;
}

static inline void put(Target& t, int px, int py, uint32_t c, int a256) {
    if (a256 <= 0 || !in_clip(t, px, py)) return;
    uint32_t* d = t.px + (size_t)py * t.pitch + px;
    *d = blend256(c, *d, a256);
}

struct Hole { int x0, y0, x1, y1, r; bool on; };

static void round_rect(Target& t, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom,
                       int alpha, const Hole& hole) {
    if (w <= 0 || h <= 0 || alpha <= 0 || !t.px) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int a256 = alpha >= 255 ? 256 : alpha;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < t.y0 || py >= t.y1) continue;
        uint32_t c = (top == bottom || h < 2) ? top : mix256(top, bottom, (j * 256) / (h - 1));
        bool top_band = j < r, bot_band = j >= h - r;
        int x0 = x, x1 = x + w;
        if (top_band || bot_band) {
            int cj = top_band ? j : h - 1 - j;
            for (int i = 0; i < r; i++) {
                int cv = corner_cov(i, cj, r);
                if (!cv) continue;
                int a = (a256 * cv) >> 4;
                put(t, x + i, py, c, a);
                put(t, x + w - 1 - i, py, c, a);
            }
            x0 = x + r; x1 = x + w - r;
        }
        if (x0 < t.x0) x0 = t.x0;
        if (x1 > t.x1) x1 = t.x1;
        uint32_t* row = t.px + (size_t)py * t.pitch;
        int hx0 = x1, hx1 = x1;
        if (hole.on && py >= hole.y0 && py < hole.y1) {
            bool mid = py >= hole.y0 + hole.r && py < hole.y1 - hole.r;
            hx0 = mid ? hole.x0 : hole.x0 + hole.r;
            hx1 = mid ? hole.x1 : hole.x1 - hole.r;
            if (hx0 < x0) hx0 = x0;
            if (hx1 > x1) hx1 = x1;
            if (hx1 < hx0) hx1 = hx0;
        }
        for (int px = x0; px < hx0; px++) row[px] = blend256(c, row[px], a256);
        for (int px = hx1; px < x1; px++) row[px] = blend256(c, row[px], a256);
    }
}

void RoundRect(Target& t, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha) {
    Hole none = {0, 0, 0, 0, 0, false};
    round_rect(t, x, y, w, h, r, top, bottom, alpha, none);
}

void RoundOutline(Target& t, int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    if (w <= 2 || h <= 2 || alpha <= 0 || !t.px) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int a256 = alpha >= 255 ? 256 : alpha;
    for (int px = x + r; px < x + w - r; px++) {
        put(t, px, y, c, a256);
        put(t, px, y + h - 1, c, a256);
    }
    for (int py = y + r; py < y + h - r; py++) {
        put(t, x, py, c, a256);
        put(t, x + w - 1, py, c, a256);
    }
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int ring = corner_cov(i, j, r) - corner_cov(i - 1, j - 1, r - 1);
            if (ring <= 0) continue;
            int a = (a256 * ring) >> 4;
            put(t, x + i, y + j, c, a);
            put(t, x + w - 1 - i, y + j, c, a);
            put(t, x + i, y + h - 1 - j, c, a);
            put(t, x + w - 1 - i, y + h - 1 - j, c, a);
        }
    }
}

void Shadow(Target& t, int x, int y, int w, int h, int r, int spread, int dy, int alpha) {
    if (spread <= 0 || alpha <= 0 || !t.px) return;
    int per = alpha / spread;
    if (per < 1) per = 1;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    Hole core = {x, y + dy, x + w, y + dy + h, r, true};
    for (int k = spread; k >= 1; k--)
        round_rect(t, x - k, y - k + dy, w + 2 * k, h + 2 * k, r + k, 0xFF000000u, 0xFF000000u, per, core);
    int keep = 256;
    for (int k = 0; k < spread; k++) keep = keep * (256 - per) / 256;
    int a = 256 - keep;
    if (a <= 0) return;
    if (a > 255) a = 255;
    for (int j = 0; j < h; j++) {
        int py = y + dy + j;
        if (py < t.y0 || py >= t.y1) continue;
        bool mid = j >= r && j < h - r;
        int x0 = mid ? x : x + r, x1 = mid ? x + w : x + w - r;
        if (x0 < t.x0) x0 = t.x0;
        if (x1 > t.x1) x1 = t.x1;
        uint32_t* row = t.px + (size_t)py * t.pitch;
        for (int px = x0; px < x1; px++) row[px] = blend256(0xFF000000u, row[px], a);
    }
}

// ---- WMMotion::SoftShadow (satoru)

static inline uint32_t scale2(uint32_t a, uint32_t w) {
    uint32_t rb = (((a & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t ag = (((a >> 8) & 0xFF00FFu) * w) & 0xFF00FF00u;
    return rb | ag;
}
static inline int ifloor_(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
static inline float ksqrt(float x) { return x > 0.0f ? __builtin_sqrtf(x) : 0.0f; }

static uint8_t g_fall[257];
static bool g_fall_ok = false;
static void fall_init() {
    for (int i = 0; i <= 256; i++) {
        float u = (float)i / 256.0f;
        float s = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
        g_fall[i] = (uint8_t)(255.0f * (1.0f - s) + 0.5f);
    }
    g_fall_ok = true;
}

void SoftShadow(Target& t, float x, float y, float w, float h, float r, float blur, float dy, int alpha) {
    if (alpha <= 0 || w < 1.0f || h < 1.0f || !t.px) return;
    if (!g_fall_ok) fall_init();
    if (alpha > 255) alpha = 255;
    if (blur < 1.0f) blur = 1.0f;
    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;
    const float cx = x + w * 0.5f, cy = y + dy + h * 0.5f;
    const float hx = w * 0.5f - r, hy = h * 0.5f - r;
    int X0 = ifloor_(x - blur), X1 = ifloor_(x + w + blur) + 1;
    int Y0 = ifloor_(y + dy - blur), Y1 = ifloor_(y + dy + h + blur) + 1;
    if (X0 < t.x0) X0 = t.x0;
    if (Y0 < t.y0) Y0 = t.y0;
    if (X1 > t.x1) X1 = t.x1;
    if (Y1 > t.y1) Y1 = t.y1;
    if (X0 >= X1 || Y0 >= Y1) return;
    const float inv = 128.0f / blur;
    int mx0 = (int)(cx - hx - 0.5f) + 1, mx1 = (int)(cx + hx - 0.5f) + 1;
    if (mx0 < X0) mx0 = X0;
    if (mx1 > X1) mx1 = X1;
    if (mx1 < mx0) mx1 = mx0;
    int hx0 = (int)x, hx1 = (int)(x + w), hy0 = (int)y, hy1 = (int)(y + h), hr = (int)r + 1;
    for (int py = Y0; py < Y1; py++) {
        float qy = (float)py + 0.5f - cy;
        if (qy < 0.0f) qy = -qy;
        qy -= hy;
        uint32_t* row = t.px + (size_t)py * t.pitch;
        int sk0 = X1, sk1 = X1;
        if (py >= hy0 && py < hy1) {
            bool mid = py >= hy0 + hr && py < hy1 - hr;
            sk0 = mid ? hx0 : hx0 + hr;
            sk1 = mid ? hx1 : hx1 - hr;
            if (sk0 < X0) sk0 = X0;
            if (sk1 > X1) sk1 = X1;
            if (sk1 < sk0) sk1 = sk0;
        }
        uint32_t ma = 0;
        bool mconst = qy >= 0.0f;
        if (mconst) {
            int li = (int)((qy - r) * inv) + 128;
            if (li < 0) li = 0;
            ma = li >= 256 ? 0 : ((uint32_t)g_fall[li] * (uint32_t)alpha) >> 8;
        }
        for (int px = X0; px < X1; px++) {
            if (px >= sk0 && px < sk1) { px = sk1 - 1; continue; }
            if (mconst && px >= mx0 && px < mx1) {
                int e = mx1;
                if (sk0 > px && sk0 < e) e = sk0;
                if (ma) for (; px < e; px++) row[px] = scale2(row[px], 256 - ma) | 0xFF000000u;
                else px = e;
                px--;
                continue;
            }
            if (py >= hy0 && py < hy1 && px >= hx0 && px < hx1) {
                float ccx = px < hx0 + hr ? x + r : x + w - r;
                float ccy = py < hy0 + hr ? y + r : y + h - r;
                float ex = (float)px + 0.5f - ccx, ey = (float)py + 0.5f - ccy;
                bool in_x = (px < hx0 + hr && ex <= 0.0f) || (px >= hx1 - hr && ex >= 0.0f);
                bool in_y = (py < hy0 + hr && ey <= 0.0f) || (py >= hy1 - hr && ey >= 0.0f);
                if (!(in_x && in_y) || ex * ex + ey * ey <= (r - 0.5f) * (r - 0.5f)) continue;
            }
            float qx = (float)px + 0.5f - cx;
            if (qx < 0.0f) qx = -qx;
            qx -= hx;
            float d;
            if (qx > 0.0f && qy > 0.0f) d = ksqrt(qx * qx + qy * qy) - r;
            else d = (qx > qy ? qx : qy) - r;
            int li = (int)(d * inv) + 128;
            if (li >= 256) continue;
            if (li < 0) li = 0;
            uint32_t a = ((uint32_t)g_fall[li] * (uint32_t)alpha) >> 8;
            if (!a) continue;
            row[px] = scale2(row[px], 256 - a) | 0xFF000000u;
        }
    }
}

// ---- images (satoru)

void Blit(Target& t, int x, int y, const uint32_t* src, int w, int h, int stride) {
    if (!t.px || !src) return;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < t.y0 || py >= t.y1) continue;
        int i0 = t.x0 - x > 0 ? t.x0 - x : 0, i1 = t.x1 - x < w ? t.x1 - x : w;
        uint32_t* d = t.px + (size_t)py * t.pitch + x;
        const uint32_t* s = src + (size_t)j * stride;
        for (int i = i0; i < i1; i++) d[i] = s[i] | 0xFF000000u;
    }
}

void BlitAlpha(Target& t, int x, int y, const uint32_t* src, int w, int h, int stride, int alpha) {
    if (!t.px || !src || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < t.y0 || py >= t.y1) continue;
        int i0 = t.x0 - x > 0 ? t.x0 - x : 0, i1 = t.x1 - x < w ? t.x1 - x : w;
        uint32_t* d = t.px + (size_t)py * t.pitch + x;
        const uint32_t* s = src + (size_t)j * stride;
        for (int i = i0; i < i1; i++) {
            uint32_t a = ((s[i] >> 24) * (uint32_t)alpha + 127u) / 255u;
            if (a) d[i] = BlendColors(s[i] | 0xFF000000u, d[i], (uint8_t)a);
        }
    }
}

void BlitPremul(Target& t, int x, int y, const uint32_t* px, int w, int h, int stride, int alpha) {
    if (!t.px || !px || alpha <= 0) return;
    int ga = alpha >= 255 ? 256 : alpha;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < t.y0 || py >= t.y1) continue;
        uint32_t* drow = t.px + (size_t)py * t.pitch;
        const uint32_t* srow = px + (size_t)j * stride;
        for (int i = 0; i < w; i++) {
            int qx = x + i;
            if (qx < t.x0 || qx >= t.x1) continue;
            uint32_t p = srow[i];
            uint32_t a = p >> 24;
            if (!a) continue;
            if (ga < 256) {
                p = (((p & 0xFF00FFu) * ga >> 8) & 0xFF00FFu) | (((p >> 8 & 0xFF00FFu) * ga) & 0xFF00FF00u);
                a = p >> 24;
            }
            uint32_t d = drow[qx], inv = 255 - a;
            uint32_t rb = (p & 0xFF00FFu) + ((((d & 0xFF00FFu) * inv) >> 8) & 0xFF00FFu);
            uint32_t g  = (p & 0x00FF00u) + ((((d & 0x00FF00u) * inv) >> 8) & 0x00FF00u);
            drow[qx] = 0xFF000000u | (rb & 0xFF00FFu) | (g & 0x00FF00u);
        }
    }
}

void BlitScaled(Target& t, int dx, int dy, int dw, int dh, const uint32_t* src, int w, int h, int stride) {
    if (!t.px || !src || dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return;
    uint32_t sx_step = (uint32_t)(((uint64_t)w << 16) / (uint32_t)dw);
    uint32_t sy_step = (uint32_t)(((uint64_t)h << 16) / (uint32_t)dh);
    for (int j = 0; j < dh; j++) {
        int py = dy + j;
        if (py < t.y0 || py >= t.y1) continue;
        uint32_t fy = (uint32_t)j * sy_step + (sy_step >> 1);
        int y0 = (int)(fy >> 16); if (y0 >= h) y0 = h - 1;
        int y1 = y0 + 1 < h ? y0 + 1 : y0;
        uint32_t wy = (fy >> 8) & 0xFF;
        const uint32_t* r0 = src + (size_t)y0 * stride;
        const uint32_t* r1 = src + (size_t)y1 * stride;
        uint32_t* d = t.px + (size_t)py * t.pitch;
        for (int i = 0; i < dw; i++) {
            int qx = dx + i;
            if (qx < t.x0 || qx >= t.x1) continue;
            uint32_t fx = (uint32_t)i * sx_step + (sx_step >> 1);
            int x0 = (int)(fx >> 16); if (x0 >= w) x0 = w - 1;
            int x1 = x0 + 1 < w ? x0 + 1 : x0;
            uint32_t wx = (fx >> 8) & 0xFF;
            uint32_t top = mix256(r0[x0], r0[x1], (int)wx);
            uint32_t bot = mix256(r1[x0], r1[x1], (int)wx);
            d[qx] = mix256(top, bot, (int)wy);
        }
    }
}

void BlitNearest(Target& t, int dx, int dy, int dw, int dh, const uint32_t* src, int w, int h, int stride) {
    if (!t.px || !src || dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return;
    uint32_t sx_step = (uint32_t)(((uint64_t)w << 16) / (uint32_t)dw);
    uint32_t sy_step = (uint32_t)(((uint64_t)h << 16) / (uint32_t)dh);
    int i0 = t.x0 - dx > 0 ? t.x0 - dx : 0, i1 = t.x1 - dx < dw ? t.x1 - dx : dw;
    for (int j = 0; j < dh; j++) {
        int py = dy + j;
        if (py < t.y0 || py >= t.y1) continue;
        int sy = (int)(((uint32_t)j * sy_step) >> 16); if (sy >= h) sy = h - 1;
        const uint32_t* r = src + (size_t)sy * stride;
        uint32_t* d = t.px + (size_t)py * t.pitch + dx;
        uint32_t fx = (uint32_t)i0 * sx_step;
        for (int i = i0; i < i1; i++, fx += sx_step) {
            int sx = (int)(fx >> 16); if (sx >= w) sx = w - 1;
            d[i] = r[sx] | 0xFF000000u;
        }
    }
}

// ---- text helpers (satoru)

void Glyph(Target& t, int x, int y, const uint8_t* cov, int w, int h, uint32_t c) {
    if (!t.px || !cov) return;
    uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < t.y0 || py >= t.y1) continue;
        const uint8_t* row = cov + (size_t)j * w;
        for (int i = 0; i < w; i++) {
            uint8_t a = row[i];
            if (a) BlendPixel(t, x + i, py, r, g, b, a);
        }
    }
}

int Bitmap8x16Width(float pxh, const char* s, int n) {
    int scale = (int)(pxh / 16.0f + 0.5f);
    if (scale < 1) scale = 1;
    int k = 0;
    while (s && s[k] && k < n) k++;
    return k * 8 * scale;
}

int Bitmap8x16(Target& t, int x, int y, float pxh, const char* s, int n, uint32_t c) {
    if (!s) return 0;
    int scale = (int)(pxh / 16.0f + 0.5f);
    if (scale < 1) scale = 1;
    int ox = x;
    uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    for (int k = 0; s[k] && k < n; k++) {
        const unsigned char* gl = vga_font_8x16[(unsigned char)s[k]];
        for (int row = 0; row < 16; row++) {
            unsigned char bits = gl[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (0x80 >> col))) continue;
                if (scale == 1) BlendPixel(t, x + col, y + row, r, g, b, 255);
                else FillRect(t, x + col * scale, y + row * scale, scale, scale, c);
            }
        }
        x += 8 * scale;
    }
    return x - ox;
}

void AreaScale(const uint32_t* src, int n, uint32_t* dst, int s) {
    uint32_t step = (uint32_t)(((uint64_t)n << 16) / (uint32_t)s);
    for (int dy = 0; dy < s; dy++) {
        uint32_t y0 = (uint32_t)dy * step, y1 = y0 + step;
        for (int dx = 0; dx < s; dx++) {
            uint32_t x0 = (uint32_t)dx * step, x1 = x0 + step;
            uint64_t acc[4] = { 0, 0, 0, 0 };
            for (uint32_t sy = y0 >> 16; sy < ((y1 + 0xFFFF) >> 16) && sy < (uint32_t)n; sy++) {
                uint32_t ty0 = sy << 16 > y0 ? sy << 16 : y0;
                uint32_t ty1 = (sy + 1) << 16 < y1 ? (sy + 1) << 16 : y1;
                uint32_t wy = ty1 - ty0;
                const uint32_t* row = src + (size_t)sy * n;
                for (uint32_t sx = x0 >> 16; sx < ((x1 + 0xFFFF) >> 16) && sx < (uint32_t)n; sx++) {
                    uint32_t tx0 = sx << 16 > x0 ? sx << 16 : x0;
                    uint32_t tx1 = (sx + 1) << 16 < x1 ? (sx + 1) << 16 : x1;
                    uint64_t w = ((uint64_t)wy * (tx1 - tx0)) >> 16;
                    uint32_t p = row[sx];
                    acc[0] += (uint64_t)(p >> 24) * w;
                    acc[1] += (uint64_t)((p >> 16) & 0xFF) * w;
                    acc[2] += (uint64_t)((p >> 8) & 0xFF) * w;
                    acc[3] += (uint64_t)(p & 0xFF) * w;
                }
            }
            uint64_t area = ((uint64_t)step * step) >> 16;
            if (!area) area = 1;
            uint32_t a = (uint32_t)(acc[0] / area), r = (uint32_t)(acc[1] / area);
            uint32_t g = (uint32_t)(acc[2] / area), b = (uint32_t)(acc[3] / area);
            if (a > 255) a = 255;
            if (r > a) r = a;
            if (g > a) g = a;
            if (b > a) b = a;
            dst[(size_t)dy * s + dx] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace raster
}  // namespace kui

// end (satoru)
