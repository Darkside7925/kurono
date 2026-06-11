#include "supr.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"

// ═══════════════════════════════════════════════════════════════════════════
//  SUPR Security Engine Implementation
// ═══════════════════════════════════════════════════════════════════════════

SUPRUser SUPR::users[SUPR_MAX_USERS];
int SUPR::user_count = 0;
SUPRSession SUPR::sessions[SUPR_MAX_SESSIONS];
int SUPR::current_session = -1;
SUPRAuditEntry SUPR::audit_log[SUPR_MAX_LOG];
int SUPR::audit_count = 0;
unsigned int SUPR::rng_state = 0x12345678;

// ── Helpers ──────────────────────────────────────────────────────────────

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

// ── Simple SHA-256-like hash (simplified for bare-metal) ─────────────────
// This is a simplified hash — NOT cryptographic grade, but functional.

static void simple_hash(const unsigned char* data, int len, const unsigned char* salt, int slen_v, unsigned char* out) {
    // Mix data and salt through xorshift rounds
    unsigned int h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Mix salt
    for (int i = 0; i < slen_v; i++) {
        h[i % 8] ^= (unsigned int)salt[i] << ((i % 4) * 8);
        h[i % 8] = (h[i % 8] << 7) | (h[i % 8] >> 25);
    }

    // Mix data through 8 rounds
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < len; i++) {
            unsigned int k = (unsigned int)data[i];
            h[(i + round) % 8] += k * 0x9e3779b9;
            h[(i + round) % 8] ^= h[(i + round + 1) % 8] >> 11;
            h[(i + round) % 8] = (h[(i + round) % 8] << 13) | (h[(i + round) % 8] >> 19);
        }
        // Avalanche
        for (int j = 0; j < 8; j++) {
            h[j] ^= h[j] >> 16;
            h[j] *= 0x85ebca6b;
            h[j] ^= h[j] >> 13;
            h[j] *= 0xc2b2ae35;
            h[j] ^= h[j] >> 16;
        }
    }

    // Copy to output
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(h[i]);
    }
}

// ── Init ─────────────────────────────────────────────────────────────────

void SUPR::Init() {
    user_count = 0;
    audit_count = 0;
    current_session = -1;
    rng_state = Time::GetTicks() ^ 0xDEADBEEF;

    for (int i = 0; i < SUPR_MAX_SESSIONS; i++) {
        sessions[i].active = false;
    }

    // Create default users
    CreateUser("root", "root", SUPR_ROOT);
    CreateUser("user", "user", SUPR_USER);
    CreateUser("guest", "", SUPR_GUEST);

    // Set root home
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

    // Auto-login user
    current_session = Authenticate("user", "user");

    Log(ACT_LOGIN, "system", "SUPR security engine initialized");
    SerialLogger::Log("SUPR: Security engine initialized\r\n");
}

// ── Hash / salt ──────────────────────────────────────────────────────────

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

// ── User management ──────────────────────────────────────────────────────

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

    // Create home directory
    KVFS::Mkdirs(u.home);

    Log(ACT_CREATE_USER, username, "User created");
    return true;
}

bool SUPR::DeleteUser(const char* username) {
    for (int i = 0; i < user_count; i++) {
        if (seq(users[i].username, username)) {
            Log(ACT_DELETE_USER, username, "User deleted");
            // Shift remaining
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

    // Verify old password
    unsigned char hash[SUPR_HASH_LEN];
    HashPassword(old_pwd, u->salt, hash);
    if (!smemeq(hash, u->pwd_hash, SUPR_HASH_LEN)) {
        Log(ACT_PERMISSION_DENIED, username, "Wrong old password");
        return false;
    }

    // Set new
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

// ── Authentication ───────────────────────────────────────────────────────

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

    // Check password
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

    // Find free session
    int sid = -1;
    for (int i = 0; i < SUPR_MAX_SESSIONS; i++) {
        if (!sessions[i].active) { sid = i; break; }
    }
    if (sid == -1) return -1;

    // Create session
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

    // Check timeout
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

// ── Authorization ────────────────────────────────────────────────────────

bool SUPR::CheckPermission(int session_id, SUPRLevel required) {
    if (!ValidateSession(session_id)) return false;
    int ui = sessions[session_id].user_index;
    if (users[ui].level >= required) return true;
    Log(ACT_PERMISSION_DENIED, users[ui].username, "Insufficient permissions");
    return false;
}

bool SUPR::Escalate(int session_id, const char* password) {
    if (!ValidateSession(session_id)) return false;

    // Authenticate as root
    SUPRUser* root = FindUser("root");
    if (!root) return false;

    unsigned char hash[SUPR_HASH_LEN];
    HashPassword(password, root->salt, hash);
    if (!smemeq(hash, root->pwd_hash, SUPR_HASH_LEN)) {
        Log(ACT_PERMISSION_DENIED, users[sessions[session_id].user_index].username, "Escalation failed");
        return false;
    }

    // Temporarily elevate (for this session)
    sessions[session_id].user_index = (int)(root - users);
    Log(ACT_ESCALATE, "root", "Escalation granted");
    return true;
}

SUPRLevel SUPR::GetLevel(int session_id) {
    if (!ValidateSession(session_id)) return SUPR_GUEST;
    return users[sessions[session_id].user_index].level;
}

// ── Audit ────────────────────────────────────────────────────────────────

void SUPR::Log(SUPRAction action, const char* username, const char* detail) {
    int idx = audit_count % SUPR_MAX_LOG;
    audit_log[idx].action = action;
    scpy(audit_log[idx].username, username, 32);
    scpy(audit_log[idx].detail, detail, 64);
    audit_log[idx].timestamp = Time::GetTicks();
    audit_count++;
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
