#include "memboot.h"
#include "pmm.h"
#include "heap.h"
#include "buddy.h"
#include "slab.h"
#include "kmemx.h"
#include "kmemx_pool.h"
#include "kdf.h"
#include "vmm.h"
#include "memacct.h"
#include "../fs/kfs.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../proc/kernel_processes.h"
#include "../proc/smp.h"
#include "../proc/scheduler.h"
#include "../linux/linux_syscall.h"
#include "../virt/vm_host.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"

// see memboot.h. everything here is a read-only snapshot for the boot ram
// census: no allocation, no struct Process field, and every number comes from
// the owning subsystem's own accounting. (satoru)
// linker symbols at global scope: gcc 15 gives an extern "C" name declared
// inside an unnamed namespace internal linkage, and the link then fails with
// "undefined reference to MemBoot::(anonymous namespace)::kernel_end" (the
// alpine guest's gcc 15.2; debian's 14.2 accepted it). (satoru)
extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;

namespace MemBoot {
namespace {


constexpr int FILE_CAP = 20;
struct BigFile { uint64_t bytes; char path[192]; };
BigFile g_big[FILE_CAP];
int     g_big_n = 0;
uint32_t g_node_count = 0;

struct Sink {
    char* b;
    int   mx;
    int   p;
    bool  ser;
    void put(const char* s) {
        if (ser) SerialLogger::Log(s);
        if (!b) return;
        while (*s && p < mx - 1) b[p++] = *s++;
        b[p] = 0;
    }
    void num(uint64_t v) {
        raw(v);
    }
    void raw(uint64_t v) {
        char t[24]; int n = 0;
        if (v == 0) t[n++] = '0';
        while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
        for (int i = n - 1; i >= 0; i--) {
            char c[2] = { t[i], 0 };
            put(c);
        }
    }
    void line(const char* key, uint64_t kb) {
        put("[memboot]   ");
        put(key);
        int pad = 28;
        for (const char* q = key; *q; q++) pad--;
        if (pad < 1) pad = 1;
        for (int i = 0; i < pad; i++) put(" ");
        raw(kb);
        put(" kB\n");
    }
    void title(const char* t) { put(t); put("\n"); }
};

void path_of(const KVFSNode* n, char* out, int mx) {
    char tmp[KVFS_MAX_PATH];
    int p = 0;
    int guard = 0;
    while (n && n->parent && guard++ < 32) {
        int nl = 0;
        while (n->name[nl] && nl < KVFS_MAX_NAME) nl++;
        if (p + nl + 1 >= (int)sizeof(tmp)) break;
        for (int i = nl - 1; i >= 0; i--) tmp[p++] = n->name[i];
        tmp[p++] = '/';
        n = n->parent;
    }
    int q = 0;
    if (p == 0) { if (mx > 1) { out[0] = '/'; out[1] = 0; } return; }
    while (p > 0 && q < mx - 1) out[q++] = tmp[--p];
    out[q] = 0;
}

void consider_file(const KVFSNode* n) {
    if (g_big_n < FILE_CAP) {
        g_big[g_big_n].bytes = n->size;
        path_of(n, g_big[g_big_n].path, (int)sizeof(g_big[0].path));
        g_big_n++;
        return;
    }
    int worst = 0;
    for (int i = 1; i < FILE_CAP; i++) if (g_big[i].bytes < g_big[worst].bytes) worst = i;
    if (n->size <= g_big[worst].bytes) return;
    g_big[worst].bytes = n->size;
    path_of(n, g_big[worst].path, (int)sizeof(g_big[0].path));
}

// resident bytes in one subtree: a file's owned heap content capacity (a
// borrowed/static blob lives in the kernel image or in a kasset pmm block and
// is counted elsewhere). (satoru)
uint64_t tree_bytes(const KVFSNode* n, int depth) {
    if (!n || depth > 40) return 0;
    g_node_count++;
    uint64_t b = 0;
    if (n->type == KVFS_FILE) {
        if (n->content && n->content_capacity) b += n->content_capacity;
        consider_file(n);
    }
    for (int i = 0; i < n->child_count; i++)
        b += tree_bytes(n->children[i], depth + 1);
    return b;
}

void kvfs_report(Sink& s) {
    KVFSNode* root = KVFS::GetRoot();
    if (!root) return;
    g_node_count = 0; g_big_n = 0;
    const int TOP = 12;
    uint64_t bytes[TOP]; char names[TOP][KVFS_MAX_NAME]; int used = 0;
    uint64_t total = 0;
    for (int i = 0; i < root->child_count; i++) {
        KVFSNode* c = root->children[i];
        if (!c) continue;
        uint64_t b = tree_bytes(c, 1);
        total += b;
        int slot = -1;
        for (int k = 0; k < used; k++) if (b > bytes[k]) { slot = k; break; }
        if (slot < 0 && used < TOP) slot = used++;
        if (slot >= 0 && slot < TOP) {
            for (int k = used - 1; k > slot; k--) {
                bytes[k] = bytes[k - 1];
                for (int j = 0; j < KVFS_MAX_NAME; j++) names[k][j] = names[k - 1][j];
            }
            bytes[slot] = b;
            for (int j = 0; j < KVFS_MAX_NAME - 1 && c->name[j]; j++) names[slot][j] = c->name[j];
            int j = 0; while (c->name[j] && j < KVFS_MAX_NAME - 1) j++;
            names[slot][j] = 0;
        }
    }
    s.put("[memboot] kvfs: files_ram_kb=");
    s.raw(total >> 10);
    s.put(" nodes=");
    s.raw(g_node_count);
    s.put("\n");
    for (int k = 0; k < used && k < 8; k++) {
        s.put("[memboot]   /");
        s.put(names[k]);
        s.put(" ");
        s.raw(bytes[k] >> 10);
        s.put(" kB\n");
    }
    for (int k = 0; k < g_big_n; k++) {
        int best = -1;
        for (int i = 0; i < g_big_n; i++) {
            if (g_big[i].bytes == ~0ULL) continue;
            if (best < 0 || g_big[i].bytes > g_big[best].bytes) best = i;
        }
        if (best < 0) break;
        s.put("[memboot]   big ");
        s.raw(g_big[best].bytes >> 10);
        s.put(" kB ");
        s.put(g_big[best].path);
        s.put("\n");
        g_big[best].bytes = ~0ULL;
    }
}

void user_report(Sink& s, uint64_t* pss_kb, uint64_t* pt_kb, uint64_t* kstack_kb,
                 uint32_t* tasks, uint32_t* user_tasks) {
    static SchedulerProcessSnapshot ss[512];
    int n = Scheduler::GetProcessSnapshot(ss, 512);
    uint64_t ks = 0;
    *tasks = 0; *user_tasks = 0;
    for (int i = 0; i < n; i++) {
        if (ss[i].state == Process_Terminated) continue;
        (*tasks)++;
        if (!ss[i].is_kernel_proc) (*user_tasks)++;
        ks += ss[i].stack_kb;
    }
    // the linux records cover tasks blocked on an ap or waiting in a syscall,
    // which the ready-queue snapshot does not list. (satoru)
    uint64_t pss = 0, pt = 0;
    uint64_t roots[128]; int nroots = 0; int procs = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* lp = LinuxSyscall::GetProcess(i);
        Process* p = lp ? lp->task : nullptr;
        if (!p || p->is_thread()) continue;
        procs++;
        uint64_t root = p->address_space;
        if (!root) continue;
        bool seen = false;
        for (int r = 0; r < nroots; r++) if (roots[r] == root) { seen = true; break; }
        if (seen || nroots >= 128) continue;
        roots[nroots++] = root;
        MemAcct::SpaceMem m;
        if (MemAcct::QuerySpace(root, 3000, &m, true)) {
            pss += m.pss_kb;
            pt += m.pt_kb;
        }
    }
    *pss_kb = pss; *pt_kb = pt; *kstack_kb = ks;
    if (procs > *user_tasks) *user_tasks = (uint32_t)procs;
    s.line("user pss (unique)", pss);
    s.line("user page tables", pt);
    s.line("task kernel stacks", ks);
    s.put("[memboot]   tasks=");
    s.raw(*tasks);
    s.put(" user=");
    s.raw(*user_tasks);
    s.put(" spaces=");
    s.raw((uint64_t)nroots);
    s.put(" linux_procs=");
    s.raw((uint64_t)procs);
    s.put("\n");
}

}  // namespace

int Report(char* out, int max, bool to_serial) {
    Sink s; s.b = out; s.mx = max; s.p = 0; s.ser = to_serial;
    if (out && max > 0) out[0] = 0;

    const uint64_t frames = PMM::GetTotalFrames();
    const uint64_t used_f  = PMM::GetUsedFrames();
    const uint64_t free_f  = PMM::GetFreeFrames();
    const uint64_t used    = used_f * PAGE_SIZE;
    const uint64_t meta    = ((frames + 63) / 64) * 8 + frames * 2 + (frames + 7) / 8 +
                             (PMM::CensusOn() ? frames * 4 : 0);
    const uint64_t kimg    = ((uint64_t)&kernel_end > (uint64_t)&kernel_start)
                             ? (((uint64_t)&kernel_end - (uint64_t)&kernel_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) : 0;
    const uint64_t heap_r  = (uint64_t)KernelHeap::GetTotal();
    const uint64_t heap_u  = (uint64_t)KernelHeap::GetUsed();
    const uint64_t buddy_r = Buddy::GetPoolBytes();
    const uint64_t buddy_u = Buddy::GetUsedBytes();
    const uint64_t kmeta   = KMemX::IsInitialized() ? (uint64_t)KMemX::GetStats().pool_bytes : 0;
    const uint64_t kpool   = KMemXPool::TotalBytes();
    const uint64_t kfsb    = KFS::MetadataBytes();
    const uint64_t low1m   = 1024 * 1024;
    const uint64_t imagew  = PMM::GetImageWindowBytes();
    const uint64_t overlay = PMM::GetOverlayBytes();
    const uint64_t fb      = PMM::GetReservedFramebufferBytes();
    const uint64_t apstk   = SMP::KernelStackBytes();
    const uint64_t dma     = KDF::DmaBytes();

    uint64_t pss = 0, pt = 0, kstk = 0;
    uint32_t tasks = 0, user_tasks = 0;

    VMHost::Info vi; VMHost::GetInfo(&vi);
    uint64_t guest = (vi.state == VMHost::VMH_OFF) ? 0 : (uint64_t)vi.ram_mb * 1024 * 1024;

    s.put("[memboot] up_s=");
    s.raw(Scheduler::NowMs() / 1000);
    s.put(" total_mb=");
    s.raw((frames * PAGE_SIZE) >> 20);
    s.put(" used_mb=");
    s.raw(used >> 20);
    s.put(" free_mb=");
    s.raw((free_f * PAGE_SIZE) >> 20);
    s.put("\n");

    user_report(s, &pss, &pt, &kstk, &tasks, &user_tasks);

    uint64_t known = 0;
    s.line("kernel image", kimg >> 10); known += kimg;
    s.line("pmm metadata", meta >> 10); known += meta;
    s.line("low 1 mb", low1m >> 10); known += low1m;
    s.line("user image window", imagew >> 10); known += imagew;
    s.line("overlay windows", overlay >> 10); known += overlay;
    s.line("framebuffer reserved", fb >> 10); known += fb;
    s.line("kernel heap reserved", heap_r >> 10); known += heap_r;
    s.put("[memboot]   (heap used ");
    s.raw(heap_u >> 10);
    s.put(" kB)\n");
    s.line("buddy pool reserved", buddy_r >> 10); known += buddy_r;
    s.put("[memboot]   (buddy used ");
    s.raw(buddy_u >> 10);
    s.put(" kB, slab in use ");
    s.raw(Slab::InUseBytes() >> 10);
    s.put(" kB)\n");
    s.line("kmemx", (kmeta + kpool) >> 10); known += kmeta + kpool;
    s.line("kfs metadata", kfsb >> 10); known += kfsb;
    s.line("ap kernel stacks", apstk >> 10); known += apstk;
    known += kstk * 1024;
    known += pss * 1024;
    known += pt * 1024;
    s.line("kdf dma", dma >> 10); known += dma;
    s.line("guest vm", guest >> 10); known += guest;

    s.put("[memboot] accounted_kb=");
    s.raw(known >> 10);
    s.put("\n");
    if (used >= known) {
        s.put("[memboot] unaccounted_kb=");
        s.raw((used - known) >> 10);
        s.put("\n");
    } else {
        s.put("[memboot] unaccounted_kb=-");
        s.raw((known - used) >> 10);
        s.put("\n");
    }
    kvfs_report(s);
    return s.p;
}

// the full report on the desktop-idle tick is a diagnostic: printed through the
// uart busy-wait inside the gui's kls section it held kls 95-200 ms once per
// boot, and every syscall and kernel proc waited behind it. off by default now;
// kurono.memboot=1 brings it back (the shell `memboot` command still prints it
// on demand). the kfs cache release below always runs. (satoru)
static bool g_boot_report = false;
void SetBootReport(bool on) { g_boot_report = on; }

void OnDesktopIdle() {
    static bool done = false;
    if (done) return;
    done = true;
    if (g_boot_report) Report(nullptr, 0, true);
    // the boot restore leaves the KFS bitmap+inode caches (tens of mb on a
    // large volume) mounted for the whole session. drop them once the desktop
    // is idle; any later persist op remounts on demand. skip when an app
    // autorun is going to read /apps, or a restore is already walking it. (satoru)
    const char* ac = KernelProcesses::GuiAutorun();
    bool appish = false;
    if (ac) {
        for (const char* p = ac; *p; p++) {
            if ((p[0] == 'f' || p[0] == 'F') && p[1] == 'i' && p[2] == 'r' && p[3] == 'e') { appish = true; break; }
            if ((p[0] == 'c' || p[0] == 'C') && p[1] == 'l' && p[2] == 'a' && p[3] == 'u') { appish = true; break; }
        }
    }
    if (!appish) {
        static SchedulerProcessSnapshot ss[512];
        int n = Scheduler::GetProcessSnapshot(ss, 512);
        for (int i = 0; i < n && !appish; i++) {
            const char* nm = ss[i].name;
            for (const char* p = nm; *p; p++) {
                if ((p[0] == 'f' || p[0] == 'F') && p[1] == 'i' && p[2] == 'r' && p[3] == 'e') { appish = true; break; }
                if ((p[0] == 'c' || p[0] == 'C') && p[1] == 'l' && p[2] == 'a' && p[3] == 'u') { appish = true; break; }
            }
        }
    }
    if (!appish && !PersistStore::IsRestoring() && KFS::IsMounted()) {
        KFS::Unmount();
        SerialLogger::Log("[memboot] kfs metadata caches released\r\n");
    }
}

}  // namespace MemBoot

// end (satoru)
