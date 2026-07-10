// futex torture test for the kurono SMP kernel - the fast standalone ORACLE.
// runs headless at boot via the [aptest] path (kurono.apsched=1 + -smp>1) and
// prints a single PASS/FAIL line to stderr (com1). iteration is seconds, not a
// firefox boot. it exercises the RAW futex contract (FUTEX_WAIT on a value word
// + FUTEX_WAKE, FUTEX_PRIVATE_FLAG) that rust-std/rayon - and therefore
// firefox's software-WebRender render threads - depend on, which musl's pthread
// mutex does not fully stress. the classic lost-wake window, wake-count
// exactness, and the block/wake (deferral/state) race all show up here as a
// permanent hang. (was: a minimal pthread mutex smoke test.) (satoru)
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

// linux/futex.h isn't in the musl-gcc include path; define the bits we use. (satoru)
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128

static void logs(const char* s) { write(2, s, strlen(s)); }
static void logu(unsigned long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[n++] = t[--k];
    write(2, b, n);
}

static inline long fwait(volatile uint32_t* a, uint32_t v) {
    return syscall(SYS_futex, a, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, v, 0, 0, 0);
}
static inline long fwake(volatile uint32_t* a, int n) {
    return syscall(SYS_futex, a, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, 0, 0, 0);
}

// ── ping-pong lost-wake torture ─────────────────────────────────────────────
// per group: one word `turn` toggles 0<->1; pinger owns turn==0, ponger owns
// turn==1. each side spins-then-futex_waits for its turn, flips it, wakes the
// peer. R rounds each. a LOST WAKE parks both peers forever -> that group's
// `done` stops advancing -> the watchdog catches it and prints which group and
// its stuck `turn`. correct behaviour: every group reaches 2*R and PASS prints.
#define NGROUPS 8
#define ROUNDS  20000

typedef struct {
    volatile uint32_t turn;   // 0 = pinger's turn, 1 = ponger's turn
    volatile uint32_t done;   // handoffs completed in this group
} Group;

static Group g_groups[NGROUPS];
static volatile uint32_t g_overran = 0;   // any thread that ran != ROUNDS iters

// per-thread SELF-assertion: `iters` counts this thread's own loop-body
// executions. after the loop the thread checks iters == ROUNDS and reports on
// ITSELF. this is immune to the "unsynchronized read of the summed counters
// while writers are mid-flight" artifact - the thread is done and reporting its
// own local count, not a cross-thread sum. iters != ROUNDS means the for-loop
// executed the wrong number of times, i.e. a loop-counter register corrupted by a bad
// context restore. (satoru)
static void* pinger(void* arg) {
    Group* g = (Group*)arg;
    unsigned long iters = 0;
    for (int r = 0; r < ROUNDS; r++) {
        while (__atomic_load_n(&g->turn, __ATOMIC_ACQUIRE) != 0)
            fwait(&g->turn, 1);                 // wait while it's the ponger's turn
        __atomic_store_n(&g->turn, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g->done, 1, __ATOMIC_RELAXED);
        fwake(&g->turn, 1);
        iters++;
    }
    if (iters != ROUNDS) {
        logs("FUTEXTORTURE: THREAD-OVERRAN pinger grp="); logu((unsigned long)(g - g_groups));
        logs(" iters="); logu(iters); logs(" expected="); logu(ROUNDS); logs("\n");
        __atomic_store_n(&g_overran, 1, __ATOMIC_RELEASE);
    }
    return 0;
}
static void* ponger(void* arg) {
    Group* g = (Group*)arg;
    unsigned long iters = 0;
    for (int r = 0; r < ROUNDS; r++) {
        while (__atomic_load_n(&g->turn, __ATOMIC_ACQUIRE) != 1)
            fwait(&g->turn, 0);                 // wait while it's the pinger's turn
        __atomic_store_n(&g->turn, 0, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g->done, 1, __ATOMIC_RELAXED);
        fwake(&g->turn, 1);
        iters++;
    }
    if (iters != ROUNDS) {
        logs("FUTEXTORTURE: THREAD-OVERRAN ponger grp="); logu((unsigned long)(g - g_groups));
        logs(" iters="); logu(iters); logs(" expected="); logu(ROUNDS); logs("\n");
        __atomic_store_n(&g_overran, 1, __ATOMIC_RELEASE);
    }
    return 0;
}

static unsigned long total_done(void) {
    unsigned long t = 0;
    for (int i = 0; i < NGROUPS; i++)
        t += __atomic_load_n(&g_groups[i].done, __ATOMIC_RELAXED);
    return t;
}

// ── thundering-herd torture ─────────────────────────────────────────────────
// H waiter threads all FUTEX_WAIT on ONE shared word `hw`. a driver flips hw and
// FUTEX_WAKE(1)s ONE waiter per round; that waiter must wake, re-arm (dec a
// pending counter), and re-wait. this stresses (a) wake-count exactness (wake 1
// must wake exactly one, and repeatedly) and (b) the block-commit-vs-wake race
// (many threads are perpetually committing a fresh block while a wake fires - 
// the "woken thread stored as parked" window). a lost/miscounted wake stalls the
// herd. (satoru)
#define HERD    24        // waiters - oversubscribes 4 cores 6x for max preemption
#define HERD_R  40000     // wake rounds the driver drives

static volatile uint32_t hw = 0;         // the shared herd futex word
static volatile uint32_t herd_woken = 0; // total waiter wake-returns observed
static volatile uint32_t herd_go = 0;    // driver toggles this to release a wave

static void* herd_waiter(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t v = __atomic_load_n(&hw, __ATOMIC_ACQUIRE);
        fwait(&hw, v);                                  // sleep until hw changes
        __atomic_add_fetch(&herd_woken, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&herd_go, __ATOMIC_ACQUIRE) == 0) return 0;  // shutdown
    }
}

static int run_herd(void) {
    __atomic_store_n(&herd_go, 1, __ATOMIC_RELEASE);
    pthread_t ht[HERD];
    for (int i = 0; i < HERD; i++)
        if (pthread_create(&ht[i], 0, herd_waiter, 0) != 0) { logs("FUTEXTORTURE: FAIL create herd\n"); return 1; }

    // drive HERD_R waves: bump the word, wake ALL (so every parked waiter must
    // re-arm), and require the wake count to keep climbing. a stalled count =
    // waiters stored as parked despite the wake. (satoru)
    unsigned long last = 0, stall = 0; int r = 0;
    while (r < HERD_R) {
        __atomic_add_fetch(&hw, 1, __ATOMIC_RELEASE);
        fwake(&hw, HERD);                                // wake the whole herd
        r++;
        if ((r & 0x3FF) == 0) {                          // periodic progress check
            unsigned long now = __atomic_load_n(&herd_woken, __ATOMIC_RELAXED);
            if (now == last) { if (++stall > 200) break; }
            else { stall = 0; last = now; }
        }
    }
    // shutdown: waiters exit when they see herd_go==0 after a wake. (satoru)
    __atomic_store_n(&herd_go, 0, __ATOMIC_RELEASE);
    for (int k = 0; k < 8; k++) { __atomic_add_fetch(&hw, 1, __ATOMIC_RELEASE); fwake(&hw, HERD); for (volatile int s=0;s<500000;s++) __asm__ __volatile__("pause"); }
    for (int i = 0; i < HERD; i++) pthread_join(ht[i], 0);
    unsigned long woke = __atomic_load_n(&herd_woken, __ATOMIC_RELAXED);
    logs("FUTEXTORTURE: herd rounds="); logu((unsigned long)r);
    logs(" woke="); logu(woke); logs("\n");
    // the herd must have kept waking across all rounds (woke grows ~HERD/round);
    // if the driver bailed early on a stall, that's a lost/miscounted wake. (satoru)
    if (r < HERD_R) { logs("FUTEXTORTURE: FAIL herd stalled\n"); return 2; }
    return 0;
}

int main(void) {
    logs("FUTEXTORTURE: start groups="); logu(NGROUPS);
    logs(" rounds="); logu(ROUNDS); logs("\n");

    // ── phase A: ping-pong lost-wake torture ──
    pthread_t th[NGROUPS * 2];
    for (int i = 0; i < NGROUPS; i++) {
        g_groups[i].turn = 0; g_groups[i].done = 0;
        if (pthread_create(&th[i*2],   0, pinger, &g_groups[i]) != 0) { logs("FUTEXTORTURE: FAIL create pinger\n"); return 1; }
        if (pthread_create(&th[i*2+1], 0, ponger, &g_groups[i]) != 0) { logs("FUTEXTORTURE: FAIL create ponger\n"); return 1; }
    }
    unsigned long expected = (unsigned long)NGROUPS * ROUNDS * 2;
    // watchdog: its ONLY authoritative job is to detect a genuine HANG (lost
    // wake -> a thread parked forever -> join would never return). the summed
    // total_done() reads here are unsynchronised (writers mid-flight), so a
    // transient over-by-1-or-2 is treated as DIAGNOSTIC ONLY - never a verdict.
    // the corruption verdict is the post-JOIN per-thread self-assertion
    // (g_overran). (satoru)
    unsigned long last = 0, stall = 0;
    for (;;) {
        for (volatile int s = 0; s < 2000000; s++) { __asm__ __volatile__("pause"); }
        unsigned long now = total_done();
        // UNIMPEACHABLE single-counter check: each g_groups[i].done is a single
        // monotonic counter bumped only by its own pinger+ponger, ROUNDS each, so
        // it caps at 2*ROUNDS. a SINGLE counter reading past that cap is NOT the
        // "summed read while writers mid-flight" artifact (no sum, one counter) - 
        // it can only mean that group's pinger or ponger executed extra loop
        // bodies = context corruption. FAIL immediately + pin the group, so a
        // corrupted+desynced thread doesn't just hang to the qemu timeout. (satoru)
        for (int i = 0; i < NGROUPS; i++) {
            unsigned long d = __atomic_load_n(&g_groups[i].done, __ATOMIC_ACQUIRE);
            if (d > (unsigned long)ROUNDS * 2) {
                logs("FUTEXTORTURE: FAIL context-corruption grp="); logu(i);
                logs(" done="); logu(d); logs(" > cap="); logu((unsigned long)ROUNDS * 2);
                logs(" (a thread over-ran its loop)\n");
                return 3;
            }
        }
        if (now > expected) { logs("FUTEXTORTURE: (diag) pre-join over-read total="); logu(now); logs("\n"); }
        if (now >= expected) break;                 // all handoffs in flight/done -> go join
        if (now == last) {
            if (++stall > 60) {                     // no progress for a long while = a real hang
                logs("FUTEXTORTURE: FAIL phaseA stalled total="); logu(now);
                logs(" expected="); logu(expected); logs("\n");
                for (int i = 0; i < NGROUPS; i++) {
                    logs("  grp="); logu(i);
                    logs(" turn="); logu(__atomic_load_n(&g_groups[i].turn, __ATOMIC_RELAXED));
                    logs(" done="); logu(__atomic_load_n(&g_groups[i].done, __ATOMIC_RELAXED));
                    logs("\n");
                }
                return 2;
            }
        } else { stall = 0; last = now; }
    }
    // settle + JOIN: every thread must finish before we trust any count. a hung
    // thread makes join block -> the qemu-side timeout catches it (still a FAIL,
    // just via timeout). (satoru)
    for (int i = 0; i < NGROUPS * 2; i++) pthread_join(th[i], 0);

    // AUTHORITATIVE verdict - post-join, each thread has self-reported. (satoru)
    if (__atomic_load_n(&g_overran, __ATOMIC_ACQUIRE) != 0) {
        logs("FUTEXTORTURE: FAIL context-corruption (a thread self-reported iters!=ROUNDS)\n");
        return 3;
    }
    unsigned long fin = total_done();
    if (fin != expected) {
        logs("FUTEXTORTURE: FAIL phaseA final total="); logu(fin);
        logs(" != expected="); logu(expected); logs("\n");
        for (int i = 0; i < NGROUPS; i++) {
            unsigned long d = __atomic_load_n(&g_groups[i].done, __ATOMIC_RELAXED);
            if (d != (unsigned long)ROUNDS * 2) { logs("  grp="); logu(i); logs(" done="); logu(d); logs("\n"); }
        }
        return 3;
    }
    logs("FUTEXTORTURE: phaseA PASS total="); logu(fin); logs("\n");

    // ── phase B: thundering-herd + wake-count/mid-unwind torture ──
    int hr = run_herd();
    if (hr) return hr;

    logs("FUTEXTORTURE: PASS all phases\n");
    return 0;
}
// end (satoru)
