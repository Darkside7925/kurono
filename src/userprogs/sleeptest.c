// interruptible-sleep self-test for the kurono linux abi. proves nanosleep,
// usleep, clock_nanosleep(ABSTIME) sleep for the requested wall-clock time, that
// a handled SIGALRM ends the sleep with -EINTR + a sane rem, that an ignored
// SIGALRM does not, and that an invalid timespec is EINVAL. the whole suite also
// runs from 3 pthreads at once. prints "[sleeptest] PASS/FAIL ..." lines and a
// final "SLEEPTEST PASS" (or FAIL n) to stderr (com1 under the cli runner).
// built musl-static, same shape as signal_test.c. (satoru)
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void logs(const char* s) { write(2, s, strlen(s)); }
static void logu(long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[n++] = t[--k];
    write(2, b, n);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
static long elapsed_ms(double t0) { return (long)(now_ms() - t0 + 0.5); }

static void report(const char* name, int ok, long got, long want) {
    logs(ok ? "[sleeptest] PASS " : "[sleeptest] FAIL ");
    logs(name);
    logs(" elapsed_ms="); logu(got);
    logs(" want_ms=");    logu(want);
    logs("\n");
    if (!ok) __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
}

static int check_elapsed(const char* name, long got, long want, long tol) {
    int ok = got >= want - tol && got <= want + tol;
    report(name, ok, got, want);
    return ok;
}

// per-thread: a signal handler runs on the thread the signal was posted to. (satoru)
static __thread volatile sig_atomic_t alrm_seen;
static pthread_mutex_t sig_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_alrm(int s) { (void)s; alrm_seen++; }

static void test_sleep_secs(const char* name, int secs) {
    struct timespec req;
    req.tv_sec = secs; req.tv_nsec = 0;
    errno = 0;
    double t0 = now_ms();
    int r = nanosleep(&req, 0);
    long el = elapsed_ms(t0);
    if (r != 0) { report(name, 0, el, secs * 1000L); return; }
    check_elapsed(name, el, secs * 1000L, 50);
}

static void test_usleep(void) {
    errno = 0;
    double t0 = now_ms();
    int r = usleep(6500000);
    long el = elapsed_ms(t0);
    if (r != 0) { report("usleep6500", 0, el, 6500); return; }
    check_elapsed("usleep6500", el, 6500, 50);
}

static void test_abstime(void) {
    struct timespec req;
    clock_gettime(CLOCK_MONOTONIC, &req);
    req.tv_sec += 8;
    errno = 0;
    double t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, 0);
    long el = elapsed_ms(t0);
    if (r != 0) { report("abstime8000", 0, el, 8000); return; }
    check_elapsed("abstime8000", el, 8000, 50);
}

static void test_alarm_eintr(void) {
    pthread_mutex_lock(&sig_mu);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, 0) != 0) {
        pthread_mutex_unlock(&sig_mu);
        report("alarm_eintr", 0, 0, 2000);
        return;
    }
    alrm_seen = 0;
    alarm(2);
    struct timespec req = { 10, 0 };
    struct timespec rem = { 0, 0 };
    errno = 0;
    double t0 = now_ms();
    int r = nanosleep(&req, &rem);
    long el = elapsed_ms(t0);
    long rem_ms = (long)rem.tv_sec * 1000L + rem.tv_nsec / 1000000L;
    int ok = (r == -1) && (errno == EINTR) && el >= 1900 && el <= 2600 &&
             rem_ms >= 7000 && rem_ms <= 8600 && alrm_seen == 1;
    if (!ok) {
        logs("[sleeptest] FAIL alarm_eintr detail r="); logu(r);
        logs(" errno="); logu(errno);
        logs(" rem_ms="); logu(rem_ms);
        logs(" handler="); logu(alrm_seen);
        logs("\n");
    }
    report("alarm_eintr", ok, el, 2000);
    pthread_mutex_unlock(&sig_mu);
}

static void test_alarm_ignored(void) {
    pthread_mutex_lock(&sig_mu);
    signal(SIGALRM, SIG_IGN);
    alarm(1);
    struct timespec req = { 2, 0 };
    errno = 0;
    double t0 = now_ms();
    int r = nanosleep(&req, 0);
    long el = elapsed_ms(t0);
    int ok = (r == 0) && el >= 1950 && el <= 2100;
    report("alarm_ignored", ok, el, 2000);
    signal(SIGALRM, SIG_DFL);
    pthread_mutex_unlock(&sig_mu);
}

// tv_sec near 2^63 must not wrap to a short sleep: it clamps to "forever" and
// is still ended by a signal with -EINTR and a huge rem. (satoru)
static void test_infinite_eintr(void) {
    pthread_mutex_lock(&sig_mu);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);
    alrm_seen = 0;
    alarm(2);
    struct timespec req;
    req.tv_sec = (time_t)((~(uint64_t)0) >> 1);   // INT64_MAX
    req.tv_nsec = 0;
    struct timespec rem = { 0, 0 };
    errno = 0;
    double t0 = now_ms();
    int r = nanosleep(&req, &rem);
    long el = elapsed_ms(t0);
    int ok = (r == -1) && (errno == EINTR) && el >= 1900 && el <= 2600 &&
             alrm_seen == 1 && rem.tv_sec > 1000000000L;
    report("inf_sleep_eintr", ok, el, 2000);
    pthread_mutex_unlock(&sig_mu);
}

// clock_nanosleep relative also reports the remaining time on EINTR. (satoru)
static void test_clock_relative_eintr(void) {
    pthread_mutex_lock(&sig_mu);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);
    alrm_seen = 0;
    alarm(2);
    struct timespec req = { 10, 0 };
    struct timespec rem = { 0, 0 };
    double t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem);
    long el = elapsed_ms(t0);
    long rem_ms = (long)rem.tv_sec * 1000L + rem.tv_nsec / 1000000L;
    int ok = (r == EINTR) && el >= 1900 && el <= 2600 &&
             rem_ms >= 7000 && rem_ms <= 8600 && alrm_seen == 1;
    report("clock_rel_eintr", ok, el, 2000);
    pthread_mutex_unlock(&sig_mu);
}

// TIMER_ABSTIME must leave rem untouched when a signal ends the wait. (satoru)
static void test_clock_abstime_rem(void) {
    pthread_mutex_lock(&sig_mu);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);
    alrm_seen = 0;
    alarm(2);
    struct timespec req;
    clock_gettime(CLOCK_MONOTONIC, &req);
    req.tv_sec += 10;
    struct timespec rem = { 1234, 567000000 };
    double t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, &rem);
    long el = elapsed_ms(t0);
    int ok = (r == EINTR) && el >= 1900 && el <= 2600 && alrm_seen == 1 &&
             rem.tv_sec == 1234 && rem.tv_nsec == 567000000;
    report("abstime_rem_keep", ok, el, 2000);
    pthread_mutex_unlock(&sig_mu);
}

// a BLOCKED SIGALRM must not end the sleep either; it is delivered once the
// mask is lifted (at that syscall boundary). (satoru)
static void test_alarm_blocked(void) {
    pthread_mutex_lock(&sig_mu);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGALRM);
    errno = 0;
    int br = sigprocmask(SIG_BLOCK, &blk, 0);
    sigset_t cur;
    sigemptyset(&cur);
    sigprocmask(SIG_BLOCK, 0, &cur);
    int mask_ok = (br == 0) && sigismember(&cur, SIGALRM) == 1;
    alrm_seen = 0;
    alarm(1);
    struct timespec req = { 2, 0 };
    double t0 = now_ms();
    int r = nanosleep(&req, 0);
    long el = elapsed_ms(t0);
    int ok = mask_ok && (r == 0) && el >= 1950 && el <= 2100 && alrm_seen == 0;
    sigprocmask(SIG_UNBLOCK, &blk, 0);
    if (alrm_seen != 1) ok = 0;
    if (!ok) {
        logs("[sleeptest] FAIL alarm_blocked detail mask="); logu(mask_ok);
        logs(" r="); logu(r);
        logs(" errno="); logu(errno);
        logs(" handler="); logu(alrm_seen);
        logs("\n");
    }
    report("alarm_blocked", ok, el, 2000);
    pthread_mutex_unlock(&sig_mu);
}

static void test_invalid(void) {
    struct timespec bad[3];
    bad[0].tv_sec = 0; bad[0].tv_nsec = 1000000000;   // nsec == 1e9
    bad[1].tv_sec = 0; bad[1].tv_nsec = -1;
    bad[2].tv_sec = -1; bad[2].tv_nsec = 0;
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        errno = 0;
        int r = nanosleep(&bad[i], 0);
        if (r != -1 || errno != EINVAL) {
            ok = 0;
            logs("[sleeptest] FAIL invalid detail i="); logu(i);
            logs(" r="); logu(r); logs(" errno="); logu(errno); logs("\n");
        }
    }
    report("invalid_einval", ok, 0, 0);
}

// the complete suite. run once on the main thread and once per worker thread,
// so three copies sleep concurrently. (satoru)
static void run_suite(const char* tag) {
    logs("[sleeptest] suite "); logs(tag); logs(" start\n");
    test_sleep_secs("sleep7", 7);
    test_sleep_secs("sleep30", 30);
    test_usleep();
    test_abstime();
    test_alarm_eintr();
    test_alarm_ignored();
    test_infinite_eintr();
    test_clock_relative_eintr();
    test_clock_abstime_rem();
    test_alarm_blocked();
    test_invalid();
    logs("[sleeptest] suite "); logs(tag); logs(" done\n");
}

static void* thread_suite(void* arg) {
    run_suite((const char*)arg);
    return 0;
}

int main(void) {
    logs("SLEEPTEST: start\n");
    run_suite("main");

    pthread_t th[3];
    const char* names[3] = { "t0", "t1", "t2" };
    int started = 0;
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&th[i], 0, thread_suite, (void*)names[i]) == 0) started++;
    }
    for (int i = 0; i < started; i++) pthread_join(th[i], 0);

    if (failures || started != 3) {
        logs("SLEEPTEST FAIL n="); logu(failures);
        logs(" threads="); logu(started); logs("\n");
        return 1;
    }
    logs("SLEEPTEST PASS\n");
    return 0;
}
// end (satoru)
