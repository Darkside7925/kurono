// churn - a long fork/exec/mmap/thread workload for the memory leak census.
//
// A long Claude Code session lost 3.5 GB of free memory in about 15 minutes on
// the laptop. Claude Code spawns a shell for every tool call, runs threads and
// maps and unmaps memory all the time. This does the same things in a loop, so
// a leak shows up as free memory that does not come back between rounds:
//
//   per round, in the parent:  fork + exec of a child copy of this program,
//                              fork + exec of busybox ash -c "...",
//                              fork + exec of the dynamic test program,
//                              4 threads that map, touch and unmap memory,
//                              an 8 MB anonymous map touched and unmapped,
//                              a memfd sized, mapped, unmapped and closed,
//                              a pipe round trip.
//                              then rewrites a 32 MB buffer it keeps for the
//                              whole run (a big-heap parent that forks, like
//                              node or bun spawning tool processes),
//   the exec'd child:          maps and touches 4 MB, mallocs 2 MB, runs 3
//                              threads, writes through a pipe, exits.
//
// Every 10 rounds it prints the free memory sysinfo reports.
// usage: churn [rounds]  (default 200; 0 = run forever)
//        churn regionsN      N separate live mappings (region table capacity test)

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <signal.h>

extern char** environ;

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logu(unsigned long v) {
    char t[24]; int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    char b[24]; int n = 0;
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}

static void touch(void* p, size_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    for (size_t o = 0; o < n; o += 4096) b[o] = (uint8_t)(o >> 12);
}

static void* thr(void* arg) {
    size_t n = (size_t)(uintptr_t)arg;
    void* p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) { touch(p, n); munmap(p, n); }
    char* m = malloc(64 * 1024);
    if (m) { memset(m, 1, 64 * 1024); free(m); }
    return 0;
}

static void threads(int n, size_t each) {
    pthread_t t[8];
    int made = 0;
    for (int i = 0; i < n && i < 8; i++)
        if (pthread_create(&t[made], 0, thr, (void*)(uintptr_t)each) == 0) made++;
    for (int i = 0; i < made; i++) pthread_join(t[i], 0);
}

static void pipe_trip(size_t bytes) {
    int fd[2];
    if (pipe(fd) != 0) return;
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    size_t done = 0;
    while (done < bytes) {
        ssize_t w = write(fd[1], buf, sizeof buf);
        if (w <= 0) break;
        ssize_t r = read(fd[0], buf, sizeof buf);
        if (r <= 0) break;
        done += (size_t)w;
    }
    close(fd[0]); close(fd[1]);
}

static int child_main(void) {
    size_t n = 4u << 20;
    void* p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) touch(p, n);
    char* m = malloc(2u << 20);
    if (m) memset(m, 2, 2u << 20);
    threads(3, 1u << 20);
    pipe_trip(64 * 1024);
    return 0;   // exit without freeing: teardown has to return all of it (satoru)
}

// a child with 3 threads blocked in read, killed with SIGKILL and reaped: the
// kill path never runs the threads' own exit, which is where dead thread records
// used to stay behind. (satoru)
static int g_killfd[2];
static void* block_thr(void* a) { char c; (void)a; (void)!read(g_killfd[0], &c, 1); return 0; }
static int kill_trip(void) {
    if (pipe(g_killfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -2;
    if (pid == 0) {
        pthread_t t[3];
        for (int i = 0; i < 3; i++) pthread_create(&t[i], 0, block_thr, 0);
        char c; (void)!read(g_killfd[0], &c, 1);
        _exit(0);
    }
    usleep(20000);
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    close(g_killfd[0]); close(g_killfd[1]);
    return WIFSIGNALED(st) ? 0 : 1;
}

static int run(const char* path, char* const* av) {
    if (access(path, X_OK) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -2;
    if (pid == 0) {
        execve(path, av, environ);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -3;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static void memfd_trip(void) {
    int fd = (int)syscall(SYS_memfd_create, "churn", 0);
    if (fd < 0) return;
    if (ftruncate(fd, 1u << 20) == 0) {
        void* p = mmap(0, 1u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) { touch(p, 1u << 20); munmap(p, 1u << 20); }
    }
    close(fd);
}

// regionsN: N one-page anonymous maps, each kept apart by unmapping the page
// after it, so every one is its own region. write + verify, fork a child that
// verifies and rewrites them (its own copy), verify the parent still sees its
// values, unmap all. the old 4096-slot table failed this past ~4000. (satoru)
static int regions_main(unsigned long n) {
    uint64_t** pg = malloc(n * sizeof(uint64_t*));
    if (!pg) { logs("[churn] regions: no table\n"); return 2; }
    unsigned long made = 0;
    for (unsigned long i = 0; i < n; i++) {
        uint8_t* p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) break;
        munmap(p + 4096, 4096);
        pg[i] = (uint64_t*)p;
        pg[i][0] = 0xC0DE0000ull + i; pg[i][511] = ~(uint64_t)i;
        made++;
    }
    logs("[churn] regions mapped="); logu(made); logs(" of "); logu(n); logs("\n");
    unsigned long bad = 0;
    for (unsigned long i = 0; i < made; i++)
        if (pg[i][0] != 0xC0DE0000ull + i || pg[i][511] != ~(uint64_t)i) bad++;
    pid_t pid = fork();
    if (pid == 0) {
        unsigned long cb = 0;
        for (unsigned long i = 0; i < made; i++) {
            if (pg[i][0] != 0xC0DE0000ull + i || pg[i][511] != ~(uint64_t)i) cb++;
            pg[i][0] = 0;
        }
        _exit(cb ? 3 : 0);
    }
    int st = 0, crc = -1;
    if (pid > 0 && waitpid(pid, &st, 0) == pid) crc = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    for (unsigned long i = 0; i < made; i++)
        if (pg[i][0] != 0xC0DE0000ull + i || pg[i][511] != ~(uint64_t)i) bad++;
    for (unsigned long i = 0; i < made; i++) munmap(pg[i], 4096);
    int ok = made == n && bad == 0 && crc == 0;
    logs(ok ? "[churn] regions PASS" : "[churn] regions FAIL");
    logs(" mapped="); logu(made); logs(" bad="); logu(bad);
    logs(" child_rc="); logu((unsigned long)(crc < 0 ? 999 : crc)); logs("\n");
    return ok ? 0 : 1;
}

// fixedlow: a MAP_FIXED page over plain low ram must be refused (it would hide
// that frame from the kernel in this address space); one above the identity map
// must still work. (satoru)
static int fixedlow_main(void) {
    void* lo = mmap((void*)0x30000000ul, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    int lo_refused = (lo == MAP_FAILED);
    if (!lo_refused) ((volatile char*)lo)[0] = 1;
    void* hi = mmap((void*)0x9600000000ul, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    int hi_ok = (hi == (void*)0x9600000000ul);
    if (hi_ok) { ((volatile char*)hi)[0] = 1; hi_ok = ((volatile char*)hi)[0] == 1; }
    logs(lo_refused && hi_ok ? "[churn] fixedlow PASS" : "[churn] fixedlow FAIL");
    logs(" low_refused="); logu((unsigned long)lo_refused);
    logs(" high_ok="); logu((unsigned long)hi_ok); logs("\n");
    return lo_refused && hi_ok ? 0 : 1;
}

// exec: item 11 reproducer. execve of a path that does not exist must fail with
// ENOENT (the child reports errno through its exit code), and /system/bin/busybox
// must be a real shell: `busybox sh -c` has to print through a pipe. (satoru)
#include <errno.h>
static int exec_main(void) {
    int ok = 1;
    pid_t pid = fork();
    if (pid == 0) {
        char* av[] = { "nosuch", 0 };
        execve("/system/bin/no-such-binary-churn", av, environ);
        _exit(errno == ENOENT ? 42 : 43);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int noent = WIFEXITED(st) && WEXITSTATUS(st) == 42;
    if (!noent) ok = 0;
    int fd[2]; char buf[64]; int got = 0;
    if (pipe(fd) == 0) {
        pid = fork();
        if (pid == 0) {
            dup2(fd[1], 1); close(fd[0]); close(fd[1]);
            char* av[] = { "busybox", "sh", "-c", "echo shell-ok", 0 };
            execve("/system/bin/busybox", av, environ);
            _exit(127);
        }
        close(fd[1]);
        int n;
        while (got < (int)sizeof(buf) - 1 && (n = (int)read(fd[0], buf + got, sizeof(buf) - 1 - got)) > 0) got += n;
        buf[got] = 0;
        close(fd[0]);
        waitpid(pid, &st, 0);
    }
    int shell = got >= 8 && strncmp(buf, "shell-ok", 8) == 0;
    if (!shell) ok = 0;
    logs(ok ? "[churn] exec PASS" : "[churn] exec FAIL");
    logs(" missing_path_enoent="); logu((unsigned long)noent);
    logs(" busybox_shell="); logu((unsigned long)shell); logs("\n");
    return ok ? 0 : 1;
}

// cow torture (churn cowF_T_M): T worker threads hammer shared words on 8
// pages with atomic add, a CAS-loop add and a pthread-mutex-protected plain
// add, until the forker thread has done F forks (M: 0 = fork, child _exit at
// once; 1 = fork, the child writes a shared page then _exit; 2 = posix_spawn
// of /bin/true). every fork turns the parent's pages copy-on-write, so the
// workers' next stores race the cow break of the page they share. each worker
// counts its own iterations; a lost store shows as a total below the sum.
// one "[cow] ..." line per run. (satoru)
#include <spawn.h>
#define COW_PAGES 8
static volatile uint32_t* g_cw;           // COW_PAGES pages (satoru)
static pthread_mutex_t g_cmx = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_cplain;
static volatile int g_cstop;
static unsigned long g_cdone[16];
static void* cow_worker(void* arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    unsigned long i = 0;
    while (!__atomic_load_n(&g_cstop, __ATOMIC_RELAXED)) {
        unsigned pg = (unsigned)((i + id) % COW_PAGES);
        volatile uint32_t* w = g_cw + pg * 1024;
        __atomic_fetch_add(&w[0], 1, __ATOMIC_SEQ_CST);                 // lock xadd
        uint32_t o = __atomic_load_n(&w[16], __ATOMIC_RELAXED);         // cas loop
        while (!__atomic_compare_exchange_n(&w[16], &o, o + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) { }
        pthread_mutex_lock(&g_cmx); g_cplain++; pthread_mutex_unlock(&g_cmx);
        w[32 + id] = (uint32_t)i;                                        // plain own-slot store
        i++;
    }
    g_cdone[id] = i;
    return 0;
}
static int cow_main(const char* spec) {
    unsigned long f = 200, t = 4, mode = 0;
    char* e;
    f = strtoul(spec, &e, 10);
    if (*e == '_') t = strtoul(e + 1, &e, 10);
    if (*e == '_') mode = strtoul(e + 1, &e, 10);
    if (t < 1) t = 1;
    if (t > 16) t = 16;
    g_cw = (volatile uint32_t*)mmap(0, COW_PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((void*)g_cw == MAP_FAILED) { logs("[cow] mmap failed\n"); return 1; }
    memset((void*)g_cw, 0, COW_PAGES * 4096);
    g_cplain = 0; g_cstop = 0;
    pthread_t th[16];
    for (unsigned long i = 0; i < t; i++) pthread_create(&th[i], 0, cow_worker, (void*)(uintptr_t)i);
    unsigned long forks = 0, fails = 0;
    while (forks < f && fails < 1000) {
        pid_t pid;
        if (mode == 2) {
            char* av[] = { (char*)"true", 0 };
            if (posix_spawn(&pid, "/bin/true", 0, 0, av, environ) != 0) pid = -1;
        } else {
            pid = fork();
            if (pid == 0) {
                if (mode == 1) g_cw[1] = 1;   // the child writes too (satoru)
                _exit(0);
            }
        }
        if (pid > 0) { int st; waitpid(pid, &st, 0); forks++; } else fails++;
    }
    __atomic_store_n(&g_cstop, 1, __ATOMIC_RELAXED);
    for (unsigned long i = 0; i < t; i++) pthread_join(th[i], 0);
    unsigned long exp = 0, got_add = 0, got_cas = 0;
    for (unsigned long i = 0; i < t; i++) exp += g_cdone[i];
    for (unsigned p2 = 0; p2 < COW_PAGES; p2++) { got_add += g_cw[p2 * 1024]; got_cas += g_cw[p2 * 1024 + 16]; }
    logs("[cow] mode="); logu(mode); logs(" threads="); logu(t);
    logs(" forks="); logu(forks); logs(" forkfail="); logu(fails);
    logs(" xadd="); logu(got_add); logs("/"); logu(exp);
    logs(" cas="); logu(got_cas); logs("/"); logu(exp);
    logs(" mutex="); logu((unsigned long)g_cplain); logs("/"); logu(exp);
    int bad = got_add != exp || got_cas != exp || g_cplain != exp;
    logs(bad ? " LOST\n" : " OK\n");
    munmap((void*)g_cw, COW_PAGES * 4096);
    return bad ? 2 : 0;
}

// exit race (churn exitN): N times, fork a child that starts 4 threads which
// keep allocating and writing heap memory, then calls exit(0) from main while
// they run (what claude does when it quits). linux kills the siblings before
// the address space goes; a kernel that tears the leader down first lets them
// fault ([usrflt] lines) and leaks the address space ([asguard] leak). the
// parent only counts exit statuses: the kernel log is the verdict. (satoru)
static void* exitrace_thr(void* a) {
    (void)a;
    for (;;) {
        char* m = malloc(4096 + (rand() & 0xFFFF));
        if (m) { memset(m, 7, 4096); free(m); }
    }
    return 0;
}
static int exitrace_main(unsigned long n) {
    unsigned long ok = 0, bad = 0;
    for (unsigned long i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            pthread_t t;
            for (int k = 0; k < 4; k++) pthread_create(&t, 0, exitrace_thr, 0);
            usleep(20000);
            exit(0);
        }
        int st = 0;
        if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++; else bad++;
    }
    logs("[exitrace] runs="); logu(n); logs(" clean="); logu(ok); logs(" other="); logu(bad); logs("\n");
    return 0;
}

// suspend handshake (churn sigN): what JSC's garbage collector does to stop a
// mutator. the target thread sits in pthread_cond_wait (a FUTEX_WAIT that never
// ends by itself); N times the "collector" thread pthread_kill()s it and
// sem_timedwait()s 2 s for the handler's sem_post. linux runs the handler in
// the middle of the futex wait, so every round answers at once; a kernel that
// leaves the target parked times out every round. (satoru)
#include <semaphore.h>
#include <time.h>
static sem_t g_sus_sem;
static pthread_mutex_t g_sus_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sus_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_sus_done;
static void sus_handler(int s) { (void)s; sem_post(&g_sus_sem); }
static void* sus_target(void* a) {
    (void)a;
    pthread_mutex_lock(&g_sus_mx);
    while (!g_sus_done) pthread_cond_wait(&g_sus_cv, &g_sus_mx);
    pthread_mutex_unlock(&g_sus_mx);
    return 0;
}
static int sigsus_main(unsigned long n) {
    sem_init(&g_sus_sem, 0, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = sus_handler; sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, 0);
    pthread_t t; pthread_create(&t, 0, sus_target, 0);
    usleep(100000);
    unsigned long ok = 0, tmo = 0;
    for (unsigned long i = 0; i < n; i++) {
        pthread_kill(t, SIGUSR2);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
        if (sem_timedwait(&g_sus_sem, &ts) == 0) ok++; else tmo++;
        if (tmo >= 3) break;
    }
    pthread_mutex_lock(&g_sus_mx); g_sus_done = 1; pthread_cond_broadcast(&g_sus_cv); pthread_mutex_unlock(&g_sus_mx);
    pthread_join(t, 0);
    logs("[sigsus] rounds="); logu(ok + tmo); logs(" answered="); logu(ok); logs(" timedout="); logu(tmo);
    logs(tmo ? " FROZEN-HANDSHAKE\n" : " OK\n");
    return tmo ? 2 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strncmp(argv[1], "sig", 3) == 0) return sigsus_main(argv[1][3] ? strtoul(argv[1] + 3, 0, 10) : 200);
    if (argc > 1 && strncmp(argv[1], "exit", 4) == 0) return exitrace_main(argv[1][4] ? strtoul(argv[1] + 4, 0, 10) : 50);
    if (argc > 1 && strncmp(argv[1], "cow", 3) == 0) {
        // several runs, one per mode, so one boot covers fork, fork+child write, spawn (satoru)
        int r = 0;
        if (argv[1][3]) r |= cow_main(argv[1] + 3);
        else {
            r |= cow_main("300_4_0");
            r |= cow_main("300_4_1");
            r |= cow_main("100_4_2");
            r |= cow_main("300_8_0");
        }
        return r;
    }
    if (argc > 1 && strcmp(argv[1], "child") == 0) return child_main();
    if (argc > 1 && strcmp(argv[1], "exec") == 0) return exec_main();
    if (argc > 1 && strcmp(argv[1], "fixedlow") == 0) return fixedlow_main();
    if (argc > 1 && strncmp(argv[1], "regions", 7) == 0)
        return regions_main(strtoul(argv[1] + 7, 0, 10));
    // one kld broker op as an ordinary process: a dead container record must
    // not be inherited through a reused PML4 frame (KLD_EPERM expected).
    // (satoru)
    if (argc > 1 && strcmp(argv[1], "kldop") == 0) {
        errno = 0;
        long r = syscall(0x4B4C4442UL, 0x0085UL, 0UL, 0UL, 0UL, 0UL, 0UL);
        int e = errno;
        logs("[churn] kldop detach="); logu((unsigned long)r);
        logs(" errno="); logu((unsigned long)e); logs("\n");
        return (r < 0 && e == 1) ? 0 : 1;
    }
    // 10000 mmap/munmap pairs as one ordinary process (D2.12 item 5): a
    // broker record that outlived its address space would make a later
    // process's munmap fail here. (satoru)
    if (argc > 1 && strcmp(argv[1], "pairs") == 0) {
        unsigned long fails = 0;
        for (int i = 0; i < 10000; i++) {
            void* p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) { fails++; continue; }
            *(volatile unsigned char*)p = (unsigned char)i;
            if (munmap(p, 4096) != 0) fails++;
        }
        logs("[churn] pairs=10000 fails="); logu(fails); logs("\n");
        return fails ? 1 : 0;
    }
    // "nkN": N rounds without the kill step (separates the deferred reap of a
    // killed group from everything else). (satoru)
    int no_kill = argc > 1 && argv[1][0] == 'n' && argv[1][1] == 'k';
    unsigned long rounds = argc > 1 ? strtoul(argv[1] + (no_kill ? 2 : 0), 0, 10) : 200;
    char* av_child[] = { "churn", "child", 0 };
    char* av_sh[]    = { "busybox", "ash", "-c", "echo churn | cat > /dev/null; ls / > /dev/null", 0 };
    char* av_dyn[]   = { "dyntest", 0 };
    unsigned long fails = 0, kfails = 0;
    struct sysinfo si;
    size_t big_n = 32u << 20;
    uint8_t* big = malloc(big_n);
    if (big) memset(big, 0x5A, big_n);
    for (unsigned long r = 1; rounds == 0 || r <= rounds; r++) {
        if (run("/system/bin/churn", av_child) != 0) fails++;
        int bs = run("/system/bin/busybox", av_sh);
        if (bs != 0 && bs != -1) fails++;
        run("/system/bin/dyntest", av_dyn);
        threads(4, 2u << 20);
        void* p = mmap(0, 8u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { touch(p, 8u << 20); munmap(p, 8u << 20); }
        memfd_trip();
        pipe_trip(32 * 1024);
        if (!no_kill && kill_trip() != 0) kfails++;
        // every page was shared copy-on-write by the forks above; writing it now
        // takes a cow fault per page once per round. (satoru)
        if (big) memset(big, (int)(r & 0xFF), big_n);
        if (r % 10 == 0 || r == 1) {
            if (sysinfo(&si) == 0) {
                logs("[churn] round="); logu(r);
                logs(" freeram_mb="); logu((unsigned long)(si.freeram * (si.mem_unit ? si.mem_unit : 1) >> 20));
                logs(" fails="); logu(fails);
                logs(" kill_fails="); logu(kfails);
                logs("\n");
            }
        }
    }
    logs("[churn] DONE rounds="); logu(rounds);
    logs(" fails="); logu(fails); logs("\n");
    return fails ? 1 : 0;
}
// end (satoru)
