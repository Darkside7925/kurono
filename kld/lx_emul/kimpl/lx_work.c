// lx_emul: timer_list and workqueues, on real threads.
//
// TIMERS: one timer thread owns every pending timer_list on an hlist, so
// timer_pending() (an inline that tests entry.pprev) stays correct. It sleeps
// on a futex until the earliest expiry or a re-arm kick, and it also keeps the
// `jiffies` DATA symbol current (at least every 2 ms), because drivers read
// jiffies directly for their timeouts. timer_delete_sync() waits for a running
// callback, like upstream.
//
// WORKQUEUES: one worker thread per workqueue (system_wq has its own), FIFO.
// The PENDING bit lives where upstream keeps it (bit 0 of work->data), so the
// inline work_pending() is truthful. Pending is cleared just before the
// callback runs, so a work item may requeue itself, as upstream allows.
// Delayed work is a timer whose callback, delayed_work_timer_fn, queues the work.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "lx_kshim.h"

// ── timers ──────────────────────────────────────────────────────────────────
static volatile unsigned int lx_tmr_lock;
static HLIST_HEAD(lx_tmr_list);
static struct timer_list *volatile lx_tmr_running;
static volatile unsigned int lx_tmr_kick;     // futex: re-arm / new timer (satoru)
static volatile unsigned int lx_tmr_done;     // futex: a callback finished (satoru)
static struct lx_task *lx_tmr_task;

static int lx_timer_thread(void *arg);
static void lx_timer_start_once(void);

// the timer thread is also the jiffies refresh; start it at container init so
// the first driver that computes "jiffies + 100 ms" does so on a current value
// (a stale jiffies made hid-multitouch's sticky-finger timer fire at once). (satoru)
void lx_timers_start(void)
{
    lx_jiffies_update();
    lx_timer_start_once();
}

static void lx_timer_start_once(void)
{
    if (__atomic_load_n(&lx_tmr_task, __ATOMIC_ACQUIRE))
        return;
    lx_flock(&lx_tmr_lock);
    if (!lx_tmr_task)
        lx_tmr_task = lx_task_spawn("kld-timer", lx_timer_thread, NULL);
    lx_funlock(&lx_tmr_lock);
}

static void lx_timer_kick(void)
{
    __atomic_fetch_add(&lx_tmr_kick, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&lx_tmr_kick, 1);
}

void init_timer_key(struct timer_list *timer, void (*func)(struct timer_list *),
                    unsigned int flags, const char *name, struct lock_class_key *key)
{
    (void)name; (void)key;
    timer->entry.pprev = NULL;
    timer->entry.next = NULL;
    timer->function = func;
    timer->flags = flags;
}

// returns 1 if the timer was pending. caller holds lx_tmr_lock. (satoru)
static int lx_timer_detach(struct timer_list *timer)
{
    if (hlist_unhashed(&timer->entry))
        return 0;
    hlist_del_init(&timer->entry);
    return 1;
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
    int was;
    lx_timer_start_once();
    lx_flock(&lx_tmr_lock);
    was = lx_timer_detach(timer);
    timer->expires = expires;
    hlist_add_head(&timer->entry, &lx_tmr_list);
    lx_funlock(&lx_tmr_lock);
    lx_timer_kick();
    return was;
}

void add_timer(struct timer_list *timer) { mod_timer(timer, timer->expires); }

int timer_reduce(struct timer_list *timer, unsigned long expires)
{
    if (timer_pending(timer) && time_before_eq(timer->expires, expires))
        return 1;
    return mod_timer(timer, expires);
}

int timer_delete(struct timer_list *timer)
{
    int was;
    lx_flock(&lx_tmr_lock);
    was = lx_timer_detach(timer);
    lx_funlock(&lx_tmr_lock);
    return was;
}


int timer_delete_sync(struct timer_list *timer)
{
    int was = timer_delete(timer);
    // wait out a callback that is running right now (unless that callback is
    // the caller, which would deadlock on upstream too). (satoru)
    for (;;) {
        unsigned int d = __atomic_load_n(&lx_tmr_done, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&lx_tmr_running, __ATOMIC_ACQUIRE) != timer)
            break;
        if (lx_current() == lx_tmr_task)
            break;
        lx_os_futex_wait(&lx_tmr_done, d, 1000000);
    }
    return was;
}


int timer_shutdown_sync(struct timer_list *timer)
{
    int was = timer_delete_sync(timer);
    timer->function = NULL;
    return was;
}

static int lx_timer_thread(void *arg)
{
    (void)arg;
    lx_log("[lx_work] timer thread online\n");
    for (;;) {
        unsigned int kick = __atomic_load_n(&lx_tmr_kick, __ATOMIC_ACQUIRE);
        lx_jiffies_update();
        struct timer_list *due = NULL, *t;
        unsigned long now = jiffies;
        long next = 0;   // jiffies to the earliest pending timer; 0 = none (satoru)
        bool have = false;
        lx_flock(&lx_tmr_lock);
        hlist_for_each_entry(t, &lx_tmr_list, entry) {
            if (time_after_eq(now, t->expires)) {
                due = t;
                break;
            }
            have = true;
            long d = (long)(t->expires - now);
            if (next == 0 || d < next)
                next = d;
        }
        if (due) {
            lx_timer_detach(due);
            __atomic_store_n(&lx_tmr_running, due, __ATOMIC_RELEASE);
        }
        lx_funlock(&lx_tmr_lock);
        if (due) {
            void (*fn)(struct timer_list *) = due->function;
            if (fn) {
                if (lx_stats_on()) {
                    unsigned long long now_ns = lx_uptime_ns();
                    unsigned long long exp_ns =
                        (unsigned long long)(due->expires - INITIAL_JIFFIES) *
                        (1000000000ull / HZ);
                    unsigned long long late = now_ns > exp_ns ? now_ns - exp_ns : 0;
                    lx_stats_timer(late, (unsigned long)(uintptr_t)fn);
                }
                lx_softirq_enter();
                fn(due);
                lx_softirq_exit();
            }
            __atomic_store_n(&lx_tmr_running, NULL, __ATOMIC_RELEASE);
            __atomic_fetch_add(&lx_tmr_done, 1, __ATOMIC_RELEASE);
            lx_os_futex_wake(&lx_tmr_done, 0x7fffffff);
            continue;
        }
        if (!have) {
            // nothing pending: block until a re-arm kicks this futex. jiffies
            // stays current because every blocking primitive and every
            // cpu_relax/udelay path refreshes it (N2 jiffies work), so this
            // wake is not needed as a refresh timer. (satoru)
            lx_os_futex_wait(&lx_tmr_kick, kick, -1);
            continue;
        }
        if (next < 1)
            next = 1;
        // block until five jiffies before the deadline, then spin that margin
        // so a real timer fires on time: the kernel's futex wake was measured up
        // to ~5 ms late under load, and a 5 ms spin per pending timer costs
        // 0.25% on the 2 s driver watchdog. only a real timer spins; an empty
        // list blocks forever. (satoru)
        if (next <= 5) {
            unsigned long target = now + (unsigned long)next;
            while (time_before(jiffies, target)) {
                lx_jiffies_update();
                __builtin_ia32_pause();
            }
            continue;
        }
        lx_os_futex_wait(&lx_tmr_kick, kick, (long long)(next - 5) * 1000000ll);
    }
    return 0;
}

// ── workqueues ──────────────────────────────────────────────────────────────
// struct workqueue_struct is opaque in the headers (upstream defines it in
// kernel/workqueue.c), so the shim's definition is legitimately its own. (satoru)
struct workqueue_struct {
    volatile unsigned int lock;
    volatile unsigned int kick;       // futex (satoru)
    volatile unsigned int done;       // futex: an item finished (satoru)
    struct list_head list;
    struct work_struct *volatile running;
    struct lx_task *task;
    char name[24];
    struct workqueue_struct *next_wq;
};

static struct workqueue_struct lx_system_wq = { .name = "events", .list = LIST_HEAD_INIT(lx_system_wq.list) };
struct workqueue_struct *system_wq            = &lx_system_wq;
struct workqueue_struct *system_highpri_wq    = &lx_system_wq;
struct workqueue_struct *system_long_wq       = &lx_system_wq;
struct workqueue_struct *system_unbound_wq    = &lx_system_wq;
struct workqueue_struct *system_freezable_wq  = &lx_system_wq;
struct workqueue_struct *system_power_efficient_wq = &lx_system_wq;
struct workqueue_struct *system_freezable_power_efficient_wq = &lx_system_wq;
struct workqueue_struct *system_bh_wq         = &lx_system_wq;

static struct workqueue_struct *volatile lx_all_wqs = &lx_system_wq;
static volatile unsigned int lx_wq_global;
static __thread struct work_struct *lx_running_work;

struct work_struct *lx_work_running(void) { return lx_running_work; }

// set while cancel_work_sync/cancel_delayed_work_sync is in progress. a work
// callback that requeues itself must not come back after the cancel returned.
#define LX_WORK_CANCELED (1ul << 62)
static inline bool lx_work_canceled(struct work_struct *w)
{
    return (__atomic_load_n((unsigned long *)work_data_bits(w), __ATOMIC_ACQUIRE) &
            LX_WORK_CANCELED) != 0;
}
static inline void lx_work_set_canceled(struct work_struct *w, bool on)
{
    if (on)
        __atomic_fetch_or((unsigned long *)work_data_bits(w), LX_WORK_CANCELED,
                          __ATOMIC_ACQ_REL);
    else
        __atomic_fetch_and((unsigned long *)work_data_bits(w), ~LX_WORK_CANCELED,
                           __ATOMIC_ACQ_REL);
}

static int lx_wq_thread(void *arg)
{
    struct workqueue_struct *wq = arg;
    for (;;) {
        unsigned int kick = __atomic_load_n(&wq->kick, __ATOMIC_ACQUIRE);
        struct work_struct *w = NULL;
        lx_flock(&wq->lock);
        if (!list_empty(&wq->list)) {
            w = list_first_entry(&wq->list, struct work_struct, entry);
            list_del_init(&w->entry);
            // clear pending BEFORE running: the item may requeue itself. (satoru)
            clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
            wq->running = w;
        }
        lx_funlock(&wq->lock);
        if (!w) {
            lx_os_futex_wait(&wq->kick, kick, -1);
            continue;
        }
        lx_running_work = w;
        w->func(w);
        lx_running_work = NULL;
        lx_flock(&wq->lock);
        wq->running = NULL;
        lx_funlock(&wq->lock);
        __atomic_fetch_add(&wq->done, 1, __ATOMIC_RELEASE);
        lx_os_futex_wake(&wq->done, 0x7fffffff);
    }
    return 0;
}

static void lx_wq_start(struct workqueue_struct *wq)
{
    if (__atomic_load_n(&wq->task, __ATOMIC_ACQUIRE))
        return;
    lx_flock(&lx_wq_global);
    if (!wq->task)
        wq->task = lx_task_spawn(wq->name, lx_wq_thread, wq);
    lx_funlock(&lx_wq_global);
}

// enqueue without the pending test (the caller already owns PENDING). (satoru)
static void lx_wq_insert(struct workqueue_struct *wq, struct work_struct *work)
{
    if (lx_work_canceled(work))
        return;
    if (!wq)
        wq = system_wq;
    lx_wq_start(wq);
    lx_flock(&wq->lock);
    list_add_tail(&work->entry, &wq->list);
    lx_funlock(&wq->lock);
    __atomic_fetch_add(&wq->kick, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&wq->kick, 1);
}

bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
    if (lx_work_canceled(work))
        return false;
    (void)cpu;
    if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
        return false;
    lx_wq_insert(wq, work);
    return true;
}

// ttm's delayed BO deletion asks for a cpu near a numa node; the container has
// one node and one worker per queue, so queue on the shim's queue. (satoru)
bool queue_work_node(int node, struct workqueue_struct *wq, struct work_struct *work)
{
    (void)node;
    return queue_work_on(0, wq, work);
}

void delayed_work_timer_fn(struct timer_list *t)
{
    struct delayed_work *dwork = from_timer(dwork, t, timer);
    lx_wq_insert(dwork->wq, &dwork->work);
}

static void lx_queue_dwork(struct workqueue_struct *wq, struct delayed_work *dwork,
                           unsigned long delay)
{
    dwork->wq = wq ? wq : system_wq;
    dwork->cpu = 0;
    if (!delay) {
        lx_wq_insert(dwork->wq, &dwork->work);
        return;
    }
    lx_jiffies_update();
    mod_timer(&dwork->timer, jiffies + delay);
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
                           struct delayed_work *dwork, unsigned long delay)
{
    (void)cpu;
    if (lx_work_canceled(&dwork->work))
        return false;
    if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dwork->work)))
        return false;
    lx_queue_dwork(wq, dwork, delay);
    return true;
}

// find + unlink a queued item from whichever workqueue holds it. returns the
// workqueue it was on (or that is running it) via *wq_out. (satoru)
static int lx_work_unqueue(struct work_struct *work, struct workqueue_struct **wq_out)
{
    int removed = 0;
    *wq_out = NULL;
    for (struct workqueue_struct *wq = lx_all_wqs; wq; wq = wq->next_wq) {
        lx_flock(&wq->lock);
        struct work_struct *w;
        list_for_each_entry(w, &wq->list, entry) {
            if (w == work) {
                list_del_init(&work->entry);
                removed = 1;
                break;
            }
        }
        if (removed || wq->running == work)
            *wq_out = wq;
        lx_funlock(&wq->lock);
        if (*wq_out)
            break;
    }
    return removed;
}

static void lx_work_wait_idle(struct workqueue_struct *wq, struct work_struct *work)
{
    if (!wq || lx_current() == wq->task)
        return;
    for (;;) {
        unsigned int d = __atomic_load_n(&wq->done, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&wq->running, __ATOMIC_ACQUIRE) != work)
            return;
        lx_os_futex_wait(&wq->done, d, 1000000);
    }
}

bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
                         struct delayed_work *dwork, unsigned long delay)
{
    struct workqueue_struct *on;
    (void)cpu;
    int was = timer_delete(&dwork->timer) | lx_work_unqueue(&dwork->work, &on);
    set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dwork->work));
    lx_queue_dwork(wq, dwork, delay);
    return was;
}

bool cancel_work_sync(struct work_struct *work)
{
    struct workqueue_struct *wq;
    lx_work_set_canceled(work, true);
    int removed = lx_work_unqueue(work, &wq);
    bool pending = test_and_clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
    lx_work_wait_idle(wq, work);
    lx_work_set_canceled(work, false);
    return removed || pending;
}

bool cancel_work(struct work_struct *work)
{
    struct workqueue_struct *wq;
    int removed = lx_work_unqueue(work, &wq);
    bool pending = test_and_clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
    return removed || pending;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
    lx_work_set_canceled(&dwork->work, true);
    int t = timer_delete_sync(&dwork->timer);
    struct workqueue_struct *wq;
    int removed = lx_work_unqueue(&dwork->work, &wq);
    bool pending = test_and_clear_bit(WORK_STRUCT_PENDING_BIT,
                                      work_data_bits(&dwork->work));
    lx_work_wait_idle(wq, &dwork->work);
    lx_work_set_canceled(&dwork->work, false);
    return t || removed || pending;
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
    int t = timer_delete(&dwork->timer);
    bool w = cancel_work(&dwork->work);
    return t || w;
}

bool flush_work(struct work_struct *work)
{
    // wait until it is neither queued nor running. (satoru)
    bool waited = false;
    for (int guard = 0; guard < 100000; guard++) {
        struct workqueue_struct *on = NULL;
        bool queued = test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
        for (struct workqueue_struct *wq = lx_all_wqs; wq; wq = wq->next_wq)
            if (__atomic_load_n(&wq->running, __ATOMIC_ACQUIRE) == work) on = wq;
        if (!queued && !on)
            return waited;
        waited = true;
        if (on)
            lx_work_wait_idle(on, work);
        else
            lx_os_sleep_ns(200000);
    }
    return waited;
}

bool flush_delayed_work(struct delayed_work *dwork)
{
    if (timer_delete_sync(&dwork->timer))
        lx_wq_insert(dwork->wq, &dwork->work);
    return flush_work(&dwork->work);
}

void __flush_workqueue(struct workqueue_struct *wq)
{
    if (!wq || lx_current() == wq->task)
        return;
    for (;;) {
        lx_flock(&wq->lock);
        int busy = !list_empty(&wq->list) || wq->running;
        unsigned int d = wq->done;
        lx_funlock(&wq->lock);
        if (!busy)
            return;
        lx_os_futex_wait(&wq->done, d, 1000000);
    }
}

__printf(1, 4) struct workqueue_struct *
alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
    va_list ap;
    struct workqueue_struct *wq = lx_os_malloc(sizeof(*wq));
    (void)flags; (void)max_active;
    if (!wq)
        return NULL;
    memset(wq, 0, sizeof(*wq));
    INIT_LIST_HEAD(&wq->list);
    va_start(ap, max_active);
    vsnprintf(wq->name, sizeof(wq->name), fmt, ap);
    va_end(ap);
    lx_flock(&lx_wq_global);
    wq->next_wq = lx_all_wqs;
    lx_all_wqs = wq;
    lx_funlock(&lx_wq_global);
    return wq;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
    // drained, then left allocated: its worker thread parks forever. stopping
    // and joining a worker that may be blocked in driver code is not worth the
    // risk for a teardown path. (satoru)
    __flush_workqueue(wq);
}

// end (satoru)
