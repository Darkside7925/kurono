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
#include <errno.h>
#include <poll.h>
#include <sys/syscall.h>
// musl does not ship <linux/futex.h>; the two ops used are fixed abi. (satoru)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#include "lx_emul.h"
#include "kimpl/lx_broker.h"

void lx_os_idle_ms(int ms);
void lx_os_sleep_ns(unsigned long long ns);
void lx_jiffies_update(void);

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

void lx_stats_yield_begin(void);
void lx_stats_yield_end(void);

// ── N1b: rx hop timestamps ──────────────────────────────────────────────────
// rdtsc on both sides: the kernel sanitizes TSC_ADJUST on the bsp before any
// base is taken, and qemu keeps the invariant tsc in sync across vcpus, so a
// raw cycle difference between a kernel stamp and a container stamp is the
// same clock. the frequency comes from one broker read (KLD_OP_TSC_HZ). the
// hop ids are 0 t0-t1 (kernel isr entry -> irq thread woke), 1 t1-t2 (driver
// handler done), 2 t2-t3 (napi thread starts the poll) and 3 t3-t4 (frame
// stored in the rx ring). (satoru)
static inline unsigned long long n1b_rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
static unsigned long long n1b_hz_cache;
static unsigned long long n1b_hz(void)
{
    if (!n1b_hz_cache) {
        long h = lx_broker_call(LX_KLD_OP_TSC_HZ, 0, 0, 0, 0);
        n1b_hz_cache = h > 0 ? (unsigned long long)h : 1000000000ull;
    }
    return n1b_hz_cache;
}
static unsigned long long n1b_n[4], n1b_sum[4], n1b_max[4], n1b_slow[4], n1b_loud;
static unsigned long long n1b_slow_found, n1b_slow_none;
volatile unsigned long long lx_n1b_t2;
volatile unsigned long long lx_n1b_t3;
unsigned long lx_n1b_wakes, lx_n1b_handlers, lx_n1b_polls, lx_n1b_empty;

extern int lx_stats_on(void);
// 0 unless KLD_STATS: every hop consumer skips a zero stamp, so with stats off
// the hop path costs one flag test and prints nothing. (satoru)
unsigned long long lx_n1b_tsc(void) { return lx_stats_on() ? n1b_rdtsc() : 0; }

static void lx_n1b_hop_tag_body(int id, unsigned long long a, unsigned long long b, int tag)
{
    if (id < 0 || id > 3 || !b || b < a || !a)
        return;
    unsigned long long d = b - a, hz = n1b_hz();
    n1b_n[id]++;
    n1b_sum[id] += d;
    if (d > n1b_max[id])
        n1b_max[id] = d;
    if (d > hz / 2) {
        n1b_slow[id]++;
        if (id == 0 && tag == 1) n1b_slow_found++;
        else if (id == 0 && tag == 2) n1b_slow_none++;
    }
    if (d > hz / 2 && b - n1b_loud > hz) {
        static const char *nm[4] = { "t0-t1", "t1-t2", "t2-t3", "t3-t4" };
        n1b_loud = b;
        lx_log("[lx_n1b] hop ");
        lx_log(nm[id]);
        lx_log(" ");
        lx_log_u(d * 1000000ull / hz);
        lx_log("us\n");
    }
}

void lx_n1b_hop_tag(int id, unsigned long long a, unsigned long long b, int tag)
{
    lx_n1b_hop_tag_body(id, a, b, tag);
}

void lx_n1b_hop(int id, unsigned long long a, unsigned long long b)
{
    lx_n1b_hop_tag_body(id, a, b, 0);
}

static void n1b_u(unsigned long long v)
{
    char b[24];
    int k = 0;
    if (!v) b[k++] = '0';
    while (v) { b[k++] = (char)('0' + (v % 10)); v /= 10; }
    char r[24];
    int n = 0;
    while (k) r[n++] = b[--k];
    r[n] = 0;
    lx_log(r);
}

void lx_n1b_dump(void)
{
    unsigned long long hz = n1b_hz();
    static const char *nm[4] = { "t0-t1", "t1-t2", "t2-t3", "t3-t4" };
    for (int i = 0; i < 4; i++) {
        lx_log("[lx_n1b] ");
        lx_log(nm[i]);
        lx_log(" n=");
        n1b_u(n1b_n[i]);
        lx_log(" avg_us=");
        n1b_u(n1b_n[i] ? n1b_sum[i] * 1000000ull / n1b_n[i] / hz : 0);
        lx_log(" max_us=");
        n1b_u(n1b_max[i] * 1000000ull / hz);
        lx_log(" slow=");
        n1b_u(n1b_slow[i]);
        lx_log("\n");
    }
    lx_log("[lx_n1b] slow_found=");
    n1b_u(n1b_slow_found);
    lx_log(" slow_none=");
    n1b_u(n1b_slow_none);
    lx_log("\n");
    lx_log("[lx_n1b] c wakes_seen=");
    n1b_u(lx_n1b_wakes);
    lx_log(" handlers=");
    n1b_u(lx_n1b_handlers);
    lx_log(" napi_polls=");
    n1b_u(lx_n1b_polls);
    lx_log(" empty=");
    n1b_u(lx_n1b_empty);
    lx_log("\n");
}

void lx_os_yield(void)
{
    lx_stats_yield_begin();
    sched_yield();
    lx_stats_yield_end();
}

long lx_os_gettid(void) { return syscall(SYS_gettid); }

// ── futex ───────────────────────────────────────────────────────────────────
// shared (non-private) futex ops on purpose: the kernel matches waiters by
// physical key as well as by (address space, va), and the broker wakes doorbell
// words by physical key from interrupt context. (satoru)
// timeout_ns < 0 means forever. returns 0 woken/changed, 1 timed out. (satoru)
void lx_stats_wait_begin(const void *key);
void lx_stats_wait_end(void);

int lx_os_futex_wait(volatile unsigned int *addr, unsigned int val, long long timeout_ns)
{
    lx_stats_wait_begin(addr);
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
        if (*__errno_location() == 110) {
            lx_stats_wait_end();
            lx_jiffies_update();
            return 1;
        }
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
    lx_stats_wait_end();
    lx_jiffies_update();
    return 0;
}

void lx_stats_wake_addr(const void *key);

int lx_os_futex_wake(volatile unsigned int *addr, int n)
{
    int r = (int)syscall(SYS_futex, (unsigned int *)addr, FUTEX_WAKE, n, NULL, NULL, 0);
    lx_stats_wake_addr(addr);
    return r;
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

long lx_os_munmap_rc(void *p, unsigned long bytes)
{
    if (!p) return -22;
    errno = 0;
    if (munmap(p, bytes) == 0) return 0;
    return -(long)errno;
}

long lx_os_madvise_rc(void *p, unsigned long bytes, int advice)
{
    if (!p) return -22;
    errno = 0;
    if (madvise(p, bytes, advice) == 0) return 0;
    return -(long)errno;
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
// read the current task's user gs base from kurono (ARCH_GET_GS). used by the
// shim diagnostics to prove whether ring 3 ever sees a nonzero base. (satoru)
long lx_os_gs_base(unsigned long long *out)
{
    return syscall(SYS_arch_prctl, 0x1004, out);
}

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
    lx_jiffies_update();
}

// idle the CALLING thread for ms through poll(). this is the only wait that
// is friendly to Kurono's cooperative kernel processes when the caller is the
// thread on the bsp: a bsp poll with nothing to switch to donates its slice to
// the kernel processes (network, input, the shell runners), where a futex wait
// or nanosleep returns early or busy-waits and starves them all (measured: the
// network process never observed link-up, nettest never ran). (satoru)
// the fd must be REAL: Kurono's poll returns at once for nfds == 0 (it never
// reaches the wait path), so poll(NULL, 0, ms) neither sleeps nor donates. the
// read end of a pipe nobody writes is never ready. one pipe per thread: two
// threads polling the same read end leave one of them unwoken on the kernel's
// poll path (N2), which wedged rcu_overlap's two readers. (satoru)
static __thread int lx_idle_pipe[2] = { -1, -1 };

void lx_os_idle_ms(int ms)
{
    if (lx_idle_pipe[0] < 0 && pipe(lx_idle_pipe) != 0) {
        lx_os_sleep_ns((unsigned long long)ms * 1000000ull);
        return;
    }
    struct pollfd pf = { .fd = lx_idle_pipe[0], .events = POLLIN, .revents = 0 };
    (void)poll(&pf, 1, ms);
    lx_jiffies_update();
}

void lx_os_exit(int rc) { _exit(rc); }

// test helpers (kld/tests): whole-file read and raw stdout write. (satoru)
#include <fcntl.h>
#include <sys/stat.h>
long lx_os_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

long lx_os_read_file(const char *path, void *buf, unsigned long max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, buf, max);
    close(fd);
    return n;
}

void lx_os_write_out(const void *buf, unsigned long n) { (void)!write(1, buf, n); }

// ── supervisor heartbeat + fault injection (kurono.kld.sup) ─────────────────
// one thread pings the broker once a second. KLD_FAULTINJECT (first start only,
// set by the kernel) fires about 8 s in: segv writes through null, hang stops
// the pings and parks the main loop. (satoru)
long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                    unsigned long a2, unsigned long a3);
static volatile int lx_os_hang_flag;

int lx_os_hang_requested(void) { return lx_os_hang_flag; }

static void lx_os_heartbeat_thread(void *arg)
{
    const char *fi = getenv("KLD_FAULTINJECT");
    int kind = !fi ? 0 : (fi[0] == 's' ? 1 : (fi[0] == 'h' ? 2 : (fi[0] == 'e' ? 3 : (fi[0] == 'n' ? 4 : 0))));
    const unsigned long long sec = 1000000000ull;
    unsigned long long start = lx_os_now_ns(), next = start;
    (void)arg;
    if (kind == 4) {
        // nobeat: hang before the first heartbeat (the 60 s first-beat bound). (satoru)
        lx_os_write_out("[kld] faultinject: nobeat now\n", 30);
        lx_os_hang_flag = 1;
        for (;;) sleep(1000);
    }
    for (;;) {
        unsigned long long now = lx_os_now_ns();
        if (kind && now - start >= 8 * sec) {
            if (kind == 1) {
                lx_os_write_out("[kld] faultinject: segv now\n", 28);
                *(volatile int *)0 = 1;
            }
            if (kind == 3) {
                // the main thread returns 0 from main: a clean exit_group. (satoru)
                lx_os_write_out("[kld] faultinject: exit0 now\n", 29);
                lx_os_hang_flag = 2;
                for (;;) sleep(1000);
            }
            lx_os_write_out("[kld] faultinject: hang now\n", 28);
            lx_os_hang_flag = 1;
            for (;;) sleep(1000);
        }
        lx_broker_call(0x003Au, 0, 0, 0, 0);
        // absolute schedule: a late wake-up does not push every later beat
        // back; a long stall skips the missed beats instead of bursting. (satoru)
        next += sec;
        now = lx_os_now_ns();
        if (next <= now) next = now + sec;
        lx_os_sleep_ns(next - now);
    }
}

// only a container the kernel supervisor started (KLD_SUPERVISED=1) runs the
// thread; any other start never sends the op. (satoru)
void lx_os_heartbeat_start(void)
{
    const char *sv = getenv("KLD_SUPERVISED");
    if (sv && sv[0] == '1')
        lx_os_thread_create(lx_os_heartbeat_thread, NULL);
}

// physical memory and cpus as the container's libc sees them (the kernel's
// sysinfo / sched affinity); 0 or less when unknown. (satoru)
unsigned long lx_os_phys_pages(void)
{
    long n = sysconf(_SC_PHYS_PAGES);
    return n > 0 ? (unsigned long)n : 0;
}

long lx_os_ncpus(void)
{
    return sysconf(_SC_NPROCESSORS_CONF);
}

// write 4 known bytes into a pipe and read(2) them straight into dst: the
// kernel writes dst, which the bar policy rig points at a read-only page.
// returns the byte count or -errno. (satoru)
long lx_os_read_into(const char *unused, void *dst, unsigned long n)
{
    int fd[2];
    long r;
    static const char msg[4] = { 'K', 'L', 'D', '!' };
    (void)unused;
    if (pipe(fd) < 0)
        return -errno;
    if (write(fd[1], msg, sizeof(msg)) != (long)sizeof(msg)) {
        close(fd[0]); close(fd[1]);
        return -EIO;
    }
    r = read(fd[0], dst, n < sizeof(msg) ? n : sizeof(msg));
    r = r < 0 ? -errno : r;
    close(fd[0]);
    close(fd[1]);
    return r;
}

// bar policy rig (turn 6): a syscall whose OUTPUT buffer is dst. kind 1
// ioctl(FIONREAD) on a pipe holding 4 bytes, 2 recvmsg on a unix socketpair,
// 3 readlink("/proc/self/exe"). returns the syscall's result or -errno.
// (satoru)
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
long lx_os_syscall_into(int kind, void *dst, unsigned long n)
{
    long r = -EINVAL;
    if (kind == 1) {
        int fd[2];
        if (pipe(fd) < 0)
            return -errno;
        if (write(fd[1], "KLD!", 4) == 4)
            r = ioctl(fd[0], FIONREAD, dst) < 0 ? -errno : 0;
        close(fd[0]);
        close(fd[1]);
    } else if (kind == 2) {
        int sv[2];
        struct iovec iov = { .iov_base = dst, .iov_len = n };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
            return -errno;
        if (write(sv[1], "KLD!", 4) == 4)
            r = recvmsg(sv[0], &mh, 0) < 0 ? -errno : 4;
        close(sv[0]);
        close(sv[1]);
    } else if (kind == 3) {
        r = readlink("/proc/self/exe", dst, n);
        r = r < 0 ? -errno : r;
    }
    return r;
}

// end (satoru)
