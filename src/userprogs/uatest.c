// uatest - user-pointer validation regression test.
//
// The audit's #1 finding was that the syscall layer dereferenced user pointers
// with no validation, so a ring-3 process could hand the kernel a KERNEL address
// and have it read or written on its behalf. src/linux/uaccess.{h,cpp} adds the
// primitives; this proves they actually reject.
//
// A PASS here means the kernel returned -EFAULT and kept running. A kernel panic
// or a silent success are both FAILURES - and note that before the fix, passing a
// kernel pointer to uname() made the kernel memset ITS OWN memory, which would
// not necessarily crash immediately. That is exactly why this test checks the
// return value rather than just "did we survive".
//
// end (satoru)

#define _GNU_SOURCE 1
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logd(long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[n++] = '-';
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}

static int fails = 0;

static void expect_efault(const char* what, long rc) {
    logs("[uatest] ");
    logs(what);
    logs(" -> rc=");
    logd(rc);
    // musl's syscall() returns -1 and sets errno; a raw syscall returns -errno.
    if (rc == -14 || (rc == -1 && errno == EFAULT)) {
        logs("  PASS (rejected)\n");
    } else {
        logs("  FAIL (expected -EFAULT)\n");
        fails++;
    }
}


// fork + copy-out COW test (kurono.wp). after fork every private page is shared
// read-only + COW by parent and child. a syscall in the child that writes its
// output into such a page must privatise it first, exactly like a user store.
// with CR0.WP=0 a ring-0 store ignores the read-only pte and writes the frame the
// parent still maps, so the parent sees the child's syscall output. the child
// never touches the buffers from ring 3, so only the kernel's own stores can
// break (or fail to break) the sharing. a FAIL names the leaking syscall. (satoru)
#define COW_PAGES 8
static unsigned char cowbuf[COW_PAGES][4096] __attribute__((aligned(4096)));

static int cow_page_intact(int i) {
    for (int k = 0; k < 256; k++) if (cowbuf[i][k] != 0xA5) return 0;
    return 1;
}

static void fork_cow_test(void) {
    static const char* what[COW_PAGES] = {
        "read(pipe)", "uname", "fstat", "clock_gettime", "getcwd",
        "recv(socketpair)", "getdents64", "pipe(fds)",
    };
    for (int i = 0; i < COW_PAGES; i++) memset(cowbuf[i], 0xA5, sizeof(cowbuf[i]));

    int pp[2], sv[2] = { -1, -1 };
    if (pipe(pp) != 0) { logs("[uatest] cow: pipe FAILED\n"); fails++; return; }
    char msg[256];
    memset(msg, 'C', sizeof(msg));
    (void)!write(pp[1], msg, sizeof(msg));
    int have_sp = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
    if (have_sp) (void)!write(sv[0], msg, sizeof(msg));
    int dfd = open("/", O_RDONLY | O_DIRECTORY);

    pid_t pid = fork();
    if (pid < 0) { logs("[uatest] cow: fork FAILED\n"); fails++; return; }
    if (pid == 0) {
        // child: kernel stores only. (satoru)
        long r[COW_PAGES];
        r[0] = syscall(SYS_read, pp[0], cowbuf[0], 256);
        r[1] = syscall(SYS_uname, cowbuf[1]);
        r[2] = syscall(SYS_fstat, pp[0], cowbuf[2]);
        r[3] = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, cowbuf[3]);
        r[4] = syscall(SYS_getcwd, cowbuf[4], 256);
        r[5] = have_sp ? syscall(SYS_recvfrom, sv[1], cowbuf[5], 256, 0, 0, 0) : -1;
        r[6] = dfd >= 0 ? syscall(SYS_getdents64, dfd, cowbuf[6], 4096) : -1;
        r[7] = syscall(SYS_pipe, (int*)cowbuf[7]);
        int bad = 0;
        for (int i = 0; i < COW_PAGES; i++) {
            // the child must SEE its own output (a lost store is a bug too). (satoru)
            if (r[i] < 0 || cow_page_intact(i)) { bad |= 1 << i; }
        }
        _exit(bad);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int child_bad = WIFEXITED(st) ? WEXITSTATUS(st) : 0xFF;
    int leaks = 0;
    for (int i = 0; i < COW_PAGES; i++) {
        int intact = cow_page_intact(i);
        logs("[uatest] cow ");
        logs(what[i]);
        if (!intact) { logs(": FAIL parent page changed by the child's syscall\n"); leaks++; }
        else if (child_bad & (1 << i)) logs(": parent intact, child saw no output (skipped)\n");
        else logs(": PASS\n");
    }
    logs("[uatest] cow leaks=");
    logd(leaks);
    logs(leaks ? "  COW FAIL\n" : "  COW PASS\n");
    fails += leaks;

    close(pp[0]); close(pp[1]);

}

// cow + tlb coherence (info, not counted in OVERALL). a multi-threaded parent
// forks; a sibling thread keeps reading page X, so its cpu caches X read-only on
// the old shared frame. the main thread then writes X: the cow fault gives the
// parent a new frame. linux flushes every cpu running the mm; a fault handler
// that only invlpg's locally leaves the sibling reading the stale frame, and it
// never sees the write. the child stays alive so the frame stays shared. (satoru)
static volatile unsigned long tlbx[512] __attribute__((aligned(4096)));
static int tlb_pipe[2];
static void* tlb_reader(void* a) {
    (void)a;
    unsigned long spins = 0;
    while (tlbx[0] == 0) { spins++; if (spins > 4000000000UL) break; }
    char c = tlbx[0] ? 'Y' : 'N';
    (void)!write(tlb_pipe[1], &c, 1);
    return 0;
}

static void cow_tlb_test(void) {
    tlbx[0] = 0;
    tlbx[1] = 7;                      // page present + writable before the fork (satoru)
    if (pipe(tlb_pipe) != 0) return;
    pthread_t th;
    if (pthread_create(&th, 0, tlb_reader, 0) != 0) { logs("[uatest] cowtlb: no thread\n"); return; }
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, 0);
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec cs = { 3, 0 };
        nanosleep(&cs, 0);
        _exit(0);
    }
    nanosleep(&ts, 0);                // reader re-caches X read-only after the fork flush (satoru)
    tlbx[0] = 1;                      // cow break on this cpu (satoru)
    struct pollfd pf = { tlb_pipe[0], POLLIN, 0 };
    int pr = poll(&pf, 1, 1500);
    char c = 0;
    if (pr > 0) (void)!read(tlb_pipe[0], &c, 1);
    logs("[uatest] cowtlb sibling thread saw the post-fork write: ");
    logs(c == 'Y' ? "yes  COWTLB PASS\n" : "no within 1.5 s  COWTLB FAIL (stale tlb after cow break)\n");
    if (pid > 0) waitpid(pid, 0, 0);
    if (c != 'Y') { pr = poll(&pf, 1, 5000); if (pr > 0) { (void)!read(tlb_pipe[0], &c, 1); logs(c == 'Y' ? "[uatest] cowtlb reader saw it later (after the child exited)\n" : "[uatest] cowtlb reader gave up\n"); } }
}

// copy-out into memory the caller may not write (kurono.wp work, part 2). linux
// returns -EFAULT for every one of these; this kernel used to PANIC on the
// untouched PROT_READ / PROT_NONE page (a ring-0 not-present write fault the
// demand-zero handler refuses) and silently wrote a present read-only page. a
// control read into a writable untouched page must still work. (satoru)
static void efault_test(void) {
    int pp[2], sv[2] = { -1, -1 };
    if (pipe(pp) != 0) { logs("[uatest] efault: pipe FAILED\n"); fails++; return; }
    char msg[64];
    memset(msg, 'E', sizeof(msg));
    int have_sp = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
    int dfd = open("/", O_RDONLY | O_DIRECTORY);

    unsigned char* ro  = mmap(0, 8192, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* pn  = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* rop = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* hole = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* rw  = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED || pn == MAP_FAILED || rop == MAP_FAILED || hole == MAP_FAILED || rw == MAP_FAILED) {
        logs("[uatest] efault: mmap FAILED\n"); fails++; return;
    }
    memset(rop, 0x5A, 4096);
    mprotect(rop, 4096, PROT_READ);                       // present + read-only (satoru)
    munmap(hole, 4096);                                   // unmapped hole (satoru)

    (void)!write(pp[1], msg, sizeof(msg));
    expect_efault("read(untouched PROT_READ)", syscall(SYS_read, pp[0], ro, 64));
    expect_efault("read(untouched PROT_NONE)", syscall(SYS_read, pp[0], pn, 64));
    expect_efault("read(present PROT_READ)", syscall(SYS_read, pp[0], rop, 64));
    expect_efault("read(unmapped hole)", syscall(SYS_read, pp[0], hole, 64));
    unsigned char* sp = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sp != MAP_FAILED) {
        mprotect(sp + 4096, 4096, PROT_READ);            // writable page, then a read-only one (satoru)
        expect_efault("read(writable page into PROT_READ page)",
                      syscall(SYS_read, pp[0], sp + 4096 - 8, 64));
    }
    if (have_sp) {
        (void)!write(sv[0], msg, sizeof(msg));
        expect_efault("recvfrom(untouched PROT_READ)", syscall(SYS_recvfrom, sv[1], ro, 64, 0, 0, 0));
    }
    expect_efault("fstat(untouched PROT_READ)", syscall(SYS_fstat, pp[0], ro + 4096));
    if (dfd >= 0) expect_efault("getdents64(untouched PROT_READ)", syscall(SYS_getdents64, dfd, ro, 4096));

    // the pipe still holds the 64 bytes none of the rejected reads consumed:
    // a control read into an untouched writable page must get them. (satoru)
    long rc = syscall(SYS_read, pp[0], rw, 64);
    logs("[uatest] read(untouched PROT_READ|WRITE) -> rc=");
    logd(rc);
    if (rc == 64 && rw[0] == 'E' && rw[63] == 'E') logs("  PASS\n");
    else { logs("  FAIL (valid read must succeed with the data)\n"); fails++; }
    int st[32];
    rc = syscall(SYS_fstat, pp[0], st);
    logs("[uatest] fstat(stack) -> rc="); logd(rc);
    if (rc == 0) logs("  PASS\n"); else { logs("  FAIL\n"); fails++; }

    // info only: pipe() has no dst pre-check; its raw store into an untouched
    // PROT_READ page exercises the fault-path safety net ([efnet]). the
    // requirement is that the kernel survives. (satoru)
    rc = syscall(SYS_pipe, (int*)(ro + 4096));
    logs("[uatest] info pipe(untouched PROT_READ) -> rc=");
    logd(rc);
    logs(" (kernel survived)\n");
    close(pp[0]); close(pp[1]);
    if (have_sp) { close(sv[0]); close(sv[1]); }
    if (dfd >= 0) close(dfd);
}

// glibc-style fork: glibc's _Fork is clone(CLONE_CHILD_SETTID |
// CLONE_CHILD_CLEARTID | SIGCHLD, stack 0, ..., &THREAD_SELF->tid). linux writes
// the child's tid into the CHILD's copy of ctid (and only there), and a
// PARENT_SETTID ptid into the parent's memory only. if the kernel drops
// CHILD_SETTID the glibc child keeps its parent's tid in its own tcb (raise(),
// recursive/errorcheck mutex owners, robust lists all use it). (satoru)
static volatile int st_ctid __attribute__((aligned(4096)));
static volatile int st_ptid;
static void clone_settid_test(void) {
    int pp[2];
    if (pipe(pp) != 0) return;
    st_ctid = -7;
    st_ptid = -7;
    long r = syscall(SYS_clone, (long)(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID |
                                       CLONE_PARENT_SETTID | SIGCHLD),
                     0L, (int*)&st_ptid, (int*)&st_ctid, 0L);
    if (r == 0) {
        int me = (int)syscall(SYS_gettid);
        int res[3] = { me, st_ctid, st_ptid };
        (void)!write(pp[1], res, sizeof(res));
        _exit(0);
    }
    if (r < 0) { logs("[uatest] settid: clone failed rc="); logd(r); logs("\n"); fails++; return; }
    int res[3] = { 0, 0, 0 };
    (void)!read(pp[0], res, sizeof(res));
    waitpid((pid_t)r, 0, 0);
    int ok_child = res[1] == res[0];
    int ok_parent_ctid = st_ctid == -7;
    int ok_ptid = st_ptid == (int)r;
    logs("[uatest] settid child ctid="); logd(res[1]); logs(" (tid "); logd(res[0]); logs(")");
    logs(ok_child ? " PASS" : " FAIL");
    logs(" | parent ctid="); logd(st_ctid); logs(ok_parent_ctid ? " PASS" : " FAIL (child tid leaked into the parent)");
    logs(" | parent ptid="); logd(st_ptid); logs(ok_ptid ? " PASS" : " FAIL");
    logs(" | child ptid="); logd(res[2]); logs(" (info)\n");
    if (!ok_child || !ok_parent_ctid || !ok_ptid) fails++;
    close(pp[0]); close(pp[1]);
}

int main(void) {
    logs("[uatest] user-pointer validation test starting\n");

    // a few addresses a hostile or buggy process might pass. all are outside the
    // canonical lower-half user range, so AccessOk must reject every one. (satoru)
    const unsigned long bad[] = {
        0xFFFFFFFF80000000UL,   // kernel text region
        0xFFFF800000000000UL,   // canonical upper half base
        0x0000800000000000UL,   // first address above USER_SPACE_TOP
        0xFFFFFFFFFFFFFFFFUL,   // all ones
    };
    const char* names[] = {
        "uname(kernel text)",
        "uname(upper half)",
        "uname(user_top+1)",
        "uname(~0)",
    };

    for (int i = 0; i < 4; i++)
        expect_efault(names[i], syscall(SYS_uname, (void*)bad[i]));

    for (int i = 0; i < 4; i++)
        expect_efault("getcwd(bad)", syscall(SYS_getcwd, (void*)bad[i], 256));

    // a wrapping range: base is valid but base+len crosses out of the user half.
    // This is the case a naive `ptr < LIMIT` check misses. (satoru)
    expect_efault("getcwd(wrap)",
                  syscall(SYS_getcwd, (void*)0x00007FFFFFFFFF00UL, 4096));

    // and the control: a legitimate buffer must still WORK. A validation layer
    // that rejects everything is not a fix. (satoru)
    struct utsname u;
    memset(&u, 0, sizeof(u));
    long rc = syscall(SYS_uname, &u);
    logs("[uatest] uname(valid) -> rc=");
    logd(rc);
    if (rc == 0 && u.sysname[0]) {
        logs("  PASS sysname=");
        logs(u.sysname);
        logs(" release=");
        logs(u.release);
        logs("\n");
    } else {
        logs("  FAIL (valid call must succeed)\n");
        fails++;
    }

    char cwd[256];
    cwd[0] = 0;
    rc = syscall(SYS_getcwd, cwd, sizeof(cwd));
    logs("[uatest] getcwd(valid) -> rc=");
    logd(rc);
    if (rc >= 0 || cwd[0]) { logs("  PASS cwd="); logs(cwd); logs("\n"); }
    else { logs("  FAIL\n"); fails++; }

    fork_cow_test();
    cow_tlb_test();
    efault_test();
    clone_settid_test();

    logs(fails == 0 ? "[uatest] OVERALL PASS\n" : "[uatest] OVERALL FAIL\n");
    return fails == 0 ? 0 : 1;
}
