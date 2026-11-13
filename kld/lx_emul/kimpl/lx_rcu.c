// lx_emul: userspace RCU and SRCU with real grace periods.
//
// Classic RCU uses the standard userspace-RCU scheme, not a global "readers
// are zero" counter. a grace period never waits for a reader that starts after
// it began, so two long overlapping read sections cannot starve it:
//
//   gp starts at 1. a thread entering its outermost read section snapshots gp.
//   synchronize_rcu() increments gp under a mutex and waits only for snapshots
//   that are nonzero and OLDER than the new gp. a reader that starts after the
//   increment sees the new value and is not waited for.
//
// The per-thread record lives in a small registry keyed by the shim's lx_task;
// lx_sched.c registers every thread it creates and removes it on exit.
//
// Callbacks run on ONE dedicated lx_rcu kthread, never on system_wq: a work
// item is allowed to call_rcu() and then rcu_barrier() without deadlocking, and
// a callback cannot occupy a workqueue worker. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

#include "lx_kshim.h"

// ── per-thread registry ─────────────────────────────────────────────────────
struct lx_rcu_thread {
    struct lx_task *task;
    volatile uint32_t used;
    volatile uint32_t nesting;
    volatile uint32_t snapshot;
};

#define LX_RCU_THREADS 128
static struct lx_rcu_thread g_rcu_threads[LX_RCU_THREADS];
static volatile unsigned int g_rcu_reg_lock;
static __thread struct lx_rcu_thread *lx_rcu_self;

static struct lx_rcu_thread *lx_rcu_lookup(struct lx_task *t, bool create)
{
    lx_flock(&g_rcu_reg_lock);
    for (int i = 0; i < LX_RCU_THREADS; i++) {
        struct lx_rcu_thread *e = &g_rcu_threads[i];
        if (__atomic_load_n(&e->used, __ATOMIC_ACQUIRE) && e->task == t) {
            lx_funlock(&g_rcu_reg_lock);
            return e;
        }
    }
    if (!create) {
        lx_funlock(&g_rcu_reg_lock);
        return NULL;
    }
    for (int i = 0; i < LX_RCU_THREADS; i++) {
        struct lx_rcu_thread *e = &g_rcu_threads[i];
        if (!__atomic_load_n(&e->used, __ATOMIC_ACQUIRE)) {
            e->task = t;
            e->nesting = 0;
            e->snapshot = 0;
            __atomic_store_n(&e->used, 1, __ATOMIC_RELEASE);
            lx_funlock(&g_rcu_reg_lock);
            return e;
        }
    }
    lx_funlock(&g_rcu_reg_lock);
    lx_log("[lx_emul] rcu registry full - reader left unprotected\n");
    return NULL;
}

void lx_rcu_thread_register(struct lx_task *t)
{
    if (!t)
        return;
    if (!lx_rcu_self)
        lx_rcu_self = lx_rcu_lookup(t, true);
}

void lx_rcu_thread_unregister(struct lx_task *t)
{
    struct lx_rcu_thread *e = lx_rcu_lookup(t, false);
    if (!e)
        return;
    __atomic_store_n(&e->snapshot, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&e->nesting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&e->used, 0, __ATOMIC_RELEASE);
    e->task = NULL;
}

static struct lx_rcu_thread *lx_rcu_mine(void)
{
    if (lx_rcu_self)
        return lx_rcu_self;
    lx_rcu_self = lx_rcu_lookup(lx_current(), true);
    return lx_rcu_self;
}

// ── classic RCU ─────────────────────────────────────────────────────────────
static uint32_t g_gp = 1;
static volatile unsigned int g_gp_lock;
static volatile uint32_t g_rcu_waiters;

static inline void lx_rcu_wake_waiters(void)
{
    if (__atomic_load_n(&g_rcu_waiters, __ATOMIC_RELAXED))
        lx_os_futex_wake((volatile unsigned int *)&g_rcu_waiters, 0x7fffffff);
}

void __rcu_read_lock(void)
{
    struct lx_rcu_thread *e = lx_rcu_mine();
    if (!e)
        return;
    if (e->nesting++ == 0) {
        uint32_t gp = __atomic_load_n(&g_gp, __ATOMIC_RELAXED);
        __atomic_store_n(&e->snapshot, gp, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
}

void __rcu_read_unlock(void)
{
    struct lx_rcu_thread *e = lx_rcu_mine();
    if (!e)
        return;
    if (--e->nesting == 0) {
        __atomic_store_n(&e->snapshot, 0, __ATOMIC_SEQ_CST);
        lx_rcu_wake_waiters();
    }
}

void synchronize_rcu(void)
{
    lx_flock(&g_gp_lock);
    uint32_t new_gp = __atomic_add_fetch(&g_gp, 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct lx_task *self = lx_current();
    for (int i = 0; i < LX_RCU_THREADS; i++) {
        struct lx_rcu_thread *e = &g_rcu_threads[i];
        if (!__atomic_load_n(&e->used, __ATOMIC_ACQUIRE))
            continue;
        if (e->task == self)
            continue;
        for (;;) {
            uint32_t snap = __atomic_load_n(&e->snapshot, __ATOMIC_SEQ_CST);
            if (!snap || snap >= new_gp)
                break;
            __atomic_fetch_add(&g_rcu_waiters, 1, __ATOMIC_ACQ_REL);
            uint32_t w = __atomic_load_n(&g_rcu_waiters, __ATOMIC_RELAXED);
            lx_os_futex_wait((volatile unsigned int *)&g_rcu_waiters, w, 1000000);
            __atomic_fetch_sub(&g_rcu_waiters, 1, __ATOMIC_ACQ_REL);
        }
    }
    lx_funlock(&g_gp_lock);
}

void synchronize_rcu_expedited(void) { synchronize_rcu(); }

// ── callbacks: one dedicated kthread ────────────────────────────────────────
struct lx_srcu;
struct lx_srcu {
    struct srcu_struct *ssp;
    volatile unsigned int readers;
    volatile unsigned int pending;
    volatile unsigned int waiters;
    volatile unsigned int lock;
};

struct lx_rcu_cb {
    struct lx_rcu_cb *next;
    struct rcu_head *head;
    rcu_callback_t func;
    void *free_ptr;
    struct lx_srcu *srcu;
    volatile int done;
    bool is_barrier;
    bool on_stack;
};

static struct lx_rcu_cb *g_rcu_head;
static struct lx_rcu_cb *g_rcu_tail;
static volatile unsigned int g_rcu_list_lock;
static volatile unsigned int g_rcu_kick;
static struct lx_task *g_rcu_task;

static int lx_rcu_thread(void *arg);

static void lx_rcu_enqueue(struct lx_rcu_cb *c)
{
    if (!g_rcu_task) {
        lx_flock(&g_rcu_list_lock);
        if (!g_rcu_task)
            g_rcu_task = lx_task_spawn("lx_rcu", lx_rcu_thread, NULL);
        lx_funlock(&g_rcu_list_lock);
    }
    c->next = NULL;
    lx_flock(&g_rcu_list_lock);
    if (g_rcu_tail)
        g_rcu_tail->next = c;
    else
        g_rcu_head = c;
    g_rcu_tail = c;
    lx_funlock(&g_rcu_list_lock);
    __atomic_fetch_add(&g_rcu_kick, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&g_rcu_kick, 1);
}

static int lx_rcu_thread(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned int kick = __atomic_load_n(&g_rcu_kick, __ATOMIC_ACQUIRE);
        struct lx_rcu_cb *list;
        lx_flock(&g_rcu_list_lock);
        list = g_rcu_head;
        g_rcu_head = g_rcu_tail = NULL;
        lx_funlock(&g_rcu_list_lock);
        if (!list) {
            lx_os_futex_wait(&g_rcu_kick, kick, -1);
            continue;
        }
        synchronize_rcu();
        while (list) {
            struct lx_rcu_cb *c = list;
            list = list->next;
            if (c->srcu)
                synchronize_srcu(c->srcu->ssp);
            if (c->free_ptr)
                kvfree(c->free_ptr);
            else if (c->func)
                c->func(c->head);
            if (c->srcu &&
                __atomic_fetch_sub(&c->srcu->pending, 1, __ATOMIC_ACQ_REL) == 1)
                lx_os_futex_wake(&c->srcu->pending, 0x7fffffff);
            if (c->is_barrier) {
                // the waiter may return and reuse its stack as soon as the wake
                // lands, so read on_stack before waking and never touch c again
                // after the wake. (satoru)
                bool on_stack = c->on_stack;
                __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
                lx_os_futex_wake((volatile unsigned int *)&c->done, 0x7fffffff);
                if (!on_stack)
                    lx_os_free(c);
                continue;
            }
            if (!c->on_stack)
                lx_os_free(c);
        }
    }
    return 0;
}

static void lx_rcu_call(struct rcu_head *head, rcu_callback_t func, void *free_ptr,
                        struct lx_srcu *srcu, bool srcu_cb)
{
    struct lx_rcu_cb *c = lx_os_malloc(sizeof(*c));
    if (!c) {
        synchronize_rcu();
        if (free_ptr)
            kvfree(free_ptr);
        else if (func)
            func(head);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->head = head;
    c->func = func;
    c->free_ptr = free_ptr;
    c->srcu = srcu_cb ? srcu : NULL;
    lx_rcu_enqueue(c);
}

void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
    lx_rcu_call(head, func, NULL, NULL, false);
}

void kvfree_call_rcu(struct rcu_head *head, void *ptr)
{
    (void)head;
    if (ptr)
        lx_rcu_call(NULL, NULL, ptr, NULL, false);
}

void rcu_barrier(void)
{
    struct lx_rcu_cb c;
    memset(&c, 0, sizeof(c));
    c.is_barrier = true;
    c.on_stack = true;
    lx_rcu_enqueue(&c);
    while (!__atomic_load_n(&c.done, __ATOMIC_ACQUIRE))
        lx_os_futex_wait((volatile unsigned int *)&c.done, 0, 1000000000ll);
}

// ── SRCU ────────────────────────────────────────────────────────────────────
// readers may sleep and are rare, so a per-struct counter is fine. the wake is
// waiter-gated exactly like classic RCU, and callbacks run on the lx_rcu
// thread. (satoru)

static struct lx_srcu g_srcu[32];
static volatile unsigned int g_srcu_init_log;

static struct lx_srcu *lx_srcu_find(struct srcu_struct *ssp, bool create)
{
    lx_flock(&g_srcu[0].lock);   // one table lock, entry 0's word (satoru)
    for (int i = 0; i < (int)(sizeof(g_srcu) / sizeof(g_srcu[0])); i++) {
        if (g_srcu[i].ssp == ssp) {
            lx_funlock(&g_srcu[0].lock);
            return &g_srcu[i];
        }
    }
    if (create) {
        for (int i = 0; i < (int)(sizeof(g_srcu) / sizeof(g_srcu[0])); i++) {
            if (!g_srcu[i].ssp) {
                g_srcu[i].ssp = ssp;
                g_srcu[i].readers = 0;
                g_srcu[i].pending = 0;
                g_srcu[i].waiters = 0;
                lx_funlock(&g_srcu[0].lock);
                return &g_srcu[i];
            }
        }
        if (!g_srcu_init_log) {
            g_srcu_init_log = 1;
            lx_log("[lx_emul] srcu table full - reader left unprotected\n");
        }
    }
    lx_funlock(&g_srcu[0].lock);
    return NULL;
}

int init_srcu_struct(struct srcu_struct *ssp)
{
    return lx_srcu_find(ssp, true) ? 0 : -ENOMEM;
}

void cleanup_srcu_struct(struct srcu_struct *ssp)
{
    struct lx_srcu *s = lx_srcu_find(ssp, false);
    if (!s)
        return;
    synchronize_srcu(ssp);
    lx_flock(&g_srcu[0].lock);
    s->ssp = NULL;
    s->readers = 0;
    s->pending = 0;
    s->waiters = 0;
    lx_funlock(&g_srcu[0].lock);
}

int __srcu_read_lock(struct srcu_struct *ssp)
{
    struct lx_srcu *s = lx_srcu_find(ssp, true);
    if (!s)
        return 0;
    __atomic_fetch_add(&s->readers, 1, __ATOMIC_ACQ_REL);
    return 0;
}

void __srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
    struct lx_srcu *s = lx_srcu_find(ssp, false);
    (void)idx;
    if (!s)
        return;
    if (__atomic_fetch_sub(&s->readers, 1, __ATOMIC_ACQ_REL) == 1 &&
        __atomic_load_n(&s->waiters, __ATOMIC_RELAXED))
        lx_os_futex_wake(&s->waiters, 0x7fffffff);
}

void synchronize_srcu(struct srcu_struct *ssp)
{
    struct lx_srcu *s = lx_srcu_find(ssp, true);
    if (!s)
        return;
    for (;;) {
        unsigned int r = __atomic_load_n(&s->readers, __ATOMIC_ACQUIRE);
        if (!r)
            return;
        __atomic_fetch_add(&s->waiters, 1, __ATOMIC_ACQ_REL);
        unsigned int w = __atomic_load_n(&s->waiters, __ATOMIC_RELAXED);
        lx_os_futex_wait(&s->waiters, w, 1000000);
        __atomic_fetch_sub(&s->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

void call_srcu(struct srcu_struct *ssp, struct rcu_head *head, rcu_callback_t func)
{
    struct lx_srcu *s = lx_srcu_find(ssp, true);
    if (s)
        __atomic_fetch_add(&s->pending, 1, __ATOMIC_ACQ_REL);
    lx_rcu_call(head, func, NULL, s, true);
}

void srcu_barrier(struct srcu_struct *ssp)
{
    struct lx_srcu *s = lx_srcu_find(ssp, true);
    if (!s)
        return;
    for (;;) {
        unsigned int p = __atomic_load_n(&s->pending, __ATOMIC_ACQUIRE);
        if (!p) {
            rcu_barrier();
            return;
        }
        lx_os_futex_wait(&s->pending, p, 1000000000ll);
    }
}

// end (satoru)
