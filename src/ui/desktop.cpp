//  kurono os  -  taskbar & desktop environment implementation
#include "desktop.h"
#include "control_center.h"
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../net/network.h"
#include "../system/ui_config.h"
#include "../fs/kvfs.h"
#include "../apps/terminal.h"
#include "../apps/file_manager.h"
#include "../apps/calculator.h"
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../shell/shell.h"
#include "../apps/task_manager.h"
#include "../apps/browser.h"
#include "../apps/media_player.h"
#include "../proc/scheduler.h"

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
static uint32_t de_frame_counter = 0;

//  taskbar
int  Taskbar::screen_width    = 0;
int  Taskbar::screen_height   = 0;
int  Taskbar::y_pos           = 0;
bool Taskbar::start_menu_open = false;
int  Taskbar::hover_button    = -1;
int  Taskbar::clock_h         = 12;
int  Taskbar::clock_m         = 0;
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
    if(cfg_position_top){
        y_pos = 10;
    } else {
        y_pos = screen_height - cfg_height - 10;
    }
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
    battery_pct=100; wifi_connected=false; wifi_strength=0; volume_pct=75;
    volume_popup_open=false; volume_slider_dragging=false;
    volume_slider_val = Audio::IsAvailable() ? Audio::GetMasterVolume() : 80;
    search_active=false; search_buf[0]=0; search_len=0;
}

int Taskbar::GetHeight(){ return TASKBAR_HEIGHT + 10; }
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

void Taskbar::RenderStartButton(){
    // K logo button, anchored to the floating bar's left edge.
    int btn_w = 44, btn_h = TASKBAR_HEIGHT - 10;
    int bx = tb_bar_x() + 6, by = y_pos + 5;
    unsigned int clr = (hover_button==0) ? COL_START_HOVER : COL_START_BTN;
    Graphics::FillRoundedRect(bx, by, btn_w, btn_h, 8, clr);
    // soft inner highlight for a more "tactile" feel
    Graphics::FillRect(bx+1, by+1, btn_w-2, 1, 0x40FFFFFF);
    // k logo  -  14px wide x 16px tall, centered in button
    int kx = bx + (btn_w - 14) / 2;
    int ky = by + (btn_h - 16) / 2;
    Graphics::FillRect(kx, ky, 3, 16, 0xFFFFFFFF);
    Graphics::FillRect(kx+3, ky+6, 2, 4, 0xFFE8EEFF);
    Graphics::FillRect(kx+5, ky+4, 3, 3, 0xFFD0DDFF);
    Graphics::FillRect(kx+8, ky+1, 3, 3, 0xFFB0C8FF);
    Graphics::FillRect(kx+11, ky, 3, 2, 0xFF90B0FF);
    Graphics::FillRect(kx+5, ky+10, 3, 3, 0xFFD0DDFF);
    Graphics::FillRect(kx+8, ky+12, 3, 3, 0xFFB0C8FF);
    Graphics::FillRect(kx+11, ky+14, 3, 2, 0xFF90B0FF);
}

// Pinned quick-launch icons immediately after the K button.
static void tb_render_quick_launch(){
    int x = tb_pin_strip_x();
    int y = tb_bar_y() + (TASKBAR_HEIGHT - TB_PIN_SZ) / 2;
    for (int i = 0; i < TB_PINNED_COUNT; i++) {
        Graphics::FillRoundedRect(x, y, TB_PIN_SZ, TB_PIN_SZ, 7, tb_pinned[i].colour);
        // top-edge highlight for a subtle 3D touch
        Graphics::FillRect(x+1, y+1, TB_PIN_SZ-2, 1, 0x55FFFFFF);
        char letter[2] = { tb_pinned[i].glyph, 0 };
        Graphics::DrawString(x + (TB_PIN_SZ - 8)/2, y + (TB_PIN_SZ - 14)/2 + 1,
                             letter, 0xFFFFFFFF, 0x00000000);

        // running indicator: any window whose title starts with the
        // glyph gets a small dot under the icon.
        bool running = false;
        for (int wi = 0; wi < WM_MAX_WINDOWS && !running; wi++) {
            Window* w = WindowManager::GetWindow(wi);
            if (w && w->state != WIN_CLOSED && w->title[0] == tb_pinned[i].glyph)
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
    // blinking cursor when active
    if (search_active) {
        int cx = sb_x + 28 + search_len * 8;
        if ((de_frame_counter / 30) & 1)
            Graphics::FillRect(cx, sb_y + 5, 2, sb_h - 10, 0xFF5C8AFF);
    }

    // count visible windows for centering
    int win_ids[WM_MAX_WINDOWS];
    int wcount = 0;
    for(int i=0;i<WM_MAX_WINDOWS;i++){
        Window* w = WindowManager::GetWindow(i);
        if(w && w->state!=WIN_CLOSED){
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

        if(focused){
            Graphics::FillRoundedRect(x, y_pos+6, bw, TASKBAR_HEIGHT-12, 6, 0xFF222240);
            // active underline indicator
            Graphics::FillRect(x+8, y_pos+TASKBAR_HEIGHT-4, bw-16, 3, COL_START_BTN);
        }

        // app icon  -  colored rounded square with first letter
        unsigned int ic = 0xFF3498DB;
        char c0 = w->title[0];
        if(c0=='T') ic = 0xFF2ECC71;
        else if(c0=='F') ic = 0xFFF39C12;
        else if(c0=='C' && w->title[1]=='o') ic = 0xFF5C8AFF;
        else if(c0=='C') ic = 0xFF9B59B6;
        else if(c0=='S') ic = 0xFF607D8B;
        else if(c0=='E') ic = 0xFFE74C3C;
        else if(c0=='B') ic = 0xFF3498DB;
        else if(c0=='M') ic = 0xFFE91E63;

        Graphics::FillRoundedRect(x+6, y_pos+10, bw-12, TASKBAR_HEIGHT-20, 5, ic);
        char letter[2] = {c0, 0};
        Graphics::DrawString(x+13, y_pos+13, letter, COL_WHITE, 0xFF000000);

        x += bw + gap;
    }
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

    DateTime dt = TimeManager::NowLocalDateTime();
    char datebuf[16] = {0};
    int_to_str(dt.mon, datebuf, 16);
    sapp(datebuf, "/", 16);
    char daybuf[4]; int_to_str(dt.dom, daybuf, 4); sapp(datebuf, daybuf, 16);
    sapp(datebuf, "/", 16);
    char yearbuf[8]; int_to_str(dt.year, yearbuf, 8); sapp(datebuf, yearbuf, 16);

    int cx = tb_clock_x() + 4;
    Graphics::DrawString(cx, y_pos+6,  full_time, 0xFFF0F0FF, 0xFF000000);
    Graphics::DrawString(cx, y_pos+24, datebuf,   0xFF8898C0, 0xFF000000);
}

void Taskbar::RenderSystemTray(){
    // System tray cluster: bell, network, volume, battery  -  packed
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
    if(!start_menu_open) return;

    // left-aligned above the k button (which is anchored to the floating bar)
    int mx0 = tb_bar_x() + 6;
    int my0 = y_pos - START_MENU_H - 8;

    // shadow
    Graphics::FillRoundedRect(mx0+6, my0+6, START_MENU_W, START_MENU_H, 14, 0xFF040408);
    // background
    Graphics::FillRoundedRect(mx0, my0, START_MENU_W, START_MENU_H, 14, COL_START_MENU);
    // border
    Graphics::DrawRect(mx0, my0, START_MENU_W, START_MENU_H, 0xFF2A2A48);

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
    static const int nit = 12;

    int iy = my0 + 56;
    for(int i=0;i<nit;i++){
        if(items[i][0]=='-'){
            Graphics::FillRect(mx0+16, iy+6, START_MENU_W-32, 1, 0xFF2A2A48);
            iy += 12;
            continue;
        }
        if (item_accents[i])
            Graphics::FillRoundedRect(mx0+8, iy+4, 3, 20, 1, item_accents[i]);
        Graphics::DrawString(mx0+20, iy+4, items[i], COL_ICON_TEXT, 0xFF000000);
        iy += 32;
    }
}

void Taskbar::Render(){
    int bx = tb_bar_x();
    int by = tb_bar_y();
    int bw = tb_bar_w();
    int bh = tb_bar_h();
    int br = tb_bar_r();

    // Soft drop shadow under the floating bar.  Two stacked alpha
    // fills produce a quick gradient feel without per-pixel maths.
    Graphics::FillRectAlpha(bx + 2, by + bh + 2, bw - 4, 4, 90,  0xFF000000);
    Graphics::FillRectAlpha(bx + 4, by + bh + 6, bw - 8, 3, 50,  0xFF000000);

    // Bar body  -  rounded "card"
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
    for(int i=0; i<app_count && match_count < 9; i++){
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
        // no matches: show "no results"
        int sx = 58, sy = y_pos - 48;
        Graphics::FillRoundedRect(sx, sy, 220, 36, 10, 0xFF141424);
        Graphics::DrawRect(sx, sy, 220, 36, 0xFF2A2A48);
        Graphics::DrawString(sx+12, sy+10, "No results", 0xFF606080, 0xFF000000);
        return;
    }

    int sx = tb_search_x();
    int sh = match_count * 30 + 12;
    int sy = y_pos - sh - 4;
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
}

void Taskbar::RenderVolumePopup(){
    if(!volume_popup_open) return;

    int vol_x = tb_vol_x();

    int pop_w = 52, pop_h = 180;
    int pop_x = vol_x - pop_w/2 + TB_VOL_W/2;
    int pop_y = y_pos - pop_h - 8;

    // shadow + background
    Graphics::FillRoundedRect(pop_x+4, pop_y+4, pop_w, pop_h, 8, 0xFF060610);
    Graphics::FillRoundedRect(pop_x, pop_y, pop_w, pop_h, 8, 0xFF181828);
    Graphics::DrawRect(pop_x, pop_y, pop_w, pop_h, 0xFF333355);

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
    if(volume_popup_open){
        int pop_w = 52, pop_h = 180;
        int vol_icon_x2 = tb_vol_x();
        int pop_x = vol_icon_x2 - pop_w/2 + TB_VOL_W/2;
        int pop_y = y_pos - pop_h - 8;

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
        volume_popup_open = false;
        volume_slider_dragging = false;
    }

    // check if click is in taskbar area
    if(my < y_pos){
        // start menu handling
        if(start_menu_open){
            int mx0 = tb_bar_x() + 6;
            int my0 = y_pos - START_MENU_H - 8;
            if(mx>=mx0 && mx<mx0+START_MENU_W && my>=my0 && my<my0+START_MENU_H){
                int iy = my0 + 56;
                static const int nit = 12;
                for(int i=0;i<nit;i++){
                    if(i==8){ iy+=12; continue; }
                    if(my>=iy && my<iy+32){
                        start_menu_open=false;
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
                            case 11: break;
                        }
                        return true;
                    }
                    iy+=32;
                }
                return true;
            }
            start_menu_open=false;
        }
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
                int sh2 = mc2*30+12, sy2 = y_pos-sh2-4, sx2=tb_search_x(), sw2=tb_search_w();
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

    // K button  -  anchored to the floating bar's left edge
    {
        int k_x = tb_bar_x() + 6;
        if(mx>=k_x && mx<k_x+44 && my>=y_pos+4 && my<y_pos+TASKBAR_HEIGHT-4){
            start_menu_open = !start_menu_open;
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
                if (tb_pinned[i].launch) tb_pinned[i].launch();
                start_menu_open = false;
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
            start_menu_open = false;
            return true;
        }
    }

    // Task buttons  -  centered, layout matches RenderTaskButtons.
    {
        int win_ids[WM_MAX_WINDOWS];
        int wcount = 0;
        for(int i=0;i<WM_MAX_WINDOWS;i++){
            Window* w = WindowManager::GetWindow(i);
            if(w && w->state!=WIN_CLOSED){
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
                if(w->state==WIN_MINIMIZED) w->state=WIN_NORMAL;
                WindowManager::BringToFront(w->id);
                return true;
            }
            x += bw + gap2;
        }
    }

    // Right cluster: bell, network, volume, battery, clock.
    {
        // Volume icon  -  opens dedicated volume popup
        int vol_x3 = tb_vol_x();
        if(mx>=vol_x3 && mx<vol_x3+TB_VOL_W && my>=y_pos){
            volume_popup_open = !volume_popup_open;
            volume_slider_dragging = false;
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
            volume_popup_open = false;
            start_menu_open   = false;
            search_active     = false;
            return true;
        }
    }

    start_menu_open=false;
    search_active=false;
    volume_popup_open=false;
    return true;
}

void Taskbar::Update(){
    // update clock from real rtc-backed time
    DateTime dt = TimeManager::NowLocalDateTime();
    clock_h = dt.h;
    clock_m = dt.m;
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

    ArrangeIcons();
    RefreshFiles();
}

void Desktop::AddIcon(const char* name,const char* path,int tp){
    if(icon_count>=DESKTOP_MAX_ICONS)return;
    DesktopIcon* ic=&icons[icon_count++];
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
    int min_y = 4;
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
        for(int slot_y=cfg_margin_y;slot_y<desktop_bottom-cfg_icon_size;slot_y+=cfg_spacing_y){
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

    for(int icon_index=0;icon_index<icon_count;){
        if(IsDesktopFileIcon(&icons[icon_index]) && !KVFS::Exists(icons[icon_index].path)){
            for(int shift_index=icon_index;shift_index<icon_count-1;shift_index++) icons[shift_index]=icons[shift_index+1];
            icon_count--;
            if(selected_icon==icon_index) selected_icon=-1;
            else if(selected_icon>icon_index) selected_icon--;
            if(drag_icon==icon_index) drag_icon=-1;
            else if(drag_icon>icon_index) drag_icon--;
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
    last_file_sync_ms = Timer::GetRealMs();
}

void Desktop::ArrangeIcons(){
    int gx = cfg_margin_x;
    int gy = cfg_margin_y;
    int tb_h = Taskbar::cfg_height;
    int col_h = screen_height - tb_h - cfg_margin_y - 40;
    for(int i=0;i<icon_count;i++){
        icons[i].x = gx;
        icons[i].y = gy;
        gy += cfg_spacing_y;
        if(gy > col_h){
            gy = cfg_margin_y;
            gx += cfg_spacing_x;
        }
    }
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
    // shift remaining icons
    for(int i=index;i<icon_count-1;i++) icons[i]=icons[i+1];
    icon_count--;
    if(selected_icon==index) selected_icon=-1;
    else if(selected_icon>index) selected_icon--;
    if(drag_icon==index) drag_icon=-1;
    else if(drag_icon>index) drag_icon--;
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
        PMM::FreeBytes(gradient_cache, gradient_cache_bytes);
        gradient_cache = nullptr;
        gradient_cache_bytes = 0;
        gradient_cache_h = 0;
    }
}

void Desktop::SetWallpaperImage(const MediaDecoder::Image& img){
    if (!img.valid || !img.data || img.width <= 0 || img.height <= 0) {
        have_image_wallpaper = false;
        if (gradient_cache) {
            PMM::FreeBytes(gradient_cache, gradient_cache_bytes);
            gradient_cache = nullptr;
            gradient_cache_bytes = 0;
            gradient_cache_h = 0;
        }
        return;
    }
    int w = screen_width;
    // Floating taskbar leaves a strip of wallpaper visible at the bottom
    // and behind the bar's rounded corners  -  render full screen height.
    int h = screen_height;
    if (w <= 0 || h <= 0) return;

    // allocate full-screen cache
    if (gradient_cache) { PMM::FreeBytes(gradient_cache, gradient_cache_bytes); gradient_cache = nullptr; }
    int total = w * h;
    size_t bytes = (size_t)total * sizeof(uint32_t);
    gradient_cache = (uint32_t*)PMM::AllocBytes(bytes);
    if (!gradient_cache) return;
    gradient_cache_bytes = bytes;
    gradient_cache_h = h;
    have_image_wallpaper = true;

    // scale image to screen using nearest-neighbor sampling
    for (int y = 0; y < h; y++) {
        int src_y = (y * img.height) / h;
        if (src_y >= img.height) src_y = img.height - 1;
        for (int x = 0; x < w; x++) {
            int src_x = (x * img.width) / w;
            if (src_x >= img.width) src_x = img.width - 1;
            int idx = (src_y * img.width + src_x);
            uint8_t* p = img.data + idx * 4;
            uint8_t r, g, b;
            if (img.order == 1) { // bgra
                b = p[0]; g = p[1]; r = p[2];
            } else { // rgba
                r = p[0]; g = p[1]; b = p[2];
            }
            gradient_cache[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

void Desktop::RenderWallpaper(){
    int h = screen_height;
    if (h <= 0) return;
    int w = screen_width;

    // if we have an image wallpaper, skip procedural generation
    if (have_image_wallpaper && gradient_cache && gradient_cache_h == h) {
        // fast blit image wallpaper  -  row-wise memcpy for maximum throughput
        uint8_t* buf = Graphics::GetBackBuffer();
        if (!buf) buf = Graphics::GetBuffer();
        uint32_t pitch = Graphics::GetPitch();
        uint32_t row_bytes = (uint32_t)w * 4;
        for (int y = 0; y < h; y++) {
            memcpy(buf + y * pitch, gradient_cache + y * w, row_bytes);
        }
        // mark full desktop area dirty so swapbuffers copies it
        Graphics::MarkDirty(0, 0, w, h);
        return;
    }

    // build gradient cache once - warm modern abstract gradient (fallback)
    if (!gradient_cache || gradient_cache_h != h) {
        // allocate full screen cache (w * h pixels)
        int total = w * h;
        if (!gradient_cache || gradient_cache_h != h) {
            if (gradient_cache) { PMM::FreeBytes(gradient_cache, gradient_cache_bytes); gradient_cache = nullptr; }
            size_t bytes = (size_t)total * sizeof(uint32_t);
            gradient_cache = (uint32_t*)PMM::AllocBytes(bytes);
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

                    // warm orb (upper-right)  -  amber/orange glow
                    float dx1 = tx - 0.78f;
                    float dy1 = ty - 0.18f;
                    float d1 = dx1*dx1 + dy1*dy1;
                    float orb1 = 1.0f / (1.0f + d1 * 6.0f);
                    r += 200.0f * orb1;
                    g += 110.0f * orb1;
                    b += 25.0f * orb1;

                    // cool orb (lower-left)  -  deep blue glow
                    float dx2 = tx - 0.15f;
                    float dy2 = ty - 0.82f;
                    float d2 = dx2*dx2 + dy2*dy2;
                    float orb2 = 1.0f / (1.0f + d2 * 8.0f);
                    r += 40.0f * orb2;
                    g += 30.0f * orb2;
                    b += 140.0f * orb2;

                    // accent orb (center)  -  soft magenta/pink
                    float dx3 = tx - 0.5f;
                    float dy3 = ty - 0.42f;
                    float d3 = dx3*dx3 + dy3*dy3;
                    float orb3 = 1.0f / (1.0f + d3 * 12.0f);
                    r += 90.0f * orb3;
                    g += 25.0f * orb3;
                    b += 70.0f * orb3;

                    // aurora ribbon (upper-center)  -  teal/cyan sweep
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

                    // vignette  -  darken edges for depth
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
    uint8_t* buf = Graphics::GetBackBuffer();
    if (!buf) buf = Graphics::GetBuffer();
    uint32_t pitch = Graphics::GetPitch();
    uint32_t row_bytes = (uint32_t)w * 4;
    for (int y = 0; y < h; y++) {
        memcpy(buf + y * pitch, gradient_cache + y * w, row_bytes);
    }
    // mark full desktop area dirty so swapbuffers copies it to the framebuffer
    Graphics::MarkDirty(0, 0, w, h);
}

void Desktop::RenderIcon(DesktopIcon* ic){
    int ix=ic->x, iy=ic->y;
    int icon_sz = ICON_SIZE;

    // selection highlight  -  glowing aura
    if(ic->selected){
        Graphics::FillRoundedRect(ix-6, iy-6, icon_sz+12, icon_sz+26, 8, COL_ICON_SEL);
    }

    // determine icon colors from name
    unsigned int ic_top, ic_bot;
    const char* nm = ic->name;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') {
        ic_top=0xFF34D058; ic_bot=0xFF1E8C3A;
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

    // icon body  -  rounded square with gradient effect
    Graphics::FillRoundedRect(ix, iy, icon_sz, icon_sz, 13, ic_top);
    // bottom gradient half
    Graphics::FillRoundedRect(ix, iy + icon_sz/2, icon_sz, icon_sz/2, 13, ic_bot);
    // smooth blend zone
    Graphics::FillRect(ix + 4, iy + icon_sz/2, icon_sz - 8, 6, ic_top);

    // inner highlight (top glossy shine  -  lighter version of top color)
    // skip: these used alpha blending and are expensive, icon already looks good

    // icon-specific symbols
    int cx = ix + icon_sz/2;
    int cy_icon = iy + icon_sz/2;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') {
        Graphics::DrawString(cx-10, cy_icon-6, ">_", 0xFFFFFFFF, 0xFF000000);
    } else if(nm[0]=='F' && nm[1]=='i') {
        // folder
        Graphics::FillRoundedRect(ix+12, iy+16, 28, 20, 3, 0xFFFFCC80);
        Graphics::FillRect(ix+12, iy+16, 14, 5, 0xFFE0A050);
    } else if(nm[0]=='C' && nm[1]=='o') {
        Graphics::DrawString(cx-11, cy_icon-6, "<>", 0xFFFFFFFF, 0xFF000000);
        Graphics::FillRect(ix+18, iy+30, 20, 3, 0xFFDDE7FF);
    } else if(nm[0]=='C') {
        // calculator grid
        Graphics::FillRect(ix+12, iy+10, 28, 8, 0xFF1E88E5);
        for(int r=0;r<2;r++) for(int c=0;c<3;c++)
            Graphics::FillRoundedRect(ix+14+c*9, iy+22+r*9, 7, 7, 2, 0xFFFFFFFF);
    } else if(nm[0]=='E') {
        // text lines
        Graphics::FillRect(ix+14, iy+14, 24, 3, 0xFFEEEEEE);
        Graphics::FillRect(ix+14, iy+20, 18, 3, 0xFFCCCCCC);
        Graphics::FillRect(ix+14, iy+26, 22, 3, 0xFFEEEEEE);
        Graphics::FillRect(ix+14, iy+32, 14, 3, 0xFFCCCCCC);
    } else if(nm[0]=='S') {
        // gear
        Graphics::FillCircle(cx, cy_icon, 14, 0xFF78909C);
        Graphics::FillCircle(cx, cy_icon, 8, 0xFFCFD8DC);
        Graphics::FillCircle(cx, cy_icon, 4, 0xFF78909C);
    } else if(nm[0]=='H') {
        // house
        Graphics::FillRect(ix+16, iy+24, 20, 14, 0xFFFFCC80);
        Graphics::FillRect(ix+12, iy+20, 28, 4, 0xFFE0A050);
        Graphics::FillRect(ix+16, iy+16, 20, 4, 0xFFE0A050);
    }

    // label  -  centered below icon with background pill for readability
    char lbl[14]; scpy(lbl, ic->name, 13);
    int tw=slen(lbl)*8;
    int tx=ix + icon_sz/2 - tw/2;
    if(tx<ix-8)tx=ix-8;
    // background pill
    Graphics::FillRoundedRect(tx-4, iy+icon_sz+1, tw+8, 14, 4, 0xFF080812);
    // text
    Graphics::DrawString(tx, iy+icon_sz+3, lbl, COL_ICON_TEXT, 0xFF000000);
}

// context menu item lists  -  icon-targeted vs empty-desktop
static const char* ctx_items_icon[]  = { "Open", "Delete", "Refresh" };
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

    // shadow
    Graphics::FillRoundedRect(context_menu_x+5, context_menu_y+5, mw, mh, 8, 0xFF060610);
    // background
    Graphics::FillRoundedRect(context_menu_x, context_menu_y, mw, mh, 8, cfg_col_ctx_bg);
    // accent border
    Graphics::DrawRect(context_menu_x, context_menu_y, mw, mh, cfg_col_ctx_border);

    int iy = context_menu_y + 6;
    for(int i=0;i<nit;i++){
        if(i > 0){
            Graphics::FillRect(context_menu_x+12, iy-1, mw-24, 1, 0xFF1A1A30);
        }
        uint32_t txt_col = cfg_col_ctx_text;
        // tint "delete" red
        if(on_icon && i==1) txt_col = 0xFFFF6666;
        Graphics::DrawString(context_menu_x+16, iy + (ih-14)/2, items[i], txt_col, 0xFF000000);
        iy += ih;
    }
}

void Desktop::Render(){
    RenderWallpaper();
    for(int i=0;i<icon_count;i++){
        RenderIcon(&icons[i]);
    }
    RenderContextMenu();
}

int Desktop::IconAt(int mx,int my){
    for(int i=0;i<icon_count;i++){
        if(mx>=icons[i].x-8 && mx<icons[i].x+60 &&
           my>=icons[i].y-8 && my<icons[i].y+80){
            return i;
        }
    }
    return -1;
}

bool Desktop::HandleClick(int mx,int my){
    int tb_h = Taskbar::cfg_height;
    if(my>=screen_height-tb_h) return false;

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
                // 0=open, 1=delete, 2=refresh
                if(row==0){ HandleDoubleClick(icons[context_menu_target].x+4, icons[context_menu_target].y+4); }
                else if(row==1 && cfg_allow_edit){ RemoveIcon(context_menu_target); }
                else if(row==2){ RefreshFiles(); }
            } else {
                // 0=new folder, 1=new file, 2=refresh, 3=settings
                if(row==0 && cfg_allow_edit){ CreateFolderInteractive(); }
                else if(row==1 && cfg_allow_edit){ CreateFileInteractive(); }
                else if(row==2){ RefreshFiles(); }
                else if(row==3){ DesktopEnvironment::LaunchSettings(); }
            }
            return true;
        }
        // click outside menu  -  close it
        context_menu_open = false;
    }

    // deselect all
    for(int i=0;i<icon_count;i++) icons[i].selected=false;
    selected_icon=-1;

    int idx=IconAt(mx,my);
    if(idx>=0){
        icons[idx].selected=true;
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
        TextEditorApp::OpenFile(icons[idx].path);
        return;
    }
    if(icons[idx].icon_type==1){
        FileManagerApp::OpenAt(icons[idx].path);
        return;
    }

    // launch app based on icon name (prefix matching)
    const char* nm = icons[idx].name;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') DesktopEnvironment::LaunchTerminal();
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
    context_menu_open = true;
    context_menu_x = mx;
    context_menu_y = my;
    context_menu_target = IconAt(mx, my);
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
                icons[drag_icon].x = mx - drag_offset_x;
                icons[drag_icon].y = my - drag_offset_y;
                ClampIcon(drag_icon);
            }
            return;
        }
        ClampIcon(drag_icon);
        icon_dragging=false;
        icon_drag_moved=false;
        drag_icon=-1;
    }

    uint32_t now = Timer::GetRealMs();
    if(now - last_file_sync_ms > 500){
        RefreshFiles();
    }
}

//  desktopenvironment
void DesktopEnvironment::Init(int sw,int sh){
    WindowManager::Init(sw, sh);
    Desktop::Init(sw, sh);
    Taskbar::Init(sw, sh);
    ControlCenter::Init(sw, sh);
    // adjust wm desktop area to exclude taskbar
    WindowManager::SetDesktopArea(0, 0, sw, sh - Taskbar::GetHeight());
}

void DesktopEnvironment::Render(){
    Desktop::Render();
    WindowManager::RenderAll();
    Taskbar::Render();
    ControlCenter::Render();
}

// double-click tracking state
static uint32_t last_click_time_ms = 0;
static int last_click_x = -1, last_click_y = -1;

void DesktopEnvironment::HandleInput(int mx,int my,bool mouse_down,bool clicked,char key){
    // advance frame counter for timing (each frame ~6ms)
    de_frame_counter++;

    WindowManager::HandlePointerMove(mx, my);

    // alt-tab: cycle focus through visible windows by id (rough z-order proxy).
    // we want true z-order but the current z_order field changes only on Focus(),
    // so a stable id-cycle gives a consistent round-trip.
    {
        const KeyboardState& ks = Keyboard::GetState();
        if (ks.alt && Keyboard::IsKeyPressed(KEY_TAB)) {
            int cur = WindowManager::GetFocusedIndex();
            Window* ws = WindowManager::GetWindows();
            int n = WindowManager::GetWindowCount();
            // gather candidate ids
            int cand[WM_MAX_WINDOWS]; int nc = 0;
            for (int i = 0; i < n; i++) {
                Window* w = &ws[i];
                if (w->state == WIN_CLOSED || !w->visible || w->state == WIN_MINIMIZED)
                    continue;
                cand[nc++] = w->id;
            }
            if (nc > 1) {
                int idx = 0;
                for (int i = 0; i < nc; i++) if (cand[i] == cur) { idx = i; break; }
                int next = cand[(idx + 1) % nc];
                WindowManager::Focus(next);
                WindowManager::BringToFront(next);
            } else if (nc == 1) {
                WindowManager::Focus(cand[0]);
            }
        }
    }

    // right-click handling  -  forward event 4 to focused window
    bool right_clicked = Mouse::RightClicked();
    if(right_clicked){
        // check if click is on a window first
        Window* fw = WindowManager::GetFocusedWindow();
        if(fw && fw->visible && fw->state != WIN_CLOSED &&
           mx >= fw->content_x && mx < fw->content_x + fw->content_w &&
           my >= fw->content_y && my < fw->content_y + fw->content_h) {
            if(fw->input){
                fw->input(fw, 4, mx, my);  // event 4 = right-click
            }
        } else {
            // desktop right-click
            Desktop::HandleRightClick(mx, my);
        }
    }

    if(clicked){
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
        if(Taskbar::HandleClick(mx,my)) return;
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
            int pop_y = Taskbar::y_pos - pop_h - 8;
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
    if(key){
        // if search is active, route keys there
        if(Taskbar::search_active){
            if(key == '\n' || key == '\r'){
                // execute search  -  launch best matching app (substring match)
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
                fw->input(fw, 2, (int)key, 0); // event 2 = keypress
            }
        }
    }
}

void DesktopEnvironment::Update(){
    Taskbar::Update();
    // Shell commands are drained by ShellProcess when the preemptive
    // scheduler is running; calling Tick here would still run Execute on
    // the GUI thread and freeze the desktop + mouse.
    if (!Scheduler::IsPreemptiveKernelSchedulerActive()) {
        TerminalApp::Tick();
    }
}

void DesktopEnvironment::ReloadFromConfig(){
    UIConfig::Reload();
    Taskbar::ReloadFromConfig();
    Desktop::ReloadFromConfig();
    WindowManager::ReloadFromConfig();
}

void DesktopEnvironment::LaunchTerminal(){
    TerminalApp::Open();
}
void DesktopEnvironment::LaunchFileBrowser(){
    FileManagerApp::Open();
}
void DesktopEnvironment::LaunchCalculator(){
    Calculator::Init(0, 0);
    Calculator::SetActive(true);
    int wid = WindowManager::CreateWindow("Calculator", -1, -1, 200, 250,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) {
            (void)w;
            Calculator::SetPosition(cx, cy);
            Calculator::SetSize(cw, ch);
            Calculator::Draw();
        },
        (WindowInputFunc)[](Window* w, int ev, int p1, int p2) {
            (void)w;
            if (ev == 1) Calculator::Input(p1, p2, true, 0);
            else if (ev == 2) Calculator::Input(0, 0, false, (char)p1);
        }
    );
    (void)wid;
}
void DesktopEnvironment::LaunchTextEditor(){
    TextEditorApp::Open();
}
void DesktopEnvironment::LaunchSettings(){
    SettingsApp::Open();
}
void DesktopEnvironment::LaunchTaskManager(){
    TaskManagerApp::Open();
}
void DesktopEnvironment::LaunchBrowser(){
    BrowserApp::Open();
}
void DesktopEnvironment::LaunchMediaPlayer(){
    MediaPlayerApp::Open();
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
