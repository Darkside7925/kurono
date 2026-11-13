// lx_emul: tasklets on a real shim thread.
//
// schedule runs a tasklet once even if it is scheduled repeatedly (the header's
// TASKLET_STATE_SCHED bit already gives us the once part). tasklet_kill removes
// a pending tasklet and waits for a running one. a disabled tasklet (count > 0)
// stays queued and is retried until tasklet_enable() drops the count. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/sched.h>

#include "lx_kshim.h"

struct lx_tl {
    struct tasklet_struct *t;
    struct list_head link;
};

static LIST_HEAD(g_tl_list);
static volatile unsigned int g_tl_lock;
static volatile unsigned int g_tl_kick;
static volatile unsigned int g_tl_done;
static struct lx_task *g_tl_task;

static void lx_tl_wake(void)
{
    __atomic_fetch_add(&g_tl_kick, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&g_tl_kick, 1);
}

static int lx_tasklet_thread(void *arg)
{
    (void)arg;
    lx_log("[lx_emul] tasklet thread online\n");
    for (;;) {
        unsigned int kick = __atomic_load_n(&g_tl_kick, __ATOMIC_ACQUIRE);
        struct lx_tl *rec = NULL;
        lx_flock(&g_tl_lock);
        struct lx_tl *r, *tmp;
        list_for_each_entry_safe(r, tmp, &g_tl_list, link) {
            if (atomic_read(&r->t->count) == 0) {
                list_del_init(&r->link);
                rec = r;
                break;
            }
        }
        lx_funlock(&g_tl_lock);
        if (!rec) {
            lx_os_futex_wait(&g_tl_kick, kick, 1000000);
            continue;
        }
        struct tasklet_struct *t = rec->t;
        set_bit(TASKLET_STATE_RUN, &t->state);
        clear_bit(TASKLET_STATE_SCHED, &t->state);
        lx_softirq_enter();
        if (t->use_callback) {
            if (t->callback)
                t->callback(t);
        } else {
            if (t->func)
                t->func(t->data);
        }
        lx_softirq_exit();
        clear_bit(TASKLET_STATE_RUN, &t->state);
        lx_os_free(rec);
        __atomic_fetch_add(&g_tl_done, 1, __ATOMIC_RELEASE);
        lx_os_futex_wake(&g_tl_done, 0x7fffffff);
    }
    return 0;
}

static void lx_tl_start(void)
{
    if (__atomic_load_n(&g_tl_task, __ATOMIC_ACQUIRE))
        return;
    lx_flock(&g_tl_lock);
    if (!g_tl_task)
        g_tl_task = lx_task_spawn("kld-tasklet", lx_tasklet_thread, NULL);
    lx_funlock(&g_tl_lock);
}

void __tasklet_schedule(struct tasklet_struct *t)
{
    struct lx_tl *rec = lx_os_malloc(sizeof(*rec));
    if (!rec) {
        clear_bit(TASKLET_STATE_SCHED, &t->state);
        return;
    }
    rec->t = t;
    INIT_LIST_HEAD(&rec->link);
    lx_tl_start();
    lx_flock(&g_tl_lock);
    list_add_tail(&rec->link, &g_tl_list);
    lx_funlock(&g_tl_lock);
    lx_tl_wake();
}

void __tasklet_hi_schedule(struct tasklet_struct *t)
{
    __tasklet_schedule(t);
}

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
                  unsigned long data)
{
    t->next = NULL;
    t->state = 0;
    atomic_set(&t->count, 0);
    t->use_callback = false;
    t->func = func;
    t->data = data;
}

void tasklet_setup(struct tasklet_struct *t,
                   void (*callback)(struct tasklet_struct *))
{
    t->next = NULL;
    t->state = 0;
    atomic_set(&t->count, 0);
    t->use_callback = true;
    t->callback = callback;
    t->data = 0;
}

void tasklet_unlock_wait(struct tasklet_struct *t)
{
    while (test_bit(TASKLET_STATE_RUN, &t->state))
        lx_os_sleep_ns(100000);
}

void tasklet_kill(struct tasklet_struct *t)
{
    // upstream warns if a tasklet reschedules itself; ours would spin forever
    // too, so bound it and report. (satoru)
    for (int guard = 0; guard < 100000; guard++) {
        lx_flock(&g_tl_lock);
        struct lx_tl *r, *tmp;
        list_for_each_entry_safe(r, tmp, &g_tl_list, link) {
            if (r->t == t) {
                list_del_init(&r->link);
                lx_os_free(r);
                clear_bit(TASKLET_STATE_SCHED, &t->state);
            }
        }
        lx_funlock(&g_tl_lock);
        if (!test_bit(TASKLET_STATE_RUN, &t->state) &&
            !test_bit(TASKLET_STATE_SCHED, &t->state))
            return;
        lx_os_sleep_ns(100000);
    }
    lx_log("[lx_emul] tasklet_kill: tasklet keeps rescheduling\n");
}

// end (satoru)
