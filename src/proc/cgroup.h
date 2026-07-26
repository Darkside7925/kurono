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
// Supported controllers (matching Linux v6 names) - what is REAL today:
//   memory   - memory.max byte cap. enforced at the user-page mapping
//              chokepoint (KernelVMM::MapPage*InAddressSpace charges the
//              current task's cgroup via ChargeUserPages; a failed charge
//              fails the map -> the mmap/demand-zero path sees OOM).
//              frees/teardown uncharge through the per-address-space ledger.
//   cpu      - cpu.max quota/period bandwidth. the scheduler ticks charge a
//              running task's slice against its cgroup pool (CpuAcquireSlice)
//              and stamp Process::cgroup_throttle_until_ms when the pool is
//              dry; the pick loops skip throttled tasks until refill.
//              cpu.weight maps onto the member task's nice value at Attach
//              time (the CFS vruntime weight the user pick already honours) -
//              weight 100 = nice 0, ~1.25x rate per step, linux-style.
//   pids     - pids.max checked at Attach time (attach fails when full).
//   io       - io.weight is stored but NOT consumed by any io path yet.
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

        // cpu.max bandwidth (cfs-bandwidth style): quota_us of runtime per
        // period_us window. quota 0 = "max" (unlimited). the pool drains as
        // scheduler ticks charge running members and refills on period
        // rollover inside CpuAcquireSlice. (satoru)
        uint64_t cpu_quota_us;              // 0 = unlimited (satoru)
        uint64_t cpu_period_us;             // default 100000 (100ms) (satoru)
        uint64_t cpu_pool_us;               // runtime left this period (satoru)
        uint64_t cpu_period_start_ms;       // Timer::GetRealMs64 period stamp (satoru)
        uint64_t cpu_nr_throttled;          // exhaustion events (stat) (satoru)

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

    // Knob setters - called from /sys/fs/cgroup/<x>/<knob> writes.
    bool SetMemoryMax(uint32_t id, uint64_t bytes);
    bool SetCpuWeight(uint32_t id, uint32_t weight);
    bool SetPidsMax(uint32_t id, uint32_t max);
    bool SetIoWeight(uint32_t id, uint32_t weight);
    bool EnableController(uint32_t id, uint32_t ctrl_bits);

    // cpu.max bandwidth: quota_us of runtime per period_us window.
    // quota_us == 0 means "max" (unlimited); period clamped to [1ms, 1s].
    // ParseCpuMax accepts the linux write format: "max [period]" or
    // "<quota> [period]". (satoru)
    bool SetCpuMax(uint32_t id, uint64_t quota_us, uint64_t period_us);
    bool ParseCpuMax(uint32_t id, const char* text);

    // does any node on the chain from cgroup_id to root enforce a quota?
    // lock-free read - the scheduler tick gates on this cheaply. (satoru)
    bool CpuHasQuota(uint32_t cgroup_id);

    // grab up to want_us of runtime from every quota'd node on the chain
    // (refilling pools whose period rolled over). returns the granted slice
    // in microseconds; 0 with *throttle_until_ms_out set to the earliest
    // moment every exhausted pool has refilled means "throttled". now_ms is
    // the Timer::GetRealMs64 clock the pick loops compare against. (satoru)
    uint64_t CpuAcquireSlice(uint32_t cgroup_id, uint64_t want_us,
                             uint64_t now_ms, uint64_t* throttle_until_ms_out);

    // Memory accounting hooks.  Returns false if the request would
    // exceed memory.max (and walks the chain up to root).
    bool MemoryCharge(uint32_t cgroup_id, uint64_t bytes);
    void MemoryUncharge(uint32_t cgroup_id, uint64_t bytes);

    // user-page accounting keyed by address-space root (cr3), so frees and
    // address-space teardown uncharge exactly what was charged even when a
    // different task performs the free. ChargeUserPages returns false when
    // memory.max would be exceeded (the caller must fail the mapping).
    // cgroup_id 0 (no cgroup) is a free pass with no ledger entry. (satoru)
    bool ChargeUserPages(uint64_t as_root, uint32_t cgroup_id, uint64_t bytes);
    void UnchargeUserPages(uint64_t as_root, uint64_t bytes);
    void ReleaseAddressSpace(uint64_t as_root);

    // Lookup helpers.
    Node*    Get(uint32_t id);
    uint32_t FindByPath(const char* path);
    int      Count();

    // Render /sys/fs/cgroup tree into the in-memory KVFS.  Called once
    // after Cgroup::Init so userland can `cat` the knob files.
    void PublishToKVFS();
}

#endif
