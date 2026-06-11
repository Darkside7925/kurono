//  kurono os  -  system settings (new, modular) public api + module interface (satoru)
//
//  this is the FOUNDATION of a detailed, modular "System Settings" app. it
//  coexists with the legacy src/apps/settings.cpp and does not touch it. the
//  shell (system_settings.cpp) owns the window, a left sidebar, a scrolling
//  content pane, and a registry of SettingsModule entries. each category lives
//  in its own settings_mod_<x>.cpp file and is registered EXPLICITLY via an
//  extern struct  -  there are NO global constructors in this kernel
//  (-fno-use-cxa-atexit), so we never rely on ctor-time registration.
//
//  ── how to add a new module in ONE file ───────────────────────────────────
//  a module author writes exactly one new .cpp, e.g. settings_mod_audio.cpp:
//
//      //  kurono os  -  settings module: audio (satoru)
//      #include "system_settings.h"
//      #include "../drivers/graphics.h"      // + whatever the module needs
//
//      static void audio_on_show() { /* load config + detect hw (satoru) */ }
//      static void audio_render(int x,int y,int w,int h,int scroll) {
//          // draw into the content pane. (x,y) is the pane's top-left in
//          // screen coords; subtract `scroll` from your running y. use the
//          // SettingsUI helpers for a consistent look. (satoru)
//      }
//      static bool audio_input(int mx,int my,bool click,char key,int scroll) {
//          // mx,my are pane-local (0,0 = pane top-left); add `scroll` back to
//          // compare against your laid-out rows. return true if state changed. (satoru)
//          (void)mx;(void)my;(void)click;(void)key;(void)scroll; return false;
//      }
//      static int  audio_content_height() { return 400; /* total px (satoru) */ }
//
//      // NOTE: the `extern` is REQUIRED. a namespace-scope `const` object has
//      // internal linkage in c++; without `extern` the shell's matching
//      // `extern const SettingsModule g_audio_module;` would fail to link. (satoru)
//      extern const SettingsModule g_audio_module = {
//          "audio", "Sound", "\x07",
//          audio_on_show, audio_render, audio_input, audio_content_height
//      };
//      // end (satoru)
//
//  then the INTEGRATOR (not the module author) does two small edits:
//    (b) in system_settings.cpp: add `extern const SettingsModule g_audio_module;`
//        near the other externs, and add `&g_audio_module` to g_modules[].
//    (c) in src/Makefile: add $(SRC)/apps/settings_mod_audio.cpp to CXX_SRCS.
//
//  the render/input/on_show/content_height functions are `static` so multiple
//  module .cpp files can reuse the same short names without colliding. only the
//  `g_<x>_module` struct has external linkage. (satoru)

#pragma once
#include "../kernel/types.h"

class Window;  // fwd (satoru)

//  one settings category. all fields are plain data / function pointers so the
//  struct is a constant-initialised aggregate with static storage  -  no runtime
//  constructor runs. function pointers may be null where noted. (satoru)
struct SettingsModule {
    const char* id;        // "display","audio",... stable key (satoru)
    const char* title;     // sidebar label, e.g. "Display & Monitors" (satoru)
    const char* icon;      // short glyph/label string, may be "" (satoru)
    void (*on_show)();     // called when the module becomes visible; may be null (satoru)
    void (*render)(int x, int y, int w, int h, int scroll);          // draw content pane (clipped/offset by shell) (satoru)
    bool (*input)(int mx, int my, bool click, char key, int scroll); // true if state changed (shell redraws) (satoru)
    int  (*content_height)();   // total pixel height of content for the scrollbar; may be null -> no scroll (satoru)
};

namespace SystemSettings {
    void Open();                       // create/raise the window (satoru)
    bool IsOpen();
    void Render(Window* w, int x, int y, int w2, int h2);
    void Input(Window* w, int ev, int p1, int p2);
}

//  reusable control helpers  -  every module draws through these so the whole
//  app stays visually consistent and the per-category fan-out is trivial.
//  drawing helpers paint at the given screen coords; the matching *Hit helpers
//  take the same anchor plus the pane-local mouse and report a hit. callers own
//  layout (they choose x/y) and own the backing bool/int state. (satoru)
namespace SettingsUI {
    // shared palette so modules can match the shell chrome. (satoru)
    extern const unsigned int COL_BG;
    extern const unsigned int COL_PANEL;
    extern const unsigned int COL_TEXT;
    extern const unsigned int COL_DIM;
    extern const unsigned int COL_HEADING;
    extern const unsigned int COL_BORDER;
    extern const unsigned int COL_ON;
    extern const unsigned int COL_OFF;
    extern const unsigned int COL_WHITE;
    unsigned int Accent();   // live theme accent from UIConfig (satoru)

    // fixed control metrics so *Hit math matches the drawing exactly. (satoru)
    static const int TOGGLE_W   = 40;
    static const int TOGGLE_H   = 20;
    static const int SLIDER_H   = 8;
    static const int DROPDOWN_H = 22;
    static const int ROW_H      = 26;

    // toggle switch. draw at (x,y); hit-test the same rect. (satoru)
    void Toggle(int x, int y, bool on);
    bool ToggleHit(int x, int y, int mx, int my);

    // horizontal slider of pixel width w, filled to pct (0..100). SliderHit
    // returns the new percentage for a click at mx, or -1 if the click missed
    // the track. (satoru)
    void Slider(int x, int y, int w, int pct);
    int  SliderHit(int x, int y, int w, int mx, int my);

    // pill-style value selector ("< value >") of width w. DropdownHit returns
    // -1 (miss), 0 (left/decrement arrow), or 1 (right/increment arrow). (satoru)
    void Dropdown(int x, int y, int w, const char* value);
    int  DropdownHit(int x, int y, int w, int mx, int my);

    // section title + a label:value info row (read-only). (satoru)
    void SectionHeader(int x, int y, const char* text);
    void Row(int x, int y, const char* label, const char* value);

    // tiny libc-free string utilities modules can share. (satoru)
    void IntToStr(int v, char* b, int mx);
    void StrCpy(char* d, const char* s, int mx);
    void StrApp(char* d, const char* s, int mx);
    int  StrLen(const char* s);
}
// end (satoru)
