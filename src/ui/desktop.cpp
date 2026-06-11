// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Taskbar & Desktop Environment Implementation
// ═══════════════════════════════════════════════════════════════════════════
#include "desktop.h"
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../net/network.h"
#include "../apps/terminal.h"
#include "../apps/file_manager.h"
#include "../apps/calculator.h"
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../shell/shell.h"
#include "../apps/task_manager.h"
#include "../apps/browser.h"
#include "../apps/media_player.h"

// ────────────── colour palette ──────────────
static const unsigned int COL_TASKBAR      = 0xF0101018;
static const unsigned int COL_TASKBAR_TOP  = 0xFF2A2A40;
static const unsigned int COL_START_BTN    = 0xFF5C8AFF;
static const unsigned int COL_START_HOVER  = 0xFF4470E0;
static const unsigned int COL_START_MENU   = 0xF0181828;
static const unsigned int COL_MENU_ITEM_HL = 0xFF2A3A5A;
static const unsigned int COL_TRAY_TEXT    = 0xFFBBBBCC;
static const unsigned int COL_DESK_BG      = 0xFF0C0818;
static const unsigned int COL_ICON_SEL     = 0x445C8AFF;
static const unsigned int COL_ICON_TEXT    = 0xFFE8E8F0;
static const unsigned int COL_CTX_BG       = 0xF0161630;
static const unsigned int COL_CTX_BORDER   = 0xFF5C8AFF;
static const unsigned int COL_WHITE        = 0xFFF0F0F0;

// ────────────── helpers ──────────────
static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static void int_to_str(int v,char* b,int mx){
    if(mx<2){b[0]=0;return;}
    if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;
    do{t[n++]=('0'+(v%10));v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}
static void int2(int v,char*b){b[0]='0'+(v/10)%10;b[1]='0'+v%10;b[2]=0;}

// ═══════════════════════════════════════════════════════════════════════════
//  Taskbar
// ═══════════════════════════════════════════════════════════════════════════
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

void Taskbar::Init(int sw,int sh){
    screen_width=sw; screen_height=sh;
    y_pos=sh-TASKBAR_HEIGHT;
    start_menu_open=false;
    hover_button=-1;
    clock_h=12; clock_m=0;
    battery_pct=100; wifi_connected=false; wifi_strength=0; volume_pct=75;
    volume_popup_open=false; volume_slider_dragging=false;
    volume_slider_val = Audio::IsAvailable() ? Audio::GetMasterVolume() : 80;
}

int Taskbar::GetHeight(){ return TASKBAR_HEIGHT; }
int Taskbar::GetY(){ return y_pos; }

void Taskbar::SetClock(int h,int m){ clock_h=h; clock_m=m; }
void Taskbar::SetBattery(int p){ battery_pct=p; }
void Taskbar::SetWiFiConnected(bool c,int s){ wifi_connected=c; wifi_strength=s; }
void Taskbar::SetVolume(int p){ volume_pct=p; }

void Taskbar::RenderStartButton(){
    // Kurono OS start button — centered in taskbar with glow
    int btn_w = 38, btn_h = TASKBAR_HEIGHT - 10;
    int center_x = screen_width / 2 - btn_w / 2;
    unsigned int clr = (hover_button==0) ? COL_START_HOVER : COL_START_BTN;
    // Subtle glow behind button
    Graphics::FillRoundedRect(center_x-2, y_pos+3, btn_w+4, btn_h+4, 10, 0x205C8AFF);
    Graphics::FillRoundedRect(center_x, y_pos+5, btn_w, btn_h, 9, clr);
    // Kurono "K" logo — centered in the button
    int kx = center_x + (btn_w/2) - 9, ky = y_pos + (TASKBAR_HEIGHT/2) - 9;
    // Vertical bar
    Graphics::FillRect(kx, ky, 3, 18, 0xFFE0E8FF);
    // Center joint
    Graphics::FillRect(kx+3, ky+7, 3, 3, 0xFFD0D8F0);
    // Upper-right diagonal (3 steps)
    Graphics::FillRect(kx+6,  ky+4, 3, 3, 0xFFA8C4F0);
    Graphics::FillRect(kx+9,  ky+1, 3, 3, 0xFF8CB4E8);
    Graphics::FillRect(kx+12, ky,   3, 2, 0xFF70A4E0);
    // Lower-right diagonal (3 steps)
    Graphics::FillRect(kx+6,  ky+10, 3, 3, 0xFFA8C4F0);
    Graphics::FillRect(kx+9,  ky+13, 3, 3, 0xFF8CB4E8);
    Graphics::FillRect(kx+12, ky+16, 3, 2, 0xFF70A4E0);
}

void Taskbar::RenderTaskButtons(){
    // Windows 11-style: search box on left, task buttons centered
    // Search box — wider with icon and text
    int sb_x = 8, sb_w = 200, sb_h = TASKBAR_HEIGHT - 14;
    int sb_y = y_pos + 7;
    Graphics::FillRoundedRect(sb_x, sb_y, sb_w, sb_h, 14, 0xFF1A1A30);
    // Inner subtle border
    Graphics::FillRoundedRect(sb_x, sb_y, sb_w, 1, 0, 0xFF2A2A48);
    // Search icon (magnifier circle + handle)
    int mag_x = sb_x + 14, mag_y = sb_y + sb_h/2 - 1;
    Graphics::DrawCircle(mag_x, mag_y, 5, 0xFF7878A0);
    Graphics::DrawLine(mag_x+4, mag_y+4, mag_x+7, mag_y+7, 0xFF7878A0);
    // Placeholder or typed text
    if (search_buf[0]) {
        Graphics::DrawString(sb_x + 28, sb_y + 5, search_buf, 0xFFD0D0E0, 0xFF000000);
    } else {
        Graphics::DrawString(sb_x + 28, sb_y + 5, "Search apps...", 0xFF606080, 0xFF000000);
    }

    // Render task buttons centered around start button
    int btn_w_k = 38;
    int center_x = screen_width / 2 - btn_w_k / 2;
    int x = center_x + btn_w_k + 8; // right of start button

    for(int i=0;i<WindowManager::GetWindowCount();i++){
        Window* w = WindowManager::GetWindow(i);
        if(!w || w->state==WIN_CLOSED) continue;

        bool focused = (WindowManager::GetFocusedIndex()==i);

        int bw = 36;
        // Icon-only style like Windows 11
        if(focused){
            Graphics::FillRoundedRect(x, y_pos+6, bw, TASKBAR_HEIGHT-12, 6, 0xFF2A3A5A);
            // Active indicator dot
            Graphics::FillRoundedRect(x+10, y_pos+TASKBAR_HEIGHT-5, bw-20, 3, 1, COL_START_BTN);
        }

        // App icon (first letter in colored circle)
        unsigned int ic = 0xFF3498DB;
        if(w->title[0]=='T') ic = 0xFF2ECC71;      // Terminal = green
        else if(w->title[0]=='F') ic = 0xFFF39C12;  // Files = amber
        else if(w->title[0]=='C') ic = 0xFF9B59B6;  // Calculator = purple
        else if(w->title[0]=='S') ic = 0xFF666688;  // Settings = gray
        else if(w->title[0]=='E') ic = 0xFFE74C3C;  // Editor = red

        Graphics::FillRoundedRect(x+4, y_pos+10, bw-8, TASKBAR_HEIGHT-20, 4, ic);
        char letter[2] = {w->title[0], 0};
        Graphics::DrawString(x+12, y_pos+12, letter, COL_WHITE, 0xFF000000);

        x += bw + 4;
        if(x > screen_width - 200) break;
    }
}

void Taskbar::RenderClock(){
    // Windows 11-style: time on top, date below, right-aligned
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

    // Dynamic date from TimeManager
    DateTime dt = TimeManager::NowLocalDateTime();
    char datebuf[16] = {0};
    int_to_str(dt.mon, datebuf, 16);
    sapp(datebuf, "/", 16);
    char daybuf[4]; int_to_str(dt.dom, daybuf, 4); sapp(datebuf, daybuf, 16);
    sapp(datebuf, "/", 16);
    char yearbuf[8]; int_to_str(dt.year, yearbuf, 8); sapp(datebuf, yearbuf, 16);

    int tw_time = slen(full_time) * 8;
    int tw_date = slen(datebuf) * 8;
    int max_tw = tw_time > tw_date ? tw_time : tw_date;
    int cx = screen_width - max_tw - 12;
    // Bright white time
    Graphics::DrawString(cx, y_pos+6, full_time, 0xFFF0F0FF, 0xFF000000);
    // Dynamic date
    Graphics::DrawString(cx, y_pos+24, datebuf, 0xFF8888AA, 0xFF000000);
}

void Taskbar::RenderSystemTray(){
    // Windows 11-style system tray: icons clustered right
    int x = screen_width - 200;

    // ^ chevron (overflow)
    Graphics::DrawString(x, y_pos+14, "^", 0xFF888899, 0xFF000000);
    x += 22;

    // WiFi icon — signal bars (always show connected for sim)
    {
        int sig = 3; // simulated signal strength 0-3
        for(int i=0;i<4;i++){
            int bh = 5 + i*3;
            unsigned int bc = (i <= sig) ? 0xFFD0D8F0 : 0xFF404060;
            Graphics::FillRoundedRect(x+i*5, y_pos+26-bh, 3, bh, 1, bc);
        }
    }
    x += 26;

    // Volume icon — cleaner speaker shape
    {
        int vy = y_pos + 14;
        Graphics::FillRect(x, vy+3, 4, 8, COL_TRAY_TEXT);
        Graphics::FillRect(x+4, vy+1, 3, 12, COL_TRAY_TEXT);
        // Sound wave arcs
        for(int a=0; a<3; a++) {
            int r = 4 + a*3;
            unsigned int wc = (a < 2) ? 0xFFBBBBCC : 0xFF666688;
            Graphics::DrawCircle(x+8, vy+7, r, wc);
        }
    }
    x += 26;

    // Battery — more modern rounded design
    {
        int bx = x, by = y_pos + 14;
        Graphics::FillRoundedRect(bx, by, 22, 12, 3, 0xFF333355);
        Graphics::FillRect(bx+22, by+3, 2, 5, 0xFF333355);
        int fill = (battery_pct * 18) / 100;
        if (fill < 2) fill = 2;
        unsigned int bclr = (battery_pct>20) ? 0xFF2ECC71 : 0xFFE74C3C;
        Graphics::FillRoundedRect(bx+2, by+2, fill, 8, 2, bclr);
    }
}

void Taskbar::RenderStartMenu(){
    if(!start_menu_open) return;

    // Windows 11-style: centered above start button
    int mx0 = screen_width / 2 - START_MENU_W / 2;
    int my0 = y_pos - START_MENU_H - 8;

    // Layered shadow for depth
    Graphics::FillRoundedRect(mx0+8, my0+8, START_MENU_W, START_MENU_H, 14, 0x50000000);
    Graphics::FillRoundedRect(mx0+4, my0+4, START_MENU_W, START_MENU_H, 14, 0x30000000);
    // Background — deeper, richer dark
    Graphics::FillRoundedRect(mx0, my0, START_MENU_W, START_MENU_H, 14, 0xF0121220);
    // Border with subtle accent
    Graphics::DrawRect(mx0, my0, START_MENU_W, START_MENU_H, 0xFF2A2A48);

    // Header with accent gradient
    Graphics::FillRoundedRect(mx0+1, my0+1, START_MENU_W-2, 54, 14, 0xFF161630);
    Graphics::FillRect(mx0+1, my0+42, START_MENU_W-2, 13, 0xFF161630);
    // Kurono branding
    Graphics::DrawString(mx0+16, my0+12, "Kurono OS", 0xFFF0F0FF, 0xFF000000);
    Graphics::DrawString(mx0+16, my0+30, "v1.0", 0xFF7070A0, 0xFF000000);
    // Accent underline
    Graphics::FillRect(mx0+16, my0+52, START_MENU_W-32, 1, 0xFF5C8AFF);

    // Menu items with icons
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
        " Shutdown",
    };
    // Item accent colors (left indicator)
    static const unsigned int item_accents[] = {
        0xFF2ECC71, 0xFFF39C12, 0xFF9B59B6, 0xFFE74C3C,
        0xFF3498DB, 0xFFE91E63, 0xFF607D8B, 0xFF00BCD4,
        0, 0xFF5C8AFF, 0xFFFF5252,
    };
    static const int nit = 11;

    int iy = my0 + 58;
    for(int i=0;i<nit;i++){
        if(items[i][0]=='-'){
            // Separator — thin accent line
            Graphics::FillRect(mx0+16, iy+6, START_MENU_W-32, 1, 0xFF2A2A48);
            iy += 12;
            continue;
        }
        // Left accent indicator bar
        if (item_accents[i]) {
            Graphics::FillRoundedRect(mx0+8, iy+4, 3, 20, 1, item_accents[i]);
        }
        Graphics::DrawString(mx0+20, iy+4, items[i], COL_ICON_TEXT, 0xFF000000);
        iy += 32;
    }
}

void Taskbar::Render(){
    // Frosted glass taskbar with layered depth — acrylic-like effect
    // Dark base
    Graphics::FillRect(0, y_pos, screen_width, TASKBAR_HEIGHT, 0xE8080814);
    // Subtle noise/texture via alternating pixel rows
    for (int rx = 0; rx < screen_width; rx += 3) {
        Graphics::FillRect(rx, y_pos + 2, 1, TASKBAR_HEIGHT - 2, 0x06FFFFFF);
    }
    // Top accent line — subtle gradient feel
    Graphics::FillRect(0, y_pos, screen_width, 1, 0xFF2A2A48);
    // Inner glow line
    Graphics::FillRect(0, y_pos + 1, screen_width, 1, 0x10FFFFFF);

    RenderStartButton();
    RenderTaskButtons();
    RenderSystemTray();
    RenderClock();
    RenderStartMenu();
    RenderVolumePopup();

    // Search results dropdown when active
    if (search_active && search_buf[0]) {
        RenderSearchResults();
    }
}

void Taskbar::RenderSearchResults(){
    // Dropdown below search box showing matching apps
    static const char* app_names[] = {
        "Terminal", "Files", "Calculator", "Editor",
        "Settings", "Browser", "Media Player", "Task Manager"
    };
    static const int app_count = 8;

    int sx = 8, sy = y_pos - 8;
    int sw = 220, max_items = 0;

    // Count matches
    for(int i=0; i<app_count; i++){
        // Simple prefix match (case-insensitive first char)
        char fc = search_buf[0];
        if(fc >= 'A' && fc <= 'Z') fc += 32;
        char ac = app_names[i][0];
        if(ac >= 'A' && ac <= 'Z') ac += 32;
        if(fc == ac) max_items++;
    }
    if(max_items == 0) max_items = app_count; // show all if no match

    int sh = max_items * 30 + 12;
    sy -= sh;

    // Shadow + background
    Graphics::FillRoundedRect(sx+4, sy+4, sw, sh, 10, 0x50000000);
    Graphics::FillRoundedRect(sx, sy, sw, sh, 10, 0xF0141424);
    Graphics::DrawRect(sx, sy, sw, sh, 0xFF2A2A48);

    int iy = sy + 6;
    for(int i=0; i<app_count; i++){
        char fc = search_buf[0];
        if(fc >= 'A' && fc <= 'Z') fc += 32;
        char ac = app_names[i][0];
        if(ac >= 'A' && ac <= 'Z') ac += 32;
        if(search_buf[0] && fc != ac) continue;

        Graphics::DrawString(sx+12, iy+6, app_names[i], 0xFFD0D0E0, 0xFF000000);
        iy += 30;
    }
}

void Taskbar::RenderVolumePopup(){
    if(!volume_popup_open) return;

    // Popup positioned above the volume icon in the system tray
    // Volume icon is at approximately screen_width - 148
    int pop_w = 52;
    int pop_h = 180;
    int pop_x = screen_width - 148 - pop_w/2 + 10;
    int pop_y = y_pos - pop_h - 8;

    // Shadow
    Graphics::FillRoundedRect(pop_x+4, pop_y+4, pop_w, pop_h, 8, 0x60000000);
    // Background
    Graphics::FillRoundedRect(pop_x, pop_y, pop_w, pop_h, 8, 0xF0181828);
    // Border
    Graphics::DrawRect(pop_x, pop_y, pop_w, pop_h, 0xFF333355);

    // Mute/unmute icon at top
    int icon_cx = pop_x + pop_w/2;
    bool muted = Audio::IsAvailable() ? Audio::IsMuted() : false;
    if(muted){
        Graphics::DrawString(icon_cx-8, pop_y+8, "XX", 0xFFE74C3C, 0xFF000000);
    } else {
        // Speaker icon
        Graphics::FillRect(icon_cx-5, pop_y+12, 4, 8, COL_WHITE);
        Graphics::FillRect(icon_cx-1, pop_y+9, 3, 14, COL_WHITE);
        Graphics::DrawCircle(icon_cx+7, pop_y+16, 4, COL_WHITE);
    }

    // Vertical slider track
    int track_x = pop_x + pop_w/2 - 2;
    int track_y = pop_y + 36;
    int track_h = 110;
    // Track background
    Graphics::FillRect(track_x, track_y, 4, track_h, 0xFF333355);

    // Filled portion (bottom = 0%, top = 100%)
    int fill_h = (volume_slider_val * track_h) / 100;
    if(fill_h > 0){
        Graphics::FillRect(track_x, track_y + track_h - fill_h, 4, fill_h, 0xFF5C8AFF);
    }

    // Slider knob
    int knob_y = track_y + track_h - fill_h - 6;
    if(knob_y < track_y - 6) knob_y = track_y - 6;
    Graphics::FillCircle(track_x + 2, knob_y + 6, 7, COL_WHITE);
    Graphics::DrawCircle(track_x + 2, knob_y + 6, 7, 0xFF5C8AFF);

    // Volume percentage text
    char vol_str[8];
    int_to_str(volume_slider_val, vol_str, 8);
    sapp(vol_str, "%", 8);
    int tw = slen(vol_str) * 8;
    Graphics::DrawString(icon_cx - tw/2, pop_y + pop_h - 20, vol_str, COL_TRAY_TEXT, 0xFF000000);
}

bool Taskbar::HandleClick(int mx,int my){
    // ── Volume popup click handling ──
    if(volume_popup_open){
        int pop_w = 52;
        int pop_h = 180;
        int pop_x = screen_width - 148 - pop_w/2 + 10;
        int pop_y = y_pos - pop_h - 8;

        if(mx>=pop_x && mx<pop_x+pop_w && my>=pop_y && my<pop_y+pop_h){
            // Mute toggle area (top 32px)
            if(my < pop_y + 32){
                if(Audio::IsAvailable()){
                    Audio::SetMuted(!Audio::IsMuted());
                }
                return true;
            }
            // Slider track area
            int track_y = pop_y + 36;
            int track_h = 110;
            if(my >= track_y - 6 && my <= track_y + track_h + 6){
                // Calculate volume from click position
                int rel = track_y + track_h - my;
                int vol = (rel * 100) / track_h;
                if(vol < 0) vol = 0;
                if(vol > 100) vol = 100;
                volume_slider_val = vol;
                if(Audio::IsAvailable()){
                    Audio::SetMasterVolume(vol);
                    Audio::SetMuted(false);
                }
                volume_slider_dragging = true;
            }
            return true;
        }
        // Click outside popup closes it
        volume_popup_open = false;
        volume_slider_dragging = false;
    }

    // Check if click is in taskbar area
    if(my < y_pos) {
        // If start menu is open and click is in its area, handle it
        if(start_menu_open){
            int mx0 = screen_width / 2 - START_MENU_W / 2;
            int my0 = y_pos - START_MENU_H - 8;
            if(mx>=mx0 && mx<mx0+START_MENU_W && my>=my0 && my<my0+START_MENU_H){
                // Determine which item was clicked
                int iy = my0 + 56;
    static const int nit = 11;
        for(int i=0;i<nit;i++){
                    // separator
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
                            case 9: /* restart shell */
                                KuronoShell::Init();
                                TerminalApp::Init();
                                DesktopEnvironment::LaunchTerminal();
                                break;
                            case 10: /* shutdown */ break;
                        }
                        return true;
                    }
                    iy+=32;
                }
                return true;
            }
            start_menu_open=false;
        }
        return false;
    }

    // Start button (truly centered)
    int btn_w = 38;
    int center_x = screen_width / 2 - btn_w / 2;
    if(mx>=center_x && mx<center_x+btn_w && my>=y_pos+4 && my<y_pos+TASKBAR_HEIGHT-4){
        start_menu_open = !start_menu_open;
        return true;
    }

    // Task buttons — centered, after start button
    {
        int btn_w_k2 = 38;
        int cx2 = screen_width / 2 - btn_w_k2 / 2;
        int x = cx2 + btn_w_k2 + 8;
        for(int i=0;i<WindowManager::GetWindowCount();i++){
            Window* w = WindowManager::GetWindow(i);
            if(!w || w->state==WIN_CLOSED) continue;
            int bw = 36;
            if(mx>=x && mx<x+bw){
                if(w->state==WIN_MINIMIZED){
                    w->state=WIN_NORMAL;
                }
                WindowManager::BringToFront(i);
                return true;
            }
            x += bw + 4;
            if(x > screen_width-200) break;
        }
    }

    // Search box click — activate search
    if(mx>=8 && mx<208 && my>=y_pos+7 && my<y_pos+TASKBAR_HEIGHT-7){
        search_active = true;
        return true;
    }

    // Volume icon click (system tray area: ~screen_width - 148 to -126)
    int vol_icon_x = screen_width - 148;
    if(mx >= vol_icon_x && mx < vol_icon_x + 22 && my >= y_pos){
        volume_popup_open = !volume_popup_open;
        volume_slider_dragging = false;
        if(volume_popup_open && Audio::IsAvailable()){
            volume_slider_val = Audio::GetMasterVolume();
        }
        return true;
    }

    start_menu_open=false;
    volume_popup_open=false;
    return true;
}

void Taskbar::Update(){
    // Update clock from real RTC-backed time
    DateTime dt = TimeManager::NowLocalDateTime();
    clock_h = dt.h;
    clock_m = dt.m;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Desktop (icons + wallpaper)
// ═══════════════════════════════════════════════════════════════════════════
int           Desktop::screen_width    = 0;
int           Desktop::screen_height   = 0;
unsigned int  Desktop::wallpaper_color = COL_DESK_BG;
DesktopIcon   Desktop::icons[DESKTOP_MAX_ICONS];
int           Desktop::icon_count      = 0;
int           Desktop::selected_icon   = -1;
bool          Desktop::context_menu_open = false;
int           Desktop::context_menu_x  = 0;
int           Desktop::context_menu_y  = 0;
uint32_t*     Desktop::gradient_cache  = nullptr;
int           Desktop::gradient_cache_h = 0;
bool          Desktop::have_image_wallpaper = false;

void Desktop::Init(int sw,int sh){
    screen_width=sw; screen_height=sh;
    wallpaper_color=COL_DESK_BG;
    icon_count=0; selected_icon=-1;
    context_menu_open=false;

    // Default desktop icons
    AddIcon("Terminal",    "/usr/bin/terminal",      2);
    AddIcon("Files",       "/usr/bin/filebrowser",   2);
    AddIcon("Calculator",  "/usr/bin/calc",          2);
    AddIcon("Editor",      "/usr/bin/editor",        2);
    AddIcon("Settings",    "/usr/bin/settings",      2);
    AddIcon("Home",        "/home/user",             1);
    AddIcon("Browser",     "/usr/bin/browser",       2);
    AddIcon("Media",       "/usr/bin/mediaplayer",   2);

    ArrangeIcons();
}

void Desktop::AddIcon(const char* name,const char* path,int tp){
    if(icon_count>=DESKTOP_MAX_ICONS)return;
    DesktopIcon* ic=&icons[icon_count++];
    scpy(ic->name,name,32);
    scpy(ic->path,path,64);
    ic->icon_type=tp;
    ic->selected=false;
    ic->x=0; ic->y=0;
}

void Desktop::ArrangeIcons(){
    int gx = ICON_MARGIN_X;
    int gy = ICON_MARGIN_Y;
    int col_h = screen_height - TASKBAR_HEIGHT - ICON_MARGIN_Y - 40;
    for(int i=0;i<icon_count;i++){
        icons[i].x = gx;
        icons[i].y = gy;
        gy += ICON_SPACING_Y;
        if(gy > col_h){
            gy = ICON_MARGIN_Y;
            gx += ICON_SPACING_X;
        }
    }
}

void Desktop::SetWallpaper(unsigned int c){ wallpaper_color=c; }

void Desktop::SetWallpaperImage(const MediaDecoder::Image& img){
    if (!img.valid || !img.data || img.width <= 0 || img.height <= 0) return;
    int w = screen_width;
    int h = screen_height - TASKBAR_HEIGHT;
    if (w <= 0 || h <= 0) return;

    // Allocate full-screen cache
    if (gradient_cache) KernelHeap::Free(gradient_cache);
    int total = w * h;
    gradient_cache = (uint32_t*)KernelHeap::Alloc(total * sizeof(uint32_t));
    if (!gradient_cache) return;
    gradient_cache_h = h;
    have_image_wallpaper = true;

    // Scale image to screen using nearest-neighbor sampling
    for (int y = 0; y < h; y++) {
        int src_y = (y * img.height) / h;
        if (src_y >= img.height) src_y = img.height - 1;
        for (int x = 0; x < w; x++) {
            int src_x = (x * img.width) / w;
            if (src_x >= img.width) src_x = img.width - 1;
            int idx = (src_y * img.width + src_x);
            uint8_t* p = img.data + idx * 4;
            uint8_t r, g, b;
            if (img.order == 1) { // BGRA
                b = p[0]; g = p[1]; r = p[2];
            } else { // RGBA
                r = p[0]; g = p[1]; b = p[2];
            }
            gradient_cache[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

void Desktop::RenderWallpaper(){
    int h = screen_height - TASKBAR_HEIGHT;
    if (h <= 0) return;
    int w = screen_width;

    // If we have an image wallpaper, skip procedural generation
    if (have_image_wallpaper && gradient_cache && gradient_cache_h == h) {
        // Fast blit image wallpaper
        uint8_t* buf = Graphics::GetBackBuffer();
        if (!buf) buf = Graphics::GetBuffer();
        uint32_t pitch = Graphics::GetPitch();
        for (int y = 0; y < h; y++) {
            uint32_t* dst = (uint32_t*)(buf + y * pitch);
            uint32_t* src = gradient_cache + y * w;
            for (int i = 0; i < w; i++) dst[i] = src[i];
        }
        return;
    }

    // Build gradient cache once - warm modern abstract gradient (fallback)
    if (!gradient_cache || gradient_cache_h != h) {
        // Allocate full screen cache (w * h pixels)
        int total = w * h;
        if (!gradient_cache || gradient_cache_h != h) {
            if (gradient_cache) KernelHeap::Free(gradient_cache);
            gradient_cache = (uint32_t*)KernelHeap::Alloc(total * sizeof(uint32_t));
        }
        gradient_cache_h = h;
        if (!gradient_cache) return;

        for (int y = 0; y < h; y++) {
            float ty = (float)y / (float)(h > 1 ? h - 1 : 1);
            for (int x = 0; x < w; x++) {
                float tx = (float)x / (float)(w > 1 ? w - 1 : 1);

                // Base: deep midnight blue-purple
                float r = 8.0f, g = 5.0f, b = 22.0f;

                // Warm orb (upper-right) — amber/orange glow
                float dx1 = tx - 0.78f;
                float dy1 = ty - 0.18f;
                float d1 = dx1*dx1 + dy1*dy1;
                float orb1 = 1.0f / (1.0f + d1 * 6.0f);
                r += 200.0f * orb1;
                g += 110.0f * orb1;
                b += 25.0f * orb1;

                // Cool orb (lower-left) — deep blue glow
                float dx2 = tx - 0.15f;
                float dy2 = ty - 0.82f;
                float d2 = dx2*dx2 + dy2*dy2;
                float orb2 = 1.0f / (1.0f + d2 * 8.0f);
                r += 40.0f * orb2;
                g += 30.0f * orb2;
                b += 140.0f * orb2;

                // Accent orb (center) — soft magenta/pink
                float dx3 = tx - 0.5f;
                float dy3 = ty - 0.42f;
                float d3 = dx3*dx3 + dy3*dy3;
                float orb3 = 1.0f / (1.0f + d3 * 12.0f);
                r += 90.0f * orb3;
                g += 25.0f * orb3;
                b += 70.0f * orb3;

                // Aurora ribbon (upper-center) — teal/cyan sweep
                float dx4 = tx - 0.4f;
                float dy4 = ty - 0.1f;
                float d4 = dx4*dx4*0.5f + dy4*dy4*4.0f;
                float orb4 = 1.0f / (1.0f + d4 * 20.0f);
                r += 10.0f * orb4;
                g += 80.0f * orb4;
                b += 90.0f * orb4;

                // Subtle warm accent (lower-right)
                float dx5 = tx - 0.85f;
                float dy5 = ty - 0.7f;
                float d5 = dx5*dx5 + dy5*dy5;
                float orb5 = 1.0f / (1.0f + d5 * 16.0f);
                r += 60.0f * orb5;
                g += 20.0f * orb5;
                b += 50.0f * orb5;

                // Vignette — darken edges for depth
                float vx = (tx - 0.5f) * 2.0f;
                float vy = (ty - 0.5f) * 2.0f;
                float vignette = 1.0f - (vx*vx + vy*vy) * 0.15f;
                if (vignette < 0.6f) vignette = 0.6f;
                r *= vignette;
                g *= vignette;
                b *= vignette;

                // Subtle dither noise (deterministic based on position)
                int noise = ((x * 7 + y * 13) & 7) - 4;
                r += (float)noise * 0.5f;
                g += (float)noise * 0.4f;
                b += (float)noise * 0.5f;

                // Clamp
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

    // Fast blit: direct memcpy from cache to back buffer
    uint8_t* buf = Graphics::GetBackBuffer();
    if (!buf) buf = Graphics::GetBuffer();
    uint32_t pitch = Graphics::GetPitch();
    for (int y = 0; y < h; y++) {
        uint32_t* dst = (uint32_t*)(buf + y * pitch);
        uint32_t* src = gradient_cache + y * w;
        for (int i = 0; i < w; i++) dst[i] = src[i];
    }
}

void Desktop::RenderIcon(DesktopIcon* ic){
    int ix=ic->x, iy=ic->y;
    int icon_sz = ICON_SIZE;

    // Selection highlight — glowing aura
    if(ic->selected){
        Graphics::FillRoundedRect(ix-10, iy-10, icon_sz+20, icon_sz+34, 12, 0x20FFFFFF);
        Graphics::FillRoundedRect(ix-8, iy-8, icon_sz+16, icon_sz+30, 10, 0x305C8AFF);
        Graphics::FillRoundedRect(ix-6, iy-6, icon_sz+12, icon_sz+26, 8, COL_ICON_SEL);
    }

    // Determine icon colors from name
    unsigned int ic_top, ic_bot, ic_glow;
    const char* nm = ic->name;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') {
        ic_top=0xFF34D058; ic_bot=0xFF1E8C3A; ic_glow=0x2034D058; // Terminal: green
    } else if(nm[0]=='F' && nm[1]=='i') {
        ic_top=0xFFFFA726; ic_bot=0xFFE08A1E; ic_glow=0x20FFA726; // Files: amber
    } else if(nm[0]=='C') {
        ic_top=0xFF42A5F5; ic_bot=0xFF1E7BC8; ic_glow=0x2042A5F5; // Calculator: blue
    } else if(nm[0]=='E') {
        ic_top=0xFFAB47BC; ic_bot=0xFF7B2D8E; ic_glow=0x20AB47BC; // Editor: purple
    } else if(nm[0]=='S') {
        ic_top=0xFF90A4AE; ic_bot=0xFF607D8B; ic_glow=0x2090A4AE; // Settings: steel
    } else if(nm[0]=='H') {
        ic_top=0xFFFFA726; ic_bot=0xFFE08A1E; ic_glow=0x20FFA726; // Home: amber
    } else if(nm[0]=='B') {
        ic_top=0xFF3498DB; ic_bot=0xFF2176AE; ic_glow=0x203498DB; // Browser: blue
    } else if(nm[0]=='M') {
        ic_top=0xFFE91E63; ic_bot=0xFFBE164F; ic_glow=0x20E91E63; // Media: pink
    } else {
        ic_top=0xFF42A5F5; ic_bot=0xFF1E7BC8; ic_glow=0x2042A5F5;
    }

    // Soft glow beneath icon
    Graphics::FillRoundedRect(ix-3, iy-2, icon_sz+6, icon_sz+6, 16, ic_glow);

    // Drop shadow — softer, offset
    Graphics::FillRoundedRect(ix+3, iy+4, icon_sz, icon_sz, 14, 0x50000000);

    // Icon body — rounded square with gradient effect
    Graphics::FillRoundedRect(ix, iy, icon_sz, icon_sz, 13, ic_top);
    // Bottom gradient half
    Graphics::FillRoundedRect(ix, iy + icon_sz/2, icon_sz, icon_sz/2, 13, ic_bot);
    // Smooth blend zone
    Graphics::FillRect(ix + 4, iy + icon_sz/2, icon_sz - 8, 6, ic_top);

    // Inner highlight (top glossy shine)
    Graphics::FillRect(ix + 5, iy + 3, icon_sz - 10, 2, 0x40FFFFFF);
    Graphics::FillRect(ix + 6, iy + 5, icon_sz - 12, 1, 0x20FFFFFF);

    // Icon-specific symbols
    int cx = ix + icon_sz/2;
    int cy_icon = iy + icon_sz/2;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') {
        Graphics::DrawString(cx-10, cy_icon-6, ">_", 0xFFFFFFFF, 0xFF000000);
    } else if(nm[0]=='F' && nm[1]=='i') {
        // Folder
        Graphics::FillRoundedRect(ix+12, iy+16, 28, 20, 3, 0xFFFFCC80);
        Graphics::FillRect(ix+12, iy+16, 14, 5, 0xFFE0A050);
    } else if(nm[0]=='C') {
        // Calculator grid
        Graphics::FillRect(ix+12, iy+10, 28, 8, 0xFF1E88E5);
        for(int r=0;r<2;r++) for(int c=0;c<3;c++)
            Graphics::FillRoundedRect(ix+14+c*9, iy+22+r*9, 7, 7, 2, 0xFFFFFFFF);
    } else if(nm[0]=='E') {
        // Text lines
        Graphics::FillRect(ix+14, iy+14, 24, 3, 0xFFEEEEEE);
        Graphics::FillRect(ix+14, iy+20, 18, 3, 0xFFCCCCCC);
        Graphics::FillRect(ix+14, iy+26, 22, 3, 0xFFEEEEEE);
        Graphics::FillRect(ix+14, iy+32, 14, 3, 0xFFCCCCCC);
    } else if(nm[0]=='S') {
        // Gear
        Graphics::FillCircle(cx, cy_icon, 14, 0xFF78909C);
        Graphics::FillCircle(cx, cy_icon, 8, 0xFFCFD8DC);
        Graphics::FillCircle(cx, cy_icon, 4, 0xFF78909C);
    } else if(nm[0]=='H') {
        // House
        Graphics::FillRect(ix+16, iy+24, 20, 14, 0xFFFFCC80);
        Graphics::FillRect(ix+12, iy+20, 28, 4, 0xFFE0A050);
        Graphics::FillRect(ix+16, iy+16, 20, 4, 0xFFE0A050);
    }

    // Label — centered below icon with background pill for readability
    char lbl[14]; scpy(lbl, ic->name, 13);
    int tw=slen(lbl)*8;
    int tx=ix + icon_sz/2 - tw/2;
    if(tx<ix-8)tx=ix-8;
    // Background pill
    Graphics::FillRoundedRect(tx-4, iy+icon_sz+1, tw+8, 14, 4, 0x80000000);
    // Text
    Graphics::DrawString(tx, iy+icon_sz+3, lbl, COL_ICON_TEXT, 0xFF000000);
}

void Desktop::RenderContextMenu(){
    if(!context_menu_open) return;
    static const char* items[] = {
        "Open", "New Folder", "New File", "Refresh", "Settings"
    };
    int nit=5;
    int mw=180, mh=nit*30+12;

    // Shadow
    Graphics::FillRoundedRect(context_menu_x+5, context_menu_y+5, mw, mh, 8, 0x50000000);
    // Background
    Graphics::FillRoundedRect(context_menu_x, context_menu_y, mw, mh, 8, 0xF0121220);
    // Accent border
    Graphics::DrawRect(context_menu_x, context_menu_y, mw, mh, 0xFF2A2A48);

    int iy=context_menu_y+6;
    for(int i=0;i<nit;i++){
        // Subtle divider between items (except first)
        if (i > 0) {
            Graphics::FillRect(context_menu_x+12, iy-1, mw-24, 1, 0xFF1A1A30);
        }
        Graphics::DrawString(context_menu_x+16, iy+6, items[i], COL_ICON_TEXT, 0xFF000000);
        iy+=30;
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
    if(my>=screen_height-TASKBAR_HEIGHT) return false;

    context_menu_open=false;

    // Deselect all
    for(int i=0;i<icon_count;i++) icons[i].selected=false;
    selected_icon=-1;

    int idx=IconAt(mx,my);
    if(idx>=0){
        icons[idx].selected=true;
        selected_icon=idx;
        return true;
    }
    return false;
}

void Desktop::HandleDoubleClick(int mx,int my){
    int idx=IconAt(mx,my);
    if(idx<0) return;

    // Launch app based on icon name
    const char* nm = icons[idx].name;
    if(nm[0]=='T' && nm[1]=='e' && nm[2]=='r') DesktopEnvironment::LaunchTerminal();
    else if(nm[0]=='F' && nm[1]=='i') DesktopEnvironment::LaunchFileBrowser();
    else if(nm[0]=='C') DesktopEnvironment::LaunchCalculator();
    else if(nm[0]=='E') DesktopEnvironment::LaunchTextEditor();
    else if(nm[0]=='S') DesktopEnvironment::LaunchSettings();
    else if(nm[0]=='H') DesktopEnvironment::LaunchFileBrowser();
    else if(nm[0]=='B') DesktopEnvironment::LaunchBrowser();
    else if(nm[0]=='M') DesktopEnvironment::LaunchMediaPlayer();
}

void Desktop::HandleRightClick(int mx,int my){
    context_menu_open=true;
    context_menu_x=mx;
    context_menu_y=my;
}

void Desktop::Update(int mx,int my,bool mouse_down,bool clicked){
    (void)mx;(void)my;(void)mouse_down;(void)clicked;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DesktopEnvironment
// ═══════════════════════════════════════════════════════════════════════════
void DesktopEnvironment::Init(int sw,int sh){
    WindowManager::Init(sw, sh);
    Desktop::Init(sw, sh);
    Taskbar::Init(sw, sh);
    // Adjust WM desktop area to exclude taskbar
    WindowManager::SetDesktopArea(0, 0, sw, sh - TASKBAR_HEIGHT);
}

void DesktopEnvironment::Render(){
    Desktop::Render();
    WindowManager::RenderAll();
    Taskbar::Render();
}

// Double-click tracking state
static uint32_t last_click_time_ms = 0;
static int last_click_x = -1, last_click_y = -1;
static uint32_t de_frame_counter = 0;

void DesktopEnvironment::HandleInput(int mx,int my,bool mouse_down,bool clicked,char key){
    // Advance frame counter for timing (each frame ~6ms)
    de_frame_counter++;

    // Right-click handling — forward event 4 to focused window
    bool right_clicked = Mouse::RightClicked();
    if(right_clicked){
        // Check if click is on a window first
        Window* fw = WindowManager::GetFocusedWindow();
        if(fw && fw->visible && fw->state != WIN_CLOSED &&
           mx >= fw->content_x && mx < fw->content_x + fw->content_w &&
           my >= fw->content_y && my < fw->content_y + fw->content_h) {
            if(fw->input){
                fw->input(fw, 4, mx, my);  // event 4 = right-click
            }
        } else {
            // Desktop right-click
            Desktop::HandleRightClick(mx, my);
        }
    }

    if(clicked){
        // Double-click detection: two clicks within ~400ms at same spot (real time)
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

        // Taskbar gets first priority
        if(Taskbar::HandleClick(mx,my)) return;
        // Then window manager
        if(WindowManager::HandleMouseDown(mx,my)) return;
        // Desktop — single click selects AND launches the app
        if(is_double){
            Desktop::HandleDoubleClick(mx,my);
        } else {
            if(Desktop::HandleClick(mx,my)){
                // If an icon was clicked, also launch it immediately
                Desktop::HandleDoubleClick(mx,my);
            }
        }
    }
    if(mouse_down){
        // Volume slider dragging
        if(Taskbar::volume_popup_open && Taskbar::volume_slider_dragging){
            int pop_w = 52;
            int pop_h = 180;
            int pop_x = Taskbar::screen_width - 148 - pop_w/2 + 10;
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
    // Forward keyboard input to focused window or search
    if(key){
        // If search is active, route keys there
        if(Taskbar::search_active){
            if(key == '\n' || key == '\r'){
                // Execute search — launch matching app
                Taskbar::search_active = false;
                const char* sb = Taskbar::search_buf;
                if(sb[0]=='t' || sb[0]=='T') DesktopEnvironment::LaunchTerminal();
                else if(sb[0]=='f' || sb[0]=='F') DesktopEnvironment::LaunchFileBrowser();
                else if(sb[0]=='c' || sb[0]=='C') DesktopEnvironment::LaunchCalculator();
                else if(sb[0]=='e' || sb[0]=='E') DesktopEnvironment::LaunchTextEditor();
                else if(sb[0]=='s' || sb[0]=='S') DesktopEnvironment::LaunchSettings();
                else if(sb[0]=='b' || sb[0]=='B') DesktopEnvironment::LaunchBrowser();
                else if(sb[0]=='m' || sb[0]=='M') DesktopEnvironment::LaunchMediaPlayer();
                else DesktopEnvironment::LaunchTerminal();
                Taskbar::search_buf[0] = 0;
                Taskbar::search_len = 0;
            } else if(key == 27){ // Escape
                Taskbar::search_active = false;
                Taskbar::search_buf[0] = 0;
                Taskbar::search_len = 0;
            } else if(key == 8 || key == 127){ // Backspace
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
}

// ─── App launchers \u2014 connect to real app Open() methods ───

void DesktopEnvironment::LaunchTerminal(){
    TerminalApp::Open();
}
void DesktopEnvironment::LaunchFileBrowser(){
    FileManagerApp::Open();
}
void DesktopEnvironment::LaunchCalculator(){
    // The calculator uses a standalone Draw/Update API, not WM windows.
    // Create a WM window that wraps it.
    Calculator::Init(0, 0);
    Calculator::SetActive(true);
    int wid = WindowManager::CreateWindow("Calculator", -1, -1, 200, 250,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) {
            (void)w;
            // Adjust calculator to render into the WM content area
            Calculator::SetPosition(cx, cy);
            Calculator::SetSize(cw, ch);
            Calculator::Draw();
        },
        (WindowInputFunc)[](Window* w, int ev, int p1, int p2) {
            (void)w; (void)ev; (void)p1; (void)p2;
            // Mouse events handled by Calculator::Update via WM
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
