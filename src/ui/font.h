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
        unsigned int codepoint;
        bool valid;
        bool resident;
        int w, h, xoff, yoff, advance;
        unsigned char* bitmap;
        uint32_t lru;
    };

    // ASCII printable fast-path table: directly indexed [0..127].
    // Extended/Unicode codepoints use the open-addressed hash slots.
    static const int ASCII_FAST = 128;
    static const int HASH_SLOTS = 256; // power of two
    static const int HASH_MASK = HASH_SLOTS - 1;

    struct CacheSlot {
        float size;
        bool active;
        uint32_t lru_clock;
        Glyph ascii[ASCII_FAST];
        Glyph hash[HASH_SLOTS];
    };

    static const int MAX_CACHE_SLOTS = 16;
    static CacheSlot caches[MAX_CACHE_SLOTS];
    static int current_cache_index;

    static CacheSlot* GetCache(float size);
    static Glyph* LookupOrLoad(CacheSlot* cache, unsigned int codepoint, float pxh);
    static void EvictSlot(CacheSlot* slot);
};
