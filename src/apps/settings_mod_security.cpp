//  kurono os - settings module: users & security (satoru)
//  the signed-in account, the account list, adding an account (UserManager
//  validates, hashes, creates the home folder and persists /etc/passwd), and
//  the live KEN privilege level + session count. (satoru)
#include "system_settings.h"
#include "settings_kit.h"
#include "../system/user_mgmt.h"
#include "../security/ken.h"

using namespace SettingsUI;

static char s_new_user[32];
static char s_new_pass[48];
static char s_msg[64];
static bool s_msg_ok = false;
static bool s_adding = false;

static const char* ken_level_label(KENLevel lvl){
    switch(lvl){
        case KEN_GUEST: return "Guest";
        case KEN_USER:  return "Standard";
        case KEN_ADMIN: return "Administrator";
        case KEN_ROOT:  return "Root";
        default:         return "Unknown";
    }
}

static void security_on_show(){
    s_new_user[0] = s_new_pass[0] = 0;
    s_msg[0] = 0;
    s_adding = false;
}

static void avatar(int cx, int cy, int r, const char* name, uint32_t col){
    Disc((float)cx, (float)cy, (float)r, col);
    char ini[2] = { (name && name[0]) ? name[0] : '?', 0 };
    if(ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 32);
    float px = r * 1.05f;
    int tw = TextWidth(px, ini, true);
    Text(cx - tw / 2, cy - (int)(px * 0.62f), px, ini, 0xFFFFFFFFu, true);
}

static void security_build(){
    const char* disp = UserManager::GetCurrentDisplayName();
    const char* uname = UserManager::GetCurrentUsername();
    int ci = UserManager::GetCurrentUserIndex();
    bool admin = ci >= 0 && ci < UserManager::MAX_USERS && UserManager::users[ci].is_admin;
    char buf[64];

    Group(nullptr);
    Rect r = Custom(88, false);
    if(r.w){
        const char* nm = (disp && disp[0]) ? disp : ((uname && uname[0]) ? uname : "Guest");
        avatar(r.x + 50, r.y + 44, 28, nm, Accent());
        Text(r.x + 92, r.y + 22, 18.0f, nm, T().text, true);
        StrCpy(buf, (uname && uname[0]) ? uname : "not signed in", 64);
        StrApp(buf, admin ? ", Administrator" : ", Standard user", 64);
        Text(r.x + 92, r.y + 48, 13.5f, buf, T().text2);
    }
    EndGroup();

    Group("Users");
    int n = UserManager::GetUserCount();
    if(n > UserManager::MAX_USERS) n = UserManager::MAX_USERS;
    if(n <= 0) Info("Accounts", "None", "Kurono is running as a guest session");
    for(int i = 0; i < n; i++){
        const User& u = UserManager::users[i];
        PushId(i);
        StrCpy(buf, u.is_admin ? "Administrator" : "Standard", 64);
        if(i == ci) StrApp(buf, ", signed in", 64);
        bool same = !u.display_name[0];
        if(!same){ int k = 0; while(u.display_name[k] && u.display_name[k] == u.username[k]) k++; same = !u.display_name[k] && !u.username[k]; }
        Info(u.display_name[0] ? u.display_name : u.username, buf, same ? nullptr : u.username);
        PopId();
    }
    if(!s_adding){
        if(Button("Add a user", "Add", BTN_SECONDARY)){ s_adding = true; s_msg[0] = 0; }
    } else {
        TextField("User name", s_new_user, 32, "letters, digits, - and _");
        bool go = TextField("Password", s_new_pass, 48, "at least 4 characters", FIELD_PASSWORD) && Submitted();
        if(Button("", "Create user", BTN_PRIMARY)) go = true;
        if(Button(" ", "Cancel", BTN_SECONDARY)){ s_adding = false; s_new_user[0] = s_new_pass[0] = 0; s_msg[0] = 0; }
        if(go){
            if(!UserManager::IsUsernameValid(s_new_user)){ StrCpy(s_msg, "Use 3 to 31 lowercase letters, digits, - or _", 64); s_msg_ok = false; }
            else if(UserManager::IsUsernameTaken(s_new_user)){ StrCpy(s_msg, "That user name is already taken", 64); s_msg_ok = false; }
            else if(StrLen(s_new_pass) < 4){ StrCpy(s_msg, "The password is too short", 64); s_msg_ok = false; }
            else if(UserManager::AddUser(s_new_user, s_new_pass)){
                StrCpy(s_msg, "Created ", 64); StrApp(s_msg, s_new_user, 64);
                s_msg_ok = true; s_adding = false; s_new_user[0] = s_new_pass[0] = 0;
            } else { StrCpy(s_msg, "Could not create the account", 64); s_msg_ok = false; }
        }
    }
    if(s_msg[0]) Info(s_msg_ok ? "Done" : "Problem", s_msg, nullptr, s_msg_ok ? T().success : T().danger);
    EndGroup();

    Group("Security");
    int sid = KEN::GetCurrentSession();
    Info("Privilege level", sid >= 0 ? ken_level_label(KEN::GetLevel(sid)) : "Guest");
    int act = 0;
    for(int i = 0; i < KEN_MAX_SESSIONS; i++){ KENSession* s = KEN::GetSession(i); if(s && s->active) act++; }
    IntToStr(act, buf, 64);
    Info("Active sessions", buf);
    EndGroup();
}

extern const SettingsModule g_security_module = {
    "security", "Users & Security", "", security_on_show, nullptr, nullptr, nullptr, security_build
};
// end (satoru)
