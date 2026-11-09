#include "kpty.h"
#include "linux_syscall.h"
#include "../proc/spinlock.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"

// see kpty.h. a pair's fields are guarded by its own lock (irq-save, leaf lock:
// nothing that can block or allocate runs under it). signals and poll wakes are
// raised after the lock is dropped. the table lock only guards slot claim/free. (satoru)

namespace {
    // termios flag bits (linux asm-generic) (satoru)
    const uint32_t I_ISTRIP = 0x20, I_INLCR = 0x40, I_IGNCR = 0x80, I_ICRNL = 0x100;
    const uint32_t O_OPOST = 0x1, O_ONLCR = 0x4, O_OCRNL = 0x8;
    const uint32_t L_ISIG = 0x1, L_ICANON = 0x2, L_ECHO = 0x8, L_ECHOE = 0x10, L_ECHOK = 0x20,
                   L_ECHONL = 0x40, L_NOFLSH = 0x80, L_ECHOCTL = 0x200, L_ECHOKE = 0x800;
    enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5, VMIN = 6,
           VSUSP = 10, VEOL = 11, VWERASE = 14, VEOL2 = 16 };

    const int OUT_SZ = 65536;      // slave -> master (program output + echo) (satoru)
    const int IN_SZ  = 8192;       // committed input the slave can read (satoru)
    const int LINE_MAX_ = 4096;    // canonical line being edited (satoru)
    const int REC_MAX = 64;        // committed line records waiting for a read (satoru)

    struct Rings {
        uint8_t out[OUT_SZ];
        uint8_t in[IN_SZ];
        uint8_t line[LINE_MAX_];
        int     rec[REC_MAX];
    };

    struct Pair {
        Spinlock lock;
        volatile int used;        // 0 free, 1 live (claimed under g_table_lock) (satoru)
        int      flags;
        bool     kernel_owned;    // the kernel (a Terminal window) holds the master (satoru)
        bool     hup;
        bool     slave_opened;    // some slave fd has existed since Reset (satoru)
        int      s_refs, m_refs;
        KTermios tio;
        KWinsize ws;
        int      sid, fg_pgrp;    // controlling session and its foreground group (satoru)
        Rings*   r;               // ring block, kept for the slot's lifetime (satoru)
        int out_h, out_t, in_h, in_t, rec_h, rec_t, line_n;
    };

    Spinlock g_table_lock;
    Pair     g_pair[Kpty::MAX_PAIRS];

    inline bool vt(int t) { return t >= 0 && t < Kpty::MAX_PAIRS && g_pair[t].used == 1 && g_pair[t].r; }

    inline int out_used(Pair& p) { return (p.out_h - p.out_t + OUT_SZ) % OUT_SZ; }
    inline int out_free(Pair& p) { return OUT_SZ - 1 - out_used(p); }
    inline int in_used(Pair& p)  { return (p.in_h - p.in_t + IN_SZ) % IN_SZ; }
    inline int in_free(Pair& p)  { return IN_SZ - 1 - in_used(p); }
    inline int rec_used(Pair& p) { return (p.rec_h - p.rec_t + REC_MAX) % REC_MAX; }

    inline void out_raw(Pair& p, uint8_t c) {
        if (out_free(p) <= 0) return;
        p.r->out[p.out_h] = c; p.out_h = (p.out_h + 1) % OUT_SZ;
    }
    // output processing (opost/onlcr/ocrnl) then into the ring. caller holds the lock. (satoru)
    inline void out_proc(Pair& p, uint8_t c) {
        if (p.tio.c_oflag & O_OPOST) {
            if (c == '\n' && (p.tio.c_oflag & O_ONLCR)) { out_raw(p, '\r'); out_raw(p, '\n'); return; }
            if (c == '\r' && (p.tio.c_oflag & O_OCRNL)) { out_raw(p, '\n'); return; }
        }
        out_raw(p, c);
    }
    inline void in_push(Pair& p, uint8_t c) {
        if (in_free(p) <= 0) return;
        p.r->in[p.in_h] = c; p.in_h = (p.in_h + 1) % IN_SZ;
    }
    inline void echo_char(Pair& p, uint8_t c) {
        if (c == '\n') { out_proc(p, '\n'); return; }
        if (c == '\t') { out_raw(p, '\t'); return; }
        if (c < 0x20 || c == 0x7F) {
            if (p.tio.c_lflag & L_ECHOCTL) { out_raw(p, '^'); out_raw(p, c == 0x7F ? '?' : (uint8_t)(c ^ 0x40)); }
            return;
        }
        out_raw(p, c);
    }
    // erase the last character (a whole utf-8 sequence) of the canonical line. (satoru)
    inline void line_erase_one(Pair& p) {
        if (p.line_n <= 0) return;
        p.line_n--;
        while (p.line_n > 0 && (p.r->line[p.line_n] & 0xC0) == 0x80) p.line_n--;
        if (p.tio.c_lflag & L_ECHO) {
            if (p.tio.c_lflag & L_ECHOE) { out_raw(p, '\b'); out_raw(p, ' '); out_raw(p, '\b'); }
        }
    }
    // move the edited line into the readable ring as one record. (satoru)
    inline void line_commit(Pair& p, bool add_nl) {
        int n = p.line_n + (add_nl ? 1 : 0);
        if (rec_used(p) >= REC_MAX - 1) { p.line_n = 0; return; }
        if (in_free(p) < n) { p.line_n = 0; return; }
        for (int i = 0; i < p.line_n; i++) in_push(p, p.r->line[i]);
        if (add_nl) in_push(p, '\n');
        p.line_n = 0;
        p.r->rec[p.rec_h] = n; p.rec_h = (p.rec_h + 1) % REC_MAX;
    }
    inline void flush_input_locked(Pair& p) {
        p.in_h = p.in_t = 0; p.rec_h = p.rec_t = 0; p.line_n = 0;
    }
    inline void post_dirty() { Graphics::MarkUIDirty(); }

    void reset_locked(Pair& p, int cols, int rows) {
        if (cols < 10) cols = 10; if (cols > 500) cols = 500;
        if (rows < 2) rows = 2;   if (rows > 300) rows = 300;
        Kpty::DefaultTermios(&p.tio);
        p.ws.ws_col = (uint16_t)cols; p.ws.ws_row = (uint16_t)rows; p.ws.ws_xpixel = 0; p.ws.ws_ypixel = 0;
        p.out_h = p.out_t = 0;
        flush_input_locked(p);
        p.sid = 0; p.fg_pgrp = 0;
        p.hup = false;
        p.slave_opened = false;
    }
    void apply_capture(Pair& p) {
        // a kernel capture takes the bytes as the program wrote them: no cr/nl mangling,
        // no echo, no line editing. (satoru)
        p.tio.c_oflag = 0;
        p.tio.c_lflag = 0;
        p.tio.c_iflag = 0;
        p.tio.c_cc[VMIN] = 1;
    }
}

// pty counters for the heartbeat: bytes in, reads, bytes read, would-block. (satoru)
namespace Kpty {
volatile uint32_t g_ptydbg[8] = {0};   // +[4] writes, [5] bytes written, [6] would-block writes, [7] peak output backlog (satoru)

void DefaultTermios(KTermios* t) {
    t->c_iflag = 0x6500;           // ICRNL | IXON | IMAXBEL | IUTF8 (satoru)
    t->c_oflag = O_OPOST | O_ONLCR;
    t->c_cflag = 0x4BF;            // B38400 | CS8 | CREAD | HUPCL (satoru)
    t->c_lflag = 0x8A3B;           // ISIG ICANON ECHO ECHOE ECHOK ECHOCTL ECHOKE IEXTEN (satoru)
    t->c_line = 0;
    for (int i = 0; i < 19; i++) t->c_cc[i] = 0;
    t->c_cc[VINTR] = 3; t->c_cc[VQUIT] = 28; t->c_cc[VERASE] = 127; t->c_cc[VKILL] = 21;
    t->c_cc[VEOF] = 4;  t->c_cc[VMIN] = 1;   t->c_cc[VTIME] = 0;
    t->c_cc[8] = 17;    t->c_cc[9] = 19;     t->c_cc[VSUSP] = 26;   t->c_cc[12] = 18;
    t->c_cc[13] = 15;   t->c_cc[VWERASE] = 23; t->c_cc[15] = 22;
}

// ---- table ----

void Init() {
    // pair 0 is the legacy single session; its ring block exists from the start so the
    // old path never allocates on the hot path. (satoru)
    Pair& p = g_pair[LEGACY];
    if (p.used) return;
    p.r = (Rings*)KernelHeap::Alloc(sizeof(Rings));
    if (!p.r) return;
    p.flags = 0; p.kernel_owned = true; p.s_refs = p.m_refs = 0;
    reset_locked(p, 80, 25);
    p.used = 1;
    p.hup = true;               // idle until Begin (satoru)
}

int Alloc(int cols, int rows, int flags) {
    int t = -1;
    {
        SpinLockGuard g(g_table_lock);
        for (int i = 1; i < MAX_PAIRS; i++) if (g_pair[i].used == 0) { g_pair[i].used = 2; t = i; break; }   // 2 = being set up (satoru)
    }
    if (t < 0) return -1;
    Pair& p = g_pair[t];
    if (!p.r) {
        p.r = (Rings*)KernelHeap::Alloc(sizeof(Rings));
        if (!p.r) { p.used = 0; return -1; }
    }
    {
        SpinLockGuard g(p.lock);
        p.flags = flags;
        p.kernel_owned = !(flags & F_USERMASTER);
        p.s_refs = 0; p.m_refs = (flags & F_USERMASTER) ? 1 : 0;
        reset_locked(p, cols, rows);
        if (flags & F_CAPTURE) apply_capture(p);
    }
    __atomic_store_n(&p.used, 1, __ATOMIC_RELEASE);
    return t;
}

static void maybe_free(int t) {
    Pair& p = g_pair[t];
    bool freeit = false;
    {
        SpinLockGuard g(p.lock);
        if (p.used == 1 && !p.kernel_owned && p.m_refs <= 0 && p.s_refs <= 0) freeit = true;
    }
    if (freeit) __atomic_store_n(&p.used, 0, __ATOMIC_RELEASE);
}

void Free(int t) {
    if (t <= LEGACY || t >= MAX_PAIRS || g_pair[t].used != 1) return;
    Pair& p = g_pair[t];
    {
        SpinLockGuard g(p.lock);
        p.kernel_owned = false;
        p.hup = true;
    }
    maybe_free(t);
}

bool Valid(int t) { return t >= 0 && t < MAX_PAIRS && __atomic_load_n(&g_pair[t].used, __ATOMIC_ACQUIRE) == 1; }
bool Visible(int t) { return Valid(t) && (g_pair[t].flags & F_VISIBLE); }
bool IsCapture(int t) { return Valid(t) && (g_pair[t].flags & F_CAPTURE); }
bool Locked(int t) { return Valid(t) && (g_pair[t].flags & F_LOCKED); }
void SetLocked(int t, bool on) {
    if (!Valid(t)) return;
    SpinLockGuard g(g_pair[t].lock);
    if (on) g_pair[t].flags |= F_LOCKED; else g_pair[t].flags &= ~F_LOCKED;
}
int CountAlive() { int n = 0; for (int i = 1; i < MAX_PAIRS; i++) if (g_pair[i].used == 1) n++; return n; }

void Reset(int t, int cols, int rows) {
    if (!vt(t)) return;
    Pair& p = g_pair[t];
    SpinLockGuard g(p.lock);
    reset_locked(p, cols, rows);
    if (p.flags & F_CAPTURE) apply_capture(p);
}

void SlaveRef(int t, int delta) {
    if (!Valid(t)) return;
    Pair& p = g_pair[t];
    bool closed_last = false;
    {
        SpinLockGuard g(p.lock);
        p.s_refs += delta;
        if (delta > 0) p.slave_opened = true;
        if (p.s_refs < 0) p.s_refs = 0;
        closed_last = (delta < 0 && p.s_refs == 0);
    }
    if (closed_last) LinuxSyscall::PtyWake(t, 1);   // a master reader sees the hangup (satoru)
    if (delta < 0) maybe_free(t);
}
void MasterRef(int t, int delta) {
    if (!Valid(t)) return;
    Pair& p = g_pair[t];
    bool last = false;
    {
        SpinLockGuard g(p.lock);
        p.m_refs += delta;
        if (p.m_refs < 0) p.m_refs = 0;
        last = (delta < 0 && p.m_refs == 0 && (p.flags & F_USERMASTER));
    }
    if (last) Hangup(t);   // the master is gone: the slave side is hung up (satoru)
    if (delta < 0) maybe_free(t);
}
int  SlaveRefs(int t) { return Valid(t) ? g_pair[t].s_refs : 0; }
bool SlaveWasOpened(int t) { return Valid(t) && g_pair[t].slave_opened; }

// ---- controlling session ----

int ForegroundPgrp(int t) { return vt(t) ? __atomic_load_n(&g_pair[t].fg_pgrp, __ATOMIC_ACQUIRE) : 0; }
void SetForegroundPgrp(int t, int pg) { if (vt(t)) __atomic_store_n(&g_pair[t].fg_pgrp, pg, __ATOMIC_RELEASE); }
int SessionOf(int t) { return vt(t) ? __atomic_load_n(&g_pair[t].sid, __ATOMIC_ACQUIRE) : 0; }
void SetSession(int t, int sid, int pgrp) {
    if (!vt(t)) return;
    SpinLockGuard g(g_pair[t].lock);
    g_pair[t].sid = sid; g_pair[t].fg_pgrp = pgrp;
}
void ClearSession(int t) {
    if (!vt(t)) return;
    SpinLockGuard g(g_pair[t].lock);
    g_pair[t].sid = 0; g_pair[t].fg_pgrp = 0;
}
int FindBySession(int sid) {
    if (sid <= 0) return -1;
    for (int i = 0; i < MAX_PAIRS; i++)
        if (g_pair[i].used == 1 && __atomic_load_n(&g_pair[i].sid, __ATOMIC_ACQUIRE) == sid) return i;
    return -1;
}

// ---- hangup / winsize ----

bool Hungup(int t) { return !vt(t) || __atomic_load_n(&g_pair[t].hup, __ATOMIC_ACQUIRE); }

void Hangup(int t) {
    if (!vt(t)) return;
    Pair& p = g_pair[t];
    int sid;
    {
        SpinLockGuard g(p.lock);
        if (p.hup && p.sid == 0) return;
        p.hup = true;
        sid = p.sid;
        p.sid = 0; p.fg_pgrp = 0;      // the tty no longer controls the session (satoru)
    }
    // __tty_hangup: SIGHUP then SIGCONT to the session leader; the shell or the program
    // hangs its jobs up. every other group of the session gets the same pair so a
    // program that ignores nothing dies with its window. (satoru)
    if (sid > 0) LinuxSyscall::TtyHangupSession(sid);
    LinuxSyscall::PtyWake(t, 0);
    LinuxSyscall::PtyWake(t, 1);
}

void Resize(int t, int cols, int rows) {
    if (!vt(t)) return;
    if (cols < 10) cols = 10; if (cols > 500) cols = 500;
    if (rows < 2) rows = 2;   if (rows > 300) rows = 300;
    Pair& p = g_pair[t];
    int pg;
    {
        SpinLockGuard g(p.lock);
        if (p.ws.ws_col == cols && p.ws.ws_row == rows) return;
        p.ws.ws_col = (uint16_t)cols; p.ws.ws_row = (uint16_t)rows;
        pg = p.fg_pgrp;
    }
    if (pg > 0) LinuxSyscall::SignalGroup(pg, 28);   // SIGWINCH to THIS tty's group (satoru)
}

// ---- master side ----

bool MasterHasData(int t) {
    if (!vt(t)) return false;
    return __atomic_load_n(&g_pair[t].out_h, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_pair[t].out_t, __ATOMIC_ACQUIRE);
}

int MasterRead(int t, uint8_t* d, int max) {
    if (!vt(t)) return 0;
    Pair& p = g_pair[t];
    SpinLockGuard g(p.lock);
    int n = 0;
    while (n < max && p.out_t != p.out_h) {
        d[n++] = p.r->out[p.out_t];
        p.out_t = (p.out_t + 1) % OUT_SZ;
    }
    return n;
}

int MasterAvail(int t) {
    if (!vt(t)) return 0;
    SpinLockGuard g(g_pair[t].lock);
    return out_used(g_pair[t]);
}

bool MasterReadable(int t) {
    if (!vt(t)) return true;
    Pair& p = g_pair[t];
    if (MasterHasData(t)) return true;
    return p.slave_opened && __atomic_load_n(&p.s_refs, __ATOMIC_ACQUIRE) <= 0;
}
bool MasterSlaveGone(int t) {
    if (!vt(t)) return true;
    return g_pair[t].slave_opened && __atomic_load_n(&g_pair[t].s_refs, __ATOMIC_ACQUIRE) <= 0;
}

// the line discipline. one byte run from the master; signals are collected and raised
// after the lock is released. (satoru)
void MasterWrite(int t, const uint8_t* d, int n) {
    if (!vt(t)) return;
    if (n > 0) __atomic_add_fetch(&g_ptydbg[0], (uint32_t)n, __ATOMIC_RELAXED);
    Pair& p = g_pair[t];
    int sig = 0, pg = 0;
    bool wake_in = false, wake_out = false;
    {
        SpinLockGuard g(p.lock);
        if (p.hup) return;
        int in_before = (p.tio.c_lflag & L_ICANON) ? rec_used(p) : in_used(p);
        int out_before = out_used(p);
        const uint32_t lf = p.tio.c_lflag;
        const bool canon = (lf & L_ICANON) != 0;
        for (int i = 0; i < n; i++) {
            uint8_t c = d[i];
            if (p.tio.c_iflag & I_ISTRIP) c &= 0x7F;
            if (c == '\r') {
                if (p.tio.c_iflag & I_IGNCR) continue;
                if (p.tio.c_iflag & I_ICRNL) c = '\n';
            } else if (c == '\n' && (p.tio.c_iflag & I_INLCR)) {
                c = '\r';
            }
            if (lf & L_ISIG) {
                int s = 0;
                if (c == p.tio.c_cc[VINTR] && c != 0) s = 2;            // SIGINT (satoru)
                else if (c == p.tio.c_cc[VQUIT] && c != 0) s = 3;       // SIGQUIT (satoru)
                else if (c == p.tio.c_cc[VSUSP] && c != 0) s = 20;      // SIGTSTP (satoru)
                if (s) {
                    if (!(lf & L_NOFLSH)) flush_input_locked(p);
                    if (lf & L_ECHO) echo_char(p, c);
                    sig = s; pg = p.fg_pgrp;
                    continue;
                }
            }
            if (canon) {
                if (c == p.tio.c_cc[VERASE] && c != 0) { line_erase_one(p); continue; }
                if (c == p.tio.c_cc[VKILL] && c != 0) {
                    if ((lf & L_ECHO) && (lf & L_ECHOKE)) { while (p.line_n > 0) line_erase_one(p); }
                    else { p.line_n = 0; if ((lf & L_ECHO) && (lf & L_ECHOK)) out_proc(p, '\n'); }
                    continue;
                }
                if (c == p.tio.c_cc[VWERASE] && c != 0) {
                    while (p.line_n > 0 && p.r->line[p.line_n - 1] == ' ') line_erase_one(p);
                    while (p.line_n > 0 && p.r->line[p.line_n - 1] != ' ') line_erase_one(p);
                    continue;
                }
                if (c == p.tio.c_cc[VEOF] && c != 0) {
                    if (p.line_n == 0) {                     // eof on an empty line = a zero length record (satoru)
                        if (rec_used(p) < REC_MAX - 1) { p.r->rec[p.rec_h] = 0; p.rec_h = (p.rec_h + 1) % REC_MAX; }
                    } else {
                        line_commit(p, false);
                    }
                    continue;
                }
                if (c == '\n' || (c == p.tio.c_cc[VEOL] && c != 0)) {
                    if (lf & (L_ECHO | L_ECHONL)) out_proc(p, '\n');
                    line_commit(p, true);
                    continue;
                }
                if (p.line_n < LINE_MAX_ - 1) {
                    p.r->line[p.line_n++] = c;
                    if (lf & L_ECHO) echo_char(p, c);
                }
            } else {
                in_push(p, c);
                if (lf & L_ECHO) echo_char(p, c);
            }
        }
        int in_after = (p.tio.c_lflag & L_ICANON) ? rec_used(p) : in_used(p);
        wake_in = in_after != in_before || (p.rec_h != p.rec_t);
        wake_out = out_used(p) != out_before;
    }
    if (sig && pg > 0) LinuxSyscall::SignalGroup(pg, sig);
    if (wake_in) LinuxSyscall::PtyWake(t, 0);     // a slave reader (satoru)
    if (wake_out) LinuxSyscall::PtyWake(t, 1);    // the echo is master readable (satoru)
    post_dirty();
}

// ---- slave side ----

static int slave_read_impl(Pair& p, uint8_t* d, int max) {
    SpinLockGuard g(p.lock);
    if (max <= 0) return 0;
    if (p.tio.c_lflag & L_ICANON) {
        if (rec_used(p) == 0) return p.hup ? 0 : -11;
        int len = p.r->rec[p.rec_t];
        if (len == 0) { p.rec_t = (p.rec_t + 1) % REC_MAX; return 0; }   // eof (satoru)
        int n = len < max ? len : max;
        for (int i = 0; i < n; i++) { d[i] = p.r->in[p.in_t]; p.in_t = (p.in_t + 1) % IN_SZ; }
        p.r->rec[p.rec_t] = len - n;
        if (p.r->rec[p.rec_t] == 0) p.rec_t = (p.rec_t + 1) % REC_MAX;
        return n;
    }
    int avail = in_used(p);
    if (avail == 0) {
        if (p.hup) return 0;
        return p.tio.c_cc[VMIN] == 0 ? 0 : -11;
    }
    int n = avail < max ? avail : max;
    for (int i = 0; i < n; i++) { d[i] = p.r->in[p.in_t]; p.in_t = (p.in_t + 1) % IN_SZ; }
    return n;
}

int SlaveRead(int t, uint8_t* d, int max) {
    if (!vt(t)) return 0;
    int r = slave_read_impl(g_pair[t], d, max);
    __atomic_add_fetch(&g_ptydbg[1], 1u, __ATOMIC_RELAXED);
    if (r > 0) __atomic_add_fetch(&g_ptydbg[2], (uint32_t)r, __ATOMIC_RELAXED);
    else if (r == -11) __atomic_add_fetch(&g_ptydbg[3], 1u, __ATOMIC_RELAXED);
    return r;
}

int SlaveWrite(int t, const uint8_t* d, int n) {
    if (!vt(t)) return -5;
    Pair& p = g_pair[t];
    int taken = 0;
    {
        SpinLockGuard g(p.lock);
        if (p.hup) return (p.flags & F_CAPTURE) ? n : -5;       // nobody listens (satoru)
        if (p.flags & F_CAPTURE) {
            // capture: whatever fits; the rest is dropped, a writer is never held up (satoru)
            while (taken < n && out_free(p) >= 1) { out_raw(p, d[taken]); taken++; }
            taken = n;
        } else {
            while (taken < n && out_free(p) >= 2) { out_proc(p, d[taken]); taken++; }
        }
        uint32_t used = (uint32_t)out_used(p);
        if (used > g_ptydbg[7]) g_ptydbg[7] = used;
    }
    if (taken > 0) { LinuxSyscall::PtyWake(t, 1); if (!(p.flags & F_CAPTURE)) post_dirty(); }
    __atomic_add_fetch(&g_ptydbg[4], 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_ptydbg[5], (uint32_t)taken, __ATOMIC_RELAXED);
    if (taken == 0 && n > 0) __atomic_add_fetch(&g_ptydbg[6], 1u, __ATOMIC_RELAXED);
    return taken > 0 ? taken : (n > 0 ? -11 : 0);
}

bool SlaveReadable(int t) {
    if (!vt(t)) return true;
    Pair& p = g_pair[t];
    SpinLockGuard g(p.lock);
    if (p.hup) return true;
    if (p.tio.c_lflag & L_ICANON) return rec_used(p) > 0;
    return in_used(p) > 0 || p.tio.c_cc[VMIN] == 0;
}

bool SlaveWritable(int t) {
    if (!vt(t)) return true;
    Pair& p = g_pair[t];
    SpinLockGuard g(p.lock);
    return p.hup || (p.flags & F_CAPTURE) || out_free(p) >= 2;
}

int SlaveAvail(int t) {
    if (!vt(t)) return 0;
    Pair& p = g_pair[t];
    SpinLockGuard g(p.lock);
    if (p.tio.c_lflag & L_ICANON) return rec_used(p) > 0 ? p.r->rec[p.rec_t] : 0;
    return in_used(p);
}

void GetTermios(int t, KTermios* tio) {
    if (!vt(t)) { DefaultTermios(tio); return; }
    SpinLockGuard g(g_pair[t].lock); *tio = g_pair[t].tio;
}

void SetTermios(int t, const KTermios* tio, int how) {
    if (!vt(t)) return;
    Pair& p = g_pair[t];
    bool wake = false;
    {
        SpinLockGuard g(p.lock);
        bool was_canon = (p.tio.c_lflag & L_ICANON) != 0;
        p.tio = *tio;
        if (how == 2) flush_input_locked(p);
        bool is_canon = (p.tio.c_lflag & L_ICANON) != 0;
        if (was_canon && !is_canon) {
            // the half-edited line becomes plain readable bytes (satoru)
            for (int i = 0; i < p.line_n; i++) in_push(p, p.r->line[i]);
            p.line_n = 0;
            p.rec_h = p.rec_t = 0;
            wake = in_used(p) > 0;
        } else if (!was_canon && is_canon) {
            int u = in_used(p);
            if (u > 0) { p.r->rec[p.rec_h] = u; p.rec_h = (p.rec_h + 1) % REC_MAX; wake = true; }
        }
    }
    if (wake) LinuxSyscall::PtyWake(t, 0);
}

void GetWinsize(int t, KWinsize* w) {
    if (!vt(t)) { w->ws_row = 25; w->ws_col = 80; w->ws_xpixel = w->ws_ypixel = 0; return; }
    SpinLockGuard g(g_pair[t].lock); *w = g_pair[t].ws;
}

void SetWinsize(int t, const KWinsize* w) {
    if (!vt(t)) return;
    Pair& p = g_pair[t];
    int pg = 0;
    {
        SpinLockGuard g(p.lock);
        if (p.ws.ws_col == w->ws_col && p.ws.ws_row == w->ws_row) { p.ws = *w; return; }
        p.ws = *w;
        pg = p.fg_pgrp;
    }
    if (pg > 0) LinuxSyscall::SignalGroup(pg, 28);
}

void FlushInput(int t) { if (!vt(t)) return; SpinLockGuard g(g_pair[t].lock); flush_input_locked(g_pair[t]); }
void FlushOutput(int t) { if (!vt(t)) return; SpinLockGuard g(g_pair[t].lock); g_pair[t].out_h = g_pair[t].out_t = 0; }

// ---- the legacy single session: pair 0 ----

static bool g_legacy_active = false;
bool Begin(int cols, int rows) {
    Init();
    if (!vt(LEGACY)) return false;
    SpinLockGuard g(g_pair[LEGACY].lock);
    reset_locked(g_pair[LEGACY], cols, rows);
    g_pair[LEGACY].hup = false;
    __atomic_store_n(&g_legacy_active, true, __ATOMIC_RELEASE);
    return true;
}
void End() {
    if (!vt(LEGACY)) return;
    SpinLockGuard g(g_pair[LEGACY].lock);
    g_pair[LEGACY].hup = true;
    g_pair[LEGACY].fg_pgrp = 0;
    __atomic_store_n(&g_legacy_active, false, __ATOMIC_RELEASE);
}
// a session is active between Begin and End (a hangup keeps it active, as before) (satoru)
bool Active() { return __atomic_load_n(&g_legacy_active, __ATOMIC_ACQUIRE); }
bool Hungup() { return Hungup(LEGACY); }
void Hangup() {
    if (!vt(LEGACY)) return;
    int pg;
    {
        SpinLockGuard g(g_pair[LEGACY].lock);
        g_pair[LEGACY].hup = true;
        pg = g_pair[LEGACY].fg_pgrp;
    }
    if (pg > 0) LinuxSyscall::SignalGroup(pg, 1);   // SIGHUP (satoru)
}
void MasterWrite(const uint8_t* d, int n) { MasterWrite(LEGACY, d, n); }
int  MasterRead(uint8_t* d, int max) { return MasterRead(LEGACY, d, max); }
bool MasterHasData() { return MasterHasData(LEGACY); }
void Resize(int cols, int rows) { Resize(LEGACY, cols, rows); }
int  SlaveRead(uint8_t* d, int max) { return SlaveRead(LEGACY, d, max); }
int  SlaveWrite(const uint8_t* d, int n) {
    int r = SlaveWrite(LEGACY, d, n);
    return r == -5 ? n : r;              // the old console discarded when nobody listened (satoru)
}
bool SlaveReadable() { return SlaveReadable(LEGACY); }
int  SlaveAvail() { return SlaveAvail(LEGACY); }
void GetTermios(KTermios* tio) { GetTermios(LEGACY, tio); }
void SetTermios(const KTermios* tio, int how) { SetTermios(LEGACY, tio, how); }
void GetWinsize(KWinsize* w) { GetWinsize(LEGACY, w); }
void SetWinsize(const KWinsize* w) { SetWinsize(LEGACY, w); }
int  ForegroundPgrp() { return ForegroundPgrp(LEGACY); }
void SetForegroundPgrp(int pg) { SetForegroundPgrp(LEGACY, pg); }
void NoteSlaveUser(int pgid) {
    int z = 0;
    if (pgid > 0 && vt(LEGACY)) __atomic_compare_exchange_n(&g_pair[LEGACY].fg_pgrp, &z, pgid, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
void FlushInput() { FlushInput(LEGACY); }

}   // namespace Kpty

// end (satoru)
