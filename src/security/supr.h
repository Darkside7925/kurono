#pragma once
//  kurono os  -  supr security engine
//  user auth, sessions, audit logging, permissions

#define SUPR_MAX_USERS     16
#define SUPR_MAX_SESSIONS   8
#define SUPR_MAX_LOG       128
#define SUPR_SALT_LEN       8
#define SUPR_HASH_LEN      32
#define SUPR_MAX_GROUPS     8

enum SUPRLevel {
    SUPR_GUEST     = 0,
    SUPR_USER      = 1,
    SUPR_ADMIN     = 2,
    SUPR_ROOT      = 3,
    SUPR_SOVEREIGN = 4   // root-equivalent owner; only role allowed --sovereign-override (satoru)
};

enum SUPRAction {
    ACT_LOGIN = 0,  ACT_LOGOUT,  ACT_FAIL_LOGIN,
    ACT_CREATE_USER, ACT_DELETE_USER, ACT_CHANGE_PWD,
    ACT_ESCALATE, ACT_FILE_ACCESS, ACT_EXEC_CMD,
    ACT_PERMISSION_DENIED,
    // ksa / auth-policy actions  -  audited even when ksa is disabled. (satoru)
    ACT_POLICY_CHANGE, ACT_KSA_PROMPT, ACT_KSA_APPROVE, ACT_KSA_DENY,
    ACT_SOVEREIGN_OVERRIDE, ACT_RISK_WARNING
};

// auth policy  -  which factors a privilege escalation must satisfy. (satoru)
enum SUPRAuthMode {
    AUTH_PASSWD = 0,   // password prompt only (default)
    AUTH_KVAULT = 1,   // ksa hypervisor prompt only
    AUTH_BOTH   = 2    // require both (max security)
};

// the persisted auth policy. both factors are tracked independently so the
// policy layer can detect (and block) the "both disabled" loophole. (satoru)
struct SUPRAuthPolicy {
    bool passwd_enabled;   // password factor required for escalation
    bool kvault_enabled;   // ksa hypervisor factor required for escalation
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
    unsigned int timeout;     // ms  -  0 = no timeout
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

    // user management
    static bool CreateUser(const char* username, const char* password, SUPRLevel level);
    static bool DeleteUser(const char* username);
    static bool ChangePassword(const char* username, const char* old_pwd, const char* new_pwd);
    static bool LockUser(const char* username);
    static bool UnlockUser(const char* username);
    static SUPRUser* FindUser(const char* username);
    static int  GetUserCount();
    static SUPRUser* GetUsers();

    // authentication
    static int  Authenticate(const char* username, const char* password);  // returns session id or -1
    static void Logout(int session_id);
    static bool ValidateSession(int session_id);
    static SUPRSession* GetSession(int session_id);
    static int  GetCurrentSession();
    static void SetCurrentSession(int sid);

    // authorization
    static bool CheckPermission(int session_id, SUPRLevel required);
    static bool Escalate(int session_id, const char* password);  // su/sudo
    static SUPRLevel GetLevel(int session_id);

    // ── auth policy (ksa) ──────────────────────────────────────────────
    // the active policy is derived from the two independent factors below.
    static SUPRAuthMode GetAuthMode();          // derived: passwd/kvault/both
    static bool IsPasswdEnabled();
    static bool IsKvaultEnabled();
    static SUPRAuthPolicy GetPolicy();

    // verify a credential against root/sovereign without mutating the session.
    // used by the escalation gate as the password factor. (satoru)
    static bool VerifyEscalationPassword(const char* password);

    // run the full escalation gate for the current session: applies the policy
    // (password and/or ksa) and returns true only if every required factor
    // passed.  pw may be nullptr (then the password factor, if required, fails
    // and the caller is expected to have prompted).  every path is audited.
    // (satoru)
    static bool RunEscalationGate(int session_id, const char* password,
                                  const char* reason);

    // sudo-style escalation for `supr <cmd>`: a line-based shell command can't
    // prompt for a password inline, so this runs the policy gate collecting the
    // credential/approval through the interactive ksa modal (available whenever
    // the hypervisor is, even if kvault is policy-off  -  it's just the secure
    // prompt). on success it elevates the session to root and writes the
    // pre-elevation user index to *out_saved; call SudoEnd to restore. (satoru)
    static bool SudoBegin(int session_id, const char* reason, int* out_saved_user);
    static void SudoEnd(int session_id, int saved_user);

    // policy mutation  -  these enforce the loophole rules and audit. each
    // returns true on success.  err (optional) receives a human reason on
    // failure. sovereign_override gates the dangerous "disable both" path.
    static bool SetAuthMode(int session_id, SUPRAuthMode mode,
                            bool sovereign_override, char* err, int err_max);
    static bool DisableKvault(int session_id, bool force, bool ack_risk,
                              bool sovereign_override, char* err, int err_max);
    static bool DisablePasswd(int session_id, bool ack_risk,
                              bool sovereign_override, char* err, int err_max);
    static bool EnableKvault(int session_id, char* err, int err_max);
    static bool EnablePasswd(int session_id, char* err, int err_max);

    // true if both factors are off  -  every escalation must then show a risk
    // warning before proceeding. (satoru)
    static bool BothAuthDisabled();

    // exercise the policy/loophole state machine deterministically and log each
    // assertion to serial. used by the boot self-test (kurono.ksa.test). does
    // not touch hardware; restores the prior policy on exit. (satoru)
    static bool PolicySelfTest();

    // audit
    static void Log(SUPRAction action, const char* username, const char* detail);
    static int  GetAuditLog(SUPRAuditEntry* entries, int max_entries);

    // hash
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
    static SUPRAuthPolicy policy;   // active auth policy (satoru)
};
