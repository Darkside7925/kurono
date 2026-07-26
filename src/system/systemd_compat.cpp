//  kurono os: systemd compatibility layer: implementation.
//
//  see systemd_compat.h for the design. the short version: this shim translates
//  systemd-shaped probes (systemctl / journalctl / loginctl / the
//  org.freedesktop.systemd1 d-bus api / the /run/systemd tree) onto kinit, which
//  is kurono's real service manager. it never touches kinit's core: it drives
//  kinit through its public control api (StartService/StopService/RestartService/
//  Reload) and reads kinit state read-only (GetServices/FindService/StateName).
//  (satoru)

#include "systemd_compat.h"
#include "kinit.h"
#include "kpaths.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../security/supr.h"
#include "../shell/shell.h"

namespace SystemdCompat {

// kinit's per-unit name buffer length, hoisted into this namespace so the local
// fixed-size buffers below match kinit's. (satoru)
using KInit::KINIT_NAME_LEN;

namespace {

// ── tiny freestanding string helpers (no libc) (satoru) ──────────────────────
int sc_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

bool sc_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

bool sc_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

void sc_cpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

int sc_cat(char* out, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) out[p++] = *s++;
    if (p < mx) out[p] = 0;
    return p;
}

int sc_cat_u(char* out, int p, int mx, uint32_t v) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < 12) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && p < mx - 1) out[p++] = tmp[--n];
    if (p < mx) out[p] = 0;
    return p;
}

// does substring `needle` occur anywhere in `hay`? (satoru)
bool sc_contains(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

// strip a trailing ".service" (or ".target"/".socket") suffix from `unit` into
// `out`. an app often passes "firefox.service"; kinit knows it as "firefox".
// (satoru)
void strip_unit_suffix(const char* unit, char* out, int mx) {
    sc_cpy(out, unit, mx);
    int n = sc_len(out);
    const char* sfx[] = { ".service", ".target", ".socket", ".mount", nullptr };
    for (int i = 0; sfx[i]; i++) {
        int sl = sc_len(sfx[i]);
        if (n > sl) {
            bool m = true;
            for (int k = 0; k < sl; k++) if (out[n - sl + k] != sfx[i][k]) { m = false; break; }
            if (m) { out[n - sl] = 0; return; }
        }
    }
}

// map a kinit KServiceState onto the systemd ActiveState string. (satoru)
const char* active_state(KInit::KServiceState s) {
    switch (s) {
        case KInit::KSVC_INACTIVE:   return "inactive";
        case KInit::KSVC_STARTING:   return "activating";
        case KInit::KSVC_RUNNING:    return "active";
        case KInit::KSVC_STOPPING:   return "deactivating";
        case KInit::KSVC_STOPPED:    return "inactive";
        case KInit::KSVC_FAILED:     return "failed";
        case KInit::KSVC_RESTARTING: return "activating";
        default:                     return "inactive";
    }
}

// map a kinit KServiceState onto the systemd SubState string. (satoru)
const char* sub_state(KInit::KServiceState s) {
    switch (s) {
        case KInit::KSVC_INACTIVE:   return "dead";
        case KInit::KSVC_STARTING:   return "start";
        case KInit::KSVC_RUNNING:    return "running";
        case KInit::KSVC_STOPPING:   return "stop";
        case KInit::KSVC_STOPPED:    return "dead";
        case KInit::KSVC_FAILED:     return "failed";
        case KInit::KSVC_RESTARTING: return "auto-restart";
        default:                     return "dead";
    }
}

// systemd LoadState: a registered kinit unit is always "loaded". (satoru)
const char* load_state(const KInit::KService*) { return "loaded"; }

// find a kinit service by an app-supplied unit name, tolerating a ".service"
// (etc) suffix. returns null if not registered. (satoru)
KInit::KService* find_unit(const char* unit) {
    KInit::KService* s = KInit::FindService(unit);
    if (s) return s;
    char base[KINIT_NAME_LEN];
    strip_unit_suffix(unit, base, sizeof(base));
    return KInit::FindService(base);
}

// resolve the canonical kinit name for an app-supplied unit (suffix-stripped if
// the bare name is registered). writes into `out`. (satoru)
void resolve_kinit_name(const char* unit, char* out, int mx) {
    if (KInit::FindService(unit)) { sc_cpy(out, unit, mx); return; }
    strip_unit_suffix(unit, out, mx);
}

// ── the current login user (for loginctl) (satoru) ───────────────────────────
const char* current_user() {
    int sid = SUPR::GetCurrentSession();
    if (sid >= 0) {
        SUPRSession* sess = SUPR::GetSession(sid);
        if (sess && sess->active) {
            SUPRUser* users = SUPR::GetUsers();
            int n = SUPR::GetUserCount();
            if (users && sess->user_index >= 0 && sess->user_index < n)
                return users[sess->user_index].username;
        }
    }
    return "user";   // honest fallback: kurono autologs a default user (satoru)
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  1. /run/systemd runtime tree
// ═════════════════════════════════════════════════════════════════════════════
void InitRuntime() {
    // the canonical live-state root is /kurono/runtime (KP_RUNTIME); /run is a
    // compat symlink onto it elsewhere. create the systemd tree under the real
    // path AND ensure /run resolves so a probe of either spelling works. an app
    // that stats /run/systemd/system to decide "is this a systemd box" must find
    // a directory there. (satoru)
    KVFS::Mkdirs("/run");
    KVFS::Mkdirs("/run/systemd");
    KVFS::Mkdirs("/run/systemd/system");      // sd_booted() probes THIS dir (satoru)
    KVFS::Mkdirs("/run/systemd/private");      // the private control socket dir (satoru)
    KVFS::Mkdirs("/run/systemd/units");        // per-unit runtime state (satoru)
    KVFS::Mkdirs("/run/systemd/notify");       // sd_notify datagram dir (satoru)

    // also materialise it under the canonical runtime root so code that uses the
    // kurono-native path sees the same tree. (satoru)
    KVFS::Mkdirs(KP_RUNTIME "/systemd/system");

    // a machine-id is the other classic systemd presence marker; apps read it via
    // sd_id128_get_machine(). drop a stable 32-hex-char id + newline. (satoru)
    if (!KVFS::Exists("/run/machine-id"))
        KVFS::WriteString("/run/machine-id", "kur0n0kur0n0kur0n0kur0n0kur0n000\n");
    if (!KVFS::Exists("/etc/machine-id"))
        KVFS::WriteString("/etc/machine-id", "kur0n0kur0n0kur0n0kur0n0kur0n000\n");

    // seed a real-world systemd .service file under the linux unit dir and run it
    // through the converter once, so the .service -> .kservice path is exercised
    // on every boot and there is a converted-from-systemd unit visible to
    // systemctl/journalctl/ListUnits. modeled on a stock daemon unit (dbus-style)
    // with [Unit]/[Service]/[Install] sections, After/Requires deps, ExecStart,
    // Restart, RestartSec and WantedBy, exactly as a packaged app ships. it is a
    // oneshot so it completes immediately and never crash-loops on a box without
    // the real binary. only written if absent (never clobbers a user's edit), and
    // the install is skipped once the converted kinit unit already exists.
    // (satoru)
    const char* sample_unit_path = "/run/systemd/system/kurono-systemd-probe.service";
    const char* sample_unit =
        "[Unit]\n"
        "Description=Kurono systemd-compat probe (converted from a .service unit)\n"
        "Documentation=man:systemd(1)\n"
        "After=dbus.service network.target\n"
        "Requires=dbus.service\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=/bin/true\n"
        "Restart=on-failure\n"
        "RestartSec=3\n"
        "RemainAfterExit=yes\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";
    if (!KVFS::Exists(sample_unit_path))
        KVFS::WriteString(sample_unit_path, sample_unit);
    if (!KInit::FindService("kurono-systemd-probe"))
        InstallServiceUnit("kurono-systemd-probe.service", sample_unit, sc_len(sample_unit));

    SerialLogger::Log("[systemd-compat] /run/systemd tree ready (kinit-backed)\r\n");
}

// ═════════════════════════════════════════════════════════════════════════════
//  3. .service unit parser + .kservice converter
// ═════════════════════════════════════════════════════════════════════════════
namespace {

// the parsed systemd unit, only the keys we map. (satoru)
struct ParsedUnit {
    char description[96];
    char after[160];          // After= + Requires= + Wants= names, space-joined (satoru)
    char exec_start[200];      // ExecStart= (leading +-!@ prefixes stripped) (satoru)
    char type[24];             // simple|forking|oneshot|notify|dbus|idle (satoru)
    char restart[24];          // no|always|on-failure|on-abnormal|... (satoru)
    char wanted_by[64];        // WantedBy= target (satoru)
    uint32_t restart_sec_ms;   // RestartSec= -> ms (satoru)
    bool have_exec;
};

// append `tok` (a unit name) to the space-separated dep list `dst`, stripping a
// trailing .service/.target so kinit's after= (which uses bare names) matches.
// dedups trivially by skipping if already present. (satoru)
void append_dep(char* dst, int mx, const char* tok, int tok_len) {
    if (tok_len <= 0) return;
    char one[64];
    int n = tok_len; if (n > (int)sizeof(one) - 1) n = (int)sizeof(one) - 1;
    for (int i = 0; i < n; i++) one[i] = tok[i];
    one[n] = 0;
    char base[64];
    strip_unit_suffix(one, base, sizeof(base));
    if (!base[0]) return;
    // already present? (satoru)
    if (sc_contains(dst, base)) return;
    int p = sc_len(dst);
    if (p > 0 && p < mx - 1) dst[p++] = ' ';
    sc_cat(dst, p, mx, base);
}

// split a whitespace/comma list (After=/Requires=/Wants= value) into dep tokens
// and append each. (satoru)
void append_dep_list(char* dst, int mx, const char* val) {
    int i = 0;
    while (val[i]) {
        while (val[i] == ' ' || val[i] == '\t' || val[i] == ',') i++;
        if (!val[i]) break;
        int s = i;
        while (val[i] && val[i] != ' ' && val[i] != '\t' && val[i] != ',') i++;
        append_dep(dst, mx, val + s, i - s);
    }
}

uint32_t parse_uint(const char* s) {
    uint32_t v = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

void trim2(const char* s, int& start, int& end) {
    while (start < end && (s[start] == ' ' || s[start] == '\t' ||
                           s[start] == '\r' || s[start] == '\n')) start++;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
}

// strip the systemd ExecStart special prefixes (@ - + ! !! :) from the front of
// a command line; kinit's Exec= is a plain argv string. (satoru)
const char* strip_exec_prefix(const char* v) {
    while (*v == '@' || *v == '-' || *v == '+' || *v == '!' || *v == ':') v++;
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

// parse a systemd .service text into `out`. sections: [Unit], [Service],
// [Install]. unknown keys/sections are ignored (forward-compatible). (satoru)
bool parse_service_unit(const char* text, int len, ParsedUnit* out) {
    for (int i = 0; i < (int)sizeof(ParsedUnit); i++) ((char*)out)[i] = 0;
    out->restart_sec_ms = 2000;   // sane default backoff (systemd's is 100ms but
                                   // kinit's floor is 2s; keep the larger) (satoru)

    int section = 0;   // 0=none 1=[Unit] 2=[Service] 3=[Install] (satoru)
    int i = 0;
    while (i < len) {
        int ls = i;
        while (i < len && text[i] != '\n') i++;
        int le = i;
        if (i < len) i++;
        int s = ls, e = le;
        trim2(text, s, e);
        if (e <= s) continue;
        if (text[s] == '#' || text[s] == ';') continue;

        if (text[s] == '[' && text[e - 1] == ']') {
            char sec[24];
            int n = e - 1 - (s + 1);
            if (n < 0) n = 0;
            if (n > (int)sizeof(sec) - 1) n = (int)sizeof(sec) - 1;
            for (int k = 0; k < n; k++) sec[k] = text[s + 1 + k];
            sec[n] = 0;
            if      (sc_ieq(sec, "Unit"))    section = 1;
            else if (sc_ieq(sec, "Service")) section = 2;
            else if (sc_ieq(sec, "Install")) section = 3;
            else                              section = 0;
            continue;
        }

        int eq = -1;
        for (int k = s; k < e; k++) if (text[k] == '=') { eq = k; break; }
        if (eq < 0) continue;
        int ks = s, ke = eq;       trim2(text, ks, ke);
        int vs = eq + 1, ve = le;  trim2(text, vs, ve);

        char key[40]; char val[200];
        int kn = ke - ks; if (kn > (int)sizeof(key) - 1) kn = (int)sizeof(key) - 1;
        for (int k = 0; k < kn; k++) key[k] = text[ks + k];
        key[kn] = 0;
        int vn = ve - vs; if (vn > (int)sizeof(val) - 1) vn = (int)sizeof(val) - 1;
        for (int k = 0; k < vn; k++) val[k] = text[vs + k];
        val[vn] = 0;

        if (section == 1) {                    // [Unit] (satoru)
            if      (sc_ieq(key, "Description")) sc_cpy(out->description, val, sizeof(out->description));
            else if (sc_ieq(key, "After"))      append_dep_list(out->after, sizeof(out->after), val);
            else if (sc_ieq(key, "Requires"))   append_dep_list(out->after, sizeof(out->after), val);
            else if (sc_ieq(key, "Wants"))      append_dep_list(out->after, sizeof(out->after), val);
            else if (sc_ieq(key, "Requisite"))  append_dep_list(out->after, sizeof(out->after), val);
        } else if (section == 2) {             // [Service] (satoru)
            if (sc_ieq(key, "ExecStart")) {
                // systemd allows ExecStart= (empty resets); a real command sets it.
                // (satoru)
                const char* cmd = strip_exec_prefix(val);
                if (cmd[0]) { sc_cpy(out->exec_start, cmd, sizeof(out->exec_start)); out->have_exec = true; }
            }
            else if (sc_ieq(key, "Type"))        sc_cpy(out->type, val, sizeof(out->type));
            else if (sc_ieq(key, "Restart"))     sc_cpy(out->restart, val, sizeof(out->restart));
            else if (sc_ieq(key, "RestartSec")) {
                // accept a bare seconds integer (systemd also allows "5min" etc;
                // we map a plain number of seconds, defaulting otherwise). (satoru)
                uint32_t sec = parse_uint(val);
                if (sec > 0) out->restart_sec_ms = sec * 1000;
            }
        } else if (section == 3) {             // [Install] (satoru)
            if (sc_ieq(key, "WantedBy")) sc_cpy(out->wanted_by, val, sizeof(out->wanted_by));
        }
    }
    return true;
}

// map a systemd Restart= onto a kinit Restart= token. systemd has many values
// (on-failure/on-abnormal/on-abort/on-watchdog/always); kinit has no|on-failure|
// always. anything crash-triggered maps to on-failure; "always" stays always.
// (satoru)
const char* map_restart(const char* r) {
    if (!r[0] || sc_ieq(r, "no") || sc_ieq(r, "never")) return "no";
    if (sc_ieq(r, "always")) return "always";
    return "on-failure";   // on-failure/on-abnormal/on-abort/on-watchdog/on-success (satoru)
}

// map a systemd WantedBy target onto a kinit boot target. (satoru)
const char* map_target(const char* w) {
    if (sc_contains(w, "network")) return "network.target";
    if (sc_contains(w, "graphical") || sc_contains(w, "desktop")) return "desktop.target";
    if (sc_contains(w, "multi-user") || sc_contains(w, "default")) return "user.target";
    if (sc_contains(w, "basic") || sc_contains(w, "sysinit")) return "user.target";
    return "user.target";   // the latest stage is the safe default (satoru)
}

// is this systemd Type a oneshot? (satoru)
bool is_oneshot(const char* t) { return sc_ieq(t, "oneshot"); }

}  // namespace

bool InstallServiceUnit(const char* unit_name, const char* text, int len) {
    if (!unit_name || !text || len <= 0) return false;

    ParsedUnit pu;
    if (!parse_service_unit(text, len, &pu)) return false;

    // the kinit unit name: the .service base name, suffix stripped. (satoru)
    char name[KINIT_NAME_LEN];
    strip_unit_suffix(unit_name, name, sizeof(name));
    if (!name[0]) return false;

    // build the .kservice text. (satoru)
    char ks[1024];
    int p = 0;
    p = sc_cat(ks, p, (int)sizeof(ks),
               "# generated by the kurono systemd-compat shim from a .service unit.\n"
               "# source: ");
    p = sc_cat(ks, p, (int)sizeof(ks), unit_name);
    p = sc_cat(ks, p, (int)sizeof(ks), "\n[Service]\nName=");
    p = sc_cat(ks, p, (int)sizeof(ks), name);
    p = sc_cat(ks, p, (int)sizeof(ks), "\nDescription=");
    p = sc_cat(ks, p, (int)sizeof(ks), pu.description[0] ? pu.description : name);
    if (pu.have_exec) {
        p = sc_cat(ks, p, (int)sizeof(ks), "\nExec=");
        p = sc_cat(ks, p, (int)sizeof(ks), pu.exec_start);
    }
    p = sc_cat(ks, p, (int)sizeof(ks), "\nRestart=");
    p = sc_cat(ks, p, (int)sizeof(ks), map_restart(pu.restart));
    p = sc_cat(ks, p, (int)sizeof(ks), "\nRestartDelay=");
    p = sc_cat_u(ks, p, (int)sizeof(ks), pu.restart_sec_ms);
    if (pu.after[0]) {
        p = sc_cat(ks, p, (int)sizeof(ks), "\nAfter=");
        p = sc_cat(ks, p, (int)sizeof(ks), pu.after);
    }
    p = sc_cat(ks, p, (int)sizeof(ks), "\nWantedBy=");
    p = sc_cat(ks, p, (int)sizeof(ks), map_target(pu.wanted_by));
    if (is_oneshot(pu.type))
        p = sc_cat(ks, p, (int)sizeof(ks), "\nType=oneshot");
    // a converted linux unit is a real user process: grant the standard caps so
    // its capability gate passes for a logged-in user. (satoru)
    p = sc_cat(ks, p, (int)sizeof(ks),
               "\n[Capabilities]\nNetwork=yes\nFilesystem=yes\nGUI=yes\n");
    (void)p;

    // write it into the kinit services dir, then reload so kinit registers it.
    // (satoru)
    KVFS::Mkdirs("/kurono/system/services");
    char path[256];
    int q = 0;
    q = sc_cat(path, q, (int)sizeof(path), "/kurono/system/services/");
    q = sc_cat(path, q, (int)sizeof(path), name);
    q = sc_cat(path, q, (int)sizeof(path), ".kservice");
    if (KVFS::WriteString(path, ks) < 0) return false;

    KInit::Reload();

    SerialLogger::Log("[systemd-compat] installed unit ");
    SerialLogger::Log(unit_name);
    SerialLogger::Log(" -> ");
    SerialLogger::Log(path);
    SerialLogger::Log("\r\n");
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  2. systemctl shim
// ═════════════════════════════════════════════════════════════════════════════
namespace {

// render a `systemctl status <unit>` block. (satoru)
int status_block(char* out, int p, int mx, const char* unit, KInit::KService* s) {
    char base[KINIT_NAME_LEN];
    strip_unit_suffix(unit, base, sizeof(base));

    // header line: "● <unit>.service - <desc>" (satoru)
    p = sc_cat(out, p, mx, s ? (active_state(s->state)[0] == 'a' ? "* " : "x ") : "x ");
    p = sc_cat(out, p, mx, base);
    p = sc_cat(out, p, mx, ".service - ");
    p = sc_cat(out, p, mx, (s && s->description[0]) ? s->description : base);
    p = sc_cat(out, p, mx, "\n");

    if (!s) {
        p = sc_cat(out, p, mx, "     Loaded: not-found (Reason: no such unit known to kinit)\n");
        p = sc_cat(out, p, mx, "     Active: inactive (dead)\n");
        return p;
    }

    p = sc_cat(out, p, mx, "     Loaded: loaded (/kurono/system/services/");
    p = sc_cat(out, p, mx, base);
    p = sc_cat(out, p, mx, ".kservice; ");
    p = sc_cat(out, p, mx, s->enabled ? "enabled" : "disabled");
    p = sc_cat(out, p, mx, ")\n");

    p = sc_cat(out, p, mx, "     Active: ");
    p = sc_cat(out, p, mx, active_state(s->state));
    p = sc_cat(out, p, mx, " (");
    p = sc_cat(out, p, mx, sub_state(s->state));
    p = sc_cat(out, p, mx, ")\n");

    if (s->pid > 0) {
        p = sc_cat(out, p, mx, "   Main PID: ");
        p = sc_cat_u(out, p, mx, (uint32_t)s->pid);
        p = sc_cat(out, p, mx, "\n");
    }
    if (s->exec[0]) {
        p = sc_cat(out, p, mx, "    CGroup: ");
        p = sc_cat(out, p, mx, s->exec);
        p = sc_cat(out, p, mx, "\n");
    }
    if (s->crash_count > 0) {
        p = sc_cat(out, p, mx, "   Restarts: ");
        p = sc_cat_u(out, p, mx, (uint32_t)s->crash_count);
        p = sc_cat(out, p, mx, "\n");
    }
    return p;
}

}  // namespace

int CmdSystemctl(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;

    // bare `systemctl` or `systemctl list-units` / `status` -> the unit table.
    // (satoru)
    bool list = (argc < 2) || sc_eq(argv[1], "list-units") ||
                sc_eq(argv[1], "list-unit-files") ||
                (sc_eq(argv[1], "status") && argc < 3);
    if (list) {
        KInit::KService* svcs = KInit::GetServices();
        int n = KInit::GetServiceCount();
        p = sc_cat(out, p, mx, "  UNIT                     LOAD   ACTIVE     SUB          DESCRIPTION\n");
        for (int i = 0; i < n; i++) {
            KInit::KService* s = &svcs[i];
            p = sc_cat(out, p, mx, "  ");
            p = sc_cat(out, p, mx, s->name);
            int pad = sc_len(s->name);
            for (int k = pad; k < 17; k++) p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, ".service ");
            p = sc_cat(out, p, mx, load_state(s));
            p = sc_cat(out, p, mx, " ");
            const char* as = active_state(s->state);
            p = sc_cat(out, p, mx, as);
            for (int k = sc_len(as); k < 10; k++) p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, " ");
            const char* ss = sub_state(s->state);
            p = sc_cat(out, p, mx, ss);
            for (int k = sc_len(ss); k < 12; k++) p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, s->description);
            p = sc_cat(out, p, mx, "\n");
        }
        p = sc_cat(out, p, mx, "\n");
        p = sc_cat_u(out, p, mx, (uint32_t)n);
        p = sc_cat(out, p, mx, " units listed (backed by kinit).\n");
        return p;
    }

    const char* action = argv[1];

    // systemctl daemon-reload / daemon-reexec (no unit arg) -> kinit reload.
    // (satoru)
    if (sc_eq(action, "daemon-reload") || sc_eq(action, "daemon-reexec")) {
        KInit::Reload();
        return sc_cat(out, 0, mx, "kinit: reloaded units from /kurono/system/services\n");
    }

    if (argc < 3) {
        p = sc_cat(out, p, mx,
                   "systemctl (kurono systemd-compat -> kinit)\n"
                   "usage: systemctl start|stop|restart|status|enable|disable|"
                   "is-active|is-enabled|list-units [unit]\n");
        return p;
    }

    const char* unit = argv[2];
    KInit::KService* s = find_unit(unit);
    char kname[KINIT_NAME_LEN];
    resolve_kinit_name(unit, kname, sizeof(kname));

    // status: full block. (satoru)
    if (sc_eq(action, "status")) {
        return status_block(out, p, mx, unit, s);
    }

    // is-active: prints the ActiveState; exit-code semantics are conveyed in text
    // since the shell has no separate status channel. (satoru)
    if (sc_eq(action, "is-active")) {
        p = sc_cat(out, p, mx, s ? active_state(s->state) : "inactive");
        p = sc_cat(out, p, mx, "\n");
        return p;
    }
    if (sc_eq(action, "is-enabled")) {
        p = sc_cat(out, p, mx, (s && s->enabled) ? "enabled" : "disabled");
        p = sc_cat(out, p, mx, "\n");
        return p;
    }
    if (sc_eq(action, "is-failed")) {
        p = sc_cat(out, p, mx, (s && s->state == KInit::KSVC_FAILED) ? "failed" : "active");
        p = sc_cat(out, p, mx, "\n");
        return p;
    }

    if (!s) {
        p = sc_cat(out, p, mx, "Failed to ");
        p = sc_cat(out, p, mx, action);
        p = sc_cat(out, p, mx, " ");
        p = sc_cat(out, p, mx, unit);
        p = sc_cat(out, p, mx, ".service: Unit ");
        p = sc_cat(out, p, mx, unit);
        p = sc_cat(out, p, mx, ".service not found.\n");
        return p;
    }

    int rc = 0;
    const char* verb = action;
    if      (sc_eq(action, "start"))                                rc = KInit::StartService(kname);
    else if (sc_eq(action, "stop"))                                 rc = KInit::StopService(kname);
    else if (sc_eq(action, "restart") || sc_eq(action, "reload-or-restart") ||
             sc_eq(action, "try-restart"))                          rc = KInit::RestartService(kname);
    else if (sc_eq(action, "reload")) {
        // real reload: re-parse the unit's source .service (if one was seeded
        // under /run/systemd/system) -> regenerate its .kservice via the same
        // converter, otherwise just re-scan the .kservice dir; then apply the new
        // config by restarting the unit if it is currently up (kinit has no in-
        // place SIGHUP reload, so a restart is the honest way to apply changes).
        // (satoru)
        verb = "reload";
        char spath[256]; int sp = 0;
        sp = sc_cat(spath, sp, (int)sizeof(spath), "/run/systemd/system/");
        sp = sc_cat(spath, sp, (int)sizeof(spath), kname);
        sp = sc_cat(spath, sp, (int)sizeof(spath), ".service");
        static char rbuf[4096];
        int got = KVFS::ReadFile(spath, rbuf, (uint32_t)sizeof(rbuf) - 1);
        if (got > 0) {
            rbuf[got] = 0;
            char ufile[KINIT_NAME_LEN + 16]; int up = 0;
            up = sc_cat(ufile, up, (int)sizeof(ufile), kname);
            up = sc_cat(ufile, up, (int)sizeof(ufile), ".service");
            InstallServiceUnit(ufile, rbuf, got);   // regenerates the .kservice + KInit::Reload() (satoru)
        } else {
            KInit::Reload();                          // no .service source: re-scan .kservice dir (satoru)
        }
        // re-resolve after reload; restart to apply if the unit is running. (satoru)
        KInit::KService* rs = find_unit(unit);
        if (rs && (rs->state == KInit::KSVC_RUNNING || rs->state == KInit::KSVC_STARTING))
            rc = KInit::RestartService(kname);
        else
            rc = 0;
    }
    else if (sc_eq(action, "enable"))                               rc = KInit::EnableService(kname);
    else if (sc_eq(action, "disable"))                              rc = KInit::DisableService(kname);
    else {
        p = sc_cat(out, p, mx, "systemctl: unknown verb '");
        p = sc_cat(out, p, mx, action);
        p = sc_cat(out, p, mx, "'\n");
        return p;
    }

    if (rc == -3) {
        p = sc_cat(out, p, mx, "Failed to ");
        p = sc_cat(out, p, mx, verb);
        p = sc_cat(out, p, mx, " ");
        p = sc_cat(out, p, mx, unit);
        p = sc_cat(out, p, mx, ".service: capability gate denied (see security.log)\n");
        return p;
    }

    // systemd is terse on success: emit nothing for start/stop/restart (matches
    // real systemctl), a confirmation for enable/disable. (satoru)
    if (sc_eq(action, "enable")) {
        p = sc_cat(out, p, mx, "Created symlink /run/systemd/system/");
        p = sc_cat(out, p, mx, kname);
        p = sc_cat(out, p, mx, ".service -> kinit unit (enabled).\n");
    } else if (sc_eq(action, "disable")) {
        p = sc_cat(out, p, mx, "Removed /run/systemd/system/");
        p = sc_cat(out, p, mx, kname);
        p = sc_cat(out, p, mx, ".service (disabled).\n");
    }
    return p;
}

// ═════════════════════════════════════════════════════════════════════════════
//  4. journalctl shim
// ═════════════════════════════════════════════════════════════════════════════
int CmdJournalctl(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;

    // parse -u <svc>, -n <N>, -f. (satoru)
    const char* unit = nullptr;
    int tail_n = 0;          // 0 = all (satoru)
    bool follow = false;
    for (int i = 1; i < argc; i++) {
        if ((sc_eq(argv[i], "-u") || sc_eq(argv[i], "--unit")) && i + 1 < argc) {
            unit = argv[++i];
        } else if ((sc_eq(argv[i], "-n") || sc_eq(argv[i], "--lines")) && i + 1 < argc) {
            tail_n = (int)parse_uint(argv[++i]);
        } else if (sc_eq(argv[i], "-f") || sc_eq(argv[i], "--follow")) {
            follow = true;
        }
    }

    char ubase[KINIT_NAME_LEN] = {0};
    if (unit) strip_unit_suffix(unit, ubase, sizeof(ubase));

    // read the kinit audit log. (satoru)
    static char buf[8192];
    int got = KVFS::ReadFile(KP_LOG_DIR "/services.log", buf, (uint32_t)sizeof(buf) - 1);
    if (got <= 0) {
        return sc_cat(out, 0, mx, "-- No entries --\n");
    }
    buf[got] = 0;

    // collect matching line offsets so -n can tail. (satoru)
    static int line_off[512];
    static int line_len[512];
    int nlines = 0;
    int i = 0;
    while (i < got && nlines < 512) {
        int ls = i;
        while (i < got && buf[i] != '\n') i++;
        int le = i;
        if (i < got) i++;
        // filter by unit name if requested. (satoru)
        bool keep = true;
        if (unit && ubase[0]) {
            // a kinit log line is "<ms> <event> <service> [detail]"; match the
            // service token. do a bounded contains-check on the line. (satoru)
            buf[le] = 0;   // temporarily terminate for the contains check (satoru)
            keep = sc_contains(buf + ls, ubase);
            if (le < got) buf[le] = '\n';
        }
        if (keep) { line_off[nlines] = ls; line_len[nlines] = le - ls; nlines++; }
    }

    int start = 0;
    if (tail_n > 0 && tail_n < nlines) start = nlines - tail_n;

    p = sc_cat(out, p, mx, "-- Logs (kinit services.log");
    if (unit) { p = sc_cat(out, p, mx, ", unit="); p = sc_cat(out, p, mx, ubase); }
    p = sc_cat(out, p, mx, ") --\n");

    for (int k = start; k < nlines; k++) {
        int off = line_off[k], ln = line_len[k];
        for (int c = 0; c < ln && p < mx - 1; c++) out[p++] = buf[off + c];
        if (p < mx - 1) out[p++] = '\n';
    }
    out[p] = 0;

    if (nlines == 0) {
        return sc_cat(out, 0, mx, "-- No entries for that unit --\n");
    }

    // -f (follow): the shell command model is one-shot (no streaming channel),
    // so we honour it minimally: print the current tail and a note. a true
    // follow would block the single-threaded shell. (satoru)
    if (follow) {
        p = sc_cat(out, p, mx,
                   "-- follow (-f): showing current tail; kinit appends new lines "
                   "live to /kurono/var/log/services.log --\n");
    }
    return p;
}

// ═════════════════════════════════════════════════════════════════════════════
//  5. loginctl stub
// ═════════════════════════════════════════════════════════════════════════════
int CmdLoginctl(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;
    const char* user = current_user();

    const char* action = (argc >= 2) ? argv[1] : "list-sessions";

    if (sc_eq(action, "list-sessions") || sc_eq(action, "list")) {
        // enumerate the SUPR security engine's live sessions instead of a fixed
        // row: each active session's real (uid, user, seat, tty). (satoru)
        p = sc_cat(out, p, mx, "SESSION  UID USER   SEAT  TTY\n");
        SUPRUser* users = SUPR::GetUsers();
        int un = SUPR::GetUserCount();
        int count = 0;
        for (int i = 0; i < SUPR_MAX_SESSIONS; i++) {
            SUPRSession* s = SUPR::GetSession(i);
            if (!s || !s->active) continue;
            const char* uname = user; uint32_t uid = 1000;
            if (users && s->user_index >= 0 && s->user_index < un) {
                uname = users[s->user_index].username;
                uid   = (uint32_t)users[s->user_index].uid;
            }
            const char* tty = s->tty[0] ? s->tty : "tty1";
            p = sc_cat_u(out, p, mx, (uint32_t)(i + 1));
            p = sc_cat(out, p, mx, "  ");
            p = sc_cat_u(out, p, mx, uid);
            p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, uname);
            for (int k = sc_len(uname); k < 6; k++) p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, " seat0 ");
            p = sc_cat(out, p, mx, tty);
            p = sc_cat(out, p, mx, "\n");
            count++;
        }
        if (count == 0) {
            // no SUPR session (e.g. before login / bare autologin): show the
            // effective default so a probe still gets a valid answer. (satoru)
            p = sc_cat(out, p, mx, "      1 1000 ");
            p = sc_cat(out, p, mx, user);
            for (int k = sc_len(user); k < 6; k++) p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, " seat0 tty1\n");
            count = 1;
        }
        p = sc_cat(out, p, mx, "\n");
        p = sc_cat_u(out, p, mx, (uint32_t)count);
        p = sc_cat(out, p, mx, " sessions listed.\n");
        return p;
    }
    if (sc_eq(action, "list-seats")) {
        return sc_cat(out, 0, mx, "SEAT\nseat0\n\n1 seats listed.\n");
    }
    if (sc_eq(action, "list-users")) {
        // one row per distinct uid across the live SUPR sessions. (satoru)
        p = sc_cat(out, p, mx, "  UID USER\n");
        SUPRUser* users = SUPR::GetUsers();
        int un = SUPR::GetUserCount();
        uint32_t seen_uid[SUPR_MAX_SESSIONS]; int nseen = 0;
        int count = 0;
        for (int i = 0; i < SUPR_MAX_SESSIONS; i++) {
            SUPRSession* s = SUPR::GetSession(i);
            if (!s || !s->active) continue;
            const char* uname = user; uint32_t uid = 1000;
            if (users && s->user_index >= 0 && s->user_index < un) {
                uname = users[s->user_index].username;
                uid   = (uint32_t)users[s->user_index].uid;
            }
            bool dup = false;
            for (int k = 0; k < nseen; k++) if (seen_uid[k] == uid) { dup = true; break; }
            if (dup) continue;
            if (nseen < SUPR_MAX_SESSIONS) seen_uid[nseen++] = uid;
            p = sc_cat_u(out, p, mx, uid);
            p = sc_cat(out, p, mx, " ");
            p = sc_cat(out, p, mx, uname);
            p = sc_cat(out, p, mx, "\n");
            count++;
        }
        if (count == 0) {
            p = sc_cat(out, p, mx, " 1000 ");
            p = sc_cat(out, p, mx, user);
            p = sc_cat(out, p, mx, "\n");
            count = 1;
        }
        p = sc_cat(out, p, mx, "\n");
        p = sc_cat_u(out, p, mx, (uint32_t)count);
        p = sc_cat(out, p, mx, " users listed.\n");
        return p;
    }
    if (sc_eq(action, "session-status") || sc_eq(action, "show-session")) {
        p = sc_cat(out, p, mx, "1 - ");
        p = sc_cat(out, p, mx, user);
        p = sc_cat(out, p, mx, " (1000)\n"
                               "           Since: boot\n"
                               "           State: active\n"
                               "            Seat: seat0\n"
                               "             TTY: tty1\n"
                               "         Service: login; type tty; class user\n"
                               "          Active: yes\n");
        return p;
    }
    if (sc_eq(action, "user-status") || sc_eq(action, "show-user")) {
        p = sc_cat(out, p, mx, user);
        p = sc_cat(out, p, mx, " (1000)\n"
                               "           State: active\n"
                               "        Sessions: 1\n"
                               "          Linger: no\n");
        return p;
    }
    // default: a one-line active-session summary so any probe gets a valid answer.
    // (satoru)
    p = sc_cat(out, p, mx, "Session 1 (");
    p = sc_cat(out, p, mx, user);
    p = sc_cat(out, p, mx, ", seat0): active\n");
    return p;
}

// ═════════════════════════════════════════════════════════════════════════════
//  6. org.freedesktop.systemd1 + .login1 d-bus bridge
// ═════════════════════════════════════════════════════════════════════════════
namespace {

inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int pad4(int p) { return (p + 3) & ~3; }
inline int pad8(int p) { return (p + 7) & ~7; }

// append a marshalled d-bus string (u32 len + bytes + nul, 4-aligned) at p,
// bounded by cap. returns new offset. (satoru)
int put_str(uint8_t* b, int p, int cap, const char* s) {
    int sl = sc_len(s);
    p = pad4(p);
    if (p + 4 + sl + 1 > cap) return p;
    put_le32(b + p, (uint32_t)sl); p += 4;
    for (int i = 0; i < sl; i++) b[p++] = (uint8_t)s[i];
    b[p++] = 0;
    return p;
}
// append a marshalled object path (same wire form as a string). (satoru)
int put_obj(uint8_t* b, int p, int cap, const char* s) { return put_str(b, p, cap, s); }

// build the object path systemd uses for a unit: /org/freedesktop/systemd1/unit/
// <escaped-name>. we use a simplified escaping (dots->_2e is not needed for our
// short ascii names; we just append "<name>_2eservice"). (satoru)
int put_unit_path(uint8_t* b, int p, int cap, const char* name) {
    char path[160];
    int q = 0;
    q = sc_cat(path, q, (int)sizeof(path), "/org/freedesktop/systemd1/unit/");
    q = sc_cat(path, q, (int)sizeof(path), name);
    q = sc_cat(path, q, (int)sizeof(path), "_2eservice");
    return put_obj(b, p, cap, path);
}

// read the first marshalled string argument from a d-bus message body into out.
// body_off points at the (8-aligned) body start; the arg is a plain string. used
// to pull the unit name out of GetUnit/StartUnit/StopUnit/RestartUnit. (satoru)
bool read_first_string_arg(const uint8_t* msg, int len, int body_off, char* out, int mx) {
    int p = body_off;
    if (p + 4 > len) return false;
    uint32_t sl = get_le32(msg + p); p += 4;
    if (p + (int)sl > len) return false;
    int n = (int)sl; if (n > mx - 1) n = mx - 1;
    for (int i = 0; i < n; i++) out[i] = (char)msg[p + i];
    out[n] = 0;
    return true;
}

}  // namespace

int DBusDispatch(const char* iface, const char* member, const char* path,
                 const uint8_t* msg, int len, int body_off,
                 uint8_t* out_body, int cap, const char** out_sig) {
    *out_sig = "";

    // ── org.freedesktop.login1 (loginctl-over-dbus) ──────────────────────────
    // many desktop apps query login1 for the session/seat. answer the common
    // calls with valid data so they do not crash. (satoru)
    if (sc_contains(iface, "login1")) {
        if (sc_eq(member, "GetSession") || sc_eq(member, "GetSessionByPID") ||
            sc_eq(member, "GetUser") || sc_eq(member, "GetSeat")) {
            // reply: object path "o". (satoru)
            int p = put_obj(out_body, 0, cap, "/org/freedesktop/login1/session/1");
            *out_sig = "o";
            return p;
        }
        if (sc_eq(member, "ListSessions")) {
            // reply: a(susso) array of (id, uid, user, seat, path). build one
            // entry. signature: "a(susso)". (satoru)
            int p = 0;
            int len_off = p; p += 4;            // array length placeholder (satoru)
            p = pad8(p);                          // struct alignment (satoru)
            int struct_start = p;
            p = put_str(out_body, p, cap, "1");          // session id (satoru)
            p = pad4(p); if (p + 4 <= cap) { put_le32(out_body + p, 1000); p += 4; }   // uid (satoru)
            p = put_str(out_body, p, cap, current_user());        // user (satoru)
            p = put_str(out_body, p, cap, "seat0");                // seat (satoru)
            p = put_obj(out_body, p, cap, "/org/freedesktop/login1/session/1");
            (void)struct_start;
            put_le32(out_body + len_off, (uint32_t)(p - 4));
            *out_sig = "a(susso)";
            return p;
        }
        // generic login1 success (Activate, Lock, etc.). (satoru)
        return 0;
    }

    // ── org.freedesktop.systemd1 ─────────────────────────────────────────────
    if (!sc_contains(iface, "systemd1")) return -1;   // not ours (satoru)

    KInit::KService* svcs = KInit::GetServices();
    int n = KInit::GetServiceCount();

    // Manager.ListUnits -> a(ssssssouso): (name, desc, load, active, sub,
    // following, unit_path, job_id, job_type, job_path). we fill the first five
    // honestly + a unit path; following/job fields are empty/zero. (satoru)
    if (sc_eq(member, "ListUnits")) {
        int p = 0;
        int len_off = p; p += 4;
        for (int i = 0; i < n; i++) {
            KInit::KService* s = &svcs[i];
            char uname[KINIT_NAME_LEN + 9];
            int q = 0;
            q = sc_cat(uname, q, (int)sizeof(uname), s->name);
            q = sc_cat(uname, q, (int)sizeof(uname), ".service");

            p = pad8(p);                                    // struct align (satoru)
            p = put_str(out_body, p, cap, uname);            // 1 unit name (satoru)
            p = put_str(out_body, p, cap, s->description);   // 2 description (satoru)
            p = put_str(out_body, p, cap, load_state(s));    // 3 load state (satoru)
            p = put_str(out_body, p, cap, active_state(s->state)); // 4 active (satoru)
            p = put_str(out_body, p, cap, sub_state(s->state));    // 5 sub (satoru)
            p = put_str(out_body, p, cap, "");               // 6 following (satoru)
            p = put_unit_path(out_body, p, cap, s->name);    // 7 unit obj path (satoru)
            p = pad4(p); if (p + 4 <= cap) { put_le32(out_body + p, 0); p += 4; }  // 8 job id (satoru)
            p = put_str(out_body, p, cap, "");               // 9 job type (satoru)
            p = put_obj(out_body, p, cap, "/");              // 10 job path (satoru)
            if (p > cap - 64) break;   // leave headroom; drop overflow (satoru)
        }
        put_le32(out_body + len_off, (uint32_t)(p - 4));
        *out_sig = "a(ssssssouso)";
        return p;
    }

    // Manager.GetUnit(name) -> o (the unit object path). (satoru)
    if (sc_eq(member, "GetUnit") || sc_eq(member, "LoadUnit")) {
        char arg[96];
        if (!read_first_string_arg(msg, len, body_off, arg, sizeof(arg))) return 0;
        char base[KINIT_NAME_LEN];
        strip_unit_suffix(arg, base, sizeof(base));
        int p = put_unit_path(out_body, 0, cap, base);
        *out_sig = "o";
        return p;
    }

    // Manager.StartUnit/StopUnit/RestartUnit(name, mode) -> o (a job path). we
    // perform the action via kinit and return a synthetic job path. (satoru)
    if (sc_eq(member, "StartUnit") || sc_eq(member, "StopUnit") ||
        sc_eq(member, "RestartUnit") || sc_eq(member, "TryRestartUnit") ||
        sc_eq(member, "ReloadOrRestartUnit")) {
        char arg[96];
        if (read_first_string_arg(msg, len, body_off, arg, sizeof(arg))) {
            char kname[KINIT_NAME_LEN];
            resolve_kinit_name(arg, kname, sizeof(kname));
            if (sc_eq(member, "StartUnit"))       KInit::StartService(kname);
            else if (sc_eq(member, "StopUnit"))   KInit::StopService(kname);
            else                                  KInit::RestartService(kname);
        }
        int p = put_obj(out_body, 0, cap, "/org/freedesktop/systemd1/job/1");
        *out_sig = "o";
        return p;
    }

    // Manager.GetUnitFileState(name) -> s ("enabled"/"disabled"). (satoru)
    if (sc_eq(member, "GetUnitFileState")) {
        char arg[96];
        const char* st = "disabled";
        if (read_first_string_arg(msg, len, body_off, arg, sizeof(arg))) {
            KInit::KService* s = find_unit(arg);
            if (s && s->enabled) st = "enabled";
        }
        int p = put_str(out_body, 0, cap, st);
        *out_sig = "s";
        return p;
    }

    // org.freedesktop.DBus.Properties.Get(iface, prop) on a unit path -> v. we
    // answer ActiveState/SubState/LoadState/Id/Description by looking the unit up
    // from the object path's trailing name. the body is (string iface, string
    // prop); we only need the prop, which is the 2nd string. (satoru)
    if (sc_eq(member, "Get") || sc_eq(member, "GetAll")) {
        // resolve the unit from the path: .../unit/<name>_2eservice (satoru)
        char uname[KINIT_NAME_LEN] = {0};
        const char* slash = path;
        for (const char* c = path; *c; c++) if (*c == '/') slash = c + 1;
        // slash now points at "<name>_2eservice"; copy until the "_2e". (satoru)
        int w = 0;
        for (const char* c = slash; *c && w < (int)sizeof(uname) - 1; c++) {
            if (c[0] == '_' && c[1] == '2' && c[2] == 'e') break;
            uname[w++] = *c;
        }
        uname[w] = 0;
        KInit::KService* s = uname[0] ? find_unit(uname) : nullptr;

        if (sc_eq(member, "Get")) {
            // pull the property name (2nd string in the body). every read is
            // bounded against `len`: a truncated/malformed message (large length
            // prefix, few actual bytes) must not read past the buffer. (satoru)
            char piface[96] = {0}, prop[64] = {0};
            int pp = body_off;
            if (msg && pp + 4 <= len) {
                uint32_t l1 = get_le32(msg + pp); pp += 4;
                // clamp the copy to BOTH the dest size and the bytes present. (satoru)
                int avail1 = len - pp; if (avail1 < 0) avail1 = 0;
                int nn = (int)l1; if (nn > (int)sizeof(piface) - 1) nn = (int)sizeof(piface) - 1;
                if (nn > avail1) nn = avail1;
                for (int i = 0; i < nn; i++) piface[i] = (char)msg[pp + i];
                piface[nn] = 0;
                // advance past the full declared string + nul (bounded later). (satoru)
                pp += (int)l1 + 1;
                pp = pad4(pp);
                if (pp + 4 <= len) {
                    uint32_t l2 = get_le32(msg + pp); pp += 4;
                    int avail2 = len - pp; if (avail2 < 0) avail2 = 0;
                    int mm = (int)l2; if (mm > (int)sizeof(prop) - 1) mm = (int)sizeof(prop) - 1;
                    if (mm > avail2) mm = avail2;
                    for (int i = 0; i < mm; i++) prop[i] = (char)msg[pp + i];
                    prop[mm] = 0;
                }
            }
            // build a variant "v": signature 'g'(1,"s") then the string. (satoru)
            const char* val = "inactive";
            if      (sc_eq(prop, "ActiveState")) val = s ? active_state(s->state) : "inactive";
            else if (sc_eq(prop, "SubState"))    val = s ? sub_state(s->state) : "dead";
            else if (sc_eq(prop, "LoadState"))   val = s ? "loaded" : "not-found";
            else if (sc_eq(prop, "Id"))          val = s ? s->name : (uname[0] ? uname : "unknown");
            else if (sc_eq(prop, "Description")) val = (s && s->description[0]) ? s->description : (uname[0] ? uname : "");
            else if (sc_eq(prop, "UnitFileState")) val = (s && s->enabled) ? "enabled" : "disabled";
            // marshal the variant: sig-len(1) 's' nul, then the string. (satoru)
            int p = 0;
            if (cap < 8) { *out_sig = "v"; return 0; }   // need room for the variant (satoru)
            out_body[p++] = 1; out_body[p++] = (uint8_t)'s'; out_body[p++] = 0;
            p = put_str(out_body, p, cap, val);
            *out_sig = "v";
            return p;
        }
        // GetAll -> a{sv}: return the core three properties. (satoru)
        int p = 0;
        int len_off = p; p += 4;
        struct { const char* k; const char* v; } props[] = {
            { "Id",          s ? s->name : (uname[0] ? uname : "unknown") },
            { "Description", (s && s->description[0]) ? s->description : (uname[0] ? uname : "") },
            { "LoadState",   s ? "loaded" : "not-found" },
            { "ActiveState", s ? active_state(s->state) : "inactive" },
            { "SubState",    s ? sub_state(s->state) : "dead" },
        };
        for (int i = 0; i < 5; i++) {
            p = pad8(p);                                  // dict entry align (satoru)
            p = put_str(out_body, p, cap, props[i].k);     // key (satoru)
            // variant value: sig 's' + string. (satoru)
            if (p + 3 > cap) break;
            out_body[p++] = 1; out_body[p++] = (uint8_t)'s'; out_body[p++] = 0;
            p = put_str(out_body, p, cap, props[i].v);
            if (p > cap - 64) break;
        }
        put_le32(out_body + len_off, (uint32_t)(p - 4));
        *out_sig = "a{sv}";
        return p;
    }

    // any other systemd1 method: empty success (keeps the client alive). (satoru)
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  shell registration
// ═════════════════════════════════════════════════════════════════════════════
void RegisterShellCommands(void* shell) {
    KuronoShell* sh = (KuronoShell*)shell;
    if (!sh) return;
    // register in ENV_KURONO so KuronoShell::FindCommand resolves these (the
    // legacy linux_init systemctl/journalctl shims registered in ENV_AUTO stay as
    // fallbacks for the linux env). additive: linux_init.cpp is untouched.
    // (satoru)
    sh->RegisterCommand("systemctl", "systemd-compat service control (-> kinit)",
                        ENV_KURONO, "system",
                        reinterpret_cast<ShellCmdHandler>(CmdSystemctl));
    sh->RegisterCommand("journalctl", "systemd-compat journal (-> kinit services.log)",
                        ENV_KURONO, "system",
                        reinterpret_cast<ShellCmdHandler>(CmdJournalctl));
    sh->RegisterCommand("loginctl", "systemd-compat login session info",
                        ENV_KURONO, "system",
                        reinterpret_cast<ShellCmdHandler>(CmdLoginctl));
}

}  // namespace SystemdCompat

// end (satoru)
