#pragma once
#include "../media/mediadecoder.h"

class GUI {
public:
    static MediaDecoder::Image wallpaper;
    static uint8_t* backbuffer;
    static uint8_t* wallpaper_buffer;
    static void UpdateBackbuffer();
    static void SetWallpaper(const MediaDecoder::Image& img);
    static void ReinitBuffers();  // Call after resolution change
    static void DrawDesktop();
    static void DrawTaskbar();
    static void DrawRegion(int x, int y, int w, int h);
    // Optimization: Restore region from backbuffer to screen
    static void RestoreRegion(int x, int y, int w, int h);
    static void DrawLogin();
    static void DrawTime(int x, int y);
    static void BlurWallpaper();
};
