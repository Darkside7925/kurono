#ifndef KURONO_PROC_CGROUP_H
#define KURONO_PROC_CGROUP_H

#include "../kernel/types.h"

// Linux cgroups v2 unified hierarchy.
//
// All cgroups live in a single tree rooted at /sys/fs/cgroup.  Each node
// has a stable numeric id (CgroupId) and tracks the controllers that
// have been enabled on it.  Processes carry a cgroup_id field
// (Process::cgroup_id) pointing at their leaf node.
//
// Supported controllers (matching Linux v6 names):
//   memory    -  sets memory.max byte cap, enforced cooperatively in
//              KernelHeap::Alloc when MEMCG_CHARGE is wrapped around it.
//   cpu       -  sets cpu.weight 1..10000, hooked into the scheduler's
//              vruntime accounting (lower weight ⇒ slower vruntime
//              accumulation, like nice).
//   pids      -  sets pids.max, enforced in Scheduler::CreateUserProcess.
//   io        -  sets io.weight, used by the block layer's elevator.
//
// We keep up to CGROUP_MAX nodes in a flat table; lookups are linear but
// the count is small (containers typically use <50 cgroups).

namespace Cgroup {

    static const int CGROUP_MAX        = 64;
    static const int CGROUP_NAME_MAX   = 32;
    static const int CGROUP_PATH_MAX   = 128;

    enum Controller : uint32_t {
        CTRL_MEMORY = 1u << 0,
        CTRL_CPU    = 1u << 1,
        CTRL_PIDS   = 1u << 2,
        CTRL_IO     = 1u << 3,
        CTRL_RDMA   = 1u << 4,
        CTRL_HUGETLB= 1u << 5,
    };

    struct Node {
        bool     in_use;
        uint32_t id;
        uint32_t parent;
        char     name[CGROUP_NAME_MAX];
        char     path[CGROUP_PATH_MAX];     // "/sys/fs/cgroup/<rel>"
        uint32_t enabled_ctrls;             // bitmap of Controller

        // memory controller
        uint64_t mem_max;                   // bytes; 0 = unlimited
        uint64_t mem_current;
        uint64_t mem_high_water;
        uint64_t mem_oom_count;

        // cpu controller
        uint32_t cpu_weight;                // 1..10000, default 100
        uint64_t cpu_usage_ns;

        // pids controller
        uint32_t pids_max;                  // 0 = unlimited
        uint32_t pids_current;

        // io controller
        uint32_t io_weight;                 // 1..10000, default 100
    };

    void Init();

    // Hierarchy management.  Returns id (>=1) or 0 on failure.  Root is id 1.
    uint32_t Create(uint32_t parent_id, const char* name);
    bool     Destroy(uint32_t id);

    // Move a process into a cgroup.  Updates pids_current of old/new.
    bool     Attach(uint32_t cgroup_id, uint32_t pid);

    // Knob setters  -  called from /sys/fs/cgroup/<x>/<knob> writes.
    bool SetMemoryMax(uint32_t id, uint64_t bytes);
    bool SetCpuWeight(uint32_t id, uint32_t weight);
    bool SetPidsMax(uint32_t id, uint32_t max);
    bool SetIoWeight(uint32_t id, uint32_t weight);
    bool EnableController(uint32_t id, uint32_t ctrl_bits);

    // Memory accounting hooks.  Returns false if the request would
    // exceed memory.max (and walks the chain up to root).
    bool MemoryCharge(uint32_t cgroup_id, uint64_t bytes);
    void MemoryUncharge(uint32_t cgroup_id, uint64_t bytes);

    // Lookup helpers.
    Node*    Get(uint32_t id);
    uint32_t FindByPath(const char* path);
    int      Count();

    // Render /sys/fs/cgroup tree into the in-memory KVFS.  Called once
    // after Cgroup::Init so userland can `cat` the knob files.
    void PublishToKVFS();
}

#endif
