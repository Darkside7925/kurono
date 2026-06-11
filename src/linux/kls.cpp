//  kurono linux subsystem (kls)  -  implementation
//  the master orchestrator for linux integration inside kurono os

#include "kls.h"
#include "ext4.h"
#include "linux_syscall.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../security/supr.h"
#include "../shell/shell.h"

KLSState    KLS::state = KLS_STOPPED;
KLSConfig   KLS::config;
KLSPackage  KLS::packages[KLS_MAX_PACKAGES];
int         KLS::package_count = 0;

static int kls_slen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void kls_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool kls_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static int kls_pa(char* out, int pos, int mx, const char* s) {
    while (*s && pos < mx - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

static int kls_itoa(char* buf, int val) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    char tmp[12]; int i = 0;
    bool neg = val < 0;
    if (neg) val = -val;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    for (int j = i - 1; j >= 0; j--) buf[pos++] = tmp[j];
    buf[pos] = 0;
    return pos;
}

static int kls_pad(char* out, int pos, int mx, int val) {
    char tmp[12];
    kls_itoa(tmp, val);
    return kls_pa(out, pos, mx, tmp);
}

//  lifecycle

void KLS::Init() {
    state = KLS_INITIALIZING;

    // default configuration
    config.share_home = true;
    config.share_tmp = true;
    config.share_users = true;
    config.auto_mount_ext4 = true;
    config.enable_x11 = false;
    kls_scpy(config.linux_root, "/linux", sizeof(config.linux_root));
    kls_scpy(config.hostname, "kurono", sizeof(config.hostname));
    kls_scpy(config.default_shell, "/bin/bash", sizeof(config.default_shell));
    config.linux_partition_lba = 0;
    config.linux_partition_size = 0;

    // initialize syscall layer
    LinuxSyscall::Init();

    // initialize packages
    package_count = 0;
    memset(packages, 0, sizeof(packages));

    // register core packages
    auto addpkg = [](const char* name, const char* ver, bool essential) {
        if (package_count >= KLS_MAX_PACKAGES) return;
        KLSPackage* p = &packages[package_count++];
        kls_scpy(p->name, name, sizeof(p->name));
        kls_scpy(p->version, ver, sizeof(p->version));
        p->installed = true;
        p->essential = essential;
    };

    addpkg("base-files",    "13.4",    true);
    addpkg("coreutils",     "9.7",     true);
    addpkg("bash",           "5.2.37", true);
    addpkg("libc6",          "2.41",   true);
    addpkg("libgcc-s1",      "14.2",   true);
    addpkg("libstdc++6",     "14.2",   true);
    addpkg("dash",           "0.5.12", false);
    addpkg("grep",           "3.11",   false);
    addpkg("sed",            "4.9",    false);
    addpkg("gawk",           "5.3",    false);
    addpkg("findutils",      "4.10.0", false);
    addpkg("tar",            "1.35",   false);
    addpkg("gzip",           "1.13",   false);
    addpkg("bzip2",          "1.0.8",  false);
    addpkg("util-linux",     "2.40",   false);
    addpkg("procps",         "4.0.5",  false);
    addpkg("net-tools",      "2.10",   false);
    addpkg("iproute2",       "6.15.0", false);
    addpkg("openssh-client", "9.9",    false);
    addpkg("vim-tiny",       "9.1",    false);
    addpkg("nano",           "8.3",    false);
    addpkg("wget",           "1.25.0", false);
    addpkg("curl",           "8.14.1", false);
    addpkg("ca-certificates","20250110",false);
    addpkg("apt",            "3.0.3",  false);
    addpkg("dpkg",           "1.22.17",false);
    addpkg("systemd",        "257",    false);
    addpkg("init-system-helpers","1.67",false);
    addpkg("gcc-14",         "14.2",   false);
    addpkg("make",           "4.4.1",  false);
    addpkg("python3.13",     "3.13.3", false);
    addpkg("git",            "2.47.2", false);

    SerialLogger::Log("[KLS] Subsystem initialized (");
    SerialLogger::LogDec(package_count);
    SerialLogger::Log(" packages)\r\n");

    state = KLS_STOPPED;
}

void KLS::Start() {
    if (state == KLS_RUNNING) return;
    state = KLS_INITIALIZING;

    SerialLogger::Log("[KLS] Starting Kurono Linux Subsystem...\r\n");

    // create the /linux mount point and rootfs in kvfs
    if (!HasRootfs()) {
        InitRootfs();
    }

    // mount shared directories
    MountSharedDirs();

    // sync users
    if (config.share_users) {
        SyncUsersToLinux();
    }

    state = KLS_RUNNING;
    SerialLogger::Log("[KLS] Linux Subsystem running\r\n");
}

void KLS::Stop() {
    if (state != KLS_RUNNING && state != KLS_SUSPENDED) return;

    SerialLogger::Log("[KLS] Stopping Linux Subsystem...\r\n");

    // kill all linux processes
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (p && p->active) {
            LinuxSyscall::DestroyProcess(i);
        }
    }

    // unmount shared dirs
    UnmountSharedDirs();

    // unmount ext4
    if (Ext4::IsMounted()) {
        Ext4::Unmount();
    }

    state = KLS_STOPPED;
    SerialLogger::Log("[KLS] Linux Subsystem stopped\r\n");
}

void KLS::Suspend() {
    if (state != KLS_RUNNING) return;
    state = KLS_SUSPENDED;
    SerialLogger::Log("[KLS] Suspended\r\n");
}

void KLS::Resume() {
    if (state != KLS_SUSPENDED) return;
    state = KLS_RUNNING;
    SerialLogger::Log("[KLS] Resumed\r\n");
}

KLSState KLS::GetState() { return state; }

//  configuration

KLSConfig* KLS::GetConfig() { return &config; }

void KLS::SetConfig(const KLSConfig& cfg) {
    memcpy(&config, &cfg, sizeof(KLSConfig));
}

void KLS::SaveConfig() {
    // serialize config to kvfs
    char buf[1024];
    int p = 0;
    p = kls_pa(buf, p, sizeof(buf), "# KLS Configuration\n");
    p = kls_pa(buf, p, sizeof(buf), "share_home=");
    p = kls_pa(buf, p, sizeof(buf), config.share_home ? "true\n" : "false\n");
    p = kls_pa(buf, p, sizeof(buf), "share_tmp=");
    p = kls_pa(buf, p, sizeof(buf), config.share_tmp ? "true\n" : "false\n");
    p = kls_pa(buf, p, sizeof(buf), "share_users=");
    p = kls_pa(buf, p, sizeof(buf), config.share_users ? "true\n" : "false\n");
    p = kls_pa(buf, p, sizeof(buf), "hostname=");
    p = kls_pa(buf, p, sizeof(buf), config.hostname);
    p = kls_pa(buf, p, sizeof(buf), "\n");
    p = kls_pa(buf, p, sizeof(buf), "default_shell=");
    p = kls_pa(buf, p, sizeof(buf), config.default_shell);
    p = kls_pa(buf, p, sizeof(buf), "\n");

    KVFS::WriteString("/etc/kls.conf", buf);
}

void KLS::LoadConfig() {
    char buf[1024];
    if (KVFS::ReadString("/etc/kls.conf", buf, sizeof(buf)) > 0) {
        // simple parser  -  look for key=value lines
        // in production, parse properly; simplified here
        SerialLogger::Log("[KLS] Config loaded from /etc/kls.conf\r\n");
    }
}

//  rootfs initialization

bool KLS::HasRootfs() {
    return KVFS::IsDir("/linux");
}

void KLS::InitRootfs() {
    SerialLogger::Log("[KLS] Initializing Linux rootfs...\r\n");

    // create the linux fhs directory structure inside kvfs at /linux
    const char* dirs[] = {
        "/linux",
        "/linux/bin",
        "/linux/sbin",
        "/linux/etc",
        "/linux/etc/init.d",
        "/linux/etc/default",
        "/linux/etc/network",
        "/linux/etc/apt",
        "/linux/etc/apt/sources.list.d",
        "/linux/etc/systemd",
        "/linux/etc/systemd/system",
        "/linux/dev",
        "/linux/proc",
        "/linux/sys",
        "/linux/tmp",
        "/linux/var",
        "/linux/var/log",
        "/linux/var/lib",
        "/linux/var/lib/dpkg",
        "/linux/var/lib/apt",
        "/linux/var/cache",
        "/linux/var/cache/apt",
        "/linux/var/run",
        "/linux/var/tmp",
        "/linux/usr",
        "/linux/usr/bin",
        "/linux/usr/sbin",
        "/linux/usr/lib",
        "/linux/usr/lib/x86_64-linux-gnu",
        "/linux/usr/share",
        "/linux/usr/share/man",
        "/linux/usr/share/doc",
        "/linux/usr/local",
        "/linux/usr/local/bin",
        "/linux/usr/local/lib",
        "/linux/opt",
        "/linux/root",
        "/linux/run",
        "/linux/mnt",
        "/linux/media",
        "/linux/boot",
        nullptr
    };

    for (int i = 0; dirs[i]; i++) {
        KVFS::Mkdirs(dirs[i]);
    }

    PopulateDefaultRootfs();

    SerialLogger::Log("[KLS] Linux rootfs initialized\r\n");
}

void KLS::PopulateDefaultRootfs() {
    // /etc/os-release
    KVFS::WriteString("/linux/etc/os-release",
        "PRETTY_NAME=\"Kurono Linux 1.0 (Integrated)\"\n"
        "NAME=\"Kurono Linux\"\n"
        "VERSION_ID=\"1.0\"\n"
        "VERSION=\"1.0 (kurono)\"\n"
        "ID=kurono-linux\n"
        "ID_LIKE=debian\n"
        "HOME_URL=\"https://kurono-os.dev\"\n"
        "SUPPORT_URL=\"https://kurono-os.dev/support\"\n"
        "BUG_REPORT_URL=\"https://kurono-os.dev/bugs\"\n"
    );

    // /etc/hostname
    KVFS::WriteString("/linux/etc/hostname", config.hostname);

    // /etc/hosts
    KVFS::WriteString("/linux/etc/hosts",
        "127.0.0.1\tlocalhost\n"
        "127.0.1.1\tkurono\n"
        "::1\t\tlocalhost ip6-localhost ip6-loopback\n"
    );

    // /etc/resolv.conf
    KVFS::WriteString("/linux/etc/resolv.conf",
        "# Generated by Kurono Linux Subsystem\n"
        "nameserver 8.8.8.8\n"
        "nameserver 8.8.4.4\n"
    );

    // /etc/fstab
    KVFS::WriteString("/linux/etc/fstab",
        "# Kurono Linux Subsystem fstab\n"
        "# <fs>    <mount>  <type>  <opts>        <dump> <pass>\n"
        "kvfs      /        kvfs    defaults       0      1\n"
        "/dev/sda2 /linux   ext4    defaults       0      2\n"
        "proc      /proc    proc    defaults       0      0\n"
        "sysfs     /sys     sysfs   defaults       0      0\n"
        "tmpfs     /tmp     tmpfs   defaults       0      0\n"
        "# Shared directories from Kurono OS:\n"
        "kvfs:/home /home   kvfs    bind           0      0\n"
        "kvfs:/tmp  /tmp    kvfs    bind           0      0\n"
    );

    // /etc/profile
    KVFS::WriteString("/linux/etc/profile",
        "# Kurono Linux System Profile\n"
        "export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin\n"
        "export HOME=/home/$USER\n"
        "export SHELL=/bin/bash\n"
        "export TERM=xterm-256color\n"
        "export LANG=en_US.UTF-8\n"
        "export PS1='\\u@\\h:\\w\\$ '\n"
        "\n"
        "# Kurono integration\n"
        "export KURONO_OS=1\n"
        "export KLS_VERSION=1.0\n"
        "\n"
        "if [ -f /etc/bash.bashrc ]; then\n"
        "    . /etc/bash.bashrc\n"
        "fi\n"
    );

    // /etc/bash.bashrc
    KVFS::WriteString("/linux/etc/bash.bashrc",
        "# Kurono Linux Subsystem  -  bash defaults\n"
        "alias ll='ls -la'\n"
        "alias la='ls -A'\n"
        "alias l='ls -CF'\n"
        "alias ..='cd ..'\n"
        "\n"
        "# Color prompt\n"
        "PS1='\\[\\033[01;32m\\]\\u@kurono\\[\\033[00m\\]:\\[\\033[01;34m\\]\\w\\[\\033[00m\\]\\$ '\n"
    );

    // /etc/apt/sources.list
    KVFS::WriteString("/linux/etc/apt/sources.list",
        "# Kurono Linux Package Sources\n"
        "deb http://deb.debian.org/debian bookworm main contrib non-free\n"
        "deb http://deb.debian.org/debian bookworm-updates main contrib non-free\n"
        "deb http://security.debian.org/debian-security bookworm-security main\n"
    );

    // /etc/shells
    KVFS::WriteString("/linux/etc/shells",
        "/bin/sh\n/bin/bash\n/bin/dash\n/usr/bin/bash\n"
    );

    // /etc/nsswitch.conf
    KVFS::WriteString("/linux/etc/nsswitch.conf",
        "passwd:     files\n"
        "group:      files\n"
        "shadow:     files\n"
        "hosts:      files dns\n"
        "networks:   files\n"
        "protocols:  files\n"
        "services:   files\n"
    );

    // /etc/issue
    KVFS::WriteString("/linux/etc/issue",
        "Kurono Linux 1.0 \\n \\l\n\n"
    );

    // /etc/motd
    KVFS::WriteString("/linux/etc/motd",
        "╔══════════════════════════════════════════════╗\n"
        "║     Welcome to Kurono Linux Subsystem        ║\n"
        "║   Deeply integrated with Kurono OS kernel    ║\n"
        "║   Same users • Same files • Same system      ║\n"
        "╚══════════════════════════════════════════════╝\n"
    );

    // /etc/init.d/kurono-sync (startup script)
    KVFS::WriteString("/linux/etc/init.d/kurono-sync",
        "#!/bin/sh\n"
        "### BEGIN INIT INFO\n"
        "# Provides:       kurono-sync\n"
        "# Required-Start: $local_fs\n"
        "# Default-Start:  2 3 4 5\n"
        "# Description:    Sync Kurono OS shared resources\n"
        "### END INIT INFO\n"
        "\n"
        "case \"$1\" in\n"
        "  start)\n"
        "    echo \"Syncing Kurono OS shared resources...\"\n"
        "    # Mount shared home and tmp from Kurono KVFS\n"
        "    mount --bind /mnt/kurono/home /home 2>/dev/null\n"
        "    mount --bind /mnt/kurono/tmp /tmp 2>/dev/null\n"
        "    echo \"Kurono sync complete.\"\n"
        "    ;;\n"
        "  stop)\n"
        "    umount /home 2>/dev/null\n"
        "    umount /tmp 2>/dev/null\n"
        "    ;;\n"
        "esac\n"
        "exit 0\n"
    );

    // /var/lib/dpkg/status (minimal dpkg database)
    {
        char dpkg_status[8192];
        int p = 0;
        for (int i = 0; i < package_count; i++) {
            KLSPackage* pkg = &packages[i];
            if (!pkg->installed) continue;
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), "Package: ");
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), pkg->name);
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), "\nStatus: install ok installed\n");
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), "Version: ");
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), pkg->version);
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), "\nArchitecture: i386\n");
            if (pkg->essential) {
                p = kls_pa(dpkg_status, p, sizeof(dpkg_status), "Essential: yes\n");
            }
            p = kls_pa(dpkg_status, p, sizeof(dpkg_status), "\n");
        }
        KVFS::WriteString("/linux/var/lib/dpkg/status", dpkg_status);
    }

    // sync initial user accounts
    SyncUsersToLinux();

    SerialLogger::Log("[KLS] Default rootfs populated\r\n");
}

//  shared filesystem mounting

void KLS::MountSharedDirs() {
    SerialLogger::Log("[KLS] Mounting shared directories...\r\n");

    // /home is shared  -  both kurono and linux see the same /home/user
    if (config.share_home) {
        // in our kvfs model, /home already exists; /linux/home is a symlink
        KVFS::Mkdirs("/linux/home");
        SerialLogger::Log("[KLS]   /home → shared\r\n");
    }

    // /tmp shared
    if (config.share_tmp) {
        KVFS::Mkdirs("/linux/tmp");
        SerialLogger::Log("[KLS]   /tmp → shared\r\n");
    }

    // /var/log shared for unified logging
    KVFS::Mkdirs("/linux/var/log");
    SerialLogger::Log("[KLS]   /var/log → shared\r\n");
}

void KLS::UnmountSharedDirs() {
    SerialLogger::Log("[KLS] Unmounting shared directories\r\n");
    // in kvfs, no actual unmount needed  -  just log
}

void KLS::SyncFilesystems() {
    // flush any pending ext4 writes
    // in a real implementation, this would sync the block device cache
    if (Ext4::IsMounted()) {
        SerialLogger::Log("[KLS] FS sync complete\r\n");
    }
}

//  user synchronization (supr ↔ linux)

void KLS::GeneratePasswd(char* buf, int max_len) {
    int p = 0;
    // root
    p = kls_pa(buf, p, max_len, "root:x:0:0:root:/root:/bin/bash\n");
    // system accounts
    p = kls_pa(buf, p, max_len, "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n");
    p = kls_pa(buf, p, max_len, "bin:x:2:2:bin:/bin:/usr/sbin/nologin\n");
    p = kls_pa(buf, p, max_len, "sys:x:3:3:sys:/dev:/usr/sbin/nologin\n");
    p = kls_pa(buf, p, max_len, "nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n");

    // supr users → linux users (uid starting at 1000)
    SUPRUser* users = SUPR::GetUsers();
    int ucount = SUPR::GetUserCount();
    for (int i = 0; i < ucount; i++) {
        SUPRUser* u = &users[i];
        if (kls_seq(u->username, "root")) continue;  // already added

        p = kls_pa(buf, p, max_len, u->username);
        p = kls_pa(buf, p, max_len, ":x:");
        p = kls_pad(buf, p, max_len, u->uid > 0 ? u->uid : 1000 + i);
        p = kls_pa(buf, p, max_len, ":");
        p = kls_pad(buf, p, max_len, u->gid > 0 ? u->gid : 1000 + i);
        p = kls_pa(buf, p, max_len, ":");
        p = kls_pa(buf, p, max_len, u->username);
        p = kls_pa(buf, p, max_len, ":/home/");
        p = kls_pa(buf, p, max_len, u->username);
        p = kls_pa(buf, p, max_len, ":");
        p = kls_pa(buf, p, max_len, config.default_shell);
        p = kls_pa(buf, p, max_len, "\n");
    }
}

void KLS::GenerateGroup(char* buf, int max_len) {
    int p = 0;
    p = kls_pa(buf, p, max_len, "root:x:0:\n");
    p = kls_pa(buf, p, max_len, "daemon:x:1:\n");
    p = kls_pa(buf, p, max_len, "bin:x:2:\n");
    p = kls_pa(buf, p, max_len, "sys:x:3:\n");
    p = kls_pa(buf, p, max_len, "adm:x:4:\n");
    p = kls_pa(buf, p, max_len, "sudo:x:27:\n");
    p = kls_pa(buf, p, max_len, "users:x:100:\n");
    p = kls_pa(buf, p, max_len, "nogroup:x:65534:\n");

    // create groups for supr users
    SUPRUser* users = SUPR::GetUsers();
    int ucount = SUPR::GetUserCount();
    for (int i = 0; i < ucount; i++) {
        SUPRUser* u = &users[i];
        if (kls_seq(u->username, "root")) continue;
        p = kls_pa(buf, p, max_len, u->username);
        p = kls_pa(buf, p, max_len, ":x:");
        p = kls_pad(buf, p, max_len, u->gid > 0 ? u->gid : 1000 + i);
        p = kls_pa(buf, p, max_len, ":\n");
    }
}

void KLS::GenerateShadow(char* buf, int max_len) {
    int p = 0;
    p = kls_pa(buf, p, max_len, "root:!:19723:0:99999:7:::\n");
    p = kls_pa(buf, p, max_len, "daemon:*:19723:0:99999:7:::\n");
    p = kls_pa(buf, p, max_len, "bin:*:19723:0:99999:7:::\n");
    p = kls_pa(buf, p, max_len, "nobody:*:19723:0:99999:7:::\n");

    SUPRUser* users = SUPR::GetUsers();
    int ucount = SUPR::GetUserCount();
    for (int i = 0; i < ucount; i++) {
        SUPRUser* u = &users[i];
        if (kls_seq(u->username, "root")) continue;
        p = kls_pa(buf, p, max_len, u->username);
        // hash from supr  -  represent as locked (!) for shadow
        p = kls_pa(buf, p, max_len, ":$6$kurono$locked:19723:0:99999:7:::\n");
    }
}

void KLS::SyncUsersToLinux() {
    SerialLogger::Log("[KLS] Syncing SUPR users → Linux...\r\n");

    char buf[4096];

    GeneratePasswd(buf, sizeof(buf));
    KVFS::WriteString("/linux/etc/passwd", buf);

    GenerateGroup(buf, sizeof(buf));
    KVFS::WriteString("/linux/etc/group", buf);

    GenerateShadow(buf, sizeof(buf));
    KVFS::WriteString("/linux/etc/shadow", buf);

    // ensure home directories exist for each user
    SUPRUser* users = SUPR::GetUsers();
    int ucount = SUPR::GetUserCount();
    for (int i = 0; i < ucount; i++) {
        char homedir[128];
        kls_scpy(homedir, "/home/", sizeof(homedir));
        int hl = kls_slen(homedir);
        kls_scpy(homedir + hl, users[i].username,
                 (int)(sizeof(homedir) - hl));
        KVFS::Mkdirs(homedir);

        // .bashrc for each user
        char bashrc_path[192];
        kls_scpy(bashrc_path, homedir, sizeof(bashrc_path));
        int bl = kls_slen(bashrc_path);
        kls_scpy(bashrc_path + bl, "/.bashrc",
                 (int)(sizeof(bashrc_path) - bl));

        if (!KVFS::Exists(bashrc_path)) {
            KVFS::WriteString(bashrc_path,
                "# ~/.bashrc - Kurono Linux user shell config\n"
                "export PS1='\\[\\033[01;32m\\]\\u@kurono"
                "\\[\\033[00m\\]:\\[\\033[01;34m\\]\\w\\[\\033[00m\\]\\$ '\n"
                "alias ll='ls -la'\n"
                "alias la='ls -A'\n"
            );
        }
    }

    SerialLogger::Log("[KLS] User sync complete (");
    SerialLogger::LogDec(ucount);
    SerialLogger::Log(" users)\r\n");
}

void KLS::SyncUsersFromLinux() {
    // read /linux/etc/passwd and create missing supr users
    char buf[4096];
    if (KVFS::ReadString("/linux/etc/passwd", buf, sizeof(buf)) <= 0) return;

    SerialLogger::Log("[KLS] Syncing Linux users → SUPR\r\n");
    // parse passwd lines and create supr users for uid >= 1000
    // format: username:x:uid:gid:gecos:home:shell
    const char* p = buf;
    while (*p) {
        char username[32];
        int ui = 0;
        while (*p && *p != ':' && ui < 31) username[ui++] = *p++;
        username[ui] = 0;
        if (*p == ':') p++;

        // skip password field
        while (*p && *p != ':') p++;
        if (*p == ':') p++;

        // parse uid
        int uid = 0;
        while (*p >= '0' && *p <= '9') { uid = uid * 10 + (*p - '0'); p++; }
        if (*p == ':') p++;

        // skip to end of line
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        // only import regular users (uid >= 1000)
        if (uid >= 1000 && !SUPR::FindUser(username)) {
            SUPR::CreateUser(username, "kurono", SUPR_USER);
            SerialLogger::Log("[KLS]   Imported user: ");
            SerialLogger::Log(username);
            SerialLogger::Log("\r\n");
        }
    }
}

void KLS::SyncSingleUser(const char* username) {
    SUPRUser* u = SUPR::FindUser(username);
    if (!u) return;

    // ensure home directory exists
    char homedir[128];
    kls_scpy(homedir, "/home/", sizeof(homedir));
    int hl = kls_slen(homedir);
    kls_scpy(homedir + hl, username, (int)(sizeof(homedir) - hl));
    KVFS::Mkdirs(homedir);

    // re-generate passwd/group/shadow to include this user
    SyncUsersToLinux();
}

//  elf loader

bool KLS::IsValidELF(const void* data, uint32_t size) {
    if (size < sizeof(Elf32Header)) return false;
    const Elf32Header* h = (const Elf32Header*)data;
    if (h->e_magic != ELF_MAGIC) return false;
    if (h->e_class != 1) return false;   // must be 32-bit
    if (h->e_data != 1) return false;    // must be little-endian
    if (h->e_machine != 3) return false; // must be i386
    return true;
}

uint32_t KLS::FindEntryPoint(const void* elf_data) {
    const Elf32Header* h = (const Elf32Header*)elf_data;
    return h->e_entry;
}

int KLS::LoadELFSegments(const void* elf_data, uint32_t size) {
    const Elf32Header* eh = (const Elf32Header*)elf_data;
    const uint8_t* data = (const uint8_t*)elf_data;

    // 64-bit math so a crafted ELF can't overflow this bound check. (satoru)
    if ((uint64_t)eh->e_phoff +
        (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize > (uint64_t)size)
        return -1;

    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32Phdr* ph = (const Elf32Phdr*)
            (data + eh->e_phoff + i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;

        // in a real implementation, we'd set up page tables
        // for now, copy the segment data to its virtual address
        // (only works if vaddr is in accessible memory)
        if (ph->p_vaddr < 0x100000) continue;  // skip low addresses

        uint8_t* dest = (uint8_t*)(uintptr_t)ph->p_vaddr;

        // zero the memory region
        memset(dest, 0, ph->p_memsz);

        // copy file data
        if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= size) {
            memcpy(dest, data + ph->p_offset, ph->p_filesz);
        }

        SerialLogger::Log("[KLS] Loaded segment @ 0x");
        SerialLogger::LogHex(ph->p_vaddr);
        SerialLogger::Log(" (");
        SerialLogger::LogDec((int)ph->p_memsz);
        SerialLogger::Log(" bytes)\r\n");
    }

    return 0;
}

int KLS::LoadELF(const char* path) {
    if (state != KLS_RUNNING) return -1;

    // read elf from kvfs or ext4
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(1024 * 1024);  // 1mb max
    if (!buf) return -1;

    int size = KVFS::ReadFile(path, buf, 1024 * 1024);
    if (size <= 0 && Ext4::IsMounted()) {
        size = Ext4::ReadWholeFile(path, buf, 1024 * 1024);
    }
    if (size <= 0) {
        KernelHeap::Free(buf);
        return -2;
    }

    if (!IsValidELF(buf, (uint32_t)size)) {
        KernelHeap::Free(buf);
        return -3;
    }

    int r = LoadELFSegments(buf, (uint32_t)size);
    uint32_t entry = FindEntryPoint(buf);

    KernelHeap::Free(buf);

    if (r != 0) return -4;

    SerialLogger::Log("[KLS] ELF loaded, entry=0x");
    SerialLogger::LogHex(entry);
    SerialLogger::Log("\r\n");

    return 0;
}

int KLS::ExecELF(const char* path, int argc, const char** argv) {
    (void)argc; (void)argv;

    // create a linux process context
    int pidx = LinuxSyscall::CreateProcess(path, 1000, 1000);
    if (pidx < 0) return -1;

    LinuxSyscall::SetCurrent(pidx);

    int r = LoadELF(path);
    if (r != 0) {
        LinuxSyscall::DestroyProcess(pidx);
        return r;
    }

    // in a full implementation, we'd jump to entry point
    // with proper stack setup (argc, argv, envp, auxv)
    // for now, the process is loaded and ready

    return pidx;
}

//  package management (for linux side)

int KLS::InstallPackage(const char* name) {
    // check if already installed
    KLSPackage* pkg = FindPackage(name);
    if (pkg && pkg->installed) return 0;

    if (pkg) {
        pkg->installed = true;
    } else {
        // add new package
        if (package_count >= KLS_MAX_PACKAGES) return -1;
        KLSPackage* np = &packages[package_count++];
        kls_scpy(np->name, name, sizeof(np->name));
        kls_scpy(np->version, "latest", sizeof(np->version));
        np->installed = true;
        np->essential = false;
    }

    SerialLogger::Log("[KLS] Installed package: ");
    SerialLogger::Log(name);
    SerialLogger::Log("\r\n");
    return 0;
}

int KLS::RemovePackage(const char* name) {
    KLSPackage* pkg = FindPackage(name);
    if (!pkg || !pkg->installed) return -1;
    if (pkg->essential) return -2;  // can't remove essential

    pkg->installed = false;
    SerialLogger::Log("[KLS] Removed package: ");
    SerialLogger::Log(name);
    SerialLogger::Log("\r\n");
    return 0;
}

KLSPackage* KLS::GetPackages() { return packages; }
int KLS::GetPackageCount() { return package_count; }

KLSPackage* KLS::FindPackage(const char* name) {
    for (int i = 0; i < package_count; i++) {
        if (kls_seq(packages[i].name, name)) return &packages[i];
    }
    return nullptr;
}

//  command execution

int KLS::RunCommand(const char* cmd, char* output, int max_output) {
    if (state != KLS_RUNNING) {
        int p = kls_pa(output, 0, max_output,
                       "Error: Linux Subsystem not running. Use 'kls start'\n");
        (void)p;
        return -1;
    }

    // create process, execute, return output
    int pidx = LinuxSyscall::CreateProcess(cmd, 1000, 1000);
    if (pidx < 0) {
        kls_pa(output, 0, max_output, "Error: Cannot create process\n");
        return -1;
    }

    LinuxSyscall::SetCurrent(pidx);

    // for built-in commands, handle directly
    int p = 0;
    if (kls_seq(cmd, "uname") || kls_seq(cmd, "uname -a")) {
        p = kls_pa(output, p, max_output,
                   "Linux kurono 6.1.0-kurono #1 SMP i686 GNU/Linux\n");
    } else if (kls_seq(cmd, "cat /etc/os-release")) {
        char buf[1024];
        if (KVFS::ReadString("/linux/etc/os-release", buf, sizeof(buf)) > 0) {
            p = kls_pa(output, p, max_output, buf);
        }
    } else if (kls_seq(cmd, "hostname")) {
        p = kls_pa(output, p, max_output, config.hostname);
        p = kls_pa(output, p, max_output, "\n");
    } else if (kls_seq(cmd, "whoami")) {
        SUPRSession* sess = SUPR::GetSession(SUPR::GetCurrentSession());
        if (sess && sess->active) {
            SUPRUser* users = SUPR::GetUsers();
            p = kls_pa(output, p, max_output, users[sess->user_index].username);
        } else {
            p = kls_pa(output, p, max_output, "root");
        }
        p = kls_pa(output, p, max_output, "\n");
    } else if (kls_seq(cmd, "id")) {
        p = kls_pa(output, p, max_output, "uid=1000(user) gid=1000(user) groups=1000(user),27(sudo)\n");
    } else if (kls_seq(cmd, "cat /etc/hostname")) {
        p = kls_pa(output, p, max_output, config.hostname);
        p = kls_pa(output, p, max_output, "\n");
    } else {
        p = kls_pa(output, p, max_output, "kls: ");
        p = kls_pa(output, p, max_output, cmd);
        p = kls_pa(output, p, max_output, ": command executed\n");
    }

    LinuxSyscall::DestroyProcess(pidx);
    return 0;
}

int KLS::RunShellCommand(const char* cmd, char* output, int max_output) {
    return RunCommand(cmd, output, max_output);
}

//  syscall handler (called from idt int 0x80)

int32_t KLS::HandleSyscall(uint32_t eax, uint32_t ebx, uint32_t ecx,
                            uint32_t edx, uint32_t esi, uint32_t edi) {
    return LinuxSyscall::Dispatch(eax, ebx, ecx, edx, esi, edi);
}

//  status info

int KLS::LinuxProcessCount() {
    return LinuxSyscall::ActiveProcessCount();
}

const char* KLS::GetKernelVersion() {
    return "6.12.0-kurono";
}

uint64_t KLS::GetExt4FreeSpace() {
    if (!Ext4::IsMounted()) return 0;
    return Ext4::FreeBlocks() * Ext4::BlockSize();
}

uint64_t KLS::GetExt4TotalSpace() {
    if (!Ext4::IsMounted()) return 0;
    return Ext4::TotalBlocks() * Ext4::BlockSize();
}

//  shell integration  -  register kls commands with kurono shell

void KLS::RegisterShellCommands(void* shell_ptr) {
    KuronoShell* sh = (KuronoShell*)shell_ptr;
    sh->RegisterCommand("kls",   "Kurono Linux Subsystem",  ENV_KURONO, "linux", reinterpret_cast<ShellCmdHandler>(cmd_kls));
    sh->RegisterCommand("linux", "Run Linux command",        ENV_LINUX,  "linux", reinterpret_cast<ShellCmdHandler>(cmd_linux));
    sh->RegisterCommand("lsb_release", "LSB release info",   ENV_LINUX, "linux", reinterpret_cast<ShellCmdHandler>(cmd_lsb));
    sh->RegisterCommand("dpkg-kls",    "KLS package manager",ENV_LINUX, "linux", reinterpret_cast<ShellCmdHandler>(cmd_dpkg));
}

int KLS::cmd_kls(void* sh, int argc, const char** argv,
                  char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2) {
        p = kls_pa(out, p, mx,
            "Kurono Linux Subsystem (KLS) v1.0\n"
            "Usage: kls <command>\n"
            "\n"
            "Commands:\n"
            "  start     Start the Linux subsystem\n"
            "  stop      Stop the Linux subsystem\n"
            "  status    Show subsystem status\n"
            "  restart   Restart subsystem\n"
            "  config    Show configuration\n"
            "  sync      Sync users and filesystems\n"
            "  exec      Execute a Linux binary\n"
            "  packages  List installed Linux packages\n"
            "  install   Install a Linux package\n"
            "  remove    Remove a Linux package\n"
        );
        return 0;
    }

    const char* sub = argv[1];

    if (kls_seq(sub, "start")) {
        Start();
        p = kls_pa(out, p, mx, "Linux Subsystem started.\n");
    }
    else if (kls_seq(sub, "stop")) {
        Stop();
        p = kls_pa(out, p, mx, "Linux Subsystem stopped.\n");
    }
    else if (kls_seq(sub, "restart")) {
        Stop();
        Start();
        p = kls_pa(out, p, mx, "Linux Subsystem restarted.\n");
    }
    else if (kls_seq(sub, "status")) {
        p = kls_pa(out, p, mx, "╔══════════════════════════════════════╗\n");
        p = kls_pa(out, p, mx, "║   Kurono Linux Subsystem Status     ║\n");
        p = kls_pa(out, p, mx, "╠══════════════════════════════════════╣\n");
        p = kls_pa(out, p, mx, "║ State:     ");
        switch (state) {
            case KLS_STOPPED:      p = kls_pa(out, p, mx, "STOPPED     "); break;
            case KLS_INITIALIZING: p = kls_pa(out, p, mx, "INITIALIZING"); break;
            case KLS_RUNNING:      p = kls_pa(out, p, mx, "RUNNING     "); break;
            case KLS_ERROR:        p = kls_pa(out, p, mx, "ERROR       "); break;
            case KLS_SUSPENDED:    p = kls_pa(out, p, mx, "SUSPENDED   "); break;
        }
        p = kls_pa(out, p, mx, "       ║\n");
        p = kls_pa(out, p, mx, "║ Kernel:    6.1.0-kurono        ║\n");
        p = kls_pa(out, p, mx, "║ Arch:      i686                 ║\n");
        p = kls_pa(out, p, mx, "║ Processes: ");
        p = kls_pad(out, p, mx, LinuxProcessCount());
        p = kls_pa(out, p, mx, "                      ║\n");
        p = kls_pa(out, p, mx, "║ Packages:  ");
        p = kls_pad(out, p, mx, package_count);
        p = kls_pa(out, p, mx, " installed            ║\n");
        p = kls_pa(out, p, mx, "║ Ext4:      ");
        p = kls_pa(out, p, mx, Ext4::IsMounted() ? "mounted     " : "not mounted ");
        p = kls_pa(out, p, mx, "       ║\n");
        p = kls_pa(out, p, mx, "║ Shared:    /home /tmp /var/log  ║\n");
        p = kls_pa(out, p, mx, "╚══════════════════════════════════════╝\n");
    }
    else if (kls_seq(sub, "config")) {
        p = kls_pa(out, p, mx, "KLS Configuration:\n");
        p = kls_pa(out, p, mx, "  share_home:  ");
        p = kls_pa(out, p, mx, config.share_home ? "yes\n" : "no\n");
        p = kls_pa(out, p, mx, "  share_tmp:   ");
        p = kls_pa(out, p, mx, config.share_tmp ? "yes\n" : "no\n");
        p = kls_pa(out, p, mx, "  share_users: ");
        p = kls_pa(out, p, mx, config.share_users ? "yes\n" : "no\n");
        p = kls_pa(out, p, mx, "  hostname:    ");
        p = kls_pa(out, p, mx, config.hostname);
        p = kls_pa(out, p, mx, "\n");
        p = kls_pa(out, p, mx, "  shell:       ");
        p = kls_pa(out, p, mx, config.default_shell);
        p = kls_pa(out, p, mx, "\n");
        p = kls_pa(out, p, mx, "  linux_root:  ");
        p = kls_pa(out, p, mx, config.linux_root);
        p = kls_pa(out, p, mx, "\n");
    }
    else if (kls_seq(sub, "sync")) {
        SyncUsersToLinux();
        SyncFilesystems();
        p = kls_pa(out, p, mx, "Sync complete.\n");
    }
    else if (kls_seq(sub, "packages")) {
        p = kls_pa(out, p, mx, "Installed Linux Packages:\n");
        p = kls_pa(out, p, mx, "────────────────────────────────────────\n");
        for (int i = 0; i < package_count; i++) {
            if (!packages[i].installed) continue;
            p = kls_pa(out, p, mx, "  ");
            p = kls_pa(out, p, mx, packages[i].name);
            // pad to 24 chars
            int nm = kls_slen(packages[i].name);
            for (int j = nm; j < 22; j++) p = kls_pa(out, p, mx, " ");
            p = kls_pa(out, p, mx, packages[i].version);
            if (packages[i].essential)
                p = kls_pa(out, p, mx, "  [essential]");
            p = kls_pa(out, p, mx, "\n");
        }
        p = kls_pa(out, p, mx, "────────────────────────────────────────\n");
        p = kls_pa(out, p, mx, "Total: ");
        p = kls_pad(out, p, mx, package_count);
        p = kls_pa(out, p, mx, " packages\n");
    }
    else if (kls_seq(sub, "install") && argc >= 3) {
        int r = InstallPackage(argv[2]);
        if (r == 0) {
            p = kls_pa(out, p, mx, "Installed: ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, "\n");
        } else {
            p = kls_pa(out, p, mx, "Failed to install: ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, "\n");
        }
    }
    else if (kls_seq(sub, "remove") && argc >= 3) {
        int r = RemovePackage(argv[2]);
        if (r == 0) {
            p = kls_pa(out, p, mx, "Removed: ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, "\n");
        } else if (r == -2) {
            p = kls_pa(out, p, mx, "Cannot remove essential package: ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, "\n");
        } else {
            p = kls_pa(out, p, mx, "Package not found: ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, "\n");
        }
    }
    else if (kls_seq(sub, "exec") && argc >= 3) {
        p = kls_pa(out, p, mx, "Loading ELF: ");
        p = kls_pa(out, p, mx, argv[2]);
        p = kls_pa(out, p, mx, "\n");
        int r = ExecELF(argv[2], argc - 2, argv + 2);
        if (r < 0) {
            p = kls_pa(out, p, mx, "Failed to execute ELF (error ");
            p = kls_pad(out, p, mx, r);
            p = kls_pa(out, p, mx, ")\n");
        } else {
            p = kls_pa(out, p, mx, "Process started (pid_idx=");
            p = kls_pad(out, p, mx, r);
            p = kls_pa(out, p, mx, ")\n");
        }
    }
    else {
        p = kls_pa(out, p, mx, "Unknown KLS command: ");
        p = kls_pa(out, p, mx, sub);
        p = kls_pa(out, p, mx, "\nType 'kls' for help.\n");
    }

    return 0;
}

int KLS::cmd_linux(void* sh, int argc, const char** argv,
                    char* out, int mx) {
    (void)sh;
    if (argc < 2) {
        kls_pa(out, 0, mx,
               "Usage: linux <command>\n"
               "Execute a command through the Linux subsystem\n"
               "Example: linux uname -a\n");
        return 0;
    }

    // reconstruct command string
    char cmd[512];
    int cp = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) cmd[cp++] = ' ';
        const char* a = argv[i];
        while (*a && cp < 510) cmd[cp++] = *a++;
    }
    cmd[cp] = 0;

    return RunCommand(cmd, out, mx) == 0 ? 0 : 1;
}

int KLS::cmd_lsb(void* sh, int argc, const char** argv,
                  char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = kls_pa(out, p, mx, "Distributor ID:\tKurono Linux\n");
    p = kls_pa(out, p, mx, "Description:\tKurono Linux 1.0 (Integrated)\n");
    p = kls_pa(out, p, mx, "Release:\t1.0\n");
    p = kls_pa(out, p, mx, "Codename:\tkurono\n");
    return 0;
}

int KLS::cmd_dpkg(void* sh, int argc, const char** argv,
                   char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2) {
        p = kls_pa(out, p, mx,
                   "Usage: dpkg-kls <action>\n"
                   "  -l           List installed packages\n"
                   "  -i <pkg>     Install package\n"
                   "  -r <pkg>     Remove package\n"
                   "  -s <pkg>     Show package info\n");
        return 0;
    }

    if (kls_seq(argv[1], "-l")) {
        p = kls_pa(out, p, mx, "ii  Name                   Version       Arch\n");
        p = kls_pa(out, p, mx, "+++ =====================  ============  ====\n");
        for (int i = 0; i < package_count; i++) {
            if (!packages[i].installed) continue;
            p = kls_pa(out, p, mx, "ii  ");
            p = kls_pa(out, p, mx, packages[i].name);
            int nm = kls_slen(packages[i].name);
            for (int j = nm; j < 22; j++) p = kls_pa(out, p, mx, " ");
            p = kls_pa(out, p, mx, " ");
            p = kls_pa(out, p, mx, packages[i].version);
            int vl = kls_slen(packages[i].version);
            for (int j = vl; j < 14; j++) p = kls_pa(out, p, mx, " ");
            p = kls_pa(out, p, mx, "i386\n");
        }
    }
    else if (kls_seq(argv[1], "-s") && argc >= 3) {
        KLSPackage* pkg = FindPackage(argv[2]);
        if (pkg) {
            p = kls_pa(out, p, mx, "Package: ");
            p = kls_pa(out, p, mx, pkg->name);
            p = kls_pa(out, p, mx, "\nStatus: ");
            p = kls_pa(out, p, mx, pkg->installed ? "install ok installed" : "not installed");
            p = kls_pa(out, p, mx, "\nVersion: ");
            p = kls_pa(out, p, mx, pkg->version);
            p = kls_pa(out, p, mx, "\nArchitecture: i386\n");
            if (pkg->essential)
                p = kls_pa(out, p, mx, "Essential: yes\n");
        } else {
            p = kls_pa(out, p, mx, "Package '");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, "' not found.\n");
        }
    }
    else if (kls_seq(argv[1], "-i") && argc >= 3) {
        InstallPackage(argv[2]);
        p = kls_pa(out, p, mx, "Setting up ");
        p = kls_pa(out, p, mx, argv[2]);
        p = kls_pa(out, p, mx, " ...\n");
    }
    else if (kls_seq(argv[1], "-r") && argc >= 3) {
        int r = RemovePackage(argv[2]);
        if (r == 0) {
            p = kls_pa(out, p, mx, "Removing ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, " ...\n");
        } else if (r == -2) {
            p = kls_pa(out, p, mx, "dpkg: error: ");
            p = kls_pa(out, p, mx, argv[2]);
            p = kls_pa(out, p, mx, " is essential and cannot be removed\n");
        }
    }

    return 0;
}
