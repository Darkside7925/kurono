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

// writers: the drawable box is [g_wx0, sw) x [g_wy0, sh), the screen or the
// damage scissor while a compositor pass runs (kurono.gfx.scissor=1).
// readers (captures) keep screen32 and see the whole screen. (satoru)
static int g_wx0 = 0, g_wy0 = 0;
static inline bool screen32w(uint32_t*& buf, int& pitch_px, int& sw, int& sh) {
    if (!screen32(buf, pitch_px, sw, sh)) return false;
    Graphics::ScissorBounds(g_wx0, g_wy0, sw, sh);
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
    if (!screen32w(buf, pitch, scr_w, scr_h)) return;
    int a256 = alpha >= 255 ? 256 : alpha;
    int x0 = dx < g_wx0 ? g_wx0 : dx, x1 = dx + dw > scr_w ? scr_w : dx + dw;
    int y0 = dy < g_wy0 ? g_wy0 : dy, y1 = dy + dh > scr_h ? scr_h : dy + dh;
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
    if (!screen32w(buf, pitch, scr_w, scr_h)) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int a256 = alpha >= 255 ? 256 : alpha;
    uint32_t c = color | 0xFF000000u;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)(yy - g_wy0) >= (unsigned)(scr_h - g_wy0)) continue;
        int inset = 0;
        int dyc = j < r ? r - j : (j >= h - r ? j - (h - 1 - r) : 0);
        if (dyc > 0) inset = r - isqrt(r * r - dyc * dyc);
        int xa = x + inset, xb = x + w - inset;
        if (xa < g_wx0) xa = g_wx0;
        if (xb > scr_w) xb = scr_w;
        uint32_t* drow = buf + (size_t)yy * pitch;
        for (int xx = xa; xx < xb; xx++)
            drow[xx] = a256 >= 256 ? c : blend(c, drow[xx], a256);
    }
}

// ── physical springs + premultiplied snapshot toolkit (satoru) ─────────────

// per-primitive cycle counters for the [wmfx] probe: 0 shadow, 1 blit, 2 genie. (satoru)
static unsigned long long g_prof[4];
static inline unsigned long long prof_tsc() { unsigned int lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
struct ProfScope { int i; unsigned long long c; ProfScope(int k) : i(k), c(prof_tsc()) {} ~ProfScope() { g_prof[i] += prof_tsc() - c; } };
unsigned long long ProfTake(int i) { unsigned long long v = g_prof[i & 3]; g_prof[i & 3] = 0; return v; }

float Exp(float x) {
    if (x < -60.0f) return 0.0f;
    if (x > 0.0f) x = 0.0f;
    float y = x * 1.44269504f;
    int i = (int)y;
    if ((float)i > y) i--;
    float f = y - (float)i;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    union { float f; uint32_t u; } v;
    v.f = p;
    int e = (int)((v.u >> 23) & 0xFF) + i;
    if (e <= 0) return 0.0f;
    v.u = (v.u & ~(0xFFu << 23)) | ((uint32_t)e << 23);
    return v.f;
}

float Sin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.28318531f;
    x = x - TWO_PI * (float)(int)(x / TWO_PI);
    if (x > PI) x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    if (x > PI * 0.5f) x = PI - x;
    else if (x < -PI * 0.5f) x = -PI - x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
}

float Cos(float x) { return Sin(x + 1.57079633f); }

float Sqrt(float x) { return x > 0.0f ? __builtin_sqrtf(x) : 0.0f; }

void SpringStep(Spring1& s, float response, float damping, float dt) {
    if (dt <= 0.0f) return;
    if (response < 0.01f) response = 0.01f;
    float x0 = s.x - s.t, v0 = s.v;
    float w0 = 6.28318531f / response;
    float x, v;
    if (damping < 0.999f) {
        float z = damping < 0.05f ? 0.05f : damping;
        float wd = w0 * Sqrt(1.0f - z * z);
        float e = Exp(-z * w0 * dt), c = Cos(wd * dt), sn = Sin(wd * dt);
        float B = (v0 + z * w0 * x0) / wd;
        float pos = x0 * c + B * sn;
        x = e * pos;
        v = e * (-z * w0 * pos + (-x0 * wd * sn + B * wd * c));
    } else {
        float e = Exp(-w0 * dt);
        float B = v0 + w0 * x0;
        float pos = x0 + B * dt;
        x = pos * e;
        v = (B - w0 * pos) * e;
    }
    s.x = s.t + x;
    s.v = v;
}

bool SpringRest(const Spring1& s, float eps) {
    float d = s.x - s.t;
    if (d < 0.0f) d = -d;
    float v = s.v < 0.0f ? -s.v : s.v;
    return d < eps && v < eps * 8.0f;
}

// 4x4 supersampled coverage (0..16) of corner cell (i, j) of radius r, the
// same sampling MDraw uses for its corners, so captures match the edges the
// window was drawn with. (satoru)
static int corner_cov16(int i, int j, int r) {
    float r2 = (float)r * r;
    int n = 0;
    for (int b = 0; b < 4; b++) {
        float dy = j + (b + 0.5f) * 0.25f - r;
        for (int a = 0; a < 4; a++) {
            float dx = i + (a + 0.5f) * 0.25f - r;
            if (dx * dx + dy * dy <= r2) n++;
        }
    }
    return n;
}

bool CapturePremul(Snapshot& s, int x, int y, int w, int h, int radius) {
    uint32_t* buf; int pitch, sw, sh;
    if (w <= 0 || h <= 0 || !screen32(buf, pitch, sw, sh)) return false;
    if (!s.px || s.w != w || s.h != h) {
        Release(s);
        s.px = (uint32_t*)KernelHeap::Alloc((size_t)w * (size_t)h * 4);
        if (!s.px) return false;
        s.w = w; s.h = h;
    }
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        uint32_t* row = s.px + (size_t)j * w;
        if ((unsigned)yy >= (unsigned)sh) {
            for (int i = 0; i < w; i++) row[i] = 0;
            continue;
        }
        const uint32_t* src = buf + (size_t)yy * pitch;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            row[i] = (unsigned)xx < (unsigned)sw ? (src[xx] | 0xFF000000u) : 0u;
        }
    }
    int r = radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int cv = corner_cov16(i, j, r);
            if (cv >= 16) continue;
            uint32_t a = (uint32_t)(cv * 255 / 16);
            uint32_t* p[4] = { &s.px[(size_t)j * w + i], &s.px[(size_t)j * w + (w - 1 - i)],
                               &s.px[(size_t)(h - 1 - j) * w + i], &s.px[(size_t)(h - 1 - j) * w + (w - 1 - i)] };
            for (int k = 0; k < 4; k++) {
                if (!cv) { *p[k] = 0u; continue; }
                // keep colour <= coverage so premultiplied maths cannot carry (satoru)
                uint32_t c = *p[k], rr = (c >> 16) & 255u, gg = (c >> 8) & 255u, bb = c & 255u;
                if (rr > a) rr = a;
                if (gg > a) gg = a;
                if (bb > a) bb = a;
                *p[k] = (a << 24) | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
    return true;
}

static uint32_t* g_under = nullptr;
static size_t    g_under_cap = 0;
static int g_ux = 0, g_uy = 0, g_uw = 0, g_uh = 0;
static bool g_under_ok = false;

bool SaveUnder(int x, int y, int w, int h) {
    g_under_ok = false;
    uint32_t* buf; int pitch, sw, sh;
    if (!screen32(buf, pitch, sw, sh)) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return false;
    size_t need = (size_t)w * (size_t)h;
    if (need > g_under_cap) {
        if (g_under) KernelHeap::Free(g_under);
        g_under = (uint32_t*)KernelHeap::Alloc(need * 4);
        g_under_cap = g_under ? need : 0;
        if (!g_under) return false;
    }
    for (int j = 0; j < h; j++)
        __builtin_memcpy(g_under + (size_t)j * w, buf + (size_t)(y + j) * pitch + x, (size_t)w * 4);
    g_ux = x; g_uy = y; g_uw = w; g_uh = h;
    g_under_ok = true;
    return true;
}

void RestoreUnder() {
    if (!g_under_ok) return;
    uint32_t* buf; int pitch, sw, sh;
    if (!screen32w(buf, pitch, sw, sh)) return;
    // only the part inside the drawable box goes back (all of it without a scissor). (satoru)
    int x0 = g_ux > g_wx0 ? g_ux : g_wx0, x1 = g_ux + g_uw < sw ? g_ux + g_uw : sw;
    int y0 = g_uy > g_wy0 ? g_uy : g_wy0, y1 = g_uy + g_uh < sh ? g_uy + g_uh : sh;
    for (int yy = y0; yy < y1 && x1 > x0; yy++)
        __builtin_memcpy(buf + (size_t)yy * pitch + x0, g_under + (size_t)(yy - g_uy) * g_uw + (x0 - g_ux),
                         (size_t)(x1 - x0) * 4);
    g_under_ok = false;
}

// two packed lanes (rb, ag) weighted: wa + wb <= 256. (satoru)
static inline uint32_t mix2w(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb) {
    uint32_t rb = (((a & 0xFF00FFu) * wa + (b & 0xFF00FFu) * wb) >> 8) & 0xFF00FFu;
    uint32_t ag = ((((a >> 8) & 0xFF00FFu) * wa + ((b >> 8) & 0xFF00FFu) * wb)) & 0xFF00FF00u;
    return rb | ag;
}
static inline uint32_t scale2(uint32_t a, uint32_t w) {
    uint32_t rb = (((a & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t ag = (((a >> 8) & 0xFF00FFu) * w) & 0xFF00FF00u;
    return rb | ag;
}
// premultiplied source over dst (satoru)
static inline uint32_t over(uint32_t s, uint32_t d) {
    uint32_t a = s >> 24;
    if (a >= 255) return s;
    if (a == 0) return d;
    return (s + scale2(d, 256 - (a + (a >> 7)))) | 0xFF000000u;
}

static inline int ifloor_(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }

// ── sse2 resampling (satoru) ──────────────────────────────────────────────
// a scaled snapshot row is made in two steps: the two source rows are blended
// vertically into a scratch row (4 pixels per op), then each output pixel
// lerps two neighbours of that row and composites premultiplied over the
// frame, 4 pixels per op with 7-bit weights. the scratch row carries one
// transparent pixel on each side, so edges fade over one source pixel with no
// branches. (satoru)
typedef char  v16qi __attribute__((vector_size(16)));
typedef short v8hi  __attribute__((vector_size(16)));
static inline v16qi ld4(const uint32_t* p) { v16qi v; __builtin_memcpy(&v, p, 16); return v; }
static inline void  st4(uint32_t* p, v16qi v) { __builtin_memcpy(p, &v, 16); }
static const v16qi kZ = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static inline v8hi lo16(v16qi a) { return (v8hi)__builtin_ia32_punpcklbw128(a, kZ); }
static inline v8hi hi16(v16qi a) { return (v8hi)__builtin_ia32_punpckhbw128(a, kZ); }

static const int ROWMAX = 8192;
static uint32_t g_trow[ROWMAX + 8];      // [0] and [sw + 1] stay 0 (satoru)
static uint32_t g_zrow[ROWMAX + 8];      // all zero (satoru)

// t[1 + i] = mix(a[i], b[i]) for i in [c0, c1), weights 0..128 (sum 128). (satoru)
static void vmix(uint32_t* t, const uint32_t* a, const uint32_t* b, int c0, int c1, int wb7) {
    v8hi w = {(short)wb7, (short)wb7, (short)wb7, (short)wb7, (short)wb7, (short)wb7, (short)wb7, (short)wb7};
    int i = c0;
    for (; i + 4 <= c1; i += 4) {
        v16qi A = ld4(a + i), B = ld4(b + i);
        v8hi l = lo16(A) + (((lo16(B) - lo16(A)) * w) >> 7);
        v8hi h = hi16(A) + (((hi16(B) - hi16(A)) * w) >> 7);
        st4(t + 1 + i, __builtin_ia32_packuswb128(l, h));
    }
    for (; i < c1; i++) t[1 + i] = mix2w(a[i], b[i], (uint32_t)(256 - wb7 * 2), (uint32_t)(wb7 * 2));
}

// premultiplied over for 4 pixels, src already resampled; g7 = opacity 0..128. (satoru)
static inline v16qi over4(v16qi P, v16qi D, v8hi g7, bool fade) {
    v8hi pl = lo16(P), ph = hi16(P);
    if (fade) { pl = (pl * g7) >> 7; ph = (ph * g7) >> 7; }
    v8hi al = __builtin_ia32_pshufhw(__builtin_ia32_pshuflw(pl, 0xFF), 0xFF);
    v8hi ah = __builtin_ia32_pshufhw(__builtin_ia32_pshuflw(ph, 0xFF), 0xFF);
    v8hi k128 = {128, 128, 128, 128, 128, 128, 128, 128};
    v8hi il = k128 - ((al + (al >> 7)) >> 1), ih = k128 - ((ah + (ah >> 7)) >> 1);
    v8hi ol = pl + ((lo16(D) * il) >> 7);
    v8hi oh = ph + ((hi16(D) * ih) >> 7);
    return __builtin_ia32_packuswb128(ol, oh);
}

// per output column: scratch-row index (already offset past the left pad and
// clamped) and the 7-bit weight of its right neighbour, pre-splatted for two
// pixels per vector. built once per blit, or once per genie row. (satoru)
typedef int v4si __attribute__((vector_size(16)));
static int  g_xi[ROWMAX + 8];
static v8hi g_xw[ROWMAX / 2 + 8];

static void xtab(int n, int sw, int32_t u, int32_t du) {
    for (int x = 0; x < n; x++) {
        int i0 = u >> 16;
        if (i0 < -1) i0 = -1;
        if (i0 > sw - 1) i0 = sw - 1;
        g_xi[x] = i0 + 1;
        short w = (short)((u >> 9) & 127);
        short* wv = (short*)&g_xw[x >> 1];
        int o = (x & 1) * 4;
        wv[o] = wv[o + 1] = wv[o + 2] = wv[o + 3] = w;
        u += du;
    }
    if (n & 1) { short* wv = (short*)&g_xw[n >> 1]; wv[4] = wv[5] = wv[6] = wv[7] = 0; }
}

// one output row of n pixels from the scratch row through the column table
// (columns tab0 .. tab0 + n). g256 = opacity. (satoru)
static void hrow(uint32_t* dst, int n, int tab0, int g256) {
    const bool fade = g256 < 256;
    short g = (short)(g256 >> 1);
    v8hi g7 = {g, g, g, g, g, g, g, g};
    const uint32_t* T = g_trow;
    const int* xi = g_xi + tab0;
    const v16qi amask = {0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1};
    const v16qi ones = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int x = 0;
    if ((tab0 & 1) == 0) {
        for (; x + 4 <= n; x += 4) {
            int i0 = xi[x], i1 = xi[x + 1], i2 = xi[x + 2], i3 = xi[x + 3];
            v4si Av = {(int)T[i0], (int)T[i1], (int)T[i2], (int)T[i3]};
            v4si Bv = {(int)T[i0 + 1], (int)T[i1 + 1], (int)T[i2 + 1], (int)T[i3 + 1]};
            v16qi A = (v16qi)Av, B = (v16qi)Bv;
            v8hi tl = g_xw[(tab0 + x) >> 1], th = g_xw[((tab0 + x) >> 1) + 1];
            v8hi l = lo16(A) + (((lo16(B) - lo16(A)) * tl) >> 7);
            v8hi h = hi16(A) + (((hi16(B) - hi16(A)) * th) >> 7);
            v16qi P = __builtin_ia32_packuswb128(l, h);
            if (!fade && __builtin_ia32_pmovmskb128(__builtin_ia32_pcmpeqb128(P | ~amask, ones)) == 0xFFFF)
                st4(dst + x, P);
            else
                st4(dst + x, over4(P, ld4(dst + x), g7, fade));
        }
    }
    for (; x < n; x++) {
        int i0 = xi[x];
        const short* wv = (const short*)&g_xw[(tab0 + x) >> 1];
        uint32_t f = (uint32_t)wv[((tab0 + x) & 1) * 4] * 2;
        uint32_t p = mix2w(T[i0], T[i0 + 1], 256 - f, f);
        if (fade) p = scale2(p, (uint32_t)g256);
        dst[x] = over(p, dst[x]);
    }
}

// vertical blend of source rows for output position v (pixel centres) into
// the scratch row, columns [c0, c1). (satoru)
static void vrow_into(const Snapshot& s, float v, int c0, int c1) {
    int j0 = ifloor_(v);
    int w7 = (int)((v - (float)j0) * 128.0f);
    if (w7 > 128) w7 = 128;
    if (w7 < 0) w7 = 0;
    const uint32_t* a = (j0 >= 0 && j0 < s.h) ? s.px + (size_t)j0 * s.w : g_zrow;
    const uint32_t* b = (j0 + 1 >= 0 && j0 + 1 < s.h) ? s.px + (size_t)(j0 + 1) * s.w : g_zrow;
    vmix(g_trow, a, b, c0, c1, w7);
    g_trow[0] = 0;
    g_trow[s.w + 1] = 0;
}

void BlitSmooth(const Snapshot& s, float dx, float dy, float dw, float dh, int alpha256) {
    ProfScope ps(1);
    if (!s.px || s.w <= 0 || s.h <= 0 || s.w > ROWMAX || dw < 0.5f || dh < 0.5f || alpha256 <= 0) return;
    uint32_t* buf; int pitch, scr_w, scr_h;
    if (!screen32w(buf, pitch, scr_w, scr_h)) return;
    if (alpha256 > 256) alpha256 = 256;
    int x0 = ifloor_(dx), x1 = ifloor_(dx + dw) + 1;
    int y0 = ifloor_(dy), y1 = ifloor_(dy + dh) + 1;
    if (x0 < g_wx0) x0 = g_wx0;
    if (y0 < g_wy0) y0 = g_wy0;
    if (x1 > scr_w) x1 = scr_w;
    if (y1 > scr_h) y1 = scr_h;
    if (x0 >= x1 || y0 >= y1) return;
    const float kx = (float)s.w / dw, ky = (float)s.h / dh;
    // exact 1:1 placement on whole pixels: plain composite, no filtering (satoru)
    bool unit = kx > 0.9999f && kx < 1.0001f && ky > 0.9999f && ky < 1.0001f &&
                (float)ifloor_(dx) == dx && (float)ifloor_(dy) == dy;
    if (unit) {
        int ox = (int)dx, oy = (int)dy;
        for (int y = y0; y < y1; y++) {
            int sy = y - oy;
            if ((unsigned)sy >= (unsigned)s.h) continue;
            const uint32_t* sr = s.px + (size_t)sy * s.w;
            uint32_t* dr = buf + (size_t)y * pitch;
            for (int x = x0; x < x1; x++) {
                int sx = x - ox;
                if ((unsigned)sx >= (unsigned)s.w) continue;
                uint32_t p = sr[sx];
                if (alpha256 < 256) p = scale2(p, (uint32_t)alpha256);
                dr[x] = over(p, dr[x]);
            }
        }
        return;
    }
    const float u0 = ((float)x0 + 0.5f - dx) * kx - 0.5f;
    xtab(x1 - x0, s.w, (int32_t)(u0 * 65536.0f), (int32_t)(kx * 65536.0f));
    for (int y = y0; y < y1; y++) {
        float v = ((float)y + 0.5f - dy) * ky - 0.5f;
        if (v <= -1.0f || v >= (float)s.h) continue;
        vrow_into(s, v, 0, s.w);
        hrow(buf + (size_t)y * pitch + x0, x1 - x0, 0, alpha256);
    }
}

static int g_gx0, g_gy0, g_gx1, g_gy1;
void LastGenieBounds(int& x0, int& y0, int& x1, int& y1) { x0 = g_gx0; y0 = g_gy0; x1 = g_gx1; y1 = g_gy1; }

static inline float sat_(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline float smooth_(float t) { t = sat_(t); return t * t * (3.0f - 2.0f * t); }

void Genie(const Snapshot& s, int wx, int wy, int ww, int wh,
           float tx, float ty, float tw, float th, float p) {
    ProfScope ps(2);
    g_gx0 = g_gy0 = 0; g_gx1 = g_gy1 = 0;
    if (!s.px || s.w <= 0 || s.h <= 0 || s.w > ROWMAX) return;
    uint32_t* buf; int pitch, scr_w, scr_h;
    if (!screen32w(buf, pitch, scr_w, scr_h)) return;
    p = sat_(p);
    const float D = ty + th;                       // bottom of the tube (satoru)
    float span = D - (float)wy;
    if (span < 8.0f || ty + th * 0.5f < (float)(wy + wh) * 0.5f) {
        // target is not below the window: a plain scale into it (satoru)
        float q = smooth_(p);
        float cx = (float)wx + ww * 0.5f + (tx + tw * 0.5f - (float)wx - ww * 0.5f) * q;
        float cy = (float)wy + wh * 0.5f + (ty + th * 0.5f - (float)wy - wh * 0.5f) * q;
        float w = (float)ww + (tw - (float)ww) * q, h = (float)wh + (th - (float)wh) * q;
        BlitSmooth(s, cx - w * 0.5f, cy - h * 0.5f, w, h, (int)(256.0f * (1.0f - smooth_((p - 0.6f) / 0.4f))));
        g_gx0 = (int)(cx - w * 0.5f) - 2; g_gy0 = (int)(cy - h * 0.5f) - 2;
        g_gx1 = (int)(cx + w * 0.5f) + 3; g_gy1 = (int)(cy + h * 0.5f) + 3;
        return;
    }
    // the sides bend into a funnel first, the bottom is pulled down to the
    // icon, then the body slides down the funnel; the top lands last. (satoru)
    const float bend = smooth_(p / 0.42f);
    const float bot = (float)(wy + wh) + (D - (float)(wy + wh)) * smooth_(p / 0.55f);
    const float slide = p < 0.18f ? 0.0f : (p - 0.18f) / 0.82f;
    const float sl = slide * slide * (3.0f - 2.0f * slide);
    const float top = (float)wy + (ty - (float)wy) * sl;
    const float yk = (ty - (float)wy) / span;      // where the funnel neck reaches icon width (satoru)
    const float hgt = bot - top;
    if (hgt < 0.5f) return;
    int alpha = (int)(256.0f * (1.0f - 0.85f * smooth_((p - 0.72f) / 0.28f)));
    if (alpha <= 0) return;
    int y0 = ifloor_(top), y1 = ifloor_(bot) + 1;
    if (y0 < g_wy0) y0 = g_wy0;
    if (y1 > scr_h) y1 = scr_h;
    const float ky = (float)s.h / hgt;
    const float L0 = (float)wx, R0 = (float)(wx + ww), L1 = tx, R1 = tx + tw;
    int bx0 = scr_w, bx1 = 0;
    for (int y = y0; y < y1; y++) {
        float fy = (float)y + 0.5f;
        float yn = (fy - (float)wy) / span;
        float shape = yk > 0.01f ? smooth_(yn / yk) : 1.0f;
        float k = bend * shape;
        float L = L0 + (L1 - L0) * k, R = R0 + (R1 - R0) * k;
        float rw = R - L;
        if (rw < 0.5f) continue;
        // vertical coverage at the top and bottom edge rows (satoru)
        float vc = 1.0f;
        if ((float)y < top) vc = (float)(y + 1) - top;
        if ((float)(y + 1) > bot) vc = vc - ((float)(y + 1) - bot);
        if (vc <= 0.0f) continue;
        int ra = (int)((float)alpha * vc);
        if (ra <= 0) continue;
        vrow_into(s, (fy - top) * ky - 0.5f, 0, s.w);
        const float kx = (float)s.w / rw;
        // interior pixels through the simd row, the two partial edge pixels
        // with their coverage (the funnel walls stay smooth) (satoru)
        int ia = ifloor_(L) + 1, ib = ifloor_(R);
        int xa = ia - 1, xb = ib + 1;
        if (xa < g_wx0) xa = g_wx0;
        if (xb > scr_w) xb = scr_w;
        if (xa < bx0) bx0 = xa;
        if (xb > bx1) bx1 = xb;
        uint32_t* dr = buf + (size_t)y * pitch;
        int ca = ia < g_wx0 ? g_wx0 : ia, cb = ib > scr_w ? scr_w : ib;
        if (cb > ca) {
            float u0 = ((float)ca + 0.5f - L) * kx - 0.5f;
            xtab(cb - ca, s.w, (int32_t)(u0 * 65536.0f), (int32_t)(kx * 65536.0f));
            hrow(dr + ca, cb - ca, 0, ra);
        }
        const uint32_t* T = g_trow + 1;
        for (int e = 0; e < 2; e++) {
            int x = e ? ib : ia - 1;
            if (x < g_wx0 || x >= scr_w || (e && ib < ia - 1)) continue;
            if (e && x == ia - 1) continue;
            float hc = 1.0f;
            if ((float)x < L) hc = (float)(x + 1) - L;
            if ((float)(x + 1) > R) hc -= (float)(x + 1) - R;
            if (hc <= 0.0f) continue;
            float u = ((float)x + 0.5f - L) * kx - 0.5f;
            int i0 = ifloor_(u);
            if (i0 < -1) i0 = -1;
            if (i0 > s.w - 1) i0 = s.w - 1;
            uint32_t f = (uint32_t)((u - (float)ifloor_(u)) * 256.0f);
            if (f > 256) f = 256;
            uint32_t px = mix2w(T[i0], T[i0 + 1], 256 - f, f);
            px = scale2(px, (uint32_t)((float)ra * hc));
            dr[x] = over(px, dr[x]);
        }
    }
    g_gx0 = bx0 - 2; g_gy0 = y0 - 2; g_gx1 = bx1 + 2; g_gy1 = y1 + 2;
}

// falloff of a blurred edge: 1 well inside, 0.5 on the edge, 0 at +blur. a
// smootherstep stands in for the gaussian's erf. (satoru)
static uint8_t g_fall[257];
static bool g_fall_ok = false;
static void fall_init() {
    for (int i = 0; i <= 256; i++) {
        float u = (float)i / 256.0f;               // 0 at -blur, 1 at +blur (satoru)
        float s = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
        g_fall[i] = (uint8_t)(255.0f * (1.0f - s) + 0.5f);
    }
    g_fall_ok = true;
}

void SoftShadow(float x, float y, float w, float h, float r, float blur, float dy, int alpha, bool hole) {
    ProfScope ps(0);
    if (alpha <= 0 || w < 1.0f || h < 1.0f) return;
    uint32_t* buf; int pitch, scr_w, scr_h;
    if (!screen32w(buf, pitch, scr_w, scr_h)) return;
    if (!g_fall_ok) fall_init();
    if (alpha > 255) alpha = 255;
    if (blur < 1.0f) blur = 1.0f;
    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;
    const float cx = x + w * 0.5f, cy = y + dy + h * 0.5f;
    const float hx = w * 0.5f - r, hy = h * 0.5f - r;
    int X0 = ifloor_(x - blur), X1 = ifloor_(x + w + blur) + 1;
    int Y0 = ifloor_(y + dy - blur), Y1 = ifloor_(y + dy + h + blur) + 1;
    if (X0 < g_wx0) X0 = g_wx0;
    if (Y0 < g_wy0) Y0 = g_wy0;
    if (X1 > scr_w) X1 = scr_w;
    if (Y1 > scr_h) Y1 = scr_h;
    if (X0 >= X1 || Y0 >= Y1) return;
    const float inv = 128.0f / blur;               // d -> lut index around 128 (satoru)
    // pixels whose column lies over the straight top/bottom edge: |qx| <= 0 (satoru)
    int mx0 = (int)(cx - hx - 0.5f) + 1, mx1 = (int)(cx + hx - 0.5f) + 1;
    if (mx0 < X0) mx0 = X0;
    if (mx1 > X1) mx1 = X1;
    if (mx1 < mx0) mx1 = mx0;
    // hole: the caster's own inside (cross shape). it is skipped always: an
    // opaque caster hides it, and under a fading one the shadow stays outside
    // like a real cast shadow would. (satoru)
    (void)hole;
    int hx0 = (int)x, hx1 = (int)(x + w), hy0 = (int)y, hy1 = (int)(y + h), hr = (int)r + 1;
    for (int py = Y0; py < Y1; py++) {
        float qy = (float)py + 0.5f - cy;
        if (qy < 0.0f) qy = -qy;
        qy -= hy;
        uint32_t* row = buf + (size_t)py * pitch;
        int sk0 = X1, sk1 = X1;
        if (py >= hy0 && py < hy1) {
            bool mid = py >= hy0 + hr && py < hy1 - hr;
            sk0 = mid ? hx0 : hx0 + hr;
            sk1 = mid ? hx1 : hx1 - hr;
            if (sk0 < X0) sk0 = X0;
            if (sk1 > X1) sk1 = X1;
            if (sk1 < sk0) sk1 = sk0;
        }
        // middle span over a straight edge: one alpha for the whole run (satoru)
        uint32_t ma = 0;
        bool mconst = qy >= 0.0f;
        if (mconst) {
            int li = (int)((qy - r) * inv) + 128;
            if (li < 0) li = 0;
            ma = li >= 256 ? 0 : ((uint32_t)g_fall[li] * (uint32_t)alpha) >> 8;
        }
        for (int px = X0; px < X1; px++) {
            if (px >= sk0 && px < sk1) { px = sk1 - 1; continue; }
            if (mconst && px >= mx0 && px < mx1) {
                int e = mx1;
                if (sk0 > px && sk0 < e) e = sk0;
                if (ma) for (; px < e; px++) row[px] = scale2(row[px], 256 - ma) | 0xFF000000u;
                else px = e;
                px--;
                continue;
            }
            if (py >= hy0 && py < hy1 && px >= hx0 && px < hx1) {
                // inside a corner square of the caster: skip what the caster
                // itself covers, so a translucent card shows no dark corners (satoru)
                float ccx = px < hx0 + hr ? x + r : x + w - r;
                float ccy = py < hy0 + hr ? y + r : y + h - r;
                float ex = (float)px + 0.5f - ccx, ey = (float)py + 0.5f - ccy;
                bool in_x = (px < hx0 + hr && ex <= 0.0f) || (px >= hx1 - hr && ex >= 0.0f);
                bool in_y = (py < hy0 + hr && ey <= 0.0f) || (py >= hy1 - hr && ey >= 0.0f);
                if (!(in_x && in_y) || ex * ex + ey * ey <= (r - 0.5f) * (r - 0.5f)) continue;
            }
            float qx = (float)px + 0.5f - cx;
            if (qx < 0.0f) qx = -qx;
            qx -= hx;
            float d;
            if (qx > 0.0f && qy > 0.0f) d = Sqrt(qx * qx + qy * qy) - r;
            else d = (qx > qy ? qx : qy) - r;
            int li = (int)(d * inv) + 128;
            if (li >= 256) continue;
            if (li < 0) li = 0;
            uint32_t a = ((uint32_t)g_fall[li] * (uint32_t)alpha) >> 8;
            if (!a) continue;
            row[px] = scale2(row[px], 256 - a) | 0xFF000000u;
        }
    }
}

}

// end (satoru)
