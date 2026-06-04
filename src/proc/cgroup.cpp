#include "cgroup.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "spinlock.h"

namespace {
    Cgroup::Node g_nodes[Cgroup::CGROUP_MAX];
    int          g_count = 0;
    uint32_t     g_next_id = 1;
    Spinlock     g_lock;

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
    (void)pid;
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

bool MemoryCharge(uint32_t cgroup_id, uint64_t bytes) {
    SpinLockGuard guard(g_lock);
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

void MemoryUncharge(uint32_t cgroup_id, uint64_t bytes) {
    SpinLockGuard guard(g_lock);
    Node* n = Get(cgroup_id);
    while (n) {
        if (n->mem_current >= bytes) n->mem_current -= bytes;
        else                         n->mem_current = 0;
        n = Get(n->parent);
    }
}

void PublishToKVFS() {
    KVFS::Mkdirs("/sys/fs/cgroup");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.controllers",
                      "cpu io memory pids\n");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.subtree_control", "");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.procs", "");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.max.depth", "max\n");
    KVFS::WriteString("/sys/fs/cgroup/cgroup.max.descendants", "max\n");

    // Pre-create a few useful subgroups so userspace tooling can find them.
    Create(1, "system.slice");
    Create(1, "user.slice");
    Create(1, "init.scope");
}

}  // namespace Cgroup
