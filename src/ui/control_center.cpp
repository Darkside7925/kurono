//  kurono os  -  control center implementation
#include "control_center.h"
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "font.h"
#include "../net/network.h"
#include "../system/user_mgmt.h"
#include "desktop.h"

// state -------------------------------------------------------------------
bool ControlCenter::open = false;
int  ControlCenter::screen_w = 0;
int  ControlCenter::screen_h = 0;
int  ControlCenter::panel_x = 0;
int  ControlCenter::panel_y = 0;
int  ControlCenter::panel_w = 360;
int  ControlCenter::panel_h = 460;
bool ControlCenter::dragging_volume = false;
bool ControlCenter::dragging_brightness = false;
int  ControlCenter::brightness_pct = 80;
bool ControlCenter::airplane_mode = false;
bool ControlCenter::night_light   = false;
bool ControlCenter::focus_mode    = false;
bool ControlCenter::do_not_disturb = false;

int ControlCenter::wifi_x=0, ControlCenter::wifi_y=0, ControlCenter::wifi_w=0, ControlCenter::wifi_h=0;
int ControlCenter::bt_x=0, ControlCenter::bt_y=0, ControlCenter::bt_w=0, ControlCenter::bt_h=0;
int ControlCenter::air_x=0, ControlCenter::air_y=0, ControlCenter::air_w=0, ControlCenter::air_h=0;
int ControlCenter::night_x=0, ControlCenter::night_y=0, ControlCenter::night_w=0, ControlCenter::night_h=0;
int ControlCenter::focus_x=0, ControlCenter::focus_y=0, ControlCenter::focus_w=0, ControlCenter::focus_h=0;
int ControlCenter::dnd_x=0, ControlCenter::dnd_y=0, ControlCenter::dnd_w=0, ControlCenter::dnd_h=0;
int ControlCenter::bright_track_x=0, ControlCenter::bright_track_y=0, ControlCenter::bright_track_w=0;
int ControlCenter::vol_track_x=0, ControlCenter::vol_track_y=0, ControlCenter::vol_track_w=0;
int ControlCenter::signout_x=0, ControlCenter::signout_y=0, ControlCenter::signout_w=0, ControlCenter::signout_h=0;
int ControlCenter::settings_x=0, ControlCenter::settings_y=0, ControlCenter::settings_w=0, ControlCenter::settings_h=0;
int ControlCenter::lock_x=0, ControlCenter::lock_y=0, ControlCenter::lock_w=0, ControlCenter::lock_h=0;

// helpers -----------------------------------------------------------------
static int slen(const char* s){ int n=0; if(s) while(s[n]) n++; return n; }
static void scpy(char* d, const char* s, int mx){
    int i=0; if(s) while(s[i] && i<mx-1){ d[i]=s[i]; i++; } d[i]=0;
}
static void int_to_str(int v, char* b, int mx){
    if(mx<2){ b[0]=0; return; }
    if(v<0){ b[0]='-'; int_to_str(-v, b+1, mx-1); return; }
    char t[16]; int n=0;
    do{ t[n++]=(char)('0'+(v%10)); v/=10; } while(v && n<15);
    int i=0; while(n>0 && i<mx-1) b[i++]=t[--n];
    b[i]=0;
}

void ControlCenter::Init(int sw, int sh){
    screen_w = sw; screen_h = sh;
    open = false;
    Layout();
    if (Audio::IsAvailable()) brightness_pct = 80;
}

void ControlCenter::OnScreenResize(int sw, int sh){
    screen_w = sw; screen_h = sh;
    Layout();
}

void ControlCenter::Layout(){
    panel_w = 360;
    panel_h = 460;
    panel_x = screen_w - panel_w - 12;
    if (panel_x < 12) panel_x = 12;
    // anchor above the taskbar (taskbar is ~44 high)
    panel_y = screen_h - panel_h - 56;
    if (panel_y < 12) panel_y = 12;
}

void ControlCenter::Open(){ Layout(); open = true; }
void ControlCenter::OpenAt(int anchor_x, int anchor_y){
    panel_w = 360; panel_h = 460;
    // Centre the panel horizontally on the click anchor and float it
    // just above the click point.  Clamp to a 10 px screen margin so
    // it never spills off-screen.
    panel_x = anchor_x - panel_w / 2;
    panel_y = anchor_y - panel_h - 8;
    if (panel_x < 10) panel_x = 10;
    if (panel_x > screen_w - panel_w - 10) panel_x = screen_w - panel_w - 10;
    if (panel_y < 10) panel_y = 10;
    if (panel_y > screen_h - panel_h - 10) panel_y = screen_h - panel_h - 10;
    open = true;
}
void ControlCenter::ToggleAt(int ax, int ay){ if (open) Close(); else OpenAt(ax, ay); }
void ControlCenter::Close(){ open = false; dragging_volume = false; dragging_brightness = false; }
void ControlCenter::Toggle(){ if (open) Close(); else Open(); }
bool ControlCenter::IsOpen(){ return open; }
int  ControlCenter::GetX(){ return panel_x; }
int  ControlCenter::GetY(){ return panel_y; }
int  ControlCenter::GetW(){ return panel_w; }
int  ControlCenter::GetH(){ return panel_h; }

// rendering helpers -------------------------------------------------------
void ControlCenter::DrawTile(int x, int y, int w, int h, const char* label,
                             const char* sub, bool active, uint32_t accent){
    uint32_t bg = active ? accent : 0xFF1E1E2E;
    Graphics::FillRoundedRect(x, y, w, h, 12, bg);
    // subtle inner highlight on top
    Graphics::FillRoundedRect(x+1, y+1, w-2, 2, 1, active ? 0x40FFFFFF : 0x10FFFFFF);
    // accent dot when inactive
    if (!active) Graphics::FillCircle(x + 14, y + h/2, 5, accent);
    // label
    uint32_t txt = active ? 0xFFFFFFFF : 0xFFE0E0F0;
    uint32_t sub_col = active ? 0xCCFFFFFF : 0xFF7878A0;
    int tx = x + 28;
    if (label) Graphics::DrawString(tx, y + 8, label, txt, 0xFF000000);
    if (sub)   Graphics::DrawString(tx, y + h - 16, sub, sub_col, 0xFF000000);
}

void ControlCenter::DrawSlider(int x, int y, int w, const char* label, int pct, uint32_t fill){
    Graphics::DrawString(x, y, label, 0xFFB0B0C8, 0xFF000000);
    int track_y = y + 18;
    int track_h = 8;
    Graphics::FillRoundedRect(x, track_y, w, track_h, 4, 0xFF252538);
    int fw = (pct * w) / 100;
    if (fw > 0) Graphics::FillRoundedRect(x, track_y, fw, track_h, 4, fill);
    int knob_x = x + fw - 8;
    if (knob_x < x) knob_x = x;
    if (knob_x > x + w - 16) knob_x = x + w - 16;
    Graphics::FillCircle(knob_x + 8, track_y + track_h/2, 8, 0xFFFFFFFF);
    // percentage text right-aligned
    char pct_buf[8] = {0};
    int_to_str(pct, pct_buf, 6);
    int p = slen(pct_buf); pct_buf[p]='%'; pct_buf[p+1]=0;
    int pw = slen(pct_buf) * 8;
    Graphics::DrawString(x + w - pw, y, pct_buf, 0xFFE0E0F0, 0xFF000000);
}

void ControlCenter::DrawUserCard(int x, int y, int w){
    Graphics::FillRoundedRect(x, y, w, 64, 12, 0xFF1A1A2C);
    User* u = (UserManager::GetUserCount() > 0) ? &UserManager::users[0] : nullptr;
    // try to get the active logged-in user
    int active = UserManager::GetCurrentUserIndex();
    if (active >= 0 && active < UserManager::GetUserCount())
        u = &UserManager::users[active];

    // avatar circle
    uint32_t accent = (u && u->accent_color) ? u->accent_color : 0xFF5C8AFF;
    Graphics::FillCircle(x + 32, y + 32, 22, accent);
    char init[2] = { 'U', 0 };
    if (u && u->display_name[0]) init[0] = u->display_name[0];
    else if (u && u->username[0]) init[0] = u->username[0];
    if (init[0] >= 'a' && init[0] <= 'z') init[0] = (char)(init[0] - 32);
    float fs = 22.0f;
    int tw = FontTTF::Measure(fs, init);
    FontTTF::DrawString(x + 32 - tw/2, y + 32 + (int)(fs * 0.27f), fs, init, 0xFFFFFFFF);

    // name + username
    const char* name = (u && u->display_name[0]) ? u->display_name :
                       (u && u->username[0])     ? u->username     : "User";
    Graphics::DrawString(x + 64, y + 14, name, 0xFFF0F0FF, 0xFF000000);
    char uline[40] = {0};
    if (u){
        scpy(uline, "@", sizeof(uline));
        int n = slen(uline);
        for (int i = 0; u->username[i] && n < (int)sizeof(uline)-1; i++) uline[n++] = u->username[i];
        uline[n] = 0;
    } else {
        scpy(uline, "Guest session", sizeof(uline));
    }
    Graphics::DrawString(x + 64, y + 32, uline, 0xFF7878A0, 0xFF000000);

    // action buttons row (Lock | Settings | Sign Out)
    int btn_y = y + 76;
    int bw = (w - 24) / 3;
    lock_x = x; lock_y = btn_y; lock_w = bw; lock_h = 30;
    settings_x = x + bw + 12; settings_y = btn_y; settings_w = bw; settings_h = 30;
    signout_x = x + 2*(bw + 12); signout_y = btn_y; signout_w = bw; signout_h = 30;

    Graphics::FillRoundedRect(lock_x, lock_y, lock_w, lock_h, 8, 0xFF222238);
    Graphics::FillRoundedRect(settings_x, settings_y, settings_w, settings_h, 8, 0xFF222238);
    Graphics::FillRoundedRect(signout_x, signout_y, signout_w, signout_h, 8, 0xFFE74C3C);

    Graphics::DrawString(lock_x + (lock_w - 4*8)/2,         lock_y + 11,    "Lock",     0xFFE0E0F0, 0xFF000000);
    Graphics::DrawString(settings_x + (settings_w - 8*8)/2, settings_y + 11, "Settings", 0xFFE0E0F0, 0xFF000000);
    Graphics::DrawString(signout_x + (signout_w - 8*8)/2,   signout_y + 11, "Sign Out", 0xFFFFFFFF, 0xFF000000);
}

// main render -------------------------------------------------------------
void ControlCenter::Render(){
    if (!open) return;
    Layout();

    // shadow
    Graphics::FillRoundedRect(panel_x + 6, panel_y + 8, panel_w, panel_h, 16, 0xC0000000);
    // body  -  frosted dark
    Graphics::FillRoundedRect(panel_x, panel_y, panel_w, panel_h, 16, 0xFF12121E);
    // top accent bar
    Graphics::FillRect(panel_x + 16, panel_y + 1, panel_w - 32, 2, 0xFF5C8AFF);

    int x = panel_x + 16;
    int y = panel_y + 18;
    int inner_w = panel_w - 32;

    // Header
    Graphics::DrawString(x, y, "Control Center", 0xFFF0F0FF, 0xFF000000);
    y += 24;

    // Tile grid 2x3 (WiFi, BT | Airplane, Night | Focus, DND)
    int tile_w = (inner_w - 12) / 2;
    int tile_h = 64;
    int gap = 12;

    bool wifi_on = WiFi::IsLinkUp();
    const char* wifi_sub = wifi_on ? "Connected" : "Off";
    wifi_x = x;            wifi_y = y; wifi_w = tile_w; wifi_h = tile_h;
    bt_x   = x + tile_w + gap; bt_y = y; bt_w = tile_w; bt_h = tile_h;
    DrawTile(wifi_x, wifi_y, wifi_w, wifi_h, "Wi-Fi", wifi_sub, wifi_on, 0xFF5C8AFF);
    DrawTile(bt_x,   bt_y,   bt_w,   bt_h,   "Bluetooth", "Off", false,    0xFF3498DB);
    y += tile_h + gap;

    air_x   = x;            air_y = y; air_w = tile_w; air_h = tile_h;
    night_x = x + tile_w + gap; night_y = y; night_w = tile_w; night_h = tile_h;
    DrawTile(air_x, air_y, air_w, air_h, "Airplane", airplane_mode ? "On" : "Off", airplane_mode, 0xFFE67E22);
    DrawTile(night_x, night_y, night_w, night_h, "Night Light", night_light ? "On" : "Off", night_light, 0xFFF39C12);
    y += tile_h + gap;

    focus_x = x;            focus_y = y; focus_w = tile_w; focus_h = tile_h;
    dnd_x   = x + tile_w + gap; dnd_y = y; dnd_w = tile_w; dnd_h = tile_h;
    DrawTile(focus_x, focus_y, focus_w, focus_h, "Focus", focus_mode ? "On" : "Off", focus_mode, 0xFF9B59B6);
    DrawTile(dnd_x,   dnd_y,   dnd_w,   dnd_h,   "Do Not Disturb", do_not_disturb ? "On" : "Off", do_not_disturb, 0xFFE74C3C);
    y += tile_h + 18;

    // Brightness slider
    bright_track_x = x; bright_track_y = y + 18; bright_track_w = inner_w;
    DrawSlider(x, y, inner_w, "Brightness", brightness_pct, 0xFFF1C40F);
    y += 38;

    // Volume slider
    int vol_pct = Audio::IsAvailable() ? Audio::GetMasterVolume() : 80;
    if (Audio::IsAvailable() && Audio::IsMuted()) vol_pct = 0;
    vol_track_x = x; vol_track_y = y + 18; vol_track_w = inner_w;
    DrawSlider(x, y, inner_w, "Volume", vol_pct, 0xFF2ECC71);
    y += 42;

    // User card + actions
    DrawUserCard(x, y, inner_w);
}

// click handling ----------------------------------------------------------
static bool point_in(int mx, int my, int x, int y, int w, int h){
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

bool ControlCenter::HandleClick(int mx, int my){
    if (!open) return false;

    // outside panel → close
    if (!point_in(mx, my, panel_x, panel_y, panel_w, panel_h)){
        Close();
        return false;
    }

    // tile toggles
    if (point_in(mx, my, wifi_x, wifi_y, wifi_w, wifi_h)){
        if (WiFi::IsLinkUp()) WiFi::Disable(); else WiFi::Enable();
        return true;
    }
    if (point_in(mx, my, bt_x, bt_y, bt_w, bt_h)){
        // bluetooth has no driver yet  -  visual toggle only
        return true;
    }
    if (point_in(mx, my, air_x, air_y, air_w, air_h)){
        airplane_mode = !airplane_mode;
        if (airplane_mode){ WiFi::Disable(); }
        return true;
    }
    if (point_in(mx, my, night_x, night_y, night_w, night_h)){
        night_light = !night_light;
        return true;
    }
    if (point_in(mx, my, focus_x, focus_y, focus_w, focus_h)){
        focus_mode = !focus_mode;
        return true;
    }
    if (point_in(mx, my, dnd_x, dnd_y, dnd_w, dnd_h)){
        do_not_disturb = !do_not_disturb;
        return true;
    }

    // brightness slider
    if (my >= bright_track_y - 8 && my <= bright_track_y + 16
        && mx >= bright_track_x && mx <= bright_track_x + bright_track_w){
        int rel = mx - bright_track_x;
        brightness_pct = (rel * 100) / bright_track_w;
        if (brightness_pct < 0) brightness_pct = 0;
        if (brightness_pct > 100) brightness_pct = 100;
        return true;
    }

    // volume slider
    if (my >= vol_track_y - 8 && my <= vol_track_y + 16
        && mx >= vol_track_x && mx <= vol_track_x + vol_track_w){
        int rel = mx - vol_track_x;
        int v = (rel * 100) / vol_track_w;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (Audio::IsAvailable()){
            Audio::SetMasterVolume(v);
            Audio::SetMuted(false);
        }
        return true;
    }

    // bottom action buttons
    if (point_in(mx, my, signout_x, signout_y, signout_w, signout_h)){
        Close();
        DesktopEnvironment::RequestLogout();
        return true;
    }
    if (point_in(mx, my, settings_x, settings_y, settings_w, settings_h)){
        Close();
        DesktopEnvironment::LaunchSettings();
        return true;
    }
    if (point_in(mx, my, lock_x, lock_y, lock_w, lock_h)){
        Close();
        DesktopEnvironment::RequestLogout();  // re-uses logout path → lockscreen
        return true;
    }

    // click inside panel but on no widget → consume to keep panel open
    return true;
}
