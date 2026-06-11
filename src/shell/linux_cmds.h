#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Bridge Commands
//  POSIX-like commands operating on KVFS for bare-metal kernel
// ═══════════════════════════════════════════════════════════════════════════

#include "../shell/shell.h"

namespace LinuxCmds {
    void RegisterAll(KuronoShell* sh);

    // Filesystem
    int cmd_ls(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_cd(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_mkdir(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_rmdir(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_rm(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_cp(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_mv(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_touch(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_cat(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_head(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_tail(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_wc(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_chmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_stat(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_df(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_du(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ln(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // Search
    int cmd_find(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_grep(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_which(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // Text
    int cmd_tee(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_sort(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_uniq(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_tr(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // System
    int cmd_ps(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_kill(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_uname(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_whoami(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_hostname(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_date(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_free(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_mount(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_dmesg(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // Network
    int cmd_ifconfig(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ping(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_wget(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_curl(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // Linux syscall execution
    int cmd_linux_exec(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_syscall(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // Hypervisor / VM management
    int cmd_vm(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
}
