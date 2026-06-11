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

extern "C" void* memset(void* dst, int v, size_t n);

#include "../drivers/serial.h"

// fast 32-bit dword copy. handles non-aligned tails. n is dword count.
static inline void dword_copy(uint32_t* __restrict dst, const uint32_t* __restrict src, size_t n) {
    size_t i = 0;
    size_t unroll_end = n & ~(size_t)7;
    for (; i < unroll_end; i += 8) {
        dst[i+0] = src[i+0]; dst[i+1] = src[i+1];
        dst[i+2] = src[i+2]; dst[i+3] = src[i+3];
        dst[i+4] = src[i+4]; dst[i+5] = src[i+5];
        dst[i+6] = src[i+6]; dst[i+7] = src[i+7];
    }
    for (; i < n; i++) dst[i] = src[i];
}

void GUI::UpdateBackbuffer() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    int bpp = Graphics::GetBpp();
    int pitch = Graphics::GetPitch();

    if (w <= 0 || h <= 0 || pitch <= 0) return;

    static bool logged = false;
    if (!logged) {
        SerialLogger::Log("GUI::UpdateBackbuffer: w="); SerialLogger::LogDec(w);
        SerialLogger::Log(" h="); SerialLogger::LogDec(h);
        SerialLogger::Log(" bpp="); SerialLogger::LogDec(bpp);
        SerialLogger::Log(" pitch="); SerialLogger::LogDec(pitch);
        SerialLogger::Log("\r\n");
        logged = true;
    }

    size_t sz = (size_t)h * (size_t)pitch;

    // resolution changed: the existing buffers are sized for the old mode, so
    // free them and reallocate at the new size. otherwise the compositor draws
    // an old-size image into the larger framebuffer and the unwritten remainder
    // shows uninitialized vram -- the "colors everywhere" corruption. (satoru)
    if (backbuffer && buffer_size != sz) {
        PMM::FreeBytes(backbuffer, buffer_size);
        backbuffer = nullptr;
        if (wallpaper_buffer) { PMM::FreeBytes(wallpaper_buffer, buffer_size); wallpaper_buffer = nullptr; }
        buffer_size = 0;
    }

    if (!backbuffer) {
        backbuffer = (uint8_t*)PMM::AllocBytes(sz);
        buffer_size = sz;
    }
    if (!wallpaper_buffer) {
        wallpaper_buffer = (uint8_t*)PMM::AllocBytes(sz);

        if (!wallpaper_buffer) return;

        uint8_t* old_fb = Graphics::GetBuffer();
        Graphics::SetBuffer(wallpaper_buffer);
        if (wallpaper.valid) {
            SerialLogger::Log("GUI: Drawing wallpaper...\r\n");
            MediaDecoder::DrawImageScaled(wallpaper, 0, 0, w, h);
        } else {
            // precompute a small (64-step) vertical gradient ramp once,
            // then row-fill at integer precision. avoids per-pixel float math.
            const int RAMP_N = 64;
            uint32_t ramp[RAMP_N];
            // colours (B,G,R packed in 0xAARRGGBB)
            //   t=0.0 → (13, 27, 42)
            //   t=0.4 → (30, 25, 50)
            //   t=1.0 → (27, 40, 56)
            for (int i = 0; i < RAMP_N; i++) {
                int t256 = (i * 256) / (RAMP_N - 1);   // 0..256
                int r, g, b;
                if (t256 < 102) {                        // 0..0.4
                    int s = (t256 * 256) / 102;          // 0..256
                    r = 13 + ((30 - 13) * s) / 256;
                    g = 27 + ((25 - 27) * s) / 256;
                    b = 42 + ((50 - 42) * s) / 256;
                } else {
                    int s = ((t256 - 102) * 256) / 154;  // 0..256
                    r = 30 + ((27 - 30) * s) / 256;
                    g = 25 + ((40 - 25) * s) / 256;
                    b = 50 + ((56 - 50) * s) / 256;
                }
                ramp[i] = 0xFF000000u
                        | ((uint32_t)(r & 0xFF) << 16)
                        | ((uint32_t)(g & 0xFF) <<  8)
                        |  (uint32_t)(b & 0xFF);
            }
            int hsafe = h > 1 ? h - 1 : 1;
            for (int y = 0; y < h; y++) {
                int ramp_idx = (y * (RAMP_N - 1)) / hsafe;
                if (ramp_idx < 0) ramp_idx = 0;
                if (ramp_idx >= RAMP_N) ramp_idx = RAMP_N - 1;
                uint32_t color = ramp[ramp_idx];
                uint32_t* row = (uint32_t*)(wallpaper_buffer + (size_t)y * (size_t)pitch);
                int x = 0;
                int end = w & ~7;
                for (; x < end; x += 8) {
                    row[x+0] = color; row[x+1] = color;
                    row[x+2] = color; row[x+3] = color;
                    row[x+4] = color; row[x+5] = color;
                    row[x+6] = color; row[x+7] = color;
                }
                for (; x < w; x++) row[x] = color;
            }
            SerialLogger::Log("GUI: Gradient wallpaper generated\r\n");
        }
        Graphics::SetBuffer(old_fb);
    }

    if (wallpaper_buffer && backbuffer && bpp == 32) {
        dword_copy((uint32_t*)backbuffer, (uint32_t*)wallpaper_buffer, sz / 4);
    } else if (wallpaper_buffer && backbuffer) {
        // byte fallback for non-32bpp
        uint8_t* d = backbuffer; const uint8_t* s = wallpaper_buffer;
        for (size_t i = 0; i < sz; i++) d[i] = s[i];
    }
}

void GUI::ReinitBuffers() {
    if (backbuffer)        { PMM::FreeBytes(backbuffer, buffer_size);        backbuffer = nullptr; }
    if (wallpaper_buffer)  { PMM::FreeBytes(wallpaper_buffer, buffer_size); wallpaper_buffer = nullptr; }
    buffer_size = 0;
    UpdateBackbuffer();
}

void GUI::SetWallpaper(const MediaDecoder::Image& img) {
    wallpaper = img;
    if (wallpaper_buffer) {
        int w = Graphics::GetWidth();
        int h = Graphics::GetHeight();
        int pitch = Graphics::GetPitch();
        if (w <= 0 || h <= 0 || pitch <= 0) return;
        size_t sz = (size_t)h * (size_t)pitch;
        memset(wallpaper_buffer, 0, sz);

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
    // hh:mm + null = 6 chars
    char timebuf[6];
    timebuf[0] = (char)('0' + ((t.h / 10) % 10));
    timebuf[1] = (char)('0' + (t.h % 10));
    timebuf[2] = ':';
    timebuf[3] = (char)('0' + ((t.m / 10) % 10));
    timebuf[4] = (char)('0' + (t.m % 10));
    timebuf[5] = 0;

    if (FontTTF::ok) {
        FontTTF::DrawString(x, y, 20.0f, timebuf, 0xFFFFFFFF);
    }
}

void GUI::DrawTaskbar() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    int tb_h = 40;
    int tb_y = h - tb_h;
    if (tb_y < 0) return;

    uint8_t* old = Graphics::GetBuffer();
    if (backbuffer) Graphics::SetBuffer(backbuffer);

    Graphics::FillRectAlpha(0, tb_y, w, tb_h, 128, 0x00000000);
    Graphics::FillRect(0, tb_y, w, 1, 0xFF444444);

    DrawTime(w - 100, tb_y + 10);

    if (backbuffer) Graphics::SetBuffer(old);
}

void GUI::BlurWallpaper() {
    if (!wallpaper_buffer) return;
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();

    uint8_t* old = Graphics::GetBuffer();
    Graphics::SetBuffer(wallpaper_buffer);
    Graphics::FillRectAlpha(0, 0, w, h, 100, 0xFF000000);
    Graphics::SetBuffer(old);
    UpdateBackbuffer();
}

void GUI::DrawDesktop() {
    if (!backbuffer) UpdateBackbuffer();
    if (!backbuffer) return;

    uint8_t* fb = Graphics::GetBuffer();
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    int pitch = Graphics::GetPitch();
    if (!fb || w <= 0 || h <= 0 || pitch <= 0) return;

    size_t sz = (size_t)h * (size_t)pitch;
    dword_copy((uint32_t*)fb, (uint32_t*)backbuffer, sz / 4);

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
    if (!fb || bpp < 8) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    int bytes_pp = bpp / 8;
    for (int j = 0; j < h; j++) {
        int ly = y + j;
        size_t offset = (size_t)ly * (size_t)pitch + (size_t)x * (size_t)bytes_pp;
        if (bpp == 32) {
            uint32_t* src = (uint32_t*)(backbuffer + offset);
            uint32_t* dst = (uint32_t*)(fb + offset);
            dword_copy(dst, src, (size_t)w);
        } else {
            uint8_t* src = backbuffer + offset;
            uint8_t* dst = fb + offset;
            int bytes = w * bytes_pp;
            for (int k = 0; k < bytes; k++) dst[k] = src[k];
        }
    }
}

void GUI::RestoreRegion(int x, int y, int w, int h) {
    if (!backbuffer) return;
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    int pitch = Graphics::GetPitch();
    int bpp = Graphics::GetBpp();
    uint8_t* fb = Graphics::GetBuffer();
    if (!fb || bpp != 32) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        int ly = y + j;
        size_t offset = (size_t)ly * (size_t)pitch + (size_t)x * 4;
        uint32_t* src = (uint32_t*)(backbuffer + offset);
        uint32_t* dst = (uint32_t*)(fb + offset);
        dword_copy(dst, src, (size_t)w);
    }
}

void GUI::DrawLogin() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    Graphics::Clear(0xFF000000);
    int panel_w = w * 4 / 10; if (panel_w < 320) panel_w = 320; if (panel_w > w - 120) panel_w = w - 120;
    int panel_h = h * 3 / 10; if (panel_h < 220) panel_h = 220; if (panel_h > h - 120) panel_h = h - 120;
    if (panel_w <= 0 || panel_h <= 0) return;
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
