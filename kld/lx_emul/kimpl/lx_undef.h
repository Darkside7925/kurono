// The shim has no per-CPU allocator and no netdev tracker allocations; use
// the plain dev_refcnt path instead. This is a build-time ABI choice, not a
// source patch. (satoru)
#undef CONFIG_PCPU_DEV_REFCNT
#undef CONFIG_NET_DEV_REFCNT_TRACKER

// the container runs in ring 3, where the kernel's %gs per-cpu accesses depend
// on a zero user gs base that kurono does not reliably preserve. measured:
// under qemu pcpu_hot.preempt_count intermittently read as 0x7fffffc9 after the
// rhashtable threads ran, and skcipher_walk then rejected every request with
// -EDEADLK. compile the absolute-address percpu path instead: this_cpu_off and
// __per_cpu_offset[] are zero, which is exactly a one-cpu container. the
// matching prefix override is shim-include/asm/percpu.h. this is a build-time
// ABI choice, not a source patch. (satoru)
#undef CONFIG_CC_HAS_NAMED_AS
#undef CONFIG_USE_X86_SEG_SUPPORT

// the container provides the clk consumer API (lx_clk.c). (satoru)
#define CONFIG_HAVE_CLK 1
