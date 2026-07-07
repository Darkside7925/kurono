#include "runtime_layout.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/buddy.h"
#include "../kernel/slab.h"
#include "../kernel/hrtimer.h"
#include "../net/network.h"
#include "../proc/scheduler.h"

namespace {

const char* g_lib_search[] = {
    "/system/lib",
    "/system/lib/kurono",
    "/system/lib/x86_64-linux-gnu",
    "/apps/lib",
    "/system/local/lib",
    nullptr
};

void mk(const char* path, uint16_t mode) {
    KVFS::Mkdirs(path);
    KVFS::Chmod(path, mode);
}

void wr(const char* path, const char* contents) {
    KVFS::WriteString(path, contents);
}

}  // namespace

namespace RuntimeLayout {

void Init() {
    SerialLogger::Log("[RuntimeLayout] Seeding kurono fs layout...\r\n");

    // ---- Top-level system tree ---------------------------------------
    static const char* dirs[] = {
        "/system",
        "/system/bin",
        "/system/lib",
        "/system/lib/kurono",
        "/system/lib/x86_64-linux-gnu",
        "/system/libexec",
        "/system/etc",
        "/system/etc/ld.so.conf.d",
        "/system/etc/security",
        "/system/etc/dbus-1",
        "/system/etc/dbus-1/session.d",
        "/system/etc/dbus-1/system.d",
        "/system/etc/pulse",
        "/system/etc/pulse/daemon.conf.d",
        "/system/etc/wayland",
        "/system/etc/X11",
        "/system/etc/gtk-3.0",
        "/system/etc/fonts",
        "/system/etc/fonts/conf.d",
        "/system/share",
        "/system/share/applications",
        "/system/share/icons",
        "/system/share/fonts",
        "/system/share/mime",
        "/system/share/locale",
        "/system/include",
        "/system/local",
        "/system/local/bin",
        "/system/local/lib",
        "/system/fonts",
        "/system/fonts/truetype",
        "/system/ssl",
        "/system/ssl/certs",
        "/system/ssl/private",
        "/system/run",
        "/system/run/user",
        "/system/run/user/1000",
        "/system/run/user/1000/pulse",
        "/system/run/user/1000/wayland",
        "/system/run/user/1000/dbus-1",
        "/system/run/user/1000/dbus-1/services",
        "/system/run/lock",
        "/system/run/dbus",
        "/system/log",
        "/system/log/journal",
        "/system/tmp",
        "/system/var",
        "/system/var/cache",
        "/system/var/lib",
        "/system/var/lib/dbus",
        "/system/var/spool",
        "/system/dev",
        "/system/dev/snd",
        "/system/dev/dri",
        "/system/dev/input",
        "/system/dev/shm",
        "/system/dev/pts",
        "/system/dev/disk",
        "/system/dev/disk/by-label",
        "/system/dev/disk/by-uuid",
        "/system/proc",
        "/system/proc/self",
        "/system/proc/sys",
        "/system/proc/sys/kernel",
        "/system/proc/sys/vm",
        "/system/proc/sys/net",
        "/system/proc/sys/net/core",
        "/system/proc/sys/net/ipv4",
        "/system/proc/sys/fs",
        "/system/proc/net",
        "/system/proc/bus",
        "/system/proc/bus/input",
        "/system/sys",
        "/system/sys/class",
        "/system/sys/class/net",
        "/system/sys/class/drm",
        "/system/sys/class/sound",
        "/system/sys/devices",
        "/system/sys/devices/system",
        "/system/sys/devices/system/cpu",
        "/system/sys/fs",
        "/apps",
        "/apps/lib",
        "/apps/bin",
        "/home",
        "/home/user",
        "/home/user/.config",
        "/home/user/.config/dconf",
        "/home/user/.config/gtk-3.0",
        "/home/user/.config/pulse",
        "/home/user/.local",
        "/home/user/.local/share",
        "/home/user/.local/share/applications",
        "/home/user/.local/share/fonts",
        "/home/user/.local/share/mime",
        "/home/user/.cache",
        "/home/user/.cache/mesa_shader_cache",
        "/home/user/.cache/fontconfig",
        "/home/user/.mozilla",
        "/home/user/.mozilla/firefox",
        "/home/user/Desktop",
        "/home/user/Documents",
        "/home/user/Downloads",
        "/home/user/Pictures",
        "/home/user/Music",
        "/home/user/Videos",
        nullptr
    };
    for (int i = 0; dirs[i]; i++) KVFS::Mkdirs(dirs[i]);

    // Sticky bit on /tmp + /system/tmp (mode 1777).  KVFS's mode field
    // holds the low 12 bits, so we OR sticky 01000 in.
    KVFS::Chmod("/system/tmp", 01777);
    KVFS::Chmod("/system/run/user/1000", 0700);
    KVFS::Chown("/system/run/user/1000", 1000, 1000);
    KVFS::Chown("/home/user", 1000, 1000);

    // ---- /system/etc -------------------------------------------------
    wr("/system/etc/hostname", "kurono\n");
    wr("/system/etc/hosts",
       "127.0.0.1   localhost kurono\n"
       "::1         localhost ip6-localhost ip6-loopback\n"
       "fe00::0     ip6-localnet\n"
       "ff00::0     ip6-mcastprefix\n"
       "ff02::1     ip6-allnodes\n"
       "ff02::2     ip6-allrouters\n");
    // single nameserver: musl's resolver fires its queries at EVERY listed
    // server in parallel on one socket  -  three servers would triple the udp
    // datagrams through the 16-slot stack for no gain. 8.8.8.8 routes out the
    // tap0 masquerade with zero host-side services. (satoru)
    wr("/system/etc/resolv.conf",
       "# Populated by Kurono NetworkManager\n"
       "nameserver 8.8.8.8\n");
    wr("/system/etc/nsswitch.conf",
       "passwd:     files\n"
       "group:      files\n"
       "shadow:     files\n"
       "hosts:      files dns\n"
       "networks:   files\n"
       "protocols:  files\n"
       "services:   files\n"
       "ethers:     files\n"
       "rpc:        files\n");
    wr("/system/etc/machine-id", "5a4b3c2d1e0f9876543210abcdef0123\n");
    wr("/system/etc/localtime", "TZif2\0\0\0\0\0\0\0\0\0\0");   // minimal UTC
    wr("/system/etc/timezone", "UTC\n");
    wr("/system/etc/os-release",
       "PRETTY_NAME=\"Kurono OS\"\n"
       "NAME=\"Kurono\"\n"
       "VERSION_ID=\"1.0\"\n"
       "VERSION=\"1.0\"\n"
       "ID=kurono\n"
       "ID_LIKE=linux\n"
       "HOME_URL=\"https://kurono-os.dev\"\n");
    wr("/system/etc/shells",
       "/system/bin/sh\n/system/bin/bash\n/system/bin/ksh\n");

    // Dynamic loader search list  -  consumed by ld.so / dlopen wrappers.
    wr("/system/etc/ld.so.conf",
       "/system/lib\n"
       "/system/lib/kurono\n"
       "/system/lib/x86_64-linux-gnu\n"
       "/apps/lib\n"
       "/system/local/lib\n");
    wr("/system/etc/ld.so.conf.d/kurono.conf",
       "/system/lib\n/system/lib/kurono\n/apps/lib\n");
    wr("/system/etc/ld.so.cache", "kurono-cache-v1\n");

    // ---- Fontconfig --------------------------------------------------
    wr("/system/fonts/fonts.conf",
       "<?xml version=\"1.0\"?>\n"
       "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
       "<fontconfig>\n"
       "  <dir>/system/fonts</dir>\n"
       "  <dir>/system/share/fonts</dir>\n"
       // the firefox bundle ships real DejaVu TTFs under /apps/firefox/fonts;
       // without an actual text font here fontconfig enumerates zero usable
       // families and gecko's gfxPlatformFontList::GetDefaultFontLocked MOZ_CRASHes
       // at startup. search the bundle dir so the DejaVu Sans/Serif/Mono aliases
       // below resolve. (satoru)
       "  <dir>/apps/firefox/fonts</dir>\n"
       "  <dir prefix=\"xdg\">fonts</dir>\n"
       "  <dir>~/.fonts</dir>\n"
       "  <cachedir>/home/user/.cache/fontconfig</cachedir>\n"
       "  <cachedir prefix=\"xdg\">fontconfig</cachedir>\n"
       "  <config><rescan><int>30</int></rescan></config>\n"
       "  <alias><family>serif</family>"
       "<prefer><family>DejaVu Serif</family></prefer></alias>\n"
       "  <alias><family>sans-serif</family>"
       "<prefer><family>DejaVu Sans</family></prefer></alias>\n"
       "  <alias><family>monospace</family>"
       "<prefer><family>DejaVu Sans Mono</family></prefer></alias>\n"
       "</fontconfig>\n");
    wr("/system/etc/fonts/fonts.conf",
       "<?xml version=\"1.0\"?><fontconfig>"
       "<include>/system/fonts/fonts.conf</include></fontconfig>\n");
    // Register at least one TTF placeholder so fc-cache succeeds.
    wr("/system/fonts/truetype/DejaVuSans.ttf.placeholder",
       "TTF placeholder  -  install real font via kpkg dejavu-fonts\n");

    // ---- SSL CA bundle placeholder ----------------------------------
    wr("/system/ssl/certs/ca-certificates.crt",
       "# Kurono CA bundle  -  install via kpkg ca-certificates\n"
       "# Until then HTTPS will fall back to system trust store.\n");
    KVFS::Mkdirs("/system/ssl/certs/source");

    // ---- /system/proc skeleton ---------------------------------------
    wr("/system/proc/sys/kernel/hostname", "kurono\n");
    wr("/system/proc/sys/kernel/ostype", "Linux\n");          // Firefox
    wr("/system/proc/sys/kernel/osrelease", "5.15.0-kurono\n"); // sniffs both
    wr("/system/proc/sys/kernel/version", "#1 SMP Kurono\n");
    wr("/system/proc/sys/kernel/random/uuid",
       "5a4b3c2d-1e0f-9876-5432-10abcdef0123\n");
    wr("/system/proc/sys/kernel/random/boot_id",
       "1a2b3c4d-5e6f-7890-abcd-ef0123456789\n");
    wr("/system/proc/sys/kernel/pid_max", "32768\n");
    wr("/system/proc/sys/kernel/threads-max", "16384\n");
    wr("/system/proc/sys/kernel/yama/ptrace_scope", "1\n");
    wr("/system/proc/sys/vm/overcommit_memory", "1\n");
    wr("/system/proc/sys/vm/overcommit_ratio", "50\n");
    wr("/system/proc/sys/vm/max_map_count", "262144\n");
    wr("/system/proc/sys/vm/swappiness", "60\n");
    wr("/system/proc/sys/vm/min_free_kbytes", "65536\n");
    wr("/system/proc/sys/vm/dirty_ratio", "20\n");
    wr("/system/proc/sys/vm/dirty_background_ratio", "10\n");
    wr("/system/proc/sys/fs/file-max", "65536\n");
    wr("/system/proc/sys/fs/file-nr", "0\t0\t65536\n");
    wr("/system/proc/sys/fs/inotify/max_user_watches", "65536\n");
    wr("/system/proc/sys/fs/inotify/max_user_instances", "1024\n");
    wr("/system/proc/sys/fs/pipe-max-size", "1048576\n");
    wr("/system/proc/sys/fs/protected_hardlinks", "1\n");
    wr("/system/proc/sys/fs/protected_symlinks", "1\n");
    wr("/system/proc/sys/net/core/somaxconn", "4096\n");
    wr("/system/proc/sys/net/core/wmem_default", "212992\n");
    wr("/system/proc/sys/net/core/rmem_default", "212992\n");
    wr("/system/proc/sys/net/ipv4/ip_local_port_range", "32768\t60999\n");
    wr("/system/proc/sys/net/ipv4/tcp_fin_timeout", "60\n");
    wr("/system/proc/cmdline",
       "BOOT_IMAGE=/boot/kurono.elf root=/dev/sda1 quiet splash\n");
    wr("/system/proc/version",
       "Linux version 5.15.0-kurono (build@kurono) "
       "(gcc (Kurono) 13.2.0) #1 SMP Kurono\n");
    wr("/system/proc/uptime", "0.00 0.00\n");
    wr("/system/proc/loadavg", "0.00 0.00 0.00 1/64 1\n");
    wr("/system/proc/stat",
       "cpu  0 0 0 0 0 0 0 0 0 0\n"
       "cpu0 0 0 0 0 0 0 0 0 0 0\n"
       "intr 0\n"
       "ctxt 0\n"
       "btime 0\n"
       "processes 1\n"
       "procs_running 1\n"
       "procs_blocked 0\n"
       "softirq 0\n");
    wr("/system/proc/meminfo",
       "MemTotal:        2097152 kB\n"
       "MemFree:         1572864 kB\n"
       "MemAvailable:    1835008 kB\n"
       "Buffers:           65536 kB\n"
       "Cached:           131072 kB\n"
       "SwapTotal:             0 kB\n"
       "SwapFree:              0 kB\n"
       "AnonPages:        262144 kB\n"
       "Shmem:             16384 kB\n"
       "Slab:              32768 kB\n"
       "PageTables:         8192 kB\n"
       "VmallocTotal:   34359738367 kB\n"
       "VmallocUsed:       16384 kB\n"
       "Hugepagesize:       2048 kB\n");
    wr("/system/proc/cpuinfo",
       "processor\t: 0\n"
       "vendor_id\t: GenuineIntel\n"
       "cpu family\t: 6\n"
       "model\t\t: 142\n"
       "model name\t: Intel(R) Kurono CPU @ 2.40GHz\n"
       "stepping\t: 10\n"
       "cpu MHz\t\t: 2400.000\n"
       "cache size\t: 6144 KB\n"
       "physical id\t: 0\n"
       "siblings\t: 1\n"
       "core id\t\t: 0\n"
       "cpu cores\t: 1\n"
       "fpu\t\t: yes\n"
       "fpu_exception\t: yes\n"
       "cpuid level\t: 22\n"
       "wp\t\t: yes\n"
       "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge "
                  "mca cmov pat pse36 clflush mmx fxsr sse sse2 ht "
                  "syscall nx pdpe1gb rdtscp lm constant_tsc rep_good "
                  "nopl xtopology nonstop_tsc pni pclmulqdq monitor "
                  "ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt tsc_deadline_timer "
                  "aes xsave avx f16c rdrand hypervisor lahf_lm abm "
                  "3dnowprefetch fsgsbase bmi1 avx2 smep bmi2 erms "
                  "invpcid rdseed adx smap clflushopt sha_ni xsaveopt "
                  "xsavec xgetbv1 arat\n"
       "bugs\t\t:\n"
       "bogomips\t: 4800.00\n"
       "address sizes\t: 39 bits physical, 48 bits virtual\n"
       "\n");
    wr("/system/proc/mounts",
       "/dev/root / ext4 rw,relatime 0 0\n"
       "kvfs /system kvfs rw,relatime 0 0\n"
       "proc /system/proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
       "sysfs /system/sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
       "devtmpfs /system/dev devtmpfs rw,nosuid,relatime 0 0\n"
       "tmpfs /system/tmp tmpfs rw,nosuid,nodev 0 0\n"
       "tmpfs /system/run tmpfs rw,nosuid,nodev,mode=755 0 0\n"
       "tmpfs /system/run/user/1000 tmpfs rw,nosuid,nodev,relatime,size=200000k,mode=700,uid=1000,gid=1000 0 0\n");
    wr("/system/proc/filesystems",
       "nodev\tproc\n"
       "nodev\tsysfs\n"
       "nodev\tdevtmpfs\n"
       "nodev\ttmpfs\n"
       "\text4\n"
       "\tfat32\n"
       "\tkvfs\n");
    wr("/system/proc/modules", "");
    wr("/system/proc/devices",
       "Character devices:\n"
       "  1 mem\n  4 /dev/vc/0\n  5 /dev/tty\n 13 input\n"
       "Block devices:\n"
       "  8 sd\n");
    wr("/system/proc/partitions",
       "major minor  #blocks  name\n"
       "    8        0  16777216 sda\n"
       "    8        1  16775168 sda1\n");
    wr("/system/proc/net/dev",
       "Inter-|   Receive                                                |  Transmit\n"
       " face |bytes    packets errs drop fifo frame compressed multicast|"
       "bytes    packets errs drop fifo colls carrier compressed\n"
       "    lo:    0       0    0    0    0     0          0         0    "
       "0       0    0    0    0     0       0          0\n"
       "  eth0:    0       0    0    0    0     0          0         0    "
       "0       0    0    0    0     0       0          0\n");
    wr("/system/proc/net/tcp",
       "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    wr("/system/proc/net/udp",
       "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    wr("/system/proc/net/unix",
       "Num       RefCount Protocol Flags    Type St Inode Path\n");
    wr("/system/proc/net/route",
       "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
       "eth0\t00000000\t0100A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n");
    wr("/system/proc/net/arp",
       "IP address       HW type     Flags       HW address            Mask     Device\n");
    wr("/system/proc/self/exe", "/system/bin/init");
    wr("/system/proc/self/cwd", "/home/user");
    wr("/system/proc/self/cmdline", "init\0");
    wr("/system/proc/self/comm", "init\n");
    wr("/system/proc/self/maps",
       "00400000-00500000 r-xp 00000000 08:01 1 /system/bin/init\n"
       "00500000-00510000 r--p 00100000 08:01 1 /system/bin/init\n"
       "00510000-00520000 rw-p 00110000 08:01 1 /system/bin/init\n"
       "00520000-00540000 rw-p 00000000 00:00 0 [heap]\n"
       // [stack] MUST cover kurono's real user stack so musl's pthread_getattr_np
       // (main thread) finds the line containing its SP and skips the per-page
       // mremap stack-extent probe. all user procs share USER_STACK_TOP=0x40200000
       // (scheduler.cpp), stack = [top-8MB, top) = 0x3fa00000-0x40200000. the old
       // hardcoded 0x7ffffffde000 never matched -> firefox wedged doing a ~2048-page
       // mremap walk per pthread_getattr_np, dominating startup cpu. (satoru)
       "3fa00000-40200000 rw-p 00000000 00:00 0 [stack]\n");

    // ---- /system/sys placeholders -----------------------------------
    wr("/system/sys/class/net/eth0/operstate", "up\n");
    wr("/system/sys/class/net/lo/operstate", "unknown\n");
    wr("/system/sys/devices/system/cpu/online", "0\n");
    wr("/system/sys/devices/system/cpu/possible", "0\n");
    wr("/system/sys/devices/system/cpu/present", "0\n");

    // ---- Library deps manifest --------------------------------------
    wr("/system/lib/firefox-deps.manifest",
       "# Shared libraries Firefox ESR resolves at startup.\n"
       "# Lines are <soname>\\t<expected install path>.\n"
       "# kpkg install firefox-deps will populate every entry.\n"
       "libc.so.6\t/system/lib/libc.so.6\n"
       "libpthread.so.0\t/system/lib/libpthread.so.0\n"
       "libdl.so.2\t/system/lib/libdl.so.2\n"
       "libm.so.6\t/system/lib/libm.so.6\n"
       "librt.so.1\t/system/lib/librt.so.1\n"
       "libresolv.so.2\t/system/lib/libresolv.so.2\n"
       "ld-linux-x86-64.so.2\t/system/lib/ld-linux-x86-64.so.2\n"
       "libstdc++.so.6\t/system/lib/libstdc++.so.6\n"
       "libgcc_s.so.1\t/system/lib/libgcc_s.so.1\n"
       "libX11.so.6\t/system/lib/libX11.so.6\n"
       "libXext.so.6\t/system/lib/libXext.so.6\n"
       "libXcomposite.so.1\t/system/lib/libXcomposite.so.1\n"
       "libXdamage.so.1\t/system/lib/libXdamage.so.1\n"
       "libXfixes.so.3\t/system/lib/libXfixes.so.3\n"
       "libXrender.so.1\t/system/lib/libXrender.so.1\n"
       "libXrandr.so.2\t/system/lib/libXrandr.so.2\n"
       "libXi.so.6\t/system/lib/libXi.so.6\n"
       "libXcursor.so.1\t/system/lib/libXcursor.so.1\n"
       "libXt.so.6\t/system/lib/libXt.so.6\n"
       "libXft.so.2\t/system/lib/libXft.so.2\n"
       "libwayland-client.so.0\t/system/lib/libwayland-client.so.0\n"
       "libwayland-egl.so.1\t/system/lib/libwayland-egl.so.1\n"
       "libwayland-cursor.so.0\t/system/lib/libwayland-cursor.so.0\n"
       "libGL.so.1\t/system/lib/libGL.so.1\n"
       "libEGL.so.1\t/system/lib/libEGL.so.1\n"
       "libGLESv2.so.2\t/system/lib/libGLESv2.so.2\n"
       "libgbm.so.1\t/system/lib/libgbm.so.1\n"
       "libdrm.so.2\t/system/lib/libdrm.so.2\n"
       "libpulse.so.0\t/system/lib/libpulse.so.0\n"
       "libpulse-simple.so.0\t/system/lib/libpulse-simple.so.0\n"
       "libdbus-1.so.3\t/system/lib/libdbus-1.so.3\n"
       "libgio-2.0.so.0\t/system/lib/libgio-2.0.so.0\n"
       "libglib-2.0.so.0\t/system/lib/libglib-2.0.so.0\n"
       "libgobject-2.0.so.0\t/system/lib/libgobject-2.0.so.0\n"
       "libgmodule-2.0.so.0\t/system/lib/libgmodule-2.0.so.0\n"
       "libgthread-2.0.so.0\t/system/lib/libgthread-2.0.so.0\n"
       "libgtk-3.so.0\t/system/lib/libgtk-3.so.0\n"
       "libgdk-3.so.0\t/system/lib/libgdk-3.so.0\n"
       "libgdk_pixbuf-2.0.so.0\t/system/lib/libgdk_pixbuf-2.0.so.0\n"
       "libcairo.so.2\t/system/lib/libcairo.so.2\n"
       "libcairo-gobject.so.2\t/system/lib/libcairo-gobject.so.2\n"
       "libpango-1.0.so.0\t/system/lib/libpango-1.0.so.0\n"
       "libpangocairo-1.0.so.0\t/system/lib/libpangocairo-1.0.so.0\n"
       "libpangoft2-1.0.so.0\t/system/lib/libpangoft2-1.0.so.0\n"
       "libharfbuzz.so.0\t/system/lib/libharfbuzz.so.0\n"
       "libfontconfig.so.1\t/system/lib/libfontconfig.so.1\n"
       "libfreetype.so.6\t/system/lib/libfreetype.so.6\n"
       "libjpeg.so.62\t/system/lib/libjpeg.so.62\n"
       "libpng16.so.16\t/system/lib/libpng16.so.16\n"
       "libwebp.so.7\t/system/lib/libwebp.so.7\n"
       "libz.so.1\t/system/lib/libz.so.1\n"
       "libbz2.so.1.0\t/system/lib/libbz2.so.1.0\n"
       "liblzma.so.5\t/system/lib/liblzma.so.5\n"
       "libssl.so.3\t/system/lib/libssl.so.3\n"
       "libcrypto.so.3\t/system/lib/libcrypto.so.3\n"
       "libsqlite3.so.0\t/system/lib/libsqlite3.so.0\n"
       "libavcodec.so.59\t/system/lib/libavcodec.so.59\n"
       "libavformat.so.59\t/system/lib/libavformat.so.59\n"
       "libavutil.so.57\t/system/lib/libavutil.so.57\n"
       "libnss3.so\t/system/lib/libnss3.so\n"
       "libnspr4.so\t/system/lib/libnspr4.so\n"
       "libsmime3.so\t/system/lib/libsmime3.so\n"
       "libnssutil3.so\t/system/lib/libnssutil3.so\n"
       "libplc4.so\t/system/lib/libplc4.so\n"
       "libplds4.so\t/system/lib/libplds4.so\n"
       "libffi.so.8\t/system/lib/libffi.so.8\n"
       "libudev.so.1\t/system/lib/libudev.so.1\n"
       "libsecret-1.so.0\t/system/lib/libsecret-1.so.0\n"
       "libnotify.so.4\t/system/lib/libnotify.so.4\n"
       "libatspi.so.0\t/system/lib/libatspi.so.0\n"
       "libatk-1.0.so.0\t/system/lib/libatk-1.0.so.0\n"
       "libatk-bridge-2.0.so.0\t/system/lib/libatk-bridge-2.0.so.0\n"
       "libvpx.so.7\t/system/lib/libvpx.so.7\n"
       "libogg.so.0\t/system/lib/libogg.so.0\n"
       "libvorbis.so.0\t/system/lib/libvorbis.so.0\n"
       "libvorbisenc.so.2\t/system/lib/libvorbisenc.so.2\n"
       "libopus.so.0\t/system/lib/libopus.so.0\n"
       "libtheora.so.0\t/system/lib/libtheora.so.0\n"
       "libicuuc.so.72\t/system/lib/libicuuc.so.72\n"
       "libicui18n.so.72\t/system/lib/libicui18n.so.72\n"
       "libicudata.so.72\t/system/lib/libicudata.so.72\n");

    // ---- Default user environment file -----------------------------
    wr("/home/user/.config/kurono/firefox.env",
       "MOZ_ENABLE_WAYLAND=1\n"
       "MOZ_DISABLE_RDD_SANDBOX=1\n"
       "LIBGL_ALWAYS_SOFTWARE=1\n"
       // kurono's compositor blits wl_shm only (no dmabuf/egl). firefox 140 is
       // webrender-only, so force its SOFTWARE backend (cpu rasterizer, no GL) which
       // commits plain shm buffers; MOZ_WEBRENDER=1 force-enables WR past the gfxInfo
       // blocklist (=0 would leave no compositor -> #PF). (satoru)
       "MOZ_WEBRENDER=1\n"
       "WEBRENDER_SOFTWARE=1\n"
       "MOZ_ACCELERATED=0\n"
       "MOZ_X11_EGL=0\n"
       "MOZ_DISABLE_GPU_SANDBOX=1\n"
       "MOZ_DISABLE_CONTENT_SANDBOX=1\n"
       // (satoru) force the socket(network) process off  -  the one child proc firefox
       // still spawns at startup; its launch parks the ipc i/o thread + wedges the
       // chrome main at AsyncLaunch (kurono can't cleanly fork+exec a child). honoured
       // before the cached pref check in nsIOService::UseSocketProcess. (satoru)
       "MOZ_DISABLE_SOCKET_PROCESS=1\n"
       // gecko runs as the system uid (0) with $HOME owned by uid 1000; this is
       // its official override for the "running as root" startup refusal. (satoru)
       "MOZ_ALLOW_ROOT=1\n"
       "GALLIUM_DRIVER=llvmpipe\n"
       "WAYLAND_DISPLAY=wayland-0\n"
       "DBUS_SESSION_BUS_ADDRESS=unix:path=/system/run/user/1000/bus\n"
       "HOME=/home/user\n"
       "USER=user\n"
       "DISPLAY=:0\n"
       "LD_LIBRARY_PATH=/apps/firefox/lib:/system/lib:/system/lib/kurono:/apps/lib\n"
       "FONTCONFIG_PATH=/system/fonts\n"
       "SSL_CERT_FILE=/system/ssl/certs/ca-certificates.crt\n"
       "XDG_RUNTIME_DIR=/system/run/user/1000\n"
       "XDG_CONFIG_HOME=/home/user/.config\n"
       "XDG_DATA_HOME=/home/user/.local/share\n"
       "XDG_CACHE_HOME=/home/user/.cache\n"
       "XDG_DATA_DIRS=/system/share:/system/local/share\n"
       "XDG_CONFIG_DIRS=/system/etc/xdg\n"
       "GTK_THEME=Adwaita:dark\n"
       "GDK_BACKEND=wayland,x11\n"
       "QT_QPA_PLATFORM=wayland;xcb\n"
       "PULSE_SERVER=unix:/system/run/user/1000/pulse/native\n");

    // ---- DBus machine-id (matches /system/etc/machine-id) ----------
    wr("/system/var/lib/dbus/machine-id", "5a4b3c2d1e0f9876543210abcdef0123\n");

    // ---- Phase 14: real buddy + slab snapshots --------------------
    {
        char buf[4096];
        if (Buddy::IsReady()) {
            Buddy::DumpProcInfo(buf, sizeof(buf));
            wr("/system/proc/buddyinfo", buf);
        } else {
            wr("/system/proc/buddyinfo",
               "Node 0, zone   Normal  0 0 0 0 0 0 0 0 0 0 0 0\n");
        }
        if (Slab::IsReady()) {
            Slab::DumpProcInfo(buf, sizeof(buf));
            wr("/system/proc/slabinfo", buf);
        } else {
            wr("/system/proc/slabinfo",
               "slabinfo - version: 2.1\n");
        }
        // live loadavg snapshot (will stay close to 0 right after boot;
        // syscalls / userland reads can refresh it as needed).
        char lbuf[64];
        Scheduler::GetLoadAverageStr(lbuf, sizeof(lbuf));
        wr("/system/proc/loadavg", lbuf);

        // hrtimer snapshot  -  refreshed by RefreshProc()
        HRTimer::DumpProcInfo(buf, sizeof(buf));
        wr("/system/proc/timer_list", buf);
    }

    SerialLogger::Log("[RuntimeLayout] Done.\r\n");
}

void EnsureUserRuntime(const char* user, uint32_t uid) {
    char path[128];
    int n = 0;
    const char* base = "/system/run/user/";
    while (base[n]) { path[n] = base[n]; n++; }
    // print uid
    char num[16]; int ni = 0;
    uint32_t v = uid;
    if (v == 0) num[ni++] = '0';
    else { char tmp[16]; int ti = 0; while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
           while (ti) num[ni++] = tmp[--ti]; }
    num[ni] = 0;
    for (int i = 0; num[i]; i++) path[n++] = num[i];
    path[n] = 0;
    KVFS::Mkdirs(path);
    KVFS::Chmod(path, 0700);
    KVFS::Chown(path, (uint16_t)uid, (uint16_t)uid);
    (void)user;
}

void RefreshProc() {
    // Refresh time-sensitive nodes.  uptime/loadavg are the cheap ones.
    // The scheduler should call us once a second from low-prio context.
    static uint64_t s_uptime_secs = 0;
    s_uptime_secs++;
    char buf[64];
    int  i = 0;
    uint64_t v = s_uptime_secs;
    if (v == 0) buf[i++] = '0';
    else { char tmp[24]; int ti = 0; while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
           while (ti) buf[i++] = tmp[--ti]; }
    buf[i++] = '.'; buf[i++] = '0'; buf[i++] = '0'; buf[i++] = ' ';
    v = s_uptime_secs;
    if (v == 0) buf[i++] = '0';
    else { char tmp[24]; int ti = 0; while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
           while (ti) buf[i++] = tmp[--ti]; }
    buf[i++] = '.'; buf[i++] = '0'; buf[i++] = '0'; buf[i++] = '\n'; buf[i] = 0;
    KVFS::WriteString("/system/proc/uptime", buf);

    // refresh hrtimer table snapshot
    static char hrt_buf[4096];
    HRTimer::DumpProcInfo(hrt_buf, sizeof(hrt_buf));
    KVFS::WriteString("/system/proc/timer_list", hrt_buf);

    // refresh live ARP cache + routing table from the network stack
    static char net_buf[2048];
    Network::DumpARPTable(net_buf, sizeof(net_buf));
    KVFS::WriteString("/system/proc/net/arp", net_buf);
    Network::DumpRoutes(net_buf, sizeof(net_buf));
    KVFS::WriteString("/system/proc/net/route", net_buf);
}

const char* const* LibrarySearchPath() {
    return g_lib_search;
}

}  // namespace RuntimeLayout
