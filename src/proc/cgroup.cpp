#include "cgroup.h"
#include "scheduler.h"          // Attach stamps Process::cgroup_id + nice (satoru)
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "spinlock.h"

namespace {
    Cgroup::Node g_nodes[Cgroup::CGROUP_MAX];
    int          g_count = 0;
    uint32_t     g_next_id = 1;
    Spinlock     g_lock;

    // per-address-space charge ledger: how many user-page bytes each address
    // space charged, and against which cgroup, so frees + teardown uncharge
    // exactly what was charged regardless of which task performs them. slots
    // are few (one live user address space per process, threads share). on
    // overflow accounting for the extra space is skipped - honest degradation,
    // never a leak of charges (nothing was charged). protected by g_lock via
    // the ledger entry points. (satoru)
    struct AsLedger {
        uint64_t root;      // cr3 / pml4 phys of the address space (satoru)
        uint32_t cg;        // cgroup the bytes were charged to (satoru)
        uint64_t bytes;     // outstanding charged bytes (satoru)
        bool     in_use;
    };
    const int AS_LEDGER_MAX = 128;
    AsLedger g_as_ledger[AS_LEDGER_MAX];

    // map a cgroup cpu.weight (default 100) onto the scheduler's nice scale
    // (default 0): each ~1.25x multiplicative step of weight away from 100 is
    // one nice step, matching the kNiceW cfs weight table's ratio. clamped to
    // [-20, 19]. (satoru)
    inline int weight_to_nice(uint32_t weight) {
        if (weight < 1) weight = 1;
        if (weight > 10000) weight = 10000;
        int nice = 0;
        uint64_t w = weight;
        while (w * 4 >= 100ull * 5 && nice > -20) { w = (w * 4) / 5; nice--; }
        while (w * 5 <= 100ull * 4 && nice < 19)  { w = (w * 5) / 4; nice++; }
        return nice;
    }

    inline void mzero(void* p, unsigned n) {
        unsigned char* b = (unsigned char*)p;
        for (unsigned i = 0; i < n; i++) b[i] = 0;
    }
    inline void scopy(char* d, const char* s, int max) {
        int i = 0;
        while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
        d[i] = 0;
    }
    inline bool seq(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b && *a == *b) { a++; b++; }
        return *a == 0 && *b == 0;
    }
    inline int slen(const char* s) {
        int n = 0; while (s && s[n]) n++; return n;
    }
    inline void scat(char* d, const char* s, int max) {
        int n = slen(d);
        while (s && *s && n < max - 1) d[n++] = *s++;
        d[n] = 0;
    }
    inline void utoa(uint64_t v, char* out) {
        if (v == 0) { out[0] = '0'; out[1] = 0; return; }
        char tmp[24]; int n = 0;
        while (v && n < 23) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
        out[n] = 0;
    }
}

namespace Cgroup {

void Init() {
    mzero(g_nodes, sizeof(g_nodes));
    mzero(g_as_ledger, sizeof(g_as_ledger));
    g_count = 0;
    g_next_id = 1;

    // Create the root cgroup at /sys/fs/cgroup.
    Node& root = g_nodes[0];
    root.in_use = true;
    root.id     = g_next_id++;
    root.parent = 0;
    scopy(root.name, "/", CGROUP_NAME_MAX);
    scopy(root.path, "/sys/fs/cgroup", CGROUP_PATH_MAX);
    // All controllers available at root by default.
    root.enabled_ctrls = CTRL_MEMORY | CTRL_CPU | CTRL_PIDS | CTRL_IO;
    root.cpu_weight = 100;
    root.io_weight  = 100;
    root.cpu_period_us = 100000;   // 100ms default period, quota "max" (satoru)
    g_count = 1;

    SerialLogger::Log("Cgroup: v2 unified hierarchy initialized (root=/sys/fs/cgroup)\r\n");
}

int Count() { return g_count; }

Node* Get(uint32_t id) {
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_nodes[i].in_use && g_nodes[i].id == id) return &g_nodes[i];
    }
    return nullptr;
}

uint32_t FindByPath(const char* path) {
    if (!path) return 0;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_nodes[i].in_use && seq(g_nodes[i].path, path)) return g_nodes[i].id;
    }
    return 0;
}

uint32_t Create(uint32_t parent_id, const char* name) {
    if (g_count >= CGROUP_MAX || !name || !name[0]) return 0;
    Node* parent = Get(parent_id);
    if (!parent) return 0;

    int slot = -1;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_nodes[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return 0;

    Node& n = g_nodes[slot];
    mzero(&n, sizeof(n));
    n.in_use = true;
    n.id     = g_next_id++;
    n.parent = parent_id;
    scopy(n.name, name, CGROUP_NAME_MAX);
    scopy(n.path, parent->path, CGROUP_PATH_MAX);
    scat(n.path, "/", CGROUP_PATH_MAX);
    scat(n.path, name, CGROUP_PATH_MAX);
    n.enabled_ctrls = parent->enabled_ctrls;
    n.cpu_weight    = 100;
    n.io_weight     = 100;
    n.cpu_period_us = 100000;   // default 100ms period, quota unlimited (satoru)
    g_count++;

    // Reflect in KVFS so userspace can ls /sys/fs/cgroup.
    KVFS::Mkdirs(n.path);
    char p[160];
    scopy(p, n.path, sizeof(p)); scat(p, "/cgroup.procs", sizeof(p));
    KVFS::WriteString(p, "");
    scopy(p, n.path, sizeof(p)); scat(p, "/memory.max", sizeof(p));
    KVFS::WriteString(p, "max\n");
    scopy(p, n.path, sizeof(p)); scat(p, "/memory.current", sizeof(p));
    KVFS::WriteString(p, "0\n");
    scopy(p, n.path, sizeof(p)); scat(p, "/cpu.weight", sizeof(p));
    KVFS::WriteString(p, "100\n");
    scopy(p, n.path, sizeof(p)); scat(p, "/cpu.max", sizeof(p));
    KVFS::WriteString(p, "max 100000\n");
    scopy(p, n.path, sizeof(p)); scat(p, "/pids.max", sizeof(p));
    KVFS::WriteString(p, "max\n");
    scopy(p, n.path, sizeof(p)); scat(p, "/pids.current", sizeof(p));
    KVFS::WriteString(p, "0\n");
    scopy(p, n.path, sizeof(p)); scat(p, "/io.weight", sizeof(p));
    KVFS::WriteString(p, "default 100\n");

    return n.id;
}

bool Destroy(uint32_t id) {
    SpinLockGuard guard(g_lock);
    Node* n = nullptr;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_nodes[i].in_use && g_nodes[i].id == id) { n = &g_nodes[i]; break; }
    }
    if (!n || n->id == 1) return false;        // refuse root
    if (n->pids_current > 0) return false;     // EBUSY
    // Refuse if any in-use child still points at us.
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_nodes[i].in_use && g_nodes[i].parent == id) return false;
    }
    n->in_use = false;
    g_count--;
    return true;
}

bool Attach(uint32_t cgroup_id, uint32_t pid) {
    SpinLockGuard guard(g_lock);
    Node* target = nullptr;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_nodes[i].in_use && g_nodes[i].id == cgroup_id) { target = &g_nodes[i]; break; }
    }
    if (!target) return false;
    if (target->pids_max && target->pids_current >= target->pids_max) return false;
    target->pids_current++;

    // membership was previously never recorded on the task - Attach bumped a
    // counter and threw the pid away, so no enforcement path could ever find
    // the cgroup. stamp Process::cgroup_id (the scheduler tick + the memory
    // charge chokepoint key off it), release the old cgroup's pid slot, and
    // translate cpu.weight into the task's cfs nice weight so the weight knob
    // biases the user pick for real. (satoru)
    Process* p = Scheduler::FindProcessByPid(pid);
    if (p) {
        if (p->cgroup_id && p->cgroup_id != cgroup_id) {
            for (int i = 0; i < CGROUP_MAX; i++) {
                if (g_nodes[i].in_use && g_nodes[i].id == p->cgroup_id) {
                    if (g_nodes[i].pids_current > 0) g_nodes[i].pids_current--;
                    break;
                }
            }
        }
        p->cgroup_id = cgroup_id;
        if ((target->enabled_ctrls & CTRL_CPU) && target->cpu_weight != 100) {
            p->nice = weight_to_nice(target->cpu_weight);
        }
        // fresh membership starts with an empty local slice; the first tick
        // acquires from the pool. (satoru)
        p->cgroup_quota_left_us = 0;
        p->cgroup_throttle_until_ms = 0;
    }
    return true;
}

bool SetMemoryMax(uint32_t id, uint64_t bytes) {
    Node* n = Get(id);
    if (!n) return false;
    n->mem_max = bytes;
    char buf[24]; utoa(bytes, buf);
    char p[160]; scopy(p, n->path, sizeof(p)); scat(p, "/memory.max", sizeof(p));
    KVFS::WriteString(p, buf);
    return true;
}

bool SetCpuWeight(uint32_t id, uint32_t weight) {
    if (weight < 1) weight = 1;
    if (weight > 10000) weight = 10000;
    Node* n = Get(id);
    if (!n) return false;
    n->cpu_weight = weight;
    char buf[12]; utoa(weight, buf);
    char p[160]; scopy(p, n->path, sizeof(p)); scat(p, "/cpu.weight", sizeof(p));
    KVFS::WriteString(p, buf);
    return true;
}

bool SetCpuMax(uint32_t id, uint64_t quota_us, uint64_t period_us) {
    Node* n = Get(id);
    if (!n) return false;
    if (period_us < 1000)    period_us = 1000;      // 1ms floor (satoru)
    if (period_us > 1000000) period_us = 1000000;   // 1s ceiling (satoru)
    {
        SpinLockGuard guard(g_lock);
        n->cpu_quota_us  = quota_us;
        n->cpu_period_us = period_us;
        n->cpu_pool_us   = quota_us;   // fresh pool; period restarts on first charge (satoru)
        n->cpu_period_start_ms = 0;
    }
    // mirror into the knob file so `cat cpu.max` shows the live value (satoru)
    char text[48]; text[0] = 0;
    if (quota_us == 0) {
        scat(text, "max ", sizeof(text));
    } else {
        char q[24]; utoa(quota_us, q);
        scat(text, q, sizeof(text));
        scat(text, " ", sizeof(text));
    }
    char per[24]; utoa(period_us, per);
    scat(text, per, sizeof(text));
    scat(text, "\n", sizeof(text));
    char p[160]; scopy(p, n->path, sizeof(p)); scat(p, "/cpu.max", sizeof(p));
    KVFS::WriteString(p, text);
    return true;
}

bool ParseCpuMax(uint32_t id, const char* text) {
    if (!text) return false;
    // linux format: "max [period]" or "<quota_us> [period_us]" (satoru)
    int i = 0;
    while (text[i] == ' ' || text[i] == '\t') i++;
    uint64_t quota = 0;
    if (text[i] == 'm' && text[i + 1] == 'a' && text[i + 2] == 'x') {
        i += 3;
    } else {
        bool any = false;
        while (text[i] >= '0' && text[i] <= '9') {
            quota = quota * 10 + (uint64_t)(text[i] - '0');
            i++; any = true;
        }
        if (!any) return false;
        if (quota == 0) quota = 1;   // an explicit 0 quota still throttles (satoru)
    }
    while (text[i] == ' ' || text[i] == '\t') i++;
    uint64_t period = 100000;
    if (text[i] >= '0' && text[i] <= '9') {
        period = 0;
        while (text[i] >= '0' && text[i] <= '9') {
            period = period * 10 + (uint64_t)(text[i] - '0');
            i++;
        }
    }
    return SetCpuMax(id, quota, period);
}

bool CpuHasQuota(uint32_t cgroup_id) {
    // lock-free chain read: single-word loads, staleness bounded by the next
    // tick. the common case (cgroup_id 0 / no quota anywhere) is a handful of
    // loads. (satoru)
    for (Node* cur = Get(cgroup_id); cur; cur = Get(cur->parent)) {
        if (cur->cpu_quota_us) return true;
    }
    return false;
}

uint64_t CpuAcquireSlice(uint32_t cgroup_id, uint64_t want_us,
                         uint64_t now_ms, uint64_t* throttle_until_ms_out) {
    if (throttle_until_ms_out) *throttle_until_ms_out = 0;
    if (want_us == 0) return 0;
    SpinLockGuard guard(g_lock);
    Node* n = Get(cgroup_id);
    if (!n) return want_us;   // unknown cgroup: never throttle (satoru)

    // pass 1: refill rolled-over periods, then find the tightest grant the
    // whole ancestor chain can honour. an exhausted node forces grant 0 and
    // contributes its period end; the caller must wait for the LATEST of
    // those (every exhausted ancestor has to refill). (satoru)
    uint64_t grant = want_us;
    uint64_t wait_until = 0;
    for (Node* cur = n; cur; cur = Get(cur->parent)) {
        if (!cur->cpu_quota_us) continue;
        uint64_t period_ms = cur->cpu_period_us / 1000;
        if (!period_ms) period_ms = 1;
        if (cur->cpu_period_start_ms == 0 ||
            now_ms >= cur->cpu_period_start_ms + period_ms) {
            cur->cpu_period_start_ms = now_ms;
            cur->cpu_pool_us = cur->cpu_quota_us;
        }
        if (cur->cpu_pool_us == 0) {
            uint64_t until = cur->cpu_period_start_ms + period_ms;
            if (until > wait_until) wait_until = until;
            grant = 0;
        } else if (cur->cpu_pool_us < grant) {
            grant = cur->cpu_pool_us;
        }
    }
    if (grant == 0) {
        n->cpu_nr_throttled++;
        if (throttle_until_ms_out) *throttle_until_ms_out = wait_until;
        return 0;
    }
    // pass 2: commit the grant against every quota'd pool on the chain (satoru)
    for (Node* cur = n; cur; cur = Get(cur->parent)) {
        if (!cur->cpu_quota_us) continue;
        cur->cpu_pool_us = (cur->cpu_pool_us >= grant) ? (cur->cpu_pool_us - grant) : 0;
    }
    return grant;
}

bool SetPidsMax(uint32_t id, uint32_t max) {
    Node* n = Get(id);
    if (!n) return false;
    n->pids_max = max;
    char buf[12]; utoa(max, buf);
    char p[160]; scopy(p, n->path, sizeof(p)); scat(p, "/pids.max", sizeof(p));
    KVFS::WriteString(p, buf);
    return true;
}

bool SetIoWeight(uint32_t id, uint32_t weight) {
    if (weight < 1) weight = 1;
    if (weight > 10000) weight = 10000;
    Node* n = Get(id);
    if (!n) return false;
    n->io_weight = weight;
    return true;
}

bool EnableController(uint32_t id, uint32_t ctrl_bits) {
    Node* n = Get(id);
    if (!n) return false;
    n->enabled_ctrls |= ctrl_bits;
    return true;
}

// lock-free bodies shared by the public entry points (which take g_lock) and
// the address-space ledger (which already holds it). taking g_lock twice on
// the non-recursive spinlock would self-deadlock. (satoru)
static bool memory_charge_nolock(uint32_t cgroup_id, uint64_t bytes) {
    Node* n = Get(cgroup_id);
    // Pre-check entire ancestor chain so a partial charge is impossible.
    for (Node* cur = n; cur; cur = Get(cur->parent)) {
        if (cur->mem_max && (cur->mem_current + bytes) > cur->mem_max) {
            cur->mem_oom_count++;
            return false;
        }
    }
    for (Node* cur = n; cur; cur = Get(cur->parent)) {
        cur->mem_current += bytes;
        if (cur->mem_current > cur->mem_high_water) cur->mem_high_water = cur->mem_current;
    }
    return true;
}

static void memory_uncharge_nolock(uint32_t cgroup_id, uint64_t bytes) {
    Node* n = Get(cgroup_id);
    while (n) {
        if (n->mem_current >= bytes) n->mem_current -= bytes;
        else                         n->mem_current = 0;
        n = Get(n->parent);
    }
}

bool MemoryCharge(uint32_t cgroup_id, uint64_t bytes) {
    SpinLockGuard guard(g_lock);
    return memory_charge_nolock(cgroup_id, bytes);
}

void MemoryUncharge(uint32_t cgroup_id, uint64_t bytes) {
    SpinLockGuard guard(g_lock);
    memory_uncharge_nolock(cgroup_id, bytes);
}

// ---- per-address-space charge ledger (satoru) -------------------------------
static AsLedger* ledger_find_nolock(uint64_t root) {
    for (int i = 0; i < AS_LEDGER_MAX; i++) {
        if (g_as_ledger[i].in_use && g_as_ledger[i].root == root)
            return &g_as_ledger[i];
    }
    return nullptr;
}

bool ChargeUserPages(uint64_t as_root, uint32_t cgroup_id, uint64_t bytes) {
    if (!cgroup_id || !as_root || !bytes) return true;   // no cgroup: free pass (satoru)
    SpinLockGuard guard(g_lock);
    AsLedger* e = ledger_find_nolock(as_root);
    if (!e) {
        for (int i = 0; i < AS_LEDGER_MAX; i++) {
            if (!g_as_ledger[i].in_use) {
                e = &g_as_ledger[i];
                e->in_use = true;
                e->root = as_root;
                e->cg = cgroup_id;
                e->bytes = 0;
                break;
            }
        }
        // ledger full: skip accounting rather than fail user allocations -
        // nothing is charged, so nothing will leak on teardown. (satoru)
        if (!e) return true;
    }
    // first charger owns the ledger's cgroup binding; a page mapped by a task
    // from another cgroup into this space still charges the owner. (satoru)
    if (!memory_charge_nolock(e->cg, bytes)) return false;
    e->bytes += bytes;
    return true;
}

void UnchargeUserPages(uint64_t as_root, uint64_t bytes) {
    if (!as_root || !bytes) return;
    SpinLockGuard guard(g_lock);
    AsLedger* e = ledger_find_nolock(as_root);
    if (!e) return;
    uint64_t n = bytes < e->bytes ? bytes : e->bytes;
    if (n) memory_uncharge_nolock(e->cg, n);
    e->bytes -= n;
}

void ReleaseAddressSpace(uint64_t as_root) {
    if (!as_root) return;
    SpinLockGuard guard(g_lock);
    AsLedger* e = ledger_find_nolock(as_root);
    if (!e) return;
    if (e->bytes) memory_uncharge_nolock(e->cg, e->bytes);
    e->bytes = 0;
    e->in_use = false;
}
// ---- end ledger (satoru) ----------------------------------------------------

void PublishToKVFS() {
    KVFS::Mkdirs("/sys/fs/cgroup");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.controllers",
                      "cpu io memory pids\n");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.subtree_control", "");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.procs", "");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.max.depth", "max\n");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.max.descendants", "max\n");
    KVFS::WriteString("/sys/fs/cgroup/cpu.max", "max 100000\n");

    // Pre-create a few useful subgroups so userspace tooling can find them.
    Create(1, "system.slice");
    Create(1, "user.slice");
    Create(1, "init.scope");
}

}  // namespace Cgroup
