// lx_emul: time, delays, jiffies, timers, timecounter.
//
// The most-referenced group in the e1000e census (usleep_range_state 8,
// __const_udelay 8, msleep 5) because hardware bring-up is mostly "poke a
// register, wait, read it back".
//
// Delays must be REAL. A no-op delay is the classic foreign-driver-shim bug:
// the driver polls a register the hardware has not had time to update, decides
// the device is broken, and fails probe with an error that looks like a shim
// gap somewhere else entirely.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/timecounter.h>
#include <linux/sched.h>

#include "lx_kshim.h"

// ── the host clock ──────────────────────────────────────────────────────────
// clock_gettime(CLOCK_MONOTONIC) via a direct syscall. We cannot call musl's
// wrapper from kernel-context code (the headers collide), so issue it raw.
// (satoru)
struct lx_timespec { long tv_sec; long tv_nsec; };

static long lx_clock_gettime(struct lx_timespec *ts)
{
    long ret;
    register long r10 __asm__("r10");
    (void)r10;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(228L) /* SYS_clock_gettime */,
                           "D"(1L)   /* CLOCK_MONOTONIC */,
                           "S"(ts)
                         : "rcx", "r11", "memory");
    return ret;
}

static u64 lx_now_ns(void)
{
    struct lx_timespec ts = { 0, 0 };
    if (lx_clock_gettime(&ts) != 0)
        return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u64 lx_boot_ns;

static void lx_time_init_once(void)
{
    if (!lx_boot_ns)
        lx_boot_ns = lx_now_ns();
}

// monotonic ns since the first call. (satoru)
u64 lx_uptime_ns(void)
{
    lx_time_init_once();
    u64 n = lx_now_ns();
    return n > lx_boot_ns ? (n - lx_boot_ns) : 0;
}

// keep the jiffies DATA symbol (lx_data.c) tracking real time. Anything that
// reads jiffies for a timeout comparison depends on it advancing. (satoru)
static void lx_jiffies_update(void)
{
    jiffies = INITIAL_JIFFIES + (unsigned long)(lx_uptime_ns() / (1000000000ull / HZ));
}

// ── busy delays ─────────────────────────────────────────────────────────────
// udelay() expands to __const_udelay(usecs * 0x10C7) on x86, where the constant
// is 2**32 / 1000000. Recover the microseconds from it rather than guessing.
// (satoru)
#define LX_UDELAY_MULT 0x10c7ul

static void lx_busy_wait_ns(u64 ns)
{
    u64 end = lx_uptime_ns() + ns;
    while (lx_uptime_ns() < end)
        __asm__ __volatile__("pause" ::: "memory");
}

void __const_udelay(unsigned long xloops)
{
    lx_busy_wait_ns((u64)(xloops / LX_UDELAY_MULT) * 1000ull);
}

void __udelay(unsigned long usecs)
{
    lx_busy_wait_ns((u64)usecs * 1000ull);
}

void __ndelay(unsigned long nsecs)
{
    lx_busy_wait_ns((u64)nsecs);
}

// ── sleeping delays ─────────────────────────────────────────────────────────
// real nanosleep so the container yields the cpu instead of spinning. measured
// ring-3 poll/nanosleep wake latency on Kurono is sub-millisecond at p99, so
// these are accurate enough for device bring-up. (satoru)
static void lx_nanosleep_ns(u64 ns)
{
    struct lx_timespec req;
    long ret;
    req.tv_sec  = (long)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(35L) /* SYS_nanosleep */, "D"(&req), "S"(0L)
                         : "rcx", "r11", "memory");
    (void)ret;
}

void msleep(unsigned int msecs)
{
    lx_nanosleep_ns((u64)msecs * 1000000ull);
    lx_jiffies_update();
}

unsigned long msleep_interruptible(unsigned int msecs)
{
    msleep(msecs);
    return 0;   // never interrupted: the container has no signal delivery here
}

void usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
    (void)max;
    (void)state;
    lx_nanosleep_ns((u64)min * 1000ull);
    lx_jiffies_update();
}

// ── ktime / timekeeping ─────────────────────────────────────────────────────
void ktime_get_ts64(struct timespec64 *ts)
{
    u64 ns = lx_uptime_ns();
    ts->tv_sec  = (time64_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

void ktime_get_raw_ts64(struct timespec64 *ts)
{
    ktime_get_ts64(ts);
}

// no RTC in the container: report uptime rather than a fabricated wall clock, so
// anything that logs a timestamp is obviously relative instead of wrong. (satoru)
void ktime_get_real_ts64(struct timespec64 *ts)
{
    ktime_get_ts64(ts);
}

ktime_t ktime_get_with_offset(enum tk_offsets offs)
{
    (void)offs;
    return (ktime_t)lx_uptime_ns();
}

struct timespec64 ns_to_timespec64(s64 nsec)
{
    struct timespec64 ts;
    if (nsec >= 0) {
        ts.tv_sec  = (time64_t)(nsec / 1000000000ll);
        ts.tv_nsec = (long)(nsec % 1000000000ll);
    } else {
        ts.tv_sec  = (time64_t)(-((-nsec + 999999999ll) / 1000000000ll));
        ts.tv_nsec = (long)(nsec - ts.tv_sec * 1000000000ll);
    }
    return ts;
}

unsigned long round_jiffies(unsigned long j)
{
    return j;   // rounding is a power-saving hint, not semantics
}

// ── timecounter (used by e1000e's PTP path) ─────────────────────────────────
void timecounter_init(struct timecounter *tc, const struct cyclecounter *cc,
                      u64 start_tstamp)
{
    tc->cc = cc;
    tc->cycle_last = cc->read(cc);
    tc->nsec = start_tstamp;
    tc->mask = cc->mask;
    tc->frac = 0;
}

u64 timecounter_read(struct timecounter *tc)
{
    u64 now = tc->cc->read(tc->cc);
    u64 delta = (now - tc->cycle_last) & tc->mask;
    tc->cycle_last = now;
    tc->nsec += ((delta * tc->cc->mult) + tc->frac) >> tc->cc->shift;
    return tc->nsec;
}

u64 timecounter_cyc2time(const struct timecounter *tc, u64 cycle_tstamp)
{
    u64 delta = (cycle_tstamp - tc->cycle_last) & tc->mask;
    return tc->nsec + ((delta * tc->cc->mult) >> tc->cc->shift);
}

// crosststamp needs a hardware-correlated clock pair we do not have; refuse
// rather than invent a correlation. e1000e treats the error as "no PTP
// crosststamp support" and continues. (satoru)
int get_device_system_crosststamp(int (*get_time_fn)(ktime_t *, struct system_counterval_t *, void *),
                                  void *ctx, struct system_time_snapshot *history,
                                  struct system_device_crosststamp *xtstamp)
{
    (void)get_time_fn; (void)ctx; (void)history; (void)xtstamp;
    return -ENODEV;
}

// end (satoru)
