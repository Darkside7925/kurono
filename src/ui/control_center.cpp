//  kurono os - control center implementation
//  a two column glass panel matching the modern shell: quick tiles, wi-fi
//  network list, per-display brightness, sound output picker, now playing,
//  battery/power mode and the session buttons. every control is wired to the
//  real subsystem and reads its live state each frame. (satoru)
#include "../system/backlight.h"   // panel backlight (satoru)
#include "../system/audio_devices.h"   // per-device volume (satoru)
#include "control_center.h"
#include "night_light.h"   // warm tint state, strength, schedule (satoru)
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_backend.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/outputs.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../drivers/display_mgr.h"
#include "../drivers/intel_gpu.h"
#include "../hal/cpufreq.h"
#include "../hal/hal.h"
#include "../net/network.h"
#include "../system/user_mgmt.h"
#include "../system/ui_config.h"
#include "../system/screenshot.h"
#include "../security/admin.h"
#include "../system/power.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../apps/media_player.h"
#include "../apps/system_settings.h"
#include "../linux/linux_drivers.h"
#include "../ui/desktop.h"
#include "../ui/font.h"
#include "../ui/kss.h"
#include "../ui/modern_draw.h"
#include "../ui/notification.h"
#include "../ui/perf_hud.h"
#include "../ui/wm_motion.h"
#include "../ui/lockscreen.h"
#include "../ui/style.h"

// state -------------------------------------------------------------------
bool ControlCenter::open = false;
int  ControlCenter::screen_w = 0;
int  ControlCenter::screen_h = 0;
int  ControlCenter::panel_x = 0;
int  ControlCenter::panel_y = 0;
int  ControlCenter::panel_w = 720;
int  ControlCenter::panel_h = 600;

namespace {

// file helpers use the same geometry the class exposes. (satoru)
static int& screen_w = ControlCenter::screen_w;
static int& screen_h = ControlCenter::screen_h;
static int& panel_x = ControlCenter::panel_x;
static int& panel_y = ControlCenter::panel_y;
static int& panel_w = ControlCenter::panel_w;
static int& panel_h = ControlCenter::panel_h;

constexpr int CC_W = 720;
constexpr int CC_H = 600;
constexpr int HEADER_H = 60;
constexpr int PAD = 20;
constexpr int GAP = 10;
constexpr int ACTION_H = 44;
constexpr int BTN_GAP = 8;

struct Rect {
    int x, y, w, h;
    bool hit(int mx, int my) const { return w > 0 && h > 0 && mx >= x && mx < x + w && my >= y && my < y + h; }
};

struct Theme {
    uint32_t panel, panel2, card, card_hi, field, text, dim, line;
    uint32_t accent, on_accent, danger, success, warn, track, white;
    bool dark;
};

struct Layout {
    Rect panel;
    Rect wifi, bt, net;
    Rect tiles[8];
    int  tile_id[8];
    int  tile_count;
    Rect airplane;
    Rect bright_card, vol_card, media_card, power_card;
    Rect bright_slider[OUTPUT_MAX];
    int  bright_slot[OUTPUT_MAX];
    int  bright_count;
    Rect vol_slider, mute_btn, out_btn;
    Rect media_prev, media_play, media_next, media_bar;
    Rect power_mode[3];
    Rect actions[5];
    Rect popup;
    Rect popup_toggle, popup_scan, popup_pass, popup_connect, popup_cancel;
    Rect popup_rows[6];
    int  popup_row_count;
    Rect popup_restart, popup_stop;
    Rect confirm_no, confirm_yes;
};

enum FocusId {
    F_NONE = 0,
    F_WIFI = 10, F_BT, F_NET,
    F_FOCUS, F_DARK, F_NIGHT, F_SHOT, F_REC, F_PERF, F_AIR,
    F_BRIGHT = 40,
    F_VOLUME = 50, F_MUTE, F_OUTPUT,
    F_MEDIA_PREV = 60, F_MEDIA_PLAY, F_MEDIA_NEXT,
    F_MODE = 70,
    F_SETTINGS = 80, F_LOCK, F_SLEEP, F_RESTART, F_SHUTDOWN,
    F_POP_TOGGLE = 100, F_POP_SCAN, F_POP_ROW = 110, F_POP_PASS = 120,
    F_POP_CONNECT, F_POP_CANCEL,
    F_AUDIO_ROW = 140, F_AUDIO_RESTART = 150, F_AUDIO_STOP,
    F_CONFIRM_NO = 160, F_CONFIRM_YES,
};

enum PopupKind { POP_NONE = 0, POP_WIFI, POP_AUDIO, POP_CONFIRM };
enum PowerAction { PA_NONE = 0, PA_SLEEP, PA_RESTART, PA_SHUTDOWN };
enum IconKind {
    IC_WIFI, IC_ETHERNET, IC_BLUETOOTH, IC_FOCUS, IC_MOON, IC_SUN, IC_NIGHT,
    IC_CAMERA, IC_RECORD, IC_STOP, IC_AIRPLANE, IC_PULSE, IC_GEAR, IC_LOCK,
    IC_POWER, IC_RESTART, IC_SPEAKER, IC_MUTE, IC_CHEVRON, IC_CHECK,
    IC_PLAY, IC_PAUSE, IC_NEXT, IC_PREV, IC_MONITOR, IC_BATTERY, IC_USER,
    IC_CLOSE, IC_WARN,
};

struct FocusItem { int id; Rect r; };

FocusItem g_focus[48];
int g_focus_count = 0;
Layout g_L;
bool g_layout_valid = false;

int  g_out_idx = 0;
int  g_anchor_x = 0, g_anchor_y = 0;
bool g_top_anchor = true;
bool g_spring_init = false;
WMMotion::Spring1 g_spring;
uint32_t g_last_ms = 0;

int  g_focus_cur = F_NONE;
bool g_focus_visible = false;
int  g_key_activate = F_NONE;
int  g_key_adjust = F_NONE;
int  g_key_adjust_dir = 0;
bool g_key_held[KEY_WAKE + 1] = {};

int  g_popup = POP_NONE;
int  g_confirm_action = PA_NONE;
int  g_drag_slider = F_NONE;

bool g_bt_present = false;
bool g_bt_on = false;
char g_bt_name[48] = {0};

char g_wifi_ssid[NET_MAX_SSID] = {0};
char g_wifi_pass[64] = {0};
int  g_wifi_pass_len = 0;
bool g_wifi_pass_focus = false;
bool g_wifi_secure_wait = false;

bool g_recording = false;
bool g_night_light = false;
bool g_focus_mode = false;
bool g_airplane = false;
uint32_t g_rec_start_ms = 0;
uint32_t g_rec_last_capture_ms = 0;
int g_rec_frames = 0;
char g_rec_dir[64] = {0};

inline uint32_t NowMs() { return Timer::GetRealMs(); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int absc(int v) { return v < 0 ? -v : v; }
static int slen(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void scpy(char* d, const char* s, int cap) {
    if (!d || cap <= 0) return;
    int i = 0;
    if (s) while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void sapp(char* d, const char* s, int cap) {
    if (!d || cap <= 0) return;
    int n = slen(d);
    int i = 0;
    if (s) while (s[i] && n < cap - 1) d[n++] = s[i++];
    d[n] = 0;
}
static void intstr(int v, char* b, int cap) {
    if (!b || cap < 2) { if (b && cap > 0) b[0] = 0; return; }
    if (v < 0) { b[0] = '-'; intstr(-v, b + 1, cap - 1); return; }
    char t[16]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 15);
    int i = 0;
    while (n > 0 && i < cap - 1) b[i++] = t[--n];
    b[i] = 0;
}
static bool starts_with(const char* s, const char* p) {
    if (!s || !p) return false;
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}
static int luma(uint32_t c) {
    return (int)((((c >> 16) & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + (c & 0xFF) * 114) / 1000);
}
static uint32_t mixc(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a | 0xFF000000u;
    if (t >= 255) return b | 0xFF000000u;
    return MDraw::Mix(a, b, t);
}

static Theme ThemeNow() {
    Theme t;
    const char* ap = UIConfig::Str("ui.appearance", "dark");
    bool light = ap && (ap[0] == 'l' || ap[0] == 'L');
    t.dark = !light;
    t.accent = UIConfig::Color("theme.accent", 0xFF0A84FF) | 0xFF000000u;
    t.on_accent = luma(t.accent) > 186 ? 0xFF1C1C1E : 0xFFFFFFFFu;
    t.white = 0xFFFFFFFFu;
    t.danger = light ? 0xFFE5342B : 0xFFFF6B61;
    t.success = light ? 0xFF2DA44E : 0xFF32D74B;
    t.warn = light ? 0xFFD97706 : 0xFFFFB340;
    if (light) {
        t.panel = 0xFFF6F6F8;
        t.panel2 = 0xFFECECF0;
        t.card = 0xFFFFFFFF;
        t.card_hi = 0xFFF2F2F5;
        t.field = 0xFFF0F0F3;
        t.text = 0xFF1C1C1E;
        t.dim = 0xFF6E6E76;
        t.line = 0xFF000000;
        t.track = 0xFFD9D9DF;
    } else {
        t.panel = 0xFF2A2B33;
        t.panel2 = 0xFF22232A;
        t.card = 0xFF34353F;
        t.card_hi = 0xFF3B3C47;
        t.field = 0xFF262730;
        t.text = 0xFFF2F3F8;
        t.dim = 0xFF9A9CA8;
        t.line = 0xFFFFFFFF;
        t.track = 0xFF3D3E49;
    }
    return t;
}

static uint32_t fade(uint32_t c, const Theme& t, int a) {
    if (a >= 255) return c;
    if (a <= 0) return t.panel;
    return mixc(t.panel, c, a);
}
static int alpha_fade(int a, int mul) { return (a * mul) / 255; }

static void AddFocus(int id, const Rect& r) {
    if (id == F_NONE || r.w <= 0 || r.h <= 0) return;
    if (g_focus_count >= (int)(sizeof(g_focus) / sizeof(g_focus[0]))) return;
    g_focus[g_focus_count].id = id;
    g_focus[g_focus_count].r = r;
    g_focus_count++;
}
static const FocusItem* FindFocus(int id) {
    for (int i = 0; i < g_focus_count; i++) if (g_focus[i].id == id) return &g_focus[i];
    return nullptr;
}

// small ui helpers ---------------------------------------------------------
static const char* fit_text(char* buf, int cap, float px, const char* s, int maxw, bool bold = false) {
    if (!buf || cap <= 0) return "";
    if (!s) { buf[0] = 0; return buf; }
    scpy(buf, s, cap);
    if (FontTTF::MeasureText(px, buf, bold) <= maxw) return buf;
    int n = slen(buf);
    while (n > 0) {
        n--;
        int p = n;
        if (p > cap - 4) p = cap - 4;
        buf[p] = '.'; buf[p + 1] = '.'; buf[p + 2] = '.'; buf[p + 3] = 0;
        if (FontTTF::MeasureText(px, buf, bold) <= maxw) break;
    }
    return buf;
}
static void txt(int x, int y, float px, const char* s, uint32_t c, bool bold = false) {
    if (s && s[0]) FontTTF::DrawText(x, y, px, s, c, bold);
}
static void txt_center(int cx, int y, float px, const char* s, uint32_t c, bool bold = false) {
    if (!s || !s[0]) return;
    int w = FontTTF::MeasureText(px, s, bold);
    FontTTF::DrawText(cx - w / 2, y, px, s, c, bold);
}
static void txt_right(int rx, int y, float px, const char* s, uint32_t c, bool bold = false) {
    if (!s || !s[0]) return;
    int w = FontTTF::MeasureText(px, s, bold);
    FontTTF::DrawText(rx - w, y, px, s, c, bold);
}

// icon painting. tiny vector glyphs on top of the drawing primitives so the
// tiles stay crisp in both themes and every size. (satoru)
static inline void ipx(int x, int y, uint32_t c, int a) {
    if (a <= 0) return;
    Graphics::BlendPixel(x, y, (uint8_t)((c >> 16) & 0xFF), (uint8_t)((c >> 8) & 0xFF),
                         (uint8_t)(c & 0xFF), (uint8_t)(a > 255 ? 255 : a));
}
static void idisc(int cx, int cy, int r, uint32_t c, int a) {
    if (r <= 0) { ipx(cx, cy, c, a); return; }
    for (int dy = -r; dy <= r; dy++) {
        int q = r * r - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= q) dx++;
        for (int x = -dx; x <= dx; x++) ipx(cx + x, cy + dy, c, a);
    }
}
static void iline(int x0, int y0, int x1, int y1, uint32_t c, int a, int th = 2) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = absc(dx) > absc(dy) ? absc(dx) : absc(dy);
    if (steps <= 0) { idisc(x0, y0, th / 2, c, a); return; }
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (dx * i) / steps;
        int y = y0 + (dy * i) / steps;
        if (th <= 1) ipx(x, y, c, a);
        else idisc(x, y, th / 2, c, a);
    }
}
static void iarc(int cx, int cy, int r, float a0, float a1, uint32_t c, int a, int th = 2) {
    if (r <= 1) return;
    int steps = r * 3;
    if (steps < 8) steps = 8;
    for (int i = 0; i <= steps; i++) {
        float t = a0 + (a1 - a0) * ((float)i / (float)steps);
        int x = cx + (int)(WMMotion::Cos(t) * (float)r + (t >= 0 ? 0.5f : -0.5f));
        int y = cy + (int)(WMMotion::Sin(t) * (float)r + (t >= 0 ? 0.5f : -0.5f));
        if (th <= 1) ipx(x, y, c, a);
        else idisc(x, y, th / 2, c, a);
    }
}
static void ioutline(int x, int y, int w, int h, int r, uint32_t c, int a, int th = 2) {
    if (w <= 0 || h <= 0) return;
    iline(x + r, y, x + w - r, y, c, a, th);
    iline(x + r, y + h - 1, x + w - r, y + h - 1, c, a, th);
    iline(x, y + r, x, y + h - r, c, a, th);
    iline(x + w - 1, y + r, x + w - 1, y + h - r, c, a, th);
    if (r > 0) {
        iarc(x + r, y + r, r, 3.14159265f, 4.71238898f, c, a, th);
        iarc(x + w - r - 1, y + r, r, 4.71238898f, 6.2831853f, c, a, th);
        iarc(x + r, y + h - r - 1, r, 1.57079633f, 3.14159265f, c, a, th);
        iarc(x + w - r - 1, y + h - r - 1, r, 0.0f, 1.57079633f, c, a, th);
    }
}

static void DrawIcon(int kind, int x, int y, int s, uint32_t c, uint32_t bg, int a) {
    if (s < 8) return;
    int cx = x + s / 2, cy = y + s / 2;
    int u = s / 16;
    if (u < 1) u = 1;
    int th = (s >= 20) ? 2 : 1;
    switch (kind) {
    case IC_WIFI: {
        int base = y + (s * 12) / 16;
        iarc(cx, base, (s * 7) / 16, 3.9f, 5.53f, c, a, th);
        iarc(cx, base, (s * 5) / 16, 3.9f, 5.53f, c, a, th);
        iarc(cx, base, (s * 3) / 16, 3.9f, 5.53f, c, a, th);
        idisc(cx, base - u / 2, u, c, a);
        break;
    }
    case IC_ETHERNET: {
        int w = (s * 10) / 16, h = (s * 7) / 16;
        int bx = cx - w / 2, by = cy - h / 2 - u;
        ioutline(bx, by, w, h, 2, c, a, th);
        iline(bx + w / 4, by + h, bx + w / 4, by + h + u * 2, c, a, th);
        iline(bx + (3 * w) / 4, by + h, bx + (3 * w) / 4, by + h + u * 2, c, a, th);
        iline(bx + w / 5, by + 2, bx + w / 5, by + h - 2, c, a, 1);
        iline(bx + (2 * w) / 5, by + 2, bx + (2 * w) / 5, by + h - 2, c, a, 1);
        iline(bx + (3 * w) / 5, by + 2, bx + (3 * w) / 5, by + h - 2, c, a, 1);
        iline(bx + (4 * w) / 5, by + 2, bx + (4 * w) / 5, by + h - 2, c, a, 1);
        break;
    }
    case IC_BLUETOOTH: {
        int top = y + (s * 3) / 16, bot = y + (s * 13) / 16;
        iline(cx, top, cx, bot, c, a, th);
        iline(cx, top, cx + (s * 3) / 16, y + (s * 6) / 16, c, a, th);
        iline(cx + (s * 3) / 16, y + (s * 6) / 16, cx - (s * 3) / 16, y + (s * 10) / 16, c, a, th);
        iline(cx, bot, cx + (s * 3) / 16, y + (s * 10) / 16, c, a, th);
        iline(cx + (s * 3) / 16, y + (s * 10) / 16, cx - (s * 3) / 16, y + (s * 6) / 16, c, a, th);
        break;
    }
    case IC_FOCUS:
        iarc(cx, cy, (s * 6) / 16, 0.0f, 6.2831853f, c, a, th);
        iarc(cx, cy, (s * 3) / 16, 0.0f, 6.2831853f, c, a, th);
        idisc(cx, cy, u, c, a);
        break;
    case IC_MOON:
        idisc(cx, cy, (s * 6) / 16, c, a);
        idisc(cx + (s * 3) / 16, cy - (s * 2) / 16, (s * 5) / 16, bg, a);
        break;
    case IC_SUN:
        idisc(cx, cy, (s * 3) / 16, c, a);
        for (int i = 0; i < 8; i++) {
            static const int dxs[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
            static const int dys[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
            int r0 = (s * 5) / 16, r1 = (s * 7) / 16;
            iline(cx + dxs[i] * r0, cy + dys[i] * r0, cx + dxs[i] * r1, cy + dys[i] * r1, c, a, th);
        }
        break;
    case IC_NIGHT:
        DrawIcon(IC_MOON, x, y, s, c, bg, a);
        idisc(x + (s * 13) / 16, y + (s * 4) / 16, u, c, a);
        break;
    case IC_CAMERA: {
        int w = (s * 12) / 16, h = (s * 9) / 16;
        int bx = cx - w / 2, by = cy - h / 2 + u;
        ioutline(bx, by, w, h, 2, c, a, th);
        iarc(cx, cy + u, (s * 3) / 16, 0.0f, 6.2831853f, c, a, th);
        iline(cx - (s * 2) / 16, by - u, cx + (s * 2) / 16, by - u, c, a, th);
        break;
    }
    case IC_RECORD:
    case IC_STOP:
        idisc(cx, cy, (s * 5) / 16, c, a);
        if (kind == IC_STOP) {
            Graphics::FillRect(cx - (s * 3) / 16, cy - (s * 3) / 16, (s * 6) / 16, (s * 6) / 16, c);
        }
        break;
    case IC_AIRPLANE: {
        iline(cx - (s * 6) / 16, cy + (s * 2) / 16, cx + (s * 6) / 16, cy - (s * 2) / 16, c, a, th);
        iline(cx - (s * 5) / 16, cy - (s * 2) / 16, cx + (s * 5) / 16, cy + (s * 2) / 16, c, a, th);
        iline(cx, cy - (s * 6) / 16, cx, cy + (s * 6) / 16, c, a, th);
        iline(cx - (s * 3) / 16, cy - (s * 4) / 16, cx + (s * 3) / 16, cy - (s * 4) / 16, c, a, th);
        break;
    }
    case IC_PULSE:
        iline(x + (s * 2) / 16, cy, x + (s * 5) / 16, cy, c, a, th);
        iline(x + (s * 5) / 16, cy, x + (s * 7) / 16, y + (s * 4) / 16, c, a, th);
        iline(x + (s * 7) / 16, y + (s * 4) / 16, x + (s * 9) / 16, y + (s * 12) / 16, c, a, th);
        iline(x + (s * 9) / 16, y + (s * 12) / 16, x + (s * 11) / 16, cy, c, a, th);
        iline(x + (s * 11) / 16, cy, x + (s * 14) / 16, cy, c, a, th);
        break;
    case IC_GEAR:
        iarc(cx, cy, (s * 5) / 16, 0.0f, 6.2831853f, c, a, th);
        idisc(cx, cy, (s * 2) / 16, c, a);
        for (int i = 0; i < 8; i++) {
            static const int dxs[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
            static const int dys[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
            int r0 = (s * 6) / 16, r1 = (s * 8) / 16;
            iline(cx + dxs[i] * r0, cy + dys[i] * r0, cx + dxs[i] * r1, cy + dys[i] * r1, c, a, th);
        }
        break;
    case IC_LOCK:
        iarc(cx, y + (s * 6) / 16, (s * 4) / 16, 3.14159265f, 6.2831853f, c, a, th);
        Graphics::FillRect(x + (s * 4) / 16, y + (s * 6) / 16, (s * 8) / 16, (s * 6) / 16, c);
        break;
    case IC_POWER:
        iarc(cx, cy, (s * 5) / 16, 2.1f, 7.1f, c, a, th);
        iline(cx, y + (s * 2) / 16, cx, cy - (s * 1) / 16, c, a, th);
        break;
    case IC_RESTART:
        iarc(cx, cy, (s * 5) / 16, 0.8f, 5.5f, c, a, th);
        iline(cx + (s * 4) / 16, cy - (s * 4) / 16, cx + (s * 6) / 16, cy - (s * 6) / 16, c, a, th);
        iline(cx + (s * 6) / 16, cy - (s * 6) / 16, cx + (s * 7) / 16, cy - (s * 3) / 16, c, a, th);
        break;
    case IC_SPEAKER:
        Graphics::FillRect(x + (s * 3) / 16, cy - (s * 2) / 16, (s * 3) / 16, (s * 4) / 16, c);
        iline(x + (s * 6) / 16, cy - (s * 2) / 16, x + (s * 9) / 16, cy - (s * 5) / 16, c, a, th);
        iline(x + (s * 6) / 16, cy + (s * 2) / 16, x + (s * 9) / 16, cy + (s * 5) / 16, c, a, th);
        iline(x + (s * 9) / 16, cy - (s * 5) / 16, x + (s * 9) / 16, cy + (s * 5) / 16, c, a, th);
        iarc(cx + (s * 2) / 16, cy, (s * 4) / 16, 5.7f, 6.8f, c, a, th);
        iarc(cx + (s * 2) / 16, cy, (s * 6) / 16, 5.7f, 6.8f, c, a, th);
        break;
    case IC_MUTE:
        DrawIcon(IC_SPEAKER, x, y, s, c, bg, a);
        iline(x + (s * 10) / 16, cy - (s * 3) / 16, x + (s * 14) / 16, cy + (s * 3) / 16, c, a, th);
        iline(x + (s * 14) / 16, cy - (s * 3) / 16, x + (s * 10) / 16, cy + (s * 3) / 16, c, a, th);
        break;
    case IC_CHEVRON:
        iline(cx - (s * 2) / 16, cy - (s * 3) / 16, cx + (s * 2) / 16, cy, c, a, th);
        iline(cx + (s * 2) / 16, cy, cx - (s * 2) / 16, cy + (s * 3) / 16, c, a, th);
        break;
    case IC_CHECK:
        iline(cx - (s * 4) / 16, cy, cx - (s * 1) / 16, cy + (s * 3) / 16, c, a, th);
        iline(cx - (s * 1) / 16, cy + (s * 3) / 16, cx + (s * 5) / 16, cy - (s * 4) / 16, c, a, th);
        break;
    case IC_PLAY:
        iline(x + (s * 5) / 16, y + (s * 3) / 16, x + (s * 12) / 16, cy, c, a, th);
        iline(x + (s * 12) / 16, cy, x + (s * 5) / 16, y + (s * 13) / 16, c, a, th);
        iline(x + (s * 5) / 16, y + (s * 3) / 16, x + (s * 5) / 16, y + (s * 13) / 16, c, a, th);
        break;
    case IC_PAUSE:
        Graphics::FillRect(x + (s * 5) / 16, y + (s * 3) / 16, (s * 2) / 16, (s * 10) / 16, c);
        Graphics::FillRect(x + (s * 9) / 16, y + (s * 3) / 16, (s * 2) / 16, (s * 10) / 16, c);
        break;
    case IC_NEXT:
        iline(x + (s * 4) / 16, y + (s * 4) / 16, x + (s * 9) / 16, cy, c, a, th);
        iline(x + (s * 9) / 16, cy, x + (s * 4) / 16, y + (s * 12) / 16, c, a, th);
        Graphics::FillRect(x + (s * 10) / 16, y + (s * 4) / 16, (s * 2) / 16, (s * 8) / 16, c);
        break;
    case IC_PREV:
        iline(x + (s * 12) / 16, y + (s * 4) / 16, x + (s * 7) / 16, cy, c, a, th);
        iline(x + (s * 7) / 16, cy, x + (s * 12) / 16, y + (s * 12) / 16, c, a, th);
        Graphics::FillRect(x + (s * 4) / 16, y + (s * 4) / 16, (s * 2) / 16, (s * 8) / 16, c);
        break;
    case IC_MONITOR:
        ioutline(x + (s * 3) / 16, y + (s * 3) / 16, (s * 10) / 16, (s * 7) / 16, 2, c, a, th);
        iline(cx, y + (s * 10) / 16, cx, y + (s * 13) / 16, c, a, th);
        iline(cx - (s * 3) / 16, y + (s * 13) / 16, cx + (s * 3) / 16, y + (s * 13) / 16, c, a, th);
        break;
    case IC_BATTERY: {
        int w = (s * 11) / 16, h = (s * 6) / 16;
        int bx = cx - w / 2, by = cy - h / 2;
        ioutline(bx, by, w, h, 2, c, a, th);
        Graphics::FillRect(bx + w, cy - u, u * 2, u * 2, c);
        Graphics::FillRect(bx + 2, by + 2, (w * 6) / 10, h - 4, c);
        break;
    }
    case IC_USER:
        idisc(cx, y + (s * 6) / 16, (s * 3) / 16, c, a);
        iarc(cx, y + (s * 15) / 16, (s * 6) / 16, 3.34f, 6.08f, c, a, th);
        break;
    case IC_CLOSE:
        iline(cx - u * 2, cy - u * 2, cx + u * 2, cy + u * 2, c, a, th);
        iline(cx + u * 2, cy - u * 2, cx - u * 2, cy + u * 2, c, a, th);
        break;
    case IC_WARN:
        iline(cx, y + (s * 3) / 16, x + (s * 13) / 16, y + (s * 13) / 16, c, a, th);
        iline(x + (s * 13) / 16, y + (s * 13) / 16, x + (s * 3) / 16, y + (s * 13) / 16, c, a, th);
        iline(x + (s * 3) / 16, y + (s * 13) / 16, cx, y + (s * 3) / 16, c, a, th);
        iline(cx, y + (s * 6) / 16, cx, y + (s * 10) / 16, c, a, th);
        idisc(cx, y + (s * 12) / 16, u / 2, c, a);
        break;
    default: break;
    }
}

static void Switch(int x, int y, bool on, const Theme& t, int a, uint32_t anim_id) {
    int w = 42, h = 24;
    float pos = KSS::Anim::Float(0xCC500000u + anim_id, on ? 1.0f : 0.0f, 170, KSS::Anim::OutCubic);
    uint32_t offc = t.dark ? 0xFF4A4B57 : 0xFFC9C9D1;
    uint32_t bg = mixc(offc, t.accent, (int)(pos * 255.0f));
    MDraw::RoundRect(x, y, w, h, h / 2, bg, alpha_fade(a, 245));
    int kx = x + 3 + (int)(pos * (float)(w - h));
    MDraw::Circle(kx + (h - 6) / 2, y + h / 2, (h - 6) / 2, 0xFFFFFFFFu, a);
}

static void DrawTile(int id, const Rect& r, IconKind ic, const char* label, const char* sub,
                     bool on, const Theme& t, int a, bool focus) {
    uint32_t bgc = KSS::Anim::Color(0xCC010000u + (uint32_t)id,
                                    on ? t.accent : t.card,
                                    180, KSS::Anim::OutCubic);
    int alpha = alpha_fade(a, on ? 238 : 190);
    float press = KSS::Anim::Float(0xCC200000u + (uint32_t)id, 1.0f, 120, KSS::Anim::OutCubic);
    int dx = (int)((1.0f - press) * (float)r.w * 0.5f);
    int dy = (int)((1.0f - press) * (float)r.h * 0.5f);
    Rect q = { r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy };
    bool hot = r.hit(Mouse::mx, Mouse::my);
    if (hot && !on) bgc = mixc(bgc, t.card_hi, 80);
    MDraw::RoundRect(q.x, q.y, q.w, q.h, 14, bgc, alpha);
    MDraw::RoundOutline(q.x, q.y, q.w, q.h, 14, t.line, alpha_fade(a, t.dark ? 26 : 18));
    uint32_t ink = on ? t.on_accent : t.text;
    uint32_t subc = on ? t.on_accent : t.dim;
    int isz = 24;
    DrawIcon(ic, q.x + 14, q.y + (q.h - isz) / 2, isz, ink, bgc, a);
    int tx = q.x + 46;
    char lb[40], sb[56];
    fit_text(lb, sizeof(lb), 15.0f, label, q.w - 54, true);
    fit_text(sb, sizeof(sb), 12.0f, sub, q.w - 54);
    txt(tx, q.y + 9, 15.0f, lb, ink, true);
    txt(tx, q.y + 30, 12.0f, sb, subc);
    if (focus) MDraw::RoundOutline(q.x - 2, q.y - 2, q.w + 4, q.h + 4, 16, t.accent, 230);
}

static void DrawRowTile(int id, const Rect& r, IconKind ic, const char* label, const char* sub,
                        bool on, bool chevron, bool toggle, const Theme& t, int a, bool focus) {
    uint32_t bgc = KSS::Anim::Color(0xCC020000u + (uint32_t)id,
                                    on ? t.accent : t.card, 180, KSS::Anim::OutCubic);
    bool hot = r.hit(Mouse::mx, Mouse::my);
    if (hot && !on) bgc = mixc(bgc, t.card_hi, 90);
    MDraw::RoundRect(r.x, r.y, r.w, r.h, 14, bgc, alpha_fade(a, on ? 238 : 190));
    MDraw::RoundOutline(r.x, r.y, r.w, r.h, 14, t.line, alpha_fade(a, t.dark ? 26 : 18));
    uint32_t ink = on ? t.on_accent : t.text;
    uint32_t subc = on ? t.on_accent : t.dim;
    int isz = 24;
    DrawIcon(ic, r.x + 14, r.y + (r.h - isz) / 2, isz, ink, bgc, a);
    int tx = r.x + 46;
    char lb[48], sb[64];
    int right_pad = toggle ? 64 : (chevron ? 34 : 12);
    fit_text(lb, sizeof(lb), 15.0f, label, r.w - 46 - right_pad, true);
    fit_text(sb, sizeof(sb), 12.0f, sub, r.w - 46 - right_pad);
    txt(tx, r.y + 7, 15.0f, lb, ink, true);
    txt(tx, r.y + 27, 12.0f, sb, subc);
    if (toggle) Switch(r.x + r.w - 56, r.y + (r.h - 24) / 2, on, t, a, (uint32_t)id);
    else if (chevron) DrawIcon(IC_CHEVRON, r.x + r.w - 28, r.y + (r.h - 20) / 2, 20, ink, bgc, a);
    if (focus) MDraw::RoundOutline(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 16, t.accent, 230);
}

static void DrawCard(const Rect& r, const Theme& t, int a) {
    MDraw::RoundRect(r.x, r.y, r.w, r.h, 14, t.card, alpha_fade(a, 172));
    MDraw::RoundOutline(r.x, r.y, r.w, r.h, 14, t.line, alpha_fade(a, t.dark ? 24 : 16));
}

static void DrawCardTitle(const Rect& r, IconKind ic, const char* label, const char* value,
                          const Theme& t, int a) {
    DrawIcon(ic, r.x + 14, r.y + 11, 18, t.dim, t.card, a);
    txt(r.x + 38, r.y + 8, 13.0f, label, t.dim, true);
    if (value) txt_right(r.x + r.w - 14, r.y + 8, 12.0f, value, t.dim);
}

static void DrawDifficultySlider(const Rect& r, int pct, uint32_t accent, const Theme& t,
                                 int a, bool focus) {
    int ty = r.y + r.h / 2 - 4;
    int track_w = r.w - 24;
    int tx = r.x + 12;
    MDraw::RoundRect(tx, ty, track_w, 8, 4, t.track, alpha_fade(a, 220));
    int fw = (track_w * clampi(pct, 0, 100)) / 100;
    if (fw > 0) MDraw::RoundRect(tx, ty, fw, 8, 4, accent, a);
    int kx = tx + fw;
    if (kx < tx + 4) kx = tx + 4;
    if (kx > tx + track_w - 4) kx = tx + track_w - 4;
    MDraw::Circle(kx, ty + 4, 9, 0xFFFFFFFFu, a);
    if (focus) MDraw::RoundOutline(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 10, accent, 210);
}

static void SliderRow(const Rect& row, IconKind ic, const char* label, int pct,
                      const char* value, const Theme& t, int a, bool focus) {
    DrawIcon(ic, row.x + 6, row.y + 2, 18, t.dim, t.card, a);
    char lb[40];
    fit_text(lb, sizeof(lb), 13.0f, label, row.w - 130, true);
    txt(row.x + 30, row.y, 13.0f, lb, t.text, true);
    if (value) txt_right(row.x + row.w - 6, row.y, 12.0f, value, t.dim);
    Rect tr = { row.x, row.y + 18, row.w, 22 };
    DrawDifficultySlider(tr, pct, t.accent, t, a, focus);
}

static void ButtonPill(const Rect& r, IconKind ic, const char* label, uint32_t base,
                       const Theme& t, int a, bool focus, bool danger) {
    bool hot = r.hit(Mouse::mx, Mouse::my);
    uint32_t bg = danger ? (hot ? MDraw::Mix(t.danger, 0xFFFFFFFFu, 45) : t.danger)
                         : (hot ? t.card_hi : t.card);
    MDraw::RoundRect(r.x, r.y, r.w, r.h, 12, bg, alpha_fade(a, 200));
    MDraw::RoundOutline(r.x, r.y, r.w, r.h, 12, t.line, alpha_fade(a, t.dark ? 26 : 18));
    (void)base;
    uint32_t ink = danger ? 0xFFFFFFFFu : t.text;
    int isz = 20;
    int tw = FontTTF::MeasureText(13.0f, label, true);
    int content = isz + 7 + tw;
    int start = r.x + (r.w - content) / 2;
    DrawIcon(ic, start, r.y + (r.h - isz) / 2, isz, ink, bg, a);
    txt(start + isz + 7, r.y + (r.h - 17) / 2, 13.0f, label, ink, true);
    if (focus) MDraw::RoundOutline(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 14, t.accent, 230);
}

static void SegButton(const Rect& r, const char* label, bool on, const Theme& t, int a, bool focus) {
    bool hot = r.hit(Mouse::mx, Mouse::my);
    uint32_t bg = on ? t.accent : (hot ? t.card_hi : t.field);
    MDraw::RoundRect(r.x, r.y, r.w, r.h, 9, bg, alpha_fade(a, on ? 240 : 200));
    txt_center(r.x + r.w / 2, r.y + (r.h - 15) / 2, 12.0f, label,
               on ? t.on_accent : t.text, true);
    if (focus) MDraw::RoundOutline(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 10, t.accent, 230);
}

static void DrawFocusRings(const Theme& t, int a) {
    if (!g_focus_visible || g_focus_cur == F_NONE) return;
    const FocusItem* f = FindFocus(g_focus_cur);
    if (!f) return;
    MDraw::RoundOutline(f->r.x - 2, f->r.y - 2, f->r.w + 4, f->r.h + 4, 13, t.accent, alpha_fade(a, 235));
}

// subsystem helpers --------------------------------------------------------
static const LinuxDriver* driver_by_prefix(const char* const* pfx, int n) {
    LinuxDriver* d = LinuxDriverFramework::GetDrivers();
    int count = LinuxDriverFramework::GetDriverCount();
    for (int i = 0; i < count; i++) {
        if (!d[i].state || d[i].state == LDRV_UNLOADED) continue;
        for (int p = 0; p < n; p++) {
            if (starts_with(d[i].name, pfx[p])) return &d[i];
        }
    }
    return nullptr;
}
static void refresh_bt() {
    static const char* pfx[] = { "bluetooth_", "bluetooth", "bt", "hci" };
    const LinuxDriver* d = driver_by_prefix(pfx, 4);
    g_bt_present = d != nullptr;
    if (d) {
        g_bt_on = (d->state == LDRV_ACTIVE && d->bound);
        scpy(g_bt_name, d->description[0] ? d->description : d->name, sizeof(g_bt_name));
    } else {
        g_bt_on = false;
        g_bt_name[0] = 0;
    }
}
static void apply_bt(bool on) {
    static const char* pfx[] = { "bluetooth_", "bluetooth", "bt", "hci" };
    const LinuxDriver* found = driver_by_prefix(pfx, 4);
    if (!found) return;
    char name[LDRV_MAX_NAME];
    scpy(name, found->name, sizeof(name));
    if (on) {
        if (LinuxDriverFramework::LoadDriver(name) != 0) {
            NotificationManager::Post("Bluetooth", "Adapter could not be started",
                                      NotificationManager::ICON_WARNING, 2600);
        }
    } else {
        LinuxDriverFramework::UnloadDriver(name);
    }
    g_bt_on = LinuxDriverFramework::FindDriver(name) &&
              LinuxDriverFramework::FindDriver(name)->state == LDRV_ACTIVE &&
              LinuxDriverFramework::FindDriver(name)->bound;
}

static int get_brightness(int slot);
static void persist_brightness(int slot) {
    char key[32]; key[0] = 0;
    scpy(key, "display.brightness", sizeof(key));
    if (slot > 0) {
        char num[8]; intstr(slot, num, sizeof(num));
        sapp(key, ".", sizeof(key)); sapp(key, num, sizeof(key));
    }
    UIConfig::SetInt(key, get_brightness(slot), true);
    UIConfig::Save();
}
static void set_brightness(int slot, int pct) {
    pct = clampi(pct, 10, 100);
    if (Graphics::GetOutputBrightness(slot) == pct) return;
    Graphics::SetOutputBrightness(slot, pct);
    if (slot == 0) {
        DisplayManager::SetBrightness((uint8_t)((pct * 255) / 100));
        Backlight::Set(pct);   // legacy pair, or the bxt layout with kurono.backlight=1 (satoru)
    }
}
static void persist_volume() {
    UIConfig::SetInt("audio.master_volume", AudioMixer::GetMasterVolume(), true);
    UIConfig::Save();
}
static int get_brightness(int slot) {
    int v = Graphics::GetOutputBrightness(slot);
    if (v <= 0) v = 100;
    return clampi(v, 10, 100);
}
static int primary_brightness_from_config() {
    int v = UIConfig::Int("display.brightness", 100);
    return clampi(v, 10, 100);
}
static void apply_volume(int pct) {
    pct = clampi(pct, 0, 100);
    AudioMixer::SetMasterVolume(pct);
    if (Audio::IsAvailable()) Audio::SetMasterVolume(pct);
}
static void toggle_mute() {
    bool m = !AudioMixer::GetMasterMute();
    AudioMixer::SetMasterMute(m);
    if (Audio::IsAvailable()) Audio::SetMuted(m);
}
static const char* audio_name(const char* name) {
    if (!name) return "Output";
    if (name[0] == 'h' && name[1] == 'd' && name[2] == 'a') return "Intel HDA";
    if (name[0] == 'a' && name[1] == 'c') return "AC'97";
    if (name[0] == 's' && name[1] == 'b') return "Sound Blaster 16";
    if (name[0] == 'p' && name[1] == 'c') return "PC Speaker";
    if (starts_with(name, "usb")) return "USB Audio";
    if (name[0] == 'n' && name[1] == 'u') return "Null Output";
    return name;
}
static void set_power_mode(int idx) {
    static const int plan[] = { 2, 1, 0 };
    int p = plan[clampi(idx, 0, 2)];
    int n = CPUFreq::CPUCount();
    CPUFreq::Governor g = p == 0 ? CPUFreq::GOV_PERFORMANCE
                        : (p == 2 ? CPUFreq::GOV_POWERSAVE : CPUFreq::GOV_ONDEMAND);
    for (int i = 0; i < n; i++) CPUFreq::SetGovernor((uint32_t)i, g);
    UIConfig::SetInt("power.plan", p, true);
    UIConfig::Save();
}
static int power_mode_idx() {
    int plan = UIConfig::Int("power.plan", 1);
    if (plan == 0) return 2;
    if (plan == 2) return 0;
    int n = CPUFreq::CPUCount();
    if (n > 0) {
        CPUFreq::Governor g = CPUFreq::GetGovernor(0);
        if (g == CPUFreq::GOV_PERFORMANCE) return 2;
        if (g == CPUFreq::GOV_POWERSAVE) return 0;
    }
    return 1;
}
static int battery_percent() {
    int v = UIConfig::Int("power.battery_percent", 100);
    return clampi(v, 0, 100);
}
static const char* battery_time_text() {
    static const char* pfx[] = { "battery", "bat", "acpi_battery" };
    if (driver_by_prefix(pfx, 3)) return "AC power";
    return "Plugged in";
}

static void request_power_confirm(int action) {
    g_confirm_action = action;
    g_popup = POP_CONFIRM;
    g_focus_cur = F_CONFIRM_NO;
    g_focus_visible = true;
    g_layout_valid = false;
    Graphics::MarkUIDirty();
    Graphics::RequestFrameAt(NowMs() + 1);
}

// the panel already asked the user to confirm, so this is the user's own
// request. hand it to the one power worker instead of saving/rebooting on the
// compositor thread: a full snapshot used to freeze the desktop here, which
// looked like a hang and invited a hard power-off. (satoru)
static void execute_power_action(int action) {
    if (action == PA_NONE) return;
    ControlCenter::Close();
    PowerManager::Action a = action == PA_SLEEP ? PowerManager::ACTION_SLEEP
                           : action == PA_RESTART ? PowerManager::ACTION_RESTART
                                                  : PowerManager::ACTION_SHUTDOWN;
    if (!PowerManager::Request(a, "control center")) {
        NotificationManager::Post("Power", "Another power action is already running",
                                  NotificationManager::ICON_WARNING, 3200);
    }
    Graphics::MarkUIDirty();
}

static void record_stop();
static void record_start() {
    SerialLogger::Log("[cc] record start\r\n");
    uint32_t ms = NowMs();
    char num[16]; intstr((int)ms, num, sizeof(num));
    scpy(g_rec_dir, "/home/user/screenrec_", sizeof(g_rec_dir));
    sapp(g_rec_dir, num, sizeof(g_rec_dir));
    KVFS::Mkdirs(g_rec_dir);
    g_recording = true;
    g_rec_start_ms = ms;
    g_rec_last_capture_ms = ms;
    g_rec_frames = 0;
    Graphics::RequestFrameAt(ms + 1);
    NotificationManager::Post("Screen recording", "Saving frames to /home/user",
                              NotificationManager::ICON_SUCCESS, 2400);
}
static void record_capture_if_due() {
    if (!g_recording) return;
    uint32_t now = NowMs();
    if (now - g_rec_last_capture_ms < 500) return;
    g_rec_last_capture_ms = now;
    char path[96];
    scpy(path, g_rec_dir, sizeof(path));
    char num[16]; intstr(g_rec_frames, num, sizeof(num));
    sapp(path, "/frame_", sizeof(path));
    if (g_rec_frames < 10) sapp(path, "00", sizeof(path));
    else if (g_rec_frames < 100) sapp(path, "0", sizeof(path));
    sapp(path, num, sizeof(path));
    sapp(path, ".bmp", sizeof(path));
    if (Screenshot::CaptureScaledToBMP(path, 512)) g_rec_frames++;
    if (g_rec_frames >= 120) {
        record_stop();
        return;
    }
    Graphics::RequestFrameAt(now + 500);
}
static void record_stop() {
    if (!g_recording) return;
    SerialLogger::Log("[cc] record stop\r\n");
    g_recording = false;
    char path[96];
    scpy(path, g_rec_dir, sizeof(path));
    sapp(path, "/recording.txt", sizeof(path));
    char line[128];
    scpy(line, "kurono screen recording, frames=", sizeof(line));
    char num[16]; intstr(g_rec_frames, num, sizeof(num));
    sapp(line, num, sizeof(line));
    sapp(line, ", interval_ms=500, size=512\r\n", sizeof(line));
    KVFS::WriteFile(path, line, (uint32_t)slen(line));
    NotificationManager::Post("Screen recording", "Recording stopped",
                              NotificationManager::ICON_INFO, 2400);
}
static void toggle_recording() {
    SerialLogger::Log(g_recording ? "[cc] rec toggle -> stop\r\n" : "[cc] rec toggle -> start\r\n");
    if (g_recording) record_stop();
    else record_start();
    Graphics::MarkUIDirty();
    Graphics::RequestFrameAt(NowMs() + 1);
}

static void toggle_dark() {
    const char* ap = UIConfig::Str("ui.appearance", "dark");
    bool light = ap && (ap[0] == 'l' || ap[0] == 'L');
    UIConfig::Set("ui.appearance", light ? "dark" : "light");
    UIConfig::Save();
    UIStyle::Apply();
    Graphics::MarkUIDirty();
    Graphics::RequestFrameAt(NowMs() + 1);
}
static void toggle_night() {
    NightLight::Toggle();   // strength + schedule live in ui/night_light (satoru)
    g_night_light = NightLight::IsOn();
    Graphics::MarkUIDirty();
}
static void toggle_focus_mode() {
    g_focus_mode = !g_focus_mode;
    if (g_focus_mode) {
        // post before the gate closes, or the toast itself would be held. (satoru)
        NotificationManager::Post("Do Not Disturb", "Notifications are held",
                                  NotificationManager::ICON_INFO, 2200);
        NotificationManager::SetDoNotDisturb(true);
    } else {
        int held = NotificationManager::HeldCount();
        NotificationManager::SetDoNotDisturb(false);
        char body[64];
        if (held > 0) {
            char n[16]; intstr(held, n, sizeof(n));
            scpy(body, n, sizeof(body));
            sapp(body, held == 1 ? " notification was held" : " notifications were held", sizeof(body));
        } else {
            scpy(body, "Notifications are back", sizeof(body));
        }
        NotificationManager::Post("Do Not Disturb off", body,
                                  NotificationManager::ICON_SUCCESS, 3000);
    }
    UIConfig::SetInt("ui.dnd", g_focus_mode ? 1 : 0, true);
    UIConfig::Save();
    Graphics::MarkUIDirty();
}
static void toggle_airplane() {
    g_airplane = !g_airplane;
    if (g_airplane) {
        WiFi::Disable();
        if (g_bt_present && g_bt_on) apply_bt(false);
        NotificationManager::Post("Airplane mode", "Wireless radios are off",
                                  NotificationManager::ICON_INFO, 2400);
    } else {
        if (WiFi::DetectedLink() == WiFi::LINK_WIFI) WiFi::Enable();
        if (g_bt_present) apply_bt(true);
        NotificationManager::Post("Airplane mode", "Wireless radios are back",
                                  NotificationManager::ICON_SUCCESS, 2400);
    }
    UIConfig::SetInt("network.airplane", g_airplane ? 1 : 0, true);
    UIConfig::Save();
}

// a network the store already knows joins with its saved passphrase. (satoru)
static bool wifi_is_saved(const char* ssid) {
    int n = WiFi::SavedCount();
    char nm[40];
    for (int i = 0; i < n; i++) {
        if (!WiFi::SavedAt(i, nm, (int)sizeof(nm), nullptr, nullptr)) continue;
        int k = 0; while (nm[k] && ssid[k] && nm[k] == ssid[k]) k++;
        if (!nm[k] && !ssid[k]) return true;
    }
    return false;
}

static void wifi_begin_connect(const WiFiNetwork& net) {
    scpy(g_wifi_ssid, net.ssid, sizeof(g_wifi_ssid));
    g_wifi_secure_wait = (net.security != WIFI_OPEN) && !wifi_is_saved(net.ssid);
    g_wifi_pass[0] = 0;
    g_wifi_pass_len = 0;
    if (!g_wifi_secure_wait) {
        if (!WiFi::Connect(net.ssid, "")) {
            NotificationManager::Post("Wi-Fi", "Could not join this network",
                                      NotificationManager::ICON_WARNING, 3000);
        }
        g_popup = POP_NONE;
    } else {
        g_wifi_pass_focus = true;
        g_focus_cur = F_POP_PASS;
    }
    g_layout_valid = false;
    Graphics::MarkUIDirty();
}
static void wifi_submit_password() {
    if (!g_wifi_ssid[0]) return;
    if (!WiFi::Connect(g_wifi_ssid, g_wifi_pass)) {
        NotificationManager::Post("Wi-Fi", "Could not join this network",
                                  NotificationManager::ICON_WARNING, 3000);
    } else {
        NotificationManager::Post("Wi-Fi", g_wifi_ssid, NotificationManager::ICON_SUCCESS, 2400);
        g_popup = POP_NONE;
    }
    // the typed passphrase is not kept here (the store has its own copy). (satoru)
    for (int i = 0; i < (int)sizeof(g_wifi_pass); i++) ((volatile char*)g_wifi_pass)[i] = 0;
    g_wifi_pass_len = 0;
    g_wifi_pass_focus = false;
    g_layout_valid = false;
    Graphics::MarkUIDirty();
}

// layout -------------------------------------------------------------------
static void apply_layout_for_anchor() {
    int ox = 0, oy = 0, ow = screen_w, oh = screen_h;
    if (Outputs::Multi()) {
        const Output* o = Outputs::Get(g_out_idx);
        if (!o || !o->connected || o->w <= 0) { o = Outputs::Get(0); g_out_idx = 0; }
        if (o && o->w > 0) { ox = o->x; oy = o->y; ow = o->w; oh = o->h; }
    }
    int maxw = ow - 24;
    panel_w = CC_W < maxw ? CC_W : maxw;
    if (panel_w < 320) panel_w = maxw;
    int anchor_y = g_anchor_y;
    g_top_anchor = (anchor_y < oy + oh / 2);
    int avail = g_top_anchor ? (oy + oh - anchor_y - 16) : (anchor_y - oy - 16);
    panel_h = CC_H < avail ? CC_H : avail;
    if (panel_h < 360) panel_h = avail > 360 ? 360 : (avail > 0 ? avail : CC_H);
    panel_x = clampi(g_anchor_x - panel_w / 2, ox + 8, ox + ow - panel_w - 8);
    if (g_top_anchor) {
        panel_y = anchor_y + 6;
        if (panel_y + panel_h > oy + oh - 8) panel_y = oy + oh - panel_h - 8;
    } else {
        panel_y = anchor_y - panel_h - 6;
        if (panel_y < oy + 8) panel_y = oy + 8;
    }
    panel_x = clampi(panel_x, ox + 8, ox + ow - panel_w - 8);
    panel_y = clampi(panel_y, oy + 8, oy + oh - panel_h - 8);
}

static void build_layout() {
    apply_layout_for_anchor();
    Layout& L = g_L;
    for (int i = 0; i < (int)sizeof(L); i++) ((uint8_t*)&L)[i] = 0;
    L.panel.x = panel_x; L.panel.y = panel_y; L.panel.w = panel_w; L.panel.h = panel_h;
    g_focus_count = 0;

    int x = panel_x + PAD;
    int y = panel_y + HEADER_H;
    int cw = panel_w - 2 * PAD;
    int action_y = panel_y + panel_h - PAD - ACTION_H;
    int content_h = action_y - 10 - y;
    if (content_h < 240) content_h = 240;
    int left_w = (cw - 16) * 52 / 100;
    int right_x = x + left_w + 16;
    int right_w = cw - left_w - 16;

    // action bar (full width)
    int abw = (cw - 4 * BTN_GAP) / 5;
    for (int i = 0; i < 5; i++) {
        L.actions[i].x = x + i * (abw + BTN_GAP);
        L.actions[i].y = action_y;
        L.actions[i].w = abw;
        L.actions[i].h = ACTION_H;
    }

    // left column
    int ly = y;
    L.wifi = { x, ly, left_w, 58 }; ly += 58 + GAP;
    refresh_bt();
    bool has_bt = g_bt_present;
    if (has_bt) { L.bt = { x, ly, left_w, 52 }; ly += 52 + GAP; }
    else        { L.bt = { 0, 0, 0, 0 }; }
    L.net = { x, ly, left_w, 54 }; ly += 54 + GAP;

    int tile_w = (left_w - GAP) / 2;
    int tile_h = 54;
    int tile_rows = 3;
    int tiles_bottom = ly + tile_rows * tile_h + (tile_rows - 1) * GAP;
    int air_y = action_y - 8 - 48;
    if (tiles_bottom > air_y - GAP) {
        int avail = air_y - GAP - ly;
        int fit = (avail - (tile_rows - 1) * GAP) / tile_rows;
        if (fit < 44) fit = 44;
        tile_h = fit;
    }
    static const struct { int id; IconKind ic; const char* label; const char* sub_on; const char* sub_off; } kTiles[] = {
        { F_FOCUS, IC_FOCUS,   "Focus",        "On",  "Off" },
        { F_DARK,  IC_MOON,    "Dark Mode",    "On",  "Off" },
        { F_NIGHT, IC_NIGHT,   "Night Light",  "Warm", "Off" },
        { F_SHOT,  IC_CAMERA,  "Screenshot",   "Capture", "Save a frame" },
        { F_REC,   IC_RECORD,  "Record",       "Recording", "Screen capture" },
        { F_PERF,  IC_PULSE,   "Performance",  "Overlay on", "Overlay off" },
    };
    L.tile_count = 6;
    for (int i = 0; i < 6; i++) {
        int row = i / 2, col = i % 2;
        L.tiles[i] = { x + col * (tile_w + GAP), ly + row * (tile_h + GAP), tile_w, tile_h };
    }
    int tiles_end = ly + 3 * (tile_h + GAP) - GAP;
    L.airplane = { x, air_y, left_w, 48 };
    // if the left column is taller than expected, keep the airplane row under it
    if (tiles_end + GAP <= air_y) L.airplane.y = air_y;
    else L.airplane.y = tiles_end + GAP;
    if (L.airplane.y + L.airplane.h > action_y - 4) L.airplane.y = action_y - 4 - L.airplane.h;

    // right column
    int nlive = 0;
    int slots[OUTPUT_MAX] = {0};
    if (Outputs::Multi()) {
        for (int i = 0; i < Outputs::Total() && nlive < OUTPUT_MAX; i++) {
            if (Outputs::IsLive(i)) { slots[nlive++] = i; }
        }
    } else {
        slots[0] = 0; nlive = 1;
    }
    if (nlive <= 0) { slots[0] = 0; nlive = 1; }
    L.bright_count = nlive;
    int bright_h = 32 + nlive * 28;
    int vol_h = 80;
    int power_h = 104;
    int media_h = content_h - bright_h - vol_h - power_h - 3 * GAP;
    if (media_h < 86) { power_h = 92; media_h = content_h - bright_h - vol_h - power_h - 3 * GAP; }
    if (media_h < 78) { bright_h = 30 + nlive * 24; media_h = content_h - bright_h - vol_h - power_h - 3 * GAP; }
    if (media_h < 70) media_h = 70;
    int ry = y;
    L.bright_card = { right_x, ry, right_w, bright_h }; ry += bright_h + GAP;
    L.vol_card = { right_x, ry, right_w, vol_h }; ry += vol_h + GAP;
    L.media_card = { right_x, ry, right_w, media_h }; ry += media_h + GAP;
    L.power_card = { right_x, ry, right_w, power_h };

    int sx = L.bright_card.x + 14;
    int sw = L.bright_card.w - 28;
    int sy = L.bright_card.y + 30;
    for (int i = 0; i < nlive; i++) {
        L.bright_slider[i] = { sx, sy + i * 28, sw, 24 };
        L.bright_slot[i] = slots[i];
    }
    int vy = L.vol_card.y + 30;
    L.vol_slider = { sx, vy, sw - 34, 24 };
    L.mute_btn = { L.vol_card.x + L.vol_card.w - 38, L.vol_card.y + 30, 26, 24 };
    L.out_btn = { L.vol_card.x + 12, L.vol_card.y + 56, L.vol_card.w - 24, 20 };

    int mid_x = L.media_card.x;
    int mid_w = L.media_card.w;
    int mby = L.media_card.y + L.media_card.h - 30;
    int bw = 32;
    L.media_prev = { mid_x + mid_w / 2 - bw - 44, mby, bw, 24 };
    L.media_play = { mid_x + mid_w / 2 - bw / 2, mby, bw, 24 };
    L.media_next = { mid_x + mid_w / 2 + 44, mby, bw, 24 };
    L.media_bar = { mid_x + 14, L.media_card.y + 62, mid_w - 28, 8 };

    int pw = (L.power_card.w - 28 - 8) / 3;
    for (int i = 0; i < 3; i++)
        L.power_mode[i] = { L.power_card.x + 14 + i * (pw + 4), L.power_card.y + L.power_card.h - 34, pw, 24 };

    // focus list
    if (g_popup == POP_NONE) {
        AddFocus(F_WIFI, L.wifi);
        if (has_bt) AddFocus(F_BT, L.bt);
        AddFocus(F_NET, L.net);
        for (int i = 0; i < 6; i++) AddFocus(kTiles[i].id, L.tiles[i]);
        AddFocus(F_AIR, L.airplane);
        for (int i = 0; i < L.bright_count; i++) AddFocus(F_BRIGHT + i, L.bright_slider[i]);
        AddFocus(F_VOLUME, L.vol_slider);
        AddFocus(F_MUTE, L.mute_btn);
        AddFocus(F_OUTPUT, L.out_btn);
        AddFocus(F_MEDIA_PREV, L.media_prev);
        AddFocus(F_MEDIA_PLAY, L.media_play);
        AddFocus(F_MEDIA_NEXT, L.media_next);
        for (int i = 0; i < 3; i++) AddFocus(F_MODE + i, L.power_mode[i]);
        static const int act_id[5] = { F_SETTINGS, F_LOCK, F_SLEEP, F_RESTART, F_SHUTDOWN };
        for (int i = 0; i < 5; i++) AddFocus(act_id[i], L.actions[i]);
    } else if (g_popup == POP_WIFI) {
        int popw = L.wifi.w;
        int popx = L.wifi.x;
        int popy = L.wifi.y + L.wifi.h + 6;
        int n = WiFi::GetNetworkCount();
        if (n > 4) n = 4;
        if (g_wifi_secure_wait) n = 0;
        int rows = n;
        int poph = 44 + (rows * 34) + 46;
        if (g_wifi_secure_wait) poph = 44 + 30 + 72;
        if (WiFi::DetectedLink() != WiFi::LINK_WIFI) poph = 44 + 46 + 46;
        else if (WiFi::GetState() == WIFI_OFF) poph = 44 + 46 + 46;
        if (popy + poph > panel_y + panel_h - 8) popy = L.wifi.y - poph - 6;
        if (popy < panel_y + 8) popy = panel_y + 8;
        L.popup = { popx, popy, popw, poph };
        L.popup_toggle = { popx + popw - 56, popy + 10, 42, 24 };
        L.popup_row_count = rows;
        for (int i = 0; i < rows; i++)
            L.popup_rows[i] = { popx + 8, popy + 44 + i * 34, popw - 16, 32 };
        if (g_wifi_secure_wait) {
            L.popup_pass = { popx + 12, popy + 74, popw - 24, 26 };
            L.popup_connect = { popx + popw - 112, popy + 74 + 32, 100, 26 };
            L.popup_cancel = { popx + 12, popy + 74 + 32, 70, 26 };
        } else {
            L.popup_scan = { popx + popw - 90, popy + poph - 36, 78, 26 };
        }
        AddFocus(F_POP_TOGGLE, L.popup_toggle);
        for (int i = 0; i < rows; i++) AddFocus(F_POP_ROW + i, L.popup_rows[i]);
        if (g_wifi_secure_wait) {
            AddFocus(F_POP_PASS, L.popup_pass);
            AddFocus(F_POP_CONNECT, L.popup_connect);
            AddFocus(F_POP_CANCEL, L.popup_cancel);
        } else {
            AddFocus(F_POP_SCAN, L.popup_scan);
        }
    } else if (g_popup == POP_AUDIO) {
        int n = AudioServer::BackendCount();
        if (n > 6) n = 6;
        int popw = 300;
        int poph = 40 + n * 32 + 40;
        int popx = L.out_btn.x + L.out_btn.w - popw;
        int popy = L.out_btn.y + L.out_btn.h + 6;
        if (popx < panel_x + 8) popx = panel_x + 8;
        if (popy + poph > panel_y + panel_h - 8) popy = L.out_btn.y - poph - 6;
        L.popup = { popx, popy, popw, poph };
        L.popup_row_count = n;
        for (int i = 0; i < n; i++)
            L.popup_rows[i] = { popx + 8, popy + 36 + i * 32, popw - 16, 30 };
        int by = popy + 36 + n * 32 + 4;
        int bw = (popw - 24) / 2;
        L.popup_restart = { popx + 8, by, bw, 28 };
        L.popup_stop    = { popx + 16 + bw, by, bw, 28 };
        for (int i = 0; i < n; i++) AddFocus(F_AUDIO_ROW + i, L.popup_rows[i]);
        AddFocus(F_AUDIO_RESTART, L.popup_restart);
        AddFocus(F_AUDIO_STOP, L.popup_stop);
    } else if (g_popup == POP_CONFIRM) {
        int w = 360, h = 158;
        int px = panel_x + (panel_w - w) / 2;
        int py = panel_y + (panel_h - h) / 2;
        L.popup = { px, py, w, h };
        L.confirm_no  = { px + 20, py + h - 54, 120, 34 };
        L.confirm_yes = { px + w - 150, py + h - 54, 130, 34 };
        AddFocus(F_CONFIRM_NO, L.confirm_no);
        AddFocus(F_CONFIRM_YES, L.confirm_yes);
    }
    g_layout_valid = true;
}

static void ensure_layout() {
    if (!g_layout_valid) build_layout();
}

// activation ---------------------------------------------------------------
static void activate(int id) {
    Layout& L = g_L;
    ensure_layout();
    if (id == F_WIFI) {
        g_popup = POP_WIFI;
        g_wifi_secure_wait = false;
        g_layout_valid = false;
        if (WiFi::DetectedLink() == WiFi::LINK_WIFI &&
            WiFi::GetState() != WIFI_OFF && WiFi::GetNetworkCount() == 0) WiFi::Scan();
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_BT) {
        apply_bt(!g_bt_on);
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_NET) {
        ControlCenter::Close();
        SystemSettings::OpenPage("network");
        return;
    }
    if (id == F_FOCUS) { toggle_focus_mode(); return; }
    if (id == F_DARK)  { toggle_dark(); return; }
    if (id == F_NIGHT) { toggle_night(); return; }
    if (id == F_SHOT) {
        if (Screenshot::CaptureTimestamped())
            NotificationManager::Post("Screenshot", "Saved to /home/user",
                                      NotificationManager::ICON_SUCCESS, 2400);
        else
            NotificationManager::Post("Screenshot", "Could not save the frame",
                                      NotificationManager::ICON_WARNING, 2400);
        return;
    }
    if (id == F_REC)  { toggle_recording(); return; }
    if (id == F_PERF) { PerfHUD::Toggle(); Graphics::MarkUIDirty(); return; }
    if (id == F_AIR)  { toggle_airplane(); Graphics::MarkUIDirty(); return; }
    if (id == F_MUTE) { toggle_mute(); Graphics::MarkUIDirty(); return; }
    if (id == F_OUTPUT) {
        g_popup = POP_AUDIO;
        g_layout_valid = false;
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_MEDIA_PREV) { MediaPlayerApp::Previous(); return; }
    if (id == F_MEDIA_PLAY) { MediaPlayerApp::TogglePlayback(); return; }
    if (id == F_MEDIA_NEXT) { MediaPlayerApp::Next(); return; }
    if (id >= F_MODE && id < F_MODE + 3) {
        set_power_mode(id - F_MODE);
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_SETTINGS) { ControlCenter::Close(); DesktopEnvironment::LaunchSettings(); return; }
    if (id == F_LOCK) {
        ControlCenter::Close();
        Graphics::MarkUIDirty();
        LockScreen::Show();
        Graphics::MarkDirty(0, 0, Graphics::GetWidth(), Graphics::GetHeight());
        Graphics::MarkUIDirty();
        Keyboard::FlushBuffers();
        return;
    }
    if (id == F_SLEEP)    { request_power_confirm(PA_SLEEP); return; }
    if (id == F_RESTART)  { request_power_confirm(PA_RESTART); return; }
    if (id == F_SHUTDOWN) { request_power_confirm(PA_SHUTDOWN); return; }
    if (id == F_POP_TOGGLE) {
        if (WiFi::DetectedLink() != WiFi::LINK_WIFI) {
            NotificationManager::Post("Wi-Fi", "No wireless adapter is present",
                                      NotificationManager::ICON_WARNING, 2600);
            return;
        }
        if (WiFi::GetState() == WIFI_OFF) {
            WiFi::Enable();
            if (WiFi::DetectedLink() == WiFi::LINK_WIFI) WiFi::Scan();
        } else {
            WiFi::Disable();
        }
        g_layout_valid = false;
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_POP_SCAN) {
        WiFi::Scan();
        g_layout_valid = false;
        Graphics::MarkUIDirty();
        return;
    }
    if (id >= F_POP_ROW && id < F_POP_ROW + 6) {
        int idx = id - F_POP_ROW;
        WiFiNetwork* nets = WiFi::GetNetworks();
        int n = WiFi::GetNetworkCount();
        if (nets && idx >= 0 && idx < n) wifi_begin_connect(nets[idx]);
        return;
    }
    if (id == F_POP_CONNECT) { wifi_submit_password(); return; }
    if (id == F_POP_CANCEL)  { g_popup = POP_NONE; g_wifi_pass_focus = false; g_layout_valid = false; Graphics::MarkUIDirty(); return; }
    if (id == F_AUDIO_RESTART || id == F_AUDIO_STOP) {
        if (id == F_AUDIO_RESTART) {
            bool ok = AudioServer::RestartActive();
            NotificationManager::Post("Sound", ok ? "Audio restarted" : "Audio restart failed",
                                      ok ? NotificationManager::ICON_SUCCESS : NotificationManager::ICON_WARNING, 2200);
        } else {
            int n = AudioServer::StopAllStreams();
            char msg[48]; char num[8];
            intstr(n, num, sizeof(num));
            scpy(msg, num, sizeof(msg));
            sapp(msg, n == 1 ? " stream stopped" : " streams stopped", sizeof(msg));
            NotificationManager::Post("Sound", msg, NotificationManager::ICON_SUCCESS, 2200);
        }
        g_popup = POP_NONE;
        g_layout_valid = false;
        Graphics::MarkUIDirty();
        return;
    }
    if (id >= F_AUDIO_ROW && id < F_AUDIO_ROW + 6) {
        int idx = id - F_AUDIO_ROW;
        AudioBackend* be = AudioServer::BackendAt(idx);
        if (be) {
            if (!AudioDevices::SelectOutput(be->Name())) {   // applies its remembered volume (satoru)
                NotificationManager::Post("Sound", "That output is not available",
                                          NotificationManager::ICON_WARNING, 2400);
            } else {
                NotificationManager::Post("Sound", audio_name(be->Name()),
                                          NotificationManager::ICON_SUCCESS, 1800);
            }
        }
        g_popup = POP_NONE;
        g_layout_valid = false;
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_CONFIRM_NO) {
        g_popup = POP_NONE;
        g_confirm_action = PA_NONE;
        g_layout_valid = false;
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_CONFIRM_YES) {
        int a = g_confirm_action;
        g_popup = POP_NONE;
        g_confirm_action = PA_NONE;
        g_layout_valid = false;
        execute_power_action(a);
        return;
    }
    (void)L;
}

// focus + keyboard ---------------------------------------------------------
static int focus_index_of(int id) {
    for (int i = 0; i < g_focus_count; i++) if (g_focus[i].id == id) return i;
    return -1;
}
static void focus_step(int dir) {
    if (g_focus_count <= 0) { g_focus_cur = F_NONE; return; }
    int idx = focus_index_of(g_focus_cur);
    if (idx < 0) idx = dir > 0 ? -1 : 0;
    idx = (idx + (dir > 0 ? 1 : g_focus_count - 1)) % g_focus_count;
    g_focus_cur = g_focus[idx].id;
    g_focus_visible = true;
    Graphics::MarkUIDirty();
}
static void focus_spatial(int dx, int dy) {
    if (g_focus_count <= 0) return;
    const FocusItem* cur = FindFocus(g_focus_cur);
    if (!cur) { focus_step(1); return; }
    int cx = cur->r.x + cur->r.w / 2, cy = cur->r.y + cur->r.h / 2;
    int best = -1, best_score = 0x7FFFFFFF;
    for (int i = 0; i < g_focus_count; i++) {
        if (g_focus[i].id == g_focus_cur) continue;
        int x = g_focus[i].r.x + g_focus[i].r.w / 2;
        int y = g_focus[i].r.y + g_focus[i].r.h / 2;
        int ddx = x - cx, ddy = y - cy;
        int primary = dx ? ddx * dx : ddy * dy;
        if (primary <= 2) continue;
        int secondary = dx ? absc(ddy) : absc(ddx);
        int score = primary * 8 + secondary;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) { g_focus_cur = g_focus[best].id; g_focus_visible = true; Graphics::MarkUIDirty(); }
    else focus_step(dx + dy > 0 ? 1 : -1);
}
static bool focus_is_slider(int id) {
    if (id >= F_BRIGHT && id < F_BRIGHT + OUTPUT_MAX) return true;
    if (id == F_VOLUME) return true;
    return false;
}
static void adjust_focus(int id, int dir) {
    ensure_layout();
    if (id >= F_BRIGHT && id < F_BRIGHT + OUTPUT_MAX) {
        int idx = id - F_BRIGHT;
        if (idx >= g_L.bright_count) return;
        int slot = g_L.bright_slot[idx];
        int pct = get_brightness(slot) + dir * 5;
        set_brightness(slot, pct);
        persist_brightness(slot);
        Graphics::MarkUIDirty();
        return;
    }
    if (id == F_VOLUME) {
        int pct = AudioMixer::GetMasterVolume() + dir * 5;
        apply_volume(pct);
        persist_volume();
        Graphics::MarkUIDirty();
        return;
    }
}

}  // namespace

void ControlCenter::ExecutePowerActionForTest(int action) {
    execute_power_action(action);
}

bool ControlCenter::CapturesKeyboard() {
    return open;
}

bool ControlCenter::HandleKeyEvent(int key, bool shift, bool ctrl, bool alt, bool pressed) {
    (void)ctrl; (void)alt;
    if (!open) return false;
    if (key < 0 || key > KEY_WAKE) return true;
    bool was = g_key_held[key];
    g_key_held[key] = pressed;
    if (!pressed) return true;
    bool repeat = was;
    ensure_layout();

    if (key == KEY_ESC) {
        if (g_popup != POP_NONE) {
            g_popup = POP_NONE;
            g_wifi_pass_focus = false;
            g_layout_valid = false;
            Graphics::MarkUIDirty();
        } else {
            ControlCenter::Close();
        }
        return true;
    }
    if (key == KEY_TAB) {
        focus_step(shift ? -1 : 1);
        return true;
    }
    if (key == KEY_DOWN || (!g_popup && key == KEY_UP)) {
        focus_step(key == KEY_DOWN ? 1 : -1);
        return true;
    }
    if (key == KEY_UP) {
        if (g_popup != POP_NONE) { focus_step(-1); return true; }
        focus_spatial(0, -1);
        return true;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        int dir = key == KEY_RIGHT ? 1 : -1;
        if (focus_is_slider(g_focus_cur)) {
            g_key_adjust = g_focus_cur;
            g_key_adjust_dir = dir;
        } else {
            focus_spatial(dir, 0);
        }
        return true;
    }
    if (key == KEY_ENTER || key == KEY_SPACE) {
        if (repeat) return true;
        if (g_focus_cur != F_NONE) {
            g_key_activate = g_focus_cur;
            Graphics::MarkUIDirty();
            Graphics::RequestFrameAt(NowMs() + 1);
        }
        return true;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_popup == POP_WIFI && g_wifi_pass_focus && g_wifi_pass_len > 0) {
            g_wifi_pass_len--;
            g_wifi_pass[g_wifi_pass_len] = 0;
            Graphics::MarkUIDirty();
        }
        return true;
    }
    if (key == KEY_HOME || key == KEY_END) {
        if (focus_is_slider(g_focus_cur)) {
            int dir = key == KEY_END ? 20 : -20;
            ensure_layout();
            if (g_focus_cur >= F_BRIGHT && g_focus_cur < F_BRIGHT + OUTPUT_MAX) {
                int idx = g_focus_cur - F_BRIGHT;
                if (idx < g_L.bright_count) {
                    int slot = g_L.bright_slot[idx];
                    set_brightness(slot, key == KEY_END ? 100 : 10);
                    persist_brightness(slot);
                }
            } else if (g_focus_cur == F_VOLUME) {
                apply_volume(key == KEY_END ? 100 : 0);
                persist_volume();
            }
            (void)dir;
            Graphics::MarkUIDirty();
        }
        return true;
    }
    return true;
}

bool ControlCenter::HandleChar(char c) {
    if (!open) return false;
    if (g_popup == POP_WIFI && g_wifi_pass_focus && c >= 32 && c < 127 && g_wifi_pass_len < (int)sizeof(g_wifi_pass) - 1) {
        g_wifi_pass[g_wifi_pass_len++] = c;
        g_wifi_pass[g_wifi_pass_len] = 0;
        Graphics::MarkUIDirty();
    }
    return true;
}

static int slider_pct_from_mouse(const Rect& track, int mx) {
    if (track.w <= 0) return 0;
    int rel = mx - track.x;
    int pct = (rel * 100) / track.w;
    return clampi(pct, 0, 100);
}
static void update_drag(int mx) {
    ensure_layout();
    if (g_drag_slider >= F_BRIGHT && g_drag_slider < F_BRIGHT + OUTPUT_MAX) {
        int idx = g_drag_slider - F_BRIGHT;
        if (idx < g_L.bright_count) {
            int slot = g_L.bright_slot[idx];
            set_brightness(slot, slider_pct_from_mouse(g_L.bright_slider[idx], mx));
        }
    } else if (g_drag_slider == F_VOLUME) {
        int pct = slider_pct_from_mouse(g_L.vol_slider, mx);
        apply_volume(pct);
    }
}

// drawing ------------------------------------------------------------------
static void draw_header(const Theme& t, int a) {
    (void)a;
    int x = panel_x + PAD;
    int y = panel_y + 14;
    txt(x, y, 21.0f, "Control Center", t.text, true);
    txt(x + 1, y + 27, 12.0f, "Quick settings and system", t.dim);

    char hm[16];
    int h = Taskbar::ClockHour24(), m = Taskbar::ClockMinute();
    hm[0] = (char)('0' + (h / 10) % 10);
    hm[1] = (char)('0' + h % 10);
    hm[2] = ':';
    hm[3] = (char)('0' + (m / 10) % 10);
    hm[4] = (char)('0' + m % 10);
    hm[5] = 0;
    txt_right(panel_x + panel_w - PAD, y - 2, 20.0f, hm, t.text, true);
    char dt[24];
    intstr(Taskbar::ClockMon(), dt, sizeof(dt));
    sapp(dt, "/", sizeof(dt));
    char day[8]; intstr(Taskbar::ClockDom(), day, sizeof(day));
    sapp(dt, day, sizeof(dt));
    txt_right(panel_x + panel_w - PAD, y + 27, 12.0f, dt, t.dim);
}

static void draw_wifi_popup(const Theme& t, int a) {
    const Layout& L = g_L;
    MDraw::Shadow(L.popup.x, L.popup.y + 4, L.popup.w, L.popup.h, 14, 12, 6, 120);
    MDraw::RoundRect(L.popup.x, L.popup.y, L.popup.w, L.popup.h, 14, t.panel, t.panel2, 246);
    MDraw::RoundOutline(L.popup.x, L.popup.y, L.popup.w, L.popup.h, 14, t.line, 30);
    txt(L.popup.x + 14, L.popup.y + 12, 15.0f, "Wi-Fi", t.text, true);
    bool wifi_present = WiFi::DetectedLink() == WiFi::LINK_WIFI;
    bool on = wifi_present && WiFi::GetState() != WIFI_OFF && !g_airplane;
    Switch(L.popup_toggle.x, L.popup_toggle.y, on, t, a, 0x77);

    if (g_wifi_secure_wait) {
        char line[96];
        scpy(line, "Password for ", sizeof(line));
        sapp(line, g_wifi_ssid, sizeof(line));
        txt(L.popup.x + 14, L.popup.y + 44, 13.0f, line, t.text);
        Rect f = L.popup_pass;
        MDraw::RoundRect(f.x, f.y, f.w, f.h, 8, t.field, 240);
        MDraw::RoundOutline(f.x, f.y, f.w, f.h, 8, t.line, 26);
        int dots = g_wifi_pass_len > 22 ? 22 : g_wifi_pass_len;
        char pw[28];
        for (int i = 0; i < dots; i++) pw[i] = '*';
        pw[dots] = 0;
        txt(f.x + 9, f.y + 6, 14.0f, dots ? pw : "Password", dots ? t.text : t.dim);
        if (g_focus_visible && g_focus_cur == F_POP_PASS)
            MDraw::RoundOutline(f.x - 1, f.y - 1, f.w + 2, f.h + 2, 9, t.accent, 235);
        Rect cn = L.popup_connect, cc = L.popup_cancel;
        bool hcn = cn.hit(Mouse::mx, Mouse::my);
        MDraw::RoundRect(cn.x, cn.y, cn.w, cn.h, 8, hcn ? t.accent : mixc(t.card, t.accent, 60), 235);
        txt_center(cn.x + cn.w / 2, cn.y + 6, 13.0f, "Connect", t.on_accent, true);
        MDraw::RoundRect(cc.x, cc.y, cc.w, cc.h, 8, t.field, 220);
        txt_center(cc.x + cc.w / 2, cc.y + 6, 13.0f, "Cancel", t.text);
        return;
    }

    int y = L.popup.y + 44;
    if (!wifi_present) {
        txt(L.popup.x + 14, y + 2, 13.0f, "No Wi-Fi adapter", t.text);
        txt(L.popup.x + 14, y + 20, 12.0f, "The wired network is available below.", t.dim);
    } else if (!on) {
        txt(L.popup.x + 14, y + 2, 13.0f, "Wi-Fi is off", t.text);
        txt(L.popup.x + 14, y + 20, 12.0f, "Turn it on to see nearby networks.", t.dim);
    } else {
        WiFiNetwork* nets = WiFi::GetNetworks();
        int rows = L.popup_row_count;
        if (rows == 0) {
            txt(L.popup.x + 14, y + 2, 13.0f,
                WiFi::GetState() == WIFI_SCANNING ? "Scanning..." : "No networks found", t.dim);
        }
        for (int i = 0; i < rows; i++) {
            Rect r = L.popup_rows[i];
            bool hot = r.hit(Mouse::mx, Mouse::my);
            if (hot) MDraw::RoundRect(r.x, r.y, r.w, r.h, 8, t.card_hi, 220);
            const WiFiNetwork& n = nets[i];
            int bars = n.signal_strength >= -50 ? 4 : n.signal_strength >= -65 ? 3 :
                       n.signal_strength >= -75 ? 2 : 1;
            int bx = r.x + 10, by = r.y + 12;
            for (int b = 0; b < 4; b++) {
                int bh = 4 + b * 3;
                MDraw::RoundRect(bx + b * 5, by + 12 - bh, 3, bh, 1,
                                 b < bars ? t.accent : t.track, b < bars ? 255 : 180);
            }
            char name[48];
            fit_text(name, sizeof(name), 13.0f, n.ssid, r.w - 90, true);
            txt(r.x + 38, r.y + 8, 13.0f, name, t.text, true);
            if (n.connected) {
                DrawIcon(IC_CHECK, r.x + r.w - 52, r.y + 6, 18, t.success, t.card_hi, 255);
            } else if (n.security != WIFI_OPEN) {
                DrawIcon(IC_LOCK, r.x + r.w - 30, r.y + 7, 16, t.dim, t.card_hi, 255);
            }
        }
        if (L.popup_scan.w > 0) {
            bool hs = L.popup_scan.hit(Mouse::mx, Mouse::my);
            MDraw::RoundRect(L.popup_scan.x, L.popup_scan.y, L.popup_scan.w, L.popup_scan.h, 8,
                             hs ? t.card_hi : t.field, 235);
            txt_center(L.popup_scan.x + L.popup_scan.w / 2, L.popup_scan.y + 6, 12.5f, "Scan", t.text, true);
        }
    }
}

static void draw_audio_popup(const Theme& t, int a) {
    const Layout& L = g_L;
    MDraw::Shadow(L.popup.x, L.popup.y + 4, L.popup.w, L.popup.h, 14, 12, 6, 120);
    MDraw::RoundRect(L.popup.x, L.popup.y, L.popup.w, L.popup.h, 14, t.panel, t.panel2, 246);
    MDraw::RoundOutline(L.popup.x, L.popup.y, L.popup.w, L.popup.h, 14, t.line, 30);
    txt(L.popup.x + 14, L.popup.y + 10, 15.0f, "Output device", t.text, true);
    const char* active = AudioServer::ActiveBackendName();
    for (int i = 0; i < L.popup_row_count; i++) {
        Rect r = L.popup_rows[i];
        AudioBackend* be = AudioServer::BackendAt(i);
        if (!be) continue;
        bool hot = r.hit(Mouse::mx, Mouse::my);
        bool sel = active && be->Name()[0] && starts_with(active, be->Name()) && slen(active) == slen(be->Name());
        if (hot) MDraw::RoundRect(r.x, r.y, r.w, r.h, 8, t.card_hi, 220);
        DrawIcon(IC_SPEAKER, r.x + 9, r.y + 6, 18, sel ? t.accent : t.dim, t.card_hi, a);
        char nm[48];
        fit_text(nm, sizeof(nm), 13.0f, audio_name(be->Name()), r.w - 90, sel);
        txt(r.x + 34, r.y + 6, 13.0f, nm, sel ? t.accent : t.text, sel);
        if (sel) DrawIcon(IC_CHECK, r.x + r.w - 28, r.y + 6, 18, t.accent, t.card_hi, 255);
    }
    // recovery actions: reopen the live output, or end every stream. both are
    // the same calls the shell `audio` command and /dev/kaudio use. (satoru)
    {
        Rect rr = L.popup_restart, rs = L.popup_stop;
        bool hr = rr.hit(Mouse::mx, Mouse::my), hs = rs.hit(Mouse::mx, Mouse::my);
        MDraw::RoundRect(rr.x, rr.y, rr.w, rr.h, 8, hr ? t.card_hi : t.field, 240);
        MDraw::RoundRect(rs.x, rs.y, rs.w, rs.h, 8, hs ? t.card_hi : t.field, 240);
        txt_center(rr.x + rr.w / 2, rr.y + 7, 12.5f, "Restart audio", t.text, true);
        txt_center(rs.x + rs.w / 2, rs.y + 7, 12.5f, "Stop all", t.text, true);
    }
}

static void draw_confirm(const Theme& t, int a) {
    (void)a;
    const Layout& L = g_L;
    MDraw::RoundRect(panel_x + 2, panel_y + 2, panel_w - 4, panel_h - 4, 18, 0xFF000000u, 140);
    MDraw::Shadow(L.popup.x, L.popup.y + 5, L.popup.w, L.popup.h, 16, 16, 8, 150);
    MDraw::RoundRect(L.popup.x, L.popup.y, L.popup.w, L.popup.h, 16, t.panel, t.panel2, 252);
    MDraw::RoundOutline(L.popup.x, L.popup.y, L.popup.w, L.popup.h, 16, t.line, 30);
    const char* title = g_confirm_action == PA_RESTART ? "Restart this system?"
                      : g_confirm_action == PA_SHUTDOWN ? "Shut down this system?"
                      : "Put this system to sleep?";
    const char* body = g_confirm_action == PA_RESTART
        ? "Open apps close. Unsaved work is lost."
        : g_confirm_action == PA_SHUTDOWN
        ? "The system powers off. Unsaved work is lost."
        : "Uses firmware S3 sleep when available.";
    txt(L.popup.x + 22, L.popup.y + 22, 17.0f, title, t.text, true);
    txt(L.popup.x + 22, L.popup.y + 52, 12.5f, body, t.dim);
    Rect cn = L.confirm_no, cy = L.confirm_yes;
    bool hcn = cn.hit(Mouse::mx, Mouse::my), hcy = cy.hit(Mouse::mx, Mouse::my);
    MDraw::RoundRect(cn.x, cn.y, cn.w, cn.h, 10, hcn ? t.card_hi : t.field, 240);
    txt_center(cn.x + cn.w / 2, cn.y + 9, 13.5f, "Cancel", t.text, true);
    MDraw::RoundRect(cy.x, cy.y, cy.w, cy.h, 10,
                     g_confirm_action == PA_SHUTDOWN ? (hcy ? t.danger : mixc(t.danger, t.card, 60))
                                                     : (hcy ? t.accent : mixc(t.accent, t.card, 60)), 245);
    const char* ok = g_confirm_action == PA_RESTART ? "Restart"
                   : g_confirm_action == PA_SHUTDOWN ? "Shut Down" : "Sleep";
    txt_center(cy.x + cy.w / 2, cy.y + 9, 13.5f, ok, g_confirm_action == PA_SHUTDOWN ? 0xFFFFFFFFu : t.on_accent, true);
    if (g_focus_visible) {
        const FocusItem* f = FindFocus(g_focus_cur);
        if (f) MDraw::RoundOutline(f->r.x - 2, f->r.y - 2, f->r.w + 4, f->r.h + 4, 12, t.accent, 235);
    }
}

static void draw_left_column(const Theme& t, int a) {
    const Layout& L = g_L;
    static const int tile_ids[6] = { F_FOCUS, F_DARK, F_NIGHT, F_SHOT, F_REC, F_PERF };
    static const IconKind tile_icons[6] = { IC_FOCUS, IC_MOON, IC_NIGHT, IC_CAMERA, IC_RECORD, IC_PULSE };

    bool wifi_present = WiFi::DetectedLink() == WiFi::LINK_WIFI;
    bool wifi_on = wifi_present && WiFi::GetState() != WIFI_OFF && !g_airplane;
    char ws[64];
    if (wifi_present) {
        WiFiNetwork* n = WiFi::GetConnectedNetwork();
        if (!wifi_on) scpy(ws, "Off", sizeof(ws));
        else if (WiFi::GetState() == WIFI_CONNECTED && n && n->ssid[0]) scpy(ws, n->ssid, sizeof(ws));
        else scpy(ws, WiFi::StateString(), sizeof(ws));
        if (wifi_on && WiFi::GetState() != WIFI_CONNECTED) {
            sapp(ws, " - tap to choose", sizeof(ws));
        }
    } else {
        scpy(ws, WiFi::DetectedLink() == WiFi::LINK_ETHERNET ? "No wireless adapter" : "No adapter", sizeof(ws));
    }
    DrawRowTile(F_WIFI, L.wifi, IC_WIFI, "Wi-Fi", ws, wifi_on, true, false, t, a,
                g_focus_visible && g_focus_cur == F_WIFI);

    refresh_bt();
    if (g_bt_present) {
        char bs[64];
        if (g_bt_on) {
            scpy(bs, "On", sizeof(bs));
            if (g_bt_name[0]) { sapp(bs, " - ", sizeof(bs)); sapp(bs, g_bt_name, sizeof(bs)); }
        } else scpy(bs, "Off", sizeof(bs));
        DrawRowTile(F_BT, L.bt, IC_BLUETOOTH, "Bluetooth", bs, g_bt_on, false, true, t, a,
                    g_focus_visible && g_focus_cur == F_BT);
    }

    NetworkInterface* eth = Network::GetInterface("eth0");
    NetworkInterface* wlan = Network::GetInterface("wlan0");
    bool eth_up = eth && eth->state == NIC_UP;
    bool wlan_up = wlan && wlan->state == NIC_UP;
    char ns[80];
    if (wlan_up && wlan) {
        char ip[32]; Network::IPToString(wlan->ip, ip, sizeof(ip));
        scpy(ns, WiFi::StateString(), sizeof(ns)); sapp(ns, " - ", sizeof(ns)); sapp(ns, ip, sizeof(ns));
    } else if (eth_up && eth) {
        char ip[32]; Network::IPToString(eth->ip, ip, sizeof(ip));
        scpy(ns, "Ethernet - ", sizeof(ns)); sapp(ns, ip, sizeof(ns));
    } else {
        scpy(ns, WiFi::DetectedLink() == WiFi::LINK_ETHERNET ? "Waiting for link" : "Offline",
              sizeof(ns));
    }
    DrawRowTile(F_NET, L.net, eth_up && !wlan_up ? IC_ETHERNET : IC_WIFI, "Network", ns,
                eth_up || wlan_up, true, false, t, a, g_focus_visible && g_focus_cur == F_NET);

    bool states[6];
    states[0] = g_focus_mode;
    states[1] = t.dark;
    g_night_light = NightLight::IsOn();   // the schedule may have flipped it (satoru)
    states[2] = g_night_light;
    states[3] = false;
    states[4] = g_recording;
    states[5] = PerfHUD::IsVisible();
    static const char* labels[6] = { "Focus", "Dark Mode", "Night Light", "Screenshot", "Record", "Performance" };
    char subs[6][48];
    if (states[0] && NotificationManager::HeldCount() > 0) {
        char n[16]; intstr(NotificationManager::HeldCount(), n, sizeof(n));
        scpy(subs[0], "On, ", 48); sapp(subs[0], n, 48); sapp(subs[0], " held", 48);
    } else {
        scpy(subs[0], states[0] ? "Do not disturb" : "Notifications on", 48);
    }
    scpy(subs[1], states[1] ? "Dark appearance" : "Light appearance", 48);
    if (NightLight::ScheduleOn()) {
        int mm = states[2] ? NightLight::ToMinutes() : NightLight::FromMinutes();
        char hm[8] = "00:00";
        hm[0] = (char)('0' + (mm / 60) / 10); hm[1] = (char)('0' + (mm / 60) % 10);
        hm[3] = (char)('0' + (mm % 60) / 10); hm[4] = (char)('0' + (mm % 60) % 10);
        scpy(subs[2], states[2] ? "On until " : "From ", 48);
        sapp(subs[2], hm, 48);
    } else
        scpy(subs[2], states[2] ? "Warm tint" : "Standard color", 48);
    scpy(subs[3], "Save a frame", 48);
    if (g_recording) {
        char n[16]; intstr(g_rec_frames, n, sizeof(n));
        scpy(subs[4], "REC ", 48); sapp(subs[4], n, 48); sapp(subs[4], " frames", 48);
    } else scpy(subs[4], "Screen capture", 48);
    scpy(subs[5], states[5] ? "Overlay on" : "Overlay off", 48);
    for (int i = 0; i < 6; i++) {
        DrawTile(tile_ids[i], L.tiles[i], tile_icons[i], labels[i], subs[i], states[i], t, a,
                 g_focus_visible && g_focus_cur == tile_ids[i]);
    }
    DrawRowTile(F_AIR, L.airplane, IC_AIRPLANE, "Airplane mode",
                g_airplane ? "Wi-Fi and Bluetooth off" : "Radios on", g_airplane, false, true, t, a,
                g_focus_visible && g_focus_cur == F_AIR);
}

static void draw_right_column(const Theme& t, int a) {
    const Layout& L = g_L;

    // brightness
    DrawCard(L.bright_card, t, a);
    char bval[32];
    intstr(L.bright_count, bval, sizeof(bval));
    sapp(bval, L.bright_count == 1 ? " display" : " displays", sizeof(bval));
    DrawCardTitle(L.bright_card, IC_MONITOR, "Display", bval, t, a);
    for (int i = 0; i < L.bright_count; i++) {
        int slot = L.bright_slot[i];
        int pct = get_brightness(slot);
        char label[32];
        const Output* o = Outputs::Get(slot);
        if (o && o->name[0]) scpy(label, o->name, sizeof(label));
        else { scpy(label, "Display ", sizeof(label)); char n[8]; intstr(slot + 1, n, sizeof(n)); sapp(label, n, sizeof(label)); }
        char val[16]; intstr(pct, val, sizeof(val)); sapp(val, "%", sizeof(val));
        Rect row = { L.bright_slider[i].x, L.bright_slider[i].y, L.bright_slider[i].w, 28 };
        SliderRow(row, IC_SUN, label, pct, val, t, a,
                  g_focus_visible && g_focus_cur == F_BRIGHT + i);
    }

    // volume
    DrawCard(L.vol_card, t, a);
    AudioServer::ServerStatus st = AudioServer::GetStatus();
    DrawCardTitle(L.vol_card, IC_SPEAKER, "Sound",
                  st.backend_name ? audio_name(st.backend_name) : "No output", t, a);
    int vol = AudioMixer::GetMasterVolume();
    bool muted = AudioMixer::GetMasterMute();
    char vv[16];
    if (muted) scpy(vv, "Muted", sizeof(vv));
    else { intstr(vol, vv, sizeof(vv)); sapp(vv, "%", sizeof(vv)); }
    Rect vrow = { L.vol_slider.x, L.vol_slider.y, L.vol_slider.w, 28 };
    SliderRow(vrow, muted ? IC_MUTE : IC_SPEAKER, "Volume", muted ? 0 : vol, vv, t, a,
              g_focus_visible && g_focus_cur == F_VOLUME);
    Rect mb = L.mute_btn;
    bool hmb = mb.hit(Mouse::mx, Mouse::my);
    MDraw::RoundRect(mb.x, mb.y, mb.w, mb.h, 8, hmb ? t.card_hi : t.field, 220);
    DrawIcon(muted ? IC_MUTE : IC_SPEAKER, mb.x + 4, mb.y + 4, 18,
             muted ? t.danger : t.text, t.field, a);
    if (g_focus_visible && g_focus_cur == F_MUTE)
        MDraw::RoundOutline(mb.x - 1, mb.y - 1, mb.w + 2, mb.h + 2, 9, t.accent, 230);
    Rect ob = L.out_btn;
    bool hob = ob.hit(Mouse::mx, Mouse::my);
    MDraw::RoundRect(ob.x, ob.y, ob.w, ob.h, 7, hob ? t.card_hi : t.field, 210);
    char oname[48];
    const char* an = st.backend_name ? audio_name(st.backend_name) : "No output device";
    scpy(oname, "Output: ", sizeof(oname)); sapp(oname, an, sizeof(oname));
    fit_text(oname, sizeof(oname), 11.5f, oname, ob.w - 30);
    txt(ob.x + 8, ob.y + 4, 11.5f, oname, t.text);
    DrawIcon(IC_CHEVRON, ob.x + ob.w - 20, ob.y + 1, 16, t.dim, t.field, a);
    if (g_focus_visible && g_focus_cur == F_OUTPUT)
        MDraw::RoundOutline(ob.x - 1, ob.y - 1, ob.w + 2, ob.h + 2, 8, t.accent, 230);

    // now playing
    DrawCard(L.media_card, t, a);
    DrawCardTitle(L.media_card, IC_PLAY, "Now Playing", "Media Player", t, a);
    bool mp = MediaPlayerApp::NowPlayingActive();
    const char* title = mp ? MediaPlayerApp::NowPlayingTitle() : "";
    char tbuf[64];
    fit_text(tbuf, sizeof(tbuf), 14.5f, mp && title[0] ? title : "Nothing playing",
             L.media_card.w - 28, true);
    txt(L.media_card.x + 14, L.media_card.y + 34, 14.5f, tbuf, mp ? t.text : t.dim, true);
    const char* stt = mp ? (MediaPlayerApp::IsPlayingNow() ? "Playing" :
                            (MediaPlayerApp::IsPausedNow() ? "Paused" : "Ready"))
                         : "Open the player to start";
    txt(L.media_card.x + 14, L.media_card.y + 52, 11.5f, stt, t.dim);
    int prog = mp ? MediaPlayerApp::NowPlayingProgress() : 0;
    Rect pb = L.media_bar;
    MDraw::RoundRect(pb.x, pb.y, pb.w, pb.h, 4, t.track, 220);
    if (prog > 0) MDraw::RoundRect(pb.x, pb.y, (pb.w * prog) / 100, pb.h, 4, t.accent, a);

    Rect btns[3] = { L.media_prev, L.media_play, L.media_next };
    IconKind bicons[3] = { IC_PREV, MediaPlayerApp::IsPlayingNow() ? IC_PAUSE : IC_PLAY, IC_NEXT };
    for (int i = 0; i < 3; i++) {
        Rect r = btns[i];
        bool hot = r.hit(Mouse::mx, Mouse::my) && mp;
        MDraw::RoundRect(r.x, r.y, r.w, r.h, 8, i == 1 ? (hot ? t.accent : mixc(t.card, t.accent, 80))
                                                       : (hot ? t.card_hi : t.field), 220);
        uint32_t ink = i == 1 ? (mp ? t.on_accent : t.dim) : (mp ? t.text : t.dim);
        DrawIcon(bicons[i], r.x + 7, r.y + 2, 20, ink, i == 1 ? t.accent : t.field, a);
    }
    if (g_focus_visible) {
        const int ids[3] = { F_MEDIA_PREV, F_MEDIA_PLAY, F_MEDIA_NEXT };
        for (int i = 0; i < 3; i++) if (g_focus_cur == ids[i])
            MDraw::RoundOutline(btns[i].x - 1, btns[i].y - 1, btns[i].w + 2, btns[i].h + 2, 9, t.accent, 230);
    }

    // battery + power mode
    DrawCard(L.power_card, t, a);
    DrawCardTitle(L.power_card, IC_BATTERY, "Battery", "AC power", t, a);
    int bp = battery_percent();
    int bx = L.power_card.x + 16, by = L.power_card.y + 34;
    DrawIcon(IC_BATTERY, bx, by, 34, bp > 20 ? t.success : t.warn, t.card, a);
    char pbuf[16]; intstr(bp, pbuf, sizeof(pbuf)); sapp(pbuf, "%", sizeof(pbuf));
    txt(L.power_card.x + 58, L.power_card.y + 34, 19.0f, pbuf, t.text, true);
    txt(L.power_card.x + 58, L.power_card.y + 56, 11.5f, battery_time_text(), t.dim);
    int mi = power_mode_idx();
    static const char* modes[3] = { "Saver", "Balanced", "Perf" };
    for (int i = 0; i < 3; i++)
        SegButton(L.power_mode[i], modes[i], i == mi, t, a,
                  g_focus_visible && g_focus_cur == F_MODE + i);

    // session buttons
    static const IconKind aic[5] = { IC_GEAR, IC_LOCK, IC_MOON, IC_RESTART, IC_POWER };
    static const char* albl[5] = { "Settings", "Lock", "Sleep", "Restart", "Shut Down" };
    static const int aid[5] = { F_SETTINGS, F_LOCK, F_SLEEP, F_RESTART, F_SHUTDOWN };
    for (int i = 0; i < 5; i++)
        ButtonPill(L.actions[i], aic[i], albl[i], 0, t, a,
                   g_focus_visible && g_focus_cur == aid[i], i == 4);
}

static void draw_recording_badge() {
    if (!g_recording) return;
    int ox = 0, oy = 0, ow = screen_w;
    if (Outputs::Multi()) {
        const Output* o = Outputs::Get(0);
        if (o) { ox = o->x; oy = o->y; ow = o->w; }
    }
    uint32_t now = NowMs();
    int sec = (int)((now - g_rec_start_ms) / 1000u);
    char line[32];
    scpy(line, "REC  ", sizeof(line));
    char n[8]; intstr(sec / 60, n, sizeof(n)); sapp(line, n, sizeof(line)); sapp(line, ":", sizeof(line));
    intstr(sec % 60, n, sizeof(n)); if (sec % 60 < 10) sapp(line, "0", sizeof(line)); sapp(line, n, sizeof(line));
    int w = 110, h = 26;
    int x = ox + ow - w - 14;
    int y = oy + 34;
    MDraw::Shadow(x, y, w, h, 10, 8, 4, 80);
    MDraw::RoundRect(x, y, w, h, 10, 0xFF171820, 0xFF171820, 220);
    MDraw::Circle(x + 17, y + h / 2, 5, 0xFFFF453Au, 255);
    txt(x + 30, y + 4, 12.5f, line, 0xFFFFFFFFu, true);
}

// public api ---------------------------------------------------------------
static bool g_inited = false;

void ControlCenter::Init(int sw, int sh) {
    screen_w = sw;
    screen_h = sh;
    open = false;
    g_popup = POP_NONE;
    g_drag_slider = F_NONE;
    g_recording = false;
    g_layout_valid = false;
    g_spring_init = false;
    WMMotion::SpringSet(g_spring, 0.0f);
    g_last_ms = NowMs();
    if (!g_inited) {
        g_inited = true;
        int prim = primary_brightness_from_config();
        char key[40];
        for (int i = 0; i < OUTPUT_MAX; i++) {
            int v = prim;
            if (i > 0) {
                scpy(key, "display.brightness.", sizeof(key));
                char n[8]; intstr(i, n, sizeof(n)); sapp(key, n, sizeof(key));
                v = UIConfig::Int(key, prim);
            }
            Graphics::SetOutputBrightness(i, clampi(v, 10, 100));
        }
        NightLight::Init();
        g_night_light = NightLight::IsOn();
        g_focus_mode = UIConfig::Int("ui.dnd", 0) != 0;
        NotificationManager::SetDoNotDisturb(g_focus_mode);
        g_airplane = UIConfig::Int("network.airplane", 0) != 0;
        if (g_airplane && WiFi::DetectedLink() == WiFi::LINK_WIFI) WiFi::Disable();
        refresh_bt();
        if (g_bt_present && UIConfig::Int("ui.bluetooth", 0)) apply_bt(true);
    }
}

void ControlCenter::OnScreenResize(int sw, int sh) {
    screen_w = sw;
    screen_h = sh;
    g_layout_valid = false;
}

void ControlCenter::Open() {
    int ox = 0, oy = 0, ow = screen_w > 0 ? screen_w : 1024;
    if (Outputs::Multi()) {
        const Output* o = Outputs::Get(0);
        if (o) { ox = o->x; oy = o->y; ow = o->w; g_out_idx = 0; }
    } else {
        g_out_idx = 0;
    }
    g_anchor_x = ox + ow - 110;
    g_anchor_y = oy + 32;
    if (!open) {
        g_focus_cur = F_NONE;
        g_focus_visible = false;
        g_popup = POP_NONE;
    }
    open = true;
    if (!g_spring_init) { WMMotion::SpringSet(g_spring, 0.0f); g_spring_init = true; }
    g_spring.t = 1.0f;
    g_layout_valid = false;
    Graphics::RequestFrameAt(NowMs() + 1);
    Graphics::MarkUIDirty();
}

void ControlCenter::OpenAt(int anchor_x, int anchor_y) {
    g_anchor_x = anchor_x;
    g_anchor_y = anchor_y;
    g_out_idx = Outputs::Multi() ? Outputs::Nearest(anchor_x, anchor_y) : 0;
    if (!open) {
        g_focus_cur = F_NONE;
        g_focus_visible = false;
        g_popup = POP_NONE;
    }
    open = true;
    if (!g_spring_init) { WMMotion::SpringSet(g_spring, 0.0f); g_spring_init = true; }
    g_spring.t = 1.0f;
    g_layout_valid = false;
    Graphics::RequestFrameAt(NowMs() + 1);
    Graphics::MarkUIDirty();
}

void ControlCenter::ToggleAt(int ax, int ay) {
    if (open) Close();
    else OpenAt(ax, ay);
}

void ControlCenter::Toggle() {
    if (open) Close();
    else Open();
}

void ControlCenter::Close() {
    open = false;
    g_drag_slider = F_NONE;
    g_popup = POP_NONE;
    g_wifi_pass_focus = false;
    if (g_spring_init) g_spring.t = 0.0f;
    Graphics::RequestFrameAt(NowMs() + 1);
    Graphics::MarkUIDirty();
}

bool ControlCenter::IsOpen() { return open; }
bool ControlCenter::IsAnimating() {
    if (!g_spring_init) return false;
    return !WMMotion::SpringRest(g_spring, 0.012f);
}
int ControlCenter::GetX() { return panel_x; }
int ControlCenter::GetY() { return panel_y; }
int ControlCenter::GetW() { return panel_w; }
int ControlCenter::GetH() { return panel_h; }

void ControlCenter::Tick() {
    uint32_t now = NowMs();
    uint32_t dt = (g_last_ms == 0) ? 16u : (now - g_last_ms);
    if (dt > 50u) dt = 50u;
    g_last_ms = now;
    if (!g_spring_init) { WMMotion::SpringSet(g_spring, 0.0f); g_spring_init = true; }
    WMMotion::SpringStep(g_spring, 0.28f, 0.82f, (float)dt / 1000.0f);

    if (g_recording) {
        record_capture_if_due();
        Graphics::RequestFrameAt(now + 500u);
    }
    if (open) {
        uint32_t next = now + 1000u;
        if (MediaPlayerApp::IsPlayingNow()) next = now + 500u;
        Graphics::RequestFrameAt(next);
    }
    if (g_key_adjust != F_NONE) {
        int id = g_key_adjust, dir = g_key_adjust_dir;
        g_key_adjust = F_NONE; g_key_adjust_dir = 0;
        adjust_focus(id, dir);
    }
    if (g_key_activate != F_NONE) {
        int id = g_key_activate;
        g_key_activate = F_NONE;
        activate(id);
    }
}

void ControlCenter::Render() {
    Tick();
    if (!open && WMMotion::SpringRest(g_spring, 0.012f)) {
        draw_recording_badge();
        return;
    }
    int tq = (int)(g_spring.x * 255.0f + 0.5f);
    if (tq <= 1) { draw_recording_badge(); return; }
    if (tq > 255) tq = 255;

    build_layout();
    Theme t = ThemeNow();
    Layout& L = g_L;

    // the panel draws through the direct-buffer helpers, which mark nothing:
    // under the damage scissor no frame ever covered it, so it opened as the
    // first animation step only and never repainted. mark its box (shadow
    // included) every time it is drawn; closing is repainted by the overlay
    // carry. (satoru)
    if (Graphics::ScissorEnabled()) Graphics::MarkDirty(L.panel.x - 48, L.panel.y - 32, L.panel.w + 96, L.panel.h + 96);

    WMMotion::SoftShadow((float)L.panel.x, (float)L.panel.y + 8.0f,
                         (float)L.panel.w, (float)L.panel.h, 18.0f, 22.0f, 10.0f,
                         (t.dark ? 150 : 90) * tq / 255, false);
    MDraw::RoundRect(L.panel.x, L.panel.y, L.panel.w, L.panel.h, 18,
                     t.panel, t.panel2, 246 * tq / 255);
    MDraw::RoundOutline(L.panel.x, L.panel.y, L.panel.w, L.panel.h, 18, t.line,
                        (t.dark ? 36 : 22) * tq / 255);

    int ca = (tq - 30) * 255 / 225;
    if (ca < 0) ca = 0;
    if (ca > 255) ca = 255;
    Theme dt = t;
    if (ca < 255) {
        dt.text = fade(t.text, t, ca);
        dt.dim = fade(t.dim, t, ca);
        dt.accent = fade(t.accent, t, ca);
        dt.on_accent = fade(t.on_accent, t, ca);
        dt.danger = fade(t.danger, t, ca);
        dt.success = fade(t.success, t, ca);
        dt.warn = fade(t.warn, t, ca);
    }
    if (ca > 12) {
        draw_header(dt, ca);
        draw_left_column(dt, ca);
        draw_right_column(dt, ca);
        if (g_popup == POP_WIFI) draw_wifi_popup(dt, ca);
        else if (g_popup == POP_AUDIO) draw_audio_popup(dt, ca);
        else if (g_popup == POP_CONFIRM) draw_confirm(dt, ca);
        if (g_popup != POP_CONFIRM) DrawFocusRings(dt, ca);
    }
    if (!open && tq > 1) draw_recording_badge();
}

bool ControlCenter::HandleClick(int mx, int my) {
    if (!open) return false;
    build_layout();
    Layout& L = g_L;
    g_focus_visible = false;

    if (!L.panel.hit(mx, my)) {
        Close();
        return true;
    }

    if (g_popup == POP_CONFIRM) {
        if (L.confirm_no.hit(mx, my)) { g_popup = POP_NONE; g_confirm_action = PA_NONE; g_layout_valid = false; }
        else if (L.confirm_yes.hit(mx, my)) {
            int act = g_confirm_action;
            g_popup = POP_NONE;
            g_confirm_action = PA_NONE;
            g_layout_valid = false;
            execute_power_action(act);
        }
        Graphics::MarkUIDirty();
        return true;
    }

    if (g_popup == POP_WIFI) {
        if (!L.popup.hit(mx, my)) {
            g_popup = POP_NONE;
            g_wifi_pass_focus = false;
            g_layout_valid = false;
            Graphics::MarkUIDirty();
            return true;
        }
        if (L.popup_toggle.hit(mx, my)) { activate(F_POP_TOGGLE); return true; }
        if (g_wifi_secure_wait) {
            if (L.popup_pass.hit(mx, my)) {
                g_wifi_pass_focus = true;
                g_focus_cur = F_POP_PASS;
                g_focus_visible = true;
                Graphics::MarkUIDirty();
                return true;
            }
            if (L.popup_connect.hit(mx, my)) { activate(F_POP_CONNECT); return true; }
            if (L.popup_cancel.hit(mx, my))  { activate(F_POP_CANCEL); return true; }
        } else if (L.popup_scan.hit(mx, my)) {
            activate(F_POP_SCAN);
            return true;
        }
        for (int i = 0; i < L.popup_row_count; i++) {
            if (L.popup_rows[i].hit(mx, my)) { activate(F_POP_ROW + i); return true; }
        }
        return true;
    }

    if (g_popup == POP_AUDIO) {
        if (!L.popup.hit(mx, my)) {
            g_popup = POP_NONE;
            g_layout_valid = false;
            Graphics::MarkUIDirty();
            return true;
        }
        for (int i = 0; i < L.popup_row_count; i++) {
            if (L.popup_rows[i].hit(mx, my)) { activate(F_AUDIO_ROW + i); return true; }
        }
        if (L.popup_restart.hit(mx, my)) { activate(F_AUDIO_RESTART); return true; }
        if (L.popup_stop.hit(mx, my))    { activate(F_AUDIO_STOP); return true; }
        return true;
    }

    if (L.wifi.hit(mx, my))      { activate(F_WIFI); return true; }
    if (g_bt_present && L.bt.hit(mx, my)) { activate(F_BT); return true; }
    if (L.net.hit(mx, my))       { activate(F_NET); return true; }
    if (L.airplane.hit(mx, my))  { activate(F_AIR); return true; }

    static const int tile_ids[6] = { F_FOCUS, F_DARK, F_NIGHT, F_SHOT, F_REC, F_PERF };
    for (int i = 0; i < 6; i++) {
        if (L.tiles[i].hit(mx, my)) { activate(tile_ids[i]); return true; }
    }

    for (int i = 0; i < L.bright_count; i++) {
        if (L.bright_slider[i].hit(mx, my)) {
            g_drag_slider = F_BRIGHT + i;
            g_focus_cur = F_BRIGHT + i;
            update_drag(mx);
            Graphics::MarkUIDirty();
            return true;
        }
    }
    if (L.vol_slider.hit(mx, my)) {
        g_drag_slider = F_VOLUME;
        g_focus_cur = F_VOLUME;
        update_drag(mx);
        Graphics::MarkUIDirty();
        return true;
    }
    if (L.mute_btn.hit(mx, my))  { activate(F_MUTE); return true; }
    if (L.out_btn.hit(mx, my))   { activate(F_OUTPUT); return true; }
    if (L.media_prev.hit(mx, my)) { activate(F_MEDIA_PREV); return true; }
    if (L.media_play.hit(mx, my)) { activate(F_MEDIA_PLAY); return true; }
    if (L.media_next.hit(mx, my)) { activate(F_MEDIA_NEXT); return true; }
    for (int i = 0; i < 3; i++) {
        if (L.power_mode[i].hit(mx, my)) { activate(F_MODE + i); return true; }
    }
    static const int action_ids[5] = { F_SETTINGS, F_LOCK, F_SLEEP, F_RESTART, F_SHUTDOWN };
    for (int i = 0; i < 5; i++) {
        if (L.actions[i].hit(mx, my)) { activate(action_ids[i]); return true; }
    }
    return true;
}

void ControlCenter::HandleMouse(int mx, int my, bool down) {
    (void)my;
    if (!open) return;
    if (!down) {
        if (g_drag_slider >= F_BRIGHT && g_drag_slider < F_BRIGHT + OUTPUT_MAX) {
            int idx = g_drag_slider - F_BRIGHT;
            if (idx < g_L.bright_count) persist_brightness(g_L.bright_slot[idx]);
        } else if (g_drag_slider == F_VOLUME) {
            persist_volume();
        }
        g_drag_slider = F_NONE;
        return;
    }
    if (g_drag_slider == F_NONE) return;
    update_drag(mx);
    Graphics::MarkUIDirty();
}

// end (satoru)
