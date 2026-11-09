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

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return child_main();
    if (argc > 1 && strcmp(argv[1], "exec") == 0) return exec_main();
    if (argc > 1 && strcmp(argv[1], "fixedlow") == 0) return fixedlow_main();
    if (argc > 1 && strncmp(argv[1], "regions", 7) == 0)
        return regions_main(strtoul(argv[1] + 7, 0, 10));
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
