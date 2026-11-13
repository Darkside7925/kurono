// claude's not the only one here anymore lol
#include "opencode_host.h"
#include "claude_host.h"      // EnsureCaBundle: the shared https trust roots (satoru)
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../fs/kvfs.h"
#include "../kernel/pmm.h"
#include "../kernel/userspace.h"
#include "../linux/ld_kurono.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"

extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;

// see opencode_host.h. modelled on claude_host.cpp, kept separate so the two
// loaders can change independently. (satoru)

namespace {
    const char* const OC_PATH = "/system/bin/opencode";
    // fat 8.3 root names, space padded (satoru)
    const char OC_83[11]  = { 'O','P','E','N','C','O','D','E',' ',' ',' ' };
    const char ENV_83[11] = { 'O','P','E','N','C','O','D','E','E','N','V' };

    uint8_t* g_image = nullptr;
    uint64_t g_image_len = 0;
    uint64_t g_last_as = 0;

    void put(char* msg, int mx, const char* s) {
        if (!msg || mx <= 0) return;
        int i = 0;
        while (s[i] && i < mx - 1) { msg[i] = s[i]; i++; }
        msg[i] = 0;
    }

    void put2(char* msg, int mx, const char* a, const char* b) {
        if (!msg || mx <= 0) return;
        int n = 0;
        for (int k = 0; a[k] && n < mx - 1; k++) msg[n++] = a[k];
        for (int k = 0; b && b[k] && n < mx - 1; k++) msg[n++] = b[k];
        msg[n] = 0;
    }

    void progress(uint64_t done, uint64_t total) {
        SerialLogger::Log("[opencode] loading ");
        SerialLogger::LogDec((int)(done >> 20));
        SerialLogger::Log(" / ");
        SerialLogger::LogDec((int)(total >> 20));
        SerialLogger::Log(" MB\r\n");
    }

    // split on spaces; 'single quotes' group words (satoru)
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

    // the launch defaults. no auto-update (the binary is a read-only root
    // file), no lsp server downloads. (satoru)
    const char* const kDefaults[] = {
        "PATH=/system/bin:/usr/bin:/bin",
        "HOME=/home/user",
        "TERM=xterm-256color",
        "LANG=C.UTF-8",
        "SHELL=/bin/bash",
        "TMPDIR=/tmp",
        "OPENCODE_DISABLE_AUTOUPDATE=1",
        "OPENCODE_DISABLE_LSP_DOWNLOAD=true",
        "NODE_EXTRA_CA_CERTS=/etc/ssl/certs/ca-certificates.crt",
        "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt",
        nullptr };

    char g_env_cache[2048];   // NAME=value\0NAME=value\0\0 (satoru)
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

bool OpenCodeHost::ProbeInstalled() {
    uint64_t sz = 0;
    return DiskLog::ProbeRootFile(OC_83, &sz) == 0 && sz > 0;
}

bool OpenCodeHost::Installed() {
    if (g_image && KVFS::Exists(OC_PATH)) return true;
    uint64_t sz = 0;
    return DiskLog::StatRootFile(OC_83, &sz) == 0 && sz > 0;
}

void OpenCodeHost::ReleaseImage() {
    if (!g_image) return;
    uint32_t live = g_last_as ? Scheduler::FindLivePidInAddressSpace(g_last_as) : 0;
    if (live) Scheduler::KillProcessGroup(live);
    KVFS::Unlink(OC_PATH);   // the borrowed node goes before its buffer (satoru)
    uint64_t alloc = (g_image_len + 4095) & ~4095ull;
    PMM::FreeBytes(g_image, (size_t)alloc);
    SerialLogger::Log("[opencode] released the loaded image (");
    SerialLogger::LogDec((int)(alloc >> 20));
    SerialLogger::Log(" MB)\r\n");
    g_image = nullptr;
    g_image_len = 0;
    g_last_as = 0;
}

bool OpenCodeHost::EnsureLoaded(char* msg, int msgmax) {
    if (g_image && KVFS::Exists(OC_PATH)) { put(msg, msgmax, "ok"); return true; }
    if (!PMM::ImageWindowFullyReserved() && !PMM::TryReserveImageWindow()) {
        put(msg, msgmax, "opencode: the fixed-image memory window is in use; reboot to run opencode");
        return false;
    }
    uint64_t sz = 0;
    int rc = DiskLog::StatRootFile(OC_83, &sz);
    if (rc != 0) {
        put2(msg, msgmax, "opencode: no OPENCODE file on the KURONO partition (copy the linux-x64-musl "
                          "opencode binary there as OPENCODE): ", DiskLog::RootFileMsg(rc));
        return false;
    }
    if (sz < 4096 || sz > 0xF0000000ull) { put(msg, msgmax, "opencode: OPENCODE has an implausible size"); return false; }
    SerialLogger::Log("[opencode] OPENCODE size ");
    SerialLogger::LogDec((int)(sz >> 20));
    SerialLogger::Log(" MB, reading (read only)\r\n");
    uint64_t alloc = (sz + 4095) & ~4095ull;
    uint8_t* buf = (uint8_t*)PMM::AllocBytes((size_t)alloc);
    if (!buf) { put(msg, msgmax, "opencode: not enough free memory to load the binary"); return false; }
    uint64_t t0 = Timer::GetRealMs64();
    rc = DiskLog::ReadRootFile(OC_83, buf, alloc, progress);
    if (rc != 0) {
        PMM::FreeBytes(buf, (size_t)alloc);
        put2(msg, msgmax, "opencode: read of OPENCODE failed: ", DiskLog::RootFileMsg(rc));
        return false;
    }
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        PMM::FreeBytes(buf, (size_t)alloc);
        put(msg, msgmax, "opencode: OPENCODE is not an ELF file (wrong file copied?)");
        return false;
    }
    KVFS::Mkdirs("/system/bin");
    if (KVFS::WriteFileStatic(OC_PATH, buf, (uint32_t)sz) != 0) {
        PMM::FreeBytes(buf, (size_t)alloc);
        put(msg, msgmax, "opencode: could not register /system/bin/opencode in the vfs");
        return false;
    }
    KVFS::Chmod(OC_PATH, 0755);
    g_image = buf; g_image_len = sz;
    SerialLogger::Log("[opencode] loaded in ");
    SerialLogger::LogDec((int)(Timer::GetRealMs64() - t0));
    SerialLogger::Log(" ms\r\n");
    put(msg, msgmax, "ok");
    return true;
}

int OpenCodeHost::Run(const char* args_line, const char* env_extra, char* msg, int msgmax) {
    if (!Userspace::IsReady()) Userspace::Init();
    if (!EnsureLoaded(msg, msgmax)) return -10;
    if (!PMM::ImageWindowFullyReserved() && !PMM::TryReserveImageWindow()) {
        put(msg, msgmax, "opencode: the fixed-image memory window is in use; reboot to run opencode");
        return -15;
    }
    if (!KVFS::Exists("/system/lib/libstdc++.so.6") || !KVFS::Exists("/system/lib/libgcc_s.so.1")) {
        put(msg, msgmax, "opencode: the musl c++ runtime is missing: copy libstdc++.so.6 and libgcc_s.so.1 "
                         "(alpine, or firefox/lib from FIREFOX.TAR) to the KURONO partition as LIBSTDC.SO "
                         "and LIBGCCS.SO, then reboot");
        return -16;
    }
    KVFSNode* node = KVFS::Resolve(OC_PATH);
    if (!node || !node->content || node->size < 4096) { put(msg, msgmax, "opencode: /system/bin/opencode has no content"); return -11; }

    // a fixed-address image over a low-linked kernel would take the gdt out from
    // under it (same check as claude). (satoru)
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
            if (lo != ~0ull && lo < ke && hi > 0x100000ull && ks < hi) {
                put(msg, msgmax, "opencode: this kernel is linked low; rebuild with KURONO_KERNEL_BASE=0x14000000");
                return -14;
            }
        }
    }

    ClaudeHost::EnsureCaBundle();
    KVFS::Mkdirs("/home/user");

    static char argbuf[2048];
    static char envbuf[2048];
    static const char* av[96];
    static const char* ev[64];
    int ac = 0;
    av[ac++] = OC_PATH;
    {
        int n = 0;
        if (args_line) while (args_line[n] && n < (int)sizeof(argbuf) - 1) { argbuf[n] = args_line[n]; n++; }
        argbuf[n] = 0;
        ac += split_args(argbuf, av + 1, 94);
        av[ac] = nullptr;
    }
    int ec = 0;
    {
        int n = 0;
        if (env_extra) while (env_extra[n] && n < (int)sizeof(envbuf) - 1) { envbuf[n] = env_extra[n]; n++; }
        envbuf[n] = 0;
        ec += split_args(envbuf, ev, 32);
    }
    // explicit pairs first so they win; then the cached env file + defaults (satoru)
    ec = AugmentExecEnv(ev, ec, 62);

    Process* proc = Scheduler::CreateUserProcess("opencode", 0, 1);
    if (!proc) { put(msg, msgmax, "opencode: CreateUserProcess failed"); return -12; }
    uint64_t entry = 0, rsp = 0;
    if (!LdKurono::ExecPIE(proc, node->content, node->size, OC_PATH, av, ev, 0, 0, &entry, &rsp, true)) {
        Scheduler::DestroyProcess(proc);
        put(msg, msgmax, "opencode: ld-kurono could not map the binary (see serial)");
        return -13;
    }
    proc->rip = entry; proc->rsp = rsp;
    proc->user_frame.rip = entry; proc->user_frame.rsp = rsp;
    proc->flags |= PROCESS_FLAG_STACK_READY;
    SerialLogger::Log("[opencode] exec ready, entering ring 3\r\n");
    LinuxSyscall::ClearConsoleOutput();
    LinuxSyscall::SetNextProcessCwd("/home/user");
    int rc = Userspace::RunProcessWithArgs(proc, av, ev, 1000, 1000);   // user program, not root (satoru)
    g_last_as = proc->address_space;
    LinuxSyscall::SetNextProcessCwd("");
    Scheduler::DestroyProcess(proc);
    put(msg, msgmax, "ok");
    return rc;
}

void OpenCodeHost::CacheEnvFile() {   // (satoru)
    int w = 0;
    static char file[1024];
    for (int src = 0; src < 2; src++) {
        int fl = 0;
        if (src == 0) {
            fl = KVFS::ReadFile("/etc/opencode.env", file, sizeof(file) - 1);
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
    for (int i = 0; i < (int)sizeof(file); i++) file[i] = 0;   // no copy of a key left behind (satoru)
    g_env_cached = true;
    SerialLogger::Log(w ? "[opencode] launch environment cached (values not logged)\r\n"
                        : "[opencode] no OPENCODE.ENV / /etc/opencode.env\r\n");
}

bool OpenCodeHost::IsOpenCodeBinary(const char* p) {   // (satoru)
    static const char* const names[] = { "/system/bin/opencode", "/usr/bin/opencode", "/bin/opencode" };
    for (const char* n : names) {
        int i = 0;
        while (n[i] && p[i] == n[i]) i++;
        if (!n[i] && !p[i]) return true;
    }
    return false;
}

int OpenCodeHost::AugmentExecEnv(const char** ev, int count, int max) {   // (satoru)
    int n = count;
    if (g_env_cached) {
        for (const char* l = g_env_cache; *l && n < max; ) {
            if (!env_has(ev, n, l)) ev[n++] = l;
            while (*l) l++;
            l++;
        }
    }
    for (int i = 0; kDefaults[i] && n < max; i++)
        if (!env_has(ev, n, kDefaults[i])) ev[n++] = kDefaults[i];
    ev[n] = nullptr;
    return n;
}

// end (satoru)
