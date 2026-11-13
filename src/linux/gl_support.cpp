//  native software gl support for job gl (see gl_support.h)
//  installs an alpine v3.24 mesa tree from a root tar on the KURONO fat32
//  partition and runs its unmodified binaries through ld-kurono + the real
//  musl interpreter, exactly like the firefox payload. (satoru)

#include "gl_support.h"
#include "../shell/shell.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/taskmem.h"
#include "../kernel/userspace.h"
#include "../proc/scheduler.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../packages/pkgmgr.h"
#include "ld_kurono.h"
#include "linux_syscall.h"

namespace {

constexpr int GL_OUT_MAX = 8192;

// kld dev harness: a deadline cancel probe for the glrun CYCLES loop. the
// detached launcher polls it every 5 ms and kills the program's group. (satoru)
static uint64_t g_glcyc_kill_at = 0;
static bool glcyc_cancel_probe() {
    return g_glcyc_kill_at != 0 && Timer::GetRealMs64() >= g_glcyc_kill_at;
}
int ga(char* b, int p, int m, const char* s) {
    while (s && *s && p < m - 1) b[p++] = *s++;
    b[p] = 0;
    return p;
}

int gi(char* b, int p, int m, int v) {
    if (p >= m - 1) return p;
    if (v < 0) { b[p++] = '-'; v = -v; }
    if (v == 0) { if (p < m - 1) b[p++] = '0'; b[p] = 0; return p; }
    char t[12]; int n = 0;
    while (v > 0 && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && p < m - 1) b[p++] = t[--n];
    b[p] = 0;
    return p;
}

void slog(const char* s) { SerialLogger::Log(s); }
void slogd(int v) { SerialLogger::LogDec(v); }

bool str_start(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return false; }
    return true;
}

// read one 8.3 root file from the KURONO partition and unpack it at kvfs /.
// same source and extractor FIREFOX.TAR uses (pkgmgr.cpp:1434). (satoru)
bool install_one(const char name83[11], const char* label, char* out, int& p, int mx) {
    uint64_t sz = 0;
    int rc = DiskLog::StatRootFile(name83, &sz);
    if (rc != 0 || sz == 0) {
        slog("[gl] "); slog(label); slog(" missing rc="); slogd(rc); slog("\r\n");
        return false;
    }
    if (sz < (1ull << 20) || sz > (1ull << 31)) {
        p = ga(out, p, mx, "glsetup: ");
        p = ga(out, p, mx, label);
        p = ga(out, p, mx, " size is implausible\n");
        return false;
    }
    slog("[gl] "); slog(label); slog(" found MB="); slogd((int)(sz >> 20)); slog("\r\n");
    uint64_t alloc = (sz + 4095) & ~4095ull;
    uint8_t* buf = (uint8_t*)PMM::AllocBytes((size_t)alloc);
    if (!buf) {
        slog("[gl] out of contiguous pmm\r\n");
        p = ga(out, p, mx, "glsetup: not enough contiguous memory for ");
        p = ga(out, p, mx, label);
        p = ga(out, p, mx, "\n");
        return false;
    }
    TaskMem::ChargeFrames(buf, (size_t)alloc);
    uint32_t t0 = Timer::GetTicks();
    rc = DiskLog::ReadRootFile(name83, buf, alloc, nullptr);
    if (rc != 0) {
        TaskMem::UnchargeFrames(buf); PMM::FreeBytes(buf, (size_t)alloc);
        p = ga(out, p, mx, "glsetup: read failed: ");
        p = ga(out, p, mx, DiskLog::RootFileMsg(rc));
        p = ga(out, p, mx, "\n");
        return false;
    }
    slog("[gl] "); slog(label); slog(" read ms="); slogd((int)(Timer::GetTicks() - t0)); slog("\r\n");
    t0 = Timer::GetTicks();
    bool ok = PackageManager::ExtractTar((const char*)buf, (int)sz, "/");
    TaskMem::UnchargeFrames(buf); PMM::FreeBytes(buf, (size_t)alloc);
    slog("[gl] "); slog(label); slog(" extract ms="); slogd((int)(Timer::GetTicks() - t0));
    slog(" ok="); slogd(ok ? 1 : 0); slog("\r\n");
    if (!ok) {
        p = ga(out, p, mx, "glsetup: extract failed\n");
        return false;
    }
    return true;
}

// recreate the soname links the ustar extractor drops. the table was packed
// into the tar as system/lib/gl-symlinks.txt ("<link>\t<target>" per line). (satoru)
int make_links(char* out, int p, int mx) {
    int sz = KVFS::GetFileSize("/system/lib/gl-symlinks.txt");
    if (sz <= 0 || sz > 65536) return p;
    char* buf = (char*)KernelHeap::Alloc((uint32_t)sz + 1);
    if (!buf) return p;
    int got = KVFS::ReadFile("/system/lib/gl-symlinks.txt", buf, (uint32_t)sz);
    if (got <= 0) { KernelHeap::Free(buf); return p; }
    buf[got] = 0;
    int made = 0, failed = 0;
    int i = 0;
    while (i < got) {
        int ls = i;
        while (i < got && buf[i] != '\t' && buf[i] != '\n') i++;
        if (i >= got || buf[i] != '\t') break;
        buf[i] = 0;
        char* link = buf + ls;
        int ts = ++i;
        while (i < got && buf[i] != '\n') i++;
        if (i < got) buf[i++] = 0;
        char* target = buf + ts;
        if (!link[0] || !target[0]) continue;
        if (KVFS::Resolve(link)) continue;   // already resolvable
        if (KVFS::Symlink(link, target) == 0) made++;
        else failed++;
    }
    KernelHeap::Free(buf);
    p = ga(out, p, mx, "glsetup: symlinks ");
    p = gi(out, p, mx, made);
    p = ga(out, p, mx, " made, ");
    p = gi(out, p, mx, failed);
    p = ga(out, p, mx, " failed\n");
    slog("[gl] symlinks made="); slogd(made); slog(" failed="); slogd(failed); slog("\r\n");
    return p;
}

// ld-kurono reads PT_INTERP /lib/ld-musl-x86_64.so.1 straight from kvfs, not
// through the linux /lib -> /system/lib mapping: point it at the tar copy. (satoru)
void make_interp_link() {
    if (!KVFS::Resolve("/lib/ld-musl-x86_64.so.1")) {
        int lr = KVFS::Symlink("/lib/ld-musl-x86_64.so.1",
                               "/system/lib/ld-musl-x86_64.so.1");
        slog("[gl] /lib/ld-musl link rc="); slogd(lr); slog("\r\n");
    }
}

const char kGLTar[11]   = { 'G','L','T','A','R',' ',' ',' ','T','A','R' };
const char kGameTar[11] = { 'G','A','M','E','T','A','R',' ','T','A','R' };

}  // namespace

namespace GlSupport {

int CmdSetup(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    bool have_gl = KVFS::Resolve("/system/lib/libEGL.so.1") != nullptr;
    if (!have_gl) {
        if (!install_one(kGLTar, "GLTAR", out, p, mx)) {
            p = ga(out, p, mx, "glsetup: install GLTAR.TAR in the KURONO partition root\n");
            return p;
        }
        p = make_links(out, p, mx);
        make_interp_link();
        p = ga(out, p, mx, "glsetup: installed software GL (EGL/GLES/GL + llvmpipe)\n");
    } else {
        p = ga(out, p, mx, "glsetup: software GL tree already installed\n");
        p = make_links(out, p, mx);
        make_interp_link();
    }
    // optional game tree: only when the disk carries it and it is not up yet. (satoru)
    uint64_t gsz = 0;
    if (!KVFS::Resolve("/system/bin/supertux2") &&
        DiskLog::StatRootFile(kGameTar, &gsz) == 0 && gsz > 0) {
        if (install_one(kGameTar, "GAMETAR", out, p, mx))
            p = ga(out, p, mx, "glsetup: installed the game tree\n");
    }
    return p;
}

int CmdRun(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    // leading KEY=VALUE words override the default environment. (satoru)
    char envbuf[2048];
    int ep = 0;
    char* envp[48];
    int en = 0;
    auto putenv = [&](const char* s) {
        if (en >= 46) return;
        int start = ep;
        while (*s && ep < (int)sizeof(envbuf) - 1) envbuf[ep++] = *s++;
        envbuf[ep++] = 0;
        envp[en++] = envbuf + start;
    };
    putenv("PATH=/system/bin:/system/lib");
    putenv("HOME=/home/user");
    putenv("USER=user");
    putenv("TMPDIR=/tmp");
    putenv("XDG_RUNTIME_DIR=/system/run/user/1000");
    putenv("WAYLAND_DISPLAY=wayland-0");
    putenv("XDG_DATA_DIRS=/system/share:/system/local/share");
    putenv("LD_LIBRARY_PATH=/system/lib:/apps/firefox/lib:/apps/firefox");
    putenv("LIBGL_ALWAYS_SOFTWARE=1");
    putenv("GALLIUM_DRIVER=llvmpipe");
    putenv("MESA_LOADER_DRIVER_OVERRIDE=llvmpipe");
    putenv("EGL_PLATFORM=wayland");
    putenv("SDL_VIDEODRIVER=wayland");
    // the in-kernel pulse server (pulse_server.cpp); openal/sdl clients use it (satoru)
    putenv("PULSE_SERVER=unix:/system/run/user/1000/pulse/native");
    int ai = 1;
    int cycles = 0, killms = 0;
    while (ai < argc && argv[ai]) {
        const char* a = argv[ai];
        bool kv = false;
        for (const char* c = a; *c; c++) {
            if (*c == '=') { kv = true; break; }
            if (*c == '/') break;
        }
        if (!kv) break;
        // dev kill loop: glrun CYCLES=N KILLMS=MS <program>. spawn, run MS,
        // kill, repeat; logs the pinned shm slots and free memory each cycle.
        if (str_start(a, "CYCLES=")) {
            for (const char* q = a + 7; *q >= '0' && *q <= '9'; q++) cycles = cycles * 10 + (*q - '0');
            ai++;
            continue;
        }
        if (str_start(a, "KILLMS=")) {
            for (const char* q = a + 7; *q >= '0' && *q <= '9'; q++) killms = killms * 10 + (*q - '0');
            ai++;
            continue;
        }
        putenv(a);
        ai++;
    }
    if (ai >= argc || !argv[ai]) {
        return ga(out, 0, mx, "usage: glrun [KEY=VALUE...] <program> [args...]\n");
    }
    char path[128];
    if (argv[ai][0] == '/') {
        int k = 0;
        while (argv[ai][k] && k < 127) { path[k] = argv[ai][k]; k++; }
        path[k] = 0;
    } else {
        int k = ga(path, 0, sizeof(path), "/system/bin/");
        k = ga(path, k, sizeof(path), argv[ai]);
    }
    // dev harness convenience: first use in a boot installs the tree from the
    // KURONO root tar, so one boot can test one program. (satoru)
    if (!KVFS::Resolve("/system/lib/libEGL.so.1") || !KVFS::Resolve(path)) {
        int sp = CmdSetup(nullptr, 0, nullptr, out, mx);
        if (sp > 0) {
            out[sp] = 0;
            slog("[gl] auto setup\r\n");
        }
    }
    if (!KVFS::Resolve(path)) {
        int p = ga(out, 0, mx, "glrun: not installed: ");
        p = ga(out, p, mx, path);
        p = ga(out, p, mx, " (run glsetup first)\n");
        return p;
    }
    if (!Userspace::IsReady()) Userspace::Init();
    int fsz = KVFS::GetFileSize(path);
    if (fsz <= 0 || fsz > 32 * 1024 * 1024) {
        return ga(out, 0, mx, "glrun: bad executable size\n");
    }
    uint8_t* image = (uint8_t*)KernelHeap::Alloc((uint32_t)fsz);
    if (!image) return ga(out, 0, mx, "glrun: out of memory for the exe\n");
    if (KVFS::ReadFile(path, image, (uint32_t)fsz) != fsz) {
        KernelHeap::Free(image);
        return ga(out, 0, mx, "glrun: read of exe failed\n");
    }
    const char* av[32];
    int ac = 0;
    av[ac++] = path;
    for (int i = ai + 1; i < argc && ac < 31; i++) {
        if (argv[i]) av[ac++] = argv[i];
    }
    av[ac] = nullptr;
    envp[en] = nullptr;

    if (cycles > 0 && killms > 0) {
        if (!Scheduler::DetachedLaunch() || !SMP::ApThreadSched()) {
            KernelHeap::Free(image);
            return ga(out, 0, mx, "glrun: cycles need kurono.apthreads=1 (detached launch)\n");
        }
        g_glcyc_kill_at = 0;
        int done = 0;
        for (int n = 0; n < cycles; n++) {
            Process* p2 = Scheduler::CreateUserProcess("glrun", 0, 1);
            if (!p2) break;
            uint64_t e2 = 0, r2 = 0;
            bool ex2 = LdKurono::ExecPIE(p2, image, (uint64_t)fsz, path, av, envp,
                                         1000, 1000, &e2, &r2);
            if (!ex2) { Scheduler::DestroyProcess(p2); break; }
            p2->rip = e2; p2->rsp = r2;
            p2->user_frame.rip = e2; p2->user_frame.rsp = r2;
            p2->flags |= PROCESS_FLAG_STACK_READY;
            unsigned t0 = Timer::GetTicks();
            g_glcyc_kill_at = Timer::GetRealMs64() + (uint64_t)killms;
            Userspace::SetCancelProbe(Scheduler::GetCurrentProcess(), glcyc_cancel_probe);
            int rc = Userspace::RunProcessWithArgs(p2, av, envp, 1000, 1000);
            g_glcyc_kill_at = 0;
            Userspace::SetCancelProbe(nullptr, nullptr);
            Scheduler::DestroyProcess(p2);
            for (int w = 0; w < 30; w++) { Timer::WaitMs(15); KuronoShell::PumpUI(); }
            int pin_n = 0, bor = 0;
            uint64_t pin_b = 0;
            LinuxSyscall::ShmKinds(&pin_n, &pin_b, &bor);
            slog("[glcyc] n="); slogd(n + 1);
            slog(" rc="); slogd(rc);
            slog(" pin="); slogd(pin_n);
            slog(" pin_kb="); slogd((int)(pin_b >> 10));
            slog(" borrow="); slogd(bor);
            slog(" free_mb="); slogd((int)(PMM::GetFreeFrames() >> 8));
            slog(" ms="); slogd((int)(Timer::GetTicks() - t0));
            slog("\r\n");
            done++;
        }
        KernelHeap::Free(image);
        int p = ga(out, 0, mx, "glrun: cycles ");
        p = gi(out, p, mx, done);
        p = ga(out, p, mx, "/");
        p = gi(out, p, mx, cycles);
        p = ga(out, p, mx, "\n");
        return p;
    }

    Process* proc = Scheduler::CreateUserProcess("glrun", 0, 1);
    if (!proc) { KernelHeap::Free(image); return ga(out, 0, mx, "glrun: CreateUserProcess failed\n"); }
    uint64_t entry = 0, rsp = 0;
    bool ok = LdKurono::ExecPIE(proc, image, (uint64_t)fsz, path, av, envp,
                                1000, 1000, &entry, &rsp);
    KernelHeap::Free(image);
    if (!ok) {
        Scheduler::DestroyProcess(proc);
        return ga(out, 0, mx, "glrun: ExecPIE failed (see serial)\n");
    }
    proc->rip = entry; proc->rsp = rsp;
    proc->user_frame.rip = entry; proc->user_frame.rsp = rsp;
    proc->flags |= PROCESS_FLAG_STACK_READY;

    slog("[gl] run "); slog(path);
    for (int i = 0; i < en; i++) {
        const char* e = envp[i];
        if (!e) continue;
        if (!str_start(e, "EGL_PLATFORM=") && !str_start(e, "LIBGL_ALWAYS_SOFTWARE=") &&
            !str_start(e, "GALLIUM_DRIVER=") && !str_start(e, "MESA_LOADER_DRIVER_OVERRIDE=") &&
            !str_start(e, "SDL_VIDEODRIVER=") && !str_start(e, "WAYLAND_DISPLAY=") &&
            !str_start(e, "XDG_RUNTIME_DIR=")) continue;
        slog(" "); slog(e);
    }
    slog("\r\n");
    LinuxSyscall::ClearConsoleOutput();
    int rc = Userspace::RunProcessWithArgs(proc, av, envp, 1000, 1000);
    int p = LinuxSyscall::ReadConsoleOutput(out, mx - 1);
    if (p < 0) p = 0;
    out[p] = 0;
    p = ga(out, p, mx, "\nglrun: ");
    p = ga(out, p, mx, path);
    p = ga(out, p, mx, " exited ");
    p = gi(out, p, mx, rc);
    p = ga(out, p, mx, "\n");
    slog("[gl] exit "); slogd(rc); slog("\r\n");
    if (out[0]) {
        slog("[gl] out begin\r\n");
        int n = p < GL_OUT_MAX ? p : GL_OUT_MAX;
        if (n >= 0 && n < mx) {
            out[n] = 0;
            SerialLogger::Log(out);
        }
        slog("\r\n[gl] out end\r\n");
    }
    Scheduler::DestroyProcess(proc);
    return p;
}

}  // namespace GlSupport

// end (satoru)
