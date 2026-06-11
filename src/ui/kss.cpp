//  kurono os  -  kurono style sheet (kss): theme tokens + widget toolkit. (satoru)
#include "kss.h"
#include "../drivers/graphics.h"
#include "font.h"
#include "ui_elements.h"   // Animation easing helpers for the tween engine (satoru)
#include "../system/ui_config.h"

namespace KSS {

// active theme: a modern black/grey chrome (windows/macos-dark flavor) with the
// user's chosen accent kept for interactive highlights. overridable per-token in
// /etc/kurono/ui.conf via theme.* keys. (satoru)
static Theme g_theme = {
    /*bg*/         0xFF1B1B1D,
    /*surface*/    0xFF2A2A2D,
    /*surface_hi*/ 0xFF35353A,
    /*sel*/        0xFF3A3A40,
    /*header*/     0xFF222225,
    /*text*/       0xFFF0F0F2,
    /*text_dim*/   0xFF9A9AA2,
    /*heading*/    0xFFF0F0F2,
    /*border*/     0xFF3A3A40,
    /*accent*/     0xFF3498DB,
    /*on*/         0xFF32D74B,
    /*off*/        0xFF48484E,
    /*track*/      0xFF3A3A40,
    /*white*/      0xFFFFFFFF,
    /*shadow*/     0xFF050507,
    /*radius*/     10,
    /*pad*/        16,
};

void Reload() {
    g_theme.bg         = UIConfig::Color("theme.bg",         0xFF1B1B1D);
    g_theme.surface    = UIConfig::Color("theme.surface",    0xFF2A2A2D);
    g_theme.surface_hi = UIConfig::Color("theme.surface_hi", 0xFF35353A);
    g_theme.sel        = UIConfig::Color("theme.sel",        0xFF3A3A40);
    g_theme.header     = UIConfig::Color("theme.header",     0xFF222225);
    g_theme.text       = UIConfig::Color("theme.text",       0xFFF0F0F2);
    g_theme.text_dim   = UIConfig::Color("theme.text_dim",   0xFF9A9AA2);
    g_theme.heading    = UIConfig::Color("theme.heading",    0xFFF0F0F2);
    g_theme.border     = UIConfig::Color("theme.border",     0xFF3A3A40);
    g_theme.accent     = UIConfig::Color("theme.accent",     0xFF3498DB);
    g_theme.on         = UIConfig::Color("theme.on",         0xFF32D74B);
    g_theme.off        = UIConfig::Color("theme.off",        0xFF48484E);
    g_theme.track      = UIConfig::Color("theme.track",      0xFF3A3A40);
    g_theme.white      = UIConfig::Color("theme.white",      0xFFFFFFFF);
    g_theme.shadow     = UIConfig::Color("theme.shadow",     0xFF050507);
    g_theme.radius     = UIConfig::Int  ("theme.radius",     10);
    g_theme.pad        = UIConfig::Int  ("theme.pad",        16);
}

void Init() { Reload(); }

const Theme& T()  { return g_theme; }
uint32_t Accent() { return g_theme.accent; }
float BodyPx()    { return 16.0f; }

// vertical text-box top so a pxh line sits centered in [ty, ty+h]. fontttf's y
// is the box top and the visible cap height is ~0.72*pxh. (satoru)
static inline int vcenter_y(int ty, int h, float pxh) {
    int vis = (int)(pxh * 0.72f + 0.5f);
    return ty + (h - vis) / 2;
}

namespace W {

void CardColor(int x, int y, int w, int h, uint32_t fill, int radius, bool shadow) {
    if (shadow) Graphics::ApplyShadow(x, y, w, h, 0, 4, 70);
    Graphics::FillRoundedRect(x, y, w, h, radius, fill);
    Graphics::DrawRect(x, y, w, h, g_theme.border);
}

void Card(int x, int y, int w, int h, bool shadow) {
    CardColor(x, y, w, h, g_theme.surface, g_theme.radius, shadow);
}

void Label(int x, int y, const char* s, uint32_t color) {
    if (s) Graphics::DrawString(x, y, s, color, 0xFF000000);
}

void LabelSz(int x, int y, const char* s, uint32_t color, float pxh) {
    if (s) FontTTF::DrawString(x, y, pxh, s, color);
}

void Center(int cx, int y, const char* s, uint32_t color, float pxh) {
    if (s) FontTTF::DrawStringCenter(cx, y, pxh, s, color);
}

void Heading(int x, int y, const char* s) {
    if (s) Graphics::DrawString(x, y, s, g_theme.heading, 0xFF000000);
}

int TextW(const char* s, float pxh) {
    return s ? FontTTF::Measure(pxh, s) : 0;
}

void Toggle(int x, int y, bool on) {
    Graphics::FillRoundedRect(x, y, TOGGLE_W, TOGGLE_H, TOGGLE_H / 2, on ? g_theme.on : g_theme.off);
    int knob_x = on ? (x + TOGGLE_W - 11) : (x + 11);
    Graphics::FillCircle(knob_x, y + TOGGLE_H / 2, 8, g_theme.white);
}

bool ToggleHit(int x, int y, int mx, int my) {
    return mx >= x && mx < x + TOGGLE_W && my >= y && my < y + TOGGLE_H;
}

void Slider(int x, int y, int w, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int ty = y + 4;
    Graphics::FillRoundedRect(x, ty, w, SLIDER_H, SLIDER_H / 2, g_theme.track);
    int fill_w = (w * pct) / 100;
    if (fill_w > 0) Graphics::FillRoundedRect(x, ty, fill_w, SLIDER_H, SLIDER_H / 2, g_theme.accent);
    Graphics::FillCircle(x + fill_w, ty + SLIDER_H / 2, 7, g_theme.white);
}

int SliderHit(int x, int y, int w, int mx, int my) {
    if (w <= 0) return -1;
    if (my < y - 4 || my > y + SLIDER_H + 8) return -1;
    if (mx < x || mx > x + w) return -1;
    int pct = ((mx - x) * 100) / w;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void Button(int x, int y, int w, int h, const char* label, bool hot) {
    uint32_t fill = hot ? g_theme.accent : g_theme.surface_hi;
    Graphics::FillRoundedRect(x, y, w, h, g_theme.radius, fill);
    Graphics::DrawRect(x, y, w, h, g_theme.border);
    if (label) FontTTF::DrawStringCenter(x + w / 2, vcenter_y(y, h, BodyPx()), BodyPx(), label, g_theme.white);
}

void Tile(int x, int y, int w, int h, const char* label, bool on) {
    uint32_t fill = on ? g_theme.accent : g_theme.surface_hi;
    Graphics::FillRoundedRect(x, y, w, h, g_theme.radius, fill);
    if (!on) Graphics::DrawRect(x, y, w, h, g_theme.border);
    if (label) {
        uint32_t lc = on ? g_theme.white : g_theme.text;
        // label sits along the lower third of the tile. (satoru)
        FontTTF::DrawStringCenter(x + w / 2, y + h - 22, BodyPx(), label, lc);
    }
}

bool RectHit(int x, int y, int w, int h, int mx, int my) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

} // namespace W

// ── animation engine ────────────────────────────────────────────────────────
namespace Anim {

static uint32_t g_now = 0;

struct ASlot {
    uint32_t id;
    bool     used;
    bool     init;
    float    f_from, f_to;
    uint32_t c_from, c_to;
    uint32_t start_ms, dur_ms;
    int      ease;
    uint32_t last_seen;
};
static const int ANIM_MAX = 128;
static ASlot g_anim[ANIM_MAX] = {};

static float ease_apply(int e, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (e) {
        case OutCubic:   return Animation::Ease(t, Animation::EaseOutCubic);
        case InOutQuint: return Animation::Ease(t, Animation::EaseInOutQuint);
        case Spring:     return Animation::SpringMs((uint32_t)(t * 1000.0f), 1000);
        default:         return t;   // Linear
    }
}

// find the slot for `id`, or claim a free/LRU one (resetting it). (satoru)
static ASlot* slot_for(uint32_t id) {
    ASlot* freeslot = nullptr;
    ASlot* lru = nullptr;
    for (int i = 0; i < ANIM_MAX; i++) {
        if (g_anim[i].used && g_anim[i].id == id) return &g_anim[i];
        if (!g_anim[i].used) { if (!freeslot) freeslot = &g_anim[i]; }
        else if (!lru || g_anim[i].last_seen < lru->last_seen) lru = &g_anim[i];
    }
    ASlot* s = freeslot ? freeslot : lru;   // evict least-recently-used when full
    s->id = id; s->used = true; s->init = false;
    return s;
}

static float cur_float(ASlot* s) {
    if (s->dur_ms == 0) return s->f_to;
    uint32_t dt = g_now - s->start_ms;
    if (dt >= s->dur_ms) return s->f_to;
    float t = (float)dt / (float)s->dur_ms;
    return s->f_from + (s->f_to - s->f_from) * ease_apply(s->ease, t);
}

static uint32_t cur_color(ASlot* s) {
    if (s->dur_ms == 0) return s->c_to;
    uint32_t dt = g_now - s->start_ms;
    if (dt >= s->dur_ms) return s->c_to;
    float t = ease_apply(s->ease, (float)dt / (float)s->dur_ms);
    return Animation::LerpColor(s->c_from, s->c_to, (uint8_t)(t * 255.0f + 0.5f));
}

void Tick(uint32_t now_ms) {
    g_now = now_ms;
    // drop ids not touched for a couple seconds so the table can't fill up. (satoru)
    for (int i = 0; i < ANIM_MAX; i++)
        if (g_anim[i].used && (now_ms - g_anim[i].last_seen) > 2000) g_anim[i].used = false;
}

bool Active() {
    for (int i = 0; i < ANIM_MAX; i++) {
        ASlot* s = &g_anim[i];
        if (s->used && s->init && s->dur_ms > 0 && (g_now - s->start_ms) < s->dur_ms) return true;
    }
    return false;
}

float Float(uint32_t id, float target, uint32_t dur_ms, Ease e) {
    ASlot* s = slot_for(id);
    s->last_seen = g_now;
    if (!s->init) {
        s->init = true;
        s->f_from = s->f_to = target;
        s->start_ms = g_now; s->dur_ms = dur_ms; s->ease = (int)e;
        return target;
    }
    if (s->f_to != target) {           // retarget from the live value
        s->f_from = cur_float(s);
        s->f_to = target;
        s->start_ms = g_now; s->dur_ms = dur_ms; s->ease = (int)e;
    }
    return cur_float(s);
}

uint32_t Color(uint32_t id, uint32_t target, uint32_t dur_ms, Ease e) {
    ASlot* s = slot_for(id);
    s->last_seen = g_now;
    if (!s->init) {
        s->init = true;
        s->c_from = s->c_to = target;
        s->start_ms = g_now; s->dur_ms = dur_ms; s->ease = (int)e;
        return target;
    }
    if (s->c_to != target) {           // retarget from the live blended color
        s->c_from = cur_color(s);
        s->c_to = target;
        s->start_ms = g_now; s->dur_ms = dur_ms; s->ease = (int)e;
    }
    return cur_color(s);
}

} // namespace Anim

} // namespace KSS
// end (satoru)
