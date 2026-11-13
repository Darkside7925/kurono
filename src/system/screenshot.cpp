#include "screenshot.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"
#include "../ui/window_manager.h"
#include "png_encode.h"

namespace {

// little-endian scalar writers - freestanding, no libc. (satoru)
void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// minimal unsigned itoa into buf (cap includes nul); returns length. (satoru)
int u32_to_str(uint32_t v, char* buf, int cap) {
    if (cap <= 0) return 0;
    char tmp[16];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int out = 0;
    while (n > 0 && out < cap - 1) buf[out++] = tmp[--n];
    buf[out] = 0;
    return out;
}

// append src to dst at *pos, bounded by cap (keeps nul). (satoru)
void append_str(char* dst, int cap, int* pos, const char* src) {
    int p = *pos;
    while (*src && p < cap - 1) dst[p++] = *src++;
    dst[p] = 0;
    *pos = p;
}

constexpr uint32_t BMP_FILE_HDR = 14;   // BITMAPFILEHEADER
constexpr uint32_t BMP_INFO_HDR = 40;   // BITMAPINFOHEADER
constexpr uint32_t BMP_HDR_SIZE = BMP_FILE_HDR + BMP_INFO_HDR;  // 54

} // namespace

bool Screenshot::CaptureToBMP(const char* kvfs_path) {
    if (!kvfs_path) return false;

    uint8_t* fb = Graphics::GetBackBuffer();
    int      w  = Graphics::GetWidth();
    int      h  = Graphics::GetHeight();
    uint32_t pitch = Graphics::GetPitch();
    uint8_t  bpp = Graphics::GetBpp();
    if (!fb || w <= 0 || h <= 0 || pitch == 0) return false;

    const uint32_t src_bpp_bytes = (uint32_t)bpp / 8u;       // source pixel stride
    const uint32_t row_bytes      = (uint32_t)w * 4u;        // 32bpp dst, no padding (4-aligned)
    const uint32_t pixel_bytes    = row_bytes * (uint32_t)h;
    const uint32_t total          = BMP_HDR_SIZE + pixel_bytes;

    uint8_t* out = (uint8_t*)KernelHeap::Alloc(total);
    if (!out) return false;

    // BITMAPFILEHEADER (14 bytes) (satoru)
    out[0] = 'B'; out[1] = 'M';
    put_u32(out + 2,  total);          // bfSize
    put_u16(out + 6,  0);              // bfReserved1
    put_u16(out + 8,  0);              // bfReserved2
    put_u32(out + 10, BMP_HDR_SIZE);   // bfOffBits

    // BITMAPINFOHEADER (40 bytes) (satoru)
    uint8_t* ih = out + BMP_FILE_HDR;
    put_u32(ih + 0,  BMP_INFO_HDR);    // biSize
    put_u32(ih + 4,  (uint32_t)w);     // biWidth
    put_u32(ih + 8,  (uint32_t)h);     // biHeight (positive => bottom-up)
    put_u16(ih + 12, 1);               // biPlanes
    put_u16(ih + 14, 32);              // biBitCount
    put_u32(ih + 16, 0);               // biCompression = BI_RGB
    put_u32(ih + 20, pixel_bytes);     // biSizeImage
    put_u32(ih + 24, 2835);            // biXPelsPerMeter (~72 dpi)
    put_u32(ih + 28, 2835);            // biYPelsPerMeter
    put_u32(ih + 32, 0);               // biClrUsed
    put_u32(ih + 36, 0);               // biClrImportant

    // pixel data, bottom-up: BMP row 0 is the bottom screen row. (satoru)
    uint8_t* dst_base = out + BMP_HDR_SIZE;
    for (int y = 0; y < h; y++) {
        const uint8_t* src_row = fb + (size_t)(h - 1 - y) * pitch;
        uint8_t*       dst_row = dst_base + (size_t)y * row_bytes;
        if (bpp == 32) {
            // fb stores 0xAARRGGBB little-endian = bytes B,G,R,A; BMP 32bpp
            // wants exactly B,G,R,A - straight copy with forced opaque. (satoru)
            for (int x = 0; x < w; x++) {
                const uint8_t* s = src_row + (size_t)x * 4u;
                uint8_t* d = dst_row + (size_t)x * 4u;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0xFF;
            }
        } else if (bpp == 24) {
            for (int x = 0; x < w; x++) {
                const uint8_t* s = src_row + (size_t)x * 3u;
                uint8_t* d = dst_row + (size_t)x * 4u;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0xFF;
            }
        } else if (bpp == 16) {
            // RGB565 -> expand to 8-bit channels. (satoru)
            for (int x = 0; x < w; x++) {
                const uint8_t* s = src_row + (size_t)x * 2u;
                uint16_t v = (uint16_t)(s[0] | (s[1] << 8));
                uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
                uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
                uint8_t b = (uint8_t)((v & 0x1F) << 3);
                uint8_t* d = dst_row + (size_t)x * 4u;
                d[0] = b; d[1] = g; d[2] = r; d[3] = 0xFF;
            }
        } else {
            // unknown depth: emit opaque black so the file stays valid. (satoru)
            for (int x = 0; x < w; x++) {
                uint8_t* d = dst_row + (size_t)x * 4u;
                d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0xFF;
            }
        }
        (void)src_bpp_bytes;
    }

    int rc = KVFS::WriteFile(kvfs_path, out, total);
    KernelHeap::Free(out);
    return rc >= 0;
}

bool Screenshot::CaptureScaledToBMP(const char* kvfs_path, int max_dim) {
    if (!kvfs_path || max_dim < 16) return false;
    uint8_t* fb = Graphics::GetBackBuffer();
    int      w  = Graphics::GetWidth();
    int      h  = Graphics::GetHeight();
    uint32_t pitch = Graphics::GetPitch();
    if (!fb || w <= 0 || h <= 0 || pitch == 0) return false;
    int dw = w, dh = h;
    if (w >= h && w > max_dim) { dw = max_dim; dh = (h * max_dim) / w; }
    else if (h > max_dim)      { dh = max_dim; dw = (w * max_dim) / h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    const uint32_t row_bytes = (uint32_t)dw * 4u;
    const uint32_t pixel_bytes = row_bytes * (uint32_t)dh;
    const uint32_t total = BMP_HDR_SIZE + pixel_bytes;
    uint8_t* out = (uint8_t*)KernelHeap::Alloc(total);
    if (!out) return false;
    out[0] = 'B'; out[1] = 'M';
    put_u32(out + 2,  total);
    put_u16(out + 6,  0);
    put_u16(out + 8,  0);
    put_u32(out + 10, BMP_HDR_SIZE);
    uint8_t* ih = out + BMP_FILE_HDR;
    put_u32(ih + 0,  BMP_INFO_HDR);
    put_u32(ih + 4,  (uint32_t)dw);
    put_u32(ih + 8,  (uint32_t)dh);
    put_u16(ih + 12, 1);
    put_u16(ih + 14, 32);
    put_u32(ih + 16, 0);
    put_u32(ih + 20, pixel_bytes);
    put_u32(ih + 24, 2835);
    put_u32(ih + 28, 2835);
    put_u32(ih + 32, 0);
    put_u32(ih + 36, 0);
    uint8_t* dst_base = out + BMP_HDR_SIZE;
    for (int y = 0; y < dh; y++) {
        int sy = (int)(((int64_t)(h - 1 - y) * h) / dh);
        if (sy < 0) sy = 0;
        if (sy >= h) sy = h - 1;
        const uint8_t* src_row = fb + (size_t)sy * pitch;
        uint8_t* dst_row = dst_base + (size_t)y * row_bytes;
        for (int x = 0; x < dw; x++) {
            int sx = (int)(((int64_t)x * w) / dw);
            if (sx < 0) sx = 0;
            if (sx >= w) sx = w - 1;
            const uint8_t* s = src_row + (size_t)sx * 4u;
            uint8_t* d = dst_row + (size_t)x * 4u;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0xFF;
        }
    }
    int rc = KVFS::WriteFile(kvfs_path, out, total);
    KernelHeap::Free(out);
    return rc >= 0;
}

bool Screenshot::CaptureTimestamped() {
    // build "/home/user/screenshot_<ms>.bmp" from the monotonic clock. (satoru)
    char path[64];
    int pos = 0;
    append_str(path, (int)sizeof(path), &pos, "/home/user/screenshot_");
    char num[16];
    u32_to_str(Timer::GetRealMs(), num, (int)sizeof(num));
    append_str(path, (int)sizeof(path), &pos, num);
    append_str(path, (int)sizeof(path), &pos, ".bmp");
    return CaptureToBMP(path);
}

bool Screenshot::CaptureRegionPNG(const char* kvfs_path, int x, int y, int w, int h) {
    if (!kvfs_path) return false;
    uint8_t* fb = Graphics::GetBackBuffer();
    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    uint32_t pitch = Graphics::GetPitch();
    int bpp = Graphics::GetBpp();
    if (!fb || sw <= 0 || sh <= 0 || pitch == 0) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return false;
    uint32_t raw_n = PngEncode::RawSize(w, h);
    uint32_t cap = PngEncode::MaxSize(w, h);
    uint8_t* raw = (uint8_t*)KernelHeap::Alloc(raw_n);
    uint8_t* out = raw ? (uint8_t*)KernelHeap::Alloc(cap) : nullptr;
    if (!raw || !out) {
        if (raw) KernelHeap::Free(raw);
        return false;
    }
    const uint8_t* src = fb + (size_t)y * pitch + (size_t)x * (size_t)(bpp / 8);
    uint32_t n = PngEncode::Encode(src, w, h, pitch, bpp, raw, out, cap);
    KernelHeap::Free(raw);
    int rc = n ? KVFS::WriteFile(kvfs_path, out, n) : -1;
    KernelHeap::Free(out);
    return rc >= 0;
}

static volatile int g_shot_request = 0;

void Screenshot::Request(bool window_only) {
    g_shot_request = window_only ? 2 : 1;
    Graphics::MarkUIDirty();   // an idle desktop loop may be asleep (satoru)
}

int Screenshot::TakeRequest() { return __atomic_exchange_n(&g_shot_request, 0, __ATOMIC_ACQ_REL); }

static void two_digits(char* d, int v) {
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

bool Screenshot::CaptureToPictures(bool window_only, char* out_path, int out_cap) {
    int x = 0, y = 0, w = Graphics::GetWidth(), h = Graphics::GetHeight();
    if (window_only) {
        Window* fw = WindowManager::GetFocusedWindow();
        if (!fw || fw->state == WIN_CLOSED || fw->state == WIN_MINIMIZED || !fw->visible) return false;
        x = fw->x; y = fw->y; w = fw->w; h = fw->h;
    }
    KVFS::Mkdirs("/home/user/Pictures");
    // Screenshot_YYYY-MM-DD_HH-MM-SS.png in local time, -2, -3 ... on a clash. (satoru)
    DateTime dt = TimeManager::NowLocalDateTime();
    char base[64];
    int pos = 0;
    append_str(base, (int)sizeof(base), &pos, "/home/user/Pictures/Screenshot_");
    char num[16];
    u32_to_str(dt.year, num, (int)sizeof(num));
    append_str(base, (int)sizeof(base), &pos, num);
    char part[16] = "-MM-DD_hh-mm-ss";
    two_digits(part + 1, dt.mon); two_digits(part + 4, dt.dom);
    two_digits(part + 7, dt.h); two_digits(part + 10, dt.m); two_digits(part + 13, dt.s);
    append_str(base, (int)sizeof(base), &pos, part);
    char path[80];
    for (int k = 1; k < 100; k++) {
        int pp = 0;
        path[0] = 0;
        append_str(path, (int)sizeof(path), &pp, base);
        if (k > 1) {
            append_str(path, (int)sizeof(path), &pp, "-");
            u32_to_str((uint32_t)k, num, (int)sizeof(num));
            append_str(path, (int)sizeof(path), &pp, num);
        }
        append_str(path, (int)sizeof(path), &pp, ".png");
        if (!KVFS::Resolve(path)) break;
    }
    if (out_path && out_cap > 0) {
        int op = 0;
        out_path[0] = 0;
        append_str(out_path, out_cap, &op, path);
    }
    return CaptureRegionPNG(path, x, y, w, h);
}

// end (satoru)
