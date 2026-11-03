// lx_emul: synchronisation.
//
// The container is SINGLE-THREADED by design, so these are genuine no-ops
// rather than lazy ones. That is a real architectural decision and it needs to
// stay true: the moment the container spawns a second thread (a kthread, a
// threaded IRQ handler, a workqueue worker on its own thread) every one of these
// becomes a correctness bug, not a shortcut.
//
// If threading is introduced in phase 2, these must become real locks FIRST.
// Leaving a comment is not enough, so lx_sync_assert_single_threaded() below is
// wired into the places that would create a thread.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/sched.h>

#include "lx_kshim.h"

// flipped the first time anything tries to create a thread. (satoru)
static int lx_threads_created;

void lx_sync_note_thread_created(void)
{
    lx_threads_created++;
    lx_log("[lx_emul] WARNING: a second thread was created, but the lock shims\n"
           "[lx_emul]          are no-ops (single-threaded assumption). Real\n"
           "[lx_emul]          locks are required before this is correct.\n");
}

int lx_sync_is_single_threaded(void)
{
    return lx_threads_created == 0;
}

// ── spinlocks ───────────────────────────────────────────────────────────────
// no contention is possible with one thread. the _irqsave variants must still
// produce a plausible flags value because the driver stores and restores it.
void _raw_spin_lock(raw_spinlock_t *lock)                  { (void)lock; }
void _raw_spin_unlock(raw_spinlock_t *lock)                { (void)lock; }

unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
    (void)lock;
    return 0;
}

void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
    (void)lock;
    (void)flags;
}

// ── mutexes ─────────────────────────────────────────────────────────────────
void mutex_lock(struct mutex *lock)   { (void)lock; }
void mutex_unlock(struct mutex *lock) { (void)lock; }

// ── rtnl ────────────────────────────────────────────────────────────────────
// the routing-netlink lock serialises netdev registration against userspace
// netlink. There is no netlink in the container, so nothing to serialise. (satoru)
void rtnl_lock(void)   { }
void rtnl_unlock(void) { }

// NOTE: the __SCT__* static-call trampolines live in lx_emul/lx_sc.c, not here.
// They have a declared static-call type in the kernel headers that conflicts
// with a plain void(void) definition, so they must be defined OUTSIDE kernel
// context. They are real no-ops there rather than aborting stubs, which matters
// because cond_resched() calls through them. (satoru)

// end (satoru)
