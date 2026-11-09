//  kurono os - settings: public api, module interface and the shared control kit (satoru)
//
//  the shell (system_settings.cpp) owns the window, the sidebar with search, a
//  scrolling content pane with page transitions, and a registry of
//  SettingsModule entries. each category lives in its own settings_mod_<x>.cpp
//  and is registered explicitly via an extern struct (no global constructors in
//  this kernel, so no ctor-time registration).
//
//  ── writing a page (new style: one build() function) ─────────────────────
//  a module fills `build`. the shell calls it in several passes (layout, draw,
//  input, search) and every control call lays itself out the same way in each
//  one, so hit boxes always match what is on screen and scrolling, focus and
//  keyboard navigation come for free:
//
//      static int  s_vol = 50;
//      static bool s_mute = false;
//      static void audio_build(){
//          using namespace SettingsUI;
//          Group("Output");
//          if(Slider("Volume", &s_vol, 0, 100, 1, nullptr)) apply_volume(s_vol);
//          if(Toggle("Mute", &s_mute)) apply_mute(s_mute);
//          EndGroup();
//      }
//      extern const SettingsModule g_audio_module = {
//          "audio", "Sound", "", audio_on_show, nullptr, nullptr, nullptr, audio_build
//      };
//
//  controls return true only in the input pass, right after they changed the
//  value you passed in. drawing never has side effects.
//
//  ── legacy modules ───────────────────────────────────────────────────────
//  a module may instead fill render/input/content_height (and leave build
//  null). the shell then calls render(x,y,w,h,scroll) and input(mx,my,...)
//  exactly as before, and the old SettingsUI helpers below (Toggle at x,y,
//  Dropdown with arrows, ...) keep their geometry, restyled to the new look.
//
//  then register it: add `extern const SettingsModule g_<x>_module;` and an
//  entry in g_modules[] in system_settings.cpp, plus the .cpp in src/Makefile.
//  (satoru)

#pragma once
#include "../kernel/types.h"

class Window;  // fwd (satoru)

//  one settings category. plain data / function pointers, constant-initialised
//  with static storage. trailing members may be omitted in an initialiser
//  (they zero-initialise). (satoru)
struct SettingsModule {
    const char* id;        // "display","audio",... stable key (satoru)
    const char* title;     // sidebar label and page title (satoru)
    const char* icon;      // unused by the new shell (icons come from the id); kept for compatibility (satoru)
    void (*on_show)();     // called when the module becomes visible; may be null (satoru)
    void (*render)(int x, int y, int w, int h, int scroll);          // legacy draw; null for build() modules (satoru)
    bool (*input)(int mx, int my, bool click, char key, int scroll); // legacy input (satoru)
    int  (*content_height)();   // legacy total content height (satoru)
    void (*build)();            // new style page builder; when set, render/input/content_height are ignored (satoru)
};

namespace SystemSettings {
    void Open();                       // create/raise the window (satoru)
    void OpenKernel();                 // the in-kernel build, no ring-3 routing (satoru)
    void OpenPage(const char* id);     // open (or raise) on a page by module id (satoru)
    bool IsOpen();
    void Render(Window* w, int x, int y, int w2, int h2);
    void Input(Window* w, int ev, int p1, int p2);
    // raw key edge from the input manager (arrows, esc, tab, home/end, pgup/pgdn
    // never reach windows as chars). queued; handled on the gui thread. (satoru)
    void KeyEvent(int key, bool shift, bool ctrl, bool alt);
    // one already-translated kit key (K_UP, K_TAB, a printable, ...) from a backend (satoru)
    void KeyCode(int k, bool shift);
    // re-apply persisted settings that no other boot path restores (solid
    // wallpaper colour, master volume + mute, power mode). called once at boot
    // after the desktop is up. (satoru)
    void RestoreAtBoot();
}

namespace SettingsUI {
    // ════════════════════════════════════════════════════════════════════════
    //  theme. live colours (they follow the system appearance, light or dark,
    //  and crossfade when it changes). (satoru)
    // ════════════════════════════════════════════════════════════════════════
    struct Theme {
        uint32_t bg, sidebar, card, card_line, sep, hover, press;
        uint32_t text, text2, text3;
        uint32_t track, knob, field, field_line;
        uint32_t accent, on_accent, accent_soft;
        uint32_t danger, success, warn;
        bool     dark;
    };
    const Theme& T();
    unsigned int Accent();   // live accent from UIConfig theme.accent (satoru)

    // legacy palette names, now theme driven (read them, never write). (satoru)
    extern unsigned int COL_BG;
    extern unsigned int COL_PANEL;
    extern unsigned int COL_TEXT;
    extern unsigned int COL_DIM;
    extern unsigned int COL_HEADING;
    extern unsigned int COL_BORDER;
    extern unsigned int COL_ON;
    extern unsigned int COL_OFF;
    extern unsigned int COL_WHITE;

    // ════════════════════════════════════════════════════════════════════════
    //  control kit (use inside a module's build()). every row sits inside a
    //  Group: a rounded card with an optional caption above it. rows put the
    //  label (and an optional dim sub line) on the left and the control on the
    //  right. labels double as identities for animation and focus, so keep
    //  them unique within a group, or wrap repeated rows in PushId/PopId. (satoru)
    // ════════════════════════════════════════════════════════════════════════
    enum ButtonKind { BTN_SECONDARY = 0, BTN_PRIMARY = 1, BTN_DESTRUCTIVE = 2 };
    enum FieldFlags { FIELD_PASSWORD = 1 };

    void Group(const char* title, const char* footer = nullptr);   // begin a card (satoru)
    void EndGroup();
    void Note(const char* text);            // dim paragraph text outside a card (satoru)

    // read-only label / value row. dot != 0 draws a small status dot in that colour. (satoru)
    void Info(const char* label, const char* value, const char* sub = nullptr, uint32_t dot = 0);
    // on/off switch. the whole row is clickable. (satoru)
    bool Toggle(const char* label, bool* v, const char* sub = nullptr);
    // horizontal slider from lo..hi in `step`s. value_text (may be null) is
    // shown at the right; null prints the number. Committed() is true once the
    // drag is released (persist then, apply live on every change). (satoru)
    bool Slider(const char* label, int* v, int lo, int hi, int step, const char* value_text,
                const char* sub = nullptr);
    // segmented control (2..5 short items). (satoru)
    bool Segmented(const char* label, const char* const* items, int n, int* sel, const char* sub = nullptr);
    // popup menu. (satoru)
    bool Dropdown(const char* label, const char* const* items, int n, int* sel, const char* sub = nullptr);
    // row with a button on the right; returns true when pressed. (satoru)
    bool Button(const char* label, const char* button_text, int kind = BTN_SECONDARY,
                const char* sub = nullptr);
    // list row with a value and a chevron; returns true when clicked. (satoru)
    bool Nav(const char* label, const char* value = nullptr, const char* sub = nullptr);
    // single line text field. returns true when the text changed or Enter was
    // pressed; Submitted() tells the two apart. (satoru)
    bool TextField(const char* label, char* buf, int cap, const char* placeholder, int flags = 0);
    // colour swatches (accent picker style). sel may be -1 (none). (satoru)
    bool Swatches(const char* label, const uint32_t* colors, int n, int* sel);
    // usage meter: label + value on top, an animated bar underneath. (satoru)
    void Meter(const char* label, const char* value, int pct, const char* sub = nullptr);
    // a row of picture tiles (theme previews, wallpapers). paint draws tile i
    // into the given rect (already clipped to the tile's rounded shape). (satoru)
    typedef void (*TilePaint)(int idx, int x, int y, int w, int h);
    bool Tiles(const char* const* names, int n, int* sel, int tile_w, int tile_h, TilePaint paint);
    // a free-form row of height h (draw with the primitives below). returns its
    // rect; Rect.w == 0 in passes that do not draw. (satoru)
    struct Rect { int x, y, w, h; };
    Rect Custom(int h, bool separator = true);

    void PushId(int n);
    void PopId();
    bool Committed();       // the last change is final (click, key, drag release) (satoru)
    bool Submitted();       // the last TextField return was Enter (satoru)
    bool Drawing();         // true in the draw pass (satoru)
    void GoTo(const char* module_id);   // switch the shell to another page (satoru)

    // ── primitives (anti-aliased, clipped to the pane; for Custom rows) ─────
    void FillRound(int x, int y, int w, int h, int r, uint32_t c, int alpha = 255);
    void StrokeRound(int x, int y, int w, int h, int r, uint32_t c, int alpha = 255);
    void Disc(float cx, float cy, float r, uint32_t c, int alpha = 255);
    int  Text(int x, int y, float px, const char* s, uint32_t c, bool bold = false);
    int  TextWidth(float px, const char* s, bool bold = false);
    // icon ids for Icon(): see the IC_* list in settings_kit.cpp. (satoru)
    void Icon(int icon, int x, int y, int size, uint32_t c, int alpha = 255);
    // opaque argb picture (w x h) at x,y, clipped to the pane. (satoru)
    void Image(const uint32_t* px, int w, int h, int x, int y, int alpha = 255);
    // time based damped spring keyed by id (frame rate independent). kind:
    // 0 snappy, 1 bouncy, 2 smooth, 3 gentle. keeps the ui redrawing until it
    // settles. (satoru)
    float Spring(uint32_t id, float target, int kind = 0);
    uint32_t Hash(const char* s, uint32_t seed = 2166136261u);
    uint32_t Mix(uint32_t a, uint32_t b, int t255);

    // ════════════════════════════════════════════════════════════════════════
    //  legacy helpers (absolute coordinates, caller owns layout). kept with
    //  their old signatures and geometry for modules that still use
    //  render/input (the display page). restyled to the new kit. (satoru)
    // ════════════════════════════════════════════════════════════════════════
    static const int TOGGLE_W   = 40;
    static const int TOGGLE_H   = 20;
    static const int SLIDER_H   = 8;
    static const int DROPDOWN_H = 22;
    static const int ROW_H      = 26;

    void Toggle(int x, int y, bool on);
    bool ToggleHit(int x, int y, int mx, int my);
    void Slider(int x, int y, int w, int pct);
    int  SliderHit(int x, int y, int w, int mx, int my);
    void Dropdown(int x, int y, int w, const char* value);
    int  DropdownHit(int x, int y, int w, int mx, int my);
    void SectionHeader(int x, int y, const char* text);
    void Row(int x, int y, const char* label, const char* value);

    // tiny libc-free string utilities modules can share. (satoru)
    void IntToStr(int v, char* b, int mx);
    void StrCpy(char* d, const char* s, int mx);
    void StrApp(char* d, const char* s, int mx);
    int  StrLen(const char* s);
}
// end (satoru)
