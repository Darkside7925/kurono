#pragma once
//  kurono os  -  kurono style sheet (kss)
//
//  a single source of truth for ui colors + metrics so every app themes the
//  same way, plus a small immediate-mode widget toolkit whose text always
//  measures through fontttf (no fixed-width *8 assumptions). tokens default to
//  the proven settings palette and can be overridden from /etc/kurono/ui.conf
//  via theme.* keys. this is NOT a browser css engine  -  just a struct + helpers.
//  (satoru)
#include "../kernel/types.h"

namespace KSS {

// themeable token set. colors are 0xaarrggbb. (satoru)
struct Theme {
    uint32_t bg;          // app / window background
    uint32_t surface;     // raised panels, sidebars, cards
    uint32_t surface_hi;  // hovered surface
    uint32_t sel;         // selected row / active item
    uint32_t header;      // header strip
    uint32_t text;        // primary text
    uint32_t text_dim;    // secondary text
    uint32_t heading;     // section headings / highlight
    uint32_t border;      // hairline borders
    uint32_t accent;      // brand / active accent (mirrors theme.accent)
    uint32_t on;          // toggle-on / success
    uint32_t off;         // toggle-off track
    uint32_t track;       // slider / scroll track
    uint32_t white;       // knobs / pure white
    uint32_t shadow;      // soft-shadow base color
    int      radius;      // default corner radius
    int      pad;         // default content padding
};

void Init();             // load tokens from UIConfig (call after UIConfig::Init)
void Reload();           // re-read tokens after a ui.conf reload
const Theme& T();        // active theme tokens
uint32_t Accent();       // == T().accent

// body text height in px (matches Graphics::DrawString's 16px base). (satoru)
float BodyPx();

// widget metrics. (satoru)
static const int TOGGLE_W   = 44;
static const int TOGGLE_H   = 24;
static const int SLIDER_H   = 8;
static const int ROW_H      = 40;

// ── immediate-mode widgets: draw + hit-test pairs. ──────────────────────────
namespace W {
    // rounded surface card with hairline border; optional soft drop shadow. (satoru)
    void Card(int x, int y, int w, int h, bool shadow);
    // same, but explicit fill + radius (for non-surface panels). (satoru)
    void CardColor(int x, int y, int w, int h, uint32_t fill, int radius, bool shadow);

    // left-aligned label (body size), explicit-size label, centered label. (satoru)
    void Label(int x, int y, const char* s, uint32_t color);
    void LabelSz(int x, int y, const char* s, uint32_t color, float pxh);
    void Center(int cx, int y, const char* s, uint32_t color, float pxh);
    // section heading in the theme heading color at body size. (satoru)
    void Heading(int x, int y, const char* s);
    // pixel width of s at the given size (fontttf-measured). (satoru)
    int  TextW(const char* s, float pxh);

    // pill toggle. ToggleHit reports whether (mx,my) lands on it. (satoru)
    void Toggle(int x, int y, bool on);
    bool ToggleHit(int x, int y, int mx, int my);

    // horizontal slider, pct 0..100. SliderHit returns pct under (mx,my) or -1. (satoru)
    void Slider(int x, int y, int w, int pct);
    int  SliderHit(int x, int y, int w, int mx, int my);

    // rounded button with centered label; hot = hover/press brighten. (satoru)
    void Button(int x, int y, int w, int h, const char* label, bool hot);

    // control-center style square/rounded tile: icon area on top, label below;
    // `on` fills with accent, else surface. hit via RectHit. (satoru)
    void Tile(int x, int y, int w, int h, const char* label, bool on);

    // generic rectangular hit-test. (satoru)
    bool RectHit(int x, int y, int w, int h, int mx, int my);
}

// ── animation engine ────────────────────────────────────────────────────────
// declarative tweening for the immediate-mode ui: a widget asks for a value that
// eases toward a `target`, keyed by a stable `id`. the engine remembers per-id
// state across frames, so callers stay stateless. think of it as the "smooth
// animations" half of the kss style/motion combo. scripting/behavior is handled
// by the kj interpreter (src/apps/kj.cpp), which binds to the Sheet api below. (satoru)
namespace Anim {
    enum Ease { Linear = 0, OutCubic = 1, InOutQuint = 2, Spring = 3 };

    // advance the clock; call once per frame BEFORE any Float()/Color() reads, and
    // evict ids not touched recently so the table never fills. (satoru)
    void Tick(uint32_t now_ms);
    // true while any tween is still in flight  -  wire into the compositor's
    // keep-rendering gate so motion doesn't freeze mid-animation. (satoru)
    bool Active();

    // current value of a float that eases toward `target` over `dur_ms`. first
    // sight of an id seeds at `target` (no jump); a changed target animates from
    // the live value. `id` is any stable key (e.g. a widget address + sub-index). (satoru)
    float    Float(uint32_t id, float target, uint32_t dur_ms, Ease e);
    // same, for an argb color (channel-lerped). (satoru)
    uint32_t Color(uint32_t id, uint32_t target, uint32_t dur_ms, Ease e);

    // now_ms as last seen by Tick(); lets the Sheet/keyframe layer share one clock. (satoru)
    uint32_t Now();
}

// ── motion tokens ────────────────────────────────────────────────────────────
// ONE source of truth for the desktop's animation feel: a tiny standard set of
// durations + easings every surface shares so windows, taskbar, menus, dialogs
// and widgets all move with the same rhythm. all values are milliseconds and
// drive the ms-based Anim engine, so motion is frame-rate-independent (looks the
// same at 14fps and 60fps). use Motion::Micro for hover/press feedback,
// Motion::Window for windows/panels/menus, Motion::Slow for big surfaces. the
// default easing for ui reveals is OutCubic; spring is reserved for tap/bounce
// feedback. (satoru)
namespace Motion {
    static const uint32_t Micro  = 140;   // hover / press / focus micro-interactions
    static const uint32_t Window = 220;   // windows, panels, menus, dropdowns
    static const uint32_t Slow   = 280;   // dialogs/modals, large surface reveals
    static const Anim::Ease Std    = Anim::OutCubic;    // standard reveal/ease-out
    static const Anim::Ease Panel  = Anim::InOutQuint;  // panel slide accel+decel
    static const Anim::Ease Bounce = Anim::Spring;      // tap/launch bounce feedback

    // a stable Anim id from a base + small index. callers pass a per-surface base
    // (e.g. a window id, a widget address) so each animated thing keeps its own
    // tween state across frames without a heap table of its own. (satoru)
    static inline uint32_t Id(uint32_t base, uint32_t idx) {
        return (base * 0x9E3779B1u) ^ (idx + 0x85u);
    }
}

// ── stylesheet layer (the "kss" in kurono style sheet) ──────────────────────
// a real, scriptable styling layer on top of the theme tokens: named style
// rules (selectors) each carry a property bag (colors + metrics), per-property
// transitions (duration + easing applied automatically when a property's value
// changes), and named keyframe animations the compositor samples per frame.
// host code AND the kj interpreter both drive this through the same api, so a
// script can restyle / animate live widgets. fixed-size tables, no heap, no stl.
// (satoru)
namespace Sheet {

    // style properties addressable by name from scripts/config. (satoru)
    enum Prop {
        P_BG = 0,        // background fill (argb)
        P_FG,            // foreground / text (argb)
        P_BORDER,        // hairline border (argb)
        P_ACCENT,        // accent (argb)
        P_SHADOW,        // shadow base (argb)
        P_RADIUS,        // corner radius (px)
        P_PAD,           // content padding (px)
        P_BORDER_W,      // border width (px)
        P_FONT_PX,       // font height (px, scalar)
        P_OPACITY,       // 0..255
        P_SCALE,         // x1000 fixed display, stored as scalar 0..n (1.0 = full)
        P_DX,            // translate x (px)
        P_DY,            // translate y (px)
        P_COUNT
    };

    // resolved property bag for one element/selector. colors are argb; scalars
    // are float so transitions/keyframes interpolate smoothly. (satoru)
    struct Style {
        uint32_t color[5];   // P_BG..P_SHADOW
        float    scalar[P_COUNT - 5]; // P_RADIUS..P_DY
    };

    void Init();   // seed the builtin rules from the active Theme. call after KSS::Init(). (satoru)

    // ── live accent (scriptable, eased) ──────────────────────────────────────
    // the desktop chrome (window borders/titlebar accent, focus rings) reads its
    // accent through here instead of straight from the theme, so a kj script doing
    //   kss.transition("window","accent",ms); kss.set("window","accent",color)
    // eases the REAL on-screen accent live. resolves the "window" rule's P_ACCENT
    // through any in-flight transition; falls back to the theme accent before Init.
    // cheap: one rule resolve, no allocation. (satoru)
    uint32_t LiveAccent();
    // point the live accent at a new color over dur_ms (sets up the transition the
    // first time, then retargets). host-side convenience mirroring the kj path. (satoru)
    void     SetAccent(uint32_t argb, uint32_t dur_ms);

    // ── named style rules (selectors) ────────────────────────────────────────
    // register / fetch a rule by selector string (e.g. "button", "button:hover",
    // "card", "window.open"). DefineRule returns a stable rule id (>=0) or -1 if
    // the table is full. (satoru)
    int   DefineRule(const char* selector);
    int   FindRule(const char* selector);          // -1 if absent (satoru)
    // set one property on a rule. color props take an argb in `v` (cast); scalar
    // props take the float bits. the typed setters below are the friendly api. (satoru)
    void  SetColor (int rule, Prop p, uint32_t argb);
    void  SetScalar(int rule, Prop p, float v);
    uint32_t GetColor (int rule, Prop p);
    float    GetScalar(int rule, Prop p);
    // resolve a rule's full Style (after applying any in-flight transitions for
    // this rule's properties). (satoru)
    void  Resolve(int rule, Style& out);

    // name->Prop lookup for scripts/config ("background"->P_BG, etc). (satoru)
    int   PropByName(const char* name);   // -1 if unknown

    // ── transitions ──────────────────────────────────────────────────────────
    // give a rule's property a transition: future SetColor/SetScalar on it will
    // ease over dur_ms with easing e instead of snapping. (satoru)
    void  SetTransition(int rule, Prop p, uint32_t dur_ms, Anim::Ease e);

    // ── keyframe animations ──────────────────────────────────────────────────
    // define a named keyframe track for a single property: `stops` are (offset
    // 0..1, value) pairs in ascending offset. returns an anim id or -1. (satoru)
    int   DefineKeyframes(const char* name, Prop p, const float* offsets,
                          const float* values, int n_stops);
    // bind a keyframe track to a rule so the compositor samples it every frame;
    // dur_ms is one loop, loop=true repeats. returns false if name/rule unknown. (satoru)
    bool  PlayKeyframes(int rule, const char* name, uint32_t dur_ms, bool loop, Anim::Ease e);
    void  StopKeyframes(int rule);

    // advance keyframe playback against Anim::Now(). call once per frame from the
    // same place that ticks the Anim engine. (satoru)
    void  Tick();
    // true while any keyframe track is playing (feeds the keep-rendering gate). (satoru)
    bool  Active();

} // namespace Sheet

} // namespace KSS
// end (satoru)
