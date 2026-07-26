// signal-delivery self-test for the kurono KLS. proves REAL syscall-boundary
// signal delivery: rt_sigaction stores handlers, kill/tkill/tgkill post them,
// rt_sigprocmask blocks/unblocks, and the handler round-trips through the rt
// sigframe + rt_sigreturn the kernel builds. built musl-static (x86_64 SYSCALL),
// so it exercises the SyscallEntryX64 delivery path. prints a single "SIGTEST
// PASS" to stderr (com1) on success and exit(0); any failure prints "SIGTEST
// FAIL <what>" and a nonzero exit. mirrors the pthread_test.c oracle style.
// (satoru)
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

static volatile sig_atomic_t usr1_count = 0;
static volatile sig_atomic_t alrm_count = 0;
static volatile int          last_signo = 0;

static void logs(const char* s) { write(2, s, strlen(s)); }

static void on_usr1(int s) { usr1_count++; last_signo = s; }
static void on_alrm(int s) { alrm_count++; last_signo = s; }

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

    logs("SIGTEST PASS\n");
    return 0;
}
// end (satoru)
