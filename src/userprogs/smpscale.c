// smpscale: does cpu-bound work scale across cores? (kurono.smpscale=1, shell
// `smpscale [threads]`). each thread runs the same fixed amount of pure ring-3
// arithmetic (no syscalls inside the loop); for 1, 2, 4, ... threads the wall
// time of the whole batch gives the throughput, and speedup = throughput / the
// one-thread throughput. a thread also reads IA32_TSC_AUX with rdtscp now and
// then: under kurono.sched=2 it holds cpu index + 1, so the program can count
// the distinct cpus its threads really ran on. prints to stderr (com1 for the
// boot runner). (satoru)
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXT 64

static uint64_t g_work = 40000000ull;   // iterations per thread (satoru)
static volatile uint64_t g_sink[MAXT * 8];
static volatile uint32_t g_cpu_seen[MAXT];   // bitmask of cpu index+1 seen (satoru)
static volatile uint64_t g_cpu_mask_all;

static void logs(const char* s) { write(2, s, strlen(s)); }

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static inline uint32_t tsc_aux(void) {
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    (void)lo; (void)hi;
    return aux;
}

struct targ { int idx; };

static void* worker(void* a) {
    int idx = ((struct targ*)a)->idx;
    uint64_t x = 0x9E3779B97F4A7C15ull ^ (uint64_t)(idx + 1);
    uint64_t acc = 0;
    uint64_t mask = 0;
    for (uint64_t i = 0; i < g_work; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;   // xorshift64 (satoru)
        acc += x * 0x2545F4914F6CDD1Dull;
        if ((i & 0xFFFFFu) == 0) {
            uint32_t aux = tsc_aux();
            if (aux && aux <= 64) mask |= 1ull << (aux - 1);
        }
    }
    g_sink[idx * 8] = acc;
    __atomic_or_fetch(&g_cpu_mask_all, mask, __ATOMIC_RELAXED);
    return 0;
}

static uint64_t run_batch(int t) {
    pthread_t th[MAXT];
    struct targ ta[MAXT];
    uint64_t t0 = now_us();
    for (int i = 0; i < t; i++) {
        ta[i].idx = i;
        if (pthread_create(&th[i], 0, worker, &ta[i]) != 0) { logs("SMPSCALE pthread_create failed\n"); return 0; }
    }
    for (int i = 0; i < t; i++) pthread_join(th[i], 0);
    return now_us() - t0;
}

static int popcount64(uint64_t v) { int n = 0; while (v) { n += (int)(v & 1); v >>= 1; } return n; }

int main(int argc, char** argv) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int maxt = (argc > 1) ? atoi(argv[1]) : (int)ncpu;
    if (argc > 2) g_work = (uint64_t)strtoull(argv[2], 0, 10);
    if (maxt < 1) maxt = 1;
    if (maxt > MAXT) maxt = MAXT;
    char b[256];
    snprintf(b, sizeof b, "SMPSCALE start ncpu_reported=%ld maxthreads=%d work=%llu\n",
             ncpu, maxt, (unsigned long long)g_work);
    logs(b);
    double base = 0;
    double best = 0;
    int counts[16]; int nc = 0;
    for (int t = 1; t < maxt && nc < 14; t *= 2) counts[nc++] = t;
    counts[nc++] = maxt;
    for (int k = 0; k < nc; k++) {
        int t = counts[k];
        uint64_t us = 0;
        g_cpu_mask_all = 0;
        for (int rep = 0; rep < 2; rep++) {   // best of two (satoru)
            uint64_t u = run_batch(t);
            if (!u) return 1;
            if (!us || u < us) us = u;
        }
        double thr = (double)t * (double)g_work / (double)us;   // iterations per us (satoru)
        if (t == 1) base = thr;
        double sp = base > 0 ? thr / base : 0;
        if (sp > best) best = sp;
        snprintf(b, sizeof b, "SMPSCALE threads=%d ms=%llu speedup=%d.%02d cpus_seen=%d\n",
                 t, (unsigned long long)(us / 1000), (int)sp, (int)((sp - (int)sp) * 100),
                 popcount64(g_cpu_mask_all));
        logs(b);
    }
    snprintf(b, sizeof b, "SMPSCALE DONE ncpu=%ld best_speedup=%d.%02d\n", ncpu, (int)best,
             (int)((best - (int)best) * 100));
    logs(b);
    return 0;
}
// end (satoru)
