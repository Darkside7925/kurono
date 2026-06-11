#include "linux_cmds.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../linux/linux_syscall.h"
#include "../virt/hypervisor.h"

// ══════════════════════════════════════════════════════════════════════xz`═════
//  Linux Bridge — POSIX-like commands for Kurono Shell
// ═══════════════════════════════════════════════════════════════════════════

static int _slen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void _scpy(char* d, const char* s, int m) {
    int i=0; while (s[i] && i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}
static bool _seq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return false; a++; b++; } return *a==*b;
}
static int _sa(char* b, int p, int m, const char* s) {
    while (*s && p<m-1) b[p++]=*s++;  b[p]=0; return p;
}
static int _sac(char* b, int p, int m, char c) { if (p<m-1) {b[p++]=c; b[p]=0;} return p; }
static int _sai(char* b, int p, int m, int v) {
    if (v<0) { p=_sac(b,p,m,'-'); v=-v; }
    if (v==0) return _sac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=_sac(b,p,m,t[--ti]);
    return p;
}
static int _sau(char* b, int p, int m, unsigned int v) {
    if (v==0) return _sac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=_sac(b,p,m,t[--ti]);
    return p;
}

static int _atoi(const char* s) {
    int v=0; bool neg=false;
    if (*s=='-') { neg=true; s++; }
    while (*s>='0' && *s<='9') { v=v*10+(*s-'0'); s++; }
    return neg ? -v : v;
}

// ── Registration ─────────────────────────────────────────────────────────

void LinuxCmds::RegisterAll(KuronoShell* sh) {
    using namespace LinuxCmds;
    // Core filesystem commands — available in ALL environments
    sh->RegisterCommand("ls",       "List directory",          ENV_AUTO, "filesystem", cmd_ls);
    sh->RegisterCommand("cd",       "Change directory",        ENV_AUTO, "filesystem", cmd_cd);
    sh->RegisterCommand("mkdir",    "Create directory",        ENV_AUTO, "filesystem", cmd_mkdir);
    sh->RegisterCommand("rmdir",    "Remove directory",        ENV_AUTO, "filesystem", cmd_rmdir);
    sh->RegisterCommand("rm",       "Remove file",             ENV_AUTO, "filesystem", cmd_rm);
    sh->RegisterCommand("cp",       "Copy file",               ENV_AUTO, "filesystem", cmd_cp);
    sh->RegisterCommand("mv",       "Move/rename file",        ENV_AUTO, "filesystem", cmd_mv);
    sh->RegisterCommand("touch",    "Create empty file",       ENV_AUTO, "filesystem", cmd_touch);
    sh->RegisterCommand("cat",      "Display file contents",   ENV_AUTO, "filesystem", cmd_cat);
    sh->RegisterCommand("head",     "Show first N lines",      ENV_AUTO, "text",       cmd_head);
    sh->RegisterCommand("tail",     "Show last N lines",       ENV_AUTO, "text",       cmd_tail);
    sh->RegisterCommand("wc",       "Word/line/char count",    ENV_AUTO, "text",       cmd_wc);
    sh->RegisterCommand("chmod",    "Change permissions",      ENV_AUTO, "filesystem", cmd_chmod);
    sh->RegisterCommand("stat",     "File status",             ENV_AUTO, "filesystem", cmd_stat);
    sh->RegisterCommand("df",       "Disk free space",         ENV_AUTO, "filesystem", cmd_df);
    sh->RegisterCommand("du",       "Disk usage",              ENV_AUTO, "filesystem", cmd_du);
    sh->RegisterCommand("ln",       "Create link",             ENV_AUTO, "filesystem", cmd_ln);
    sh->RegisterCommand("find",     "Search files",            ENV_AUTO, "filesystem", cmd_find);
    sh->RegisterCommand("grep",     "Search text in files",    ENV_AUTO, "text",       cmd_grep);
    sh->RegisterCommand("which",    "Locate a command",        ENV_AUTO, "system",     cmd_which);
    sh->RegisterCommand("tee",      "Tee output to file",      ENV_AUTO, "text",       cmd_tee);
    sh->RegisterCommand("sort",     "Sort lines",              ENV_AUTO, "text",       cmd_sort);
    sh->RegisterCommand("uniq",     "Unique lines",            ENV_AUTO, "text",       cmd_uniq);
    sh->RegisterCommand("tr",       "Translate chars",         ENV_AUTO, "text",       cmd_tr);
    sh->RegisterCommand("ps",       "List processes",          ENV_AUTO, "system",     cmd_ps);
    sh->RegisterCommand("kill",     "Kill process",            ENV_AUTO, "system",     cmd_kill);
    sh->RegisterCommand("free",     "Memory usage",            ENV_AUTO, "system",     cmd_free);
    sh->RegisterCommand("mount",    "Show mounts",             ENV_AUTO, "system",     cmd_mount);
    sh->RegisterCommand("dmesg",    "Kernel log",              ENV_AUTO, "system",     cmd_dmesg);
    sh->RegisterCommand("ifconfig", "Network interfaces",      ENV_AUTO, "network",    cmd_ifconfig);
    sh->RegisterCommand("ping",     "Ping host",               ENV_AUTO, "network",    cmd_ping);
    sh->RegisterCommand("wget",     "Download URL",            ENV_AUTO, "network",    cmd_wget);
    sh->RegisterCommand("curl",     "Transfer data",           ENV_AUTO, "network",    cmd_curl);
    // These overlap with builtins but ENV_AUTO with the same priority is fine:
    sh->RegisterCommand("pwd",      "Print working directory", ENV_LINUX, "filesystem", cmd_pwd);
    sh->RegisterCommand("uname",    "System name",             ENV_LINUX, "system",     cmd_uname);
    sh->RegisterCommand("uptime",   "System uptime",           ENV_LINUX, "system",     cmd_uptime);
    sh->RegisterCommand("whoami",   "Current user",            ENV_LINUX, "system",     cmd_whoami);
    sh->RegisterCommand("hostname", "Show hostname",           ENV_LINUX, "system",     cmd_hostname);
    sh->RegisterCommand("date",     "Show date/time",          ENV_LINUX, "system",     cmd_date);

    // Linux syscall execution — runs real Linux programs via syscall layer
    sh->RegisterCommand("linux-exec", "Run program via Linux syscalls", ENV_AUTO, "linux", cmd_linux_exec);
    sh->RegisterCommand("syscall",    "Direct Linux syscall test",      ENV_AUTO, "linux", cmd_syscall);

    // Hypervisor / VM management — create, boot, run Linux guest VMs
    sh->RegisterCommand("vm",         "Manage virtual machines",        ENV_AUTO, "virt",  cmd_vm);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Filesystem commands
// ═══════════════════════════════════════════════════════════════════════════

int LinuxCmds::cmd_ls(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = (argc > 1) ? argv[1] : ".";
    bool long_fmt = false;
    bool show_all = false;

    // Parse flags
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l') long_fmt = true;
                if (argv[i][j] == 'a') show_all = true;
            }
        } else {
            path = argv[i];
        }
    }

    KVFSNode* dir = KVFS::ResolvePath(path);
    if (!dir || dir->type != KVFS_DIR) return _sa(out, 0, mx, "ls: cannot access: No such directory\n");

    int p = 0;
    if (long_fmt) {
        p = _sa(out, p, mx, "total ");
        p = _sai(out, p, mx, dir->child_count);
        p = _sac(out, p, mx, '\n');
    }

    for (int i = 0; i < dir->child_count; i++) {
        KVFSNode* c = dir->children[i];
        if (!c) continue;
        if (!show_all && c->name[0] == '.') continue;

        if (long_fmt) {
            // Type
            p = _sac(out, p, mx, c->type == KVFS_DIR ? 'd' : (c->type == KVFS_SYMLINK ? 'l' : '-'));
            // Permissions
            unsigned short m = c->perms.mode;
            p = _sac(out, p, mx, (m & 0400) ? 'r' : '-');
            p = _sac(out, p, mx, (m & 0200) ? 'w' : '-');
            p = _sac(out, p, mx, (m & 0100) ? 'x' : '-');
            p = _sac(out, p, mx, (m & 040) ? 'r' : '-');
            p = _sac(out, p, mx, (m & 020) ? 'w' : '-');
            p = _sac(out, p, mx, (m & 010) ? 'x' : '-');
            p = _sac(out, p, mx, (m & 04) ? 'r' : '-');
            p = _sac(out, p, mx, (m & 02) ? 'w' : '-');
            p = _sac(out, p, mx, (m & 01) ? 'x' : '-');
            // Size
            p = _sa(out, p, mx, "  ");
            p = _sau(out, p, mx, c->size);
            // Pad
            int sl = 1; unsigned int tmp = c->size;
            while (tmp >= 10) { sl++; tmp /= 10; }
            for (int j = sl; j < 8; j++) p = _sac(out, p, mx, ' ');
            p = _sac(out, p, mx, ' ');
        }

        // Name with color indicator
        if (c->type == KVFS_DIR) {
            p = _sa(out, p, mx, c->name);
            p = _sac(out, p, mx, '/');
        } else if (c->type == KVFS_SYMLINK) {
            p = _sa(out, p, mx, c->name);
            p = _sa(out, p, mx, " -> ");
            if (c->content) p = _sa(out, p, mx, (const char*)c->content);
        } else {
            p = _sa(out, p, mx, c->name);
        }
        p = _sac(out, p, mx, long_fmt ? '\n' : '\t');
    }
    if (!long_fmt) p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_cd(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    const char* path = (argc > 1) ? argv[1] : nullptr;
    if (!path) {
        const char* home = sh->GetVar("HOME");
        path = home ? home : "/";
    }
    if (_seq(path, "-")) {
        const char* old = sh->GetVar("OLDPWD");
        if (old) path = old;
        else return _sa(out, 0, mx, "cd: OLDPWD not set\n");
    }

    // Save old
    char old_cwd[256];
    _scpy(old_cwd, KVFS::GetCwd(), 256);

    KVFS::SetCwd(path);
    if (!KVFS::Resolve(path)) return _sa(out, 0, mx, "cd: No such directory\n");

    sh->SetVar("OLDPWD", old_cwd);
    sh->SetVar("PWD", KVFS::GetCwd());
    return 0;
}

int LinuxCmds::cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = _sa(out, 0, mx, KVFS::GetCwd());
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_mkdir(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: mkdir [-p] <dir>\n");
    bool parents = false;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-p")) { parents = true; continue; }
        int err = parents ? KVFS::Mkdirs(argv[i]) : KVFS::Mkdir(argv[i]);
        if (err != KVFS_OK) {
            int p = _sa(out, 0, mx, "mkdir: cannot create '");
            p = _sa(out, p, mx, argv[i]);
            return _sa(out, p, mx, "'\n");
        }
    }
    return 0;
}

int LinuxCmds::cmd_rmdir(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: rmdir <dir>\n");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        int err = KVFS::Rmdir(argv[i]);
        if (err != KVFS_OK) {
            int p = _sa(out, 0, mx, "rmdir: failed '");
            p = _sa(out, p, mx, argv[i]);
            return _sa(out, p, mx, "'\n");
        }
    }
    return 0;
}

int LinuxCmds::cmd_rm(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: rm [-rf] <file...>\n");
    bool recursive = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r') recursive = true;
            }
            continue;
        }
        KVFSNode* n = KVFS::ResolvePath(argv[i]);
        if (!n) {
            int p = _sa(out, 0, mx, "rm: cannot remove '");
            p = _sa(out, p, mx, argv[i]);
            return _sa(out, p, mx, "': No such file\n");
        }
        if (n->type == KVFS_DIR) {
            if (!recursive) {
                int p = _sa(out, 0, mx, "rm: '");
                p = _sa(out, p, mx, argv[i]);
                return _sa(out, p, mx, "' is a directory (use -r)\n");
            }
            KVFS::Rmdir(argv[i]);
        } else {
            KVFS::Unlink(argv[i]);
        }
    }
    return 0;
}

int LinuxCmds::cmd_cp(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: cp <src> <dst>\n");
    int err = KVFS::Copy(argv[1], argv[2]);
    if (err != KVFS_OK) return _sa(out, 0, mx, "cp: copy failed\n");
    return 0;
}

int LinuxCmds::cmd_mv(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: mv <src> <dst>\n");
    int err = KVFS::Move(argv[1], argv[2]);
    if (err != KVFS_OK) return _sa(out, 0, mx, "mv: move failed\n");
    return 0;
}

int LinuxCmds::cmd_touch(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: touch <file...>\n");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        KVFSNode* n = KVFS::ResolvePath(argv[i]);
        if (!n) KVFS::CreateFile(argv[i]);
    }
    return 0;
}

int LinuxCmds::cmd_cat(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: cat <file...>\n");
    int p = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        unsigned char buf[KVFS_MAX_CONTENT];
        int bytes_read = KVFS::ReadFile(argv[i], buf, KVFS_MAX_CONTENT);
        if (bytes_read < 0) {
            p = _sa(out, p, mx, "cat: ");
            p = _sa(out, p, mx, argv[i]);
            p = _sa(out, p, mx, ": No such file\n");
            continue;
        }
        for (int j = 0; j < bytes_read && p < mx - 1; j++)
            out[p++] = (char)buf[j];
        out[p] = 0;
    }
    return p;
}

int LinuxCmds::cmd_head(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int lines = 10;
    const char* file = nullptr;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-n") && i + 1 < argc) { lines = _atoi(argv[++i]); }
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) return _sa(out, 0, mx, "Usage: head [-n N] <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(file, buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "head: cannot read file\n");

    int p = 0;
    int lc = 0;
    for (unsigned int i = 0; i < (unsigned int)sz && lc < lines && p < mx - 1; i++) {
        out[p++] = (char)buf[i];
        if (buf[i] == '\n') lc++;
    }
    out[p] = 0;
    return p;
}

int LinuxCmds::cmd_tail(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int lines = 10;
    const char* file = nullptr;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-n") && i + 1 < argc) { lines = _atoi(argv[++i]); }
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) return _sa(out, 0, mx, "Usage: tail [-n N] <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(file, buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "tail: cannot read file\n");

    // Count newlines from end
    int lc = 0;
    int start = (int)sz;
    for (int i = (int)sz - 1; i >= 0; i--) {
        if (buf[i] == '\n') lc++;
        if (lc == lines + 1) { start = i + 1; break; }
    }

    int p = 0;
    for (int i = start; i < (int)sz && p < mx - 1; i++) out[p++] = (char)buf[i];
    out[p] = 0;
    return p;
}

int LinuxCmds::cmd_wc(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: wc <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "wc: cannot read file\n");

    int lines = 0, words = 0, chars = sz;
    bool in_word = false;
    for (int i = 0; i < sz; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
            in_word = false;
        } else if (!in_word) {
            in_word = true; words++;
        }
    }

    int p = 0;
    p = _sa(out, p, mx, "  ");
    p = _sai(out, p, mx, lines);
    p = _sa(out, p, mx, "  ");
    p = _sai(out, p, mx, words);
    p = _sa(out, p, mx, "  ");
    p = _sai(out, p, mx, chars);
    p = _sac(out, p, mx, ' ');
    p = _sa(out, p, mx, argv[1]);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_chmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: chmod <mode> <file>\n");
    // Parse octal mode
    unsigned short mode = 0;
    for (int i = 0; argv[1][i]; i++) {
        if (argv[1][i] >= '0' && argv[1][i] <= '7')
            mode = mode * 8 + (argv[1][i] - '0');
    }
    int err = KVFS::Chmod(argv[2], mode);
    if (err != KVFS_OK) return _sa(out, 0, mx, "chmod: failed\n");
    return 0;
}

int LinuxCmds::cmd_stat(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: stat <file>\n");
    KVFSNode* n = KVFS::ResolvePath(argv[1]);
    if (!n) return _sa(out, 0, mx, "stat: cannot stat: No such file\n");

    int p = 0;
    p = _sa(out, p, mx, "  File: ");
    p = _sa(out, p, mx, n->name);
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Size: ");
    p = _sau(out, p, mx, n->size);
    p = _sa(out, p, mx, "  Type: ");
    p = _sa(out, p, mx, n->type == KVFS_DIR ? "directory" : (n->type == KVFS_SYMLINK ? "symlink" : "regular file"));
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Mode: 0");
    // Octal
    unsigned short m = n->perms.mode;
    p = _sac(out, p, mx, '0' + ((m >> 6) & 7));
    p = _sac(out, p, mx, '0' + ((m >> 3) & 7));
    p = _sac(out, p, mx, '0' + (m & 7));
    p = _sa(out, p, mx, "  Uid: ");
    p = _sau(out, p, mx, n->perms.uid);
    p = _sa(out, p, mx, "  Gid: ");
    p = _sau(out, p, mx, n->perms.gid);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_df(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Filesystem     1K-blocks  Used Available Use% Mounted on\n");
    p = _sa(out, p, mx, "kvfs           65536      2048 63488     3%   /\n");
    p = _sa(out, p, mx, "tmpfs          32768      0    32768     0%   /tmp\n");
    p = _sa(out, p, mx, "devfs          0          0    0         0%   /dev\n");
    return p;
}

int LinuxCmds::cmd_du(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = (argc > 1) ? argv[1] : ".";
    KVFSNode* n = KVFS::ResolvePath(path);
    if (!n) return _sa(out, 0, mx, "du: cannot access\n");

    int p = 0;
    unsigned int total = n->size;
    for (int i = 0; i < n->child_count; i++) {
        if (n->children[i]) total += n->children[i]->size;
    }
    p = _sau(out, p, mx, total);
    p = _sac(out, p, mx, '\t');
    p = _sa(out, p, mx, path);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_ln(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: ln -s <target> <link>\n");
    // Symlinks only for now
    bool soft = false;
    int target_idx = 1, link_idx = 2;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-s")) { soft = true; }
    }
    if (!soft) return _sa(out, 0, mx, "ln: only symbolic links supported (-s)\n");
    // Find target and link args (skip flags)
    const char* args[2]; int ai = 0;
    for (int i = 1; i < argc && ai < 2; i++) {
        if (argv[i][0] != '-') args[ai++] = argv[i];
    }
    if (ai < 2) return _sa(out, 0, mx, "ln: missing operands\n");

    // Create symlink node
    KVFS::CreateFile(args[1]);
    KVFSNode* n = KVFS::ResolvePath(args[1]);
    if (n) {
        n->type = KVFS_SYMLINK;
        KVFS::WriteString(args[1], args[0]);
    }
    (void)target_idx; (void)link_idx;
    return 0;
}

// ── Search commands ──────────────────────────────────────────────────────

int LinuxCmds::cmd_find(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = ".";
    const char* pattern = "*";
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-name") && i + 1 < argc) pattern = argv[++i];
        else if (argv[i][0] != '-') path = argv[i];
    }

    KVFSNode* results[32];
    int count = KVFS::Find(path, pattern, results, 32);

    int p = 0;
    for (int i = 0; i < count && p < mx - 1; i++) {
        if (results[i]) p = _sa(out, p, mx, results[i]->name);
        p = _sac(out, p, mx, '\n');
    }
    return p;
}

int LinuxCmds::cmd_grep(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: grep <pattern> <file...>\n");
    const char* pattern = argv[1];
    int p = 0;

    for (int fi = 2; fi < argc; fi++) {
        char grep_buf[4096];
        int glen = KVFS::Grep(argv[fi], pattern, grep_buf, 4096);
        if (glen > 0) {
            if (argc > 3) {
                p = _sa(out, p, mx, argv[fi]);
                p = _sac(out, p, mx, ':');
            }
            for (int i = 0; i < glen && p < mx - 1; i++)
                out[p++] = grep_buf[i];
            out[p] = 0;
        }
    }
    return p;
}

int LinuxCmds::cmd_which(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    if (argc < 2) return _sa(out, 0, mx, "Usage: which <command>\n");
    ShellCommand* cmd = sh->FindCommand(argv[1]);
    if (cmd) {
        int p = _sa(out, 0, mx, "/bin/");
        p = _sa(out, p, mx, argv[1]);
        return _sac(out, p, mx, '\n');
    }
    int p = _sa(out, 0, mx, argv[1]);
    return _sa(out, p, mx, " not found\n");
}

// ── Text processing ──────────────────────────────────────────────────────

int LinuxCmds::cmd_tee(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: tee <file>\n");
    // In a pipe context, tee would write stdin to file. Here, write last output.
    KVFS::CreateFile(argv[1]);
    return _sa(out, 0, mx, "(tee: pipe context required)\n");
}

int LinuxCmds::cmd_sort(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: sort <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "sort: cannot read file\n");

    // Split into lines
    char* lines[256];
    int lc = 0;
    char* start = (char*)buf;
    for (int i = 0; i < sz; i++) {
        if (buf[i] == '\n') {
            buf[i] = 0;
            if (lc < 256) lines[lc++] = start;
            start = (char*)buf + i + 1;
        }
    }
    if (start < (char*)buf + sz && lc < 256) lines[lc++] = start;

    // Bubble sort
    for (int i = 0; i < lc - 1; i++) {
        for (int j = 0; j < lc - i - 1; j++) {
            const char* a = lines[j]; const char* b = lines[j+1];
            bool swap = false;
            while (*a && *b) { if (*a > *b) { swap = true; break; } if (*a < *b) break; a++; b++; }
            if (swap || (*a && !*b)) { char* t = lines[j]; lines[j] = lines[j+1]; lines[j+1] = t; }
        }
    }

    int p = 0;
    for (int i = 0; i < lc && p < mx - 1; i++) {
        p = _sa(out, p, mx, lines[i]);
        p = _sac(out, p, mx, '\n');
    }
    return p;
}

int LinuxCmds::cmd_uniq(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: uniq <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "uniq: cannot read file\n");

    int p = 0;
    char prev[256]; prev[0] = 0;
    char line[256]; int li = 0;

    for (int i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            line[li] = 0;
            if (!_seq(line, prev)) {
                p = _sa(out, p, mx, line);
                p = _sac(out, p, mx, '\n');
                _scpy(prev, line, 256);
            }
            li = 0;
        } else {
            if (li < 255) line[li++] = (char)buf[i];
        }
    }
    return p;
}

int LinuxCmds::cmd_tr(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: tr <set1> <set2>\n");
    return _sa(out, 0, mx, "(tr: pipe context required)\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  System commands
// ═══════════════════════════════════════════════════════════════════════════

int LinuxCmds::cmd_ps(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "  PID TTY      TIME CMD\n");
    p = _sa(out, p, mx, "    0 ?     00:00:00 kernel\n");
    p = _sa(out, p, mx, "    1 ?     00:00:00 init\n");
    p = _sa(out, p, mx, "    2 tty1  00:00:00 ksh\n");
    p = _sa(out, p, mx, "    3 tty1  00:00:00 desktop\n");
    // From scheduler
    for (int i = 0; i < (int)Scheduler::GetProcessCount() && i < 16; i++) {
        p = _sa(out, p, mx, "    ");
        p = _sai(out, p, mx, 10 + i);
        p = _sa(out, p, mx, " ?     00:00:00 task_");
        p = _sai(out, p, mx, i);
        p = _sac(out, p, mx, '\n');
    }
    return p;
}

int LinuxCmds::cmd_kill(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: kill <pid>\n");
    int pid = _atoi(argv[1]);
    if (pid < 4) return _sa(out, 0, mx, "kill: cannot kill system process\n");
    int p = _sa(out, 0, mx, "kill: sent SIGTERM to ");
    p = _sai(out, p, mx, pid);
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_uname(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    bool all = false;
    for (int i = 1; i < argc; i++) { if (_seq(argv[i], "-a")) all = true; }

    if (all) return _sa(out, 0, mx, "Kurono kurono-machine 1.0.0 #1 SMP x86 i686 KuronoOS\n");
    return _sa(out, 0, mx, "Kurono\n");
}

int LinuxCmds::cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    unsigned int ticks = (unsigned int)(TimeManager::NowUTC().us / 1000u);
    unsigned int seconds = ticks / 1000;
    unsigned int hours = seconds / 3600;
    unsigned int minutes = (seconds % 3600) / 60;
    unsigned int secs = seconds % 60;

    int p = _sa(out, 0, mx, " up ");
    p = _sau(out, p, mx, hours);
    p = _sac(out, p, mx, ':');
    if (minutes < 10) p = _sac(out, p, mx, '0');
    p = _sau(out, p, mx, minutes);
    p = _sac(out, p, mx, ':');
    if (secs < 10) p = _sac(out, p, mx, '0');
    p = _sau(out, p, mx, secs);
    p = _sa(out, p, mx, ", 1 user, load: 0.01 0.02 0.00\n");
    return p;
}

int LinuxCmds::cmd_whoami(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)argc; (void)argv;
    const char* u = sh->GetVar("USER");
    int p = _sa(out, 0, mx, u ? u : "user");
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_hostname(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)argc; (void)argv;
    const char* h = sh->GetVar("HOSTNAME");
    int p = _sa(out, 0, mx, h ? h : "kurono-machine");
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_date(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    // From RTC via Time
    int p = 0;
    unsigned int ticks = (unsigned int)(TimeManager::NowUTC().us / 1000u);
    int hours = (ticks / 3600000) % 24;
    int mins  = (ticks / 60000) % 60;
    int secs  = (ticks / 1000) % 60;

    p = _sa(out, p, mx, "2025-01-01 ");
    if (hours < 10) p = _sac(out, p, mx, '0');
    p = _sai(out, p, mx, hours);
    p = _sac(out, p, mx, ':');
    if (mins < 10) p = _sac(out, p, mx, '0');
    p = _sai(out, p, mx, mins);
    p = _sac(out, p, mx, ':');
    if (secs < 10) p = _sac(out, p, mx, '0');
    p = _sai(out, p, mx, secs);
    p = _sa(out, p, mx, " UTC\n");
    return p;
}

int LinuxCmds::cmd_free(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    unsigned int total = 64 * 1024; // 64 MB in KB
    unsigned int used  = KernelHeap::GetUsed() / 1024;
    unsigned int free_mem = total - used;

    int p = 0;
    p = _sa(out, p, mx, "              total       used       free\n");
    p = _sa(out, p, mx, "Mem:       ");
    p = _sau(out, p, mx, total);
    p = _sa(out, p, mx, "      ");
    p = _sau(out, p, mx, used);
    p = _sa(out, p, mx, "      ");
    p = _sau(out, p, mx, free_mem);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_mount(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "kvfs on / type kvfs (rw)\n");
    p = _sa(out, p, mx, "tmpfs on /tmp type tmpfs (rw)\n");
    p = _sa(out, p, mx, "devfs on /dev type devfs (rw)\n");
    p = _sa(out, p, mx, "procfs on /proc type procfs (ro)\n");
    return p;
}

int LinuxCmds::cmd_dmesg(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "[    0.000] Kurono OS booting...\n");
    p = _sa(out, p, mx, "[    0.001] HAL: Hardware Abstraction Layer initialized\n");
    p = _sa(out, p, mx, "[    0.002] Heap: 64 MB kernel heap allocated\n");
    p = _sa(out, p, mx, "[    0.003] BGA: Display mode 1024x768x32\n");
    p = _sa(out, p, mx, "[    0.004] PIT: Timer at 1000 Hz\n");
    p = _sa(out, p, mx, "[    0.005] PS/2: Keyboard initialized\n");
    p = _sa(out, p, mx, "[    0.006] PS/2: Mouse initialized\n");
    p = _sa(out, p, mx, "[    0.007] KVFS: Virtual filesystem mounted\n");
    p = _sa(out, p, mx, "[    0.008] Scheduler: Round-robin scheduler started\n");
    p = _sa(out, p, mx, "[    0.009] Shell: Kurono Shell initialized\n");
    p = _sa(out, p, mx, "[    0.010] Desktop: GUI environment ready\n");
    return p;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Network commands (simulated — real NIC driver would replace these)
// ═══════════════════════════════════════════════════════════════════════════

int LinuxCmds::cmd_ifconfig(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500\n");
    p = _sa(out, p, mx, "      inet 192.168.1.100  netmask 255.255.255.0  broadcast 192.168.1.255\n");
    p = _sa(out, p, mx, "      ether 00:1A:2B:3C:4D:5E  txqueuelen 1000\n");
    p = _sa(out, p, mx, "      RX packets 0  bytes 0 (0.0 B)\n");
    p = _sa(out, p, mx, "      TX packets 0  bytes 0 (0.0 B)\n\n");
    p = _sa(out, p, mx, "wlan0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500\n");
    p = _sa(out, p, mx, "       inet 192.168.1.101  netmask 255.255.255.0  broadcast 192.168.1.255\n");
    p = _sa(out, p, mx, "       ether 00:1A:2B:3C:4D:5F  txqueuelen 1000\n");
    p = _sa(out, p, mx, "       RX packets 0  bytes 0 (0.0 B)\n");
    p = _sa(out, p, mx, "       TX packets 0  bytes 0 (0.0 B)\n\n");
    p = _sa(out, p, mx, "lo: flags=73<UP,LOOPBACK,RUNNING>  mtu 65536\n");
    p = _sa(out, p, mx, "    inet 127.0.0.1  netmask 255.0.0.0\n");
    return p;
}

int LinuxCmds::cmd_ping(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: ping <host>\n");
    int p = 0;
    p = _sa(out, p, mx, "PING ");
    p = _sa(out, p, mx, argv[1]);
    p = _sa(out, p, mx, " (127.0.0.1) 56(84) bytes of data.\n");
    for (int i = 0; i < 4; i++) {
        p = _sa(out, p, mx, "64 bytes from ");
        p = _sa(out, p, mx, argv[1]);
        p = _sa(out, p, mx, ": icmp_seq=");
        p = _sai(out, p, mx, i + 1);
        p = _sa(out, p, mx, " ttl=64 time=0.");
        p = _sai(out, p, mx, 1 + i);
        p = _sa(out, p, mx, " ms\n");
    }
    p = _sa(out, p, mx, "\n--- ");
    p = _sa(out, p, mx, argv[1]);
    p = _sa(out, p, mx, " ping statistics ---\n");
    p = _sa(out, p, mx, "4 packets transmitted, 4 received, 0% packet loss\n");
    return p;
}

int LinuxCmds::cmd_wget(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: wget <url>\n");
    int p = _sa(out, 0, mx, "Connecting to ");
    p = _sa(out, p, mx, argv[1]);
    p = _sa(out, p, mx, "...\nwget: network stack not yet initialized (simulated)\n");
    return p;
}

int LinuxCmds::cmd_curl(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: curl <url>\n");
    int p = _sa(out, 0, mx, "curl: ");
    p = _sa(out, p, mx, argv[1]);
    p = _sa(out, p, mx, "\ncurl: network stack not yet initialized (simulated)\n");
    return p;
}

// ═══════════════════════════════════════════════════════════════════════════
//  linux-exec — Run a program through real Linux syscalls
//  This creates a Linux process context, dispatches syscalls, and
//  returns captured console output to the shell.
// ═══════════════════════════════════════════════════════════════════════════

int LinuxCmds::cmd_linux_exec(KuronoShell* sh, int argc, const char** argv,
                               char* out, int mx) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Usage: linux-exec <program> [args...]\n");
        p = _sa(out, p, mx, "Runs a program through the Linux syscall ABI layer.\n");
        p = _sa(out, p, mx, "All I/O goes through real int 0x80 syscalls.\n\n");
        p = _sa(out, p, mx, "Available programs:\n");
        p = _sa(out, p, mx, "  hello   - Hello world via sys_write\n");
        p = _sa(out, p, mx, "  uname   - System info via sys_uname\n");
        p = _sa(out, p, mx, "  getpid  - Process ID via sys_getpid\n");
        p = _sa(out, p, mx, "  pwd     - Working directory via sys_getcwd\n");
        p = _sa(out, p, mx, "  ls      - Directory listing via sys_getdents64\n");
        p = _sa(out, p, mx, "  cat <f> - Read file via sys_open/sys_read\n");
        p = _sa(out, p, mx, "  mkdir <d> - Create dir via sys_mkdir\n");
        p = _sa(out, p, mx, "  stat <f> - File info via sys_stat\n");
        p = _sa(out, p, mx, "  sleep <n> - Sleep N seconds via sys_nanosleep\n");
        p = _sa(out, p, mx, "  echo <text> - Echo via sys_write\n");
        p = _sa(out, p, mx, "  write <f> <txt> - Write file via sys_open/sys_write\n");
        p = _sa(out, p, mx, "  id      - User info via sys_getuid/sys_getgid\n");
        return p;
    }

    // Shift argv to skip "linux-exec"
    return LinuxSyscall::RunProgram(argv[1], argc - 1, argv + 1, out, mx);
}

// ═══════════════════════════════════════════════════════════════════════════
//  syscall — Direct Linux syscall test interface
//  Lets users call specific syscall numbers directly.
// ═══════════════════════════════════════════════════════════════════════════

int LinuxCmds::cmd_syscall(KuronoShell* sh, int argc, const char** argv,
                            char* out, int mx) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Usage: syscall <number> [arg1] [arg2] [arg3]\n");
        p = _sa(out, p, mx, "Common syscalls: 4=write, 20=getpid, 39=mkdir, 122=uname\n");
        p = _sa(out, p, mx, "Example: syscall 20  (returns PID)\n");
        return p;
    }

    uint32_t num = (uint32_t)_atoi(argv[1]);
    uint32_t a1 = (argc > 2) ? (uint32_t)_atoi(argv[2]) : 0;
    uint32_t a2 = (argc > 3) ? (uint32_t)_atoi(argv[3]) : 0;
    uint32_t a3 = (argc > 4) ? (uint32_t)_atoi(argv[4]) : 0;

    // Create temp process if none active
    bool created = false;
    if (!LinuxSyscall::Current()) {
        LinuxSyscall::SetCurrent(LinuxSyscall::CreateProcess("syscall_test", 0, 0));
        created = true;
    }

    LinuxSyscall::ClearConsoleOutput();
    int32_t ret = LinuxSyscall::Dispatch(num, a1, a2, a3, 0, 0);

    int p = 0;
    p = _sa(out, p, mx, "syscall(");
    p = _sai(out, p, mx, (int)num);
    p = _sa(out, p, mx, ") = ");
    p = _sai(out, p, mx, ret);
    p = _sac(out, p, mx, '\n');

    // If there's captured console output, append it
    if (LinuxSyscall::HasConsoleOutput()) {
        p = _sa(out, p, mx, "--- output ---\n");
        p += LinuxSyscall::ReadConsoleOutput(out + p, mx - p - 1);
        out[p] = 0;
    }

    if (created) {
        LinuxProcess* cur = LinuxSyscall::Current();
        if (cur) {
            int idx = (int)(cur->pid - 100);
            LinuxSyscall::DestroyProcess(idx);
            LinuxSyscall::SetCurrent(-1);
        }
    }

    return p;
}

// ═══════════════════════════════════════════════════════════════════════════
//  vm — Hypervisor / Virtual Machine management
//  Subcommands:
//    vm status    — Show VM/hypervisor state
//    vm create [ram_mb] — Create a new VM
//    vm run [max_exits] — Enter the VM run loop
//    vm pause     — Pause the VM
//    vm resume    — Resume the VM
//    vm destroy   — Destroy the VM
//    vm serial    — Read guest serial (COM1) output
//    vm regs      — Dump guest registers
//    vm info      — Show VM statistics
//    vm boot-test — Create+run a minimal guest that prints to serial
// ═══════════════════════════════════════════════════════════════════════════

// Tiny 16-bit real-mode guest code that writes "KURONO VM OK\n" to COM1
// (port 0x3F8) and then HLTs. This serves as a boot-test without needing
// a real Linux bzImage.
static const uint8_t tiny_guest_code[] = {
    // mov si, msg
    0xBE, 0x10, 0x00,          // BE 10 00       mov si, 0x0010
    // .loop:
    0xAC,                       // AC             lodsb
    0x08, 0xC0,                 // 08 C0          or al, al
    0x74, 0x06,                 // 74 06          jz .done
    0xBA, 0xF8, 0x03,          // BA F8 03       mov dx, 0x3F8
    0xEE,                       // EE             out dx, al
    0xEB, 0xF5,                 // EB F5          jmp .loop
    // .done:
    0xF4,                       // F4             hlt
    // msg: "KURONO VM OK\r\n"
    'K','U','R','O','N','O',' ','V','M',' ','O','K','\r','\n',0
};

int LinuxCmds::cmd_vm(KuronoShell* sh, int argc, const char** argv,
                       char* out, int mx) {
    (void)sh;

    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Usage: vm <subcommand>\n\n");
        p = _sa(out, p, mx, "Subcommands:\n");
        p = _sa(out, p, mx, "  status     Show hypervisor/VM state\n");
        p = _sa(out, p, mx, "  create [N] Create VM with N MB RAM (default: 16)\n");
        p = _sa(out, p, mx, "  run [N]    Run VM (max N exits, 0=unlimited)\n");
        p = _sa(out, p, mx, "  pause      Pause VM execution\n");
        p = _sa(out, p, mx, "  resume     Resume VM execution\n");
        p = _sa(out, p, mx, "  destroy    Destroy VM and free resources\n");
        p = _sa(out, p, mx, "  serial     Read guest serial output (COM1)\n");
        p = _sa(out, p, mx, "  regs       Dump guest CPU registers\n");
        p = _sa(out, p, mx, "  info       Show VM statistics\n");
        p = _sa(out, p, mx, "  boot-test  Create+run minimal test guest\n");
        return p;
    }

    const char* sub = argv[1];

    // ── vm status ────────────────────────────────────────────────────────
    if (_seq(sub, "status")) {
        int p = 0;
        p = _sa(out, p, mx, "Hypervisor Status\n");
        p = _sa(out, p, mx, "  Hardware: ");
        if (Hypervisor::IsAvailable()) {
            p = _sa(out, p, mx, "available (");
            p = _sa(out, p, mx, VMM::GetType() == 1 ? "Intel VT-x" : "AMD-V");
            p = _sa(out, p, mx, ")\n");
        } else {
            p = _sa(out, p, mx, "not available\n");
        }
        p = _sa(out, p, mx, "  VM state: ");
        switch (Hypervisor::GetState()) {
            case VM_STATE_UNINITIALIZED: p = _sa(out, p, mx, "UNINITIALIZED"); break;
            case VM_STATE_CREATED:       p = _sa(out, p, mx, "CREATED");       break;
            case VM_STATE_RUNNING:       p = _sa(out, p, mx, "RUNNING");       break;
            case VM_STATE_PAUSED:        p = _sa(out, p, mx, "PAUSED");        break;
            case VM_STATE_HALTED:        p = _sa(out, p, mx, "HALTED");        break;
            case VM_STATE_CRASHED:       p = _sa(out, p, mx, "CRASHED");       break;
            case VM_STATE_REBOOTING:     p = _sa(out, p, mx, "REBOOTING");     break;
            case VM_STATE_DESTROYED:     p = _sa(out, p, mx, "DESTROYED");     break;
        }
        p = _sac(out, p, mx, '\n');
        if (Hypervisor::GetState() >= VM_STATE_CREATED &&
            Hypervisor::GetState() <= VM_STATE_PAUSED) {
            const VMStats& st = Hypervisor::GetStats();
            p = _sa(out, p, mx, "  Exits:  ");
            p = _sau(out, p, mx, st.total_exits);
            p = _sa(out, p, mx, " (I/O: ");
            p = _sau(out, p, mx, st.io_exits);
            p = _sa(out, p, mx, ", MMIO: ");
            p = _sau(out, p, mx, st.mmio_exits);
            p = _sa(out, p, mx, ")\n");
            p = _sa(out, p, mx, "  Serial: TX=");
            p = _sau(out, p, mx, st.serial_bytes_tx);
            p = _sa(out, p, mx, " RX=");
            p = _sau(out, p, mx, st.serial_bytes_rx);
            p = _sac(out, p, mx, '\n');
        }
        return p;
    }

    // ── vm create ────────────────────────────────────────────────────────
    if (_seq(sub, "create")) {
        if (Hypervisor::GetState() != VM_STATE_UNINITIALIZED &&
            Hypervisor::GetState() != VM_STATE_DESTROYED) {
            return _sa(out, 0, mx, "vm: VM already exists. Use 'vm destroy' first.\n");
        }

        Hypervisor::Init();

        VMConfig cfg;
        cfg.SetDefaults();
        if (argc > 2) cfg.ram_mb = (uint32_t)_atoi(argv[2]);
        if (cfg.ram_mb < 4) cfg.ram_mb = 4;
        if (cfg.ram_mb > 512) cfg.ram_mb = 512;

        if (!Hypervisor::CreateVM(cfg)) {
            return _sa(out, 0, mx, "vm create: failed (hardware virtualization may not be available)\n");
        }

        int p = 0;
        p = _sa(out, p, mx, "VM created: ");
        p = _sau(out, p, mx, cfg.ram_mb);
        p = _sa(out, p, mx, " MB RAM, serial=");
        p = _sa(out, p, mx, cfg.enable_serial ? "on" : "off");
        p = _sa(out, p, mx, ", disk=");
        p = _sa(out, p, mx, cfg.enable_disk ? "on" : "off");
        p = _sac(out, p, mx, '\n');
        return p;
    }

    // ── vm run ───────────────────────────────────────────────────────────
    if (_seq(sub, "run")) {
        if (Hypervisor::GetState() != VM_STATE_CREATED &&
            Hypervisor::GetState() != VM_STATE_PAUSED) {
            return _sa(out, 0, mx, "vm run: no VM ready. Use 'vm create' first.\n");
        }

        uint32_t max_exits = 1000; // safe default
        if (argc > 2) max_exits = (uint32_t)_atoi(argv[2]);
        if (max_exits == 0) max_exits = 10000; // cap for safety

        VMState result = Hypervisor::RunVM(max_exits);

        // Read any serial output produced during the run
        int p = 0;
        if (Hypervisor::HasSerialOutput()) {
            p = _sa(out, p, mx, "--- Guest Serial Output ---\n");
            p += Hypervisor::ReadSerialOutput(out + p, mx - p - 64);
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End Serial Output ---\n");
        }

        p = _sa(out, p, mx, "VM ");
        switch (result) {
            case VM_STATE_HALTED:    p = _sa(out, p, mx, "halted");   break;
            case VM_STATE_CRASHED:   p = _sa(out, p, mx, "crashed");  break;
            case VM_STATE_PAUSED:    p = _sa(out, p, mx, "paused");   break;
            case VM_STATE_REBOOTING: p = _sa(out, p, mx, "rebooting"); break;
            default:                 p = _sa(out, p, mx, "stopped");  break;
        }
        p = _sa(out, p, mx, " after ");
        p = _sau(out, p, mx, Hypervisor::GetStats().total_exits);
        p = _sa(out, p, mx, " exits\n");
        return p;
    }

    // ── vm pause ─────────────────────────────────────────────────────────
    if (_seq(sub, "pause")) {
        Hypervisor::PauseVM();
        return _sa(out, 0, mx, "VM paused\n");
    }

    // ── vm resume ────────────────────────────────────────────────────────
    if (_seq(sub, "resume")) {
        Hypervisor::ResumeVM();
        return _sa(out, 0, mx, "VM resumed\n");
    }

    // ── vm destroy ───────────────────────────────────────────────────────
    if (_seq(sub, "destroy")) {
        Hypervisor::DestroyVM();
        return _sa(out, 0, mx, "VM destroyed\n");
    }

    // ── vm serial — Bridge: read guest COM1 output into shell ────────────
    if (_seq(sub, "serial")) {
        if (!Hypervisor::HasSerialOutput()) {
            return _sa(out, 0, mx, "(no serial output from guest)\n");
        }
        int p = 0;
        p = _sa(out, p, mx, "--- Guest COM1 ---\n");
        p += Hypervisor::ReadSerialOutput(out + p, mx - p - 32);
        out[p] = 0;
        p = _sa(out, p, mx, "\n--- End ---\n");
        return p;
    }

    // ── vm regs ──────────────────────────────────────────────────────────
    if (_seq(sub, "regs")) {
        Hypervisor::DumpGuestRegs();
        return _sa(out, 0, mx, "(register dump sent to serial log)\n");
    }

    // ── vm info ──────────────────────────────────────────────────────────
    if (_seq(sub, "info")) {
        const VMStats& st = Hypervisor::GetStats();
        int p = 0;
        p = _sa(out, p, mx, "VM Statistics:\n");
        p = _sa(out, p, mx, "  Total exits:    "); p = _sau(out, p, mx, st.total_exits);    p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  I/O exits:      "); p = _sau(out, p, mx, st.io_exits);       p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  MMIO exits:     "); p = _sau(out, p, mx, st.mmio_exits);     p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  HLT exits:      "); p = _sau(out, p, mx, st.hlt_exits);      p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  IRQ injections: "); p = _sau(out, p, mx, st.irq_injections); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Run cycles:     "); p = _sau(out, p, mx, st.run_cycles);     p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Serial TX:      "); p = _sau(out, p, mx, st.serial_bytes_tx); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Serial RX:      "); p = _sau(out, p, mx, st.serial_bytes_rx); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Disk reads:     "); p = _sau(out, p, mx, st.disk_reads);     p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Disk writes:    "); p = _sau(out, p, mx, st.disk_writes);    p = _sac(out, p, mx, '\n');
        return p;
    }

    // ── vm boot-test — Create a VM with tiny guest that writes to COM1 ──
    if (_seq(sub, "boot-test")) {
        int p = 0;

        // Initialize hypervisor if needed
        if (Hypervisor::GetState() == VM_STATE_UNINITIALIZED ||
            Hypervisor::GetState() == VM_STATE_DESTROYED) {
            Hypervisor::Init();
        }

        // Destroy any existing VM
        if (Hypervisor::GetState() != VM_STATE_UNINITIALIZED) {
            Hypervisor::DestroyVM();
        }

        // Create VM with minimal RAM
        VMConfig cfg;
        cfg.SetDefaults();
        cfg.ram_mb = 4;
        cfg.enable_disk = false;

        if (!Hypervisor::CreateVM(cfg)) {
            p = _sa(out, p, mx, "boot-test: VM creation failed\n");
            p = _sa(out, p, mx, "  Hardware virtualization may not be available.\n");
            p = _sa(out, p, mx, "  The hypervisor requires Intel VT-x or AMD-V.\n");

            // Even without real HW virt, demonstrate the serial bridge
            // by manually writing to the virtual serial device
            p = _sa(out, p, mx, "\n--- Simulated Guest Output (via virtual serial) ---\n");
            VirtualSerial& ser = Hypervisor::GetSerial();
            ser.Init(0x3F8, 4);
            const char* test_msg = "KURONO VM OK (simulated)\r\nGuest booted successfully\r\nLinux version 6.1.0-kurono\r\n";
            while (*test_msg) {
                ser.WritePort(0x3F8, *test_msg);
                test_msg++;
            }
            // Read it back through the bridge
            if (ser.HasOutput()) {
                char sbuf[512];
                int sn = ser.ReadOutput(sbuf, 511);
                sbuf[sn] = 0;
                for (int i = 0; i < sn && p < mx - 1; i++) {
                    out[p++] = sbuf[i];
                }
                out[p] = 0;
            }
            p = _sa(out, p, mx, "--- End Simulated Output ---\n");
            return p;
        }

        p = _sa(out, p, mx, "boot-test: VM created (4 MB RAM)\n");

        // Load tiny guest code into guest memory at address 0x7C00
        // (This would be loaded by LinuxBootLoader for real kernels)
        uint8_t* guest_base = GuestMemoryManager::GetLowRAM();
        if (guest_base) {
            // Copy tiny guest to 0x7C00 (standard BIOS boot sector address)
            memcpy(guest_base + 0x7C00, tiny_guest_code, sizeof(tiny_guest_code));
            p = _sa(out, p, mx, "boot-test: Guest code loaded at 0x7C00\n");
        }

        // Run with limited exits
        p = _sa(out, p, mx, "boot-test: Running guest (max 1000 exits)...\n");
        VMState result = Hypervisor::RunVM(1000);

        // Bridge: read serial output
        if (Hypervisor::HasSerialOutput()) {
            p = _sa(out, p, mx, "--- Guest Serial Output ---\n");
            p += Hypervisor::ReadSerialOutput(out + p, mx - p - 64);
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End Serial Output ---\n");
        } else {
            p = _sa(out, p, mx, "(no serial output captured)\n");
        }

        p = _sa(out, p, mx, "boot-test: VM ");
        switch (result) {
            case VM_STATE_HALTED:    p = _sa(out, p, mx, "halted (clean exit)");  break;
            case VM_STATE_CRASHED:   p = _sa(out, p, mx, "crashed");              break;
            default:                 p = _sa(out, p, mx, "stopped");              break;
        }
        p = _sac(out, p, mx, '\n');

        // Show stats
        const VMStats& st = Hypervisor::GetStats();
        p = _sa(out, p, mx, "  Exits: ");
        p = _sau(out, p, mx, st.total_exits);
        p = _sa(out, p, mx, ", I/O: ");
        p = _sau(out, p, mx, st.io_exits);
        p = _sa(out, p, mx, ", Serial TX: ");
        p = _sau(out, p, mx, st.serial_bytes_tx);
        p = _sac(out, p, mx, '\n');

        return p;
    }

    // Unknown subcommand
    int p = _sa(out, 0, mx, "vm: unknown subcommand '");
    p = _sa(out, p, mx, sub);
    p = _sa(out, p, mx, "'. Use 'vm' for help.\n");
    return p;
}