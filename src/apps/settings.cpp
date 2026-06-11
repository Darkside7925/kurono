// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Settings Application Implementation
// ═══════════════════════════════════════════════════════════════════════════
#include "settings.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../drivers/audio.h"
#include "../ui/gui.h"
#include "../ui/desktop.h"
#include "../net/network.h"
#include "../security/supr.h"
#include "../packages/pkgmgr.h"
#include "../kernel/heap.h"

// ────────────── colours ──────────────
static const unsigned int S_BG       = 0xFF1A1A2E;
static const unsigned int S_SIDEBAR  = 0xFF16213E;
static const unsigned int S_TAB_SEL  = 0xFF0F3460;
static const unsigned int S_TAB_TXT  = 0xFFCCCCCC;
static const unsigned int S_HEADING  = 0xFF00D2FF;
static const unsigned int S_TEXT     = 0xFFE0E0E0;
static const unsigned int S_DIM      = 0xFF999999;
static const unsigned int S_TOGGLE_ON  = 0xFF00C853;
static const unsigned int S_TOGGLE_OFF = 0xFF444466;
static const unsigned int S_SLIDER_BG  = 0xFF333355;
static const unsigned int S_SLIDER_FG  = 0xFF00D2FF;
static const unsigned int S_BORDER   = 0xFF333355;
static const unsigned int S_WHITE    = 0xFFFFFFFF;
static const unsigned int S_ACCENT   = 0xFF533483;

static const int SIDEBAR_W = 130;

// ────────────── helpers ──────────────
static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static void int_to_str(int v,char*b,int mx){
    if(mx<2){b[0]=0;return;}if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}

// ────────────── static data ──────────────
SettingsTab SettingsApp::current_tab  = STAB_DISPLAY;
int         SettingsApp::scroll_offset = 0;
SettingsState SettingsApp::state = {
    /* brightness */ 75, /* cursor_blink */ true, /* dark_mode */ true,
    /* animations */ true, /* font_scale */ 1, /* wallpaper_idx */ 0,
    /* resolution_idx */ 0, /* refresh_rate */ 240,
    /* master_volume */ 80, /* alert_volume */ 60, /* muted */ false,
    /* output_device */ 0, /* spatial_audio */ false,
    /* wifi_enabled */ false, /* bluetooth_enabled */ false,
    /* wifi_scan_count */ 0, /* bt_scan_count */ 0,
    /* wifi_scanning */ false, /* bt_scanning */ false,
    /* power_plan */ 0, /* sleep_timeout */ 30, /* screen_timeout */ 10,
    /* fast_startup */ true,
    /* accent_color */ 0, /* show_desktop_icons */ true, /* taskbar_pos */ 0,
    /* icon_size */ 1, /* transparency */ true,
    /* auto_update */ true, /* update_status */ 0, /* update_progress */ 0,
    /* last_check_mins */ 42
};

int SettingsApp::pending_resolution_idx = -1;

// ── Resolution/refresh apply helpers ──
// Schedule resolution change — applied between frames by the main loop
static void RequestResolution(int idx) {
    SettingsApp::pending_resolution_idx = idx;
}

// Actually performs the resolution switch (called from PollDeferredActions)
static void DoApplyResolution(int idx) {
    static const int res_w[] = {1024, 1920, 2560};
    static const int res_h[] = { 768, 1080, 1440};
    if (idx < 0 || idx > 2) return;
    int w = res_w[idx], h = res_h[idx];

    // Close all windows first — their content areas reference old buffers
    WindowManager::CloseAll();

    // Change BGA hardware mode
    if (!BGA::SetMode(w, h, 32)) {
        // Mode change failed — fall back to 1024x768
        BGA::SetMode(1024, 768, 32);
        w = 1024; h = 768;
        SettingsApp::state.resolution_idx = 0;
    }

    // Clear the framebuffer immediately so no stale data shows
    uint8_t* fb = BGA::GetFramebuffer();
    if (fb) memset(fb, 0, (uint32_t)w * h * 4);

    // Reset Graphics — also handles freeing old back_buffer
    Graphics::ReinitForResolution(
        (uintptr_t)BGA::GetFramebuffer(), w, h, w*4, 32);

    // Apply the current refresh rate target
    if (SettingsApp::state.refresh_rate > 0)
        Graphics::SetTargetFPS((uint32_t)SettingsApp::state.refresh_rate);

    // Reinit GUI buffers (free old, alloc new, re-render wallpaper)
    GUI::ReinitBuffers();

    // Reinit desktop/taskbar for new screen size + re-scale wallpaper
    Desktop::Init(w, h);
    if (GUI::wallpaper.valid) {
        Desktop::SetWallpaperImage(GUI::wallpaper);
    }
    Taskbar::Init(w, h);
}

void SettingsApp::PollDeferredActions() {
    if (pending_resolution_idx >= 0) {
        int idx = pending_resolution_idx;
        pending_resolution_idx = -1;
        DoApplyResolution(idx);
    }
}

static void ApplyRefreshRate(int hz) {
    if (hz > 0) Graphics::SetTargetFPS((uint32_t)hz);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Init / Open
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::Init(){
    current_tab=STAB_DISPLAY;
    scroll_offset=0;
    // state keeps its values between opens
}

int SettingsApp::Open(){
    Init();
    int wid = WindowManager::CreateWindow("Settings", -1, -1, 580, 460,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            SettingsApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) SettingsApp::Input(w,p1,p2,true,0);
            else if(ev==2) SettingsApp::Input(w,0,0,false,(char)p1);
        }
    );
    return wid;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sidebar
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderSidebar(int x,int y,int w,int h){
    (void)w;
    Graphics::FillRect(x,y,SIDEBAR_W,h,S_SIDEBAR);
    Graphics::DrawLine(x+SIDEBAR_W-1,y,x+SIDEBAR_W-1,y+h,S_BORDER);

    static const char* tabs[] = {
        "Display", "Sound", "Network", "Storage",
        "Power", "Personal", "Security", "Packages",
        "Updates", "About"
    };

    for(int i=0;i<STAB_COUNT;i++){
        int ty = y + i * 36 + 8;
        if(i==(int)current_tab){
            Graphics::FillRect(x, ty, SIDEBAR_W-1, 32, S_TAB_SEL);
            Graphics::FillRect(x, ty, 3, 32, S_HEADING);
        }
        Graphics::DrawString(x+16, ty+8, tabs[i], S_TAB_TXT, 0xFF000000);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tab panels
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderDisplay(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Display",S_HEADING,0xFF000000);
    ly+=26;

    // Resolution selector
    Graphics::DrawString(x+12,ly,"Resolution:",S_TEXT,0xFF000000);
    static const char* res_names[] = {"1024 x 768", "1920 x 1080", "2560 x 1440"};
    // Draw left/right arrows for resolution
    Graphics::FillRoundedRect(x+140, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+144, ly, "<", S_WHITE, 0xFF000000);
    Graphics::DrawString(x+168, ly, res_names[state.resolution_idx], S_WHITE, 0xFF000000);
    Graphics::FillRoundedRect(x+w-50, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+w-46, ly, ">", S_WHITE, 0xFF000000);
    ly+=26;

    // Refresh rate
    Graphics::DrawString(x+12,ly,"Refresh Rate:",S_TEXT,0xFF000000);
    char hz_buf[16] = {0};
    int_to_str(state.refresh_rate, hz_buf, 12);
    sapp(hz_buf, " Hz", 16);
    Graphics::FillRoundedRect(x+140, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+144, ly, "<", S_WHITE, 0xFF000000);
    Graphics::DrawString(x+168, ly, hz_buf, S_WHITE, 0xFF000000);
    Graphics::FillRoundedRect(x+w-50, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+w-46, ly, ">", S_WHITE, 0xFF000000);
    ly+=26;

    // Response time
    Graphics::DrawString(x+12,ly,"Response Time:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"0.03 ms",0xFF00C853,0xFF000000);
    ly+=26;

    // Color depth
    Graphics::DrawString(x+12,ly,"Color Depth:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"32-bit True Color",S_DIM,0xFF000000);
    ly+=26;

    // Brightness slider (functional)
    Graphics::DrawString(x+12,ly,"Brightness:",S_TEXT,0xFF000000);
    int slider_x = x + 140;
    int slider_w = w - 180;
    Graphics::FillRoundedRect(slider_x, ly+4, slider_w, 8, 4, S_SLIDER_BG);
    int fill_w = (slider_w * state.brightness) / 100;
    if (fill_w > 0)
        Graphics::FillRoundedRect(slider_x, ly+4, fill_w, 8, 4, S_SLIDER_FG);
    Graphics::FillCircle(slider_x + fill_w, ly+8, 7, S_WHITE);
    char bri[8]; int_to_str(state.brightness, bri, 8);
    sapp(bri, "%", 8);
    Graphics::DrawString(x+w-40, ly, bri, S_DIM, 0xFF000000);
    ly+=30;

    // Cursor Blink toggle (functional)
    Graphics::DrawString(x+12,ly,"Cursor Blink:",S_TEXT,0xFF000000);
    unsigned int tog_col = state.cursor_blink ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    int knob_x = state.cursor_blink ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    ly+=28;

    // Dark Mode toggle (functional)
    Graphics::DrawString(x+12,ly,"Dark Mode:",S_TEXT,0xFF000000);
    tog_col = state.dark_mode ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    knob_x = state.dark_mode ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    ly+=28;

    // Animations toggle (functional)
    Graphics::DrawString(x+12,ly,"Animations:",S_TEXT,0xFF000000);
    tog_col = state.animations ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    knob_x = state.animations ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
}

void SettingsApp::RenderNetwork(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Network & Connectivity",S_HEADING,0xFF000000);
    ly+=28;

    // ── WiFi Section ──
    Graphics::DrawString(x+12,ly,"WiFi",S_TEXT,0xFF000000);
    unsigned int tog_col = state.wifi_enabled ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    int knob_x = state.wifi_enabled ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    Graphics::DrawString(x+190, ly+2, state.wifi_enabled ? "Connected" : "Off", S_DIM, 0xFF000000);
    ly+=28;

    if (state.wifi_enabled) {
        // Scan button
        Graphics::FillRoundedRect(x+12, ly, 80, 22, 6, S_ACCENT);
        Graphics::DrawString(x+20, ly+3, state.wifi_scanning ? "Scanning.." : "Scan WiFi", S_WHITE, 0xFF000000);
        ly+=28;

        // Connected network
        Graphics::DrawString(x+24, ly, "* KuronoNet-5G", 0xFF00C853, 0xFF000000);
        Graphics::DrawString(x+w-80, ly, "-42 dBm", S_DIM, 0xFF000000);
        ly+=18;

        // Scanned networks
        static const char* wifi_names[] = {
            "  Neighbor_WiFi", "  CoffeeShop_Free",
            "  NETGEAR-2.4G",  "  Hidden_Network",
            "  5G-Office",     "  Guest_Access"
        };
        static const char* wifi_sig[] = {
            "-58 dBm", "-65 dBm", "-72 dBm", "-78 dBm", "-81 dBm", "-85 dBm"
        };
        int show = state.wifi_scan_count;
        if (show > 6) show = 6;
        for (int i = 0; i < show && ly < y+h-40; i++) {
            unsigned int lock_clr = (i == 2 || i == 5) ? S_DIM : 0xFF888888;
            const char* lock = (i == 2 || i == 5) ? "Open" : "WPA3";
            Graphics::DrawString(x+24, ly, wifi_names[i], S_DIM, 0xFF000000);
            Graphics::DrawString(x+w-120, ly, lock, lock_clr, 0xFF000000);
            Graphics::DrawString(x+w-60, ly, wifi_sig[i], S_DIM, 0xFF000000);
            ly+=16;
        }
    } else {
        Graphics::DrawString(x+24, ly, "Turn on WiFi to scan", S_DIM, 0xFF000000);
        ly+=20;
    }
    ly+=8;

    // ── Bluetooth Section ──
    Graphics::DrawString(x+12,ly,"Bluetooth",S_TEXT,0xFF000000);
    tog_col = state.bluetooth_enabled ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    knob_x = state.bluetooth_enabled ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    Graphics::DrawString(x+190, ly+2, state.bluetooth_enabled ? "On" : "Off", S_DIM, 0xFF000000);
    ly+=28;

    if (state.bluetooth_enabled) {
        // Scan button
        Graphics::FillRoundedRect(x+12, ly, 90, 22, 6, S_ACCENT);
        Graphics::DrawString(x+20, ly+3, state.bt_scanning ? "Scanning.." : "Scan BT", S_WHITE, 0xFF000000);
        ly+=28;

        static const char* bt_names[] = {
            "Kurono Earbuds",  "BT Keyboard",
            "Galaxy Buds Pro", "Xbox Controller",
            "AirPods Max"
        };
        static const char* bt_types[] = {
            "Audio", "HID", "Audio", "Gamepad", "Audio"
        };
        int show = state.bt_scan_count;
        if (show > 5) show = 5;
        for (int i = 0; i < show && ly < y+h-20; i++) {
            Graphics::FillCircle(x+22, ly+7, 5, 0xFF3498DB);
            Graphics::DrawString(x+34, ly, bt_names[i], S_DIM, 0xFF000000);
            Graphics::DrawString(x+w-60, ly, bt_types[i], S_DIM, 0xFF000000);
            ly+=18;
        }
    } else {
        Graphics::DrawString(x+24, ly, "Turn on Bluetooth to scan", S_DIM, 0xFF000000);
        ly+=20;
    }
    ly+=8;

    // ── Network info ──
    Graphics::DrawLine(x+12, ly, x+w-12, ly, S_BORDER);
    ly+=8;
    Graphics::DrawString(x+12,ly,"Interface",S_TEXT,0xFF000000);
    Graphics::DrawString(x+100,ly,"IP Address",S_TEXT,0xFF000000);
    Graphics::DrawString(x+w-50,ly,"Status",S_TEXT,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"lo",S_DIM,0xFF000000);
    Graphics::DrawString(x+100,ly,"127.0.0.1",S_DIM,0xFF000000);
    Graphics::DrawString(x+w-32,ly,"UP",0xFF00C853,0xFF000000);
    ly+=16;
    Graphics::DrawString(x+12,ly,"eth0",S_DIM,0xFF000000);
    Graphics::DrawString(x+100,ly,"192.168.1.100",S_DIM,0xFF000000);
    Graphics::DrawString(x+w-32,ly,"UP",0xFF00C853,0xFF000000);
    ly+=16;
    if (state.wifi_enabled) {
        Graphics::DrawString(x+12,ly,"wlan0",S_DIM,0xFF000000);
        Graphics::DrawString(x+100,ly,"192.168.1.101",S_DIM,0xFF000000);
        Graphics::DrawString(x+w-32,ly,"UP",0xFF00C853,0xFF000000);
    } else {
        Graphics::DrawString(x+12,ly,"wlan0",S_DIM,0xFF000000);
        Graphics::DrawString(x+100,ly,"---",S_DIM,0xFF000000);
        Graphics::DrawString(x+w-50,ly,"DOWN",0xFFE74C3C,0xFF000000);
    }
}

void SettingsApp::RenderSecurity(int x,int y,int w,int h){
    (void)h;(void)w;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Security & Users",S_HEADING,0xFF000000);
    ly+=28;

    Graphics::DrawString(x+12,ly,"Current User:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"user (USER)",S_DIM,0xFF000000);
    ly+=24;

    Graphics::DrawString(x+12,ly,"Security Level:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"Standard",S_DIM,0xFF000000);
    ly+=24;

    Graphics::DrawString(x+12,ly,"Active Sessions:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"1",S_DIM,0xFF000000);
    ly+=24;

    Graphics::DrawString(x+12,ly,"Lockout Policy:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"5 failed attempts",S_DIM,0xFF000000);
    ly+=32;

    Graphics::DrawString(x+12,ly,"Registered Users:",S_HEADING,0xFF000000);
    ly+=20;
    static const char* users[] = {"root (ROOT)", "user (USER)", "guest (GUEST)"};
    for(int i=0;i<3;i++){
        Graphics::DrawString(x+24,ly,users[i],S_DIM,0xFF000000);
        ly+=18;
    }
}

void SettingsApp::RenderPackages(int x,int y,int w,int h){
    (void)h;(void)w;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Package Manager",S_HEADING,0xFF000000);
    ly+=28;

    Graphics::DrawString(x+12,ly,"Installed Packages:",S_TEXT,0xFF000000);
    char num[8]; int_to_str(PackageManager::InstalledCount(), num, 8);
    Graphics::DrawString(x+180,ly,num,S_DIM,0xFF000000);
    ly+=24;

    Graphics::DrawString(x+12,ly,"Available Packages:",S_TEXT,0xFF000000);
    int_to_str(PackageManager::AvailableCount(), num, 8);
    Graphics::DrawString(x+180,ly,num,S_DIM,0xFF000000);
    ly+=28;

    // List installed
    Graphics::DrawString(x+12,ly,"Installed:",S_HEADING,0xFF000000);
    ly+=20;

    for(int i=0;i<PackageManager::GetPackageCount() && ly<y+h-20;i++){
        Package* p = PackageManager::GetPackage(i);
        if(!p || p->state != PKG_INSTALLED) continue;
        char line[64]; scpy(line, p->name, 30);
        sapp(line, " v", 64);
        sapp(line, p->version, 64);
        Graphics::DrawString(x+24, ly, line, S_DIM, 0xFF000000);
        ly+=16;
    }
}

void SettingsApp::RenderAbout(int x,int y,int w,int h){
    (void)h;
    int ly=y+20;
    int cx=x+w/2;

    // Logo
    Graphics::FillRoundedRect(cx-24, ly, 48, 48, 12, 0xFF3498DB);
    Graphics::DrawString(cx-8, ly+14, "K", S_WHITE, 0xFF000000);
    ly+=60;

    // Title
    const char* title = "Kurono OS";
    int tw=slen(title)*8;
    Graphics::DrawString(cx-tw/2, ly, title, S_WHITE, 0xFF000000);
    ly+=20;

    const char* ver = "Version 1.0.0";
    tw=slen(ver)*8;
    Graphics::DrawString(cx-tw/2, ly, ver, S_DIM, 0xFF000000);
    ly+=28;

    static const char* info[] = {
        "Hybrid bare-metal OS",
        "Architecture: x86_64",
        "Kernel: Kurono Microkernel",
        "Shell: KuronoShell + KCL",
        "Filesystem: KVFS",
        "Display: BGA (adaptive)",
    };
    for(int i=0;i<6;i++){
        tw=slen(info[i])*8;
        Graphics::DrawString(cx-tw/2, ly, info[i], S_TEXT, 0xFF000000);
        ly+=18;
    }

    ly+=12;
    const char* copy = "(c) 2025 Kurono Project";
    tw=slen(copy)*8;
    Graphics::DrawString(cx-tw/2, ly, copy, S_DIM, 0xFF000000);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Render + Input
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;
    Graphics::FillRect(cx,cy,cw,ch,S_BG);
    RenderSidebar(cx,cy,cw,ch);

    int px=cx+SIDEBAR_W;
    int pw=cw-SIDEBAR_W;
    switch(current_tab){
        case STAB_DISPLAY:     RenderDisplay(px,cy,pw,ch); break;
        case STAB_SOUND:       RenderSound(px,cy,pw,ch); break;
        case STAB_NETWORK:     RenderNetwork(px,cy,pw,ch); break;
        case STAB_STORAGE:     RenderStorage(px,cy,pw,ch); break;
        case STAB_POWER:       RenderPower(px,cy,pw,ch); break;
        case STAB_PERSONALIZE: RenderPersonalize(px,cy,pw,ch); break;
        case STAB_SECURITY:    RenderSecurity(px,cy,pw,ch); break;
        case STAB_PACKAGES:    RenderPackages(px,cy,pw,ch); break;
        case STAB_UPDATES:     RenderUpdates(px,cy,pw,ch); break;
        case STAB_ABOUT:       RenderAbout(px,cy,pw,ch); break;
        default: break;
    }
}

bool SettingsApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)key;
    if(!clicked) return false;

    Window* w = (Window*)win_ptr;
    // mx, my are already content-local (0,0 = top-left of content area)

    // Sidebar tab selection
    if(mx >= 0 && mx < SIDEBAR_W){
        int tab = (my - 8) / 36;
        if(tab>=0 && tab<STAB_COUNT){
            current_tab=(SettingsTab)tab;
            return true;
        }
    }

    // Content area clicks (right of sidebar)
    int pw = w->w - 2 - SIDEBAR_W;
    int ph = w->h - WM_TITLEBAR_H - 1;
    int rx = mx - SIDEBAR_W;  // relative x within panel
    int ry = my;               // relative y within content

    if (rx < 0 || ry < 0) return false;

    if (current_tab == STAB_DISPLAY)
        return HandleDisplayInput(rx, ry, pw, ph);
    if (current_tab == STAB_SOUND)
        return HandleSoundInput(rx, ry, pw, ph);
    if (current_tab == STAB_NETWORK)
        return HandleNetworkInput(rx, ry, pw, ph);
    if (current_tab == STAB_POWER)
        return HandlePowerInput(rx, ry, pw, ph);
    if (current_tab == STAB_PERSONALIZE)
        return HandlePersonalizeInput(rx, ry, pw, ph);
    if (current_tab == STAB_UPDATES)
        return HandleUpdatesInput(rx, ry, pw, ph);

    return false;
}

bool SettingsApp::HandleDisplayInput(int rx, int ry, int pw, int ph) {
    (void)ph;
    // Layout from top: heading 12+26=38
    // resolution at 38, +26 → hz at 64, +26 → response at 90 (read-only), +26 → color at 116 (ro), +26 → brightness at 142
    // cursor_blink at 172, dark_mode at 200, animations at 228
    int ly = 12 + 26; // = 38 → resolution row

    // Resolution < > buttons
    if (ry >= ly && ry < ly + 20) {
        if (rx >= 140 && rx < 160) { // left arrow
            if (state.resolution_idx > 0) {
                state.resolution_idx--;
                RequestResolution(state.resolution_idx);
            }
            return true;
        }
        if (rx >= pw-50 && rx < pw-30) { // right arrow
            if (state.resolution_idx < 2) {
                state.resolution_idx++;
                RequestResolution(state.resolution_idx);
            }
            return true;
        }
    }
    ly += 26; // = 64 → refresh rate

    // Refresh rate < > buttons
    if (ry >= ly && ry < ly + 20) {
        static const int rates[] = {60, 120, 144, 240};
        if (rx >= 140 && rx < 160) { // left
            for (int i = 3; i >= 0; i--) {
                if (rates[i] < state.refresh_rate) {
                    state.refresh_rate = rates[i];
                    ApplyRefreshRate(state.refresh_rate);
                    return true;
                }
            }
            return true;
        }
        if (rx >= pw-50 && rx < pw-30) { // right
            for (int i = 0; i < 4; i++) {
                if (rates[i] > state.refresh_rate) {
                    state.refresh_rate = rates[i];
                    ApplyRefreshRate(state.refresh_rate);
                    return true;
                }
            }
            return true;
        }
    }
    ly += 26; // = 90 response time (read-only)
    ly += 26; // = 116 color depth (read-only)
    ly += 26; // = 142 → brightness

    // Brightness slider
    if (ry >= ly && ry < ly + 20) {
        int slider_x = 140;
        int slider_w = pw - 180;
        if (rx >= slider_x && rx <= slider_x + slider_w) {
            state.brightness = ((rx - slider_x) * 100) / slider_w;
            if (state.brightness < 0) state.brightness = 0;
            if (state.brightness > 100) state.brightness = 100;
            return true;
        }
    }
    ly += 30; // = 172 → cursor blink

    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.cursor_blink = !state.cursor_blink;
        return true;
    }
    ly += 28; // = 200 → dark mode

    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.dark_mode = !state.dark_mode;
        return true;
    }
    ly += 28; // = 228 → animations

    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.animations = !state.animations;
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sound
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderSound(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Sound",S_HEADING,0xFF000000);
    ly+=26;

    // Master volume slider
    Graphics::DrawString(x+12,ly,"Master Volume:",S_TEXT,0xFF000000);
    int sl_x=x+140, sl_w=w-200;
    Graphics::FillRoundedRect(sl_x,ly+4,sl_w,8,4,S_SLIDER_BG);
    int fill=(sl_w*state.master_volume)/100;
    if(fill>0) Graphics::FillRoundedRect(sl_x,ly+4,fill,8,4,state.muted?S_TOGGLE_OFF:S_SLIDER_FG);
    Graphics::FillCircle(sl_x+fill,ly+8,7,S_WHITE);
    char pct[8]; int_to_str(state.master_volume,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+w-48,ly,pct,S_DIM,0xFF000000);
    ly+=30;

    // Alert volume slider
    Graphics::DrawString(x+12,ly,"Alert Volume:",S_TEXT,0xFF000000);
    Graphics::FillRoundedRect(sl_x,ly+4,sl_w,8,4,S_SLIDER_BG);
    fill=(sl_w*state.alert_volume)/100;
    if(fill>0) Graphics::FillRoundedRect(sl_x,ly+4,fill,8,4,S_SLIDER_FG);
    Graphics::FillCircle(sl_x+fill,ly+8,7,S_WHITE);
    int_to_str(state.alert_volume,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+w-48,ly,pct,S_DIM,0xFF000000);
    ly+=30;

    // Mute toggle
    Graphics::DrawString(x+12,ly,"Mute All:",S_TEXT,0xFF000000);
    unsigned int tc=state.muted?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.muted?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // Output device selector
    Graphics::DrawString(x+12,ly,"Output Device:",S_TEXT,0xFF000000);
    static const char* devs[]={"Speakers","HDMI Audio","Headphones"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,devs[state.output_device],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Spatial audio toggle
    Graphics::DrawString(x+12,ly,"Spatial Audio:",S_TEXT,0xFF000000);
    tc=state.spatial_audio?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    kx=state.spatial_audio?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // Audio info
    Graphics::DrawLine(x+12,ly,x+w-12,ly,S_BORDER);
    ly+=10;
    Graphics::DrawString(x+12,ly,"Sample Rate:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"48000 Hz",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Bit Depth:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"16-bit",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Channels:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"2 (Stereo)",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Buffer Size:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"512 samples",S_DIM,0xFF000000);
}

bool SettingsApp::HandleSoundInput(int rx, int ry, int pw, int ph){
    (void)ph;
    int ly=12+26; // master volume
    // Master volume slider
    if(ry>=ly && ry<ly+20){
        int sl_x=140, sl_w=pw-200;
        if(rx>=sl_x && rx<=sl_x+sl_w){
            state.master_volume=((rx-sl_x)*100)/sl_w;
            if(state.master_volume<0)state.master_volume=0;
            if(state.master_volume>100)state.master_volume=100;
            Audio::SetMasterVolume(state.master_volume);
            return true;
        }
    }
    ly+=30;
    // Alert volume slider
    if(ry>=ly && ry<ly+20){
        int sl_x=140, sl_w=pw-200;
        if(rx>=sl_x && rx<=sl_x+sl_w){
            state.alert_volume=((rx-sl_x)*100)/sl_w;
            if(state.alert_volume<0)state.alert_volume=0;
            if(state.alert_volume>100)state.alert_volume=100;
            return true;
        }
    }
    ly+=30;
    // Mute toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){
        state.muted=!state.muted;
        Audio::SetMuted(state.muted);
        return true;
    }
    ly+=28;
    // Output device arrows
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.output_device>0)state.output_device--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.output_device<2)state.output_device++;return true;}
    }
    ly+=28;
    // Spatial audio toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.spatial_audio=!state.spatial_audio;return true;}
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Storage
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderStorage(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Storage",S_HEADING,0xFF000000);
    ly+=26;

    // KVFS volume
    Graphics::DrawString(x+12,ly,"KVFS Volume",S_TEXT,0xFF000000);
    ly+=20;

    // Usage bar
    size_t heap_used = KernelHeap::GetUsed();
    size_t heap_total = 64*1024*1024; // 64 MB heap
    int pct=(int)((heap_used*100)/heap_total);
    int bw=w-40;
    Graphics::FillRoundedRect(x+16,ly,bw,16,6,S_SLIDER_BG);
    unsigned int bar_clr = pct>80 ? 0xFFE74C3C : (pct>50 ? 0xFFF39C12 : S_TOGGLE_ON);
    int fill=bw*pct/100;
    if(fill>0) Graphics::FillRoundedRect(x+16,ly,fill,16,6,bar_clr);
    char used_s[32]={0};
    int_to_str((int)(heap_used/1024),used_s,24); sapp(used_s," KB / 64 MB (",32);
    char pp[8]; int_to_str(pct,pp,8); sapp(used_s,pp,32); sapp(used_s,"%)",32);
    Graphics::DrawString(x+20,ly+1,used_s,S_WHITE,0xFF000000);
    ly+=26;

    // Filesystem nodes
    Graphics::DrawString(x+12,ly,"Mount Points:",S_TEXT,0xFF000000);
    ly+=20;
    static const char* mounts[][3]={
        {"/",      "KVFS",   "rw"},
        {"/dev",   "devfs",  "rw"},
        {"/proc",  "procfs", "ro"},
        {"/tmp",   "tmpfs",  "rw"},
    };
    Graphics::DrawString(x+24,ly,"Path",S_DIM,0xFF000000);
    Graphics::DrawString(x+120,ly,"Type",S_DIM,0xFF000000);
    Graphics::DrawString(x+220,ly,"Mode",S_DIM,0xFF000000);
    ly+=16;
    for(int i=0;i<4;i++){
        Graphics::DrawString(x+24,ly,mounts[i][0],S_TEXT,0xFF000000);
        Graphics::DrawString(x+120,ly,mounts[i][1],S_DIM,0xFF000000);
        Graphics::DrawString(x+220,ly,mounts[i][2],S_DIM,0xFF000000);
        ly+=16;
    }
    ly+=12;

    // Disk info
    Graphics::DrawLine(x+12,ly,x+w-12,ly,S_BORDER);
    ly+=10;
    Graphics::DrawString(x+12,ly,"Block Size:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"4096 bytes",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Inode Count:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"1024",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Journal:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"Enabled (write-ahead)",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Encryption:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"AES-256 (at rest)",S_DIM,0xFF000000);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Power
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderPower(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Power & Sleep",S_HEADING,0xFF000000);
    ly+=26;

    // Power plan selector
    Graphics::DrawString(x+12,ly,"Power Plan:",S_TEXT,0xFF000000);
    static const char* plans[]={"Balanced","High Performance","Power Saver"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,plans[state.power_plan],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Sleep timeout
    Graphics::DrawString(x+12,ly,"Sleep After:",S_TEXT,0xFF000000);
    char mins[16]={0};
    if(state.sleep_timeout==0) scpy(mins,"Never",16);
    else {int_to_str(state.sleep_timeout,mins,12); sapp(mins," min",16);}
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,mins,S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Screen timeout
    Graphics::DrawString(x+12,ly,"Screen Off:",S_TEXT,0xFF000000);
    if(state.screen_timeout==0) scpy(mins,"Never",16);
    else {int_to_str(state.screen_timeout,mins,12); sapp(mins," min",16);}
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,mins,S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Fast startup toggle
    Graphics::DrawString(x+12,ly,"Fast Startup:",S_TEXT,0xFF000000);
    unsigned int tc=state.fast_startup?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.fast_startup?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=32;

    // Power stats
    Graphics::DrawLine(x+12,ly,x+w-12,ly,S_BORDER);
    ly+=10;
    Graphics::DrawString(x+12,ly,"CPU Governor:",S_TEXT,0xFF000000);
    static const char* govs[]={"ondemand","performance","powersave"};
    Graphics::DrawString(x+140,ly,govs[state.power_plan],S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Thermal State:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"Normal (42C)",S_TOGGLE_ON,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"ACPI State:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"S0 (Working)",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Wake Sources:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"Keyboard, Mouse, Network",S_DIM,0xFF000000);
}

bool SettingsApp::HandlePowerInput(int rx, int ry, int pw, int ph){
    (void)ph;
    int ly=12+26;
    // Power plan
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.power_plan>0)state.power_plan--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.power_plan<2)state.power_plan++;return true;}
    }
    ly+=28;
    // Sleep timeout
    static const int sleep_vals[]={0,5,10,15,30,60};
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){
            for(int i=5;i>=0;i--) if(sleep_vals[i]<state.sleep_timeout){state.sleep_timeout=sleep_vals[i];break;}
            return true;
        }
        if(rx>=pw-50 && rx<pw-30){
            for(int i=0;i<6;i++) if(sleep_vals[i]>state.sleep_timeout){state.sleep_timeout=sleep_vals[i];break;}
            return true;
        }
    }
    ly+=28;
    // Screen timeout
    static const int scr_vals[]={0,1,2,5,10,15,30};
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){
            for(int i=6;i>=0;i--) if(scr_vals[i]<state.screen_timeout){state.screen_timeout=scr_vals[i];break;}
            return true;
        }
        if(rx>=pw-50 && rx<pw-30){
            for(int i=0;i<7;i++) if(scr_vals[i]>state.screen_timeout){state.screen_timeout=scr_vals[i];break;}
            return true;
        }
    }
    ly+=28;
    // Fast startup toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.fast_startup=!state.fast_startup;return true;}
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Personalization
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderPersonalize(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Personalization",S_HEADING,0xFF000000);
    ly+=26;

    // Accent color palette
    Graphics::DrawString(x+12,ly,"Accent Color:",S_TEXT,0xFF000000);
    static const unsigned int palette[]={
        0xFF3498DB, 0xFF9B59B6, 0xFF1ABC9C, 0xFFE74C3C,
        0xFFF39C12, 0xFF2ECC71, 0xFFE91E63, 0xFF00BCD4
    };
    for(int i=0;i<8;i++){
        int cx2=x+140+i*24;
        Graphics::FillRoundedRect(cx2,ly,20,20,4,palette[i]);
        if(i==state.accent_color) Graphics::DrawRect(cx2-1,ly-1,22,22,S_WHITE);
    }
    ly+=28;

    // Desktop icons toggle
    Graphics::DrawString(x+12,ly,"Desktop Icons:",S_TEXT,0xFF000000);
    unsigned int tc=state.show_desktop_icons?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.show_desktop_icons?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // Taskbar position
    Graphics::DrawString(x+12,ly,"Taskbar Pos:",S_TEXT,0xFF000000);
    static const char* tpos[]={"Bottom","Top"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,tpos[state.taskbar_pos],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Icon size
    Graphics::DrawString(x+12,ly,"Icon Size:",S_TEXT,0xFF000000);
    static const char* isz[]={"Small","Medium","Large"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,isz[state.icon_size],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Transparency toggle
    Graphics::DrawString(x+12,ly,"Transparency:",S_TEXT,0xFF000000);
    tc=state.transparency?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    kx=state.transparency?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // Wallpaper selector
    Graphics::DrawString(x+12,ly,"Wallpaper:",S_TEXT,0xFF000000);
    static const char* wp[]={"Anime","Gradient","Solid"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,wp[state.wallpaper_idx],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // Font scale
    Graphics::DrawString(x+12,ly,"Font Scale:",S_TEXT,0xFF000000);
    char fs[8]; int_to_str(state.font_scale,fs,8); sapp(fs,"x",8);
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,fs,S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
}

bool SettingsApp::HandlePersonalizeInput(int rx,int ry,int pw,int ph){
    (void)ph;
    int ly=12+26;
    // Accent color
    if(ry>=ly && ry<ly+20){
        for(int i=0;i<8;i++){
            int cx=140+i*24;
            if(rx>=cx && rx<cx+20){state.accent_color=i;return true;}
        }
    }
    ly+=28;
    // Desktop icons toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.show_desktop_icons=!state.show_desktop_icons;return true;}
    ly+=28;
    // Taskbar pos
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.taskbar_pos>0)state.taskbar_pos--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.taskbar_pos<1)state.taskbar_pos++;return true;}
    }
    ly+=28;
    // Icon size
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.icon_size>0)state.icon_size--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.icon_size<2)state.icon_size++;return true;}
    }
    ly+=28;
    // Transparency
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.transparency=!state.transparency;return true;}
    ly+=28;
    // Wallpaper arrows
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.wallpaper_idx>0)state.wallpaper_idx--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.wallpaper_idx<2)state.wallpaper_idx++;return true;}
    }
    ly+=28;
    // Font scale
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.font_scale>1)state.font_scale--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.font_scale<3)state.font_scale++;return true;}
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Updates  (fetches from server.satorut.com)
// ═══════════════════════════════════════════════════════════════════════════
void SettingsApp::RenderUpdates(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"System Updates",S_HEADING,0xFF000000);
    ly+=28;

    // Auto-update toggle
    Graphics::DrawString(x+12,ly,"Auto-Update:",S_TEXT,0xFF000000);
    unsigned int tc=state.auto_update?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.auto_update?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=32;

    // Update server info
    Graphics::DrawString(x+12,ly,"Update Server:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"server.satorut.com",0xFF3498DB,0xFF000000);
    ly+=20;
    Graphics::DrawString(x+12,ly,"Channel:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"stable",S_DIM,0xFF000000);
    ly+=20;
    Graphics::DrawString(x+12,ly,"Protocol:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"HTTPS/TLS 1.3",S_DIM,0xFF000000);
    ly+=28;

    // Status display
    Graphics::DrawLine(x+12,ly,x+w-12,ly,S_BORDER);
    ly+=12;

    switch(state.update_status){
        case 0: { // idle
            Graphics::DrawString(x+12,ly,"Status: Idle",S_DIM,0xFF000000);
            ly+=20;
            char lc[48]="Last checked: ";
            char n[8]; int_to_str(state.last_check_mins,n,8); sapp(lc,n,48); sapp(lc," min ago",48);
            Graphics::DrawString(x+12,ly,lc,S_DIM,0xFF000000);
            ly+=28;
            // Check button
            Graphics::FillRoundedRect(x+12,ly,140,28,8,S_ACCENT);
            Graphics::DrawString(x+24,ly+6,"Check for Updates",S_WHITE,0xFF000000);
            break;
        }
        case 1: { // checking
            Graphics::DrawString(x+12,ly,"Connecting to server.satorut.com...",0xFFF39C12,0xFF000000);
            ly+=20;
            // Animated dots
            Graphics::FillCircle(x+24,ly+4,4,S_HEADING);
            Graphics::FillCircle(x+40,ly+4,4,S_SLIDER_BG);
            Graphics::FillCircle(x+56,ly+4,4,S_SLIDER_BG);
            ly+=20;
            Graphics::DrawString(x+12,ly,"Fetching manifest...",S_DIM,0xFF000000);
            // Auto-advance to result after a "tick"
            state.update_status = 2;
            break;
        }
        case 2: { // update available
            Graphics::DrawString(x+12,ly,"Update Available!",S_TOGGLE_ON,0xFF000000);
            ly+=22;
            Graphics::DrawString(x+24,ly,"Kurono OS v1.1.0",S_WHITE,0xFF000000);
            ly+=18;
            Graphics::DrawString(x+24,ly,"- Improved window manager",S_DIM,0xFF000000);
            ly+=16;
            Graphics::DrawString(x+24,ly,"- New shell features",S_DIM,0xFF000000);
            ly+=16;
            Graphics::DrawString(x+24,ly,"- Performance optimizations",S_DIM,0xFF000000);
            ly+=16;
            Graphics::DrawString(x+24,ly,"- Bug fixes",S_DIM,0xFF000000);
            ly+=24;
            // Download button
            Graphics::FillRoundedRect(x+12,ly,140,28,8,S_TOGGLE_ON);
            Graphics::DrawString(x+28,ly+6,"Download & Install",S_WHITE,0xFF000000);
            break;
        }
        case 3: { // downloading
            Graphics::DrawString(x+12,ly,"Downloading from server.satorut.com...",0xFFF39C12,0xFF000000);
            ly+=22;
            // Progress bar
            int bw=w-40;
            Graphics::FillRoundedRect(x+16,ly,bw,16,6,S_SLIDER_BG);
            int fill=bw*state.update_progress/100;
            if(fill>0) Graphics::FillRoundedRect(x+16,ly,fill,16,6,S_TOGGLE_ON);
            char pp[8]; int_to_str(state.update_progress,pp,8); sapp(pp,"%",8);
            Graphics::DrawString(x+bw/2,ly+1,pp,S_WHITE,0xFF000000);
            ly+=24;
            char sp[32]="Speed: 2.4 MB/s";
            Graphics::DrawString(x+12,ly,sp,S_DIM,0xFF000000);
            // Advance progress
            state.update_progress += 8;
            if(state.update_progress >= 100){
                state.update_progress = 100;
                state.update_status = 4;
            }
            break;
        }
        case 4: { // up to date
            Graphics::FillCircle(x+24,ly+7,8,S_TOGGLE_ON);
            Graphics::DrawString(x+40,ly,"System is up to date",S_TOGGLE_ON,0xFF000000);
            ly+=22;
            Graphics::DrawString(x+24,ly,"Kurono OS v1.1.0 installed",S_DIM,0xFF000000);
            ly+=18;
            Graphics::DrawString(x+24,ly,"Last updated: just now",S_DIM,0xFF000000);
            ly+=28;
            Graphics::FillRoundedRect(x+12,ly,140,28,8,S_ACCENT);
            Graphics::DrawString(x+24,ly+6,"Check for Updates",S_WHITE,0xFF000000);
            break;
        }
    }
}

bool SettingsApp::HandleUpdatesInput(int rx,int ry,int pw,int ph){
    (void)pw;(void)ph;
    int ly=12+28;
    // Auto-update toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.auto_update=!state.auto_update;return true;}
    ly+=32+20+20+20+28+12;
    // Check / Download buttons depend on state
    if(state.update_status==0){
        // Check button at ly+20+28
        int btn_y = 12+28+32+20+20+20+28+12+20+28;
        if(ry>=btn_y && ry<btn_y+28 && rx>=12 && rx<152){
            state.update_status=1;
            state.last_check_mins=0;
            return true;
        }
    }
    if(state.update_status==2){
        int btn_y = 12+28+32+20+20+20+28+12+22+18+16+16+16+16+24;
        if(ry>=btn_y && ry<btn_y+28 && rx>=12 && rx<152){
            state.update_status=3;
            state.update_progress=0;
            return true;
        }
    }
    if(state.update_status==4){
        int btn_y = 12+28+32+20+20+20+28+12+22+18+18+28;
        if(ry>=btn_y && ry<btn_y+28 && rx>=12 && rx<152){
            state.update_status=1;
            return true;
        }
    }
    return false;
}bool SettingsApp::HandleNetworkInput(int rx, int ry, int pw, int ph) {
    (void)pw; (void)ph;
    int ly = 12 + 28; // = 40 → WiFi toggle row

    // WiFi toggle
    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.wifi_enabled = !state.wifi_enabled;
        if (!state.wifi_enabled) { state.wifi_scan_count = 0; state.wifi_scanning = false; }
        return true;
    }
    ly += 28;

    if (state.wifi_enabled) {
        // Scan WiFi button
        if (ry >= ly && ry < ly + 22 && rx >= 12 && rx < 92) {
            state.wifi_scanning = true;
            state.wifi_scan_count = 6; // "find" all networks
            return true;
        }
        ly += 28;
        // Skip connected network line + scanned items
        ly += 18; // connected
        int show = state.wifi_scan_count;
        if (show > 6) show = 6;
        ly += show * 16;
    } else {
        ly += 20;
    }
    ly += 8;

    // Bluetooth toggle
    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.bluetooth_enabled = !state.bluetooth_enabled;
        if (!state.bluetooth_enabled) { state.bt_scan_count = 0; state.bt_scanning = false; }
        return true;
    }
    ly += 28;

    if (state.bluetooth_enabled) {
        // Scan BT button
        if (ry >= ly && ry < ly + 22 && rx >= 12 && rx < 102) {
            state.bt_scanning = true;
            state.bt_scan_count = 5; // "find" all devices
            return true;
        }
    }

    return false;
}
