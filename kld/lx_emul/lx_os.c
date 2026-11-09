// lx_emul: the container's OS glue, in USERSPACE context (musl headers).
//
// Kernel-context shim files cannot include libc headers (they collide with the
// Linux headers), so everything that needs a real syscall or a libc facility
// lives here behind a tiny C ABI declared in kimpl/lx_kshim.h. Only plain
// integers and void pointers cross this line.
//
// end (satoru)

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/syscall.h>
// musl does not ship <linux/futex.h>; the two ops used are fixed abi. (satoru)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#include "lx_emul.h"

void lx_os_idle_ms(int ms);
void lx_os_sleep_ns(unsigned long long ns);

// ── threads ─────────────────────────────────────────────────────────────────
struct lx_os_thread_arg {
    void (*fn)(void *);
    void *arg;
};

static void *lx_os_trampoline(void *p)
{
    struct lx_os_thread_arg a = *(struct lx_os_thread_arg *)p;
    free(p);
    a.fn(a.arg);
    return NULL;
}

// real pthread with a 256 KiB stack (linux kernel stacks are 16 KiB, so this
// is generous for driver code). returns 0 on success. (satoru)
int lx_os_thread_create(void (*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t at;
    struct lx_os_thread_arg *a = malloc(sizeof(*a));
    int rc;
    if (!a)
        return -1;
    a->fn = fn;
    a->arg = arg;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&t, &at, lx_os_trampoline, a);
    pthread_attr_destroy(&at);
    if (rc) {
        free(a);
        return -1;
    }
    return 0;
}

void lx_os_yield(void) { sched_yield(); }

long lx_os_gettid(void) { return syscall(SYS_gettid); }

// ── futex ───────────────────────────────────────────────────────────────────
// shared (non-private) futex ops on purpose: the kernel matches waiters by
// physical key as well as by (address space, va), and the broker wakes doorbell
// words by physical key from interrupt context. (satoru)
// timeout_ns < 0 means forever. returns 0 woken/changed, 1 timed out. (satoru)
int lx_os_futex_wait(volatile unsigned int *addr, unsigned int val, long long timeout_ns)
{
    struct timespec ts, *tsp = NULL, t0, t1;
    long r;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000ll);
        ts.tv_nsec = (long)(timeout_ns % 1000000000ll);
        tsp = &ts;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = syscall(SYS_futex, (unsigned int *)addr, FUTEX_WAIT, val, tsp, NULL, 0);
    if (r < 0 && timeout_ns >= 0) {
        // -ETIMEDOUT vs -EAGAIN (value changed) vs -EINTR: only the first is a
        // timeout. errno is libc state, which is fine here (userspace ctx).
        extern int *__errno_location(void);
        if (*__errno_location() == 110)
            return 1;
    }
    // Kurono cannot deschedule the last runnable task: such a FUTEX_WAIT comes
    // straight back with the word unchanged. left alone, the waiter spins and,
    // on the bsp, starves the cooperative kernel processes (network, shell
    // runners). when that happens, idle 1 ms through poll(), which on the bsp
    // donates the slice to them. a real wake changes the word or arrives during
    // that ms, so the cost is bounded at 1 ms and only when nothing else could
    // run anyway. (satoru)
    if (r == 0 && *addr == val && (timeout_ns < 0 || timeout_ns > 300000)) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long long el = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec);
        if (el < 200000)
            lx_os_idle_ms(1);
    }
    return 0;
}

int lx_os_futex_wake(volatile unsigned int *addr, int n)
{
    return (int)syscall(SYS_futex, (unsigned int *)addr, FUTEX_WAKE, n, NULL, NULL, 0);
}

// ── memory ──────────────────────────────────────────────────────────────────
// reserve a VA range with NO backing frames yet (Kurono's anonymous mmap is
// demand-zero), so the broker can install device or dma frames into it. the
// broker refuses a range that already has a frame, which is why nothing may
// touch these pages before the broker op. (satoru)
void *lx_os_reserve(unsigned long bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void lx_os_unreserve(void *p, unsigned long bytes)
{
    if (p)
        munmap(p, bytes);
}

void *lx_os_malloc(unsigned long n) { return malloc(n); }
void *lx_os_aligned_alloc(unsigned long align, unsigned long n)
{
    void *p = NULL;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, n))
        return NULL;
    return p;
}
void lx_os_free(void *p) { free(p); }

// ── misc ────────────────────────────────────────────────────────────────────
void lx_os_random(void *buf, unsigned long n)
{
    unsigned char *b = buf;
    unsigned long got = 0;
    while (got < n) {
        long r = syscall(SYS_getrandom, b + got, n - got, 0);
        if (r <= 0)
            break;
        got += (unsigned long)r;
    }
    // never silently leave it zero: mix the clock in as a last resort. (satoru)
    if (got < n) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        for (unsigned long i = got; i < n; i++)
            b[i] = (unsigned char)((ts.tv_nsec >> ((i % 4) * 8)) ^ (i * 131));
    }
}

unsigned long long lx_os_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

void lx_os_sleep_ns(unsigned long long ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0) { }
}

// idle the CALLING thread for ms through poll(). this is the only wait that
// is friendly to Kurono's cooperative kernel processes when the caller is the
// thread on the bsp: a bsp poll with nothing to switch to donates its slice to
// the kernel processes (network, input, the shell runners), where a futex wait
// or nanosleep returns early or busy-waits and starves them all (measured: the
// network process never observed link-up, nettest never ran). (satoru)
// the fd must be REAL: Kurono's poll returns at once for nfds == 0 (it never
// reaches the wait path), so poll(NULL, 0, ms) neither sleeps nor donates. the
// read end of a pipe nobody writes is never ready. (satoru)
static int lx_idle_pipe[2] = { -1, -1 };

void lx_os_idle_ms(int ms)
{
    if (lx_idle_pipe[0] < 0 && pipe(lx_idle_pipe) != 0) {
        lx_os_sleep_ns((unsigned long long)ms * 1000000ull);
        return;
    }
    struct pollfd pf = { .fd = lx_idle_pipe[0], .events = POLLIN, .revents = 0 };
    (void)poll(&pf, 1, ms);
}

void lx_os_exit(int rc) { _exit(rc); }

// test helpers (kld/tests): whole-file read and raw stdout write. (satoru)
#include <fcntl.h>
long lx_os_read_file(const char *path, void *buf, unsigned long max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, buf, max);
    close(fd);
    return n;
}

void lx_os_write_out(const void *buf, unsigned long n) { (void)!write(1, buf, n); }

// end (satoru)
