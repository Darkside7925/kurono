//  kurono os - settings module: accessibility (satoru)
//  ports the legacy settings.cpp accessibility tab onto the modular shell:
//  visual toggles (high contrast, reduced motion), keyboard assists (sticky /
//  slow / bounce keys), screen reader, a text-scale stepper, and a colour-blind
//  filter selector. every control persists through UIConfig and re-applies the
//  same runtime effect the old code did. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"        // SetColorFilter / SetHighContrast / GetColorFilter (satoru)
#include "../drivers/keyboard.h"        // SetStickyKeys / SetSlowKeys / SetBounceKeys / SetScreenReader (satoru)
#include "../ui/ui_elements.h"          // Widget::high_contrast_mode (satoru)
#include "../ui/window_manager.h"       // WindowManager::ReloadFromConfig (satoru)
#include "../ui/desktop.h"              // Desktop / Taskbar ReloadFromConfig (satoru)
#include "../system/ui_config.h"        // UIConfig persistence (satoru)
#include "settings.h"                   // SettingsApp::state.font_scale is the live consumer (satoru)

// ── module state (constant-initialised statics - ctor-free) ─────────────────
// six boolean assists, mirrored from ui.conf on every on_show. (satoru)
static bool s_high_contrast  = false;   // a11y.high_contrast (satoru)
static bool s_reduced_motion = false;   // compositor.reduced_motion (satoru)
static bool s_sticky_keys    = false;   // a11y.sticky_keys (satoru)
static bool s_slow_keys      = false;   // a11y.slow_keys (satoru)
static bool s_bounce_keys    = false;   // a11y.bounce_keys (satoru)
static bool s_screen_reader  = false;   // a11y.screen_reader (satoru)

static int  s_text_scale     = 1;       // display.font_scale, clamped 1..3 (satoru)
static int  s_color_filter   = 0;       // a11y.color_filter, 0..4 (satoru)

// timing the legacy tab used when arming the keyboard assists. (satoru)
static const uint32_t kSlowKeyMs   = 250;
static const uint32_t kBounceKeyMs = 200;

// colour-filter labels - index matches Graphics::SetColorFilter modes. (satoru)
static const char* kFilters[] = { "Off", "Protanopia", "Deuteranopia", "Tritanopia", "Grayscale" };
static const int   kFilterCount = (int)(sizeof(kFilters) / sizeof(kFilters[0]));

// the six toggle rows, in draw order. each carries its label, backing state
// pointer, and the ui.conf key it persists to. (satoru)
struct A11yToggle { const char* label; bool* state; const char* key; };
static A11yToggle s_toggles[] = {
    { "High Contrast",  &s_high_contrast,  "a11y.high_contrast" },
    { "Reduced Motion", &s_reduced_motion, "compositor.reduced_motion" },
    { "Sticky Keys",    &s_sticky_keys,    "a11y.sticky_keys" },
    { "Slow Keys",      &s_slow_keys,      "a11y.slow_keys" },
    { "Bounce Keys",    &s_bounce_keys,    "a11y.bounce_keys" },
    { "Screen Reader",  &s_screen_reader,  "a11y.screen_reader" },
};
static const int kToggleCount = (int)(sizeof(s_toggles) / sizeof(s_toggles[0]));

// ── runtime apply - mirrors persist_a11y() in the legacy tab ────────────────
// re-arms every accessibility subsystem from the cached state so a click takes
// effect immediately, then nudges the chrome to repaint from config. (satoru)
static void apply_a11y(){
    Graphics::SetColorFilter(s_color_filter);
    Graphics::SetHighContrast(s_high_contrast);
    Widget::high_contrast_mode = s_high_contrast;          // widget-layer mirror (satoru)
    Keyboard::SetStickyKeys(s_sticky_keys);
    Keyboard::SetSlowKeys(s_slow_keys ? kSlowKeyMs : 0);
    Keyboard::SetBounceKeys(s_bounce_keys ? kBounceKeyMs : 0);
    Keyboard::SetScreenReader(s_screen_reader);
    // font scale: the live text renderer (graphics.cpp) reads
    // SettingsApp::state.font_scale, not the ui.conf key directly, so push the
    // value through to take effect immediately. (satoru)
    if(s_text_scale >= 1 && s_text_scale <= 3) SettingsApp::state.font_scale = s_text_scale;
    // reduced motion lives in the compositor; reloading the chrome picks it up. (satoru)
    WindowManager::ReloadFromConfig();
    Desktop::ReloadFromConfig();
    Taskbar::ReloadFromConfig();
    Graphics::MarkUIDirty();
}

// write the full a11y block back to ui.conf in one Save(), then apply. (satoru)
static void persist_a11y(){
    UIConfig::SetInt("a11y.high_contrast",        s_high_contrast  ? 1 : 0, true);
    UIConfig::SetInt("compositor.reduced_motion", s_reduced_motion ? 1 : 0, true);
    UIConfig::SetInt("a11y.sticky_keys",          s_sticky_keys    ? 1 : 0, true);
    UIConfig::SetInt("a11y.slow_keys",            s_slow_keys      ? 1 : 0, true);
    UIConfig::SetInt("a11y.bounce_keys",          s_bounce_keys    ? 1 : 0, true);
    UIConfig::SetInt("a11y.screen_reader",        s_screen_reader  ? 1 : 0, true);
    UIConfig::SetInt("a11y.color_filter",         s_color_filter,           true);
    UIConfig::SetInt("display.font_scale",        s_text_scale,             true);
    UIConfig::Save();
    apply_a11y();
}

// ── on_show: (re)load persisted config + clamp ──────────────────────────────
static void a11y_on_show(){
    s_high_contrast  = UIConfig::Int("a11y.high_contrast",        s_high_contrast  ? 1 : 0) != 0;
    s_reduced_motion = UIConfig::Int("compositor.reduced_motion", s_reduced_motion ? 1 : 0) != 0;
    s_sticky_keys    = UIConfig::Int("a11y.sticky_keys",          s_sticky_keys    ? 1 : 0) != 0;
    s_slow_keys      = UIConfig::Int("a11y.slow_keys",            s_slow_keys      ? 1 : 0) != 0;
    s_bounce_keys    = UIConfig::Int("a11y.bounce_keys",          s_bounce_keys    ? 1 : 0) != 0;
    s_screen_reader  = UIConfig::Int("a11y.screen_reader",        s_screen_reader  ? 1 : 0) != 0;

    s_color_filter = UIConfig::Int("a11y.color_filter", s_color_filter);
    if(s_color_filter < 0) s_color_filter = 0;
    if(s_color_filter > kFilterCount - 1) s_color_filter = kFilterCount - 1;

    s_text_scale = UIConfig::Int("display.font_scale", s_text_scale);
    if(s_text_scale < 1) s_text_scale = 1;
    if(s_text_scale > 3) s_text_scale = 3;
    // re-assert the persisted scale onto the live renderer's source of truth on
    // open, so a value saved last session is actually in effect. (satoru)
    SettingsApp::state.font_scale = s_text_scale;
}

// ── layout constants shared by render + input so hit-testing matches the
//    drawing exactly. the input() signature gives no pane width, so we fix the
//    control geometry here rather than derive it. (satoru)
static const int PAD          = 8;     // top padding of the content pane (satoru)
static const int HDR_ADV      = 26;    // advance after a section header (satoru)
static const int ROW_ADV      = 28;    // advance per toggle row (satoru)
static const int SEL_ADV      = 30;    // advance per selector row (satoru)
static const int CTRL_X_OFF   = 200;   // controls column, relative to pane left (satoru)
static const int SEL_W        = 260;   // dropdown width in px (satoru)

// clamp the selector width to whatever the pane actually offers. (satoru)
static int sel_w_for(int pane_w){
    int avail = pane_w - CTRL_X_OFF - PAD;
    int sw = SEL_W;
    if(sw > avail) sw = avail;
    if(sw < 90) sw = 90;
    return sw;
}

// ── render ──────────────────────────────────────────────────────────────────
static void a11y_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int sel_w  = sel_w_for(w);
    int ly = y - scroll + PAD;
    char buf[16];

    // ── visual ───────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Visual");
    ly += HDR_ADV;
    // first two rows are the visual toggles (high contrast, reduced motion). (satoru)
    for(int i = 0; i < 2; i++){
        Graphics::DrawString(x, ly + 2, s_toggles[i].label, SettingsUI::COL_TEXT, 0xFF000000);
        SettingsUI::Toggle(ctrl_x, ly, *s_toggles[i].state);
        ly += ROW_ADV;
    }

    // ── keyboard assists ─────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Keyboard");
    ly += HDR_ADV;
    // remaining four toggles (sticky / slow / bounce keys, screen reader). (satoru)
    for(int i = 2; i < kToggleCount; i++){
        Graphics::DrawString(x, ly + 2, s_toggles[i].label, SettingsUI::COL_TEXT, 0xFF000000);
        SettingsUI::Toggle(ctrl_x, ly, *s_toggles[i].state);
        ly += ROW_ADV;
    }

    // ── text & colour ────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Text & Color");
    ly += HDR_ADV;

    // text-scale stepper rendered as a "< Nx >" pill. (satoru)
    Graphics::DrawString(x, ly + 4, "Text Scale:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::IntToStr(s_text_scale, buf, 16);
    SettingsUI::StrApp(buf, "x", 16);
    SettingsUI::Dropdown(ctrl_x, ly, sel_w, buf);
    ly += SEL_ADV;

    // colour-blindness filter selector. (satoru)
    Graphics::DrawString(x, ly + 4, "Color Filter:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(ctrl_x, ly, sel_w, kFilters[s_color_filter]);
    ly += SEL_ADV;

    // ── status (read-only) ───────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Status");
    ly += HDR_ADV;
    SettingsUI::Row(x, ly, "Slow Key Hold:",   s_slow_keys   ? "250 ms" : "Off");
    ly += 22;
    SettingsUI::Row(x, ly, "Bounce Window:",   s_bounce_keys ? "200 ms" : "Off");
    ly += 22;
    SettingsUI::Row(x, ly, "Screen Reader:",   s_screen_reader ? "Speaking" : "Silent");
    ly += SEL_ADV;

    // footer note, matching the legacy tab's guidance. (satoru)
    Graphics::DrawLine(x, ly, x + w - PAD, ly, SettingsUI::COL_BORDER);
    ly += 10;
    Graphics::DrawString(x, ly, "Reduced motion forces compositor animations off.",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 16;
    Graphics::DrawString(x, ly, "All changes persist to /etc/kurono/ui.conf immediately.",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 16;
}

// ── input: pane-local mx,my. walk the SAME running-y layout as render (already
//    offset by -scroll) so hit rects line up exactly. controls sit at pane-local
//    x = CTRL_X_OFF with the fixed widths declared above. (satoru)
static bool a11y_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ly = -scroll + PAD;

    // "Visual" header + first two toggles. (satoru)
    ly += HDR_ADV;
    for(int i = 0; i < 2; i++){
        if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
            *s_toggles[i].state = !*s_toggles[i].state;
            persist_a11y();
            return true;
        }
        ly += ROW_ADV;
    }

    // "Keyboard" header + remaining four toggles. (satoru)
    ly += HDR_ADV;
    for(int i = 2; i < kToggleCount; i++){
        if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
            *s_toggles[i].state = !*s_toggles[i].state;
            persist_a11y();
            return true;
        }
        ly += ROW_ADV;
    }

    // "Text & Color" header. (satoru)
    ly += HDR_ADV;

    // text-scale stepper: arrows step within 1..3. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, SEL_W, mx, my);
        if(hit == 0){ if(s_text_scale > 1){ s_text_scale--; persist_a11y(); } return true; }
        if(hit == 1){ if(s_text_scale < 3){ s_text_scale++; persist_a11y(); } return true; }
    }
    ly += SEL_ADV;

    // colour-filter selector: arrows cycle the filter list. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, SEL_W, mx, my);
        if(hit == 0){ if(s_color_filter > 0){ s_color_filter--; persist_a11y(); } return true; }
        if(hit == 1){ if(s_color_filter < kFilterCount - 1){ s_color_filter++; persist_a11y(); } return true; }
    }
    ly += SEL_ADV;

    return false;
}

// total content height for the scrollbar (sum of the row advances + tail). (satoru)
static int a11y_content_height(){
    int h = PAD;
    h += HDR_ADV + 2 * ROW_ADV;           // visual: header + 2 toggles (satoru)
    h += HDR_ADV + 4 * ROW_ADV;           // keyboard: header + 4 toggles (satoru)
    h += HDR_ADV + 2 * SEL_ADV;           // text & color: header + 2 selectors (satoru)
    h += HDR_ADV + 22 + 22 + SEL_ADV;     // status: header + 3 rows (satoru)
    h += 10 + 16 + 16;                     // divider + footer note (satoru)
    h += 16;                               // tail (satoru)
    return h;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_a11y_module;` resolves at link time. without
// it, a namespace-scope const has internal linkage and the link fails. (satoru)
extern const SettingsModule g_a11y_module = {
    "a11y", "Accessibility", "\x01",
    a11y_on_show, a11y_render, a11y_input, a11y_content_height
};
// end (satoru)
