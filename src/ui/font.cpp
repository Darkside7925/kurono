#include "font.h"
#include "vga_font.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

extern "C" int tt_init_from_memory(const unsigned char* data, int size);
extern "C" unsigned char* tt_raster(unsigned int codepoint, float pixel_height, int* w, int* h, int* xoff, int* yoff, int* xadvance);
extern "C" void tt_free_bitmap(unsigned char* p);
extern "C" int tt_get_kern(unsigned int c1, unsigned int c2);

bool FontTTF::ok = false;
FontTTF::CacheSlot FontTTF::caches[FontTTF::MAX_CACHE_SLOTS];
int FontTTF::current_cache_index = 0;

void FontTTF::Init(const uint8_t* data, int size) { 
    SerialLogger::Log("FontTTF: Init called with size "); SerialLogger::LogDec(size); SerialLogger::Log("\r\n");
    ok = tt_init_from_memory(data, size) != 0; 
    if (ok) SerialLogger::Log("FontTTF: Init OK\r\n");
    else SerialLogger::Log("FontTTF: Init FAILED\r\n");
    FlushCache();
}

void FontTTF::FlushCache() {
    SerialLogger::Log("FontTTF: FlushCache called\r\n");
    for(int s=0; s<MAX_CACHE_SLOTS; s++) {
        caches[s].active = false;
        caches[s].size = 0.0f;
        for (int i = 0; i < 256; i++) {
            if (caches[s].glyphs[i].valid) {
                if (caches[s].glyphs[i].bitmap) tt_free_bitmap(caches[s].glyphs[i].bitmap);
                caches[s].glyphs[i].bitmap = nullptr;
                caches[s].glyphs[i].valid = false;
            }
        }
    }
}

FontTTF::CacheSlot* FontTTF::GetCache(float size) {
    // 1. search for existing
    for(int i=0; i<MAX_CACHE_SLOTS; i++) {
        if (caches[i].active && caches[i].size == size) {
            return &caches[i];
        }
    }
    
    // 2. find empty slot
    for(int i=0; i<MAX_CACHE_SLOTS; i++) {
        if (!caches[i].active) {
            caches[i].active = true;
            caches[i].size = size;
            return &caches[i];
        }
    }
    
    // 3. evict oldest (simple round robin)
    static int evict_idx = 0;
    int idx = evict_idx;
    evict_idx = (evict_idx + 1) % MAX_CACHE_SLOTS;
    
    // seriallogger::log("fontttf: cache eviction! slot "); seriallogger::logdec(idx); seriallogger::log("\r\n");

    CacheSlot* slot = &caches[idx];
    for (int i = 0; i < 256; i++) {
        if (slot->glyphs[i].valid) {
             if (slot->glyphs[i].bitmap) tt_free_bitmap(slot->glyphs[i].bitmap);
             slot->glyphs[i].bitmap = nullptr;
             slot->glyphs[i].valid = false;
        }
    }
    slot->active = true;
    slot->size = size;
    return slot;
}

// renders crisp, clean text at any scale using the standard vga font.
// at scale=1: 8px wide × 16px tall per character (matches real terminals)
// larger pxh values scale proportionally with bilinear-like rendering.

static int DrawStringFallback(int x, int y, float pxh, const char* str, uint32_t color) {
    // scale factor: at pxh=16 → scale=1 (native), pxh=32 → scale=2, etc.
    int scale = (int)(pxh / 16.0f + 0.5f);
    if (scale < 1) scale = 1;
    
    int char_w = 8 * scale;
    int char_h = 16 * scale;
    int ox = x;
    
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    while (*str) {
        unsigned char c = (unsigned char)*str++;
        const unsigned char* glyph = vga_font_8x16[c];
        
        if (scale == 1) {
            // fast path: 1:1 pixel rendering, no scaling
            for (int row = 0; row < 16; row++) {
                unsigned char bits = glyph[row];
                if (!bits) continue;
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        Graphics::BlendPixel(x + col, y + row, r, g, b, 255);
                    }
                }
            }
        } else {
            // scaled rendering
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
    (void)char_h;
    return x - ox;
}

int FontTTF::DrawString(int x, int y, float pxh, const char* s, uint32_t color) {
    if (!ok) {
        return DrawStringFallback(x, y, pxh, s, color);
    }
    
    CacheSlot* cache = GetCache(pxh);
    
    uint8_t r = (color >> 16) & 0xFF; uint8_t g = (color >> 8) & 0xFF; uint8_t b = color & 0xFF;
    int penx = x; int peny = y;
    
    unsigned int prev_c = 0;
    (void)prev_c;
    
    while (*s) {
        unsigned int c = (unsigned int)(unsigned char)*s;
        
        if (c >= 256) { s++; continue; }
        
        // check cache using local cache pointer
        if (!cache->glyphs[c].valid) {
            cache->glyphs[c].bitmap = tt_raster(c, pxh, &cache->glyphs[c].w, &cache->glyphs[c].h, &cache->glyphs[c].xoff, &cache->glyphs[c].yoff, &cache->glyphs[c].advance);
            cache->glyphs[c].valid = true;
        }
        
        Glyph& g_char = cache->glyphs[c];
        
        if (g_char.bitmap) {
            int ox = penx + g_char.xoff; int oy = peny + g_char.yoff;
            for (int yy = 0; yy < g_char.h; yy++) {
                for (int xx = 0; xx < g_char.w; xx++) {
                    uint8_t a = g_char.bitmap[yy*g_char.w + xx];
                    if (a) Graphics::BlendPixel(ox + xx, oy + yy, r, g, b, a);
                }
            }
        }
        
        penx += g_char.advance;
        prev_c = c;
        s++;
    }
    return penx - x;
}

void FontTTF::DrawStringCenter(int cx, int y, float pxh, const char* s, uint32_t color) {
    int w = Measure(pxh, s);
    int x = cx - w/2;
    DrawString(x, y, pxh, s, color);
}

int FontTTF::Measure(float pxh, const char* s) {
    if (!ok) {
        // vga 8x16 fallback measure
        int scale = (int)(pxh / 16.0f + 0.5f);
        if (scale < 1) scale = 1;
        int w = 0;
        while (*s++) w += 8 * scale;
        return w;
    }
    
    CacheSlot* cache = GetCache(pxh);
    
    int penx = 0; 
    while (*s) { 
        unsigned int c = (unsigned int)(unsigned char)*s;
        if (c < 256) {
            if (!cache->glyphs[c].valid) {
                cache->glyphs[c].bitmap = tt_raster(c, pxh, &cache->glyphs[c].w, &cache->glyphs[c].h, &cache->glyphs[c].xoff, &cache->glyphs[c].yoff, &cache->glyphs[c].advance);
                cache->glyphs[c].valid = true;
            }
            penx += cache->glyphs[c].advance;
        }
        s++; 
    } 
    return penx;
}
