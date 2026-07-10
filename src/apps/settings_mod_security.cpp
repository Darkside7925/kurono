//  kurono os - settings module: security & users (satoru)
//  a detailed "Security & Users" page: the current user + role, the full user
//  roster (UserManager), a sign-in section (auto-login toggle that persists
//  security.autologin + a lock-screen timeout slider), the live SUPR security
//  level/status, and the active session count. all persisted writes go through
//  UIConfig; live identity/role/session data is read from UserManager + SUPR.
//  the auto-login toggle records intent in UIConfig - the actual live mechanism
//  is the `kurono.autologin` grub boot flag parsed in kurono_kernel.cpp. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../system/ui_config.h"
#include "../system/user_mgmt.h"
#include "../security/supr.h"

// ── module state (constant-initialised statics - ctor-free) ─────────────────
static bool s_autologin    = false;  // persisted to security.autologin (satoru)
static int  s_lock_timeout = 5;      // minutes, persisted to security.lock_timeout_min (satoru)
static int  s_user_count   = 0;      // UserManager roster size, from on_show (satoru)
static int  s_active_sess  = 0;      // live SUPR sessions with active==true (satoru)

// candidate lock-screen timeouts in minutes (0 == never). the slider picks the
// nearest of these so the readout is always a clean value. (satoru)
static const int kTimeouts[]   = {0, 1, 2, 5, 10, 15, 30, 60};
static const int kTimeoutCount = (int)(sizeof(kTimeouts) / sizeof(kTimeouts[0]));

// ── helpers ─────────────────────────────────────────────────────────────────
// human label for a SUPR privilege level. (satoru)
static const char* supr_level_label(SUPRLevel lvl){
    switch(lvl){
        case SUPR_GUEST: return "Guest";
        case SUPR_USER:  return "Standard User";
        case SUPR_ADMIN: return "Administrator";
        case SUPR_ROOT:  return "Root / Superuser";
        default:         return "Unknown";
    }
}

// count live SUPR sessions flagged active across the session table. (satoru)
static int count_active_sessions(){
    int n = 0;
    for(int i = 0; i < SUPR_MAX_SESSIONS; i++){
        SUPRSession* s = SUPR::GetSession(i);
        if(s && s->active) n++;
    }
    return n;
}

// map a percentage (0..100) from the slider onto the nearest timeout step, and
// return that step's array index. (satoru)
static int timeout_index_for_pct(int pct){
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    // nearest bucket across kTimeoutCount evenly-spaced stops. (satoru)
    int idx = (pct * (kTimeoutCount - 1) + 50) / 100;
    if(idx < 0) idx = 0;
    if(idx >= kTimeoutCount) idx = kTimeoutCount - 1;
    return idx;
}

// inverse: the slider fill percentage for the current timeout value. (satoru)
static int pct_for_timeout(int minutes){
    for(int i = 0; i < kTimeoutCount; i++){
        if(kTimeouts[i] == minutes) return (i * 100) / (kTimeoutCount - 1);
    }
    return 0;
}

static void persist_autologin(){
    UIConfig::SetInt("security.autologin", s_autologin ? 1 : 0, true);
    UIConfig::Save();
}

static void persist_timeout(){
    UIConfig::SetInt("security.lock_timeout_min", s_lock_timeout, true);
    UIConfig::Save();
}

// ── on_show: load persisted config + re-detect live security state ──────────
static void security_on_show(){
    s_autologin = UIConfig::Int("security.autologin", s_autologin ? 1 : 0) != 0;

    int t = UIConfig::Int("security.lock_timeout_min", s_lock_timeout);
    // snap a loaded value to a known bucket so the slider readout is clean. (satoru)
    s_lock_timeout = kTimeouts[timeout_index_for_pct(pct_for_timeout(t))];
    // if it didn't match a bucket, pct_for_timeout returned 0 -> bucket 0; keep
    // the raw value instead so an out-of-band config isn't silently zeroed. (satoru)
    if(s_lock_timeout != t){
        bool known = false;
        for(int i = 0; i < kTimeoutCount; i++) if(kTimeouts[i] == t) known = true;
        if(!known && t >= 0) s_lock_timeout = t;
    }

    s_user_count  = UserManager::GetUserCount();
    if(s_user_count < 0) s_user_count = 0;
    if(s_user_count > UserManager::MAX_USERS) s_user_count = UserManager::MAX_USERS;

    s_active_sess = count_active_sessions();
}

// ── layout constants shared by render + input so hit-testing matches the
//    drawing exactly (the input() signature gives no pane width, so the control
//    geometry is fixed here rather than derived). (satoru)
static const int CTRL_X_OFF = 200;   // controls column, relative to pane left (satoru)
static const int CTRL_W     = 280;   // pill/control width in px (satoru)
static const int SLIDER_W   = 240;   // timeout track width in px (satoru)

// how many user rows we draw (so render + input + height all agree). (satoru)
static int roster_rows(){
    int n = s_user_count;
    if(n < 1) n = 1;                 // always show at least the placeholder line (satoru)
    if(n > UserManager::MAX_USERS) n = UserManager::MAX_USERS;
    return n;
}

static void security_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int ly = y - scroll + 8;
    char buf[64];

    // ── current user ─────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Current User");
    ly += 26;
    {
        const char* disp = UserManager::GetCurrentDisplayName();
        const char* uname = UserManager::GetCurrentUsername();
        SettingsUI::Row(x, ly, "Name:", (disp && disp[0]) ? disp
                                       : ((uname && uname[0]) ? uname : "(none)"));
        ly += 22;
        SettingsUI::Row(x, ly, "Username:", (uname && uname[0]) ? uname : "(none)");
        ly += 22;

        // role: admin flag on the live UserManager record. (satoru)
        int ci = UserManager::GetCurrentUserIndex();
        const char* role = "Standard User";
        if(ci >= 0 && ci < UserManager::MAX_USERS){
            role = UserManager::users[ci].is_admin ? "Administrator" : "Standard User";
        }
        SettingsUI::Row(x, ly, "Role:", role);
        ly += 30;
    }

    // ── users roster ─────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Users");
    ly += 26;
    {
        SettingsUI::IntToStr(s_user_count, buf, 64);
        SettingsUI::StrApp(buf, " account(s) on this device", 64);
        SettingsUI::Row(x, ly, "Registered:", buf);
        ly += 26;

        if(s_user_count <= 0){
            Graphics::DrawString(x + 8, ly + 2, "No user accounts registered.",
                                 SettingsUI::COL_DIM, 0xFF000000);
            ly += SettingsUI::ROW_H;
        } else {
            int n = roster_rows();
            for(int i = 0; i < n; i++){
                const User& u = UserManager::users[i];
                const char* nm = u.display_name[0] ? u.display_name
                                : (u.username[0] ? u.username : "(unnamed)");
                // bullet + name on the left. (satoru)
                Graphics::FillCircle(x + 6, ly + 9, 2, SettingsUI::Accent());
                Graphics::DrawString(x + 16, ly + 2, nm, SettingsUI::COL_TEXT, 0xFF000000);

                // role + login flags on the right column. (satoru)
                SettingsUI::StrCpy(buf, u.is_admin ? "Administrator" : "Standard", 64);
                if(u.auto_login) SettingsUI::StrApp(buf, " · auto", 64);
                if(u.has_pin)    SettingsUI::StrApp(buf, " · pin", 64);
                Graphics::DrawString(ctrl_x, ly + 2, buf, SettingsUI::COL_DIM, 0xFF000000);
                ly += SettingsUI::ROW_H;
            }
        }

        // honest note: creating an account needs a username + password, and this
        // panel has no text-entry field - that flow lives in the Login/Accounts UI.
        // (UserManager::AddUser/RemoveUser exist but both require typed input.) (satoru)
        Graphics::DrawString(x, ly + 4, "Manage:", SettingsUI::COL_TEXT, 0xFF000000);
        Graphics::DrawString(ctrl_x, ly + 4, "Use the Login screen to add accounts",
                             SettingsUI::COL_DIM, 0xFF000000);
        ly += 34;
    }

    // ── sign-in ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Sign-in");
    ly += 26;

    // auto-login toggle. (satoru)
    Graphics::DrawString(x, ly + 2, "Auto Sign-in:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_autologin);
    ly += 28;
    Graphics::DrawString(x + 8, ly, "Live boot flag: kurono.autologin (grub cmdline)",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 26;

    // lock-screen timeout slider with a readout (minutes, or "Never"). (satoru)
    Graphics::DrawString(x, ly + 2, "Lock Timeout:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Slider(ctrl_x, ly, SLIDER_W, pct_for_timeout(s_lock_timeout));
    if(s_lock_timeout <= 0){
        SettingsUI::StrCpy(buf, "Never", 64);
    } else {
        SettingsUI::IntToStr(s_lock_timeout, buf, 64);
        SettingsUI::StrApp(buf, " min", 64);
    }
    Graphics::DrawString(ctrl_x + SLIDER_W + 10, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    ly += 34;

    // ── security level (SUPR) ────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Security Level");
    ly += 26;
    {
        int sid = SUPR::GetCurrentSession();
        if(sid >= 0){
            SettingsUI::Row(x, ly, "Privilege:", supr_level_label(SUPR::GetLevel(sid)));
            ly += 22;
            SettingsUI::Row(x, ly, "Engine:", "SUPR - active session");
            ly += 22;
        } else {
            SettingsUI::Row(x, ly, "Privilege:", "Guest (no SUPR session)");
            ly += 22;
            SettingsUI::Row(x, ly, "Engine:", "SUPR - no active session");
            ly += 22;
        }
        SettingsUI::IntToStr(SUPR::GetUserCount(), buf, 64);
        SettingsUI::StrApp(buf, " SUPR account(s)", 64);
        SettingsUI::Row(x, ly, "Accounts:", buf);
        ly += 30;
    }

    // ── sessions ─────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Sessions");
    ly += 26;
    {
        SettingsUI::IntToStr(s_active_sess, buf, 64);
        SettingsUI::StrApp(buf, " active", 64);
        SettingsUI::Row(x, ly, "Open Sessions:", buf);
        ly += 22;

        SettingsUI::IntToStr(SUPR_MAX_SESSIONS, buf, 64);
        SettingsUI::StrApp(buf, " concurrent", 64);
        SettingsUI::Row(x, ly, "Capacity:", buf);
        ly += 22;
    }
}

// ── input: pane-local mx,my. walk the SAME running-y layout as render (already
//    offset by -scroll) so hit rects line up exactly. controls sit at pane-local
//    x = CTRL_X_OFF with the fixed widths declared above. (satoru)
static bool security_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ly = -scroll + 8;

    ly += 26;          // "Current User" header (satoru)
    ly += 22;          // name row (satoru)
    ly += 22;          // username row (satoru)
    ly += 30;          // role row (satoru)

    ly += 26;          // "Users" header (satoru)
    ly += 26;          // registered row (satoru)
    // roster rows (or the single placeholder line). (satoru)
    if(s_user_count <= 0){
        ly += SettingsUI::ROW_H;
    } else {
        ly += roster_rows() * SettingsUI::ROW_H;
    }

    // manage row is now a read-only note (account creation needs typed input the
    // shell can't provide), so there is nothing to hit-test here. (satoru)
    ly += 34;          // manage row (satoru)

    ly += 26;          // "Sign-in" header (satoru)

    // auto-login toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_autologin = !s_autologin;
        persist_autologin();
        return true;
    }
    ly += 28;          // toggle row (satoru)
    ly += 26;          // boot-flag note row (satoru)

    // lock-timeout slider. (satoru)
    {
        int p = SettingsUI::SliderHit(ctrl_x, ly, SLIDER_W, mx, my);
        if(p >= 0){
            s_lock_timeout = kTimeouts[timeout_index_for_pct(p)];
            persist_timeout();
            return true;
        }
    }
    ly += 34;          // slider row (satoru)

    // remaining rows (security level + sessions) are read-only. (satoru)
    return false;
}

// total content height for the scrollbar (sum of the row advances + tail). the
// roster block is variable, so compute it from the live row count. (satoru)
static int security_content_height(){
    int roster = (s_user_count <= 0) ? SettingsUI::ROW_H
                                     : roster_rows() * SettingsUI::ROW_H;
    return 8
         + 26 + 22 + 22 + 30                 // current user (satoru)
         + 26 + 26 + roster + 34             // users (satoru)
         + 26 + 28 + 26 + 34                 // sign-in (satoru)
         + 26 + 22 + 22 + 30                 // security level (satoru)
         + 26 + 22 + 22                      // sessions (satoru)
         + 16;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_security_module;` resolves at link time. (satoru)
extern const SettingsModule g_security_module = {
    "security", "Security & Users", "\x16",
    security_on_show, security_render, security_input, security_content_height
};
// end (satoru)
