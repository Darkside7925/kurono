// kernel-context view of the shim's own helpers.
//
// lx_emul.h includes <stdint.h>/<stddef.h>, which collides with the kernel
// headers (conflicting int64_t/uint64_t). Kernel-context sources must therefore
// NOT include lx_emul.h - they include this instead, which declares the same
// handful of helpers with no libc headers at all.
//
// end (satoru)

#ifndef KLD_LX_KSHIM_H
#define KLD_LX_KSHIM_H

void lx_log(const char *s);
void lx_log_u(unsigned long v);
void lx_log_hex(unsigned long v);
void lx_log_line(const char *s);
__attribute__((noreturn)) void lx_unimpl(const char *sym);

// monotonic ns since container start (lx_time.c). (satoru)
unsigned long long lx_uptime_ns(void);
void lx_jiffies_update(void);
void lx_timers_start(void);   // lx_work.c: timer thread + current jiffies at init (satoru)

// ── userspace-context OS glue (lx_emul/lx_os.c) ─────────────────────────────
int   lx_os_thread_create(void (*fn)(void *), void *arg);
void  lx_os_yield(void);
long  lx_os_gettid(void);
int   lx_os_futex_wait(volatile unsigned int *addr, unsigned int val, long long timeout_ns);
int   lx_os_futex_wake(volatile unsigned int *addr, int n);
void *lx_os_reserve(unsigned long bytes);
void  lx_os_unreserve(void *p, unsigned long bytes);
// raw results for the hostile-map rig: 0 ok, else -errno. (satoru)
long  lx_os_munmap_rc(void *p, unsigned long bytes);
long  lx_os_madvise_rc(void *p, unsigned long bytes, int advice);
// true when p is inside the DMA page arena (vmap's contiguous fast path). (satoru)
int   lx_arena_contains(const void *p);
// pages from an arena va to the end of its broker DMA record (vmap runs). (satoru)
unsigned long lx_arena_chunk_left(unsigned long va);
void *lx_os_malloc(unsigned long n);
void *lx_os_aligned_alloc(unsigned long align, unsigned long n);
void  lx_os_free(void *p);
void  lx_os_random(void *buf, unsigned long n);
unsigned long long lx_os_now_ns(void);
void  lx_os_sleep_ns(unsigned long long ns);
__attribute__((noreturn)) void lx_os_exit(int rc);
long lx_os_file_size(const char *path);
long lx_os_gs_base(unsigned long long *out);
long lx_os_read_file(const char *path, void *buf, unsigned long max);

// ── futex lock primitive (lx_sync.c) ────────────────────────────────────────
// a 3-state futex mutex (0 free, 1 locked, 2 locked with waiters) on any
// 32-bit word. every linux lock type in the container is built on it. (satoru)
void lx_flock(volatile unsigned int *w);
void lx_funlock(volatile unsigned int *w);
int  lx_ftrylock(volatile unsigned int *w);

// ── tasks (lx_sched.c) ──────────────────────────────────────────────────────
struct task_struct;
struct lx_task;
struct lx_task *lx_current(void);
const char *lx_current_name(void);
struct work_struct *lx_work_running(void);
struct task_struct *lx_task_ts(struct lx_task *t);
struct lx_task *lx_task_of(struct task_struct *ts);
// park the calling task until woken or timeout_ns passes (<0 = forever).
// returns 1 on timeout. (satoru)
int  lx_task_park(long long timeout_ns);
int  lx_task_wake(struct lx_task *t);
// start a named container thread with its own lx_task. (satoru)
struct lx_task *lx_task_spawn(const char *name, int (*fn)(void *), void *arg);
// contiguous run of broker-backed order-0 pages for vmap-able objects
// (drm gem shmem). returns the first struct page. (satoru)
struct page *lx_alloc_page_run(unsigned long pages);

void lx_sched_wake_start(struct task_struct *p);
void lx_task_join(struct lx_task *t);
// per-thread RCU registry (lx_rcu.c); every shim thread joins on creation and
// leaves on exit. (satoru)
void lx_rcu_thread_register(struct lx_task *t);
void lx_rcu_thread_unregister(struct lx_task *t);

// ── wait/yield statistics (KLD_STATS=1, lx_sched.c) ────────────────────────
// n1 instrumentation: per-shim-thread run/wake counts, wake-to-run latency and
// time spent in lx_os_yield. off unless lx_stats_start() ran. (satoru)
void lx_stats_start(void);
void lx_stats_log(void);
int  lx_stats_on(void);
void lx_stats_wait_begin(const void *key);
void lx_stats_wait_end(void);
void lx_stats_wake_addr(const void *key);
void lx_stats_yield_begin(void);
void lx_stats_yield_end(void);
// timer lateness and jiffies staleness, both only when KLD_STATS=1. (satoru)
void lx_stats_timer(unsigned long long late_ns, unsigned long fn);
void lx_stats_jiffies(unsigned long long stale_ns);

// ── N1b rx hop timestamps (lx_os.c; same rdtsc clock as the kernel) ────────
unsigned long long lx_n1b_tsc(void);
void lx_n1b_hop(int id, unsigned long long a, unsigned long long b);
void lx_n1b_hop_tag(int id, unsigned long long a, unsigned long long b, int tag);
void lx_n1b_dump(void);
extern volatile unsigned long long lx_n1b_t2;   // last handler-done TSC (satoru)
extern volatile unsigned long long lx_n1b_t3;   // current poll-start TSC (satoru)
extern unsigned long lx_n1b_wakes, lx_n1b_handlers, lx_n1b_polls, lx_n1b_empty;

// ── irq context marker (lx_irq.c) ───────────────────────────────────────────
int lx_in_irq(void);
// run fn in the primary-handler context (hardirq bits + lx_in_irq). (satoru)
int lx_irq_test_handler_ctx(int (*fn)(void *), void *arg);
// software irq for the hidsim model: allocate one, raise one. (satoru)
int lx_irq_alloc_soft(unsigned int *out_irq);
void lx_irq_raise_soft(unsigned int irq);
void lx_irq_free_soft(unsigned int irq);
void lx_mmio_sim_reset(void);
int lx_mmio_sim_claimed(unsigned long start, unsigned long len);
int lx_irqdomain_request_resources(unsigned int irq);
int lx_irqdomain_activate(unsigned int irq);
void lx_irqdomain_deactivate(unsigned int irq);
void lx_irqdomain_release_resources(unsigned int irq);
int lx_mmio_sim_claim(void *base, unsigned long len,
                      u64 (*read)(void *, unsigned long, int),
                      void (*write)(void *, unsigned long, int, u64),
                      void *ctx);

// ── pci probe deferral (lx_pci.c) ───────────────────────────────────────────
void lx_pci_defer_probe(bool on);

struct lx_pci_sim_dev {
    u16 vendor, device, subvendor, subdevice;
    u32 class_code;
    u8 revision;
    u8 config[256];
    unsigned long bar0_len;
    int irq;
};
int lx_pci_sim_add(const struct lx_pci_sim_dev *d);
void *lx_pci_sim_bar0(void);
unsigned long lx_pci_sim_bar0_len(void);
int lx_pci_sim_irq(void);

// ── pci probe deferral (lx_pci.c) ───────────────────────────────────────────
void lx_pci_defer_probe(bool on);
void lx_pci_probe_deferred(void);
// --kld-ids: registration only, no probe, no broker call. (satoru)
void lx_pci_set_bus_disabled(bool on);
struct pci_driver *lx_pci_driver_at(int idx);

// ── net bridge (lx_net.c) ───────────────────────────────────────────────────
void lx_net_bringup_all(void);
void lx_net_wiphy_report(void);
int  lx_net_open_name(const char *name);
int  lx_net_close_name(const char *name);
int  lx_net_ifindex_name(const char *name);
unsigned long lx_irq_total(void);
u64 lx_time_spin_max_ns(void);
void lx_softirq_enter(void);
void lx_softirq_exit(void);
extern volatile unsigned long lx_softirq_bad_after;

// in-container generic netlink dispatcher (lx_genl.c). (satoru)
struct sk_buff;
struct nlmsghdr;
typedef void (*lx_genl_msg_cb)(struct sk_buff *skb, struct nlmsghdr *nlh);
int  lx_genl_doit(const char *family_name, struct sk_buff *skb);
int  lx_genl_dump(const char *family_name, struct sk_buff *skb, lx_genl_msg_cb reply);
void lx_genl_set_reply_cb(lx_genl_msg_cb cb);
int  lx_genl_listen(const char *family, const char *group, lx_genl_msg_cb fn);
int  lx_genl_listen_portid(u32 portid, lx_genl_msg_cb fn);
void lx_genl_clear_portid(u32 portid);
int  lx_genl_notify_release(u32 portid);

#endif
// end (satoru)
