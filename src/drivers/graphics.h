#pragma once
#include "../kernel/types.h"

// enhanced graphics driver with 180hz support
// integrates with displaycontroller for high refresh rate rendering
// supports double buffering, vsync, and advanced rendering features

class Graphics {
public:
    enum RenderMode {
        SINGLE_BUFFER,
        DOUBLE_BUFFER,
        TRIPLE_BUFFER
    };

    enum BlendMode {
        BLEND_NONE,
        BLEND_ALPHA,
        BLEND_ADDITIVE,
        BLEND_MULTIPLY
    };

    struct DrawStats {
        uint32_t frames_rendered;
        uint32_t frames_dropped;
        uint32_t avg_frame_time_us;
        uint32_t last_frame_time_us;
        uint32_t target_fps;
        uint32_t current_fps;
        bool vsync_enabled;
    };

    static void Init(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
    static void ReinitForResolution(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
    static void InitAdvanced(); // initialize with display controller
    static void SetRenderMode(RenderMode mode);
    static RenderMode GetRenderMode();
    
    // frame management for high refresh rates
    static void BeginFrame();
    static void EndFrame();
    static void SetTargetFPS(uint32_t fps);
    static uint32_t GetTargetFPS();
    static uint32_t DetectRefreshRate();  // measure via vsync timing, fallback 60 hz
    static uint32_t GetMonitorHz();       // last detected monitor refresh rate
    static bool IsFramebufferWC();        // true if fb is write-combining (bare-metal safe)
    static bool ShouldRender(); // frame pacing for 180hz
    static void WaitForVSync();
    
    // basic drawing
    static void Clear(uint32_t color);
    static void DrawPixel(int x, int y, uint32_t color);
    static uint32_t ReadPixel(int x, int y);
    static void BlendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    static void FillRect(int x, int y, int w, int h, uint32_t color);
    static void FillRectRounded(int x, int y, int w, int h, int r, uint32_t color);
    static void FillRectAlpha(int x, int y, int w, int h, uint8_t a, uint32_t color);
    static void FillGradientV(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
    
    // advanced drawing
    static void DrawLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness = 1);
    static void DrawCircle(int cx, int cy, int radius, uint32_t color, bool filled = false);
    static void DrawEllipse(int cx, int cy, int rx, int ry, uint32_t color, bool filled = false);
    static void DrawPolygon(const int* points, int count, uint32_t color, bool filled = false);
    
    // effect rendering
    static void BlurRect(int x, int y, int w, int h, int radius);
    static void ApplyGlow(int x, int y, int w, int h, uint32_t color, int intensity);
    static void ApplyShadow(int x, int y, int w, int h, int offset_x, int offset_y, uint8_t alpha);
    
    // texture/image operations
    static void DrawImage(const uint8_t* image_data, int src_x, int src_y, int src_w, int src_h,
                         int dst_x, int dst_y, int dst_w, int dst_h);
    static void DrawImageScaled(const uint8_t* image_data, int img_w, int img_h, 
                               int x, int y, int w, int h);
    static void DrawImageRotated(const uint8_t* image_data, int img_w, int img_h,
                                int x, int y, float angle);
    
    // buffer management
    static void SwapBuffers();
    struct Rect { int x, y, w, h; };
    // atomic present of an explicit damage list (max 32 rects, NULL/0 => full)
    static void Present(const Rect* rects, int count);
    static uint8_t* GetActiveBuffer();
    static uint8_t* GetBackBuffer();
    static void ClearBackBuffer(uint32_t color);
    static void CopyRegion(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
    
    // performance optimization
    static void SetClipRect(int x, int y, int w, int h);
    static void ClearClipRect();
    static void SetBlendMode(BlendMode mode);
    static BlendMode GetBlendMode();
    
    // dirty region tracking for optimized rendering
    static void MarkDirty(int x, int y, int w, int h);
    static void ClearDirtyRegions();
    static void RenderDirtyRegions();
    
    // information
    static int GetWidth();
    static int GetHeight();
    static uint32_t GetPitch();
    static uint8_t GetBpp();
    static uint8_t* GetBuffer();
    static void SetBuffer(uint8_t* addr);
    static const DrawStats& GetDrawStats();
    
    // color utilities
    static uint32_t RGB(uint8_t r, uint8_t g, uint8_t b);
    static uint32_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    static void ColorToRGB(uint32_t color, uint8_t& r, uint8_t& g, uint8_t& b);
    static uint32_t BlendColors(uint32_t src, uint32_t dst, uint8_t alpha);
    static uint32_t InterpolateColor(uint32_t c1, uint32_t c2, float t);

    static inline void FillRoundedRect(int x, int y, int w, int h, int r, uint32_t c) {
        FillRectRounded(x, y, w, h, r, c);
    }
    static inline void FillCircle(int cx, int cy, int r, uint32_t c) {
        DrawCircle(cx, cy, r, c, true);
    }
    static inline void DrawRect(int x, int y, int w, int h, uint32_t c) {
        DrawLine(x, y, x+w-1, y, c);
        DrawLine(x, y+h-1, x+w-1, y+h-1, c);
        DrawLine(x, y, x, y+h-1, c);
        DrawLine(x+w-1, y, x+w-1, y+h-1, c);
    }

    // text rendering  -  delegates to fontttf w/ bitmap fallback
    static void DrawString(int x, int y, const char* s, uint32_t fg, uint32_t bg);

    // accessibility post-processing applied just before SwapBuffers copies
    // the back buffer to the framebuffer.  Modes:
    //   0 = off, 1 = protanopia, 2 = deuteranopia, 3 = tritanopia, 4 = grayscale
    static void SetColorFilter(int mode);
    static int  GetColorFilter();
    static void SetHighContrast(bool on);

private:
    static uint8_t* fb_addr;
    static uint8_t* back_buffer;
    static uint8_t* active_buffer;
    static size_t   back_buffer_size;   // track for pmm freeing
    static uint32_t fb_width;
    static uint32_t fb_height;
    static uint32_t fb_pitch;
    static uint8_t fb_bpp;
    
    static RenderMode render_mode;
    static BlendMode blend_mode;
    
    // frame timing  -  64-bit μs to avoid wrap (uint32 wraps every ~71 min)
    static uint32_t target_frame_time_us;
    static uint64_t last_frame_time;
    static uint32_t frame_count;
    static uint64_t fps_sample_time;
    static DrawStats draw_stats;
    
    // bare-metal state
    static bool fb_wc_active;     // true if fb pages are write-combining
    static uint32_t monitor_hz;   // last detected monitor hz (0 = unknown)
    
    // clipping
    static int clip_x;
    static int clip_y;
    static int clip_w;
    static int clip_h;
    static bool clipping_enabled;
    
    // dirty regions for optimized rendering
    struct DirtyRegion {
        int x, y, w, h;
        bool active;
    };
    static DirtyRegion dirty_regions[16];
    static int dirty_count;
    
    // internal helpers
    static bool IsPointInBounds(int x, int y);
    static void ClipRect(int& x, int& y, int& w, int& h);
    static void DrawPixelUnsafe(int x, int y, uint32_t color);
    static void DrawPixelBlended(int x, int y, uint32_t color, uint8_t alpha);
    static uint32_t GetPixelUnsafe(int x, int y);
    
    // line drawing algorithms
    static void DrawLineBresenham(int x0, int y0, int x1, int y1, uint32_t color);
    static void DrawThickLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    
    // circle/ellipse helpers
    static void PlotCirclePoints(int cx, int cy, int x, int y, uint32_t color, bool filled);
    static void PlotEllipsePoints(int cx, int cy, int x, int y, uint32_t color, bool filled);
};
