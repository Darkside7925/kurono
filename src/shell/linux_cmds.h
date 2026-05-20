#pragma once
//  kurono os  -  linux bridge commands
//  posix-like commands operating on kvfs for bare-metal kernel

#include "../shell/shell.h"

namespace LinuxCmds {
    void RegisterAll(KuronoShell* sh);

    // filesystem
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

    // search
    int cmd_find(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_grep(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_which(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // text
    int cmd_tee(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_sort(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_uniq(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_tr(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // system
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
    int cmd_journal(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_lspci(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_lsmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_drivers(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_lsblk(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_lsusb(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_lscpu(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_modprobe(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_modinfo(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_insmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_rmmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_dmidecode(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_hwinfo(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_top(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ip(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ss(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_iotop(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // network
    int cmd_ifconfig(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ping(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_wget(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_curl(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // linux syscall execution
    int cmd_linux_exec(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_syscall(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // hypervisor / vm management
    int cmd_vm(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

    // alpine linux guest vm management
    int cmd_alpine(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_apk(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_debian(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_apt(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
}
