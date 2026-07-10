//  kurono os - linux signal system - implementation

#include "linux_signals.h"
#include "linux_syscall.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"

ProcessSignalState LinuxSignals::states[256];   // matches LINUX_MAX_PROCS (satoru)
LinuxSignals::AlarmEntry LinuxSignals::alarms[64];

void LinuxSignals::SetBit(LinuxSigset* set, int signo) {
    if (signo >= 1 && signo < LSIG_MAX)
        set->bits |= (1ULL << (signo - 1));
}

void LinuxSignals::ClearBit(LinuxSigset* set, int signo) {
    if (signo >= 1 && signo < LSIG_MAX)
        set->bits &= ~(1ULL << (signo - 1));
}

bool LinuxSignals::IsMasked(const LinuxSigset* set, int signo) {
    if (signo < 1 || signo >= LSIG_MAX) return false;
    return (set->bits & (1ULL << (signo - 1))) != 0;
}

bool LinuxSignals::IsBlocked(ProcessSignalState* s, int signo) {
    return IsMasked(&s->blocked, signo);
}

bool LinuxSignals::IsValid(int signo) {
    return signo >= 1 && signo < LSIG_MAX;
}

//  init

void LinuxSignals::Init() {
    memset(states, 0, sizeof(states));
    memset(alarms, 0, sizeof(alarms));
    SerialLogger::Log("[LinuxSignals] Signal system initialized\r\n");
}

void LinuxSignals::InitProcess(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= 64) return;
    ProcessSignalState* s = &states[pid_idx];
    memset(s, 0, sizeof(ProcessSignalState));

    // set default handlers
    for (int i = 0; i < LSIG_MAX; i++) {
        s->handlers[i].sa_handler = LSIG_DFL;
        s->handlers[i].sa_flags = 0;
        s->handlers[i].sa_restorer = 0;
        s->handlers[i].sa_mask.bits = 0;
    }
}

ProcessSignalState* LinuxSignals::GetState(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= 64) return nullptr;
    return &states[pid_idx];
}

//  signal delivery

int LinuxSignals::Kill(int sender_pid, int target_pid, int signo) {
    if (!IsValid(signo) && signo != 0) return -1;
    if (signo == 0) return 0;  // signal 0 = check if process exists

    // find target process index
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (p && p->active && (int)p->pid == target_pid) {
            return SendSignal(i, signo, (uint32_t)sender_pid);
        }
    }
    return -1;  // esrch
}

int LinuxSignals::TKill(int tid, int signo) {
    // thread-specific kill - for now, same as process kill
    return Kill(0, tid, signo);
}

int LinuxSignals::SendSignal(int pid_idx, int signo, uint32_t sender_pid) {
    if (pid_idx < 0 || pid_idx >= 64) return -1;
    ProcessSignalState* s = &states[pid_idx];

    // sigkill and sigstop cannot be caught or ignored
    if (signo == LSIG_KILL) {
        LinuxSyscall::DestroyProcess(pid_idx);
        return 0;
    }

    if (signo == LSIG_STOP) {
        // stop the process
        LinuxProcess* p = LinuxSyscall::GetProcess(pid_idx);
        if (p) p->active = false;
        return 0;
    }

    if (signo == LSIG_CONT) {
        LinuxProcess* p = LinuxSyscall::GetProcess(pid_idx);
        if (p) p->active = true;
        return 0;
    }

    // check handler
    if (s->handlers[signo].sa_handler == LSIG_IGN) {
        return 0;  // ignored
    }

    // mark pending
    SetBit(&s->pending, signo);
    EnqueueSignal(s, signo, sender_pid);

    return 0;
}

void LinuxSignals::EnqueueSignal(ProcessSignalState* s, int signo, uint32_t sender) {
    if (s->queue_count >= SIG_QUEUE_MAX) return;
    SignalQueueEntry* e = &s->queue[s->queue_count++];
    e->signo = signo;
    e->sender_pid = sender;
    e->code = 0;
    e->pending = true;
}

int LinuxSignals::DequeueSignal(ProcessSignalState* s) {
    for (int i = 0; i < s->queue_count; i++) {
        if (s->queue[i].pending && !IsBlocked(s, s->queue[i].signo)) {
            s->queue[i].pending = false;
            int signo = s->queue[i].signo;
            ClearBit(&s->pending, signo);

            // remove from queue (compact)
            for (int j = i; j < s->queue_count - 1; j++) {
                s->queue[j] = s->queue[j + 1];
            }
            s->queue_count--;
            return signo;
        }
    }
    return 0;
}

//  signal handling

int LinuxSignals::Sigaction(int pid_idx, int signo,
                             const LinuxSigaction* act,
                             LinuxSigaction* oldact) {
    if (!IsValid(signo)) return -1;
    if (signo == LSIG_KILL || signo == LSIG_STOP) return -1;  // can't change
    if (pid_idx < 0 || pid_idx >= 64) return -1;

    ProcessSignalState* s = &states[pid_idx];

    if (oldact) {
        memcpy(oldact, &s->handlers[signo], sizeof(LinuxSigaction));
    }
    if (act) {
        memcpy(&s->handlers[signo], act, sizeof(LinuxSigaction));
    }
    return 0;
}

int LinuxSignals::Sigprocmask(int pid_idx, int how,
                                const LinuxSigset* set,
                                LinuxSigset* oldset) {
    if (pid_idx < 0 || pid_idx >= 64) return -1;
    ProcessSignalState* s = &states[pid_idx];

    if (oldset) {
        oldset->bits = s->blocked.bits;
    }

    if (set) {
        switch (how) {
            case LSIG_BLOCK:
                s->blocked.bits |= set->bits;
                break;
            case LSIG_UNBLOCK:
                s->blocked.bits &= ~set->bits;
                break;
            case LSIG_SETMASK:
                s->blocked.bits = set->bits;
                break;
            default:
                return -1;
        }
        // sigkill and sigstop can never be blocked
        ClearBit(&s->blocked, LSIG_KILL);
        ClearBit(&s->blocked, LSIG_STOP);
    }
    return 0;
}

int LinuxSignals::SigPending(int pid_idx, LinuxSigset* set) {
    if (pid_idx < 0 || pid_idx >= 64 || !set) return -1;
    set->bits = states[pid_idx].pending.bits;
    return 0;
}

int LinuxSignals::SigSuspend(int pid_idx, const LinuxSigset* mask) {
    if (pid_idx < 0 || pid_idx >= 64 || !mask) return -1;
    // temporarily replace signal mask and sleep until a signal arrives
    ProcessSignalState* s = &states[pid_idx];
    LinuxSigset old = s->blocked;
    s->blocked.bits = mask->bits;
    ClearBit(&s->blocked, LSIG_KILL);
    ClearBit(&s->blocked, LSIG_STOP);

    // in a real kernel, this would block. we just restore and return.
    s->blocked = old;
    return -1;  // always returns -1 with errno = eintr
}

bool LinuxSignals::HasPendingSignal(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= 64) return false;
    ProcessSignalState* s = &states[pid_idx];

    // check if any pending signal is not blocked
    uint64_t deliverable = s->pending.bits & ~s->blocked.bits;
    return deliverable != 0;
}

int LinuxSignals::DeliverPending(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= 64) return 0;
    ProcessSignalState* s = &states[pid_idx];

    int signo = DequeueSignal(s);
    if (signo == 0) return 0;

    uint32_t handler = s->handlers[signo].sa_handler;

    if (handler == LSIG_DFL) {
        // execute default action
        SignalAction da = DefaultAction(signo);
        switch (da) {
            case SIG_DFL_TERM:
            case SIG_DFL_CORE:
                LinuxSyscall::DestroyProcess(pid_idx);
                break;
            case SIG_DFL_STOP: {
                LinuxProcess* p = LinuxSyscall::GetProcess(pid_idx);
                if (p) p->active = false;
                break;
            }
            case SIG_DFL_CONT: {
                LinuxProcess* p = LinuxSyscall::GetProcess(pid_idx);
                if (p) p->active = true;
                break;
            }
            case SIG_DFL_IGN:
            default:
                break;
        }
    } else if (handler != LSIG_IGN) {
        // user handler - would set up signal frame on stack
        // for now, mark that we're in a handler
        s->in_handler = true;
        s->current_signal = signo;

        // block signals specified in sa_mask during handler
        s->blocked.bits |= s->handlers[signo].sa_mask.bits;
        if (!(s->handlers[signo].sa_flags & LSA_NODEFER)) {
            SetBit(&s->blocked, signo);
        }

        // sa_resethand - reset to default after delivery
        if (s->handlers[signo].sa_flags & LSA_RESETHAND) {
            s->handlers[signo].sa_handler = LSIG_DFL;
        }
    }

    return signo;
}

void LinuxSignals::Sigreturn(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= 64) return;
    ProcessSignalState* s = &states[pid_idx];
    s->in_handler = false;
    s->current_signal = 0;
    // restore any blocked signals that were temporarily added
    // (simplified - real kernel saves/restores the mask)
}

//  alarm

int LinuxSignals::Alarm(int pid_idx, uint32_t seconds) {
    if (pid_idx < 0 || pid_idx >= 64) return 0;

    uint32_t now = Time::GetTicks();
    int remaining = 0;

    // cancel existing alarm and get remaining time
    for (int i = 0; i < 64; i++) {
        if (alarms[i].active && alarms[i].pid_idx == pid_idx) {
            remaining = (int)((alarms[i].fire_time - now) / 1000);
            if (remaining < 0) remaining = 0;
            alarms[i].active = false;
            break;
        }
    }

    if (seconds == 0) return remaining;  // just cancel

    // set new alarm
    for (int i = 0; i < 64; i++) {
        if (!alarms[i].active) {
            alarms[i].pid_idx = pid_idx;
            alarms[i].fire_time = now + seconds * 1000;
            alarms[i].active = true;
            break;
        }
    }
    return remaining;
}

void LinuxSignals::TickAlarms(uint32_t now_ms) {
    for (int i = 0; i < 64; i++) {
        if (alarms[i].active && now_ms >= alarms[i].fire_time) {
            alarms[i].active = false;
            SendSignal(alarms[i].pid_idx, LSIG_ALRM, 0);
        }
    }
}

//  utility

SignalAction LinuxSignals::DefaultAction(int signo) {
    switch (signo) {
        case LSIG_HUP:  case LSIG_INT:  case LSIG_PIPE:
        case LSIG_ALRM: case LSIG_TERM: case LSIG_USR1:
        case LSIG_USR2: case LSIG_STKFLT: case LSIG_IO:
        case LSIG_PROF: case LSIG_VTALRM: case LSIG_PWR:
            return SIG_DFL_TERM;

        case LSIG_QUIT: case LSIG_ILL:  case LSIG_TRAP:
        case LSIG_ABRT: case LSIG_BUS:  case LSIG_FPE:
        case LSIG_SEGV: case LSIG_SYS:  case LSIG_XCPU:
        case LSIG_XFSZ:
            return SIG_DFL_CORE;

        case LSIG_CHLD: case LSIG_URG:  case LSIG_WINCH:
            return SIG_DFL_IGN;

        case LSIG_STOP: case LSIG_TSTP:
        case LSIG_TTIN: case LSIG_TTOU:
            return SIG_DFL_STOP;

        case LSIG_CONT:
            return SIG_DFL_CONT;

        default:
            if (signo >= LSIG_RTMIN && signo <= LSIG_RTMAX)
                return SIG_DFL_TERM;
            return SIG_DFL_TERM;
    }
}

const char* LinuxSignals::SignalName(int signo) {
    switch (signo) {
        case LSIG_HUP:    return "SIGHUP";
        case LSIG_INT:    return "SIGINT";
        case LSIG_QUIT:   return "SIGQUIT";
        case LSIG_ILL:    return "SIGILL";
        case LSIG_TRAP:   return "SIGTRAP";
        case LSIG_ABRT:   return "SIGABRT";
        case LSIG_BUS:    return "SIGBUS";
        case LSIG_FPE:    return "SIGFPE";
        case LSIG_KILL:   return "SIGKILL";
        case LSIG_USR1:   return "SIGUSR1";
        case LSIG_SEGV:   return "SIGSEGV";
        case LSIG_USR2:   return "SIGUSR2";
        case LSIG_PIPE:   return "SIGPIPE";
        case LSIG_ALRM:   return "SIGALRM";
        case LSIG_TERM:   return "SIGTERM";
        case LSIG_CHLD:   return "SIGCHLD";
        case LSIG_CONT:   return "SIGCONT";
        case LSIG_STOP:   return "SIGSTOP";
        case LSIG_TSTP:   return "SIGTSTP";
        case LSIG_TTIN:   return "SIGTTIN";
        case LSIG_TTOU:   return "SIGTTOU";
        case LSIG_WINCH:  return "SIGWINCH";
        default:           return "SIG???";
    }
}
