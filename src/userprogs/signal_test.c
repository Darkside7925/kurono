// signal-delivery self-test for the kurono KLS. proves REAL syscall-boundary
// signal delivery: rt_sigaction stores handlers, kill/tkill/tgkill post them,
// rt_sigprocmask blocks/unblocks, and the handler round-trips through the rt
// sigframe + rt_sigreturn the kernel builds. built musl-static (x86_64 SYSCALL),
// so it exercises the SyscallEntryX64 delivery path. prints a single "SIGTEST
// PASS" to stderr (com1) on success and exit(0); any failure prints "SIGTEST
// FAIL <what>" and a nonzero exit. mirrors the pthread_test.c oracle style.
// (satoru)
#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <stdlib.h>

static volatile sig_atomic_t usr1_count = 0;
static volatile sig_atomic_t alrm_count = 0;
static volatile int          last_signo = 0;

static void logs(const char* s) { write(2, s, strlen(s)); }

static void on_usr1(int s) { usr1_count++; last_signo = s; }
static void on_alrm(int s) { alrm_count++; last_signo = s; }

// sigaltstack: the SA_ONSTACK handler records where its own frame lives. (satoru)
static char altstk[65536] __attribute__((aligned(16)));
static volatile uintptr_t alt_sp_seen = 0;
static volatile int alt_flags_seen = -1;
// a handler that corrupts its saved rip: rt_sigreturn must refuse it with a
// SIGSEGV instead of handing a non-canonical address to iretq. (satoru)
static void on_bad(int s, siginfo_t* si, void* ctx) {
    (void)s; (void)si;
    ucontext_t* uc = (ucontext_t*)ctx;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)0x8000000000001000ull;
}
static void on_usr2(int s) {
    (void)s;
    int local = 0;
    alt_sp_seen = (uintptr_t)&local;
    stack_t cur;
    if (sigaltstack(0, &cur) == 0) alt_flags_seen = cur.ss_flags;
}

int main(void) {
    logs("SIGTEST: start\n");

    struct sigaction sa;

    // register SIGUSR1 handler (satoru)
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, 0) != 0) { logs("SIGTEST FAIL sigaction-usr1\n"); return 1; }

    // register SIGALRM handler (satoru)
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, 0) != 0) { logs("SIGTEST FAIL sigaction-alrm\n"); return 1; }

    // 1) raise(SIGUSR1): musl blocks-tkill-unblocks; the handler must fire at the
    //    unblock boundary with the right signum. (satoru)
    last_signo = 0;
    raise(SIGUSR1);
    if (usr1_count != 1 || last_signo != SIGUSR1) { logs("SIGTEST FAIL raise\n"); return 2; }

    // 2) kill(getpid(), SIGUSR1): posts to the process, delivered at the boundary. (satoru)
    last_signo = 0;
    kill(getpid(), SIGUSR1);
    if (usr1_count != 2 || last_signo != SIGUSR1) { logs("SIGTEST FAIL kill\n"); return 3; }

    // 3) sigprocmask blocking: block SIGUSR1, raise it (must NOT deliver), then
    //    unblock (must deliver the now-pending signal). (satoru)
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0) { logs("SIGTEST FAIL sigprocmask-block\n"); return 4; }
    raise(SIGUSR1);
    if (usr1_count != 2) { logs("SIGTEST FAIL delivered-while-blocked\n"); return 4; }
    if (sigprocmask(SIG_UNBLOCK, &block, 0) != 0) { logs("SIGTEST FAIL sigprocmask-unblock\n"); return 5; }
    if (usr1_count != 3) { logs("SIGTEST FAIL no-delivery-after-unblock\n"); return 5; }

    // 4) a second distinct signal (SIGALRM) via kill, to prove the handler table
    //    is per-signal. (satoru)
    last_signo = 0;
    kill(getpid(), SIGALRM);
    if (alrm_count != 1 || last_signo != SIGALRM) { logs("SIGTEST FAIL alrm\n"); return 6; }

    // 5) sigaltstack: an SA_ONSTACK handler runs on the alternate stack, sees
    //    SS_ONSTACK while there, and the main stack is back after return. (satoru)
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstk;
    ss.ss_size = sizeof(altstk);
    if (sigaltstack(&ss, 0) != 0) { logs("SIGTEST FAIL sigaltstack-set\n"); return 7; }
    stack_t got;
    if (sigaltstack(0, &got) != 0 || got.ss_sp != altstk || got.ss_size != sizeof(altstk) ||
        (got.ss_flags & SS_DISABLE)) { logs("SIGTEST FAIL sigaltstack-get\n"); return 7; }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr2;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, 0) != 0) { logs("SIGTEST FAIL sigaction-usr2\n"); return 7; }
    raise(SIGUSR2);
    if (alt_sp_seen < (uintptr_t)altstk || alt_sp_seen >= (uintptr_t)altstk + sizeof(altstk)) {
        logs("SIGTEST FAIL altstack-not-used\n"); return 8;
    }
    if (!(alt_flags_seen & SS_ONSTACK)) { logs("SIGTEST FAIL altstack-flags\n"); return 8; }
    if (sigaltstack(0, &got) != 0 || (got.ss_flags & SS_ONSTACK)) { logs("SIGTEST FAIL altstack-after\n"); return 8; }
    ss.ss_size = 512;
    if (sigaltstack(&ss, 0) == 0) { logs("SIGTEST FAIL altstack-small-accepted\n"); return 9; }
    ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, 0) != 0) { logs("SIGTEST FAIL altstack-disable\n"); return 9; }
    logs("SIGTEST altstack ok\n");

    // 6) a corrupt sigreturn context kills only the child, with SIGSEGV. (satoru)
    pid_t ch = fork();
    if (ch == 0) {
        struct sigaction b;
        memset(&b, 0, sizeof(b));
        b.sa_sigaction = on_bad;
        b.sa_flags = SA_SIGINFO;
        sigemptyset(&b.sa_mask);
        sigaction(SIGUSR1, &b, 0);
        raise(SIGUSR1);
        _exit(42);   /* reached only if the bad rip was silently ignored */
    }
    if (ch < 0) { logs("SIGTEST FAIL fork\n"); return 10; }
    int st = 0;
    if (waitpid(ch, &st, 0) != ch) { logs("SIGTEST FAIL waitpid\n"); return 10; }
    int segv = (WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV) ||
               (WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGSEGV);
    if (!segv) { logs("SIGTEST FAIL badsigreturn-not-segv\n"); return 10; }
    logs("SIGTEST badsigreturn ok\n");

    logs("SIGTEST PASS\n");
    return 0;
}
// end (satoru)
