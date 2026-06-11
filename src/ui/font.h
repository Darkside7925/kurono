#pragma once
#include "../kernel/types.h"

class FontTTF {
public:
    static bool ok;
    static void Init(const uint8_t* data, int size);
    static int DrawString(int x, int y, float pxh, const char* s, uint32_t color);
    static void DrawStringCenter(int cx, int y, float pxh, const char* s, uint32_t color);
    static int Measure(float pxh, const char* s);
    static void FlushCache();

private:
    struct Glyph {
        bool valid;
        int w, h, xoff, yoff, advance;
        unsigned char* bitmap;
    };
    
    struct CacheSlot {
        float size;
        bool active;
        Glyph glyphs[256];
    };
    
    static const int MAX_CACHE_SLOTS = 16;
    static CacheSlot caches[MAX_CACHE_SLOTS];
    static int current_cache_index;
    
    static CacheSlot* GetCache(float size);
};
