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
// animations" half of the kss style/motion combo (scripting/behavior is handled
// by the existing python/kcl interpreters, not a bespoke js engine). (satoru)
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
}

} // namespace KSS
// end (satoru)
