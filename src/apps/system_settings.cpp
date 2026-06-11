//  kurono os  -  system settings shell: window, sidebar, content pane, scrolling,
//  module registry + dispatch, and the shared SettingsUI control helpers (satoru)
#include "system_settings.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../ui/kss.h"
#include "../system/ui_config.h"
#include "../system/logging.h"

// ── module registry ─────────────────────────────────────────────────────────
// each reference module defines `const SettingsModule g_<x>_module` in its own
// .cpp with external linkage; we pull them in by extern decl and list pointers
// in g_modules[]. integrators add one extern + one array entry per module. all
// initialisers are constant, so this is static (no global ctor runs). (satoru)
extern const SettingsModule g_display_module;
extern const SettingsModule g_about_module;
extern const SettingsModule g_audio_module;
extern const SettingsModule g_personalize_module;
extern const SettingsModule g_network_module;
extern const SettingsModule g_power_module;
extern const SettingsModule g_storage_module;
extern const SettingsModule g_devices_module;
extern const SettingsModule g_a11y_module;
extern const SettingsModule g_security_module;
extern const SettingsModule g_system_module;

static const SettingsModule* g_modules[] = {
    &g_display_module,
    &g_audio_module,
    &g_personalize_module,
    &g_network_module,
    &g_power_module,
    &g_storage_module,
    &g_devices_module,
    &g_a11y_module,
    &g_security_module,
    &g_system_module,
    &g_about_module,
};
static const int g_module_count = (int)(sizeof(g_modules) / sizeof(g_modules[0]));

// ── shell palette: modern black/grey chrome (matches the kss theme defaults so
// settings is consistent with the control center). headings are white instead of
// cyan; interactive highlights still use the user's theme.accent. (satoru)
static const unsigned int SH_BG       = 0xFF1B1B1D;
static const unsigned int SH_SIDEBAR  = 0xFF202023;
static const unsigned int SH_SEL_BG   = 0xFF323238;
static const unsigned int SH_HOVER_BG = 0xFF2A2A2E;
static const unsigned int SH_HEADER   = 0xFF202023;
static const unsigned int SH_TEXT     = 0xFFF0F0F2;
static const unsigned int SH_DIM      = 0xFF9A9AA2;
static const unsigned int SH_HEADING  = 0xFFF0F0F2;
static const unsigned int SH_BORDER   = 0xFF3A3A40;
static const unsigned int SH_ON       = 0xFF32D74B;
static const unsigned int SH_OFF      = 0xFF48484E;
static const unsigned int SH_WHITE    = 0xFFFFFFFF;
static const unsigned int SH_TRACK    = 0xFF3A3A40;

static unsigned int sh_accent() { return UIConfig::Color("theme.accent", 0xFF3498DB); }

// ── layout constants ────────────────────────────────────────────────────────
static const int SIDEBAR_W   = 170;
static const int HEADER_H    = 40;   // content-pane title strip (satoru)
static const int ROW_H       = 38;   // sidebar entry height (satoru)
static const int SCROLLBAR_W = 8;
static const int CONTENT_PAD = 16;   // inner padding inside the content pane (satoru)

// ── shell state (constant-initialised statics  -  ctor-free) ──────────────────
static int  s_win_id        = -1;
static int  s_selected      = 0;                 // index into g_modules[] (satoru)
static int  s_scroll[16]    = {0};               // per-module scroll offset, indexed like g_modules (satoru)
static int  s_hover_tab     = -1;                // sidebar hover highlight (satoru)
static bool s_shown_once[16] = {false};          // has on_show() run for this module yet (satoru)

// ── tiny libc-free string helpers ───────────────────────────────────────────
static int  s_len(const char* s){ int n=0; if(s) while(s[n]) n++; return n; }
static void s_cpy(char* d,const char* s,int mx){ int i=0; if(s) while(s[i]&&i<mx-1){ d[i]=s[i]; i++; } if(mx>0) d[i]=0; }
static void s_app(char* d,const char* s,int mx){ int n=s_len(d),i=0; if(s) while(s[i]&&n<mx-1){ d[n++]=s[i++]; } if(mx>0) d[n]=0; }
static void s_itoa(int v,char* b,int mx){
    if(mx<2){ if(mx>0) b[0]=0; return; }
    if(v<0){ b[0]='-'; s_itoa(-v,b+1,mx-1); return; }
    char t[16]; int n=0; do{ t[n++]='0'+(v%10); v/=10; }while(v&&n<15);
    int i=0; while(n>0&&i<mx-1) b[i++]=t[--n]; b[i]=0;
}

// clamp the active module's scroll offset to [0, max] given a viewport. (satoru)
static int content_view_h(int pane_h){ int v = pane_h - HEADER_H; return v < 0 ? 0 : v; }
static int clamp_scroll(const SettingsModule* m, int scroll, int pane_h){
    if(!m || !m->content_height) return 0;
    int total = m->content_height();
    int view  = content_view_h(pane_h);
    int maxsc = total - view;
    if(maxsc < 0) maxsc = 0;
    if(scroll < 0) scroll = 0;
    if(scroll > maxsc) scroll = maxsc;
    return scroll;
}

// ── sidebar ─────────────────────────────────────────────────────────────────
static void render_sidebar(int x, int y, int h){
    Graphics::FillRect(x, y, SIDEBAR_W, h, SH_SIDEBAR);
    Graphics::DrawLine(x + SIDEBAR_W - 1, y, x + SIDEBAR_W - 1, y + h, SH_BORDER);

    // app title at the top of the sidebar. (satoru)
    Graphics::DrawString(x + 16, y + 12, "Settings", SH_HEADING, 0xFF000000);
    int ty = y + 40;

    // selection highlight slides smoothly to the chosen row via the kss tween
    // engine (the row fill + accent left-bar ease into place). first open seeds
    // at the current row, so there's no slide-from-zero on launch. (satoru)
    int sel_target = ty + s_selected * ROW_H;
    int sel_y = (int)(KSS::Anim::Float(0xACCE5501u, (float)sel_target, 200, KSS::Anim::OutCubic) + 0.5f);
    Graphics::FillRect(x, sel_y, SIDEBAR_W - 1, ROW_H, SH_SEL_BG);
    Graphics::FillRect(x, sel_y, 3, ROW_H, sh_accent());

    for(int i = 0; i < g_module_count; i++){
        int row_y = ty + i * ROW_H;
        bool sel   = (i == s_selected);
        bool hover = (i == s_hover_tab) && !sel;
        if(hover){
            Graphics::FillRect(x, row_y, SIDEBAR_W - 1, ROW_H, SH_HOVER_BG);
        }
        int tx = x + 14;
        const char* icon = g_modules[i]->icon;
        if(icon && icon[0]){
            Graphics::DrawString(tx, row_y + 11, icon, sel ? SH_WHITE : SH_DIM, 0xFF000000);
            tx += 18;
        }
        Graphics::DrawString(tx, row_y + 11, g_modules[i]->title,
                             sel ? SH_WHITE : SH_TEXT, 0xFF000000);
    }
}

// ── content pane (header + clipped, scrolled module body + scrollbar) ────────
static void render_content(int x, int y, int w, int h){
    const SettingsModule* m = g_modules[s_selected];

    // header strip with the module title. (satoru)
    Graphics::FillRect(x, y, w, HEADER_H, SH_HEADER);
    Graphics::DrawLine(x, y + HEADER_H - 1, x + w - 1, y + HEADER_H - 1, SH_BORDER);
    Graphics::DrawString(x + CONTENT_PAD, y + 14, m->title, SH_HEADING, 0xFF000000);

    int body_x = x;
    int body_y = y + HEADER_H;
    int body_w = w;
    int body_h = content_view_h(h);

    int sc = clamp_scroll(m, s_scroll[s_selected], h);
    s_scroll[s_selected] = sc;

    // does this module need a scrollbar? leave room for it if so. (satoru)
    bool has_bar = false;
    if(m->content_height){
        int total = m->content_height();
        if(total > body_h) has_bar = true;
    }
    int inner_w = body_w - (has_bar ? SCROLLBAR_W : 0);

    // clip the module's drawing to the body rect so scrolled-out content never
    // bleeds over the header / other windows. the module renders at body_x with
    // its own y running from body_y, already offset by `sc`. (satoru)
    Graphics::PushClipRect(body_x, body_y, inner_w, body_h);
    if(m->render) m->render(body_x + CONTENT_PAD, body_y, inner_w - CONTENT_PAD, body_h, sc);
    Graphics::PopClipRect();

    // scrollbar track + thumb. (satoru)
    if(has_bar){
        int total = m->content_height();
        int bar_x = body_x + body_w - SCROLLBAR_W;
        Graphics::FillRect(bar_x, body_y, SCROLLBAR_W, body_h, SH_TRACK);
        int thumb_h = (body_h * body_h) / total;
        if(thumb_h < 24) thumb_h = 24;
        int maxsc = total - body_h;
        int travel = body_h - thumb_h;
        int thumb_y = body_y + (maxsc > 0 ? (sc * travel) / maxsc : 0);
        Graphics::FillRoundedRect(bar_x + 1, thumb_y, SCROLLBAR_W - 2, thumb_h, 3, sh_accent());
    }
}

// ── public api ──────────────────────────────────────────────────────────────
static void render_cb(Window* w, int cx, int cy, int cw, int ch){
    SystemSettings::Render(w, cx, cy, cw, ch);
}
static void input_cb(Window* w, int ev, int p1, int p2){
    SystemSettings::Input(w, ev, p1, p2);
}

void SystemSettings::Render(Window* w, int x, int y, int wd, int ht){
    (void)w;
    Graphics::FillRect(x, y, wd, ht, SH_BG);
    render_sidebar(x, y, ht);
    render_content(x + SIDEBAR_W, y, wd - SIDEBAR_W, ht);
}

// run on_show() the first time a module is selected (and whenever we switch to
// it) so it can (re)load config + re-detect hardware. (satoru)
static void show_module(int idx){
    if(idx < 0 || idx >= g_module_count) return;
    s_selected = idx;
    const SettingsModule* m = g_modules[idx];
    if(m->on_show) m->on_show();
    s_shown_once[idx] = true;
    Graphics::MarkUIDirty();
}

void SystemSettings::Input(Window* w, int ev, int p1, int p2){
    if(!w) return;

    // event codes (see window_manager.cpp): 1=click, 2=key, 3=scroll,
    // 4=right-click, 5=pointer-move, 6=pointer-button. coords are content-local
    // (0,0 = top-left of the window content area). (satoru)
    int content_h = w->content_h;
    if(content_h <= 0) content_h = w->h - WM_TITLEBAR_H;

    if(ev == 5){ // pointer move → sidebar hover (satoru)
        int mx = p1, my = p2;
        int new_hover = -1;
        if(mx >= 0 && mx < SIDEBAR_W){
            int t = (my - 40) / ROW_H;
            if(t >= 0 && t < g_module_count) new_hover = t;
        }
        if(new_hover != s_hover_tab){
            s_hover_tab = new_hover;
            Graphics::MarkUIDirty();
        }
        return;
    }

    if(ev == 3){ // scroll wheel → scroll the active module (satoru)
        const SettingsModule* m = g_modules[s_selected];
        if(m->content_height){
            int delta = p1;                       // +up / -down in wheel notches (satoru)
            s_scroll[s_selected] = clamp_scroll(m, s_scroll[s_selected] - delta * 32, content_h);
            Graphics::MarkUIDirty();
        }
        return;
    }

    if(ev == 1){ // left click (satoru)
        int mx = p1, my = p2;

        // sidebar selection. (satoru)
        if(mx >= 0 && mx < SIDEBAR_W){
            int t = (my - 40) / ROW_H;
            if(t >= 0 && t < g_module_count && t != s_selected){
                show_module(t);
            }
            return;
        }

        // content pane → forward to the module's input(), pane-local. (satoru)
        const SettingsModule* m = g_modules[s_selected];
        int pane_x  = SIDEBAR_W;
        int pane_top = HEADER_H;                  // body starts below the header (satoru)
        int rel_x = mx - pane_x - CONTENT_PAD;    // matches render's CONTENT_PAD offset (satoru)
        int rel_y = my - pane_top;
        if(rel_y < 0) return;                     // click landed on the header (satoru)
        int sc = clamp_scroll(m, s_scroll[s_selected], content_h);
        if(m->input && m->input(rel_x, rel_y, true, 0, sc)){
            Graphics::MarkUIDirty();
        }
        return;
    }

    if(ev == 2){ // keypress → active module (satoru)
        const SettingsModule* m = g_modules[s_selected];
        int sc = clamp_scroll(m, s_scroll[s_selected], content_h);
        if(m->input && m->input(0, 0, false, (char)p1, sc)){
            Graphics::MarkUIDirty();
        }
        return;
    }
}

bool SystemSettings::IsOpen(){
    return s_win_id >= 0 && WindowManager::GetWindow(s_win_id) != nullptr;
}

void SystemSettings::Open(){
    // raise the existing window instead of spawning a duplicate. (satoru)
    if(IsOpen()){
        WindowManager::Focus(s_win_id);
        WindowManager::BringToFront(s_win_id);
        return;
    }
    RuntimeLog::LogAppEvent("system_settings", "open");
    s_hover_tab = -1;
    s_win_id = WindowManager::CreateWindow("System Settings", -1, -1, 720, 520,
                                           render_cb, input_cb);
    // load config + detect hw for the initially-visible module. (satoru)
    show_module(s_selected);
}

// ════════════════════════════════════════════════════════════════════════════
//  SettingsUI  -  shared control helpers (drawing + hit-testing)
// ════════════════════════════════════════════════════════════════════════════
namespace SettingsUI {
    const unsigned int COL_BG      = SH_BG;
    const unsigned int COL_PANEL   = SH_SIDEBAR;
    const unsigned int COL_TEXT    = SH_TEXT;
    const unsigned int COL_DIM     = SH_DIM;
    const unsigned int COL_HEADING = SH_HEADING;
    const unsigned int COL_BORDER  = SH_BORDER;
    const unsigned int COL_ON      = SH_ON;
    const unsigned int COL_OFF     = SH_OFF;
    const unsigned int COL_WHITE   = SH_WHITE;

    unsigned int Accent(){ return sh_accent(); }

    void Toggle(int x, int y, bool on){
        unsigned int c = on ? SH_ON : SH_OFF;
        Graphics::FillRoundedRect(x, y, TOGGLE_W, TOGGLE_H, TOGGLE_H/2, c);
        int knob_x = on ? (x + TOGGLE_W - 11) : (x + 11);
        Graphics::FillCircle(knob_x, y + TOGGLE_H/2, 8, SH_WHITE);
    }
    bool ToggleHit(int x, int y, int mx, int my){
        return mx >= x && mx < x + TOGGLE_W && my >= y && my < y + TOGGLE_H;
    }

    void Slider(int x, int y, int w, int pct){
        if(pct < 0) pct = 0;
        if(pct > 100) pct = 100;
        int ty = y + 4;
        Graphics::FillRoundedRect(x, ty, w, SLIDER_H, SLIDER_H/2, SH_TRACK);
        int fill_w = (w * pct) / 100;
        if(fill_w > 0) Graphics::FillRoundedRect(x, ty, fill_w, SLIDER_H, SLIDER_H/2, sh_accent());
        Graphics::FillCircle(x + fill_w, ty + SLIDER_H/2, 7, SH_WHITE);
    }
    int SliderHit(int x, int y, int w, int mx, int my){
        // generous vertical band around the 8px track so it's easy to grab. (satoru)
        if(my < y - 4 || my > y + SLIDER_H + 8) return -1;
        if(mx < x || mx > x + w || w <= 0) return -1;
        int pct = ((mx - x) * 100) / w;
        if(pct < 0) pct = 0;
        if(pct > 100) pct = 100;
        return pct;
    }

    void Dropdown(int x, int y, int w, const char* value){
        // left arrow, value pill, right arrow. arrows are 20px square. (satoru)
        Graphics::FillRoundedRect(x, y, 20, DROPDOWN_H, 4, sh_accent());
        Graphics::DrawString(x + 6, y + 4, "<", SH_WHITE, 0xFF000000);
        Graphics::FillRoundedRect(x + 24, y, w - 48, DROPDOWN_H, 4, SH_TRACK);
        if(value) Graphics::DrawString(x + 32, y + 4, value, SH_WHITE, 0xFF000000);
        Graphics::FillRoundedRect(x + w - 20, y, 20, DROPDOWN_H, 4, sh_accent());
        Graphics::DrawString(x + w - 14, y + 4, ">", SH_WHITE, 0xFF000000);
    }
    int DropdownHit(int x, int y, int w, int mx, int my){
        if(my < y || my >= y + DROPDOWN_H) return -1;
        if(mx >= x && mx < x + 20) return 0;             // decrement (satoru)
        if(mx >= x + w - 20 && mx < x + w) return 1;     // increment (satoru)
        return -1;
    }

    void SectionHeader(int x, int y, const char* text){
        Graphics::DrawString(x, y, text, SH_HEADING, 0xFF000000);
    }
    void Row(int x, int y, const char* label, const char* value){
        Graphics::DrawString(x, y, label, SH_TEXT, 0xFF000000);
        if(value) Graphics::DrawString(x + 150, y, value, SH_DIM, 0xFF000000);
    }

    void IntToStr(int v, char* b, int mx){ s_itoa(v, b, mx); }
    void StrCpy(char* d, const char* s, int mx){ s_cpy(d, s, mx); }
    void StrApp(char* d, const char* s, int mx){ s_app(d, s, mx); }
    int  StrLen(const char* s){ return s_len(s); }
}
// end (satoru)
