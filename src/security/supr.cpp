#include "supr.h"
#include "ksa.h"                  // hypervisor-backed auth factor (satoru)
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"
#include "../system/logging.h"   // mirror the audit trail to /kurono/var/log/security.log (satoru)

//  supr security engine implementation

SUPRUser SUPR::users[SUPR_MAX_USERS];
int SUPR::user_count = 0;
SUPRSession SUPR::sessions[SUPR_MAX_SESSIONS];
int SUPR::current_session = -1;
SUPRAuditEntry SUPR::audit_log[SUPR_MAX_LOG];
int SUPR::audit_count = 0;
unsigned int SUPR::rng_state = 0x12345678;
// default policy: password prompt only (--auth=passwd), per spec. the ksa
// factor is opt-in via `supr policy --auth=kvault|both`. (satoru)
SUPRAuthPolicy SUPR::policy = { /*passwd*/ true, /*kvault*/ false };

static int slen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void scpy(char* d, const char* s, int m) {
    int i=0; while (s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}
static bool seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; } return *a==*b;
}
static void smemset(void* p, int val, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)val;
}
static void smemcpy(void* d, const void* s, int n) {
    unsigned char* db = (unsigned char*)d;
    const unsigned char* sb = (const unsigned char*)s;
    for (int i = 0; i < n; i++) db[i] = sb[i];
}
static bool smemeq(const void* a, const void* b, int n) {
    const unsigned char* ab = (const unsigned char*)a;
    const unsigned char* bb = (const unsigned char*)b;
    for (int i = 0; i < n; i++) if (ab[i] != bb[i]) return false;
    return true;
}

// this is a simplified hash  -  not cryptographic grade, but functional.

static void simple_hash(const unsigned char* data, int len, const unsigned char* salt, int slen_v, unsigned char* out) {
    // mix data and salt through xorshift rounds
    unsigned int h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // mix salt
    for (int i = 0; i < slen_v; i++) {
        h[i % 8] ^= (unsigned int)salt[i] << ((i % 4) * 8);
        h[i % 8] = (h[i % 8] << 7) | (h[i % 8] >> 25);
    }

    // mix data through 8 rounds
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < len; i++) {
            unsigned int k = (unsigned int)data[i];
            h[(i + round) % 8] += k * 0x9e3779b9;
            h[(i + round) % 8] ^= h[(i + round + 1) % 8] >> 11;
            h[(i + round) % 8] = (h[(i + round) % 8] << 13) | (h[(i + round) % 8] >> 19);
        }
        // avalanche
        for (int j = 0; j < 8; j++) {
            h[j] ^= h[j] >> 16;
            h[j] *= 0x85ebca6b;
            h[j] ^= h[j] >> 13;
            h[j] *= 0xc2b2ae35;
            h[j] ^= h[j] >> 16;
        }
    }

    // copy to output
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(h[i]);
    }
}

void SUPR::Init() {
    user_count = 0;
    audit_count = 0;
    current_session = -1;
    rng_state = Time::GetTicks() ^ 0xDEADBEEF;

    for (int i = 0; i < SUPR_MAX_SESSIONS; i++) {
        sessions[i].active = false;
    }

    // create default users
    CreateUser("root", "root", SUPR_ROOT);
    CreateUser("user", "user", SUPR_USER);
    CreateUser("guest", "", SUPR_GUEST);

    // set root home
    if (user_count >= 1) {
        scpy(users[0].home, "/root", 64);
        users[0].uid = 0;
        users[0].gid = 0;
    }
    if (user_count >= 2) {
        scpy(users[1].home, "/home/user", 64);
        users[1].uid = 1000;
        users[1].gid = 1000;
    }
    if (user_count >= 3) {
        scpy(users[2].home, "/home/guest", 64);
        users[2].uid = 1001;
        users[2].gid = 1001;
    }

    // auto-login user
    current_session = Authenticate("user", "user");

    // bring up the ksa (kurono secure authorization) factor. this probes the
    // hypervisor; if unavailable, ksa reports so and policy degrades to the
    // password factor. default policy stays passwd-only until the user opts
    // into kvault via `supr policy`. (satoru)
    KSA::Init();

    Log(ACT_LOGIN, "system", "SUPR security engine initialized");
    SerialLogger::Log("SUPR: Security engine initialized\r\n");
}

void SUPR::GenerateSalt(unsigned char* salt_out) {
    for (int i = 0; i < SUPR_SALT_LEN; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        salt_out[i] = (unsigned char)(rng_state & 0xFF);
    }
}

void SUPR::HashPassword(const char* password, const unsigned char* salt, unsigned char* hash_out) {
    simple_hash((const unsigned char*)password, slen(password), salt, SUPR_SALT_LEN, hash_out);
}

bool SUPR::CreateUser(const char* username, const char* password, SUPRLevel level) {
    if (user_count >= SUPR_MAX_USERS) return false;
    if (FindUser(username)) return false;

    SUPRUser& u = users[user_count];
    scpy(u.username, username, 32);
    u.level = level;
    u.locked = false;
    u.failed_attempts = 0;
    u.uid = 1000 + user_count;
    u.gid = 1000 + user_count;
    u.group_count = 1;
    scpy(u.groups[0], username, 16);
    scpy(u.home, "/home/", 64);
    int hl = slen(u.home);
    scpy(u.home + hl, username, 64 - hl);
    scpy(u.shell, "/bin/ksh", 32);

    GenerateSalt(u.salt);
    HashPassword(password, u.salt, u.pwd_hash);

    user_count++;

    // create home directory
    KVFS::Mkdirs(u.home);

    Log(ACT_CREATE_USER, username, "User created");
    return true;
}

bool SUPR::DeleteUser(const char* username) {
    for (int i = 0; i < user_count; i++) {
        if (seq(users[i].username, username)) {
            Log(ACT_DELETE_USER, username, "User deleted");
            // shift remaining
            for (int j = i; j < user_count - 1; j++) {
                smemcpy(&users[j], &users[j + 1], sizeof(SUPRUser));
            }
            user_count--;
            return true;
        }
    }
    return false;
}

bool SUPR::ChangePassword(const char* username, const char* old_pwd, const char* new_pwd) {
    SUPRUser* u = FindUser(username);
    if (!u) return false;

    // verify old password
    unsigned char hash[SUPR_HASH_LEN];
    HashPassword(old_pwd, u->salt, hash);
    if (!smemeq(hash, u->pwd_hash, SUPR_HASH_LEN)) {
        Log(ACT_PERMISSION_DENIED, username, "Wrong old password");
        return false;
    }

    // set new
    GenerateSalt(u->salt);
    HashPassword(new_pwd, u->salt, u->pwd_hash);
    Log(ACT_CHANGE_PWD, username, "Password changed");
    return true;
}

bool SUPR::LockUser(const char* username) {
    SUPRUser* u = FindUser(username);
    if (!u) return false;
    u->locked = true;
    Log(ACT_PERMISSION_DENIED, username, "Account locked");
    return true;
}

bool SUPR::UnlockUser(const char* username) {
    SUPRUser* u = FindUser(username);
    if (!u) return false;
    u->locked = false;
    u->failed_attempts = 0;
    return true;
}

SUPRUser* SUPR::FindUser(const char* username) {
    for (int i = 0; i < user_count; i++) {
        if (seq(users[i].username, username)) return &users[i];
    }
    return nullptr;
}

int SUPR::GetUserCount() { return user_count; }
SUPRUser* SUPR::GetUsers() { return users; }

int SUPR::Authenticate(const char* username, const char* password) {
    SUPRUser* u = FindUser(username);
    if (!u) {
        Log(ACT_FAIL_LOGIN, username, "Unknown user");
        return -1;
    }

    if (u->locked) {
        Log(ACT_FAIL_LOGIN, username, "Account locked");
        return -1;
    }

    // check password
    unsigned char hash[SUPR_HASH_LEN];
    HashPassword(password, u->salt, hash);

    if (!smemeq(hash, u->pwd_hash, SUPR_HASH_LEN)) {
        u->failed_attempts++;
        if (u->failed_attempts >= 5) {
            u->locked = true;
            Log(ACT_FAIL_LOGIN, username, "Locked after 5 failures");
        } else {
            Log(ACT_FAIL_LOGIN, username, "Wrong password");
        }
        return -1;
    }

    // find free session
    int sid = -1;
    for (int i = 0; i < SUPR_MAX_SESSIONS; i++) {
        if (!sessions[i].active) { sid = i; break; }
    }
    if (sid == -1) return -1;

    // create session
    int user_idx = (int)(u - users);
    sessions[sid].user_index = user_idx;
    sessions[sid].active = true;
    sessions[sid].login_time = Time::GetTicks();
    sessions[sid].last_activity = sessions[sid].login_time;
    sessions[sid].timeout = 0;
    scpy(sessions[sid].tty, "tty1", 16);

    u->failed_attempts = 0;
    Log(ACT_LOGIN, username, "Logged in");
    return sid;
}

void SUPR::Logout(int session_id) {
    if (session_id < 0 || session_id >= SUPR_MAX_SESSIONS) return;
    if (!sessions[session_id].active) return;

    int ui = sessions[session_id].user_index;
    Log(ACT_LOGOUT, users[ui].username, "Logged out");
    sessions[session_id].active = false;

    if (current_session == session_id) current_session = -1;
}

bool SUPR::ValidateSession(int session_id) {
    if (session_id < 0 || session_id >= SUPR_MAX_SESSIONS) return false;
    if (!sessions[session_id].active) return false;

    // check timeout
    if (sessions[session_id].timeout > 0) {
        unsigned int elapsed = Time::GetTicks() - sessions[session_id].last_activity;
        if (elapsed > sessions[session_id].timeout) {
            Logout(session_id);
            return false;
        }
    }

    sessions[session_id].last_activity = Time::GetTicks();
    return true;
}

SUPRSession* SUPR::GetSession(int session_id) {
    if (session_id < 0 || session_id >= SUPR_MAX_SESSIONS) return nullptr;
    return &sessions[session_id];
}

int SUPR::GetCurrentSession() { return current_session; }
void SUPR::SetCurrentSession(int sid) { current_session = sid; }

bool SUPR::CheckPermission(int session_id, SUPRLevel required) {
    if (!ValidateSession(session_id)) return false;
    int ui = sessions[session_id].user_index;
    if (users[ui].level >= required) return true;
    Log(ACT_PERMISSION_DENIED, users[ui].username, "Insufficient permissions");
    return false;
}

bool SUPR::Escalate(int session_id, const char* password) {
    if (!ValidateSession(session_id)) return false;

    // run the full auth gate (password and/or ksa per the active policy). this
    // is THE single escalation chokepoint  -  su/sudo and the gui all funnel here
    // so the policy is enforced uniformly. (satoru)
    if (!RunEscalationGate(session_id, password, "su/sudo escalation")) {
        Log(ACT_PERMISSION_DENIED, users[sessions[session_id].user_index].username, "Escalation failed");
        return false;
    }

    // temporarily elevate (for this session)
    SUPRUser* root = FindUser("root");
    if (!root) return false;
    sessions[session_id].user_index = (int)(root - users);
    Log(ACT_ESCALATE, "root", "Escalation granted");
    return true;
}

SUPRLevel SUPR::GetLevel(int session_id) {
    if (!ValidateSession(session_id)) return SUPR_GUEST;
    return users[sessions[session_id].user_index].level;
}

// ── auth policy / ksa wiring ───────────────────────────────────────────

bool SUPR::IsPasswdEnabled() { return policy.passwd_enabled; }
bool SUPR::IsKvaultEnabled() { return policy.kvault_enabled; }
SUPRAuthPolicy SUPR::GetPolicy() { return policy; }
bool SUPR::BothAuthDisabled() { return !policy.passwd_enabled && !policy.kvault_enabled; }

SUPRAuthMode SUPR::GetAuthMode() {
    if (policy.passwd_enabled && policy.kvault_enabled) return AUTH_BOTH;
    if (policy.kvault_enabled) return AUTH_KVAULT;
    return AUTH_PASSWD;   // passwd-only, or (degenerate) both-off treated as passwd surface (satoru)
}

bool SUPR::VerifyEscalationPassword(const char* password) {
    // accept the credential of any root/sovereign account. (satoru)
    for (int i = 0; i < user_count; i++) {
        if (users[i].level < SUPR_ROOT) continue;
        unsigned char hash[SUPR_HASH_LEN];
        HashPassword(password ? password : "", users[i].salt, hash);
        if (smemeq(hash, users[i].pwd_hash, SUPR_HASH_LEN)) return true;
    }
    return false;
}

bool SUPR::RunEscalationGate(int session_id, const char* password, const char* reason) {
    if (!ValidateSession(session_id)) return false;
    const char* actor = users[sessions[session_id].user_index].username;

    // both factors disabled => proceed but loudly warn + audit (no real check).
    if (BothAuthDisabled()) {
        Log(ACT_RISK_WARNING, actor, "both auth factors disabled  -  proceeding");
        RuntimeLog::LogSecurity("RISK: escalation with no auth factor", reason);
        SerialLogger::Log("SUPR: WARNING  -  escalation with BOTH auth factors disabled\r\n");
        return true;
    }

    bool need_pw  = policy.passwd_enabled;
    // an unavailable ksa factor cannot be *required*  -  if policy wants kvault but
    // the hypervisor isn't there, the password factor must carry it. we never
    // silently pass an unsatisfiable factor; instead we downgrade and audit. (satoru)
    bool need_ksa = policy.kvault_enabled && KSA::IsAvailable();
    if (policy.kvault_enabled && !KSA::IsAvailable()) {
        RuntimeLog::LogSecurity("ksa required by policy but unavailable", reason);
        SerialLogger::Log("SUPR: ksa required but unavailable  -  falling back to password factor\r\n");
        if (!policy.passwd_enabled) {
            // policy wanted kvault-only but ksa is gone and passwd is off: refuse.
            Log(ACT_PERMISSION_DENIED, actor, "kvault-only policy but ksa unavailable");
            return false;
        }
        need_pw = true;
    }

    bool pw_ok  = true;
    bool ksa_ok = true;

    if (need_pw) {
        pw_ok = VerifyEscalationPassword(password);
        if (!pw_ok) Log(ACT_PERMISSION_DENIED, actor, "password factor failed");
    }

    if (need_ksa) {
        KSARequest req;
        req.title    = "Privilege Escalation";
        req.detail   = reason ? reason : "An action requires elevated rights.";
        req.username = "root";
        // if password is NOT a separate factor, ksa collects the credential too.
        req.want_cred = !need_pw;
        KSAVerdict v;
        if (!KSA::Prompt(req, v)) {
            // ksa could not run at all  -  treat as denial of the ksa factor. (satoru)
            Log(ACT_KSA_DENY, actor, "ksa prompt could not run");
            ksa_ok = false;
        } else {
            ksa_ok = v.approved;
            // when ksa is the sole credential collector, verify its hash too.
            if (ksa_ok && req.want_cred) {
                SUPRUser* root = FindUser("root");
                ksa_ok = root && v.have_cred_hash &&
                         smemeq(v.cred_hash, root->pwd_hash, SUPR_HASH_LEN);
                if (!ksa_ok) Log(ACT_PERMISSION_DENIED, actor, "ksa credential mismatch");
            }
        }
    }

    return pw_ok && ksa_ok;
}

// copy a reason string into err if provided. (satoru)
static void set_err(char* err, int err_max, const char* msg) {
    if (!err || err_max <= 0) return;
    int i = 0; while (msg[i] && i < err_max - 1) { err[i] = msg[i]; i++; } err[i] = 0;
}

bool SUPR::SetAuthMode(int session_id, SUPRAuthMode mode, bool sovereign_override,
                       char* err, int err_max) {
    if (!ValidateSession(session_id)) { set_err(err, err_max, "invalid session"); return false; }
    const char* actor = users[sessions[session_id].user_index].username;

    bool want_pw, want_kv;
    switch (mode) {
        case AUTH_PASSWD: want_pw = true;  want_kv = false; break;
        case AUTH_KVAULT: want_pw = false; want_kv = true;  break;
        case AUTH_BOTH:   want_pw = true;  want_kv = true;  break;
        default: set_err(err, err_max, "unknown auth mode"); return false;
    }

    // loophole guard: you cannot land in the "both disabled" state through
    // SetAuthMode (none of the modes do that), but if a future mode could, the
    // sovereign-override gate would apply here too. (satoru)
    if (!want_pw && !want_kv) {
        if (!sovereign_override) {
            set_err(err, err_max, "refusing to disable both factors without --sovereign-override");
            return false;
        }
        if (GetLevel(session_id) != SUPR_SOVEREIGN) {
            set_err(err, err_max, "--sovereign-override requires the sovereign role");
            return false;
        }
        Log(ACT_SOVEREIGN_OVERRIDE, actor, "disable both via SetAuthMode");
    }

    // selecting kvault requires ksa to actually be available. (satoru)
    if (want_kv && !KSA::IsAvailable()) {
        set_err(err, err_max, "ksa (kvault) unavailable on this host");
        return false;
    }

    policy.passwd_enabled = want_pw;
    policy.kvault_enabled = want_kv;
    Log(ACT_POLICY_CHANGE, actor,
        mode == AUTH_BOTH ? "auth=both" : (mode == AUTH_KVAULT ? "auth=kvault" : "auth=passwd"));
    RuntimeLog::LogSecurity("auth policy changed",
        mode == AUTH_BOTH ? "auth=both" : (mode == AUTH_KVAULT ? "auth=kvault" : "auth=passwd"));
    return true;
}

bool SUPR::EnableKvault(int session_id, char* err, int err_max) {
    if (!ValidateSession(session_id)) { set_err(err, err_max, "invalid session"); return false; }
    if (!KSA::IsAvailable()) { set_err(err, err_max, "ksa unavailable on this host"); return false; }
    policy.kvault_enabled = true;
    Log(ACT_POLICY_CHANGE, users[sessions[session_id].user_index].username, "kvault enabled");
    RuntimeLog::LogSecurity("kvault enabled", nullptr);
    return true;
}

bool SUPR::EnablePasswd(int session_id, char* err, int err_max) {
    if (!ValidateSession(session_id)) { set_err(err, err_max, "invalid session"); return false; }
    policy.passwd_enabled = true;
    Log(ACT_POLICY_CHANGE, users[sessions[session_id].user_index].username, "passwd enabled");
    RuntimeLog::LogSecurity("passwd enabled", nullptr);
    return true;
}

bool SUPR::DisableKvault(int session_id, bool force, bool ack_risk,
                         bool sovereign_override, char* err, int err_max) {
    if (!ValidateSession(session_id)) { set_err(err, err_max, "invalid session"); return false; }
    const char* actor = users[sessions[session_id].user_index].username;

    if (!force || !ack_risk) {
        set_err(err, err_max, "disabling ksa requires --force --acknowledge-risk");
        return false;
    }

    // cannot disable if password auth is also disabled  -  that would leave both
    // off  -  unless the sovereign explicitly overrides. (satoru)
    if (!policy.passwd_enabled && !sovereign_override) {
        set_err(err, err_max, "cannot disable ksa while password auth is off (use --sovereign-override)");
        return false;
    }
    if (!policy.passwd_enabled && sovereign_override) {
        if (GetLevel(session_id) != SUPR_SOVEREIGN) {
            set_err(err, err_max, "--sovereign-override requires the sovereign role");
            return false;
        }
        Log(ACT_SOVEREIGN_OVERRIDE, actor, "disable ksa with passwd already off");
    }

    policy.kvault_enabled = false;
    Log(ACT_POLICY_CHANGE, actor, "kvault disabled");
    RuntimeLog::LogSecurity("kvault disabled", force ? "force+acknowledged" : "acknowledged");
    if (BothAuthDisabled())
        RuntimeLog::LogSecurity("RISK: both auth factors now disabled", actor);
    return true;
}

bool SUPR::DisablePasswd(int session_id, bool ack_risk, bool sovereign_override,
                         char* err, int err_max) {
    if (!ValidateSession(session_id)) { set_err(err, err_max, "invalid session"); return false; }
    const char* actor = users[sessions[session_id].user_index].username;

    if (!ack_risk) {
        set_err(err, err_max, "disabling password auth requires --acknowledge-risk");
        return false;
    }

    // requires ksa to be active first; refuses if ksa is off (unless sovereign
    // override, which would leave both off  -  and only sovereign may do that).
    if (!policy.kvault_enabled) {
        if (!sovereign_override) {
            set_err(err, err_max, "cannot disable password auth unless ksa is active (use --sovereign-override)");
            return false;
        }
        if (GetLevel(session_id) != SUPR_SOVEREIGN) {
            set_err(err, err_max, "--sovereign-override requires the sovereign role");
            return false;
        }
        Log(ACT_SOVEREIGN_OVERRIDE, actor, "disable passwd with ksa off");
    } else if (!KSA::IsAvailable()) {
        set_err(err, err_max, "ksa policy is on but unavailable on this host  -  refusing to disable password");
        return false;
    }

    policy.passwd_enabled = false;
    Log(ACT_POLICY_CHANGE, actor, "passwd disabled");
    RuntimeLog::LogSecurity("passwd disabled", "acknowledged");
    if (BothAuthDisabled())
        RuntimeLog::LogSecurity("RISK: both auth factors now disabled", actor);
    return true;
}

// ── policy / loophole self-test ─────────────────────────────────────────
static void pst_check(const char* name, bool cond, bool& all) {
    SerialLogger::Log("POLICY-SELFTEST: ");
    SerialLogger::Log(name);
    SerialLogger::Log(cond ? " ... PASS\r\n" : " ... FAIL\r\n");
    if (!cond) all = false;
}

bool SUPR::PolicySelfTest() {
    SerialLogger::Log("POLICY-SELFTEST: begin\r\n");
    bool all = true;
    char err[160];

    // use the current session; remember + restore policy and the user's level
    // so the self-test is non-destructive. (satoru)
    int sid = current_session;
    if (sid < 0 || !sessions[sid].active) {
        SerialLogger::Log("POLICY-SELFTEST: no active session  -  skipping\r\n");
        return true;
    }
    SUPRAuthPolicy saved = policy;
    int ui = sessions[sid].user_index;
    SUPRLevel saved_level = users[ui].level;

    // start from a known state: passwd on, kvault off (the default). (satoru)
    policy.passwd_enabled = true; policy.kvault_enabled = false;

    // 1. a non-sovereign cannot use --sovereign-override. promote to root only
    //    (not sovereign) and try to disable passwd with ksa off + override. (satoru)
    users[ui].level = SUPR_ROOT;
    err[0]=0;
    bool r1 = DisablePasswd(sid, /*ack*/true, /*sov*/true, err, sizeof(err));
    pst_check("non-sovereign override refused", !r1, all);

    // 2. cannot disable passwd while ksa is off without override (root). (satoru)
    err[0]=0;
    bool r2 = DisablePasswd(sid, /*ack*/true, /*sov*/false, err, sizeof(err));
    pst_check("disable-passwd refused while ksa off (no override)", !r2, all);

    // 3. cannot disable ksa without --force --acknowledge-risk. (satoru)
    err[0]=0;
    bool r3 = DisableKvault(sid, /*force*/false, /*ack*/false, /*sov*/false, err, sizeof(err));
    pst_check("disable-kvault refused without force/ack", !r3, all);

    // 4. cannot disable ksa while passwd is also off (no override). simulate by
    //    turning passwd off in policy directly, then attempt. (satoru)
    policy.passwd_enabled = false; policy.kvault_enabled = true;
    err[0]=0;
    bool r4 = DisableKvault(sid, /*force*/true, /*ack*/true, /*sov*/false, err, sizeof(err));
    pst_check("disable-kvault refused while passwd off (no override)", !r4, all);

    // 5. sovereign CAN override the both-off path. promote to sovereign and
    //    repeat scenario 4  -  should now succeed and land in both-off. (satoru)
    users[ui].level = SUPR_SOVEREIGN;
    policy.passwd_enabled = false; policy.kvault_enabled = true;
    err[0]=0;
    bool r5 = DisableKvault(sid, /*force*/true, /*ack*/true, /*sov*/true, err, sizeof(err));
    pst_check("sovereign override disables kvault (both off)", r5, all);
    pst_check("both factors now disabled", BothAuthDisabled(), all);

    // 6. with both off, the escalation gate proceeds but flags the risk. it
    //    returns true (no factor to fail)  -  the warning is audited. (satoru)
    bool r6 = RunEscalationGate(sid, "anything", "policy-selftest");
    pst_check("both-off gate proceeds with risk warning", r6, all);

    // restore a sane state for subsequent assertions. (satoru)
    policy.passwd_enabled = true; policy.kvault_enabled = false;

    // 7. SetAuthMode(both) requires ksa available; on a host without ksa it is
    //    refused (and on one with ksa it succeeds). either outcome is correct  - 
    //    assert it AGREES with KSA availability. (satoru)
    err[0]=0;
    bool r7 = SetAuthMode(sid, AUTH_BOTH, /*sov*/false, err, sizeof(err));
    pst_check("auth=both gated on ksa availability", r7 == KSA::IsAvailable(), all);

    // 8. password factor actually verifies: the default root password "root"
    //    must pass, a wrong one must fail. (satoru)
    pst_check("root password verifies", VerifyEscalationPassword("root"), all);
    pst_check("wrong password rejected", !VerifyEscalationPassword("nope-xyz"), all);

    // restore everything. (satoru)
    policy = saved;
    users[ui].level = saved_level;

    SerialLogger::Log(all ? "POLICY-SELFTEST: OVERALL PASS\r\n"
                          : "POLICY-SELFTEST: OVERALL FAIL\r\n");
    RuntimeLog::LogSecurity("policy selftest", all ? "pass" : "fail");
    return all;
}

void SUPR::Log(SUPRAction action, const char* username, const char* detail) {
    int idx = audit_count % SUPR_MAX_LOG;
    audit_log[idx].action = action;
    scpy(audit_log[idx].username, username, 32);
    scpy(audit_log[idx].detail, detail, 64);
    audit_log[idx].timestamp = Time::GetTicks();
    audit_count++;
    // persist the audit event to the security log (detail already carries the
    // human description, e.g. "Escalation granted"); username is the actor. (satoru)
    RuntimeLog::LogSecurity(detail && *detail ? detail : "event", username);
}

int SUPR::GetAuditLog(SUPRAuditEntry* entries, int max_entries) {
    int total = audit_count < SUPR_MAX_LOG ? audit_count : SUPR_MAX_LOG;
    int start = audit_count > SUPR_MAX_LOG ? audit_count - SUPR_MAX_LOG : 0;
    int count = 0;
    for (int i = start; i < audit_count && count < max_entries; i++) {
        smemcpy(&entries[count++], &audit_log[i % SUPR_MAX_LOG], sizeof(SUPRAuditEntry));
    }
    return count;
}
