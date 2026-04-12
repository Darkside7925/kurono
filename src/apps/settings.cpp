//  kurono os  -  settings application implementation
#include "settings.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/display_mgr.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "../ui/gui.h"
#include "../ui/desktop.h"
#include "../linux/linux_drivers.h"
#include "../net/network.h"
#include "../security/supr.h"
#include "../packages/pkgmgr.h"
#include "../kernel/heap.h"
#include "../system/logging.h"
#include "../virt/hypervisor.h"
#include "media_player.h"
#include "browser.h"
#include "conduit.h"

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
static bool starts_with(const char* s,const char* prefix){
    if(!s||!prefix) return false;
    int i=0; while(prefix[i]){ if(s[i]!=prefix[i]) return false; i++; }
    return true;
}
static const LinuxDriver* find_bound_driver_by_prefixes(const char* const* prefixes,int count){
    LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
    int driver_count = LinuxDriverFramework::GetDriverCount();
    for(int i=0;i<driver_count;i++){
        if(!(drivers[i].bound || drivers[i].state == LDRV_ACTIVE)) continue;
        for(int p=0;p<count;p++){
            if(starts_with(drivers[i].name, prefixes[p])) return &drivers[i];
        }
    }
    return nullptr;
}
static const LinuxDriver* find_wifi_driver(){
    static const char* prefixes[] = {
        "wifi_", "iwl", "ath", "rtw", "rtl", "brcm", "mt76", "cfg80211", "mac80211"
    };
    return find_bound_driver_by_prefixes(prefixes, (int)(sizeof(prefixes)/sizeof(prefixes[0])));
}
static const LinuxDriver* find_bt_driver(){
    static const char* prefixes[] = {
        "bluetooth_", "bluetooth", "bt", "hci"
    };
    return find_bound_driver_by_prefixes(prefixes, (int)(sizeof(prefixes)/sizeof(prefixes[0])));
}
static void append_int(char* dst,int value,int mx){ char tmp[16]={0}; int_to_str(value,tmp,16); sapp(dst,tmp,mx); }
static void format_display_summary(char* line1,int mx1,char* line2,int mx2){
    scpy(line1,"Hybrid bare-metal OS",mx1);
    scpy(line2,DisplayManager::GetBackendName(),mx2);
    sapp(line2," ",mx2);
    append_int(line2,(int)Graphics::GetWidth(),mx2);
    sapp(line2,"x",mx2);
    append_int(line2,(int)Graphics::GetHeight(),mx2);
    sapp(line2," ",mx2);
    append_int(line2,(int)Graphics::GetBpp(),mx2);
    sapp(line2,"bpp",mx2);
}
static void format_cpu_summary(char* line1,int mx1,char* line2,int mx2){
    CpuInfo info = CPUDetect::GetInfo();
    const char* brand = info.brand_string[0] ? info.brand_string : CPUDetect::GetVendorName();
    int cores = info.topology.physical_cores > 0 ? info.topology.physical_cores : CPUDetect::GetCoreCount();
    int threads = info.topology.logical_cores > 0 ? info.topology.logical_cores : CPUDetect::GetThreadCount();
    scpy(line1, brand, mx1);
    scpy(line2, "Cores: ", mx2);
    append_int(line2, cores, mx2);
    sapp(line2, "  Threads: ", mx2);
    append_int(line2, threads, mx2);
    if (info.frequency.base_mhz > 0) {
        sapp(line2, "  Base: ", mx2);
        append_int(line2, info.frequency.base_mhz, mx2);
        sapp(line2, " MHz", mx2);
    }
}
static void format_gpu_summary(char* line1,int mx1,char* line2,int mx2){
    const GpuProbeResult& gpr = GpuProbe::GetResult();
    const char* gpu_desc = "No GPU detected";
    if (gpr.count > 0) {
        if (gpr.primary_idx >= 0 && gpr.primary_idx < gpr.count) gpu_desc = gpr.gpus[gpr.primary_idx].desc;
        else gpu_desc = gpr.gpus[0].desc;
    }
    scpy(line1, gpu_desc, mx1);
    if (NvidiaGPU::IsDetected()) {
        const NvidiaGPUInfo& nv = NvidiaGPU::GetInfo();
        scpy(line2, NvidiaGPU::GetArchName(), mx2);
        sapp(line2, "  VRAM: ", mx2);
        append_int(line2, (int)nv.vram_mb, mx2);
        sapp(line2, " MB", mx2);
    } else if (AmdGPU::IsAvailable()) {
        const AmdGPUInfo& amd = AmdGPU::GetInfo();
        scpy(line2, AmdGPU::GetArchName(), mx2);
        if (amd.vram_size > 0) {
            sapp(line2, "  VRAM: ", mx2);
            append_int(line2, (int)(amd.vram_size / (1024 * 1024)), mx2);
            sapp(line2, " MB", mx2);
        }
    } else if (IntelGPU::IsDetected()) {
        const IntelGPUInfo& ig = IntelGPU::GetInfo();
        scpy(line2, IntelGPU::GetGenName(), mx2);
        sapp(line2, "  Stolen: ", mx2);
        append_int(line2, (int)ig.stolen_mem_mb, mx2);
        sapp(line2, " MB", mx2);
    } else {
        scpy(line2, "Display managed by framebuffer backend", mx2);
    }
}
static void format_radio_summary(char* line1,int mx1,char* line2,int mx2){
    const LinuxDriver* wifi_drv = find_wifi_driver();
    const LinuxDriver* bt_drv = find_bt_driver();
    WiFiState wifi_state = WiFi::GetState();
    scpy(line1, "WiFi: ", mx1);
    sapp(line1, WiFi::StateString(), mx1);
    WiFiNetwork* connected = WiFi::GetConnectedNetwork();
    if (wifi_state == WIFI_CONNECTED && connected) {
        sapp(line1, " (", mx1);
        sapp(line1, connected->ssid, mx1);
        sapp(line1, ")", mx1);
    } else if (WiFi::GetNetworkCount() > 0) {
        sapp(line1, " networks: ", mx1);
        append_int(line1, WiFi::GetNetworkCount(), mx1);
    }
    scpy(line2, "BT: ", mx2);
    if (bt_drv) {
        sapp(line2, bt_drv->description[0] ? bt_drv->description : bt_drv->name, mx2);
    } else {
        sapp(line2, "No controller metadata", mx2);
    }
    if (wifi_drv) {
        sapp(line2, "  WiFi drv: ", mx2);
        sapp(line2, wifi_drv->name, mx2);
    }
}
static void draw_info_card(int x,int y,int w,int h,const char* title,const char* line1,const char* line2){
    Graphics::FillRoundedRect(x, y, w, h, 10, S_SIDEBAR);
    Graphics::DrawString(x+12, y+8, title, S_HEADING, 0xFF000000);
    Graphics::DrawString(x+12, y+28, line1, S_TEXT, 0xFF000000);
    Graphics::DrawString(x+12, y+46, line2, S_DIM, 0xFF000000);
}

SettingsTab SettingsApp::current_tab  = STAB_DISPLAY;
int         SettingsApp::scroll_offset = 0;
SettingsState SettingsApp::state = {
    /* brightness */ 75, /* cursor_blink */ true, /* dark_mode */ true,
    /* animations */ true, /* font_scale */ 1, /* wallpaper_idx */ 0,
    /* resolution_idx */ 0, /* refresh_rate */ 60, /* synced to monitor in Init() */
    /* mouse_sensitivity */ 1,
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
    /* last_check_mins */ 42,
    /* linux_guest_enabled */ true, /* linux_guest_profile */ 0
};

int SettingsApp::pending_resolution_idx = -1;

// schedule resolution change  -  applied between frames by the main loop
static void RequestResolution(int idx) {
    SettingsApp::pending_resolution_idx = idx;
}

static int DetectResolutionIndex(int w, int h) {
    static const int res_w[] = {1024, 1920, 2560};
    static const int res_h[] = { 768, 1080, 1440};
    int best = 0;
    long best_score = 0x7FFFFFFF;
    for (int i = 0; i < 3; i++) {
        long dw = (long)res_w[i] - (long)w;
        long dh = (long)res_h[i] - (long)h;
        long score = dw * dw + dh * dh;
        if (score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

static void SyncRuntimeDisplayState() {
    int w = (int)Graphics::GetWidth();
    int h = (int)Graphics::GetHeight();
    if (w > 0 && h > 0) {
        SettingsApp::state.resolution_idx = DetectResolutionIndex(w, h);
    }
    if (SettingsApp::state.mouse_sensitivity < 1) SettingsApp::state.mouse_sensitivity = 1;
    if (SettingsApp::state.mouse_sensitivity > 4) SettingsApp::state.mouse_sensitivity = 4;
}

// actually performs the resolution switch (called from polldeferredactions)
static void DoApplyResolution(int idx) {
    static const int res_w[] = {1024, 1920, 2560};
    static const int res_h[] = { 768, 1080, 1440};
    if (idx < 0 || idx > 2) return;
    int w = res_w[idx], h = res_h[idx];

    // on native gop/framebuffer backends, resolution is firmware-controlled.
    if (!DisplayManager::SetResolution((uint32_t)w, (uint32_t)h, 32)) {
        SyncRuntimeDisplayState();
        return;
    }

    // close all windows first  -  their content areas reference old buffers
    WindowManager::CloseAll();

    // reset app-level win_id statics so apps can reopen after resolution change
    // without this, apps with win_id guards think they're still open and refuse to launch
    MediaPlayerApp::win_id = -1;
    KBrowse::win_id = -1;
    ConduitApp::win_id = -1;

    // clear the framebuffer immediately so no stale data shows
    uint8_t* fb = (uint8_t*)DisplayManager::GetFramebuffer();
    if (fb) memset(fb, 0, (uint32_t)w * h * 4);

    // apply the current refresh rate target
    if (SettingsApp::state.refresh_rate > 0)
        Graphics::SetTargetFPS((uint32_t)SettingsApp::state.refresh_rate);

    // reinit desktop/taskbar for new screen size + re-scale wallpaper
    Desktop::Init(w, h);
    if (GUI::wallpaper.valid) {
        Desktop::SetWallpaperImage(GUI::wallpaper);
    }
    Taskbar::Init(w, h);

    // update wm desktop area for new resolution (exclude taskbar)
    WindowManager::SetDesktopArea(0, 0, w, h - 44);

    // reopen settings so user can see the new resolution applied
    SettingsApp::Open();
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

static void ApplyWallpaperSelection() {
    switch (SettingsApp::state.wallpaper_idx) {
        case 0:
            if (GUI::wallpaper.valid) {
                Desktop::SetWallpaperImage(GUI::wallpaper);
            } else {
                Desktop::SetWallpaper(0xFF0B0F1A);
            }
            break;
        case 1:
            Desktop::SetWallpaper(0xFF0B0F1A);
            break;
        case 2:
            Desktop::SetWallpaper(0xFF101827);
            break;
        default:
            SettingsApp::state.wallpaper_idx = 1;
            Desktop::SetWallpaper(0xFF0B0F1A);
            break;
    }
}

//  init / open
void SettingsApp::Init(){
    current_tab=STAB_DISPLAY;
    scroll_offset=0;
    SyncRuntimeDisplayState();
    state.linux_guest_enabled = Hypervisor::IsLinuxGuestEnabled();
    state.linux_guest_profile = (int)Hypervisor::GetLinuxGuestProfile();
    // sync refresh_rate to actual monitor hz on first init
    uint32_t mon = Graphics::GetMonitorHz();
    if (mon > 0) {
        state.refresh_rate = (int)mon;
    }
    Mouse::SetSensitivity((uint16_t)state.mouse_sensitivity);
    ApplyWallpaperSelection();
}

int SettingsApp::Open(){
    Init();
    RuntimeLog::LogAppEvent("settings", "open");
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

//  sidebar
void SettingsApp::RenderSidebar(int x,int y,int w,int h){
    (void)w;
    Graphics::FillRect(x,y,SIDEBAR_W,h,S_SIDEBAR);
    Graphics::DrawLine(x+SIDEBAR_W-1,y,x+SIDEBAR_W-1,y+h,S_BORDER);

    static const char* tabs[] = {
        "Display", "Sound", "Network", "Storage",
        "Power", "Personal", "Security", "Packages",
        "Updates", "System", "About"
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

//  tab panels
void SettingsApp::RenderDisplay(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Display",S_HEADING,0xFF000000);
    ly+=26;

    // resolution selector
    Graphics::DrawString(x+12,ly,"Resolution:",S_TEXT,0xFF000000);
    static const char* res_names[] = {"1024 x 768", "1920 x 1080", "2560 x 1440"};
    char actual_res[32] = {0};
    char hbuf[12] = {0};
    int_to_str((int)Graphics::GetWidth(), actual_res, 12);
    sapp(actual_res, " x ", 32);
    int_to_str((int)Graphics::GetHeight(), hbuf, 12);
    sapp(actual_res, hbuf, 32);
    // draw left/right arrows for resolution
    Graphics::FillRoundedRect(x+140, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+144, ly, "<", S_WHITE, 0xFF000000);
    Graphics::DrawString(x+168, ly, actual_res, S_WHITE, 0xFF000000);
    Graphics::FillRoundedRect(x+w-50, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+w-46, ly, ">", S_WHITE, 0xFF000000);
    ly+=26;

    Graphics::DrawString(x+12,ly,"Mode Control:",S_TEXT,0xFF000000);
    const char* mode_text = ((DisplayManager::GetBackend() == DISPLAY_BACKEND_BGA) ||
                             (DisplayManager::GetBackend() == DISPLAY_BACKEND_VIRTIO_GPU))
                                ? res_names[state.resolution_idx]
                                : "Native / firmware controlled";
    Graphics::DrawString(x+140, ly, mode_text, S_DIM, 0xFF000000);
    ly+=26;

    // monitor detected hz (read-only info line)
    {
        uint32_t mon_hz = Graphics::GetMonitorHz();
        Graphics::DrawString(x+12,ly,"Monitor Hz:",S_TEXT,0xFF000000);
        char mon_buf[24] = {0};
        if (mon_hz > 0) {
            int_to_str((int)mon_hz, mon_buf, 12);
            sapp(mon_buf, " Hz detected", 24);
        } else {
            scpy(mon_buf, "Unknown", 24);
        }
        Graphics::DrawString(x+140, ly, mon_buf, S_TOGGLE_ON, 0xFF000000);
    }
    ly+=26;

    // refresh rate (target fps cap)
    Graphics::DrawString(x+12,ly,"Max FPS:",S_TEXT,0xFF000000);
    char hz_buf[16] = {0};
    int_to_str(state.refresh_rate, hz_buf, 12);
    sapp(hz_buf, " Hz", 16);
    Graphics::FillRoundedRect(x+140, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+144, ly, "<", S_WHITE, 0xFF000000);
    Graphics::DrawString(x+168, ly, hz_buf, S_WHITE, 0xFF000000);
    Graphics::FillRoundedRect(x+w-50, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+w-46, ly, ">", S_WHITE, 0xFF000000);
    // show current actual fps next to the selector
    {
        uint32_t actual_fps = Graphics::GetTargetFPS();
        char act_buf[24] = "(target: ";
        char nstr[8]; int_to_str((int)actual_fps, nstr, 8);
        sapp(act_buf, nstr, 24); sapp(act_buf, ")", 24);
        Graphics::DrawString(x+w-130, ly, act_buf, S_DIM, 0xFF000000);
    }
    ly+=26;

    Graphics::DrawString(x+12,ly,"Mouse Sense:",S_TEXT,0xFF000000);
    char sens[8] = {0};
    int_to_str(state.mouse_sensitivity, sens, 8);
    sapp(sens, "x", 8);
    Graphics::FillRoundedRect(x+140, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+144, ly, "<", S_WHITE, 0xFF000000);
    Graphics::DrawString(x+168, ly, sens, S_WHITE, 0xFF000000);
    Graphics::FillRoundedRect(x+w-50, ly-2, 20, 20, 4, S_ACCENT);
    Graphics::DrawString(x+w-46, ly, ">", S_WHITE, 0xFF000000);
    ly+=26;

    // framebuffer info (wc status  -  important for bare-metal debugging)
    {
        Graphics::DrawString(x+12,ly,"FB Cache:",S_TEXT,0xFF000000);
        if (Graphics::IsFramebufferWC()) {
            Graphics::DrawString(x+140,ly,"Write-Combining (optimal)", S_TOGGLE_ON, 0xFF000000);
        } else {
            Graphics::DrawString(x+140,ly,"Write-Back (slow, wbinvd fallback)", 0xFFE74C3C, 0xFF000000);
        }
    }
    ly+=26;

    // response time
    Graphics::DrawString(x+12,ly,"Response Time:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"0.03 ms",0xFF00C853,0xFF000000);
    ly+=26;

    // color depth
    Graphics::DrawString(x+12,ly,"Color Depth:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"32-bit True Color",S_DIM,0xFF000000);
    ly+=26;

    // brightness slider (functional)
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

    // cursor blink toggle (functional)
    Graphics::DrawString(x+12,ly,"Cursor Blink:",S_TEXT,0xFF000000);
    unsigned int tog_col = state.cursor_blink ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    int knob_x = state.cursor_blink ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    ly+=28;

    // dark mode toggle (functional)
    Graphics::DrawString(x+12,ly,"Dark Mode:",S_TEXT,0xFF000000);
    tog_col = state.dark_mode ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    knob_x = state.dark_mode ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    ly+=28;

    // animations toggle (functional)
    Graphics::DrawString(x+12,ly,"Animations:",S_TEXT,0xFF000000);
    tog_col = state.animations ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140, ly, 40, 20, 10, tog_col);
    knob_x = state.animations ? (x+140+29) : (x+140+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
}

void SettingsApp::RenderNetwork(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    const LinuxDriver* wifi_drv = find_wifi_driver();
    const LinuxDriver* bt_drv = find_bt_driver();
    Graphics::DrawString(x+12,ly,"Network & Connectivity",S_HEADING,0xFF000000);
    ly+=28;

    Graphics::DrawString(x+12,ly,"Ethernet (E1000)",S_TEXT,0xFF000000);
    NetworkInterface* eth = Network::GetInterface("eth0");
    bool eth_up = eth && eth->state == NIC_UP;
    Graphics::DrawString(x+190, ly+2, eth_up ? "Connected" : "Disconnected",
                         eth_up ? S_TOGGLE_ON : 0xFFE74C3C, 0xFF000000);
    ly+=22;

    // show real ip from interface
    if (eth) {
        char ip_str[32]={0};
        for(int i=0;i<4;i++){
            int val = eth->ip.bytes[i];
            char t[4]; int n=0;
            do{t[n++]='0'+(val%10);val/=10;}while(val);
            int pos=slen(ip_str);
            while(n>0 && pos<30) ip_str[pos++]=t[--n];
            if(i<3 && pos<30) ip_str[pos++]='.';
            ip_str[pos]=0;
        }
        Graphics::DrawString(x+24,ly,"IP:",S_DIM,0xFF000000);
        Graphics::DrawString(x+60,ly,ip_str,S_TEXT,0xFF000000);
        ly+=18;

        // mac address
        char mac[24]={0};
        static const char hex[]="0123456789ABCDEF";
        int mp=0;
        for(int i=0;i<6;i++){
            mac[mp++]=hex[(eth->mac.bytes[i]>>4)&0xF];
            mac[mp++]=hex[eth->mac.bytes[i]&0xF];
            if(i<5) mac[mp++]=':';
        }
        mac[mp]=0;
        Graphics::DrawString(x+24,ly,"MAC:",S_DIM,0xFF000000);
        Graphics::DrawString(x+70,ly,mac,S_TEXT,0xFF000000);
        ly+=18;

        Graphics::DrawString(x+24,ly,"Driver:",S_DIM,0xFF000000);
        Graphics::DrawString(x+90,ly,"E1000 (Intel 82540EM)",S_TEXT,0xFF000000);
        ly+=18;
    } else {
        Graphics::DrawString(x+24,ly,"No Ethernet adapter detected",S_DIM,0xFF000000);
        ly+=18;
    }
    ly+=8;

    Graphics::DrawLine(x+12, ly, x+w-12, ly, S_BORDER);
    ly+=8;
    Graphics::DrawString(x+12,ly,"WiFi",S_TEXT,0xFF000000);
    WiFiState wifi_state = WiFi::GetState();
    unsigned int wifi_status_color = S_DIM;
    if (wifi_state == WIFI_CONNECTED) wifi_status_color = S_TOGGLE_ON;
    else if (wifi_state == WIFI_SCANNING || wifi_state == WIFI_CONNECTING) wifi_status_color = S_ACCENT;
    Graphics::DrawString(x+190, ly, WiFi::StateString(), wifi_status_color, 0xFF000000);
    ly+=22;
    WiFiNetwork* connected_wifi = WiFi::GetConnectedNetwork();
    if (wifi_state == WIFI_CONNECTED && connected_wifi) {
        Graphics::DrawString(x+24,ly,"Connected network:",S_DIM,0xFF000000);
        Graphics::DrawString(x+150,ly,connected_wifi->ssid,S_TEXT,0xFF000000);
        ly+=16;

        Graphics::DrawString(x+24,ly,"Signal:",S_DIM,0xFF000000);
        char sig[8]={0};
        int_to_str(WiFi::GetSignalStrength(), sig, sizeof(sig));
        sapp(sig,"/4",sizeof(sig));
        Graphics::DrawString(x+90,ly,sig,S_TEXT,0xFF000000);
    } else if (wifi_state == WIFI_SCANNING) {
        Graphics::DrawString(x+24,ly,"Scanning for wireless networks...",S_DIM,0xFF000000);
    } else if (WiFi::GetNetworkCount() > 0) {
        Graphics::DrawString(x+24,ly,"Wireless networks are available.",S_DIM,0xFF000000);
    } else {
        Graphics::DrawString(x+24,ly,"No wireless networks available.",S_DIM,0xFF000000);
    }
    ly+=24;
    Graphics::DrawString(x+24,ly,"Driver:",S_DIM,0xFF000000);
    Graphics::DrawString(x+90,ly,
        wifi_drv ? (wifi_drv->description[0] ? wifi_drv->description : wifi_drv->name)
                 : "No native WiFi binding yet",
        S_TEXT,0xFF000000);
    ly+=18;

    Graphics::DrawLine(x+12, ly, x+w-12, ly, S_BORDER);
    ly+=8;
    Graphics::DrawString(x+12,ly,"Bluetooth",S_TEXT,0xFF000000);
    Graphics::DrawString(x+190, ly, bt_drv ? "Bound" : "No adapter",
                         bt_drv ? S_TOGGLE_ON : S_DIM, 0xFF000000);
    ly+=22;
    Graphics::DrawString(x+24,ly,
        bt_drv ? (bt_drv->description[0] ? bt_drv->description : bt_drv->name)
               : "No Bluetooth controller metadata detected.",
        bt_drv ? S_TEXT : S_DIM,0xFF000000);
    ly+=18;
    if (bt_drv) {
        Graphics::DrawString(x+24,ly,"Binding:",S_DIM,0xFF000000);
        Graphics::DrawString(x+90,ly,bt_drv->name,S_TEXT,0xFF000000);
        ly+=18;
    }
    ly+=6;

    Graphics::DrawLine(x+12, ly, x+w-12, ly, S_BORDER);
    ly+=8;
    Graphics::DrawString(x+12,ly,"Interface",S_TEXT,0xFF000000);
    Graphics::DrawString(x+100,ly,"IP Address",S_TEXT,0xFF000000);
    Graphics::DrawString(x+w-50,ly,"Status",S_TEXT,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"lo",S_DIM,0xFF000000);
    Graphics::DrawString(x+100,ly,"127.0.0.1",S_DIM,0xFF000000);
    Graphics::DrawString(x+w-32,ly,"UP",S_TOGGLE_ON,0xFF000000);
    ly+=16;

    if (eth) {
        char ip2[32]={0};
        for(int i=0;i<4;i++){
            int val=eth->ip.bytes[i]; char t[4]; int n=0;
            do{t[n++]='0'+(val%10);val/=10;}while(val);
            int pos=slen(ip2);
            while(n>0&&pos<30) ip2[pos++]=t[--n];
            if(i<3&&pos<30) ip2[pos++]='.';
            ip2[pos]=0;
        }
        Graphics::DrawString(x+12,ly,"eth0",S_DIM,0xFF000000);
        Graphics::DrawString(x+100,ly,ip2,S_DIM,0xFF000000);
        Graphics::DrawString(x+w-32,ly, eth_up?"UP":"DOWN",
                             eth_up?S_TOGGLE_ON:0xFFE74C3C, 0xFF000000);
    } else {
        Graphics::DrawString(x+12,ly,"eth0",S_DIM,0xFF000000);
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

    // list installed
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
    char system_line1[80]={0}, system_line2[80]={0};
    char cpu_line1[80]={0}, cpu_line2[80]={0};
    char gpu_line1[80]={0}, gpu_line2[80]={0};
    char radio_line1[80]={0}, radio_line2[80]={0};
    format_display_summary(system_line1, sizeof(system_line1), system_line2, sizeof(system_line2));
    format_cpu_summary(cpu_line1, sizeof(cpu_line1), cpu_line2, sizeof(cpu_line2));
    format_gpu_summary(gpu_line1, sizeof(gpu_line1), gpu_line2, sizeof(gpu_line2));
    format_radio_summary(radio_line1, sizeof(radio_line1), radio_line2, sizeof(radio_line2));

    // logo
    Graphics::FillRoundedRect(cx-24, ly, 48, 48, 12, 0xFF3498DB);
    Graphics::DrawString(cx-8, ly+14, "K", S_WHITE, 0xFF000000);
    ly+=60;

    // title
    const char* title = "Kurono OS";
    int tw=slen(title)*8;
    Graphics::DrawString(cx-tw/2, ly, title, S_WHITE, 0xFF000000);
    ly+=20;

    const char* ver = "Version 1.0.0";
    tw=slen(ver)*8;
    Graphics::DrawString(cx-tw/2, ly, ver, S_DIM, 0xFF000000);
    ly+=28;

    int card_w = (w - 40) / 2;
    draw_info_card(x+12, ly, card_w, 66, "System", system_line1, system_line2);
    draw_info_card(x+20+card_w, ly, card_w, 66, "CPU", cpu_line1, cpu_line2);
    ly += 78;
    draw_info_card(x+12, ly, card_w, 66, "Graphics", gpu_line1, gpu_line2);
    draw_info_card(x+20+card_w, ly, card_w, 66, "Connectivity", radio_line1, radio_line2);
    ly += 86;

    const char* info1 = "Kernel: Kurono Microkernel  Shell: KuronoShell + KCL";
    tw=slen(info1)*8;
    Graphics::DrawString(cx-tw/2, ly, info1, S_TEXT, 0xFF000000);
    ly+=18;
    const char* info2 = "Filesystem: KVFS  Architecture: x86_64";
    tw=slen(info2)*8;
    Graphics::DrawString(cx-tw/2, ly, info2, S_TEXT, 0xFF000000);
    ly+=18;

    ly+=12;
    const char* copy = "(c) 2025 Kurono Project";
    tw=slen(copy)*8;
    Graphics::DrawString(cx-tw/2, ly, copy, S_DIM, 0xFF000000);
}

void SettingsApp::RenderSystem(int x,int y,int w,int h){
    (void)h;
    int ly = y + 12;
    bool guest_enabled = Hypervisor::IsLinuxGuestEnabled();
    LinuxGuestProfile profile = Hypervisor::GetLinuxGuestProfile();
    bool can_switch = Hypervisor::CanSwitchLinuxGuestProfile();
    const char* distro = profile == LINUX_GUEST_DEBIAN ? "Debian" : "Alpine";
    const char* command_line = profile == LINUX_GUEST_DEBIAN ? "debian / apt / vm boot-debian"
                                                             : "alpine / apk / vm boot-alpine";

    Graphics::DrawString(x+12,ly,"System",S_HEADING,0xFF000000);
    ly += 26;
    Graphics::DrawString(x+12,ly,"Linux Guest Integration",S_TEXT,0xFF000000);
    unsigned int tog_col = guest_enabled ? S_TOGGLE_ON : S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+190, ly, 40, 20, 10, tog_col);
    int knob_x = guest_enabled ? (x+190+29) : (x+190+11);
    Graphics::FillCircle(knob_x, ly+10, 8, S_WHITE);
    ly += 30;

    Graphics::DrawString(x+12,ly,"Linux Distro:",S_TEXT,0xFF000000);
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+170,ly,distro,guest_enabled?S_WHITE:S_DIM,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly += 28;

    Graphics::DrawString(x+12,ly,"Switch State:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,can_switch?"Unlocked":"Locked while a VM exists",
                         can_switch?S_TOGGLE_ON:S_DIM,0xFF000000);
    ly += 22;

    Graphics::DrawString(x+12,ly,"Commands:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,guest_enabled?command_line:"Disabled",S_DIM,0xFF000000);
    ly += 22;

    Graphics::DrawString(x+12,ly,"Guest Model:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,
        profile == LINUX_GUEST_DEBIAN ? "debootstrap minbase rootfs (planned)"
                                      : "embedded kernel + initramfs",
        S_DIM,0xFF000000);
    ly += 26;

    Graphics::FillRoundedRect(x+12, ly, w-24, 94, 10, S_SIDEBAR);
    Graphics::DrawString(x+24,ly+10,"Linux",S_HEADING,0xFF000000);
    if (profile == LINUX_GUEST_DEBIAN) {
        Graphics::DrawString(x+24,ly+30,"Debian mode is selection-only for now.",S_TEXT,0xFF000000);
        Graphics::DrawString(x+24,ly+48,"Boot will stay disabled until a minbase rootfs is embedded.",S_DIM,0xFF000000);
        Graphics::DrawString(x+24,ly+66,"Planned host step: debootstrap --variant=minbase stable ./debian-root",S_DIM,0xFF000000);
    } else {
        Graphics::DrawString(x+24,ly+30,"Alpine mode remains the active bootable guest.",S_TEXT,0xFF000000);
        Graphics::DrawString(x+24,ly+48,"Driver extraction, apk, ffmpeg, and pwsh stay routed here.",S_DIM,0xFF000000);
        Graphics::DrawString(x+24,ly+66,"Only one guest profile can be active at a time.",S_DIM,0xFF000000);
    }
}

//  render + input
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
        case STAB_SYSTEM:      RenderSystem(px,cy,pw,ch); break;
        case STAB_ABOUT:       RenderAbout(px,cy,pw,ch); break;
        default: break;
    }
}

bool SettingsApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)key;
    if(!clicked) return false;

    Window* w = (Window*)win_ptr;
    // mx, my are already content-local (0,0 = top-left of content area)

    // sidebar tab selection
    if(mx >= 0 && mx < SIDEBAR_W){
        int tab = (my - 8) / 36;
        if(tab>=0 && tab<STAB_COUNT){
            current_tab=(SettingsTab)tab;
            return true;
        }
    }

    // content area clicks (right of sidebar)
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
    if (current_tab == STAB_SYSTEM)
        return HandleSystemInput(rx, ry, pw, ph);

    return false;
}

bool SettingsApp::HandleDisplayInput(int rx, int ry, int pw, int ph) {
    (void)ph;
    // layout (y offsets from content top):
    //   38  = resolution
    //   64  = mode control info (read-only)
    //   90  = monitor hz (read-only)
    //  116  = max fps selector
    //  142  = mouse sensitivity selector
    //  168  = fb cache (read-only)
    //  194  = response time (read-only)
    //  220  = color depth (read-only)
    //  246  = brightness slider
    //  276  = cursor blink toggle
    //  304  = dark mode toggle
    //  332  = animations toggle
    int ly = 12 + 26; // = 38 → resolution row

    // resolution < > buttons
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
    ly += 26; // = 64 → mode control info
    ly += 26; // = 90 → monitor hz (read-only, skip)
    ly += 26; // = 116 → max fps selector

    // max fps < > buttons  -  expanded rate options including monitor-detected hz
    if (ry >= ly && ry < ly + 20) {
        // build dynamic rate list  -  standard rates + detected monitor hz
        int rates[8]; int nrates = 0;
        static const int std_rates[] = {30, 60, 75, 120, 144, 165, 240, 360};
        uint32_t mon_hz = Graphics::GetMonitorHz();

        // build rate list: only include standard rates up to the monitor max.
        // the monitor's detected hz is always the ceiling.
        int max_hz = (mon_hz > 0) ? (int)mon_hz : 360;
        for (int i = 0; i < 8; i++) {
            if (std_rates[i] <= max_hz && nrates < 8)
                rates[nrates++] = std_rates[i];
        }
        // ensure the monitor hz itself is in the list as the top option
        if (mon_hz > 0 && (nrates == 0 || rates[nrates-1] != max_hz)) {
            // insert sorted
            int ins = nrates;
            for (int i = 0; i < nrates; i++) {
                if (max_hz < rates[i]) { ins = i; break; }
            }
            if (nrates < 8) {
                for (int i = nrates; i > ins; i--) rates[i] = rates[i-1];
                rates[ins] = max_hz;
                nrates++;
            }
        }

        if (rx >= 140 && rx < 160) { // left
            for (int i = nrates - 1; i >= 0; i--) {
                if (rates[i] < state.refresh_rate) {
                    state.refresh_rate = rates[i];
                    ApplyRefreshRate(state.refresh_rate);
                    return true;
                }
            }
            return true;
        }
        if (rx >= pw-50 && rx < pw-30) { // right
            for (int i = 0; i < nrates; i++) {
                if (rates[i] > state.refresh_rate) {
                    state.refresh_rate = rates[i];
                    ApplyRefreshRate(state.refresh_rate);
                    return true;
                }
            }
            return true;
        }
    }
    ly += 26; // = 142 → mouse sensitivity

    if (ry >= ly && ry < ly + 20) {
        if (rx >= 140 && rx < 160) {
            if (state.mouse_sensitivity > 1) state.mouse_sensitivity--;
            Mouse::SetSensitivity((uint16_t)state.mouse_sensitivity);
            return true;
        }
        if (rx >= pw-50 && rx < pw-30) {
            if (state.mouse_sensitivity < 4) state.mouse_sensitivity++;
            Mouse::SetSensitivity((uint16_t)state.mouse_sensitivity);
            return true;
        }
    }

    ly += 26; // = 168 → fb cache (read-only)
    ly += 26; // = 194 → response time (read-only)
    ly += 26; // = 220 → color depth (read-only)
    ly += 26; // = 246 → brightness

    // brightness slider
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
    ly += 30; // → cursor blink

    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.cursor_blink = !state.cursor_blink;
        return true;
    }
    ly += 28; // → dark mode

    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.dark_mode = !state.dark_mode;
        return true;
    }
    ly += 28; // → animations

    if (ry >= ly && ry < ly + 20 && rx >= 140 && rx < 180) {
        state.animations = !state.animations;
        return true;
    }

    return false;
}

//  sound
void SettingsApp::RenderSound(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Sound",S_HEADING,0xFF000000);
    ly+=26;

    // master volume slider
    Graphics::DrawString(x+12,ly,"Master Volume:",S_TEXT,0xFF000000);
    int sl_x=x+140, sl_w=w-200;
    Graphics::FillRoundedRect(sl_x,ly+4,sl_w,8,4,S_SLIDER_BG);
    int fill=(sl_w*state.master_volume)/100;
    if(fill>0) Graphics::FillRoundedRect(sl_x,ly+4,fill,8,4,state.muted?S_TOGGLE_OFF:S_SLIDER_FG);
    Graphics::FillCircle(sl_x+fill,ly+8,7,S_WHITE);
    char pct[8]; int_to_str(state.master_volume,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+w-48,ly,pct,S_DIM,0xFF000000);
    ly+=30;

    // alert volume slider
    Graphics::DrawString(x+12,ly,"Alert Volume:",S_TEXT,0xFF000000);
    Graphics::FillRoundedRect(sl_x,ly+4,sl_w,8,4,S_SLIDER_BG);
    fill=(sl_w*state.alert_volume)/100;
    if(fill>0) Graphics::FillRoundedRect(sl_x,ly+4,fill,8,4,S_SLIDER_FG);
    Graphics::FillCircle(sl_x+fill,ly+8,7,S_WHITE);
    int_to_str(state.alert_volume,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+w-48,ly,pct,S_DIM,0xFF000000);
    ly+=30;

    // mute toggle
    Graphics::DrawString(x+12,ly,"Mute All:",S_TEXT,0xFF000000);
    unsigned int tc=state.muted?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.muted?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // output device selector
    Graphics::DrawString(x+12,ly,"Output Device:",S_TEXT,0xFF000000);
    static const char* devs[]={"Speakers","HDMI Audio","Headphones"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,devs[state.output_device],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // spatial audio toggle
    Graphics::DrawString(x+12,ly,"Spatial Audio:",S_TEXT,0xFF000000);
    tc=state.spatial_audio?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    kx=state.spatial_audio?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // audio info
    Graphics::DrawLine(x+12,ly,x+w-12,ly,S_BORDER);
    ly+=10;
    Graphics::DrawString(x+12,ly,"Device:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,Audio::IsAvailable()?"Sound Blaster 16":"Not detected",
                         Audio::IsAvailable()?S_DIM:0xFFE74C3C,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Sample Rate:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"22050 Hz",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Bit Depth:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"8-bit",S_DIM,0xFF000000);
    ly+=18;
    Graphics::DrawString(x+12,ly,"Channels:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"1 (Mono)",S_DIM,0xFF000000);
    ly+=18;
    if (Audio::IsAvailable()) {
        Graphics::DrawString(x+12,ly,"DSP Version:",S_TEXT,0xFF000000);
        char dsp[16]={0};
        int dver = Audio::GetDSPVersion();
        int_to_str((dver>>8)&0xFF, dsp, 16);
        sapp(dsp, ".", 16);
        char lo[4]; int_to_str(dver&0xFF, lo, 4);
        sapp(dsp, lo, 16);
        Graphics::DrawString(x+140,ly,dsp,S_DIM,0xFF000000);
    }
}

bool SettingsApp::HandleSoundInput(int rx, int ry, int pw, int ph){
    (void)ph;
    int ly=12+26; // master volume
    // master volume slider
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
    // alert volume slider
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
    // mute toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){
        state.muted=!state.muted;
        Audio::SetMuted(state.muted);
        return true;
    }
    ly+=28;
    // output device arrows
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.output_device>0)state.output_device--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.output_device<2)state.output_device++;return true;}
    }
    ly+=28;
    // spatial audio toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.spatial_audio=!state.spatial_audio;return true;}
    return false;
}

//  storage
void SettingsApp::RenderStorage(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Storage",S_HEADING,0xFF000000);
    ly+=26;

    // kvfs volume
    Graphics::DrawString(x+12,ly,"KVFS Volume",S_TEXT,0xFF000000);
    ly+=20;

    // usage bar
    size_t heap_used = KernelHeap::GetUsed();
    size_t heap_total = 64*1024*1024; // 64 mb heap
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

    // filesystem nodes
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

    // disk info
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

//  power
void SettingsApp::RenderPower(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Power & Sleep",S_HEADING,0xFF000000);
    ly+=26;

    // power plan selector
    Graphics::DrawString(x+12,ly,"Power Plan:",S_TEXT,0xFF000000);
    static const char* plans[]={"Balanced","High Performance","Power Saver"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,plans[state.power_plan],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // sleep timeout
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

    // screen timeout
    Graphics::DrawString(x+12,ly,"Screen Off:",S_TEXT,0xFF000000);
    if(state.screen_timeout==0) scpy(mins,"Never",16);
    else {int_to_str(state.screen_timeout,mins,12); sapp(mins," min",16);}
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,mins,S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // fast startup toggle
    Graphics::DrawString(x+12,ly,"Fast Startup:",S_TEXT,0xFF000000);
    unsigned int tc=state.fast_startup?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.fast_startup?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=32;

    // power stats
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
    // power plan
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.power_plan>0)state.power_plan--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.power_plan<2)state.power_plan++;return true;}
    }
    ly+=28;
    // sleep timeout
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
    // screen timeout
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
    // fast startup toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.fast_startup=!state.fast_startup;return true;}
    return false;
}

//  personalization
void SettingsApp::RenderPersonalize(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"Personalization",S_HEADING,0xFF000000);
    ly+=26;

    // accent color palette
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

    // desktop icons toggle
    Graphics::DrawString(x+12,ly,"Desktop Icons:",S_TEXT,0xFF000000);
    unsigned int tc=state.show_desktop_icons?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.show_desktop_icons?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // taskbar position
    Graphics::DrawString(x+12,ly,"Taskbar Pos:",S_TEXT,0xFF000000);
    static const char* tpos[]={"Bottom","Top"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,tpos[state.taskbar_pos],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // icon size
    Graphics::DrawString(x+12,ly,"Icon Size:",S_TEXT,0xFF000000);
    static const char* isz[]={"Small","Medium","Large"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,isz[state.icon_size],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // transparency toggle
    Graphics::DrawString(x+12,ly,"Transparency:",S_TEXT,0xFF000000);
    tc=state.transparency?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    kx=state.transparency?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=28;

    // wallpaper selector
    Graphics::DrawString(x+12,ly,"Wallpaper:",S_TEXT,0xFF000000);
    static const char* wp[]={"Anime","Gradient","Solid"};
    Graphics::FillRoundedRect(x+140,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+144,ly,"<",S_WHITE,0xFF000000);
    Graphics::DrawString(x+168,ly,wp[state.wallpaper_idx],S_WHITE,0xFF000000);
    Graphics::FillRoundedRect(x+w-50,ly-2,20,20,4,S_ACCENT);
    Graphics::DrawString(x+w-46,ly,">",S_WHITE,0xFF000000);
    ly+=28;

    // ui scale
    Graphics::DrawString(x+12,ly,"UI Scale:",S_TEXT,0xFF000000);
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
    // accent color
    if(ry>=ly && ry<ly+20){
        for(int i=0;i<8;i++){
            int cx=140+i*24;
            if(rx>=cx && rx<cx+20){state.accent_color=i;return true;}
        }
    }
    ly+=28;
    // desktop icons toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.show_desktop_icons=!state.show_desktop_icons;return true;}
    ly+=28;
    // taskbar pos
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.taskbar_pos>0)state.taskbar_pos--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.taskbar_pos<1)state.taskbar_pos++;return true;}
    }
    ly+=28;
    // icon size
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.icon_size>0)state.icon_size--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.icon_size<2)state.icon_size++;return true;}
    }
    ly+=28;
    // transparency
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.transparency=!state.transparency;return true;}
    ly+=28;
    // wallpaper arrows
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){
            if(state.wallpaper_idx>0){
                state.wallpaper_idx--;
                ApplyWallpaperSelection();
            }
            return true;
        }
        if(rx>=pw-50 && rx<pw-30){
            if(state.wallpaper_idx<2){
                state.wallpaper_idx++;
                ApplyWallpaperSelection();
            }
            return true;
        }
    }
    ly+=28;
    // font scale
    if(ry>=ly && ry<ly+20){
        if(rx>=140 && rx<160){if(state.font_scale>1)state.font_scale--;return true;}
        if(rx>=pw-50 && rx<pw-30){if(state.font_scale<3)state.font_scale++;return true;}
    }
    return false;
}

//  updates  (fetches from server.satorut.com)
void SettingsApp::RenderUpdates(int x,int y,int w,int h){
    (void)h;
    int ly=y+12;
    Graphics::DrawString(x+12,ly,"System Updates",S_HEADING,0xFF000000);
    ly+=28;

    // auto-update toggle
    Graphics::DrawString(x+12,ly,"Auto-Update:",S_TEXT,0xFF000000);
    unsigned int tc=state.auto_update?S_TOGGLE_ON:S_TOGGLE_OFF;
    Graphics::FillRoundedRect(x+140,ly,40,20,10,tc);
    int kx=state.auto_update?(x+140+29):(x+140+11);
    Graphics::FillCircle(kx,ly+10,8,S_WHITE);
    ly+=32;

    // update server info
    Graphics::DrawString(x+12,ly,"Update Server:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"server.satorut.com",0xFF3498DB,0xFF000000);
    ly+=20;
    Graphics::DrawString(x+12,ly,"Channel:",S_TEXT,0xFF000000);
    Graphics::DrawString(x+140,ly,"stable",S_DIM,0xFF000000);
    ly+=28;

    // status display
    Graphics::DrawLine(x+12,ly,x+w-12,ly,S_BORDER);
    ly+=12;

    switch(state.update_status){
        case 0: { // idle
            // check real network status
            NetworkInterface* eth = Network::GetInterface("eth0");
            bool net_up = eth && eth->state == NIC_UP;
            if (!net_up) {
                Graphics::DrawString(x+12,ly,"Network: Disconnected",0xFFE74C3C,0xFF000000);
                ly+=20;
                Graphics::DrawString(x+12,ly,"Connect to a network to check for updates.",S_DIM,0xFF000000);
            } else {
                Graphics::DrawString(x+12,ly,"Status: Ready to check",S_DIM,0xFF000000);
                ly+=20;
                Graphics::DrawString(x+12,ly,"Network: Connected via eth0",S_TOGGLE_ON,0xFF000000);
            }
            ly+=28;
            // check button
            Graphics::FillRoundedRect(x+12,ly,140,28,8,S_ACCENT);
            Graphics::DrawString(x+24,ly+6,"Check for Updates",S_WHITE,0xFF000000);
            break;
        }
        case 1: { // checking
            Graphics::DrawString(x+12,ly,"Resolving server.satorut.com...",0xFFF39C12,0xFF000000);
            ly+=20;
            // animated dots
            Graphics::FillCircle(x+24,ly+4,4,S_HEADING);
            Graphics::FillCircle(x+40,ly+4,4,S_SLIDER_BG);
            Graphics::FillCircle(x+56,ly+4,4,S_SLIDER_BG);
            ly+=20;
            // check if dns resolution works (real attempt)
            IPv4Address resolved;
            bool dns_ok = Network::Resolve("server.satorut.com", &resolved);
            if (dns_ok && (resolved.bytes[0] != 0)) {
                // dns resolved  -  but we can't actually http fetch
                state.update_status = 5; // new state: server unreachable
            } else {
                state.update_status = 5;
            }
            break;
        }
        case 5: { // server unreachable (honest state)
            Graphics::DrawString(x+12,ly,"Could not reach update server",0xFFE74C3C,0xFF000000);
            ly+=22;
            Graphics::DrawString(x+24,ly,"DNS resolved, but HTTP not",S_DIM,0xFF000000);
            ly+=16;
            Graphics::DrawString(x+24,ly,"implemented yet. Updates require",S_DIM,0xFF000000);
            ly+=16;
            Graphics::DrawString(x+24,ly,"an HTTP/TLS stack.",S_DIM,0xFF000000);
            ly+=28;
            // current version info
            Graphics::FillCircle(x+24,ly+7,8,S_TOGGLE_ON);
            Graphics::DrawString(x+40,ly,"Kurono OS v1.0.0 (installed)",S_TOGGLE_ON,0xFF000000);
            ly+=28;
            Graphics::FillRoundedRect(x+12,ly,140,28,8,S_ACCENT);
            Graphics::DrawString(x+24,ly+6,"Check for Updates",S_WHITE,0xFF000000);
            break;
        }
        default: {
            // fallback: treat as idle
            state.update_status = 0;
            break;
        }
    }
}

bool SettingsApp::HandleUpdatesInput(int rx,int ry,int pw,int ph){
    (void)pw;(void)ph;
    int ly=12+28;
    // auto-update toggle
    if(ry>=ly && ry<ly+20 && rx>=140 && rx<180){state.auto_update=!state.auto_update;return true;}

    // check for updates button (appears in states 0 and 5)
    if(state.update_status==0 || state.update_status==5){
        // calculate button position based on state
        int btn_y;
        if(state.update_status==0){
            btn_y = 12+28+32+20+20+28+12+20+28;
        } else {
            btn_y = 12+28+32+20+20+28+12+22+16+16+16+28+28;
        }
        if(ry>=btn_y && ry<btn_y+28 && rx>=12 && rx<152){
            state.update_status=1;
            return true;
        }
    }
    return false;
}bool SettingsApp::HandleNetworkInput(int rx, int ry, int pw, int ph) {
    (void)rx; (void)ry; (void)pw; (void)ph;
    // network tab is read-only  -  shows real hardware status
    return false;
}

bool SettingsApp::HandleSystemInput(int rx,int ry,int pw,int ph){
    (void)ph;
    int ly = 12 + 26;
    if (ry >= ly && ry < ly + 20 && rx >= 190 && rx < 230) {
        state.linux_guest_enabled = !state.linux_guest_enabled;
        Hypervisor::SetLinuxGuestEnabled(state.linux_guest_enabled);
        return true;
    }
    ly += 30;
    if (ry >= ly && ry < ly + 20 && state.linux_guest_enabled) {
        int new_profile = state.linux_guest_profile;
        if (rx >= 140 && rx < 160) new_profile = state.linux_guest_profile > 0 ? state.linux_guest_profile - 1 : 0;
        if (rx >= pw - 50 && rx < pw - 30) new_profile = state.linux_guest_profile < 1 ? state.linux_guest_profile + 1 : 1;
        if (new_profile != state.linux_guest_profile) {
            if (Hypervisor::SetLinuxGuestProfile((LinuxGuestProfile)new_profile)) {
                state.linux_guest_profile = new_profile;
            }
            return true;
        }
    }
    return false;
}
