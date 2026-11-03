#include "font.h"
#include "vga_font.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

extern "C" int tt_init_from_memory(const unsigned char* data, int size);
extern "C" void tt_free_bitmap(unsigned char* p);
extern "C" int tt_init_face(int face, const unsigned char* data, int size);
extern "C" unsigned char* tt_raster_face(int face, unsigned int codepoint, float pixel_height,
                                         int* w, int* h, int* xoff, int* yoff, int* xadvance_26_6);
extern "C" int tt_kern_face(int face, unsigned int c1, unsigned int c2, float pixel_height);
extern "C" int tt_ascent_face(int face, float pixel_height);
extern "C" int tt_has_glyph(int face, unsigned int codepoint);

static int DrawStringFallback(int x, int y, float pxh, const char* str, uint32_t color);

bool FontTTF::ok = false;
static bool g_loaded[3] = { false, false, false };
static bool g_active = true;

// coverage contrast curve: stb rasterizes linear coverage with no hinting, which
// reads thin and washed out at ui sizes on a dark theme. lift mid-tones
// (roughly coverage^0.8) so strokes look solid without going pixel-hard.
// (satoru)
static unsigned char g_gamma[256];
static bool g_gamma_ready = false;
static void build_gamma() {
    for (int i = 0; i < 256; i++) {
        int v = i + (i * (255 - i)) / 340;
        g_gamma[i] = (unsigned char)(v > 255 ? 255 : v);
    }
    g_gamma_ready = true;
}
FontTTF::CacheSlot FontTTF::caches[FontTTF::MAX_CACHE_SLOTS];
int FontTTF::current_cache_index = 0;

static const unsigned int REPLACEMENT_CP = 0xFFFD;

static inline bool size_eq(float a, float b) {
    float d = a - b; if (d < 0) d = -d;
    return d < 0.0625f;
}

// Decode one UTF-8 codepoint starting at *p, advancing p. Returns U+FFFD on
// invalid/overlong/truncated sequences and skips one byte to make progress.
static inline unsigned int utf8_next(const char*& p) {
    unsigned char c0 = (unsigned char)*p;
    if (c0 == 0) return 0;
    if (c0 < 0x80) { p++; return c0; }
    auto cont = [](unsigned char b){ return (b & 0xC0) == 0x80; };
    if ((c0 & 0xE0) == 0xC0) {
        unsigned char c1 = (unsigned char)p[1];
        if (!cont(c1)) { p++; return REPLACEMENT_CP; }
        unsigned int cp = ((c0 & 0x1Fu) << 6) | (c1 & 0x3Fu);
        if (cp < 0x80) { p++; return REPLACEMENT_CP; }
        p += 2; return cp;
    }
    if ((c0 & 0xF0) == 0xE0) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = c1 ? (unsigned char)p[2] : 0;
        if (!cont(c1) || !cont(c2)) { p++; return REPLACEMENT_CP; }
        unsigned int cp = ((c0 & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
        if (cp < 0x800) { p++; return REPLACEMENT_CP; }
        p += 3; return cp;
    }
    if ((c0 & 0xF8) == 0xF0) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = c1 ? (unsigned char)p[2] : 0;
        unsigned char c3 = c2 ? (unsigned char)p[3] : 0;
        if (!cont(c1) || !cont(c2) || !cont(c3)) { p++; return REPLACEMENT_CP; }
        unsigned int cp = ((c0 & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) { p++; return REPLACEMENT_CP; }
        p += 4; return cp;
    }
    p++; return REPLACEMENT_CP;
}

void FontTTF::EvictSlot(CacheSlot* slot) {
    for (int i = 0; i < ASCII_FAST; i++) {
        if (slot->ascii[i].resident && slot->ascii[i].bitmap) tt_free_bitmap(slot->ascii[i].bitmap);
        slot->ascii[i].bitmap = nullptr;
        slot->ascii[i].valid = false;
        slot->ascii[i].resident = false;
    }
    for (int i = 0; i < HASH_SLOTS; i++) {
        if (slot->hash[i].resident && slot->hash[i].bitmap) tt_free_bitmap(slot->hash[i].bitmap);
        slot->hash[i].bitmap = nullptr;
        slot->hash[i].valid = false;
        slot->hash[i].resident = false;
        slot->hash[i].codepoint = 0;
    }
    slot->lru_clock = 0;
}

void FontTTF::Init(const uint8_t* data, int size) {
    SerialLogger::Log("FontTTF: Init called with size "); SerialLogger::LogDec(size); SerialLogger::Log("\r\n");
    FlushCache();
    g_loaded[0] = tt_init_face(0, data, size) != 0;
    ok = g_loaded[0] && g_active;
    if (g_loaded[0]) SerialLogger::Log("FontTTF: Init OK\r\n");
    else SerialLogger::Log("FontTTF: Init FAILED\r\n");
}

void FontTTF::InitBold(const uint8_t* data, int size) {
    FlushCache();
    g_loaded[1] = tt_init_face(1, data, size) != 0;
    SerialLogger::Log(g_loaded[1] ? "FontTTF: bold face OK\r\n" : "FontTTF: bold face FAILED\r\n");
}

void FontTTF::InitMono(const uint8_t* data, int size) {
    FlushCache();
    g_loaded[2] = tt_init_face(2, data, size) != 0;
    SerialLogger::Log(g_loaded[2] ? "FontTTF: mono face OK\r\n" : "FontTTF: mono face FAILED\r\n");
}

bool FontTTF::Loaded() { return g_loaded[0]; }
bool FontTTF::HasMono() { return g_loaded[2]; }

int FontTTF::DrawMono(int x, int top, float pxh, const char* s, uint32_t color) {
    if (!ok || !s) return DrawStringFallback(x, top, pxh, s, color);
    int face = g_loaded[2] ? 2 : 0;
    return DrawFace(face, x, top + tt_ascent_face(face, pxh), pxh, s, color);
}

int FontTTF::MeasureMono(float pxh, const char* s) {
    if (!ok) return Measure(pxh, s);
    return MeasureFace(g_loaded[2] ? 2 : 0, pxh, s);
}

void FontTTF::SetActive(bool active) {
    g_active = active;
    ok = g_loaded[0] && g_active;
}

void FontTTF::FlushCache() {
    for (int s = 0; s < MAX_CACHE_SLOTS; s++) {
        EvictSlot(&caches[s]);
        caches[s].active = false;
        caches[s].size = 0.0f;
    }
}

FontTTF::CacheSlot* FontTTF::GetCache(float size, int face) {
    if (face > 0 && !g_loaded[face]) face = 0;
    for (int i = 0; i < MAX_CACHE_SLOTS; i++) {
        if (caches[i].active && caches[i].face == face && size_eq(caches[i].size, size)) return &caches[i];
    }
    for (int i = 0; i < MAX_CACHE_SLOTS; i++) {
        if (!caches[i].active) {
            EvictSlot(&caches[i]);
            caches[i].active = true;
            caches[i].size = size;
            caches[i].face = face;
            return &caches[i];
        }
    }
    static int evict_idx = 0;
    int idx = evict_idx;
    evict_idx = (evict_idx + 1) % MAX_CACHE_SLOTS;
    CacheSlot* slot = &caches[idx];
    EvictSlot(slot);
    slot->active = true;
    slot->size = size;
    slot->face = face;
    return slot;
}

FontTTF::Glyph* FontTTF::LookupOrLoad(CacheSlot* cache, unsigned int cp, float pxh) {
    Glyph* g;
    if (cp < (unsigned)ASCII_FAST) {
        g = &cache->ascii[cp];
    } else {
        unsigned int h = cp * 2654435761u;
        unsigned int idx = h & HASH_MASK;
        unsigned int probe = 0;
        for (;;) {
            Glyph* slot = &cache->hash[idx];
            if (!slot->resident) { g = slot; break; }
            if (slot->codepoint == cp) { g = slot; break; }
            probe++;
            if (probe >= HASH_SLOTS) {
                // table saturated for this size; evict probed entry
                g = slot;
                if (g->bitmap) tt_free_bitmap(g->bitmap);
                g->bitmap = nullptr;
                g->resident = false;
                g->valid = false;
                break;
            }
            idx = (idx + 1) & HASH_MASK;
        }
    }

    if (!g->valid) {
        // advances are 26.6 fixed point so layout keeps sub-pixel precision.
        int gw = 0, gh = 0, xo = 0, yo = 0, ad = 0;
        unsigned char* bmp = tt_raster_face(cache->face, cp, pxh, &gw, &gh, &xo, &yo, &ad);
        if (!bmp && cp != REPLACEMENT_CP && cp != '?' && !tt_has_glyph(cache->face, cp)) {
            // missing-glyph fallback: rasterize '?' and use its advance
            bmp = tt_raster_face(cache->face, '?', pxh, &gw, &gh, &xo, &yo, &ad);
        }
        if (ad <= 0) {
            // last-resort advance so the pen still moves
            ad = (int)(pxh * 0.5f * 64.0f + 0.5f);
        }
        if (bmp) {
            if (!g_gamma_ready) build_gamma();
            for (int k = 0; k < gw * gh; k++) bmp[k] = g_gamma[bmp[k]];
        }
        g->codepoint = cp;
        g->bitmap = bmp;
        g->w = gw; g->h = gh; g->xoff = xo; g->yoff = yo; g->advance = ad;
        g->valid = true;
        g->resident = true;
    }
    g->lru = ++cache->lru_clock;
    return g;
}

static int DrawStringFallback(int x, int y, float pxh, const char* str, uint32_t color) {
    if (!str) return 0;
    int scale = (int)(pxh / 16.0f + 0.5f);
    if (scale < 1) scale = 1;
    int char_w = 8 * scale;
    int ox = x;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    while (*str) {
        unsigned char c = (unsigned char)*str++;
        const unsigned char* glyph = vga_font_8x16[c];
        if (scale == 1) {
            for (int row = 0; row < 16; row++) {
                unsigned char bits = glyph[row];
                if (!bits) continue;
                if (bits & 0x80) Graphics::BlendPixel(x + 0, y + row, r, g, b, 255);
                if (bits & 0x40) Graphics::BlendPixel(x + 1, y + row, r, g, b, 255);
                if (bits & 0x20) Graphics::BlendPixel(x + 2, y + row, r, g, b, 255);
                if (bits & 0x10) Graphics::BlendPixel(x + 3, y + row, r, g, b, 255);
                if (bits & 0x08) Graphics::BlendPixel(x + 4, y + row, r, g, b, 255);
                if (bits & 0x04) Graphics::BlendPixel(x + 5, y + row, r, g, b, 255);
                if (bits & 0x02) Graphics::BlendPixel(x + 6, y + row, r, g, b, 255);
                if (bits & 0x01) Graphics::BlendPixel(x + 7, y + row, r, g, b, 255);
            }
        } else {
            for (int row = 0; row < 16; row++) {
                unsigned char bits = glyph[row];
                if (!bits) continue;
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        Graphics::FillRect(x + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        x += char_w;
    }
    return x - ox;
}

static inline void BlitGlyphRow(int dx0, int dy, const unsigned char* row, int w,
                                uint8_t r, uint8_t g, uint8_t b) {
    int x = 0;
    // 8-wide chunks; the compiler can unroll and hoist invariants.
    for (; x + 8 <= w; x += 8) {
        uint8_t a0 = row[x + 0]; if (a0) Graphics::BlendPixel(dx0 + x + 0, dy, r, g, b, a0);
        uint8_t a1 = row[x + 1]; if (a1) Graphics::BlendPixel(dx0 + x + 1, dy, r, g, b, a1);
        uint8_t a2 = row[x + 2]; if (a2) Graphics::BlendPixel(dx0 + x + 2, dy, r, g, b, a2);
        uint8_t a3 = row[x + 3]; if (a3) Graphics::BlendPixel(dx0 + x + 3, dy, r, g, b, a3);
        uint8_t a4 = row[x + 4]; if (a4) Graphics::BlendPixel(dx0 + x + 4, dy, r, g, b, a4);
        uint8_t a5 = row[x + 5]; if (a5) Graphics::BlendPixel(dx0 + x + 5, dy, r, g, b, a5);
        uint8_t a6 = row[x + 6]; if (a6) Graphics::BlendPixel(dx0 + x + 6, dy, r, g, b, a6);
        uint8_t a7 = row[x + 7]; if (a7) Graphics::BlendPixel(dx0 + x + 7, dy, r, g, b, a7);
    }
    for (; x + 4 <= w; x += 4) {
        uint8_t a0 = row[x + 0]; if (a0) Graphics::BlendPixel(dx0 + x + 0, dy, r, g, b, a0);
        uint8_t a1 = row[x + 1]; if (a1) Graphics::BlendPixel(dx0 + x + 1, dy, r, g, b, a1);
        uint8_t a2 = row[x + 2]; if (a2) Graphics::BlendPixel(dx0 + x + 2, dy, r, g, b, a2);
        uint8_t a3 = row[x + 3]; if (a3) Graphics::BlendPixel(dx0 + x + 3, dy, r, g, b, a3);
    }
    for (; x < w; x++) {
        uint8_t a = row[x];
        if (a) Graphics::BlendPixel(dx0 + x, dy, r, g, b, a);
    }
}

int FontTTF::DrawString(int x, int y, float pxh, const char* s, uint32_t color) {
    if (!ok || !s) {
        return DrawStringFallback(x, y, pxh, s ? s : "", color);
    }
    return DrawFace(0, x, y, pxh, s, color);
}

int FontTTF::DrawPixel(int x, int y, float pxh, const char* s, uint32_t color) {
    return DrawStringFallback(x, y, pxh, s, color);
}

int FontTTF::Ascent(float pxh, bool bold) {
    return tt_ascent_face(bold && g_loaded[1] ? 1 : 0, pxh);
}

int FontTTF::DrawText(int x, int top, float pxh, const char* s, uint32_t color, bool bold) {
    if (!ok || !s) return DrawStringFallback(x, top, pxh, s, color);
    int face = bold && g_loaded[1] ? 1 : 0;
    return DrawFace(face, x, top + tt_ascent_face(face, pxh), pxh, s, color);
}

int FontTTF::MeasureText(float pxh, const char* s, bool bold) {
    if (!ok) return Measure(pxh, s);
    return MeasureFace(bold && g_loaded[1] ? 1 : 0, pxh, s);
}

int FontTTF::DrawFace(int face, int x, int y, float pxh, const char* s, uint32_t color) {
    CacheSlot* cache = GetCache(pxh, face);
    const uint8_t r = (color >> 16) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = color & 0xFF;
    const int startx = x;
    int pen26 = x * 64;
    const int peny = y;

    unsigned int prev_cp = 0;
    const char* p = s;
    while (*p) {
        // ASCII printable fast path: skip UTF-8 decode entirely.
        unsigned int cp;
        unsigned char c0 = (unsigned char)*p;
        if (c0 >= 0x20 && c0 <= 0x7E) { cp = c0; p++; }
        else if (c0 < 0x80) { cp = c0; p++; }
        else { cp = utf8_next(p); }
        if (cp == 0) break;

        Glyph* gl = LookupOrLoad(cache, cp, pxh);

        if (prev_cp) pen26 += tt_kern_face(cache->face, prev_cp, cp, pxh);

        if (gl->bitmap && gl->w > 0 && gl->h > 0) {
            const int ox = ((pen26 + 32) >> 6) + gl->xoff;
            const int oy = peny + gl->yoff;
            const int gw = gl->w;
            const int gh = gl->h;
            const unsigned char* __restrict bm = gl->bitmap;
            for (int yy = 0; yy < gh; yy++) {
                BlitGlyphRow(ox, oy + yy, bm + (size_t)yy * gw, gw, r, g, b);
            }
        }

        pen26 += gl->advance;
        prev_cp = cp;
    }
    return ((pen26 + 32) >> 6) - startx;
}

void FontTTF::DrawStringCenter(int cx, int y, float pxh, const char* s, uint32_t color) {
    int w = Measure(pxh, s);
    DrawString(cx - w / 2, y, pxh, s, color);
}

int FontTTF::Measure(float pxh, const char* s) {
    if (!s) return 0;
    if (!ok) {
        int scale = (int)(pxh / 16.0f + 0.5f);
        if (scale < 1) scale = 1;
        int w = 0;
        while (*s++) w += 8 * scale;
        return w;
    }

    return MeasureFace(0, pxh, s);
}

int FontTTF::MeasureFace(int face, float pxh, const char* s) {
    if (!s) return 0;
    CacheSlot* cache = GetCache(pxh, face);
    int pen26 = 0;
    unsigned int prev_cp = 0;
    const char* p = s;
    while (*p) {
        unsigned int cp;
        unsigned char c0 = (unsigned char)*p;
        if (c0 < 0x80) { cp = c0; p++; }
        else { cp = utf8_next(p); }
        if (cp == 0) break;

        Glyph* gl = LookupOrLoad(cache, cp, pxh);
        if (prev_cp) pen26 += tt_kern_face(cache->face, prev_cp, cp, pxh);
        pen26 += gl->advance;
        prev_cp = cp;
    }
    return (pen26 + 32) >> 6;
}
