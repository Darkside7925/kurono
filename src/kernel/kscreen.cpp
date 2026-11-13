// kscreen: see kscreen.h. (satoru)
#include "kscreen.h"
#include "../ui/vga_font.h"

namespace KScreen {
namespace {

struct Ctx {
    const Fb* fb;
    uint32_t bg;
    uint32_t fg;
};

inline float fsqrt(float x) {
    float r;
    __asm__ ("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
inline float fclamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

uint32_t mix(uint32_t a, uint32_t b, int t255) {
    if (t255 <= 0) return a;
    if (t255 >= 255) return b;
    uint32_t r = (((a >> 16) & 0xFF) * (255 - t255) + ((b >> 16) & 0xFF) * t255) / 255;
    uint32_t g = (((a >> 8) & 0xFF) * (255 - t255) + ((b >> 8) & 0xFF) * t255) / 255;
    uint32_t bl = ((a & 0xFF) * (255 - t255) + (b & 0xFF) * t255) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

void px(const Fb& fb, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb.width || (uint32_t)y >= fb.height) return;
    volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)(fb.addr + (uint64_t)y * fb.pitch + (uint64_t)x * (fb.bpp / 8));
    uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    if (fb.bpp == 32) *(volatile uint32_t*)p = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    else if (fb.bpp == 24) { p[0] = b; p[1] = g; p[2] = r; }
    else if (fb.bpp == 16) *(volatile uint16_t*)p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void rect(const Fb& fb, int x, int y, int w, int h, uint32_t c) {
    int x0 = imax(x, 0), y0 = imax(y, 0);
    int x1 = imin(x + w, (int)fb.width), y1 = imin(y + h, (int)fb.height);
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) {
        if (fb.bpp == 32) {
            volatile uint32_t* row = (volatile uint32_t*)(uintptr_t)(fb.addr + (uint64_t)yy * fb.pitch) + x0;
            uint32_t v = 0xFF000000u | c;
            for (int xx = x0; xx < x1; xx++) *row++ = v;
        } else {
            for (int xx = x0; xx < x1; xx++) px(fb, xx, yy, c);
        }
    }
    __asm__ __volatile__("sfence" ::: "memory");
}

// ---- text: the vga 8x16 font at an integer scale --------------------------
int text_w(const char* s, int scale, int n = -1) {
    int k = 0;
    while (s[k] && (n < 0 || k < n)) k++;
    return k * 8 * scale;
}

void glyph(const Ctx& c, int x, int y, unsigned char ch, int scale, uint32_t col) {
    const unsigned char* g = vga_font_8x16[ch];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        int col0 = -1;
        for (int bit = 0; bit <= 8; bit++) {
            bool on = bit < 8 && (bits & (0x80 >> bit));
            if (on && col0 < 0) col0 = bit;
            if (!on && col0 >= 0) {
                rect(*c.fb, x + col0 * scale, y + row * scale, (bit - col0) * scale, scale, col);
                col0 = -1;
            }
        }
    }
}

void text(const Ctx& c, int x, int y, const char* s, int scale, uint32_t col, int n = -1) {
    for (int i = 0; s[i] && (n < 0 || i < n); i++) glyph(c, x + i * 8 * scale, y, (unsigned char)s[i], scale, col);
}

// word wrap into lines no wider than maxw; returns the y after the last line. (satoru)
int wrap(const Ctx& c, int x, int y, const char* s, int scale, uint32_t col, int maxw, int line_h) {
    int maxc = maxw / (8 * scale);
    if (maxc < 8) maxc = 8;
    int i = 0;
    while (s[i]) {
        if (s[i] == '\n') { i++; y += line_h / 2; continue; }
        int len = 0, last_space = -1;
        while (s[i + len] && s[i + len] != '\n' && len < maxc) {
            if (s[i + len] == ' ') last_space = len;
            len++;
        }
        int take = len;
        if (s[i + len] && s[i + len] != '\n' && last_space > 0) take = last_space;
        text(c, x, y, s + i, scale, col, take);
        y += line_h;
        i += take;
        while (s[i] == ' ') i++;
    }
    return y;
}

// ---- anti-aliased shapes over the known background ------------------------
void disc(const Ctx& c, float cx, float cy, float r, uint32_t col) {
    int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 2), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 2);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = fclamp(r + 0.5f - fsqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            if (cov > 0.0f) px(*c.fb, x, y, mix(c.bg, col, (int)(cov * 255.0f)));
        }
}

// the mouth of the ":(": the left part of a ring (cos(angle) < lim). (satoru)
void arc_left(const Ctx& c, float cx, float cy, float r, float t, float lim, uint32_t col) {
    int x0 = (int)(cx - r - t - 1), x1 = (int)(cx + 1), y0 = (int)(cy - r - t - 1), y1 = (int)(cy + r + t + 2);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = fsqrt(dx * dx + dy * dy);
            if (d < 0.001f || dx / d > lim) continue;
            float e = d - r; if (e < 0) e = -e;
            float cov = fclamp(t * 0.5f + 0.5f - e, 0.0f, 1.0f);
            if (cov > 0.0f) px(*c.fb, x, y, mix(c.bg, col, (int)(cov * 255.0f)));
        }
}

void face(const Ctx& c, int x, int y, int size) {
    float f = (float)size;
    disc(c, x + f * 0.10f, y + f * 0.30f, f * 0.085f, c.fg);
    disc(c, x + f * 0.10f, y + f * 0.72f, f * 0.085f, c.fg);
    arc_left(c, x + f * 0.98f, y + f * 0.51f, f * 0.50f, f * 0.11f, -0.62f, c.fg);
}

// rounded rectangle: filled, or a 2 px outline. (satoru)
void rrect(const Ctx& c, int x, int y, int w, int h, float r, uint32_t col, bool fill) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            float px0 = xx + 0.5f - (x + w * 0.5f), py0 = yy + 0.5f - (y + h * 0.5f);
            float qx = (px0 < 0 ? -px0 : px0) - (w * 0.5f - r), qy = (py0 < 0 ? -py0 : py0) - (h * 0.5f - r);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float sd = fsqrt(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
            float cov = fill ? fclamp(0.5f - sd, 0.0f, 1.0f)
                             : fclamp(1.5f - (sd < 0 ? -sd : sd), 0.0f, 1.0f);
            if (cov > 0.0f) px(*c.fb, xx, yy, mix(c.bg, col, (int)(cov * 255.0f)));
        }
}

// twelve dots around a circle, the lead one brightest. (satoru)
void spinner(const Ctx& c, int cx, int cy, int r, int phase) {
    static const float kc[12] = { 1.0f, 0.866f, 0.5f, 0.0f, -0.5f, -0.866f, -1.0f, -0.866f, -0.5f, 0.0f, 0.5f, 0.866f };
    static const float ks[12] = { 0.0f, 0.5f, 0.866f, 1.0f, 0.866f, 0.5f, 0.0f, -0.5f, -0.866f, -1.0f, -0.866f, -0.5f };
    rect(*c.fb, cx - r - 6, cy - r - 6, 2 * r + 12, 2 * r + 12, c.bg);
    for (int i = 0; i < 12; i++) {
        int age = (phase - i + 120) % 12;
        int a = 255 - age * 18;
        if (a < 50) a = 50;
        disc(c, cx + kc[i] * r, cy + ks[i] * r, r * 0.16f + 1.0f, mix(c.bg, c.fg, a));
    }
}

struct Geo {
    int unit;        // text scale for the body (satoru)
    int head;        // text scale for the headline (satoru)
    int left, top, maxw;
    int face;
};

Geo geo(const Fb& fb) {
    Geo g;
    g.unit = fb.height >= 1400 ? 3 : (fb.height >= 700 ? 2 : 1);
    g.head = fb.width >= 2400 ? 5 : (fb.width >= 1500 ? 4 : (fb.width >= 900 ? 3 : 2));
    g.left = (int)fb.width / 9;
    g.top = (int)fb.height / 7;
    g.maxw = (int)fb.width - 2 * g.left;
    g.face = (int)fb.height / 6;
    return g;
}

int to_dec(int v, char* out) {
    char t[12]; int n = 0;
    if (v <= 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0; while (n) out[o++] = t[--n];
    out[o] = 0;
    return o;
}

// where the progress / countdown line sits, shared by Draw and DrawProgress. (satoru)
int g_prog_y = 0;

void draw_progress(const Ctx& c, const Geo& g, const Screen& s) {
    if (!g_prog_y) return;
    int lh = 16 * g.unit;
    rect(*c.fb, g.left, g_prog_y - 4, g.maxw, lh * 2 + 8, c.bg);
    uint32_t dim = mix(c.bg, c.fg, 200);
    if (s.progress >= 0) {
        int r = lh * 3 / 4;
        spinner(c, g.left + r, g_prog_y + lh / 2 + 2, r, s.progress);
        if (s.progress_text) text(c, g.left + 2 * r + 8 * g.unit + 4, g_prog_y, s.progress_text, g.unit, dim);
    } else if (s.countdown >= 0) {
        char buf[64]; int p = 0;
        const char* a = "Restarting in ";
        while (*a) buf[p++] = *a++;
        p += to_dec(s.countdown, buf + p);
        a = s.countdown == 1 ? " second" : " seconds";
        while (*a) buf[p++] = *a++;
        buf[p] = 0;
        text(c, g.left, g_prog_y, buf, g.unit, dim);
    }
}

}  // namespace

void InitScreen(Screen& s) {
    s.tone = TONE_BLUE; s.headline = nullptr; s.body = nullptr; s.code = nullptr; s.tech = nullptr;
    s.progress = -1; s.progress_text = nullptr; s.choices = nullptr; s.nchoices = 0; s.selected = 0;
    s.countdown = -1;
}

// the entry points realign the stack: they also run from exception handlers,
// whose stacks are not 16 byte aligned, and the float code below may use
// aligned sse spills (a misaligned movaps is a #GP inside the panic). (satoru)
__attribute__((force_align_arg_pointer))
void Draw(const Fb& fb, const Screen& s) {
    if (!fb.addr || !fb.width || !fb.height || (fb.bpp != 16 && fb.bpp != 24 && fb.bpp != 32)) return;
    Ctx c;
    c.fb = &fb;
    c.bg = s.tone == TONE_RED ? 0xFF8A1C1Cu : 0xFF103E86u;
    c.fg = 0xFFFFFFFFu;
    Geo g = geo(fb);
    rect(fb, 0, 0, (int)fb.width, (int)fb.height, c.bg);
    face(c, g.left, g.top, g.face);
    int y = g.top + g.face + g.face / 3;
    if (s.headline) y = wrap(c, g.left, y, s.headline, g.head, c.fg, g.maxw, 16 * g.head + 8 * g.head / 2);
    y += 10 * g.unit;
    if (s.body) y = wrap(c, g.left, y, s.body, g.unit, mix(c.bg, c.fg, 225), g.maxw, 16 * g.unit + 6 * g.unit);
    y += 14 * g.unit;
    g_prog_y = 0;
    if (s.progress >= 0 || s.countdown >= 0) {
        g_prog_y = y;
        draw_progress(c, g, s);
        y += 16 * g.unit * 2 + 8;
    }
    if (s.choices && s.nchoices > 0) {
        int bh = 16 * g.unit + 18 * g.unit / 2 + 8;
        int x = g.left;
        for (int i = 0; i < s.nchoices; i++) {
            int bw = text_w(s.choices[i], g.unit) + 24 * g.unit;
            if (x + bw > g.left + g.maxw && x > g.left) { x = g.left; y += bh + 10 * g.unit; }
            bool sel = i == s.selected;
            if (sel) rrect(c, x, y, bw, bh, bh * 0.5f, c.fg, true);
            else rrect(c, x, y, bw, bh, bh * 0.5f, mix(c.bg, c.fg, 170), false);
            Ctx tc = c;
            if (sel) tc.bg = c.fg;
            text(tc, x + 12 * g.unit, y + (bh - 16 * g.unit) / 2, s.choices[i], g.unit, sel ? c.bg : c.fg);
            x += bw + 10 * g.unit;
        }
        y += bh;
    }
    // the technical line sits near the bottom, small and grey. (satoru)
    int ty = (int)fb.height - (int)fb.height / 9;
    if (ty < y + 20) ty = y + 20;
    int tscale = g.unit > 1 ? g.unit - 1 : 1;
    int tx = g.left;
    uint32_t grey = mix(c.bg, c.fg, 150);
    if (s.code) { text(c, tx, ty, s.code, tscale, mix(c.bg, c.fg, 215)); tx += text_w(s.code, tscale) + 16 * tscale; }
    if (s.tech) wrap(c, tx, ty, s.tech, tscale, grey, g.left + g.maxw - tx, 16 * tscale + 4);
    __asm__ __volatile__("sfence; mfence" ::: "memory");
}

__attribute__((force_align_arg_pointer))
void DrawProgress(const Fb& fb, const Screen& s) {
    if (!fb.addr || !g_prog_y) return;
    Ctx c;
    c.fb = &fb;
    c.bg = s.tone == TONE_RED ? 0xFF8A1C1Cu : 0xFF103E86u;
    c.fg = 0xFFFFFFFFu;
    Geo g = geo(fb);
    draw_progress(c, g, s);
    __asm__ __volatile__("sfence; mfence" ::: "memory");
}

__attribute__((force_align_arg_pointer))
void DrawConsole(const Fb& fb, Tone tone, const char* const* lines, int n, const char* input) {
    if (!fb.addr || !fb.width || !fb.height) return;
    Ctx c;
    c.fb = &fb;
    c.bg = tone == TONE_RED ? 0xFF5E1212u : 0xFF0A2A5Cu;   // a shade darker than the screen (satoru)
    c.fg = 0xFFFFFFFFu;
    int scale = fb.height >= 1400 ? 2 : 1;
    int lh = 16 * scale + 2;
    int x = 24, y = 20;
    rect(fb, 0, 0, (int)fb.width, (int)fb.height, c.bg);
    int rows = ((int)fb.height - 2 * y) / lh - 1;
    int maxc = ((int)fb.width - 2 * x) / (8 * scale);
    int first = n > rows ? n - rows : 0;
    uint32_t dim = mix(c.bg, c.fg, 215);
    for (int i = first; i < n; i++) {
        const char* l = lines[i] ? lines[i] : "";
        text(c, x, y, l, scale, dim, maxc);
        y += lh;
    }
    text(c, x, y, "> ", scale, c.fg);
    int il = 0; while (input && input[il]) il++;
    int show = il > maxc - 4 ? il - (maxc - 4) : 0;
    text(c, x + 16 * scale, y, input ? input + show : "", scale, c.fg);
    rect(fb, x + 16 * scale + (il - show) * 8 * scale, y + 2, 2 * scale, 14 * scale, c.fg);
    __asm__ __volatile__("sfence; mfence" ::: "memory");
}

__attribute__((force_align_arg_pointer))
void DrawMenu(const Fb& fb, const char* title, const char* subtitle,
              const MenuItem* items, int n, int sel, const char* hint) {
    if (!fb.addr || !fb.width || !fb.height) return;
    Ctx c;
    c.fb = &fb;
    c.bg = 0xFF16181Du;
    c.fg = 0xFFFFFFFFu;
    Geo g = geo(fb);
    rect(fb, 0, 0, (int)fb.width, (int)fb.height, c.bg);
    int unit = g.unit;
    int left = (int)fb.width / 6, w = (int)fb.width - 2 * left;
    int y = (int)fb.height / 10;
    if (title) { text(c, left, y, title, g.head > 2 ? g.head - 1 : 2, c.fg); y += 16 * (g.head > 2 ? g.head - 1 : 2) + 10 * unit; }
    if (subtitle) y = wrap(c, left, y, subtitle, unit, mix(c.bg, c.fg, 170), w, 16 * unit + 4 * unit);
    y += 12 * unit;
    int sub_scale = unit > 1 ? unit - 1 : 1;
    int rh = 16 * unit + 16 * sub_scale + 18 * unit / 2 + 6;
    int avail = (int)fb.height - y - 40 * unit;
    int per = rh + 6 * unit;
    int first = 0;
    if (per > 0 && n * per > avail) {
        int vis = avail / per;
        if (vis < 1) vis = 1;
        first = sel - vis / 2;
        if (first < 0) first = 0;
        if (first > n - vis) first = n - vis;
    }
    for (int i = first; i < n; i++) {
        if (y + rh > (int)fb.height - 30 * unit) break;
        bool on = i == sel;
        uint32_t row_bg = on ? 0xFF2E5BDAu : mix(c.bg, c.fg, 16);
        rrect(c, left, y, w, rh, 12.0f * unit / 2 + 4, row_bg, true);
        Ctx rc = c; rc.bg = row_bg;
        text(rc, left + 16 * unit, y + 8 * unit / 2 + 4, items[i].label ? items[i].label : "", unit, c.fg);
        if (items[i].sub) text(rc, left + 16 * unit, y + 8 * unit / 2 + 6 + 16 * unit, items[i].sub, sub_scale,
                               on ? mix(row_bg, c.fg, 215) : mix(c.bg, c.fg, 150), (w - 32 * unit) / (8 * sub_scale));
        y += per;
    }
    if (hint) text(c, left, (int)fb.height - 26 * unit, hint, sub_scale, mix(c.bg, c.fg, 130));
    __asm__ __volatile__("sfence; mfence" ::: "memory");
}

void FormatStopCode(char* out, const char* area, uint32_t number) {
    static const char* hex = "0123456789ABCDEF";
    int p = 0;
    out[p++] = 'K'; out[p++] = '-';
    for (int i = 0; area && area[i] && i < 4; i++) out[p++] = area[i];
    out[p++] = '-';
    for (int i = 3; i >= 0; i--) out[p++] = hex[(number >> (i * 4)) & 0xF];
    out[p] = 0;
}

void PanicStopCode(uint32_t sc, char* out) {
    // panic.h codes: < 0x20 mirror the cpu vector; 0x0001xxxx subsystem. (satoru)
    if (sc < 0x20) { FormatStopCode(out, "KRN", sc); return; }
    switch (sc) {
        case 0x00010001: FormatStopCode(out, "MEM", 0x0101); return;   // heap corruption (satoru)
        case 0x00010005: FormatStopCode(out, "MEM", 0x0105); return;   // out of memory (satoru)
        case 0x0001000A: FormatStopCode(out, "MEM", 0x010A); return;   // vmm fault (satoru)
        case 0x0001000B: FormatStopCode(out, "MEM", 0x010B); return;   // kmemx crc (satoru)
        case 0x00010008: FormatStopCode(out, "SCH", 0x0108); return;   // scheduler (satoru)
        case 0x00010006: FormatStopCode(out, "DRV", 0x0106); return;   // driver fault (satoru)
        default: break;
    }
    if ((sc & 0xFFFF0000u) == 0x00010000u) { FormatStopCode(out, "KRN", 0x0100 | (sc & 0xFF)); return; }
    if ((sc & 0xFFFF0000u) == 0xDEAD0000u) { FormatStopCode(out, "KRN", 0x0200 | (sc & 0xFF)); return; }
    FormatStopCode(out, "KRN", 0x0FFF);
}

}  // namespace KScreen

// end (satoru)
