#include "modern_draw.h"
#include "app_icons.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"
#include "style.h"

// the modern icon artwork: 128x128 premultiplied argb bitmaps, one per
// AppIcons::Id (Makefile objcopy of assets/icons/modern_icons.bin, built from
// the svg sources by tools/icons). (satoru)
extern "C" {
    extern const uint8_t _binary_modern_icons_bin_start[] __attribute__((weak));
    extern const uint8_t _binary_modern_icons_bin_end[]   __attribute__((weak));
}

namespace MDraw {

static inline bool target(uint32_t*& buf, int& pitch, int& sw, int& sh) {
    if (Graphics::GetBpp() != 32 || !Graphics::GetBuffer()) return false;
    buf = (uint32_t*)Graphics::GetBuffer();
    pitch = (int)(Graphics::GetPitch() / 4);
    sw = Graphics::GetWidth();
    sh = Graphics::GetHeight();
    return true;
}

static inline uint32_t blend(uint32_t src, uint32_t dst, int a256) {
    if (a256 >= 256) return src | 0xFF000000u;
    uint32_t rb = (((src & 0xFF00FFu) * a256 + (dst & 0xFF00FFu) * (256 - a256)) >> 8) & 0xFF00FFu;
    uint32_t g  = (((src & 0x00FF00u) * a256 + (dst & 0x00FF00u) * (256 - a256)) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

static inline uint32_t mix(uint32_t a, uint32_t b, int t256) {
    uint32_t rb = (((a & 0xFF00FFu) * (256 - t256) + (b & 0xFF00FFu) * t256) >> 8) & 0xFF00FFu;
    uint32_t g  = (((a & 0x00FF00u) * (256 - t256) + (b & 0x00FF00u) * t256) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

// coverage (0..16) of corner-box cell (i,j) of a radius-r rounded corner,
// 4x4 supersampled. cell (0,0) is the outermost pixel of the top-left corner;
// the other three corners are mirror images, since the sample offsets are
// symmetric. masks are built once per radius and kept, so drawing a corner is
// a table lookup rather than 16 float tests per pixel per frame.
static const int MASK_MAX_R = 160;
static uint8_t* g_mask[MASK_MAX_R + 1];

static const uint8_t* corner_mask(int r) {
    if (r <= 0 || r > MASK_MAX_R) return nullptr;
    if (g_mask[r]) return g_mask[r];
    uint8_t* m = (uint8_t*)KernelHeap::Alloc((size_t)r * r);
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

// coverage of cell (i,j) of a radius-r corner, 0..16. falls back to sampling
// when the radius is past the cache.
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

static void put(uint32_t* buf, int pitch, int sw, int sh, int px, int py, uint32_t c, int a256) {
    if ((unsigned)px >= (unsigned)sw || (unsigned)py >= (unsigned)sh || a256 <= 0) return;
    uint32_t* d = buf + (size_t)py * pitch + px;
    *d = blend(c, *d, a256);
}

// a rounded rect whose fully covered inside is a cross: the full-width rows
// between the corner bands, and the columns between the corners.
struct Hole { int x0, y0, x1, y1, r; bool on; };

static void round_rect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom,
                       int alpha, const Hole& hole) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    uint32_t* buf; int pitch, sw, sh;
    if (!target(buf, pitch, sw, sh)) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int a256 = alpha >= 255 ? 256 : alpha;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)sh) continue;
        uint32_t c = (top == bottom || h < 2) ? top : mix(top, bottom, (j * 256) / (h - 1));
        bool top_band = j < r, bot_band = j >= h - r;
        int x0 = x, x1 = x + w;
        if (top_band || bot_band) {
            int cj = top_band ? j : h - 1 - j;
            for (int i = 0; i < r; i++) {
                int cv = corner_cov(i, cj, r);
                if (!cv) continue;
                int a = (a256 * cv) >> 4;
                put(buf, pitch, sw, sh, x + i, py, c, a);
                put(buf, pitch, sw, sh, x + w - 1 - i, py, c, a);
            }
            x0 = x + r; x1 = x + w - r;
        }
        if (x0 < 0) x0 = 0;
        if (x1 > sw) x1 = sw;
        uint32_t* row = buf + (size_t)py * pitch;
        int hx0 = x1, hx1 = x1;   // span skipped on this row, if any
        if (hole.on && py >= hole.y0 && py < hole.y1) {
            bool mid = py >= hole.y0 + hole.r && py < hole.y1 - hole.r;
            hx0 = mid ? hole.x0 : hole.x0 + hole.r;
            hx1 = mid ? hole.x1 : hole.x1 - hole.r;
            if (hx0 < x0) hx0 = x0;
            if (hx1 > x1) hx1 = x1;
            if (hx1 < hx0) hx1 = hx0;
        }
        for (int px = x0; px < hx0; px++) row[px] = blend(c, row[px], a256);
        for (int px = hx1; px < x1; px++) row[px] = blend(c, row[px], a256);
    }
}

void RoundRect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha) {
    Hole none = {0, 0, 0, 0, 0, false};
    round_rect(x, y, w, h, r, top, bottom, alpha, none);
}

void RoundOutline(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    if (w <= 2 || h <= 2 || alpha <= 0) return;
    uint32_t* buf; int pitch, sw, sh;
    if (!target(buf, pitch, sw, sh)) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int a256 = alpha >= 255 ? 256 : alpha;
    // straight edges
    for (int px = x + r; px < x + w - r; px++) {
        put(buf, pitch, sw, sh, px, y, c, a256);
        put(buf, pitch, sw, sh, px, y + h - 1, c, a256);
    }
    for (int py = y + r; py < y + h - r; py++) {
        put(buf, pitch, sw, sh, x, py, c, a256);
        put(buf, pitch, sw, sh, x + w - 1, py, c, a256);
    }
    // corners: coverage of the outer arc minus the arc one pixel in
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int ring_tl = corner_cov(i, j, r) - corner_cov(i - 1, j - 1, r - 1);
            if (ring_tl <= 0) continue;
            int a = (a256 * ring_tl) >> 4;
            put(buf, pitch, sw, sh, x + i, y + j, c, a);
            put(buf, pitch, sw, sh, x + w - 1 - i, y + j, c, a);
            put(buf, pitch, sw, sh, x + i, y + h - 1 - j, c, a);
            put(buf, pitch, sw, sh, x + w - 1 - i, y + h - 1 - j, c, a);
        }
    }
}

void Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha) {
    if (spread <= 0 || alpha <= 0) return;
    // stacked rounded rects, each a little larger and fainter: the overlaps add
    // up to a soft falloff that is darkest under the shape. every layer covers
    // the shape's own inside, so each layer skips it and the inside is filled
    // once at the alpha all the layers would have stacked to. that keeps the
    // cost to the falloff band instead of `spread` full fills of the shape.
    int per = alpha / spread;
    if (per < 1) per = 1;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    Hole core = {x, y + dy, x + w, y + dy + h, r, true};
    for (int k = spread; k >= 1; k--)
        round_rect(x - k, y - k + dy, w + 2 * k, h + 2 * k, r + k, 0xFF000000u, 0xFF000000u, per, core);
    // 1 - (1 - per)^spread, in 0..256
    int keep = 256;
    for (int k = 0; k < spread; k++) keep = keep * (256 - per) / 256;
    int a = 256 - keep;
    if (a <= 0) return;
    if (a > 255) a = 255;
    uint32_t* buf; int pitch, sw, sh;
    if (!target(buf, pitch, sw, sh)) return;
    // the cross-shaped inside the layers skipped; the corner cells were not
    // skipped, so the layers already drew them.
    for (int j = 0; j < h; j++) {
        int py = y + dy + j;
        if ((unsigned)py >= (unsigned)sh) continue;
        bool mid = j >= r && j < h - r;
        int x0 = mid ? x : x + r, x1 = mid ? x + w : x + w - r;
        if (x0 < 0) x0 = 0;
        if (x1 > sw) x1 = sw;
        uint32_t* row = buf + (size_t)py * pitch;
        for (int px = x0; px < x1; px++) row[px] = blend(0xFF000000u, row[px], a);
    }
}

void Circle(int cx, int cy, int radius, uint32_t c, int alpha) {
    RoundRect(cx - radius, cy - radius, radius * 2, radius * 2, radius, c, alpha);
}

uint32_t Mix(uint32_t a, uint32_t b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    return mix(a, b, t + (t >> 7));
}

struct TilePalette { uint32_t top, bottom; };

static TilePalette palette(int id) {
    switch (id) {
        // matches the gradients in assets/icons/*.svg
        case AppIcons::TERMINAL:   return { 0xFF3A3E4A, 0xFF15171D };
        case AppIcons::FILES:      return { 0xFF62C2FF, 0xFF2667F0 };
        case AppIcons::CALCULATOR: return { 0xFFFFB547, 0xFFFF6E2C };
        case AppIcons::EDITOR:     return { 0xFFFFFFFF, 0xFFE9ECF2 };
        case AppIcons::SETTINGS:   return { 0xFFA9B0BE, 0xFF5B6273 };
        case AppIcons::BROWSER:    return { 0xFF5BD3FF, 0xFF2A72E8 };
        case AppIcons::TASKS:      return { 0xFF2A2F3D, 0xFF10131A };
        case AppIcons::MEDIA:      return { 0xFFFF7096, 0xFFE02A62 };
        case AppIcons::HOME:       return { 0xFF86B4FF, 0xFF4B5CF2 };
        case AppIcons::FIREFOX:    return { 0xFFFFA25A, 0xFF8B4FE6 };
        default:                   return { 0xFF9AA4BA, 0xFF566078 };
    }
}

uint32_t AppTileColor(int id) {
    if (UIStyle::MinimalIcons()) return 0xFF202128;   // the minimal set's glass
    TilePalette p = palette(id);
    return mix(p.top, p.bottom, 128);
}

// ── modern icon artwork ──────────────────────────────────────────────────
static const uint32_t* icon_src(int id, int& n) {
    const uint8_t* b = _binary_modern_icons_bin_start;
    n = 0;
    if (!b || !_binary_modern_icons_bin_end) return nullptr;
    if (b[0] != 'K' || b[1] != 'I' || b[2] != 'C' || b[3] != 'N') return nullptr;
    uint32_t count = *(const uint32_t*)(b + 4), sz = *(const uint32_t*)(b + 8);
    size_t need = 12 + (size_t)count * sz * sz * 4;
    if (sz == 0 || (size_t)(_binary_modern_icons_bin_end - b) < need) return nullptr;
    if (id < 0 || (uint32_t)id >= count) id = AppIcons::GENERIC;  // generic
    n = (int)sz;
    return (const uint32_t*)(b + 12) + (size_t)id * sz * sz;
}

// box-filter (area-average) resample of a premultiplied square bitmap, so a
// 128 px master stays crisp and alias-free at dock / desktop / menu sizes.
static void area_scale(const uint32_t* src, int n, uint32_t* dst, int s) {
    uint32_t step = (uint32_t)(((uint64_t)n << 16) / (uint32_t)s);   // 16.16 source px per dest px
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

// a small lru of scaled copies keyed by (icon, size): the dock magnification
// asks for many sizes, but only a handful at a time.
struct IconCacheEntry { int id, size; uint32_t* px; uint32_t used; };
static IconCacheEntry g_icache[48];
static uint32_t g_iclock = 0;

static const uint32_t* icon_scaled(int id, int size) {
    for (int i = 0; i < 48; i++)
        if (g_icache[i].px && g_icache[i].id == id && g_icache[i].size == size) {
            g_icache[i].used = ++g_iclock;
            return g_icache[i].px;
        }
    int n;
    const uint32_t* src = icon_src(id, n);
    if (!src) return nullptr;
    int slot = 0;
    for (int i = 0; i < 48; i++) {
        if (!g_icache[i].px) { slot = i; break; }
        if (g_icache[i].used < g_icache[slot].used) slot = i;
    }
    if (g_icache[slot].px) KernelHeap::Free(g_icache[slot].px);
    uint32_t* px = (uint32_t*)KernelHeap::Alloc((size_t)size * size * 4);
    g_icache[slot].px = px;
    if (!px) return nullptr;
    area_scale(src, n, px, size);
    g_icache[slot].id = id; g_icache[slot].size = size; g_icache[slot].used = ++g_iclock;
    return px;
}

// blend a premultiplied bitmap at (x, y), scaled by alpha 0..255.
static void blit_premul(const uint32_t* px, int s, int x, int y, int alpha) {
    uint32_t* buf; int pitch, sw, sh;
    if (!target(buf, pitch, sw, sh) || alpha <= 0) return;
    int ga = alpha >= 255 ? 256 : alpha;
    for (int j = 0; j < s; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)sh) continue;
        uint32_t* drow = buf + (size_t)py * pitch;
        const uint32_t* srow = px + (size_t)j * s;
        for (int i = 0; i < s; i++) {
            int qx = x + i;
            if ((unsigned)qx >= (unsigned)sw) continue;
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

void AppTile(int id, int x, int y, int size) {
    if (size < 8) return;
    int r = (size * 23) / 100;                       // squircle-ish corner
    // the blob holds the colourful set, then the minimal set.
    int set_ofs = UIStyle::MinimalIcons() ? AppIcons::GENERIC + 1 : 0;
    const uint32_t* art = size <= 256 ? icon_scaled(id + set_ofs, size) : nullptr;
    if (art) {
        Shadow(x + size / 16, y, size - size / 8, size, r, size >= 40 ? 4 : 2, size >= 40 ? 3 : 1, 80);
        blit_premul(art, size, x, y, 255);
        return;
    }
    TilePalette p = palette(id);
    Shadow(x, y, size, size, r, size >= 40 ? 4 : 2, size >= 40 ? 3 : 1, 90);
    RoundRect(x, y, size, size, r, p.top, p.bottom, 255);
    // soft sheen on the upper half, then a hairline edge to lift it off dark
    // backgrounds.
    RoundRect(x, y, size, size / 2, r, 0xFFFFFFFFu, 0xFFFFFFFFu, 22);
    RoundOutline(x, y, size, size, r, 0xFFFFFFFFu, 34);
    int pad = size / 6;
    AppIcons::Draw(id, x + pad, y + pad, size - pad * 2);
}

}
