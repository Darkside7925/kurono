// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Kernel Emulation Layer — Implementation
// ═══════════════════════════════════════════════════════════════════════════

#include "linux_kernel.h"
#include "linux_syscall.h"
#include "shared_mount.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"

// ─── Static storage ──────────────────────────────────────────────────────

bool         LinuxKernel::running = false;
uint32_t     LinuxKernel::start_time = 0;
uint32_t     LinuxKernel::jiffies = 0;

ProcEntry    LinuxKernel::proc_entries[PROC_MAX_ENTRIES];
int          LinuxKernel::proc_count = 0;

SysEntry     LinuxKernel::sys_entries[SYS_MAX_ENTRIES];
int          LinuxKernel::sys_count = 0;

LinuxTTY     LinuxKernel::ttys[LINUX_MAX_TTYS];
int          LinuxKernel::tty_count = 0;

LinuxThread  LinuxKernel::threads[LINUX_MAX_THREADS];
int          LinuxKernel::next_tid = 1;

LinuxFutex   LinuxKernel::futexes[LINUX_MAX_FUTEXES];

int          LinuxKernel::load_1 = 0;
int          LinuxKernel::load_5 = 0;
int          LinuxKernel::load_15 = 0;
uint32_t     LinuxKernel::rng_state = 0x12345678;

// ─── Helpers ─────────────────────────────────────────────────────────────

static int lk_slen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void lk_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool lk_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool lk_starts(const char* s, const char* prefix) {
    while (*prefix) { if (*s != *prefix) return false; s++; prefix++; }
    return true;
}

int LinuxKernel::PutStr(char* buf, int pos, int mx, const char* s) {
    while (*s && pos < mx - 1) buf[pos++] = *s++;
    buf[pos] = 0;
    return pos;
}

int LinuxKernel::PutDec(char* buf, int pos, int mx, int val) {
    if (val == 0) { if (pos < mx - 1) buf[pos++] = '0'; buf[pos] = 0; return pos; }
    char tmp[12]; int i = 0;
    bool neg = val < 0;
    if (neg) val = -val;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    if (neg && pos < mx - 1) buf[pos++] = '-';
    for (int j = i - 1; j >= 0 && pos < mx - 1; j--) buf[pos++] = tmp[j];
    buf[pos] = 0;
    return pos;
}

int LinuxKernel::PutHex(char* buf, int pos, int mx, uint32_t val) {
    const char* hex = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) {
        if (pos < mx - 1) buf[pos++] = hex[(val >> i) & 0xF];
    }
    buf[pos] = 0;
    return pos;
}

int LinuxKernel::PutLine(char* buf, int pos, int mx, const char* key, const char* val) {
    pos = PutStr(buf, pos, mx, key);
    pos = PutStr(buf, pos, mx, ":\t");
    pos = PutStr(buf, pos, mx, val);
    pos = PutStr(buf, pos, mx, "\n");
    return pos;
}

int LinuxKernel::PutKB(char* buf, int pos, int mx, const char* key, uint32_t kb) {
    pos = PutStr(buf, pos, mx, key);
    pos = PutStr(buf, pos, mx, ":\t");
    pos = PutDec(buf, pos, mx, (int)kb);
    pos = PutStr(buf, pos, mx, " kB\n");
    return pos;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

void LinuxKernel::Init() {
    memset(proc_entries, 0, sizeof(proc_entries));
    memset(sys_entries, 0, sizeof(sys_entries));
    memset(ttys, 0, sizeof(ttys));
    memset(threads, 0, sizeof(threads));
    memset(futexes, 0, sizeof(futexes));
    proc_count = 0;
    sys_count = 0;
    tty_count = 0;
    next_tid = 1;
    jiffies = 0;
    running = false;
    rng_state = 0xDEAD1337;

    SerialLogger::Log("[LinuxKernel] Virtual kernel initialized\r\n");
}

void LinuxKernel::Start() {
    if (running) return;
    start_time = Time::GetTicks();
    running = true;

    InitProcFS();
    InitSysFS();
    InitTTY();

    SerialLogger::Log("[LinuxKernel] Virtual Linux ");
    SerialLogger::Log(LINUX_KERNEL_VERSION);
    SerialLogger::Log(" started\r\n");
}

void LinuxKernel::Stop() {
    if (!running) return;
    running = false;

    // Destroy all threads
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (threads[i].active) DestroyThread(threads[i].tid);
    }

    // Free PTYs
    for (int i = 0; i < LINUX_MAX_TTYS; i++) {
        if (ttys[i].active) FreePTY(i);
    }

    SerialLogger::Log("[LinuxKernel] Virtual kernel stopped\r\n");
}

void LinuxKernel::Tick(uint32_t now_ms) {
    if (!running) return;
    jiffies++;

    // Update load average every ~5 seconds (5000 ticks at 1kHz)
    if ((jiffies % 5000) == 0) {
        int running_count = 0;
        for (int i = 0; i < LINUX_MAX_THREADS; i++) {
            if (threads[i].active) running_count++;
        }
        // Exponential moving average (simplified)
        load_1  = (load_1  * 90 + running_count * 100 * 10) / 100;
        load_5  = (load_5  * 98 + running_count * 100 *  2) / 100;
        load_15 = (load_15 * 99 + running_count * 100 *  1) / 100;
    }

    // Tick threads — accumulate CPU time
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (threads[i].active) threads[i].cpu_time++;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  /proc filesystem
// ═══════════════════════════════════════════════════════════════════════════

void LinuxKernel::InitProcFS() {
    proc_count = 0;

    // Register standard /proc entries
    RegisterProcEntry("version",     "/proc", PROC_FILE, GenVersion, nullptr);
    RegisterProcEntry("uptime",      "/proc", PROC_FILE, GenUptime, nullptr);
    RegisterProcEntry("meminfo",     "/proc", PROC_FILE, GenMeminfo, nullptr);
    RegisterProcEntry("cpuinfo",     "/proc", PROC_FILE, GenCpuinfo, nullptr);
    RegisterProcEntry("stat",        "/proc", PROC_FILE, GenStat, nullptr);
    RegisterProcEntry("loadavg",     "/proc", PROC_FILE, GenLoadavg, nullptr);
    RegisterProcEntry("mounts",      "/proc", PROC_FILE, GenMounts, nullptr);
    RegisterProcEntry("filesystems", "/proc", PROC_FILE, GenFilesystems, nullptr);
    RegisterProcEntry("cmdline",     "/proc", PROC_FILE, GenCmdline, nullptr);
    RegisterProcEntry("partitions",  "/proc", PROC_FILE, GenPartitions, nullptr);
    RegisterProcEntry("net",         "/proc", PROC_DIR, nullptr, nullptr);

    // /proc/net/dev
    RegisterProcEntry("dev", "/proc/net", PROC_FILE, GenNetDev, nullptr);

    // /proc/self → symlink to current PID
    RegisterProcEntry("self", "/proc", PROC_LINK, nullptr, nullptr);

    // Create in KVFS
    KVFS::Mkdirs("/proc");
    KVFS::Mkdirs("/proc/net");

    SerialLogger::Log("[LinuxKernel] /proc initialized (");
    SerialLogger::LogDec(proc_count);
    SerialLogger::Log(" entries)\r\n");
}

void LinuxKernel::RegisterProcEntry(const char* name, const char* parent,
                                     ProcEntryType type,
                                     int (*gen)(char*, int, void*), void* ctx) {
    if (proc_count >= PROC_MAX_ENTRIES) return;
    ProcEntry* e = &proc_entries[proc_count++];
    lk_scpy(e->name, name, sizeof(e->name));
    lk_scpy(e->parent, parent, sizeof(e->parent));
    e->type = type;
    e->generator = gen;
    e->ctx = ctx;
    e->active = true;
    e->static_len = 0;
}

int LinuxKernel::ProcIndex(const char* path) {
    // path = "/proc/version" → name="version", parent="/proc"
    // Find last '/'
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash < 0) return -1;

    char parent[64], name[64];
    memcpy(parent, path, last_slash);
    parent[last_slash] = 0;
    if (parent[0] == 0) lk_scpy(parent, "/proc", sizeof(parent));
    lk_scpy(name, path + last_slash + 1, sizeof(name));

    for (int i = 0; i < proc_count; i++) {
        if (proc_entries[i].active &&
            lk_seq(proc_entries[i].name, name) &&
            lk_seq(proc_entries[i].parent, parent)) {
            return i;
        }
    }
    return -1;
}

int LinuxKernel::ReadProc(const char* path, char* buf, int max_len) {
    int idx = ProcIndex(path);
    if (idx < 0) return -1;
    ProcEntry* e = &proc_entries[idx];

    if (e->generator) {
        return e->generator(buf, max_len, e->ctx);
    }
    if (e->static_len > 0) {
        int copy = e->static_len < max_len ? e->static_len : max_len - 1;
        memcpy(buf, e->static_content, copy);
        buf[copy] = 0;
        return copy;
    }
    return 0;
}

int LinuxKernel::WriteProc(const char* path, const char* data, int len) {
    (void)path; (void)data; (void)len;
    return -1;  // Most /proc entries are read-only
}

bool LinuxKernel::ProcExists(const char* path) {
    return ProcIndex(path) >= 0;
}

// ─── /proc generators ───────────────────────────────────────────────────

int LinuxKernel::GenVersion(char* buf, int mx, void*) {
    int p = 0;
    p = PutStr(buf, p, mx, LINUX_KERNEL_SYSNAME);
    p = PutStr(buf, p, mx, " version ");
    p = PutStr(buf, p, mx, LINUX_KERNEL_RELEASE);
    p = PutStr(buf, p, mx, " (kurono@");
    p = PutStr(buf, p, mx, "kurono-build) (gcc (Kurono) 13.2.0) #1 SMP ");
    p = PutStr(buf, p, mx, "Fri Mar 28 00:00:00 UTC 2026\n");
    return p;
}

int LinuxKernel::GenUptime(char* buf, int mx, void*) {
    uint32_t uptime_s = GetUptime();
    int p = 0;
    p = PutDec(buf, p, mx, (int)uptime_s);
    p = PutStr(buf, p, mx, ".00 ");
    p = PutDec(buf, p, mx, (int)(uptime_s * 90 / 100));  // idle ~90%
    p = PutStr(buf, p, mx, ".00\n");
    return p;
}

int LinuxKernel::GenMeminfo(char* buf, int mx, void*) {
    uint32_t total = GetTotalMemory();     // KB
    uint32_t free_mem = GetFreeMemory();   // KB
    uint32_t used = total - free_mem;
    uint32_t cached = free_mem / 4;
    uint32_t buffers = free_mem / 8;

    int p = 0;
    p = PutKB(buf, p, mx, "MemTotal",        total);
    p = PutKB(buf, p, mx, "MemFree",         free_mem);
    p = PutKB(buf, p, mx, "MemAvailable",    free_mem + cached);
    p = PutKB(buf, p, mx, "Buffers",         buffers);
    p = PutKB(buf, p, mx, "Cached",          cached);
    p = PutKB(buf, p, mx, "SwapCached",      0);
    p = PutKB(buf, p, mx, "Active",          used / 2);
    p = PutKB(buf, p, mx, "Inactive",        used / 2);
    p = PutKB(buf, p, mx, "SwapTotal",       0);
    p = PutKB(buf, p, mx, "SwapFree",        0);
    p = PutKB(buf, p, mx, "Dirty",           0);
    p = PutKB(buf, p, mx, "Writeback",       0);
    p = PutKB(buf, p, mx, "AnonPages",       used / 3);
    p = PutKB(buf, p, mx, "Mapped",          used / 4);
    p = PutKB(buf, p, mx, "Shmem",           0);
    p = PutKB(buf, p, mx, "Slab",            1024);
    return p;
}

int LinuxKernel::GenCpuinfo(char* buf, int mx, void*) {
    int p = 0;
    p = PutLine(buf, p, mx, "processor",  "0");
    p = PutLine(buf, p, mx, "vendor_id",  "KuronoVirt");
    p = PutLine(buf, p, mx, "cpu family", "6");
    p = PutLine(buf, p, mx, "model",      "1");
    p = PutLine(buf, p, mx, "model name", "Kurono Virtual CPU @ 1.00GHz");
    p = PutLine(buf, p, mx, "stepping",   "0");
    p = PutLine(buf, p, mx, "cpu MHz",    "1000.000");
    p = PutLine(buf, p, mx, "cache size", "4096 KB");
    p = PutLine(buf, p, mx, "fpu",        "yes");
    p = PutLine(buf, p, mx, "fpu_exception", "yes");
    p = PutLine(buf, p, mx, "cpuid level", "13");
    p = PutLine(buf, p, mx, "wp",          "yes");
    p = PutLine(buf, p, mx, "flags",       "fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat clflush mmx fxsr sse sse2");
    p = PutLine(buf, p, mx, "bogomips",    "2000.00");
    p = PutStr(buf, p, mx, "\n");
    return p;
}

int LinuxKernel::GenStat(char* buf, int mx, void*) {
    int p = 0;
    // CPU line: user nice sys idle iowait irq softirq steal guest guest_nice
    p = PutStr(buf, p, mx, "cpu  ");
    p = PutDec(buf, p, mx, (int)(jiffies / 10)); p = PutStr(buf, p, mx, " 0 ");
    p = PutDec(buf, p, mx, (int)(jiffies / 20)); p = PutStr(buf, p, mx, " ");
    p = PutDec(buf, p, mx, (int)(jiffies * 9 / 10)); p = PutStr(buf, p, mx, " 0 0 0 0 0 0\n");

    p = PutStr(buf, p, mx, "cpu0 ");
    p = PutDec(buf, p, mx, (int)(jiffies / 10)); p = PutStr(buf, p, mx, " 0 ");
    p = PutDec(buf, p, mx, (int)(jiffies / 20)); p = PutStr(buf, p, mx, " ");
    p = PutDec(buf, p, mx, (int)(jiffies * 9 / 10)); p = PutStr(buf, p, mx, " 0 0 0 0 0 0\n");

    p = PutStr(buf, p, mx, "intr 0\n");
    p = PutStr(buf, p, mx, "ctxt ");
    p = PutDec(buf, p, mx, (int)(jiffies * 2));
    p = PutStr(buf, p, mx, "\n");
    p = PutStr(buf, p, mx, "btime ");
    p = PutDec(buf, p, mx, (int)(start_time / 1000));
    p = PutStr(buf, p, mx, "\n");
    p = PutStr(buf, p, mx, "processes ");
    p = PutDec(buf, p, mx, LinuxSyscall::ActiveProcessCount() + 10);
    p = PutStr(buf, p, mx, "\n");
    p = PutStr(buf, p, mx, "procs_running ");
    p = PutDec(buf, p, mx, LinuxSyscall::ActiveProcessCount());
    p = PutStr(buf, p, mx, "\n");
    p = PutStr(buf, p, mx, "procs_blocked 0\n");
    return p;
}

int LinuxKernel::GenLoadavg(char* buf, int mx, void*) {
    int p = 0;
    p = PutDec(buf, p, mx, load_1 / 100); p = PutStr(buf, p, mx, ".");
    int frac = (load_1 % 100);
    if (frac < 10) p = PutStr(buf, p, mx, "0");
    p = PutDec(buf, p, mx, frac);
    p = PutStr(buf, p, mx, " ");

    p = PutDec(buf, p, mx, load_5 / 100); p = PutStr(buf, p, mx, ".");
    frac = (load_5 % 100);
    if (frac < 10) p = PutStr(buf, p, mx, "0");
    p = PutDec(buf, p, mx, frac);
    p = PutStr(buf, p, mx, " ");

    p = PutDec(buf, p, mx, load_15 / 100); p = PutStr(buf, p, mx, ".");
    frac = (load_15 % 100);
    if (frac < 10) p = PutStr(buf, p, mx, "0");
    p = PutDec(buf, p, mx, frac);
    p = PutStr(buf, p, mx, " ");

    int running_count = 0, total_count = 0;
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (threads[i].active) { total_count++; running_count++; }
    }
    p = PutDec(buf, p, mx, running_count);
    p = PutStr(buf, p, mx, "/");
    p = PutDec(buf, p, mx, total_count);
    p = PutStr(buf, p, mx, " ");
    p = PutDec(buf, p, mx, next_tid);
    p = PutStr(buf, p, mx, "\n");
    return p;
}

int LinuxKernel::GenMounts(char* buf, int mx, void*) {
    int p = 0;
    p = PutStr(buf, p, mx, "kvfs / kvfs rw,relatime 0 0\n");
    p = PutStr(buf, p, mx, "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n");
    p = PutStr(buf, p, mx, "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n");
    p = PutStr(buf, p, mx, "devtmpfs /dev devtmpfs rw,nosuid 0 0\n");
    p = PutStr(buf, p, mx, "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n");
    p = PutStr(buf, p, mx, "tmpfs /run tmpfs rw,nosuid,noexec,relatime 0 0\n");

    // Shared mounts
    SharedMount* mounts = SharedMountMgr::GetMounts();
    int mc = SharedMountMgr::GetMountCount();
    for (int i = 0; i < mc; i++) {
        if (!mounts[i].active) continue;
        p = PutStr(buf, p, mx, "kvfs:");
        p = PutStr(buf, p, mx, mounts[i].kurono_path);
        p = PutStr(buf, p, mx, " ");
        p = PutStr(buf, p, mx, mounts[i].linux_path);
        p = PutStr(buf, p, mx, " kvfs ");
        p = PutStr(buf, p, mx, mounts[i].read_only ? "ro" : "rw");
        p = PutStr(buf, p, mx, ",bind 0 0\n");
    }
    return p;
}

int LinuxKernel::GenFilesystems(char* buf, int mx, void*) {
    int p = 0;
    p = PutStr(buf, p, mx, "\tkvfs\n");
    p = PutStr(buf, p, mx, "\text4\n");
    p = PutStr(buf, p, mx, "\ttmpfs\n");
    p = PutStr(buf, p, mx, "\tproc\n");
    p = PutStr(buf, p, mx, "\tsysfs\n");
    p = PutStr(buf, p, mx, "\tdevtmpfs\n");
    return p;
}

int LinuxKernel::GenCmdline(char* buf, int mx, void*) {
    return PutStr(buf, 0, mx,
        "BOOT_IMAGE=/boot/kurono.elf root=/dev/kvfs0 ro quiet splash "
        "kurono.linux=integrated\n");
}

int LinuxKernel::GenPidStatus(char* buf, int mx, void* ctx) {
    LinuxProcess* proc = (LinuxProcess*)ctx;
    if (!proc) return 0;
    int p = 0;
    p = PutLine(buf, p, mx, "Name", proc->name);
    p = PutStr(buf, p, mx, "State:\tS (sleeping)\n");
    p = PutStr(buf, p, mx, "Pid:\t");
    p = PutDec(buf, p, mx, (int)proc->pid);
    p = PutStr(buf, p, mx, "\nPPid:\t");
    p = PutDec(buf, p, mx, (int)proc->ppid);
    p = PutStr(buf, p, mx, "\nUid:\t");
    p = PutDec(buf, p, mx, (int)proc->uid);
    p = PutStr(buf, p, mx, "\t");
    p = PutDec(buf, p, mx, (int)proc->euid);
    p = PutStr(buf, p, mx, "\nGid:\t");
    p = PutDec(buf, p, mx, (int)proc->gid);
    p = PutStr(buf, p, mx, "\t");
    p = PutDec(buf, p, mx, (int)proc->egid);
    p = PutStr(buf, p, mx, "\nVmSize:\t4096 kB\n");
    p = PutStr(buf, p, mx, "VmRSS:\t2048 kB\n");
    p = PutStr(buf, p, mx, "Threads:\t1\n");
    return p;
}

int LinuxKernel::GenPidStat(char* buf, int mx, void* ctx) {
    LinuxProcess* proc = (LinuxProcess*)ctx;
    if (!proc) return 0;
    int p = 0;
    p = PutDec(buf, p, mx, (int)proc->pid);
    p = PutStr(buf, p, mx, " (");
    p = PutStr(buf, p, mx, proc->name);
    p = PutStr(buf, p, mx, ") S ");
    p = PutDec(buf, p, mx, (int)proc->ppid);
    p = PutStr(buf, p, mx, " 0 0 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 0 4096 512 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    return p;
}

int LinuxKernel::GenPidMaps(char* buf, int mx, void*) {
    int p = 0;
    p = PutStr(buf, p, mx, "08048000-08100000 r-xp 00000000 00:00 0  [text]\n");
    p = PutStr(buf, p, mx, "08100000-0c000000 rw-p 00000000 00:00 0  [heap]\n");
    p = PutStr(buf, p, mx, "bf800000-c0000000 rw-p 00000000 00:00 0  [stack]\n");
    return p;
}

int LinuxKernel::GenNetDev(char* buf, int mx, void*) {
    int p = 0;
    p = PutStr(buf, p, mx, "Inter-|   Receive                                                |  Transmit\n");
    p = PutStr(buf, p, mx, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    p = PutStr(buf, p, mx, "    lo:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0\n");
    p = PutStr(buf, p, mx, "  eth0:    1024      16    0    0    0     0          0         0      512       8    0    0    0     0       0          0\n");
    return p;
}

int LinuxKernel::GenPartitions(char* buf, int mx, void*) {
    int p = 0;
    p = PutStr(buf, p, mx, "major minor  #blocks  name\n\n");
    p = PutStr(buf, p, mx, "   8        0    262144 sda\n");      // 256MB total
    p = PutStr(buf, p, mx, "   8        1     65536 sda1\n");     // 64MB Kurono
    p = PutStr(buf, p, mx, "   8        2    196608 sda2\n");     // 192MB Linux ext4
    return p;
}

// ═══════════════════════════════════════════════════════════════════════════
//  /sys filesystem
// ═══════════════════════════════════════════════════════════════════════════

void LinuxKernel::InitSysFS() {
    sys_count = 0;

    // Create directories in KVFS
    KVFS::Mkdirs("/sys");
    KVFS::Mkdirs("/sys/class");
    KVFS::Mkdirs("/sys/class/net");
    KVFS::Mkdirs("/sys/class/tty");
    KVFS::Mkdirs("/sys/class/block");
    KVFS::Mkdirs("/sys/devices");
    KVFS::Mkdirs("/sys/kernel");
    KVFS::Mkdirs("/sys/fs");

    // Populate basic entries
    auto add_sys = [](const char* path, const char* value, bool wr) {
        if (sys_count >= SYS_MAX_ENTRIES) return;
        SysEntry* e = &sys_entries[sys_count++];
        lk_scpy(e->path, path, sizeof(e->path));
        lk_scpy(e->value, value, sizeof(e->value));
        e->writable = wr;
        e->active = true;
    };

    add_sys("/sys/kernel/hostname",       "kurono",  true);
    add_sys("/sys/kernel/ostype",         "Linux",   false);
    add_sys("/sys/kernel/osrelease",      LINUX_KERNEL_RELEASE, false);
    add_sys("/sys/kernel/version",        "#1 SMP", false);
    add_sys("/sys/class/net/lo/address",  "00:00:00:00:00:00", false);
    add_sys("/sys/class/net/lo/mtu",      "65536", true);
    add_sys("/sys/class/net/lo/operstate","up", false);
    add_sys("/sys/class/net/eth0/address","02:42:ac:11:00:02", false);
    add_sys("/sys/class/net/eth0/mtu",    "1500", true);
    add_sys("/sys/class/net/eth0/operstate","up", false);
    add_sys("/sys/class/net/eth0/speed",  "1000", false);
    add_sys("/sys/class/block/sda/size",  "524288", false);  // 256MB in sectors
    add_sys("/sys/class/block/sda/queue/scheduler", "cfq", true);
    add_sys("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1000000", false);

    SerialLogger::Log("[LinuxKernel] /sys initialized (");
    SerialLogger::LogDec(sys_count);
    SerialLogger::Log(" entries)\r\n");
}

int LinuxKernel::SysIndex(const char* path) {
    for (int i = 0; i < sys_count; i++) {
        if (sys_entries[i].active && lk_seq(sys_entries[i].path, path))
            return i;
    }
    return -1;
}

int LinuxKernel::ReadSys(const char* path, char* buf, int max_len) {
    int idx = SysIndex(path);
    if (idx < 0) return -1;
    int len = lk_slen(sys_entries[idx].value);
    if (len >= max_len) len = max_len - 1;
    memcpy(buf, sys_entries[idx].value, len);
    buf[len] = '\n';
    buf[len + 1] = 0;
    return len + 1;
}

int LinuxKernel::WriteSys(const char* path, const char* data, int len) {
    int idx = SysIndex(path);
    if (idx < 0) return -1;
    if (!sys_entries[idx].writable) return -1;  // EPERM
    int copy = len < 255 ? len : 255;
    memcpy(sys_entries[idx].value, data, copy);
    sys_entries[idx].value[copy] = 0;
    // Trim trailing newline
    if (copy > 0 && sys_entries[idx].value[copy - 1] == '\n')
        sys_entries[idx].value[copy - 1] = 0;
    return copy;
}

bool LinuxKernel::SysExists(const char* path) {
    return SysIndex(path) >= 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  TTY/PTY subsystem
// ═══════════════════════════════════════════════════════════════════════════

void LinuxKernel::InitTTY() {
    memset(ttys, 0, sizeof(ttys));
    tty_count = 0;

    // Create /dev/console (tty0)
    LinuxTTY* con = &ttys[0];
    con->type = TTY_CONSOLE;
    lk_scpy(con->name, "tty0", sizeof(con->name));
    con->active = true;
    con->owner_pid = 0;
    con->ws_row = 25;
    con->ws_col = 80;
    con->ws_xpixel = 640;
    con->ws_ypixel = 400;
    // Default termios
    con->termios.c_iflag = 0x0500;   // ICRNL | IXON
    con->termios.c_oflag = 0x0005;   // OPOST | ONLCR
    con->termios.c_cflag = 0x00BF;   // CS8 | CREAD | HUPCL
    con->termios.c_lflag = 0x8A3B;   // ECHO | ECHOE | ECHOK | ISIG | ICANON | IEXTEN
    con->termios.c_ispeed = 38400;
    con->termios.c_ospeed = 38400;
    tty_count = 1;

    // Create /dev/ttyS0 (serial)
    LinuxTTY* ser = &ttys[1];
    ser->type = TTY_SERIAL;
    lk_scpy(ser->name, "ttyS0", sizeof(ser->name));
    ser->active = true;
    ser->owner_pid = 0;
    ser->ws_row = 25;
    ser->ws_col = 80;
    ser->termios.c_ispeed = 115200;
    ser->termios.c_ospeed = 115200;
    tty_count = 2;

    // Create device nodes in KVFS
    KVFS::Mkdirs("/dev");
    KVFS::Mkdirs("/dev/pts");

    SerialLogger::Log("[LinuxKernel] TTY subsystem initialized\r\n");
}

int LinuxKernel::AllocPTY() {
    for (int i = 2; i < LINUX_MAX_TTYS; i++) {
        if (!ttys[i].active) {
            ttys[i].type = TTY_PTY;
            char name[16] = "pts/";
            char num[8];
            int n = i - 2;
            if (n < 10) { num[0] = '0' + n; num[1] = 0; }
            else { num[0] = '0' + (n / 10); num[1] = '0' + (n % 10); num[2] = 0; }
            // Append num to name
            int p = 4;
            for (int j = 0; num[j]; j++) name[p++] = num[j];
            name[p] = 0;
            lk_scpy(ttys[i].name, name, sizeof(ttys[i].name));
            ttys[i].active = true;
            ttys[i].ws_row = 24;
            ttys[i].ws_col = 80;
            ttys[i].input_head = ttys[i].input_tail = 0;
            ttys[i].output_head = ttys[i].output_tail = 0;
            // Default termios
            ttys[i].termios.c_iflag = 0x0500;
            ttys[i].termios.c_oflag = 0x0005;
            ttys[i].termios.c_cflag = 0x00BF;
            ttys[i].termios.c_lflag = 0x8A3B;
            ttys[i].termios.c_ispeed = 38400;
            ttys[i].termios.c_ospeed = 38400;
            tty_count++;
            return i;
        }
    }
    return -1;
}

void LinuxKernel::FreePTY(int idx) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS) return;
    if (!ttys[idx].active) return;
    ttys[idx].active = false;
    tty_count--;
}

int LinuxKernel::TTYRead(int idx, char* buf, int max_len) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS || !ttys[idx].active) return -1;
    LinuxTTY* t = &ttys[idx];
    int count = 0;
    while (count < max_len && t->input_head != t->input_tail) {
        buf[count++] = t->input_buf[t->input_tail];
        t->input_tail = (t->input_tail + 1) % LINUX_TTY_BUF_SIZE;
    }
    return count;
}

int LinuxKernel::TTYWrite(int idx, const char* buf, int len) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS || !ttys[idx].active) return -1;
    LinuxTTY* t = &ttys[idx];

    // For console TTY, also write to serial for debugging
    if (t->type == TTY_CONSOLE || t->type == TTY_SERIAL) {
        char tmp[2] = {0, 0};
        for (int i = 0; i < len; i++) {
            tmp[0] = buf[i];
            SerialLogger::Log(tmp);
        }
    }

    int count = 0;
    while (count < len) {
        int next = (t->output_head + 1) % LINUX_TTY_BUF_SIZE;
        if (next == t->output_tail) break;  // Buffer full
        t->output_buf[t->output_head] = buf[count++];
        t->output_head = next;
    }
    return count;
}

void LinuxKernel::TTYSetTermios(int idx, const LinuxTermios* t) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS || !ttys[idx].active) return;
    memcpy(&ttys[idx].termios, t, sizeof(LinuxTermios));
}

void LinuxKernel::TTYGetTermios(int idx, LinuxTermios* t) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS || !ttys[idx].active) return;
    memcpy(t, &ttys[idx].termios, sizeof(LinuxTermios));
}

void LinuxKernel::TTYSetWinSize(int idx, uint16_t rows, uint16_t cols) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS || !ttys[idx].active) return;
    ttys[idx].ws_row = rows;
    ttys[idx].ws_col = cols;
}

LinuxTTY* LinuxKernel::GetTTY(int idx) {
    if (idx < 0 || idx >= LINUX_MAX_TTYS) return nullptr;
    return ttys[idx].active ? &ttys[idx] : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Thread management
// ═══════════════════════════════════════════════════════════════════════════

int LinuxKernel::CreateThread(uint32_t pid, uint32_t flags, uint32_t stack, uint32_t tls) {
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (!threads[i].active) {
            threads[i].tid = next_tid++;
            threads[i].pid = pid;
            threads[i].tgid = pid;
            threads[i].active = true;
            threads[i].tls_base = tls;
            threads[i].stack_base = stack;
            threads[i].stack_size = 0x10000;  // 64KB default
            threads[i].priority = 20;         // Normal priority
            threads[i].cpu_time = 0;
            threads[i].exit_code = 0;
            (void)flags;  // used for clone semantics
            return (int)threads[i].tid;
        }
    }
    return -1;
}

void LinuxKernel::DestroyThread(int tid) {
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (threads[i].active && (int)threads[i].tid == tid) {
            threads[i].active = false;

            // Wake any futexes waiting on this thread
            for (int f = 0; f < LINUX_MAX_FUTEXES; f++) {
                if (futexes[f].active) {
                    for (int w = 0; w < futexes[f].waiter_count; w++) {
                        if (futexes[f].waiters[w] == tid) {
                            // Remove waiter
                            for (int k = w; k < futexes[f].waiter_count - 1; k++)
                                futexes[f].waiters[k] = futexes[f].waiters[k + 1];
                            futexes[f].waiter_count--;
                            break;
                        }
                    }
                }
            }
            return;
        }
    }
}

LinuxThread* LinuxKernel::GetThread(int tid) {
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (threads[i].active && (int)threads[i].tid == tid)
            return &threads[i];
    }
    return nullptr;
}

int LinuxKernel::GetThreadCount() {
    int count = 0;
    for (int i = 0; i < LINUX_MAX_THREADS; i++) {
        if (threads[i].active) count++;
    }
    return count;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Futex
// ═══════════════════════════════════════════════════════════════════════════

int LinuxKernel::FutexWait(uint32_t* addr, uint32_t expected, uint32_t timeout) {
    if (!addr) return -1;
    if (*addr != expected) return -1;  // EAGAIN — value changed

    // Find or create futex entry
    int idx = -1;
    for (int i = 0; i < LINUX_MAX_FUTEXES; i++) {
        if (futexes[i].active && futexes[i].addr == addr) {
            idx = i; break;
        }
    }
    if (idx < 0) {
        for (int i = 0; i < LINUX_MAX_FUTEXES; i++) {
            if (!futexes[i].active) {
                futexes[i].addr = addr;
                futexes[i].active = true;
                futexes[i].waiter_count = 0;
                idx = i; break;
            }
        }
    }
    if (idx < 0) return -1;

    // Add current thread as waiter (simplified — normally blocks)
    if (futexes[idx].waiter_count < LINUX_MAX_THREADS) {
        futexes[idx].waiters[futexes[idx].waiter_count++] = next_tid - 1;
    }

    (void)timeout;
    return 0;
}

int LinuxKernel::FutexWake(uint32_t* addr, int count) {
    for (int i = 0; i < LINUX_MAX_FUTEXES; i++) {
        if (futexes[i].active && futexes[i].addr == addr) {
            int woken = 0;
            while (woken < count && futexes[i].waiter_count > 0) {
                futexes[i].waiter_count--;
                woken++;
            }
            if (futexes[i].waiter_count == 0) futexes[i].active = false;
            return woken;
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Misc kernel interfaces
// ═══════════════════════════════════════════════════════════════════════════

uint32_t LinuxKernel::GetUptime() {
    if (!running) return 0;
    return (Time::GetTicks() - start_time) / 1000;
}

uint64_t LinuxKernel::GetUptimeMs() {
    if (!running) return 0;
    return Time::GetTicks() - start_time;
}

void LinuxKernel::GetLoadAvg(int* avg1, int* avg5, int* avg15) {
    if (avg1)  *avg1  = load_1;
    if (avg5)  *avg5  = load_5;
    if (avg15) *avg15 = load_15;
}

uint32_t LinuxKernel::GetTotalMemory() {
    return 256 * 1024;   // 256 MB in KB (matches QEMU -m 256M)
}

uint32_t LinuxKernel::GetFreeMemory() {
    // Estimate: total - heap used
    return 200 * 1024;   // ~200MB free
}

uint32_t LinuxKernel::GetJiffies() { return jiffies; }
int LinuxKernel::GetHZ() { return 1000; }  // 1kHz timer

const char* LinuxKernel::GetVersion()    { return LINUX_KERNEL_VERSION; }
const char* LinuxKernel::GetRelease()    { return LINUX_KERNEL_RELEASE; }
const char* LinuxKernel::GetSysname()    { return LINUX_KERNEL_SYSNAME; }
const char* LinuxKernel::GetMachine()    { return LINUX_KERNEL_MACHINE; }
const char* LinuxKernel::GetDomainname() { return "(none)"; }

int LinuxKernel::GetRandom(void* buf, uint32_t len) {
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < len; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        p[i] = (uint8_t)(rng_state & 0xFF);
    }
    return (int)len;
}

int LinuxKernel::GetURandom(void* buf, uint32_t len) {
    return GetRandom(buf, len);  // Same for now; /dev/urandom = /dev/random
}
