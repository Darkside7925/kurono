// lx_emul: tasks and kthreads.
//
// Every container thread that runs Linux code has an lx_task: a real (zeroed)
// struct task_struct for code that wants one, plus the shim's own state - a
// sticky wake word the task parks on, and kthread stop/exit bookkeeping.
//
// `current` in the Linux headers reads pcpu_hot.current_task through %gs, and
// %gs base is 0 in a Kurono user process, so `current` is ONE shared dummy task
// for the whole container (lx_data.c). The shim therefore never uses `current`
// to find a task; it uses lx_current(), which is per thread (__thread). Inline
// header code that writes current->__state only scribbles on the dummy, which
// nothing reads. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "lx_kshim.h"

// KLD_STATS=1 instrumentation, one per shim thread. (satoru)
struct lx_stat {
    unsigned long long waits, runs, wakes;
    unsigned long long lat_sum, lat_max, lat_n;
    unsigned long long yield_sum, yield_max, yield_n;
    unsigned int hist[24];           // wake-to-run latency, log2(us+1)
};

struct lx_task {
    struct task_struct ts;          // first: lx_task_of() is a cast (satoru)
    unsigned int magic;
    volatile unsigned int woken;    // sticky wake word (futex) (satoru)
    int (*fn)(void *);
    void *arg;
    volatile unsigned int started;  // kthreads start parked until woken (satoru)
    volatile int should_stop;
    volatile unsigned int exited;   // futex: 1 once fn returned (satoru)
    int exit_code;
    char name[TASK_COMM_LEN];
    // stats (only touched when g_stats_on) (satoru)
    struct lx_stat st;
    unsigned long long wait_start_ns, wait_key, wake_ns, yield_start_ns;
};

// every lx_task ever created, for the periodic dump. (satoru)
#define LX_STAT_TASKS 64
static struct lx_task *g_stat_tasks[LX_STAT_TASKS];
static volatile int g_stat_task_n;
static volatile int g_stats;
static volatile int g_stats_thread_up;

int lx_stats_on(void) { return g_stats; }

#define LX_TASK_MAGIC 0x4c585441u   /* "LXTA" */

static __thread struct lx_task *lx_self;

static struct lx_task *lx_task_alloc(const char *name)
{
    struct lx_task *t = lx_os_aligned_alloc(64, sizeof(*t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));
    t->magic = LX_TASK_MAGIC;
    strscpy(t->name, name ? name : "lx", sizeof(t->name));
    strscpy(t->ts.comm, t->name, sizeof(t->ts.comm));
    t->ts.pid = 0;
    int slot = __atomic_fetch_add(&g_stat_task_n, 1, __ATOMIC_ACQ_REL);
    if (slot < LX_STAT_TASKS)
        g_stat_tasks[slot] = t;
    else
        __atomic_store_n(&g_stat_task_n, LX_STAT_TASKS, __ATOMIC_RELEASE);
    return t;
}

// the calling thread's task. threads the shim did not start (main) get one
// lazily. (satoru)
struct lx_task *lx_current(void)
{
    if (!lx_self) {
        lx_self = lx_task_alloc("kld-main");
        if (lx_self) {
            lx_self->started = 1;
            lx_rcu_thread_register(lx_self);
        }
    }
    return lx_self;
}

struct task_struct *lx_task_ts(struct lx_task *t) { return t ? &t->ts : NULL; }
const char *lx_current_name(void)
{
    struct lx_task *t = lx_current();
    return (t && t->name[0]) ? t->name : "?";
}

struct lx_task *lx_task_of(struct task_struct *ts)
{
    struct lx_task *t = (struct lx_task *)ts;
    if (!t || t->magic != LX_TASK_MAGIC)
        return NULL;   // the shared `current` dummy, or garbage (satoru)
    return t;
}

int lx_task_park(long long timeout_ns)
{
    struct lx_task *t = lx_current();
    unsigned long long end = timeout_ns >= 0 ? lx_os_now_ns() + (unsigned long long)timeout_ns : 0;
    for (;;) {
        if (__atomic_exchange_n(&t->woken, 0, __ATOMIC_ACQUIRE))
            return 0;
        long long left = -1;
        if (timeout_ns >= 0) {
            unsigned long long now = lx_os_now_ns();
            if (now >= end)
                return 1;
            left = (long long)(end - now);
        }
        lx_os_futex_wait(&t->woken, 0, left);
    }
}

int lx_task_wake(struct lx_task *t)
{
    if (!t)
        return 0;
    if (__atomic_exchange_n(&t->woken, 1, __ATOMIC_RELEASE) == 0) {
        if (g_stats) {
            t->wake_ns = lx_os_now_ns();
            t->st.wakes++;
        }
        lx_os_futex_wake(&t->woken, 1);
        return 1;
    }
    return 0;   // already had a pending wake (satoru)
}

static void lx_task_entry(void *p)
{
    struct lx_task *t = p;
    lx_self = t;
    lx_rcu_thread_register(t);
    // linux kthreads are created stopped and start on wake_up_process. (satoru)
    while (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE))
        lx_os_futex_wait(&t->started, 0, -1);
    t->exit_code = t->fn ? t->fn(t->arg) : 0;
    lx_rcu_thread_unregister(t);
    __atomic_store_n(&t->exited, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&t->exited, 0x7fffffff);
}

static struct lx_task *lx_task_create(const char *name, int (*fn)(void *), void *arg, int start)
{
    struct lx_task *t = lx_task_alloc(name);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->started = start ? 1 : 0;
    if (lx_os_thread_create(lx_task_entry, t) != 0) {
        lx_os_free(t);
        return NULL;
    }
    return t;
}

struct lx_task *lx_task_spawn(const char *name, int (*fn)(void *), void *arg)
{
    return lx_task_create(name, fn, arg, 1);
}

// wake_up_process on a not-yet-started kthread starts it. (satoru)
static void lx_task_start(struct lx_task *t)
{
    if (!__atomic_exchange_n(&t->started, 1, __ATOMIC_RELEASE))
        lx_os_futex_wake(&t->started, 1);
}

// ── kthreads ────────────────────────────────────────────────────────────────
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data), void *data,
                                           int node, const char namefmt[], ...)
{
    char name[TASK_COMM_LEN];
    va_list ap;
    (void)node;
    va_start(ap, namefmt);
    vsnprintf(name, sizeof(name), namefmt, ap);
    va_end(ap);
    struct lx_task *t = lx_task_create(name, threadfn, data, 0);
    if (!t)
        return ERR_PTR(-ENOMEM);
    lx_log("[lx_sched] kthread created: ");
    lx_log_line(name);
    return &t->ts;
}

bool kthread_should_stop(void)
{
    struct lx_task *t = lx_current();
    return t && __atomic_load_n(&t->should_stop, __ATOMIC_ACQUIRE);
}

int kthread_stop(struct task_struct *k)
{
    struct lx_task *t = lx_task_of(k);
    if (!t)
        return -EINVAL;
    __atomic_store_n(&t->should_stop, 1, __ATOMIC_RELEASE);
    lx_task_start(t);
    lx_task_wake(t);
    while (!__atomic_load_n(&t->exited, __ATOMIC_ACQUIRE))
        lx_os_futex_wait(&t->exited, 0, -1);
    // the task struct stays allocated: a driver may still hold the pointer. a
    // tiny leak per stopped kthread is the honest price. (satoru)
    return t->exit_code;
}

// wait for a shim thread to return from its function. (satoru)
void lx_task_join(struct lx_task *t)
{
    if (!t || t == lx_current())
        return;
    while (!__atomic_load_n(&t->exited, __ATOMIC_ACQUIRE))
        lx_os_futex_wait(&t->exited, 0, -1);
}

// starting a kthread is a wake. (satoru)
void lx_sched_wake_start(struct task_struct *p)
{
    struct lx_task *t = lx_task_of(p);
    if (t)
        lx_task_start(t);
}

// ── timer / jiffies health counters (KLD_STATS=1) ───────────────────────────
static unsigned long long g_timer_late_max, g_timer_late_sum, g_timer_n, g_timer_over50;
static unsigned long g_timer_late_fn;
static unsigned long long g_jiffies_stale_max, g_jiffies_stale_n;

void lx_stats_timer(unsigned long long late_ns, unsigned long fn)
{
    if (!g_stats) return;
    g_timer_n++;
    g_timer_late_sum += late_ns;
    if (late_ns > g_timer_late_max) {
        g_timer_late_max = late_ns;
        g_timer_late_fn = fn;
    }
    if (late_ns > 50000000ull) {
        g_timer_over50++;
        lx_log("[lx_stat] timer late ");
        lx_log_u((unsigned long)(late_ns / 1000000ull));
        lx_log(" ms fn=0x");
        lx_log_hex(fn);
        lx_log("\n");
    }
}

void lx_stats_jiffies(unsigned long long stale_ns)
{
    if (!g_stats) return;
    g_jiffies_stale_n++;
    if (stale_ns > g_jiffies_stale_max)
        g_jiffies_stale_max = stale_ns;
}

// ── KLD_STATS=1 instrumentation ─────────────────────────────────────────────
// a small table of the newest futex wake per address, for the direct
// lx_os_futex_wait callers (net, irq, rcu, ...). (satoru)
struct lx_wake_rec { const void *key; unsigned long long ns; };
static struct lx_wake_rec g_wake_rec[32];
static volatile unsigned int g_wake_rec_next;

void lx_stats_wake_addr(const void *key)
{
    if (!g_stats || !key)
        return;
    unsigned int i = __atomic_fetch_add(&g_wake_rec_next, 1, __ATOMIC_RELAXED) & 31u;
    g_wake_rec[i].ns = lx_os_now_ns();
    g_wake_rec[i].key = key;
}

void lx_stats_wait_begin(const void *key)
{
    if (!g_stats)
        return;
    struct lx_task *t = lx_current();
    t->wait_start_ns = lx_os_now_ns();
    t->wait_key = (unsigned long long)(uintptr_t)key;
    t->st.waits++;
}

static unsigned int us_bucket(unsigned long long ns)
{
    unsigned long long us = ns / 1000ull;
    unsigned int b = 0;
    while (us > 1 && b < 23) { us >>= 1; b++; }
    return b;
}

void lx_stats_wait_end(void)
{
    if (!g_stats)
        return;
    struct lx_task *t = lx_current();
    unsigned long long now = lx_os_now_ns();
    t->st.runs++;
    unsigned long long wake = 0;
    if (t->wake_ns && t->wake_ns >= t->wait_start_ns) {
        wake = t->wake_ns;
        t->wake_ns = 0;
    } else {
        // direct futex wait: newest recorded wake for this address. (satoru)
        unsigned int n = __atomic_load_n(&g_wake_rec_next, __ATOMIC_RELAXED);
        if (n > 32) n = 32;
        for (unsigned int i = 0; i < n; i++) {
            if ((unsigned long long)(uintptr_t)g_wake_rec[i].key == t->wait_key &&
                g_wake_rec[i].ns >= t->wait_start_ns && g_wake_rec[i].ns > wake)
                wake = g_wake_rec[i].ns;
        }
    }
    if (wake) {
        unsigned long long lat = now - wake;
        t->st.lat_sum += lat;
        t->st.lat_n++;
        if (lat > t->st.lat_max) t->st.lat_max = lat;
        unsigned int b = us_bucket(lat);
        if (b < 24) t->st.hist[b]++;
    }
}

void lx_stats_yield_begin(void)
{
    if (!g_stats)
        return;
    struct lx_task *t = lx_current();
    t->yield_start_ns = lx_os_now_ns();
    t->st.yield_n++;
}

void lx_stats_yield_end(void)
{
    if (!g_stats)
        return;
    struct lx_task *t = lx_current();
    unsigned long long d = lx_os_now_ns() - t->yield_start_ns;
    t->st.yield_sum += d;
    if (d > t->st.yield_max) t->st.yield_max = d;
}

static void stat_puts(const char *s)
{
    lx_log(s);
}

static void stat_hex(unsigned long v)
{
    char b[20]; int k = 0;
    if (!v) { lx_log("0"); return; }
    while (v) { unsigned d = (unsigned)(v & 15u); b[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (k) { char r[2] = { b[--k], 0 }; lx_log(r); }
}

static void stat_u(unsigned long long v)
{
    char b[24];
    int k = 0;
    if (!v) b[k++] = '0';
    while (v) { b[k++] = (char)('0' + (v % 10)); v /= 10; }
    char r[24];
    int n = 0;
    while (k) r[n++] = b[--k];
    r[n] = 0;
    lx_log(r);
}

void lx_stats_log(void)
{
    if (!g_stats)
        return;
    int n = __atomic_load_n(&g_stat_task_n, __ATOMIC_ACQUIRE);
    if (n > LX_STAT_TASKS) n = LX_STAT_TASKS;
    lx_log("[lx_stat] --- per-thread waits/yields ---\n");
    for (int i = 0; i < n; i++) {
        struct lx_task *t = g_stat_tasks[i];
        if (!t || t->magic != LX_TASK_MAGIC)
            continue;
        struct lx_stat *s = &t->st;
        unsigned long long avg = s->lat_n ? s->lat_sum / s->lat_n : 0;
        // p99 bucket: the smallest bucket whose cumulative count reaches 99%
        unsigned int total = 0, cum = 0, p99b = 0;
        for (int b = 0; b < 24; b++) total += s->hist[b];
        if (total) {
            unsigned int want = total - total / 100;   // ceil(99%)
            for (int b = 0; b < 24; b++) {
                cum += s->hist[b];
                if (cum >= want) { p99b = (unsigned int)b; break; }
            }
        }
        unsigned long long p99 = p99b ? (1ull << p99b) : 0;
        lx_log("[lx_stat] ");
        lx_log(t->name);
        stat_puts(" waits="); stat_u(s->waits);
        stat_puts(" runs="); stat_u(s->runs);
        stat_puts(" yields="); stat_u(s->yield_n);
        stat_puts(" yield_ms="); stat_u(s->yield_sum / 1000000ull);
        stat_puts(" wake_n="); stat_u(s->lat_n);
        stat_puts(" wake_avg_us="); stat_u(avg / 1000ull);
        stat_puts(" wake_p99_us="); stat_u(p99);
        stat_puts(" wake_max_us="); stat_u(s->lat_max / 1000ull);
        stat_puts("\n");
    }
    lx_log("[lx_stat] timer n="); stat_u(g_timer_n);
    stat_puts(" late_avg_us="); stat_u(g_timer_n ? g_timer_late_sum / g_timer_n / 1000ull : 0);
    stat_puts(" late_max_us="); stat_u(g_timer_late_max / 1000ull);
    stat_puts(" over50ms="); stat_u(g_timer_over50);
    stat_puts(" latemax_fn=0x"); stat_hex(g_timer_late_fn);
    stat_puts("\n");
    lx_log("[lx_stat] jiffies stale_max_us=");
    stat_u(g_jiffies_stale_max / 1000ull);
    stat_puts(" samples="); stat_u(g_jiffies_stale_n);
    stat_puts("\n");
    lx_n1b_dump();
}

static void lx_stats_thread(void *arg)
{
    (void)arg;
    for (;;) {
        lx_os_sleep_ns(5000000000ull);
        lx_stats_log();
    }
}

void lx_stats_start(void)
{
    if (__atomic_exchange_n(&g_stats, 1, __ATOMIC_ACQ_REL))
        return;
    if (__atomic_exchange_n(&g_stats_thread_up, 1, __ATOMIC_ACQ_REL))
        return;
    lx_os_thread_create(lx_stats_thread, NULL);
    lx_log("[lx_stat] instrumentation on (KLD_STATS=1), dumping every 5 s\n");
}

// end (satoru)
