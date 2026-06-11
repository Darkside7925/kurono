#include "graphics.h"
#include "display.h"
#include "serial.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../ui/font.h"

// Utility functions (static to avoid linking conflicts)
static int abs(int x) { return x < 0 ? -x : x; }
static int max(int a, int b) { return a > b ? a : b; }

uint8_t* Graphics::fb_addr = 0;
uint8_t* Graphics::back_buffer = 0;
uint8_t* Graphics::active_buffer = 0;
uint32_t Graphics::fb_width = 0;
uint32_t Graphics::fb_height = 0;
uint32_t Graphics::fb_pitch = 0;
uint8_t Graphics::fb_bpp = 0;

Graphics::RenderMode Graphics::render_mode = SINGLE_BUFFER;
Graphics::BlendMode Graphics::blend_mode = BLEND_ALPHA;

uint32_t Graphics::target_frame_time_us = 5555; // ~180Hz (1000000/180)
uint32_t Graphics::last_frame_time = 0;
uint32_t Graphics::frame_count = 0;
Graphics::DrawStats Graphics::draw_stats = {0};

int Graphics::clip_x = 0;
int Graphics::clip_y = 0;
int Graphics::clip_w = 0;
int Graphics::clip_h = 0;
bool Graphics::clipping_enabled = false;

Graphics::DirtyRegion Graphics::dirty_regions[16];
int Graphics::dirty_count = 0;

void Graphics::Init(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    fb_addr = (uint8_t*)addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = bpp;
    active_buffer = fb_addr;
    
    SerialLogger::Log("Graphics: Basic Init ");
    SerialLogger::LogHex(width); SerialLogger::Log("x"); SerialLogger::LogHex(height);
    SerialLogger::Log(" @ "); SerialLogger::LogHex(addr); SerialLogger::Log("\r\n");
    
    // Initialize stats
    draw_stats.target_fps = 180;
    draw_stats.vsync_enabled = true;
    
    // Clear dirty regions
    for (int i = 0; i < 16; i++) {
        dirty_regions[i].active = false;
    }
    dirty_count = 0;
}

void Graphics::ReinitForResolution(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    // Free old back buffer if allocated
    if (back_buffer) {
        KernelHeap::Free(back_buffer);
        back_buffer = 0;
    }
    // Re-initialize base state
    Init(addr, width, height, pitch, bpp);
    // Re-allocate double buffer at new size if needed
    if (render_mode == DOUBLE_BUFFER || render_mode == TRIPLE_BUFFER) {
        size_t buffer_size = fb_pitch * fb_height;
        back_buffer = (uint8_t*)KernelHeap::Alloc(buffer_size);
        if (back_buffer) {
            memset(back_buffer, 0, buffer_size);
            active_buffer = back_buffer;
        } else {
            render_mode = SINGLE_BUFFER;
            active_buffer = fb_addr;
        }
    }
}

void Graphics::InitAdvanced() {
    if (!DisplayController::Init()) {
        SerialLogger::Log("Graphics: Display controller init failed, using basic mode\r\n");
        return;
    }
    
    // Find best mode for 180Hz
    const DisplayController::DisplayMode* mode = DisplayController::FindBestMode(
        1920, 1080, 32, DisplayController::REFRESH_180HZ);
    
    if (!mode) {
        SerialLogger::Log("Graphics: No 180Hz mode found, trying alternatives\r\n");
        // Try other high refresh rates
        mode = DisplayController::FindBestMode(1920, 1080, 32, DisplayController::REFRESH_144HZ);
        if (!mode) mode = DisplayController::FindBestMode(1024, 768, 32, DisplayController::REFRESH_180HZ);
        if (!mode) mode = DisplayController::FindBestMode(1024, 768, 32, DisplayController::REFRESH_120HZ);
    }
    
    if (mode && DisplayController::SetMode(mode)) {
        Init(mode->framebuffer_addr, mode->width, mode->height, mode->pitch, mode->bpp);
        target_frame_time_us = 1000000 / mode->refresh_rate;
        draw_stats.target_fps = mode->refresh_rate;
        
        SerialLogger::Log("Graphics: Advanced mode set - ");
        SerialLogger::Log(mode->description);
        SerialLogger::Log("\r\n");
        
        // Set up double buffering if supported
        if (DisplayController::IsDoubleBufferSupported()) {
            SetRenderMode(DOUBLE_BUFFER);
        }
    } else {
        SerialLogger::Log("Graphics: Failed to set advanced mode\r\n");
    }
}

void Graphics::SetRenderMode(RenderMode mode) {
    render_mode = mode;
    
    if (mode == DOUBLE_BUFFER || mode == TRIPLE_BUFFER) {
        // Allocate back buffer
        size_t buffer_size = fb_pitch * fb_height;
        back_buffer = (uint8_t*)KernelHeap::Alloc(buffer_size);
        if (back_buffer) {
            active_buffer = back_buffer;
            SerialLogger::Log("Graphics: Double buffering enabled\r\n");
        } else {
            SerialLogger::Log("Graphics: Failed to allocate back buffer\r\n");
            render_mode = SINGLE_BUFFER;
            active_buffer = fb_addr;
        }
    } else {
        if (back_buffer) {
            KernelHeap::Free(back_buffer);
            back_buffer = 0;
        }
        active_buffer = fb_addr;
    }
}

Graphics::RenderMode Graphics::GetRenderMode() {
    return render_mode;
}

void Graphics::BeginFrame() {
    uint32_t current_time = TimeManager::NowUTC().us;
    if (frame_count > 0) {
        draw_stats.last_frame_time_us = current_time - last_frame_time;
    }
    last_frame_time = current_time;
    
    // Clear dirty regions from previous frame
    ClearDirtyRegions();
}

void Graphics::EndFrame() {
    frame_count++;
    draw_stats.frames_rendered++;
    
    if (render_mode == DOUBLE_BUFFER || render_mode == TRIPLE_BUFFER) {
        SwapBuffers();
    }
    
    // VSync for frame pacing
    if (draw_stats.vsync_enabled) {
        WaitForVSync();
    }
    
    // Update FPS calculation every 30 frames
    if (frame_count % 30 == 0) {
        uint32_t current_time = TimeManager::NowUTC().us;
        if (frame_count > 30) {
            uint32_t elapsed = current_time - (last_frame_time - 30 * draw_stats.last_frame_time_us);
            draw_stats.current_fps = 30000000 / elapsed;
        }
    }
}

void Graphics::SetTargetFPS(uint32_t fps) {
    target_frame_time_us = 1000000 / fps;
    draw_stats.target_fps = fps;
}

bool Graphics::ShouldRender() {
    uint32_t current_time = TimeManager::NowUTC().us;
    return (current_time - last_frame_time) >= target_frame_time_us;
}

void Graphics::WaitForVSync() {
    DisplayController::WaitVSync();
}

void Graphics::SwapBuffers() {
    if (render_mode == SINGLE_BUFFER) return;
    
    if (back_buffer && fb_addr) {
        size_t bytes_per_line = (fb_width * fb_bpp) / 8;
        if (bytes_per_line == fb_pitch) {
            // Pitch matches width — single bulk memcpy (fastest)
            memcpy(fb_addr, back_buffer, fb_pitch * fb_height);
        } else {
            // Pitch != width — per-line copy
            uint8_t* src = back_buffer;
            uint8_t* dst = fb_addr;
            for (uint32_t y = 0; y < fb_height; y++) {
                memcpy(dst, src, bytes_per_line);
                src += fb_pitch;
                dst += fb_pitch;
            }
        }
    }
}

uint8_t* Graphics::GetActiveBuffer() {
    return active_buffer;
}

uint8_t* Graphics::GetBackBuffer() {
    return back_buffer ? back_buffer : fb_addr;
}

void Graphics::Clear(uint32_t color) {
    FillRect(0, 0, fb_width, fb_height, color);
}

void Graphics::ClearBackBuffer(uint32_t color) {
    uint8_t* original = active_buffer;
    if (back_buffer) active_buffer = back_buffer;
    Clear(color);
    active_buffer = original;
}

void Graphics::DrawPixel(int x, int y, uint32_t color) {
    if (!IsPointInBounds(x, y)) return;
    
    if (blend_mode == BLEND_ALPHA) {
        uint8_t alpha = (color >> 24) & 0xFF;
        if (alpha == 0) return;  // fully transparent — skip
        if (alpha == 0xFF) {
            DrawPixelUnsafe(x, y, color);
        } else {
            DrawPixelBlended(x, y, color, alpha);
        }
    } else {
        DrawPixelUnsafe(x, y, color);
    }
}

void Graphics::DrawPixelUnsafe(int x, int y, uint32_t color) {
    if (!active_buffer) return;
    
    uint32_t offset = y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        *(volatile uint32_t*)(active_buffer + offset) = color;
    } else if (fb_bpp == 24) {
        volatile uint8_t* p = active_buffer + offset;
        p[0] = color & 0xFF;
        p[1] = (color >> 8) & 0xFF;
        p[2] = (color >> 16) & 0xFF;
    } else if (fb_bpp == 16) {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        *(volatile uint16_t*)(active_buffer + offset) = v;
    }
}

void Graphics::DrawPixelBlended(int x, int y, uint32_t color, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { DrawPixelUnsafe(x, y, color); return; }
    
    uint32_t dst = ReadPixel(x, y);
    uint32_t blended = BlendColors(color, dst, alpha);
    DrawPixelUnsafe(x, y, blended);
}

uint32_t Graphics::ReadPixel(int x, int y) {
    if (!IsPointInBounds(x, y)) return 0xFF000000;
    return GetPixelUnsafe(x, y);
}

uint32_t Graphics::GetPixelUnsafe(int x, int y) {
    if (!active_buffer) return 0xFF000000;
    
    uint32_t offset = y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        return *(volatile uint32_t*)(active_buffer + offset);
    } else if (fb_bpp == 24) {
        volatile uint8_t* p = active_buffer + offset;
        uint8_t b = p[0];
        uint8_t g = p[1];
        uint8_t r = p[2];
        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    } else if (fb_bpp == 16) {
        uint16_t v = *(volatile uint16_t*)(active_buffer + offset);
        uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
        uint8_t b = (uint8_t)((v & 0x1F) << 3);
        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    }
    return 0xFF000000;
}

void Graphics::FillRect(int x, int y, int w, int h, uint32_t color) {
    ClipRect(x, y, w, h);
    if (w <= 0 || h <= 0) return;
    
    uint8_t alpha = (color >> 24) & 0xFF;
    if (fb_bpp == 32 && (blend_mode == BLEND_NONE || alpha == 0xFF)) {
        // Fast path for 32-bit opaque fills — direct memory write
        for (int j = 0; j < h; j++) {
            uint32_t* row = (uint32_t*)(active_buffer + (y + j) * fb_pitch + x * 4);
            for (int i = 0; i < w; i++) {
                row[i] = color;
            }
        }
    } else if (alpha == 0) {
        return; // Fully transparent — skip
    } else {
        // General case (blended)
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                DrawPixel(x + i, y + j, color);
            }
        }
    }
    
    MarkDirty(x, y, w, h);
}

void Graphics::DrawLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    if (thickness <= 1) {
        DrawLineBresenham(x0, y0, x1, y1, color);
    } else {
        DrawThickLine(x0, y0, x1, y1, color, thickness);
    }
    
    // Mark dirty region
    int min_x = x0 < x1 ? x0 : x1;
    int max_x = x0 > x1 ? x0 : x1;
    int min_y = y0 < y1 ? y0 : y1;
    int max_y = y0 > y1 ? y0 : y1;
    MarkDirty(min_x - thickness, min_y - thickness, 
             (max_x - min_x) + 2*thickness, (max_y - min_y) + 2*thickness);
}

void Graphics::DrawLineBresenham(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    while (true) {
        DrawPixel(x0, y0, color);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void Graphics::DrawThickLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    // Draw multiple parallel lines for thickness
    for (int i = -thickness/2; i <= thickness/2; i++) {
        for (int j = -thickness/2; j <= thickness/2; j++) {
            DrawLineBresenham(x0 + i, y0 + j, x1 + i, y1 + j, color);
        }
    }
}

void Graphics::MarkDirty(int x, int y, int w, int h) {
    if (dirty_count >= 16) return; // Max regions reached
    
    DirtyRegion& region = dirty_regions[dirty_count++];
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    region.active = true;
}

void Graphics::ClearDirtyRegions() {
    dirty_count = 0;
    for (int i = 0; i < 16; i++) {
        dirty_regions[i].active = false;
    }
}

bool Graphics::IsPointInBounds(int x, int y) {
    if (clipping_enabled) {
        return x >= clip_x && x < clip_x + clip_w && y >= clip_y && y < clip_y + clip_h;
    }
    return x >= 0 && x < (int)fb_width && y >= 0 && y < (int)fb_height;
}

void Graphics::ClipRect(int& x, int& y, int& w, int& h) {
    int right = x + w;
    int bottom = y + h;
    
    int clip_right = clipping_enabled ? clip_x + clip_w : (int)fb_width;
    int clip_bottom = clipping_enabled ? clip_y + clip_h : (int)fb_height;
    int clip_left = clipping_enabled ? clip_x : 0;
    int clip_top = clipping_enabled ? clip_y : 0;
    
    if (x < clip_left) x = clip_left;
    if (y < clip_top) y = clip_top;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    
    w = right - x;
    h = bottom - y;
}

void Graphics::SetClipRect(int x, int y, int w, int h) {
    clip_x = x;
    clip_y = y;
    clip_w = w;
    clip_h = h;
    clipping_enabled = true;
}

void Graphics::ClearClipRect() {
    clipping_enabled = false;
}

uint32_t Graphics::RGB(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint32_t Graphics::RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t Graphics::BlendColors(uint32_t src, uint32_t dst, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    
    uint8_t src_r = (src >> 16) & 0xFF;
    uint8_t src_g = (src >> 8) & 0xFF;
    uint8_t src_b = src & 0xFF;
    
    uint8_t dst_r = (dst >> 16) & 0xFF;
    uint8_t dst_g = (dst >> 8) & 0xFF;
    uint8_t dst_b = dst & 0xFF;
    
    uint8_t inv_alpha = 255 - alpha;
    uint8_t out_r = (src_r * alpha + dst_r * inv_alpha) / 255;
    uint8_t out_g = (src_g * alpha + dst_g * inv_alpha) / 255;
    uint8_t out_b = (src_b * alpha + dst_b * inv_alpha) / 255;
    
    return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
}

// Keep the rest of the existing methods for compatibility
void Graphics::BlendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    if (a == 255) { DrawPixel(x, y, RGB(r, g, b)); return; }
    uint32_t dst = ReadPixel(x, y);
    uint32_t src = RGBA(r, g, b, a);
    uint32_t blended = BlendColors(src, dst, a);
    DrawPixelUnsafe(x, y, blended);
}

void Graphics::FillRectRounded(int x, int y, int w, int h, int r, uint32_t color) {
    if (r <= 0) { FillRect(x, y, w, h, color); return; }
    FillRect(x + r, y, w - 2 * r, h, color);
    FillRect(x, y + r, r, h - 2 * r, color);
    FillRect(x + w - r, y + r, r, h - 2 * r, color);
    
    // Draw corners (simplified)
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            if ((dx * dx + dy * dy) <= (r * r)) {
                DrawPixel(x + r - 1 - dx, y + r - 1 - dy, color); // TL
                DrawPixel(x + w - r + dx, y + r - 1 - dy, color); // TR
                DrawPixel(x + r - 1 - dx, y + h - r + dy, color); // BL
                DrawPixel(x + w - r + dx, y + h - r + dy, color); // BR
            }
        }
    }
}

void Graphics::FillRectAlpha(int x, int y, int w, int h, uint8_t a, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            BlendPixel(x + i, y + j, r, g, b, a);
        }
    }
}

void Graphics::FillGradientV(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    uint8_t tr = (top >> 16) & 0xFF;
    uint8_t tg = (top >> 8) & 0xFF;
    uint8_t tb = top & 0xFF;
    uint8_t br = (bottom >> 16) & 0xFF;
    uint8_t bg = (bottom >> 8) & 0xFF;
    uint8_t bb = bottom & 0xFF;
    for (int j = 0; j < h; j++) {
        uint32_t r = tr + (br - tr) * j / (h ? h : 1);
        uint32_t g = tg + (bg - tg) * j / (h ? h : 1);
        uint32_t b = tb + (bb - tb) * j / (h ? h : 1);
        uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
        for (int i = 0; i < w; i++) DrawPixel(x + i, y + j, c);
    }
}

void Graphics::BlurRect(int x, int y, int w, int h, int radius) {
    // Simplified box blur - just average neighboring pixels
    if (radius < 1) return;
    ClipRect(x, y, w, h);
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint32_t r = 0, g = 0, b = 0, count = 0;
            for (int ky = -radius; ky <= radius; ky++) {
                for (int kx = -radius; kx <= radius; kx++) {
                    int px = x + i + kx;
                    int py = y + j + ky;
                    if (IsPointInBounds(px, py)) {
                         uint32_t c = ReadPixel(px, py);
                         r += (c >> 16) & 0xFF;
                         g += (c >> 8) & 0xFF;
                         b += c & 0xFF;
                         count++;
                    }
                }
            }
            if (count) {
                DrawPixel(x + i, y + j, RGB(r/count, g/count, b/count));
            }
        }
    }
}

int Graphics::GetWidth() { return fb_width; }
int Graphics::GetHeight() { return fb_height; }
uint32_t Graphics::GetPitch() { return fb_pitch; }
uint8_t Graphics::GetBpp() { return fb_bpp; }
uint8_t* Graphics::GetBuffer() { return active_buffer; }
void Graphics::SetBuffer(uint8_t* addr) { active_buffer = addr; }
const Graphics::DrawStats& Graphics::GetDrawStats() { return draw_stats; }

// ═══════════════════════════════════════════════════════════════════════════
//  DrawString — convenience text renderer for desktop/app layers
//  Font size scales with resolution: 16px at 1024x768, 16px at 1080p,
//  20px at 1440p for readable text at every density.
// ═══════════════════════════════════════════════════════════════════════════
void Graphics::DrawString(int x, int y, const char* s, uint32_t fg, uint32_t /*bg*/) {
    if (!s) return;
    // Scale font: base 16px for <=1080p, 20px for 1440p+
    float pxh = 16.0f;
    if (fb_height >= 1440) pxh = 20.0f;
    FontTTF::DrawString(x, y, pxh, s, fg);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DrawCircle — Midpoint circle algorithm
// ═══════════════════════════════════════════════════════════════════════════
void Graphics::DrawCircle(int cx, int cy, int radius, uint32_t color, bool filled) {
    if (radius <= 0) { DrawPixel(cx, cy, color); return; }

    int x = 0, y = radius;
    int d = 1 - radius;

    auto hline = [&](int lx, int rx, int ly) {
        if (ly < 0 || ly >= (int)fb_height) return;
        if (lx < 0) lx = 0;
        if (rx >= (int)fb_width) rx = (int)fb_width - 1;
        for (int i = lx; i <= rx; i++) DrawPixel(i, ly, color);
    };

    while (x <= y) {
        if (filled) {
            hline(cx - x, cx + x, cy + y);
            hline(cx - x, cx + x, cy - y);
            hline(cx - y, cx + y, cy + x);
            hline(cx - y, cx + y, cy - x);
        } else {
            DrawPixel(cx + x, cy + y, color);
            DrawPixel(cx - x, cy + y, color);
            DrawPixel(cx + x, cy - y, color);
            DrawPixel(cx - x, cy - y, color);
            DrawPixel(cx + y, cy + x, color);
            DrawPixel(cx - y, cy + x, color);
            DrawPixel(cx + y, cy - x, color);
            DrawPixel(cx - y, cy - x, color);
        }
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}
