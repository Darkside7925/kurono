// lx_emul: DATA symbols.
//
// These are the dangerous ones. An auto-generated stub is a zero-arg FUNCTION,
// which satisfies the linker for a data reference but makes the driver read the
// stub's code bytes as if they were the value. That is silent corruption, not a
// loud abort - so every data symbol the census turns up must land here as real
// storage before anything else is trusted.
//
// Compiled in KERNEL context, so the types below are checked against the real
// upstream declarations. A mismatch is a build error rather than a mystery.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/hex.h>
#include <net/net_namespace.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/cpumask.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <asm/processor.h>
#include <asm/current.h>
#include <linux/jump_label.h>
#include <linux/sched.h>
#include <linux/suspend.h>

// ── time base ───────────────────────────────────────────────────────────────
// the real jiffies counter. lx_time.c advances it from the monotonic clock;
// declaring it here keeps all shim DATA in one place. (satoru)
unsigned long volatile __jiffy_arch_data jiffies = INITIAL_JIFFIES;

// ── module identity ─────────────────────────────────────────────────────────
// THIS_MODULE resolves to &__this_module. Drivers pass it around as an owner
// token and read .name; they must never get a code-bytes-as-struct read. (satoru)
struct module __this_module = {
    .name  = KBUILD_MODNAME,
    .state = MODULE_STATE_LIVE,
};

// ── x86 address-space bases ─────────────────────────────────────────────────
// used by virt_to_phys/page_to_virt style macros the driver expands inline.
// A container has no kernel direct map, so the identity values below make those
// macros arithmetic-neutral. They are NOT physically meaningful - any code path
// that actually depends on them is a DMA path, and DMA must go through the
// broker rather than these. Kept explicit so that is visible. (satoru)
unsigned long phys_base        = 0;
unsigned long page_offset_base = 0;
unsigned long vmemmap_base     = 0;

// ── cpu topology ────────────────────────────────────────────────────────────
// cpu_online_mask is ((const struct cpumask *)&__cpu_online_mask), so the
// symbol the driver needs is the bare struct. one cpu, cpu 0 online - the
// container is single-threaded by design (see lx_sync.c). (satoru)
// one network namespace and one uts namespace: the container has no netns
// unshare and cfg80211's pernet ops only need a valid struct net pointer. (satoru)
struct net init_net;
struct uts_namespace init_uts_ns;
enum system_states system_state = SYSTEM_RUNNING;
atomic_t __num_online_cpus = ATOMIC_INIT(1);
const char hex_asc[] = "0123456789abcdef";
const char hex_asc_upper[] = "0123456789ABCDEF";
char __start_rodata[] = { 0 };
char __end_rodata[] = { 0 };

struct cpumask __cpu_online_mask = { .bits = { 1 } };
// one shim cpu. a generated stub here was read as code bytes by
// num_possible_cpus()/for_each_possible_cpu. (satoru)
struct cpumask __cpu_possible_mask = { .bits = { 1 } };
// per_cpu_ptr() adds __per_cpu_offset[cpu]; all zero means the single copy
// pcpu_alloc_noprof() returns is every "cpu's" copy. (satoru)
unsigned long __per_cpu_offset[64];

// boot_cpu_data is read for cache line size / capability bits. zeroed with a
// sane cache alignment so alignment maths does not divide by zero. (satoru)
struct cpuinfo_x86 boot_cpu_data = {
    .x86_clflush_size    = 64,
    .x86_cache_alignment = 64,
};

// per-cpu offset: single cpu, no per-cpu area, so zero. (satoru)
unsigned long this_cpu_off = 0;

// pcpu_hot is the hot per-cpu block x86 reads for current/preempt_count/etc.
// Leaving it as a generated stub meant the driver read a FUNCTION's code bytes
// as this struct and dereferenced current -> instant SIGSEGV inside probe().
// That was the data-symbol hole biting for real, exactly as predicted. (satoru)
// current_task points at a zeroed dummy so header-inline `current->...` reads
// and writes have somewhere harmless to go; the shim's per-thread task state is
// lx_current() (lx_sched.c). (satoru)
static struct task_struct lx_shared_current;
static struct signal_struct lx_dummy_signal;
static struct pid lx_dummy_pid;

// the shared `current` has to be plausible enough for header-inline code that
// dereferences it. drm_file_alloc() is the first such user: task_tgid(current)
// reads current->signal->pids[PIDTYPE_TGID]. fill in a one-process signal/pid
// pair once, before any driver code runs. (satoru)
void lx_task_shared_init(void)
{
    if (lx_shared_current.signal)
        return;
    refcount_set(&lx_dummy_pid.count, 1);
    lx_dummy_signal.pids[PIDTYPE_PID] = &lx_dummy_pid;
    lx_dummy_signal.pids[PIDTYPE_TGID] = &lx_dummy_pid;
    lx_shared_current.signal = &lx_dummy_signal;
    lx_shared_current.group_leader = &lx_shared_current;
    lx_shared_current.real_parent = &lx_shared_current;
    lx_shared_current.pid = 1;
    lx_shared_current.tgid = 1;
}

struct pcpu_hot pcpu_hot = {
    .current_task    = &lx_shared_current,
    .preempt_count   = 0,
    .cpu_number      = 0,
    .top_of_stack    = NULL,
};

// const_pcpu_hot: the const alias of pcpu_hot that vmlinux's linker script
// provides; 6.12's get_current() reads through it. DATA, so a generated stub
// would be read as code bytes. its declared type is __seg_gs qualified, so C
// cannot spell the alias; an assembler .set can, but only in the object that
// DEFINES pcpu_hot (a .set to an external symbol silently loses to the stub,
// which is what the first attempt in lx_sc.c did). (satoru)
__asm__(".globl const_pcpu_hot\n.set const_pcpu_hot, pcpu_hot\n");

// the pci bus type is a real data object shared by every family: lx_pci.c
// points its synthetic pci_dev's dev.bus at &pci_bus_type, which is what
// dev_is_pci() and the drm core compare against. platform_bus_type lives in
// lx_device.c with its match/probe/remove callbacks (the touchpad family
// binds platform drivers to it). (satoru)
struct bus_type pci_bus_type = {
    .name = "pci",
};

// softnet_data is per-cpu NAPI/backlog state. Real storage so napi_schedule and
// friends have something valid to touch. (satoru)
struct softnet_data softnet_data;

// a static_key the vmemmap-optimisation checks expand into. Zeroed = disabled,
// which is correct: there is no hugetlb vmemmap optimisation in a container.
// Must be DATA - a stub here is read as a key value, not called. (satoru)
struct static_key_false hugetlb_optimize_vmemmap_key;

// ── workqueues ──────────────────────────────────────────────────────────────
// system_wq and friends now live in lx_work.c next to the real workqueue
// implementation they point at. (satoru)

// ── power management ────────────────────────────────────────────────────────
// DATA: pm_suspend_global_flags is read by drivers' suspend paths. zero = no
// suspend-to-idle / no firmware-assisted suspend, which is the truth here.
unsigned int pm_suspend_global_flags;

// ── slab ────────────────────────────────────────────────────────────────────
// kmalloc_caches is indexed by kmalloc's inlined size lookup. All-NULL is safe
// because lx_mem.c implements the allocation entry points directly rather than
// walking these caches. (satoru)
struct kmem_cache *kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1];

// ── module parameters ───────────────────────────────────────────────────────
// e1000e declares module_param arrays. The ops structs are only invoked when
// parameters are actually parsed from a command line, which a container does
// not do - but they must EXIST as data because the param tables point at them.
const struct kernel_param_ops param_ops_int   = { 0 };
const struct kernel_param_ops param_ops_uint  = { 0 };
const struct kernel_param_ops param_array_ops = { 0 };


// ── numa topology: one node ─────────────────────────────────────────────────
// the container is one process on one host node; x86_64's defconfig enables
// CONFIG_NUMA, so the page allocator's node_online()/numa_mem_id() must see a
// sane single-node mask instead of a generated stub. (satoru)
int numa_node;
nodemask_t node_states[NR_NODE_STATES] = {
    [N_POSSIBLE] = { .bits = { 1UL } },
    [N_ONLINE]   = { .bits = { 1UL } },
    [N_MEMORY]   = { .bits = { 1UL } },
    [N_CPU]      = { .bits = { 1UL } },
};

// end (satoru)
