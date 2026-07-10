#pragma once
//  kurono os - linux signal system
//  posix signal handling for linux processes running inside kurono.
//  implements signal delivery, handlers, masks, and queuing.

#include "../kernel/types.h"

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

// default signal actions
enum SignalAction {
    SIG_DFL_TERM = 0,    // terminate
    SIG_DFL_IGN  = 1,    // ignore
    SIG_DFL_CORE = 2,    // terminate + core dump
    SIG_DFL_STOP = 3,    // stop process
    SIG_DFL_CONT = 4     // continue stopped process
};

// signal handler types
#define LSIG_DFL    ((uint32_t)0)
#define LSIG_IGN    ((uint32_t)1)
#define LSIG_ERR    ((uint32_t)-1)

// signal flags (sa_xxx)
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

struct LinuxSigset {
    uint64_t bits;
};

struct LinuxSigaction {
    uint32_t  sa_handler;     // function pointer (or sig_dfl/sig_ign)
    uint32_t  sa_flags;
    uint32_t  sa_restorer;    // signal return trampoline
    LinuxSigset sa_mask;      // signals blocked during handler
};

#define SIG_QUEUE_MAX  16

struct SignalQueueEntry {
    int      signo;
    uint32_t sender_pid;
    int      code;
    bool     pending;
};

struct ProcessSignalState {
    // handler table
    LinuxSigaction handlers[LSIG_MAX];

    // masks
    LinuxSigset blocked;     // signals currently blocked
    LinuxSigset pending;     // signals awaiting delivery

    // queue for real-time signals
    SignalQueueEntry queue[SIG_QUEUE_MAX];
    int queue_count;

    // saved context for signal return (sigreturn)
    uint32_t saved_esp;
    uint32_t saved_eip;
    uint32_t saved_eflags;
    bool     in_handler;     // currently executing a signal handler
    int      current_signal; // which signal is being handled
};

//  linuxsignals - signal management

class LinuxSignals {
public:
    static void Init();

    // initialize per-process signal state
    static void InitProcess(int pid_idx);

    // get signal state for a process
    static ProcessSignalState* GetState(int pid_idx);

    // signal delivery
    static int  Kill(int sender_pid, int target_pid, int signo);
    static int  TKill(int tid, int signo);         // thread-specific
    static int  SendSignal(int pid_idx, int signo, uint32_t sender_pid);

    // signal handling
    static int  Sigaction(int pid_idx, int signo,
                           const LinuxSigaction* act,
                           LinuxSigaction* oldact);
    static int  Sigprocmask(int pid_idx, int how,
                              const LinuxSigset* set,
                              LinuxSigset* oldset);
    static int  SigPending(int pid_idx, LinuxSigset* set);
    static int  SigSuspend(int pid_idx, const LinuxSigset* mask);

    // delivery check - called before returning to userspace
    static bool HasPendingSignal(int pid_idx);
    static int  DeliverPending(int pid_idx);

    // sigreturn - called when signal handler returns
    static void Sigreturn(int pid_idx);

    // alarm support
    static int  Alarm(int pid_idx, uint32_t seconds);
    static void TickAlarms(uint32_t now_ms);

    // utility
    static SignalAction DefaultAction(int signo);
    static const char*  SignalName(int signo);
    static bool IsValid(int signo);

private:
    static ProcessSignalState states[256];  // matches LINUX_MAX_PROCS (satoru)

    // alarm tracking
    struct AlarmEntry {
        int      pid_idx;
        uint32_t fire_time;  // ms
        bool     active;
    };
    static AlarmEntry alarms[64];

    // helpers
    static void EnqueueSignal(ProcessSignalState* s, int signo, uint32_t sender);
    static int  DequeueSignal(ProcessSignalState* s);
    static bool IsBlocked(ProcessSignalState* s, int signo);
    static bool IsMasked(const LinuxSigset* set, int signo);
    static void SetBit(LinuxSigset* set, int signo);
    static void ClearBit(LinuxSigset* set, int signo);
};
