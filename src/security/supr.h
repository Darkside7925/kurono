#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — SUPR Security Engine
//  User auth, sessions, audit logging, permissions
// ═══════════════════════════════════════════════════════════════════════════

#define SUPR_MAX_USERS     16
#define SUPR_MAX_SESSIONS   8
#define SUPR_MAX_LOG       128
#define SUPR_SALT_LEN       8
#define SUPR_HASH_LEN      32
#define SUPR_MAX_GROUPS     8

enum SUPRLevel {
    SUPR_GUEST = 0,
    SUPR_USER  = 1,
    SUPR_ADMIN = 2,
    SUPR_ROOT  = 3
};

enum SUPRAction {
    ACT_LOGIN = 0,  ACT_LOGOUT,  ACT_FAIL_LOGIN,
    ACT_CREATE_USER, ACT_DELETE_USER, ACT_CHANGE_PWD,
    ACT_ESCALATE, ACT_FILE_ACCESS, ACT_EXEC_CMD,
    ACT_PERMISSION_DENIED
};

struct SUPRUser {
    char username[32];
    unsigned char pwd_hash[SUPR_HASH_LEN];
    unsigned char salt[SUPR_SALT_LEN];
    SUPRLevel level;
    char home[64];
    char shell[32];
    bool locked;
    int  failed_attempts;
    int  uid;
    int  gid;
    char groups[SUPR_MAX_GROUPS][16];
    int  group_count;
};

struct SUPRSession {
    int  user_index;
    bool active;
    unsigned int login_time;
    unsigned int last_activity;
    unsigned int timeout;     // ms — 0 = no timeout
    char tty[16];
};

struct SUPRAuditEntry {
    SUPRAction action;
    char username[32];
    char detail[64];
    unsigned int timestamp;
};

class SUPR {
public:
    static void Init();

    // User management
    static bool CreateUser(const char* username, const char* password, SUPRLevel level);
    static bool DeleteUser(const char* username);
    static bool ChangePassword(const char* username, const char* old_pwd, const char* new_pwd);
    static bool LockUser(const char* username);
    static bool UnlockUser(const char* username);
    static SUPRUser* FindUser(const char* username);
    static int  GetUserCount();
    static SUPRUser* GetUsers();

    // Authentication
    static int  Authenticate(const char* username, const char* password);  // returns session id or -1
    static void Logout(int session_id);
    static bool ValidateSession(int session_id);
    static SUPRSession* GetSession(int session_id);
    static int  GetCurrentSession();
    static void SetCurrentSession(int sid);

    // Authorization
    static bool CheckPermission(int session_id, SUPRLevel required);
    static bool Escalate(int session_id, const char* password);  // su/sudo
    static SUPRLevel GetLevel(int session_id);

    // Audit
    static void Log(SUPRAction action, const char* username, const char* detail);
    static int  GetAuditLog(SUPRAuditEntry* entries, int max_entries);

    // Hash
    static void HashPassword(const char* password, const unsigned char* salt, unsigned char* hash_out);
    static void GenerateSalt(unsigned char* salt_out);

private:
    static SUPRUser users[SUPR_MAX_USERS];
    static int user_count;
    static SUPRSession sessions[SUPR_MAX_SESSIONS];
    static int current_session;
    static SUPRAuditEntry audit_log[SUPR_MAX_LOG];
    static int audit_count;
    static unsigned int rng_state;
};
