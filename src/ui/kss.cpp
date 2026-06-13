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
    // ease the fill between resting and hover (accent) colors so hover/leave
    // animates instead of snapping. key the tween by the button's on-screen
    // position so each button keeps its own state across frames. (satoru)
    uint32_t target = hot ? g_theme.accent : g_theme.surface_hi;
    uint32_t id   = 0xB0000000u ^ ((uint32_t)x << 16) ^ ((uint32_t)y << 4) ^ (uint32_t)w;
    uint32_t fill = Anim::Color(id, target, 140, Anim::OutCubic);
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

uint32_t Now() { return g_now; }

} // namespace Anim

// ── stylesheet layer ─────────────────────────────────────────────────────────
namespace Sheet {

// small freestanding string helpers  -  no libc. (satoru)
static bool seq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static void scpy(char* d, const char* s, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}

static const int RULE_MAX  = 48;   // selectors
static const int KF_MAX    = 24;   // keyframe tracks
static const int KF_STOPS  = 8;    // stops per track

// per-property transition spec on a rule. (satoru)
struct Trans { uint32_t dur_ms; int ease; bool on; };

struct Keyframes {
    bool  used;
    char  name[24];
    int   prop;                 // which Prop this track drives (satoru)
    int   n;
    float off[KF_STOPS];        // ascending 0..1 (satoru)
    float val[KF_STOPS];
};

struct Rule {
    bool used;
    char sel[40];
    Style base;                 // the target ("source of truth") values (satoru)
    Trans trans[P_COUNT];       // optional per-prop transitions (satoru)
    // active keyframe binding (one track per rule, the common case). (satoru)
    bool     kf_on;
    int      kf_idx;            // index into g_kf
    uint32_t kf_start;
    uint32_t kf_dur;
    bool     kf_loop;
    int      kf_ease;
};

static Rule      g_rule[RULE_MAX];
static Keyframes g_kf[KF_MAX];
static bool      g_init = false;

static inline bool is_color_prop(int p) { return p >= P_BG && p <= P_SHADOW; }

// stable per-(rule,prop) transition id for the Anim engine. high bit set so it
// never collides with widget-address-based ids used elsewhere. (satoru)
static inline uint32_t trans_id(int rule, int p) {
    return 0x5000000u | ((uint32_t)rule << 8) | (uint32_t)p;
}

void Init() {
    if (g_init) return;
    for (int i = 0; i < RULE_MAX; i++) { g_rule[i].used = false; }
    for (int i = 0; i < KF_MAX; i++)   { g_kf[i].used = false; }
    g_init = true;

    const Theme& t = T();
    // seed a handful of builtin rules from the theme so scripts/host code have
    // sensible defaults to read and override. (satoru)
    struct Seed { const char* sel; uint32_t bg, fg, border, accent; int radius, pad; };
    Seed seeds[] = {
        { "root",          t.bg,         t.text,     t.border, t.accent, t.radius, t.pad },
        { "card",          t.surface,    t.text,     t.border, t.accent, t.radius, t.pad },
        { "button",        t.surface_hi, t.white,    t.border, t.accent, t.radius, 8 },
        { "button:hover",  t.accent,     t.white,    t.border, t.accent, t.radius, 8 },
        { "button:focus",  t.accent,     t.white,    t.accent, t.accent, t.radius, 8 },
        { "tile",          t.surface_hi, t.text,     t.border, t.accent, t.radius, 8 },
        { "tile:on",       t.accent,     t.white,    t.accent, t.accent, t.radius, 8 },
        { "window",        0xFF121218,   t.text,     t.border, t.accent, 10,       12 },
        { "notification",  0xFF1E1E24,   t.white,    t.border, t.accent, 12,       12 },
    };
    for (unsigned i = 0; i < sizeof(seeds)/sizeof(seeds[0]); i++) {
        int r = DefineRule(seeds[i].sel);
        if (r < 0) continue;
        SetColor (r, P_BG,       seeds[i].bg);
        SetColor (r, P_FG,       seeds[i].fg);
        SetColor (r, P_BORDER,   seeds[i].border);
        SetColor (r, P_ACCENT,   seeds[i].accent);
        SetColor (r, P_SHADOW,   t.shadow);
        SetScalar(r, P_RADIUS,   (float)seeds[i].radius);
        SetScalar(r, P_PAD,      (float)seeds[i].pad);
        SetScalar(r, P_BORDER_W, 1.0f);
        SetScalar(r, P_FONT_PX,  BodyPx());
        SetScalar(r, P_OPACITY,  255.0f);
        SetScalar(r, P_SCALE,    1.0f);
        SetScalar(r, P_DX,       0.0f);
        SetScalar(r, P_DY,       0.0f);
    }
    // give the interactive rules a default color transition so a script (or the
    // host) flipping button -> button:hover eases instead of snapping. (satoru)
    int rb = FindRule("button");
    if (rb >= 0) { SetTransition(rb, P_BG, 140, Anim::OutCubic);
                   SetTransition(rb, P_FG, 140, Anim::OutCubic); }
}

int DefineRule(const char* selector) {
    if (!selector) return -1;
    int existing = FindRule(selector);
    if (existing >= 0) return existing;
    for (int i = 0; i < RULE_MAX; i++) {
        if (!g_rule[i].used) {
            Rule& r = g_rule[i];
            r.used = true;
            scpy(r.sel, selector, sizeof(r.sel));
            for (int c = 0; c < 5; c++) r.base.color[c] = 0xFF000000u;
            for (int s = 0; s < P_COUNT - 5; s++) r.base.scalar[s] = 0.0f;
            for (int p = 0; p < P_COUNT; p++) { r.trans[p].on = false; }
            r.kf_on = false;
            return i;
        }
    }
    return -1;
}

int FindRule(const char* selector) {
    if (!selector) return -1;
    for (int i = 0; i < RULE_MAX; i++)
        if (g_rule[i].used && seq(g_rule[i].sel, selector)) return i;
    return -1;
}

static inline bool rule_ok(int rule) { return rule >= 0 && rule < RULE_MAX && g_rule[rule].used; }

void SetColor(int rule, Prop p, uint32_t argb) {
    if (!rule_ok(rule) || !is_color_prop(p)) return;
    g_rule[rule].base.color[p] = argb;
    // if this prop has a transition, kick the Anim engine toward the new value.
    if (g_rule[rule].trans[p].on)
        Anim::Color(trans_id(rule, p), argb, g_rule[rule].trans[p].dur_ms,
                    (Anim::Ease)g_rule[rule].trans[p].ease);
}

void SetScalar(int rule, Prop p, float v) {
    if (!rule_ok(rule) || is_color_prop(p) || p >= P_COUNT) return;
    g_rule[rule].base.scalar[p - 5] = v;
    if (g_rule[rule].trans[p].on)
        Anim::Float(trans_id(rule, p), v, g_rule[rule].trans[p].dur_ms,
                    (Anim::Ease)g_rule[rule].trans[p].ease);
}

uint32_t GetColor(int rule, Prop p) {
    if (!rule_ok(rule) || !is_color_prop(p)) return 0;
    return g_rule[rule].base.color[p];
}
float GetScalar(int rule, Prop p) {
    if (!rule_ok(rule) || is_color_prop(p) || p >= P_COUNT) return 0.0f;
    return g_rule[rule].base.scalar[p - 5];
}

void SetTransition(int rule, Prop p, uint32_t dur_ms, Anim::Ease e) {
    if (!rule_ok(rule) || p >= P_COUNT) return;
    g_rule[rule].trans[p].on = true;
    g_rule[rule].trans[p].dur_ms = dur_ms;
    g_rule[rule].trans[p].ease = (int)e;
}

// keyframe value at offset 0..1 (linear between stops; the Anim ease is applied
// to the playback clock by the caller). (satoru)
static float kf_sample(const Keyframes& k, float t) {
    if (k.n <= 0) return 0.0f;
    if (t <= k.off[0])       return k.val[0];
    if (t >= k.off[k.n - 1]) return k.val[k.n - 1];
    for (int i = 0; i < k.n - 1; i++) {
        if (t >= k.off[i] && t <= k.off[i + 1]) {
            float span = k.off[i + 1] - k.off[i];
            float local = span > 0.0f ? (t - k.off[i]) / span : 0.0f;
            return k.val[i] + (k.val[i + 1] - k.val[i]) * local;
        }
    }
    return k.val[k.n - 1];
}

void Resolve(int rule, Style& out) {
    if (!rule_ok(rule)) {
        for (int c = 0; c < 5; c++) out.color[c] = 0;
        for (int s = 0; s < P_COUNT - 5; s++) out.scalar[s] = 0.0f;
        return;
    }
    Rule& r = g_rule[rule];
    out = r.base;
    // overlay in-flight transitions (read the live eased value from Anim). (satoru)
    for (int p = 0; p < P_COUNT; p++) {
        if (!r.trans[p].on) continue;
        if (is_color_prop(p))
            out.color[p] = Anim::Color(trans_id(rule, p), r.base.color[p],
                                       r.trans[p].dur_ms, (Anim::Ease)r.trans[p].ease);
        else
            out.scalar[p - 5] = Anim::Float(trans_id(rule, p), r.base.scalar[p - 5],
                                            r.trans[p].dur_ms, (Anim::Ease)r.trans[p].ease);
    }
    // overlay an active keyframe track (drives one prop). (satoru)
    if (r.kf_on && r.kf_idx >= 0 && r.kf_idx < KF_MAX && g_kf[r.kf_idx].used) {
        const Keyframes& k = g_kf[r.kf_idx];
        uint32_t elapsed = Anim::Now() - r.kf_start;
        float t;
        if (r.kf_dur == 0) t = 1.0f;
        else {
            uint32_t m = r.kf_loop ? (elapsed % r.kf_dur)
                                   : (elapsed > r.kf_dur ? r.kf_dur : elapsed);
            t = (float)m / (float)r.kf_dur;
        }
        // run the playback clock through the chosen ease before sampling. (satoru)
        float te = Animation::Ease(t,
            r.kf_ease == Anim::OutCubic   ? Animation::EaseOutCubic :
            r.kf_ease == Anim::InOutQuint ? Animation::EaseInOutQuint :
                                            Animation::Linear);
        float v = kf_sample(k, te);
        if (is_color_prop(k.prop)) out.color[k.prop] = (uint32_t)v;
        else if (k.prop >= 5 && k.prop < P_COUNT) out.scalar[k.prop - 5] = v;
    }
}

int PropByName(const char* n) {
    if (!n) return -1;
    struct M { const char* n; int p; };
    static const M map[] = {
        {"background", P_BG}, {"bg", P_BG}, {"color", P_FG}, {"foreground", P_FG},
        {"border", P_BORDER}, {"accent", P_ACCENT}, {"shadow", P_SHADOW},
        {"radius", P_RADIUS}, {"padding", P_PAD}, {"pad", P_PAD},
        {"border-width", P_BORDER_W}, {"font", P_FONT_PX}, {"font-size", P_FONT_PX},
        {"opacity", P_OPACITY}, {"scale", P_SCALE}, {"dx", P_DX}, {"dy", P_DY},
        {"translate-x", P_DX}, {"translate-y", P_DY},
    };
    for (unsigned i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        if (seq(map[i].n, n)) return map[i].p;
    return -1;
}

int DefineKeyframes(const char* name, Prop p, const float* offsets,
                    const float* values, int n_stops) {
    if (!name || !offsets || !values || n_stops <= 0) return -1;
    if (n_stops > KF_STOPS) n_stops = KF_STOPS;
    // reuse a track with the same name if present. (satoru)
    int idx = -1;
    for (int i = 0; i < KF_MAX; i++) if (g_kf[i].used && seq(g_kf[i].name, name)) { idx = i; break; }
    if (idx < 0) for (int i = 0; i < KF_MAX; i++) if (!g_kf[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    Keyframes& k = g_kf[idx];
    k.used = true;
    scpy(k.name, name, sizeof(k.name));
    k.prop = (int)p;
    k.n = n_stops;
    for (int i = 0; i < n_stops; i++) { k.off[i] = offsets[i]; k.val[i] = values[i]; }
    return idx;
}

bool PlayKeyframes(int rule, const char* name, uint32_t dur_ms, bool loop, Anim::Ease e) {
    if (!rule_ok(rule) || !name) return false;
    int idx = -1;
    for (int i = 0; i < KF_MAX; i++) if (g_kf[i].used && seq(g_kf[i].name, name)) { idx = i; break; }
    if (idx < 0) return false;
    Rule& r = g_rule[rule];
    r.kf_on = true; r.kf_idx = idx; r.kf_dur = dur_ms; r.kf_loop = loop;
    r.kf_ease = (int)e; r.kf_start = Anim::Now();
    return true;
}

void StopKeyframes(int rule) { if (rule_ok(rule)) g_rule[rule].kf_on = false; }

void Tick() {
    // retire non-looping tracks whose single play finished, so Active() can
    // drop and the render gate releases. (satoru)
    uint32_t now = Anim::Now();
    for (int i = 0; i < RULE_MAX; i++) {
        Rule& r = g_rule[i];
        if (!r.used || !r.kf_on || r.kf_loop) continue;
        if (r.kf_dur != 0 && (now - r.kf_start) >= r.kf_dur) r.kf_on = false;
    }
}

bool Active() {
    uint32_t now = Anim::Now();
    for (int i = 0; i < RULE_MAX; i++) {
        const Rule& r = g_rule[i];
        if (!r.used || !r.kf_on) continue;
        if (r.kf_loop) return true;
        if (r.kf_dur == 0 || (now - r.kf_start) < r.kf_dur) return true;
    }
    return false;
}

} // namespace Sheet

} // namespace KSS
// end (satoru)
