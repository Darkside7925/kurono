// kui_canvas.cpp: kui::Canvas and the backend-neutral helpers, shared by both backends (satoru)
// shapes go through kui_raster (ports of today's kernel drawing); text goes through the backend's (satoru)
// bk:: hooks (FontTTF in the kernel, the same stb glue + glyph code in ring 3). Menu, Toast and (satoru)
// AppTile are MDraw's, rebuilt on Canvas calls so they clip and look the same on both sides. (satoru)
#include "kui_internal.h"

namespace kui {

// ---- setup (satoru)

void CanvasInit(Canvas::Impl& ci, uint32_t* px, int pitch, int tw, int th,
                int ox, int oy, int cw, int ch, int bx0, int by0, int bx1, int by1) {
    ci.t.px = px; ci.t.pitch = pitch; ci.t.w = tw; ci.t.h = th;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > tw) bx1 = tw;
    if (by1 > th) by1 = th;
    if (bx0 < ox) bx0 = ox;
    if (by0 < oy) by0 = oy;
    if (bx1 > ox + cw) bx1 = ox + cw;
    if (by1 > oy + ch) by1 = oy + ch;
    if (bx1 < bx0) bx1 = bx0;
    if (by1 < by0) by1 = by0;
    ci.t.x0 = bx0; ci.t.y0 = by0; ci.t.x1 = bx1; ci.t.y1 = by1;
    ci.ox = ox; ci.oy = oy; ci.cw = cw; ci.ch = ch;
    ci.damage = Rect{ bx0 - ox, by0 - oy, bx1 - bx0, by1 - by0 };
    ci.sp = 0;
}

uint8_t ClickCount(int x, int y, uint32_t now) {
    static uint32_t last_ms = 0;
    static int lx = -100000, ly = -100000;
    static uint8_t last_n = 0;
    int dx = x - lx, dy = y - ly;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    uint8_t n = (last_n == 1 && now - last_ms < 400 && dx < 10 && dy < 10) ? 2 : 1;
    last_ms = now; lx = x; ly = y; last_n = n;
    return n;
}

// ---- small helpers (satoru)

Color Mix(Color a, Color b, int t) { return raster::Mix(a, b, t); }

float Ease(uint32_t t0, int delay_ms, int dur_ms) {
    if (CurrentTheme().reduced_motion) return 1.0f;
    int el = (int)(NowMs() - t0) - delay_ms;
    if (el <= 0) return 0.0f;
    if (el >= dur_ms) return 1.0f;
    float t = (float)el / (float)dur_ms;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static int k_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

bool FontsReady() { return bk::FontsOk(); }

int MeasureWidth(Face f, float px, const char* s) { return s ? bk::TextWidth(f, px, s) : 0; }

int Ascent(Face f, float px) { return bk::TextAscent(f, px); }

TextMetrics Measure(Face f, float px, const char* s) {
    TextMetrics m;
    m.width = MeasureWidth(f, px, s);
    m.ascent = Ascent(f, px);
    m.line_h = (int)(px * 1.25f + 0.5f);
    m.descent = m.line_h - m.ascent;
    if (m.descent < 0) m.descent = 0;
    return m;
}

int MeasureN(Face f, float px, const char* s, int n) {
    if (!s || n <= 0) return 0;
    char buf[512];
    int k = 0;
    while (s[k] && k < n && k < (int)sizeof(buf) - 1) { buf[k] = s[k]; k++; }
    buf[k] = 0;
    return MeasureWidth(f, px, buf);
}

int FitChars(Face f, float px, const char* s, int maxw) {
    int n = k_len(s);
    if (n > 511) n = 511;
    while (n > 0 && MeasureN(f, px, s, n) > maxw) n--;
    return n;
}

// ---- canvas (satoru)

int  Canvas::Width() const  { return impl_->cw; }
int  Canvas::Height() const { return impl_->ch; }
Rect Canvas::Damage() const { return impl_->damage; }

void Canvas::PushClip(int x, int y, int w, int h) {
    Impl& c = *impl_;
    if (c.sp < 16) c.stack[c.sp] = Impl::Box{ c.t.x0, c.t.y0, c.t.x1, c.t.y1 };
    c.sp++;
    int x0 = c.ox + x, y0 = c.oy + y, x1 = x0 + (w > 0 ? w : 0), y1 = y0 + (h > 0 ? h : 0);
    if (x0 < c.t.x0) x0 = c.t.x0;
    if (y0 < c.t.y0) y0 = c.t.y0;
    if (x1 > c.t.x1) x1 = c.t.x1;
    if (y1 > c.t.y1) y1 = c.t.y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    c.t.x0 = x0; c.t.y0 = y0; c.t.x1 = x1; c.t.y1 = y1;
}

void Canvas::PopClip() {
    Impl& c = *impl_;
    if (c.sp <= 0) return;
    c.sp--;
    if (c.sp < 16) { c.t.x0 = c.stack[c.sp].x0; c.t.y0 = c.stack[c.sp].y0; c.t.x1 = c.stack[c.sp].x1; c.t.y1 = c.stack[c.sp].y1; }
}

void Canvas::Clear(Color c) { raster::FillRect(impl_->t, impl_->ox, impl_->oy, impl_->cw, impl_->ch, c); }
void Canvas::FillRect(int x, int y, int w, int h, Color c) { raster::FillRect(impl_->t, impl_->ox + x, impl_->oy + y, w, h, c); }
void Canvas::FillRectAlpha(int x, int y, int w, int h, uint8_t a, Color c) { raster::FillRectAlpha(impl_->t, impl_->ox + x, impl_->oy + y, w, h, a, c); }
void Canvas::FillRoundedRect(int x, int y, int w, int h, int r, Color c) { raster::FillRectRounded(impl_->t, impl_->ox + x, impl_->oy + y, w, h, r, c); }

void Canvas::DrawRect(int x, int y, int w, int h, Color c) {
    int X = impl_->ox + x, Y = impl_->oy + y;
    raster::Line(impl_->t, X, Y, X + w - 1, Y, c, 1);
    raster::Line(impl_->t, X, Y + h - 1, X + w - 1, Y + h - 1, c, 1);
    raster::Line(impl_->t, X, Y, X, Y + h - 1, c, 1);
    raster::Line(impl_->t, X + w - 1, Y, X + w - 1, Y + h - 1, c, 1);
}

void Canvas::DrawLine(int x0, int y0, int x1, int y1, Color c, int th) {
    raster::Line(impl_->t, impl_->ox + x0, impl_->oy + y0, impl_->ox + x1, impl_->oy + y1, c, th);
}
void Canvas::FillCircle(int cx, int cy, int r, Color c) { raster::CircleMid(impl_->t, impl_->ox + cx, impl_->oy + cy, r, c, true); }
void Canvas::DrawCircle(int cx, int cy, int r, Color c) { raster::CircleMid(impl_->t, impl_->ox + cx, impl_->oy + cy, r, c, false); }

void Canvas::RoundRect(int x, int y, int w, int h, int r, Color top, Color bottom, int alpha) {
    raster::RoundRect(impl_->t, impl_->ox + x, impl_->oy + y, w, h, r, top, bottom, alpha);
}
void Canvas::RoundRect(int x, int y, int w, int h, int r, Color c, int alpha) {
    raster::RoundRect(impl_->t, impl_->ox + x, impl_->oy + y, w, h, r, c, c, alpha);
}
void Canvas::RoundOutline(int x, int y, int w, int h, int r, Color c, int alpha) {
    raster::RoundOutline(impl_->t, impl_->ox + x, impl_->oy + y, w, h, r, c, alpha);
}
void Canvas::Circle(int cx, int cy, int r, Color c, int alpha) {
    raster::RoundRect(impl_->t, impl_->ox + cx - r, impl_->oy + cy - r, r * 2, r * 2, r, c, c, alpha);
}
void Canvas::Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha) {
    raster::Shadow(impl_->t, impl_->ox + x, impl_->oy + y, w, h, r, spread, dy, alpha);
}
void Canvas::SoftShadow(float x, float y, float w, float h, float r, float blur, float dy, int alpha) {
    raster::SoftShadow(impl_->t, (float)impl_->ox + x, (float)impl_->oy + y, w, h, r, blur, dy, alpha);
}

void Canvas::Blit(int x, int y, const uint32_t* a, int w, int h, int s) { raster::Blit(impl_->t, impl_->ox + x, impl_->oy + y, a, w, h, s); }
void Canvas::BlitAlpha(int x, int y, const uint32_t* a, int w, int h, int s, int al) { raster::BlitAlpha(impl_->t, impl_->ox + x, impl_->oy + y, a, w, h, s, al); }
void Canvas::BlitPremul(int x, int y, const uint32_t* a, int w, int h, int s, int al) { raster::BlitPremul(impl_->t, impl_->ox + x, impl_->oy + y, a, w, h, s, al); }
void Canvas::BlitScaled(const Rect& d, const uint32_t* a, int w, int h, int s) {
    raster::BlitScaled(impl_->t, impl_->ox + d.x, impl_->oy + d.y, d.w, d.h, a, w, h, s);
}

// ---- text (satoru)

int Canvas::Text(int x, int y, Face f, float px, const char* s, Color c) {
    if (!s) return 0;
    return bk::TextDraw(*impl_, impl_->ox + x, impl_->oy + y, f, px, s, c);
}

int Canvas::TextN(int x, int y, Face f, float px, const char* s, int n, Color c) {
    if (!s || n <= 0) return 0;
    char buf[512];
    int k = 0;
    while (s[k] && k < n && k < (int)sizeof(buf) - 1) { buf[k] = s[k]; k++; }
    buf[k] = 0;
    return Text(x, y, f, px, buf, c);
}

void Canvas::TextCenter(int cx, int y, Face f, float px, const char* s, Color c) {
    int w = MeasureWidth(f, px, s);
    Text(cx - w / 2, y, f, px, s, c);
}

int Canvas::TextFit(int x, int y, Face f, float px, const char* s, Color c, int maxw) {
    if (!s || maxw <= 0) return 0;
    if (MeasureWidth(f, px, s) <= maxw) return Text(x, y, f, px, s, c);
    char buf[128];
    int n = k_len(s); if (n > 120) n = 120;
    int ew = MeasureWidth(f, px, "...");
    while (n > 0) {
        for (int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = 0;
        if (MeasureWidth(f, px, buf) + ew <= maxw) break;
        n--;
    }
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = 0;
    return Text(x, y, f, px, buf, c);
}

// ---- integer twins for -mno-sse callers (satoru)

int MeasureWidthI(Face f, int px, const char* s) { return MeasureWidth(f, (float)px, s); }
int AscentI(Face f, int px) { return Ascent(f, (float)px); }
int EaseQ8(uint32_t t0, int delay_ms, int dur_ms) { return (int)(Ease(t0, delay_ms, dur_ms) * 256.0f + 0.5f); }
int  Canvas::TextI(int x, int y, Face f, int px, const char* s, Color c) { return Text(x, y, f, (float)px, s, c); }
void Canvas::TextCenterI(int cx, int y, Face f, int px, const char* s, Color c) { TextCenter(cx, y, f, (float)px, s, c); }
int  Canvas::TextFitI(int x, int y, Face f, int px, const char* s, Color c, int maxw) { return TextFit(x, y, f, (float)px, s, c, maxw); }
void Canvas::SoftShadowI(int x, int y, int w, int h, int r, int blur, int dy, int alpha) {
    SoftShadow((float)x, (float)y, (float)w, (float)h, (float)r, (float)blur, (float)dy, alpha);
}
void Canvas::BlitScaledFast(const Rect& d, const uint32_t* a, int w, int h, int s) {
    raster::BlitNearest(impl_->t, impl_->ox + d.x, impl_->oy + d.y, d.w, d.h, a, w, h, s);
}

// ---- kit pieces, after MDraw (satoru)

void Canvas::Menu(int x, int y, int w, int top, int item_h, const char* const* items, int n, int hover, float t) {
    const Palette& P = CurrentTheme().pal;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int h = top * 2 + n * item_h;
    for (int i = 0; i < n; i++) if (items[i][0] == '-' && items[i][1] == 0) h -= item_h - 9;
    float sc = 0.94f + 0.06f * t;
    int dw = (int)(w * sc + 0.5f), dh = (int)(h * sc + 0.5f);
    int a = (int)(255.0f * t);
    if (a <= 0) return;
    SoftShadow((float)x, (float)y, (float)dw, (float)dh, 10.0f, 18.0f, 8.0f, (P.dark ? 110 : 60) * a / 255);
    RoundRect(x, y, dw, dh, 10, P.panel, P.panel2, (P.dark ? 238 : 245) * a / 255);
    RoundOutline(x, y, dw, dh, 10, P.line, (P.dark ? 34 : 22) * a / 255);
    if (t < 0.55f) return;
    int ta = (int)(255.0f * (t - 0.55f) / 0.45f);
    int iy = y + top;
    for (int i = 0; i < n; i++) {
        const char* it = items[i];
        if (it[0] == '-' && it[1] == 0) {
            FillRectAlpha(x + 10, iy + 4, dw - 20, 1, (uint8_t)((P.dark ? 30 : 24) * ta / 255),
                          P.dark ? 0xFFFFFFFFu : 0xFF000000u);
            iy += 9;
            continue;
        }
        bool danger = it[0] == '!', dis = it[0] == '~';
        const char* label = (danger || dis) ? it + 1 : it;
        Color col = dis ? P.text2 : (danger ? P.danger : P.text);
        if (i == hover && !dis) {
            RoundRect(x + 5, iy + 1, dw - 10, item_h - 2, 6, P.hover, P.hover, 235 * ta / 255);
            col = 0xFFFFFFFFu;
        }
        col = kui::Mix(P.panel, col, ta);
        Text(x + 14, iy + (item_h - 17) / 2, Face::Sans, 14.5f, label, col);
        iy += item_h;
    }
}

static void badge(Canvas& cv, int cx, int cy, int r, int kind, Color c, int alpha) {
    cv.Circle(cx, cy, r, c, alpha);
    Color w = 0xFFFFFFFFu;
    if (kind == 1) {
        for (int k = 0; k < 4; k++) cv.RoundRect(cx - 5 + k, cy + k - 1, 2, 2, 1, w, alpha);
        for (int k = 0; k < 7; k++) cv.RoundRect(cx - 1 + k, cy + 2 - k, 2, 2, 1, w, alpha);
    } else if (kind == 2) {
        cv.RoundRect(cx - 1, cy - 6, 3, 8, 1, w, alpha);
        cv.RoundRect(cx - 1, cy + 4, 3, 3, 1, w, alpha);
    } else if (kind == 3) {
        for (int k = -4; k <= 4; k++) {
            cv.RoundRect(cx + k - 1, cy + k - 1, 3, 3, 1, w, alpha);
            cv.RoundRect(cx + k - 1, cy - k - 1, 3, 3, 1, w, alpha);
        }
    } else {
        cv.RoundRect(cx - 1, cy - 6, 3, 3, 1, w, alpha);
        cv.RoundRect(cx - 1, cy - 2, 3, 9, 1, w, alpha);
    }
}

void Canvas::Toast(int x, int y, int w, int h, int kind, const char* title, const char* body, int alpha) {
    if (alpha <= 0) return;
    const Palette& P = CurrentTheme().pal;
    static const Color kc[4] = { 0xFF0A84FF, 0xFF30C85A, 0xFFFFB020, 0xFFFF453A };
    Color c = kc[(kind < 0 || kind > 3) ? 0 : kind];
    SoftShadow((float)x, (float)y, (float)w, (float)h, 16.0f, 22.0f, 10.0f, (P.dark ? 120 : 70) * alpha / 255);
    RoundRect(x, y, w, h, 16, P.panel, P.panel2, (P.dark ? 240 : 248) * alpha / 255);
    RoundOutline(x, y, w, h, 16, P.line, (P.dark ? 32 : 20) * alpha / 255);
    badge(*this, x + 30, y + h / 2, 13, kind, c, alpha);
    Color tc = kui::Mix(P.panel, P.text, alpha), bc = kui::Mix(P.panel, P.text2, alpha);
    int tx = x + 54;
    if (body && body[0]) {
        Text(tx, y + h / 2 - 20, Face::SansBold, 15.0f, title, tc);
        Text(tx, y + h / 2 + 1, Face::Sans, 14.0f, body, bc);
    } else {
        Text(tx, y + (h - 18) / 2, Face::SansBold, 15.0f, title, tc);
    }
}

// icon art: the same blob and lru as MDraw's, kept per process (satoru)
static const int GENERIC_ICON = 10;   // AppIcons::GENERIC (satoru)
struct IconEnt { int id, size; uint32_t* px; uint32_t used; };
static IconEnt g_icons[16];
static uint32_t g_iclock = 0;

static const uint32_t* icon_scaled(int id, int size) {
    for (int i = 0; i < 16; i++)
        if (g_icons[i].px && g_icons[i].id == id && g_icons[i].size == size) { g_icons[i].used = ++g_iclock; return g_icons[i].px; }
    size_t bn = 0;
    const uint8_t* b = bk::IconBlob(&bn);
    if (!b || bn < 12 || b[0] != 'K' || b[1] != 'I' || b[2] != 'C' || b[3] != 'N') return nullptr;
    uint32_t count = *(const uint32_t*)(b + 4), sz = *(const uint32_t*)(b + 8);
    if (sz == 0 || bn < 12 + (size_t)count * sz * sz * 4) return nullptr;
    if (id < 0 || (uint32_t)id >= count) id = GENERIC_ICON;
    const uint32_t* src = (const uint32_t*)(b + 12) + (size_t)id * sz * sz;
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (!g_icons[i].px) { slot = i; break; }
        if (g_icons[i].used < g_icons[slot].used) slot = i;
    }
    if (g_icons[slot].px) kui::Free(g_icons[slot].px);
    uint32_t* px = (uint32_t*)kui::Alloc((size_t)size * size * 4);
    g_icons[slot].px = px;
    if (!px) return nullptr;
    raster::AreaScale(src, (int)sz, px, size);
    g_icons[slot].id = id; g_icons[slot].size = size; g_icons[slot].used = ++g_iclock;
    return px;
}

void Canvas::AppTile(int id, int x, int y, int size) {
    if (size < 8) return;
    int r = (size * 23) / 100;
    int set_ofs = CurrentTheme().minimal_icons ? GENERIC_ICON + 1 : 0;
    const uint32_t* art = size <= 256 ? icon_scaled(id + set_ofs, size) : nullptr;
    if (art) {
        Shadow(x + size / 16, y, size - size / 8, size, r, size >= 40 ? 4 : 2, size >= 40 ? 3 : 1, 80);
        BlitPremul(x, y, art, size, size, size, 255);
        return;
    }
    Shadow(x, y, size, size, r, size >= 40 ? 4 : 2, size >= 40 ? 3 : 1, 90);
    RoundRect(x, y, size, size, r, 0xFF9AA4BA, 0xFF566078, 255);
    RoundRect(x, y, size, size / 2, r, 0xFFFFFFFFu, 0xFFFFFFFFu, 22);
    RoundOutline(x, y, size, size, r, 0xFFFFFFFFu, 34);
}

}  // namespace kui

// end (satoru)
