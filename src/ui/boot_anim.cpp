#include "boot_anim.h"
#include "wm_motion.h"
#include "../drivers/graphics.h"
#include "../drivers/outputs.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../drivers/cpu_detect.h"
#include "../kernel/pmm.h"
#include "../proc/smp.h"
#include "../hal/hal.h"

//  boot animation, see boot_anim.h. all lengths are derived from the primary
//  output height (oh), all times are anim milliseconds derived from the real
//  clock, so 1024x768 and 2560x1600 at 60 or 240 hz show the same film. (satoru)

namespace BootAnim {
namespace {

// shared state (bsp, one ap, gui process) (satoru)
enum { M_OFF = 0, M_AP = 1, M_PAUSE = 2, M_GUI = 3, M_DONE = 4 };
volatile int g_mode = M_OFF;
volatile int g_job = 0;         // posted for an ap, not yet claimed (satoru)
volatile int g_claim = 0;       // cas-claimed by the ap or the gui (satoru)
volatile int g_drawing = 0;     // ap is inside a frame (satoru)
volatile int g_ap_in = 0;       // ap is inside ApService (satoru)
volatile int g_bg_ready = 0;
volatile int g_need_full = 1;   // next ap frame repaints the whole screen (satoru)
volatile int g_busy = 0;        // composite reentry guard (satoru)
volatile int g_rebg = 0;        // ap must repaint the background (mode change) (satoru)
int  g_static = 0;              // no ap: the bsp drew one formed frame (satoru)
int  g_fast = 0;
uint64_t g_t0 = 0;
int  g_ready = 0;
float g_ready_r = 0;            // real ms since t0 when the gui took over (satoru)
float g_tb = 0;                 // anim ms when the break starts (satoru)

// timeline (anim ms) (satoru)
const float FADE_IN   = 160.0f;
const float STROKE0   = 80.0f,  STROKE1 = 760.0f;
const float FILL0     = 520.0f, FILL1   = 980.0f;
const float SOLID1    = 1380.0f;           // formed (satoru)
const float HOLD_CYC  = 2600.0f;
const float WARP      = 2.5f;              // formation speed-up once boot is done (satoru)
const float BEAT      = 140.0f;            // pause between formed and break (satoru)
// tail, relative to the break (satoru)
const float ANTIC     = 160.0f;
const float FALL      = 560.0f;
const float SKID      = 120.0f;
const float TC        = ANTIC + FALL + SKID;   // collision (satoru)
const float WAVE0     = 60.0f,  WAVE_DUR = 880.0f;      // after collision (satoru)
const float RES0      = 520.0f, RES_DUR  = 900.0f;      // after collision (satoru)
const float TAIL_END  = 1440.0f;                         // after collision (satoru)

// test hooks: kurono.bootanim.hold=MS treats the system as ready no earlier
// than MS (shows the hold loop), kurono.bootanim.early=MS pretends the gui came
// up MS after the start (shows the formation warp). (satoru)
int g_test_hold = 0, g_test_early = 0;

// small math (no libm in the kernel) (satoru)
inline float sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float smooth(float t) { t = sat(t); return t * t * (3.0f - 2.0f * t); }
inline float ease_out3(float t) { t = sat(t); float k = 1.0f - t; return 1.0f - k * k * k; }
inline float ease_out5(float t) { t = sat(t); float k = 1.0f - t; return 1.0f - k * k * k * k * k; }
inline float ease_io3(float t) {
    t = sat(t);
    if (t < 0.5f) return 4.0f * t * t * t;
    float k = -2.0f * t + 2.0f;
    return 1.0f - k * k * k * 0.5f;
}
inline float fabsf_(float v) { return v < 0.0f ? -v : v; }
inline float fsqrt(float v) { return v > 0.0f ? __builtin_sqrtf(v) : 0.0f; }
const float PI = 3.14159265f;

float fsin(float x) {
    const float TWO_PI = 6.28318531f;
    x = x - TWO_PI * (float)(int)(x / TWO_PI);
    if (x > PI) x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    if (x > PI * 0.5f) x = PI - x;
    else if (x < -PI * 0.5f) x = -PI - x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}
inline float fcos(float x) { return fsin(x + PI * 0.5f); }

// e^x for x <= 0 via 2^(x log2 e) (satoru)
float fexpn(float x) {
    if (x < -40.0f) return 0.0f;
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

inline uint32_t mixp(uint32_t a, uint32_t b, uint32_t t) {   // t 0..256 (satoru)
    uint32_t it = 256u - t;
    uint32_t rb = (((a & 0xFF00FFu) * it + (b & 0xFF00FFu) * t) >> 8) & 0xFF00FFu;
    uint32_t g  = (((a & 0x00FF00u) * it + (b & 0x00FF00u) * t) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}
inline uint32_t addp(uint32_t d, int r, int g, int b) {
    int R = (int)((d >> 16) & 255u) + r, G = (int)((d >> 8) & 255u) + g, B = (int)(d & 255u) + b;
    if (R > 255) R = 255;
    if (G > 255) G = 255;
    if (B > 255) B = 255;
    return 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}
inline uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
inline uint32_t to256(float t) { return (uint32_t)(sat(t) * 256.0f + 0.5f); }

// sse2 through gcc builtins (the intrinsics headers need a hosted libc).
// mix4 blends 4 pixels at once with a 7-bit weight, t7 = 0..128. (satoru)
typedef char  v16qi __attribute__((vector_size(16)));
typedef short v8hi  __attribute__((vector_size(16)));
inline v16qi ld4(const uint32_t* p) { v16qi v; __builtin_memcpy(&v, p, 16); return v; }
inline void  st4(uint32_t* p, v16qi v) { __builtin_memcpy(p, &v, 16); }
inline v16qi mix4(v16qi a, v16qi b, v8hi t7) {
    const v16qi z = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    v8hi alo = (v8hi)__builtin_ia32_punpcklbw128(a, z), ahi = (v8hi)__builtin_ia32_punpckhbw128(a, z);
    v8hi blo = (v8hi)__builtin_ia32_punpcklbw128(b, z), bhi = (v8hi)__builtin_ia32_punpckhbw128(b, z);
    v8hi lo = alo + (((blo - alo) * t7) >> 7);
    v8hi hi = ahi + (((bhi - ahi) * t7) >> 7);
    return __builtin_ia32_packuswb128(lo, hi);
}
inline v8hi splat7(uint32_t t256) {
    short t = (short)(t256 >> 1);
    v8hi v = {t, t, t, t, t, t, t, t};
    return v;
}
// dst[x] = mix(a[x], b[x], t) for n pixels (satoru)
void mix_row(uint32_t* dst, const uint32_t* a, const uint32_t* b, uint32_t t256, int n) {
    v8hi t = splat7(t256);
    int x = 0;
    for (; x + 4 <= n; x += 4) st4(dst + x, mix4(ld4(a + x), ld4(b + x), t));
    for (; x < n; x++) dst[x] = mixp(a[x], b[x], t256);
}

inline uint64_t rdtsc_() { uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
uint64_t tsc_per_us() {
    uint64_t f = CPUDetect::GetInfo().frequency.tsc_frequency;
    return f >= 1000000ull ? f / 1000000ull : 2000ull;
}

// frame statistics, logged once at the end (satoru)
struct Stat { uint32_t n; uint64_t sum; uint64_t max; };
Stat g_st_ap = {0, 0, 0}, g_st_scene = {0, 0, 0}, g_st_blur = {0, 0, 0};
void stat_add(Stat& s, uint64_t cyc) { s.n++; s.sum += cyc; if (cyc > s.max) s.max = cyc; }
void stat_log(const char* name, const Stat& s) {
    uint64_t k = tsc_per_us();
    SerialLogger::Log(name);
    SerialLogger::Log(" frames="); SerialLogger::LogDec((int)s.n);
    SerialLogger::Log(" avg_us="); SerialLogger::LogDec(s.n ? (int)(s.sum / s.n / k) : 0);
    SerialLogger::Log(" max_us="); SerialLogger::LogDec((int)(s.max / k));
}

// scene geometry (satoru)
struct Scene {
    int W, H;              // virtual screen (satoru)
    int ox, oy, ow, oh;    // primary output (satoru)
    float u, s, cx, cy;    // unit, svg scale, emblem centre (satoru)
    float lw;              // stroke width (satoru)
    int rx0, ry0, rx1, ry1;   // formation dirty rect (satoru)
};
Scene g_sc;
uint32_t* g_bg = nullptr;
size_t g_bg_bytes = 0;

const float kA[8] = {187, 80, 182, 113, 124, 171, 97, 169};
const float kB[8] = {155, 154, 190, 187, 196, 221, 142, 168};
const float kSvgCX = 146.5f, kSvgCY = 150.5f;

struct Poly { float x[4], y[4]; };

void scene_setup(Scene& sc, int W, int H) {
    sc.W = W; sc.H = H;
    sc.ox = 0; sc.oy = 0; sc.ow = W; sc.oh = H;
    if (Outputs::Multi()) {
        const Output* o = Outputs::Get(0);
        if (o && o->w > 0 && o->h > 0 && o->x + o->w <= W && o->y + o->h <= H) {
            sc.ox = o->x; sc.oy = o->y; sc.ow = o->w; sc.oh = o->h;
        }
    }
    float oh = (float)sc.oh;
    sc.u  = oh / 768.0f;
    sc.s  = 0.072f * oh / 141.0f;     // a small emblem: the story is what it sets off (satoru)
    sc.cx = (float)sc.ox + (float)sc.ow * 0.5f;
    sc.cy = (float)sc.oy + oh * 0.46f;
    sc.lw = 1.0f * sc.u < 1.0f ? 1.0f : 1.0f * sc.u;
    int hx = (int)(0.075f * oh) + 2;
    sc.rx0 = (int)sc.cx - hx; sc.rx1 = (int)sc.cx + hx;
    sc.ry0 = (int)(sc.cy - 0.075f * oh) - 2;
    sc.ry1 = (int)(sc.cy + 0.115f * oh) + 2;
    if (sc.rx0 < 0) sc.rx0 = 0;
    if (sc.ry0 < 0) sc.ry0 = 0;
    if (sc.rx1 > W) sc.rx1 = W;
    if (sc.ry1 > H) sc.ry1 = H;
}

void rest_poly(const Scene& sc, const float* v, Poly& p) {
    for (int i = 0; i < 4; i++) {
        p.x[i] = sc.cx + (v[2 * i] - kSvgCX) * sc.s;
        p.y[i] = sc.cy + (v[2 * i + 1] - kSvgCY) * sc.s;
    }
}

// background: true black for oled panels; the only light near the emblem is
// the tiny additive halo drawn with it (satoru)
void render_bg(uint32_t* dst, const Scene& sc) {
    const size_t n = (size_t)sc.W * (size_t)sc.H;
    for (size_t k = 0; k < n; k++) dst[k] = 0xFF000000u;
}

// primitives (satoru)
struct Clip { int x0, y0, x1, y1; };   // half-open (satoru)

// additive soft ellipse, falloff (1 - d^2)^2 (satoru)
void add_glow(uint32_t* bb, int pitch, const Clip& c, float x, float y, float rx, float ry,
              int R, int G, int B, float inten) {
    if (inten <= 0.003f || rx <= 0.0f || ry <= 0.0f) return;
    if (rx < 0.8f) { inten *= rx / 0.8f; rx = 0.8f; }
    if (ry < 0.8f) { inten *= ry / 0.8f; ry = 0.8f; }
    int x0 = (int)(x - rx), x1 = (int)(x + rx) + 1, y0 = (int)(y - ry), y1 = (int)(y + ry) + 1;
    if (x0 < c.x0) x0 = c.x0;
    if (y0 < c.y0) y0 = c.y0;
    if (x1 > c.x1) x1 = c.x1;
    if (y1 > c.y1) y1 = c.y1;
    if (x0 >= x1 || y0 >= y1) return;
    const float irx2 = 1.0f / (rx * rx), iry2 = 1.0f / (ry * ry);
    const float kr = (float)R * inten, kg = (float)G * inten, kb = (float)B * inten;
    if ((x1 - x0) * (y1 - y0) > 6000) {
        // big glows (bloom, flare): falloff from a table, 4 pixels per
        // saturating add, and only the span inside the ellipse (satoru)
        uint32_t lut[260];
        for (int i = 0; i < 256; i++) {
            float w = 1.0f - (float)i / 256.0f;
            w *= w;
            int ar = (int)(kr * w), ag = (int)(kg * w), ab = (int)(kb * w);
            if (ar > 255) ar = 255;
            if (ag > 255) ag = 255;
            if (ab > 255) ab = 255;
            lut[i] = ((uint32_t)ar << 16) | ((uint32_t)ag << 8) | (uint32_t)ab;
        }
        lut[256] = lut[257] = lut[258] = lut[259] = 0;
        for (int py = y0; py < y1; py++) {
            float dy = (float)py + 0.5f - y;
            float ty = dy * dy * iry2;
            if (ty >= 1.0f) continue;
            float span = rx * fsqrt(1.0f - ty);
            int xa = (int)(x - span), xb = (int)(x + span) + 1;
            if (xa < x0) xa = x0;
            if (xb > x1) xb = x1;
            if (xa >= xb) continue;
            uint32_t* row = bb + (size_t)py * pitch;
            float dx = (float)xa + 0.5f - x;
            float d = dx * dx * irx2 + ty, dd = (2.0f * dx + 1.0f) * irx2;
            const float dd2 = 2.0f * irx2;
            int px = xa;
            uint32_t tmp[4];
            for (; px + 4 <= xb; px += 4) {
                for (int k = 0; k < 4; k++) {
                    int i = (int)(d * 256.0f);
                    tmp[k] = lut[i < 0 ? 0 : (i > 256 ? 256 : i)];
                    d += dd; dd += dd2;
                }
                st4(row + px, __builtin_ia32_paddusb128(ld4(row + px), ld4(tmp)));
            }
            for (; px < xb; px++) {
                int i = (int)(d * 256.0f);
                uint32_t a = lut[i < 0 ? 0 : (i > 256 ? 256 : i)];
                d += dd; dd += dd2;
                if (a) row[px] = addp(row[px], (int)((a >> 16) & 255u), (int)((a >> 8) & 255u), (int)(a & 255u));
            }
        }
        return;
    }
    for (int py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - y;
        float ty = dy * dy * iry2;
        if (ty >= 1.0f) continue;
        uint32_t* row = bb + (size_t)py * pitch;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - x;
            float d = dx * dx * irx2 + ty;
            if (d >= 1.0f) continue;
            float w = (1.0f - d);
            w *= w;
            int ar = (int)(kr * w), ag = (int)(kg * w), ab = (int)(kb * w);
            if (ar | ag | ab) row[px] = addp(row[px], ar, ag, ab);
        }
    }
}

struct Shine { bool on; float nx, ny, pos, width, amt; };

// fill wipe and metal gradient for the formation, read by draw_quad. the wipe
// reveals the fill behind a soft front moving along (nx, ny); the gradient
// lightens the fill toward the top of the emblem. (satoru)
struct Wipe { bool on; float nx, ny, pos, soft; };
Wipe g_wipe = {false, 0, 0, 0, 1};
struct Grad { bool on; float y0, y1; uint32_t top, bottom; };
Grad g_grad = {false, 0, 1, 0, 0};

// anti-aliased convex quad: fill (alpha fa) plus a centred edge line (alpha ea) (satoru)
void draw_quad(uint32_t* bb, int pitch, const Clip& c, const Poly& p, uint32_t fill, float fa,
               uint32_t edge, float ea, float lw, const Shine* sh) {
    float ea_[4], eb_[4], ec_[4];
    float area = 0.0f;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        area += p.x[i] * p.y[j] - p.x[j] * p.y[i];
    }
    float sg = area >= 0.0f ? 1.0f : -1.0f;
    int ne = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        float ex = p.x[j] - p.x[i], ey = p.y[j] - p.y[i];
        float len = fsqrt(ex * ex + ey * ey);
        if (len < 0.01f) continue;
        float k = sg / len;
        ea_[ne] = -ey * k;
        eb_[ne] = ex * k;
        ec_[ne] = (ey * p.x[i] - ex * p.y[i]) * k;
        ne++;
    }
    if (ne < 3) return;
    float mnx = p.x[0], mxx = p.x[0], mny = p.y[0], mxy = p.y[0];
    for (int i = 1; i < 4; i++) {
        if (p.x[i] < mnx) mnx = p.x[i];
        if (p.x[i] > mxx) mxx = p.x[i];
        if (p.y[i] < mny) mny = p.y[i];
        if (p.y[i] > mxy) mxy = p.y[i];
    }
    float pad = lw + 1.5f;
    int x0 = (int)(mnx - pad), x1 = (int)(mxx + pad) + 1, y0 = (int)(mny - pad), y1 = (int)(mxy + pad) + 1;
    if (x0 < c.x0) x0 = c.x0;
    if (y0 < c.y0) y0 = c.y0;
    if (x1 > c.x1) x1 = c.x1;
    if (y1 > c.y1) y1 = c.y1;
    if (x0 >= x1 || y0 >= y1) return;
    const float half = lw * 0.5f;
    const float fa256 = sat(fa) * 256.0f, ea256 = sat(ea) * 256.0f;
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f, fx0 = (float)x0 + 0.5f;
        float d[4];
        for (int i = 0; i < ne; i++) d[i] = ea_[i] * fx0 + eb_[i] * fy + ec_[i];
        uint32_t* row = bb + (size_t)py * pitch;
        for (int px = x0; px < x1; px++) {
            float m = d[0];
            for (int i = 1; i < ne; i++) if (d[i] < m) m = d[i];
            for (int i = 0; i < ne; i++) d[i] += ea_[i];
            if (m < -pad) continue;
            uint32_t o = row[px];
            float cf = m + 0.5f;
            if (cf > 0.0f && fa256 > 0.0f && g_wipe.on) {
                float wv = (g_wipe.pos - ((float)px * g_wipe.nx + fy * g_wipe.ny)) / g_wipe.soft;
                cf *= wv <= 0.0f ? 0.0f : (wv >= 1.0f ? 1.0f : wv * wv * (3.0f - 2.0f * wv));
            }
            if (cf > 0.0f && fa256 > 0.0f) {
                if (cf > 1.0f) cf = 1.0f;
                uint32_t col = fill;
                if (g_grad.on) col = mixp(g_grad.top, g_grad.bottom, to256((fy - g_grad.y0) / (g_grad.y1 - g_grad.y0)));
                if (sh && sh->on) {
                    float s = ((float)px * sh->nx + fy * sh->ny - sh->pos) / sh->width;
                    float b = 1.0f - s * s;
                    if (b > 0.0f) col = mixp(col, 0xFFFFFFFFu, to256(b * b * sh->amt));
                }
                o = mixp(o, col, (uint32_t)(cf * fa256));
            }
            if (ea256 > 0.0f) {
                float ce = half + 0.5f - fabsf_(m);
                if (ce > 0.0f) {
                    if (ce > 1.0f) ce = 1.0f;
                    o = mixp(o, edge, (uint32_t)(ce * ea256));
                }
            }
            row[px] = o;
        }
    }
}

// partial outline: frac of the perimeter from vertex 0 in path order. returns
// the pen head position. (satoru)
void draw_stroke(uint32_t* bb, int pitch, const Clip& c, const Poly& p, float frac, float lw,
                 uint32_t col, float alpha, float* hx, float* hy) {
    float L[4], tot = 0.0f;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        float ex = p.x[j] - p.x[i], ey = p.y[j] - p.y[i];
        L[i] = fsqrt(ex * ex + ey * ey);
        tot += L[i];
    }
    float want = sat(frac) * tot;
    float sx0[4], sy0[4], sx1[4], sy1[4];
    int ns = 0;
    *hx = p.x[0]; *hy = p.y[0];
    for (int i = 0; i < 4 && want > 0.0f; i++) {
        int j = (i + 1) & 3;
        float k = want >= L[i] ? 1.0f : want / L[i];
        sx0[ns] = p.x[i]; sy0[ns] = p.y[i];
        sx1[ns] = p.x[i] + (p.x[j] - p.x[i]) * k;
        sy1[ns] = p.y[i] + (p.y[j] - p.y[i]) * k;
        *hx = sx1[ns]; *hy = sy1[ns];
        ns++;
        want -= L[i];
    }
    if (!ns) return;
    float mnx = sx0[0], mxx = sx0[0], mny = sy0[0], mxy = sy0[0];
    for (int i = 0; i < ns; i++) {
        float xs[2] = {sx0[i], sx1[i]}, ys[2] = {sy0[i], sy1[i]};
        for (int k = 0; k < 2; k++) {
            if (xs[k] < mnx) mnx = xs[k];
            if (xs[k] > mxx) mxx = xs[k];
            if (ys[k] < mny) mny = ys[k];
            if (ys[k] > mxy) mxy = ys[k];
        }
    }
    float pad = lw + 1.5f;
    int x0 = (int)(mnx - pad), x1 = (int)(mxx + pad) + 1, y0 = (int)(mny - pad), y1 = (int)(mxy + pad) + 1;
    if (x0 < c.x0) x0 = c.x0;
    if (y0 < c.y0) y0 = c.y0;
    if (x1 > c.x1) x1 = c.x1;
    if (y1 > c.y1) y1 = c.y1;
    const float half = lw * 0.5f, lim2 = (half + 1.0f) * (half + 1.0f);
    const float a256 = sat(alpha) * 256.0f;
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f;
        uint32_t* row = bb + (size_t)py * pitch;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f;
            float best = lim2;
            for (int i = 0; i < ns; i++) {
                float vx = sx1[i] - sx0[i], vy = sy1[i] - sy0[i];
                float wx = fx - sx0[i], wy = fy - sy0[i];
                float vv = vx * vx + vy * vy;
                float t = vv > 0.0001f ? (wx * vx + wy * vy) / vv : 0.0f;
                t = sat(t);
                float qx = wx - vx * t, qy = wy - vy * t;
                float d2 = qx * qx + qy * qy;
                if (d2 < best) best = d2;
            }
            if (best >= lim2) continue;
            float cov = half + 0.5f - fsqrt(best);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            row[px] = mixp(row[px], col, (uint32_t)(cov * a256));
        }
    }
}

// loading bar under the emblem: a hairline track with the filled part and a
// soft glint at its head; the right end is anti-aliased so slow progress
// still moves every frame (satoru)
float g_bar = 0.0f, g_bar_alpha = 0.0f;
void draw_bar(uint32_t* bb, int pitch, const Clip& c, const Scene& sc, float p, float al) {
    if (al <= 0.003f) return;
    const float oh = (float)sc.oh;
    // a hairline: 1 px at 1080p, rounded ends, filled part in soft white with
    // a small glint riding its head (satoru)
    const float bw = 0.085f * oh, x0 = sc.cx - bw * 0.5f, x1 = x0 + bw;
    const float xe = x0 + bw * sat(p);
    float th = 1.0f * sc.u;
    if (th < 1.0f) th = 1.0f;
    const float yc = sc.cy + 0.092f * oh;
    const float hr = th * 0.5f;
    int ya = (int)(yc - hr - 1.0f), yb = (int)(yc + hr + 1.0f) + 1;
    for (int py = ya; py < yb; py++) {
        if (py < c.y0 || py >= c.y1) continue;
        float fy = (float)py + 0.5f;
        uint32_t* row = bb + (size_t)py * pitch;
        for (int px = (int)(x0 - 1.0f); px <= (int)(x1 + 1.0f); px++) {
            if (px < c.x0 || px >= c.x1) continue;
            float fx = (float)px + 0.5f;
            // capsule distance (rounded ends) (satoru)
            float qx = fx < x0 + hr ? x0 + hr - fx : (fx > x1 - hr ? fx - (x1 - hr) : 0.0f);
            float dy = fy - yc;
            float d = fsqrt(qx * qx + dy * dy) - hr;
            float cov = sat(0.5f - d);
            if (cov <= 0.0f) continue;
            uint32_t o = row[px];
            o = mixp(o, 0xFFFFFFFFu, to256(0.11f * al * cov));
            float cf = sat(xe - (float)px);       // filled part, antialiased head (satoru)
            if (cf > 0.0f) o = mixp(o, rgb(238, 240, 245), to256(0.90f * al * cov * cf));
            row[px] = o;
        }
    }
    if (p > 0.002f && p < 0.999f)
        add_glow(bb, pitch, c, xe, yc, 5.0f * sc.u, 2.2f * sc.u, 190, 205, 240, 0.45f * al);
}

// formation + hold at anim time a, drawn over an already restored bg (satoru)
void draw_formed(uint32_t* bb, int pitch, const Clip& c, const Scene& sc, float a, float brighten) {
    const float oh = (float)sc.oh;
    // no halo: the panel stays true black around the emblem (oled). only a
    // faint breath of light while it waits, well under one grey level. (satoru)
    if (brighten > 0.0f)
        add_glow(bb, pitch, c, sc.cx, sc.cy, 0.06f * oh, 0.06f * oh, 6, 7, 10, brighten * 0.8f);

    // fill: a soft front sweeps up the emblem (bottom left to top right),
    // leaving brushed metal: lighter at the top, cooler at the bottom (satoru)
    float k1 = ease_io3((a - FILL0) / (FILL1 - FILL0));
    float k2 = ease_out5((a - FILL1) / (SOLID1 - FILL1));
    uint32_t c1 = rgb(150, 153, 160), c2 = rgb(186, 189, 195);
    uint32_t fill = mixp(c1, c2, to256(k2));
    float fa = 0.55f + 0.45f * k2;
    if (k1 <= 0.0f) fa = 0.0f;
    if (brighten > 0.0f) fill = mixp(fill, 0xFFFFFFFFu, to256(brighten));
    const float eh = 0.075f * oh;
    g_wipe.on = k1 < 1.0f;
    g_wipe.nx = 0.6f; g_wipe.ny = -0.8f;
    g_wipe.soft = 0.05f * oh;
    {
        float d0 = (sc.cx - eh) * g_wipe.nx + (sc.cy + eh) * g_wipe.ny;   // bottom left (satoru)
        float d1 = (sc.cx + eh) * g_wipe.nx + (sc.cy - eh) * g_wipe.ny;   // top right (satoru)
        g_wipe.pos = d0 + (d1 - d0 + g_wipe.soft) * k1;
    }
    g_grad.on = true;
    g_grad.y0 = sc.cy - eh * 0.75f; g_grad.y1 = sc.cy + eh * 0.75f;
    g_grad.top = mixp(fill, 0xFFFFFFFFu, 70); g_grad.bottom = mixp(fill, rgb(120, 126, 140), 60);

    Shine sh;
    sh.on = false;
    if (a > SOLID1) {
        float ph = a - SOLID1;
        ph -= HOLD_CYC * (float)(int)(ph / HOLD_CYC);
        if (ph < 760.0f) {
            sh.on = true;
            float ux = 0.857f, uy = -0.514f;       // diagonal sweep direction (satoru)
            sh.nx = ux; sh.ny = uy;
            float span = 0.10f * oh;
            float cdot = sc.cx * ux + sc.cy * uy;
            sh.pos = cdot - span + 2.0f * span * ease_io3(ph / 760.0f);
            sh.width = 0.018f * oh;
            sh.amt = 0.28f * (0.5f - 0.5f * fcos(2.0f * PI * ph / 760.0f));
        }
    }

    float sp = ease_io3((a - STROKE0) / (STROKE1 - STROKE0));
    bool stroking = sp < 1.0f;
    float settle = smooth((a - FILL1) / (SOLID1 - FILL1));
    uint32_t edge = mixp(rgb(244, 246, 250), rgb(226, 229, 235), to256(settle));
    float ea = stroking ? 0.0f : 1.0f - 0.45f * settle;

    Poly pa, pb;
    rest_poly(sc, kA, pa);
    rest_poly(sc, kB, pb);
    draw_quad(bb, pitch, c, pa, fill, fa, edge, ea, sc.lw, &sh);
    draw_quad(bb, pitch, c, pb, fill, fa, edge, ea, sc.lw, &sh);
    g_wipe.on = false;
    g_grad.on = false;
    if (stroking && a > STROKE0) {
        // the pen: a bright point with a tight bloom, fading as it closes (satoru)
        float hx, hy;
        float pen = 1.0f - smooth((sp - 0.85f) / 0.15f);
        draw_stroke(bb, pitch, c, pa, sp, sc.lw, edge, 1.0f, &hx, &hy);
        add_glow(bb, pitch, c, hx, hy, 4.5f * sc.u, 4.5f * sc.u, 170, 195, 255, 0.55f * pen);
        add_glow(bb, pitch, c, hx, hy, 1.6f * sc.u, 1.6f * sc.u, 255, 255, 255, 0.9f * pen);
        draw_stroke(bb, pitch, c, pb, sp, sc.lw, edge, 1.0f, &hx, &hy);
        add_glow(bb, pitch, c, hx, hy, 4.5f * sc.u, 4.5f * sc.u, 170, 195, 255, 0.55f * pen);
        add_glow(bb, pitch, c, hx, hy, 1.6f * sc.u, 1.6f * sc.u, 255, 255, 255, 0.9f * pen);
    }

    // the loading bar (state advanced by bar_update in the frame owner) (satoru)
    draw_bar(bb, pitch, c, sc, g_bar, g_bar_alpha);
}

// tail: two pieces, physics and particles (satoru)
struct Piece {
    float rx[4], ry[4];     // rest verts relative to the centroid (satoru)
    float c0x, c0y;         // rest centroid (satoru)
    float th;               // rotation at landing (lies flat) (satoru)
    float yb;               // bottom extent at landing pose (satoru)
    float xland, yland, xcont;
    float v0, g;            // vertical launch and gravity, px/ms (satoru)
    float dirx, diry;       // anticipation push direction (satoru)
};
Piece g_pc[2];
float g_ex = 0, g_ey = 0;   // impact point (satoru)

struct Part { float x0, y0, vx, vy, life, r; int type; };
const int NPART = 150;        // 0..109 sparks (streaks), 110.. floor dust (satoru)
const int NSPARK = 110;
Part g_part[NPART];
struct Shard { float x0, y0, vx, vy, life, sz, rot0, w, asp; };
const int NSHARD = 40;
// embers: the small thing's afterglow. they rise slowly, sway and twinkle
// over the desktop as it comes into focus. (satoru)
struct Ember { float x0, y0, vx, vy, life, delay, r, sway, freq, ph, warm; };
const int NEMBER = 44;
Ember g_ember[NEMBER];
const float FLOOR_K = 0.80f;   // the floor the pieces land on, from the top of the output (satoru)
Shard g_shard[NSHARD];

uint32_t g_rng = 0x9E3779B9u;
float frand() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFF) / 16777216.0f;
}

void piece_pose(const Piece& p, float th, Poly& out, float x, float y) {
    float cs = fcos(th), sn = fsin(th);
    for (int i = 0; i < 4; i++) {
        out.x[i] = x + p.rx[i] * cs - p.ry[i] * sn;
        out.y[i] = y + p.rx[i] * sn + p.ry[i] * cs;
    }
}

void tail_setup(const Scene& sc) {
    const float oh = (float)sc.oh;
    const float* src[2] = {kA, kB};
    const float th[2] = {0.782f, -0.768f};   // rotate each strip flat (satoru)
    for (int k = 0; k < 2; k++) {
        Piece& p = g_pc[k];
        Poly r;
        rest_poly(sc, src[k], r);
        p.c0x = (r.x[0] + r.x[1] + r.x[2] + r.x[3]) * 0.25f;
        p.c0y = (r.y[0] + r.y[1] + r.y[2] + r.y[3]) * 0.25f;
        for (int i = 0; i < 4; i++) { p.rx[i] = r.x[i] - p.c0x; p.ry[i] = r.y[i] - p.c0y; }
        p.th = th[k];
        Poly q;
        piece_pose(p, p.th, q, 0.0f, 0.0f);
        float yb = q.y[0], xl = q.x[0], xr = q.x[0];
        for (int i = 1; i < 4; i++) {
            if (q.y[i] > yb) yb = q.y[i];
            if (q.x[i] < xl) xl = q.x[i];
            if (q.x[i] > xr) xr = q.x[i];
        }
        p.yb = yb;
        p.yland = (float)sc.oy + FLOOR_K * oh - yb - 0.5f;
        p.xcont = k == 0 ? sc.cx - xr : sc.cx - xl;
        p.xland = k == 0 ? p.xcont - 0.06f * oh : p.xcont + 0.06f * oh;
        p.v0 = -0.30f * oh / 1000.0f;
        float D = p.yland - p.c0y;
        p.g = 2.0f * (D - p.v0 * FALL) / (FALL * FALL);
        float dx = p.c0x - sc.cx, dy = p.c0y - sc.cy;
        float dl = fsqrt(dx * dx + dy * dy);
        p.dirx = dl > 0.01f ? dx / dl : 0.0f;
        p.diry = dl > 0.01f ? dy / dl : 0.0f;
    }
    g_ex = sc.cx;
    g_ey = (float)sc.oy + FLOOR_K * oh;
    g_rng = 0x9E3779B9u;
    const float hu = oh / 1000.0f;   // px per ms at 1 screen height per second (satoru)
    for (int i = 0; i < NPART; i++) {
        Part& q = g_part[i];
        bool dust = i >= NSPARK;
        float ang = -PI * 0.5f + (frand() + frand() - 1.0f) * (dust ? 1.45f : 0.85f);
        float sp = (dust ? 0.25f + 0.45f * frand() : 0.8f + 1.9f * frand() * frand() + 0.35f * frand()) * hu;
        q.vx = fcos(ang) * sp;
        q.vy = fsin(ang) * sp;
        q.x0 = g_ex + (frand() - 0.5f) * 0.06f * oh;
        q.y0 = g_ey - 0.004f * oh * frand();
        q.life = dust ? 600.0f + 400.0f * frand() : 360.0f + 560.0f * frand();
        q.r = dust ? (0.010f + 0.010f * frand()) * oh : (0.0012f + 0.0012f * frand()) * oh;
        q.type = dust ? 1 : 0;
    }
    for (int i = 0; i < NSHARD; i++) {
        Shard& s = g_shard[i];
        float ang = -PI * 0.5f + (frand() + frand() - 1.0f) * 1.1f;
        float sp = (0.35f + 0.95f * frand()) * hu;
        s.vx = fcos(ang) * sp;
        s.vy = fsin(ang) * sp;
        s.x0 = g_ex + (frand() - 0.5f) * 0.09f * oh;
        s.y0 = g_ey - 0.006f * oh * frand();
        s.life = 480.0f + 420.0f * frand();
        s.sz = (0.0025f + 0.0055f * frand()) * oh;
        s.rot0 = frand() * 6.28f;
        s.w = (frand() - 0.5f) * 0.04f;
        s.asp = 0.35f + 0.5f * frand();
    }
    for (int i = 0; i < NEMBER; i++) {
        Ember& e = g_ember[i];
        e.x0 = g_ex + (frand() + frand() - 1.0f) * 0.06f * oh;
        e.y0 = g_ey - 0.01f * oh * frand();
        e.vx = (frand() - 0.5f) * 0.36f * hu;
        e.vy = -(0.28f + 0.40f * frand() + 0.20f * frand() * frand()) * hu;
        e.delay = 40.0f * frand() + 160.0f * frand() * frand();
        e.life = 900.0f + 450.0f * frand();
        if (e.delay + e.life > TAIL_END - 60.0f) e.life = TAIL_END - 60.0f - e.delay;   // done before the film ends (satoru)
        e.r = (0.0011f + 0.0016f * frand()) * oh;
        e.sway = (0.006f + 0.016f * frand()) * oh;
        e.freq = 0.0016f + 0.0026f * frand();
        e.ph = frand() * 6.28f;
        e.warm = frand();
    }
}

// pose of piece k at tail time b (ms since the break). returns alpha. (satoru)
float tail_piece(const Scene& sc, int k, float b, Poly& out, uint32_t* fill, float* bright) {
    const Piece& p = g_pc[k];
    const float oh = (float)sc.oh;
    *bright = 0.0f;
    *fill = rgb(179, 182, 188);
    if (b < ANTIC) {
        float s = fsin(PI * b / ANTIC);
        float sw = 1.0f + 0.035f * s;
        float push = 0.005f * oh * s;
        float cx = sc.cx + (p.c0x - sc.cx) * sw + p.dirx * push;
        float cy = sc.cy + (p.c0y - sc.cy) * sw + p.diry * push;
        for (int i = 0; i < 4; i++) { out.x[i] = cx + p.rx[i] * sw; out.y[i] = cy + p.ry[i] * sw; }
        *bright = 0.22f * s;
        return 1.0f;
    }
    if (b < ANTIC + FALL) {
        float t = b - ANTIC;
        float x = p.c0x + (p.xland - p.c0x) * (t / FALL);
        float y = p.c0y + p.v0 * t + 0.5f * p.g * t * t;
        piece_pose(p, p.th * (t / FALL), out, x, y);
        return 1.0f;
    }
    float s = b - ANTIC - FALL;
    float x;
    if (s < SKID) {
        float q = s / SKID;
        q = q * q * __builtin_sqrtf(q > 0.0f ? q : 0.0f) ;   // q^2.5: they accelerate into each other (satoru)
        x = p.xland + (p.xcont - p.xland) * q;
    } else {
        x = p.xcont;
    }
    float sq = (float)WMMotion::Spring(WMMotion::SPRING_ICON, (int)s, 420) / 4096.0f;
    float sy = 0.62f + 0.38f * sq, sx = 1.0f + 0.22f * (1.0f - sq);
    float alpha = 1.0f;
    if (b >= TC) {
        // they meet and shatter: a white flash and gone, the shards take over (satoru)
        float d = sat((b - TC) / 70.0f);
        sx *= 1.0f + 0.10f * d;
        sy *= 1.0f + 0.10f * d;
        *bright = sat(d * 3.0f);
        alpha = 1.0f - d;
    }
    float cs = fcos(p.th), sn = fsin(p.th);
    float floor_y = p.yland + p.yb;
    for (int i = 0; i < 4; i++) {
        float rx = p.rx[i] * cs - p.ry[i] * sn;
        float ry = p.rx[i] * sn + p.ry[i] * cs;
        out.x[i] = x + rx * sx;
        out.y[i] = floor_y - (p.yb - ry) * sy;
    }
    return alpha;
}

void draw_pieces(uint32_t* bb, int pitch, const Clip& c, const Scene& sc, float b) {
    if (b >= TC + 70.0f) return;
    uint32_t edge = rgb(226, 229, 235);
    for (int k = 0; k < 2; k++) {
        // motion smear while falling: faint copies a few ms behind, closer
        // together than a frame so they read as blur, not as echoes (satoru)
        if (b > ANTIC + 60.0f && b < ANTIC + FALL) {
            static const float ga[5] = {0.0f, 0.20f, 0.13f, 0.08f, 0.04f};
            for (int gi = 4; gi >= 1; gi--) {
                Poly g; uint32_t f; float br;
                tail_piece(sc, k, b - 3.5f * (float)gi, g, &f, &br);
                draw_quad(bb, pitch, c, g, f, ga[gi], edge, 0.0f, sc.lw, nullptr);
            }
        }
        Poly q; uint32_t f; float br;
        float al = tail_piece(sc, k, b, q, &f, &br);
        if (br > 0.0f) f = mixp(f, 0xFFFFFFFFu, to256(br));
        draw_quad(bb, pitch, c, q, f, al, edge, al * 0.55f, sc.lw, nullptr);
    }
}

// additive anti-aliased streak from (x0, y0) (tail) to (x1, y1) (head):
// brightest at the head, fading to nothing at the tail. this is the motion
// blur of a spark. (satoru)
void add_streak(uint32_t* bb, int pitch, const Clip& c, float x0, float y0, float x1, float y1,
                float w, int R, int G, int B, float inten) {
    if (inten <= 0.004f) return;
    float vx = x1 - x0, vy = y1 - y0;
    float vv = vx * vx + vy * vy;
    if (w < 0.6f) { inten *= w / 0.6f; w = 0.6f; }
    float pad = w + 1.0f;
    int bx0 = (int)((x0 < x1 ? x0 : x1) - pad), bx1 = (int)((x0 > x1 ? x0 : x1) + pad) + 1;
    int by0 = (int)((y0 < y1 ? y0 : y1) - pad), by1 = (int)((y0 > y1 ? y0 : y1) + pad) + 1;
    if (bx0 < c.x0) bx0 = c.x0;
    if (by0 < c.y0) by0 = c.y0;
    if (bx1 > c.x1) bx1 = c.x1;
    if (by1 > c.y1) by1 = c.y1;
    if (bx0 >= bx1 || by0 >= by1) return;
    const float iw = 1.0f / w;
    const float ivv = vv > 0.0001f ? 1.0f / vv : 0.0f;
    for (int py = by0; py < by1; py++) {
        uint32_t* row = bb + (size_t)py * pitch;
        float fy = (float)py + 0.5f;
        for (int px = bx0; px < bx1; px++) {
            float fx = (float)px + 0.5f;
            float wx = fx - x0, wy = fy - y0;
            float t = (wx * vx + wy * vy) * ivv;
            t = sat(t);
            float qx = wx - vx * t, qy = wy - vy * t;
            float d2 = (qx * qx + qy * qy) * iw * iw;
            if (d2 >= 1.0f) continue;
            float k = (1.0f - d2);
            k *= k * t * inten;
            int ar = (int)((float)R * k), ag = (int)((float)G * k), ab = (int)((float)B * k);
            if (ar | ag | ab) row[px] = addp(row[px], ar, ag, ab);
        }
    }
}

void draw_burst(uint32_t* bb, int pitch, const Clip& c, const Scene& sc, float b) {
    const float oh = (float)sc.oh;
    const float u = sc.u;
    // a breath of dust where each piece touches the floor (satoru)
    float sl = b - ANTIC - FALL;
    if (sl >= 0.0f && sl < 220.0f) {
        float k = 1.0f - sl / 220.0f;
        for (int i = 0; i < 2; i++) {
            Poly q; uint32_t f; float br;
            tail_piece(sc, i, b, q, &f, &br);
            float px = (q.x[0] + q.x[1] + q.x[2] + q.x[3]) * 0.25f;
            add_glow(bb, pitch, c, px, g_ey, 0.06f * oh, 0.012f * oh, 150, 165, 200, 0.40f * k * k);
        }
    }
    float tc = b - TC;
    if (tc < 0.0f) return;
    // impact: a hot white point, a thin anamorphic line of light along the
    // floor, and a soft bloom that rises and lets go (satoru)
    if (tc < 220.0f) {
        float k = 1.0f - tc / 220.0f;
        add_glow(bb, pitch, c, g_ex, g_ey, 0.030f * oh, 0.026f * oh, 255, 255, 255, k * k * 1.3f);
    }
    if (tc < 420.0f) {
        float k = 1.0f - tc / 420.0f;
        float e = ease_out3(tc / 420.0f);
        add_glow(bb, pitch, c, g_ex, g_ey, (0.12f + 0.30f * e) * oh, 0.0045f * oh + 1.0f, 200, 218, 255, 0.95f * k * k);
    }
    if (tc < 640.0f) {
        float p = tc / 640.0f;
        float e = ease_out3(p);
        float k = 1.0f - p;
        add_glow(bb, pitch, c, g_ex, g_ey - 0.05f * oh * e, (0.08f + 0.10f * e) * oh, (0.06f + 0.10f * e) * oh,
                 140, 160, 215, 0.42f * k * k);
    }
    // shards of the emblem, thrown up and falling back, tumbling (satoru)
    const float gs = 2.4f * oh / 1000000.0f;
    for (int i = 0; i < NSHARD; i++) {
        const Shard& s = g_shard[i];
        if (tc > s.life) continue;
        float t = tc;
        float x = s.x0 + s.vx * t;
        float y = s.y0 + s.vy * t + 0.5f * gs * t * t;
        float a = 1.0f - t / s.life;
        a = a * fsqrt(a);
        float ang = s.rot0 + s.w * t;
        float cs = fcos(ang), sn = fsin(ang);
        float hw = s.sz, hh = s.sz * s.asp;
        Poly q;
        float lx[4] = {-hw, hw, hw * 0.4f, -hw * 0.8f}, ly[4] = {-hh, -hh * 0.3f, hh, hh * 0.6f};
        for (int k = 0; k < 4; k++) { q.x[k] = x + lx[k] * cs - ly[k] * sn; q.y[k] = y + lx[k] * sn + ly[k] * cs; }
        // the face catches the light as it turns (satoru)
        uint32_t col = mixp(rgb(150, 155, 166), rgb(240, 243, 248), to256(0.5f + 0.5f * cs));
        draw_quad(bb, pitch, c, q, col, a, 0xFFFFFFFFu, a * 0.35f, 1.0f, nullptr);
    }
    // sparks: closed-form drag + gravity, so every frame rate lands on the
    // same curve; each is drawn as the streak it would smear on film (satoru)
    const float kd = 2.6f / 1000.0f;
    const float gp = 1.1f * oh / 1000000.0f;
    for (int i = 0; i < NPART; i++) {
        const Part& q = g_part[i];
        if (tc > q.life) continue;
        float t = tc;
        float e = fexpn(-kd * t);
        float m = (1.0f - e) / kd;
        float x = q.x0 + q.vx * m;
        float y = q.y0 + q.vy * m + gp / kd * (t - m);
        float life = 1.0f - t / q.life;
        float in = life * fsqrt(life) * sat(t / 25.0f);
        if (q.type == 0) {
            float tt = t > 16.0f ? t - 16.0f : 0.0f;          // tail: 16 ms behind (satoru)
            float e2 = fexpn(-kd * tt), m2 = (1.0f - e2) / kd;
            float x2 = q.x0 + q.vx * m2, y2 = q.y0 + q.vy * m2 + gp / kd * (tt - m2);
            float w = q.r * (1.0f - 0.5f * (t / q.life)) + 0.4f * u;
            add_streak(bb, pitch, c, x2, y2, x, y, w * 1.8f, 140, 170, 255, in * 0.55f);
            add_streak(bb, pitch, c, x2, y2, x, y, w, 255, 255, 255, in);
        } else {
            add_glow(bb, pitch, c, x, y, q.r, q.r * 0.6f, 110, 122, 150, in * 0.22f);
        }
    }
    // embers: the small cause, the beautiful effect. they keep rising over
    // the desktop as it comes into focus (satoru)
    for (int i = 0; i < NEMBER; i++) {
        const Ember& em = g_ember[i];
        float t = tc - em.delay;
        if (t < 0.0f || t > em.life) continue;
        float drag = fexpn(-0.0009f * t);
        float m = (1.0f - drag) / 0.0009f;
        float x = em.x0 + em.vx * m + em.sway * fsin(em.ph + em.freq * t) * sat(t / 300.0f);
        float y = em.y0 + em.vy * m - 0.00000012f * oh * t * t;   // buoyancy: they keep rising (satoru)
        float life = t / em.life;
        float fade = sat(t / 120.0f) * (1.0f - smooth((life - 0.55f) / 0.45f));
        float tw = 0.70f + 0.30f * fsin(em.ph * 3.0f + t * 0.021f);
        float in = fade * tw;
        int R = em.warm > 0.6f ? 255 : 210, G = em.warm > 0.6f ? 226 : 225, B = em.warm > 0.6f ? 190 : 255;
        add_glow(bb, pitch, c, x, y, em.r * 3.2f, em.r * 3.2f, R, G, B, 0.30f * in);
        add_glow(bb, pitch, c, x, y, em.r, em.r, 255, 255, 255, 0.95f * in);
    }
}

// blur pyramid for the reveal (satoru)
// levels: L1 = desktop / 4, L1b = L1 softened, L2 = desktop / 16 softened, U2 =
// L2 brought back to L1 size. each frame the focus stage is mixed ONCE at L1
// size into C, C is widened to full width row by row (HC), and an output pixel
// then costs a single vertical lerp. (satoru)
uint8_t* g_pool = nullptr;
size_t g_pool_bytes = 0;
int g_pw = 0, g_ph = 0;
uint32_t *g_l1 = nullptr, *g_l1b = nullptr, *g_l1t = nullptr, *g_l2 = nullptr, *g_l2t = nullptr;
uint32_t *g_u2 = nullptr, *g_c = nullptr, *g_hc = nullptr;
uint32_t *g_x1 = nullptr;     // full width -> L1 (index << 8 | frac) (satoru)
uint32_t *g_x21 = nullptr;    // L1 width -> L2 (satoru)
uint32_t* g_acc = nullptr;    // row accumulators (satoru)
uint32_t* g_rowt = nullptr;   // one scratch row (satoru)
int g_w1, g_h1n, g_w2, g_h2n;
uint64_t g_built_ms = 0;      // last completed pyramid build (satoru)
int g_bstep = 0;              // next pyramid slice (satoru)

void pool_free() {
    if (g_pool) PMM::FreeBytes(g_pool, g_pool_bytes);
    g_pool = nullptr; g_pool_bytes = 0; g_pw = g_ph = 0;
}

void make_map(uint32_t* map, int out_n, int in_n, float f) {
    for (int x = 0; x < out_n; x++) {
        float s = ((float)x + 0.5f) / f - 0.5f;
        if (s < 0.0f) s = 0.0f;
        int i0 = (int)s;
        uint32_t fr = (uint32_t)((s - (float)i0) * 256.0f);
        if (i0 >= in_n - 1) { i0 = in_n - 1; fr = 0; }
        map[x] = ((uint32_t)i0 << 8) | fr;
    }
}

bool pool_alloc(int W, int H) {
    if (g_pool && g_pw == W && g_ph == H) return true;
    pool_free();
    int w1 = (W + 3) / 4, h1 = (H + 3) / 4, w2 = (w1 + 3) / 4, h2 = (h1 + 3) / 4;
    size_t a1 = (size_t)w1 * h1, a2 = (size_t)w2 * h2;
    size_t n = 5u * a1 + 2u * a2 + (size_t)h1 * W + 2u * (size_t)W + (size_t)w1 + 3u * (size_t)w1;
    size_t bytes = n * 4u;
    g_pool = (uint8_t*)PMM::AllocBytes(bytes);
    if (!g_pool) return false;
    g_pool_bytes = bytes;
    uint32_t* p = (uint32_t*)g_pool;
    g_l1 = p;  p += a1;
    g_l1b = p; p += a1;
    g_l1t = p; p += a1;
    g_u2 = p;  p += a1;
    g_c = p;   p += a1;
    g_l2 = p;  p += a2;
    g_l2t = p; p += a2;
    g_hc = p;  p += (size_t)h1 * W;
    g_x1 = p;  p += W;
    g_x21 = p; p += w1;
    g_rowt = p; p += W;
    g_acc = p;
    g_w1 = w1; g_h1n = h1; g_w2 = w2; g_h2n = h2;
    g_pw = W; g_ph = H;
    g_built_ms = 0; g_bstep = 0;
    make_map(g_x1, W, w1, 4.0f);
    make_map(g_x21, w1, w2, 4.0f);
    return true;
}

// 4x4 box downsample of destination rows [ry0, ry1). step 2 samples every
// other row and column of each block, which is fine because the result is
// blurred further (satoru)
void down4(const uint32_t* src, int sw, int sh, int spitch, uint32_t* dst, int dw, int dh, int step,
           int ry0 = 0, int ry1 = -1) {
    uint32_t* acc = g_acc;
    if (ry1 < 0 || ry1 > dh) ry1 = dh;
    for (int y = ry0; y < ry1; y++) {
        for (int i = 0; i < dw * 3; i++) acc[i] = 0;
        int ys = y * 4, ye = ys + 4 < sh ? ys + 4 : sh;
        int rows = 0;
        for (int yy = ys; yy < ye; yy += step) {
            const uint32_t* row = src + (size_t)yy * spitch;
            rows++;
            for (int x = 0; x < sw; x += step) {
                uint32_t v = row[x];
                uint32_t* a = acc + (x >> 2) * 3;
                a[0] += (v >> 16) & 255u; a[1] += (v >> 8) & 255u; a[2] += v & 255u;
            }
        }
        uint32_t* out = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int cols = (x * 4 + 4 <= sw) ? 4 : sw - x * 4;
            cols = (cols + step - 1) / step;
            uint32_t n = (uint32_t)(rows * cols);
            if (!n) n = 1;
            uint32_t* a = acc + x * 3;
            out[x] = 0xFF000000u | ((a[0] / n) << 16) | ((a[1] / n) << 8) | (a[2] / n);
        }
    }
}

// one separable [1 2 1] pass (satoru)
void blur121(uint32_t* img, uint32_t* tmp, int w, int h) {
    for (int y = 0; y < h; y++) {
        const uint32_t* r = img + (size_t)y * w;
        uint32_t* o = tmp + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t a = r[x > 0 ? x - 1 : 0], m = r[x], b = r[x + 1 < w ? x + 1 : w - 1];
            uint32_t rb = (((a & 0xFF00FFu) + 2u * (m & 0xFF00FFu) + (b & 0xFF00FFu)) >> 2) & 0xFF00FFu;
            uint32_t g = (((a & 0xFF00u) + 2u * (m & 0xFF00u) + (b & 0xFF00u)) >> 2) & 0xFF00u;
            o[x] = 0xFF000000u | rb | g;
        }
    }
    for (int y = 0; y < h; y++) {
        const uint32_t* ra = tmp + (size_t)(y > 0 ? y - 1 : 0) * w;
        const uint32_t* rm = tmp + (size_t)y * w;
        const uint32_t* rbw = tmp + (size_t)(y + 1 < h ? y + 1 : h - 1) * w;
        uint32_t* o = img + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t a = ra[x], m = rm[x], b = rbw[x];
            uint32_t rb = (((a & 0xFF00FFu) + 2u * (m & 0xFF00FFu) + (b & 0xFF00FFu)) >> 2) & 0xFF00FFu;
            uint32_t g = (((a & 0xFF00u) + 2u * (m & 0xFF00u) + (b & 0xFF00u)) >> 2) & 0xFF00u;
            o[x] = 0xFF000000u | rb | g;
        }
    }
}

inline void vrow(int y, float f, int n, int* a, int* b, uint32_t* t) {
    float s = ((float)y + 0.5f) / f - 0.5f;
    if (s < 0.0f) s = 0.0f;
    int i0 = (int)s;
    uint32_t fr = (uint32_t)((s - (float)i0) * 256.0f);
    if (i0 >= n - 1) { i0 = n - 1; fr = 0; }
    *a = i0; *b = i0 + 1 < n ? i0 + 1 : i0; *t = fr;
}

// L2 bilinear back up to L1 size (satoru)
void up_l2() {
    for (int y = 0; y < g_h1n; y++) {
        int a, b; uint32_t t;
        vrow(y, 4.0f, g_h2n, &a, &b, &t);
        const uint32_t* ra = g_l2 + (size_t)a * g_w2;
        const uint32_t* rb = g_l2 + (size_t)b * g_w2;
        uint32_t* o = g_u2 + (size_t)y * g_w1;
        for (int x = 0; x < g_w1; x++) {
            uint32_t m = g_x21[x];
            uint32_t i0 = m >> 8, fr = m & 255u;
            uint32_t i1 = i0 + 1 < (uint32_t)g_w2 ? i0 + 1 : i0;
            o[x] = mixp(mixp(ra[i0], ra[i1], fr), mixp(rb[i0], rb[i1], fr), t);
        }
    }
}

// focus stages: 0 L2 -> L1b, 1 L1b -> L1, 2 L1 -> sharp, 3 sharp (satoru)
inline int focus_stage(float f, uint32_t* w256) {
    if (f < 0.40f) { *w256 = to256(f / 0.40f); return 0; }
    if (f < 0.70f) { *w256 = to256((f - 0.40f) / 0.30f); return 1; }
    if (f < 1.0f)  { *w256 = to256((f - 0.70f) / 0.30f); return 2; }
    *w256 = 256; return 3;
}

inline int iceil(float v) { int c = (int)v; return ((float)c < v) ? c + 1 : c; }
inline int ifloor(float v) { int c = (int)v; return ((float)c > v) ? c - 1 : c; }

// the pyramid is rebuilt in 5 slices, one per frame (4 bands of L1, then the
// small levels), so no frame pays for a whole rebuild. g_built_ms is set once
// a full pass has completed. (satoru)
void build_step(const uint32_t* bb, int pitch, int W, int H, uint64_t now) {
    if (g_bstep < 4) {
        int y0 = g_h1n * g_bstep / 4, y1 = g_h1n * (g_bstep + 1) / 4;
        down4(bb, W, H, pitch, g_l1, g_w1, g_h1n, 2, y0, y1);
        g_bstep++;
        return;
    }
    down4(g_l1, g_w1, g_h1n, g_w1, g_l2, g_w2, g_h2n, 1);
    blur121(g_l2, g_l2t, g_w2, g_h2n);
    blur121(g_l2, g_l2t, g_w2, g_h2n);
    up_l2();
    __builtin_memcpy(g_l1b, g_l1, (size_t)g_w1 * g_h1n * 4u);
    blur121(g_l1b, g_l1t, g_w1, g_h1n);
    blur121(g_l1b, g_l1t, g_w1, g_h1n);
    g_built_ms = now ? now : 1;
    g_bstep = 0;
}

typedef int v4si __attribute__((vector_size(16)));
inline v16qi mix4v(v16qi a, v16qi b, v8hi tlo, v8hi thi) {
    const v16qi z = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    v8hi alo = (v8hi)__builtin_ia32_punpcklbw128(a, z), ahi = (v8hi)__builtin_ia32_punpckhbw128(a, z);
    v8hi blo = (v8hi)__builtin_ia32_punpcklbw128(b, z), bhi = (v8hi)__builtin_ia32_punpckhbw128(b, z);
    v8hi lo = alo + (((blo - alo) * tlo) >> 7);
    v8hi hi = ahi + (((bhi - ahi) * thi) >> 7);
    return __builtin_ia32_packuswb128(lo, hi);
}

// widen one L1-size row to full width. with a 4x factor every group of 4
// outputs reads the same 3 neighbours at fixed weights, so it is one shuffle
// pair + one blend per 4 pixels. (satoru)
void widen_row(const uint32_t* r, int w1, uint32_t* o, int W) {
    const v8hi tlo = {80, 80, 80, 80, 112, 112, 112, 112}, thi = {16, 16, 16, 16, 48, 48, 48, 48};
    int k = 1, kend = 1;
    while (kend + 2 < w1 && 4 * kend + 3 < W) kend++;
    for (int x = 0; x < 4 && x < W; x++) {
        uint32_t m = g_x1[x], i0 = m >> 8, i1 = i0 + 1 < (uint32_t)w1 ? i0 + 1 : i0;
        o[x] = mixp(r[i0], r[i1], m & 255u);
    }
    for (; k < kend; k++) {
        v4si V;
        __builtin_memcpy(&V, r + k - 1, 16);
        v16qi A = (v16qi)__builtin_ia32_pshufd(V, 0x50);
        v16qi B = (v16qi)__builtin_ia32_pshufd(V, 0xA5);
        st4(o + 4 * k, mix4v(A, B, tlo, thi));
    }
    for (int x = 4 * kend; x < W; x++) {
        uint32_t m = g_x1[x], i0 = m >> 8, i1 = i0 + 1 < (uint32_t)w1 ? i0 + 1 : i0;
        o[x] = mixp(r[i0], r[i1], m & 255u);
    }
}

// keep refreshing the pyramid one slice per frame (a full refresh every 5
// frames tracks a changing desktop), then mix this frame's stage into C and
// widen it to HC. (satoru)
void prepare_levels(const uint32_t* bb, int pitch, int W, int H, int stage, uint32_t w256, uint32_t lift256,
                    uint64_t now) {
    if (stage >= 3) return;
    if (!g_built_ms) {
        while (!g_built_ms) build_step(bb, pitch, W, H, now);   // slow display: finish it now (satoru)
    } else {
        build_step(bb, pitch, W, H, now);
    }
    const int n = g_w1 * g_h1n;
    const uint32_t* A = stage == 0 ? g_u2 : (stage == 1 ? g_l1b : g_l1);
    const uint32_t* B = stage == 0 ? g_l1b : g_l1;
    if (stage == 2) __builtin_memcpy(g_c, A, (size_t)n * 4u);
    else mix_row(g_c, A, B, w256, n);
    if (lift256) {
        v8hi t = splat7(lift256);
        const v16qi wv = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        int i = 0;
        for (; i + 4 <= n; i += 4) st4(g_c + i, mix4(ld4(g_c + i), wv, t));
        for (; i < n; i++) g_c[i] = mixp(g_c[i], 0xFFFFFFFFu, lift256);
    }
    for (int y = 0; y < g_h1n; y++) widen_row(g_c + (size_t)y * g_w1, g_w1, g_hc + (size_t)y * W, W);
}

// reveal: wave front (radius R, rim E) over the scene. inside the front the
// pixel is the prepared blur (or its mix with the sharp desktop); the rim uses
// a per-frame table indexed by the squared distance, so no sqrt per pixel.
// (satoru)
void compose_reveal(uint32_t* bb, int pitch, const Scene& sc, float R, float E, int stage, uint32_t w256,
                    float rim) {
    const int W = sc.W, H = sc.H;
    const float AX = 1.25f, iax2 = 1.0f / (AX * AX);
    const float half = E * 0.5f;
    const float tin = R - half, tout = R + half;
    const float tout2 = tout * tout;
    const float q2min = tin > 0.0f ? tin * tin : 0.0f;
    const int LUTN = 512;
    static uint16_t lut_t[512];
    static int16_t lut_g[512];
    const float lscale = (tout2 > q2min) ? (float)(LUTN - 1) / (tout2 - q2min) : 0.0f;
    for (int i = 0; i < LUTN; i++) {
        float q = fsqrt(q2min + (float)i / (lscale > 0.0f ? lscale : 1.0f));
        lut_t[i] = (uint16_t)to256(smooth((tout - q) / E));
        float g = 1.0f - fabsf_(q - R) / half;
        lut_g[i] = (int16_t)(g > 0.0f ? rim * 255.0f * g * g : 0.0f);
    }
    for (int y = 0; y < H; y++) {
        uint32_t* row = bb + (size_t)y * pitch;
        const uint32_t* bgr = g_bg + (size_t)y * W;
        float dy = (float)y + 0.5f - g_ey;
        float dy2 = dy * dy;
        float hin = (tin > 0.0f && tin * tin > dy2) ? AX * fsqrt(tin * tin - dy2) : -1.0f;
        float hout = (tout > 0.0f && tout2 > dy2) ? AX * fsqrt(tout2 - dy2) : -1.0f;
        if (hout < 0.0f) {
            __builtin_memcpy(row, bgr, (size_t)W * 4u);
            continue;
        }
        int a1 = 0, b1 = 0; uint32_t t1 = 0;
        vrow(y, 4.0f, g_h1n, &a1, &b1, &t1);
        const uint32_t* ca = g_hc + (size_t)a1 * W;
        const uint32_t* cb = g_hc + (size_t)b1 * W;
        int xo0 = ifloor(g_ex - hout), xo1 = iceil(g_ex + hout) + 1;
        if (xo0 < 0) xo0 = 0;
        if (xo1 > W) xo1 = W;
        int xi0 = xo1, xi1 = xo1;
        if (hin >= 0.0f) {
            xi0 = iceil(g_ex - hin - 0.5f);
            xi1 = ifloor(g_ex + hin - 0.5f) + 1;
            if (xi0 < xo0) xi0 = xo0;
            if (xi1 > xo1) xi1 = xo1;
            if (xi1 < xi0) xi1 = xi0;
        }
        if (xo0 > 0) __builtin_memcpy(row, bgr, (size_t)xo0 * 4u);
        if (xo1 < W) __builtin_memcpy(row + xo1, bgr + xo1, (size_t)(W - xo1) * 4u);
        if (xi1 > xi0) {
            if (stage < 2) {
                mix_row(row + xi0, ca + xi0, cb + xi0, t1, xi1 - xi0);
            } else if (stage == 2) {
                mix_row(g_rowt + xi0, ca + xi0, cb + xi0, t1, xi1 - xi0);
                mix_row(row + xi0, g_rowt + xi0, row + xi0, w256, xi1 - xi0);
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            int s0 = pass ? xi1 : xo0, s1 = pass ? xo1 : xi0;
            for (int x = s0; x < s1; x++) {
                float dx = (float)x + 0.5f - g_ex;
                float q2 = dx * dx * iax2 + dy2;
                if (q2 >= tout2) { row[x] = bgr[x]; continue; }
                int li = (int)((q2 - q2min) * lscale);
                if (li < 0) li = 0;
                if (li >= LUTN) li = LUTN - 1;
                uint32_t B = stage < 2 ? mixp(ca[x], cb[x], t1)
                           : (stage == 2 ? mixp(mixp(ca[x], cb[x], t1), row[x], w256) : row[x]);
                uint32_t o = mixp(bgr[x], B, lut_t[li]);
                int add = lut_g[li];
                if (add > 0) o = addp(o, (add * 190) >> 8, (add * 205) >> 8, (add * 235) >> 8);
                row[x] = o;
            }
        }
    }
}

// time (satoru)
// shutdown plays the same film backwards from its last frame (satoru)
volatile int g_rev = 0, g_rev_done = 0, g_rev_pending = 0;
int g_rev_action = 0;            // 1 = halt the cpu when the film ends (satoru)
uint64_t g_rev_t0 = 0, g_rev_black = 0;
const float REV_SPEED = 1.6f;
int g_tail_ok = 0;

float anim_time(uint64_t now) {
    if (g_rev) {
        float a_end = g_tb + TC + TAIL_END - 1.0f;
        return a_end - (float)(now - g_rev_t0) * REV_SPEED;
    }
    float r = (float)(now - g_t0);
    float a;
    if (!g_ready || g_ready_r >= SOLID1) a = r;
    else {
        float rr = g_ready_r;
        float rF = rr + (SOLID1 - rr) / WARP;
        if (r < rr) a = r;
        else if (r < rF) a = rr + (r - rr) * WARP;
        else a = SOLID1 + (r - rF);
    }
    if (g_static && a < SOLID1) a = SOLID1;
    return a;
}

inline float tail_scale() { return g_fast ? 1.6f : 1.0f; }

void copy_bg_full(uint32_t* bb, int pitch, const Scene& sc, float fade) {
    if (fade >= 1.0f) {
        for (int y = 0; y < sc.H; y++)
            __builtin_memcpy(bb + (size_t)y * pitch, g_bg + (size_t)y * sc.W, (size_t)sc.W * 4u);
        return;
    }
    uint32_t k = to256(fade);
    for (int y = 0; y < sc.H; y++) {
        const uint32_t* s = g_bg + (size_t)y * sc.W;
        uint32_t* d = bb + (size_t)y * pitch;
        for (int x = 0; x < sc.W; x++) d[x] = mixp(0xFF000000u, s[x], k);
    }
}

void copy_bg_rect(uint32_t* bb, int pitch, const Scene& sc, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        __builtin_memcpy(bb + (size_t)y * pitch + x0, g_bg + (size_t)y * sc.W + x0, (size_t)(x1 - x0) * 4u);
}

bool geometry_ok() {
    return Graphics::GetBackBuffer() && Graphics::GetWidth() == g_sc.W && Graphics::GetHeight() == g_sc.H &&
           Graphics::GetBpp() == 32;
}

// boot progress: real milestones (Progress, pause, resume, gui) set a target;
// between milestones the goal creeps toward the next step on a slow
// exponential, so the bar never stops but never reaches the end on its own.
// the shown value chases the goal with a time constant, so it is the same at
// any frame rate. (satoru)
volatile float g_prog_target = 0.04f;
volatile uint64_t g_prog_ms = 0;
uint64_t g_bar_ms = 0, g_bar_done = 0;

bool system_ready(uint64_t now) {
    return g_ready && (float)(now - g_t0) >= g_ready_r;
}

void bar_update(uint64_t now, float a) {
    if (g_rev) { g_bar_alpha = 0.0f; return; }
    bool ready = system_ready(now);
    float dt = g_bar_ms ? (float)(now - g_bar_ms) : 0.0f;
    g_bar_ms = now;
    if (a >= FILL1) {
        float goal;
        if (ready) {
            goal = 1.0f;
        } else {
            float t = g_prog_target;
            float cap = t + 0.22f > 0.94f ? 0.94f : t + 0.22f;
            float k = 1.0f - fexpn(-(float)(now - g_prog_ms) / 2500.0f);
            goal = t + (cap - t) * k;
        }
        float follow = 1.0f - fexpn(-dt / (ready ? 130.0f : 260.0f));
        if (goal > g_bar) g_bar += (goal - g_bar) * follow;
        if (ready && g_bar > 0.996f) {
            g_bar = 1.0f;
            if (!g_bar_done) g_bar_done = now;
        }
    }
    float al = smooth((a - FILL1) / 300.0f);
    if (g_bar_done) al *= 1.0f - smooth((float)(now - g_bar_done) / 240.0f);
    g_bar_alpha = al;
}

void set_progress(float p) {
    if (p > g_prog_target) {
        g_prog_target = p;
        g_prog_ms = Timer::GetRealMs64();
    }
}

// ap frame (formation + hold, dirty rect) (satoru)
void ap_frame(uint64_t now) {
    if (!geometry_ok()) return;
    uint64_t c0 = rdtsc_();
    uint32_t* bb = (uint32_t*)Graphics::GetBackBuffer();
    int pitch = (int)(Graphics::GetPitch() / 4u);
    float a = anim_time(now);
    bar_update(now, a);
    const Scene& sc = g_sc;
    if (g_need_full || a < FADE_IN + 40.0f) {
        copy_bg_full(bb, pitch, sc, ease_out3(a / FADE_IN));
        Clip c = {0, 0, sc.W, sc.H};
        draw_formed(bb, pitch, c, sc, a, 0.0f);
        Graphics::MarkDirty(0, 0, sc.W, sc.H);
        if (a >= FADE_IN) g_need_full = 0;
    } else {
        copy_bg_rect(bb, pitch, sc, sc.rx0, sc.ry0, sc.rx1, sc.ry1);
        Clip c = {sc.rx0, sc.ry0, sc.rx1, sc.ry1};
        draw_formed(bb, pitch, c, sc, a, 0.0f);
        Graphics::MarkDirty(sc.rx0, sc.ry0, sc.rx1 - sc.rx0, sc.ry1 - sc.ry0);
    }
    Graphics::SwapBuffers();
    Graphics::PresentVirtioIfActive();
    stat_add(g_st_ap, rdtsc_() - c0);
}

bool start_rev(int action);

void finish() {
    g_mode = M_DONE;
    g_rev = 0;
    uint64_t now = Timer::GetRealMs64();
    SerialLogger::Log("[bootanim] done r=");
    SerialLogger::LogDec((int)(now - g_t0));
    SerialLogger::Log("ms ");
    stat_log("ap", g_st_ap);
    stat_log(" scene", g_st_scene);
    stat_log(" reveal", g_st_blur);
    SerialLogger::Log(" res=");
    SerialLogger::LogDec(g_sc.W); SerialLogger::Log("x"); SerialLogger::LogDec(g_sc.H);
    SerialLogger::Log("\r\n");
    pool_free();
    if (g_bg) PMM::FreeBytes(g_bg, g_bg_bytes);
    g_bg = nullptr; g_bg_bytes = 0;
    Graphics::MarkUIDirty();
    // a shutdown asked for while the boot film played starts now (satoru)
    if (g_rev_pending) {
        int act = g_rev_pending - 1;
        g_rev_pending = 0;
        if (!start_rev(act)) g_rev_done = 1;
    }
}

bool bg_alloc(int W, int H) {
    size_t need = (size_t)W * (size_t)H * 4u;
    if (g_bg && g_bg_bytes == need) return true;
    if (g_bg) PMM::FreeBytes(g_bg, g_bg_bytes);
    g_bg = (uint32_t*)PMM::AllocBytes(need);
    g_bg_bytes = g_bg ? need : 0;
    return g_bg != nullptr;
}

bool start_rev(int action) {
    int W = Graphics::GetWidth(), H = Graphics::GetHeight();
    if (W < 320 || H < 240 || Graphics::GetBpp() != 32 || !Graphics::GetBackBuffer()) return false;
    scene_setup(g_sc, W, H);
    if (!bg_alloc(W, H)) return false;
    render_bg(g_bg, g_sc);
    g_bg_ready = 1;
    tail_setup(g_sc);
    g_tail_ok = 1;
    g_fast = 0; g_static = 0; g_ready = 1;
    g_tb = SOLID1 + BEAT;
    g_st_scene.n = g_st_blur.n = 0;
    g_st_scene.sum = g_st_blur.sum = g_st_scene.max = g_st_blur.max = 0;
    g_rev_action = action;
    g_rev_done = 0;
    g_rev_black = 0;
    g_rev_t0 = Timer::GetRealMs64();
    g_rev = 1;
    __sync_synchronize();
    g_mode = M_GUI;
    SerialLogger::Log("[bootanim] shutdown film start\r\n");
    Graphics::MarkUIDirty();
    return true;
}

void spin_until(volatile int* flag, int want_clear, uint32_t max_ms) {
    uint64_t t = Timer::GetRealMs64();
    while (*flag && want_clear) {
        if (Timer::GetRealMs64() - t > max_ms) break;
        __asm__ volatile("pause");
    }
}

} // namespace (satoru)

// public (satoru)
bool Start(int mode) {
    if (mode <= 0) return false;
    int W = Graphics::GetWidth(), H = Graphics::GetHeight();
    if (W < 320 || H < 240 || Graphics::GetBpp() != 32 || !Graphics::GetBackBuffer()) return false;
    scene_setup(g_sc, W, H);
    if (!bg_alloc(W, H)) return false;
    g_fast = (mode == 2);
    g_t0 = Timer::GetRealMs64();
    g_prog_ms = g_t0;
    g_need_full = 1;
    g_mode = M_AP;
    bool have_ap = SMP::OnlineCount() > 1;
    SerialLogger::Log("[bootanim] start ");
    SerialLogger::LogDec(W); SerialLogger::Log("x"); SerialLogger::LogDec(H);
    SerialLogger::Log(have_ap ? " on an ap" : " static (no ap)");
    SerialLogger::Log(g_fast ? " fast\r\n" : "\r\n");
    if (have_ap) {
        __sync_synchronize();
        g_job = 1;
        return true;
    }
    // single cpu: one formed frame from the bsp, the tail still plays later (satoru)
    __sync_val_compare_and_swap(&g_claim, 0, 1);
    render_bg(g_bg, g_sc);
    g_bg_ready = 1;
    g_static = 1;
    uint32_t* bb = (uint32_t*)Graphics::GetBackBuffer();
    int pitch = (int)(Graphics::GetPitch() / 4u);
    copy_bg_full(bb, pitch, g_sc, 1.0f);
    Clip c = {0, 0, W, H};
    draw_formed(bb, pitch, c, g_sc, SOLID1, 0.0f);
    Graphics::MarkDirty(0, 0, W, H);
    Graphics::SwapBuffers();
    g_mode = M_PAUSE;
    return true;
}

bool ApPending() { return g_job != 0; }

void ApService() {
    if (!g_job) return;
    if (__sync_val_compare_and_swap(&g_claim, 0, 1) != 0) { g_job = 0; return; }
    g_job = 0;
    g_ap_in = 1;
    __sync_synchronize();
    render_bg(g_bg, g_sc);
    __sync_synchronize();
    g_bg_ready = 1;
    uint64_t next = 0;
    for (;;) {
        if (g_rebg) {   // display mode changed under us (satoru)
            __sync_synchronize();
            render_bg(g_bg, g_sc);
            g_rebg = 0;
            __sync_synchronize();
            g_bg_ready = 1;
            continue;
        }
        int m = g_mode;
        if (m != M_AP && m != M_PAUSE) break;
        uint64_t now = Timer::GetRealMs64();
        if (m == M_AP && now >= next) {
            g_drawing = 1;
            __sync_synchronize();
            if (g_mode == M_AP) ap_frame(now);
            __sync_synchronize();
            g_drawing = 0;
            uint32_t fps = Graphics::GetTargetFPS();
            if (fps < 30) fps = 60;
            if (fps > 240) fps = 240;
            uint64_t fms = 1000u / fps;
            next = now + (fms ? fms : 1);
            continue;
        }
        // far from the next frame: halt until the 100 hz ap tick, else spin (satoru)
        if (m == M_PAUSE || (next > now && next - now > 11)) __asm__ volatile("sti; hlt");
        else __asm__ volatile("pause");
    }
    __sync_synchronize();
    g_ap_in = 0;
}

void SetTest(int hold_ms, int early_ms) {
    g_test_hold = hold_ms > 0 ? hold_ms : 0;
    g_test_early = early_ms > 0 ? early_ms : 0;
}

void Pause() {
    if (g_mode != M_AP) return;
    g_mode = M_PAUSE;
    __sync_synchronize();
    spin_until(&g_drawing, 1, 200);
    set_progress(0.60f);   // desktop layout reached (satoru)
    SerialLogger::Log("[bootanim] pause r=");
    SerialLogger::LogDec((int)(Timer::GetRealMs64() - g_t0));
    SerialLogger::Log("ms\r\n");
}

void Progress(int pct) {
    set_progress((float)(pct < 0 ? 0 : (pct > 95 ? 95 : pct)) / 100.0f);
}

void Resume() {
    set_progress(0.80f);   // display manager is up (satoru)
    if (g_mode != M_PAUSE || g_static) return;
    if (!geometry_ok()) {
        // new mode or ui scale: lay the scene out again here (cheap) and let
        // the ap repaint the background before its next frame (satoru)
        int W = Graphics::GetWidth(), H = Graphics::GetHeight();
        if (W < 320 || H < 240 || Graphics::GetBpp() != 32 || !Graphics::GetBackBuffer() || !g_bg_ready) return;
        g_bg_ready = 0;
        __sync_synchronize();
        scene_setup(g_sc, W, H);
        if (!bg_alloc(W, H)) { g_mode = M_DONE; return; }
        g_rebg = 1;
        SerialLogger::Log("[bootanim] display changed, new scene\r\n");
    }
    g_need_full = 1;
    __sync_synchronize();
    g_mode = M_AP;
    SerialLogger::Log("[bootanim] resume r=");
    SerialLogger::LogDec((int)(Timer::GetRealMs64() - g_t0));
    SerialLogger::Log("ms\r\n");
}

void GuiTakeover() {
    int m = g_mode;
    if (m == M_OFF || m == M_DONE || m == M_GUI) return;   // once per boot (satoru)
    g_mode = M_GUI;
    __sync_synchronize();
    spin_until(&g_drawing, 1, 300);
    if (!g_bg_ready) {
        if (__sync_val_compare_and_swap(&g_claim, 0, 1) == 0) {
            g_job = 0;
            render_bg(g_bg, g_sc);
            g_bg_ready = 1;
        } else {
            uint64_t t = Timer::GetRealMs64();
            while (!g_bg_ready && Timer::GetRealMs64() - t < 300) __asm__ volatile("pause");
            if (!g_bg_ready) { finish(); return; }
        }
    }
    uint64_t now = Timer::GetRealMs64();
    g_ready_r = (float)(now - g_t0);
    if (g_test_early && g_ready_r > (float)g_test_early) {
        g_t0 = now - (uint64_t)g_test_early;
        g_ready_r = (float)g_test_early;
        g_static = 0;
    }
    if (g_test_hold && (float)g_test_hold > g_ready_r) g_ready_r = (float)g_test_hold;
    g_ready = 1;
    // the break waits for the loading bar to fill and fade (Composite sets it) (satoru)
    g_tb = 1.0e9f;
    SerialLogger::Log("[bootanim] gui takeover r=");
    SerialLogger::LogDec((int)g_ready_r);
    SerialLogger::Log("ms ");
    stat_log("ap", g_st_ap);
    SerialLogger::Log("\r\n");
    Graphics::MarkUIDirty();
}

bool Active() { return g_mode == M_GUI; }

bool StartShutdown(int action) {
    if (g_mode == M_OFF) return false;   // animations disabled or no display (satoru)
    if (g_rev) return true;
    if (g_mode == M_GUI || g_mode == M_AP || g_mode == M_PAUSE) {
        g_rev_pending = action + 1;      // after the boot film (satoru)
        return true;
    }
    return start_rev(action);
}

bool ShutdownDone() { return g_rev_done != 0; }

bool CoversScreen() {
    if (g_mode != M_GUI || !g_ready) return false;
    float a = anim_time(Timer::GetRealMs64());
    if (a < g_tb) return true;
    // the reveal needs the desktop from shortly before the wave (satoru)
    return (a - g_tb) * tail_scale() < TC - 260.0f;
}

void Composite() {
    if (g_mode != M_GUI) return;
    if (__sync_lock_test_and_set(&g_busy, 1)) return;
    uint64_t c0 = rdtsc_();
    uint32_t* bb = (uint32_t*)Graphics::GetBackBuffer();
    int W = Graphics::GetWidth(), H = Graphics::GetHeight();
    if (!bb || Graphics::GetBpp() != 32 || W < 320 || H < 240) { finish(); __sync_lock_release(&g_busy); return; }
    if (W != g_sc.W || H != g_sc.H) {
        // the display mode or ui scale changed after the ap phase (satoru)
        scene_setup(g_sc, W, H);
        if (!bg_alloc(W, H)) { finish(); __sync_lock_release(&g_busy); return; }
        render_bg(g_bg, g_sc);
        g_tail_ok = 0;
    }
    if (!g_tail_ok) { tail_setup(g_sc); g_tail_ok = 1; }
    int pitch = (int)(Graphics::GetPitch() / 4u);
    const Scene& sc = g_sc;
    Clip full = {0, 0, W, H};
    uint64_t now = Timer::GetRealMs64();
    float a = anim_time(now);
    bool reveal = false;
    Graphics::MarkUIDirty();   // keep every ui pump drawing while the film plays (satoru)
    if (g_rev && a <= 0.0f) {
        // shutdown film over: hold black until the caller powers off (satoru)
        for (int y = 0; y < H; y++) __builtin_memset(bb + (size_t)y * pitch, 0, (size_t)W * 4u);
        Graphics::MarkDirty(0, 0, W, H);
        if (!g_rev_done) {
            g_rev_done = 1;
            g_rev_black = now;
            SerialLogger::Log("[bootanim] shutdown film done\r\n");
        }
        __sync_lock_release(&g_busy);
        if (g_rev_action == 1 && now - g_rev_black > 150u) {
            HAL::DisableInterrupts();
            HAL::Halt();
        }
        return;
    }
    if (!g_rev && g_tb > 1.0e8f) {
        // loading finished: once the bar has filled and faded on a formed
        // emblem, schedule the break (satoru)
        bar_update(now, a);
        if (g_bar_done && a >= SOLID1 && now - g_bar_done >= 240u) {
            g_tb = a + BEAT;
            SerialLogger::Log("[bootanim] loaded, break at r=");
            SerialLogger::LogDec((int)(now - g_t0));
            SerialLogger::Log("ms\r\n");
        }
    }
    if (a < g_tb) {
        copy_bg_full(bb, pitch, sc, ease_out3(a / FADE_IN));
        draw_formed(bb, pitch, full, sc, a, 0.0f);
    } else {
        float b = (a - g_tb) * tail_scale();
        float tc = b - TC;
        if (tc >= TAIL_END && !g_rev) {
            finish();
            __sync_lock_release(&g_busy);
            return;
        }
        if (tc < WAVE0) {
            // the back buffer still holds this frame's desktop: build the blur
            // pyramid now, in a cheap frame, so the first reveal frame does not
            // pay for it (satoru)
            if (tc >= -160.0f && pool_alloc(W, H) && !g_built_ms) build_step(bb, pitch, W, H, Timer::GetRealMs64());
            copy_bg_full(bb, pitch, sc, 1.0f);
            if (b < ANTIC) {
                // the formed emblem flashes and swells before it breaks (satoru)
                Poly pa, pb2; uint32_t f; float br;
                float hb = 0.0f;
                tail_piece(sc, 0, b, pa, &f, &br); hb = br;
                tail_piece(sc, 1, b, pb2, &f, &br);
                (void)hb;
                // the crack: a glint of light where the two strips meet (satoru)
                float gx = sc.cx + (133.0f - kSvgCX) * sc.s, gy = sc.cy + (169.5f - kSvgCY) * sc.s;
                float gk = fsin(PI * sat(b / ANTIC));
                float gl = 0.05f * sc.oh * gk;
                add_streak(bb, pitch, full, gx - gl, gy, gx, gy, 0.9f * sc.u, 230, 238, 255, 0.9f * gk);
                add_streak(bb, pitch, full, gx + gl, gy, gx, gy, 0.9f * sc.u, 230, 238, 255, 0.9f * gk);
                add_streak(bb, pitch, full, gx, gy - gl * 0.45f, gx, gy, 0.8f * sc.u, 230, 238, 255, 0.7f * gk);
                add_streak(bb, pitch, full, gx, gy + gl * 0.45f, gx, gy, 0.8f * sc.u, 230, 238, 255, 0.7f * gk);
                add_glow(bb, pitch, full, gx, gy, 4.0f * sc.u, 4.0f * sc.u, 255, 255, 255, 0.8f * gk);
            }
            draw_pieces(bb, pitch, full, sc, b);
        } else {
            reveal = true;
            if (!pool_alloc(W, H)) { finish(); __sync_lock_release(&g_busy); return; }
            float f = ease_io3((tc - RES0) / RES_DUR);
            // the wave keeps some speed to the end, so it reads as rising
            // light rather than a pop (satoru)
            float wp = (float)WMMotion::Bezier(0.32f, 0.42f, 0.25f, 1.0f, (int)(tc - WAVE0), (int)WAVE_DUR) / 4096.0f;
            float AX = 1.25f;
            float far_x = g_ex > (float)W - g_ex ? g_ex : (float)W - g_ex;
            float E = 0.36f * (float)sc.oh;   // a wider, softer front than a hard dome edge (satoru)
            float Rreq = fsqrt((far_x / AX) * (far_x / AX) + g_ey * g_ey);
            float R = (Rreq + E) * wp;
            uint32_t w256;
            int stage = focus_stage(f, &w256);
            float lift = 0.14f * (1.0f - f);
            float rim = 0.10f * (1.0f - wp);
            prepare_levels(bb, pitch, W, H, stage, w256, to256(lift), Timer::GetRealMs64());
            compose_reveal(bb, pitch, sc, R, E, stage, w256, rim);
        }
        draw_burst(bb, pitch, full, sc, b);
    }
    Graphics::MarkDirty(0, 0, W, H);
    uint64_t cyc = rdtsc_() - c0;
    stat_add(reveal ? g_st_blur : g_st_scene, cyc);
    {   // name the few frames that blow a 60 hz budget (satoru)
        static int s_slow = 0;
        uint64_t us = cyc / tsc_per_us();
        if (us > 16000 && s_slow < 6) {
            s_slow++;
            SerialLogger::Log("[bootanim] slow frame us=");
            SerialLogger::LogDec((int)us);
            SerialLogger::Log(" a=");
            SerialLogger::LogDec((int)a);
            SerialLogger::Log(reveal ? " reveal\r\n" : " scene\r\n");
        }
    }
    __sync_lock_release(&g_busy);
}

} // namespace BootAnim (satoru)

// end (satoru)
