#pragma once
#include "../kernel/types.h"

class FontTTF {
public:
    // true when a face is loaded AND the active ui style renders smooth text;
    // every caller treats !ok as "use the 8x16 pixel font". (satoru)
    static bool ok;
    static void Init(const uint8_t* data, int size);       // regular face
    static void InitBold(const uint8_t* data, int size);   // semibold face
    static void InitMono(const uint8_t* data, int size);   // monospace face
    static bool Loaded();
    // the classic style keeps the pixel font even with a face loaded.
    static void SetActive(bool active);
    static int DrawString(int x, int y, float pxh, const char* s, uint32_t color);
    static void DrawStringCenter(int cx, int y, float pxh, const char* s, uint32_t color);
    static int Measure(float pxh, const char* s);
    // y is the TOP of the line box (like the pixel font), not the baseline.
    static int DrawText(int x, int top, float pxh, const char* s, uint32_t color, bool bold = false);
    static int MeasureText(float pxh, const char* s, bool bold = false);
    static int Ascent(float pxh, bool bold = false);
    // monospace face (terminal grid); falls back to the regular face.
    static bool HasMono();
    static int DrawMono(int x, int top, float pxh, const char* s, uint32_t color);
    static int MeasureMono(float pxh, const char* s);
    // the 8x16 pixel font, regardless of style.
    static int DrawPixel(int x, int y, float pxh, const char* s, uint32_t color);
    static void FlushCache();
    // the compositor's text target: the 32 bpp buffer (null for any other
    // depth, then BlendPixel draws) and the visible box, the active clip within
    // the screen. glyphs outside it are skipped, the rest blended inline. the
    // kernel registers it; without it text draws as before. (satoru)
    typedef void (*TargetFn)(uint32_t** buf, size_t* pitch_px, int* x0, int* y0, int* x1, int* y1);
    static void SetTarget(TargetFn fn);

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
        int face;
        bool active;
        uint32_t lru_clock;
        Glyph ascii[ASCII_FAST];
        Glyph hash[HASH_SLOTS];
    };

    static const int MAX_CACHE_SLOTS = 16;
    static CacheSlot caches[MAX_CACHE_SLOTS];
    static int current_cache_index;

    static CacheSlot* GetCache(float size, int face = 0);
    static int DrawFace(int face, int x, int y, float pxh, const char* s, uint32_t color);
    static int MeasureFace(int face, float pxh, const char* s);
    static Glyph* LookupOrLoad(CacheSlot* cache, unsigned int codepoint, float pxh);
    static void EvictSlot(CacheSlot* slot);
};
