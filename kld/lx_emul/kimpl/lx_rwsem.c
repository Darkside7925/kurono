// lx_emul: rwsem, a real reader/writer lock on the shim futex primitive.
//
// state in sem->count:
//   > 0  number of readers
//   = 0  free
//   < 0  a writer owns it
//
// readers may enter while the count is non-negative; a writer may enter only
// at zero. every transition to zero wakes all waiters. that is enough for the
// driver's "many readers, one writer" use and gives the apitest its writer
// exclusion invariant under contention. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/rwsem.h>
#include <linux/sched.h>

#include "lx_kshim.h"

#define LX_RWSEM_W(sem) ((volatile unsigned int *)&(sem)->count.counter)

static void lx_rwsem_wake(struct rw_semaphore *sem)
{
    lx_os_futex_wake(LX_RWSEM_W(sem), 0x7fffffff);
}

void __init_rwsem(struct rw_semaphore *sem, const char *name,
                  struct lock_class_key *key)
{
    (void)name; (void)key;
    atomic_long_set(&sem->count, 0);
    atomic_long_set(&sem->owner, 0);
    raw_spin_lock_init(&sem->wait_lock);
    INIT_LIST_HEAD(&sem->wait_list);
}

void down_read(struct rw_semaphore *sem)
{
    for (;;) {
        long c = atomic_long_read(&sem->count);
        if (c >= 0) {
            if (atomic_long_try_cmpxchg_acquire(&sem->count, &c, c + 1))
                return;
            continue;
        }
        lx_os_futex_wait(LX_RWSEM_W(sem), (unsigned int)c, -1);
    }
}

int down_read_trylock(struct rw_semaphore *sem)
{
    long c = atomic_long_read(&sem->count);
    while (c >= 0) {
        if (atomic_long_try_cmpxchg_acquire(&sem->count, &c, c + 1))
            return 1;
    }
    return 0;
}

void down_write(struct rw_semaphore *sem)
{
    for (;;) {
        long c = 0;
        if (atomic_long_try_cmpxchg_acquire(&sem->count, &c, -1))
            return;
        lx_os_futex_wait(LX_RWSEM_W(sem), (unsigned int)c, -1);
    }
}

int down_write_trylock(struct rw_semaphore *sem)
{
    long c = 0;
    return atomic_long_try_cmpxchg_acquire(&sem->count, &c, -1) ? 1 : 0;
}

void up_read(struct rw_semaphore *sem)
{
    if (atomic_long_sub_return_release(1, &sem->count) == 0)
        lx_rwsem_wake(sem);
}

void up_write(struct rw_semaphore *sem)
{
    atomic_long_set_release(&sem->count, 0);
    lx_rwsem_wake(sem);
}

// end (satoru)
