#include "gui.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"

#include "../kernel/time.h"
#include "../drivers/rtc.h"
#include "font.h"

MediaDecoder::Image GUI::wallpaper;
uint8_t* GUI::backbuffer = nullptr;
uint8_t* GUI::wallpaper_buffer = nullptr;
size_t   GUI::buffer_size = 0;

#include "../kernel/types.h"

// helper memset to avoid dependency on libc if not available in this context
// although we have kernelheap, we might not have a global memset exposed here.
// but wait, types.h declares memset.
extern "C" void* memset(void* dst, int v, size_t n);

#include "../drivers/serial.h"

void GUI::UpdateBackbuffer() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    int bpp = Graphics::GetBpp();
    int pitch = Graphics::GetPitch();
    
    // log once
    static bool logged = false;
    if (!logged) {
        SerialLogger::Log("GUI::UpdateBackbuffer: w="); SerialLogger::LogDec(w);
        SerialLogger::Log(" h="); SerialLogger::LogDec(h);
        SerialLogger::Log(" bpp="); SerialLogger::LogDec(bpp);
        SerialLogger::Log(" pitch="); SerialLogger::LogDec(pitch);
        SerialLogger::Log("\r\n");
        logged = true;
    }

    if (!backbuffer) {
        // allocate using pitch to match framebuffer layout exactly (pmm for large bufs)
        size_t sz = (size_t)h * pitch;
        backbuffer = (uint8_t*)PMM::AllocBytes(sz);
        buffer_size = sz;
    }
    if (!wallpaper_buffer) {
        size_t sz = (size_t)h * pitch;
        wallpaper_buffer = (uint8_t*)PMM::AllocBytes(sz);
        
        // draw wallpaper to wallpaper_buffer once
        uint8_t* old_fb = Graphics::GetBuffer();
        Graphics::SetBuffer(wallpaper_buffer);
        if (wallpaper.valid) {
            SerialLogger::Log("GUI: Drawing wallpaper...\r\n");
            // use standard scaling, no auto-crop for wallpaper to avoid artifacts
            MediaDecoder::DrawImageScaled(wallpaper, 0, 0, w, h);
        } else {
            // no wallpaper  -  draw a beautiful gradient background
            // deep midnight blue (#0d1b2a) at top → dark teal (#1b2838) at bottom
            // with a subtle purple accent in the middle
            for (int y = 0; y < h; y++) {
                float t = (float)y / (float)(h > 1 ? h - 1 : 1);
                // top color: dark blue-black (13, 27, 42)
                // mid color: muted purple  (30, 25, 50) at t=0.4
                // bot color: dark teal     (27, 40, 56) 
                uint8_t r, g, b;
                if (t < 0.4f) {
                    float s = t / 0.4f;
                    r = (uint8_t)(13 + (30 - 13) * s);
                    g = (uint8_t)(27 + (25 - 27) * s);
                    b = (uint8_t)(42 + (50 - 42) * s);
                } else {
                    float s = (t - 0.4f) / 0.6f;
                    r = (uint8_t)(30 + (27 - 30) * s);
                    g = (uint8_t)(25 + (40 - 25) * s);
                    b = (uint8_t)(50 + (56 - 50) * s);
                }
                uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                for (int x = 0; x < w; x++) {
                    // write directly to wallpaper_buffer
                    uint32_t* row = (uint32_t*)(wallpaper_buffer + y * pitch);
                    row[x] = color;
                }
            }
            SerialLogger::Log("GUI: Gradient wallpaper generated\r\n");
        }
        Graphics::SetBuffer(old_fb);
    }
    
    // optimization: only update if dirty. 
    // for now, we only need to restore the wallpaper buffer to backbuffer if we are redrawing the whole frame.
    // if the gui loop is drawing ui on top of backbuffer, we need a clean slate every frame.
    // copy wallpaper_buffer to backbuffer (fast)
    if (wallpaper_buffer && backbuffer) {
        // since both are pitch-aligned and same size, we can just memcpy
        // but we use uint32 copy for speed if aligned
        int size = h * pitch;
        uint32_t* src = (uint32_t*)wallpaper_buffer;
        uint32_t* dst = (uint32_t*)backbuffer;
        int n = size / 4;
        
        // manual unrolling for speed
        int i = 0;
        for(; i < n - 8; i+=8) {
            dst[i] = src[i]; dst[i+1] = src[i+1]; dst[i+2] = src[i+2]; dst[i+3] = src[i+3];
            dst[i+4] = src[i+4]; dst[i+5] = src[i+5]; dst[i+6] = src[i+6]; dst[i+7] = src[i+7];
        }
        for(; i < n; i++) dst[i] = src[i];
    }
}

void GUI::ReinitBuffers() {
    // free old buffers (pmm-backed)
    if (backbuffer)        { PMM::FreeBytes(backbuffer, buffer_size);        backbuffer = nullptr; }
    if (wallpaper_buffer)  { PMM::FreeBytes(wallpaper_buffer, buffer_size); wallpaper_buffer = nullptr; }
    buffer_size = 0;
    // updatebackbuffer will reallocate with new dimensions
    UpdateBackbuffer();
}

void GUI::SetWallpaper(const MediaDecoder::Image& img) { 
    wallpaper = img;
    // invalidate wallpaper buffer
    if (wallpaper_buffer) {
        int w = Graphics::GetWidth();
        int h = Graphics::GetHeight();
        int pitch = Graphics::GetPitch();
        memset(wallpaper_buffer, 0, h * pitch); // clear first
        
        uint8_t* old_fb = Graphics::GetBuffer();
        Graphics::SetBuffer(wallpaper_buffer);
        if (wallpaper.valid) {
            MediaDecoder::DrawImageScaled(wallpaper, 0, 0, w, h);
        } else {
            Graphics::Clear(0xFF000000);
        }
        Graphics::SetBuffer(old_fb);
    }
    UpdateBackbuffer();
}

void GUI::DrawTime(int x, int y) {
    auto dt_loc = TimeManager::NowLocalDateTime();
    RTC::Time t; t.h = dt_loc.h; t.m = dt_loc.m; t.s = dt_loc.s;
    char timebuf[8];
    timebuf[0] = (char)('0' + (t.h/10));
    timebuf[1] = (char)('0' + (t.h%10));
    timebuf[2] = ':';
    timebuf[3] = (char)('0' + (t.m/10));
    timebuf[4] = (char)('0' + (t.m%10));
    timebuf[5] = 0;
    
    if (FontTTF::ok) {
        FontTTF::DrawString(x, y, 20.0f, timebuf, 0xFFFFFFFF);
    }
}

void GUI::DrawTaskbar() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    // taskbar at bottom
    int tb_h = 40;
    int tb_y = h - tb_h;
    
    // draw directly to backbuffer (updatebackbuffer should have been called)
    uint8_t* old = Graphics::GetBuffer();
    if (backbuffer) Graphics::SetBuffer(backbuffer);
    
    // semi-transparent black background
    Graphics::FillRectAlpha(0, tb_y, w, tb_h, 128, 0x00000000);
    Graphics::FillRect(0, tb_y, w, 1, 0xFF444444); // top border
    
    // draw time
    DrawTime(w - 100, tb_y + 10);
    
    if (backbuffer) Graphics::SetBuffer(old);
}

void GUI::BlurWallpaper() {
    if (!wallpaper_buffer) return;
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    
    // use simple dimming instead of blur for now to avoid artifacts/lag
    uint8_t* old = Graphics::GetBuffer();
    Graphics::SetBuffer(wallpaper_buffer);
    Graphics::FillRectAlpha(0, 0, w, h, 100, 0xFF000000); // darken
    Graphics::SetBuffer(old);
    UpdateBackbuffer();
}

void GUI::DrawDesktop() {
    if (!backbuffer) UpdateBackbuffer();
    
    uint8_t* fb = Graphics::GetBuffer();
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    int pitch = Graphics::GetPitch();
    
    // copy backbuffer to screen
    int size = h * pitch;
    uint32_t* src = (uint32_t*)backbuffer;
    uint32_t* dst = (uint32_t*)fb;
    int n = size / 4;
    
    // manual unrolling for speed
    int i = 0;
    for(; i < n - 8; i+=8) {
        dst[i] = src[i]; dst[i+1] = src[i+1]; dst[i+2] = src[i+2]; dst[i+3] = src[i+3];
        dst[i+4] = src[i+4]; dst[i+5] = src[i+5]; dst[i+6] = src[i+6]; dst[i+7] = src[i+7];
    }
    for(; i < n; i++) dst[i] = src[i];

    // drawdesktop does a full-frame blit, so swapbuffers must do a full copy.
    // clear any stale dirty regions (accumulated while drawing to gui::backbuffer)
    // then mark the whole screen so swapbuffers falls through to full-frame copy.
    Graphics::ClearDirtyRegions();
    Graphics::MarkDirty(0, 0, w, h);
}

void GUI::DrawRegion(int x, int y, int w, int h) {
    if (!backbuffer) return;
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    int pitch = Graphics::GetPitch();
    int bpp = Graphics::GetBpp();
    uint8_t* fb = Graphics::GetBuffer();
    
    // clip
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    
    for (int j = 0; j < h; j++) {
        int ly = y + j; 
        int py = ly; 
        
        uint32_t offset = py * pitch + x * (bpp / 8);
        
        if (bpp == 32) {
             uint32_t* src = (uint32_t*)(backbuffer + offset);
             uint32_t* dst = (uint32_t*)(fb + offset);
             for(int k=0; k<w; k++) dst[k] = src[k];
        } else {
             uint8_t* src = backbuffer + offset;
             uint8_t* dst = fb + offset;
             int bytes = w * (bpp / 8);
             for(int k=0; k<bytes; k++) dst[k] = src[k];
        }
    }
}

void GUI::RestoreRegion(int x, int y, int w, int h) {
    if (!backbuffer) return;
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    int pitch = Graphics::GetPitch();
    int bpp = Graphics::GetBpp();
    (void)bpp;
    uint8_t* fb = Graphics::GetBuffer();
    
    // clip
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    
    // copy from backbuffer to fb
    // assuming 32bpp and pitch aligned
    for (int j = 0; j < h; j++) {
        int ly = y + j; 
        uint32_t offset = ly * pitch + x * 4;
        
        uint32_t* src = (uint32_t*)(backbuffer + offset);
        uint32_t* dst = (uint32_t*)(fb + offset);
        
        // manual unroll 
        int i = 0;
        for (; i < w - 4; i += 4) {
             dst[i] = src[i]; dst[i+1] = src[i+1]; dst[i+2] = src[i+2]; dst[i+3] = src[i+3];
        }
        for (; i < w; i++) dst[i] = src[i];
    }
}

void GUI::DrawLogin() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    Graphics::Clear(0xFF000000);
    int panel_w = w * 4 / 10; if (panel_w < 320) panel_w = 320; if (panel_w > w - 120) panel_w = w - 120;
    int panel_h = h * 3 / 10; if (panel_h < 220) panel_h = 220; if (panel_h > h - 120) panel_h = h - 120;
    int px = (w - panel_w) / 2;
    int py = (h - panel_h) / 2;
    Graphics::FillRect(px, py, panel_w, panel_h, 0xFF121212);
    int b = 2;
    Graphics::FillRect(px, py, panel_w, b, 0xFFCCCCCC);
    Graphics::FillRect(px, py + panel_h - b, panel_w, b, 0xFFCCCCCC);
    Graphics::FillRect(px, py, b, panel_h, 0xFFCCCCCC);
    Graphics::FillRect(px + panel_w - b, py, b, panel_h, 0xFFCCCCCC);
    int title_h = 30;
    Graphics::FillRect(px, py, panel_w, title_h, 0xFF1D1D1D);
    int field_w = panel_w - 40;
    int field_h = 28;
    int fx = px + 20;
    int fy1 = py + title_h + 24;
    int fy2 = fy1 + field_h + 24;
    Graphics::FillRect(fx, fy1, field_w, field_h, 0xFF0F0F0F);
    Graphics::FillRect(fx, fy2, field_w, field_h, 0xFF0F0F0F);
    Graphics::FillRect(fx, fy1, field_w, 1, 0xFF777777);
    Graphics::FillRect(fx, fy1 + field_h - 1, field_w, 1, 0xFF777777);
    Graphics::FillRect(fx, fy2, field_w, 1, 0xFF777777);
    Graphics::FillRect(fx, fy2 + field_h - 1, field_w, 1, 0xFF777777);
    int btn_w = 90;
    int btn_h = 28;
    int bx = px + panel_w - btn_w - 20;
    int by = py + panel_h - btn_h - 20;
    Graphics::FillRect(bx, by, btn_w, btn_h, 0xFF2B2B2B);
    Graphics::FillRect(bx, by, btn_w, 1, 0xFF777777);
    Graphics::FillRect(bx, by + btn_h - 1, btn_w, 1, 0xFF777777);
    Graphics::FillRect(bx, by, 1, btn_h, 0xFF777777);
    Graphics::FillRect(bx + btn_w - 1, by, 1, btn_h, 0xFF777777);
}
