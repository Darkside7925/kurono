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
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/mman.h>

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
static uint64_t hist_sum_us;

static void hist_reset(void) { memset(hist, 0, sizeof(hist)); hist_sum_us = 0; }
static void hist_add(uint64_t us) {
    uint64_t b = us / BUCK_US;
    if (b >= NBUCK) b = NBUCK - 1;
    hist[b]++;
    hist_sum_us += us;
}
// total samples and worst sample in the current histogram. (satoru)
static uint32_t hist_total(void) {
    uint32_t n = 0;
    for (int i = 0; i < NBUCK; i++) n += hist[i];
    return n;
}
static uint64_t hist_worst(void) {
    for (int i = NBUCK - 1; i >= 0; i--) if (hist[i]) return (uint64_t)i * BUCK_US;
    return 0;
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

// ── contention mode: kurono.irqlat.busy=N [secs [mode]] ─────────────────────
// the KLD-free reproduction of the 2-cpu starvation: one thread blocks (futex,
// poll on a pipe, nanosleep, a blocking read interrupted by a timer signal)
// while N busy ring-3 threads compete for the same program cpus. every wake
// latency is the time from the waker's stamp to the thread running again
// (timer cases: the excess over the requested period). mode 0 = compute
// spinners, 1 = sched_yield spinners (what the KLD shim threads do), 2 = half
// and half. prints [irqlat] busy=... case=... lines to stderr. (satoru)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

static long fwait_u(volatile uint32_t* a, uint32_t v) {
    return syscall(SYS_futex, a, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, v, 0, 0, 0);
}
static long fwake_u(volatile uint32_t* a, int n) {
    return syscall(SYS_futex, a, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, 0, 0, 0);
}

static uint64_t ct_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static void ct_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (long)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, 0);
}

static volatile int g_wt_stop;
static volatile int g_wt_case_stop;
static int g_wt_mode;
static int g_wt_busy_n;
static volatile uint64_t g_wt_sink;
static volatile uint64_t g_wt_busy_iters;

static void* wt_busy(void* arg) {
    (void)arg;
    uint64_t x = 0x9E3779B97F4A7C15ull;
    uint64_t it = 0;
    while (!__atomic_load_n(&g_wt_stop, __ATOMIC_ACQUIRE)) {
        int m = g_wt_mode;
        if (m == 1 || (m == 2 && (it & 1))) {
            sched_yield();   // the shim's yield loop (satoru)
        } else {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            x += 0x2545F4914F6CDD1Dull;
        }
        it++;
    }
    __atomic_store_n(&g_wt_sink, x, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_wt_busy_iters, it, __ATOMIC_RELAXED);
    return 0;
}

// futex case: the waiter parks on a sequence word; the waker stamps the time,
// bumps the word and wakes it. latency = run time - stamp. (satoru)
static volatile uint32_t wt_f_seq, wt_f_seen, wt_f_acked, wt_f_target;
static volatile uint64_t wt_f_stamp;

static void* wt_futex_waiter(void* arg) {
    (void)arg;
    uint32_t seen = 0;
    while (!__atomic_load_n(&g_wt_case_stop, __ATOMIC_ACQUIRE)) {
        uint32_t s = __atomic_load_n(&wt_f_seq, __ATOMIC_ACQUIRE);
        if (s == seen) { fwait_u(&wt_f_seq, s); continue; }
        uint64_t t = ct_us();
        uint64_t st = __atomic_load_n(&wt_f_stamp, __ATOMIC_ACQUIRE);
        seen = s;
        if (t >= st) hist_add(t - st);
        __atomic_add_fetch(&wt_f_acked, 1u, __ATOMIC_RELEASE);
    }
    return 0;
}

// pipe case: poll(POLLIN) on an empty pipe, the waker writes one byte. (satoru)
static int wt_p_fd[2];
static volatile uint32_t wt_p_seq, wt_p_seen, wt_p_acked, wt_p_target;
static volatile uint64_t wt_p_stamp;

static void* wt_pipe_waiter(void* arg) {
    (void)arg;
    uint32_t seen = 0;
    while (!__atomic_load_n(&g_wt_case_stop, __ATOMIC_ACQUIRE)) {
        struct pollfd p = { .fd = wt_p_fd[0], .events = POLLIN, .revents = 0 };
        poll(&p, 1, -1);
        if (__atomic_load_n(&wt_p_seq, __ATOMIC_ACQUIRE) != seen) {
            uint64_t t = ct_us();
            uint64_t st = __atomic_load_n(&wt_p_stamp, __ATOMIC_ACQUIRE);
            { char c; (void)!read(wt_p_fd[0], &c, 1); }
            seen = __atomic_load_n(&wt_p_seq, __ATOMIC_ACQUIRE);
            if (t >= st) hist_add(t - st);
            __atomic_add_fetch(&wt_p_acked, 1u, __ATOMIC_RELEASE);
        } else {
            char c;
            (void)!read(wt_p_fd[0], &c, 1);   // spurious wakeup: drain nothing (satoru)
        }
    }
    return 0;
}

// timer case: nanosleep parks in the kernel; the deadline is promoted from an
// irq (cpu0's pit) so this is the timer-interrupt wake shape. (satoru)
static void* wt_nanosleep_waiter(void* arg) {
    (void)arg;
    while (!__atomic_load_n(&g_wt_case_stop, __ATOMIC_ACQUIRE)) {
        uint64_t t0 = ct_us();
        ct_sleep_us(2000);
        uint64_t d = ct_us() - t0;
        hist_add(d > 2000 ? d - 2000 : 0);
    }
    return 0;
}

// blocking read on a pipe interrupted by SIGALRM (an fd wait woken from an
// irq-context signal post); latency is the excess over the 2 ms period. (satoru)
static volatile uint32_t wt_alarm_n;
static void wt_alarm_handler(int sig) { (void)sig; __atomic_add_fetch(&wt_alarm_n, 1u, __ATOMIC_RELAXED); }

static void* wt_alarm_waiter(void* arg) {
    (void)arg;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = wt_alarm_handler;
    sigaction(SIGALRM, &sa, 0);
    int rp[2];
    if (pipe(rp) != 0) return 0;
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_interval.tv_usec = 2000;
    it.it_value.tv_usec = 2000;
    setitimer(ITIMER_REAL, &it, 0);
    uint64_t last = 0;
    while (!__atomic_load_n(&g_wt_case_stop, __ATOMIC_ACQUIRE)) {
        struct pollfd p = { .fd = rp[0], .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, 1000);   // bounded: a missing signal must not hang (satoru)
        uint64_t t = ct_us();
        if (pr < 0 && errno == EINTR) {
            if (last && t > last) {
                uint64_t d = t - last;
                hist_add(d > 2000 ? d - 2000 : 0);
            }
            last = t;
        }
        if (!last) last = t;
    }
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, 0);
    close(rp[0]); close(rp[1]);
    return 0;
}

static void wt_report(const char* name, uint32_t n, uint64_t mean) {
    logs("[irqlat] busy="); logu((unsigned long)g_wt_busy_n);
    logs(" case="); logs(name);
    logs(" n="); logu(n);
    logs(" mean_us="); logu((unsigned long)mean);
    logs(" p50="); logu((unsigned long)hist_pct(n, 500));
    logs(" p99="); logu((unsigned long)hist_pct(n, 990));
    logs(" p999="); logu((unsigned long)hist_pct(n, 999));
    logs(" max="); logu((unsigned long)hist_worst());
    logs("\n");
}

// ── blocking-connect case (kurono.irqlat.mode=3) ───────────────────────────
// asserts what userspace SEES for the parked blocking connect: 0 for a
// listening port (then send/recv), ECONNREFUSED for a closed one, ETIMEDOUT at
// the kernel 10 s deadline for a black hole, EINTR for a signal mid-park, and
// two concurrent connects. a nanosleep monitor thread reports its wake p99, so
// a connect that held the only program cpu would show up there. (satoru)
static volatile int wt_mon_stop;
static int wt_connect_to(const char* ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -errno;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -22; }
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        int e = errno;
        close(s);
        return -e;
    }
    return s;
}
static void* wt_monitor(void* arg) {
    (void)arg;
    while (!__atomic_load_n(&wt_mon_stop, __ATOMIC_ACQUIRE)) {
        uint64_t t0 = ct_us();
        ct_sleep_us(2000);
        uint64_t d = ct_us() - t0;
        hist_add(d > 2000 ? d - 2000 : 0);
    }
    return 0;
}
static void wt_sigalrm(int sig) { (void)sig; }
struct wt_args { const char* ip; int port; int rc; int err; };
static void* wt_conn_thread(void* a) {
    struct wt_args* w = (struct wt_args*)a;
    int s = wt_connect_to(w->ip, w->port);
    w->rc = s;
    w->err = s < 0 ? -s : 0;
    if (s >= 0) close(s);
    return 0;
}
static void* wt_conn_run(void* v) { wt_conn_thread(v); return 0; }

struct wt_park_shared {
    volatile int fd;
    volatile int rc;
    volatile int err;
};
static void* wt_conn_park_thread(void* v) {
    struct wt_park_shared* sh = (struct wt_park_shared*)v;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { sh->rc = -errno; sh->err = errno; return 0; }
    sh->fd = s;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);
    int r = connect(s, (struct sockaddr*)&a, sizeof(a));
    sh->rc = r == 0 ? 0 : -errno;
    sh->err = r == 0 ? 0 : errno;
    if (r == 0) close(s);
    return 0;
}

static volatile int g_evt_n;
static void wt_evt_handler(int sig) { (void)sig; g_evt_n++; }
static pthread_t g_main_th;
static void* wt_kill_main_soon(void* v) {
    int sig = (int)(long)v;
    ct_sleep_us(150000);
    pthread_kill(g_main_th, sig);
    return 0;
}

// ── gated-park tests (kurono.irqlat.mode=4) ─────────────────────────────────
// pty integrity through a slowly draining reader, and fork-based flock waits.
// prints [irqlat] lines with lengths/hashes/timings; the kernel's wakelat
// wait_parks=lock/pty counts how many parks happened. (satoru)
#define PTY_TEST_BYTES (128 * 1024)
static uint8_t g_pty_exp[PTY_TEST_BYTES];
static volatile long g_pty_written;
static uint64_t wt_fnv(uint64_t h, const uint8_t* p, size_t n) {
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}
static int wt_open_pty(char* slave, int mx) {
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    int unlock = 0;
    if (ioctl(m, 0x40045431u, &unlock) != 0) { close(m); return -1; }
    int n = -1;
    if (ioctl(m, 0x80045430u, &n) != 0 || n < 0) { close(m); return -1; }
    snprintf(slave, mx, "/dev/pts/%d", n);
    return m;
}
struct wt_ptyw { int fd; volatile long done; volatile int err; };
static void* wt_pty_writer(void* v) {
    struct wt_ptyw* w = (struct wt_ptyw*)v;
    long off = 0;
    while (off < PTY_TEST_BYTES) {
        int chunk = (int)((off % 7 == 0) ? 1 : (off % 5 == 0) ? 8192 : 1024);
        if (chunk > PTY_TEST_BYTES - off) chunk = PTY_TEST_BYTES - (int)off;
        ssize_t r = write(w->fd, g_pty_exp + off, (size_t)chunk);
        if (r <= 0) { w->err = errno ? errno : -1; break; }
        off += (long)r;
    }
    w->done = off;
    return 0;
}
static void pty_case(void) {
    for (int i = 0; i < PTY_TEST_BYTES; i++)
        g_pty_exp[i] = (uint8_t)(((uint32_t)i * 2654435761u) >> 13);
    char slave[64];
    int m = wt_open_pty(slave, (int)sizeof(slave));
    char b[256];
    if (m < 0) { snprintf(b, sizeof b, "[irqlat] pty open FAILED errno=%d\n", errno); logs(b); return; }
    int sf = open(slave, O_RDWR | O_NOCTTY);
    if (sf < 0) { snprintf(b, sizeof b, "[irqlat] pty slave %s FAILED errno=%d\n", slave, errno); logs(b); close(m); return; }
    {   // raw tty: no ONLCR/OPOST translation, so the byte stream is comparable (satoru)
        struct termios tio;
        if (tcgetattr(sf, &tio) == 0) { cfmakeraw(&tio); tcsetattr(sf, TCSANOW, &tio); }
    }
    struct wt_ptyw w; w.fd = sf; w.done = 0; w.err = 0;
    uint64_t t0 = ct_us();
    pthread_t wt;
    pthread_create(&wt, 0, wt_pty_writer, &w);
    long rn = 0, bad = -1, badgot = -1, badwant = -1;
    uint8_t rb[1024];
    uint64_t last_ms = ct_us();
    while (rn < PTY_TEST_BYTES) {
        struct pollfd pf = { .fd = m, .events = POLLIN, .revents = 0 };
        int pr = poll(&pf, 1, 300);
        if (pr <= 0) {
            if (ct_us() - last_ms > 3000000ull) break;   // no progress: the writer dropped (old path) (satoru)
            continue;
        }
        ssize_t r = read(m, rb, sizeof(rb));
        if (r <= 0) {
            if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }
        last_ms = ct_us();
        for (ssize_t i = 0; i < r && rn < PTY_TEST_BYTES; i++, rn++) {
            if (bad < 0 && rb[i] != g_pty_exp[rn]) { bad = rn; badgot = rb[i]; badwant = g_pty_exp[rn]; }
        }
        if ((rn & 0x3FF) < 1024) ct_sleep_us(200);
    }
    pthread_join(wt, 0);
    snprintf(b, sizeof b,
             "[irqlat] pty integrity wrote=%ld read=%ld %s ms=%llu err=%d bad=%ld got=%ld want=%ld\n",
             w.done, rn, (rn == w.done && rn == PTY_TEST_BYTES && bad < 0) ? "MATCH" : "MISMATCH",
             (unsigned long long)((ct_us() - t0) / 1000), w.err, bad, badgot, badwant);
    logs(b);
    close(sf); close(m);
}
static long lock_wait_ms(const char* path, int ms, int sig_ms) {
    unlink(path);
    pid_t pid = fork();
    if (pid == 0) {
        int f = open(path, O_RDWR | O_CREAT, 0644);
        if (f >= 0) {
            flock(f, LOCK_EX);
            if (ms > 0) ct_sleep_us((uint64_t)ms * 1000ull);
            flock(f, LOCK_UN);
            close(f);
        }
        _exit(0);
    }
    ct_sleep_us(100000);
    int f = open(path, O_RDWR);
    if (f < 0) { waitpid(pid, 0, 0); return -1; }
    if (sig_ms > 0) {
        struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = wt_evt_handler;
        sigaction(SIGALRM, &sa, 0);
        struct itimerval it; memset(&it, 0, sizeof(it));
        it.it_value.tv_usec = (long)sig_ms * 1000; setitimer(ITIMER_REAL, &it, 0);
    }
    uint64_t t0 = ct_us();
    int r = flock(f, LOCK_EX);
    long el = (long)((ct_us() - t0) / 1000);
    if (sig_ms > 0) { struct itimerval it; memset(&it, 0, sizeof(it)); setitimer(ITIMER_REAL, &it, 0); }
    if (r == 0) flock(f, LOCK_UN);
    close(f);
    waitpid(pid, 0, 0);
    unlink(path);
    return r == 0 ? el : -el - 100000;
}
// 100 iterations: child holds the lock 30 ms, parent waits. both record their
// timestamps on the same clock and a shared counter proves the critical section
// never overlaps. (satoru)
struct wt_lk_shared { volatile int held; volatile int maxheld; volatile long long acq_us, rel_us; };
static void lock_excl_case(const char* path) {
    int r = 0, i;
    for (i = 0; i < 100; i++) {
        struct wt_lk_shared* sh = (struct wt_lk_shared*)mmap(0, sizeof(*sh),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (sh == MAP_FAILED) break;
        sh->held = 0; sh->maxheld = 0; sh->acq_us = 0; sh->rel_us = 0;
        pid_t pid = fork();
        if (pid == 0) {
            int f = open(path, O_RDWR | O_CREAT, 0644);
            if (f >= 0) {
                flock(f, LOCK_EX);
                int h = __atomic_add_fetch(&sh->held, 1, __ATOMIC_SEQ_CST);
                if (h > sh->maxheld) sh->maxheld = h;
                sh->acq_us = (long long)ct_us();
                ct_sleep_us(30000);
                sh->rel_us = (long long)ct_us();
                __atomic_sub_fetch(&sh->held, 1, __ATOMIC_SEQ_CST);
                flock(f, LOCK_UN);
                close(f);
            }
            _exit(0);
        }
        ct_sleep_us(10000);   // let the child take it
        int f = open(path, O_RDWR);
        if (f >= 0) {
            if (flock(f, LOCK_EX) == 0) {
                int h = __atomic_add_fetch(&sh->held, 1, __ATOMIC_SEQ_CST);
                if (h > sh->maxheld) sh->maxheld = h;
                long long a = (long long)ct_us();
                if (a < sh->rel_us) r++;   // acquired before the holder released
                flock(f, LOCK_UN);
                __atomic_sub_fetch(&sh->held, 1, __ATOMIC_SEQ_CST);
            }
            close(f);
        }
        waitpid(pid, 0, 0);
        if (sh->maxheld > 1) r += 1000;
        munmap(sh, sizeof(*sh));
    }
    char b[192];
    snprintf(b, sizeof b, "[irqlat] lock excl iterations=%d bad=%d maxheld=%d\n", i, r, 1);
    logs(b);
    unlink(path);
}

// SIGTERM to a process whose main thread polls(-1) on an empty pipe while a
// sibling busy-yields: is it killed, and how long does it take? (satoru)
static int sigterm_case(void) {
    int pp[2];
    if (pipe(pp) != 0) return 1;
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t bt;
        pthread_create(&bt, 0, wt_busy, 0);
        struct pollfd pf = { .fd = pp[0], .events = POLLIN, .revents = 0 };
        poll(&pf, 1, -1);
        _exit(0);   // only reached if the signal did not kill us
    }
    ct_sleep_us(300000);
    uint64_t t0 = ct_us();
    kill(pid, SIGTERM);
    int st = 0;
    while (ct_us() - t0 < 3000000ull) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            char b[160];
            snprintf(b, sizeof b, "[irqlat] sigterm killed=1 ms=%llu sig=%d\n",
                     (unsigned long long)((ct_us() - t0) / 1000),
                     WIFSIGNALED(st) ? WTERMSIG(st) : 0);
            logs(b); close(pp[0]); close(pp[1]);
            return 0;
        }
        ct_sleep_us(5000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    char b[160];
    snprintf(b, sizeof b, "[irqlat] sigterm killed=0 ms=3000 (still alive, had to SIGKILL)\n");
    logs(b);
    close(pp[0]); close(pp[1]);
    return 0;
}

static int parks_case(int busy, int secs) {
    (void)secs;
    pthread_t busy_th[64];
    int nb = busy > 64 ? 64 : busy;
    for (int i = 0; i < nb; i++) pthread_create(&busy_th[i], 0, wt_busy, 0);
    char b[256];
    pty_case();
    long l1 = lock_wait_ms("/tmp/irqlat.lk", 300, 0);
    snprintf(b, sizeof b, "[irqlat] lock release300 wait_ms=%ld (0 expected)\n", l1);
    logs(b);
    long l2 = lock_wait_ms("/tmp/irqlat.lk2", 12000, 0);
    snprintf(b, sizeof b, "[irqlat] lock never wait_result=%ld (EDEADLK ~10 s expected)\n", l2);
    logs(b);
    long l3 = lock_wait_ms("/tmp/irqlat.lk3", 2000, 300);
    snprintf(b, sizeof b, "[irqlat] lock signal result=%ld (EINTR expected)\n", l3);
    logs(b);
    lock_excl_case("/tmp/irqlat.lk4");
    sigterm_case();
    g_wt_stop = 1;
    for (int i = 0; i < nb; i++) pthread_join(busy_th[i], 0);
    logs("[irqlat] parks DONE\n");
    return 0;
}

static int connect_case(int busy, int secs) {
    (void)secs;
    pthread_t busy_th[64];
    int nb = busy > 64 ? 64 : busy;
    for (int i = 0; i < nb; i++) pthread_create(&busy_th[i], 0, wt_busy, 0);

    hist_reset();
    __atomic_store_n(&wt_mon_stop, 0, __ATOMIC_RELEASE);
    pthread_t mon;
    pthread_create(&mon, 0, wt_monitor, 0);

    char b[192];
    char b2buf[192];
    // 1. a listening port: connect succeeds, then the socket really works.
    uint64_t t0 = ct_us();
    int s = wt_connect_to("10.0.2.2", 8001);
    uint64_t el = (ct_us() - t0) / 1000;
    snprintf(b, sizeof b, "[irqlat] connect listen rc=%d err=%d ms=%llu\n",
             s >= 0 ? 0 : s, s < 0 ? -s : 0, (unsigned long long)el);
    logs(b);
    if (s >= 0) {
        const char req[] = "GET /hello.txt HTTP/1.0\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n";
        ssize_t wr = send(s, req, sizeof(req) - 1, 0);
        char buf[256];
        ssize_t rd = recv(s, buf, sizeof(buf) - 1, 0);
        snprintf(b, sizeof b, "[irqlat] connect listen io wr=%d rd=%d\n", (int)wr, (int)rd);
        logs(b);
        close(s);
    }
    // 2. a closed port: refused promptly.
    t0 = ct_us();
    s = wt_connect_to("10.0.2.2", 1);
    el = (ct_us() - t0) / 1000;
    snprintf(b, sizeof b, "[irqlat] connect refused rc=%d err=%d ms=%llu\n",
             s >= 0 ? 0 : s, s < 0 ? -s : 0, (unsigned long long)el);
    logs(b);
    if (s >= 0) close(s);
    // 3. a black hole: -ETIMEDOUT at the kernel deadline, cpu not held.
    t0 = ct_us();
    s = wt_connect_to("192.0.2.1", 9);
    el = (ct_us() - t0) / 1000;
    snprintf(b, sizeof b, "[irqlat] connect blackhole rc=%d err=%d ms=%llu\n",
             s >= 0 ? 0 : s, s < 0 ? -s : 0, (unsigned long long)el);
    logs(b);
    if (s >= 0) close(s);
    // 4. a signal mid-park: EINTR.
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = wt_sigalrm;
        sigaction(SIGALRM, &sa, 0);
        struct itimerval it;
        memset(&it, 0, sizeof(it));
        it.it_value.tv_usec = 200000;
        setitimer(ITIMER_REAL, &it, 0);
        t0 = ct_us();
        s = wt_connect_to("192.0.2.1", 9);
        el = (ct_us() - t0) / 1000;
        memset(&it, 0, sizeof(it));
        setitimer(ITIMER_REAL, &it, 0);
        snprintf(b, sizeof b, "[irqlat] connect signal rc=%d err=%d ms=%llu\n",
                 s >= 0 ? 0 : s, s < 0 ? -s : 0, (unsigned long long)el);
        logs(b);
        if (s >= 0) close(s);
    }
    // 6. close the fd from another thread while the connect is parked: the parked
    //    call must end (EBADF), and a new socket that reuses the fd number must
    //    connect normally (a stale in-flight record must not change its result).
    {
        struct wt_park_shared sh;
        sh.fd = -1; sh.rc = -999; sh.err = 0;
        pthread_t t;
        pthread_create(&t, 0, wt_conn_park_thread, &sh);
        for (int i = 0; i < 500 && sh.fd < 0; i++) ct_sleep_us(1000);
        ct_sleep_us(300000);   // let it park on the black hole
        int closing = sh.fd;
        if (closing >= 0) close(closing);
        pthread_join(t, 0);
        struct wt_args b = { "10.0.2.2", 8001, -999, 0 };
        wt_conn_thread(&b);
        snprintf(b2buf, sizeof b2buf,
                 "[irqlat] connect close_while_parked fd=%d parked_rc=%d new_rc=%d new_err=%d\n",
                 closing, sh.rc, b.rc < 0 ? b.rc : 0, b.err);
        logs(b2buf);
    }
    // 7. dup2 over the parked fd: the record mismatches (backend/instance), so the
    //    call is treated as a brand-new connect on the socket now at that fd.
    {
        struct wt_park_shared sh;
        sh.fd = -1; sh.rc = -999; sh.err = 0;
        pthread_t t;
        pthread_create(&t, 0, wt_conn_park_thread, &sh);
        for (int i = 0; i < 500 && sh.fd < 0; i++) ct_sleep_us(1000);
        ct_sleep_us(300000);
        int s2 = wt_connect_to("10.0.2.2", 8001);
        int over = sh.fd;
        if (s2 >= 0 && over >= 0) dup2(s2, over);
        pthread_join(t, 0);
        if (s2 >= 0) close(s2);
        snprintf(b2buf, sizeof b2buf,
                 "[irqlat] connect dup2_while_parked fd=%d parked_rc=%d (EISCONN expected)\n",
                 over, sh.rc);
        logs(b2buf);
    }
    // 8. a default-ignore signal (SIGWINCH) while parked must NOT end the connect:
    //    it still times out at the deadline, unlike an acting signal (test 4).
    {
        struct wt_args a = { "192.0.2.1", 9, -999, 0 };
        pthread_t t;
        pthread_create(&t, 0, wt_conn_run, &a);
        ct_sleep_us(300000);
        raise(SIGWINCH);
        uint64_t t0w = ct_us();
        pthread_join(t, 0);
        snprintf(b2buf, sizeof b2buf,
                 "[irqlat] connect ignored_signal rc=%d err=%d ms=%llu\n",
                 a.rc < 0 ? a.rc : 0, a.err, (unsigned long long)((ct_us() - t0w) / 1000));
        logs(b2buf);
    }

    // 9. poll EINTR policy: a default-ignore signal (SIGWINCH) must NOT end a
    //    poll (it waits out its timeout), while a handled SIGUSR1 must (EINTR).
    {
        int pp[2];
        int prc = pipe(pp);
        if (prc != 0) {
            snprintf(b2buf, sizeof b2buf, "[irqlat] poll pipe failed rc=%d errno=%d\n", prc, errno);
            logs(b2buf);
        }
        if (prc == 0) {
            struct pollfd pf = { .fd = pp[0], .events = POLLIN, .revents = 0 };
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_IGN;
            sigaction(SIGWINCH, &sa, 0);
            g_main_th = pthread_self();
            pthread_t kt;
            pthread_create(&kt, 0, wt_kill_main_soon, (void*)(long)SIGWINCH);
            uint64_t t0p = ct_us();
            int pr = poll(&pf, 1, 300);
            int pe = errno;
            pthread_join(kt, 0);
            snprintf(b2buf, sizeof b2buf,
                     "[irqlat] poll ignored_sig rc=%d errno=%d ms=%llu (0 expected)\n",
                     pr, pe, (unsigned long long)((ct_us() - t0p) / 1000));
            logs(b2buf);
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = wt_evt_handler;
            sigaction(SIGUSR1, &sa, 0);
            pthread_create(&kt, 0, wt_kill_main_soon, (void*)(long)SIGUSR1);
            t0p = ct_us();
            pr = poll(&pf, 1, 1000);
            pe = errno;
            pthread_join(kt, 0);
            snprintf(b2buf, sizeof b2buf,
                     "[irqlat] poll acting_sig rc=%d errno=%d ms=%llu (-1/EINTR expected)\n",
                     pr, pe, (unsigned long long)((ct_us() - t0p) / 1000));
            logs(b2buf);
            close(pp[0]); close(pp[1]);
        }
    }

    // 5. two concurrent connects on two sockets.
    {
        pthread_t t1, t2;
        struct wt_args a1 = { "10.0.2.2", 8001, -999, 0 };
        struct wt_args a2 = { "10.0.2.2", 8001, -999, 0 };
        pthread_create(&t1, 0, wt_conn_thread, &a1);
        pthread_create(&t2, 0, wt_conn_thread, &a2);
        pthread_join(t1, 0);
        pthread_join(t2, 0);
        snprintf(b, sizeof b, "[irqlat] connect two rc=%d,%d err=%d,%d\n",
                 a1.rc < 0 ? a1.rc : 0, a2.rc < 0 ? a2.rc : 0, a1.err, a2.err);
        logs(b);
    }
    __atomic_store_n(&wt_mon_stop, 1, __ATOMIC_RELEASE);
    pthread_join(mon, 0);
    wt_report("monitor_during_connect", hist_total(), hist_sum_us / (hist_total() ? hist_total() : 1));
    g_wt_stop = 1;
    for (int i = 0; i < nb; i++) pthread_join(busy_th[i], 0);
    logs("[irqlat] connect DONE\n");
    return 0;
}

static int contention_main(int busy, int secs, int mode) {
    g_wt_mode = mode;
    g_wt_busy_n = busy;
    g_wt_stop = 0;
    g_wt_case_stop = 0;
    char hdr[160];
    int hp = 0;
    const char* t1 = "[irqlat] contention busy=";
    while (*t1) hdr[hp++] = *t1++;
    { char b[12]; int k = 0, v = busy; if (!v) b[k++] = '0'; while (v) { b[k++] = (char)('0' + v % 10); v /= 10; } while (k) hdr[hp++] = b[--k]; }
    const char* t2 = " mode=";
    while (*t2) hdr[hp++] = *t2++;
    { char b[12]; int k = 0, v = mode; if (!v) b[k++] = '0'; while (v) { b[k++] = (char)('0' + v % 10); v /= 10; } while (k) hdr[hp++] = b[--k]; }
    const char* t3 = " secs=";
    while (*t3) hdr[hp++] = *t3++;
    { char b[12]; int k = 0, v = secs; if (!v) b[k++] = '0'; while (v) { b[k++] = (char)('0' + v % 10); v /= 10; } while (k) hdr[hp++] = b[--k]; }
    hdr[hp++] = '\n'; hdr[hp] = 0;
    logs(hdr);

    pthread_t busy_th[64];
    int nb = busy > 64 ? 64 : busy;
    for (int i = 0; i < nb; i++) pthread_create(&busy_th[i], 0, wt_busy, 0);

    hist_reset();
    pthread_t w;
    pthread_create(&w, 0, wt_futex_waiter, 0);
    uint64_t t_end = ct_us() + (uint64_t)secs * 500000ull;
    uint32_t target = 0;
    while (ct_us() < t_end) {
        while (__atomic_load_n(&wt_f_acked, __ATOMIC_ACQUIRE) < target) {
            if (ct_us() >= t_end) break;
            __asm__ __volatile__("pause");
        }
        uint64_t t = ct_us();
        __atomic_store_n(&wt_f_stamp, t, __ATOMIC_RELEASE);
        __atomic_add_fetch(&wt_f_seq, 1u, __ATOMIC_RELEASE);
        target++;
        fwake_u(&wt_f_seq, 1);
        ct_sleep_us(2000);
    }
    g_wt_case_stop = 1;
    fwake_u(&wt_f_seq, 1);
    pthread_join(w, 0);
    wt_report("futex", hist_total(), hist_sum_us / (hist_total() ? hist_total() : 1));
    g_wt_case_stop = 0;

    if (pipe(wt_p_fd) == 0) {
        hist_reset();
        pthread_create(&w, 0, wt_pipe_waiter, 0);
        target = 0;
        t_end = ct_us() + (uint64_t)secs * 500000ull;
        while (ct_us() < t_end) {
            while (__atomic_load_n(&wt_p_acked, __ATOMIC_ACQUIRE) < target) {
                if (ct_us() >= t_end) break;
                __asm__ __volatile__("pause");
            }
            uint64_t t = ct_us();
            __atomic_store_n(&wt_p_stamp, t, __ATOMIC_RELEASE);
            __atomic_add_fetch(&wt_p_seq, 1u, __ATOMIC_RELEASE);
            target++;
            char c = 'x';
            (void)!write(wt_p_fd[1], &c, 1);
            ct_sleep_us(2000);
        }
        g_wt_case_stop = 1;
        char c = 'x';
        (void)!write(wt_p_fd[1], &c, 1);
        pthread_join(w, 0);
        wt_report("pipe", hist_total(), hist_sum_us / (hist_total() ? hist_total() : 1));
        close(wt_p_fd[0]); close(wt_p_fd[1]);
        g_wt_case_stop = 0;
    }

    hist_reset();
    pthread_create(&w, 0, wt_nanosleep_waiter, 0);
    ct_sleep_us((uint64_t)secs * 1000000ull);
    g_wt_case_stop = 1;
    pthread_join(w, 0);
    wt_report("nanosleep", hist_total(), hist_sum_us / (hist_total() ? hist_total() : 1));
    g_wt_case_stop = 0;

    hist_reset();
    pthread_create(&w, 0, wt_alarm_waiter, 0);
    ct_sleep_us((uint64_t)secs * 1000000ull);
    g_wt_case_stop = 1;
    pthread_join(w, 0);
    wt_report("alarm_read", hist_total(), hist_sum_us / (hist_total() ? hist_total() : 1));

    g_wt_stop = 1;
    for (int i = 0; i < nb; i++) pthread_join(busy_th[i], 0);
    logs("[irqlat] contention DONE\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        int busy = atoi(argv[1]);
        int secs = argc >= 3 ? atoi(argv[2]) : 10;
        int mode = argc >= 4 ? atoi(argv[3]) : 0;
        if (busy < 0) busy = 0;
        if (secs < 1) secs = 1;
        logs("[irqlat] userspace contention probe starting\n");
        if (mode == 3) return connect_case(busy, secs);
        if (mode == 4) return parks_case(busy, secs);
        return contention_main(busy, secs, mode);
    }
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
