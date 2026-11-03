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
struct cpumask __cpu_online_mask = { .bits = { 1 } };

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
struct pcpu_hot pcpu_hot = {
    .current_task    = NULL,
    .preempt_count   = 0,
    .cpu_number      = 0,
    .top_of_stack    = NULL,
};

// softnet_data is per-cpu NAPI/backlog state. Real storage so napi_schedule and
// friends have something valid to touch. (satoru)
struct softnet_data softnet_data;

// a static_key the vmemmap-optimisation checks expand into. Zeroed = disabled,
// which is correct: there is no hugetlb vmemmap optimisation in a container.
// Must be DATA - a stub here is read as a key value, not called. (satoru)
struct static_key_false hugetlb_optimize_vmemmap_key;

// ── workqueues ──────────────────────────────────────────────────────────────
// struct workqueue_struct is OPAQUE in the headers (defined in workqueue.c), so
// we cannot declare storage for it. A non-NULL token is all the driver needs -
// it only ever passes system_wq back to queue_work_on(), which lx_work.c
// implements by running the work inline and ignoring the queue. (satoru)
static unsigned long lx_system_wq_storage[8];
struct workqueue_struct *system_wq =
    (struct workqueue_struct *)lx_system_wq_storage;

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

// end (satoru)
