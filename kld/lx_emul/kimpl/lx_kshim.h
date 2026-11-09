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

// ── userspace-context OS glue (lx_emul/lx_os.c) ─────────────────────────────
int   lx_os_thread_create(void (*fn)(void *), void *arg);
void  lx_os_yield(void);
long  lx_os_gettid(void);
int   lx_os_futex_wait(volatile unsigned int *addr, unsigned int val, long long timeout_ns);
int   lx_os_futex_wake(volatile unsigned int *addr, int n);
void *lx_os_reserve(unsigned long bytes);
void  lx_os_unreserve(void *p, unsigned long bytes);
void *lx_os_malloc(unsigned long n);
void *lx_os_aligned_alloc(unsigned long align, unsigned long n);
void  lx_os_free(void *p);
void  lx_os_random(void *buf, unsigned long n);
unsigned long long lx_os_now_ns(void);
void  lx_os_sleep_ns(unsigned long long ns);
__attribute__((noreturn)) void lx_os_exit(int rc);

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
struct task_struct *lx_task_ts(struct lx_task *t);
struct lx_task *lx_task_of(struct task_struct *ts);
// park the calling task until woken or timeout_ns passes (<0 = forever).
// returns 1 on timeout. (satoru)
int  lx_task_park(long long timeout_ns);
int  lx_task_wake(struct lx_task *t);
// start a named container thread with its own lx_task. (satoru)
struct lx_task *lx_task_spawn(const char *name, int (*fn)(void *), void *arg);
void lx_sched_wake_start(struct task_struct *p);
void lx_task_join(struct lx_task *t);

// ── irq context marker (lx_irq.c) ───────────────────────────────────────────
int lx_in_irq(void);

// ── net bridge (lx_net.c) ───────────────────────────────────────────────────
void lx_net_bringup_all(void);

#endif
// end (satoru)
