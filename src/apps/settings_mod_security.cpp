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
static int  s_open_user = -1;     // account row showing its options (satoru)
static bool s_pw_change = false;  // the change password fields are open (satoru)
static char s_pw_old[48], s_pw_new[48], s_pw_again[48];
static char s_pw_msg[64];
static bool s_pw_ok = false;

// the KSA switch (satoru)
static bool s_ksa_on = true;
static bool s_ksa_asking = false;   // the password row for switching it off is open
static char s_ksa_pw[48];
static char s_ksa_msg[72];
static bool s_ksa_ok = false;

static void pw_clear(){
    for(int i = 0; i < 48; i++){ s_pw_old[i] = 0; s_pw_new[i] = 0; s_pw_again[i] = 0; }
}

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
    s_open_user = -1;
    s_pw_change = false;
    s_pw_msg[0] = 0;
    pw_clear();
    s_ksa_on = KEN::IsKsaEnabled();
    s_ksa_asking = false;
    s_ksa_pw[0] = 0;
    s_ksa_msg[0] = 0;
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
        // an administrator can open another account to change its kind. (satoru)
        if(admin && i != ci){
            if(Nav(u.display_name[0] ? u.display_name : u.username, buf, same ? nullptr : u.username))
                s_open_user = (s_open_user == i) ? -1 : i;
            if(s_open_user == i){
                bool adm = u.is_admin;
                if(Toggle("Administrator", &adm, "Can approve changes for everyone")){
                    if(!UserManager::SetAdmin(u.username, adm)){
                        StrCpy(s_msg, "Kurono needs at least one administrator", 64); s_msg_ok = false;
                    }
                }
            }
        } else {
            Info(u.display_name[0] ? u.display_name : u.username, buf, same ? nullptr : u.username);
        }
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

    // password of the signed-in account. (satoru)
    if(ci >= 0 && ci < n){
        Group("Password");
        if(!s_pw_change){
            if(Button("Password", "Change", BTN_SECONDARY)){ s_pw_change = true; s_pw_msg[0] = 0; pw_clear(); }
        } else {
            TextField("Current", s_pw_old, 48, "current password", FIELD_PASSWORD);
            TextField("New", s_pw_new, 48, "at least 4 characters", FIELD_PASSWORD);
            bool go = TextField("Again", s_pw_again, 48, "new password again", FIELD_PASSWORD) && Submitted();
            if(Button("", "Change password", BTN_PRIMARY)) go = true;
            if(Button(" ", "Cancel", BTN_SECONDARY)){ s_pw_change = false; pw_clear(); s_pw_msg[0] = 0; }
            if(go){
                bool same_pw = true;
                for(int k = 0; k < 48; k++){ if(s_pw_new[k] != s_pw_again[k]){ same_pw = false; break; } if(!s_pw_new[k]) break; }
                if(!same_pw){ StrCpy(s_pw_msg, "The new passwords do not match", 64); s_pw_ok = false; }
                else if(StrLen(s_pw_new) < 4){ StrCpy(s_pw_msg, "The new password is too short", 64); s_pw_ok = false; }
                else if(UserManager::ChangePassword(uname, s_pw_old, s_pw_new)){
                    StrCpy(s_pw_msg, "Password changed", 64); s_pw_ok = true; s_pw_change = false;
                } else { StrCpy(s_pw_msg, "The current password is not right", 64); s_pw_ok = false; }
                pw_clear();
            }
        }
        if(s_pw_msg[0]) Info(s_pw_ok ? "Done" : "Problem", s_pw_msg, nullptr, s_pw_ok ? T().success : T().danger);
        EndGroup();
    }

    // KSA, the second approval behind amdo. off needs the password (the service
    // checks it through the KEN policy call); on again needs nothing. (satoru)
    Group("Approval");
    {
        bool on = s_ksa_on;
        if(Toggle("KSA", &on, s_ksa_on ? "Approvals use the secure KSA prompt"
                                       : "Off for now: amdo and approvals use your password only")){
            if(on){
                char e[72]; e[0] = 0;
                if(KEN::SetKsaEnabled(KEN::GetCurrentSession(), true, nullptr, e, 72)){
                    s_ksa_on = true; s_ksa_asking = false; s_ksa_pw[0] = 0;
                    StrCpy(s_ksa_msg, "KSA is on", 72); s_ksa_ok = true;
                } else { StrCpy(s_ksa_msg, e[0] ? e : "Could not turn KSA on", 72); s_ksa_ok = false; }
            } else {
                s_ksa_asking = true; s_ksa_pw[0] = 0; s_ksa_msg[0] = 0;
            }
        }
        if(s_ksa_asking && s_ksa_on){
            bool go = TextField("Password", s_ksa_pw, 48, "your password turns KSA off", FIELD_PASSWORD) && Submitted();
            if(Button("", "Turn KSA off", BTN_PRIMARY)) go = true;
            if(Button(" ", "Cancel", BTN_SECONDARY)){ s_ksa_asking = false; s_ksa_pw[0] = 0; s_ksa_msg[0] = 0; }
            if(go){
                char e[72]; e[0] = 0;
                if(KEN::SetKsaEnabled(KEN::GetCurrentSession(), false, s_ksa_pw, e, 72)){
                    s_ksa_on = false; s_ksa_asking = false;
                    StrCpy(s_ksa_msg, "KSA is off. amdo asks for your password", 72); s_ksa_ok = true;
                } else { StrCpy(s_ksa_msg, e[0] ? e : "Could not turn KSA off", 72); s_ksa_ok = false; }
                for(int k = 0; k < 48; k++) s_ksa_pw[k] = 0;
            }
        }
        if(s_ksa_msg[0]) Info(s_ksa_ok ? "Done" : "Problem", s_ksa_msg, nullptr, s_ksa_ok ? T().success : T().danger);
    }
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
