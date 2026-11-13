// lx_emul: synchronisation, for real.
//
// The container is MULTI-THREADED now (irq threads, the napi thread, the timer
// thread, workqueue workers, kthreads), so every Linux lock is a real lock. All
// of them are built on one primitive: a 3-state futex mutex on a 32-bit word
// (Drepper, "Futexes are tricky", mutex #2): 0 free, 1 locked, 2 locked with
// waiters.
//
// Mapping of the Linux lock types onto it, using storage the real structs
// already have, so static initialisers (__SPIN_LOCK_UNLOCKED, DEFINE_MUTEX)
// still mean "unlocked" because they are all zero:
//   spinlock_t / raw_spinlock_t : the qspinlock's 32-bit val
//   struct mutex                : the low 32 bits of owner
//   completion                  : done is the counter, waiters park on it
//   wait_queue_head_t           : its spinlock + list, entries name lx_tasks
//
// "irqsave" means "exclude the interrupt handler". In a container the handler
// runs on an irq THREAD, so taking the lock is exactly that exclusion; the
// returned flags are a token. There is no "irqs off on this cpu" state to
// emulate because nothing in the container is ever interrupted by its own
// handler.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/swait.h>
#include <linux/rtnetlink.h>
#include <linux/jiffies.h>
#include <linux/rcupdate.h>
#include <linux/preempt.h>

#include "lx_kshim.h"

// ── the primitive ───────────────────────────────────────────────────────────
static inline unsigned int lx_cas(volatile unsigned int *w, unsigned int o, unsigned int n)
{
    __atomic_compare_exchange_n(w, &o, n, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    return o;
}

void lx_flock(volatile unsigned int *w)
{
    unsigned int c = lx_cas(w, 0, 1);
    if (c == 0)
        return;
    // a short spin first: most driver critical sections are a few register
    // accesses, and a futex round trip costs far more than that. (satoru)
    for (int i = 0; i < 100; i++) {
        __builtin_ia32_pause();
        c = lx_cas(w, 0, 1);
        if (c == 0)
            return;
    }
    if (c != 2)
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        lx_os_futex_wait(w, 2, -1);
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    }
}

void lx_funlock(volatile unsigned int *w)
{
    if (__atomic_fetch_sub(w, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(w, 0, __ATOMIC_RELEASE);
        lx_os_futex_wake(w, 1);
    }
}

int lx_ftrylock(volatile unsigned int *w)
{
    return lx_cas(w, 0, 1) == 0;
}

#define LX_SPINW(l) ((volatile unsigned int *)&(l)->raw_lock.val.counter)

// ── spinlocks ───────────────────────────────────────────────────────────────
// a held spinlock is an RCU read-side section in Linux: preemption is off and
// call_rcu() must not free memory the holder can still reach. (satoru)
// the preempt count is per thread now (see shim-include/asm/preempt.h), so
// these out-of-line lock paths must maintain it exactly like
// kernel/locking/spinlock.c does, or a lock/unlock pair leaks count. (satoru)
void _raw_spin_lock(raw_spinlock_t *lock)
{
    preempt_disable();
    __rcu_read_lock();
    lx_flock(LX_SPINW(lock));
}
void _raw_spin_unlock(raw_spinlock_t *lock)
{
    lx_funlock(LX_SPINW(lock));
    __rcu_read_unlock();
    preempt_enable();
}
void _raw_spin_lock_irq(raw_spinlock_t *lock)
{
    preempt_disable();
    __rcu_read_lock();
    lx_flock(LX_SPINW(lock));
}
void _raw_spin_unlock_irq(raw_spinlock_t *lock)
{
    lx_funlock(LX_SPINW(lock));
    __rcu_read_unlock();
    preempt_enable();
}
void _raw_spin_lock_bh(raw_spinlock_t *lock)
{
    preempt_count_add(SOFTIRQ_LOCK_OFFSET);
    __rcu_read_lock();
    lx_flock(LX_SPINW(lock));
}
void _raw_spin_unlock_bh(raw_spinlock_t *lock)
{
    lx_funlock(LX_SPINW(lock));
    __rcu_read_unlock();
    preempt_count_sub(SOFTIRQ_LOCK_OFFSET);
}
int  _raw_spin_trylock(raw_spinlock_t *lock)
{
    preempt_disable();
    if (!lx_ftrylock(LX_SPINW(lock))) {
        preempt_enable();
        return 0;
    }
    __rcu_read_lock();
    return 1;
}

int _raw_spin_trylock_bh(raw_spinlock_t *lock)
{
    preempt_count_add(SOFTIRQ_LOCK_OFFSET);
    if (!lx_ftrylock(LX_SPINW(lock))) {
        preempt_count_sub(SOFTIRQ_LOCK_OFFSET);
        return 0;
    }
    __rcu_read_lock();
    return 1;
}

unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
    preempt_disable();
    __rcu_read_lock();
    lx_flock(LX_SPINW(lock));
    return 0x200;   // "interrupts were on" - the value is only ever handed back (satoru)
}

void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
    (void)flags;
    lx_funlock(LX_SPINW(lock));
    __rcu_read_unlock();
    preempt_enable();
}

// rwlock_t: the qrwlock's cnts word is the futex. readers serialize on it
// (correct, if not scalable) and the whole acquisition is an RCU read section.
#define LX_RWW(l) ((volatile unsigned int *)&(l)->raw_lock.cnts.counter)

void _raw_read_lock(rwlock_t *lock)
{
    preempt_disable();
    __rcu_read_lock();
    lx_flock(LX_RWW(lock));
}
void _raw_read_unlock(rwlock_t *lock)
{
    lx_funlock(LX_RWW(lock));
    __rcu_read_unlock();
    preempt_enable();
}
void _raw_write_lock(rwlock_t *lock)
{
    preempt_disable();
    __rcu_read_lock();
    lx_flock(LX_RWW(lock));
}
void _raw_write_unlock(rwlock_t *lock)
{
    lx_funlock(LX_RWW(lock));
    __rcu_read_unlock();
    preempt_enable();
}
void _raw_read_lock_bh(rwlock_t *lock)
{
    preempt_count_add(SOFTIRQ_LOCK_OFFSET);
    __rcu_read_lock();
    lx_flock(LX_RWW(lock));
}
void _raw_read_unlock_bh(rwlock_t *lock)
{
    lx_funlock(LX_RWW(lock));
    __rcu_read_unlock();
    preempt_count_sub(SOFTIRQ_LOCK_OFFSET);
}
void _raw_write_lock_bh(rwlock_t *lock)
{
    preempt_count_add(SOFTIRQ_LOCK_OFFSET);
    __rcu_read_lock();
    lx_flock(LX_RWW(lock));
}
void _raw_write_unlock_bh(rwlock_t *lock)
{
    lx_funlock(LX_RWW(lock));
    __rcu_read_unlock();
    preempt_count_sub(SOFTIRQ_LOCK_OFFSET);
}
int  _raw_read_trylock(rwlock_t *lock)
{
    preempt_disable();
    if (!lx_ftrylock(LX_RWW(lock))) {
        preempt_enable();
        return 0;
    }
    __rcu_read_lock();
    return 1;
}
int  _raw_write_trylock(rwlock_t *lock)
{
    preempt_disable();
    if (!lx_ftrylock(LX_RWW(lock))) {
        preempt_enable();
        return 0;
    }
    __rcu_read_lock();
    return 1;
}

// ── mutexes ─────────────────────────────────────────────────────────────────
#define LX_MUTW(m) ((volatile unsigned int *)&(m)->owner.counter)

void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
    (void)name; (void)key;
    atomic_long_set(&lock->owner, 0);
    INIT_LIST_HEAD(&lock->wait_list);
}

void mutex_lock(struct mutex *lock)        { lx_flock(LX_MUTW(lock)); }
void mutex_unlock(struct mutex *lock)      { lx_funlock(LX_MUTW(lock)); }
int  mutex_trylock(struct mutex *lock)     { return lx_ftrylock(LX_MUTW(lock)); }
int  mutex_lock_interruptible(struct mutex *lock) { lx_flock(LX_MUTW(lock)); return 0; }
int  mutex_lock_killable(struct mutex *lock)      { lx_flock(LX_MUTW(lock)); return 0; }

// ── rtnl ────────────────────────────────────────────────────────────────────
// the routing-netlink lock serialises netdev configuration. there is no netlink
// in the container, but the container itself configures the netdev from more
// than one thread (bring-up vs the driver's reset task), so it is a real
// mutex. (satoru)
static DEFINE_MUTEX(lx_rtnl_mutex);
void rtnl_lock(void)   { mutex_lock(&lx_rtnl_mutex); }
void rtnl_unlock(void) { mutex_unlock(&lx_rtnl_mutex); }
int  rtnl_trylock(void) { return mutex_trylock(&lx_rtnl_mutex); }
int  rtnl_is_locked(void) { return atomic_long_read(&lx_rtnl_mutex.owner) != 0; }

// ── completions ─────────────────────────────────────────────────────────────
// done counts completions; complete_all sets UINT_MAX like upstream. waiters
// park on the done word itself, so a complete() is visible to the futex even
// with no list bookkeeping. (satoru)
#define LX_DONEW(x) ((volatile unsigned int *)&(x)->done)

void __init_swait_queue_head(struct swait_queue_head *q, const char *name,
                             struct lock_class_key *key)
{
    (void)name; (void)key;
    raw_spin_lock_init(&q->lock);
    INIT_LIST_HEAD(&q->task_list);
}

void complete(struct completion *x)
{
    unsigned int d = __atomic_load_n(LX_DONEW(x), __ATOMIC_RELAXED);
    for (;;) {
        unsigned int n = (d == UINT_MAX) ? d : d + 1;
        if (__atomic_compare_exchange_n(LX_DONEW(x), &d, n, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            break;
    }
    lx_os_futex_wake(LX_DONEW(x), 1);
}

void complete_all(struct completion *x)
{
    __atomic_store_n(LX_DONEW(x), UINT_MAX, __ATOMIC_RELEASE);
    lx_os_futex_wake(LX_DONEW(x), 0x7fffffff);
}

// consume one completion; timeout_ns < 0 = forever. returns 1 if consumed.
static int lx_wait_done(struct completion *x, long long timeout_ns)
{
    unsigned long long end = timeout_ns >= 0 ? lx_os_now_ns() + (unsigned long long)timeout_ns : 0;
    for (;;) {
        unsigned int d = __atomic_load_n(LX_DONEW(x), __ATOMIC_ACQUIRE);
        if (d) {
            if (d == UINT_MAX)
                return 1;
            if (__atomic_compare_exchange_n(LX_DONEW(x), &d, d - 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return 1;
            continue;
        }
        long long left = -1;
        if (timeout_ns >= 0) {
            unsigned long long now = lx_os_now_ns();
            if (now >= end)
                return 0;
            left = (long long)(end - now);
        }
        lx_os_futex_wait(LX_DONEW(x), 0, left);
    }
}

void wait_for_completion(struct completion *x) { lx_wait_done(x, -1); }

unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
    unsigned long long t0 = lx_os_now_ns();
    if (!lx_wait_done(x, (long long)jiffies_to_usecs(timeout) * 1000ll))
        return 0;
    unsigned long used = usecs_to_jiffies((unsigned int)((lx_os_now_ns() - t0) / 1000ull));
    return used >= timeout ? 1 : timeout - used;
}

int wait_for_completion_interruptible(struct completion *x) { lx_wait_done(x, -1); return 0; }

long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout)
{
    return (long)wait_for_completion_timeout(x, timeout);
}

bool try_wait_for_completion(struct completion *x) { return lx_wait_done(x, 0) != 0; }

bool completion_done(struct completion *x)
{
    return __atomic_load_n(LX_DONEW(x), __ATOMIC_ACQUIRE) != 0;
}

// ── wait queues ─────────────────────────────────────────────────────────────
// the entry's private names the waiting task. DEFINE_WAIT-style inline
// initialisers store `current`, which in the container is a shared dummy, so
// prepare_to_wait*() always re-stamp it with the real calling task. (satoru)
void __init_waitqueue_head(struct wait_queue_head *wq_head, const char *name,
                           struct lock_class_key *key)
{
    (void)name; (void)key;
    spin_lock_init(&wq_head->lock);
    INIT_LIST_HEAD(&wq_head->head);
}

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int wake_flags, void *key)
{
    (void)mode; (void)wake_flags; (void)key;
    struct lx_task *t = lx_task_of((struct task_struct *)wq_entry->private);
    return t ? lx_task_wake(t) : 0;
}

int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
    int ret = default_wake_function(wq_entry, mode, sync, key);
    if (ret)
        list_del_init_careful(&wq_entry->entry);
    return ret;
}

void init_wait_entry(struct wait_queue_entry *wq_entry, int flags)
{
    wq_entry->flags = flags;
    wq_entry->private = lx_task_ts(lx_current());
    wq_entry->func = autoremove_wake_function;
    INIT_LIST_HEAD(&wq_entry->entry);
}

long prepare_to_wait_event(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
    unsigned long flags;
    (void)state;
    struct task_struct *ts = lx_task_ts(lx_current());
    wq_entry->private = ts;
    if (ts)
        WRITE_ONCE(ts->__state, state);
    spin_lock_irqsave(&wq_head->lock, flags);
    if (list_empty(&wq_entry->entry)) {
        if (wq_entry->flags & WQ_FLAG_EXCLUSIVE)
            list_add_tail(&wq_entry->entry, &wq_head->head);
        else
            list_add(&wq_entry->entry, &wq_head->head);
    }
    spin_unlock_irqrestore(&wq_head->lock, flags);
    return 0;   // no signals in a container (satoru)
}

void prepare_to_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
    wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
    prepare_to_wait_event(wq_head, wq_entry, state);
}

bool prepare_to_wait_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
    wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
    prepare_to_wait_event(wq_head, wq_entry, state);
    return true;
}

void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
    unsigned long flags;
    struct task_struct *ts = lx_task_ts(lx_current());
    if (ts)
        WRITE_ONCE(ts->__state, TASK_RUNNING);
    if (!list_empty_careful(&wq_entry->entry)) {
        spin_lock_irqsave(&wq_head->lock, flags);
        list_del_init(&wq_entry->entry);
        spin_unlock_irqrestore(&wq_head->lock, flags);
    }
}

void add_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
    unsigned long flags;
    wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
    spin_lock_irqsave(&wq_head->lock, flags);
    list_add(&wq_entry->entry, &wq_head->head);
    spin_unlock_irqrestore(&wq_head->lock, flags);
}

void remove_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
    finish_wait(wq_head, wq_entry);
}

static int lx_wake_common(struct wait_queue_head *wq_head, unsigned int mode, int nr, void *key)
{
    struct wait_queue_entry *curr, *next;
    unsigned long flags;
    int woken = 0;
    spin_lock_irqsave(&wq_head->lock, flags);
    list_for_each_entry_safe(curr, next, &wq_head->head, entry) {
        unsigned int fl = curr->flags;
        int ret = curr->func(curr, mode, 0, key);
        if (ret < 0)
            break;
        if (ret)
            woken++;
        if (ret && (fl & WQ_FLAG_EXCLUSIVE) && !--nr)
            break;
    }
    spin_unlock_irqrestore(&wq_head->lock, flags);
    return woken;
}

int __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr_exclusive, void *key)
{
    return lx_wake_common(wq_head, mode, nr_exclusive ? nr_exclusive : 0x7fffffff, key);
}

void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr)
{
    struct wait_queue_entry *curr, *next;
    list_for_each_entry_safe(curr, next, &wq_head->head, entry) {
        unsigned int fl = curr->flags;
        int ret = curr->func(curr, mode, 0, NULL);
        if (ret && (fl & WQ_FLAG_EXCLUSIVE) && !--nr)
            break;
    }
}

// ── schedule ────────────────────────────────────────────────────────────────
// park the calling task until a wake_up_process / wait-queue wake. a wake that
// lands between prepare_to_wait and schedule is NOT lost: the task's wake word
// is sticky, so schedule returns at once and the caller's loop re-tests its
// condition - the same contract as Linux's TASK_RUNNING race. (satoru)
asmlinkage __visible void schedule(void)
{
    // a bare schedule() with task state TASK_RUNNING is a resched point, not a
    // sleep. parking forever there stopped test_xarray (M1); a bare yield is
    // not enough either: in a tight loop it is a yield-spinner for the kernel
    // picker and starves the woken thread it is waiting for (M9/N1). block for
    // one poll tick instead: bounded, never early, and other threads RUN.
    // (satoru)
    struct task_struct *ts = lx_task_ts(lx_current());
    if (!ts || READ_ONCE(ts->__state) == TASK_RUNNING) {
        lx_os_idle_ms(1);
        lx_jiffies_update();
        return;
    }
    lx_task_park(-1);
    WRITE_ONCE(ts->__state, TASK_RUNNING);
}

long schedule_timeout(long timeout)
{
    if (timeout == MAX_SCHEDULE_TIMEOUT) {
        lx_task_park(-1);
        return timeout;
    }
    if (timeout <= 0)
        return 0;
    unsigned long long t0 = lx_os_now_ns();
    lx_task_park((long long)jiffies_to_usecs((unsigned long)timeout) * 1000ll);
    unsigned long used = usecs_to_jiffies((unsigned int)((lx_os_now_ns() - t0) / 1000ull));
    return used >= (unsigned long)timeout ? 0 : timeout - (long)used;
}

long schedule_timeout_uninterruptible(long timeout) { return schedule_timeout(timeout); }
long schedule_timeout_interruptible(long timeout)   { return schedule_timeout(timeout); }

int wake_up_process(struct task_struct *p)
{
    struct lx_task *t = lx_task_of(p);
    if (!t)
        return 0;
    lx_sched_wake_start(p);   // a fresh kthread starts on its first wake (satoru)
    return lx_task_wake(t);
}

// NOTE: the __SCT__* static-call trampolines live in lx_emul/lx_sc.c, not here.
// They have a declared static-call type in the kernel headers that conflicts
// with a plain void(void) definition, so they must be defined OUTSIDE kernel
// context. (satoru)

// end (satoru)
