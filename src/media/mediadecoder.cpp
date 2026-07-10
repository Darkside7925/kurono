#include "mediadecoder.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ASSERT(x)
#define STBI_MALLOC KernelHeap::Alloc
#define STBI_REALLOC KernelHeap::Realloc
#define STBI_FREE KernelHeap::Free
#include "../third_party/stb_image.h"

bool MediaDecoder::IsPNG(const uint8_t* p, size_t n) {
    const uint8_t sig[8] = {0x89, 'P','N','G', 0x0D,0x0A,0x1A,0x0A};
    return n >= 8 && memcmp(p, sig, 8) == 0;
}
bool MediaDecoder::IsJPEG(const uint8_t* p, size_t n) {
    return n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}
bool MediaDecoder::IsWebP(const uint8_t* p, size_t n) {
    return n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0;
}

MediaDecoder::Image MediaDecoder::DecodeRaw(uint32_t data_addr) {
    uint32_t* header = (uint32_t*)data_addr;
    Image img = {0, 0, 0, false, 1, false};
    if (header[0] != 0x4F474F4C) return img;
    img.width = (int)header[1];
    img.height = (int)header[2];
    img.data = (uint8_t*)(data_addr + 12);
    img.valid = true;
    SerialLogger::Log("MediaDecoder: RAW Image Decoded ");
    SerialLogger::LogHex(img.width); SerialLogger::Log("x"); SerialLogger::LogHex(img.height); SerialLogger::Log("\r\n");
    return img;
}

MediaDecoder::Image MediaDecoder::DecodeModule(uint32_t start, uint32_t end) {
    size_t size = (size_t)(end - start);
    const uint8_t* data = (const uint8_t*)start;
    Image img = {0,0,0,false,0,false};
    if (IsPNG(data, size) || IsJPEG(data, size)) {
        int w=0,h=0,comp=0;
        int ok = stbi_info_from_memory(data, (int)size, &w, &h, &comp);
        if (!ok) {
            SerialLogger::Log("MediaDecoder: stbi_info failed\r\n");
        }
        stbi_uc* rgba = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
        if (rgba) {
            img.width = w; img.height = h; img.data = (uint8_t*)rgba; img.valid = true; img.order = 0; img.owns = true;
            SerialLogger::Log("MediaDecoder: PNG/JPEG Decoded ");
            SerialLogger::LogHex(img.width); SerialLogger::Log("x"); SerialLogger::LogHex(img.height); SerialLogger::Log("\r\n");
            return img;
        } else {
            const char* reason = stbi_failure_reason();
            SerialLogger::Log("MediaDecoder: stbi_load_from_memory failed ");
            if (reason) SerialLogger::Log(reason);
            SerialLogger::Log("\r\n");
        }
    }
    img = DecodeRaw(start);
    return img;
}

void MediaDecoder::DrawImage(const Image& img, int x, int y) {
    if (!img.valid) return;

    for (int iy = 0; iy < img.height; iy++) {
        for (int ix = 0; ix < img.width; ix++) {
            int offset = (iy * img.width + ix) * 4;
            uint8_t r,g,b,a;
            if (img.order == 1) { // bgra
                b = img.data[offset + 0];
                g = img.data[offset + 1];
                r = img.data[offset + 2];
                a = img.data[offset + 3];
            } else { // rgba
                r = img.data[offset + 0];
                g = img.data[offset + 1];
                b = img.data[offset + 2];
                a = img.data[offset + 3];
            }

            if (a >= kAlphaCutoff) {
                Graphics::BlendPixel(x + ix, y + iy, r, g, b, a);
            }
        }
    }
}

void MediaDecoder::FreeImage(Image& img) {
    if (img.valid && img.owns && img.data) {
        STBI_FREE(img.data);
        img.data = nullptr;
        img.valid = false;
        img.owns = false;
    }
}

void MediaDecoder::DrawImageScaled(const Image& img, int x, int y, int tw, int th) {
    if (!img.valid || tw <= 0 || th <= 0 || !img.data) return;
    for (int iy = 0; iy < th; iy++) {
        int sy = (iy * img.height) / th;
        if (sy >= img.height) sy = img.height - 1;
        int screen_y = y + iy;  // standard top-down (no flip)
        
        for (int ix = 0; ix < tw; ix++) {
            int sx = (ix * img.width) / tw;
            if (sx >= img.width) sx = img.width - 1;
            int offset = (sy * img.width + sx) * 4;
            uint8_t r,g,b,a;
            if (img.order == 1) { // bgra
                b = img.data[offset + 0];
                g = img.data[offset + 1];
                r = img.data[offset + 2];
                a = img.data[offset + 3];
            } else { // rgba
                r = img.data[offset + 0];
                g = img.data[offset + 1];
                b = img.data[offset + 2];
                a = img.data[offset + 3];
            }
            if (a >= kAlphaCutoff) {
                Graphics::BlendPixel(x + ix, screen_y, r, g, b, a);
            }
        }
    }
}

void MediaDecoder::DrawImageScaledAlpha(const Image& img, int x, int y, int tw, int th, uint8_t alpha_mul) {
    if (!img.valid || tw <= 0 || th <= 0 || !img.data) return;
    for (int iy = 0; iy < th; iy++) {
        int sy = (iy * img.height) / th;
        if (sy >= img.height) sy = img.height - 1;
        int screen_y = y + iy;  // standard top-down (no flip)
        
        for (int ix = 0; ix < tw; ix++) {
            int sx = (ix * img.width) / tw;
            int offset = (sy * img.width + sx) * 4;
            uint8_t r,g,b,a;
            if (img.order == 1) {
                b = img.data[offset + 0];
                g = img.data[offset + 1];
                r = img.data[offset + 2];
                a = img.data[offset + 3];
            } else {
                r = img.data[offset + 0];
                g = img.data[offset + 1];
                b = img.data[offset + 2];
                a = img.data[offset + 3];
            }
            uint32_t aa = ((uint32_t)a * (uint32_t)alpha_mul) / 255u;
            if (aa >= kAlphaCutoff) {
                Graphics::BlendPixel(x + ix, screen_y, r, g, b, (uint8_t)aa);
            }
        }
    }
}

void MediaDecoder::DrawImageScaledAutoCrop(const Image& img, int x, int y, int tw, int th, uint8_t alpha_thresh) {
    if (!img.valid || tw <= 0 || th <= 0) return;
    auto closeColor = [](uint8_t r,uint8_t g,uint8_t b,uint8_t rr,uint8_t gg,uint8_t bb,int t){
        int dr = (int)r - (int)rr; if (dr < 0) dr = -dr;
        int dg = (int)g - (int)gg; if (dg < 0) dg = -dg;
        int db = (int)b - (int)bb; if (db < 0) db = -db;
        return dr <= t && dg <= t && db <= t;
    };
    uint8_t bg_r = img.data[0], bg_g = img.data[1], bg_b = img.data[2];
    int c1 = ((img.width - 1) * 4);
    int c2 = ((img.height - 1) * img.width * 4);
    int c3 = c2 + c1;
    // average 4 corners
    uint32_t ar = bg_r + img.data[c1 + 0] + img.data[c2 + 0] + img.data[c3 + 0];
    uint32_t ag = bg_g + img.data[c1 + 1] + img.data[c2 + 1] + img.data[c3 + 1];
    uint32_t ab = bg_b + img.data[c1 + 2] + img.data[c2 + 2] + img.data[c3 + 2];
    bg_r = (uint8_t)(ar / 4); bg_g = (uint8_t)(ag / 4); bg_b = (uint8_t)(ab / 4);
    int minx = img.width, miny = img.height, maxx = -1, maxy = -1;
    for (int iy = 0; iy < img.height; iy++) {
        for (int ix = 0; ix < img.width; ix++) {
            int off = (iy * img.width + ix) * 4;
            uint8_t r = img.data[off + 0];
            uint8_t g = img.data[off + 1];
            uint8_t b = img.data[off + 2];
            uint8_t a = img.data[off + 3];
            if (a >= alpha_thresh && !closeColor(r,g,b,bg_r,bg_g,bg_b,24)) {
                if (ix < minx) minx = ix;
                if (iy < miny) miny = iy;
                if (ix > maxx) maxx = ix;
                if (iy > maxy) maxy = iy;
            }
        }
    }
    if (maxx < minx || maxy < miny) return; // nothing opaque
    int srcw = maxx - minx + 1;
    int srch = maxy - miny + 1;
    for (int dy = 0; dy < th; dy++) {
        int sy = miny + (dy * srch) / th;
        
        // use standard coordinate
        int screen_y = y + dy;
        
        for (int dx = 0; dx < tw; dx++) {
            int sx = minx + (dx * srcw) / tw;
            int offset = (sy * img.width + sx) * 4;
            uint8_t r = img.data[offset + 0];
            uint8_t g = img.data[offset + 1];
            uint8_t b = img.data[offset + 2];
            uint8_t a = img.data[offset + 3];
            if (a >= alpha_thresh && !closeColor(r,g,b,bg_r,bg_g,bg_b,24)) {
                Graphics::BlendPixel(x + dx, screen_y, r, g, b, a);
            }
        }
    }
}
