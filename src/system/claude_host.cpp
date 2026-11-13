#include "claude_host.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../fs/persist.h"
#include "../drivers/timer.h"
#include "../fs/kvfs.h"
#include "../kernel/pmm.h"
#include "../kernel/heap.h"   // IsValidBlock: borrowed image vs heap copy (satoru)
#include "../kernel/userspace.h"
#include "../linux/ld_kurono.h"
#include "../kernel/elf_loader.h"
#include "../userprogs/embedded_userprogs.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"
#include "claude_shim.inc"
#include "claude_cabundle.inc"
#include "claude_skill.inc"

extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;

// see claude_host.h. the binary is read-only from the KURONO partition and is
// never copied onto the kernel heap: it sits in a pmm block that the vfs borrows
// (WriteFileStatic), the same trick the embedded blobs use. (satoru)

namespace {
    // the vfs path the binary lives at, and the one /proc/self/exe reports. it
    // must be a path a linux program can open back: guest /usr/bin/x is
    // translated to vfs /system/bin/x, so a binary registered at vfs
    // /usr/bin/claude reported an exe path that resolved nowhere. claude code
    // runs its embedded ripgrep by exec'ing process.execPath with argv0 "rg",
    // so every rg call (Grep, Glob, the file index) died at execve with exit
    // 127. /system/bin passes through untranslated, and guest /usr/bin/claude
    // and /bin/claude land on it too. (satoru)
    const char* const CLAUDE_PATH = "/system/bin/claude";
    // fat 8.3 root entry name, space padded. a windows copy of a file called
    // "claude" with no extension lands here. (satoru)
    const char CLAUDE_83[11] = { 'C','L','A','U','D','E',' ',' ',' ',' ',' ' };

    char g_boot_args[512];
    char g_boot_env[512];
    bool g_window_test = false;
    uint8_t* g_image = nullptr;
    uint64_t g_image_len = 0;

    void put(char* msg, int mx, const char* s) {
        if (!msg || mx <= 0) return;
        int i = 0;
        while (s[i] && i < mx - 1) { msg[i] = s[i]; i++; }
        msg[i] = 0;
    }

    void progress(uint64_t done, uint64_t total) {
        SerialLogger::Log("[claude] loading ");
        SerialLogger::LogDec((int)(done >> 20));
        SerialLogger::Log(" / ");
        SerialLogger::LogDec((int)(total >> 20));
        SerialLogger::Log(" MB\r\n");
    }

    // split line on spaces into argv slots; 'single quotes' group words. (satoru)
    int split_args(char* line, const char** argv, int max) {
        int n = 0;
        char* p = line;
        while (*p && n < max - 1) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (*p == '\'') {
                p++;
                argv[n++] = p;
                while (*p && *p != '\'') p++;
            } else {
                argv[n++] = p;
                while (*p && *p != ' ') p++;
            }
            if (*p) *p++ = 0;
        }
        argv[n] = nullptr;
        return n;
    }
}

void ClaudeHost::AdoptImage(uint8_t* buf, uint64_t len) {
    g_image = buf;
    g_image_len = len;
}

bool ClaudeHost::ProbeInstalled() {
    if (PersistStore::ClaudeImage(nullptr, nullptr, false)) return true;
    uint64_t sz = 0;
    return DiskLog::ProbeRootFile(CLAUDE_83, &sz) == 0 && sz > 0;
}

bool ClaudeHost::Installed() {
    if (g_image && KVFS::Exists(CLAUDE_PATH)) return true;
    if (PersistStore::ClaudeImage(nullptr, nullptr, false)) return true;
    uint64_t sz = 0;
    return DiskLog::StatRootFile(CLAUDE_83, &sz) == 0 && sz > 0;
}

// a forked tool child (rg, sh) can outlive the leader; kill the group before
// the image its address space maps goes away. (satoru)
static uint64_t g_last_image_as = 0;

void ClaudeHost::ReleaseImage() {
    if (!g_image) return;
    uint32_t live = g_last_image_as ? Scheduler::FindLivePidInAddressSpace(g_last_image_as) : 0;
    if (live) Scheduler::KillProcessGroup(live);
    // borrowed nodes must go before the buffer they point at.
    KVFS::Unlink("/system/bin/claude");
    KVFS::Unlink("/opt/claude/claude");
    uint64_t alloc = (g_image_len + 4095) & ~4095ull;
    PMM::FreeBytes(g_image, (size_t)alloc);
    SerialLogger::Log("[claude] released the loaded image (");
    SerialLogger::LogDec((int)(alloc >> 20));
    SerialLogger::Log(" MB)\r\n");
    g_image = nullptr;
    g_image_len = 0;
    g_last_image_as = 0;
}

bool ClaudeHost::EnsureLoaded(char* msg, int msgmax) {
    if (g_image && KVFS::Exists(CLAUDE_PATH)) { put(msg, msgmax, "ok"); return true; }
    // the fixed-address image needs the low ram window reserved before its pmm
    // buffer is allocated, or the two would overlap. a boot with no installed
    // claude releases the window; take it back if it is still free. (satoru)
    if (!PMM::ImageWindowFullyReserved() && !PMM::TryReserveImageWindow()) {
        put(msg, msgmax, "claude: the fixed-image memory window is in use; reboot to run claude");
        return -15;
    }
    // a copy installed by `kpkg install claude` (downloaded from anthropic and
    // sha256-checked at install time) comes first. after a reboot it is brought
    // back from the persistent volume into one pmm block, no heap copy. (satoru)
    {
        const uint8_t* img = nullptr;
        uint64_t ilen = 0;
        if (PersistStore::ClaudeImage(&img, &ilen, true) && img && ilen >= 4096 &&
            img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F') {
            KVFS::Mkdirs("/system/bin");
            if (KVFS::WriteFileStatic(CLAUDE_PATH, img, (uint32_t)ilen) == 0) {
                KVFS::Chmod(CLAUDE_PATH, 0755);
                g_image = (uint8_t*)img; g_image_len = ilen;
                SerialLogger::Log("[claude] using the kpkg-installed copy (/opt/claude)\r\n");
                put(msg, msgmax, "ok");
                return true;
            }
        }
    }
    uint64_t sz = 0;
    int rc = DiskLog::StatRootFile(CLAUDE_83, &sz);
    if (rc != 0) {
        char buf[200];
        int n = 0;
        const char* a = "claude: not installed (run: kpkg install claude). no CLAUDE file on the KURONO partition either: ";
        while (a[n] && n < 120) { buf[n] = a[n]; n++; }
        const char* r = DiskLog::RootFileMsg(rc);
        for (int k = 0; r[k] && n < 198; k++) buf[n++] = r[k];
        buf[n] = 0;
        put(msg, msgmax, buf);
        return false;
    }
    if (sz < 4096 || sz > 0xF0000000ull) { put(msg, msgmax, "claude: CLAUDE file has an implausible size"); return false; }
    SerialLogger::Log("[claude] CLAUDE size ");
    SerialLogger::LogDec((int)(sz >> 20));
    SerialLogger::Log(" MB, reading (read only)\r\n");
    uint64_t alloc = (sz + 4095) & ~4095ull;
    uint8_t* buf = (uint8_t*)PMM::AllocBytes((size_t)alloc);
    if (!buf) { put(msg, msgmax, "claude: not enough free memory to load the 300 MB binary"); return false; }
    uint64_t t0 = Timer::GetRealMs64();
    rc = DiskLog::ReadRootFile(CLAUDE_83, buf, alloc, progress);
    if (rc != 0) {
        PMM::FreeBytes(buf, (size_t)alloc);
        char b[160]; int n = 0;
        const char* a = "claude: read of CLAUDE failed: ";
        while (a[n]) { b[n] = a[n]; n++; }
        const char* r = DiskLog::RootFileMsg(rc);
        for (int k = 0; r[k] && n < 158; k++) b[n++] = r[k];
        b[n] = 0;
        put(msg, msgmax, b);
        return false;
    }
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        PMM::FreeBytes(buf, (size_t)alloc);
        put(msg, msgmax, "claude: CLAUDE is not an ELF file (wrong file copied?)");
        return false;
    }
    KVFS::Mkdirs("/system/bin");
    if (KVFS::WriteFileStatic(CLAUDE_PATH, buf, (uint32_t)sz) != 0) {
        PMM::FreeBytes(buf, (size_t)alloc);
        put(msg, msgmax, "claude: could not register /system/bin/claude in the vfs");
        return false;
    }
    KVFS::Chmod(CLAUDE_PATH, 0755);
    g_image = buf; g_image_len = sz;
    SerialLogger::Log("[claude] loaded in ");
    SerialLogger::LogDec((int)(Timer::GetRealMs64() - t0));
    SerialLogger::Log(" ms\r\n");
    put(msg, msgmax, "ok");
    return true;
}

int ClaudeHost::Run(const char* args_line, const char* env_extra, char* msg, int msgmax) {
    if (!Userspace::IsReady()) Userspace::Init();
    // the kurono skill: how to inspect and repair this machine. it is ours, not the user's, so
    // it is rewritten every launch to stay current; vfs only, never a disk. (satoru)
    KVFS::Mkdirs("/home/user/.claude/skills/kurono");
    KVFS::WriteFileStatic("/home/user/.claude/skills/kurono/SKILL.md", (const uint8_t*)kKuronoSkill, (uint32_t)(sizeof(kKuronoSkill) - 1));
    if (!EnsureLoaded(msg, msgmax)) return -10;
    if (!PMM::ImageWindowFullyReserved() && !PMM::TryReserveImageWindow()) {
        put(msg, msgmax, "claude: the fixed-image memory window is in use; reboot to run claude");
        return -15;
    }
    KVFSNode* node = KVFS::Resolve(CLAUDE_PATH);
    if (!node || !node->content || node->size < 4096) { put(msg, msgmax, "claude: /system/bin/claude has no content"); return -11; }

    // claude is an ET_EXEC at a fixed low address. the kernel is identity mapped
    // and runs on the process's own page tables, so a user image that overlays the
    // kernel image takes the gdt out from under it and the machine triple faults.
    // refuse that cleanly instead; the cure is a kernel linked high
    // (make KURONO_KERNEL_BASE=0x14000000). (satoru)
    {
        const uint8_t* img = node->content;
        uint64_t phoff = *(const uint64_t*)(img + 32);
        uint16_t phnum = *(const uint16_t*)(img + 56);
        uint16_t phent = *(const uint16_t*)(img + 54);
        uint16_t etype = *(const uint16_t*)(img + 16);
        uint64_t lo = ~0ull, hi = 0;
        if (etype == 2 && phent >= 56 && phoff + (uint64_t)phnum * phent <= node->size) {
            for (uint16_t i = 0; i < phnum; i++) {
                const uint8_t* ph = img + phoff + (uint64_t)i * phent;
                if (*(const uint32_t*)ph != 1) continue;   // PT_LOAD
                uint64_t va = *(const uint64_t*)(ph + 16);
                uint64_t ms = *(const uint64_t*)(ph + 40);
                if (va < lo) lo = va;
                if (va + ms > hi) hi = va + ms;
            }
            uint64_t ks = (uint64_t)(uintptr_t)&kernel_start;
            uint64_t ke = (uint64_t)(uintptr_t)&kernel_end;
            // the whole identity window up to the kernel image must be pmm-reserved
            // too, which the high link guarantees (pmm.cpp). (satoru)
            if (lo != ~0ull && lo < ke && hi > 0x100000ull && ks < hi) {
                put(msg, msgmax, "claude: this kernel is linked low (0x100000) and the claude binary is a fixed-address "
                                 "image at 0x200000 that would overwrite it. rebuild with KURONO_KERNEL_BASE=0x14000000");
                return -14;
            }
        }
    }

    // claude code insists on a binary called bash or zsh. kurono ships busybox (sh/ash);
    // lay a tiny shim named bash that execs busybox as sh. same bytes every time, written
    // into the vfs only (never to a disk). (satoru)
    if (!KVFS::Exists("/system/bin/bash")) {
        if (KVFS::Exists("/system/bin/busybox")) {
            KVFS::Mkdirs("/system/bin");
            KVFS::WriteFileStatic("/system/bin/bash", kBashShim, (uint32_t)sizeof(kBashShim));
            KVFS::Chmod("/system/bin/bash", 0755);
        } else {
            SerialLogger::Log("[claude] warning: /system/bin/busybox missing, the Bash tool has no shell\r\n");
        }
    }

    // https needs trust roots: lay the mozilla root store into the vfs (borrowed, not copied)
    // where bun and openssl look, and point NODE_EXTRA_CA_CERTS / SSL_CERT_FILE at it. (satoru)
    EnsureCaBundle();

    // uname has to say Linux for linux software to run, so that is all claude can see. tell it
    // what this machine really is. only laid once, so the user's own edits survive. (satoru)
    if (!KVFS::Exists("/home/user/CLAUDE.md")) {
        static const char kHomeNotes[] =
            "# You are running on Kurono OS\n\n"
            "This machine runs Kurono, an x86_64 operating system written from scratch. It is not Linux and there is no Linux kernel here. "
            "`uname` says Linux, and paths like /proc exist, only because Kurono's kernel implements the Linux syscall ABI so that unmodified Linux programs run. You are one of those programs. "
            "If asked what OS this is, the answer is Kurono.\n\n"
            "## What is here\n\n"
            "- Shell: real bash 5.2 at `/bin/bash`; `/bin/sh` and all 272 busybox applets are on PATH.\n"
            "- Tools: busybox, git (static; local repositories only), wget with HTTPS, tar/gzip/unzip/diff/patch, vi, less, the full coreutils set, and the disk tools. Run `ls /system/bin`. There is no python3, no compiler and no Linux package manager.\n"
            "- Disks show up as `/dev/sda`, `/dev/nvme0n1p2` and so on.\n"
            "- Your home is `/home/user`. It is saved across reboots when a KURONO partition is present.\n"
            "- Kurono's own commands are available from bash: `kcmd <command>` (or `ksh-run <command>`) runs any of them through `/dev/kshell`; common ones also have direct names (`ksys`, `kdrv`, `kinit`, `kctl`, `audio`, `display`, `gpu`, `nic`, `wifi`, `hwinfo`, `lsusb`, `lspci`, `journal`, ...). `dmesg` is the live kernel log.\n"
            "- To inspect or repair Kurono itself (drivers, services, logs, hardware, network, audio, storage, persistence), use the `kurono` skill at `/home/user/.claude/skills/kurono/SKILL.md`.\n\n"
            "## Rules on this machine\n\n"
            "On real hardware a write guard blocks disk writes outside the KURONO partition, so a failed write to a disk may be that guard rather than a bad disk. Say so instead of retrying. "
            "Privileged commands ask the desktop operator for approval; a headless prompt cannot be answered, so do not loop on a denied command. "
            "Never stop or kill core services, never write raw disks, and save work under /home/user. "
            "Some syscalls and /proc and /sys entries are missing or partial, so if a normal Linux command misbehaves, consider that Kurono may not implement it yet.\n";
        KVFS::Mkdirs("/home/user");
        KVFS::WriteFileStatic("/home/user/CLAUDE.md", (const uint8_t*)kHomeNotes, (uint32_t)(sizeof(kHomeNotes) - 1));
    }

    static char argbuf[512];
    static char envbuf[512];
    static const char* av[24];
    static const char* ev[40];
    int ac = 0;
    av[ac++] = CLAUDE_PATH;
    {
        int n = 0;
        if (args_line) while (args_line[n] && n < (int)sizeof(argbuf) - 1) { argbuf[n] = args_line[n]; n++; }
        argbuf[n] = 0;
        ac += split_args(argbuf, av + 1, 22);
        av[ac] = nullptr;
    }
    // no explicit env (the shell command found none): fall back to the kurono.claude.env boot value,
    // a runtime setting for headless tests. (satoru)
    if ((!env_extra || !*env_extra) && g_boot_env[0]) env_extra = g_boot_env;
    int ec = 0;
    ev[ec++] = "PATH=/system/bin:/usr/bin:/bin";
    ev[ec++] = "HOME=/home/user";
    ev[ec++] = "TERM=xterm-256color";
    ev[ec++] = "LANG=C.UTF-8";
    ev[ec++] = "SHELL=/bin/bash";
    ev[ec++] = "CLAUDE_CODE_SHELL=/bin/bash";
    ev[ec++] = "TMPDIR=/tmp";
    ev[ec++] = "DISABLE_AUTOUPDATER=1";
    ev[ec++] = "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1";
    ev[ec++] = "GIT_TEMPLATE_DIR=/system/share/git-core/templates";
    ev[ec++] = "NODE_EXTRA_CA_CERTS=/etc/ssl/certs/ca-certificates.crt";
    ev[ec++] = "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt";
    {
        int n = 0;
        if (env_extra) while (env_extra[n] && n < (int)sizeof(envbuf) - 1) { envbuf[n] = env_extra[n]; n++; }
        envbuf[n] = 0;
        ec += split_args(envbuf, ev + ec, 40 - ec);
        ev[ec] = nullptr;
    }

    Process* proc = Scheduler::CreateUserProcess("claude", 0, 1);
    if (!proc) { put(msg, msgmax, "claude: CreateUserProcess failed"); return -12; }
    uint64_t entry = 0, rsp = 0;
    // /system/bin/claude is always WriteFileStatic (borrowed, content_capacity
    // 0), so the loader can parse and map from it directly; copying 300 mb per
    // tool-search child is what made concurrent Glob/Grep runs exhaust memory. (satoru)
    bool persistent = node->content_capacity == 0 && !KernelHeap::IsValidBlock(node->content);
    if (!LdKurono::ExecPIE(proc, node->content, node->size, CLAUDE_PATH, av, ev, 0, 0, &entry, &rsp, persistent)) {
        Scheduler::DestroyProcess(proc);
        put(msg, msgmax, "claude: ld-kurono could not map the binary (see serial)");
        return -13;
    }
    proc->rip = entry; proc->rsp = rsp;
    proc->user_frame.rip = entry; proc->user_frame.rsp = rsp;
    proc->flags |= PROCESS_FLAG_STACK_READY;
    SerialLogger::Log("[claude] exec ready, entering ring 3\r\n");
    LinuxSyscall::ClearConsoleOutput();
    // start in the home directory (claude refuses to trust "/" by default); the linux
    // process is created inside RunProcessWithArgs and takes this once. (satoru)
    LinuxSyscall::SetNextProcessCwd("/home/user");
    int rc = Userspace::RunProcessWithArgs(proc, av, ev, 1000, 1000);   // user program, not root (satoru)
    g_last_image_as = proc->address_space;
    LinuxSyscall::SetNextProcessCwd("");
    Scheduler::DestroyProcess(proc);
    put(msg, msgmax, "ok");
    return rc;
}

void ClaudeHost::SetBootArgs(const char* args, const char* env) {
    int n = 0;
    if (args) while (args[n] && n < (int)sizeof(g_boot_args) - 1) { g_boot_args[n] = args[n]; n++; }
    g_boot_args[n] = 0;
    n = 0;
    if (env) while (env[n] && n < (int)sizeof(g_boot_env) - 1) { g_boot_env[n] = env[n]; n++; }
    g_boot_env[n] = 0;
}

void ClaudeHost::SetWindowTest(bool on) { g_window_test = on; }

void ClaudeHost::EnsureCaBundle() {   // (satoru)
    if (KVFS::Exists("/system/etc/ssl/certs/ca-certificates.crt")) return;
    KVFS::Mkdirs("/system/etc/ssl/certs");
    KVFS::WriteFileStatic("/system/etc/ssl/certs/ca-certificates.crt", kCaBundle, (uint32_t)sizeof(kCaBundle));
    KVFS::Chmod("/system/etc/ssl/certs/ca-certificates.crt", 0644);
}

extern const uint8_t _binary_hello_musl_elf_start[] __attribute__((weak));
extern const uint8_t _binary_hello_musl_elf_end[]   __attribute__((weak));

namespace {
    // run one static low-address ET_EXEC (hello_musl, linked at 0x400000) five
    // times. exercises the same fixed-image window as claude but without the
    // 300 mb image buffer. (satoru)
    int run_embedded_hello(int* out_rc) {
        if (!_binary_hello_musl_elf_start || _binary_hello_musl_elf_end <= _binary_hello_musl_elf_start)
            return -1;
        uint64_t sz = (uint64_t)(_binary_hello_musl_elf_end - _binary_hello_musl_elf_start);
        Process* p = ElfLoader::LoadELF64(_binary_hello_musl_elf_start, sz, "hello_musl");
        if (!p) return -2;
        const char* av[] = { "hello_musl", nullptr };
        const char* ev[] = { "PATH=/system/bin", "HOME=/root", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, ev, 0, 0);
        Scheduler::DestroyProcess(p);
        if (out_rc) *out_rc = rc;
        return 0;
    }

    // coordinator's low window proof: frames inside the window may already be
    // owned when a fixed image faults a page in. claiming one by hand must make
    // claude refuse that map (clean failure), never steal or shadow it; after
    // the claim is dropped, five claude runs and five static runs must work and
    // a 32 mb heap pattern must be untouched. (satoru)
    void window_stress() {
        const uint64_t wbase = 0x800000ULL;    // 8 mb: inside claude's first load span, clear of the image (satoru)
        bool reserved = PMM::ImageWindowFullyReserved();
        bool tiny = PMM::GetTotalMemory() < (1ULL << 30);   // 512 mb class: claude cannot fit (satoru)
        char msg[160];
        if (!reserved && !tiny) {
            int held = 0;
            for (int i = 0; i < 4; i++)
                if (PMM::AllocFrameAt(wbase + (uint64_t)i * PAGE_SIZE)) held++;
            SerialLogger::Log("[wintest] occupied ");
            SerialLogger::LogDec(held);
            SerialLogger::Log(" window frames at 0x800000\r\n");
            int rc_owned = ClaudeHost::Run("--version", nullptr, msg, sizeof(msg));
            SerialLogger::Log("[wintest] claude with owned window rc=");
            SerialLogger::LogDec(rc_owned);
            SerialLogger::Log(" msg=");
            SerialLogger::Log(msg);
            SerialLogger::Log("\r\n");
            for (int i = 0; i < held; i++) PMM::FreeFrame(wbase + (uint64_t)i * PAGE_SIZE);
        } else if (reserved) {
            SerialLogger::Log("[wintest] window is reserved: image pages come out of the reservation\r\n");
        } else {
            SerialLogger::Log("[wintest] tiny ram: skipping the claude legs (needs >=2 gb)\r\n");
        }

        const uint64_t HB = 32ULL << 20;
        uint8_t* buf = (uint8_t*)KernelHeap::Alloc((size_t)HB);
        if (buf) { for (uint64_t i = 0; i < HB; i++) buf[i] = (uint8_t)(i * 131 + 7); }
        SerialLogger::Log("[wintest] heap pattern buffer ");
        SerialLogger::Log(buf ? "ok" : "FAILED");
        SerialLogger::Log("\r\n");

        int ok = 0;
        for (int i = 0; i < 5 && !tiny; i++) {
            int rc = ClaudeHost::Run("--version", nullptr, msg, sizeof(msg));
            SerialLogger::Log("[wintest] claude run ");
            SerialLogger::LogDec(i + 1);
            SerialLogger::Log(" rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
            if (rc == 0) ok++;
        }
        int hello_ok = 0;
        for (int i = 0; i < 5; i++) {
            int rc = 0;
            int r = run_embedded_hello(&rc);
            SerialLogger::Log("[wintest] hello_musl run ");
            SerialLogger::LogDec(i + 1);
            SerialLogger::Log(" load=");
            SerialLogger::LogDec(r);
            SerialLogger::Log(" rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
            if (r == 0 && rc == 0) hello_ok++;
        }
        int bad = 0;
        if (buf) {
            for (uint64_t i = 0; i < HB; i++)
                if (buf[i] != (uint8_t)(i * 131 + 7)) { bad++; if (bad < 4) { SerialLogger::Log("[wintest] heap mismatch at "); SerialLogger::LogDec((int)(i >> 10)); SerialLogger::Log(" kb\r\n"); } }
        }
        // let sched=2's reaper drop the last address space so the window
        // frames it backed are actually free again. (satoru)
        Scheduler::SleepMs(2000);
        SerialLogger::Log("[wintest] RESULT claude ");
        SerialLogger::LogDec(ok);
        SerialLogger::Log("/5 static ");
        SerialLogger::LogDec(hello_ok);
        SerialLogger::Log("/5 heap_bad=");
        SerialLogger::LogDec(bad);
        SerialLogger::Log(" window_base_used=");
        SerialLogger::LogDec(PMM::IsFrameUsed(wbase) ? 1 : 0);
        SerialLogger::Log("\r\n");
        if (buf) KernelHeap::Free(buf);
    }
}  // namespace

[[noreturn]] void ClaudeHost::BootRunnerEntry() {
    // let the nic / dhcp settle first when the test talks to a server, and the
    // window stress wait for the desktop/firefox autorun to be up. (satoru)
    Scheduler::SleepMs(g_window_test ? 12000 : (g_boot_env[0] ? 8000 : 2000));
    SerialLogger::Log("[claude] gate: args=");
    SerialLogger::Log(g_boot_args);
    SerialLogger::Log("\r\n");
    char msg[256];
    if (g_window_test) {
        window_stress();
        msg[0] = 0;
        SerialLogger::Log("[wintest] stress complete, firefox still on screen\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    int rc = Run(g_boot_args, g_boot_env, msg, sizeof(msg));
    SerialLogger::Log("\r\n[claude] runner rc=");
    SerialLogger::LogDec(rc);
    SerialLogger::Log(" msg=");
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

namespace {
    char g_env_cache[1024];        // NAME=value\0NAME=value\0\0 (satoru)
    bool g_env_cached = false;
    bool env_has(const char** ev, int n, const char* line) {
        int nl = 0;
        while (line[nl] && line[nl] != '=') nl++;
        for (int i = 0; i < n; i++) {
            const char* e = ev[i];
            if (!e) continue;
            int k = 0;
            while (k < nl && e[k] == line[k]) k++;
            if (k == nl && e[nl] == '=') return true;
        }
        return false;
    }
}

void ClaudeHost::CacheEnvFile() {   // (satoru)
    int w = 0;
    static char file[512];
    static const char ENV_83[11] = { 'C','L','A','U','D','E',' ',' ','E','N','V' };
    for (int src = 0; src < 2; src++) {
        int fl = 0;
        if (src == 0) {
            fl = KVFS::ReadFile("/etc/claude.env", file, sizeof(file) - 1);
        } else {
            uint64_t fsz = 0;
            if (DiskLog::StatRootFile(ENV_83, &fsz) == 0 && fsz > 0 && fsz < sizeof(file) &&
                DiskLog::ReadRootFile(ENV_83, file, sizeof(file) - 1, nullptr) == 0)
                fl = (int)fsz;
        }
        if (fl <= 0) continue;
        file[fl] = 0;
        int ls = 0;
        for (int k = 0; k <= fl; k++) {
            if (file[k] != '\n' && file[k] != '\r' && file[k] != 0) continue;
            file[k] = 0;
            const char* ln = file + ls;
            ls = k + 1;
            int ll = 0; bool eq = false, sp = false;
            while (ln[ll]) { if (ln[ll] == '=') eq = true; if (ln[ll] == ' ') sp = true; ll++; }
            if (ll < 3 || ln[0] == '#' || !eq || sp || w + ll + 2 >= (int)sizeof(g_env_cache)) continue;
            for (int q = 0; q < ll; q++) g_env_cache[w++] = ln[q];
            g_env_cache[w++] = 0;
        }
    }
    g_env_cache[w] = 0;
    for (int i = 0; i < (int)sizeof(file); i++) file[i] = 0;   // no copy of the token left behind (satoru)
    g_env_cached = true;
    SerialLogger::Log(w ? "[claude] launch environment cached (values not logged)\r\n"
                        : "[claude] no CLAUDE.ENV / /etc/claude.env: claude launches keep their own env\r\n");
}

bool ClaudeHost::IsClaudeBinary(const char* p) {   // (satoru)
    static const char* const names[] = { "/system/bin/claude", "/opt/claude/claude", "/usr/bin/claude", "/bin/claude" };
    for (const char* n : names) {
        int i = 0;
        while (n[i] && p[i] == n[i]) i++;
        if (!n[i] && !p[i]) return true;
    }
    return false;
}

int ClaudeHost::AugmentExecEnv(const char** ev, int count, int max) {   // (satoru)
    static const char* const defaults[] = {
        "HOME=/home/user", "TERM=xterm-256color", "LANG=C.UTF-8", "SHELL=/bin/bash",
        "CLAUDE_CODE_SHELL=/bin/bash", "TMPDIR=/tmp", "DISABLE_AUTOUPDATER=1",
        "NODE_EXTRA_CA_CERTS=/etc/ssl/certs/ca-certificates.crt",
        "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt", nullptr };
    int n = count;
    for (int i = 0; defaults[i] && n < max; i++)
        if (!env_has(ev, n, defaults[i])) ev[n++] = defaults[i];
    if (g_env_cached) {
        for (const char* l = g_env_cache; *l && n < max; ) {
            if (!env_has(ev, n, l)) ev[n++] = l;
            while (*l) l++;
            l++;
        }
    }
    ev[n] = nullptr;
    return n;
}

// end (satoru)
