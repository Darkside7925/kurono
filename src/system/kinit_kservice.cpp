//  kurono os: kinit .kservice parser.
//
//  parses the ini-style unit files under /kurono/system/services/*.kservice:
//
//    [Service]
//    Name=kpkg-daemon
//    Description=kurono package install daemon
//    Exec=/kurono/system/bin/kpkg --daemon
//    Restart=on-failure
//    RestartDelay=2000
//    After=dbus network
//    WantedBy=user.target
//    Critical=no
//    [Capabilities]
//    Network=yes
//    Filesystem=yes
//    GUI=no
//
//  the parser is allocation-free and bounds every copy; an unknown key or a
//  malformed line is skipped rather than rejected so a future key never breaks
//  an older parser. WantedBy maps onto a KTarget; Restart onto KRestartPolicy.
//  (satoru)

#include "kinit.h"
#include "kpaths.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"

namespace KInit {

namespace {

// ── tiny freestanding string helpers (no libc) (satoru) ──────────────────────
int ks_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

void ks_cpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

bool ks_ieq(const char* a, const char* b) {
    // case-insensitive ascii equality (keys/values are ascii). (satoru)
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

bool ks_truthy(const char* v) {
    return ks_ieq(v, "yes") || ks_ieq(v, "true") || ks_ieq(v, "1") || ks_ieq(v, "on");
}

uint32_t ks_atou(const char* s) {
    uint32_t v = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

// parse a memory size into kib. accepts a bare number (bytes) or a K/M/G suffix
// (case-insensitive, optional trailing 'B'): "64M" -> 65536 kib, "512K" -> 512,
// "1G" -> 1048576. a bare "65536" is treated as bytes (-> 64 kib). (satoru)
uint32_t ks_atomem_kb(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    char suf = *s;
    if (suf >= 'a' && suf <= 'z') suf = (char)(suf - 'a' + 'A');
    uint64_t kb;
    if (suf == 'G')      kb = v * 1024ULL * 1024ULL;
    else if (suf == 'M') kb = v * 1024ULL;
    else if (suf == 'K') kb = v;
    else                 kb = (v + 1023ULL) / 1024ULL;   // bare = bytes (satoru)
    if (kb > 0xFFFFFFFFULL) kb = 0xFFFFFFFFULL;
    return (uint32_t)kb;
}

// parse a CPUQuota value to a percent: "50%" -> 50, "200%" -> 200, "50" -> 50.
// (satoru)
uint32_t ks_atopct(const char* s) {
    return ks_atou(s);   // the trailing '%' is ignored by ks_atou (satoru)
}

KTarget target_from_wantedby(const char* v) {
    // accept "<stage>.target" or a bare "<stage>". (satoru)
    if (ks_ieq(v, "kernel.target")  || ks_ieq(v, "kernel"))  return KTGT_KERNEL;
    if (ks_ieq(v, "network.target") || ks_ieq(v, "network")) return KTGT_NETWORK;
    if (ks_ieq(v, "dbus.target")    || ks_ieq(v, "dbus"))    return KTGT_DBUS;
    if (ks_ieq(v, "desktop.target") || ks_ieq(v, "desktop")) return KTGT_DESKTOP;
    if (ks_ieq(v, "user.target")    || ks_ieq(v, "user"))    return KTGT_USER;
    return KTGT_USER;   // default to the latest stage if unknown (satoru)
}

KRestartPolicy restart_from_str(const char* v) {
    if (ks_ieq(v, "always"))     return KRESTART_ALWAYS;
    if (ks_ieq(v, "on-failure") || ks_ieq(v, "onfailure")) return KRESTART_ON_FAILURE;
    return KRESTART_NO;
}

// trim leading/trailing ascii whitespace in place by adjusting [start,end). (satoru)
void trim(const char* s, int& start, int& end) {
    while (start < end && (s[start] == ' ' || s[start] == '\t' ||
                           s[start] == '\r' || s[start] == '\n')) start++;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
}

}  // namespace

bool ParseKService(const char* text, int len, KService* out) {
    if (!text || !out) return false;

    // zero the struct, then set sane defaults. (satoru)
    for (int i = 0; i < (int)sizeof(KService); i++) ((char*)out)[i] = 0;
    out->kind = KUNIT_PROCESS;       // .kservice files describe real processes (satoru)
    out->type = KTYPE_SIMPLE;
    out->target = KTGT_USER;
    out->restart = KRESTART_NO;
    out->restart_delay_ms = 2000;
    out->enabled = true;
    out->state = KSVC_INACTIVE;
    out->listen_sd = -1;             // sentinel: no socket (satoru)

    bool have_name = false;
    int section = 0;   // 0=none, 1=[Service], 2=[Capabilities], 3=[Limits] (satoru)

    int i = 0;
    while (i < len) {
        // grab one line [line_start, line_end). (satoru)
        int line_start = i;
        while (i < len && text[i] != '\n') i++;
        int line_end = i;
        if (i < len) i++;   // step past the newline (satoru)

        int ls = line_start, le = line_end;
        trim(text, ls, le);
        if (le <= ls) continue;                       // blank line (satoru)
        if (text[ls] == '#' || text[ls] == ';') continue;  // comment (satoru)

        // section header? (satoru)
        if (text[ls] == '[' && text[le - 1] == ']') {
            char sec[24];
            int n = le - 1 - (ls + 1);
            if (n < 0) n = 0;
            if (n > (int)sizeof(sec) - 1) n = (int)sizeof(sec) - 1;
            for (int k = 0; k < n; k++) sec[k] = text[ls + 1 + k];
            sec[n] = 0;
            if (ks_ieq(sec, "Service"))           section = 1;
            else if (ks_ieq(sec, "Capabilities")) section = 2;
            else if (ks_ieq(sec, "Limits"))       section = 3;
            else                                  section = 0;
            continue;
        }

        // key=value. (satoru)
        int eq = -1;
        for (int k = ls; k < le; k++) { if (text[k] == '=') { eq = k; break; } }
        if (eq < 0) continue;

        int ks_s = ls, ks_e = eq;       trim(text, ks_s, ks_e);
        int vs_s = eq + 1, vs_e = le;   trim(text, vs_s, vs_e);

        char key[32]; char val[KINIT_PATH_LEN];
        int kn = ks_e - ks_s; if (kn > (int)sizeof(key) - 1) kn = (int)sizeof(key) - 1;
        for (int k = 0; k < kn; k++) key[k] = text[ks_s + k];
        key[kn] = 0;
        int vn = vs_e - vs_s; if (vn > (int)sizeof(val) - 1) vn = (int)sizeof(val) - 1;
        for (int k = 0; k < vn; k++) val[k] = text[vs_s + k];
        val[vn] = 0;

        if (section == 1) {
            if (ks_ieq(key, "Name"))             { ks_cpy(out->name, val, sizeof(out->name)); have_name = true; }
            else if (ks_ieq(key, "Description"))  ks_cpy(out->description, val, sizeof(out->description));
            else if (ks_ieq(key, "Exec"))         ks_cpy(out->exec, val, sizeof(out->exec));
            else if (ks_ieq(key, "After"))        ks_cpy(out->after, val, sizeof(out->after));
            else if (ks_ieq(key, "Restart"))      out->restart = restart_from_str(val);
            else if (ks_ieq(key, "RestartDelay")) { uint32_t d = ks_atou(val); if (d > 0) out->restart_delay_ms = d; }
            else if (ks_ieq(key, "WantedBy"))     out->target = target_from_wantedby(val);
            else if (ks_ieq(key, "Critical"))     out->critical = ks_truthy(val);
            else if (ks_ieq(key, "Enabled"))      out->enabled = ks_truthy(val);
            else if (ks_ieq(key, "Type")) {
                // "oneshot" marks a run-once unit; "notify" defers "started" until
                // sd_notify READY=1; everything else is a simple daemon. (satoru)
                if (ks_ieq(val, "oneshot"))     out->kind = KUNIT_ONESHOT;
                else if (ks_ieq(val, "notify")) out->type = KTYPE_NOTIFY;
            }
            // resource limits may appear directly in [Service] (systemd style) or
            // in a dedicated [Limits] section; accept both. (satoru)
            else if (ks_ieq(key, "MemoryMax"))   out->limits.memory_max_kb = ks_atomem_kb(val);
            else if (ks_ieq(key, "CPUQuota"))    out->limits.cpu_quota_pct = ks_atopct(val);
            else if (ks_ieq(key, "LimitNOFILE")) out->limits.limit_nofile  = ks_atou(val);
            // socket activation: only start on first connect to this AF_UNIX path. (satoru)
            else if (ks_ieq(key, "ListenStream")) ks_cpy(out->listen_path, val, sizeof(out->listen_path));
            // watchdog: missing WATCHDOG=1 within this many seconds -> kill+restart. (satoru)
            else if (ks_ieq(key, "WatchdogSec"))  out->watchdog_sec = ks_atou(val);
            // conservative true isolation opt-in (only honoured for safe units). (satoru)
            else if (ks_ieq(key, "Isolate"))      out->isolate = ks_truthy(val);
            // per-user session unit owner (also set from the per-user dir scan). (satoru)
            else if (ks_ieq(key, "User"))         ks_cpy(out->owner_user, val, sizeof(out->owner_user));
        } else if (section == 2) {
            if (ks_ieq(key, "Network"))         out->caps.network = ks_truthy(val);
            else if (ks_ieq(key, "Filesystem")) out->caps.filesystem = ks_truthy(val);
            else if (ks_ieq(key, "GUI"))        out->caps.gui = ks_truthy(val);
        } else if (section == 3) {
            if (ks_ieq(key, "MemoryMax"))        out->limits.memory_max_kb = ks_atomem_kb(val);
            else if (ks_ieq(key, "CPUQuota"))    out->limits.cpu_quota_pct = ks_atopct(val);
            else if (ks_ieq(key, "LimitNOFILE")) out->limits.limit_nofile  = ks_atou(val);
        }
    }

    if (!(have_name && out->name[0] != 0)) return false;

    // a unit whose name ends in '@' is a TEMPLATE: it is never started directly,
    // only instantiated as "name@instance". record it so the loader marks it. the
    // trailing '@' is kept in the stored name so InstantiateTemplate can find it.
    // (satoru)
    int nl = ks_len(out->name);
    if (nl > 0 && out->name[nl - 1] == '@') out->is_template = true;
    return true;
}

int LoadServiceDir() {
    // the canonical services dir is /kurono/system/services. ensure it exists so
    // a fresh boot has somewhere to drop unit files. (satoru)
    const char* services_dir = "/kurono/system/services";
    KVFS::Mkdirs(services_dir);

    KVFSNode* entries[64];
    int n = KVFS::Listdir(services_dir, entries, 64);
    if (n <= 0) return 0;

    int registered = 0;
    static char buf[8192];
    char path[256];

    for (int e = 0; e < n; e++) {
        if (!entries[e]) continue;
        const char* fname = entries[e]->name;
        int fl = ks_len(fname);
        // only *.kservice files. (satoru)
        if (fl < 10) continue;
        if (!ks_ieq(fname + fl - 9, ".kservice")) continue;

        // build the full path. (satoru)
        int p = 0;
        for (int k = 0; services_dir[k] && p < (int)sizeof(path) - 1; k++) path[p++] = services_dir[k];
        if (p < (int)sizeof(path) - 1) path[p++] = '/';
        for (int k = 0; fname[k] && p < (int)sizeof(path) - 1; k++) path[p++] = fname[k];
        path[p] = 0;

        int got = KVFS::ReadFile(path, buf, (uint32_t)sizeof(buf) - 1);
        if (got <= 0) continue;
        buf[got] = 0;

        KService svc;
        if (!ParseKService(buf, got, &svc)) continue;

        // register the whole parsed struct so every key (limits, sockets, notify,
        // watchdog, template/isolate flags) is preserved; dedup + live-state reset
        // live in RegisterService. (satoru)
        int idx = RegisterService(&svc);
        if (idx >= 0) {
            registered++;
            SerialLogger::Log("[kinit] loaded .kservice: ");
            SerialLogger::Log(svc.name);
            if (svc.is_template) SerialLogger::Log(" (template)");
            SerialLogger::Log("\r\n");
        }
    }
    return registered;
}

}  // namespace KInit

// end (satoru)
