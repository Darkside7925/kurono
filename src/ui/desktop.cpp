//  kurono os - taskbar & desktop environment implementation
#include "../system/battery.h"   // menu bar battery (satoru)
#include "desktop.h"
#include "boot_anim.h"
#include "../../logo.h"
#include "control_center.h"
#include "notification.h"
#include "../system/screenshot.h"
#include "night_light.h"   // schedule tick (satoru)
#include "../system/power.h"
#include "wayland_server.h"
#include "perf_hud.h"
#include "gui.h"   // GUI::UpdateBackbuffer for the resize self-heal relayout (satoru)
#include "../drivers/graphics.h"
#include "../drivers/outputs.h"   // multi-monitor: menu bar/dock/icons live on the primary output (satoru)
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "gfx_bench.h"   // scissor debug lines only while the bench runs (satoru)
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../net/network.h"
#include "../system/ui_config.h"
#include "../fs/kvfs.h"
#include "../media/mediadecoder.h"   // MediaDecoder::Image type for the wallpaper (satoru)
#include "wallpaper.h"               // wallpaper_rgba_data  -> builtin index 0 (satoru)
#include "wallpaper2.h"              // wallpaper2_rgba_data -> builtin index 1 (satoru)
#include "app_icons.h"               // flat vector app icons (replaces letter glyphs) (satoru)
#include "kss.h"                      // motion tokens for shared animation feel (satoru)
#include "ui_elements.h"             // Animation easing/spring helpers (satoru)
#include "../apps/terminal.h"
#include "../apps/alpine_term.h"   // (satoru)
#include "../apps/settings_linux.h"   // linux tile + guest app tiles (satoru)
#include "../virt/vm_host.h"   // (satoru)
#include "../apps/file_manager.h"
#include "../apps/calculator.h"
#include "../kui/kui_kernel.h"   // kui apps (satoru)
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../apps/system_settings.h"   // new modular settings app (satoru)
#include "style.h"
#include "modern_draw.h"
#include "font.h"
#include "../shell/shell.h"
#include "../apps/task_manager.h"
#include "../apps/browser.h"
#include "../apps/media_player.h"
#include "../apps/firefox_launcher.h"   // install-state gate for the firefox icon (satoru)
#include "../system/installer_gui.h"   // live-medium install state for the desktop icon (satoru)
#include "../system/installer_service.h"   // payload availability for the installer icon (satoru)
#include "../fs/persist.h"              // persisted-install gate for the firefox icon (satoru)
#include "../proc/kernel_processes.h"   // detached shell launch (no terminal) (satoru)
#include "../proc/scheduler.h"
#include "../hal/hal.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)

static const unsigned int COL_TASKBAR      = 0xFF0C0C14;
static const unsigned int COL_TASKBAR_TOP  = 0xFF2A2A40;
static const unsigned int COL_START_BTN    = 0xFF5C8AFF;
static const unsigned int COL_START_HOVER  = 0xFF4470E0;
static const unsigned int COL_START_MENU   = 0xFF141422;
static const unsigned int COL_MENU_ITEM_HL = 0xFF2A3A5A;
static const unsigned int COL_TRAY_TEXT    = 0xFFBBBBCC;
static const unsigned int COL_DESK_BG      = 0xFF0C0818;
static const unsigned int COL_ICON_SEL     = 0xFF2A3860;
static const unsigned int COL_ICON_TEXT    = 0xFFE8E8F0;
static const unsigned int COL_CTX_BG       = 0xFF121228;
static const unsigned int COL_CTX_BORDER   = 0xFF5C8AFF;
static const unsigned int COL_WHITE        = 0xFFF0F0F0;

static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static bool seq(const char* a,const char* b){
    int pos=0;while(a&&b&&a[pos]&&b[pos]){if(a[pos]!=b[pos])return false;pos++;}
    return a&&b&&a[pos]==0&&b[pos]==0;
}
static bool starts_with(const char* text,const char* prefix){
    int pos=0;if(!text||!prefix)return false;
    while(prefix[pos]){if(text[pos]!=prefix[pos])return false;pos++;}
    return true;
}
static void int_to_str(int v,char* b,int mx){
    if(mx<2){b[0]=0;return;}
    if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;
    do{t[n++]=('0'+(v%10));v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}
static void int2(int v,char*b){b[0]='0'+(v/10)%10;b[1]='0'+v%10;b[2]=0;}

// ──── Frame timing & easing helpers ───────────────────────────────────
// All animations are driven from Timer::GetRealMs() so they run at a
// consistent perceived speed regardless of monitor Hz.
static uint32_t de_last_frame_ms        = 0;

static inline float clamp01(float v){ return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// Ease-out cubic - fast start, gentle landing.  Used for one-shot pops.
static inline float ease_out_cubic(float t){
    t = clamp01(t);
    float inv = 1.0f - t;
    return 1.0f - inv*inv*inv;
}
// Ease-in-out cubic - for symmetric open/close transitions.
static inline float ease_in_out_cubic(float t){
    t = clamp01(t);
    if (t < 0.5f) return 4.0f * t*t*t;
    float p = -2.0f*t + 2.0f;
    return 1.0f - (p*p*p) * 0.5f;
}
// Move x toward target by at most rate*dt_ms, clamped to [0,1].
static inline float approach(float x, float target, float rate_per_ms, uint32_t dt_ms){
    float step = rate_per_ms * (float)dt_ms;
    if (target > x) { x += step; if (x > target) x = target; }
    else            { x -= step; if (x < target) x = target; }
    return clamp01(x);
}

// modern style geometry: menu bar height, dock icon size/padding and the gap
// between the dock and the screen edge. (satoru)
static const int MBAR_H       = 28;
static const int MDOCK_ICON   = 46;
static const int MDOCK_PAD    = 9;
static const int MDOCK_GAP    = 8;
static const int MDOCK_BOTTOM = 8;
static const int MDOCK_H      = MDOCK_ICON + MDOCK_PAD * 2;

//  taskbar
int  Taskbar::screen_width    = 0;
int  Taskbar::screen_height   = 0;
int  Taskbar::y_pos           = 0;
bool Taskbar::start_menu_open = false;
int  Taskbar::hover_button    = -1;
int  Taskbar::clock_h         = 12;
int  Taskbar::clock_m         = 0;
int  Taskbar::clock_mon       = 1;
int  Taskbar::clock_dom       = 1;
int  Taskbar::clock_year      = 1970;
uint32_t Taskbar::clock_last_update_ms = 0;
int  Taskbar::battery_pct     = 100;
bool Taskbar::wifi_connected  = true;
int  Taskbar::wifi_strength   = 3;
int  Taskbar::volume_pct      = 75;
bool Taskbar::volume_popup_open = false;
int  Taskbar::volume_slider_val = 80;
bool Taskbar::volume_slider_dragging = false;
bool Taskbar::search_active   = false;
char Taskbar::search_buf[64]  = {0};
int  Taskbar::search_len      = 0;

int      Taskbar::cfg_height         = TASKBAR_HEIGHT;
uint32_t Taskbar::cfg_col_bg         = 0xFF0C0C14;
uint32_t Taskbar::cfg_col_top        = 0xFF2A2A40;
uint32_t Taskbar::cfg_col_text       = 0xFFBBBBCC;
uint32_t Taskbar::cfg_col_start_btn  = 0xFF5C8AFF;
uint32_t Taskbar::cfg_col_start_hover= 0xFF4470E0;
bool     Taskbar::cfg_show_clock     = true;
bool     Taskbar::cfg_show_battery   = true;
bool     Taskbar::cfg_show_wifi      = true;
bool     Taskbar::cfg_show_volume    = true;
bool     Taskbar::cfg_show_search    = true;
bool     Taskbar::cfg_position_top   = false;

uint32_t Taskbar::start_menu_anim_start_ms  = 0;
bool     Taskbar::start_menu_anim_opening   = false;
float    Taskbar::start_menu_phase          = 0.0f;
uint32_t Taskbar::volume_popup_anim_start_ms= 0;
bool     Taskbar::volume_popup_anim_opening = false;
float    Taskbar::volume_popup_phase        = 0.0f;
float    Taskbar::pinned_hover_phase[8]     = {0,0,0,0,0,0,0,0};
int      Taskbar::pinned_hover_target       = -1;
uint32_t Taskbar::pinned_anim_last_ms       = 0;
uint32_t Taskbar::pinned_launch_ms[8]       = {0,0,0,0,0,0,0,0};

// record a launch bounce on a pinned icon (out-of-line so the header stays free
// of the Timer include). (satoru)
void Taskbar::PinnedLaunched(int i){ if (i >= 0 && i < 8) pinned_launch_ms[i] = Timer::GetRealMs(); }
uint32_t Taskbar::search_cursor_t0_ms       = 0;
int      Taskbar::clock_dow                 = 0;
float    Taskbar::dock_scale[24]            = {0};
int      Taskbar::dock_mouse_x              = -1;
int      Taskbar::dock_mouse_y              = -1;

void Taskbar::ReloadFromConfig(){
    cfg_height          = UIConfig::Int  ("taskbar.height",          TASKBAR_HEIGHT);
    cfg_col_bg          = UIConfig::Color("taskbar.bg",              0xFF0C0C14);
    cfg_col_top         = UIConfig::Color("taskbar.top_edge",        0xFF2A2A40);
    cfg_col_text        = UIConfig::Color("taskbar.text",            0xFFBBBBCC);
    cfg_col_start_btn   = UIConfig::Color("taskbar.start_btn_bg",    0xFF5C8AFF);
    cfg_col_start_hover = UIConfig::Color("taskbar.start_btn_hover", 0xFF4470E0);
    cfg_show_clock      = UIConfig::Bool ("taskbar.show_clock",      true);
    cfg_show_battery    = UIConfig::Bool ("taskbar.show_battery",    true);
    cfg_show_wifi       = UIConfig::Bool ("taskbar.show_wifi",       true);
    cfg_show_volume     = UIConfig::Bool ("taskbar.show_volume",     true);
    cfg_show_search     = UIConfig::Bool ("taskbar.show_search",     true);
    const char* pos     = UIConfig::Str  ("taskbar.position",        "bottom");
    cfg_position_top    = (pos && (pos[0]=='t'||pos[0]=='T'));

    // recompute y position from current screen size + cfg height
    if(UIStyle::Modern()){
        y_pos = screen_height - MDOCK_BOTTOM - MDOCK_H;     // top of the dock
    } else if(cfg_position_top){
        y_pos = 10;
    } else {
        y_pos = screen_height - cfg_height - 10;
    }
}

// Internal helpers: open/close popups with animation kickoff.  Setting
// the bool directly without these would leave the eased phase stuck.
void Taskbar::StartMenuSet(bool open){
    if (start_menu_open == open) return;
    start_menu_open = open;
    start_menu_anim_opening = open;
    start_menu_anim_start_ms = Timer::GetRealMs();
}
void Taskbar::VolumePopupSet(bool open){
    if (volume_popup_open == open) return;
    volume_popup_open = open;
    volume_popup_anim_opening = open;
    volume_popup_anim_start_ms = Timer::GetRealMs();
    if (!open) volume_slider_dragging = false;
}

void Taskbar::Init(int sw,int sh){
    screen_width=sw; screen_height=sh;
    // Floating-bar layout: 14 px side margin, 10 px bottom gap.  The
    // wallpaper is visible behind the bar's rounded corners, giving it
    // a modern "dock" look while still keeping a stable y_pos so the
    // window manager can reserve space.
    y_pos = sh - TASKBAR_HEIGHT - 10;
    start_menu_open=false;
    hover_button=-1;
    ReloadFromConfig();
    clock_h=12; clock_m=0;
    // seed date cache; clock_last_update_ms=0 forces an immediate rtc read on
    // the first Update() so the bar shows real time without a 1s lag. (satoru)
    clock_mon=1; clock_dom=1; clock_year=1970;
    clock_last_update_ms=0;
    battery_pct=100; wifi_connected=false; wifi_strength=0; volume_pct=75;
    volume_popup_open=false; volume_slider_dragging=false;
    volume_slider_val = Audio::IsAvailable() ? Audio::GetMasterVolume() : 80;
    search_active=false; search_buf[0]=0; search_len=0;
    search_cursor_t0_ms = Timer::GetRealMs();

    start_menu_anim_start_ms = 0;
    start_menu_anim_opening = false;
    start_menu_phase = 0.0f;
    volume_popup_anim_start_ms = 0;
    volume_popup_anim_opening = false;
    volume_popup_phase = 0.0f;
    pinned_hover_target = -1;
    pinned_anim_last_ms = 0;
    for (int i = 0; i < 8; i++) { pinned_hover_phase[i] = 0.0f; pinned_launch_ms[i] = 0; }
}

int Taskbar::GetHeight(){ return UIStyle::Modern() ? MDOCK_H + MDOCK_BOTTOM : TASKBAR_HEIGHT + 10; }
int Taskbar::TopInset(){ return UIStyle::Modern() ? MBAR_H : 0; }
void Taskbar::ApplyDesktopArea(){
    int top = TopInset();
    WindowManager::SetDesktopArea(0, top, screen_width, screen_height - top - GetHeight());
}
int Taskbar::GetY(){ return y_pos; }

void Taskbar::SetClock(int h,int m){ clock_h=h; clock_m=m; }
void Taskbar::SetBattery(int p){ battery_pct=p; }
void Taskbar::SetWiFiConnected(bool c,int s){ wifi_connected=c; wifi_strength=s; }
void Taskbar::SetVolume(int p){ volume_pct=p; }

// ----- Taskbar geometry helpers (floating-bar layout) -----------------
//   The bar is rendered as a rounded "card" inset from the screen
//   edges so the wallpaper shows through.  All sub-renderers compute
//   their positions from these helpers, never from raw screen edges.
static inline int tb_bar_x()  { return 14; }
static inline int tb_bar_w()  { return Taskbar::screen_width - 28; }
static inline int tb_bar_y()  { return Taskbar::y_pos; }
static inline int tb_bar_h()  { return TASKBAR_HEIGHT; }
static inline int tb_bar_r()  { return TASKBAR_HEIGHT / 2 - 4; }       // pill-ish

// y where a taskbar popup of height h floats just off the bar: above it when the
// bar is docked at the bottom, below it when docked at the top. `lift` is an
// optional open-animation slide (px) that always eases toward the bar. used by
// both render and hit-test so the two stay in lockstep at either dock. (satoru)
static inline int tb_popup_y(int h, int gap, int lift){
    if(Taskbar::cfg_position_top)
        return Taskbar::y_pos + TASKBAR_HEIGHT + gap - lift;
    return Taskbar::y_pos - h - gap + lift;
}

// start menu / volume popup origins. classic floats them off the bar; modern
// drops them down from the menu bar (the logo, and the speaker icon). render,
// hit-test and drag all go through these so they stay in lockstep. (satoru)
static int mbar_vol_cx();
static void tb_start_menu_xy(int h, int lift, int& x, int& y){
    if(UIStyle::Modern()){ x = 8; y = MBAR_H + 6 - lift; return; }
    x = tb_bar_x() + 6;
    y = tb_popup_y(h, 8, lift);
}

// Quick-launch pinned apps.  Order = leftmost first.  Letter is the
// 1-char glyph drawn in the icon (mirrors the start menu's coding so
// the colour palette stays consistent).
struct PinnedApp {
    const char* name;
    char        glyph;
    uint32_t    colour;
    void      (*launch)();
};
static const PinnedApp tb_pinned[] = {
    { "Files",    'F', 0xFFF39C12, &DesktopEnvironment::LaunchFileBrowser },
    { "Terminal", 'T', 0xFF2ECC71, &DesktopEnvironment::LaunchTerminal    },
    { "Editor",   'E', 0xFFE74C3C, &DesktopEnvironment::LaunchTextEditor  },
    { "Browser",  'B', 0xFF3498DB, &DesktopEnvironment::LaunchBrowser     },
    { "Alpine",   'A', 0xFF0D597F, &DesktopEnvironment::LaunchAlpine      },   // linux vm (satoru)
};
static const int TB_PINNED_COUNT = (int)(sizeof(tb_pinned)/sizeof(tb_pinned[0]));
static const int TB_PIN_SZ       = 30;
static const int TB_PIN_GAP      = 6;
static inline int tb_pin_strip_x(){ return tb_bar_x() + 56; }       // right of K button
static inline int tb_pin_strip_w(){ return TB_PINNED_COUNT * TB_PIN_SZ + (TB_PINNED_COUNT-1)*TB_PIN_GAP; }
static inline int tb_search_x()  { return tb_pin_strip_x() + tb_pin_strip_w() + 12; }
static inline int tb_search_w()  { return 200; }

// Right cluster widths (packed from bar_right inward).
//   [ ... gap ... | bell | net | vol | bat | clock+date ]
static const int TB_CLUSTER_PAD       = 10;
static const int TB_BELL_W            = 26;
static const int TB_NET_W             = 26;
static const int TB_VOL_W             = 26;
static const int TB_BAT_W             = 30;
static const int TB_CLOCK_W           = 92;        // enough for "12:34 PM" + date
static inline int tb_cluster_right()  { return tb_bar_x() + tb_bar_w() - TB_CLUSTER_PAD; }
static inline int tb_clock_x()        { return tb_cluster_right() - TB_CLOCK_W; }
static inline int tb_bat_x()          { return tb_clock_x()    - TB_BAT_W - 6; }
static inline int tb_vol_x()          { return tb_bat_x()      - TB_VOL_W - 6; }
static inline int tb_net_x()          { return tb_vol_x()      - TB_NET_W - 6; }
static inline int tb_bell_x()         { return tb_net_x()      - TB_BELL_W - 6; }
static inline int tb_cluster_left()   { return tb_bell_x() - 4; }

// the modern dock's items: pinned apps, a divider, then one tile per open
// window. kind 0 = pinned, 1 = window, 2 = divider. (satoru)
struct DockItem { int kind; int pin; int win_id; int icon; int x; int size; };
static const int DOCK_MAX = 24;
// where each dock tile's magnification is heading, so IsAnimating can tell a
// glide in progress from a pointer resting over the dock. (satoru)
static float g_dock_target[DOCK_MAX] = {0};
static int dock_build(DockItem* it, int& bg_x, int& bg_w, bool scaled);

static void tb_volume_popup_xy(int h, int lift, int& x, int& y){
    if(UIStyle::Modern()){ x = mbar_vol_cx() - 26; y = MBAR_H + 6 - lift; return; }
    x = tb_vol_x() - 52/2 + TB_VOL_W/2;
    y = tb_popup_y(h, 8, lift);
}

void Taskbar::RenderStartButton(){
    // K logo button, anchored to the floating bar's left edge.
    int btn_w = 44, btn_h = TASKBAR_HEIGHT - 10;
    int bx = tb_bar_x() + 6, by = y_pos + 5;
    // transparent start button: the boot emblem sits directly on the bar (no box).
    // only a faint translucent highlight on hover so there's still a hover cue. (satoru)
    if (hover_button == 0) Graphics::FillRectAlpha(bx, by, btn_w, btn_h, 30, 0xFFFFFF);
    // kurono boot logo (the angular emblem from the splash screen), cropped to
    // its bounding box in logo_data and scaled to fill the button - replaces the
    // old hand-drawn "k". (satoru)
    const int LBX0 = 97, LBY0 = 80, LBW = 99, LBH = 141;   // emblem bbox (satoru)
    int lh = btn_h - 8;
    int lw = (lh * LBW) / LBH;
    if (lw > btn_w - 8){ lw = btn_w - 8; lh = (lw * LBH) / LBW; }
    int lx = bx + (btn_w - lw) / 2;
    int ly = by + (btn_h - lh) / 2;
    for (int dy = 0; dy < lh; dy++){
        int sy = LBY0 + (dy * LBH) / lh;
        for (int dx = 0; dx < lw; dx++){
            int sx = LBX0 + (dx * LBW) / lw;
            uint32_t p = logo_data[sy * LOGO_WIDTH + sx];
            uint8_t a = (p >> 24) & 0xFF;
            if (a < 10) continue;
            if (a >= 250) Graphics::DrawPixel(lx + dx, ly + dy, p | 0xFF000000);
            else Graphics::BlendPixel(lx + dx, ly + dy, (p>>16)&0xFF, (p>>8)&0xFF, p&0xFF, a);
        }
    }
}

// Pinned quick-launch icons immediately after the K button.
static void tb_render_quick_launch(){
    int x = tb_pin_strip_x();
    int y = tb_bar_y() + (TASKBAR_HEIGHT - TB_PIN_SZ) / 2;
    // Walk the windows array directly (id != index - GetWindow(i) was wrong).
    Window* wlist = WindowManager::GetWindows();
    for (int i = 0; i < TB_PINNED_COUNT; i++) {
        // hover lift: scale [0..1] -> shrink rect by up to 2 px so the icon
        // visually pops without disturbing neighbours.
        float t = Taskbar::PinnedHoverPhase(i);
        int pop = (int)(t * 2.0f + 0.5f);
        // launch bounce: a spring pop on top of the hover lift, peaking ~+4px
        // just after launch then settling. driven off elapsed ms so it reads the
        // same at any frame rate. (satoru)
        bool bouncing = false;
        uint32_t lms = Taskbar::PinnedLaunchMs(i);
        if (lms) {
            uint32_t e = Timer::GetRealMs() - lms;
            if (e < KSS::Motion::Window) {
                float s = Animation::SpringMs(e, KSS::Motion::Window);   // 0..~1 overshoot
                // bell-shaped pop: rises then returns to 0 by the end. (satoru)
                float bell = s * (1.0f - (float)e / (float)KSS::Motion::Window);
                pop += (int)(bell * 5.0f + 0.5f);
                bouncing = true;
            }
        }
        // while this icon is mid hover-lift or launch-bounce, damage only its
        // tile (plus the max pop margin) so the present stays partial - a single
        // icon animating must not re-copy the whole framebuffer. the +8 margin
        // covers the largest pop (2px hover + 5px bounce, rounded). (satoru)
        if (bouncing || t > 0.01f)
            Graphics::MarkDirty(x - 8, y - 8, TB_PIN_SZ + 16, TB_PIN_SZ + 18);
        int ix = x - pop;
        int iy = y - pop;
        int iw = TB_PIN_SZ + pop*2;
        int ih = TB_PIN_SZ + pop*2;
        Graphics::FillRoundedRect(ix, iy, iw, ih, 7, tb_pinned[i].colour);
        Graphics::FillRect(ix+1, iy+1, iw-2, 1, 0x55FFFFFF);
        // flat vector glyph in place of the old first-letter draw, inset a few
        // px so the tile color frames it. (satoru)
        int gpad = iw / 6;
        AppIcons::Draw(AppIcons::IdForName(tb_pinned[i].name),
                       ix + gpad, iy + gpad, iw - gpad*2);

        // running indicator: any window whose title starts with the glyph.
        bool running = false;
        for (int wi = 0; wi < WM_MAX_WINDOWS && !running; wi++) {
            Window* w = &wlist[wi];
            if (w->state != WIN_CLOSED && w->title[0] == tb_pinned[i].glyph)
                running = true;
        }
        if (running) {
            Graphics::FillRoundedRect(x + TB_PIN_SZ/2 - 4, y + TB_PIN_SZ + 1,
                                      8, 2, 1, 0xFFFFFFFF);
        }
        x += TB_PIN_SZ + TB_PIN_GAP;
    }
}

void Taskbar::RenderTaskButtons(){
    // Quick-launch pinned strip first (no-op if disabled).
    tb_render_quick_launch();

    // Search pill
    int sb_x = tb_search_x(), sb_w = tb_search_w();
    int sb_h = TASKBAR_HEIGHT - 14;
    int sb_y = y_pos + 7;
    uint32_t sb_bg = search_active ? 0xFF22223A : 0xFF1A1A2C;
    Graphics::FillRoundedRect(sb_x, sb_y, sb_w, sb_h, 14, sb_bg);
    if (search_active)
        Graphics::FillRoundedRect(sb_x, sb_y, sb_w, 1, 0, 0x80AABBFF);
    // magnifier icon
    int mag_x = sb_x + 14, mag_y = sb_y + sb_h/2;
    Graphics::DrawCircle(mag_x, mag_y, 5, 0xFF7878A0);
    Graphics::DrawLine(mag_x+4, mag_y+4, mag_x+7, mag_y+7, 0xFF7878A0);
    // search text or placeholder
    if (search_buf[0]) {
        Graphics::DrawString(sb_x + 28, sb_y + 6, search_buf, 0xFFE0E0F0, 0xFF000000);
    } else {
        Graphics::DrawString(sb_x + 28, sb_y + 6, "Search apps...", 0xFF606080, 0xFF000000);
    }
    // blinking cursor when active - driven by real time (~500ms period).
    if (search_active) {
        int cx = sb_x + 28 + search_len * 8;
        uint32_t dt = Timer::GetRealMs() - search_cursor_t0_ms;
        if (((dt / 500u) & 1u) == 0u)
            Graphics::FillRect(cx, sb_y + 5, 2, sb_h - 10, 0xFF5C8AFF);
    }

    // count visible windows for centering - walk array directly.
    int win_ids[WM_MAX_WINDOWS];
    int wcount = 0;
    Window* wlist = WindowManager::GetWindows();
    for(int i=0;i<WM_MAX_WINDOWS && wcount<WM_MAX_WINDOWS;i++){
        Window* w = &wlist[i];
        if(w->state!=WIN_CLOSED){
            win_ids[wcount++] = w->id;
        }
    }
    int bw = 40, gap = 4;
    int total_w = wcount * bw + (wcount > 1 ? (wcount-1)*gap : 0);
    int center = tb_bar_x() + tb_bar_w() / 2;
    int x = center - total_w / 2;
    int min_x = sb_x + sb_w + 12;
    int max_x = tb_cluster_left() - 8;
    if (x < min_x) x = min_x;

    int focused_id = WindowManager::GetFocusedIndex();
    for(int i=0;i<wcount;i++){
        Window* w = WindowManager::GetWindow(win_ids[i]);
        if(!w || w->state==WIN_CLOSED) continue;
        if (x + bw > max_x) break;                 // ran out of room
        bool focused = (focused_id == w->id);

        // eased hover: a soft rounded highlight fades in and the icon lifts
        // 2px, through the shared kss tween engine so the motion matches the
        // rest of the desktop (and keeps the render gate open mid-ease). (satoru)
        bool hov = (Mouse::mx >= x && Mouse::mx < x + bw &&
                    Mouse::my >= y_pos && Mouse::my < y_pos + TASKBAR_HEIGHT);
        float ht = KSS::Anim::Float(KSS::Motion::Id((uint32_t)w->id, 0x40u),
                                    hov ? 1.0f : 0.0f,
                                    KSS::Motion::Micro, KSS::Motion::Std);
        int lift = (int)(ht * 2.0f + 0.5f);
        if(focused){
            Graphics::FillRoundedRect(x, y_pos+5, bw, TASKBAR_HEIGHT-10, 9, 0xFF23233F);
        } else if(ht > 0.01f){
            uint32_t hbg = Animation::LerpColor(COL_TASKBAR, 0xFF32324E,
                                                (int)(ht * 255.0f));
            Graphics::FillRoundedRect(x, y_pos+5, bw, TASKBAR_HEIGHT-10, 9, hbg);
        }

        // app icon - colored rounded square with a flat vector glyph. a pinned
        // app_icon (wayland set_app_id, survives page-title changes) wins over
        // the title-derived id. (satoru)
        int gid = (w->app_icon >= 0) ? w->app_icon : AppIcons::IdForName(w->title);
        unsigned int ic = WindowTileColor(w->title);
        // firefox: dark night-sky tile so the orange fox mark pops. (satoru)
        if(gid == AppIcons::FIREFOX) ic = 0xFF14103C;

        // the window manager's minimize landing / restore press squashes and
        // springs the tile about its centre and flashes it. (satoru)
        int tw = bw-12, th = TASKBAR_HEIGHT-20;
        int tx = x+6, ty = y_pos+10 - lift;
        int sx_q8 = 256, sy_q8 = 256, flash = 0;
        if (WindowManager::TaskbarCatch(w->id, tx, ty, tw, th, sx_q8, sy_q8, flash)) {
            int nw = (tw * sx_q8) >> 8, nh = (th * sy_q8) >> 8;
            tx += (tw - nw) / 2; ty += (th - nh) / 2;
            tw = nw; th = nh;
        }
        Graphics::FillRoundedRect(tx, ty, tw, th, 5, ic);
        int tile = th < tw ? th : tw;
        int gpad = tile / 6;
        AppIcons::Draw(gid, tx + (tw - tile) / 2 + gpad, ty + (th - tile) / 2 + gpad, tile - gpad*2);
        if (flash > 0) Graphics::FillRectAlpha(tx, ty, tw, th, (uint8_t)flash, 0xFFFFFFFFu);

        // dock-style state indicator: focused = a wide accent pill, other open
        // windows = a small dot (dimmed when minimized). (satoru)
        if(focused){
            Graphics::FillRoundedRect(x + bw/2 - 8, y_pos+TASKBAR_HEIGHT-5, 16, 3, 2, COL_START_BTN);
        } else {
            unsigned int dot = (w->state==WIN_MINIMIZED) ? 0xFF4A4A66 : 0xFF8A8AB0;
            Graphics::FillRoundedRect(x + bw/2 - 2, y_pos+TASKBAR_HEIGHT-5, 4, 3, 2, dot);
        }

        x += bw + gap;
    }
}

uint32_t Taskbar::WindowTileColor(const char* title){
    if (UIStyle::Modern()) return MDraw::AppTileColor(AppIcons::IdForName(title));
    unsigned int ic = 0xFF3498DB;
    char c0 = title[0];
    if(c0=='T') ic = 0xFF2ECC71;
    else if(c0=='F') ic = 0xFFF39C12;
    else if(c0=='C' && title[1]=='o') ic = 0xFF5C8AFF;
    else if(c0=='C') ic = 0xFF9B59B6;
    else if(c0=='S') ic = 0xFF607D8B;
    else if(c0=='E') ic = 0xFFE74C3C;
    else if(c0=='B') ic = 0xFF3498DB;
    else if(c0=='M') ic = 0xFFE91E63;
    return ic;
}

bool Taskbar::WindowTileRect(int win_id, int& tx, int& ty, int& tw, int& th){
    if (UIStyle::Modern()) {
        DockItem it[DOCK_MAX];
        int bg_x, bg_w;
        int n = dock_build(it, bg_x, bg_w, true);
        for (int i = 0; i < n; i++) {
            if (it[i].kind != 1 || it[i].win_id != win_id) continue;
            tw = th = it[i].size;
            tx = it[i].x;
            ty = y_pos + MDOCK_PAD + MDOCK_ICON - it[i].size;
            return true;
        }
        return false;
    }
    // mirrors the window-button layout in Render() and HandleClick().
    int wcount = 0, slot = -1;
    Window* wlist = WindowManager::GetWindows();
    for(int i=0;i<WM_MAX_WINDOWS;i++){
        if(wlist[i].state==WIN_CLOSED) continue;
        if(wlist[i].id==win_id) slot = wcount;
        wcount++;
    }
    if(slot < 0) return false;
    int bw = 40, gap = 4;
    int total_w = wcount * bw + (wcount > 1 ? (wcount-1)*gap : 0);
    int center = tb_bar_x() + tb_bar_w() / 2;
    int x = center - total_w / 2;
    int min_x = tb_search_x() + tb_search_w() + 12;
    int max_x = tb_cluster_left() - 8;
    if (x < min_x) x = min_x;
    x += slot * (bw + gap);
    if (x + bw > max_x) return false;
    tx = x + 6; ty = y_pos + 10;
    tw = bw - 12; th = TASKBAR_HEIGHT - 20;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  modern style: menu bar + floating dock (satoru)
// ═══════════════════════════════════════════════════════════════════════

static const char* const k_dow[7]  = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const k_mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const float MBAR_TEXT_PX = 15.5f;           // ~13 px em, macos menu bar size

static float mfsqrt(float v){
    if (!(v > 0.0f)) return 0.0f;
    union { float f; uint32_t u; } c; c.f = v; c.u = 0x1fbd1df5u + (c.u >> 1);
    float x = c.f;
    for (int i = 0; i < 3; i++) x = 0.5f * (x + v / x);
    return x;
}

// anti-aliased arc on the right side of (cx, cy): the speaker's sound waves.
static void m_arc(int cx, int cy, float r, float thick, uint32_t c, int alpha){
    int R = (int)(r + thick) + 1;
    for (int y = cy - R; y <= cy + R; y++)
        for (int x = cx; x <= cx + R; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx <= 0.0f || (dy < 0 ? -dy : dy) > dx * 1.15f) continue;
            float d = mfsqrt(dx * dx + dy * dy) - r;
            if (d < 0) d = -d;
            float cov = thick * 0.5f + 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            MDraw::RoundRect(x, y, 1, 1, 0, c, (int)(alpha * cov));
        }
}

// menu bar layout, right to left: clock, control center, battery, volume,
// network. shared by render and hit-test.
struct MBarLayout { int clock_x, clock_w, cc_x, bat_x, vol_x, net_x; };
// clock.24h (settings: language & region), read at most once a second. (satoru)
static bool mbar_24h(){
    static bool v = false;
    static uint32_t at = 0;
    uint32_t now = Timer::GetRealMs();
    if(at == 0 || now - at > 1000u){ at = now ? now : 1; v = UIConfig::Bool("clock.24h", false); }
    return v;
}
static void mbar_clock_text(char* out, int mx){
    if(mbar_24h()){
        int h = Taskbar::ClockHour24();
        char hh[4]; hh[0] = (char)('0' + h / 10); hh[1] = (char)('0' + h % 10); hh[2] = 0;
        char m2[4]; m2[0] = (char)('0' + Taskbar::ClockMinute() / 10); m2[1] = (char)('0' + Taskbar::ClockMinute() % 10); m2[2] = 0;
        char d2[4]; int_to_str(Taskbar::ClockDom(), d2, 4);
        out[0] = 0;
        int dw2 = Taskbar::ClockDow(); if (dw2 < 0 || dw2 > 6) dw2 = 0;
        int mo2 = Taskbar::ClockMon() - 1; if (mo2 < 0 || mo2 > 11) mo2 = 0;
        sapp(out, k_dow[dw2], mx); sapp(out, " ", mx); sapp(out, d2, mx); sapp(out, " ", mx);
        sapp(out, k_mon[mo2], mx); sapp(out, "   ", mx); sapp(out, hh, mx); sapp(out, ":", mx);
        sapp(out, m2, mx);
        return;
    }
    int h12 = Taskbar::ClockHour12();
    char tb[8]; int_to_str(h12, tb, 8);
    char mm[4]; mm[0] = (char)('0' + Taskbar::ClockMinute() / 10); mm[1] = (char)('0' + Taskbar::ClockMinute() % 10); mm[2] = 0;
    char dom[4]; int_to_str(Taskbar::ClockDom(), dom, 4);
    out[0] = 0;
    int dw = Taskbar::ClockDow(); if (dw < 0 || dw > 6) dw = 0;
    int mo = Taskbar::ClockMon() - 1; if (mo < 0 || mo > 11) mo = 0;
    sapp(out, k_dow[dw], mx); sapp(out, " ", mx); sapp(out, dom, mx); sapp(out, " ", mx);
    sapp(out, k_mon[mo], mx); sapp(out, "   ", mx); sapp(out, tb, mx); sapp(out, ":", mx);
    sapp(out, mm, mx); sapp(out, Taskbar::ClockHour24() < 12 ? " AM" : " PM", mx);
}
static MBarLayout mbar_layout(){
    MBarLayout L;
    char clk[40]; mbar_clock_text(clk, 40);
    L.clock_w = FontTTF::MeasureText(MBAR_TEXT_PX, clk);
    L.clock_x = Taskbar::screen_width - 14 - L.clock_w;
    L.cc_x    = L.clock_x - 14 - 18;
    L.bat_x   = L.cc_x - 14 - 25;
    // no battery source: no battery icon, the volume moves up (satoru)
    L.vol_x   = Battery::Present() ? L.bat_x - 12 - 20 : L.cc_x - 14 - 20;
    L.net_x   = L.vol_x - 12 - 18;
    return L;
}
static int mbar_vol_cx(){ MBarLayout L = mbar_layout(); return L.vol_x + 10; }

static int dock_build(DockItem* it, int& bg_x, int& bg_w, bool scaled){
    int n = 0;
    for (int i = 0; i < TB_PINNED_COUNT && n < DOCK_MAX; i++) {
        // linux turned off in settings: no Alpine tile (satoru)
        if (tb_pinned[i].launch == &DesktopEnvironment::LaunchAlpine && !LinuxSettings::ShowLinuxTile()) continue;
        it[n].kind = 0; it[n].pin = i; it[n].win_id = -1;
        it[n].icon = AppIcons::IdForName(tb_pinned[i].name); n++;
    }
    // guest apps pinned in settings > linux, kind 3 (pin = their index) (satoru)
    for (int i = 0; i < LinuxSettings::DockAppCount() && n < DOCK_MAX; i++) {
        it[n].kind = 3; it[n].pin = i; it[n].win_id = -1;
        it[n].icon = AppIcons::GENERIC; n++;   // guest names would match native prefixes (satoru)
    }
    Window* wl = WindowManager::GetWindows();
    bool any = false;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (wl[i].state != WIN_CLOSED) { any = true; break; }
    if (any && n < DOCK_MAX) { it[n].kind = 2; it[n].pin = -1; it[n].win_id = -1; it[n].icon = -1; n++; }
    for (int i = 0; i < WM_MAX_WINDOWS && n < DOCK_MAX; i++) {
        if (wl[i].state == WIN_CLOSED) continue;
        it[n].kind = 1; it[n].pin = -1; it[n].win_id = wl[i].id;
        it[n].icon = AppIcons::IdForName(wl[i].title); n++;
    }
    int total = MDOCK_PAD * 2;
    for (int i = 0; i < n; i++) {
        float sc = scaled ? 1.0f + Taskbar::DockScale(i) : 1.0f;
        it[i].size = it[i].kind == 2 ? 1 : (int)(MDOCK_ICON * sc + 0.5f);
        total += it[i].size + (i ? MDOCK_GAP : 0);
    }
    bg_w = total;
    bg_x = (Taskbar::screen_width - total) / 2;
    int x = bg_x + MDOCK_PAD;
    for (int i = 0; i < n; i++) { it[i].x = x; x += it[i].size + MDOCK_GAP; }
    return n;
}

static void m_logo(int x, int y, int h){
    const int LBX0 = 97, LBY0 = 80, LBW = 99, LBH = 141;   // emblem bbox in logo_data
    int lw = (h * LBW) / LBH;
    for (int dy = 0; dy < h; dy++){
        int sy = LBY0 + (dy * LBH) / h;
        for (int dx = 0; dx < lw; dx++){
            int sx = LBX0 + (dx * LBW) / lw;
            uint32_t p = logo_data[sy * LOGO_WIDTH + sx];
            uint8_t a = (p >> 24) & 0xFF;
            if (a < 10) continue;
            Graphics::BlendPixel(x + dx, y + dy, 0xF2, 0xF3, 0xF8, a);
        }
    }
}

void Taskbar::RenderMenuBar(){
    const int sw = screen_width;
    // translucent bar over the wallpaper with a hairline underneath.
    Graphics::FillRectAlpha(0, 0, sw, MBAR_H, 150, 0xFF0E0F17);
    Graphics::FillRectAlpha(0, MBAR_H - 1, sw, 1, 40, 0xFFFFFFFF);
    if (start_menu_open) MDraw::RoundRect(6, 3, 34, MBAR_H - 6, 6, 0xFFFFFFFF, 40);
    m_logo(15, 6, MBAR_H - 12);

    Window* fw = WindowManager::GetFocusedWindow();
    const char* app = (fw && fw->state != WIN_MINIMIZED) ? fw->title : "Kurono";
    FontTTF::DrawText(50, 5, MBAR_TEXT_PX, app, 0xFFF2F3F8, true);

    MBarLayout L = mbar_layout();
    const uint32_t ink = 0xFFE8EAF2;
    int cy = MBAR_H / 2;

    // network: four rising bars (ethernet shows them all lit)
    {
        bool up = WiFi::IsLinkUp();
        int bars = WiFi::DetectedLink() == WiFi::LINK_ETHERNET ? 4 : WiFi::SignalBars();
        for (int i = 0; i < 4; i++) {
            int bh = 4 + i * 3;
            int a = (up && i < bars) ? 255 : 70;
            MDraw::RoundRect(L.net_x + i * 5, cy + 6 - bh, 3, bh, 1, ink, a);
        }
    }
    // volume: speaker with sound waves
    {
        bool muted = Audio::IsAvailable() ? Audio::IsMuted() : false;
        int vx = L.vol_x, a = muted ? 110 : 255;
        MDraw::RoundRect(vx, cy - 3, 4, 6, 1, ink, a);
        for (int k = 0; k < 5; k++) MDraw::RoundRect(vx + 4 + k, cy - 3 - k, 1, 6 + 2 * k, 0, ink, a);
        if (!muted) { m_arc(vx + 9, cy, 4.0f, 1.4f, ink, 255); m_arc(vx + 9, cy, 7.5f, 1.4f, ink, 200); }
        else Graphics::DrawLine(vx + 11, cy - 4, vx + 18, cy + 4, 0xFFFF6B6B);
    }
    // battery (only when a battery source reports one) (satoru)
    if (Battery::Present()) {
        int bx = L.bat_x, by = cy - 6;
        MDraw::RoundOutline(bx, by, 22, 12, 3, ink, 170);
        MDraw::RoundRect(bx + 23, by + 4, 2, 4, 1, ink, 170);
        int fill = (battery_pct * 18) / 100; if (fill < 2) fill = 2;
        MDraw::RoundRect(bx + 2, by + 2, fill, 8, 2, battery_pct > 20 ? ink : 0xFFFF6B6B, 255);
        if (Battery::GetState() == Battery::S_CHARGING) {
            // a small bolt over the fill (satoru)
            Graphics::DrawLine(bx + 12, by + 1, bx + 8, by + 6, 0xFF1C1C1E);
            Graphics::DrawLine(bx + 8, by + 6, bx + 13, by + 6, 0xFF1C1C1E);
            Graphics::DrawLine(bx + 13, by + 6, bx + 9, by + 11, 0xFF1C1C1E);
        }
    }
    // control center: two stacked toggles
    {
        int x = L.cc_x;
        MDraw::RoundRect(x, cy - 7, 18, 6, 3, ink, 120); MDraw::Circle(x + 13, cy - 4, 3, ink, 255);
        MDraw::RoundRect(x, cy + 1, 18, 6, 3, ink, 120); MDraw::Circle(x + 5,  cy + 4, 3, ink, 255);
    }
    char clk[40]; mbar_clock_text(clk, 40);
    FontTTF::DrawText(L.clock_x, 5, MBAR_TEXT_PX, clk, 0xFFF2F3F8);
    // the bar never damaged itself: a new minute, app name, battery or link
    // state reached the screen only when something else damaged the bar (the
    // default path presents the full screen every frame, so it never showed).
    // with damage-only rendering (kurono.gfx.scissor=1, kurono.wpdamage=1)
    // the bar damages itself when what it shows changed. (satoru)
    if (Graphics::ScissorEnabled() || Graphics::WallpaperDamageOnly()) {
        static uint32_t s_sig = 0;
        uint32_t sig = 2166136261u;
        for (const char* p = clk; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
        for (const char* p = app; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
        sig = (sig ^ (uint32_t)battery_pct) * 16777619u;
        sig = (sig ^ (uint32_t)(WiFi::IsLinkUp() ? 1 : 0) ^ ((uint32_t)WiFi::SignalBars() << 1)) * 16777619u;
        sig = (sig ^ (uint32_t)(Audio::IsAvailable() && Audio::IsMuted() ? 1 : 0) ^ (start_menu_open ? 2u : 0u)) * 16777619u;
        if (sig != s_sig) { s_sig = sig; Graphics::MarkDirty(0, 0, sw, MBAR_H); }
    }
}

void Taskbar::RenderDock(){
    DockItem it[DOCK_MAX];
    int bg_x, bg_w;
    int n = dock_build(it, bg_x, bg_w, true);
    int dock_y = y_pos;
    int base_bottom = dock_y + MDOCK_PAD + MDOCK_ICON;

    MDraw::Shadow(bg_x, dock_y, bg_w, MDOCK_H, 18, 6, 4, 120);
    MDraw::RoundRect(bg_x, dock_y, bg_w, MDOCK_H, 18, 0xFF2A2B38, 0xFF1A1B24, 170);
    MDraw::RoundOutline(bg_x, dock_y, bg_w, MDOCK_H, 18, 0xFFFFFFFF, 40);

    Window* wl = WindowManager::GetWindows();
    uint32_t now = Timer::GetRealMs();
    int hover = -1;
    for (int i = 0; i < n; i++) {
        DockItem& d = it[i];
        if (d.kind == 2) {
            Graphics::FillRectAlpha(d.x, dock_y + 12, 1, MDOCK_H - 24, 60, 0xFFFFFFFF);
            continue;
        }
        int sz = d.size, tx = d.x, ty = base_bottom - sz;
        // launch bounce: a pinned app hops while its window opens.
        if (d.kind == 0) {
            uint32_t lms = PinnedLaunchMs(d.pin);
            if (lms && now - lms < 900) {
                // two hops, the second smaller, like a ball settling: |sin| under
                // a decaying envelope (satoru)
                float t = (float)(now - lms);
                float ph = t / 380.0f * 3.14159265f;
                float sn = ph - 3.14159265f * (float)(int)(ph / 3.14159265f);   // fold to 0..pi (satoru)
                float x2 = (sn - 1.5707963f) * (sn - 1.5707963f);
                float hs = 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f));   // sin via cos(x - pi/2) (satoru)
                if (hs < 0.0f) hs = 0.0f;
                float env = t < 380.0f ? 1.0f : (t < 760.0f ? 0.42f : 0.0f);
                float hop = hs * env;
                ty -= (int)(hop * 18.0f + 0.5f);
                Graphics::MarkDirty(tx - 4, dock_y - 40, sz + 8, MDOCK_H + 40);
            }
        }
        // window tiles take the minimize catch / restore press from the wm.
        int sx_q8 = 256, sy_q8 = 256, flash = 0;
        if (d.kind == 1 && WindowManager::TaskbarCatch(d.win_id, tx, ty, sz, sz, sx_q8, sy_q8, flash)) {
            int nw = (sz * sx_q8) >> 8, nh = (sz * sy_q8) >> 8;
            MDraw::AppTile(d.icon, tx + (sz - nw) / 2, ty + (sz - nh), nw < nh ? nw : nh);
            if (flash > 0) MDraw::RoundRect(tx + (sz - nw) / 2, ty + (sz - nh), nw, nh, (nw * 23) / 100, 0xFFFFFFFF, flash);
        } else {
            MDraw::AppTile(d.icon, tx, ty, sz);
        }
        // running dot
        bool running = d.kind == 1;
        if (d.kind == 0)
            for (int w = 0; w < WM_MAX_WINDOWS && !running; w++)
                if (wl[w].state != WIN_CLOSED && wl[w].title[0] == tb_pinned[d.pin].glyph) running = true;
        if (running) MDraw::Circle(d.x + d.size / 2, dock_y + MDOCK_H - 4, 2, 0xFFE8EAF2, 220);
        if (dock_mouse_y >= dock_y - 4 && dock_mouse_x >= d.x && dock_mouse_x < d.x + d.size + MDOCK_GAP) hover = i;
    }
    // tooltip above the hovered tile
    if (hover >= 0 && it[hover].kind != 2) {
        const char* label = "";
        if (it[hover].kind == 0) label = tb_pinned[it[hover].pin].name;
        else if (it[hover].kind == 3) label = LinuxSettings::DockAppName(it[hover].pin);   // (satoru)
        else { Window* w = WindowManager::GetWindow(it[hover].win_id); if (w) label = w->title; }
        int tw = FontTTF::MeasureText(14.0f, label);
        int cx = it[hover].x + it[hover].size / 2;
        int ty = base_bottom - it[hover].size - 34;
        MDraw::Shadow(cx - tw / 2 - 10, ty, tw + 20, 24, 8, 3, 2, 70);
        MDraw::RoundRect(cx - tw / 2 - 10, ty, tw + 20, 24, 8, 0xFF2C2D3A, 235);
        MDraw::RoundOutline(cx - tw / 2 - 10, ty, tw + 20, 24, 8, 0xFFFFFFFF, 30);
        FontTTF::DrawText(cx - tw / 2, ty + 4, 14.0f, label, 0xFFF2F3F8);
    }
    // the dock used to damage itself on every frame. that only cost a small
    // present before; under the damage scissor (kurono.gfx.scissor=1) it is
    // an extra render pass per frame, so there it damages itself only when
    // what it shows changed: item layout, hover, running dots, a bounce or a
    // catch. (satoru)
    if (Graphics::ScissorEnabled()) {
        static uint32_t s_sig = 0;
        static int s_rx = 0, s_ry = 0, s_rw = 0, s_rh = 0;
        uint32_t sig = 0x811C9DC5u ^ (uint32_t)n ^ ((uint32_t)bg_x << 8) ^ ((uint32_t)bg_w << 20) ^ ((uint32_t)(hover + 1) << 3);
        for (int i = 0; i < n; i++) {
            sig = sig * 16777619u ^ (uint32_t)it[i].x;
            sig = sig * 16777619u ^ (uint32_t)(it[i].size * 7 + it[i].kind * 131 + it[i].icon * 977);
            sig = sig * 16777619u ^ (uint32_t)it[i].win_id;
        }
        for (int w = 0; w < WM_MAX_WINDOWS; w++) sig = sig * 31u + (uint32_t)(wl[w].state != WIN_CLOSED) + (uint32_t)wl[w].state * 3u;
        bool moving = false;
        for (int i = 0; i < n && !moving; i++) {
            if (it[i].kind == 0) { uint32_t lms = PinnedLaunchMs(it[i].pin); if (lms && now - lms < 900) moving = true; }
            if (it[i].kind == 1) { int e = 0, f = 0, g = 0; if (WindowManager::TaskbarCatch(it[i].win_id, it[i].x, y_pos, it[i].size, it[i].size, e, f, g)) moving = true; }
        }
        int rx = bg_x - 40, ry = dock_y - 60, rw = bg_w + 80, rh = MDOCK_H + 70;
        if (moving || sig != s_sig || rx != s_rx || rw != s_rw || ry != s_ry || rh != s_rh) {
            Graphics::MarkDirty(rx, ry, rw, rh);
            if (s_rw > 0) Graphics::MarkDirty(s_rx, s_ry, s_rw, s_rh);
        }
        s_sig = sig; s_rx = rx; s_ry = ry; s_rw = rw; s_rh = rh;
        return;
    }
    Graphics::MarkDirty(bg_x - 40, dock_y - 60, bg_w + 80, MDOCK_H + 70);
}

void Taskbar::RenderStartMenuModern(){
    if(!start_menu_open && start_menu_phase <= 0.01f) return;
    float t  = ease_in_out_cubic(start_menu_phase);
    int full_h = START_MENU_H;
    int lift   = (int)((1.0f - t) * 12.0f);
    int mx0, my0;
    tb_start_menu_xy(full_h, lift, mx0, my0);
    int alpha = (int)(t * 255.0f);
    MDraw::Shadow(mx0, my0, START_MENU_W, full_h, 14, 8, 6, (int)(t * 110));
    MDraw::RoundRect(mx0, my0, START_MENU_W, full_h, 14, 0xFF262734, 0xFF1C1D27, alpha * 240 / 255);
    MDraw::RoundOutline(mx0, my0, START_MENU_W, full_h, 14, 0xFFFFFFFF, alpha * 36 / 255);
    Graphics::MarkDirty(mx0 - 12, my0 - 12, START_MENU_W + 24, full_h + 30);
    if (t < 0.55f) return;
    int ta = (int)((t - 0.55f) / 0.45f * 255.0f);
    uint32_t ink = MDraw::Mix(0xFF1C1D27, 0xFFF2F3F8, ta), sub = MDraw::Mix(0xFF1C1D27, 0xFF9AA0B8, ta);
    FontTTF::DrawText(mx0 + 16, my0 + 12, 17.0f, "Kurono", ink, true);
    FontTTF::DrawText(mx0 + 16, my0 + 32, 14.0f, "v1.0", sub);
    Graphics::FillRectAlpha(mx0 + 14, my0 + 50, START_MENU_W - 28, 1, ta / 5, 0xFFFFFFFF);
    static const char* items[] = {
        "Terminal", "File Browser", "Calculator", "Text Editor", "Browser", "Media Player",
        "Settings", "Task Manager", "---", "Restart Shell", "Sign Out", "Shut Down",
    };
    static const int icons[] = {
        AppIcons::TERMINAL, AppIcons::FILES, AppIcons::CALCULATOR, AppIcons::EDITOR,
        AppIcons::BROWSER,  AppIcons::MEDIA, AppIcons::SETTINGS,   AppIcons::TASKS, -1, -1, -1, -1,
    };
    int iy = my0 + 56;
    for (int i = 0; i < 12; i++) {
        if (items[i][0] == '-') { Graphics::FillRectAlpha(mx0 + 14, iy + 6, START_MENU_W - 28, 1, ta / 5, 0xFFFFFFFF); iy += 12; continue; }
        if (dock_mouse_x >= mx0 + 6 && dock_mouse_x < mx0 + START_MENU_W - 6 && dock_mouse_y >= iy && dock_mouse_y < iy + 32)
            MDraw::RoundRect(mx0 + 6, iy + 1, START_MENU_W - 12, 30, 7, 0xFF5C8AFF, ta * 200 / 255);
        if (icons[i] >= 0) {
            MDraw::AppTile(icons[i], mx0 + 14, iy + 4, 24);
            FontTTF::DrawText(mx0 + 48, iy + 8, 15.0f, items[i], ink);
        } else {
            FontTTF::DrawText(mx0 + 18, iy + 8, 15.0f, items[i], i == 11 ? MDraw::Mix(0xFF1C1D27, 0xFFFF8A8A, ta) : ink);
        }
        iy += 32;
    }
}

void Taskbar::RenderModern(){
    RenderMenuBar();
    RenderDock();
    RenderStartMenuModern();
    RenderVolumePopup();
}

void Taskbar::TickModern(uint32_t delta_ms, int mx, int my){
    dock_mouse_x = mx; dock_mouse_y = my;
    // magnification follows the pointer along the dock and glides with a
    // first-order lag, so tiles swell and settle rather than snap.
    DockItem it[DOCK_MAX];
    int bg_x, bg_w;
    int n = dock_build(it, bg_x, bg_w, false);
    bool near = my >= y_pos - 12 && my < screen_height && mx >= bg_x - 30 && mx < bg_x + bg_w + 30;
    float k = (float)delta_ms / 90.0f; if (k > 1.0f) k = 1.0f;
    for (int i = 0; i < DOCK_MAX; i++) {
        float target = 0.0f;
        if (near && i < n && it[i].kind != 2) {
            float d = (float)(mx - (it[i].x + it[i].size / 2)) / (float)MDOCK_ICON;
            if (d < 0) d = -d;
            float tt = 1.0f - d / 2.3f;
            if (tt > 0.0f) target = 0.40f * tt * tt * (3.0f - 2.0f * tt);
        }
        dock_scale[i] += (target - dock_scale[i]) * k;
        if (dock_scale[i] < 0.001f && target == 0.0f) dock_scale[i] = 0.0f;
        g_dock_target[i] = target;
    }
}

bool Taskbar::HandleClickModern(int mx, int my){
    // menu bar
    if (my < MBAR_H) {
        if (mx < 44) { VolumePopupSet(false); StartMenuSet(!start_menu_open); return true; }
        MBarLayout L = mbar_layout();
        if (mx >= L.vol_x - 4 && mx < L.vol_x + 24) {
            VolumePopupSet(!volume_popup_open);
            StartMenuSet(false);
            if (volume_popup_open && Audio::IsAvailable()) volume_slider_val = Audio::GetMasterVolume();
            return true;
        }
        if (mx >= L.net_x - 6) {
            ControlCenter::ToggleAt(mx, MBAR_H + 4);
            VolumePopupSet(false); StartMenuSet(false);
            return true;
        }
        StartMenuSet(false); VolumePopupSet(false);
        return true;
    }
    // dock
    DockItem it[DOCK_MAX];
    int bg_x, bg_w;
    int n = dock_build(it, bg_x, bg_w, true);
    if (my >= y_pos && mx >= bg_x && mx < bg_x + bg_w) {
        StartMenuSet(false); VolumePopupSet(false);
        for (int i = 0; i < n; i++) {
            if (it[i].kind == 2 || mx < it[i].x - MDOCK_GAP / 2 || mx >= it[i].x + it[i].size + MDOCK_GAP / 2) continue;
            if (it[i].kind == 0) {
                PinnedLaunched(it[i].pin);
                tb_pinned[it[i].pin].launch();
                return true;
            }
            if (it[i].kind == 3) { LinuxSettings::LaunchDockApp(it[i].pin); return true; }   // (satoru)
            Window* w = WindowManager::GetWindow(it[i].win_id);
            if (!w) return true;
            if (w->state == WIN_MINIMIZED || w->anim_kind == 3) {
                WindowManager::Unminimize(w->id);
                WindowManager::BringToFront(w->id);
            } else if (WindowManager::GetFocusedWindow() == w) {
                WindowManager::Minimize(w->id);
            } else {
                WindowManager::BringToFront(w->id);
            }
            return true;
        }
        return true;
    }
    StartMenuSet(false);
    VolumePopupSet(false);
    return false;
}

void Taskbar::RenderClock(){
    // Time + date packed in the right cluster slot.
    char timebuf[8];
    int h12 = clock_h % 12;
    if(h12 == 0) h12 = 12;
    int2(h12, timebuf);
    timebuf[2]=':';
    int2(clock_m, timebuf+3);
    timebuf[5]=0;
    const char* ampm = (clock_h < 12) ? " AM" : " PM";
    char full_time[16] = {0};
    scpy(full_time, timebuf, 16);
    sapp(full_time, ampm, 16);

    // date pulled from the once-per-second cache, not recomputed here. (satoru)
    char datebuf[16] = {0};
    int_to_str(clock_mon, datebuf, 16);
    sapp(datebuf, "/", 16);
    char daybuf[4]; int_to_str(clock_dom, daybuf, 4); sapp(datebuf, daybuf, 16);
    sapp(datebuf, "/", 16);
    char yearbuf[8]; int_to_str(clock_year, yearbuf, 8); sapp(datebuf, yearbuf, 16);

    int cx = tb_clock_x() + 4;
    Graphics::DrawString(cx, y_pos+6,  full_time, 0xFFF0F0FF, 0xFF000000);
    Graphics::DrawString(cx, y_pos+24, datebuf,   0xFF8898C0, 0xFF000000);
}

void Taskbar::RenderSystemTray(){
    // System tray cluster: bell, network, volume, battery - packed
    // from right to left using helpers.

    // Notifications bell (decorative for now; click opens ControlCenter).
    {
        int x = tb_bell_x();
        int by_ = y_pos + 10;
        int bell_h = TASKBAR_HEIGHT - 20;
        // bell body
        Graphics::FillRoundedRect(x+5, by_+1, 10, bell_h-4, 4, 0xFFD0D8F0);
        // bell rim
        Graphics::FillRect(x+3, by_+bell_h-4, 14, 2, 0xFFD0D8F0);
        // clapper
        Graphics::FillRect(x+9, by_+bell_h-2, 2, 2, 0xFFD0D8F0);
    }

    // Network indicator
    {
        int x = tb_net_x();
        WiFi::LinkKind link = WiFi::DetectedLink();
        int bars = WiFi::SignalBars();
        bool up   = WiFi::IsLinkUp();
        unsigned int on  = up ? 0xFFD0D8F0 : 0xFF606080;
        unsigned int off = 0xFF303050;
        if (link == WiFi::LINK_ETHERNET) {
            int ex = x + 4, ey = y_pos + 14;
            Graphics::FillRect(ex, ey + 2, 16, 12, on);
            Graphics::FillRect(ex + 6, ey, 4, 4, on);
            unsigned int pin = up ? 0xFF1A1A24 : 0xFF202030;
            for (int i = 0; i < 4; i++) Graphics::FillRect(ex + 2 + i*4, ey + 12, 2, 2, pin);
        } else {
            for (int i = 0; i < 4; i++) {
                int bh = 5 + i*3;
                unsigned int bc = (i < bars) ? on : off;
                Graphics::FillRect(x + 3 + i*5, y_pos + 26 - bh, 3, bh, bc);
            }
        }
    }

    // Volume speaker icon
    {
        int x = tb_vol_x();
        int vy = y_pos + 14;
        bool muted = Audio::IsAvailable() ? Audio::IsMuted() : false;
        uint32_t vc = muted ? 0xFF707088 : COL_TRAY_TEXT;
        Graphics::FillRect(x+4, vy+4, 4, 6, vc);
        Graphics::FillRect(x+8, vy+2, 3, 10, vc);
        if (!muted) {
            for(int a=0; a<2; a++) {
                int r = 4 + a*3;
                for(int dy=-r; dy<=r; dy++) {
                    int dx_sq = r*r - dy*dy;
                    if(dx_sq < 0) continue;
                    int dx = 0;
                    while((dx+1)*(dx+1) <= dx_sq) dx++;
                    int px = x + 12 + dx;
                    int py = vy + 7 + dy;
                    if(py >= vy && py < vy+14)
                        Graphics::DrawPixel(px, py, 0xFFAAAABB);
                }
            }
        } else {
            // mute slash
            Graphics::DrawLine(x+12, vy+2, x+22, vy+12, 0xFFE74C3C);
        }
    }

    // Battery
    {
        int bx = tb_bat_x();
        int by_ = y_pos + 14;
        Graphics::FillRoundedRect(bx, by_, 22, 12, 3, 0xFF2A2A44);
        Graphics::FillRect(bx+22, by_+3, 2, 5, 0xFF2A2A44);
        int fill = (battery_pct * 18) / 100;
        if (fill < 2) fill = 2;
        unsigned int bclr = (battery_pct>20) ? 0xFF2ECC71 : 0xFFE74C3C;
        Graphics::FillRoundedRect(bx+2, by_+2, fill, 8, 2, bclr);
    }
}

void Taskbar::RenderStartMenu(){
    // Skip render entirely when fully closed; while closing keep drawing
    // until the eased phase decays to ~0.
    if(!start_menu_open && start_menu_phase <= 0.01f) return;

    float t  = ease_in_out_cubic(start_menu_phase);

    // Slide-up + scale: the menu grows from 92% -> 100% of full height
    // and lifts a few pixels into place.
    int full_h = START_MENU_H;
    int draw_h = (int)(full_h * (0.92f + 0.08f * t) + 0.5f);
    int lift   = (int)((1.0f - t) * 16.0f);

    // left-aligned off the k button (which is anchored to the floating bar) - 
    // above the bar when docked bottom, below it when docked top. (satoru)
    int mx0, my0;
    tb_start_menu_xy(draw_h, lift, mx0, my0);

    // shadow
    Graphics::FillRoundedRect(mx0+6, my0+6, START_MENU_W, draw_h, 14, 0xFF040408);
    // background
    Graphics::FillRoundedRect(mx0, my0, START_MENU_W, draw_h, 14, COL_START_MENU);
    // border
    Graphics::DrawRect(mx0, my0, START_MENU_W, draw_h, 0xFF2A2A48);
    // Skip drawing the content while still small to avoid jarring text squish.
    if (t < 0.35f) {
        Graphics::MarkDirty(mx0-2, my0-2, START_MENU_W+8, draw_h+10);
        return;
    }

    // header
    Graphics::FillRoundedRect(mx0+1, my0+1, START_MENU_W-2, 52, 14, 0xFF0E0E1C);
    Graphics::FillRect(mx0+1, my0+40, START_MENU_W-2, 13, 0xFF0E0E1C);
    Graphics::DrawString(mx0+16, my0+12, "Kurono OS", 0xFFF0F0FF, 0xFF000000);
    Graphics::DrawString(mx0+16, my0+30, "v1.0", 0xFF606090, 0xFF000000);
    Graphics::FillRect(mx0+16, my0+50, START_MENU_W-32, 1, 0xFF5C8AFF);

    static const char* items[] = {
        " Terminal",
        " File Browser",
        " Calculator",
        " Text Editor",
        " Browser",
        " Media Player",
        " Settings",
        " Task Manager",
        "---",
        " Restart Shell",
        " Sign Out",
        " Shutdown",
    };
    static const unsigned int item_accents[] = {
        0xFF2ECC71, 0xFFF39C12, 0xFF9B59B6, 0xFFE74C3C,
        0xFF3498DB, 0xFFE91E63, 0xFF607D8B, 0xFF00BCD4,
        0, 0xFF5C8AFF, 0xFFFFA726, 0xFFFF5252,
    };
    // per-row vector icon for the app launchers (rows 0..7). the separator and
    // the session-control rows (restart/sign out/shutdown) keep their accent bar
    // and carry no icon (-1). (satoru)
    static const int item_icons[] = {
        AppIcons::TERMINAL, AppIcons::FILES, AppIcons::CALCULATOR, AppIcons::EDITOR,
        AppIcons::BROWSER,  AppIcons::MEDIA, AppIcons::SETTINGS,   AppIcons::TASKS,
        -1, -1, -1, -1,
    };
    static const int nit = 12;

    int iy = my0 + 56;
    for(int i=0;i<nit;i++){
        if(items[i][0]=='-'){
            Graphics::FillRect(mx0+16, iy+6, START_MENU_W-32, 1, 0xFF2A2A48);
            iy += 12;
            continue;
        }
        if (item_icons[i] >= 0){
            // 24px vector glyph on the left; text shifts right to clear it. the
            // accent bar is dropped here since the icon already carries the
            // accent color. (satoru)
            AppIcons::Draw(item_icons[i], mx0+12, iy+1, 24);
            Graphics::DrawString(mx0+44, iy+4, items[i], COL_ICON_TEXT, 0xFF000000);
        } else {
            if (item_accents[i])
                Graphics::FillRoundedRect(mx0+8, iy+4, 3, 20, 1, item_accents[i]);
            Graphics::DrawString(mx0+20, iy+4, items[i], COL_ICON_TEXT, 0xFF000000);
        }
        iy += 32;
    }
    Graphics::MarkDirty(mx0-2, my0-2, START_MENU_W+8, draw_h+10);
}

void Taskbar::Render(){
    if (UIStyle::Modern()) { RenderModern(); return; }
    int bx = tb_bar_x();
    int by = tb_bar_y();
    int bw = tb_bar_w();
    int bh = tb_bar_h();
    int br = tb_bar_r();

    // Soft drop shadow under the floating bar.  Two stacked alpha
    // fills produce a quick gradient feel without per-pixel maths.
    Graphics::FillRectAlpha(bx + 2, by + bh + 2, bw - 4, 4, 90,  0xFF000000);
    Graphics::FillRectAlpha(bx + 4, by + bh + 6, bw - 8, 3, 50,  0xFF000000);

    // Bar body - rounded "card"
    Graphics::FillRoundedRect(bx, by, bw, bh, br, COL_TASKBAR);
    // Subtle top inner highlight (fakes a glossy edge)
    Graphics::FillRoundedRect(bx, by, bw, 1, 0, COL_TASKBAR_TOP);

    RenderStartButton();
    RenderTaskButtons();
    RenderSystemTray();
    RenderClock();
    RenderStartMenu();
    RenderVolumePopup();

    if (search_active && search_buf[0]) {
        RenderSearchResults();
    }
}

void Taskbar::RenderSearchResults(){
    // apps database
    static const char* app_names[] = {
        "Terminal", "Files", "Calculator", "Editor",
        "Settings", "Browser", "Media Player", "Task Manager"
    };
    static const int app_count = 8;

    // substring matching (case-insensitive)
    int matches[8]; int match_count = 0;
    int qlen = slen(search_buf);
    if (qlen <= 0) return;
    for(int i=0; i<app_count && match_count < 8; i++){
        // check if search_buf is a substring of app_names[i]
        int alen = slen(app_names[i]);
        bool found = false;
        for(int s=0; s <= alen - qlen && !found; s++){
            bool ok = true;
            for(int j=0; j<qlen && ok; j++){
                char a = app_names[i][s+j];
                char b = search_buf[j];
                if(a>='A' && a<='Z') a += 32;
                if(b>='A' && b<='Z') b += 32;
                if(a != b) ok = false;
            }
            if(ok) found = true;
        }
        if(found) matches[match_count++] = i;
    }
    if(match_count == 0){
        // no matches: show "no results" anchored under the search pill.
        int sx = tb_search_x();
        int sw = tb_search_w();
        int sh_ = 36;
        int sy = tb_popup_y(sh_, 4, 0);
        Graphics::FillRoundedRect(sx, sy, sw, sh_, 10, 0xFF141424);
        Graphics::DrawRect(sx, sy, sw, sh_, 0xFF2A2A48);
        Graphics::DrawString(sx+12, sy+10, "No results", 0xFF606080, 0xFF000000);
        Graphics::MarkDirty(sx-2, sy-2, sw+8, sh_+8);
        return;
    }

    int sx = tb_search_x();
    int sh = match_count * 30 + 12;
    int sy = tb_popup_y(sh, 4, 0);
    int sw = tb_search_w();

    // shadow + bg
    Graphics::FillRoundedRect(sx+4, sy+4, sw, sh, 10, 0xFF060610);
    Graphics::FillRoundedRect(sx, sy, sw, sh, 10, 0xFF141424);
    Graphics::DrawRect(sx, sy, sw, sh, 0xFF2A2A48);

    int iy = sy + 6;
    for(int i=0; i<match_count; i++){
        Graphics::DrawString(sx+12, iy+6, app_names[matches[i]], 0xFFD0D0E0, 0xFF000000);
        iy += 30;
    }
    Graphics::MarkDirty(sx-2, sy-2, sw+8, sh+8);
}

void Taskbar::RenderVolumePopup(){
    if(!volume_popup_open && volume_popup_phase <= 0.01f) return;
    float t = ease_in_out_cubic(volume_popup_phase);

    int pop_w = 52, pop_h_full = 180;
    int pop_h = (int)(pop_h_full * (0.5f + 0.5f * t) + 0.5f);
    int pop_x, pop_y;
    tb_volume_popup_xy(pop_h, (int)((1.0f - t) * 10.0f), pop_x, pop_y);

    // shadow + background
    Graphics::FillRoundedRect(pop_x+4, pop_y+4, pop_w, pop_h, 8, 0xFF060610);
    Graphics::FillRoundedRect(pop_x, pop_y, pop_w, pop_h, 8, 0xFF181828);
    Graphics::DrawRect(pop_x, pop_y, pop_w, pop_h, 0xFF333355);
    Graphics::MarkDirty(pop_x-4, pop_y-4, pop_w+12, pop_h_full+16);
    if (t < 0.55f) return;   // skip content while still squat - looks smoother

    // mute/unmute icon at top
    int icon_cx = pop_x + pop_w/2;
    bool muted = Audio::IsAvailable() ? Audio::IsMuted() : false;
    if(muted){
        Graphics::DrawString(icon_cx-8, pop_y+8, "XX", 0xFFE74C3C, 0xFF000000);
    } else {
        // speaker icon
        Graphics::FillRect(icon_cx-5, pop_y+12, 4, 8, COL_WHITE);
        Graphics::FillRect(icon_cx-1, pop_y+9, 3, 14, COL_WHITE);
        Graphics::DrawCircle(icon_cx+7, pop_y+16, 4, COL_WHITE);
    }

    // vertical slider track
    int track_x = pop_x + pop_w/2 - 2;
    int track_y = pop_y + 36;
    int track_h = 110;
    // track background
    Graphics::FillRect(track_x, track_y, 4, track_h, 0xFF2A2A44);

    // filled portion (bottom = 0%, top = 100%)
    int fill_h = (volume_slider_val * track_h) / 100;
    if(fill_h > 0){
        Graphics::FillRect(track_x, track_y + track_h - fill_h, 4, fill_h, 0xFF5C8AFF);
    }

    // slider knob
    int knob_y = track_y + track_h - fill_h - 6;
    if(knob_y < track_y - 6) knob_y = track_y - 6;
    Graphics::FillCircle(track_x + 2, knob_y + 6, 7, COL_WHITE);
    Graphics::DrawCircle(track_x + 2, knob_y + 6, 7, 0xFF5C8AFF);

    // volume percentage text
    char vol_str[8];
    int_to_str(volume_slider_val, vol_str, 8);
    sapp(vol_str, "%", 8);
    int tw = slen(vol_str) * 8;
    Graphics::DrawString(icon_cx - tw/2, pop_y + pop_h - 20, vol_str, COL_TRAY_TEXT, 0xFF000000);
}

bool Taskbar::HandleClick(int mx,int my){
    if(volume_popup_open && volume_popup_phase > 0.5f){
        int pop_w = 52, pop_h = 180;
        int pop_x, pop_y;
        tb_volume_popup_xy(pop_h, 0, pop_x, pop_y);

        if(mx>=pop_x && mx<pop_x+pop_w && my>=pop_y && my<pop_y+pop_h){
            if(my < pop_y + 32){
                if(Audio::IsAvailable()) Audio::SetMuted(!Audio::IsMuted());
                return true;
            }
            int track_y = pop_y + 36, track_h = 110;
            if(my >= track_y - 6 && my <= track_y + track_h + 6){
                int rel = track_y + track_h - my;
                int vol = (rel * 100) / track_h;
                if(vol < 0) vol = 0; if(vol > 100) vol = 100;
                volume_slider_val = vol;
                if(Audio::IsAvailable()){ Audio::SetMasterVolume(vol); Audio::SetMuted(false); }
                volume_slider_dragging = true;
            }
            return true;
        }
        VolumePopupSet(false);
    }

    // popups (start menu / search results) float off the bar: above it for a
    // bottom dock (my < y_pos), below it for a top dock. handle those clicks
    // here; bar-row clicks fall through to the widgets below. (satoru)
    bool off_bar = cfg_position_top ? (my >= y_pos + TASKBAR_HEIGHT) : (my < y_pos);
    if (UIStyle::Modern()) off_bar = true;     // menus hang off the menu bar
    if(off_bar){
        // start menu handling
        if(start_menu_open){
            int mx0, my0;
            tb_start_menu_xy(START_MENU_H, 0, mx0, my0);
            if(mx>=mx0 && mx<mx0+START_MENU_W && my>=my0 && my<my0+START_MENU_H){
                int iy = my0 + 56;
                static const int nit = 12;
                for(int i=0;i<nit;i++){
                    if(i==8){ iy+=12; continue; }
                    if(my>=iy && my<iy+32){
                        StartMenuSet(false);
                        switch(i){
                            case 0: DesktopEnvironment::LaunchTerminal(); break;
                            case 1: DesktopEnvironment::LaunchFileBrowser(); break;
                            case 2: DesktopEnvironment::LaunchCalculator(); break;
                            case 3: DesktopEnvironment::LaunchTextEditor(); break;
                            case 4: DesktopEnvironment::LaunchBrowser(); break;
                            case 5: DesktopEnvironment::LaunchMediaPlayer(); break;
                            case 6: DesktopEnvironment::LaunchSettings(); break;
                            case 7: DesktopEnvironment::LaunchTaskManager(); break;
                            case 9: KuronoShell::Init(); TerminalApp::Init();
                                    DesktopEnvironment::LaunchTerminal(); break;
                            case 10: DesktopEnvironment::RequestLogout(); break;
                            // shutdown: the one power sequence saves, stops
                            // services and power-offs (acpi s5). (satoru)
                            case 11: PowerManager::Request(PowerManager::ACTION_SHUTDOWN, "start menu"); break;
                        }
                        return true;
                    }
                    iy+=32;
                }
                return true;
            }
            StartMenuSet(false);
            // a click on the logo while the menu is open just closes it.
            if (UIStyle::Modern() && my < MBAR_H && mx < 44) return true;
        }
        if (UIStyle::Modern()) return HandleClickModern(mx, my);
        // search results click
        if(search_active && search_buf[0]){
            // check if click is in search results area
            static const char* sr_names[] = {
                "Terminal","Files","Calculator","Editor",
                "Settings","Browser","Media Player","Task Manager"
            };
            int qlen = slen(search_buf);
            int matches2[8]; int mc2=0;
            for(int i=0;i<8 && mc2<8;i++){
                int alen = slen(sr_names[i]); bool found=false;
                for(int s=0;s<=alen-qlen && !found;s++){
                    bool ok=true;
                    for(int j=0;j<qlen && ok;j++){
                        char a=sr_names[i][s+j], b=search_buf[j];
                        if(a>='A'&&a<='Z') a+=32; if(b>='A'&&b<='Z') b+=32;
                        if(a!=b) ok=false;
                    }
                    if(ok) found=true;
                }
                if(found) matches2[mc2++]=i;
            }
            if(mc2 > 0){
                int sh2 = mc2*30+12, sy2 = tb_popup_y(sh2, 4, 0), sx2=tb_search_x(), sw2=tb_search_w();
                if(mx>=sx2 && mx<sx2+sw2 && my>=sy2 && my<sy2+sh2){
                    int ry = sy2+6;
                    for(int i=0;i<mc2;i++){
                        if(my>=ry && my<ry+30){
                            search_active=false; search_buf[0]=0; search_len=0;
                            switch(matches2[i]){
                                case 0: DesktopEnvironment::LaunchTerminal(); break;
                                case 1: DesktopEnvironment::LaunchFileBrowser(); break;
                                case 2: DesktopEnvironment::LaunchCalculator(); break;
                                case 3: DesktopEnvironment::LaunchTextEditor(); break;
                                case 4: DesktopEnvironment::LaunchSettings(); break;
                                case 5: DesktopEnvironment::LaunchBrowser(); break;
                                case 6: DesktopEnvironment::LaunchMediaPlayer(); break;
                                case 7: DesktopEnvironment::LaunchTaskManager(); break;
                            }
                            return true;
                        }
                        ry+=30;
                    }
                    return true;
                }
            }
        }
        return false;
    }

    // a click that is neither off-bar (handled above) nor inside the bar row is
    // in the thin wallpaper gap on the bar's far side - pass it through rather
    // than swallow it (swallowing it is what broke all input with a top dock). (satoru)
    if(my < y_pos || my >= y_pos + TASKBAR_HEIGHT){
        StartMenuSet(false);
        VolumePopupSet(false);
        search_active = false;
        return false;
    }

    // K button - anchored to the floating bar's left edge
    {
        int k_x = tb_bar_x() + 6;
        if(mx>=k_x && mx<k_x+44 && my>=y_pos+4 && my<y_pos+TASKBAR_HEIGHT-4){
            StartMenuSet(!start_menu_open);
            VolumePopupSet(false);
            search_active = false;
            return true;
        }
    }

    // Quick-launch pinned icons
    {
        int strip_x = tb_pin_strip_x();
        int strip_y = tb_bar_y() + (TASKBAR_HEIGHT - TB_PIN_SZ) / 2;
        for (int i = 0; i < TB_PINNED_COUNT; i++) {
            int ix = strip_x + i * (TB_PIN_SZ + TB_PIN_GAP);
            if (mx >= ix && mx < ix + TB_PIN_SZ &&
                my >= strip_y && my < strip_y + TB_PIN_SZ) {
                PinnedLaunched(i);                 // spring bounce feedback (satoru)
                if (tb_pinned[i].launch) tb_pinned[i].launch();
                StartMenuSet(false);
                VolumePopupSet(false);
                search_active   = false;
                return true;
            }
        }
    }

    // Search pill
    {
        int sb_x = tb_search_x(), sb_w = tb_search_w();
        if(mx>=sb_x && mx<sb_x+sb_w && my>=y_pos+7 && my<y_pos+TASKBAR_HEIGHT-7){
            search_active = true;
            search_cursor_t0_ms = Timer::GetRealMs();
            StartMenuSet(false);
            VolumePopupSet(false);
            return true;
        }
    }

    // Task buttons - centered, layout matches RenderTaskButtons.
    {
        int win_ids[WM_MAX_WINDOWS];
        int wcount = 0;
        Window* wlist = WindowManager::GetWindows();
        for(int i=0;i<WM_MAX_WINDOWS && wcount<WM_MAX_WINDOWS;i++){
            Window* w = &wlist[i];
            if(w->state!=WIN_CLOSED){
                win_ids[wcount++] = w->id;
            }
        }
        int bw=40, gap2=4;
        int total_w = wcount*bw + (wcount>1?(wcount-1)*gap2:0);
        int center = tb_bar_x() + tb_bar_w() / 2;
        int x = center - total_w/2;
        int min_x = tb_search_x() + tb_search_w() + 12;
        int max_x = tb_cluster_left() - 8;
        if(x < min_x) x = min_x;

        for(int i=0;i<wcount;i++){
            Window* w = WindowManager::GetWindow(win_ids[i]);
            if(!w || w->state==WIN_CLOSED) continue;
            if(x + bw > max_x) break;
            if(mx>=x && mx<x+bw){
                // minimized -> restore (the old direct state write left
                // visible=false, so the window never came back); focused ->
                // minimize (standard taskbar toggle); else raise + focus
                // (BringToFront alone never moved keyboard focus). (satoru)
                if(w->state==WIN_MINIMIZED || w->anim_kind==3){
                    WindowManager::Unminimize(w->id);
                    WindowManager::Focus(w->id);
                    WindowManager::BringToFront(w->id);
                } else if(WindowManager::GetFocusedIndex() == w->id){
                    WindowManager::Minimize(w->id);
                } else {
                    WindowManager::Focus(w->id);
                    WindowManager::BringToFront(w->id);
                }
                return true;
            }
            x += bw + gap2;
        }
    }

    // Right cluster: bell, network, volume, battery, clock.
    {
        // Volume icon - opens dedicated volume popup
        int vol_x3 = tb_vol_x();
        if(mx>=vol_x3 && mx<vol_x3+TB_VOL_W && my>=y_pos){
            bool was_open = volume_popup_open;
            VolumePopupSet(!was_open);
            StartMenuSet(false);
            if(volume_popup_open && Audio::IsAvailable())
                volume_slider_val = Audio::GetMasterVolume();
            return true;
        }

        // Anything else in the cluster (bell / network / battery / clock)
        // opens the Control Center.
        int cluster_left  = tb_bell_x() - 4;
        int cluster_right = tb_bar_x() + tb_bar_w();
        if(mx >= cluster_left && mx < cluster_right && my >= y_pos){
            ControlCenter::ToggleAt(mx, y_pos);
            VolumePopupSet(false);
            StartMenuSet(false);
            search_active     = false;
            return true;
        }
    }

    StartMenuSet(false);
    VolumePopupSet(false);
    search_active=false;
    return true;
}

void Taskbar::Update(){
    // refresh the clock from the real rtc-backed time at most once per second
    // instead of every render frame; renderclock() reads the cached fields. (satoru)
    uint32_t now = Timer::GetRealMs();
    if (clock_last_update_ms != 0 && (now - clock_last_update_ms) < 1000) return;
    clock_last_update_ms = now;
    Battery::Tick();   // (satoru)
    if (Battery::Present()) battery_pct = Battery::Percent();
    DateTime dt = TimeManager::NowLocalDateTime();
    clock_h = dt.h;
    clock_m = dt.m;
    clock_mon = dt.mon;
    clock_dom = dt.dom;
    clock_year = dt.year;
    clock_dow = dt.dow;
}

void Taskbar::Tick(uint32_t delta_ms, int mx, int my){
    if (delta_ms == 0) delta_ms = 1;
    if (delta_ms > 100) delta_ms = 100;   // ignore huge stalls
    if (UIStyle::Modern()) TickModern(delta_ms, mx, my);

    // ── Start menu phase (220 ms open, 160 ms close) ──────────────────
    {
        uint32_t now = Timer::GetRealMs();
        uint32_t dur = start_menu_anim_opening ? 220u : 160u;
        uint32_t e   = now - start_menu_anim_start_ms;
        float raw    = (e >= dur) ? 1.0f : (float)e / (float)dur;
        start_menu_phase = start_menu_anim_opening ? raw : (1.0f - raw);
    }
    // ── Volume popup phase (180 ms / 140 ms) ──────────────────────────
    {
        uint32_t now = Timer::GetRealMs();
        uint32_t dur = volume_popup_anim_opening ? 180u : 140u;
        uint32_t e   = now - volume_popup_anim_start_ms;
        float raw    = (e >= dur) ? 1.0f : (float)e / (float)dur;
        volume_popup_phase = volume_popup_anim_opening ? raw : (1.0f - raw);
    }

    // ── Pinned-icon hover phase - ease toward 1 if hovered, 0 if not ─
    int strip_x = tb_pin_strip_x();
    int strip_y = tb_bar_y() + (TASKBAR_HEIGHT - TB_PIN_SZ) / 2;
    int hover = -1;
    if (my >= strip_y - 4 && my < strip_y + TB_PIN_SZ + 4) {
        for (int i = 0; i < TB_PINNED_COUNT; i++) {
            int ix = strip_x + i * (TB_PIN_SZ + TB_PIN_GAP);
            if (mx >= ix - 2 && mx < ix + TB_PIN_SZ + 2) { hover = i; break; }
        }
    }
    pinned_hover_target = hover;
    // rate: ~1.0 / 140ms toward target -> approximately 7px per frame
    // at 60fps.  Ease-out cubic curve applied at render time.
    for (int i = 0; i < TB_PINNED_COUNT && i < 8; i++) {
        float target = (i == hover) ? 1.0f : 0.0f;
        pinned_hover_phase[i] = approach(pinned_hover_phase[i], target,
                                        1.0f / 140.0f, delta_ms);
    }
}

bool Taskbar::IsAnimating(){
    // dock magnification glides in and out.
    // "animating" means a value is still travelling to its target, not that it
    // is non-zero: a pointer resting on the dock or a pinned icon used to keep
    // the gui loop rendering at the full refresh rate forever (60 fps idle at
    // 60 hz, 240 at 240). (satoru)
    if (UIStyle::Modern())
        for (int i = 0; i < 24; i++) {
            float d = dock_scale[i] - g_dock_target[i];
            if (d > 0.003f || d < -0.003f) return true;
        }
    // open popups animate; closing ones keep a decaying phase until ~0.
    if (start_menu_open   || start_menu_phase   > 0.01f) return true;
    if (volume_popup_open || volume_popup_phase > 0.01f) return true;
    // hover-lift on pinned icons eases out after the pointer leaves.
    for (int i = 0; i < TB_PINNED_COUNT && i < 8; i++) {
        float t = (i == pinned_hover_target) ? 1.0f : 0.0f;
        float d = pinned_hover_phase[i] - t;
        if (d > 0.01f || d < -0.01f) return true;
    }
    // a launch bounce is in flight until ~Motion::Window after the tap. (satoru)
    uint32_t now = Timer::GetRealMs();
    for (int i = 0; i < TB_PINNED_COUNT && i < 8; i++)
        if (pinned_launch_ms[i] && (now - pinned_launch_ms[i]) < (UIStyle::Modern() ? 900u : (uint32_t)KSS::Motion::Window)) return true;   // the dock's two-hop bounce (satoru)
    // blinking search cursor needs periodic repaint while typing.
    if (search_active) return true;
    return false;
}

//  desktop (icons + wallpaper)
int           Desktop::screen_width    = 0;
int           Desktop::screen_height   = 0;
unsigned int  Desktop::wallpaper_color = COL_DESK_BG;
DesktopIcon   Desktop::icons[DESKTOP_MAX_ICONS];
int           Desktop::icon_count      = 0;
int           Desktop::selected_icon   = -1;
bool          Desktop::context_menu_open = false;
int           Desktop::context_menu_x  = 0;
int           Desktop::context_menu_y  = 0;
uint32_t      Desktop::context_menu_open_ms = 0;
int           Desktop::context_menu_target = -1;
int           Desktop::new_folder_counter  = 1;
int           Desktop::new_file_counter    = 1;
bool          Desktop::icon_dragging       = false;
bool          Desktop::icon_drag_moved     = false;
int           Desktop::drag_icon           = -1;
int           Desktop::drag_offset_x       = 0;
int           Desktop::drag_offset_y       = 0;
int           Desktop::drag_start_x        = 0;
int           Desktop::drag_start_y        = 0;
uint32_t      Desktop::last_file_sync_ms   = 0;
uint32_t*     Desktop::gradient_cache  = nullptr;
int           Desktop::gradient_cache_h = 0;
size_t        Desktop::gradient_cache_bytes = 0;
bool          Desktop::have_image_wallpaper = false;
uint8_t*      Desktop::wallpaper_src       = nullptr;
// a builtin wallpaper already lives in rodata for the whole boot; point the
// scaler straight at it instead of copying 2 mb into the pmm. (satoru)
static bool   g_wallpaper_src_borrowed     = false;
int           Desktop::wallpaper_src_w     = 0;
int           Desktop::wallpaper_src_h     = 0;
int           Desktop::wallpaper_src_order = 0;
size_t        Desktop::wallpaper_src_bytes = 0;

int      Desktop::cfg_icon_size     = ICON_SIZE;
int      Desktop::cfg_spacing_x    = ICON_SPACING_X;
int      Desktop::cfg_spacing_y    = ICON_SPACING_Y;
int      Desktop::cfg_margin_x     = ICON_MARGIN_X;
int      Desktop::cfg_margin_y     = ICON_MARGIN_Y;
uint32_t Desktop::cfg_col_desk_bg  = COL_DESK_BG;
uint32_t Desktop::cfg_col_icon_text= COL_ICON_TEXT;
uint32_t Desktop::cfg_col_icon_sel = COL_ICON_SEL;
uint32_t Desktop::cfg_col_ctx_bg   = 0xFF121228;
uint32_t Desktop::cfg_col_ctx_border = 0xFF5C8AFF;
uint32_t Desktop::cfg_col_ctx_text = COL_ICON_TEXT;
int      Desktop::cfg_ctx_item_h   = 30;
int      Desktop::cfg_ctx_width    = 180;
bool     Desktop::cfg_allow_edit   = true;

float    Desktop::icon_hover_phase[DESKTOP_MAX_ICONS] = {0};
int      Desktop::icon_hover_target  = -1;
uint32_t Desktop::icon_anim_last_ms  = 0;
uint32_t Desktop::last_render_ms     = 0;
int      Desktop::hovered_icon       = -1;

void Desktop::ReloadFromConfig(){
    cfg_icon_size    = UIConfig::Int  ("desktop.icon_size",       ICON_SIZE);
    cfg_spacing_x    = UIConfig::Int  ("desktop.icon_spacing_x",  ICON_SPACING_X);
    cfg_spacing_y    = UIConfig::Int  ("desktop.icon_spacing_y",  ICON_SPACING_Y);
    cfg_margin_x     = UIConfig::Int  ("desktop.icon_margin_x",   ICON_MARGIN_X);
    cfg_margin_y     = UIConfig::Int  ("desktop.icon_margin_y",   ICON_MARGIN_Y);
    cfg_col_desk_bg  = UIConfig::Color("desktop.bg",              COL_DESK_BG);
    cfg_col_icon_text= UIConfig::Color("desktop.icon_text",       COL_ICON_TEXT);
    cfg_col_icon_sel = UIConfig::Color("desktop.icon_selected",   COL_ICON_SEL);
    cfg_col_ctx_bg   = UIConfig::Color("ctxmenu.bg",              0xFF121228);
    cfg_col_ctx_border = UIConfig::Color("ctxmenu.border",        0xFF5C8AFF);
    cfg_col_ctx_text = UIConfig::Color("ctxmenu.text",            COL_ICON_TEXT);
    cfg_ctx_item_h   = UIConfig::Int  ("ctxmenu.item_h",          30);
    cfg_ctx_width    = UIConfig::Int  ("ctxmenu.width",            180);
    cfg_allow_edit   = UIConfig::Bool ("desktop.allow_edit",       true);
    ArrangeIcons();
}

void Desktop::Init(int sw,int sh){
    screen_width=sw; screen_height=sh;
    wallpaper_color=COL_DESK_BG;
    icon_count=0; selected_icon=-1;
    context_menu_open=false;
    context_menu_target=-1;
    icon_hover_target = -1;
    hovered_icon = -1;
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) icon_hover_phase[i] = 0.0f;

    ReloadFromConfig();

    // ensure desktop directory exists in kvfs for user files
    KVFS::Mkdirs("/home/user/Desktop");

    // default desktop icons
    AddIcon("Terminal",    "/usr/bin/terminal",      2);
    AddIcon("Files",       "/usr/bin/filebrowser",   2);
    AddIcon("Calculator",  "/usr/bin/calc",          2);
    AddIcon("Editor",      "/usr/bin/editor",        2);
    AddIcon("Settings",    "/usr/bin/settings",      2);
    AddIcon("Task Manager","/usr/bin/tasks",         3);
    AddIcon("Browser",     "/usr/bin/browser",       2);
    AddIcon("Home",        "/home/user",             1);
    AddIcon("Media",       "/usr/bin/mediaplayer",   2);
    if (!InstallerGUI::IsInstalled() && InstallerService::PayloadAvailable())
        AddIcon("Install Kurono", "/usr/bin/installer", 2);

    ArrangeIcons();
    RefreshFiles();
}

void Desktop::AddIcon(const char* name,const char* path,int tp){
    if(icon_count>=DESKTOP_MAX_ICONS)return;
    DesktopIcon* ic=&icons[icon_count];
    icon_hover_phase[icon_count] = 0.0f;
    icon_count++;
    scpy(ic->name,name,32);
    scpy(ic->path,path,DESKTOP_ICON_PATH_MAX);
    ic->icon_type=tp;
    ic->selected=false;
    ic->x=0; ic->y=0;
}

int Desktop::FindIconByPath(const char* path){
    for(int icon_index=0;icon_index<icon_count;icon_index++){
        if(seq(icons[icon_index].path,path)) return icon_index;
    }
    return -1;
}

bool Desktop::IsDesktopFileIcon(const DesktopIcon* icon){
    if(!icon) return false;
    if(icon->icon_type!=0 && icon->icon_type!=1) return false;
    return starts_with(icon->path,"/home/user/Desktop/");
}

void Desktop::ClampIcon(int index){
    if(index<0 || index>=icon_count) return;
    int icon_sz = cfg_icon_size;
    int min_x = 4;
    int min_y = 4 + Taskbar::TopInset();
    int max_x = screen_width - icon_sz - 8;
    int max_y = Taskbar::GetY() - icon_sz - 26;
    if(max_x < min_x) max_x = min_x;
    if(max_y < min_y) max_y = screen_height - icon_sz - 8;
    if(icons[index].x < min_x) icons[index].x = min_x;
    if(icons[index].x > max_x) icons[index].x = max_x;
    if(icons[index].y < min_y) icons[index].y = min_y;
    if(icons[index].y > max_y) icons[index].y = max_y;
}

void Desktop::PlaceIcon(int index){
    if(index<0 || index>=icon_count) return;
    int desktop_bottom = Taskbar::GetY() - 20;
    if(desktop_bottom <= 0) desktop_bottom = screen_height - Taskbar::GetHeight();
    for(int slot_x=cfg_margin_x;slot_x<screen_width-cfg_icon_size;slot_x+=cfg_spacing_x){
        for(int slot_y=cfg_margin_y+Taskbar::TopInset();slot_y<desktop_bottom-cfg_icon_size;slot_y+=cfg_spacing_y){
            bool occupied=false;
            for(int other_index=0;other_index<icon_count;other_index++){
                if(other_index==index) continue;
                int delta_x = icons[other_index].x - slot_x;
                int delta_y = icons[other_index].y - slot_y;
                if(delta_x<0) delta_x=-delta_x;
                if(delta_y<0) delta_y=-delta_y;
                if(delta_x < cfg_spacing_x/2 && delta_y < cfg_spacing_y/2){ occupied=true; break; }
            }
            if(!occupied){
                icons[index].x = slot_x;
                icons[index].y = slot_y;
                ClampIcon(index);
                return;
            }
        }
    }
    ClampIcon(index);
}

void Desktop::AddOrUpdateDesktopFile(KVFSNode* node){
    if(!node || !node->name[0] || node->name[0]=='.') return;
    char path[DESKTOP_ICON_PATH_MAX];
    scpy(path,"/home/user/Desktop/",DESKTOP_ICON_PATH_MAX);
    sapp(path,node->name,DESKTOP_ICON_PATH_MAX);
    int existing = FindIconByPath(path);
    int icon_type = node->is_dir() ? 1 : 0;
    if(existing>=0){
        scpy(icons[existing].name,node->name,32);
        icons[existing].icon_type = icon_type;
        ClampIcon(existing);
        return;
    }
    if(icon_count>=DESKTOP_MAX_ICONS) return;
    AddIcon(node->name,path,icon_type);
    PlaceIcon(icon_count-1);
}

void Desktop::RefreshFiles(){
    KVFS::Mkdirs("/home/user/Desktop");
    // the icon set changes here without any damage (only the ui dirty flag):
    // under the damage scissor the added or dropped icon never reached the
    // screen. a signature of the set before and after, a full repaint when it
    // differs. (satoru)
    auto icon_sig = [&]() {
        uint32_t h = 2166136261u + (uint32_t)icon_count;
        for (int i = 0; i < icon_count; i++) {
            h = (h ^ (uint32_t)icons[i].x) * 16777619u;
            h = (h ^ (uint32_t)icons[i].y) * 16777619u;
            for (const char* c = icons[i].name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        }
        return h;
    };
    const uint32_t sig_before = icon_sig();

    for(int icon_index=0;icon_index<icon_count;){
        if(IsDesktopFileIcon(&icons[icon_index]) && !KVFS::Exists(icons[icon_index].path)){
            for(int shift_index=icon_index;shift_index<icon_count-1;shift_index++){
                icons[shift_index]=icons[shift_index+1];
                icon_hover_phase[shift_index] = icon_hover_phase[shift_index+1];
            }
            icon_count--;
            if (icon_count >= 0 && icon_count < DESKTOP_MAX_ICONS)
                icon_hover_phase[icon_count] = 0.0f;
            if(selected_icon==icon_index) selected_icon=-1;
            else if(selected_icon>icon_index) selected_icon--;
            if(drag_icon==icon_index) drag_icon=-1;
            else if(drag_icon>icon_index) drag_icon--;
            if(icon_hover_target==icon_index) icon_hover_target=-1;
            else if(icon_hover_target>icon_index) icon_hover_target--;
            if(hovered_icon==icon_index) hovered_icon=-1;
            else if(hovered_icon>icon_index) hovered_icon--;
            continue;
        }
        icon_index++;
    }

    KVFSNode* desktop_dir = KVFS::ResolvePath("/home/user/Desktop");
    if(desktop_dir && desktop_dir->is_dir()){
        for(int child_index=0;child_index<desktop_dir->child_count;child_index++){
            AddOrUpdateDesktopFile(desktop_dir->children[child_index]);
        }
    }
    SyncFirefoxIcon();
    last_file_sync_ms = Timer::GetRealMs();
    if (Graphics::ScissorEnabled() && icon_sig() != sig_before) Graphics::MarkFullScreenDirty();   // (satoru)
}

void Desktop::SyncFirefoxIcon(){
    // present when firefox is installed in the live tree OR a persisted
    // install sits on the nvme disk (the lazy /apps restore means the live
    // tree is empty on a restored boot until first launch), unless the user
    // deleted the icon. the persisted answer is the boot-seeded CACHE only -
    // this runs on the gui process, which must never touch KFS/nvme (the
    // shell process may be mid-restore on the same volume). (satoru)
    bool user_wants = UIConfig::Int("desktop.firefox_icon", 1) != 0;
    bool installed  = FirefoxLauncher::IsInstalled();
    bool present = installed || PersistStore::HasPersistedFirefoxCached();
    int idx = FindIconByPath("/apps/firefox/firefox");
    if(present && user_wants && idx < 0 && icon_count < DESKTOP_MAX_ICONS){
        AddIcon("Firefox", "/apps/firefox/firefox", 2);
        PlaceIcon(icon_count - 1);
        Graphics::MarkUIDirty();
    } else if((!present || !user_wants) && idx >= 0){
        DropIconAt(idx);
        Graphics::MarkUIDirty();
    }
}

void Desktop::ArrangeIcons(){
    int gx = cfg_margin_x;
    int gy = cfg_margin_y + Taskbar::TopInset();
    int tb_h = Taskbar::cfg_height;
    int col_h = screen_height - tb_h - cfg_margin_y - 40;
    for(int i=0;i<icon_count;i++){
        icons[i].x = gx;
        icons[i].y = gy;
        gy += cfg_spacing_y;
        if(gy > col_h){
            gy = cfg_margin_y + Taskbar::TopInset();
            gx += cfg_spacing_x;
        }
    }
}

void Desktop::DropIconAt(int index){
    if(index<0 || index>=icon_count) return;
    // shift remaining icons (and their hover phases) down one slot.
    for(int i=index;i<icon_count-1;i++){
        icons[i]=icons[i+1];
        icon_hover_phase[i] = icon_hover_phase[i+1];
    }
    icon_count--;
    if (icon_count >= 0 && icon_count < DESKTOP_MAX_ICONS)
        icon_hover_phase[icon_count] = 0.0f;
    if(selected_icon==index) selected_icon=-1;
    else if(selected_icon>index) selected_icon--;
    if(drag_icon==index) drag_icon=-1;
    else if(drag_icon>index) drag_icon--;
    if(icon_hover_target==index) icon_hover_target=-1;
    else if(icon_hover_target>index) icon_hover_target--;
    if(hovered_icon==index) hovered_icon=-1;
    else if(hovered_icon>index) hovered_icon--;
}

void Desktop::RemoveIcon(int index){
    if(index<0 || index>=icon_count) return;
    if(IsDesktopFileIcon(&icons[index])){
        const char* p = icons[index].path;
        if(KVFS::IsDir(p)){
            KVFS::Rmdir(p);
        } else if(KVFS::IsFile(p)){
            KVFS::Unlink(p);
        }
    }
    // deleting the firefox app icon is a persisted user choice - it stays
    // gone across boots until a fresh install re-enables it. (satoru)
    if(icons[index].icon_type==2 && starts_with(icons[index].path,"/apps/firefox/"))
        UIConfig::SetInt("desktop.firefox_icon", 0, true);
    DropIconAt(index);
    RefreshFiles();
    FileManagerApp::NotifyFilesystemChanged("/home/user/Desktop");
}

bool Desktop::CreateFolderInteractive(){
    if(!cfg_allow_edit || icon_count>=DESKTOP_MAX_ICONS) return false;
    // build name "new folder", "new folder 2", etc.
    char name[32]; char path[64];
    if(new_folder_counter==1){
        scpy(name,"New Folder",32);
    } else {
        scpy(name,"New Folder ",32);
        char nb[8]; int_to_str(new_folder_counter,nb,8);
        sapp(name,nb,32);
    }
    scpy(path,"/home/user/Desktop/",64); sapp(path,name,64);
    new_folder_counter++;
    KVFS::Mkdirs(path);
    RefreshFiles();
    FileManagerApp::NotifyFilesystemChanged("/home/user/Desktop");
    return true;
}

bool Desktop::CreateFileInteractive(){
    if(!cfg_allow_edit || icon_count>=DESKTOP_MAX_ICONS) return false;
    char name[32]; char path[64];
    if(new_file_counter==1){
        scpy(name,"New File.txt",32);
    } else {
        scpy(name,"New File ",32);
        char nb[8]; int_to_str(new_file_counter,nb,8);
        sapp(name,nb,32);
        sapp(name,".txt",32);
    }
    scpy(path,"/home/user/Desktop/",64); sapp(path,name,64);
    new_file_counter++;
    KVFS::WriteString(path, "");
    RefreshFiles();
    FileManagerApp::NotifyFilesystemChanged("/home/user/Desktop");
    return true;
}

void Desktop::SetWallpaper(unsigned int c){
    wallpaper_color=c;
    have_image_wallpaper = false;
    if (gradient_cache) {
        TaskMem::UnchargeFrames(gradient_cache); PMM::FreeBytes(gradient_cache, gradient_cache_bytes);
        gradient_cache = nullptr;
        gradient_cache_bytes = 0;
        gradient_cache_h = 0;
    }
    Graphics::MarkUIDirty();   // wallpaper swap is async (Settings); must repaint
}

// (re)build the full-screen wallpaper cache at w x h from the retained source
// image using nearest-neighbor scaling. shared by SetWallpaperImage (initial)
// and RenderWallpaper (on a mode change) so an image wallpaper survives
// resolution switches instead of dropping to the procedural gradient. (satoru)
bool Desktop::ScaleWallpaperCache(int w, int h){
    if (!wallpaper_src || wallpaper_src_w <= 0 || wallpaper_src_h <= 0) return false;
    if (w <= 0 || h <= 0) return false;

    if (gradient_cache) { TaskMem::UnchargeFrames(gradient_cache); PMM::FreeBytes(gradient_cache, gradient_cache_bytes); gradient_cache = nullptr; }
    size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    gradient_cache = (uint32_t*)PMM::AllocBytes(bytes); TaskMem::ChargeFrames(gradient_cache, bytes);
    if (!gradient_cache) { gradient_cache_bytes = 0; gradient_cache_h = 0; have_image_wallpaper = false; return false; }
    gradient_cache_bytes = bytes;
    gradient_cache_h = h;
    have_image_wallpaper = true;

    for (int y = 0; y < h; y++) {
        int src_y = (y * wallpaper_src_h) / h;
        if (src_y >= wallpaper_src_h) src_y = wallpaper_src_h - 1;
        for (int x = 0; x < w; x++) {
            int src_x = (x * wallpaper_src_w) / w;
            if (src_x >= wallpaper_src_w) src_x = wallpaper_src_w - 1;
            uint8_t* p = wallpaper_src + (size_t)(src_y * wallpaper_src_w + src_x) * 4;
            uint8_t r, g, b;
            if (wallpaper_src_order == 1) { // bgra
                b = p[0]; g = p[1]; r = p[2];
            } else { // rgba
                r = p[0]; g = p[1]; b = p[2];
            }
            gradient_cache[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    return true;
}

static uint32_t g_wp_gen = 1;   // bumped when the wallpaper pixels change (secondary outputs repaint) (satoru)

void Desktop::SetWallpaperImage(const MediaDecoder::Image& img){
    g_wp_gen++;
    if (!img.valid || !img.data || img.width <= 0 || img.height <= 0) {
        have_image_wallpaper = false;
        if (wallpaper_src) {
            if (!g_wallpaper_src_borrowed) {
                TaskMem::UnchargeFrames(wallpaper_src); PMM::FreeBytes(wallpaper_src, wallpaper_src_bytes);
            }
            wallpaper_src = nullptr; wallpaper_src_bytes = 0;
            wallpaper_src_w = 0; wallpaper_src_h = 0;
            g_wallpaper_src_borrowed = false;
        }
        if (gradient_cache) {
            TaskMem::UnchargeFrames(gradient_cache); PMM::FreeBytes(gradient_cache, gradient_cache_bytes);
            gradient_cache = nullptr;
            gradient_cache_bytes = 0;
            gradient_cache_h = 0;
        }
        Graphics::MarkUIDirty();   // cleared image wallpaper -> repaint
        return;
    }

    // retain a copy of the source pixels so the wallpaper can be re-scaled on a
    // later resolution change (the display backend may finalize a different mode
    // after boot, e.g. virtio-gpu coming up at 1080p). (satoru)
    size_t src_bytes = (size_t)img.width * (size_t)img.height * 4u;
    if (wallpaper_src) {
        if (!g_wallpaper_src_borrowed) {
            TaskMem::UnchargeFrames(wallpaper_src); PMM::FreeBytes(wallpaper_src, wallpaper_src_bytes);
        }
        wallpaper_src = nullptr;
    }
    if (!img.owns) {
        // embedded builtin: the rodata never goes away, use it in place. (satoru)
        wallpaper_src = (uint8_t*)img.data;
        g_wallpaper_src_borrowed = true;
    } else {
        wallpaper_src = (uint8_t*)PMM::AllocBytes(src_bytes); TaskMem::ChargeFrames(wallpaper_src, src_bytes);
        if (!wallpaper_src) { wallpaper_src_bytes = 0; have_image_wallpaper = false; return; }
        memcpy(wallpaper_src, img.data, src_bytes);
        g_wallpaper_src_borrowed = false;
    }
    wallpaper_src_bytes = src_bytes;
    wallpaper_src_w     = img.width;
    wallpaper_src_h     = img.height;
    wallpaper_src_order = img.order;

    // Floating taskbar leaves a strip of wallpaper visible at the bottom and
    // behind the bar's rounded corners - render full screen height.
    if (screen_width <= 0 || screen_height <= 0) return;
    ScaleWallpaperCache(screen_width, screen_height);
    Graphics::MarkUIDirty();   // new wallpaper image ready -> repaint
}

// decode + apply one of the embedded builtin wallpapers (0 = primary, 1 =
// secondary). used by the settings personalization page to switch the wallpaper
// live, and at boot to honour the persisted desktop.wallpaper_index. (satoru)
const uint8_t* Desktop::BuiltinWallpaperRGBA(int idx, int* w, int* h){
    if (idx == 1) { if (w) *w = (int)wallpaper2_rgba_w; if (h) *h = (int)wallpaper2_rgba_h; return wallpaper2_rgba_data; }
    if (w) *w = (int)wallpaper_rgba_w;
    if (h) *h = (int)wallpaper_rgba_h;
    return wallpaper_rgba_data;
}

bool Desktop::ApplyBuiltinWallpaper(int idx){
    // the builtin wallpapers are embedded as pre-decoded raw rgba (decoded on the
    // host) so we never run the freestanding image decoder on them -- it corrupts
    // a band of rows on large images. just point an Image at the rodata. (satoru)
    MediaDecoder::Image img = {};
    if (idx == 1) {
        img.width  = (int)wallpaper2_rgba_w;
        img.height = (int)wallpaper2_rgba_h;
        img.data   = (uint8_t*)wallpaper2_rgba_data;
    } else {
        img.width  = (int)wallpaper_rgba_w;
        img.height = (int)wallpaper_rgba_h;
        img.data   = (uint8_t*)wallpaper_rgba_data;
    }
    img.valid = true;
    img.order = 0;   // rgba
    img.owns  = false;
    SetWallpaperImage(img);
    return true;
}

// multi-monitor: every output except the primary shows its own copy of the
// wallpaper (nearest-scaled when its size differs from the primary's cache).
// with wallpaper damage-only the cache is blitted only under the frame's
// damaged rects; full is forced when the cache, wallpaper or output set
// changed. an idle secondary screen then costs no blit and no present at all.
// (satoru)
static bool g_wp_painted = false;
static uint32_t g_wp_painted_gen = 0;
static uint32_t g_wp_painted_color = 0;
static bool g_wp_painted_img = false;
static const uint32_t* g_wp_painted_cache = nullptr;
static int g_wp_painted_w = 0, g_wp_painted_h = 0;

static bool desk_wallpaper_needs_full(int w, int h, uint32_t color, bool img, const uint32_t* cache) {
    bool changed = !g_wp_painted || g_wp_painted_w != w || g_wp_painted_h != h ||
                   g_wp_painted_gen != g_wp_gen || g_wp_painted_color != color ||
                   g_wp_painted_img != img || g_wp_painted_cache != cache;
    // a damage scissor pass paints only its region: a changed cache asks the
    // compositor for one full pass (which then paints everything). (satoru)
    int sx, sy, sw, sh;
    if (Graphics::GetScissor(sx, sy, sw, sh)) {
        bool whole = sx == 0 && sy == 0 && sw >= w && sh >= h;
        if (changed && !whole) Graphics::MarkFullScreenDirty();
        return false;
    }
    if (!Graphics::WallpaperDamageOnly()) return true;
    return changed;
}

static void desk_wallpaper_mark_painted(int w, int h, uint32_t color, bool img, const uint32_t* cache) {
    g_wp_painted = true;
    g_wp_painted_w = w;
    g_wp_painted_h = h;
    g_wp_painted_gen = g_wp_gen;
    g_wp_painted_color = color;
    g_wp_painted_img = img;
    g_wp_painted_cache = cache;
}

// the rects the wallpaper repaints: the frame's damage, or exactly the region
// of the damage scissor pass that is running (kurono.gfx.scissor=1), which is
// repainted from the wallpaper up. (satoru)
static int desk_damage_rects(Graphics::Rect* rr, int max) {
    int sx, sy, sw, sh;
    if (Graphics::GetScissor(sx, sy, sw, sh)) {
        if (sw <= 0 || sh <= 0 || max < 1) return 0;
        rr[0].x = sx; rr[0].y = sy; rr[0].w = sw; rr[0].h = sh;
        return 1;
    }
    return Graphics::GetDirtyRects(rr, max);
}

// blit the cached wallpaper into the render target: the whole screen after a
// cache/wallpaper change (which also marks full damage), otherwise only the
// rows and columns the frame actually damaged. an untouched wallpaper costs
// no memory traffic at all. (satoru)
static void desk_wallpaper_primary(const uint32_t* cache, int w, int h, bool full) {
    if (!cache || w <= 0 || h <= 0) return;
    uint8_t* buf = Graphics::GetBackBuffer();
    if (!buf) return;
    uint32_t pitch = Graphics::GetPitch();
    if (full) {
        size_t row_bytes = (size_t)w * 4;
        for (int y = 0; y < h; y++)
            memcpy(buf + (size_t)y * pitch, cache + (size_t)y * w, row_bytes);
        Graphics::MarkDirty(0, 0, w, h);
        return;
    }
    Graphics::Rect rr[32];
    int n = desk_damage_rects(rr, 32);
    for (int i = 0; i < n; i++) {
        int x0 = rr[i].x, y0 = rr[i].y;
        int x1 = x0 + rr[i].w, y1 = y0 + rr[i].h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        if (x1 <= x0 || y1 <= y0) continue;
        size_t off = (size_t)x0 * 4;
        size_t len = (size_t)(x1 - x0) * 4;
        for (int y = y0; y < y1; y++)
            memcpy(buf + (size_t)y * pitch + off, cache + (size_t)y * w + x0, len);
    }
}

static void desk_wallpaper_secondary(const uint32_t* cache, int cw, int ch, uint32_t color, bool full) {
    if (!cache || !Outputs::Multi() || cw <= 0 || ch <= 0) return;
    uint8_t* buf = Graphics::GetBackBuffer();
    if (!buf) return;
    uint32_t pitch = Graphics::GetPitch();
    static uint32_t last_sig = 0;
    uint32_t sig = g_wp_gen * 2654435761u ^ (uint32_t)(uintptr_t)cache ^ color ^
                   ((uint32_t)cw * 131u + (uint32_t)ch * 17u);
    for (int i = 1; i < Outputs::Total(); i++) {
        const Output* o = Outputs::Get(i);
        if (!o) continue;
        sig = sig * 31u + (uint32_t)(o->connected ? (o->x + 7 * o->y + 13 * o->w + 17 * o->h) : 0);
    }
    bool force = full || !Graphics::WallpaperDamageOnly() || sig != last_sig;
    // a scissor pass repaints only its region; a changed wallpaper or output
    // set asks the compositor for a full pass instead. (satoru)
    {
        int sx, sy, sw, sh;
        if (Graphics::GetScissor(sx, sy, sw, sh)) {
            if (full || sig != last_sig) Graphics::MarkFullScreenDirty();
            force = false;
        }
    }
    last_sig = sig;
    if (!force) {
        Graphics::Rect rr[32];
        int n = desk_damage_rects(rr, 32);
        if (!n) return;
        for (int i = 1; i < Outputs::Total(); i++) {
            const Output* o = Outputs::Get(i);
            if (!o || !o->connected || o->w <= 0 || o->h <= 0) continue;
            for (int r = 0; r < n; r++) {
                int x0 = rr[r].x > o->x ? rr[r].x : o->x;
                int y0 = rr[r].y > o->y ? rr[r].y : o->y;
                int x1 = rr[r].x + rr[r].w < o->x + o->w ? rr[r].x + rr[r].w : o->x + o->w;
                int y1 = rr[r].y + rr[r].h < o->y + o->h ? rr[r].y + rr[r].h : o->y + o->h;
                if (x1 <= x0 || y1 <= y0) continue;
                // same nearest mapping as before, src x = (x - o->x) * cw / o->w,
                // but stepped with a running remainder instead of a 64-bit
                // divide per pixel (the laptop panel is the secondary output,
                // so every damaged pixel there paid one), and a straight row
                // copy when the widths match. pixel for pixel identical. (satoru)
                const int dx0 = x0 - o->x;
                const long long num0 = (long long)dx0 * cw;
                for (int y = y0; y < y1; y++) {
                    int sy = (int)(((long long)(y - o->y) * ch) / o->h);
                    const uint32_t* src = cache + (size_t)sy * cw;
                    uint32_t* dst = (uint32_t*)(buf + (size_t)y * pitch) + x0;
                    if (o->w == cw) {
                        memcpy(dst, src + dx0, (size_t)(x1 - x0) * 4u);
                        continue;
                    }
                    int sx = (int)(num0 / o->w);
                    int rem = (int)(num0 % o->w);
                    for (int x = x0; x < x1; x++) {
                        dst[x - x0] = src[sx];
                        rem += cw;
                        while (rem >= o->w) { rem -= o->w; sx++; }
                    }
                }
            }
        }
        return;
    }
    for (int i = 1; i < Outputs::Total(); i++) {
        const Output* o = Outputs::Get(i);
        if (!o || !o->connected || o->w <= 0 || o->h <= 0) continue;
        for (int y = 0; y < o->h; y++) {
            uint32_t* d = (uint32_t*)(buf + (size_t)(o->y + y) * pitch) + o->x;
            int sy = (int)(((long long)y * ch) / o->h);
            const uint32_t* src = cache + (size_t)sy * cw;
            if (o->w == cw) { memcpy(d, src, (size_t)cw * 4); continue; }
            // stepped nearest mapping, identical to x * cw / o->w (satoru)
            int sx = 0, rem = 0;
            for (int x = 0; x < o->w; x++) {
                d[x] = src[sx];
                rem += cw;
                while (rem >= o->w) { rem -= o->w; sx++; }
            }
        }
        Graphics::MarkDirty(o->x, o->y, o->w, o->h);
    }
}

void Desktop::RenderWallpaper(){
    int h = screen_height;
    if (h <= 0) return;
    int w = screen_width;
    size_t need_bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    // If the screen dimensions changed since the cache was built (mode switch),
    // rebuild it at the new size. when an image wallpaper is set we re-scale it
    // from the retained source so it survives the resolution change; otherwise
    // we drop the stale cache and let the procedural gradient regenerate. (satoru)
    if (gradient_cache && (gradient_cache_h != h || gradient_cache_bytes != need_bytes)) {
        if (wallpaper_src) {
            ScaleWallpaperCache(w, h);
        } else {
            TaskMem::UnchargeFrames(gradient_cache); PMM::FreeBytes(gradient_cache, gradient_cache_bytes);
            gradient_cache = nullptr;
            gradient_cache_bytes = 0;
            gradient_cache_h = 0;
            have_image_wallpaper = false;
        }
    }

    // the software cursor is painted outside RenderWallpaper, so make sure the
    // wallpaper repaints under wherever it was last drawn. without this a
    // damage-only pass can leave stale arrows behind on real hardware. (satoru)
    Mouse::DamageLastSoftware();
    // a moving/animated window marks its old footprint while it renders, which
    // is after this wallpaper pass: damage the last painted rects here too so
    // the old pixels are repainted in the frame they moved. (satoru)
    {
        int sx_, sy_, sw_, sh_;
        // a damage scissor pass collected these before its first pass. (satoru)
        if (Graphics::WallpaperDamageOnly() && !Graphics::GetScissor(sx_, sy_, sw_, sh_))
            WindowManager::CollectLastPaintedDamage();
    }
    // arm the scanned-vs-back crc check once the damage-only path is really
    // exercised, so a missed damage case disables it instead of corrupting the
    // screen. skipped while zero-copy owns the present path. (satoru)
    static bool s_wp_check_armed = false;
    if (Graphics::WallpaperDamageOnly() && !Graphics::ZeroCopyActive() && !s_wp_check_armed &&
        !Graphics::ScissorEnabled()) {   // the scissor arms its own check (satoru)
        s_wp_check_armed = true;
        Graphics::ArmSelfCheck(Graphics::SELFCHECK_WPDAMAGE, 24);
    }

    // if we have an image wallpaper, skip procedural generation
    if (have_image_wallpaper && gradient_cache && gradient_cache_h == h) {
        // fast blit image wallpaper - row-wise memcpy for maximum throughput
        bool full = desk_wallpaper_needs_full(w, h, wallpaper_color, have_image_wallpaper, gradient_cache);
        desk_wallpaper_primary(gradient_cache, w, h, full);
        desk_wallpaper_secondary(gradient_cache, w, h, wallpaper_color, full);
        desk_wallpaper_mark_painted(w, h, wallpaper_color, have_image_wallpaper, gradient_cache);
        return;
    }

    // build gradient cache once - warm modern abstract gradient (fallback)
    if (!gradient_cache || gradient_cache_h != h) {
        // allocate full screen cache (w * h pixels)
        int total = w * h;
        if (!gradient_cache || gradient_cache_h != h) {
            if (gradient_cache) { TaskMem::UnchargeFrames(gradient_cache); PMM::FreeBytes(gradient_cache, gradient_cache_bytes); gradient_cache = nullptr; }
            size_t bytes = (size_t)total * sizeof(uint32_t);
            gradient_cache = (uint32_t*)PMM::AllocBytes(bytes); TaskMem::ChargeFrames(gradient_cache, bytes);
            gradient_cache_bytes = bytes;
        }
        gradient_cache_h = h;
        if (!gradient_cache) return;

        if (wallpaper_color != COL_DESK_BG) {
            for (int i = 0; i < total; i++) {
                gradient_cache[i] = wallpaper_color;
            }
        } else {

            for (int y = 0; y < h; y++) {
                float ty = (float)y / (float)(h > 1 ? h - 1 : 1);
                for (int x = 0; x < w; x++) {
                    float tx = (float)x / (float)(w > 1 ? w - 1 : 1);

                    // base: deep midnight blue-purple
                    float r = 8.0f, g = 5.0f, b = 22.0f;

                    // warm orb (upper-right) - amber/orange glow
                    float dx1 = tx - 0.78f;
                    float dy1 = ty - 0.18f;
                    float d1 = dx1*dx1 + dy1*dy1;
                    float orb1 = 1.0f / (1.0f + d1 * 6.0f);
                    r += 200.0f * orb1;
                    g += 110.0f * orb1;
                    b += 25.0f * orb1;

                    // cool orb (lower-left) - deep blue glow
                    float dx2 = tx - 0.15f;
                    float dy2 = ty - 0.82f;
                    float d2 = dx2*dx2 + dy2*dy2;
                    float orb2 = 1.0f / (1.0f + d2 * 8.0f);
                    r += 40.0f * orb2;
                    g += 30.0f * orb2;
                    b += 140.0f * orb2;

                    // accent orb (center) - soft magenta/pink
                    float dx3 = tx - 0.5f;
                    float dy3 = ty - 0.42f;
                    float d3 = dx3*dx3 + dy3*dy3;
                    float orb3 = 1.0f / (1.0f + d3 * 12.0f);
                    r += 90.0f * orb3;
                    g += 25.0f * orb3;
                    b += 70.0f * orb3;

                    // aurora ribbon (upper-center) - teal/cyan sweep
                    float dx4 = tx - 0.4f;
                    float dy4 = ty - 0.1f;
                    float d4 = dx4*dx4*0.5f + dy4*dy4*4.0f;
                    float orb4 = 1.0f / (1.0f + d4 * 20.0f);
                    r += 10.0f * orb4;
                    g += 80.0f * orb4;
                    b += 90.0f * orb4;

                    // subtle warm accent (lower-right)
                    float dx5 = tx - 0.85f;
                    float dy5 = ty - 0.7f;
                    float d5 = dx5*dx5 + dy5*dy5;
                    float orb5 = 1.0f / (1.0f + d5 * 16.0f);
                    r += 60.0f * orb5;
                    g += 20.0f * orb5;
                    b += 50.0f * orb5;

                    // vignette - darken edges for depth
                    float vx = (tx - 0.5f) * 2.0f;
                    float vy = (ty - 0.5f) * 2.0f;
                    float vignette = 1.0f - (vx*vx + vy*vy) * 0.15f;
                    if (vignette < 0.6f) vignette = 0.6f;
                    r *= vignette;
                    g *= vignette;
                    b *= vignette;

                    // subtle dither noise (deterministic based on position)
                    int noise = ((x * 7 + y * 13) & 7) - 4;
                    r += (float)noise * 0.5f;
                    g += (float)noise * 0.4f;
                    b += (float)noise * 0.5f;

                    // clamp
                    if (r > 255.0f) r = 255.0f; if (r < 0.0f) r = 0.0f;
                    if (g > 255.0f) g = 255.0f; if (g < 0.0f) g = 0.0f;
                    if (b > 255.0f) b = 255.0f; if (b < 0.0f) b = 0.0f;

                    gradient_cache[y * w + x] = 0xFF000000u |
                        ((uint32_t)(uint8_t)r << 16) |
                        ((uint32_t)(uint8_t)g << 8) |
                        (uint32_t)(uint8_t)b;
                }
            }
        }
    }

    // fast blit: row-wise memcpy from cache to back buffer
    // mark full desktop area dirty so swapbuffers copies it to the framebuffer
    bool full = desk_wallpaper_needs_full(w, h, wallpaper_color, have_image_wallpaper, gradient_cache);
    desk_wallpaper_primary(gradient_cache, w, h, full);
    desk_wallpaper_secondary(gradient_cache, w, h, wallpaper_color, full);
    desk_wallpaper_mark_painted(w, h, wallpaper_color, have_image_wallpaper, gradient_cache);
}

static void guard_section(const char* what);
void Desktop::RenderIcon(DesktopIcon* ic, float hover_t){
    int ix=ic->x, iy=ic->y;
    int icon_sz = cfg_icon_size;

    // Hover lift - eased pop, max 3 px lift.  Drawn before the icon body
    // by offsetting iy so the label below stays put.
    int lift = (int)(hover_t * 3.0f + 0.5f);
    iy -= lift;

    // modern: gradient app tile and a smooth label with a soft shadow instead
    // of the boxed pixel-font caption. (satoru)
    if (UIStyle::Modern()) {
        int gid = ic->icon_type == 1 ? AppIcons::FILES
                : ic->icon_type == 0 ? AppIcons::EDITOR
                : AppIcons::IdForName(ic->name);
        int label_y = ic->y + icon_sz + 6;
        int lw = FontTTF::MeasureText(14.0f, ic->name);
        int span = lw > icon_sz + 20 ? icon_sz + 20 : lw;
        guard_section("icon-start");
        if (ic->selected)
            MDraw::RoundRect(ix - 8, iy - 6, icon_sz + 16, icon_sz + 32, 10, 0xFFFFFFFF, 46);
        guard_section("icon-plate");
        MDraw::AppTile(gid, ix, iy, icon_sz);
        guard_section("icon-tile");
        char lbl[24]; scpy(lbl, ic->name, 20);
        int tx = ix + icon_sz / 2 - span / 2;
        if (ic->selected)
            MDraw::RoundRect(tx - 5, label_y - 1, span + 10, 19, 5, 0xFF2F64E8, 230);
        Graphics::PushClipRect(tx - 2, label_y - 2, span + 4, 22);
        FontTTF::DrawText(tx + 1, label_y + 1, 14.0f, lbl, 0xFF000000);
        FontTTF::DrawText(tx, label_y, 14.0f, lbl, 0xFFF4F5FA);
        Graphics::PopClipRect();
        return;
    }

    // selection highlight - glowing aura
    if(ic->selected){
        Graphics::FillRoundedRect(ix-6, iy-6, icon_sz+12, icon_sz+26, 8, cfg_col_icon_sel);
    }

    // determine icon colors from name
    unsigned int ic_top, ic_bot;
    const char* nm = ic->name;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') {
        ic_top=0xFF34D058; ic_bot=0xFF1E8C3A;
    } else if(nm[0]=='F' && nm[1]=='i' && nm[2]=='r') {
        // firefox: dark night-sky tile so the fox mark pops. (satoru)
        ic_top=0xFF1E1852; ic_bot=0xFF0E0A30;
    } else if(nm[0]=='F' && nm[1]=='i') {
        ic_top=0xFFFFA726; ic_bot=0xFFE08A1E;
    } else if(nm[0]=='C' && nm[1]=='o') {
        ic_top=0xFF5C8AFF; ic_bot=0xFF3557C9;
    } else if(nm[0]=='C') {
        ic_top=0xFF42A5F5; ic_bot=0xFF1E7BC8;
    } else if(nm[0]=='E') {
        ic_top=0xFFAB47BC; ic_bot=0xFF7B2D8E;
    } else if(nm[0]=='S') {
        ic_top=0xFF90A4AE; ic_bot=0xFF607D8B;
    } else if(nm[0]=='H') {
        ic_top=0xFFFFA726; ic_bot=0xFFE08A1E;
    } else if(nm[0]=='B') {
        ic_top=0xFF3498DB; ic_bot=0xFF2176AE;
    } else if(nm[0]=='M') {
        ic_top=0xFFE91E63; ic_bot=0xFFBE164F;
    } else {
        ic_top=0xFF42A5F5; ic_bot=0xFF1E7BC8;
    }

    // drop shadow
    Graphics::FillRoundedRect(ix+3, iy+4, icon_sz, icon_sz, 14, 0xFF060610);

    // icon body - rounded square with gradient effect
    Graphics::FillRoundedRect(ix, iy, icon_sz, icon_sz, 13, ic_top);
    // bottom gradient half
    Graphics::FillRoundedRect(ix, iy + icon_sz/2, icon_sz, icon_sz/2, 13, ic_bot);
    // smooth blend zone
    Graphics::FillRect(ix + 4, iy + icon_sz/2, icon_sz - 8, 6, ic_top);

    // inner highlight (top glossy shine - lighter version of top color)
    // skip: these used alpha blending and are expensive, icon already looks good

    // flat vector glyph centered on the tile (replaces the old per-name symbol
    // block). desktop file/folder entries carry arbitrary filenames, so map by
    // icon_type first (0=file -> document, 1=folder -> files); app/system
    // entries (2/3) map by name. the glyph follows the lifted tile y. (satoru)
    int gid;
    if(ic->icon_type==1)       gid = AppIcons::FILES;
    else if(ic->icon_type==0)  gid = AppIcons::EDITOR;
    else                       gid = AppIcons::IdForName(nm);
    int gpad = (icon_sz * 5 + 14) / 28;           // ~18% inset so glyph fills ~64% (satoru)
    AppIcons::Draw(gid, ix + gpad, iy + gpad, icon_sz - gpad*2);

    // label - centered below the un-lifted icon position so it stays put.
    char lbl[14]; scpy(lbl, ic->name, 13);
    int tw=slen(lbl)*8;
    int tx=ix + icon_sz/2 - tw/2;
    if(tx<ix-8)tx=ix-8;
    int label_y = ic->y + icon_sz + 1;     // anchor to original y (no lift)
    Graphics::FillRoundedRect(tx-4, label_y, tw+8, 14, 4, 0xFF080812);
    Graphics::DrawString(tx, label_y+2, lbl, cfg_col_icon_text, 0xFF000000);
}

// context menu item lists - icon-targeted vs empty-desktop.
// Ordered so trimming with nit=2 yields a sensible (Open / Refresh) menu
// when editing is disabled.  The "Delete" row stays last.
static const char* ctx_items_icon[]  = { "Open", "Refresh", "Delete" };
static const int   ctx_icon_count    = 3;
static const char* ctx_items_empty[] = { "New Folder", "New File", "Refresh", "Settings" };
static const int   ctx_empty_count   = 4;

void Desktop::RenderContextMenu(){
    if(!context_menu_open) return;
    bool on_icon = (context_menu_target >= 0 && context_menu_target < icon_count);
    const char** items = on_icon ? ctx_items_icon : ctx_items_empty;
    int nit = on_icon ? ctx_icon_count : ctx_empty_count;

    // hide "delete" when editing disabled
    if(on_icon && !cfg_allow_edit) nit = 2; // only open, refresh

    int mw = cfg_ctx_width;
    int ih = cfg_ctx_item_h;
    int mh = nit * ih + 12;

    // fade+scale-from-origin: the menu unfolds downward from its top-left anchor
    // (the click point) while a dim overlay fades out. driven off elapsed ms so it
    // looks right at any frame rate; eased ease-out-cubic to match the desktop's
    // standard reveal feel. (satoru)
    uint32_t e = Timer::GetRealMs() - context_menu_open_ms;
    float t = (e >= KSS::Motion::Window) ? 1.0f
                                         : Animation::EaseMs(e, KSS::Motion::Window, Animation::EaseOutCubic);
    if (UIStyle::Modern()) {
        // the shared shell menu: glass panel, accent hover pill, "Delete" in red (satoru)
        const char* mi[8];
        for (int i = 0; i < nit && i < 8; i++) mi[i] = items[i];
        if (on_icon && nit > 2) mi[2] = "!Delete";
        int hv = -1;
        if (Mouse::mx >= context_menu_x && Mouse::mx < context_menu_x + mw && Mouse::my >= context_menu_y + 6) {
            hv = (Mouse::my - context_menu_y - 6) / ih;
            if (hv >= nit) hv = -1;
        }
        MDraw::Menu(context_menu_x, context_menu_y, mw, 6, ih, mi, nit, hv, t);
        Graphics::MarkDirty(context_menu_x - 30, context_menu_y - 30, mw + 60, mh + 70);
        return;
    }
    int draw_h = (int)(mh * (0.62f + 0.38f * t) + 0.5f);   // grow 62% -> 100% height
    if (draw_h < 8) draw_h = 8;

    // shadow
    Graphics::FillRoundedRect(context_menu_x+5, context_menu_y+5, mw, draw_h, 8, 0xFF060610);
    // background
    Graphics::FillRoundedRect(context_menu_x, context_menu_y, mw, draw_h, 8, cfg_col_ctx_bg);
    // accent border
    Graphics::DrawRect(context_menu_x, context_menu_y, mw, draw_h, cfg_col_ctx_border);

    // clip item rows to the growing body so they reveal as the menu unfolds. (satoru)
    Graphics::PushClipRect(context_menu_x, context_menu_y, mw, draw_h);
    int iy = context_menu_y + 6;
    for(int i=0;i<nit;i++){
        if(i > 0){
            Graphics::FillRect(context_menu_x+12, iy-1, mw-24, 1, 0xFF1A1A30);
        }
        uint32_t txt_col = cfg_col_ctx_text;
        // tint "Delete" red - it's the last icon-menu row.
        if(on_icon && i==2) txt_col = 0xFFFF6666;
        Graphics::DrawString(context_menu_x+16, iy + (ih-14)/2, items[i], txt_col, 0xFF000000);
        iy += ih;
    }
    Graphics::PopClipRect();
    // fade overlay over the menu footprint, fully clear by t=1. (satoru)
    if (t < 1.0f) {
        uint8_t dim = (uint8_t)((1.0f - t) * 150.0f);
        if (dim > 0) Graphics::FillRectAlpha(context_menu_x, context_menu_y, mw, draw_h, dim, 0xFF000000u);
    }
    Graphics::MarkDirty(context_menu_x-2, context_menu_y-2, mw+10, mh+10);
}

void Desktop::Render(){
    RenderWallpaper();
    guard_section("wallpaper");
    for(int i=0;i<icon_count;i++){
        float ht = (i < DESKTOP_MAX_ICONS) ? ease_out_cubic(icon_hover_phase[i]) : 0.0f;
        RenderIcon(&icons[i], ht);
    }
    RenderContextMenu();
    last_render_ms = Timer::GetRealMs();
}

// an icon's whole footprint: the selection plate, the lifted tile and the label
// pill, which is wider than the tile for a long name. a selection or hover change
// marked nothing, so under kurono.gfx.scissor=1 the old plate stayed on screen. (satoru)
void Desktop::DamageIcon(int i){
    if(i<0 || i>=icon_count) return;
    int sz=cfg_icon_size;
    const DesktopIcon& ic=icons[i];
    Graphics::MarkDirty(ic.x + sz/2 - 70, ic.y - 14, 140, sz + 50);
}

void Desktop::Tick(uint32_t delta_ms, int mx, int my){
    if (delta_ms == 0) delta_ms = 1;
    if (delta_ms > 100) delta_ms = 100;

    int hover = IconAt(mx, my);
    if (icon_dragging) hover = -1;            // no hover-pop while dragging
    icon_hover_target = hover;
    hovered_icon = hover;

    // Per-icon: ease toward 1 if hovered, 0 otherwise (160 ms full).
    for (int i = 0; i < icon_count && i < DESKTOP_MAX_ICONS; i++) {
        float target = (i == hover) ? 1.0f : 0.0f;
        float before = icon_hover_phase[i];
        icon_hover_phase[i] = approach(icon_hover_phase[i], target,
                                      1.0f / 160.0f, delta_ms);
        if (icon_hover_phase[i] != before) DamageIcon(i);   // (satoru)
    }
}

bool Desktop::IsAnimating(){
    // hover-pop lift eases in/out over ~160ms; keep rendering until settled
    // so the lift never freezes mid-transition once the pointer comes to
    // rest on (or just off) an icon. mirrors Taskbar::IsAnimating(). (satoru)
    // settled at the target (fully lifted under a resting pointer, or fully
    // down) is not animating; only a phase still in transit is. (satoru)
    for (int i = 0; i < icon_count && i < DESKTOP_MAX_ICONS; i++) {
        float t = (i == icon_hover_target) ? 1.0f : 0.0f;
        float d = icon_hover_phase[i] - t;
        if (d > 0.01f || d < -0.01f) return true;
    }
    // context menu fade+scale-in needs continuous frames until it settles. (satoru)
    if (context_menu_open && (Timer::GetRealMs() - context_menu_open_ms) < KSS::Motion::Window)
        return true;
    return false;
}

int Desktop::IconAt(int mx,int my){
    int sz = cfg_icon_size;
    for(int i=0;i<icon_count;i++){
        if(mx>=icons[i].x-8 && mx<icons[i].x+sz+8 &&
           my>=icons[i].y-8 && my<icons[i].y+sz+22){
            return i;
        }
    }
    return -1;
}

bool Desktop::HandleClick(int mx,int my){
    // ignore clicks that land on the taskbar's own row; the desktop owns the
    // rest of the screen. with a top dock the desktop is *below* the bar, so the
    // old `my >= GetY()` guard rejected nearly everything. (satoru)
    if(Taskbar::cfg_position_top){
        if(my < Taskbar::GetY() + TASKBAR_HEIGHT) return false;
    } else {
        if(my >= Taskbar::GetY()) return false;
    }

    // if context menu is open, check for menu item click first
    if(context_menu_open){
        bool on_icon = (context_menu_target >= 0 && context_menu_target < icon_count);
        int nit = on_icon ? ctx_icon_count : ctx_empty_count;
        if(on_icon && !cfg_allow_edit) nit = 2;
        int mw = cfg_ctx_width;
        int ih = cfg_ctx_item_h;
        int mh = nit * ih + 12;

        if(mx >= context_menu_x && mx < context_menu_x + mw &&
           my >= context_menu_y && my < context_menu_y + mh){
            // which row?
            int row = (my - context_menu_y - 6) / ih;
            if(row < 0) row = 0;
            if(row >= nit) row = nit - 1;
            context_menu_open = false;

            if(on_icon){
                // 0=open, 1=refresh, 2=delete
                if(row==0){ HandleDoubleClick(icons[context_menu_target].x+4, icons[context_menu_target].y+4); }
                else if(row==1){ RefreshFiles(); }
                else if(row==2 && cfg_allow_edit){ RemoveIcon(context_menu_target); }
            } else {
                // 0=new folder, 1=new file, 2=refresh, 3=settings
                if(row==0 && cfg_allow_edit){ CreateFolderInteractive(); }
                else if(row==1 && cfg_allow_edit){ CreateFileInteractive(); }
                else if(row==2){ RefreshFiles(); }
                else if(row==3){ DesktopEnvironment::LaunchSettings(); }
            }
            return true;
        }
        // click outside menu - close it
        context_menu_open = false;
    }

    // deselect all
    for(int i=0;i<icon_count;i++){ if(icons[i].selected) DamageIcon(i); icons[i].selected=false; }   // (satoru)
    selected_icon=-1;

    int idx=IconAt(mx,my);
    if(idx>=0){
        icons[idx].selected=true;
        DamageIcon(idx);   // (satoru)
        selected_icon=idx;
        icon_dragging=true;
        icon_drag_moved=false;
        drag_icon=idx;
        drag_offset_x=mx-icons[idx].x;
        drag_offset_y=my-icons[idx].y;
        drag_start_x=mx;
        drag_start_y=my;
        return true;
    }
    icon_dragging=false;
    drag_icon=-1;
    return false;
}

void Desktop::HandleDoubleClick(int mx,int my){
    int idx=IconAt(mx,my);
    if(idx<0) return;
    icon_dragging=false;
    icon_drag_moved=false;
    drag_icon=-1;

    if(icons[idx].icon_type==0){
        // ring 3 when kurono.r3apps asks for texteditor (the path is its argv[1]), else the same (satoru)
        // in-kernel editor through the launcher choice. (satoru)
        KuiKernel::OpenArg("texteditor", icons[idx].path);
        return;
    }
    if(icons[idx].icon_type==1){
        // ring 3 when files is enabled, else the same source in the kernel. (satoru)
        KuiKernel::OpenArg("files", icons[idx].path);
        return;
    }

    // launch app based on icon name (prefix matching)
    const char* nm = icons[idx].name;
    // firefox before the broad 'F' files test. (satoru)
    if(nm[0]=='I' && nm[1]=='n' && nm[2]=='s') DesktopEnvironment::LaunchInstaller();
    else if(nm[0]=='F' && nm[1]=='i' && nm[2]=='r') DesktopEnvironment::LaunchFirefox();
    else if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') DesktopEnvironment::LaunchTerminal();
    else if(nm[0]=='T' && nm[1]=='a') DesktopEnvironment::LaunchTaskManager();
    else if(nm[0]=='T' && nm[1]=='e' && nm[2]=='x') DesktopEnvironment::LaunchTextEditor();
    else if(nm[0]=='F') DesktopEnvironment::LaunchFileBrowser();
    else if(nm[0]=='C') DesktopEnvironment::LaunchCalculator();
    else if(nm[0]=='E') DesktopEnvironment::LaunchTextEditor();
    else if(nm[0]=='S') DesktopEnvironment::LaunchSettings();
    else if(nm[0]=='H') DesktopEnvironment::LaunchFileBrowser();
    else if(nm[0]=='B') DesktopEnvironment::LaunchBrowser();
    else if(nm[0]=='M') DesktopEnvironment::LaunchMediaPlayer();
}

void Desktop::HandleRightClick(int mx,int my){
    context_menu_target = IconAt(mx, my);
    bool on_icon = (context_menu_target >= 0 && context_menu_target < icon_count);
    int nit = on_icon ? ctx_icon_count : ctx_empty_count;
    if(on_icon && !cfg_allow_edit) nit = 2;
    int mw = cfg_ctx_width;
    int mh = nit * cfg_ctx_item_h + 12;

    // clamp so the whole menu stays on screen (and above the taskbar).
    int max_x = screen_width - mw - 4;
    int max_y = Taskbar::GetY() - mh - 4;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (mx > max_x) mx = max_x;
    if (my > max_y) my = max_y;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;

    context_menu_x = mx;
    context_menu_y = my;
    context_menu_open = true;
    context_menu_open_ms = Timer::GetRealMs();   // start the fade+scale-in (satoru)
}

void Desktop::Update(int mx,int my,bool mouse_down,bool clicked){
    (void)clicked;
    if(icon_dragging && drag_icon>=0 && drag_icon<icon_count){
        if(mouse_down){
            int delta_x = mx - drag_start_x;
            int delta_y = my - drag_start_y;
            if(delta_x<0) delta_x=-delta_x;
            if(delta_y<0) delta_y=-delta_y;
            if(delta_x>3 || delta_y>3) icon_drag_moved=true;
            if(icon_drag_moved){
                DamageIcon(drag_icon);   // the old place; the scissor repaints only damage (satoru)
                icons[drag_icon].x = mx - drag_offset_x;
                icons[drag_icon].y = my - drag_offset_y;
                ClampIcon(drag_icon);
                DamageIcon(drag_icon);
            }
            return;
        }
        DamageIcon(drag_icon);
        ClampIcon(drag_icon);
        DamageIcon(drag_icon);
        icon_dragging=false;
        icon_drag_moved=false;
        drag_icon=-1;
    }

    uint32_t now = Timer::GetRealMs();
    if(now - last_file_sync_ms > 500){
        RefreshFiles();
    }
}

// set by a whole-scene render while the damage scissor is enabled (satoru)
static bool g_scis_need_full = true;

//  desktopenvironment
void DesktopEnvironment::Init(int sw,int sh){
    UIStyle::Apply();
    // multi-monitor: the window manager spans the whole virtual desktop but the
    // menu bar, dock, desktop icons and control center belong to the primary
    // output only. one screen: psw/psh == sw/sh, nothing changes. (satoru)
    int psw = sw, psh = sh;
    if (Outputs::Multi()) {
        const Output* po = Outputs::Get(0);
        if (po && po->connected && po->w > 0 && po->h > 0) { psw = po->w; psh = po->h; }
    }
    WindowManager::Init(sw, sh);
    Desktop::Init(psw, psh);
    Taskbar::Init(psw, psh);
    ControlCenter::Init(psw, psh);
    // adjust wm desktop area to exclude taskbar
    Taskbar::ApplyDesktopArea();
}

void DesktopEnvironment::Relayout(int sw, int sh, const int* old_x, const int* old_y, int swap_a, int swap_b){
    int psw = sw, psh = sh;
    if (Outputs::Multi()) {
        const Output* po = Outputs::Get(0);
        if (po && po->connected && po->w > 0 && po->h > 0) { psw = po->w; psh = po->h; }
    }
    WindowManager::SetScreenSize(sw, sh);
    Desktop::Init(psw, psh);
    if (GUI::wallpaper.valid) Desktop::SetWallpaperImage(GUI::wallpaper);
    Taskbar::Init(psw, psh);
    ControlCenter::Init(psw, psh);
    Taskbar::ApplyDesktopArea();
    if (old_x && old_y) WindowManager::RelocateForOutputs(old_x, old_y, swap_a, swap_b);
    WindowManager::ClampAllToDesktop();
    Graphics::MarkUIDirty();
}

// true when the scene behind a dragged window is "clean" enough that a cached
// backdrop is safe to use: nothing that animates or overlays the window is
// active. when any of these hold we fall back to a normal full render, so the
// backdrop can never show stale/animating content. correctness over speed.
// (satoru)
static bool de_drag_backdrop_safe(){
    if (WindowManager::HasActiveAnimations())       return false; // open/close/min/restore
    if (ControlCenter::IsOpen() || ControlCenter::IsAnimating()) return false;
    if (NotificationManager::ActiveCount() > 0)     return false; // a toast is visible
    if (Taskbar::IsAnimating())                     return false; // start menu / tray anim
    if (Taskbar::volume_popup_open)                 return false; // volume slider popup
    if (Desktop::IsAnimating())                     return false; // icon hover-pop behind win
    if (WindowManager::IsContextMenuOpen())         return false; // wm titlebar menu
    return true;
}

// compose the full desktop once: wallpaper + icons, every window, taskbar,
// control center, toasts. shared by the normal path and the drag-backdrop
// capture (which first arms the wm to omit the dragged window). (satoru)
// a settled full screen window in front owns the whole output: no menu bar,
// no dock (super+f or the window menu brings them back). (satoru)
static bool de_fullscreen_front(){
    Window* fw = WindowManager::GetFocusedWindow();
    return fw && fw->state == WIN_FULLSCREEN && fw->fx_mode == 0 && fw->visible &&
           fw->y <= 0 && fw->h >= Graphics::GetHeight();   // only when it covers the output (satoru)
}

// kurono.gfx.scissorguard=1 (debug): after every section of a scissored pass, check that
// no pixel outside the pass box changed and say which section wrote it. (satoru)
static bool g_guard_on = false;
static uint8_t* g_guard_snap = nullptr;
static size_t g_guard_cap = 0;
static int g_gb[4] = {0, 0, 0, 0};
static int g_guard_logs = 0;
static void guard_section(const char* what) {
    if (!g_guard_on || !g_guard_snap) return;
    uint8_t* bb = Graphics::GetBackBuffer();
    if (!bb) return;
    const int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    const uint32_t pitch = Graphics::GetPitch();
    uint64_t n = 0; int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
    for (int y = 0; y < sh; y++) {
        const uint32_t* a = (const uint32_t*)(g_guard_snap + (size_t)y * pitch);
        const uint32_t* b = (const uint32_t*)(bb + (size_t)y * pitch);
        bool row_in = y >= g_gb[1] && y < g_gb[3];
        for (int x = 0; x < sw; x++) {
            if (a[x] == b[x]) continue;
            if (row_in && x >= g_gb[0] && x < g_gb[2]) continue;
            n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
    }
    if (n && g_guard_logs < 24) {
        g_guard_logs++;
        SerialLogger::Log("[guard] section "); SerialLogger::Log(what); SerialLogger::Log(" wrote "); SerialLogger::LogDec((int)n);
        SerialLogger::Log(" px outside the pass box ("); SerialLogger::LogDec(g_gb[0]); SerialLogger::Log(","); SerialLogger::LogDec(g_gb[1]);
        SerialLogger::Log(")-("); SerialLogger::LogDec(g_gb[2]); SerialLogger::Log(","); SerialLogger::LogDec(g_gb[3]);
        SerialLogger::Log("), changed bbox ("); SerialLogger::LogDec(x0); SerialLogger::Log(","); SerialLogger::LogDec(y0);
        SerialLogger::Log(")-("); SerialLogger::LogDec(x1 + 1); SerialLogger::Log(","); SerialLogger::LogDec(y1 + 1); SerialLogger::Log(")\r\n");
    }
    memcpy(g_guard_snap, bb, (size_t)pitch * (size_t)sh);   // later sections are judged on their own (satoru)
}

static void de_render_full(){
    // resize self-heal FIRST, before any drawing: if the host resized the
    // qemu window the bga changed mode but a starved gui res-sync may not
    // have caught it, leaving the compositor blitting at the old pitch (the
    // diagonal-tear garbage). resync here on whatever process is rendering,
    // and relayout the desktop to the new size so this frame is correct. (satoru)
    if (Graphics::SyncToHardwareGeometry()) {
        // the legacy GUI buffers (lockscreen/software cursor) rebuild on first
        // use; the desktop itself draws through Graphics' own back buffer. (satoru)
        DesktopEnvironment::Init(Graphics::GetWidth(), Graphics::GetHeight());
        Graphics::MarkUIDirty();
    }
    Desktop::Render();
    guard_section("desktop");
    WindowManager::RenderAll();
    guard_section("windows");
    // the drag snap target tints the target region over the windows, as the
    // common desktop shells do, so it stays visible under the dragged card. (satoru)
    WindowManager::RenderSnapPreview();
    // pump queued wayland frame callbacks once per composited frame so clients
    // are released to submit their next buffer. (satoru)
    WaylandServer::DispatchPendingFrames();
    if (!de_fullscreen_front()) Taskbar::Render();
    guard_section("taskbar");
    // minimize/restore pour into and out of the taskbar tile, so it draws
    // over the taskbar rather than sliding under it. (satoru)
    WindowManager::RenderMotionOverlay();
    // the alt-tab card deck floats above the dock and the motion overlay. (satoru)
    WindowManager::RenderTabSwitcher();
    ControlCenter::Render();
    // toasts render topmost so they sit above every panel. (satoru)
    NotificationManager::Render();
}

void DesktopEnvironment::Render(){
    // ── cached-desktop snapshot fast path during a titlebar drag ──────
    // while a window is being dragged and the scene behind it is static, we
    // recompose ONLY the moving window each frame over a one-time snapshot of
    // everything else, instead of re-blitting the wallpaper and every window /
    // panel. that is what gives the drag steady frame times. (satoru)
    //
    // DISABLED: on the gtk/virtio-gpu present path this backdrop fast-path blacked
    // out the whole desktop while dragging (the partial backdrop blit + damage left
    // the non-footprint area unrepainted). drag now falls through to a full
    // recompose each frame - correct, just a little more work per frame. re-enable
    // only once the backdrop capture/present is reworked to cover the full scene. (satoru)
    if (false && WindowManager::IsWindowDragActive() && de_drag_backdrop_safe()){
        if (WindowManager::DragBackdropReady()){
            // common case: blit backdrop + redraw just the dragged window.
            WindowManager::RenderDragFast();
            return;
        }
        // first clean drag frame (or after an invalidation): compose one full
        // frame WITHOUT the dragged window, snapshot it as the backdrop, then
        // draw the dragged window on top so this frame is itself complete. the
        // cost equals a normal frame - no regression on the transition. (satoru)
        WindowManager::BeginDragCapture();
        de_render_full();                       // dragged window omitted by the wm
        WindowManager::CaptureDragBackdrop();   // snapshot back buffer -> backdrop
        WindowManager::RenderDraggedWindowOnly();
        return;
    }

    de_render_full();
    // a whole-scene render (boot film, a modal, the shell pump without the
    // scissor) leaves translucent layers blended over themselves outside the
    // damage: the next scissored frame starts from a full repaint. (satoru)
    if (Graphics::ScissorEnabled()) g_scis_need_full = true;
}

// ── damage scissor rendering (kurono.gfx.scissor=1) ─────────────────
// the old frame drew the whole scene and presented only the damage, and the
// wallpaper damage-only pass (kurono.wpdamage=1) repainted the wallpaper only
// there, so translucent layers (menu bar, dock, shadows) were blended again
// over themselves everywhere else: the back buffer drifted away from what was
// presented, and any later copy of those pixels (a full present, the page
// flip's repair copy) showed the drift. here a frame is drawn in passes. a
// round takes the damage not yet repainted this frame, groups it into a few
// boxes (rects whose common box wastes little area are merged) and repaints
// each box from the wallpaper up with every draw confined to it, marking the
// box for the present. damage a round adds outside what is already repainted
// (an animation, a hover) gets the next round; after 3 rounds, or when the
// boxes cover most of the screen, the pass is the whole screen. a frame with
// no damage at all gets a probe pass with an empty box, which draws nothing
// but lets the scene mark what it wants to change. a dragged window is moved
// by copy first (WindowManager::DragMoveByCopy). (satoru)
namespace {
struct DeBox { int x0, y0, x1, y1; };
inline bool de_box_has(const DeBox& b, const Graphics::Rect& r) {
    return r.x >= b.x0 && r.y >= b.y0 && r.x + r.w <= b.x1 && r.y + r.h <= b.y1;
}
inline uint64_t de_area(const DeBox& b) { return (uint64_t)(b.x1 - b.x0) * (uint64_t)(b.y1 - b.y0); }
inline DeBox de_union(const DeBox& a, const DeBox& b) {
    return DeBox{ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                  a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}
constexpr int DE_MAX_BOXES = 4;     // passes per round: each one walks the whole scene (satoru)
constexpr int DE_MAX_DONE = 16;

// group rects into at most DE_MAX_BOXES boxes: merge the pair whose common
// box adds the least area, while that is small or there are too many. (satoru)
int de_cluster(DeBox* bx, int n) {
    for (;;) {
        if (n <= 1) return n;
        int bi = 0, bj = 1;
        uint64_t best = ~0ull;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                uint64_t u = de_area(de_union(bx[i], bx[j]));
                uint64_t s = de_area(bx[i]) + de_area(bx[j]);
                uint64_t waste = u > s ? u - s : 0;
                if (waste < best) { best = waste; bi = i; bj = j; }
            }
        if (n <= DE_MAX_BOXES && best > 16384) return n;
        bx[bi] = de_union(bx[bi], bx[bj]);
        bx[bj] = bx[n - 1];
        n--;
    }
}
}

static uint64_t g_scis_area = 0;     // pixels the last frame's passes repainted (satoru)
static bool g_scis_full = false;
static bool g_scis_copied = false;
void DesktopEnvironment::LastScissorStats(uint64_t* area_px, bool* full) {
    if (area_px) *area_px = g_scis_area;
    if (full) *full = g_scis_full;
}

// the gfx oracle's reference frame: the whole scene drawn by the default
// renderer (the scissor flag is off while it runs), nothing presented. (satoru)
void DesktopEnvironment::RenderReference() {
    bool was = Graphics::ScissorEnabled();
    Graphics::SetScissorEnabled(false);
    bool gg = g_guard_on; g_guard_on = false;
    de_render_full();
    g_guard_on = gg;
    Graphics::SetScissorEnabled(was);
}

void DesktopEnvironment::SetScissorGuard(bool on) { g_guard_on = on; }

int DesktopEnvironment::RenderScissored() {
    const int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    g_scis_area = 0;
    g_scis_full = false;
    // damage the scene would only add while drawing: the vacated software
    // cursor and the old footprint of a moved window. (satoru)
    Mouse::DamageLastSoftware();
    WindowManager::CollectLastPaintedDamage();
    // what overlays drew over the scene last frame (a menu, the control center,
    // a toast, a dock tooltip) is repainted from below this frame: when one closes
    // or moves nothing else marks the place it vacated, and it stayed on screen. (satoru)
    static Graphics::Rect s_self_prev[32];
    static int s_self_prev_n = 0;
    (void)Graphics::TakeScissorSelfDamage(nullptr, 0);
    if (Graphics::ScissorSelfCarry())
    for (int i = 0; i < s_self_prev_n; i++)
        Graphics::MarkDirty(s_self_prev[i].x, s_self_prev[i].y, s_self_prev[i].w, s_self_prev[i].h);
    if (g_scis_need_full) { g_scis_need_full = false; Graphics::MarkFullScreenDirty(); }
    int cx = 0, cy = 0, cw = 0, ch = 0;
    g_scis_copied = WindowManager::DragMoveByCopy(&cx, &cy, &cw, &ch);
    DeBox done[DE_MAX_DONE];
    int ndone = 0, passes = 0, rounds = 0;
    bool probed = false, full = false;
    while (!full) {
        Graphics::Rect rr[32];
        int n = Graphics::GetFrameDamage(rr, 32);
        DeBox bx[32];
        int nb = 0;
        for (int i = 0; i < n; i++) {
            Graphics::Rect r = rr[i];
            if (r.x < 0) { r.w += r.x; r.x = 0; }
            if (r.y < 0) { r.h += r.y; r.y = 0; }
            if (r.x + r.w > sw) r.w = sw - r.x;
            if (r.y + r.h > sh) r.h = sh - r.y;
            if (r.w <= 0 || r.h <= 0) continue;
            bool covered = false;
            for (int k = 0; k < ndone && !covered; k++) covered = de_box_has(done[k], r);
            if (!covered) bx[nb++] = DeBox{ r.x, r.y, r.x + r.w, r.y + r.h };
        }
        if (nb == 0) {
            if (passes > 0 || probed) break;
            probed = true;
            passes++;
            Graphics::SetScissor(0, 0, 0, 0);
            de_render_full();
            Graphics::ClearScissor();
            continue;
        }
        if (Graphics::ScissorDebug() && GfxBench::Running()) {
            static int s_raw = 0;
            if (rounds > 0 && s_raw < 48) {
                for (int i = 0; i < nb && i < 6; i++, s_raw++) {
                    SerialLogger::Log("[scissor] round ");
                    SerialLogger::LogDec(rounds);
                    SerialLogger::Log(" new damage (");
                    SerialLogger::LogDec(bx[i].x0); SerialLogger::Log(","); SerialLogger::LogDec(bx[i].y0);
                    SerialLogger::Log(")-(");
                    SerialLogger::LogDec(bx[i].x1); SerialLogger::Log(","); SerialLogger::LogDec(bx[i].y1);
                    SerialLogger::Log(rounds == 1 && g_scis_copied ? ") after a copy\r\n" : ")\r\n");
                }
            }
        }
        nb = de_cluster(bx, nb);
        uint64_t tot = 0;
        for (int i = 0; i < nb; i++) tot += de_area(bx[i]);
        full = rounds >= 3 || ndone + nb > DE_MAX_DONE || tot * 5 >= (uint64_t)sw * (uint64_t)sh * 3;
        if (full) { nb = 1; bx[0] = DeBox{ 0, 0, sw, sh }; Graphics::MarkFullScreenDirty(); }
        if (rounds > 0 && Graphics::ScissorDebug() && GfxBench::Running()) {
            static int s_dbg = 0;
            for (int i = 0; i < nb && s_dbg < 64; i++, s_dbg++) {
                SerialLogger::Log("[scissor] round ");
                SerialLogger::LogDec(rounds);
                SerialLogger::Log(" box (");
                SerialLogger::LogDec(bx[i].x0); SerialLogger::Log(","); SerialLogger::LogDec(bx[i].y0);
                SerialLogger::Log(")-(");
                SerialLogger::LogDec(bx[i].x1); SerialLogger::Log(","); SerialLogger::LogDec(bx[i].y1);
                SerialLogger::Log(full ? ") full\r\n" : ")\r\n");
            }
        }
        // mark every box of the round first: the boxes of this round count as
        // repainted for the marks the scene makes while drawing them. (satoru)
        if (!full) for (int i = 0; i < nb; i++) Graphics::MarkDirty(bx[i].x0, bx[i].y0, bx[i].x1 - bx[i].x0, bx[i].y1 - bx[i].y0);
        for (int i = 0; i < nb; i++) {
            Graphics::SetScissor(bx[i].x0, bx[i].y0, bx[i].x1 - bx[i].x0, bx[i].y1 - bx[i].y0);
            if (g_guard_on) {
                size_t need = (size_t)Graphics::GetPitch() * (size_t)sh;
                if (g_guard_cap < need) { g_guard_snap = (uint8_t*)PMM::AllocBytes(need); g_guard_cap = g_guard_snap ? need : 0; }
                if (g_guard_snap && Graphics::GetBackBuffer()) memcpy(g_guard_snap, Graphics::GetBackBuffer(), need);
                g_gb[0] = bx[i].x0; g_gb[1] = bx[i].y0; g_gb[2] = bx[i].x1; g_gb[3] = bx[i].y1;
            }
            de_render_full();
            Graphics::ClearScissor();
            passes++;
            g_scis_area += de_area(bx[i]);
            if (ndone < DE_MAX_DONE) done[ndone++] = bx[i];
        }
        rounds++;
    }
    g_scis_full = full;
    s_self_prev_n = Graphics::TakeScissorSelfDamage(s_self_prev, 32);   // (satoru)
    // the moved window's new place goes to the present; it was not repainted. (satoru)
    if (g_scis_copied && !full) Graphics::MarkDirty(cx, cy, cw, ch);
    // kurono.gfx.scissorverify=N: every Nth frame, keep what the passes (and a
    // move by copy) produced, repaint the whole frame from scratch and compare.
    // a difference means the partial frame was wrong, not just unpresented;
    // the box is logged. the full frame is what gets presented. (satoru)
    int every = Graphics::ScissorVerifyEvery();
    if (every > 0 && !full) {
        static uint32_t s_n = 0, s_ok = 0, s_bad = 0;
        static uint8_t* s_ref = nullptr;
        static size_t s_ref_bytes = 0;
        uint8_t* bb = Graphics::GetBackBuffer();
        size_t need = (size_t)Graphics::GetPitch() * (size_t)sh;
        // time driven animations would differ between the two renders, and a
        // resting software arrow stays in the back buffer on purpose (it is
        // drawn over every frame): those are not checked. (satoru)
        bool moving = WindowManager::HasActiveAnimations() || Taskbar::IsAnimating() || Desktop::IsAnimating() ||
                      ControlCenter::IsAnimating() || NotificationManager::ActiveCount() > 0 || KSS::Anim::Active();
        if (bb && !moving && (++s_n % (uint32_t)every) == 0 && Graphics::GetBpp() == 32) {
            if (s_ref_bytes < need) {
                if (s_ref) PMM::FreeBytes(s_ref, s_ref_bytes);
                s_ref = (uint8_t*)PMM::AllocBytes(need);
                s_ref_bytes = s_ref ? need : 0;
            }
            if (s_ref) {
                memcpy(s_ref, bb, need);
                Graphics::MarkFullScreenDirty();
                Graphics::SetScissor(0, 0, sw, sh);
                de_render_full();
                Graphics::ClearScissor();
                uint32_t pitch = Graphics::GetPitch();
                uint32_t nd = 0;
                int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
                int ax = -100, ay = -100;
                if (!Mouse::LastSoftwarePos(ax, ay)) { ax = -100; ay = -100; }
                for (int y = 0; y < sh; y++) {
                    const uint32_t* a = (const uint32_t*)(s_ref + (size_t)y * pitch);
                    const uint32_t* b = (const uint32_t*)(bb + (size_t)y * pitch);
                    for (int x = 0; x < sw; x++) {
                        if (a[x] == b[x]) continue;
                        if (x >= ax - 1 && x < ax + 13 && y >= ay - 1 && y < ay + 17) continue;
                        nd++;
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
                }
                if (nd) s_bad++; else s_ok++;
                if (nd && s_bad <= 24) {
                    SerialLogger::Log("[scissor] verify MISMATCH ");
                    SerialLogger::LogDec((int)nd);
                    SerialLogger::Log(" px in (");
                    SerialLogger::LogDec(x0); SerialLogger::Log(","); SerialLogger::LogDec(y0);
                    SerialLogger::Log(")-(");
                    SerialLogger::LogDec(x1 + 1); SerialLogger::Log(","); SerialLogger::LogDec(y1 + 1);
                    SerialLogger::Log(g_scis_copied ? ") on a move-by-copy frame\r\n" : ")\r\n");
                }
                if (((s_ok + s_bad) % 50) == 0) {
                    SerialLogger::Log("[scissor] verify: ");
                    SerialLogger::LogDec((int)s_ok);
                    SerialLogger::Log(" frames equal to a full repaint, ");
                    SerialLogger::LogDec((int)s_bad);
                    SerialLogger::Log(" differ\r\n");
                }
            }
        }
    }
    return passes;
}

// double-click tracking state
static uint32_t last_click_time_ms = 0;
static int last_click_x = -1, last_click_y = -1;

void DesktopEnvironment::HandleInput(int mx,int my,bool mouse_down,bool clicked,char key){
    // the control center owns the keyboard and printable text while it is open. (satoru)
    if (ControlCenter::HandleChar(key)) key = 0;
    ControlCenter::HandleMouse(mx, my, mouse_down);
    // Delta-time bookkeeping for all UI animations.
    uint32_t now_ms  = Timer::GetRealMs();
    uint32_t delta_ms = (de_last_frame_ms == 0) ? 16 : (now_ms - de_last_frame_ms);
    if (delta_ms > 100) delta_ms = 100;
    de_last_frame_ms = now_ms;

    Desktop::Tick(delta_ms, mx, my);
    Taskbar::Tick(delta_ms, mx, my);

    WindowManager::HandlePointerMove(mx, my);
    // forward pointer motion + left-button transitions to wayland clients;
    // mouse_down is the held-state, so edge-detecting it yields press/release. (satoru)
    WaylandServer::ForwardPointerMotion(mx, my);
    {
        static bool wl_prev_left = false;
        if (mouse_down != wl_prev_left) {
            WaylandServer::ForwardPointerButton(mx, my, WaylandServer::WL_BTN_LEFT, mouse_down);
            wl_prev_left = mouse_down;
        }
    }

    // wayland key forwarding lives in InputManager::Poll (event-driven). (satoru)
    // alt-tab: cycle focus through visible windows by id (rough z-order proxy).
    // we want true z-order but the current z_order field changes only on Focus(),
    // so a stable id-cycle gives a consistent round-trip.
    // gather candidate ids - iterate the full slot array since
    // closed slots can sit between live windows.
    // the live switcher now steps these from InputManager::Poll. (satoru)

    NightLight::Tick();   // the night light schedule (satoru)

    // print screen: a png of the screen (shift: the focused window) in
    // ~/Pictures. the key press is recorded by InputManager::Poll. (satoru)
    {
        int shot = Screenshot::TakeRequest();
        if (shot) {
            char path[96];
            path[0] = 0;
            bool ok = Screenshot::CaptureToPictures(shot == 2, path, (int)sizeof(path));
            SerialLogger::Log(ok ? "[screenshot] saved " : "[screenshot] FAILED ");
            SerialLogger::Log(path);
            SerialLogger::Log("\r\n");
            const char* name = path;
            for (const char* q = path; *q; q++) if (*q == '/') name = q + 1;
            if (ok) NotificationManager::Post(shot == 2 ? "Window screenshot saved" : "Screenshot saved",
                                              name, NotificationManager::ICON_SUCCESS, 3000);
            else    NotificationManager::Post("Screenshot", shot == 2 ? "No window to capture" : "Could not save the screenshot",
                                              NotificationManager::ICON_WARNING, 3000);
        }
    }

    // perf hud toggle (f12) + window snapping (super+arrows), press-edge only. (satoru)
    {
        const KeyboardState& kss = Keyboard::GetState();
        static bool f12_was = false;
        bool f12_now = Keyboard::IsKeyPressed(KEY_F12);
        if (f12_now && !f12_was) PerfHUD::Toggle();
        f12_was = f12_now;

        static bool snap_was = false;
        bool snap_l = Keyboard::IsKeyPressed(KEY_LEFT);
        bool snap_r = Keyboard::IsKeyPressed(KEY_RIGHT);
        bool snap_u = Keyboard::IsKeyPressed(KEY_UP);
        bool snap_d = Keyboard::IsKeyPressed(KEY_DOWN);
        bool snap_now = kss.super && (snap_l || snap_r || snap_u || snap_d);
        if (snap_now && !snap_was) {
            if (snap_l)      WindowManager::SnapFocused(0); // left half (satoru)
            else if (snap_r) WindowManager::SnapFocused(1); // right half (satoru)
            else if (snap_u) WindowManager::SnapFocused(2); // maximize (satoru)
            else if (snap_d) WindowManager::SnapFocused(3); // restore (satoru)
        }
        snap_was = snap_now;
        // super+f: full screen toggle for the focused window (satoru)
        static bool fs_was = false;
        bool fs_now = kss.super && Keyboard::IsKeyPressed(KEY_F);
        if (fs_now && !fs_was) {
            Window* fw = WindowManager::GetFocusedWindow();
            if (fw) WindowManager::ToggleFullscreen(fw->id);
        }
        fs_was = fs_now;
    }

    // right-click handling - forward event 4 to focused window
    bool right_clicked = Mouse::RightClicked();
    if(right_clicked){
        // window context menu gets first crack on a titlebar right-click. (satoru)
        if (WindowManager::HandleRightClick(mx, my)) {
            // consumed by the wm context menu - skip default right-click routing.
        } else {
        // check if click is on a window first
        Window* fw = WindowManager::GetFocusedWindow();
        if(fw && fw->visible && fw->state != WIN_CLOSED &&
           mx >= fw->content_x && mx < fw->content_x + fw->content_w &&
           my >= fw->content_y && my < fw->content_y + fw->content_h) {
            if(fw->input){
                { TaskMem::WindowScope _tms(fw->id); fw->input(fw, 4, mx, my); }  // event 4 = right-click
            }
        } else {
            // desktop right-click
            Desktop::HandleRightClick(mx, my);
        }
        }
    }

    if(clicked){
        // an open window context menu eats the next left-click. (satoru)
        if (WindowManager::HandleContextMenuClick(mx, my)) return;
        // double-click detection: two clicks within ~400ms at same spot (real time)
        uint32_t now = Timer::GetRealMs();
        bool is_double = false;
        int dx = mx - last_click_x;
        int dy = my - last_click_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if ((now - last_click_time_ms) < 400 && dx < 10 && dy < 10 && last_click_x >= 0) {
            is_double = true;
        }
        last_click_time_ms = now;
        last_click_x = mx;
        last_click_y = my;

        // control center popup gets first priority while open
        if(ControlCenter::IsOpen()){
            if(ControlCenter::HandleClick(mx, my)) return;
            // click outside the panel was already handled (closed it)
        }

        // taskbar gets next priority
        if(!de_fullscreen_front() && Taskbar::HandleClick(mx,my)) return;
        // then window manager
        if(WindowManager::HandleMouseDown(mx,my)) return;
        // desktop: single click selects/drags, double click opens
        if(Desktop::HandleClick(mx,my)){
            if(is_double) Desktop::HandleDoubleClick(mx,my);
        }
    }
    if(mouse_down){
        // volume slider dragging
        if(Taskbar::volume_popup_open && Taskbar::volume_slider_dragging){
            // use same popup position calculation as rendervolumepopup
            int pop_h = 180;
            int pop_x_unused, pop_y;
            tb_volume_popup_xy(pop_h, 0, pop_x_unused, pop_y);
            int track_y = pop_y + 36;
            int track_h = 110;
            int rel = track_y + track_h - my;
            int vol = (rel * 100) / track_h;
            if(vol < 0) vol = 0;
            if(vol > 100) vol = 100;
            Taskbar::volume_slider_val = vol;
            if(Audio::IsAvailable()){
                Audio::SetMasterVolume(vol);
            }
        }
        WindowManager::HandleMouseMove(mx,my);
    }
    if(!mouse_down){
        Taskbar::volume_slider_dragging = false;
        WindowManager::HandleMouseUp(mx,my);
    }
    Desktop::Update(mx,my,mouse_down,clicked);
    // forward keyboard input to focused window or search
    // super+letter and alt+tab are shell shortcuts: the raw char queue still
    // produces their character, so drop it before an app can take it. (satoru)
    {
        const KeyboardState& kbs = Keyboard::GetState();
        if (key && kbs.super && key >= 32 && key < 127) key = 0;
        if (key && kbs.alt && (key == '\t' || key == 27)) key = 0;
    }
    if(key){
        // if search is active, route keys there
        if(Taskbar::search_active){
            if(key == '\n' || key == '\r'){
                // execute search - launch best matching app (substring match)
                Taskbar::search_active = false;
                const char* sb = Taskbar::search_buf;
                int sblen = slen(sb);
                // app database for matching
                static const char* sr2_names[] = {
                    "Terminal","Files","Calculator","Editor",
                    "Settings","Browser","Media Player","Task Manager"
                };
                int best = -1;
                for(int i=0; i<8 && best<0; i++){
                    int alen2 = slen(sr2_names[i]);
                    for(int s=0; s<=alen2-sblen; s++){
                        bool ok=true;
                        for(int j=0;j<sblen && ok;j++){
                            char a=sr2_names[i][s+j], b=sb[j];
                            if(a>='A'&&a<='Z') a+=32;
                            if(b>='A'&&b<='Z') b+=32;
                            if(a!=b) ok=false;
                        }
                        if(ok){ best=i; break; }
                    }
                }
                if(best>=0){
                    switch(best){
                        case 0: DesktopEnvironment::LaunchTerminal(); break;
                        case 1: DesktopEnvironment::LaunchFileBrowser(); break;
                        case 2: DesktopEnvironment::LaunchCalculator(); break;
                        case 3: DesktopEnvironment::LaunchTextEditor(); break;
                        case 4: DesktopEnvironment::LaunchSettings(); break;
                        case 5: DesktopEnvironment::LaunchBrowser(); break;
                        case 6: DesktopEnvironment::LaunchMediaPlayer(); break;
                        case 7: DesktopEnvironment::LaunchTaskManager(); break;
                    }
                } else {
                    DesktopEnvironment::LaunchTerminal();
                }
                Taskbar::search_buf[0] = 0;
                Taskbar::search_len = 0;
            } else if(key == 27){ // escape
                Taskbar::search_active = false;
                Taskbar::search_buf[0] = 0;
                Taskbar::search_len = 0;
            } else if(key == 8 || key == 127){ // backspace
                if(Taskbar::search_len > 0){
                    Taskbar::search_len--;
                    Taskbar::search_buf[Taskbar::search_len] = 0;
                }
            } else if(key >= 32 && key < 127 && Taskbar::search_len < 60){
                Taskbar::search_buf[Taskbar::search_len++] = key;
                Taskbar::search_buf[Taskbar::search_len] = 0;
            }
        } else {
            Window* fw = WindowManager::GetFocusedWindow();
            if(fw && fw->input){
                { TaskMem::WindowScope _tms(fw->id); fw->input(fw, 2, (int)key, 0); } // event 2 = keypress
            }
        }
    }
}

void DesktopEnvironment::Update(){
    Taskbar::Update();
    KuiKernel::Pump();   // kui apps: timers, frame requests, focus edges, closed windows (satoru)
    TaskManagerApp::DebugKillTick();   // no-op unless kurono.tmkill=1 (satoru)
    // Shell commands are drained by ShellProcess when the preemptive
    // scheduler is running; calling Tick here would still run Execute on
    // the GUI thread and freeze the desktop + mouse.
    if (!Scheduler::IsPreemptiveKernelSchedulerActive()) {
        TerminalApp::Tick();
    }
}

void DesktopEnvironment::ReloadFromConfig(){
    UIConfig::Reload();
    UIStyle::Apply();
    Taskbar::ReloadFromConfig();
    Taskbar::ApplyDesktopArea();
    Desktop::ReloadFromConfig();
    WindowManager::ReloadFromConfig();
    Graphics::MarkUIDirty();          // colors/layout/wallpaper may have changed
}

void DesktopEnvironment::LaunchTerminal(){
    TerminalApp::Open();
}
void DesktopEnvironment::LaunchAlpine(){
    // linux turned off in settings: show the linux page instead (satoru)
    if (!VMHost::Enabled()) { LinuxSettings::OpenPage(); return; }
    AlpineTerminal::Open();   // starts the vm too when it is off (satoru)
}
void DesktopEnvironment::LaunchFileBrowser(){
    // a kui app now: ring 3 (/system/apps/files) when kurono.r3apps asks for it and the ELF is there,
    // else the same source built into the kernel. (satoru)
    KuiKernel::Open("files");
}
void DesktopEnvironment::LaunchCalculator(){
    // a kui app: ring 3 (/system/apps/calculator) when kurono.r3apps / ui.conf apps.ring3 asks for
    // it and a launcher is free, else the same source built into the kernel. (satoru)
    if (KuiKernel::LaunchRing3("calculator")) return;
    TaskMem::AppScope _tms(TaskMem::APP_CALC);   // kurono.taskmem=1: charge to the app (satoru)
    kui_launch_calculator();
}
void DesktopEnvironment::LaunchTextEditor(){
    // ring 3 (/system/apps/texteditor) when wanted, else the in-kernel kui build (satoru)
    KuiKernel::Open("texteditor");
}
void DesktopEnvironment::LaunchSettings(){
    SystemSettings::Open();   // route to the new modular settings ui (satoru)
}
void DesktopEnvironment::LaunchTaskManager(){
    // ring 3 (/system/apps/taskmgr) when wanted, else the in-kernel kui build (satoru)
    KuiKernel::Open("taskmgr");
}
void DesktopEnvironment::LaunchBrowser(){
    // the browser is firefox on the linux runtime; the old placeholder tile
    // app is no longer reachable from the shell. (satoru)
    LaunchFirefox();
}
void DesktopEnvironment::LaunchMediaPlayer(){
    // ring 3 (/system/apps/denji) when wanted, else the in-kernel video library. (satoru)
    KuiKernel::Open("denji");
}
void DesktopEnvironment::LaunchInstaller(){
    // ring 3 (/system/apps/installer) when wanted, else the in-kernel kui build. (satoru)
    KuiKernel::Open("installer");
}
void DesktopEnvironment::LaunchFirefox(){
    // run the full `firefox` shell flow (lazy restore / install / exec) on the
    // shell kernel-process, detached - no terminal window. the gui thread only
    // queues + toasts; firefox's own wayland window appears when gecko maps
    // its toplevel. (satoru)
    // one firefox: while it is open, the dock or menu brings its window forward
    // instead of starting a second instance on the same profile (that one hit
    // the profile lock, or under sched=2 was refused with nothing on screen). (satoru)
    {
        Window* ws = WindowManager::GetWindows();
        for (int i = 0; ws && i < WM_MAX_WINDOWS; i++) {
            Window* w = &ws[i];
            if (w->state == WIN_CLOSED || w->app_icon != (int)AppIcons::FIREFOX) continue;
            if (!WaylandServer::IsWaylandWindow(w->id)) continue;
            if (w->state == WIN_MINIMIZED) WindowManager::Unminimize(w->id);
            WindowManager::Focus(w->id);
            return;
        }
    }
    bool installed = FirefoxLauncher::IsInstalled();
    if(!KernelProcesses::RunShellCommandDetached("firefox")){
        NotificationManager::Post("Firefox",
            "Already starting - give it a moment.",
            NotificationManager::ICON_INFO, 4000);
        return;
    }
    NotificationManager::Post("Firefox",
        installed ? "Starting Firefox..."
                  : "Setting up Firefox - first launch takes a minute...",
        NotificationManager::ICON_INFO, 6000);
}

// session control --------------------------------------------------------
static bool de_logout_requested = false;
void DesktopEnvironment::RequestLogout(){
    de_logout_requested = true;
}
bool DesktopEnvironment::ConsumeLogoutRequest(){
    if(de_logout_requested){ de_logout_requested = false; return true; }
    return false;
}
