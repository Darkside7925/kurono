#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Signal System
//  POSIX signal handling for Linux processes running inside Kurono.
//  Implements signal delivery, handlers, masks, and queuing.
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// ─── Standard POSIX signals (Linux i386 numbering) ──────────────────────
#define LSIG_HUP      1
#define LSIG_INT      2
#define LSIG_QUIT     3
#define LSIG_ILL      4
#define LSIG_TRAP     5
#define LSIG_ABRT     6
#define LSIG_BUS      7
#define LSIG_FPE      8
#define LSIG_KILL     9
#define LSIG_USR1    10
#define LSIG_SEGV    11
#define LSIG_USR2    12
#define LSIG_PIPE    13
#define LSIG_ALRM    14
#define LSIG_TERM    15
#define LSIG_STKFLT  16
#define LSIG_CHLD    17
#define LSIG_CONT    18
#define LSIG_STOP    19
#define LSIG_TSTP    20
#define LSIG_TTIN    21
#define LSIG_TTOU    22
#define LSIG_URG     23
#define LSIG_XCPU    24
#define LSIG_XFSZ    25
#define LSIG_VTALRM  26
#define LSIG_PROF    27
#define LSIG_WINCH   28
#define LSIG_IO      29
#define LSIG_PWR     30
#define LSIG_SYS     31
#define LSIG_RTMIN   32
#define LSIG_RTMAX   64
#define LSIG_MAX     65

// Default signal actions
enum SignalAction {
    SIG_DFL_TERM = 0,    // Terminate
    SIG_DFL_IGN  = 1,    // Ignore
    SIG_DFL_CORE = 2,    // Terminate + core dump
    SIG_DFL_STOP = 3,    // Stop process
    SIG_DFL_CONT = 4     // Continue stopped process
};

// Signal handler types
#define LSIG_DFL    ((uint32_t)0)
#define LSIG_IGN    ((uint32_t)1)
#define LSIG_ERR    ((uint32_t)-1)

// Signal flags (SA_xxx)
#define LSA_NOCLDSTOP  0x00000001
#define LSA_NOCLDWAIT  0x00000002
#define LSA_SIGINFO    0x00000004
#define LSA_RESTART    0x10000000
#define LSA_NODEFER    0x40000000
#define LSA_RESETHAND  0x80000000

// sigprocmask how values
#define LSIG_BLOCK     0
#define LSIG_UNBLOCK   1
#define LSIG_SETMASK   2

// ─── Signal set (64 signals in a bitmask) ───────────────────────────────

struct LinuxSigset {
    uint64_t bits;
};

// ─── sigaction structure ────────────────────────────────────────────────

struct LinuxSigaction {
    uint32_t  sa_handler;     // Function pointer (or SIG_DFL/SIG_IGN)
    uint32_t  sa_flags;
    uint32_t  sa_restorer;    // Signal return trampoline
    LinuxSigset sa_mask;      // Signals blocked during handler
};

// ─── Per-process signal state ───────────────────────────────────────────

#define SIG_QUEUE_MAX  16

struct SignalQueueEntry {
    int      signo;
    uint32_t sender_pid;
    int      code;
    bool     pending;
};

struct ProcessSignalState {
    // Handler table
    LinuxSigaction handlers[LSIG_MAX];

    // Masks
    LinuxSigset blocked;     // Signals currently blocked
    LinuxSigset pending;     // Signals awaiting delivery

    // Queue for real-time signals
    SignalQueueEntry queue[SIG_QUEUE_MAX];
    int queue_count;

    // Saved context for signal return (sigreturn)
    uint32_t saved_esp;
    uint32_t saved_eip;
    uint32_t saved_eflags;
    bool     in_handler;     // Currently executing a signal handler
    int      current_signal; // Which signal is being handled
};

// ═══════════════════════════════════════════════════════════════════════════
//  LinuxSignals — Signal management
// ═══════════════════════════════════════════════════════════════════════════

class LinuxSignals {
public:
    static void Init();

    // Initialize per-process signal state
    static void InitProcess(int pid_idx);

    // Get signal state for a process
    static ProcessSignalState* GetState(int pid_idx);

    // Signal delivery
    static int  Kill(int sender_pid, int target_pid, int signo);
    static int  TKill(int tid, int signo);         // Thread-specific
    static int  SendSignal(int pid_idx, int signo, uint32_t sender_pid);

    // Signal handling
    static int  Sigaction(int pid_idx, int signo,
                           const LinuxSigaction* act,
                           LinuxSigaction* oldact);
    static int  Sigprocmask(int pid_idx, int how,
                              const LinuxSigset* set,
                              LinuxSigset* oldset);
    static int  SigPending(int pid_idx, LinuxSigset* set);
    static int  SigSuspend(int pid_idx, const LinuxSigset* mask);

    // Delivery check — called before returning to userspace
    static bool HasPendingSignal(int pid_idx);
    static int  DeliverPending(int pid_idx);

    // Sigreturn — called when signal handler returns
    static void Sigreturn(int pid_idx);

    // Alarm support
    static int  Alarm(int pid_idx, uint32_t seconds);
    static void TickAlarms(uint32_t now_ms);

    // Utility
    static SignalAction DefaultAction(int signo);
    static const char*  SignalName(int signo);
    static bool IsValid(int signo);

private:
    static ProcessSignalState states[16];  // Matches LINUX_MAX_PROCS

    // Alarm tracking
    struct AlarmEntry {
        int      pid_idx;
        uint32_t fire_time;  // ms
        bool     active;
    };
    static AlarmEntry alarms[16];

    // Helpers
    static void EnqueueSignal(ProcessSignalState* s, int signo, uint32_t sender);
    static int  DequeueSignal(ProcessSignalState* s);
    static bool IsBlocked(ProcessSignalState* s, int signo);
    static bool IsMasked(const LinuxSigset* set, int signo);
    static void SetBit(LinuxSigset* set, int signo);
    static void ClearBit(LinuxSigset* set, int signo);
};
