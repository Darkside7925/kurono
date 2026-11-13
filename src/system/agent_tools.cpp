//  agent tools: seed the linux-visible command tree. (satoru)
//
//  two kinds of names live in /system/bin:
//   - busybox applets, as symlinks to /system/bin/busybox. busybox's shell is
//     built with standalone applet support, but most applets (cat, grep,
//     dmesg) re-exec /proc/self/exe, which only works now that procfs exposes
//     a real exe symlink. the symlinks make them work from any shell, not
//     just busybox's.
//   - kurono shell commands with no busybox equivalent (ksys, kdrv, kinit,
//     audio, gpu...), as symlinks to ksh-run, which drives /dev/kshell.
//  plus ksh-run itself, kcmd, and a real bash when this build embeds one.
#include "agent_tools.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../userprogs/embedded_userprogs.h"

namespace {

// every applet this busybox build reports (`busybox --list`), space separated.
const char kApplets[] =
    "[ [[ acpid adjtimex ar arch arp arping ascii ash awk base64 basename bc "
    "blkdiscard blockdev brctl bunzip2 busybox bzcat bzip2 cal cat chgrp chmod "
    "chown chroot chvt clear cmp cp cpio crc32 crond crontab cttyhack cut date "
    "dc dd deallocvt depmod devmem df diff dirname dmesg dnsdomainname dos2unix "
    "dpkg dpkg-deb du dumpkmap dumpleases echo ed egrep env expand expr factor "
    "fallocate false fatattr fdisk fgrep find findfs fold free freeramdisk "
    "fsfreeze fstrim ftpget ftpput getfattr getopt getty grep groups gunzip gzip "
    "halt head hexdump hostid hostname httpd hwclock i2cdetect i2cdump i2cget "
    "i2cset i2ctransfer id ifconfig ifdown ifup init insmod install ionice ip "
    "ipcalc kill killall klogd last less link linux32 linux64 linuxrc ln "
    "loadfont loadkmap logger login logname logread losetup ls lsmod lsscsi "
    "lzcat lzma lzop md5sum mdev microcom mim mkdir mkdosfs mke2fs mkfifo mknod "
    "mkpasswd mkswap mktemp modinfo modprobe more mount mt mv nameif nbd-client "
    "nc netstat nl nologin nproc nsenter nslookup nuke od openvt partprobe "
    "passwd paste patch pidof ping ping6 pivot_root poweroff printf ps pwd rdate "
    "readlink realpath reboot renice reset resume rev rm rmdir rmmod route rpm "
    "rpm2cpio run-init run-parts sed seq setkeycodes setpriv setsid sh sha1sum "
    "sha256sum sha3sum sha512sum shred shuf sleep sort ssl_client "
    "start-stop-daemon stat strings stty su sulogin svc svok swapoff swapon "
    "switch_root sync sysctl syslogd tac tail tar taskset tee telnet telnetd "
    "test tftp time timeout top touch tr traceroute traceroute6 true truncate "
    "ts tty tunctl ubirename udhcpc udhcpc6 udhcpd uevent umount uname "
    "uncompress unexpand uniq unix2dos unlink unlzma unshare unxz unzip uptime "
    "usleep uudecode uuencode vconfig vi w watch watchdog wc wget which who "
    "whoami xargs xxd xz xzcat yes zcat";

// kurono built-ins worth a PATH name. names busybox also provides are skipped
// so `ls`, `ip`, `mount`, `wget` keep their normal linux behaviour; the kurono
// spelling is still reachable as `kcmd <name>`. (satoru)
const char kWrappers[] =
    "ksys kdrv kinit kctl audio recovery persisttest gpu gpuprobe gpuaux "
    "gpumode gpuaccel display sysinfo hwinfo drivers lsblk lspci lsusb lscpu "
    "dmidecode hwfw iotop ss nic wifi nmcli journal ver version tree fls fcat "
    "codecs vgpu screenshot help usermode ken amdo";

bool is_applet(const char* name) {
    const char* p = kApplets;
    int nl = 0; while (name[nl]) nl++;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* b = p;
        while (*p && *p != ' ') p++;
        if ((int)(p - b) == nl) {
            bool same = true;
            for (int i = 0; i < nl; i++) if (b[i] != name[i]) { same = false; break; }
            if (same) return true;
        }
    }
    return false;
}

int slen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

// create path -> target unless a real node already exists there. returns 1
// when this call created it. (satoru)
int link_if_absent(const char* path, const char* target) {
    if (KVFS::Exists(path)) {
        // an existing symlink with a stale target is rewritten by Symlink; a
        // real file (recovery toolbox binary, user's own script) wins. (satoru)
        KVFSNode* n = KVFS::Resolve(path);
        if (n && n->type != KVFS_SYMLINK) return 0;
    }
    return KVFS::Symlink(path, target) == 0 ? 1 : 0;
}

}  // namespace

namespace AgentTools {

void Install() {
    KVFS::Mkdirs("/system/bin");
    int links = 0;

    bool have_busybox = KVFS::Exists("/system/bin/busybox");
    if (have_busybox) {
        const char* p = kApplets;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char name[40]; int n = 0;
            while (*p && *p != ' ' && n < 39) name[n++] = *p++;
            name[n] = 0;
            if (!n) continue;
            char path[64];
            int pl = 0;
            const char* pre = "/system/bin/";
            while (pre[pl]) { path[pl] = pre[pl]; pl++; }
            for (int i = 0; i < n && pl < 63; i++) path[pl++] = name[i];
            path[pl] = 0;
            links += link_if_absent(path, "/system/bin/busybox");
        }
    } else {
        SerialLogger::Log("[AgentTools] /system/bin/busybox missing: no applet links\r\n");
    }

    // ksh-run: the /dev/kshell client. borrow the embedded blob, never copy. (satoru)
    if (EmbeddedUserprogs::HasKshrun()) {
        if (!KVFS::Exists("/system/bin/ksh-run")) {
            KVFS::WriteFileStatic("/system/bin/ksh-run",
                                  EmbeddedUserprogs::KshrunData(),
                                  EmbeddedUserprogs::KshrunSize());
        }
        KVFS::Chmod("/system/bin/ksh-run", 0755);
        links += link_if_absent("/system/bin/kcmd", "/system/bin/ksh-run");
    } else {
        SerialLogger::Log("[AgentTools] ksh-run not in this build: no kurono command bridge\r\n");
    }

    // kurono built-in names. skip any busybox applet spelling, and skip kcmd. (satoru)
    if (KVFS::Exists("/system/bin/ksh-run")) {
        const char* p = kWrappers;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char name[40]; int n = 0;
            while (*p && *p != ' ' && n < 39) name[n++] = *p++;
            name[n] = 0;
            if (!n || is_applet(name)) continue;
            char path[64];
            int pl = 0;
            const char* pre = "/system/bin/";
            while (pre[pl]) { path[pl] = pre[pl]; pl++; }
            for (int i = 0; i < n && pl < 63; i++) path[pl++] = name[i];
            path[pl] = 0;
            links += link_if_absent(path, "/system/bin/ksh-run");
        }
    }

    // the linux-visible account database. user_mgmt writes /etc/passwd through
    // the kvfs /etc symlink (/kurono/system/config), but a linux getpwuid reads
    // /system/etc/passwd; without this `whoami` said "unknown uid 1000" and
    // git could not find an author name. keep it minimal and stable. (satoru)
    if (!KVFS::Exists("/system/etc/passwd")) {
        KVFS::Mkdirs("/system/etc");
        KVFS::WriteString("/system/etc/passwd",
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:User:/home/user:/bin/bash\n");
    }
    if (!KVFS::Exists("/system/etc/group")) {
        KVFS::Mkdirs("/system/etc");
        KVFS::WriteString("/system/etc/group",
            "root:x:0:\n"
            "user:x:1000:\n");
    }

    // kfetch: the small bearssl https/http downloader kpkg already uses. it is
    // registered under /usr/bin for the kernel shell, which a linux process
    // cannot see (guest /usr/bin maps to /system/bin), so expose it there and
    // put a small curl front end on it. (satoru)
    if (EmbeddedUserprogs::HasKfetch()) {
        if (!KVFS::Exists("/system/bin/kfetch")) {
            KVFS::WriteFileStatic("/system/bin/kfetch",
                                  EmbeddedUserprogs::KfetchData(),
                                  EmbeddedUserprogs::KfetchSize());
        }
        KVFS::Chmod("/system/bin/kfetch", 0755);
        if (!KVFS::Exists("/system/bin/curl")) {
            static const char kCurl[] =
                "#!/system/bin/sh\n"
                "# minimal curl over busybox wget: -s -S -L -f -k -o FILE -O, -H, -A, -d URL (satoru)\n"
                "out=''; remote=0; url=''; opts=''\n"
                "while [ $# -gt 0 ]; do\n"
                "  case \"$1\" in\n"
                "    -s|--silent) opts=\"$opts -q\"; shift ;;\n"
                "    -S|--show-error|-L|--location|-f|--fail) shift ;;\n"
                "    -k|--insecure) opts=\"$opts --no-check-certificate\"; shift ;;\n"
                "    -o|--output) out=\"$2\"; shift 2 ;;\n"
                "    -O|--remote-name) remote=1; shift ;;\n"
                "    -H|--header) opts=\"$opts --header $2\"; shift 2 ;;\n"
                "    -A|--user-agent) opts=\"$opts --user-agent $2\"; shift 2 ;;\n"
                "    -d|--data|--data-raw) opts=\"$opts --post-data $2\"; shift 2 ;;\n"
                "    -X|--request) shift 2 ;;\n"
                "    -*) shift ;;\n"
                "    *) url=\"$1\"; shift ;;\n"
                "  esac\n"
                "done\n"
                "[ -z \"$url\" ] && { echo 'curl: no URL' >&2; exit 2; }\n"
                "if [ -n \"$out\" ]; then exec wget $opts -O \"$out\" \"$url\"; fi\n"
                "if [ $remote = 1 ]; then b=\${url##*/}; [ -z \"$b\" ] && b=index.html; exec wget $opts -O \"$b\" \"$url\"; fi\n"
                "exec wget $opts -O - \"$url\"\n";
            KVFS::WriteString("/system/bin/curl", kCurl);
            KVFS::Chmod("/system/bin/curl", 0755);
        }
    }

    // git: static musl build, local repo operations only (no curl transport in
    // this build; https fetch is busybox wget). an empty template dir stops
    // `git init` warning that the compiled-in templates are missing. (satoru)
    if (EmbeddedUserprogs::HasGitStatic()) {
        KAsset::Deliver("GIT     ELF", _binary_git_elf_start, _binary_git_elf_end);
        if (!KVFS::Exists("/system/bin/git")) {
            KVFS::WriteFileStatic("/system/bin/git",
                                  EmbeddedUserprogs::GitStaticData(),
                                  EmbeddedUserprogs::GitStaticSize());
        }
        KVFS::Chmod("/system/bin/git", 0755);
        KVFS::Mkdirs("/system/share/git-core/templates");
    }

    // a real bash beats the busybox-sh shim for claude's shell snapshots and
    // for pipefail/arrays. it is a weak blob: absent from a slimmer build. (satoru)
    if (EmbeddedUserprogs::HasBashStatic()) {
        KAsset::Deliver("BASH    ELF", _binary_bash_elf_start, _binary_bash_elf_end);
        if (!KVFS::Exists("/system/bin/bash")) {
            KVFS::WriteFileStatic("/system/bin/bash",
                                  EmbeddedUserprogs::BashStaticData(),
                                  EmbeddedUserprogs::BashStaticSize());
        }
        KVFS::Chmod("/system/bin/bash", 0755);
        links += link_if_absent("/system/bin/bash-static", "/system/bin/bash");
    }

    SerialLogger::Log("[AgentTools] command tree ready: ");
    SerialLogger::LogDec(links);
    SerialLogger::Log(" links\r\n");
}

}  // namespace AgentTools

// end (satoru)
