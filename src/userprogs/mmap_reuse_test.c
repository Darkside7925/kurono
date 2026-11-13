// mmap_reuse_test.c - L2 mmap hole reuse tests (musl static, like pthread_test).
//
// T1: thread churn. 8 threads x 100000 anonymous mmap/fill/verify/munmap rounds
//     with the sizes the llvmpipe workload uses (plus 4k..1m).
// T2: stale-TLB across cpus. A maps and writes pattern1, B reads it so its cpu
//     caches the translation, A munmaps and immediately remaps the same size;
//     when the address comes back the same, B must read zero and then pattern2,
//     never pattern1. 100000 rounds.
// T3: thread stacks. 20000 pthreads in batches of 16; each recurses 32 frames
//     with a local canary.
//
// pool2 / share: wl_shm pool pin tests for kurono.wl.poolfree=1 (a raw-protocol
// client, like wl_shm_test). pool2: two pools on one memfd, the owner closes its
// fd, destroy one pool, the other must still display, then destroy it. share:
// the memfd also goes to a second receiver over SCM_RIGHTS; after the owner
// closes and the compositor drops its pool the receiver's copy must still read
// the pattern.
//
// T4: a dying thread must not write into a reused mapping (clear_child_tid).
// usage: mmap_reuse_test [t1|t2|t3|t4|all|pool2|share]   (all = t1 t2 t3; default all)
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define T1_THREADS 8
#define T1_ITERS   100000
#define T2_ROUNDS  100000
#define T3_THREADS 20000
#define T3_BATCH   16

// musl's sched_getcpu() reads its rseq area (the kernel does not implement
// rseq here), so use the getcpu syscall directly. (satoru)
static int test_cpu(void) {
    unsigned c = 0;
    long r = syscall(SYS_getcpu, &c, 0, 0);
    if (r != 0) return (int)(-1000 - r);   // encode the syscall error in the report
    return (int)c;
}
static uint64_t rng_next(uint64_t* s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s >> 33;
}
static const size_t t1_sizes[] = { 0xD000, 0x11000, 0x1000, 0x2000, 0x8000, 0x10000, 0x100000 };
#define T1_NSIZES ((int)(sizeof(t1_sizes) / sizeof(t1_sizes[0])))

struct t1_arg {
    int tid;
    uint64_t seed;
    atomic_ullong bad, fail;
};

static void* t1_thread(void* ap) {
    struct t1_arg* a = (struct t1_arg*)ap;
    uint64_t bad = 0, fail = 0;
    for (int it = 0; it < T1_ITERS; it++) {
        size_t sz = t1_sizes[rng_next(&a->seed) % T1_NSIZES];
        uint8_t* p = (uint8_t*)mmap(0, sz, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { fail++; continue; }
        // fresh anonymous memory must be zero (catches a frame not re-zeroed).
        if (p[0] != 0 || p[sz - 1] != 0) bad++;
        uint64_t pat = ((uint64_t)(uint32_t)a->tid << 32) | (uint64_t)(uint32_t)(it + 1);
        uint64_t* w = (uint64_t*)p;
        size_t nw = sz / 8;
        for (size_t i = 0; i < nw; i++) w[i] = pat;
        if (w[0] != pat || w[nw - 1] != pat || w[nw / 2] != pat) bad++;
        if ((it & 7) == 0) sched_yield();
        munmap(p, sz);
    }
    atomic_fetch_add_explicit(&a->bad, bad, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->fail, fail, memory_order_relaxed);
    return 0;
}

struct t2_ctl {
    atomic_int st;
    uint8_t* a;
    uint8_t* b;
    size_t   sz;
    uint64_t pat1, pat2;
    uint64_t rbad, zbad, pbad, same, diffcpu;
    int      cpu_a, cpu_b;
};

static void* t2_a(void* ap) {
    struct t2_ctl* c = (struct t2_ctl*)ap;
    uint64_t seed = 0x12345678;
    for (int r = 0; r < T2_ROUNDS; r++) {
        size_t sz = (rng_next(&seed) & 1) ? 0xD000 : 0x11000;
        uint8_t* p = (uint8_t*)mmap(0, sz, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { atomic_store(&c->st, 99); return 0; }
        c->pat1 = 0xA5A5A5A500000000ULL ^ (uint64_t)r;
        c->pat2 = 0x5A5A5A5A00000000ULL ^ (uint64_t)r;
        uint64_t* w = (uint64_t*)p;
        size_t nw = sz / 8;
        for (size_t i = 0; i < nw; i++) w[i] = c->pat1;
        c->a = p; c->sz = sz;
        c->cpu_a = test_cpu();
        atomic_store_explicit(&c->st, 1, memory_order_seq_cst);
        while (atomic_load_explicit(&c->st, memory_order_seq_cst) != 2) sched_yield();
        munmap(p, sz);
        uint8_t* q = (uint8_t*)mmap(0, sz, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (q == MAP_FAILED) { atomic_store(&c->st, 99); return 0; }
        c->b = q;
        if (q == p) c->same++;
        atomic_store_explicit(&c->st, 3, memory_order_seq_cst);
        while (atomic_load_explicit(&c->st, memory_order_seq_cst) != 4) sched_yield();
        uint64_t* qw = (uint64_t*)q;
        for (size_t i = 0; i < nw; i++) qw[i] = c->pat2;
        atomic_store_explicit(&c->st, 5, memory_order_seq_cst);
        while (atomic_load_explicit(&c->st, memory_order_seq_cst) != 6) sched_yield();
        munmap(q, sz);
    }
    atomic_store_explicit(&c->st, 7, memory_order_seq_cst);
    return 0;
}

static void* t2_b(void* ap) {
    struct t2_ctl* c = (struct t2_ctl*)ap;
    // try to run on a cpu other than A's. If the call is ignored the test still
    // checks the values, just without a guaranteed second TLB. (satoru)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(1, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    for (int r = 0; r < T2_ROUNDS; r++) {
        while (atomic_load_explicit(&c->st, memory_order_seq_cst) != 1) sched_yield();
        c->cpu_b = test_cpu();
        if (c->cpu_a >= 0 && c->cpu_b >= 0 && c->cpu_a != c->cpu_b) c->diffcpu++;
        uint64_t v = *(volatile uint64_t*)c->a;      // cache A's translation on this cpu
        if (v != c->pat1) c->rbad++;
        atomic_store_explicit(&c->st, 2, memory_order_seq_cst);
        while (atomic_load_explicit(&c->st, memory_order_seq_cst) != 3) sched_yield();
        if (c->b == c->a) {
            uint64_t z = *(volatile uint64_t*)c->a;  // must be the new zero page
            if (z != 0) c->zbad++;
        }
        atomic_store_explicit(&c->st, 4, memory_order_seq_cst);
        while (atomic_load_explicit(&c->st, memory_order_seq_cst) != 5) sched_yield();
        if (c->b == c->a) {
            uint64_t p = *(volatile uint64_t*)c->a;  // must be pattern2, never pattern1
            if (p != c->pat2) {
                c->pbad++;
                if (p == c->pat1) c->rbad++;
            }
        }
        atomic_store_explicit(&c->st, 6, memory_order_seq_cst);
    }
    return 0;
}

static int recurse(int depth, uint32_t magic) {
    volatile uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(magic + (uint32_t)depth + (uint32_t)i);
    int r = depth ? recurse(depth - 1, magic) : 1;
    for (int i = 0; i < 256; i++)
        if (buf[i] != (uint8_t)(magic + (uint32_t)depth + (uint32_t)i)) r = 0;
    return r;
}

static void* t3_thread(void* ap) {
    uint32_t magic = (uint32_t)(uintptr_t)ap;
    int ok = recurse(32, magic);
    return (void*)(uintptr_t)(ok ? 1 : 0);
}

static int run_t1(void) {
    pthread_t th[T1_THREADS];
    struct t1_arg arg[T1_THREADS];
    for (int i = 0; i < T1_THREADS; i++) {
        arg[i].tid = i;
        arg[i].seed = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 3);
        atomic_init(&arg[i].bad, 0);
        atomic_init(&arg[i].fail, 0);
        if (pthread_create(&th[i], 0, t1_thread, &arg[i]) != 0) return 1;
    }
    uint64_t bad = 0, fail = 0;
    for (int i = 0; i < T1_THREADS; i++) {
        pthread_join(th[i], 0);
        bad += atomic_load(&arg[i].bad);
        fail += atomic_load(&arg[i].fail);
    }
    printf("[mmrt] t1 iters=%d bad=%llu fail=%llu\n",
           T1_THREADS * T1_ITERS, (unsigned long long)bad, (unsigned long long)fail);
    return bad || fail;
}

static int run_t2(int expect_reuse) {
    struct t2_ctl c;
    memset(&c, 0, sizeof(c));
    atomic_init(&c.st, 0);
    c.cpu_a = c.cpu_b = -1;
    pthread_t a, b;
    if (pthread_create(&a, 0, t2_a, &c) != 0) return 1;
    if (pthread_create(&b, 0, t2_b, &c) != 0) return 1;
    pthread_join(a, 0);
    pthread_join(b, 0);
    printf("[mmrt] t2 rounds=%d same=%llu diffcpu=%llu rbad=%llu zbad=%llu pbad=%llu cpu=%d/%d\n",
           T2_ROUNDS, (unsigned long long)c.same, (unsigned long long)c.diffcpu,
           (unsigned long long)c.rbad, (unsigned long long)c.zbad,
           (unsigned long long)c.pbad, c.cpu_a, c.cpu_b);
    int fail = c.rbad || c.zbad || c.pbad;
    if (expect_reuse) {
        if (c.same == 0) fail = 1;          // the flag did not reuse at all
        if (c.diffcpu == 0) fail = 1;       // the reader never ran on another cpu
    } else if (c.same != 0) {
        fail = 1;                           // baseline must not reuse
    }
    return fail;
}

static int run_t3(void) {
    int bad = 0, made = 0;
    for (int base = 0; base < T3_THREADS; base += T3_BATCH) {
        pthread_t th[T3_BATCH];
        int n = T3_THREADS - base; if (n > T3_BATCH) n = T3_BATCH;
        for (int i = 0; i < n; i++) {
            void* magic = (void*)(uintptr_t)(0x1000 + base + i);
            if (pthread_create(&th[i], 0, t3_thread, magic) != 0) { n = i; break; }
            made++;
        }
        for (int i = 0; i < n; i++) {
            void* r = 0;
            pthread_join(th[i], &r);
            if (r != (void*)(uintptr_t)1) bad++;
        }
        if (made < base + n) break;
    }
    printf("[mmrt] t3 threads=%d bad=%d\n", made, bad);
    return bad || made != T3_THREADS;
}

// ---- T4: a dying thread must never write into a mapping that reused its address
// (clear_child_tid). mapper threads mmap the sizes the exiting threads used, fill
// every word with a nonzero pattern, yield, verify and unmap; a stray zero word
// is a bad. 20000 detached threads exit meanwhile (T4a). T4b: threads point
// set_tid_address into an mmap'd page, unmap it and exit raw, so the kernel's
// exit-time clear_child_tid write must not land in whatever got the address next. (satoru)
static atomic_int t4_stop, t4_done;
static atomic_ullong t4_bad, t4_maps;
static atomic_ullong t4_fb_addr, t4_fb_val, t4_fb_exp;   // first bad word (satoru)
static atomic_int t4_fb_set;
static const size_t t4_sizes[] = { 0xD000, 0x11000, 0x10000, 0x2000 };

static void* t4_mapper(void* ap) {
    uint64_t id = (uint64_t)(uintptr_t)ap, seed = 0x1234567ULL * (id + 1);
    uint64_t it = 0;
    while (!atomic_load(&t4_stop)) {
        size_t sz = t4_sizes[rng_next(&seed) % 4];
        uint64_t* p = (uint64_t*)mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        uint64_t pat = 0xC0DE000000000000ULL | (id << 32) | (++it & 0xFFFFFFFFULL) | 1;
        size_t nw = sz / 8;
        for (size_t i = 0; i < nw; i += 64) p[i] = pat;       // one word per 512 B
        p[nw - 1] = pat; p[0] = pat;
        for (int y = 0; y < 3; y++) sched_yield();
        uint64_t bad = 0;
        for (size_t i = 0; i < nw; i += 64) if (p[i] != pat) {
            bad++;
            int z = 0;
            if (atomic_compare_exchange_strong(&t4_fb_set, &z, 1)) {
                atomic_store(&t4_fb_addr, (unsigned long long)(uintptr_t)&p[i]);
                atomic_store(&t4_fb_val, p[i]); atomic_store(&t4_fb_exp, pat);
            }
        }
        if (p[nw - 1] != pat || p[0] != pat) bad++;
        if (bad) atomic_fetch_add(&t4_bad, bad);
        atomic_fetch_add(&t4_maps, 1);
        munmap(p, sz);
    }
    return 0;
}
static void* t4_exiter(void* ap) {
    (void)ap;
    atomic_fetch_add(&t4_done, 1);
    return 0;
}
static void* t4_raw(void* ap) {
    (void)ap;
    size_t sz = 0x11000;
    void* pg = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pg == MAP_FAILED) { atomic_fetch_add(&t4_done, 1); return 0; }
    syscall(SYS_set_tid_address, pg);
    munmap(pg, sz);
    atomic_fetch_add(&t4_done, 1);
    // stay alive long enough for the freed range to be proven and handed to the
    // mappers: only then does the exit-time write have something to corrupt. (satoru)
    for (int y = 0; y < 200; y++) sched_yield();
    usleep(4000);
    syscall(SYS_exit, 0);                      // the kernel clears *pg now
    return 0;
}
// creation can fail with EAGAIN while the kernel still reaps earlier exits: wait and retry. (satoru)
static int t4_create(pthread_t* t, const pthread_attr_t* at, void* (*fn)(void*)) {
    int r = 0;
    for (int k = 0; k < 2000; k++) {
        r = pthread_create(t, at, fn, 0);
        if (r == 0) return 0;
        usleep(2000);
    }
    return r;
}
static int run_t4(void) {
    pthread_t m[4];
    atomic_init(&t4_stop, 0); atomic_init(&t4_done, 0);
    atomic_init(&t4_bad, 0); atomic_init(&t4_maps, 0);
    for (int i = 0; i < 4; i++) if (pthread_create(&m[i], 0, t4_mapper, (void*)(uintptr_t)i) != 0) return 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int made = 0, fail = 0;
    for (int base = 0; base < 20000; base += 16) {
        int n0 = atomic_load(&t4_done);
        int n = 0;
        for (int i = 0; i < 16; i++) {
            pthread_t t;
            if (t4_create(&t, &at, t4_exiter) != 0) break;
            n++; made++;
        }
        while (atomic_load(&t4_done) - n0 < n) sched_yield();
        if (n < 16) { fail = 1; break; }
    }
    int rawmade = 0;
    for (int base = 0; base < 2000; base += 8) {
        int n0 = atomic_load(&t4_done);
        int n = 0;
        for (int i = 0; i < 8; i++) {
            pthread_t t;
            if (t4_create(&t, &at, t4_raw) != 0) break;
            n++; rawmade++;
        }
        while (atomic_load(&t4_done) - n0 < n) sched_yield();
        while (atomic_load(&t4_done) - n0 < n) sched_yield();
        if (n < 8) { fail = 1; break; }
    }
    usleep(100000);                               // let the last raw exits land while mappers run
    atomic_store(&t4_stop, 1);
    for (int i = 0; i < 4; i++) pthread_join(m[i], 0);
    printf("[mmrt] t4 detached=%d raw=%d maps=%llu bad=%llu\n", made, rawmade,
           (unsigned long long)atomic_load(&t4_maps), (unsigned long long)atomic_load(&t4_bad));
    if (atomic_load(&t4_fb_set))
        printf("[mmrt] t4 first bad at 0x%llx value 0x%llx expected 0x%llx\n",
               (unsigned long long)atomic_load(&t4_fb_addr), (unsigned long long)atomic_load(&t4_fb_val),
               (unsigned long long)atomic_load(&t4_fb_exp));
    return fail || atomic_load(&t4_bad) != 0;
}

// ---- bench: cost of one auto-placed mmap+munmap pair with a Firefox-sized thread group
// (64 sleeping threads), 100000 pairs. prints ns per pair. (satoru)
static atomic_int bench_stop;
static void* bench_sleeper(void* ap) { (void)ap; while (!atomic_load(&bench_stop)) usleep(20000); return 0; }
static int run_bench(void) {
    pthread_t th[64];
    atomic_init(&bench_stop, 0);
    int n = 0;
    for (; n < 64; n++) if (pthread_create(&th[n], 0, bench_sleeper, 0) != 0) break;
    usleep(100000);
    struct timespec a, b;
    for (int i = 0; i < 2000; i++) { void* p = mmap(0, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (p != MAP_FAILED) munmap(p, 0x2000); }
    clock_gettime(CLOCK_MONOTONIC, &a);
    int fail = 0;
    for (int i = 0; i < 100000; i++) {
        void* p = mmap(0, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { fail++; continue; }
        munmap(p, 0x2000);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / 100000.0;
    atomic_store(&bench_stop, 1);
    for (int i = 0; i < n; i++) pthread_join(th[i], 0);
    printf("[mmrt] bench threads=%d pairs=100000 fail=%d ns_per_pair=%d\n", n, fail, (int)ns);
    return fail != 0;
}

// ---- wl_shm pool pin tests (raw wayland client) (satoru)
#define PW 320
#define PH 240
#define PSTRIDE (PW * 4)
#define PPOOL (PSTRIDE * PH)
enum { W_DISPLAY = 1, W_REGISTRY = 2, W_SYNC = 3, W_COMP = 4, W_XDGBASE = 5, W_SHM = 6,
       W_SURF = 7, W_XDGSURF = 8, W_TOP = 9, W_POOL1 = 20, W_POOL2 = 21, W_BUF1 = 22, W_BUF2 = 23 };
static int w_fd;
static uint32_t w_name_comp, w_name_xdg, w_name_shm;

static void w_send(uint32_t obj, uint16_t op, const uint32_t* args, int n) {
    uint32_t m[64];
    int size = 8 + n * 4;
    m[0] = obj; m[1] = ((uint32_t)size << 16) | op;
    for (int i = 0; i < n; i++) m[2 + i] = args[i];
    if (write(w_fd, m, size) != size) { /* a dead server shows up as a failed test */ }
}
static void w_bind(uint32_t name, const char* iface, uint32_t ver, uint32_t id) {
    uint32_t m[64];
    int p = 0;
    m[p++] = W_REGISTRY; m[p++] = 0; m[p++] = name;
    int il = (int)strlen(iface) + 1;
    m[p++] = (uint32_t)il;
    int words = (il + 3) / 4;
    memset(&m[p], 0, (size_t)words * 4);
    memcpy(&m[p], iface, (size_t)il - 1);
    p += words;
    m[p++] = ver; m[p++] = id;
    m[1] = ((uint32_t)(p * 4) << 16) | 0;
    if (write(w_fd, m, (size_t)p * 4) < 0) { }
}
static void w_pool(int memfd, uint32_t id, uint32_t bytes) {
    uint32_t body[4] = { W_SHM, ((uint32_t)16 << 16) | 0, id, bytes };
    struct iovec iov = { body, sizeof(body) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &memfd, sizeof(int));
    sendmsg(w_fd, &msg, 0);
}
// answer configure / ping for ms milliseconds of wall clock (usleep on this
// kernel can take far longer than asked, so count time, not iterations).
static long w_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void w_pump(int ms) {
    uint8_t buf[8192];
    long end = w_ms() + ms;
    while (w_ms() < end) {
        int n = (int)recv(w_fd, buf, sizeof(buf), MSG_DONTWAIT);   // never block: the clock decides (satoru)
        if (n <= 0) { usleep(2000); continue; }
        int off = 0;
        while (n - off >= 8) {
            uint32_t obj = *(uint32_t*)(buf + off), hw = *(uint32_t*)(buf + off + 4);
            int sz = (int)(hw >> 16), op = (int)(hw & 0xffff);
            if (sz < 8 || off + sz > n) break;
            if (obj == W_XDGSURF && op == 0) {
                uint32_t serial = *(uint32_t*)(buf + off + 8);
                w_send(W_XDGSURF, 4, &serial, 1);
                w_send(W_SURF, 6, 0, 0);
            } else if (obj == W_XDGBASE && op == 0) {
                uint32_t serial = *(uint32_t*)(buf + off + 8);
                w_send(W_XDGBASE, 3, &serial, 1);
            }
            off += sz;
        }
    }
}
static int w_connect(void) {
    w_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (w_fd < 0) return 0;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/system/run/user/1000/wayland-0");
    if (connect(w_fd, (struct sockaddr*)&a, sizeof(a)) < 0) return 0;
    uint32_t arg = W_REGISTRY; w_send(W_DISPLAY, 1, &arg, 1);
    arg = W_SYNC;              w_send(W_DISPLAY, 0, &arg, 1);
    uint8_t buf[8192];
    int have = 0, synced = 0;
    for (int tries = 0; !synced && tries < 3000; tries++) {
        int n = (int)recv(w_fd, buf + have, sizeof(buf) - (size_t)have, MSG_DONTWAIT);
        if (n <= 0) { usleep(2000); continue; }
        have += n;
        int off = 0;
        while (have - off >= 8) {
            uint32_t obj = *(uint32_t*)(buf + off), hw = *(uint32_t*)(buf + off + 4);
            int sz = (int)(hw >> 16), op = (int)(hw & 0xffff);
            if (sz < 8 || off + sz > have) break;
            if (obj == W_REGISTRY && op == 0) {
                uint32_t name = *(uint32_t*)(buf + off + 8), il = *(uint32_t*)(buf + off + 12);
                const char* iface = (const char*)(buf + off + 16);
                if (il && il < 64) {
                    if (!strcmp(iface, "wl_compositor")) w_name_comp = name;
                    else if (!strcmp(iface, "xdg_wm_base")) w_name_xdg = name;
                    else if (!strcmp(iface, "wl_shm")) w_name_shm = name;
                }
            } else if (obj == W_SYNC && op == 0) synced = 1;
            off += sz;
        }
        if (off) { memmove(buf, buf + off, (size_t)(have - off)); have -= off; }
    }
    if (!synced) return 0;
    w_bind(w_name_comp, "wl_compositor", 4, W_COMP);
    w_bind(w_name_xdg, "xdg_wm_base", 1, W_XDGBASE);
    w_bind(w_name_shm, "wl_shm", 1, W_SHM);
    uint32_t a1 = W_SURF; w_send(W_COMP, 0, &a1, 1);
    uint32_t xa[2] = { W_XDGSURF, W_SURF }; w_send(W_XDGBASE, 2, xa, 2);
    uint32_t ta = W_TOP; w_send(W_XDGSURF, 1, &ta, 1);
    w_send(W_SURF, 6, 0, 0);
    return 1;
}
static void w_show(uint32_t buf) {
    uint32_t at[3] = { buf, 0, 0 }; w_send(W_SURF, 1, at, 3);
    uint32_t dm[4] = { 0, 0, PW, PH }; w_send(W_SURF, 2, dm, 4);
    w_send(W_SURF, 6, 0, 0);
}
static int w_memfd(uint32_t** px_out) {
    int fd = (int)syscall(SYS_memfd_create, "mmrt_pool", 0);
    if (fd < 0 || ftruncate(fd, PPOOL) < 0) return -1;
    uint32_t* px = (uint32_t*)mmap(0, PPOOL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) return -1;
    for (int i = 0; i < PW * PH; i++) px[i] = 0xFF1E90FF;   // dodger blue
    *px_out = px;
    return fd;
}
// two pools on one memfd. the owner closes and unmaps, so the compositor's
// passes are the only references; destroying pool 1 must not free the backing
// pool 2 still shows. the driver screenshots during the "[mmrt] pool2 shown" window.
static int run_pool2(void) {
    if (!w_connect()) { printf("[mmrt] pool2 connect failed\n"); return 1; }
    uint32_t* px;
    int fd = w_memfd(&px);
    if (fd < 0) { printf("[mmrt] pool2 memfd failed\n"); return 1; }
    w_pool(fd, W_POOL1, PPOOL);
    w_pool(fd, W_POOL2, PPOOL);
    uint32_t b1[6] = { W_BUF1, 0, PW, PH, PSTRIDE, 1 }; w_send(W_POOL1, 0, b1, 6);
    uint32_t b2[6] = { W_BUF2, 0, PW, PH, PSTRIDE, 1 }; w_send(W_POOL2, 0, b2, 6);
    munmap(px, PPOOL);
    close(fd);
    w_show(W_BUF1);
    w_pump(2000);
    w_show(W_BUF2);                                   // displayed from pool 2 from here on
    w_pump(500);
    w_send(W_BUF1, 0, 0, 0);                          // wl_buffer.destroy
    w_send(W_POOL1, 1, 0, 0);                         // wl_shm_pool.destroy
    printf("[mmrt] pool2 destroyed pool 1; pool 2 shown\n");
    fflush(stdout);
    w_pump(8000);                                     // screenshots land here
    w_send(W_BUF2, 0, 0, 0);
    w_send(W_POOL2, 1, 0, 0);
    w_pump(7000);                                     // room for two census lines
    printf("[mmrt] pool2 done\n");
    return 0;
}
// the memfd goes to the compositor and to a second receiver. owner closes, the
// compositor drops its pool; the receiver must still read the pattern, also
// after the machine reallocated and dirtied 64 MiB.
static int run_share(void) {
    if (!w_connect()) { printf("[mmrt] share connect failed\n"); return 1; }
    uint32_t* px;
    int fd = w_memfd(&px);
    if (fd < 0) { printf("[mmrt] share memfd failed\n"); return 1; }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { printf("[mmrt] share socketpair failed\n"); return 1; }
    {
        char one = 'x';
        struct iovec iov = { &one, 1 };
        char cbuf[CMSG_SPACE(sizeof(int))];
        memset(cbuf, 0, sizeof(cbuf));
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
        if (sendmsg(sv[0], &msg, 0) < 0) { printf("[mmrt] share sendmsg failed\n"); return 1; }
    }
    int fd2 = -1;
    {
        char one;
        struct iovec iov = { &one, 1 };
        char cbuf[CMSG_SPACE(sizeof(int))];
        memset(cbuf, 0, sizeof(cbuf));
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
        if (recvmsg(sv[1], &msg, 0) < 0) { printf("[mmrt] share recvmsg failed\n"); return 1; }
        struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        if (cm && cm->cmsg_type == SCM_RIGHTS) memcpy(&fd2, CMSG_DATA(cm), sizeof(int));
    }
    if (fd2 < 0) { printf("[mmrt] share no fd received\n"); return 1; }
    w_pool(fd, W_POOL1, PPOOL);
    uint32_t b1[6] = { W_BUF1, 0, PW, PH, PSTRIDE, 1 }; w_send(W_POOL1, 0, b1, 6);
    w_show(W_BUF1);
    w_pump(1500);
    munmap(px, PPOOL);
    close(fd);                                        // the owner is gone
    w_send(W_BUF1, 0, 0, 0);
    w_send(W_POOL1, 1, 0, 0);                         // the compositor drops its pass
    w_pump(1500);
    uint32_t* p2 = (uint32_t*)mmap(0, PPOOL, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    if (p2 == MAP_FAILED) { printf("[mmrt] share receiver map failed\n"); return 1; }
    int bad = 0;
    for (int i = 0; i < PW * PH; i++) if (p2[i] != 0xFF1E90FF) bad++;
    // churn: take 64 MiB, dirty it, give it back, then read the pattern again.
    size_t cn = 64u << 20;
    uint8_t* ch = (uint8_t*)mmap(0, cn, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ch != MAP_FAILED) { memset(ch, 0xA5, cn); munmap(ch, cn); }
    for (int i = 0; i < PW * PH; i++) if (p2[i] != 0xFF1E90FF) bad++;
    printf("[mmrt] share pattern bad=%d\n", bad);
    munmap(p2, PPOOL);
    close(fd2);                                       // last reference: the backing may go now
    close(sv[0]); close(sv[1]);
    w_pump(7000);                                     // room for two census lines
    printf("[mmrt] share done\n");
    return bad != 0;
}

// ---- gc: spidermonkey's gc chunk allocator (js/src/gc/Memory.cpp, esr140) ----
// FindAddressLimit probes 1<<bit hints, then MapAlignedPagesRandom maps 1 mb
// chunks at random 1 mb aligned hints below 2^46 (no MAP_FIXED, any address
// accepted), aligns an unaligned result by trimming, falls back to the slow
// map-2mb-and-trim path, writes the chunk header and touches every page.
// a fault the kernel cannot resolve hangs this test at the first store, the
// laptop firefox symptom. (satoru)
static uint64_t gc_seed = 0x9E3779B97F4A7C15ULL;
static uint64_t gc_rand64(void) { return (rng_next(&gc_seed) << 31) ^ rng_next(&gc_seed); }
static void* gc_map_at(uint64_t hint, size_t len) {
    void* p = mmap((void*)hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
static void* gc_slow(size_t len, size_t align) {
    size_t res = len + align - 4096;
    uint8_t* r = (uint8_t*)gc_map_at(0, res);
    if (!r) return 0;
    uint64_t a = ((uint64_t)r + align - 1) & ~(uint64_t)(align - 1);
    if (a > (uint64_t)r) munmap(r, a - (uint64_t)r);
    uint64_t e = (uint64_t)r + res, ae = a + len;
    if (e > ae) munmap((void*)ae, e - ae);
    return (void*)a;
}
static int run_gc(void) {
    const size_t CH = 1u << 20;
    gc_seed ^= (uint64_t)time(0);
    uint64_t highest = 0; int bits = 0;
    for (int hb = 47; hb >= 30; hb--) {       // FindAddressLimit, coarse (satoru)
        for (int t = 0; t < 4; t++) {
            uint64_t h = (1ULL << hb) + ((gc_rand64() % (1ULL << hb)) & ~0xFFFFULL);
            void* p = gc_map_at(h, 0x10000);
            if (p) { if ((uint64_t)p > highest) highest = (uint64_t)p; munmap(p, 0x10000); }
        }
        if (highest >= (1ULL << hb)) { bits = hb + 1; break; }
    }
    printf("[mmrt] gc addrbits=%d highest=0x%llx\n", bits, (unsigned long long)highest);
    uint64_t maxnum = ((1ULL << 46) - 1 - 4096 - (CH - 1)) / CH;
    enum { NK = 512 };
    static uint8_t* keep[NK];
    int nk = 0, exact = 0, moved = 0, slow = 0, fail = 0, bad = 0, loops = 0;
    for (int i = 1; nk < NK && i <= 8192; i++) {
        uint8_t* p = 0;
        if (i & 0xF) {
            uint64_t hint = CH * (1 + gc_rand64() % maxnum);
            p = (uint8_t*)gc_map_at(hint, CH);
            if (!p) { fail++; continue; }
            if ((uint64_t)p == hint) exact++; else moved++;
            if ((uint64_t)p & (CH - 1)) {                  // TryToAlignChunk: trim, else retry (satoru)
                munmap(p, CH);
                p = (uint8_t*)gc_slow(CH, CH);
                slow++;
                if (!p) { fail++; continue; }
            }
        } else {
            p = (uint8_t*)gc_slow(CH, CH);
            slow++;
            if (!p) { fail++; continue; }
        }
        loops++;
        ((uint64_t*)p)[1] = 0x1234;                        // the faulting store's slot (satoru)
        for (size_t o = 0; o < CH; o += 4096) p[o + 7] = (uint8_t)(o >> 12);
        for (size_t o = 0; o < CH; o += 4096) if (p[o + 7] != (uint8_t)(o >> 12)) bad++;
        if (((uint64_t*)p)[1] != 0x1234) bad++;
        if (i % 3 == 0) {                                  // decommit + reuse, like the gc (satoru)
            madvise(p + CH / 2, CH / 2, MADV_DONTNEED);
            for (size_t o = CH / 2; o < CH; o += 4096) if (p[o + 7] != 0) bad++;
            p[CH - 1] = 1;
        }
        if (i % 4 == 0) munmap(p, CH); else keep[nk++] = p;
    }
    for (int k = 0; k < nk; k++) if (((uint64_t*)keep[k])[1] != 0x1234) bad++;
    for (int k = 0; k < nk; k++) munmap(keep[k], CH);
    printf("[mmrt] gc chunks=%d kept=%d hint_exact=%d hint_moved=%d slow=%d fail=%d bad=%d\n",
           loops, nk, exact, moved, slow, fail, bad);
    printf("[mmrt] gc %s\n", (bad || fail > loops) ? "FAIL" : "PASS");
    return bad != 0 || fail > loops;
}

int main(int argc, char** argv) {
    const char* which = "all";
    int expect_reuse = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "t1") || !strcmp(argv[i], "t2") ||
            !strcmp(argv[i], "t3") || !strcmp(argv[i], "t4") || !strcmp(argv[i], "all") ||
            !strcmp(argv[i], "pool2") || !strcmp(argv[i], "share") || !strcmp(argv[i], "bench") ||
            !strcmp(argv[i], "gc")) which = argv[i];
        else if (!strncmp(argv[i], "expect=", 7)) expect_reuse = atoi(argv[i] + 7);
    }
    int fail = 0;
    if (!strcmp(which, "t1") || !strcmp(which, "all")) fail |= run_t1();
    if (!strcmp(which, "t2") || !strcmp(which, "all")) fail |= run_t2(expect_reuse);
    if (!strcmp(which, "t3") || !strcmp(which, "all")) fail |= run_t3();
    if (!strcmp(which, "t4")) fail |= run_t4();   // its own run: 22000 more threads (satoru)
    if (!strcmp(which, "bench")) fail |= run_bench();
    if (!strcmp(which, "pool2")) fail |= run_pool2();
    if (!strcmp(which, "share")) fail |= run_share();
    if (!strcmp(which, "gc")) fail |= run_gc();   // gc chunk allocator mimic (satoru)
    printf("[mmrt] %s\n", fail ? "FAIL" : "PASS");
    fflush(stdout);
    return fail ? 1 : 0;
}
// end (satoru)
