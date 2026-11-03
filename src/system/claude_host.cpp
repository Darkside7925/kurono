#include "claude_host.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../fs/kvfs.h"
#include "../kernel/pmm.h"
#include "../kernel/userspace.h"
#include "../linux/ld_kurono.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"
#include "claude_shim.inc"
#include "claude_cabundle.inc"

extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;

// see claude_host.h. the binary is read-only from the KURONO partition and is
// never copied onto the kernel heap: it sits in a pmm block that the vfs borrows
// (WriteFileStatic), the same trick the embedded blobs use. (satoru)

namespace {
    const char* const CLAUDE_PATH = "/usr/bin/claude";
    // fat 8.3 root entry name, space padded. a windows copy of a file called
    // "claude" with no extension lands here. (satoru)
    const char CLAUDE_83[11] = { 'C','L','A','U','D','E',' ',' ',' ',' ',' ' };

    char g_boot_args[512];
    char g_boot_env[512];
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

bool ClaudeHost::EnsureLoaded(char* msg, int msgmax) {
    if (g_image && KVFS::Exists(CLAUDE_PATH)) { put(msg, msgmax, "ok"); return true; }
    uint64_t sz = 0;
    int rc = DiskLog::StatRootFile(CLAUDE_83, &sz);
    if (rc != 0) {
        char buf[200];
        int n = 0;
        const char* a = "claude: cannot open CLAUDE on the KURONO partition: ";
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
    KVFS::Mkdirs("/usr/bin");
    if (KVFS::WriteFileStatic(CLAUDE_PATH, buf, (uint32_t)sz) != 0) {
        PMM::FreeBytes(buf, (size_t)alloc);
        put(msg, msgmax, "claude: could not register /usr/bin/claude in the vfs");
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
    if (!EnsureLoaded(msg, msgmax)) return -10;
    KVFSNode* node = KVFS::Resolve(CLAUDE_PATH);
    if (!node || !node->content || node->size < 4096) { put(msg, msgmax, "claude: /usr/bin/claude has no content"); return -11; }

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
    if (!KVFS::Exists("/system/etc/ssl/certs/ca-certificates.crt")) {
        KVFS::Mkdirs("/system/etc/ssl/certs");
        KVFS::WriteFileStatic("/system/etc/ssl/certs/ca-certificates.crt", kCaBundle, (uint32_t)sizeof(kCaBundle));
        KVFS::Chmod("/system/etc/ssl/certs/ca-certificates.crt", 0644);
    }

    // uname has to say Linux for linux software to run, so that is all claude can see. tell it
    // what this machine really is. only laid once, so the user's own edits survive. (satoru)
    if (!KVFS::Exists("/home/user/CLAUDE.md")) {
        static const char kHomeNotes[] =
            "# You are running on Kurono OS\n\n"
            "This machine runs Kurono, an x86_64 operating system written from scratch. It is not Linux and there is no Linux kernel here. "
            "`uname` says Linux, and paths like /proc exist, only because Kurono's kernel implements the Linux syscall ABI so that unmodified Linux programs run. You are one of those programs. "
            "If asked what OS this is, the answer is Kurono.\n\n"
            "## What is here\n\n"
            "- Shell: busybox `sh` (`/bin/bash` is a small shim to it). There is no package manager for Linux software and no compiler.\n"
            "- Tools: `/system/bin` has busybox plus disk tools (ntfs tools, e2fsprogs, dosfstools, testdisk, photorec, chntpw). Run `ls /system/bin`.\n"
            "- Disks show up as `/dev/sda`, `/dev/nvme0n1p2` and so on.\n"
            "- Your home is `/home/user`. It is saved across reboots when a KURONO partition is present.\n"
            "- Kurono's own desktop terminal commands (`audio`, `nic`, `recovery`, `kpkg`, `help`) belong to its built-in shell. They do not exist in your Bash tool.\n\n"
            "## Rules on this machine\n\n"
            "On real hardware a write guard blocks disk writes outside the KURONO partition, so a failed write to a disk may be that guard rather than a bad disk. Say so instead of retrying. "
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
    if (!LdKurono::ExecPIE(proc, node->content, node->size, CLAUDE_PATH, av, ev, 0, 0, &entry, &rsp)) {
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
    int rc = Userspace::RunProcessWithArgs(proc, av, ev);
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

[[noreturn]] void ClaudeHost::BootRunnerEntry() {
    // let the nic / dhcp settle first when the test talks to a server (satoru)
    Scheduler::SleepMs(g_boot_env[0] ? 8000 : 2000);
    SerialLogger::Log("[claude] gate: args=");
    SerialLogger::Log(g_boot_args);
    SerialLogger::Log("\r\n");
    char msg[256];
    int rc = Run(g_boot_args, g_boot_env, msg, sizeof(msg));
    SerialLogger::Log("\r\n[claude] runner rc=");
    SerialLogger::LogDec(rc);
    SerialLogger::Log(" msg=");
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// end (satoru)
