//  /dev/dri for Linux programs: Linux ABI half of the KLD drm userspace
//  interface. See dri_dev.h, ../kernel/kld_dri.h and kld/lx_emul/kimpl/lx_dri.h.
//
//  How a call works: the caller's syscall submits a request to the container
//  (one of 32 ring slots) and parks, restartably, exactly like poll(): the
//  frame is rewound so the syscall re-runs on wake and finds its pending
//  request by task. Each wake is one of: the request is done; the container
//  wants to touch the caller's memory (the CALLER copies it, here, in its own
//  address space, through the slot's bounce area); or nothing yet. A pending
//  SIGKILL cancels a queued request or abandons a served one. Other signals
//  stay pending until the call returns, as in a real uninterruptible ioctl.
//  (satoru)
//
//  end (satoru)

#include "dri_dev.h"
#include "linux_syscall.h"
#include "uaccess.h"
#include "../kernel/kld_dri.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../kernel/heap.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"
#include "../proc/cgroup.h"
#include "../shell/shell.h"
#include "../drivers/timer.h"
#include "../kernel/elf_loader.h"
#include "../kernel/userspace.h"
#include "../kernel/kld_dri_proto.h"

namespace DriDev {

bool g_enabled = false;
static uint32_t g_test = 0;
static bool g_noreap = false;
static bool g_cgtest = false;      // rig: item 9 (satoru)
static uint32_t g_cg_id;
uint32_t TestCg() { return g_cg_id; }
static bool g_killnow = false;   // rig (I2): no 150 ms wait before a requested kill
static bool g_reapexit = false;  // rig (I1): queue the deferred group reap for a child that exited by itself
static bool g_runold = false;    // rig (I2): the first runner: RunProcessWithArgs + a kill from the op context
static bool g_runexit = false;   // rig (I2): a runner ends itself with Scheduler::Exit() after one job (the first version did)
static uint32_t g_refs[256];   // open fd aliases per file index (dup, fork) (satoru)

static bool has_token(const char* cmd, const char* tok) {
    if (!cmd) return false;
    uint32_t tl = 0; while (tok[tl]) tl++;
    const char* p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* t = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((uint32_t)(p - t) == tl) {
            uint32_t i = 0; while (i < tl && t[i] == tok[i]) i++;
            if (i == tl) return true;
        }
    }
    return false;
}

void BootInit(const char* cmdline) {
    g_enabled = has_token(cmdline, "kurono.kld.dri=1");
    if (!g_enabled || !cmdline) return;
    const char* k = "kurono.kld.dri.test=";
    for (const char* p = cmdline; *p; p++) {
        uint32_t i = 0; while (k[i] && p[i] == k[i]) i++;
        if (!k[i] && (p == cmdline || p[-1] == ' ')) {
            uint32_t v = 0; const char* d = p + i;
            while (*d >= '0' && *d <= '9') v = v * 10 + (uint32_t)(*d++ - '0');
            g_test = v;
            break;
        }
    }
    if (has_token(cmdline, "kurono.kld.dri.noflush=1")) KldDri::SetNoFlush(true);   // rig (satoru)
    g_killnow = has_token(cmdline, "kurono.kld.dri.killnow=1");
    g_runexit = has_token(cmdline, "kurono.kld.dri.runexit=1");
    g_runold = has_token(cmdline, "kurono.kld.dri.runold=1");
    g_reapexit = has_token(cmdline, "kurono.kld.dri.reapexit=1");
    g_cgtest = has_token(cmdline, "kurono.kld.dri.cg=1");                             // rig: item 9, cgroup memory ledger (satoru)
    g_noreap = has_token(cmdline, "kurono.kld.dri.noreap=1");                         // rig: reap a killed child 10 s late (satoru)
    SerialLogger::Log("[dri] kurono.kld.dri=1: /dev/dri appears when a drm container attaches\r\n");
}

uint32_t TestId() { return g_test; }

bool MayOpen(uint32_t pid, uint32_t node) { (void)pid; (void)node; return true; }

static int parse_node(const char* resolved) {
    const char* q = nullptr;
    if (resolved[0] == '/' && resolved[1] == 's' && resolved[2] == 'y') q = resolved + 16;   // "/system/dev/dri/"
    else q = resolved + 9;                                                                     // "/dev/dri/"
    if (q[0] == 'c' && q[1] == 'a' && q[2] == 'r' && q[3] == 'd' && q[4] == '0' && !q[5]) return 0;
    if (q[0] == 'r' && q[1] == 'e' && q[2] == 'n' && q[3] == 'd' && q[4] == 'e' && q[5] == 'r' &&
        q[6] == 'D' && q[7] == '1' && q[8] == '2' && q[9] == '8' && !q[10]) return 128;
    return -1;
}

static bool starts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return false; }
    return true;
}

bool PathIsDri(const char* resolved) {
    return starts(resolved, "/system/dev/dri/") || starts(resolved, "/dev/dri/");
}

// run the caller's side of a pending request. returns true with *rep filled
// when it completed (the slot is released); false with *ret set when the
// syscall must return *ret now (parked, or aborted by SIGKILL). (satoru)
static bool Drive(LinuxProcess* p, int slot, KldDri::Reply* rep, int64_t* ret) {
    for (;;) {
        KldDri::UaReq ua;
        int pr = KldDri::Poll(slot, &ua, rep);
        if (pr == KldDri::POLL_DONE) { KldDri::Release(slot); return true; }
        if (pr == KldDri::POLL_UACC) {
            int64_t nc = (int64_t)ua.len;
            if (LinuxSyscall::DriUserOk(p, ua.ptr, ua.len, ua.dir == LXDRI_UA_TO_USER)) {
                uint8_t* b = (uint8_t*)PMM::PhysToVirt(ua.bounce_phys);
                if (ua.dir == LXDRI_UA_FROM_USER) memcpy(b, (const void*)(uintptr_t)ua.ptr, ua.len);
                else                              memcpy((void*)(uintptr_t)ua.ptr, b, ua.len);
                nc = 0;
            }
            KldDri::UaFinish(slot, &ua, nc);
            continue;
        }
        if (LinuxSyscall::DriFatalPending(p)) { KldDri::Abandon(slot); *ret = -4; return false; }
        *ret = LinuxSyscall::DriPark((uint32_t)slot, KldDri::ReqReady);
        return false;
    }
}

int64_t Open(LinuxProcess* p, const char* resolved, uint32_t flags, int* file_out) {
    Process* t = p->task;
    if (!t) return -3;
    int minor = parse_node(resolved);
    if (minor < 0) return -2;
    int slot = KldDri::FindBound(t, LXDRI_K_OPEN, ~0u, (uint32_t)minor, flags, 0, 0);
    if (slot < 0) {
        if (!KldDri::Attached() || !KldDri::MinorAvailable((uint32_t)minor)) return -19;
        if (!MayOpen(p->pid, (uint32_t)minor)) return -13;
        int fid = KldDri::FileAlloc((uint32_t)minor);
        if (fid < 0) return fid;
        slot = KldDri::Submit(t, t->address_space, LXDRI_K_OPEN, (uint32_t)fid, (uint32_t)minor, flags,
                              p->pid, p->pid, p->euid, p->egid, flags, 0, 0, 0, 0);
        if (slot < 0) { KldDri::FileFree((uint32_t)fid); return slot; }
    }
    KldDri::Reply rep; int64_t ret = 0;
    if (!Drive(p, slot, &rep, &ret)) return ret;
    if (rep.result < 0) { KldDri::FileFree(rep.file); return rep.result; }
    // a new file owns its index: a live count here would be a bug elsewhere, so add instead of overwriting (satoru)
    if (__atomic_add_fetch(&g_refs[rep.file & 0xFF], 1u, __ATOMIC_SEQ_CST) != 1)
        SerialLogger::Log("[dri] file index reused with a live alias count\r\n");
    *file_out = (int)rep.file;
    return OPEN_READY;
}

void OpenAbort(int file) {
    KldDri::FileClose((uint32_t)file);   // the open succeeded but no fd could be made: never counted (satoru)
}

int32_t Ioctl(LinuxProcess* p, LinuxFd* lfd, int fd, uint32_t cmd, uint64_t arg) {
    (void)fd;
    if (lfd->type == LFD_DMABUF) return -25;   // ENOTTY until D5.4 (satoru)
    Process* t = p->task;
    if (!t) return -3;
    uint32_t fid = (uint32_t)lfd->backend_fd;
    int slot = KldDri::FindBound(t, LXDRI_K_IOCTL, fid, cmd, arg, 0, 0);
    if (slot < 0) {
        if (!KldDri::FileAlive(fid)) return -19;   // ENODEV: the container is gone (satoru)
        uint32_t sz = (cmd >> 16) & 0x3FFF;        // _IOC_SIZE (satoru)
        if (sz && !UAccess::AccessOk(arg, sz)) return -14;
        slot = KldDri::Submit(t, t->address_space, LXDRI_K_IOCTL, fid, cmd, arg,
                              p->pid, p->pid, p->euid, p->egid, lfd->flags, 0, 0, 0, 0);
        if (slot < 0) return slot;
    }
    KldDri::Reply rep; int64_t ret = 0;
    if (!Drive(p, slot, &rep, &ret)) return (int32_t)ret;
    return (int32_t)rep.result;
}

int64_t Read(LinuxProcess* p, LinuxFd* lfd, uint64_t buf, uint64_t count) {
    (void)p; (void)lfd; (void)buf; (void)count;
    return -22;   // the event stream arrives with D5.5 (satoru)
}

int64_t Mmap(LinuxProcess* p, LinuxFd* lfd, uint64_t addr, uint64_t len, uint32_t prot,
             uint32_t flags, uint64_t offset) {
    if (lfd->type == LFD_DMABUF) return -19;   // D5.4 (satoru)
    Process* t = p->task;
    if (!t) return -3;
    // checked before rounding: (len + 4095) wraps for a huge length (satoru)
    if (len == 0 || len > (256ull << 20) || (offset & 4095) || (flags & 0x10) || (flags & 3) == 0 || (flags & 3) == 2)
        return (flags & 0x10) ? -22 : -22;                          // MAP_FIXED is refused, auto placement only (satoru)
    uint64_t alen = (len + 4095) & ~4095ull;
    uint32_t fid = (uint32_t)lfd->backend_fd;
    int slot = KldDri::FindBound(t, LXDRI_K_MMAP, fid, 0, 0, offset, alen);
    if (slot < 0) {
        if ((flags & 3) != 1 && (flags & 3) != 3) return -22;      // MAP_SHARED only (satoru)
        if (prot & 4) return -13;                                  // no PROT_EXEC on a device (satoru)
        if (!KldDri::FileAlive(fid)) return -19;
        slot = KldDri::Submit(t, t->address_space, LXDRI_K_MMAP, fid, 0, 0,
                              p->pid, p->pid, p->euid, p->egid, lfd->flags, offset, alen, prot, flags);
        if (slot < 0) return slot;
    }
    KldDri::Reply rep; int64_t ret = 0;
    if (!Drive(p, slot, &rep, &ret)) return ret;
    if (rep.result < 0) return rep.result;
    uint64_t pages = alen / 4096;
    uint64_t* frames = (uint64_t*)KernelHeap::Alloc(pages * 8);
    if (!frames) return -12;
    uint64_t n = 0;
    int rc = KldDri::ObjResolve(&rep, pages, frames, &n);
    if (rc < 0 || n != pages) {
        if (rc == 0) KldDri::FramesRelease(frames, n);
        KernelHeap::Free(frames);
        return rc < 0 ? rc : -22;
    }
    int64_t r = LinuxSyscall::MapForeignFrames(t, 0, alen, prot, flags, frames, n);   // owns the n references (satoru)
    KernelHeap::Free(frames);
    return r;
}

void Retain(const LinuxFd* lfd) {
    if (lfd->type == LFD_DRM) __atomic_add_fetch(&g_refs[(uint32_t)lfd->backend_fd & 0xFF], 1u, __ATOMIC_SEQ_CST);
}

void FdClose(LinuxProcess* p, LinuxFd* lfd) {
    (void)p;
    if (lfd->type != LFD_DRM) return;
    uint32_t idx = (uint32_t)lfd->backend_fd & 0xFF;
    uint32_t left = __atomic_sub_fetch(&g_refs[idx], 1u, __ATOMIC_SEQ_CST);
    if (left == 0) KldDri::FileClose((uint32_t)lfd->backend_fd);
}

uint32_t Readiness(const LinuxFd* lfd) {
    if (lfd->type != LFD_DRM) return 4;
    uint32_t id = (uint32_t)lfd->backend_fd;
    if (!KldDri::FileAlive(id)) return 4 | 8 | 0x10;     // OUT | ERR | HUP (satoru)
    return 4 | (KldDri::FileEvBytes(id) ? 1u : 0u);
}

// ── rig: unprivileged test processes ─────────────────────────────────────────
// the container cannot fork (it is multithreaded and holds broker mappings), so the
// kernel starts the test children: this same image run as uid 1000 with
// "--dri-child <mode> <handle> <arg>", one kernel process each. (satoru)
struct Child { bool used; uint32_t mode, arg; volatile int state; volatile bool kill_req; int rc; uint32_t pid; uint64_t t0; };
static Child g_child[16];
static uint8_t g_pend[16];
static uint32_t g_pend_head, g_pend_tail;
static Spinlock g_child_lock;

// four persistent runner kernel processes; a kernel process cannot be ended safely by returning,
// so they wait for work forever. (satoru)
static volatile uint32_t g_kill_pid;
void RequestKill(uint32_t pid) { g_kill_pid = pid; }

static void child_runner() {
  for (;;) {
    { uint32_t kp = g_kill_pid; if (kp && __sync_bool_compare_and_swap(&g_kill_pid, kp, 0)) Scheduler::KillProcessGroup(kp); }
    int h = -1;
    { SpinLockGuard g(g_child_lock); if (g_pend_head != g_pend_tail) h = g_pend[g_pend_tail++ % 16]; }
    if (h < 0) { Scheduler::SleepMs(10); continue; }
    if (g_child[h].mode == 11) {      // rig (I1): the shell's own path, `fdtest` = RunProcessWithArgs + DestroyProcess (satoru)
        Child& c = g_child[h];
        static char sob[SHELL_OUTPUT_BUF];
        c.t0 = Timer::GetRealMs64(); c.state = 1;
        sob[0] = 0;
        KuronoShell::Execute("fdtest", sob, (int)sizeof sob);
        c.rc = 0; c.state = 2;
    } else {
        Child& c = g_child[h];
        Process* p = ElfLoader::LoadELF64FromVFS(c.mode == 8 ? "/usr/bin/fdtest" : "/usr/bin/kld-drmcirrus", "dri-child");   // mode 8 (I1): a small embedded program through the same launch path (satoru)
        DetachedHandle dh;
        char hb[8], ab[16];
        static const char* const names[] = { "", "map-hold", "map-exit", "fork", "fuzz", "noop", "sleep", "hammer", "fdtest", "cgloop", "cganon" };
        const char* mode = c.mode < 11 ? names[c.mode] : "map-exit";
        { uint32_t v = (uint32_t)h, n = 0; char t[8]; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } uint32_t k = 0; while (n) hb[k++] = t[--n]; hb[k] = 0; }
        { uint32_t v = c.arg, n = 0; char t[12]; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } uint32_t k = 0; while (n) ab[k++] = t[--n]; ab[k] = 0; }
        const char* av[] = { "kld-drmcirrus", "--dri-child", mode, hb, ab, nullptr };
        const char* envp[] = { "PATH=/usr/bin:/system/bin", nullptr };
        if (p && g_runold) {      // rig (I2): how the first version ran a child (satoru)
            c.pid = p->pid; c.t0 = Timer::GetRealMs64(); c.state = 1;
            c.rc = Userspace::RunProcessWithArgs(p, av, envp, 1000, 1000);
            c.state = 2;
        } else if (!p || Userspace::SpawnDetached(p, av, envp, 1000, 1000, &dh) != 0) {
            SerialLogger::Log("[dri] child spawn failed\r\n");
            c.rc = -1; c.state = 2;
        } else {
            c.pid = p->pid;
            c.t0 = Timer::GetRealMs64();
            c.state = 1;
            if ((c.mode == 9 || c.mode == 10) && g_cgtest) {     // rig (item 9): a cgroup with a memory limit for this child (satoru)
                {   // one cgroup per child: an unreaped leader keeps its address space ledger charged (satoru)
                    char nm[16] = "dricg"; uint32_t v = c.pid, n = 5; char t[10]; int k = 0;
                    if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
                    while (k) nm[n++] = t[--k];
                    nm[n] = 0;
                    g_cg_id = Cgroup::Create(1, nm);
                    if (g_cg_id) { Cgroup::EnableController(g_cg_id, Cgroup::CTRL_MEMORY); Cgroup::SetMemoryMax(g_cg_id, 16ull << 20); }
                }
                if (!g_cg_id || !Cgroup::Attach(g_cg_id, p->pid)) SerialLogger::Log("[dri] cgroup attach failed\r\n");
            }
            bool killed = false;
            while (Userspace::DetachedAlive(dh)) {
                // a kill request waits until the process has run a little: killing one that is
                // still being launched is the scheduler's business, not this interface's. (satoru)
                if (c.kill_req && !killed && Timer::GetRealMs64() - c.t0 >= (g_killnow ? 0u : 150u)) {
                    Scheduler::KillProcessGroup(c.pid);
                    killed = true;
                }
                Scheduler::SleepMs(5);
            }
            if (killed && g_noreap) Scheduler::SleepMs(10000);   // rig: the caller stays dead but unreaped (satoru)
            Userspace::ReapDetached(dh);
            if (!killed && g_reapexit) Scheduler::DestroyProcess(p);   // rig (I1): the zombie leader nobody waits for (satoru)
            c.rc = dh.exit_code;
            c.state = 2;
        }
    }
    if (g_runexit) Scheduler::Exit();   // rig (I2) (satoru)
  }
}

int64_t ChildOp(uint32_t op, uint64_t a0, uint64_t a1) {
    if (op == 0xA7) {
        int h = -1;
        { SpinLockGuard g(g_child_lock);
          for (int i = 1; i < 16; i++) if (!g_child[i].used) { h = i; break; }
          if (h < 0 || g_pend_head - g_pend_tail >= 16) return -12;
          g_child[h] = Child();
          g_child[h].used = true; g_child[h].mode = (uint32_t)a0; g_child[h].arg = (uint32_t)a1; g_child[h].state = 0;
          g_pend[g_pend_head++ % 16] = (uint8_t)h; }
        static bool runners_started;
        if (!runners_started) {
            runners_started = true;
            for (int i = 0; i < 4; i++)
                Scheduler::SpawnKernelProcess("dri-child", child_runner, PRIO_NORMAL, 64, 64 * 1024);
        }
        return h;
    }
    if (a0 == 0 || a0 >= 16 || !g_child[a0].used) return -22;
    Child& c = g_child[a0];
    if (op == 0xA8) {
        if (c.state == 2) return 0;
        if (g_runold) {      // rig (I2): the first version killed from here, after the wait (satoru)
            if (c.state != 1 || !c.pid || Timer::GetRealMs64() - c.t0 < (g_killnow ? 0u : 150u)) return -11;
            Scheduler::KillProcessGroup(c.pid);
            return 0;
        }
        c.kill_req = true;       // the child's own runner does the kill (satoru)
        return 0;
    }
    if (op == 0xA9) return (int64_t)((uint64_t)c.state | ((uint64_t)c.pid << 8) | ((uint64_t)(c.rc & 0xff) << 40));
    if (op == 0xAA) { if (c.state == 1) return -16; SpinLockGuard g(g_child_lock); c.used = false; return 0; }
    return -22;
}

// a thread that was parked in a drm call resumes through its syscall; when that syscall now fails before
// reaching this file (the fd was closed by another thread, or names something else) the parked request
// is abandoned here, so slots never pile up and a reply never lands on the wrong call. (satoru)
void StaleCheck(LinuxProcess* p, int fd) {
    if (!p || !p->task) return;
    int slot = KldDri::FindByTask(p->task);
    if (slot < 0) return;
    bool ok = fd >= 0 && fd < LINUX_MAX_FDS && p->fds[fd].open && p->fds[fd].type == LFD_DRM &&
              (uint32_t)p->fds[fd].backend_fd == KldDri::ReqFile(slot);
    if (!ok) KldDri::Abandon(slot);
}

void TaskGone(Process* task) { KldDri::TaskGone(task); }

void OnAttach(uint32_t minors) {
    KVFS::Mkdirs("/system/dev/dri");
    const char* names[2] = { "/system/dev/dri/card0", "/system/dev/dri/renderD128" };
    for (int i = 0; i < 2; i++) {
        if (!(minors & (1u << i))) continue;
        if (!KVFS::Exists(names[i])) KVFS::CreateFile(names[i], 0666);
        KVFSNode* n = KVFS::Resolve(names[i]);
        if (n) n->type = KVFS_DEVICE;
    }
}

void OnDetach() {
    // the nodes stay: opening them now answers ENODEV, not ENOENT. (satoru)
}

}  // namespace DriDev

// end (satoru)
