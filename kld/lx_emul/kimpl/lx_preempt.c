// lx_preempt: storage for the per-thread preempt count.
//
// one word per container thread, initialised to PREEMPT_ENABLED exactly like a
// boot cpu's pcpu_hot.preempt_count. the accessors live in the shim's
// asm/preempt.h so every inline preempt_disable/enable, local_bh_disable and
// this_cpu read of the count sees the calling thread's word. (satoru)

#include <asm/preempt.h>
#include <linux/preempt.h>
#include <linux/jiffies.h>
#include "lx_kshim.h"

__thread volatile int lx_preempt_count_word = (int)PREEMPT_ENABLED;

volatile unsigned long lx_softirq_bad_after;

void lx_softirq_enter(void)
{
    if (lx_stats_on()) {
        unsigned long long now_ns = lx_uptime_ns();
        unsigned long long j_ns =
            (unsigned long long)(jiffies - INITIAL_JIFFIES) * (1000000000ull / HZ);
        lx_stats_jiffies(now_ns > j_ns ? now_ns - j_ns : 0);
    }
    preempt_count_add(SOFTIRQ_OFFSET);
}

void lx_softirq_exit(void)
{
    preempt_count_sub(SOFTIRQ_OFFSET);
    if (preempt_count() != 0)
        lx_softirq_bad_after++;
}

// end (satoru)
