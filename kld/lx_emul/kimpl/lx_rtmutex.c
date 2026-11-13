// lx_rtmutex: the rt_mutex subset i2c-core uses for the adapter bus lock.
//
// the container has no PI scheduler; the lock is the same futex mutex the rest
// of the shim uses, stored in the rt_mutex's own wait_lock word so no side
// table is needed. i2c transfers never nest, which is the only user here.
// (satoru)

#include <linux/kernel.h>
#include <linux/rtmutex.h>
#include <linux/sched.h>

#include "lx_kshim.h"

#define LX_RTMW(lock) ((volatile unsigned int *)&(lock)->rtmutex.wait_lock.raw_lock.val.counter)

void __rt_mutex_init(struct rt_mutex *lock, const char *name,
                     struct lock_class_key *key)
{
    (void)name; (void)key;
    memset(lock, 0, sizeof(*lock));
    raw_spin_lock_init(&lock->rtmutex.wait_lock);
}

void rt_mutex_lock(struct rt_mutex *lock)
{
    lx_flock(LX_RTMW(lock));
}

int rt_mutex_trylock(struct rt_mutex *lock)
{
    return lx_ftrylock(LX_RTMW(lock));
}

void rt_mutex_unlock(struct rt_mutex *lock)
{
    lx_funlock(LX_RTMW(lock));
}

// end (satoru)
