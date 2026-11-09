//  kurono os - admin gate implementation
//  see admin.h. the gate sits in front of every operation that would
//  otherwise be available to a normal ring-3 user: system-path writes, raw
//  block devices, killing processes you do not own, the kld broker and the
//  /dev/kexec kernel-command bridge. (satoru)
#include "admin.h"
#include "ken.h"
#include "ksa.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../system/logging.h"
#include "../fs/kvfs.h"

namespace {

bool g_enforce = true;

// one approval lasts a few seconds: enough for a batch of writes, short
// enough that a process cannot sit elevated for its whole lifetime. (satoru)
constexpr uint32_t ADMIN_GRANT_MS = 15000;

struct Grant {
    uint64_t until_ms;
};
Grant g_grants[LINUX_MAX_PROCS];

uint64_t now_ms() { return Timer::GetRealMs64(); }

int cur_index() {
    int i = LinuxSyscall::GetCurrentIndex();
    return (i >= 0 && i < LINUX_MAX_PROCS) ? i : -1;
}

bool has_grant() {
    int i = cur_index();
    return i >= 0 && g_grants[i].until_ms > now_ms();
}

void grant_index(int i, uint32_t ms) {
    if (i < 0 || i >= LINUX_MAX_PROCS) return;
    g_grants[i].until_ms = now_ms() + ms;
}

bool starts(const char* s, const char* p) {
    if (!s || !p) return false;
    int i = 0;
    while (p[i]) { if (s[i] != p[i]) return false; i++; }
    return true;
}

bool same_str(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return false; i++; }
    return a[i] == 0 && b[i] == 0;
}

bool is_under(const char* path, const char* dir) {
    int n = 0; while (dir[n]) n++;
    if (!starts(path, dir)) return false;
    return path[n] == 0 || path[n] == '/' || dir[n - 1] == '/';
}

// lexical path cleanup: collapse repeated slashes and resolve "." and "..".
// the gate must judge the path kvfs will open, not the spelling a process
// handed the syscall. (satoru)
void path_canonical(const char* in, char* out, int max) {
    if (max <= 0) return;
    out[0] = 0;
    if (!in || !in[0]) return;
    int p = 0;
    if (in[0] != '/' && p < max - 1) out[p++] = '/';
    int i = 0;
    while (in[i]) {
        while (in[i] == '/') i++;
        if (!in[i]) break;
        int b = i;
        while (in[i] && in[i] != '/') i++;
        int n = i - b;
        if (n == 1 && in[b] == '.') continue;
        if (n == 2 && in[b] == '.' && in[b + 1] == '.') {
            while (p > 0 && out[p - 1] != '/') p--;
            if (p > 0) p--;
            continue;
        }
        if (p < max - 1) out[p++] = '/';
        for (int k = 0; k < n && p < max - 1; k++) out[p++] = in[b + k];
    }
    if (p == 0 && max > 1) out[p++] = '/';
    out[p] = 0;
}

// rebuild the absolute path of a kvfs node after symlinks have been followed.
// (satoru)
bool kvfs_node_path(KVFSNode* n, char* out, int max) {
    if (!n || max <= 1) return false;
    const char* parts[48];
    int np = 0;
    while (n && n->parent) {
        if (np >= 48) return false;
        parts[np++] = n->name;
        n = n->parent;
    }
    if (!n) return false;
    int p = 0;
    out[p++] = '/';
    for (int i = np - 1; i >= 0; i--) {
        if (p > 1 && p < max - 1) out[p++] = '/';
        for (const char* q = parts[i]; *q && p < max - 1; q++) out[p++] = *q;
    }
    out[p] = 0;
    return true;
}

// the real target of a path: kvfs resolves every symlink component, so this is
// what an open/write will actually touch. an existing path maps to its node;
// a new file maps to its parent directory plus the leaf name. returns false
// when kvfs has no node for it (ext4 or a not-yet-created tree). (satoru)
bool kvfs_realpath(const char* canon, char* out, int max) {
    if (!canon || !canon[0] || max <= 1) return false;
    KVFSNode* n = KVFS::Resolve(canon);
    if (n) return kvfs_node_path(n, out, max);

    char parent[KVFS_MAX_PATH];
    int len = 0; while (canon[len]) len++;
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (canon[i] == '/') { slash = i; break; }
    if (slash < 0) return false;
    const char* leaf = canon + slash + 1;
    if (!leaf[0]) return false;
    int pl = slash > 0 ? slash : 1;
    if (pl >= (int)sizeof(parent)) return false;
    for (int i = 0; i < pl; i++) parent[i] = canon[i];
    parent[pl] = 0;
    KVFSNode* pn = KVFS::Resolve(parent);
    if (!pn || !pn->is_dir()) return false;
    char base[KVFS_MAX_PATH];
    if (!kvfs_node_path(pn, base, sizeof(base))) return false;
    int p = 0;
    while (base[p] && p < max - 1) { out[p] = base[p]; p++; }
    if (p > 1 && p < max - 1) out[p++] = '/';
    for (int i = 0; leaf[i] && p < max - 1; i++) out[p++] = leaf[i];
    out[p] = 0;
    return true;
}

// paths a normal user may write. everything else under a system root needs
// elevation. this is deliberately path based: kvfs does not enforce the unix
// mode bits on a per-syscall basis, and seed files are root-owned. (satoru)
bool ends_with(const char* s, const char* suf) {
    int n = 0; while (s && s[n]) n++;
    int m = 0; while (suf && suf[m]) m++;
    if (m > n) return false;
    for (int i = 0; i < m; i++) if (s[n - m + i] != suf[i]) return false;
    return true;
}

bool allowed_path(const char* p) {
    if (!p || !p[0]) return true;
    // fontconfig refreshes its per-directory cache marker in place; it is a
    // dotfile, not system policy, and firefox refuses to start if the unlink is
    // denied. (satoru)
    if (ends_with(p, "/.uuid") || ends_with(p, ".uuid")) return true;
    // user data + scratch space (satoru)
    if (is_under(p, "/home") || is_under(p, "/tmp") || is_under(p, "/mnt")) return true;
    if (is_under(p, "/kurono/user") || is_under(p, "/kurono/runtime")) return true;
    // /dev entries are device nodes, not policy files; the dangerous raw-block
    // open is gated separately in sys_open before this policy is consulted. (satoru)
    if (is_under(p, "/system/dev") || is_under(p, "/dev")) return true;
    // runtime + cache (wayland sockets, fontconfig cache, app scratch). the
    // /kurono/system spellings are the same directories after the canonical
    // /system symlink is followed, so both forms count as writable. (satoru)
    if (is_under(p, "/system/run") || is_under(p, "/system/tmp")) return true;
    if (is_under(p, "/system/var/cache") || is_under(p, "/system/var/tmp")) return true;
    if (is_under(p, "/system/log")) return true;
    if (is_under(p, "/kurono/system/run") || is_under(p, "/kurono/system/tmp")) return true;
    if (is_under(p, "/kurono/system/var/cache") || is_under(p, "/kurono/system/var/tmp")) return true;
    if (is_under(p, "/kurono/system/log") || is_under(p, "/kurono/system/dev")) return true;
    if (is_under(p, "/kurono/runtime") || is_under(p, "/kurono/var/log") ||
        is_under(p, "/kurono/var/cache") || is_under(p, "/kurono/var/tmp")) return true;
    // everything else is system state (satoru)
    return false;
}

bool protected_path(const char* p) {
    if (!p || !p[0]) return false;
    if (allowed_path(p)) return false;
    // a path is protected when it names a system root. a bare relative name
    // is resolved by the syscall layer before it reaches here. (satoru)
    static const char* roots[] = {
        "/system", "/kurono", "/apps", "/boot", "/root", "/opt", "/linux",
        "/windows", "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib64",
        "/var", "/proc", "/sys", "/dev", "/",
        nullptr
    };
    for (int i = 0; roots[i]; i++) {
        const char* r = roots[i];
        if (r[0] == '/' && r[1] == 0) continue;   // never treat everything as protected (satoru)
        if (is_under(p, r)) return true;
    }
    return false;
}

// called with the kls held (all syscall paths). release it around the modal
// so other cpus are not serialized behind a human decision. (satoru)
struct KlsRelax {
    int depth = 0;
    KlsRelax()  { depth = LinuxSyscall::KlsDropAll(); }
    ~KlsRelax() { LinuxSyscall::KlsRestore(depth); }
};

bool run_prompt(const char* detail) {
    KSARequest req;
    req.title     = "Administrator Approval";
    req.detail    = detail ? detail : "An application wants to make a privileged change.";
    req.username  = "root";
    req.want_cred = false;   // consent prompt; sudo-style commands verify a credential (satoru)
    KSAVerdict v;
    bool ok = KSA::Prompt(req, v) && v.approved;
    return ok;
}

void copy_detail(char* d, int mx, const char* a, const char* b) {
    int p = 0;
    const char* s = a;
    while (s && *s && p < mx - 1) d[p++] = *s++;
    if (b && p < mx - 2) { d[p++] = ':'; d[p++] = ' '; }
    s = b;
    while (s && *s && p < mx - 1) d[p++] = *s++;
    d[p] = 0;
}

}  // namespace (satoru)

namespace AdminGate {

void SetEnforce(bool on) { g_enforce = on; }
bool EnforceEnabled() { return g_enforce; }

bool IsPrivileged() {
    if (!g_enforce) return true;
    Process* t = Scheduler::GetCurrentProcess();
    if (!t) return true;
    if (t->is_kernel_proc) return true;
    LinuxProcess* lp = LinuxSyscall::Current();
    if (!lp) return true;
    if (lp->euid == 0) return true;
    if (has_grant()) return true;
    // a ken/sudo session elevated to root covers the command it wraps. (satoru)
    int sid = KEN::GetCurrentSession();
    if (sid >= 0 && (int)KEN::GetLevel(sid) >= (int)KEN_ROOT) return true;
    return false;
}

bool Authorize(const char* action, const char* detail) {
    if (IsPrivileged()) return true;
    if (!g_enforce) return true;

    char d[160];
    copy_detail(d, sizeof(d), action, detail);
    SerialLogger::Log("[admin] prompt: ");
    SerialLogger::Log(d);
    SerialLogger::Log("\r\n");

    bool ok;
    {
        KlsRelax relax;
        ok = run_prompt(d);
    }
    if (ok) {
        int i = cur_index();
        grant_index(i, ADMIN_GRANT_MS);
        RuntimeLog::LogSecurity("admin approved", d);
        SerialLogger::Log("[admin] approved: ");
        SerialLogger::Log(d);
        SerialLogger::Log("\r\n");
    } else {
        RuntimeLog::LogSecurity("admin denied", d);
        SerialLogger::Log("[admin] denied: ");
        SerialLogger::Log(d);
        SerialLogger::Log("\r\n");
    }
    return ok;
}

bool PathNeedsElevation(const char* resolved_path) {
    if (!resolved_path || !resolved_path[0]) return false;
    // judge the normalized path first: "/home/../system/x" is a system write. (satoru)
    char canon[KVFS_MAX_PATH];
    path_canonical(resolved_path, canon, sizeof(canon));
    if (protected_path(canon)) return true;
    // then the real node behind any symlinks: "/tmp/link" whose target is a
    // system path must be gated too. (satoru)
    char real[KVFS_MAX_PATH];
    if (kvfs_realpath(canon, real, sizeof(real)) && protected_path(real)) return true;
    return false;
}

bool AuthorizePathWrite(const char* resolved_path, const char* how) {
    if (!g_enforce) return true;
    if (!PathNeedsElevation(resolved_path)) return true;
    char d[160];
    copy_detail(d, sizeof(d), how ? how : "write", resolved_path);
    return Authorize("system path write", d);
}

bool ShellCommandNeedsElevation(const char* cmdline) {
    if (!cmdline) return false;
    while (*cmdline == ' ' || *cmdline == '\t') cmdline++;
    if (!cmdline[0]) return false;

    char name[32];
    int n = 0;
    while (cmdline[n] && cmdline[n] != ' ' && cmdline[n] != '\t' && n < 31) {
        name[n] = cmdline[n];
        n++;
    }
    name[n] = 0;

    // the sudo-style forms run their target through the ken credential gate,
    // so they self-authenticate. the policy/passwd/selftest subcommands do
    // not: they mutate the security policy without a password, so they must
    // go through this gate. exact names only, so "kenfoo" cannot inherit the
    // exemption. (satoru)
    if ((same_str(name, "sudo") && !name[4]) || (same_str(name, "ken") && !name[3]) ||
        (same_str(name, "supr") && !name[4])) {
        const char* sub = cmdline + n;
        while (*sub == ' ' || *sub == '\t') sub++;
        char subn[32];
        int sn = 0;
        while (sub[sn] && sub[sn] != ' ' && sub[sn] != '\t' && sn < 31) {
            subn[sn] = sub[sn];
            sn++;
        }
        subn[sn] = 0;
        if (!same_str(subn, "policy") && !same_str(subn, "passwd") &&
            !same_str(subn, "selftest")) return false;
    }

    // allowlist: read-only, informational, media and app launchers. anything
    // not listed is treated as privileged, which is the safe default for a
    // channel that can run kernel builtins. (satoru)
    static const char* safe[] = {
        "help", "version", "uname", "hostname", "date", "uptime", "whoami",
        "pwd", "env", "printenv", "set", "export", "alias", "history",
        "clear", "echo", "sysinfo", "systeminfo", "ps", "top", "free", "df",
        "du", "ls", "lsblk", "lspci", "lsusb", "lscpu", "dmidecode", "hwinfo",
        "hwfw", "dmesg", "journal", "journalctl", "cpufreq", "display", "gpu",
        "gpuprobe", "gpuaux", "codecs", "netstat_l", "ping", "ping_l",
        "nslookup", "ifconfig", "ifconfig_l", "ip", "ip_l", "iwconfig",
        "nmcli", "wifi", "nic", "tree", "which", "kj", "python",
        "python3", "node", "kpy", "firefox", "claude", "kapp", "ffmpeg",
        "wltest", "pthtest", "usermode", "audio", "denji",
        "playvideo", "vgpu", "blkid", "lsb_release", "cat", "head", "tail",
        "wc", "sort", "uniq", "cut", "tr", "grep", "find", "xargs",
        "lsmod", "modinfo", "tasklist", "version", "ver", "type", "dir",
        "findstr", "search", "systeminfo", "nic", nullptr
    };
    for (int i = 0; safe[i]; i++) {
        const char* s = safe[i];
        int k = 0;
        while (name[k] && s[k] == name[k]) k++;
        if (!name[k] && !s[k]) return false;
    }
    return true;
}

void GrantCurrent(uint32_t ms) { grant_index(cur_index(), ms ? ms : ADMIN_GRANT_MS); }
void RevokeCurrent() { grant_index(cur_index(), 0); }

void ForgetProcess(int pid_index) {
    if (pid_index >= 0 && pid_index < LINUX_MAX_PROCS) g_grants[pid_index].until_ms = 0;
}

}  // namespace AdminGate (satoru)

// end (satoru)
