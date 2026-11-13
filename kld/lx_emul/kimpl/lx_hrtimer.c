// lx_emul: hrtimers on the existing timer thread.
//
// struct hrtimer's rb node is not a timer_list, so each live hrtimer gets a
// small side record holding a real timer_list. The timer thread in lx_work.c
// therefore runs the callback, exactly like it runs timer_list callbacks.
//
// restart semantics: the callback's HRTIMER_RESTART return re-arms at the
// expiry the callback left in timer->node.expires (hrtimer_forward updates it),
// which is what the M1 apitest counts. cancel uses timer_delete_sync, so a
// callback that is running is waited for. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "lx_kshim.h"

struct lx_hrt {
    struct hrtimer *t;
    struct timer_list timer;
    struct list_head link;
    int active;
};

static LIST_HEAD(g_hrt_list);
static volatile unsigned int g_hrt_lock;

static u64 lx_hrt_now(void) { return lx_uptime_ns(); }

static struct hrtimer_clock_base g_hrt_base[4] = {
    { .clockid = CLOCK_REALTIME,  .get_time = (ktime_t (*)(void))lx_hrt_now },
    { .clockid = CLOCK_MONOTONIC, .get_time = (ktime_t (*)(void))lx_hrt_now },
    { .clockid = CLOCK_BOOTTIME,  .get_time = (ktime_t (*)(void))lx_hrt_now },
    { .clockid = CLOCK_TAI,       .get_time = (ktime_t (*)(void))lx_hrt_now },
};

static struct hrtimer_clock_base *lx_hrt_base_of(clockid_t id)
{
    switch (id) {
    case CLOCK_REALTIME: return &g_hrt_base[0];
    case CLOCK_MONOTONIC: return &g_hrt_base[1];
    case CLOCK_BOOTTIME: return &g_hrt_base[2];
    case CLOCK_TAI: return &g_hrt_base[3];
    default: return &g_hrt_base[1];
    }
}

static struct lx_hrt *lx_hrt_find(struct hrtimer *t, bool create)
{
    struct lx_hrt *r;
    lx_flock(&g_hrt_lock);
    list_for_each_entry(r, &g_hrt_list, link) {
        if (r->t == t) {
            lx_funlock(&g_hrt_lock);
            return r;
        }
    }
    if (create) {
        r = lx_os_malloc(sizeof(*r));
        if (r) {
            memset(r, 0, sizeof(*r));
            r->t = t;
            INIT_LIST_HEAD(&r->link);
            list_add_tail(&r->link, &g_hrt_list);
        }
        lx_funlock(&g_hrt_lock);
        return r;
    }
    lx_funlock(&g_hrt_lock);
    return NULL;
}

static void lx_hrt_timer_fn(struct timer_list *timer)
{
    struct lx_hrt *r = from_timer(r, timer, timer);
    struct hrtimer *t = r->t;
    enum hrtimer_restart ret = HRTIMER_NORESTART;

    if (t->function)
        ret = t->function(t);
    if (ret == HRTIMER_RESTART && t->function) {
        u64 now = lx_uptime_ns();
        u64 exp = (u64)t->node.expires;
        u64 delta = exp > now ? exp - now : 0;
        lx_jiffies_update();
        mod_timer(&r->timer, jiffies + usecs_to_jiffies((unsigned int)((delta + 999) / 1000)));
    } else {
        r->active = 0;
    }
}

void hrtimer_init(struct hrtimer *timer, clockid_t which_clock,
                  enum hrtimer_mode mode)
{
    struct lx_hrt *r = lx_hrt_find(timer, true);
    timer->base = lx_hrt_base_of(which_clock);
    timer->state = HRTIMER_STATE_INACTIVE;
    timer->is_rel = (mode & HRTIMER_MODE_REL) ? 1 : 0;
    timer->is_soft = (mode & HRTIMER_MODE_SOFT) ? 1 : 0;
    timer->is_hard = (mode & HRTIMER_MODE_HARD) ? 1 : 0;
    if (r)
        timer_setup(&r->timer, lx_hrt_timer_fn, 0);
}

void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
                            u64 delta_ns, const enum hrtimer_mode mode)
{
    struct lx_hrt *r = lx_hrt_find(timer, true);
    u64 now = lx_uptime_ns();
    u64 exp;
    u64 wait_ns;

    if (!r)
        return;
    timer->is_rel = (mode & HRTIMER_MODE_REL) ? 1 : 0;
    exp = timer->is_rel ? now + (u64)tim : (u64)tim;
    timer->node.expires = exp;
    timer->_softexpires = exp;
    wait_ns = exp > now ? exp - now : 0;
    if (delta_ns)
        wait_ns += delta_ns;   // slack: never earlier than the request (satoru)
    lx_jiffies_update();
    r->active = 1;
    mod_timer(&r->timer, jiffies + usecs_to_jiffies((unsigned int)((wait_ns + 999) / 1000)));
}

int hrtimer_cancel(struct hrtimer *timer)
{
    struct lx_hrt *r = lx_hrt_find(timer, false);
    int was;
    if (!r)
        return 0;
    was = timer_delete_sync(&r->timer);
    r->active = 0;
    return was;
}

int hrtimer_try_to_cancel(struct hrtimer *timer)
{
    struct lx_hrt *r = lx_hrt_find(timer, false);
    int was;
    if (!r)
        return 0;
    was = timer_delete(&r->timer);
    if (was)
        r->active = 0;
    return was;
}

u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
    u64 overrun = 0;
    if (interval <= 0)
        return 0;
    while ((u64)timer->node.expires <= (u64)now) {
        timer->node.expires += interval;
        timer->_softexpires += interval;
        overrun++;
    }
    return overrun;
}

// enqueued or its callback running: the shim's active flag covers both
// (cleared after a NORESTART callback returns, or by cancel). (satoru)
bool hrtimer_active(const struct hrtimer *timer)
{
    struct lx_hrt *r = lx_hrt_find((struct hrtimer *)timer, false);
    return r && r->active;
}

// end (satoru)
