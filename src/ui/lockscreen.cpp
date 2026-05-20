// Kurono LockScreen - modern login + multi-step registration wizard.
// All inputs from the spec implemented end-to-end:
//   * blurred wallpaper background
//   * large centered clock + seconds + full date
//   * circular profile picture with white ring + soft drop shadow
//   * frosted-glass pill password input with show/hide toggle
//   * full-width accent sign-in button
//   * "Switch user" link if multiple users exist
//   * fade-in on appear, shake on wrong password, fade-out on success
//   * 4-step registration wizard with validation, avatar grid, password
//     strength meter, prefs (timezone/language/accent/auto-login), summary

#include "lockscreen.h"
#include "../drivers/graphics.h"
#include "../drivers/rtc.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "font.h"
#include "../kernel/time.h"
#include "gui.h"
#include "ui_elements.h"
#include "../system/user_mgmt.h"
#include "../drivers/serial.h"
#include "../system/input_manager.h"
#include "../fs/kvfs.h"

LockScreen::State LockScreen::current_state = LockScreen::IDLE;

// ---------------------------------------------------- shared frame state
static int g_w = 0, g_h = 0;
static float g_fade = 0.0f;             // 0..1
static int   g_shake_t = 0;             // ms remaining
static bool  g_running = true;
static bool  g_show_password = false;
static int   g_active_user_idx = 0;
static int   g_login_pw_len = 0;
static char  g_login_pw[64] = {0};
static char  g_login_user[32] = {0};
static int   g_login_user_len = 0;
static bool  g_pw_focused = true;
static int   g_blink_t = 0;
static bool  g_blink_on = true;
static int   g_step_anim_x = 0;         // wizard slide-in offset
static const uint32_t WIZ_ANIM_MS = 240;

// ---------------------------------------------------- registration state
struct RegState {
    char username[32];
    char display_name[48];
    char password[64];
    char confirm[64];
    char pin[16];
    int  avatar_id;
    uint32_t accent;
    int  timezone_idx;
    int  language_idx;
    bool auto_login;
    int  active_field;  // depends on step
    int  user_len;
    int  display_len;
    int  pw_len;
    int  cf_len;
    int  pin_len;
};
static RegState g_reg = {};

// 8 default avatar accent palette (used as background tints for avatar circles)
static const uint32_t AVATAR_COLORS[8] = {
    0xFF5C8AFF, 0xFFE74C3C, 0xFF1ABC9C, 0xFFF1C40F,
    0xFF9B59B6, 0xFFE67E22, 0xFF2ECC71, 0xFF34495E
};
static const uint32_t ACCENTS[6] = {
    0xFF5C8AFF, 0xFFE74C3C, 0xFF1ABC9C, 0xFFF1C40F, 0xFF9B59B6, 0xFF2ECC71
};
static const char* TIMEZONES[6] = {
    "UTC", "America/Los_Angeles", "America/New_York", "Europe/London", "Asia/Tokyo", "Australia/Sydney"
};
static const char* LANGUAGES[5] = { "en_US", "en_GB", "ja_JP", "fr_FR", "de_DE" };
static const char* DOW[7] = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char* MON[12] = { "January","February","March","April","May","June","July","August","September","October","November","December" };

// ---------------------------------------------------- small string utils
static int slen(const char* s){ int n=0; while (s && s[n]) n++; return n; }
static void scpy(char* d, const char* s, int max){
    int i=0; if (!d||max<1) return;
    while (s && s[i] && i<max-1){ d[i]=s[i]; i++; }
    d[i]=0;
}
static void int2(int n, char* out){ out[0]=(char)('0'+(n/10)%10); out[1]=(char)('0'+n%10); out[2]=0; }
static void itoa_pos(int n, char* out){
    if (n<=0){ out[0]='0'; out[1]=0; return; }
    char tmp[12]; int i=0; while (n>0){ tmp[i++]=(char)('0'+n%10); n/=10; }
    int o=0; while (i>0) out[o++]=tmp[--i]; out[o]=0;
}

// ---------------------------------------------------- background
static void draw_background(){
    // Backbuffer already holds wallpaper; we add a dim overlay so UI pops.
    Graphics::FillRectAlpha(0, 0, g_w, g_h, 140, 0x000A0A14);
    const int bands = 14;
    int band_h = (g_h + bands - 1) / bands;
    for (int i = 0; i < bands; i++){
        int y = i * band_h;
        int alpha = 18 + ((bands - i) * 32) / bands;
        uint32_t tint = (i < bands / 2) ? 0x0016223D : 0x00110F1F;
        Graphics::FillRectAlpha(0, y, g_w, band_h, alpha, tint);
    }
    Graphics::FillRectAlpha(0, 0, g_w, g_h/3, 18, 0x00203B72);
    Graphics::FillRectAlpha(0, g_h - 180, g_w, 180, 54, 0x00060A16);
}

static void draw_glass_panel(int x, int y, int w, int h, uint32_t accent){
    Graphics::FillRoundedRect(x + 8, y + 12, w, h, 24, 0x24000000);
    Graphics::FillRoundedRect(x + 3, y + 5, w, h, 24, 0x38000000);
    Graphics::FillRoundedRect(x, y, w, h, 24, 0xB8141828);
    Graphics::FillRectAlpha(x, y, w, h / 2, 42, 0x00283C66);
    Graphics::FillRoundedRect(x + 16, y + 16, w - 32, 8, 4, accent);
    Graphics::FillRectAlpha(x + 1, y + 1, w - 2, 2, 54, 0x00FFFFFF);
}

// circular profile picture with white ring + soft drop shadow
static void draw_avatar(int cx, int cy, int radius, int avatar_id, const char* initials){
    // soft drop shadow (3 expanding circles, dim alpha)
    for (int s = 0; s < 3; s++){
        Graphics::FillRectAlpha(cx-radius-6+s, cy-radius-2+s, (radius+6-s)*2, (radius+6-s)*2, 30, 0x00000000);
    }
    // white ring
    Graphics::FillCircle(cx, cy, radius+3, 0xFFFFFFFF);
    // colored disk
    uint32_t bg = (avatar_id >= 0 && avatar_id < 8) ? AVATAR_COLORS[avatar_id] : 0xFF5C8AFF;
    Graphics::FillCircle(cx, cy, radius, bg);
    // initials in white
    if (initials && initials[0]){
        char ini[3]; ini[0] = initials[0];
        ini[1] = (initials[1] && initials[1] != ' ') ? initials[1] : (char)0;
        ini[2] = 0;
        if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 32);
        if (ini[1] >= 'a' && ini[1] <= 'z') ini[1] = (char)(ini[1] - 32);
        // Visual centering: TTF y param is the baseline. For most fonts ascent ≈ 0.75*fs,
        // descent ≈ 0.20*fs. Visual center of caps is roughly cy with baseline at
        // cy + 0.27*fs.  Use 0.27 to crisp-center initials inside the disc.
        float fs = (float)radius * 0.85f;
        int tw = FontTTF::Measure(fs, ini);
        FontTTF::DrawString(cx - tw/2, cy + (int)(fs*0.27f), fs, ini, 0xFFFFFFFF);
    }
}

// frosted-glass pill input
static void draw_pill(int x, int y, int w, int h, const char* text, int text_len, bool focused, bool is_password, const char* placeholder){
    // shadow
    Graphics::FillRoundedRect(x+2, y+3, w, h, h/2, 0x40000000);
    // frosted background
    Graphics::FillRoundedRect(x, y, w, h, h/2, 0xCC1F2030);
    // border ring when focused
    if (focused){
        for (int i=0;i<2;i++)
            Graphics::FillRoundedRect(x-i, y-i, w+i*2, h+i*2, h/2+i, 0x00000000); // no-op outline placeholder
        // simulate outline by drawing thin rect edges
        Graphics::FillRect(x, y-1, w, 1, 0xFF5C8AFF);
        Graphics::FillRect(x, y+h, w, 1, 0xFF5C8AFF);
        Graphics::FillRect(x-1, y, 1, h, 0xFF5C8AFF);
        Graphics::FillRect(x+w, y, 1, h, 0xFF5C8AFF);
    }
    // text or placeholder
    float fs = (float)h * 0.45f;
    int pad = h/2;
    if (text_len > 0){
        if (is_password){
            char dots[64];
            int n = text_len < 60 ? text_len : 60;
            for (int i=0;i<n;i++) dots[i] = '*';
            dots[n] = 0;
            FontTTF::DrawString(x + pad, y + (int)(fs*1.1f), fs, dots, 0xFFEEEEFF);
        } else {
            FontTTF::DrawString(x + pad, y + (int)(fs*1.1f), fs, text, 0xFFEEEEFF);
        }
    } else if (placeholder){
        FontTTF::DrawString(x + pad, y + (int)(fs*1.1f), fs, placeholder, 0xFF8090A0);
    }
    // blinking cursor
    if (focused && g_blink_on){
        int tw = is_password ? text_len * (int)(fs*0.55f) : FontTTF::Measure(fs, text);
        int cx = x + pad + tw + 2;
        Graphics::FillRect(cx, y + pad/2, 2, h - pad, 0xFFFFFFFF);
    }
}

// show/hide eye toggle (returns hit area as out params for click detection)
static void draw_eye(int cx, int cy, bool shown){
    // outer eye outline (oval done as two circles)
    Graphics::FillCircle(cx, cy, 9, 0xFFFFFFFF);
    Graphics::FillCircle(cx, cy, 7, 0xFF1F2030);
    if (shown){
        Graphics::FillCircle(cx, cy, 4, 0xFFEEEEFF);
        Graphics::FillCircle(cx, cy, 2, 0xFF1F2030);
    } else {
        // slash
        for (int i = -8; i <= 8; i++){
            Graphics::FillRect(cx + i, cy - i, 2, 2, 0xFFFFFFFF);
        }
    }
}

// full-width accent button  -  auto-shrinks label to stay inside the pill
static bool draw_accent_button(int x, int y, int w, int h, const char* label, uint32_t accent, bool hover){
    uint32_t fill = hover ? 0xFF7AA1FF : accent;
    Graphics::FillRoundedRect(x+2, y+3, w, h, h/2, 0x60000000);
    Graphics::FillRoundedRect(x, y, w, h, h/2, fill);
    Graphics::FillRectAlpha(x + 2, y + 2, w - 4, h / 2, 36, 0x00FFFFFF);
    // start at button height * 0.42 then shrink until label fits inside
    // the rounded pill (we leave h/2 padding on each side for the radius).
    float fs = (float)h * 0.42f;
    int max_label_w = w - h; // h/2 padding either side ≈ pill radius
    if (max_label_w < 16) max_label_w = w - 8;
    int tw = FontTTF::Measure(fs, label);
    while (tw > max_label_w && fs > 9.0f){
        fs -= 1.0f;
        tw = FontTTF::Measure(fs, label);
    }
    // vertical-center the glyph (baseline ≈ y + h/2 + fs*0.30)
    int bx = x + w/2 - tw/2;
    int by = y + h/2 + (int)(fs * 0.30f);
    FontTTF::DrawString(bx, by, fs, label, 0xFFFFFFFF);
    int mx, my; Mouse::GetPosition(mx, my);
    return mx >= x && mx < x+w && my >= y && my < y+h;
}

// ---------------------------------------------------- IDLE: clock + tap
static void draw_idle(){
    draw_background();

    RTC::Date dr; RTC::Time tr; RTC::ReadDateTime(dr, tr);
    int cx = g_w/2;

    // big HH:MM
    char hm[6]; int2(tr.h, hm); hm[2]=':'; int2(tr.m, hm+3); hm[5]=0;
    float fs_big = (float)g_h * 0.18f;
    if (fs_big > 240.0f) fs_big = 240.0f;
    int tw = FontTTF::Measure(fs_big, hm);
    int ty = g_h/2 - (int)(fs_big*0.6f);
    FontTTF::DrawString(cx - tw/2, ty, fs_big, hm, 0xFFFFFFFF);

    // seconds smaller
    char ss[3]; int2(tr.s, ss);
    float fs_sec = fs_big * 0.30f;
    int sw = FontTTF::Measure(fs_sec, ss);
    FontTTF::DrawString(cx - sw/2, ty + (int)(fs_big*0.6f) + 4, fs_sec, ss, 0xC0FFFFFF);

    // full date "Tuesday, April 28 2026"
    char date[64]; int p = 0;
    const char* dn = (dr.dow >= 1 && dr.dow <= 7) ? DOW[dr.dow-1] : "Day";
    const char* mn = (dr.mon >= 1 && dr.mon <= 12) ? MON[dr.mon-1] : "Mon";
    while (*dn && p < 60) date[p++] = *dn++;
    date[p++] = ','; date[p++] = ' ';
    while (*mn && p < 60) date[p++] = *mn++;
    date[p++] = ' ';
    char dom[4]; itoa_pos(dr.dom, dom);
    for (int i=0;dom[i];i++) date[p++] = dom[i];
    date[p++] = ' ';
    char yr[8]; itoa_pos((int)dr.year, yr);
    for (int i=0;yr[i];i++) date[p++] = yr[i];
    date[p] = 0;
    float fs_date = fs_big * 0.20f;
    int dw = FontTTF::Measure(fs_date, date);
    FontTTF::DrawString(cx - dw/2, ty + (int)(fs_big*0.6f) + (int)(fs_sec*1.4f) + 18, fs_date, date, 0xCCFFFFFF);

    // hint at bottom
    const char* hint = (UserManager::GetUserCount() > 0) ? "Press any key or click to sign in" : "Press any key or click to set up your first user";
    float fs_hint = 16.0f;
    int hw = FontTTF::Measure(fs_hint, hint);
    FontTTF::DrawString(cx - hw/2, g_h - 60, fs_hint, hint, 0x80FFFFFF);
}

// ---------------------------------------------------- LOGIN
struct LoginHit {
    int eye_cx, eye_cy;
    int btn_x, btn_y, btn_w, btn_h;
    int switch_x, switch_y, switch_w, switch_h;
    int pw_x, pw_y, pw_w, pw_h;
    int user_x, user_y, user_w, user_h;
};
static LoginHit g_login_hit;

static void draw_login(int shake_offset_x){
    draw_background();

    int cx = g_w/2 + shake_offset_x;
    int cy = g_h/2;

    // figure active user
    if (g_active_user_idx < 0) g_active_user_idx = 0;
    if (g_active_user_idx >= UserManager::GetUserCount()) g_active_user_idx = 0;
    User* u = (UserManager::GetUserCount() > 0) ? &UserManager::users[g_active_user_idx] : nullptr;

    // big clock at the top
    RTC::Time tr; RTC::ReadDateTime(*(RTC::Date*)__builtin_alloca(sizeof(RTC::Date)), tr);
    char hm[6]; int2(tr.h, hm); hm[2]=':'; int2(tr.m, hm+3); hm[5]=0;
    float fs_clk = 80.0f;
    int twc = FontTTF::Measure(fs_clk, hm);
    FontTTF::DrawString(cx - twc/2, 90, fs_clk, hm, 0xFFFFFFFF);

    // avatar
    const char* dispn = u ? (u->display_name[0] ? u->display_name : u->username) : "New User";
    int avatar_id = u ? u->avatar_id : 0;
    int avatar_radius = 56;
    int avatar_cy = cy - 110;
    draw_glass_panel(cx - 230, avatar_cy - 90, 460, 332, u ? (u->accent_color ? u->accent_color : 0xFF5C8AFF) : 0xFF5C8AFF);
    char init[3] = { dispn[0], 0, 0 };
    draw_avatar(cx, avatar_cy, avatar_radius, avatar_id, init);

    // username
    float fs_name = 26.0f;
    int nw = FontTTF::Measure(fs_name, dispn);
    FontTTF::DrawString(cx - nw/2, avatar_cy + avatar_radius + 36, fs_name, dispn, 0xFFFFFFFF);

    // username field (only if no users yet OR more than one  -  let user type)
    int field_w = 320;
    int field_h = 44;
    int fy = avatar_cy + avatar_radius + 70;

    if (UserManager::GetUserCount() == 0){
        g_login_hit.user_x = cx - field_w/2; g_login_hit.user_y = fy;
        g_login_hit.user_w = field_w; g_login_hit.user_h = field_h;
        draw_pill(g_login_hit.user_x, g_login_hit.user_y, field_w, field_h, g_login_user, g_login_user_len, !g_pw_focused, false, "Username");
        fy += field_h + 12;
    } else {
        scpy(g_login_user, u->username, 32);
        g_login_user_len = slen(g_login_user);
    }

    // password pill with eye
    g_login_hit.pw_x = cx - field_w/2; g_login_hit.pw_y = fy;
    g_login_hit.pw_w = field_w; g_login_hit.pw_h = field_h;
    draw_pill(g_login_hit.pw_x, g_login_hit.pw_y, field_w, field_h, g_login_pw, g_login_pw_len, g_pw_focused, !g_show_password, "Password");
    g_login_hit.eye_cx = g_login_hit.pw_x + field_w - field_h/2 - 4;
    g_login_hit.eye_cy = g_login_hit.pw_y + field_h/2;
    draw_eye(g_login_hit.eye_cx, g_login_hit.eye_cy, g_show_password);

    // sign in button full-width
    g_login_hit.btn_x = cx - field_w/2;
    g_login_hit.btn_y = fy + field_h + 14;
    g_login_hit.btn_w = field_w;
    g_login_hit.btn_h = 48;
    int mx, my; Mouse::GetPosition(mx, my);
    bool hover = mx >= g_login_hit.btn_x && mx < g_login_hit.btn_x+g_login_hit.btn_w
              && my >= g_login_hit.btn_y && my < g_login_hit.btn_y+g_login_hit.btn_h;
    uint32_t accent = u ? u->accent_color : 0xFF5C8AFF;
    if (accent == 0) accent = 0xFF5C8AFF;
    draw_accent_button(g_login_hit.btn_x, g_login_hit.btn_y, g_login_hit.btn_w, g_login_hit.btn_h, "Sign In", accent, hover);

    // switch user link
    if (UserManager::GetUserCount() > 1){
        const char* sw = "Switch User";
        float fs_sw = 14.0f;
        int sww = FontTTF::Measure(fs_sw, sw);
        g_login_hit.switch_x = cx - sww/2;
        g_login_hit.switch_y = g_login_hit.btn_y + g_login_hit.btn_h + 18;
        g_login_hit.switch_w = sww;
        g_login_hit.switch_h = 18;
        FontTTF::DrawString(g_login_hit.switch_x, g_login_hit.switch_y + 14, fs_sw, sw, 0xC0FFFFFF);
    } else {
        g_login_hit.switch_w = 0;
    }

    // create account link if zero or more users
    const char* ca = (UserManager::GetUserCount() == 0) ? "" : "Create new account";
    if (ca[0]){
        float fs_ca = 13.0f;
        int caw = FontTTF::Measure(fs_ca, ca);
        FontTTF::DrawString(cx - caw/2, g_h - 36, fs_ca, ca, 0xA0FFFFFF);
    }

    // fade overlay
    if (g_fade < 1.0f){
        uint8_t a = (uint8_t)((1.0f - g_fade) * 220.0f);
        Graphics::FillRectAlpha(0, 0, g_w, g_h, a, 0x00000000);
    }
}

// ---------------------------------------------------- WIZARD HELPERS
static void wiz_reset(){
    for (int i=0;i<(int)sizeof(RegState);i++) ((char*)&g_reg)[i] = 0;
    g_reg.avatar_id = 0;
    g_reg.accent = ACCENTS[0];
    g_reg.timezone_idx = 0;
    g_reg.language_idx = 0;
    g_reg.auto_login = false;
    g_reg.active_field = 0;
}

static void draw_step_indicator(int step){
    int cx = g_w/2;
    int top = 80;
    int total = 4;
    int dot_r = 7;
    int gap = 56;
    int total_w = (total - 1) * gap;
    int x0 = cx - total_w/2;
    int line_y = top + dot_r;
    // connecting line between first and last dot centers
    Graphics::FillRect(x0, line_y - 1, total_w, 2, 0x40FFFFFF);
    if (step > 0)
        Graphics::FillRect(x0, line_y - 1, step * gap, 2, 0xFF5C8AFF);
    for (int i = 0; i < total; i++){
        int dot_cx = x0 + i * gap;
        uint32_t c = (i <= step) ? 0xFF5C8AFF : 0x60FFFFFF;
        Graphics::FillCircle(dot_cx, line_y, dot_r, c);
        if (i < step)
            Graphics::FillCircle(dot_cx, line_y, dot_r - 3, 0xFFFFFFFF);
    }
    const char* names[4] = { "Profile", "Security", "Preferences", "Summary" };
    float fs = 15.0f;
    int tw = FontTTF::Measure(fs, names[step]);
    FontTTF::DrawString(cx - tw/2, top + dot_r * 2 + 18, fs, names[step], 0xFFFFFFFF);
    const char* hint = (step == 0 || step == 1)
        ? "Tab switches fields \xE2\x80\xA2 Enter continues \xE2\x80\xA2 Esc back"
        : "Enter continues \xE2\x80\xA2 Esc back";
    float fs_hint = 11.0f;
    int hw = FontTTF::Measure(fs_hint, hint);
    FontTTF::DrawString(cx - hw/2, top + dot_r * 2 + 38, fs_hint, hint, 0x90FFFFFF);
}

// click hit boxes for wizard
struct WizHit {
    int next_x, next_y, next_w, next_h;
    int back_x, back_y, back_w, back_h;
    // step1
    int avatar_box_x[8], avatar_box_y[8];
    int avatar_box_size;
    int display_x, display_y, display_w, display_h;
    int user_x, user_y, user_w, user_h;
    // step2
    int pw_x, pw_y, pw_w, pw_h;
    int cf_x, cf_y, cf_w, cf_h;
    int pin_x, pin_y, pin_w, pin_h;
    int eye_cx, eye_cy;
    // step3
    int tz_x[6], tz_y[6], tz_w, tz_h;
    int lang_x[5], lang_y[5], lang_w, lang_h;
    int accent_x[6], accent_y[6], accent_size;
    int auto_x, auto_y, auto_w, auto_h;
    // step4 - edit links
    int edit_x[3], edit_y[3], edit_w, edit_h;
};
static WizHit g_wiz;

// clamp header text to panel width, reducing font size if needed
static void draw_panel_header(int cx, int y, int panel_w, const char* text, float fs_max){
    float fs = fs_max;
    int max_tw = panel_w - 48;
    int tw = FontTTF::Measure(fs, text);
    while (tw > max_tw && fs > 14.0f){ fs -= 1.0f; tw = FontTTF::Measure(fs, text); }
    FontTTF::DrawString(cx - tw/2, y, fs, text, 0xFFFFFFFF);
}

// --------- step 1: profile (avatar + display name + username)
static void draw_wizard_step1(int slide_dx){
    draw_background();
    draw_step_indicator(0);
    int cx = g_w/2 + slide_dx;
    int panel_w = 520;
    int panel_x = cx - panel_w/2;
    int panel_y = 140;
    int panel_h = 460;
    draw_glass_panel(panel_x, panel_y, panel_w, panel_h, g_reg.accent ? g_reg.accent : 0xFF5C8AFF);

    int top = panel_y + 36;
    draw_panel_header(cx, top, panel_w, "Choose your avatar and name", 26.0f);

    // avatar grid 4 x 2
    int ay = top + 48;
    int sz = 58;
    int gap = 16;
    int row_w = 4 * sz + 3 * gap;
    int x0 = cx - row_w/2;
    g_wiz.avatar_box_size = sz;
    for (int i=0;i<8;i++){
        int row = i/4, col = i%4;
        int ax = x0 + col * (sz + gap);
        int ayy = ay + row * (sz + gap);
        g_wiz.avatar_box_x[i] = ax;
        g_wiz.avatar_box_y[i] = ayy;
        if (i == g_reg.avatar_id){
            Graphics::FillCircle(ax + sz/2, ayy + sz/2, sz/2 + 4, 0xFFFFFFFF);
        }
        Graphics::FillCircle(ax + sz/2, ayy + sz/2, sz/2, AVATAR_COLORS[i]);
        char ini[2] = { (char)('A'+i), 0 };
        float fs = (float)sz * 0.5f;
        int tw = FontTTF::Measure(fs, ini);
        FontTTF::DrawString(ax + sz/2 - tw/2, ayy + sz/2 + (int)(fs*0.27f), fs, ini, 0xFFFFFFFF);
    }

    // display name input
    int field_w = panel_w - 80;
    int field_h = 40;
    int fy = ay + 2 * (sz + gap) + 24;
    g_wiz.display_x = cx - field_w/2; g_wiz.display_y = fy;
    g_wiz.display_w = field_w; g_wiz.display_h = field_h;
    draw_pill(g_wiz.display_x, fy, field_w, field_h, g_reg.display_name, g_reg.display_len,
              g_reg.active_field == 0, false, "Display name");

    fy += field_h + 12;
    g_wiz.user_x = cx - field_w/2; g_wiz.user_y = fy;
    g_wiz.user_w = field_w; g_wiz.user_h = field_h;
    draw_pill(g_wiz.user_x, fy, field_w, field_h, g_reg.username, g_reg.user_len,
              g_reg.active_field == 1, false, "Username (lowercase, 3-31)");

    // inline validation  -  clamped to panel bounds
    if (g_reg.user_len > 0){
        const char* err = nullptr;
        if (!UserManager::IsUsernameValid(g_reg.username)) err = "Invalid: letters, digits, _, - only";
        else if (UserManager::IsUsernameTaken(g_reg.username)) err = "Username already taken";
        if (err){
            float fs_e = 12.0f;
            int ew = FontTTF::Measure(fs_e, err);
            int max_ew = field_w;
            while (ew > max_ew && fs_e > 9.0f){ fs_e -= 0.5f; ew = FontTTF::Measure(fs_e, err); }
            FontTTF::DrawString(cx - ew/2, fy + field_h + 10, fs_e, err, 0xFFE74C3C);
        }
    }

    // next/back  -  inside panel bottom area
    int btn_y = panel_y + panel_h - 60;
    g_wiz.next_x = cx + 60; g_wiz.next_y = btn_y; g_wiz.next_w = 130; g_wiz.next_h = 42;
    int mx,my; Mouse::GetPosition(mx,my);
    bool hover = mx>=g_wiz.next_x && mx<g_wiz.next_x+g_wiz.next_w && my>=g_wiz.next_y && my<g_wiz.next_y+g_wiz.next_h;
    draw_accent_button(g_wiz.next_x, g_wiz.next_y, g_wiz.next_w, g_wiz.next_h, "Next", 0xFF5C8AFF, hover);

    g_wiz.back_w = 0;

    if (g_fade < 1.0f){
        uint8_t a = (uint8_t)((1.0f - g_fade) * 220.0f);
        Graphics::FillRectAlpha(0, 0, g_w, g_h, a, 0x00000000);
    }
}

// --------- step 2: security
static void draw_wizard_step2(int slide_dx){
    draw_background();
    draw_step_indicator(1);
    int cx = g_w/2 + slide_dx;
    int panel_w = 520;
    int panel_x = cx - panel_w/2;
    int panel_y = 140;
    int panel_h = 440;
    draw_glass_panel(panel_x, panel_y, panel_w, panel_h, g_reg.accent ? g_reg.accent : 0xFF5C8AFF);

    int top = panel_y + 36;
    draw_panel_header(cx, top, panel_w, "Set up a password", 26.0f);

    int field_w = panel_w - 80;
    int field_h = 44;
    int fy = top + 56;

    g_wiz.pw_x = cx - field_w/2; g_wiz.pw_y = fy;
    g_wiz.pw_w = field_w; g_wiz.pw_h = field_h;
    draw_pill(g_wiz.pw_x, fy, field_w, field_h, g_reg.password, g_reg.pw_len,
              g_reg.active_field == 0, !g_show_password, "Password");
    g_wiz.eye_cx = g_wiz.pw_x + field_w - field_h/2 - 4;
    g_wiz.eye_cy = g_wiz.pw_y + field_h/2;
    draw_eye(g_wiz.eye_cx, g_wiz.eye_cy, g_show_password);

    // strength meter
    int strength = UserManager::MeasurePassword(g_reg.password);
    const char* labels[4] = { "Weak", "Fair", "Strong", "Very Strong" };
    uint32_t scols[4] = { 0xFFE74C3C, 0xFFE67E22, 0xFFF1C40F, 0xFF2ECC71 };
    int mw = field_w;
    int my2 = fy + field_h + 12;
    Graphics::FillRoundedRect(g_wiz.pw_x, my2, mw, 6, 3, 0x40FFFFFF);
    int filled = (strength + 1) * (mw / 4);
    if (g_reg.pw_len == 0) filled = 0;
    Graphics::FillRoundedRect(g_wiz.pw_x, my2, filled, 6, 3, scols[strength]);
    if (g_reg.pw_len > 0){
        FontTTF::DrawString(g_wiz.pw_x, my2 + 20, 12.0f, labels[strength], scols[strength]);
    }

    // confirm
    fy = my2 + 46;
    g_wiz.cf_x = cx - field_w/2; g_wiz.cf_y = fy;
    g_wiz.cf_w = field_w; g_wiz.cf_h = field_h;
    draw_pill(g_wiz.cf_x, fy, field_w, field_h, g_reg.confirm, g_reg.cf_len,
              g_reg.active_field == 1, !g_show_password, "Confirm password");
    if (g_reg.cf_len > 0){
        bool match = (g_reg.cf_len == g_reg.pw_len);
        if (match) for (int i=0;i<g_reg.cf_len;i++) if (g_reg.confirm[i] != g_reg.password[i]) { match=false; break; }
        if (!match){
            FontTTF::DrawString(g_wiz.cf_x, fy + field_h + 10, 12.0f, "Passwords do not match", 0xFFE74C3C);
        }
    }

    // PIN (optional)
    fy += field_h + 28;
    FontTTF::DrawString(cx - field_w/2, fy, 13.0f, "Optional PIN for quick login:", 0xC0FFFFFF);
    g_wiz.pin_x = cx - field_w/2; g_wiz.pin_y = fy + 20;
    g_wiz.pin_w = 120; g_wiz.pin_h = field_h;
    draw_pill(g_wiz.pin_x, g_wiz.pin_y, g_wiz.pin_w, g_wiz.pin_h, g_reg.pin, g_reg.pin_len,
              g_reg.active_field == 2, true, "PIN");

    // nav  -  inside panel bottom area
    int btn_y = panel_y + panel_h - 60;
    g_wiz.next_x = cx + 60; g_wiz.next_y = btn_y; g_wiz.next_w = 130; g_wiz.next_h = 42;
    g_wiz.back_x = cx - 190; g_wiz.back_y = btn_y; g_wiz.back_w = 130; g_wiz.back_h = 42;
    int mx,my; Mouse::GetPosition(mx,my);
    bool hovn = mx>=g_wiz.next_x && mx<g_wiz.next_x+g_wiz.next_w && my>=g_wiz.next_y && my<g_wiz.next_y+g_wiz.next_h;
    bool hovb = mx>=g_wiz.back_x && mx<g_wiz.back_x+g_wiz.back_w && my>=g_wiz.back_y && my<g_wiz.back_y+g_wiz.back_h;
    draw_accent_button(g_wiz.back_x, g_wiz.back_y, g_wiz.back_w, g_wiz.back_h, "Back", 0xFF555570, hovb);
    draw_accent_button(g_wiz.next_x, g_wiz.next_y, g_wiz.next_w, g_wiz.next_h, "Next", 0xFF5C8AFF, hovn);
}

// --------- step 3: preferences
static void draw_wizard_step3(int slide_dx){
    draw_background();
    draw_step_indicator(2);
    int cx = g_w/2 + slide_dx;
    int panel_w = 580;
    int panel_x = cx - panel_w/2;
    int panel_y = 140;
    int panel_h = 470;
    draw_glass_panel(panel_x, panel_y, panel_w, panel_h, g_reg.accent ? g_reg.accent : 0xFF5C8AFF);

    int top = panel_y + 36;
    draw_panel_header(cx, top, panel_w, "Personalize your experience", 26.0f);

    // accent color row
    int content_w = panel_w - 60;
    int rx = panel_x + 30;
    int sw = 36;
    int gap = 12;
    int ry = top + 54;
    FontTTF::DrawString(rx, ry - 6, 13.0f, "Accent color:", 0xFFFFFFFF);
    ry += 16;
    g_wiz.accent_size = sw;
    for (int i=0;i<6;i++){
        int x = rx + i * (sw + gap);
        g_wiz.accent_x[i] = x; g_wiz.accent_y[i] = ry;
        if (g_reg.accent == ACCENTS[i]){
            Graphics::FillCircle(x + sw/2, ry + sw/2, sw/2 + 4, 0xFFFFFFFF);
        }
        Graphics::FillCircle(x + sw/2, ry + sw/2, sw/2, ACCENTS[i]);
    }

    // timezone radio list
    int ty = ry + sw + 26;
    FontTTF::DrawString(rx, ty, 13.0f, "Timezone:", 0xFFFFFFFF);
    ty += 20;
    g_wiz.tz_w = content_w/2 - 8; g_wiz.tz_h = 24;
    for (int i=0;i<6;i++){
        int row = i/2, col = i%2;
        int x = rx + col * (g_wiz.tz_w + 12);
        int y = ty + row * (g_wiz.tz_h + 5);
        g_wiz.tz_x[i] = x; g_wiz.tz_y[i] = y;
        bool sel = (g_reg.timezone_idx == i);
        Graphics::FillCircle(x + 8, y + g_wiz.tz_h/2, 6, sel ? 0xFF5C8AFF : 0x40FFFFFF);
        if (sel) Graphics::FillCircle(x + 8, y + g_wiz.tz_h/2, 3, 0xFFFFFFFF);
        float tz_fs = 12.0f;
        int tz_tw = FontTTF::Measure(tz_fs, TIMEZONES[i]);
        int tz_max = g_wiz.tz_w - 28;
        while (tz_tw > tz_max && tz_fs > 9.0f){ tz_fs -= 0.5f; tz_tw = FontTTF::Measure(tz_fs, TIMEZONES[i]); }
        FontTTF::DrawString(x + 22, y + 16, tz_fs, TIMEZONES[i], 0xFFEEEEFF);
    }

    // language row
    int ly = ty + 3 * (g_wiz.tz_h + 5) + 10;
    FontTTF::DrawString(rx, ly, 13.0f, "Language:", 0xFFFFFFFF);
    ly += 20;
    g_wiz.lang_w = 94; g_wiz.lang_h = 24;
    for (int i=0;i<5;i++){
        int x = rx + i * (g_wiz.lang_w + 6);
        g_wiz.lang_x[i] = x; g_wiz.lang_y[i] = ly;
        bool sel = (g_reg.language_idx == i);
        Graphics::FillRoundedRect(x, ly, g_wiz.lang_w, g_wiz.lang_h, 12, sel ? 0xFF5C8AFF : 0x301F2030);
        int tw = FontTTF::Measure(11.0f, LANGUAGES[i]);
        FontTTF::DrawString(x + g_wiz.lang_w/2 - tw/2, ly + 16, 11.0f, LANGUAGES[i], 0xFFFFFFFF);
    }

    // auto-login toggle
    int auy = ly + g_wiz.lang_h + 20;
    g_wiz.auto_x = rx; g_wiz.auto_y = auy; g_wiz.auto_w = 48; g_wiz.auto_h = 26;
    uint32_t toc = g_reg.auto_login ? 0xFF2ECC71 : 0x40FFFFFF;
    Graphics::FillRoundedRect(g_wiz.auto_x, g_wiz.auto_y, g_wiz.auto_w, g_wiz.auto_h, 13, toc);
    int knob_x = g_reg.auto_login ? (g_wiz.auto_x + g_wiz.auto_w - g_wiz.auto_h + 2) : (g_wiz.auto_x + 2);
    Graphics::FillCircle(knob_x + g_wiz.auto_h/2 - 2, g_wiz.auto_y + g_wiz.auto_h/2, g_wiz.auto_h/2 - 4, 0xFFFFFFFF);
    FontTTF::DrawString(g_wiz.auto_x + g_wiz.auto_w + 12, auy + 17, 13.0f, "Enable auto-login", 0xFFEEEEFF);

    // nav  -  inside panel bottom area
    int btn_y = panel_y + panel_h - 60;
    g_wiz.next_x = cx + 60; g_wiz.next_y = btn_y; g_wiz.next_w = 130; g_wiz.next_h = 42;
    g_wiz.back_x = cx - 190; g_wiz.back_y = btn_y; g_wiz.back_w = 130; g_wiz.back_h = 42;
    int mx,my; Mouse::GetPosition(mx,my);
    bool hovn = mx>=g_wiz.next_x && mx<g_wiz.next_x+g_wiz.next_w && my>=g_wiz.next_y && my<g_wiz.next_y+g_wiz.next_h;
    bool hovb = mx>=g_wiz.back_x && mx<g_wiz.back_x+g_wiz.back_w && my>=g_wiz.back_y && my<g_wiz.back_y+g_wiz.back_h;
    draw_accent_button(g_wiz.back_x, g_wiz.back_y, g_wiz.back_w, g_wiz.back_h, "Back", 0xFF555570, hovb);
    draw_accent_button(g_wiz.next_x, g_wiz.next_y, g_wiz.next_w, g_wiz.next_h, "Next", g_reg.accent, hovn);
}

// --------- step 4: summary
static void draw_wizard_step4(int slide_dx){
    draw_background();
    draw_step_indicator(3);
    int cx = g_w/2 + slide_dx;
    int outer_w = 540;
    int outer_x = cx - outer_w/2;
    int outer_y = 140;
    int outer_h = 430;
    draw_glass_panel(outer_x, outer_y, outer_w, outer_h, g_reg.accent ? g_reg.accent : 0xFF5C8AFF);

    int top = outer_y + 36;
    draw_panel_header(cx, top, outer_w, "Confirm and create", 26.0f);

    int inner_w = outer_w - 60;
    int inner_x = cx - inner_w/2;
    int py = top + 52;
    Graphics::FillRoundedRect(inner_x, py, inner_w, 260, 14, 0xCC141428);

    // section: profile
    int sy = py + 14;
    draw_avatar(inner_x + 44, sy + 28, 24, g_reg.avatar_id, g_reg.display_name);
    FontTTF::DrawString(inner_x + 86, sy + 20, 15.0f, g_reg.display_name[0]?g_reg.display_name:g_reg.username, 0xFFFFFFFF);
    FontTTF::DrawString(inner_x + 86, sy + 40, 12.0f, g_reg.username, 0xC0FFFFFF);
    g_wiz.edit_w = 46; g_wiz.edit_h = 20;
    g_wiz.edit_x[0] = inner_x + inner_w - 56; g_wiz.edit_y[0] = sy + 24;
    Graphics::FillRoundedRect(g_wiz.edit_x[0], g_wiz.edit_y[0], g_wiz.edit_w, g_wiz.edit_h, 10, 0x801F2030);
    FontTTF::DrawString(g_wiz.edit_x[0] + 10, g_wiz.edit_y[0] + 14, 11.0f, "Edit", 0xFFFFFFFF);

    Graphics::FillRect(inner_x + 14, sy + 62, inner_w - 28, 1, 0x40FFFFFF);

    // section: security
    sy += 74;
    FontTTF::DrawString(inner_x + 20, sy + 14, 13.0f, "Password:", 0xFFFFFFFF);
    FontTTF::DrawString(inner_x + 110, sy + 14, 13.0f, "**********", 0xC0FFFFFF);
    FontTTF::DrawString(inner_x + 20, sy + 32, 13.0f, "PIN:", 0xFFFFFFFF);
    FontTTF::DrawString(inner_x + 110, sy + 32, 13.0f, g_reg.pin_len > 0 ? "Enabled" : "Disabled", 0xC0FFFFFF);
    g_wiz.edit_x[1] = inner_x + inner_w - 56; g_wiz.edit_y[1] = sy + 20;
    Graphics::FillRoundedRect(g_wiz.edit_x[1], g_wiz.edit_y[1], g_wiz.edit_w, g_wiz.edit_h, 10, 0x801F2030);
    FontTTF::DrawString(g_wiz.edit_x[1] + 10, g_wiz.edit_y[1] + 14, 11.0f, "Edit", 0xFFFFFFFF);

    Graphics::FillRect(inner_x + 14, sy + 52, inner_w - 28, 1, 0x40FFFFFF);

    // section: prefs
    sy += 64;
    FontTTF::DrawString(inner_x + 20, sy + 14, 13.0f, "Timezone:", 0xFFFFFFFF);
    float tz_fs = 13.0f;
    int tz_tw = FontTTF::Measure(tz_fs, TIMEZONES[g_reg.timezone_idx]);
    int tz_max = inner_w - 160;
    while (tz_tw > tz_max && tz_fs > 9.0f){ tz_fs -= 0.5f; tz_tw = FontTTF::Measure(tz_fs, TIMEZONES[g_reg.timezone_idx]); }
    FontTTF::DrawString(inner_x + 110, sy + 14, tz_fs, TIMEZONES[g_reg.timezone_idx], 0xC0FFFFFF);
    FontTTF::DrawString(inner_x + 20, sy + 32, 13.0f, "Language:", 0xFFFFFFFF);
    FontTTF::DrawString(inner_x + 110, sy + 32, 13.0f, LANGUAGES[g_reg.language_idx], 0xC0FFFFFF);
    FontTTF::DrawString(inner_x + 20, sy + 50, 13.0f, "Auto-login:", 0xFFFFFFFF);
    FontTTF::DrawString(inner_x + 110, sy + 50, 13.0f, g_reg.auto_login?"On":"Off", 0xC0FFFFFF);
    Graphics::FillCircle(inner_x + inner_w - 24, sy + 34, 8, g_reg.accent);
    g_wiz.edit_x[2] = inner_x + inner_w - 56; g_wiz.edit_y[2] = sy + 48;
    Graphics::FillRoundedRect(g_wiz.edit_x[2], g_wiz.edit_y[2], g_wiz.edit_w, g_wiz.edit_h, 10, 0x801F2030);
    FontTTF::DrawString(g_wiz.edit_x[2] + 10, g_wiz.edit_y[2] + 14, 11.0f, "Edit", 0xFFFFFFFF);

    // confirm  -  inside panel bottom area
    int btn_y = outer_y + outer_h - 60;
    g_wiz.next_x = cx - 85; g_wiz.next_y = btn_y; g_wiz.next_w = 170; g_wiz.next_h = 44;
    g_wiz.back_x = cx - 265; g_wiz.back_y = btn_y; g_wiz.back_w = 130; g_wiz.back_h = 44;
    int mx,my; Mouse::GetPosition(mx,my);
    bool hovn = mx>=g_wiz.next_x && mx<g_wiz.next_x+g_wiz.next_w && my>=g_wiz.next_y && my<g_wiz.next_y+g_wiz.next_h;
    bool hovb = mx>=g_wiz.back_x && mx<g_wiz.back_x+g_wiz.back_w && my>=g_wiz.back_y && my<g_wiz.back_y+g_wiz.back_h;
    draw_accent_button(g_wiz.back_x, g_wiz.back_y, g_wiz.back_w, g_wiz.back_h, "Back", 0xFF555570, hovb);
    draw_accent_button(g_wiz.next_x, g_wiz.next_y, g_wiz.next_w, g_wiz.next_h, "Create Account", g_reg.accent, hovn);
}

// ---------------------------------------------------- INPUT HANDLERS
static void edit_buf(char* buf, int* len, int max, char c){
    if (c == '\b' || c == 127){ if (*len > 0){ (*len)--; buf[*len]=0; } return; }
    if (c < 32 || c > 126) return;
    if (*len < max - 1){ buf[(*len)++] = c; buf[*len] = 0; }
}

static bool point_in(int mx, int my, int x, int y, int w, int h){
    return mx>=x && mx<x+w && my>=y && my<y+h;
}

static bool wizard_can_advance(){
    if (LockScreen::current_state == LockScreen::WIZ_PROFILE){
        return UserManager::IsUsernameValid(g_reg.username) &&
               !UserManager::IsUsernameTaken(g_reg.username) &&
               g_reg.user_len > 0;
    }
    if (LockScreen::current_state == LockScreen::WIZ_SECURITY){
        bool match = (g_reg.pw_len == g_reg.cf_len);
        if (match) {
            for (int i = 0; i < g_reg.pw_len; i++) {
                if (g_reg.password[i] != g_reg.confirm[i]) {
                    match = false;
                    break;
                }
            }
        }
        return match && g_reg.pw_len >= 6;
    }
    return LockScreen::current_state == LockScreen::WIZ_PREFS ||
           LockScreen::current_state == LockScreen::WIZ_SUMMARY;
}

static void wizard_go_back(){
    if (LockScreen::current_state == LockScreen::WIZ_SECURITY) LockScreen::current_state = LockScreen::WIZ_PROFILE;
    else if (LockScreen::current_state == LockScreen::WIZ_PREFS) LockScreen::current_state = LockScreen::WIZ_SECURITY;
    else if (LockScreen::current_state == LockScreen::WIZ_SUMMARY) LockScreen::current_state = LockScreen::WIZ_PREFS;
    else return;
    g_step_anim_x = -200;
    g_fade = 0.0f;
}

static void wizard_go_next(){
    if (!wizard_can_advance()) return;

    if (LockScreen::current_state == LockScreen::WIZ_PROFILE){
        if (g_reg.display_len == 0){
            scpy(g_reg.display_name, g_reg.username, 48);
            g_reg.display_len = g_reg.user_len;
        }
        LockScreen::current_state = LockScreen::WIZ_SECURITY;
        g_reg.active_field = 0;
        g_fade = 0.0f;
        g_step_anim_x = 200;
        return;
    }

    if (LockScreen::current_state == LockScreen::WIZ_SECURITY){
        LockScreen::current_state = LockScreen::WIZ_PREFS;
        g_reg.active_field = 0;
        g_fade = 0.0f;
        g_step_anim_x = 200;
        return;
    }

    if (LockScreen::current_state == LockScreen::WIZ_PREFS){
        LockScreen::current_state = LockScreen::WIZ_SUMMARY;
        g_fade = 0.0f;
        g_step_anim_x = 200;
        return;
    }

    if (LockScreen::current_state == LockScreen::WIZ_SUMMARY){
        User u;
        for (int i = 0; i < (int)sizeof(User); i++) ((char*)&u)[i] = 0;
        scpy(u.username, g_reg.username, 32);
        scpy(u.display_name, g_reg.display_name, 48);
        u.avatar_id = g_reg.avatar_id;
        u.accent_color = g_reg.accent;
        scpy(u.timezone, TIMEZONES[g_reg.timezone_idx], 32);
        scpy(u.language, LANGUAGES[g_reg.language_idx], 16);
        u.auto_login = g_reg.auto_login;
        u.is_admin = (UserManager::GetUserCount() == 0);
        if (g_reg.pin_len > 0){
            char salt[24];
            UserManager::GenerateSalt(salt);
            scpy(u.salt, salt, 24);
            UserManager::HashPassword(salt, g_reg.pin, u.pin_hash);
            u.has_pin = true;
        }
        if (UserManager::RegisterUser(u, g_reg.password)){
            UserManager::Login(g_reg.username, g_reg.password);
            LockScreen::current_state = LockScreen::FADE_OUT;
            g_fade = 1.0f;
        }
    }
}

// ---------------------------------------------------- MAIN LOOP
void LockScreen::Show(){
    Keyboard::FlushBuffers();
    UserManager::Init();
    InputManager::Init();
    g_w = Graphics::GetWidth();
    g_h = Graphics::GetHeight();

    // first boot: no users -> jump straight into the wizard
    if (UserManager::GetUserCount() == 0){
        wiz_reset();
        current_state = WIZ_PROFILE;
    } else {
        current_state = IDLE;
    }
    g_fade = 0.0f;
    g_running = true;
    g_step_anim_x = 0;
    g_login_pw[0] = 0; g_login_pw_len = 0;
    g_login_user[0] = 0; g_login_user_len = 0;
    g_pw_focused = true;

    // ensure wallpaper fully blurred for backdrop
    if (GUI::wallpaper.valid){
        GUI::SetWallpaper(GUI::wallpaper);
        GUI::BlurWallpaper();
    }
    Timer::ElapsedSinceLast();

    int frame_accum = 0;
    while (g_running){
        InputManager::Poll();
        uint32_t dt = Timer::ElapsedSinceLast();
        TimeManager::AdvanceByMs(dt);
        frame_accum += dt;
        g_blink_t += dt;
        if (g_blink_t > 500){ g_blink_on = !g_blink_on; g_blink_t = 0; }

        bool clicked = Mouse::LeftClicked();
        int mx, my; Mouse::GetPosition(mx, my);
        bool enter_pressed = Keyboard::IsKeyPressed(KEY_ENTER);
        bool tab_pressed   = Keyboard::IsKeyPressed(KEY_TAB);
        bool esc_pressed   = Keyboard::IsKeyPressed(KEY_ESC);
        (void)esc_pressed;

        // animate fades
        if (current_state == LOGIN || (current_state >= WIZ_PROFILE && current_state <= WIZ_SUMMARY)){
            if (g_fade < 1.0f){ g_fade += (float)dt / WIZ_ANIM_MS; if (g_fade > 1.0f) g_fade = 1.0f; }
        }
        if (current_state == FADE_OUT){
            g_fade -= (float)dt / 400.0f;
            if (g_fade <= 0.0f){ g_running = false; }
        }
        if (current_state == SHAKE){
            g_shake_t -= (int)dt;
            if (g_shake_t <= 0){ current_state = LOGIN; }
        }

        // ---------- IDLE: any input transitions out
        if (current_state == IDLE){
            if (clicked || enter_pressed || Keyboard::HasChar()){
                if (UserManager::GetUserCount() > 0){
                    current_state = LOGIN;
                    g_fade = 0.0f;
                    g_pw_focused = true;
                } else {
                    wiz_reset();
                    current_state = WIZ_PROFILE;
                    g_fade = 0.0f;
                }
                Keyboard::FlushBuffers();
            }
        }

        // ---------- LOGIN input
        if (current_state == LOGIN){
            // text input
            while (Keyboard::HasChar()){
                char c = Keyboard::GetChar();
                if (c == '\t'){ g_pw_focused = !g_pw_focused; continue; }
                if (c == '\n' || c == '\r'){
                    // try login
                    bool ok = UserManager::Login(g_login_user, g_login_pw);
                    if (ok){
                        current_state = FADE_OUT;
                        g_fade = 1.0f;
                    } else {
                        current_state = SHAKE;
                        g_shake_t = 320;
                        g_login_pw[0] = 0; g_login_pw_len = 0;
                    }
                    continue;
                }
                if (g_pw_focused) edit_buf(g_login_pw, &g_login_pw_len, 64, c);
                else              edit_buf(g_login_user, &g_login_user_len, 32, c);
            }
            // mouse
            if (clicked){
                // eye toggle
                int dx = mx - g_login_hit.eye_cx, dy = my - g_login_hit.eye_cy;
                if (dx*dx + dy*dy < 100) g_show_password = !g_show_password;
                else if (point_in(mx,my,g_login_hit.btn_x,g_login_hit.btn_y,g_login_hit.btn_w,g_login_hit.btn_h)){
                    bool ok = UserManager::Login(g_login_user, g_login_pw);
                    if (ok){ current_state = FADE_OUT; g_fade = 1.0f; }
                    else { current_state = SHAKE; g_shake_t = 320; g_login_pw[0]=0; g_login_pw_len=0; }
                }
                else if (g_login_hit.switch_w > 0
                      && point_in(mx,my,g_login_hit.switch_x,g_login_hit.switch_y,g_login_hit.switch_w,g_login_hit.switch_h)){
                    g_active_user_idx = (g_active_user_idx + 1) % UserManager::GetUserCount();
                    g_login_pw[0] = 0; g_login_pw_len = 0;
                }
                else if (point_in(mx,my,g_login_hit.pw_x,g_login_hit.pw_y,g_login_hit.pw_w,g_login_hit.pw_h)){
                    g_pw_focused = true;
                }
                else if (g_login_hit.user_w > 0
                      && point_in(mx,my,g_login_hit.user_x,g_login_hit.user_y,g_login_hit.user_w,g_login_hit.user_h)){
                    g_pw_focused = false;
                }
            }
        }

        // ---------- WIZARD input
        if (current_state >= WIZ_PROFILE && current_state <= WIZ_SUMMARY){
            if (enter_pressed) wizard_go_next();
            if (esc_pressed) wizard_go_back();
            // text input depending on step + active field
            while (Keyboard::HasChar()){
                char c = Keyboard::GetChar();
                if (c == '\t'){
                    if (current_state == WIZ_PROFILE) g_reg.active_field = (g_reg.active_field + 1) % 2;
                    else if (current_state == WIZ_SECURITY) g_reg.active_field = (g_reg.active_field + 1) % 3;
                    continue;
                }
                if (current_state == WIZ_PROFILE){
                    if (g_reg.active_field == 0) edit_buf(g_reg.display_name, &g_reg.display_len, 48, c);
                    else                          edit_buf(g_reg.username,     &g_reg.user_len,    32, c);
                } else if (current_state == WIZ_SECURITY){
                    if (g_reg.active_field == 0)      edit_buf(g_reg.password, &g_reg.pw_len, 64, c);
                    else if (g_reg.active_field == 1) edit_buf(g_reg.confirm,  &g_reg.cf_len, 64, c);
                    else if (g_reg.active_field == 2 && (c >= '0' && c <= '9') && g_reg.pin_len < 8)
                                                       edit_buf(g_reg.pin,      &g_reg.pin_len, 16, c);
                    else if (g_reg.active_field == 2 && (c == '\b' || c == 127))
                                                       edit_buf(g_reg.pin,      &g_reg.pin_len, 16, c);
                }
            }
            // mouse
            if (clicked){
                // common: next button advances if validation passes
                if (point_in(mx,my,g_wiz.next_x,g_wiz.next_y,g_wiz.next_w,g_wiz.next_h)){
                    wizard_go_next();
                }
                // back
                if (g_wiz.back_w > 0 && point_in(mx,my,g_wiz.back_x,g_wiz.back_y,g_wiz.back_w,g_wiz.back_h)){
                    wizard_go_back();
                }
                // step1: avatar grid + field focus
                if (current_state == WIZ_PROFILE){
                    int sz = g_wiz.avatar_box_size;
                    for (int i=0;i<8;i++) if (point_in(mx,my,g_wiz.avatar_box_x[i],g_wiz.avatar_box_y[i],sz,sz)) g_reg.avatar_id = i;
                    if (point_in(mx,my,g_wiz.display_x,g_wiz.display_y,g_wiz.display_w,g_wiz.display_h)) g_reg.active_field = 0;
                    if (point_in(mx,my,g_wiz.user_x,g_wiz.user_y,g_wiz.user_w,g_wiz.user_h)) g_reg.active_field = 1;
                }
                // step2: field focus + eye
                if (current_state == WIZ_SECURITY){
                    if (point_in(mx,my,g_wiz.pw_x,g_wiz.pw_y,g_wiz.pw_w,g_wiz.pw_h)) g_reg.active_field = 0;
                    if (point_in(mx,my,g_wiz.cf_x,g_wiz.cf_y,g_wiz.cf_w,g_wiz.cf_h)) g_reg.active_field = 1;
                    if (point_in(mx,my,g_wiz.pin_x,g_wiz.pin_y,g_wiz.pin_w,g_wiz.pin_h)) g_reg.active_field = 2;
                    int dx = mx - g_wiz.eye_cx, dy = my - g_wiz.eye_cy;
                    if (dx*dx + dy*dy < 100) g_show_password = !g_show_password;
                }
                // step3: accents, tz, lang, auto-login toggle
                if (current_state == WIZ_PREFS){
                    int sz = g_wiz.accent_size;
                    for (int i=0;i<6;i++) if (point_in(mx,my,g_wiz.accent_x[i],g_wiz.accent_y[i],sz,sz)) g_reg.accent = ACCENTS[i];
                    for (int i=0;i<6;i++) if (point_in(mx,my,g_wiz.tz_x[i],g_wiz.tz_y[i],g_wiz.tz_w,g_wiz.tz_h)) g_reg.timezone_idx = i;
                    for (int i=0;i<5;i++) if (point_in(mx,my,g_wiz.lang_x[i],g_wiz.lang_y[i],g_wiz.lang_w,g_wiz.lang_h)) g_reg.language_idx = i;
                    if (point_in(mx,my,g_wiz.auto_x,g_wiz.auto_y,g_wiz.auto_w,g_wiz.auto_h)) g_reg.auto_login = !g_reg.auto_login;
                }
                // step4: edit jumps to specific step
                if (current_state == WIZ_SUMMARY){
                    if (point_in(mx,my,g_wiz.edit_x[0],g_wiz.edit_y[0],g_wiz.edit_w,g_wiz.edit_h)) current_state = WIZ_PROFILE;
                    if (point_in(mx,my,g_wiz.edit_x[1],g_wiz.edit_y[1],g_wiz.edit_w,g_wiz.edit_h)) current_state = WIZ_SECURITY;
                    if (point_in(mx,my,g_wiz.edit_x[2],g_wiz.edit_y[2],g_wiz.edit_w,g_wiz.edit_h)) current_state = WIZ_PREFS;
                }
            }
            (void)tab_pressed;
        }

        // animate slide
        if (g_step_anim_x > 0){ g_step_anim_x -= (int)dt; if (g_step_anim_x < 0) g_step_anim_x = 0; }
        if (g_step_anim_x < 0){ g_step_anim_x += (int)dt; if (g_step_anim_x > 0) g_step_anim_x = 0; }

        // render at ~60fps
        if (frame_accum >= 16){
            frame_accum = 0;
            // backbuffer with blurred wallpaper
            GUI::UpdateBackbuffer();
            uint8_t* screen = Graphics::GetBuffer();
            if (GUI::backbuffer) Graphics::SetBuffer(GUI::backbuffer);

            int shake = 0;
            if (current_state == SHAKE){
                int phase = (320 - g_shake_t) / 30;
                shake = (phase % 2 == 0) ? -8 : 8;
            }

            switch (current_state){
                case IDLE:        draw_idle(); break;
                case LOGIN:
                case FADE_OUT:    draw_login(0); break;
                case SHAKE:       draw_login(shake); break;
                case FADE_IN:     draw_login(0); break;
                case WIZ_PROFILE: draw_wizard_step1(g_step_anim_x); break;
                case WIZ_SECURITY:draw_wizard_step2(g_step_anim_x); break;
                case WIZ_PREFS:   draw_wizard_step3(g_step_anim_x); break;
                case WIZ_SUMMARY: draw_wizard_step4(g_step_anim_x); break;
            }

            // fade-out overlay
            if (current_state == FADE_OUT){
                uint8_t a = (uint8_t)((1.0f - g_fade) * 255.0f);
                Graphics::FillRectAlpha(0, 0, g_w, g_h, a, 0x00000000);
            }

            if (GUI::backbuffer) Graphics::SetBuffer(screen);
            GUI::DrawDesktop();

            // mouse cursor
            Mouse::DrawAt(mx, my);
            Graphics::SwapBuffers();
        }

        __asm__ __volatile__("pause");
    }

    // Persist user metadata once at successful login (no-op if nothing changed)
    UserManager::PersistToDisk();
}
