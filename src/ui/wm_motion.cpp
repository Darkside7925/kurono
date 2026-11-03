#include "wm_motion.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"

namespace WMMotion {

// damped-spring step responses, Q12, 65 samples each. same physics as the
// motion drafts: x(t) = 1 - e^(-z*w*t) * (cos(wd*t) + z*w/wd * sin(wd*t)),
// w = 2*pi / response. regenerate rather than hand-edit. (satoru)

// response 0.34s, damping 0.86, sampled over 620ms
static const short kOpenX[65] = {
    0, 59, 214, 434, 697, 984, 1280, 1575, 1861, 2133, 2387, 2621, 2833,
    3024, 3194, 3343, 3474, 3587, 3684, 3767, 3837, 3895, 3944, 3983, 4016, 4042,
    4062, 4079, 4091, 4100, 4107, 4111, 4114, 4116, 4117, 4116, 4116, 4115, 4114,
    4112, 4111, 4109, 4108, 4106, 4105, 4104, 4103, 4102, 4101, 4100, 4099, 4099,
    4098, 4098, 4097, 4097, 4097, 4097, 4096, 4096, 4096, 4096, 4096, 4096, 4096,
};
// response 0.42s, damping 0.8, sampled over 620ms
static const short kOpenY[65] = {
    0, 40, 147, 306, 504, 727, 967, 1216, 1468, 1717, 1958, 2190, 2409,
    2615, 2806, 2981, 3141, 3286, 3416, 3531, 3634, 3724, 3802, 3869, 3927, 3976,
    4017, 4051, 4079, 4102, 4119, 4133, 4143, 4150, 4155, 4157, 4158, 4158, 4156,
    4154, 4151, 4148, 4144, 4140, 4136, 4133, 4129, 4125, 4122, 4119, 4116, 4113,
    4111, 4109, 4107, 4105, 4103, 4102, 4101, 4100, 4099, 4098, 4097, 4097, 4096,
};
// response 0.32s, damping 0.74, sampled over 620ms
static const short kOpenW[65] = {
    0, 67, 245, 500, 806, 1140, 1485, 1827, 2157, 2467, 2752, 3009, 3237,
    3437, 3608, 3752, 3872, 3970, 4047, 4107, 4152, 4184, 4206, 4219, 4224, 4225,
    4221, 4214, 4205, 4195, 4184, 4174, 4163, 4153, 4143, 4135, 4127, 4120, 4114,
    4109, 4105, 4102, 4099, 4097, 4095, 4094, 4093, 4092, 4092, 4092, 4092, 4092,
    4092, 4093, 4093, 4093, 4094, 4094, 4094, 4095, 4095, 4095, 4095, 4095, 4096,
};
// response 0.38s, damping 0.78, sampled over 620ms
static const short kOpenH[65] = {
    0, 48, 178, 367, 599, 859, 1135, 1417, 1697, 1970, 2230, 2476, 2703,
    2912, 3102, 3272, 3423, 3556, 3672, 3772, 3857, 3929, 3988, 4037, 4077, 4108,
    4132, 4150, 4162, 4171, 4176, 4177, 4177, 4175, 4172, 4167, 4162, 4157, 4151,
    4146, 4140, 4135, 4130, 4125, 4121, 4117, 4114, 4110, 4108, 4105, 4103, 4101,
    4100, 4099, 4097, 4097, 4096, 4095, 4095, 4095, 4095, 4094, 4094, 4094, 4096,
};
// response 0.26s, damping 0.55, sampled over 420ms
static const short kIconBack[65] = {
    0, 49, 183, 385, 641, 935, 1255, 1589, 1928, 2262, 2585, 2891, 3176,
    3436, 3670, 3876, 4055, 4205, 4330, 4428, 4504, 4558, 4592, 4610, 4613, 4604,
    4584, 4556, 4522, 4484, 4443, 4401, 4358, 4316, 4276, 4238, 4203, 4171, 4142,
    4117, 4096, 4077, 4063, 4051, 4042, 4036, 4032, 4031, 4031, 4032, 4035, 4039,
    4043, 4048, 4054, 4059, 4064, 4070, 4075, 4079, 4084, 4088, 4091, 4094, 4096,
};

int Spring(SpringId id, int t_ms, int dur_ms) {
    const short* tab = kOpenX;
    switch (id) {
        case SPRING_OPEN_X: tab = kOpenX; break;
        case SPRING_OPEN_Y: tab = kOpenY; break;
        case SPRING_OPEN_W: tab = kOpenW; break;
        case SPRING_OPEN_H: tab = kOpenH; break;
        case SPRING_ICON:   tab = kIconBack; break;
    }
    if (t_ms <= 0 || dur_ms <= 0) return t_ms <= 0 ? 0 : 4096;
    if (t_ms >= dur_ms) return 4096;
    int pos = (int)(((long long)t_ms * 64 * 256) / dur_ms);   // Q8 table index
    int i = pos >> 8, f = pos & 255;
    return tab[i] + (((tab[i + 1] - tab[i]) * f) >> 8);
}

int Bezier(float x1, float y1, float x2, float y2, int t_ms, int dur_ms) {
    if (t_ms <= 0 || dur_ms <= 0) return t_ms <= 0 ? 0 : 4096;
    if (t_ms >= dur_ms) return 4096;
    float x = (float)t_ms / (float)dur_ms;
    // solve bx(u) = x, then return by(u). newton first, bisection if the
    // slope flattens out. coefficients of the 1-d cubic through 0, p1, p2, 1.
    float cx = 3.0f * x1, bx = 3.0f * (x2 - x1) - cx, ax = 1.0f - cx - bx;
    float cy = 3.0f * y1, by = 3.0f * (y2 - y1) - cy, ay = 1.0f - cy - by;
    float u = x;
    for (int i = 0; i < 8; i++) {
        float fx = ((ax * u + bx) * u + cx) * u - x;
        float d  = (3.0f * ax * u + 2.0f * bx) * u + cx;
        if (fx > -1e-5f && fx < 1e-5f) break;
        if (d > -1e-6f && d < 1e-6f) break;
        u -= fx / d;
    }
    if (u < 0.0f || u > 1.0f) {
        float lo = 0.0f, hi = 1.0f;
        u = x;
        for (int i = 0; i < 24; i++) {
            float fx = ((ax * u + bx) * u + cx) * u;
            if (fx < x) lo = u; else hi = u;
            u = 0.5f * (lo + hi);
        }
    }
    float y = ((ay * u + by) * u + cy) * u;
    int q = (int)(y * 4096.0f + 0.5f);
    if (q < 0) q = 0;
    if (q > 4096) q = 4096;
    return q;
}

uint32_t MixColor(uint32_t a, uint32_t b, int q12) {
    if (q12 <= 0) return a;
    if (q12 >= 4096) return b;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (uint32_t)(ca + (((cb - ca) * q12) >> 12)) << sh;
    }
    return out;
}

static inline bool screen32(uint32_t*& buf, int& pitch_px, int& sw, int& sh) {
    if (Graphics::GetBpp() != 32 || !Graphics::GetBuffer()) return false;
    buf = (uint32_t*)Graphics::GetBuffer();
    pitch_px = (int)(Graphics::GetPitch() / 4);
    sw = Graphics::GetWidth();
    sh = Graphics::GetHeight();
    return true;
}

bool Capture(Snapshot& s, int x, int y, int w, int h, int radius) {
    Release(s);
    uint32_t* buf; int pitch, sw, sh;
    if (w <= 0 || h <= 0 || !screen32(buf, pitch, sw, sh)) return false;
    uint32_t* px = (uint32_t*)KernelHeap::Alloc((size_t)w * (size_t)h * 4);
    if (!px) return false;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        uint32_t* row = px + (size_t)j * w;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            row[i] = ((unsigned)xx < (unsigned)sw && (unsigned)yy < (unsigned)sh)
                     ? (buf[(size_t)yy * pitch + xx] | 0xFF000000u) : 0u;
        }
    }
    // key out what lies beyond the rounded corners.
    int r = radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int dx = r - i, dy = r - j;            // distance from the corner centre
            if (dx * dx + dy * dy <= r * r) continue;
            px[(size_t)j * w + i]                       = 0;
            px[(size_t)j * w + (w - 1 - i)]             = 0;
            px[(size_t)(h - 1 - j) * w + i]             = 0;
            px[(size_t)(h - 1 - j) * w + (w - 1 - i)]   = 0;
        }
    }
    s.px = px; s.w = w; s.h = h;
    return true;
}

void Release(Snapshot& s) {
    if (s.px) KernelHeap::Free(s.px);
    s.px = nullptr; s.w = s.h = 0;
}

static inline uint32_t blend(uint32_t src, uint32_t dst, int a256) {
    uint32_t rb = (((src & 0xFF00FFu) * a256 + (dst & 0xFF00FFu) * (256 - a256)) >> 8) & 0xFF00FFu;
    uint32_t g  = (((src & 0x00FF00u) * a256 + (dst & 0x00FF00u) * (256 - a256)) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

void Blit(const Snapshot& s, int sx, int sy, int sw_, int sh_,
          int dx, int dy, int dw, int dh, int alpha) {
    if (!s.px || dw <= 0 || dh <= 0 || sw_ <= 0 || sh_ <= 0 || alpha <= 0) return;
    uint32_t* buf; int pitch, scr_w, scr_h;
    if (!screen32(buf, pitch, scr_w, scr_h)) return;
    int a256 = alpha >= 255 ? 256 : alpha;
    int x0 = dx < 0 ? 0 : dx, x1 = dx + dw > scr_w ? scr_w : dx + dw;
    int y0 = dy < 0 ? 0 : dy, y1 = dy + dh > scr_h ? scr_h : dy + dh;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t step = (uint32_t)(((uint64_t)sw_ << 16) / (uint64_t)dw);
    for (int y = y0; y < y1; y++) {
        int src_row = sy + (int)(((long long)(y - dy) * sh_) / dh);
        if ((unsigned)src_row >= (unsigned)s.h) continue;
        const uint32_t* srow = s.px + (size_t)src_row * s.w;
        uint32_t* drow = buf + (size_t)y * pitch;
        uint32_t fx = (uint32_t)(x0 - dx) * step;
        for (int x = x0; x < x1; x++, fx += step) {
            int src_col = sx + (int)(fx >> 16);
            if ((unsigned)src_col >= (unsigned)s.w) continue;
            uint32_t p = srow[src_col];
            if ((p >> 24) == 0) continue;
            drow[x] = a256 >= 256 ? p : blend(p, drow[x], a256);
        }
    }
}

static int isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

void FillRoundAlpha(int x, int y, int w, int h, int r, uint32_t color, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    uint32_t* buf; int pitch, scr_w, scr_h;
    if (!screen32(buf, pitch, scr_w, scr_h)) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int a256 = alpha >= 255 ? 256 : alpha;
    uint32_t c = color | 0xFF000000u;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)scr_h) continue;
        int inset = 0;
        int dyc = j < r ? r - j : (j >= h - r ? j - (h - 1 - r) : 0);
        if (dyc > 0) inset = r - isqrt(r * r - dyc * dyc);
        int xa = x + inset, xb = x + w - inset;
        if (xa < 0) xa = 0;
        if (xb > scr_w) xb = scr_w;
        uint32_t* drow = buf + (size_t)yy * pitch;
        for (int xx = xa; xx < xb; xx++)
            drow[xx] = a256 >= 256 ? c : blend(c, drow[xx], a256);
    }
}

}
