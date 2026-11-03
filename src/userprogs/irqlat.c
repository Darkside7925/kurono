// irqlat - USERSPACE wake-latency probe for the KLD go/no-go.
//
// KLD phase 0 requires "an interrupt must be able to wake a userspace thread
// with bounded latency". That is unfalsifiable as prose, so measure it.
//
// A KLD driver container waits on a doorbell (an fd) and is woken when the
// kernel forwards a device MSI. The latency of that wake decides whether a NIC
// at line rate is servicable at all. We cannot raise a device IRQ from
// userspace, but we can measure the same fabric end to end:
//
//   mode=nanosleep : timer IRQ -> scheduler -> this ring-3 thread resumes
//   mode=poll      : the poll/timeout wakeup path, which is what an fd-based
//                    doorbell would actually use (closer to the real thing)
//
// The number that matters is the EXCESS over the requested sleep, and
// specifically its TAIL - a good p50 with a 30ms p99.9 still kills a NIC.
// Kernel-side kmemx probing already showed a 10ms sleep landing at 10.5ms mean
// with 28-35ms worst-case gaps; this asks whether ring-3 is better or worse.
//
// Self-calibrates TSC against clock_gettime so no external MHz is needed.
// Prints greppable [irqlat] lines to stderr (com1). (satoru)

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>

static void logs(const char* s) { write(2, s, strlen(s)); }
static void logu(unsigned long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[n++] = t[--k];
    write(2, b, n);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// 10us buckets up to 50ms; everything above lands in the last bucket. (satoru)
#define NBUCK 5000
#define BUCK_US 10
static uint32_t hist[NBUCK];

static void hist_reset(void) { memset(hist, 0, sizeof(hist)); }
static void hist_add(uint64_t us) {
    uint64_t b = us / BUCK_US;
    if (b >= NBUCK) b = NBUCK - 1;
    hist[b]++;
}
// exact-to-10us percentile from the histogram. (satoru)
static uint64_t hist_pct(uint32_t total, unsigned pct_x10) {
    uint64_t want = ((uint64_t)total * pct_x10) / 1000ull;
    if (want == 0) want = 1;
    uint64_t acc = 0;
    for (int i = 0; i < NBUCK; i++) {
        acc += hist[i];
        if (acc >= want) return (uint64_t)i * BUCK_US;
    }
    return (uint64_t)(NBUCK - 1) * BUCK_US;
}

// cycles per microsecond, measured against the monotonic clock. (satoru)
static uint64_t calibrate(void) {
    uint64_t t0 = now_ns();
    if (t0 == 0) return 0;                 // no usable clock -> report cycles
    uint64_t c0 = rdtsc();
    // busy-spin ~150ms of wall time so the ratio is stable even if the clock
    // is coarse. no sleeping here - a sleep would measure the thing we are
    // trying to calibrate. (satoru)
    uint64_t t1;
    do { t1 = now_ns(); } while (t1 - t0 < 150000000ull);
    uint64_t c1 = rdtsc();
    uint64_t dns = t1 - t0;
    if (dns == 0) return 0;
    return ((c1 - c0) * 1000ull) / dns;    // cycles per us
}

static void run_mode(const char* name, int use_poll, int iters,
                     uint64_t cyc_per_us, uint64_t req_us) {
    hist_reset();
    uint64_t worst = 0, sum = 0;
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = (long)(req_us * 1000ull);

    for (int i = 0; i < iters; i++) {
        uint64_t c0 = rdtsc();
        if (use_poll) {
            // no fds: a pure timeout wait through the poll path. this is the
            // machinery an fd doorbell would ride on. (satoru)
            poll(0, 0, (int)(req_us / 1000ull ? req_us / 1000ull : 1));
        } else {
            nanosleep(&req, 0);
        }
        uint64_t d = rdtsc() - c0;
        uint64_t us = cyc_per_us ? (d / cyc_per_us) : d;
        // record the EXCESS over what we asked for - that is the wake latency
        uint64_t ex = (us > req_us) ? (us - req_us) : 0;
        hist_add(ex);
        sum += ex;
        if (ex > worst) worst = ex;
    }

    logs("[irqlat] mode="); logs(name);
    logs(" n=");     logu((unsigned long)iters);
    logs(" req_us="); logu((unsigned long)req_us);
    logs(" excess_us mean="); logu((unsigned long)(sum / (uint64_t)iters));
    logs(" p50=");   logu((unsigned long)hist_pct((uint32_t)iters, 500));
    logs(" p99=");   logu((unsigned long)hist_pct((uint32_t)iters, 990));
    logs(" p999=");  logu((unsigned long)hist_pct((uint32_t)iters, 999));
    logs(" max=");   logu((unsigned long)worst);
    logs(cyc_per_us ? "\n" : " (UNCALIBRATED: units are cycles)\n");
}

int main(void) {
    logs("[irqlat] userspace wake-latency probe starting\n");
    uint64_t cpu = calibrate();
    logs("[irqlat] tsc cycles_per_us=");
    logu((unsigned long)cpu);
    logs(cpu ? "\n" : " (clock_gettime unusable)\n");

    // 1ms request is the interesting point: it is the order a NIC interrupt
    // must be serviced at, and it is short enough that scheduler overshoot
    // dominates the sample. (satoru)
    run_mode("nanosleep", 0, 2000, cpu, 1000);
    run_mode("poll",      1, 2000, cpu, 1000);
    // a 10ms request matches the kernel-side kmemx probe so the ring-3 and
    // ring-0 numbers are directly comparable. (satoru)
    run_mode("nanosleep10ms", 0, 500, cpu, 10000);

    logs("[irqlat] DONE\n");
    return 0;
}
