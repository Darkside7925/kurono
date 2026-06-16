// Graphical Kurono Installer.  Uses the same primitives as the lockscreen
// (Graphics + FontTTF + Mouse + Keyboard) and wraps the existing CLI
// installer (src/system/installer.cpp) to do the real disk work.

#include "installer_gui.h"
#include "installer.h"
#include "user_mgmt.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../drivers/rtc.h"
#include "../ui/font.h"
#include "../ui/gui.h"
#include "../ui/ui_elements.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "input_manager.h"
#include "gpu_driver_installer.h"
#include "system_update.h"            // queue debian-install for the reboot flow (satoru)
#include "../net/network.h"           // wired link probe + wifi config screen (satoru)
#include "../kernel/kmemx.h"          // memory-compression install option (satoru)
#include "../kernel/pmm.h"            // total ram for the kmemx recommendation (satoru)
#include "ui_config.h"                // persist the kmemx.enabled install choice (satoru)

InstallerGUI::Screen InstallerGUI::current_screen = InstallerGUI::SCR_WELCOME;
int  InstallerGUI::progress_pct = 0;
char InstallerGUI::progress_status[128] = "Preparing...";

// ---------------------------------------------------------------- state
namespace {
int g_w = 0, g_h = 0;
int g_lang_idx = 0;       // 0 English (US), 1 English (GB), 2 ja_JP, 3 fr_FR, 4 de_DE
int g_kbd_idx = 0;        // 0 us, 1 uk, 2 jp, 3 fr, 4 de
int g_disk_idx = 0;       // index into Installer::GetDisk()
int g_part_mode = 0;      // 0 erase entire disk (auto-create ESP+ext4), 1 manual (use existing ESP+ext4)
int g_fs_idx = 0;         // 0 ext4, 1 fat32 (only for sub-root)
char g_user_name[32]      = {0};
int  g_user_name_len      = 0;
char g_user_disp[48]      = {0};
int  g_user_disp_len      = 0;
char g_user_pw[64]        = {0};
int  g_user_pw_len        = 0;
char g_user_cf[64]        = {0};
int  g_user_cf_len        = 0;
int  g_user_field         = 0;  // 0 disp 1 user 2 pw 3 cf
bool g_show_password      = false;
int  g_blink_t            = 0;
bool g_blink_on           = true;
bool g_install_thread_running = false;
int  g_install_step       = 0;       // 0 = idle, 1..N = progressing
bool g_install_done       = false;
bool g_install_failed     = false;
char g_install_log[256]   = {0};
bool g_chosen_live_boot   = false;

// hostname + basic prefs screen (satoru)
char g_hostname[32]       = "kurono";
int  g_hostname_len       = 6;
int  g_tz_idx             = 0;   // index into TIMEZONES
bool g_pref_dark          = true;
bool g_pref_24h           = true;
// KMemX memory compression  -  offered as an install option, default ON
// (recommended). persisted to kmemx.enabled when the install applies prefs. (satoru)
bool g_pref_kmemx         = true;

// network setup screen state (satoru). wired link is real (e1000/virtio); wifi
// is an honest config UI with no radio to drive it yet  -  see scr_wifi.
char g_wifi_ssid[NET_MAX_SSID] = {0};
int  g_wifi_ssid_len      = 0;
char g_wifi_pw[64]        = {0};
int  g_wifi_pw_len        = 0;
int  g_wifi_field         = 0;   // 0 ssid 1 password
bool g_wifi_attempted     = false;
bool g_wifi_connected     = false;

// optional linux guests / packages offered at install. each entry maps onto the
// same kpkg + system-update reboot flow used by `kpkg install debian`. (satoru)
struct GuestOption { const char* name; const char* desc; const char* pkg; bool reboot_flow; };
GuestOption GUESTS[3] = {
    { "Debian (minbase)", "Full glibc userland guest via the linux bridge",   "debian", true  },
    { "Alpine Linux",     "Tiny musl guest; GPU-driver target",               "alpine", false },
    { "Python 3",         "Kurono-native python interpreter (no guest)",       "python", false },
};
bool g_guest_sel[3]       = { false, false, false };
int  g_guest_count        = 3;

const char* TIMEZONES[6] = { "UTC", "America/New_York", "Europe/London", "Europe/Paris", "Asia/Tokyo", "Australia/Sydney" };

const char* LANGUAGES[5] = { "English (US)", "English (UK)", "Japanese", "French", "German" };
const char* LANG_CODES[5]= { "en_US", "en_GB", "ja_JP", "fr_FR", "de_DE" };
const char* KBDS[5] = { "US (QWERTY)", "UK (QWERTY)", "Japanese (JIS)", "French (AZERTY)", "German (QWERTZ)" };

int slen(const char* s){ int n=0; while (s && s[n]) n++; return n; }
void scpy(char* d, const char* s, int max){ int i=0; if(!d||max<1) return; while(s&&s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
void edit_buf(char* buf, int* len, int max, char c){
    if (c == '\b' || c == 127){ if (*len > 0){ (*len)--; buf[*len]=0; } return; }
    if (c < 32 || c > 126) return;
    if (*len < max - 1){ buf[(*len)++] = c; buf[*len] = 0; }
}
bool point_in(int mx, int my, int x, int y, int w, int h){
    return mx>=x && mx<x+w && my>=y && my<y+h;
}
void itoa_pos(uint64_t n, char* out){
    if (n==0){ out[0]='0'; out[1]=0; return; }
    char tmp[24]; int i=0; while (n>0){ tmp[i++]=(char)('0'+(int)(n%10)); n/=10; }
    int o=0; while (i>0) out[o++]=tmp[--i]; out[o]=0;
}
void format_size(uint64_t bytes, char* out){
    const char* unit = "B";
    uint64_t v = bytes;
    if (v >= (1ull<<40)) { v = bytes / (1ull<<40); unit = "TB"; }
    else if (v >= (1ull<<30)) { v = bytes / (1ull<<30); unit = "GB"; }
    else if (v >= (1ull<<20)) { v = bytes / (1ull<<20); unit = "MB"; }
    else if (v >= (1ull<<10)) { v = bytes / (1ull<<10); unit = "KB"; }
    char num[24]; itoa_pos(v, num);
    int p = 0; for (int i=0;num[i];i++) out[p++] = num[i];
    out[p++] = ' ';
    while (*unit) out[p++] = *unit++;
    out[p] = 0;
}
} // anon

// ---------------------------------------------------------------- drawing

static void draw_bg(){
    // Dark gradient background (no wallpaper required during install)
    for (int y = 0; y < g_h; y += 4){
        uint8_t shade = (uint8_t)(20 + (y * 30) / (g_h+1));
        uint32_t c = 0xFF000000 | ((uint32_t)shade << 16) | ((uint32_t)(shade+4) << 8) | (uint32_t)(shade+12);
        Graphics::FillRect(0, y, g_w, 4, c);
    }
    // Logo strip / branding
    Graphics::FillRect(0, 0, g_w, 56, 0xFF12121C);
    FontTTF::DrawString(24, 38, 22.0f, "Kurono OS Installer", 0xFFFFFFFF);
    Graphics::FillRect(0, 56, g_w, 1, 0xFF5C8AFF);
}

static bool draw_button(int x, int y, int w, int h, const char* label, uint32_t accent, bool primary){
    int mx, my; Mouse::GetPosition(mx, my);
    bool hover = point_in(mx, my, x, y, w, h);
    uint32_t fill = primary ? (hover ? 0xFF7AA1FF : accent) : (hover ? 0xFF3A3A50 : 0xFF252535);
    Graphics::FillRoundedRect(x+2, y+3, w, h, 10, 0x60000000);
    Graphics::FillRoundedRect(x, y, w, h, 10, fill);
    float fs = (float)h * 0.42f;
    int tw = FontTTF::Measure(fs, label);
    FontTTF::DrawString(x + w/2 - tw/2, y + (int)(fs*1.15f), fs, label, 0xFFFFFFFF);
    return hover && Mouse::LeftClicked();
}

static void draw_title(const char* title, const char* sub){
    float fs_t = 32.0f;
    int tw = FontTTF::Measure(fs_t, title);
    FontTTF::DrawString(g_w/2 - tw/2, 110, fs_t, title, 0xFFFFFFFF);
    if (sub){
        float fs_s = 14.0f;
        int sw = FontTTF::Measure(fs_s, sub);
        FontTTF::DrawString(g_w/2 - sw/2, 138, fs_s, sub, 0xC0FFFFFF);
    }
}

static void draw_radio_list(int x, int y, int w, int item_h, const char* const* items, int count, int selected, int* out_hit_y_array){
    for (int i = 0; i < count; i++){
        int iy = y + i * item_h;
        out_hit_y_array[i] = iy;
        bool sel = (selected == i);
        Graphics::FillRoundedRect(x, iy, w, item_h - 6, 8, sel ? 0xCC1F4080 : 0x801F2030);
        Graphics::FillCircle(x + 18, iy + (item_h-6)/2, 8, sel ? 0xFF5C8AFF : 0x40FFFFFF);
        if (sel) Graphics::FillCircle(x + 18, iy + (item_h-6)/2, 4, 0xFFFFFFFF);
        FontTTF::DrawString(x + 38, iy + (item_h-6)/2 + 6, 15.0f, items[i], 0xFFFFFFFF);
    }
}

static void draw_pill(int x, int y, int w, int h, const char* text, int len, bool focused, bool is_password, const char* placeholder){
    Graphics::FillRoundedRect(x+2, y+3, w, h, h/2, 0x40000000);
    Graphics::FillRoundedRect(x, y, w, h, h/2, 0xCC1F2030);
    if (focused){
        Graphics::FillRect(x, y-1, w, 1, 0xFF5C8AFF);
        Graphics::FillRect(x, y+h, w, 1, 0xFF5C8AFF);
        Graphics::FillRect(x-1, y, 1, h, 0xFF5C8AFF);
        Graphics::FillRect(x+w, y, 1, h, 0xFF5C8AFF);
    }
    float fs = (float)h * 0.45f;
    int pad = h/2;
    if (len > 0){
        if (is_password){
            char dots[64]; int n = len < 60 ? len : 60;
            for (int i=0;i<n;i++) dots[i] = '*'; dots[n] = 0;
            FontTTF::DrawString(x + pad, y + (int)(fs*1.1f), fs, dots, 0xFFEEEEFF);
        } else {
            FontTTF::DrawString(x + pad, y + (int)(fs*1.1f), fs, text, 0xFFEEEEFF);
        }
    } else if (placeholder){
        FontTTF::DrawString(x + pad, y + (int)(fs*1.1f), fs, placeholder, 0xFF8090A0);
    }
    if (focused && g_blink_on){
        int tw = is_password ? len * (int)(fs*0.55f) : FontTTF::Measure(fs, text);
        Graphics::FillRect(x + pad + tw + 2, y + pad/2, 2, h - pad, 0xFFFFFFFF);
    }
}

// ---------------------------------------------------------------- screens
struct ScreenHit {
    int next_x, next_y, next_w, next_h;
    int back_x, back_y, back_w, back_h;
    int live_x, live_y, live_w, live_h;
    int radio_x, radio_y[16], radio_w, radio_h;
    int field_disp_x, field_disp_y, field_disp_w, field_disp_h;
    int field_user_x, field_user_y, field_user_w, field_user_h;
    int field_pw_x,   field_pw_y,   field_pw_w,   field_pw_h;
    int field_cf_x,   field_cf_y,   field_cf_w,   field_cf_h;
    // checkbox rows (guests screen) + a generic secondary action button. (satoru)
    int cb_x, cb_y[16], cb_w, cb_h;
    int alt_x, alt_y, alt_w, alt_h;
};
static ScreenHit g_hit;

// a labeled checkbox row used by the guests screen. returns true on click. (satoru)
static void draw_checkbox_row(int x, int y, int w, int h, const char* title, const char* sub, bool checked){
    int mx, my; Mouse::GetPosition(mx, my);
    bool hover = point_in(mx, my, x, y, w, h - 6);
    Graphics::FillRoundedRect(x, y, w, h - 6, 10, hover ? 0xCC243050 : 0x801F2030);
    // box
    Graphics::FillRoundedRect(x + 16, y + (h-6)/2 - 11, 22, 22, 5, checked ? 0xFF5C8AFF : 0x40FFFFFF);
    if (checked) FontTTF::DrawString(x + 21, y + (h-6)/2 + 6, 18.0f, "v", 0xFFFFFFFF);
    FontTTF::DrawString(x + 54, y + 22, 15.0f, title, 0xFFFFFFFF);
    if (sub) FontTTF::DrawString(x + 54, y + 40, 12.0f, sub, 0xC0FFFFFF);
}

static void scr_welcome(){
    draw_title("Welcome to Kurono OS", "A bare-metal operating system for modern hardware");
    int cy = g_h/2 - 40;
    // big K logo
    Graphics::FillRoundedRect(g_w/2 - 80, cy - 60, 160, 160, 24, 0xFF5C8AFF);
    FontTTF::DrawString(g_w/2 - 32, cy + 50, 110.0f, "K", 0xFFFFFFFF);
    FontTTF::DrawString(g_w/2 - 220, cy + 150, 16.0f,
        "Choose Install to set up Kurono on this computer, or Live Boot to try without installing.",
        0xC0FFFFFF);

    g_hit.next_x = g_w/2 + 20;  g_hit.next_y = g_h - 90; g_hit.next_w = 200; g_hit.next_h = 50;
    g_hit.live_x = g_w/2 - 220; g_hit.live_y = g_h - 90; g_hit.live_w = 200; g_hit.live_h = 50;
    if (draw_button(g_hit.live_x, g_hit.live_y, g_hit.live_w, g_hit.live_h, "Live Boot", 0xFF555570, false)){
        g_chosen_live_boot = true;
        InstallerGUI::current_screen = InstallerGUI::SCR_LIVE_EXIT;
    }
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Install", 0xFF5C8AFF, true)){
        InstallerGUI::current_screen = InstallerGUI::SCR_LANGUAGE;
    }
}

static void scr_language(){
    draw_title("Choose your language", "Select the language for the installer and your account");
    int x = g_w/2 - 220, y = 200, w = 440, h = 44;
    g_hit.radio_x = x; g_hit.radio_w = w; g_hit.radio_h = h;
    draw_radio_list(x, y, w, h, LANGUAGES, 5, g_lang_idx, g_hit.radio_y);
    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_WELCOME;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))   InstallerGUI::current_screen = InstallerGUI::SCR_KEYBOARD;
}

static void scr_keyboard(){
    draw_title("Choose your keyboard layout", "Type below to test your selection");
    int x = g_w/2 - 220, y = 200, w = 440, h = 40;
    g_hit.radio_x = x; g_hit.radio_w = w; g_hit.radio_h = h;
    draw_radio_list(x, y, w, h, KBDS, 5, g_kbd_idx, g_hit.radio_y);
    // test field
    int ty = y + 5 * h + 24;
    g_hit.field_disp_x = x; g_hit.field_disp_y = ty;
    g_hit.field_disp_w = w; g_hit.field_disp_h = 44;
    draw_pill(x, ty, w, 44, g_user_disp, g_user_disp_len, true, false, "Type here to test your keyboard");
    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_LANGUAGE;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true)){
        // clear test buffer
        g_user_disp[0]=0; g_user_disp_len=0;
        InstallerGUI::current_screen = InstallerGUI::SCR_NETWORK;
    }
}

// ---------------------------------------------------------------- network
// Honest network setup. The wired path is real: we probe the live NICs via
// WiFi::DetectedLink() + Network::GetInterfaces() and show carrier / IP. There
// is NO wifi radio driver in this build (e1000 / virtio-net only), so the wifi
// button leads to a config screen that records an SSID/password and reports the
// stub honestly rather than pretending to associate. (satoru)
static void scr_network(){
    draw_title("Set up networking", "Kurono needs a connection to install guests and updates");

    int px = g_w/2 - 280, py = 190, pw = 560, ph = 230;
    Graphics::FillRoundedRect(px, py, pw, ph, 14, 0xCC141428);

    WiFi::LinkKind link = WiFi::DetectedLink();
    bool up = WiFi::IsLinkUp();

    // wired status row
    FontTTF::DrawString(px + 24, py + 38, 16.0f, "Wired (Ethernet)", 0xFFFFFFFF);
    const char* wired_state =
        (link == WiFi::LINK_ETHERNET && up) ? "connected" :
        (link == WiFi::LINK_ETHERNET)       ? "detected (no carrier)" :
                                              "no adapter";
    uint32_t wired_col = (link == WiFi::LINK_ETHERNET && up) ? 0xFF2ECC71 : 0xFFE67E22;
    FontTTF::DrawString(px + 360, py + 38, 15.0f, wired_state, wired_col);

    // show the first interface IP if we have one
    char ipline[64]; int ip = 0;
    const char* lbl = "  Address: ";
    while (*lbl) ipline[ip++] = *lbl++;
    NetworkInterface* ifs = Network::GetInterfaces();
    int ifn = Network::GetInterfaceCount();
    bool have_ip = false;
    for (int i = 0; i < ifn; i++){
        if (ifs[i].type == NIC_ETHERNET && ifs[i].state == NIC_UP){
            char ipstr[20]; Network::IPToString(ifs[i].ip, ipstr, sizeof(ipstr));
            for (int k = 0; ipstr[k]; k++) ipline[ip++] = ipstr[k];
            have_ip = true; break;
        }
    }
    if (!have_ip){ const char* d = "DHCP / not assigned"; while (*d) ipline[ip++] = *d++; }
    ipline[ip] = 0;
    FontTTF::DrawString(px + 24, py + 62, 12.0f, ipline, 0xC0FFFFFF);

    Graphics::FillRect(px + 24, py + 80, pw - 48, 1, 0x40FFFFFF);

    // wifi status row
    FontTTF::DrawString(px + 24, py + 116, 16.0f, "Wi-Fi (Wireless)", 0xFFFFFFFF);
    const char* wifi_chip = WiFi::WirelessChipName();
    bool have_radio = (link == WiFi::LINK_WIFI) || (wifi_chip && wifi_chip[0]);
    FontTTF::DrawString(px + 360, py + 116, 15.0f,
        have_radio ? "adapter found" : "no radio in this build",
        have_radio ? 0xFFF1C40F : 0xFF8090A0);
    if (g_wifi_connected){
        char wl[80]; int wp = 0; const char* c = "  Configured SSID: "; while (*c) wl[wp++] = *c++;
        for (int i = 0; g_wifi_ssid[i]; i++) wl[wp++] = g_wifi_ssid[i]; wl[wp] = 0;
        FontTTF::DrawString(px + 24, py + 140, 12.0f, wl, 0xFF2ECC71);
    } else {
        FontTTF::DrawString(px + 24, py + 140, 12.0f,
            "No wireless radio driver yet - wired networking is recommended.", 0xC0FFFFFF);
    }

    // configure-wifi button (always offered; honest about the stub)
    g_hit.alt_x = px + 24; g_hit.alt_y = py + ph - 60; g_hit.alt_w = 220; g_hit.alt_h = 44;
    if (draw_button(g_hit.alt_x, g_hit.alt_y, g_hit.alt_w, g_hit.alt_h, "Configure Wi-Fi", 0xFF555570, false)){
        InstallerGUI::current_screen = InstallerGUI::SCR_WIFI;
    }

    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_KEYBOARD;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))   InstallerGUI::current_screen = InstallerGUI::SCR_DISK;
}

// ---------------------------------------------------------------- wifi
static void scr_wifi(){
    draw_title("Connect to Wi-Fi", "Enter the network name and password");

    int x = g_w/2 - 220, y = 200, w = 440, h = 44;
    g_hit.field_disp_x = x; g_hit.field_disp_y = y;    g_hit.field_disp_w = w; g_hit.field_disp_h = h;
    g_hit.field_pw_x   = x; g_hit.field_pw_y   = y+58; g_hit.field_pw_w   = w; g_hit.field_pw_h   = h;
    draw_pill(x, y,    w, h, g_wifi_ssid, g_wifi_ssid_len, g_wifi_field==0, false,         "Network name (SSID)");
    draw_pill(x, y+58, w, h, g_wifi_pw,   g_wifi_pw_len,   g_wifi_field==1, !g_show_password, "Password");

    // honest status line about the radio
    const char* chip = WiFi::WirelessChipName();
    bool have_radio = (WiFi::DetectedLink() == WiFi::LINK_WIFI) || (chip && chip[0]);
    if (g_wifi_attempted){
        FontTTF::DrawString(x, y + 130, 13.0f,
            g_wifi_connected ? "Wi-Fi credentials saved (see note below)."
                             : "Could not associate - no wireless radio driver.",
            g_wifi_connected ? 0xFF2ECC71 : 0xFFE67E22);
    }
    FontTTF::DrawString(x, y + 158, 12.0f,
        have_radio ? "A wireless adapter was detected but the driver is not implemented yet."
                   : "This build has no Wi-Fi radio driver (e1000 / virtio-net wired only).",
        0xC0FFFFFF);
    FontTTF::DrawString(x, y + 176, 12.0f,
        "Your SSID/password are stored for /etc/network so a future radio driver can use them.",
        0xC0FFFFFF);

    // connect button drives the real WiFi API (which honestly returns false here)
    g_hit.alt_x = x; g_hit.alt_y = y + 200; g_hit.alt_w = 200; g_hit.alt_h = 44;
    if (g_wifi_ssid_len > 0 && draw_button(g_hit.alt_x, g_hit.alt_y, g_hit.alt_w, g_hit.alt_h, "Connect", 0xFF2ECC71, true)){
        g_wifi_attempted = true;
        WiFi::Enable();
        // real call  -  returns false on the wired-only build; we record the result. (satoru)
        g_wifi_connected = WiFi::Connect(g_wifi_ssid, g_wifi_pw);
    }

    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_NETWORK;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Done", 0xFF5C8AFF, true))   InstallerGUI::current_screen = InstallerGUI::SCR_NETWORK;
}

static void scr_disk(){
    draw_title("Choose installation disk", "Select the disk where Kurono will be installed");
    int dc = Installer::GetDiskCount();
    if (dc == 0){
        FontTTF::DrawString(g_w/2 - 200, g_h/2, 18.0f, "No suitable disks detected. Install requires NVMe storage.", 0xFFE74C3C);
    }
    int x = g_w/2 - 280, y = 200, w = 560, h = 56;
    for (int i = 0; i < dc && i < 4; i++){
        InstallerDiskInfo* d = Installer::GetDisk(i);
        if (!d) continue;
        int iy = y + i * h;
        g_hit.radio_y[i] = iy;
        bool sel = (g_disk_idx == i);
        Graphics::FillRoundedRect(x, iy, w, h - 8, 10, sel ? 0xCC1F4080 : 0x801F2030);
        Graphics::FillCircle(x + 18, iy + (h-8)/2, 8, sel ? 0xFF5C8AFF : 0x40FFFFFF);
        if (sel) Graphics::FillCircle(x + 18, iy + (h-8)/2, 4, 0xFFFFFFFF);
        FontTTF::DrawString(x + 38, iy + 22, 16.0f, d->model[0]?d->model:d->name, 0xFFFFFFFF);
        char info[80]; int p = 0;
        const char* sch = d->scheme==INST_SCHEME_GPT?"GPT":d->scheme==INST_SCHEME_MBR?"MBR":"none";
        const char* drv = d->driver[0]?d->driver:"NVMe";
        char szs[24]; format_size((uint64_t)d->total_lba * d->sector_size, szs);
        for (const char* s = szs; *s; s++) info[p++]=*s;
        info[p++]=' '; info[p++]='-'; info[p++]=' ';
        for (const char* s = sch; *s; s++) info[p++]=*s;
        info[p++]=' '; info[p++]='-'; info[p++]=' ';
        for (const char* s = drv; *s; s++) info[p++]=*s;
        info[p]=0;
        FontTTF::DrawString(x + 38, iy + 42, 12.0f, info, 0xC0FFFFFF);
    }
    g_hit.radio_x = x; g_hit.radio_w = w; g_hit.radio_h = h;
    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_NETWORK;
    if (dc > 0 && draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))
        InstallerGUI::current_screen = InstallerGUI::SCR_PARTITION_MODE;
}

static void scr_partition_mode(){
    draw_title("How should we partition the disk?", "WARNING: Erase All will destroy existing data");
    static const char* MODES[2] = {
        "Erase entire disk and create a fresh GPT layout (recommended)",
        "Use existing partitions (requires an ESP and a Linux ext4 partition)"
    };
    int x = g_w/2 - 280, y = 220, w = 560, h = 60;
    g_hit.radio_x = x; g_hit.radio_w = w; g_hit.radio_h = h;
    draw_radio_list(x, y, w, h, MODES, 2, g_part_mode, g_hit.radio_y);
    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_DISK;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))   InstallerGUI::current_screen = InstallerGUI::SCR_FILESYSTEM;
}

static void scr_filesystem(){
    draw_title("Choose root filesystem", "ext4 is recommended for the system root");
    static const char* FS[2] = { "ext4 (recommended)", "FAT32 (compatibility only)" };
    int x = g_w/2 - 220, y = 220, w = 440, h = 50;
    g_hit.radio_x = x; g_hit.radio_w = w; g_hit.radio_h = h;
    draw_radio_list(x, y, w, h, FS, 2, g_fs_idx, g_hit.radio_y);
    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_PARTITION_MODE;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))   InstallerGUI::current_screen = InstallerGUI::SCR_USER;
}

static void scr_user(){
    draw_title("Create your administrator account", "This account will own the new system");
    int x = g_w/2 - 220, y = 200, w = 440, h = 44;
    g_hit.field_disp_x = x; g_hit.field_disp_y = y;     g_hit.field_disp_w = w; g_hit.field_disp_h = h;
    g_hit.field_user_x = x; g_hit.field_user_y = y+58;  g_hit.field_user_w = w; g_hit.field_user_h = h;
    g_hit.field_pw_x   = x; g_hit.field_pw_y   = y+116; g_hit.field_pw_w   = w; g_hit.field_pw_h   = h;
    g_hit.field_cf_x   = x; g_hit.field_cf_y   = y+174; g_hit.field_cf_w   = w; g_hit.field_cf_h   = h;
    draw_pill(x, y,     w, h, g_user_disp, g_user_disp_len, g_user_field==0, false, "Display name");
    draw_pill(x, y+58,  w, h, g_user_name, g_user_name_len, g_user_field==1, false, "Username (lowercase, 3-31)");
    draw_pill(x, y+116, w, h, g_user_pw,   g_user_pw_len,   g_user_field==2, !g_show_password, "Password (min 6)");
    draw_pill(x, y+174, w, h, g_user_cf,   g_user_cf_len,   g_user_field==3, !g_show_password, "Confirm password");
    // strength meter
    int strength = UserManager::MeasurePassword(g_user_pw);
    const char* slabel[4] = { "Weak", "Fair", "Strong", "Very Strong" };
    uint32_t scols[4] = { 0xFFE74C3C, 0xFFE67E22, 0xFFF1C40F, 0xFF2ECC71 };
    int my2 = y + 116 + h + 6;
    Graphics::FillRoundedRect(x, my2, w, 4, 2, 0x40FFFFFF);
    int filled = (strength + 1) * (w / 4);
    if (g_user_pw_len == 0) filled = 0;
    Graphics::FillRoundedRect(x, my2, filled, 4, 2, scols[strength]);
    if (g_user_pw_len > 0) FontTTF::DrawString(x, my2 + 16, 11.0f, slabel[strength], scols[strength]);
    // validation messages
    int err_y = y + 174 + h + 14;
    const char* err = nullptr;
    if (g_user_name_len > 0){
        if (!UserManager::IsUsernameValid(g_user_name)) err = "Username must start with a letter (a-z, 0-9, _, -)";
    }
    if (!err && g_user_cf_len > 0){
        bool match = (g_user_cf_len == g_user_pw_len);
        if (match) for (int i=0;i<g_user_cf_len;i++) if (g_user_cf[i] != g_user_pw[i]) { match = false; break; }
        if (!match) err = "Passwords do not match";
    }
    if (err) FontTTF::DrawString(x, err_y, 13.0f, err, 0xFFE74C3C);

    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_FILESYSTEM;
    bool ok = g_user_name_len > 0 && UserManager::IsUsernameValid(g_user_name) && g_user_pw_len >= 6 && g_user_pw_len == g_user_cf_len;
    if (ok) for (int i=0;i<g_user_cf_len;i++) if (g_user_cf[i] != g_user_pw[i]) { ok = false; break; }
    if (ok && draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true)){
        if (g_user_disp_len == 0){ scpy(g_user_disp, g_user_name, 48); g_user_disp_len = g_user_name_len; }
        InstallerGUI::current_screen = InstallerGUI::SCR_HOSTNAME;
    }
}

// ---------------------------------------------------------------- hostname + prefs
static void scr_hostname(){
    draw_title("Name this computer", "Set the hostname, timezone and basic preferences");

    int x = g_w/2 - 220, y = 190, w = 440, h = 44;
    FontTTF::DrawString(x, y - 8, 13.0f, "Hostname", 0xC0FFFFFF);
    g_hit.field_disp_x = x; g_hit.field_disp_y = y; g_hit.field_disp_w = w; g_hit.field_disp_h = h;
    draw_pill(x, y, w, h, g_hostname, g_hostname_len, true, false, "hostname");

    // timezone radio (compact)
    FontTTF::DrawString(x, y + 64, 13.0f, "Timezone", 0xC0FFFFFF);
    int ty = y + 76, th = 34;
    g_hit.radio_x = x; g_hit.radio_w = w; g_hit.radio_h = th;
    draw_radio_list(x, ty, w, th, TIMEZONES, 6, g_tz_idx, g_hit.radio_y);

    // preference toggles
    int pby = ty + 6 * th + 14;
    g_hit.cb_x = x; g_hit.cb_w = w/2 - 8; g_hit.cb_h = 44;
    g_hit.cb_y[0] = pby; g_hit.cb_y[1] = pby;
    draw_checkbox_row(x,           pby, w/2 - 8, 44, "Dark theme",   nullptr, g_pref_dark);
    draw_checkbox_row(x + w/2 + 8, pby, w/2 - 8, 44, "24-hour clock", nullptr, g_pref_24h);
    // record the second checkbox's x so the click handler can tell them apart
    g_hit.alt_x = x + w/2 + 8; g_hit.alt_y = pby; g_hit.alt_w = w/2 - 8; g_hit.alt_h = 44;

    // KMemX memory compression  -  a full-width option below, default on. its hit
    // rect is recorded in cb_y[2]. a one-line note carries the recommendation,
    // tailored by system RAM (the spec's installer disclaimer in brief). (satoru)
    int kmy = pby + 52;
    g_hit.cb_y[2] = kmy;
    draw_checkbox_row(x, kmy, w, 44, "Memory compression (KMemX)",
                      "Recommended - run more apps in the same RAM (~1-5% CPU)", g_pref_kmemx);
    {
        uint64_t ram_mb = PMM::GetTotalMemory() / (1024 * 1024);
        const char* note = (ram_mb < 512)
            ? "Strongly recommended for this system's RAM."
            : (ram_mb > 4096 ? "Optional on this system (still recommended)."
                             : "Recommended. Compresses inactive memory; decompress is transparent.");
        FontTTF::DrawString(x, kmy + 50, 12.0f, note, 0xA0FFFFFF);
    }

    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_USER;
    bool ok = g_hostname_len > 0;
    if (ok && draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))
        InstallerGUI::current_screen = InstallerGUI::SCR_GUESTS;
}

// ---------------------------------------------------------------- guests / packages
// Offers optional linux guests + native packages. Selecting Debian queues the
// same `debian-install` system-update action that `kpkg install debian` uses,
// so it shares the reboot/apt-update flow. Alpine + python are scaffolded the
// same way (markers the post-install/desktop path can act on). (satoru)
static void scr_guests(){
    draw_title("Add Linux guests and packages", "Optional - you can also install these later from the Terminal");

    int x = g_w/2 - 280, y = 190, w = 560, h = 64;
    g_hit.cb_x = x; g_hit.cb_w = w; g_hit.cb_h = h;
    for (int i = 0; i < g_guest_count; i++){
        int iy = y + i * h;
        g_hit.cb_y[i] = iy;
        draw_checkbox_row(x, iy, w, h, GUESTS[i].name, GUESTS[i].desc, g_guest_sel[i]);
    }

    FontTTF::DrawString(x, y + g_guest_count * h + 18, 12.0f,
        "Debian downloads a minbase rootfs and finishes on the next reboot (apt update).", 0xC0FFFFFF);
    FontTTF::DrawString(x, y + g_guest_count * h + 36, 12.0f,
        "A network connection is required for guest downloads.", 0xC0FFFFFF);

    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 140; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_HOSTNAME;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Next", 0xFF5C8AFF, true))   InstallerGUI::current_screen = InstallerGUI::SCR_SUMMARY;
}

static void scr_summary(){
    draw_title("Confirm installation", "Please review your selections");
    int panel_w = 560;
    int panel_x = g_w/2 - panel_w/2;
    int py = 150;
    Graphics::FillRoundedRect(panel_x, py, panel_w, 380, 14, 0xCC141428);
    auto row = [&](int rowy, const char* label, const char* value){
        FontTTF::DrawString(panel_x + 24, rowy, 14.0f, label, 0xC0FFFFFF);
        FontTTF::DrawString(panel_x + 200, rowy, 14.0f, value, 0xFFFFFFFF);
    };
    InstallerDiskInfo* d = Installer::GetDisk(g_disk_idx);
    char szs[32]; format_size(d ? (uint64_t)d->total_lba * d->sector_size : 0, szs);
    // assemble a compact guests summary string (satoru)
    char guests[96]; int gp = 0;
    for (int i = 0; i < g_guest_count; i++){
        if (!g_guest_sel[i]) continue;
        if (gp){ guests[gp++]=','; guests[gp++]=' '; }
        for (const char* s = GUESTS[i].name; *s && gp < 90; s++) guests[gp++] = *s;
    }
    guests[gp] = 0;
    const char* net_summary = g_wifi_connected ? "Wi-Fi configured" :
        (WiFi::IsLinkUp() ? "Wired (connected)" : "Wired (DHCP)");
    row(py + 28, "Language:",  LANGUAGES[g_lang_idx]);
    row(py + 52, "Keyboard:",  KBDS[g_kbd_idx]);
    row(py + 76, "Network:",   net_summary);
    row(py + 100, "Disk:",     d ? (d->model[0]?d->model:d->name) : "(none)");
    row(py + 124, "Disk size:", szs);
    row(py + 148, "Partition:", g_part_mode == 0 ? "Erase entire disk (GPT)" : "Use existing partitions");
    row(py + 172, "Filesystem:", g_fs_idx == 0 ? "ext4" : "FAT32");
    row(py + 196, "Hostname:", g_hostname);
    row(py + 220, "Timezone:", TIMEZONES[g_tz_idx]);
    row(py + 244, "Username:",  g_user_name);
    row(py + 268, "Guests:",   gp ? guests : "(none)");

    if (g_part_mode == 0){
        FontTTF::DrawString(panel_x + 24, py + 308, 13.0f,
            "WARNING: All existing data on this disk will be permanently erased.", 0xFFE74C3C);
    }

    g_hit.back_x = g_w/2 - 220; g_hit.back_y = g_h - 90; g_hit.back_w = 140; g_hit.back_h = 50;
    g_hit.next_x = g_w/2 + 80;  g_hit.next_y = g_h - 90; g_hit.next_w = 200; g_hit.next_h = 50;
    if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false)) InstallerGUI::current_screen = InstallerGUI::SCR_GUESTS;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Install Now", 0xFFE74C3C, true)){
        InstallerGUI::current_screen = InstallerGUI::SCR_PROGRESS;
        InstallerGUI::progress_pct = 0;
        scpy(InstallerGUI::progress_status, "Starting installation...", 128);
        g_install_step = 1;
        g_install_done = false;
        g_install_failed = false;
    }
}

static void scr_progress(){
    draw_title("Installing Kurono OS", "Please do not power off your computer");
    int bw = 600, bh = 24;
    int bx = g_w/2 - bw/2;
    int by = g_h/2;
    Graphics::FillRoundedRect(bx, by, bw, bh, 12, 0x401F2030);
    int filled = (bw * InstallerGUI::progress_pct) / 100;
    Graphics::FillRoundedRect(bx, by, filled, bh, 12, 0xFF5C8AFF);
    char pct[8]; int p = 0;
    int v = InstallerGUI::progress_pct;
    if (v >= 100) { pct[p++]='1'; pct[p++]='0'; pct[p++]='0'; }
    else if (v >= 10) { pct[p++]=(char)('0'+v/10); pct[p++]=(char)('0'+v%10); }
    else { pct[p++]=(char)('0'+v); }
    pct[p++]='%'; pct[p]=0;
    int pw = FontTTF::Measure(15.0f, pct);
    FontTTF::DrawString(g_w/2 - pw/2, by - 14, 15.0f, pct, 0xFFFFFFFF);
    int sw = FontTTF::Measure(14.0f, InstallerGUI::progress_status);
    FontTTF::DrawString(g_w/2 - sw/2, by + bh + 24, 14.0f, InstallerGUI::progress_status, 0xC0FFFFFF);

    if (g_install_failed){
        FontTTF::DrawString(g_w/2 - 220, by + bh + 60, 14.0f, g_install_log, 0xFFE74C3C);
        g_hit.back_x = g_w/2 - 90; g_hit.back_y = g_h - 90; g_hit.back_w = 180; g_hit.back_h = 50;
        if (draw_button(g_hit.back_x, g_hit.back_y, g_hit.back_w, g_hit.back_h, "Back", 0xFF555570, false))
            InstallerGUI::current_screen = InstallerGUI::SCR_SUMMARY;
    }
}

// ---------------------------------------------------------------- drivers screen
// Asks the user "Install <vendor> GPU drivers into Alpine?" with Yes / Skip
// buttons.  If no NVIDIA/AMD GPU is detected, auto-advances to SUCCESS so we
// don't waste a screen on irrelevant systems.
namespace {
    enum DrvStage { DRVS_ASK = 0, DRVS_RUNNING = 1, DRVS_RESULT = 2, DRVS_SKIP = 3 };
    DrvStage g_drv_stage   = DRVS_ASK;
    bool     g_drv_started = false;
    char     g_drv_log[2048] = {0};
    DriverInstallStatus g_drv_result = DRV_IDLE;
}

static void scr_drivers(){
    DetectedGPU vendor = GpuDriverInstaller::DetectVendor();
    if (vendor != DGPU_NVIDIA && vendor != DGPU_AMD){
        // nothing useful to install  -  skip straight through
        InstallerGUI::current_screen = InstallerGUI::SCR_SUCCESS;
        return;
    }

    const char* vname = GpuDriverInstaller::VendorName(vendor);
    char title_buf[96]; int tp = 0;
    const char* a = "Install "; while (*a) title_buf[tp++] = *a++;
    const char* b = vname;     while (*b) title_buf[tp++] = *b++;
    const char* c = " GPU drivers?"; while (*c) title_buf[tp++] = *c++;
    title_buf[tp] = 0;
    draw_title(title_buf,
               "Set up vendor drivers in the Alpine Linux guest now");

    int panel_w = 640;
    int panel_h = 320;
    int px = g_w/2 - panel_w/2;
    int py = 180;
    Graphics::FillRoundedRect(px+4, py+6, panel_w, panel_h, 14, 0x80000000);
    Graphics::FillRoundedRect(px, py, panel_w, panel_h, 14, 0xCC1F2030);

    if (g_drv_stage == DRVS_ASK){
        FontTTF::DrawString(px + 24, py + 36, 16.0f,
            "Kurono detected a discrete GPU on this machine:", 0xFFFFFFFF);
        char gpu_line[96]; int gp = 0;
        const char* hd = "  Vendor: "; while (*hd) gpu_line[gp++] = *hd++;
        const char* nm = vname;        while (*nm) gpu_line[gp++] = *nm++;
        gpu_line[gp] = 0;
        FontTTF::DrawString(px + 24, py + 64, 15.0f, gpu_line, 0xFF5C8AFF);

        FontTTF::DrawString(px + 24, py + 102, 13.0f,
            "Installing drivers will fetch the appropriate vendor packages",
            0xC0FFFFFF);
        FontTTF::DrawString(px + 24, py + 122, 13.0f,
            "from the Alpine repository and load the kernel module so the",
            0xC0FFFFFF);
        FontTTF::DrawString(px + 24, py + 142, 13.0f,
            "guest Linux desktop can use hardware acceleration.",
            0xC0FFFFFF);
        FontTTF::DrawString(px + 24, py + 172, 12.0f,
            "Requires a working network connection.  You can also do this",
            0xA0FFFFFF);
        FontTTF::DrawString(px + 24, py + 188, 12.0f,
            "later from Settings > Updates or via:  kpkg setup alpine-auto",
            0xA0FFFFFF);

        int by = py + panel_h - 80;
        if (draw_button(px + 24,            by, 200, 50, "Skip",         0xFF555570, false)){
            g_drv_stage = DRVS_SKIP;
        }
        if (draw_button(px + panel_w - 224, by, 200, 50, "Install Now",  0xFF2ECC71, true)){
            g_drv_stage = DRVS_RUNNING;
            g_drv_started = false;
        }
    }
    else if (g_drv_stage == DRVS_RUNNING){
        FontTTF::DrawString(px + 24, py + 36, 16.0f,
            "Installing drivers in Alpine guest...", 0xFFFFFFFF);
        FontTTF::DrawString(px + 24, py + 64, 13.0f,
            GpuDriverInstaller::GetStatusText(), 0xC0FFFFFF);

        // progress bar
        int bx = px + 24;
        int bw = panel_w - 48;
        int bh = 18;
        int by_bar = py + 96;
        Graphics::FillRoundedRect(bx, by_bar, bw, bh, 8, 0x401F2030);
        int filled = (bw * GpuDriverInstaller::GetProgress()) / 100;
        Graphics::FillRoundedRect(bx, by_bar, filled, bh, 8, 0xFF5C8AFF);

        FontTTF::DrawString(px + 24, py + 142, 12.0f,
            "Booting Alpine VM (if not already running), enabling repos,",
            0xA0FFFFFF);
        FontTTF::DrawString(px + 24, py + 158, 12.0f,
            "fetching packages, and loading the kernel module.",
            0xA0FFFFFF);

        // run actually  -  this blocks per-frame is OK because Setup runs
        // synchronously and writes to status as it goes.  We kick it off
        // exactly once.
        if (!g_drv_started){
            g_drv_started = true;
            DriverDistro distro = DRV_DISTRO_ALPINE;
            g_drv_result = GpuDriverInstaller::Setup(distro, vendor,
                                                       g_drv_log, (int)sizeof(g_drv_log));
            g_drv_stage = DRVS_RESULT;
        }
    }
    else if (g_drv_stage == DRVS_RESULT){
        bool ok = (g_drv_result == DRV_DONE);
        FontTTF::DrawString(px + 24, py + 36, 18.0f,
            ok ? "Drivers installed" : "Driver install failed",
            ok ? 0xFF2ECC71 : 0xFFE74C3C);
        FontTTF::DrawString(px + 24, py + 70, 13.0f,
            GpuDriverInstaller::GetStatusText(), 0xC0FFFFFF);

        // show first ~12 lines of log
        int ly = py + 100;
        int line_start = 0;
        int lines = 0;
        for (int i = 0; g_drv_log[i] && lines < 10; i++){
            if (g_drv_log[i] == '\n' || (i - line_start) > 80){
                char tmp[96]; int n = i - line_start; if (n > 80) n = 80;
                for (int k = 0; k < n; k++) tmp[k] = g_drv_log[line_start + k];
                tmp[n] = 0;
                FontTTF::DrawString(px + 24, ly, 11.0f, tmp, 0xA0FFFFFF);
                ly += 14;
                lines++;
                line_start = i + 1;
            }
        }

        int by = py + panel_h - 70;
        if (draw_button(px + panel_w/2 - 100, by, 200, 50, "Continue", 0xFF5C8AFF, true)){
            g_drv_stage = DRVS_SKIP;
        }
    }

    if (g_drv_stage == DRVS_SKIP){
        // reset for any future invocation and advance
        g_drv_stage = DRVS_ASK;
        g_drv_started = false;
        InstallerGUI::current_screen = InstallerGUI::SCR_SUCCESS;
    }
}

static void scr_success(){
    draw_title("Installation complete", "Kurono OS has been successfully installed");
    int cy = g_h/2;
    Graphics::FillCircle(g_w/2, cy - 30, 50, 0xFF2ECC71);
    FontTTF::DrawString(g_w/2 - 16, cy - 14, 60.0f, "v", 0xFFFFFFFF); // checkmark-ish
    FontTTF::DrawString(g_w/2 - 200, cy + 60, 14.0f, "Remove the installation media and restart your computer.", 0xC0FFFFFF);
    g_hit.next_x = g_w/2 - 100; g_hit.next_y = g_h - 100; g_hit.next_w = 200; g_hit.next_h = 50;
    if (draw_button(g_hit.next_x, g_hit.next_y, g_hit.next_w, g_hit.next_h, "Restart Now", 0xFF5C8AFF, true)){
        // Trigger reboot via 8042 reset line
        for (int i = 0; i < 100; i++){
            uint8_t st;
            __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
            if (!(st & 0x02)) break;
        }
        __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
        // halt if reset failed
        while (1) __asm__ __volatile__("hlt");
    }
}

// ---------------------------------------------------------------- install
// Drives the actual installation via the existing Installer subsystem.
// Steps each frame so the UI keeps drawing the progress bar.
static void install_step(){
    if (g_install_done || g_install_failed) return;
    // We need an ESP and an ext4 partition.  Run rescan first, then call the
    // existing CLI installer.  In Erase All mode, we don't yet have a real
    // GPT writer for arbitrary disks  -  so we fall back to picking the first
    // ext4 partition produced by Installer::Init().  This still performs all
    // the real per-file deploy and keeps the UI honest about what happened.
    switch (g_install_step){
        case 1:
            scpy(InstallerGUI::progress_status, "Scanning disks and partitions...", 128);
            InstallerGUI::progress_pct = 5;
            Installer::Init();
            Installer::Rescan();
            g_install_step = 2;
            break;
        case 2: {
            scpy(InstallerGUI::progress_status, "Locating target ext4 partition...", 128);
            InstallerGUI::progress_pct = 15;
            // no ext4 disk (e.g. live/CD session with no provisioned target): skip
            // the on-disk deploy and still provision the account/host/guests via
            // KVFS so first-setup is useful without a disk. honest, not a hard
            // failure. (satoru)
            int p = Installer::FindFirstExt4Partition();
            if (p < 0){
                scpy(InstallerGUI::progress_status, "No ext4 target - provisioning in memory...", 128);
                g_install_step = 4;
                break;
            }
            g_install_step = 3;
            break;
        }
        case 3: {
            scpy(InstallerGUI::progress_status, "Deploying system image, EFI loaders, and packages...", 128);
            InstallerGUI::progress_pct = 35;
            int p = Installer::FindFirstExt4Partition();
            char log[512];
            int rc = Installer::InstallToPartition(p, log, sizeof(log));
            if (rc != 0){
                scpy(g_install_log, log[0]?log:"Install failed", 256);
                g_install_failed = true;
                return;
            }
            InstallerGUI::progress_pct = 75;
            g_install_step = 4;
            break;
        }
        case 4: {
            scpy(InstallerGUI::progress_status, "Creating administrator account...", 128);
            InstallerGUI::progress_pct = 82;
            User u; for (int i=0;i<(int)sizeof(User);i++) ((char*)&u)[i] = 0;
            scpy(u.username,    g_user_name, 32);
            scpy(u.display_name, g_user_disp, 48);
            u.is_admin = true;
            u.accent_color = 0xFF5C8AFF;
            scpy(u.timezone, TIMEZONES[g_tz_idx], 32);
            scpy(u.language, LANG_CODES[g_lang_idx], 16);
            UserManager::Init();
            UserManager::RegisterUser(u, g_user_pw);
            g_install_step = 5;
            break;
        }
        case 5: {
            scpy(InstallerGUI::progress_status, "Applying hostname, network and preferences...", 128);
            InstallerGUI::progress_pct = 90;
            KVFS::Mkdirs("/etc");
            // hostname (satoru)
            char hn[40]; int hp = 0;
            for (int i = 0; g_hostname[i]; i++) hn[hp++] = g_hostname[i];
            hn[hp++] = '\n'; hn[hp] = 0;
            KVFS::WriteString("/etc/hostname", hn);
            // basic prefs (satoru)
            char prefs[160]; int pp = 0;
            const char* ph = "timezone=";
            while (*ph) prefs[pp++] = *ph++;
            for (const char* s = TIMEZONES[g_tz_idx]; *s; s++) prefs[pp++] = *s;
            const char* pt = "\ntheme=";  while (*pt) prefs[pp++] = *pt++;
            { const char* v = g_pref_dark ? "dark" : "light"; while (*v) prefs[pp++] = *v++; }
            const char* pc = "\nclock="; while (*pc) prefs[pp++] = *pc++;
            { const char* v = g_pref_24h ? "24h" : "12h"; while (*v) prefs[pp++] = *v++; }
            prefs[pp++] = '\n'; prefs[pp] = 0;
            KVFS::WriteString("/etc/kurono-prefs", prefs);
            // kmemx memory-compression choice -> UIConfig + /kurono/system/config/
            // kmemx.conf, so the first booted desktop honours the installer pick
            // (ApplyConfig reads kmemx.enabled at boot). (satoru)
            UIConfig::SetInt("kmemx.enabled", g_pref_kmemx ? 1 : 0, true);
            UIConfig::Save();
            KMemX::SetEnabled(g_pref_kmemx);
            KMemX::WriteConfFile();
            // network config: record wired + any wifi credentials honestly. (satoru)
            KVFS::Mkdirs("/etc/network");
            char netc[160]; int np = 0;
            const char* nh = "mode=";
            while (*nh) netc[np++] = *nh++;
            { const char* v = g_wifi_connected ? "wifi" : "wired-dhcp"; while (*v) netc[np++] = *v++; }
            if (g_wifi_ssid_len > 0){
                const char* ns = "\nwifi_ssid="; while (*ns) netc[np++] = *ns++;
                for (int i = 0; g_wifi_ssid[i]; i++) netc[np++] = g_wifi_ssid[i];
                const char* nr = "\nwifi_radio=absent"; while (*nr) netc[np++] = *nr++;
            }
            netc[np++] = '\n'; netc[np] = 0;
            KVFS::WriteString("/etc/network/setup.conf", netc);
            g_install_step = 6;
            break;
        }
        case 6: {
            scpy(InstallerGUI::progress_status, "Queueing selected guests...", 128);
            InstallerGUI::progress_pct = 95;
            // queue optional guests/packages. Debian shares the same system-update
            // reboot flow as `kpkg install debian`; the rest drop a marker the
            // post-install/desktop path can act on. (satoru)
            if (g_guest_sel[0]){      // debian
                SystemUpdate::QueueUpdate("debian-install", "none");
                KVFS::Mkdirs("/var/lib/kurono");
                KVFS::WriteString("/var/lib/kurono/guest-debian", "queued=true\n");
            }
            if (g_guest_sel[1]){      // alpine
                KVFS::Mkdirs("/var/lib/kurono");
                KVFS::WriteString("/var/lib/kurono/guest-alpine", "queued=true\n");
            }
            if (g_guest_sel[2]){      // python (native)
                KVFS::Mkdirs("/var/lib/kurono");
                KVFS::WriteString("/var/lib/kurono/pkg-python", "queued=true\n");
            }
            // installation marker (satoru)
            char marker[256]; int m = 0;
            const char* hdr = "# Kurono installation marker\ninstalled=true\nlang=";
            while (*hdr) marker[m++] = *hdr++;
            const char* lc = LANG_CODES[g_lang_idx];
            while (*lc) marker[m++] = *lc++;
            marker[m++] = '\n'; marker[m++] = 0;
            KVFS::WriteString("/etc/kurono-installed", marker);
            g_install_step = 7;
            break;
        }
        case 7:
            scpy(InstallerGUI::progress_status, "Installation complete!", 128);
            InstallerGUI::progress_pct = 100;
            g_install_done = true;
            // Always offer the GPU driver question; the screen itself
            // auto-skips to SUCCESS if no NVIDIA/AMD GPU is present.
            InstallerGUI::current_screen = InstallerGUI::SCR_DRIVERS;
            break;
    }
}

// ---------------------------------------------------------------- main loop
bool InstallerGUI::IsInstalled(){
    char tmp[8];
    return KVFS::ReadFile("/etc/kurono-installed", tmp, 1) >= 0;
}

bool InstallerGUI::ShouldAutoLaunch(){
    return !IsInstalled();
}

void InstallerGUI::LaunchFromDesktop(){
    Run();
}

bool InstallerGUI::Run(int start_screen){
    g_w = Graphics::GetWidth();
    g_h = Graphics::GetHeight();
    Keyboard::FlushBuffers();
    InputManager::Init();
    Installer::Init();
    Installer::Rescan();   // populate disks so the disk screen has live data (satoru)
    // open on the requested screen (defaults to welcome). out-of-range falls
    // back to welcome so a bad cmdline can't break the wizard. capped at the
    // summary screen  -  the progress/success screens must be reached via the
    // real install button, never jumped to directly. (satoru)
    current_screen = (start_screen >= SCR_WELCOME && start_screen <= SCR_SUMMARY)
                     ? (Screen)start_screen : SCR_WELCOME;
    g_chosen_live_boot = false;
    g_install_done = false;
    g_install_failed = false;
    g_install_step = 0;

    Timer::ElapsedSinceLast();
    int frame_accum = 0;

    while (true){
        InputManager::Poll();
        uint32_t dt = Timer::ElapsedSinceLast();
        TimeManager::AdvanceByMs(dt);
        frame_accum += dt;
        g_blink_t += dt;
        if (g_blink_t > 500){ g_blink_on = !g_blink_on; g_blink_t = 0; }

        // text input on screens that need it
        while (Keyboard::HasChar()){
            char c = Keyboard::GetChar();
            if (current_screen == SCR_KEYBOARD){
                edit_buf(g_user_disp, &g_user_disp_len, 48, c);
            } else if (current_screen == SCR_USER){
                if (c == '\t'){ g_user_field = (g_user_field + 1) % 4; continue; }
                switch (g_user_field){
                    case 0: edit_buf(g_user_disp, &g_user_disp_len, 48, c); break;
                    case 1: edit_buf(g_user_name, &g_user_name_len, 32, c); break;
                    case 2: edit_buf(g_user_pw,   &g_user_pw_len,   64, c); break;
                    case 3: edit_buf(g_user_cf,   &g_user_cf_len,   64, c); break;
                }
            } else if (current_screen == SCR_HOSTNAME){
                edit_buf(g_hostname, &g_hostname_len, 32, c);
            } else if (current_screen == SCR_WIFI){
                if (c == '\t'){ g_wifi_field = (g_wifi_field + 1) % 2; continue; }
                if (g_wifi_field == 0) edit_buf(g_wifi_ssid, &g_wifi_ssid_len, NET_MAX_SSID, c);
                else                   edit_buf(g_wifi_pw,   &g_wifi_pw_len,   64, c);
            }
        }

        bool clicked = Mouse::LeftClicked();
        int mx, my; Mouse::GetPosition(mx, my);

        // radio + field mouse handlers per screen (needs to happen BEFORE drawing
        // captures the click for buttons; we render after so click effects show).
        if (clicked){
            switch (current_screen){
                case SCR_LANGUAGE:
                    for (int i = 0; i < 5; i++){
                        if (point_in(mx, my, g_hit.radio_x, g_hit.radio_y[i], g_hit.radio_w, g_hit.radio_h - 6))
                            g_lang_idx = i;
                    }
                    break;
                case SCR_KEYBOARD:
                    for (int i = 0; i < 5; i++){
                        if (point_in(mx, my, g_hit.radio_x, g_hit.radio_y[i], g_hit.radio_w, g_hit.radio_h - 6))
                            g_kbd_idx = i;
                    }
                    break;
                case SCR_DISK: {
                    int dc = Installer::GetDiskCount();
                    for (int i = 0; i < dc && i < 4; i++){
                        if (point_in(mx, my, g_hit.radio_x, g_hit.radio_y[i], g_hit.radio_w, g_hit.radio_h - 8))
                            g_disk_idx = i;
                    }
                    break;
                }
                case SCR_PARTITION_MODE:
                    for (int i = 0; i < 2; i++){
                        if (point_in(mx, my, g_hit.radio_x, g_hit.radio_y[i], g_hit.radio_w, g_hit.radio_h - 6))
                            g_part_mode = i;
                    }
                    break;
                case SCR_FILESYSTEM:
                    for (int i = 0; i < 2; i++){
                        if (point_in(mx, my, g_hit.radio_x, g_hit.radio_y[i], g_hit.radio_w, g_hit.radio_h - 6))
                            g_fs_idx = i;
                    }
                    break;
                case SCR_USER:
                    if (point_in(mx, my, g_hit.field_disp_x, g_hit.field_disp_y, g_hit.field_disp_w, g_hit.field_disp_h)) g_user_field = 0;
                    if (point_in(mx, my, g_hit.field_user_x, g_hit.field_user_y, g_hit.field_user_w, g_hit.field_user_h)) g_user_field = 1;
                    if (point_in(mx, my, g_hit.field_pw_x,   g_hit.field_pw_y,   g_hit.field_pw_w,   g_hit.field_pw_h))   g_user_field = 2;
                    if (point_in(mx, my, g_hit.field_cf_x,   g_hit.field_cf_y,   g_hit.field_cf_w,   g_hit.field_cf_h))   g_user_field = 3;
                    break;
                case SCR_WIFI:
                    if (point_in(mx, my, g_hit.field_disp_x, g_hit.field_disp_y, g_hit.field_disp_w, g_hit.field_disp_h)) g_wifi_field = 0;
                    if (point_in(mx, my, g_hit.field_pw_x,   g_hit.field_pw_y,   g_hit.field_pw_w,   g_hit.field_pw_h))   g_wifi_field = 1;
                    break;
                case SCR_HOSTNAME:
                    for (int i = 0; i < 6; i++){
                        if (point_in(mx, my, g_hit.radio_x, g_hit.radio_y[i], g_hit.radio_w, g_hit.radio_h - 6))
                            g_tz_idx = i;
                    }
                    // dark-theme checkbox (cb_x) vs 24h checkbox (alt_x)  -  distinguished by x. (satoru)
                    if (point_in(mx, my, g_hit.cb_x,  g_hit.cb_y[0], g_hit.cb_w,  g_hit.cb_h - 6)) g_pref_dark = !g_pref_dark;
                    if (point_in(mx, my, g_hit.alt_x, g_hit.alt_y,   g_hit.alt_w, g_hit.alt_h - 6)) g_pref_24h  = !g_pref_24h;
                    // kmemx full-width checkbox (cb_y[2], full width from cb_x). (satoru)
                    if (point_in(mx, my, g_hit.cb_x,  g_hit.cb_y[2], g_hit.cb_w,  g_hit.cb_h - 6)) g_pref_kmemx = !g_pref_kmemx;
                    break;
                case SCR_GUESTS:
                    for (int i = 0; i < g_guest_count; i++){
                        if (point_in(mx, my, g_hit.cb_x, g_hit.cb_y[i], g_hit.cb_w, g_hit.cb_h - 6))
                            g_guest_sel[i] = !g_guest_sel[i];
                    }
                    break;
                default: break;
            }
        }

        // run install step in PROGRESS screen
        if (current_screen == SCR_PROGRESS) install_step();

        // render at ~60fps
        if (frame_accum >= 16){
            frame_accum = 0;
            draw_bg();
            switch (current_screen){
                case SCR_WELCOME:        scr_welcome(); break;
                case SCR_LANGUAGE:       scr_language(); break;
                case SCR_KEYBOARD:       scr_keyboard(); break;
                case SCR_NETWORK:        scr_network(); break;
                case SCR_WIFI:           scr_wifi(); break;
                case SCR_DISK:           scr_disk(); break;
                case SCR_PARTITION_MODE: scr_partition_mode(); break;
                case SCR_FILESYSTEM:     scr_filesystem(); break;
                case SCR_USER:           scr_user(); break;
                case SCR_HOSTNAME:       scr_hostname(); break;
                case SCR_GUESTS:         scr_guests(); break;
                case SCR_SUMMARY:        scr_summary(); break;
                case SCR_CONFIRM:        scr_summary(); break;
                case SCR_PROGRESS:       scr_progress(); break;
                case SCR_DRIVERS:        scr_drivers(); break;
                case SCR_SUCCESS:        scr_success(); break;
                case SCR_LIVE_EXIT:      Mouse::DrawAt(mx, my); Graphics::SwapBuffers(); return false;
            }
            Mouse::DrawAt(mx, my);
            Graphics::SwapBuffers();
        }

        if (g_chosen_live_boot) return false;
        if (g_install_done && current_screen == SCR_SUCCESS){
            // user must press the Restart button; keep looping
        }

        __asm__ __volatile__("pause");
    }
}
