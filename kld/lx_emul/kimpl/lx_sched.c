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
};

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
    return t;
}

// the calling thread's task. threads the shim did not start (main) get one
// lazily. (satoru)
struct lx_task *lx_current(void)
{
    if (!lx_self) {
        lx_self = lx_task_alloc("kld-main");
        if (lx_self)
            lx_self->started = 1;
    }
    return lx_self;
}

struct task_struct *lx_task_ts(struct lx_task *t) { return t ? &t->ts : NULL; }

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
        lx_os_futex_wake(&t->woken, 1);
        return 1;
    }
    return 0;   // already had a pending wake (satoru)
}

static void lx_task_entry(void *p)
{
    struct lx_task *t = p;
    lx_self = t;
    // linux kthreads are created stopped and start on wake_up_process. (satoru)
    while (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE))
        lx_os_futex_wait(&t->started, 0, -1);
    t->exit_code = t->fn ? t->fn(t->arg) : 0;
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

// end (satoru)
