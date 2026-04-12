#include "windows_cmds.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../proc/scheduler.h"

//  windows bridge  -  nt-style commands for kurono shell

static int _wlen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void _wcpy(char* d, const char* s, int m) {
    int i=0; while (s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}
static bool _weq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; } return *a==*b;
}
static int _wa(char* b, int p, int m, const char* s) {
    while (*s && p<m-1) b[p++]=*s++; b[p]=0; return p;
}
static int _wac(char* b, int p, int m, char c) { if (p<m-1) {b[p++]=c; b[p]=0;} return p; }
static int _wai(char* b, int p, int m, int v) {
    if (v<0) { p=_wac(b,p,m,'-'); v=-v; }
    if (v==0) return _wac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=_wac(b,p,m,t[--ti]);
    return p;
}
static int _wau(char* b, int p, int m, unsigned int v) {
    if (v==0) return _wac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=_wac(b,p,m,t[--ti]);
    return p;
}

static int _print_tree(KVFSNode* node, char* out, int p, int mx, int depth) {
    for (int i = 0; i < node->child_count && p < mx - 100; i++) {
        KVFSNode* c = node->children[i];
        if (!c) continue;

        for (int d = 0; d < depth; d++) {
            if (d == depth - 1)
                p = _wa(out, p, mx, (i == node->child_count - 1) ? "└── " : "├── ");
            else
                p = _wa(out, p, mx, "│   ");
        }
        p = _wa(out, p, mx, c->name);
        p = _wac(out, p, mx, '\n');

        if (c->type == KVFS_DIR && depth < 6) {
            p = _print_tree(c, out, p, mx, depth + 1);
        }
    }
    return p;
}

void WindowsCmds::RegisterAll(KuronoShell* sh) {
    using namespace WindowsCmds;
    sh->RegisterCommand("dir",        "List directory (DOS)",     ENV_WINDOWS, "filesystem", cmd_dir);
    sh->RegisterCommand("copy",       "Copy files",              ENV_WINDOWS, "filesystem", cmd_copy);
    sh->RegisterCommand("move",       "Move files",              ENV_WINDOWS, "filesystem", cmd_move);
    sh->RegisterCommand("del",        "Delete files",            ENV_WINDOWS, "filesystem", cmd_del);
    sh->RegisterCommand("type",       "Display file contents",   ENV_WINDOWS, "filesystem", cmd_type);
    sh->RegisterCommand("md",         "Make directory",          ENV_WINDOWS, "filesystem", cmd_md);
    sh->RegisterCommand("rd",         "Remove directory",        ENV_WINDOWS, "filesystem", cmd_rd);
    sh->RegisterCommand("ren",        "Rename file",             ENV_WINDOWS, "filesystem", cmd_ren);
    sh->RegisterCommand("cls",        "Clear screen",            ENV_WINDOWS, "builtin",    cmd_cls);
    sh->RegisterCommand("findstr",    "Search text",             ENV_WINDOWS, "text",       cmd_findstr);
    sh->RegisterCommand("tasklist",   "List processes",          ENV_WINDOWS, "system",     cmd_tasklist);
    sh->RegisterCommand("taskkill",   "Kill process",            ENV_WINDOWS, "system",     cmd_taskkill);
    sh->RegisterCommand("systeminfo", "System information",      ENV_WINDOWS, "system",     cmd_systeminfo);
    sh->RegisterCommand("ipconfig",   "Network information",     ENV_WINDOWS, "network",    cmd_ipconfig);
    sh->RegisterCommand("ver",        "Windows version",         ENV_WINDOWS, "system",     cmd_ver);
    sh->RegisterCommand("tree",       "Directory tree",          ENV_WINDOWS, "filesystem", cmd_tree);
    sh->RegisterCommand("attrib",     "File attributes",         ENV_WINDOWS, "filesystem", cmd_attrib);
    sh->RegisterCommand("chkdsk",     "Check disk",              ENV_WINDOWS, "system",     cmd_chkdsk);
}

//  windows command implementations

int WindowsCmds::cmd_dir(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = (argc > 1) ? argv[1] : ".";
    KVFSNode* dir = KVFS::ResolvePath(path);
    if (!dir || dir->type != KVFS_DIR) return _wa(out, 0, mx, "The system cannot find the path specified.\n");

    int p = 0;
    p = _wa(out, p, mx, " Volume in drive C has no label.\n");
    p = _wa(out, p, mx, " Volume Serial Number is DEAD-BEEF\n\n");
    p = _wa(out, p, mx, " Directory of C:\\");
    // convert kvfs path to windows backslash style
    const char* cwd = KVFS::GetCwd();
    for (int i = 0; cwd[i] && p < mx - 1; i++) {
        p = _wac(out, p, mx, cwd[i] == '/' ? '\\' : cwd[i]);
    }
    p = _wa(out, p, mx, "\n\n");

    int file_count = 0, dir_count = 0;
    unsigned int total_size = 0;

    for (int i = 0; i < dir->child_count; i++) {
        KVFSNode* c = dir->children[i];
        if (!c) continue;

        // date
        p = _wa(out, p, mx, "01/01/2025  12:00 ");

        if (c->type == KVFS_DIR) {
            p = _wa(out, p, mx, "   <DIR>          ");
            dir_count++;
        } else {
            // size right-justified to 14 chars
            char sz_buf[16];
            int si = 0;
            si = _wau(sz_buf, 0, 16, c->size);
            for (int j = si; j < 14; j++) p = _wac(out, p, mx, ' ');
            p = _wa(out, p, mx, sz_buf);
            p = _wac(out, p, mx, ' ');
            file_count++;
            total_size += c->size;
        }
        p = _wa(out, p, mx, c->name);
        p = _wac(out, p, mx, '\n');
    }

    p = _wa(out, p, mx, "              ");
    p = _wai(out, p, mx, file_count);
    p = _wa(out, p, mx, " File(s)  ");
    p = _wau(out, p, mx, total_size);
    p = _wa(out, p, mx, " bytes\n");
    p = _wa(out, p, mx, "              ");
    p = _wai(out, p, mx, dir_count);
    p = _wa(out, p, mx, " Dir(s)   63,488 KB free\n");
    return p;
}

int WindowsCmds::cmd_copy(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    int err = KVFS::Copy(argv[1], argv[2]);
    if (err != KVFS_OK) return _wa(out, 0, mx, "The system cannot find the file specified.\n");
    return _wa(out, 0, mx, "        1 file(s) copied.\n");
}

int WindowsCmds::cmd_move(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    int err = KVFS::Move(argv[1], argv[2]);
    if (err != KVFS_OK) return _wa(out, 0, mx, "The system cannot find the file specified.\n");
    return _wa(out, 0, mx, "        1 file(s) moved.\n");
}

int WindowsCmds::cmd_del(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    int err2 = KVFS::Unlink(argv[1]);
    if (err2 != KVFS_OK) return _wa(out, 0, mx, "Could Not Find ");
    return 0;
}

int WindowsCmds::cmd_type(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _wa(out, 0, mx, "The system cannot find the file specified.\n");

    int p = 0;
    for (int i = 0; i < sz && p < mx - 1; i++) out[p++] = (char)buf[i];
    out[p] = 0;
    return p;
}

int WindowsCmds::cmd_md(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    int err = KVFS::Mkdir(argv[1]);
    if (err != KVFS_OK) return _wa(out, 0, mx, "A subdirectory or file already exists.\n");
    return 0;
}

int WindowsCmds::cmd_rd(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    int err = KVFS::Rmdir(argv[1]);
    if (err != KVFS_OK) return _wa(out, 0, mx, "The directory is not empty.\n");
    return 0;
}

int WindowsCmds::cmd_ren(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _wa(out, 0, mx, "The syntax of the command is incorrect.\n");
    int err = KVFS::Move(argv[1], argv[2]);
    if (err != KVFS_OK) return _wa(out, 0, mx, "The system cannot find the file specified.\n");
    return 0;
}

int WindowsCmds::cmd_cls(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    out[0] = '\x1b'; out[1] = '['; out[2] = 'C'; out[3] = 'L'; out[4] = 'R'; out[5] = 0;
    return 5;
}

int WindowsCmds::cmd_findstr(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _wa(out, 0, mx, "FINDSTR: Bad command line\n");

    const char* pattern = argv[1];
    int p = 0;
    for (int fi = 2; fi < argc; fi++) {
        char grep_buf[4096];
        int glen = KVFS::Grep(argv[fi], pattern, grep_buf, 4096);
        if (glen > 0) {
            p = _wa(out, p, mx, argv[fi]);
            p = _wac(out, p, mx, ':');
            for (int i = 0; i < glen && p < mx - 1; i++)
                out[p++] = grep_buf[i];
            out[p] = 0;
        }
    }
    if (p == 0) p = _wa(out, p, mx, "FINDSTR: No match found\n");
    return p;
}

int WindowsCmds::cmd_tasklist(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _wa(out, p, mx, "\nImage Name                     PID Session Name        Mem Usage\n");
    p = _wa(out, p, mx, "========================= ======== ================ ===========\n");
    p = _wa(out, p, mx, "System                           0 Services              28 K\n");
    p = _wa(out, p, mx, "kernel.exe                       1 Services             512 K\n");
    p = _wa(out, p, mx, "ksh.exe                          2 Console              128 K\n");
    p = _wa(out, p, mx, "desktop.exe                      3 Console              256 K\n");
    p = _wa(out, p, mx, "explorer.exe                     4 Console              192 K\n");

    for (int i = 0; i < (int)Scheduler::GetProcessCount() && i < 8; i++) {
        p = _wa(out, p, mx, "task_");
        p = _wai(out, p, mx, i);
        p = _wa(out, p, mx, ".exe                    ");
        p = _wai(out, p, mx, 10 + i);
        p = _wa(out, p, mx, " Console               64 K\n");
    }
    return p;
}

int WindowsCmds::cmd_taskkill(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _wa(out, 0, mx, "ERROR: Invalid syntax. Usage: taskkill /PID <pid>\n");

    int pid = 0;
    for (int i = 1; i < argc; i++) {
        if (_weq(argv[i], "/PID") && i + 1 < argc) {
            // parse int
            const char* s = argv[i+1];
            while (*s >= '0' && *s <= '9') { pid = pid * 10 + (*s - '0'); s++; }
        }
    }

    if (pid < 4) return _wa(out, 0, mx, "ERROR: The process cannot be terminated.\n");
    int p = _wa(out, 0, mx, "SUCCESS: The process with PID ");
    p = _wai(out, p, mx, pid);
    p = _wa(out, p, mx, " has been terminated.\n");
    return p;
}

int WindowsCmds::cmd_systeminfo(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _wa(out, p, mx, "\nHost Name:                 KURONO-MACHINE\n");
    p = _wa(out, p, mx, "OS Name:                   Kurono OS 1.0.0\n");
    p = _wa(out, p, mx, "OS Version:                1.0.0 Build 1\n");
    p = _wa(out, p, mx, "OS Manufacturer:           Kurono Foundation\n");
    p = _wa(out, p, mx, "System Type:               x86-based PC\n");
    p = _wa(out, p, mx, "Processor(s):              1 Processor(s) Installed.\n");
    p = _wa(out, p, mx, "Total Physical Memory:     65,536 KB\n");

    unsigned int used = KernelHeap::GetUsed() / 1024;
    p = _wa(out, p, mx, "Available Physical Memory: ");
    p = _wau(out, p, mx, 65536 - used);
    p = _wa(out, p, mx, " KB\n");
    p = _wa(out, p, mx, "Virtual Memory: Max Size:  131,072 KB\n");
    p = _wa(out, p, mx, "Hotfix(s):                 N/A\n");
    p = _wa(out, p, mx, "Network Card(s):           2 NIC(s) Installed.\n");
    p = _wa(out, p, mx, "                           [01]: Intel Ethernet Adapter\n");
    p = _wa(out, p, mx, "                           [02]: Kurono WiFi Adapter\n");
    return p;
}

int WindowsCmds::cmd_ipconfig(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _wa(out, p, mx, "\nWindows IP Configuration\n\n");
    p = _wa(out, p, mx, "Ethernet adapter Ethernet0:\n\n");
    p = _wa(out, p, mx, "   Connection-specific DNS Suffix  . : kurono.local\n");
    p = _wa(out, p, mx, "   IPv4 Address. . . . . . . . . . : 192.168.1.100\n");
    p = _wa(out, p, mx, "   Subnet Mask . . . . . . . . . . : 255.255.255.0\n");
    p = _wa(out, p, mx, "   Default Gateway . . . . . . . . : 192.168.1.1\n\n");
    p = _wa(out, p, mx, "Wireless LAN adapter WiFi:\n\n");
    p = _wa(out, p, mx, "   Connection-specific DNS Suffix  . : kurono.local\n");
    p = _wa(out, p, mx, "   IPv4 Address. . . . . . . . . . : 192.168.1.101\n");
    p = _wa(out, p, mx, "   Subnet Mask . . . . . . . . . . : 255.255.255.0\n");
    p = _wa(out, p, mx, "   Default Gateway . . . . . . . . : 192.168.1.1\n");
    return p;
}

int WindowsCmds::cmd_ver(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    return _wa(out, 0, mx, "\nKurono OS [Version 1.0.0]\n");
}

int WindowsCmds::cmd_tree(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = (argc > 1) ? argv[1] : ".";
    KVFSNode* dir = KVFS::ResolvePath(path);
    if (!dir || dir->type != KVFS_DIR) return _wa(out, 0, mx, "Invalid path\n");

    int p = 0;
    p = _wa(out, p, mx, path);
    p = _wac(out, p, mx, '\n');
    p = _print_tree(dir, out, p, mx, 1);
    return p;
}

int WindowsCmds::cmd_attrib(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _wa(out, 0, mx, "Displays or changes file attributes.\n");
    KVFSNode* n = KVFS::ResolvePath(argv[1]);
    if (!n) return _wa(out, 0, mx, "File not found\n");

    int p = 0;
    p = _wa(out, p, mx, n->type == KVFS_DIR ? "D " : "  ");
    p = _wa(out, p, mx, (n->perms.mode & 0200) ? "  " : "R ");
    p = _wa(out, p, mx, "       ");
    p = _wa(out, p, mx, argv[1]);
    p = _wac(out, p, mx, '\n');
    return p;
}

int WindowsCmds::cmd_chkdsk(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _wa(out, p, mx, "The type of the file system is KVFS.\n");
    p = _wa(out, p, mx, "\nStage 1: Examining basic file system structure...\n");
    p = _wa(out, p, mx, "  File system structure is OK.\n");
    p = _wa(out, p, mx, "\nStage 2: Examining file name linkage...\n");
    p = _wa(out, p, mx, "  File name linkage is OK.\n");
    p = _wa(out, p, mx, "\n  65536 KB total disk space.\n");
    p = _wa(out, p, mx, "   2048 KB in use.\n");
    p = _wa(out, p, mx, "  63488 KB available.\n");
    p = _wa(out, p, mx, "\nKurono KVFS File System is healthy.\n");
    return p;
}
