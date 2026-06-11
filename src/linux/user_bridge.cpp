// ═══════════════════════════════════════════════════════════════════════════
//  Kurono Linux Subsystem — User Bridge Implementation
//  Bidirectional SUPR ↔ Linux user synchronization
// ═══════════════════════════════════════════════════════════════════════════

#include "user_bridge.h"
#include "../security/supr.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"

// Forward declaration
static void GenerateLinuxUserFiles();

// ─── Static storage ──────────────────────────────────────────────────────

UBMappedUser    UserBridge::mapped[UB_MAX_MAPPED_USERS];
int             UserBridge::mapped_count = 0;
UBSyncDirection UserBridge::direction = UB_BIDIRECTIONAL;
UBConflictPolicy UserBridge::conflict_policy = UB_SUPR_WINS;
bool            UserBridge::auto_sync = true;
uint32_t        UserBridge::last_auto_sync = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────

static int ub_slen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}
static void ub_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static bool ub_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}
static int ub_pa(char* out, int pos, int mx, const char* s) {
    while (*s && pos < mx - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}
static int ub_pad(char* out, int pos, int mx, int val) {
    char tmp[12]; int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    else {
        bool neg = val < 0; if (neg) val = -val;
        while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
        if (neg) tmp[i++] = '-';
    }
    for (int j = i - 1; j >= 0 && pos < mx - 1; j--) out[pos++] = tmp[j];
    out[pos] = 0;
    return pos;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

void UserBridge::Init() {
    mapped_count = 0;
    memset(mapped, 0, sizeof(mapped));
    direction = UB_BIDIRECTIONAL;
    conflict_policy = UB_SUPR_WINS;
    auto_sync = true;
    last_auto_sync = 0;

    SerialLogger::Log("[UserBridge] Initialized\r\n");
}

void UserBridge::SetDirection(UBSyncDirection dir)      { direction = dir; }
void UserBridge::SetConflictPolicy(UBConflictPolicy p)  { conflict_policy = p; }
UBSyncDirection  UserBridge::GetDirection()              { return direction; }
UBConflictPolicy UserBridge::GetConflictPolicy()         { return conflict_policy; }

// ═══════════════════════════════════════════════════════════════════════════
//  Passwd file parsing helpers
// ═══════════════════════════════════════════════════════════════════════════

// Parse one line of /etc/passwd format:
// username:x:uid:gid:gecos:home:shell
void UserBridge::ParsePasswdLine(const char* line, char* user, int* uid, int* gid,
                                  char* home, char* shell, int maxstr) {
    int field = 0, pos = 0;
    user[0] = 0; home[0] = 0; shell[0] = 0;
    *uid = -1; *gid = -1;

    while (*line && *line != '\n') {
        if (*line == ':') {
            field++;
            line++;
            pos = 0;
            continue;
        }
        switch (field) {
            case 0:  // username
                if (pos < maxstr - 1) user[pos++] = *line;
                user[pos] = 0;
                break;
            case 1:  // password (skip)
                break;
            case 2:  // uid
                if (*uid < 0) *uid = 0;
                *uid = *uid * 10 + (*line - '0');
                break;
            case 3:  // gid
                if (*gid < 0) *gid = 0;
                *gid = *gid * 10 + (*line - '0');
                break;
            case 4:  // gecos (skip)
                break;
            case 5:  // home
                if (pos < maxstr - 1) home[pos++] = *line;
                home[pos] = 0;
                break;
            case 6:  // shell
                if (pos < maxstr - 1) shell[pos++] = *line;
                shell[pos] = 0;
                break;
        }
        line++;
        pos++;
    }
}

int UserBridge::ParsePasswdUID(const char* line) {
    // Skip to 3rd field (UID)
    int colons = 0;
    while (*line && colons < 2) {
        if (*line == ':') colons++;
        line++;
    }
    int uid = 0;
    while (*line >= '0' && *line <= '9') {
        uid = uid * 10 + (*line - '0');
        line++;
    }
    return uid;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sync operations
// ═══════════════════════════════════════════════════════════════════════════

UBSyncResult UserBridge::SyncToLinux() {
    UBSyncResult result = {0, 0, 0, 0, true};

    SUPRUser* users = SUPR::GetUsers();
    int ucount = SUPR::GetUserCount();

    for (int i = 0; i < ucount; i++) {
        SUPRUser* su = &users[i];
        UBMappedUser* mu = FindMapped(su->username);

        if (!mu) {
            // New user — add mapping
            if (mapped_count >= UB_MAX_MAPPED_USERS) continue;
            mu = &mapped[mapped_count++];
            memset(mu, 0, sizeof(UBMappedUser));
            ub_scpy(mu->username, su->username, sizeof(mu->username));
            mu->supr_uid = su->uid > 0 ? su->uid : 1000 + i;
            mu->linux_uid = mu->supr_uid;  // Same UID by default
            mu->supr_gid = su->gid > 0 ? su->gid : 1000 + i;
            mu->linux_gid = mu->supr_gid;

            // Build home dir path
            ub_scpy(mu->home, "/home/", sizeof(mu->home));
            int hl = ub_slen(mu->home);
            ub_scpy(mu->home + hl, su->username, (int)(sizeof(mu->home) - hl));

            ub_scpy(mu->shell, su->shell[0] ? su->shell : "/bin/bash",
                    sizeof(mu->shell));
            mu->synced = true;
            mu->last_sync = Time::GetTicks();
            mu->conflict = false;

            result.users_added++;
        } else {
            // Existing mapping — update if changed
            bool changed = false;
            if (su->uid > 0 && su->uid != mu->supr_uid) {
                mu->supr_uid = su->uid;
                changed = true;
            }
            if (su->gid > 0 && su->gid != mu->supr_gid) {
                mu->supr_gid = su->gid;
                changed = true;
            }
            if (changed) {
                mu->linux_uid = mu->supr_uid;
                mu->linux_gid = mu->supr_gid;
                mu->last_sync = Time::GetTicks();
                result.users_updated++;
            }
        }

        // Ensure home directory exists in KVFS
        KVFS::Mkdirs(mu->home);
    }

    // Generate /linux/etc/passwd, /linux/etc/group, /linux/etc/shadow
    GenerateLinuxUserFiles();

    SerialLogger::Log("[UserBridge] Synced ");
    SerialLogger::LogDec(ucount);
    SerialLogger::Log(" users to Linux\r\n");

    return result;
}

UBSyncResult UserBridge::SyncFromLinux() {
    UBSyncResult result = {0, 0, 0, 0, true};

    char buf[4096];
    if (KVFS::ReadString("/linux/etc/passwd", buf, sizeof(buf)) <= 0) {
        result.success = false;
        return result;
    }

    // Parse each line
    const char* line = buf;
    while (*line) {
        // Find end of line
        const char* eol = line;
        while (*eol && *eol != '\n') eol++;

        // Parse this line
        char username[32], home[64], shell[32];
        int uid = -1, gid = -1;
        // Copy line to temp buffer
        char tmp[256];
        int len = (int)(eol - line);
        if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, line, len);
        tmp[len] = 0;

        ParsePasswdLine(tmp, username, &uid, &gid, home, shell, 32);

        // Only import regular users (UID >= 1000)
        if (uid >= 1000 && username[0]) {
            // Check if this user exists in SUPR
            if (!SUPR::FindUser(username)) {
                // Create in SUPR
                SUPR::CreateUser(username, "kurono", SUPR_USER);
                result.users_added++;

                SerialLogger::Log("[UserBridge] Imported from Linux: ");
                SerialLogger::Log(username);
                SerialLogger::Log("\r\n");
            }

            // Update or create mapping
            UBMappedUser* mu = FindMapped(username);
            if (!mu && mapped_count < UB_MAX_MAPPED_USERS) {
                mu = &mapped[mapped_count++];
                memset(mu, 0, sizeof(UBMappedUser));
                ub_scpy(mu->username, username, sizeof(mu->username));
            }
            if (mu) {
                mu->linux_uid = uid;
                mu->linux_gid = gid;
                ub_scpy(mu->home, home, sizeof(mu->home));
                ub_scpy(mu->shell, shell, sizeof(mu->shell));
                mu->synced = true;
                mu->last_sync = Time::GetTicks();

                // Check for UID conflict
                if (mu->supr_uid > 0 && mu->supr_uid != mu->linux_uid) {
                    if (conflict_policy == UB_SUPR_WINS) {
                        mu->linux_uid = mu->supr_uid;
                    } else if (conflict_policy == UB_LINUX_WINS) {
                        mu->supr_uid = mu->linux_uid;
                    } else if (conflict_policy == UB_MANUAL) {
                        mu->conflict = true;
                        result.conflicts++;
                    }
                }
            }
        }

        // Move to next line
        line = eol;
        if (*line == '\n') line++;
    }

    return result;
}

UBSyncResult UserBridge::Sync() {
    UBSyncResult combined = {0, 0, 0, 0, true};

    if (direction == UB_SUPR_TO_LINUX || direction == UB_BIDIRECTIONAL) {
        UBSyncResult r = SyncToLinux();
        combined.users_added += r.users_added;
        combined.users_updated += r.users_updated;
        combined.conflicts += r.conflicts;
        if (!r.success) combined.success = false;
    }

    if (direction == UB_LINUX_TO_SUPR || direction == UB_BIDIRECTIONAL) {
        UBSyncResult r = SyncFromLinux();
        combined.users_added += r.users_added;
        combined.users_updated += r.users_updated;
        combined.conflicts += r.conflicts;
        if (!r.success) combined.success = false;
    }

    return combined;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Single user operations
// ═══════════════════════════════════════════════════════════════════════════

bool UserBridge::SyncUser(const char* username) {
    SUPRUser* su = SUPR::FindUser(username);
    UBMappedUser* mu = FindMapped(username);

    if (!su && !mu) return false;

    if (su && !mu) {
        // SUPR user not yet mapped
        if (mapped_count >= UB_MAX_MAPPED_USERS) return false;
        mu = &mapped[mapped_count++];
        memset(mu, 0, sizeof(UBMappedUser));
        ub_scpy(mu->username, username, sizeof(mu->username));
        mu->supr_uid = su->uid > 0 ? su->uid : 1000;
        mu->linux_uid = mu->supr_uid;
        mu->supr_gid = su->gid > 0 ? su->gid : 1000;
        mu->linux_gid = mu->supr_gid;
        ub_scpy(mu->home, "/home/", sizeof(mu->home));
        int hl = ub_slen(mu->home);
        ub_scpy(mu->home + hl, username, (int)(sizeof(mu->home) - hl));
        ub_scpy(mu->shell, su->shell[0] ? su->shell : "/bin/bash",
                sizeof(mu->shell));
    }

    if (mu) {
        mu->synced = true;
        mu->last_sync = Time::GetTicks();
        KVFS::Mkdirs(mu->home);
    }

    GenerateLinuxUserFiles();
    return true;
}

bool UserBridge::MapUser(const char* username, int linux_uid, int linux_gid) {
    UBMappedUser* mu = FindMapped(username);
    if (!mu) {
        if (mapped_count >= UB_MAX_MAPPED_USERS) return false;
        mu = &mapped[mapped_count++];
        memset(mu, 0, sizeof(UBMappedUser));
        ub_scpy(mu->username, username, sizeof(mu->username));
    }
    mu->linux_uid = linux_uid;
    mu->linux_gid = linux_gid;
    mu->synced = true;
    mu->last_sync = Time::GetTicks();
    return true;
}

bool UserBridge::UnmapUser(const char* username) {
    for (int i = 0; i < mapped_count; i++) {
        if (ub_seq(mapped[i].username, username)) {
            // Shift remaining entries
            for (int j = i; j < mapped_count - 1; j++) {
                memcpy(&mapped[j], &mapped[j + 1], sizeof(UBMappedUser));
            }
            mapped_count--;
            return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Query
// ═══════════════════════════════════════════════════════════════════════════

UBMappedUser* UserBridge::FindMapped(const char* username) {
    for (int i = 0; i < mapped_count; i++) {
        if (ub_seq(mapped[i].username, username)) return &mapped[i];
    }
    return nullptr;
}

int UserBridge::GetLinuxUID(const char* username) {
    UBMappedUser* m = FindMapped(username);
    return m ? m->linux_uid : -1;
}

int UserBridge::GetSUPRUID(int linux_uid) {
    for (int i = 0; i < mapped_count; i++) {
        if (mapped[i].linux_uid == linux_uid) return mapped[i].supr_uid;
    }
    return -1;
}

UBMappedUser* UserBridge::GetMappedUsers() { return mapped; }
int UserBridge::GetMappedCount() { return mapped_count; }

// ═══════════════════════════════════════════════════════════════════════════
//  Conflict resolution
// ═══════════════════════════════════════════════════════════════════════════

bool UserBridge::HasConflicts() {
    for (int i = 0; i < mapped_count; i++) {
        if (mapped[i].conflict) return true;
    }
    return false;
}

int UserBridge::ResolveConflict(const char* username, UBSyncDirection winner) {
    UBMappedUser* mu = FindMapped(username);
    if (!mu || !mu->conflict) return -1;

    if (winner == UB_SUPR_TO_LINUX) {
        mu->linux_uid = mu->supr_uid;
        mu->linux_gid = mu->supr_gid;
    } else {
        mu->supr_uid = mu->linux_uid;
        mu->supr_gid = mu->linux_gid;
    }
    mu->conflict = false;
    mu->last_sync = Time::GetTicks();

    GenerateLinuxUserFiles();
    return 0;
}

void UserBridge::DumpStatus(char* out, int max_out) {
    int p = 0;
    p = ub_pa(out, p, max_out, "User Bridge Status\n");
    p = ub_pa(out, p, max_out, "══════════════════════════════════════\n");
    p = ub_pa(out, p, max_out, "Direction:  ");
    switch (direction) {
        case UB_SUPR_TO_LINUX: p = ub_pa(out, p, max_out, "SUPR → Linux\n"); break;
        case UB_LINUX_TO_SUPR: p = ub_pa(out, p, max_out, "Linux → SUPR\n"); break;
        case UB_BIDIRECTIONAL: p = ub_pa(out, p, max_out, "Bidirectional\n"); break;
    }
    p = ub_pa(out, p, max_out, "Policy:     ");
    switch (conflict_policy) {
        case UB_SUPR_WINS:    p = ub_pa(out, p, max_out, "SUPR wins\n"); break;
        case UB_LINUX_WINS:   p = ub_pa(out, p, max_out, "Linux wins\n"); break;
        case UB_NEWEST_WINS:  p = ub_pa(out, p, max_out, "Newest wins\n"); break;
        case UB_MANUAL:       p = ub_pa(out, p, max_out, "Manual\n"); break;
    }
    p = ub_pa(out, p, max_out, "Auto-sync:  ");
    p = ub_pa(out, p, max_out, auto_sync ? "enabled\n" : "disabled\n");
    p = ub_pa(out, p, max_out, "Mapped:     ");
    p = ub_pad(out, p, max_out, mapped_count);
    p = ub_pa(out, p, max_out, " users\n");
    p = ub_pa(out, p, max_out, "──────────────────────────────────────\n");

    for (int i = 0; i < mapped_count; i++) {
        UBMappedUser* m = &mapped[i];
        p = ub_pa(out, p, max_out, "  ");
        p = ub_pa(out, p, max_out, m->username);
        int nm = ub_slen(m->username);
        for (int j = nm; j < 16; j++) p = ub_pa(out, p, max_out, " ");
        p = ub_pa(out, p, max_out, " SUPR:");
        p = ub_pad(out, p, max_out, m->supr_uid);
        p = ub_pa(out, p, max_out, " Linux:");
        p = ub_pad(out, p, max_out, m->linux_uid);
        if (m->conflict) p = ub_pa(out, p, max_out, " [CONFLICT]");
        if (!m->synced)  p = ub_pa(out, p, max_out, " [UNSYNCED]");
        p = ub_pa(out, p, max_out, "\n");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Auto-sync tick
// ═══════════════════════════════════════════════════════════════════════════

void UserBridge::SetAutoSync(bool enabled) { auto_sync = enabled; }

void UserBridge::Tick(uint32_t now_ms) {
    if (!auto_sync) return;
    if (now_ms - last_auto_sync < UB_SYNC_INTERVAL) return;

    last_auto_sync = now_ms;
    Sync();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Internal: generate /linux/etc/{passwd,group,shadow} from mapped users
// ═══════════════════════════════════════════════════════════════════════════

static void GenerateLinuxUserFiles() {
    char buf[4096];
    int p = 0;

    // ── /etc/passwd ──
    p = 0;
    p = ub_pa(buf, p, sizeof(buf), "root:x:0:0:root:/root:/bin/bash\n");
    p = ub_pa(buf, p, sizeof(buf), "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n");
    p = ub_pa(buf, p, sizeof(buf), "bin:x:2:2:bin:/bin:/usr/sbin/nologin\n");
    p = ub_pa(buf, p, sizeof(buf), "sys:x:3:3:sys:/dev:/usr/sbin/nologin\n");
    p = ub_pa(buf, p, sizeof(buf), "nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n");

    UBMappedUser* mapped = UserBridge::GetMappedUsers();
    int count = UserBridge::GetMappedCount();
    for (int i = 0; i < count; i++) {
        UBMappedUser* m = &mapped[i];
        if (ub_seq(m->username, "root")) continue;
        p = ub_pa(buf, p, sizeof(buf), m->username);
        p = ub_pa(buf, p, sizeof(buf), ":x:");
        p = ub_pad(buf, p, sizeof(buf), m->linux_uid);
        p = ub_pa(buf, p, sizeof(buf), ":");
        p = ub_pad(buf, p, sizeof(buf), m->linux_gid);
        p = ub_pa(buf, p, sizeof(buf), ":");
        p = ub_pa(buf, p, sizeof(buf), m->username);
        p = ub_pa(buf, p, sizeof(buf), ":");
        p = ub_pa(buf, p, sizeof(buf), m->home);
        p = ub_pa(buf, p, sizeof(buf), ":");
        p = ub_pa(buf, p, sizeof(buf), m->shell[0] ? m->shell : "/bin/bash");
        p = ub_pa(buf, p, sizeof(buf), "\n");
    }
    KVFS::WriteString("/linux/etc/passwd", buf);

    // ── /etc/group ──
    p = 0;
    p = ub_pa(buf, p, sizeof(buf), "root:x:0:\n");
    p = ub_pa(buf, p, sizeof(buf), "daemon:x:1:\n");
    p = ub_pa(buf, p, sizeof(buf), "bin:x:2:\n");
    p = ub_pa(buf, p, sizeof(buf), "sys:x:3:\n");
    p = ub_pa(buf, p, sizeof(buf), "adm:x:4:\n");
    p = ub_pa(buf, p, sizeof(buf), "sudo:x:27:\n");
    p = ub_pa(buf, p, sizeof(buf), "users:x:100:\n");
    p = ub_pa(buf, p, sizeof(buf), "nogroup:x:65534:\n");

    for (int i = 0; i < count; i++) {
        UBMappedUser* m = &mapped[i];
        if (ub_seq(m->username, "root")) continue;
        p = ub_pa(buf, p, sizeof(buf), m->username);
        p = ub_pa(buf, p, sizeof(buf), ":x:");
        p = ub_pad(buf, p, sizeof(buf), m->linux_gid);
        p = ub_pa(buf, p, sizeof(buf), ":\n");
    }
    KVFS::WriteString("/linux/etc/group", buf);

    // ── /etc/shadow ──
    p = 0;
    p = ub_pa(buf, p, sizeof(buf), "root:!:19723:0:99999:7:::\n");
    p = ub_pa(buf, p, sizeof(buf), "daemon:*:19723:0:99999:7:::\n");
    p = ub_pa(buf, p, sizeof(buf), "nobody:*:19723:0:99999:7:::\n");

    for (int i = 0; i < count; i++) {
        UBMappedUser* m = &mapped[i];
        if (ub_seq(m->username, "root")) continue;
        p = ub_pa(buf, p, sizeof(buf), m->username);
        p = ub_pa(buf, p, sizeof(buf), ":$6$kurono$synced:19723:0:99999:7:::\n");
    }
    KVFS::WriteString("/linux/etc/shadow", buf);
}
