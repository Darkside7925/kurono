#pragma once
#include "../kernel/types.h"
#include "../drivers/graphics.h"

class MediaDecoder {
public:
    struct Image {
        int width;
        int height;
        uint8_t* data;
        bool valid;
        uint8_t order; // 0=rgba, 1=bgra
        bool owns;
    };
    static bool IsPNG(const uint8_t* p, size_t n);
    static bool IsJPEG(const uint8_t* p, size_t n);
    static bool IsWebP(const uint8_t* p, size_t n);
    static Image DecodeRaw(uint32_t data_addr);
    static Image DecodeModule(uint32_t start, uint32_t end);
    static void DrawImage(const Image& img, int x, int y);
    static void DrawImageScaled(const Image& img, int x, int y, int tw, int th);
    static void DrawImageScaledAlpha(const Image& img, int x, int y, int tw, int th, uint8_t alpha_mul);
    static void DrawImageScaledAutoCrop(const Image& img, int x, int y, int tw, int th, uint8_t alpha_thresh);
    static void FreeImage(Image& img);

    static inline uint8_t kAlphaCutoff = 32;
};
