#include "kpty.h"
#include "linux_syscall.h"
#include "../proc/spinlock.h"
#include "../drivers/graphics.h"

// see kpty.h. every field below is guarded by g_lock (irq-save, leaf lock: nothing
// that can block or allocate runs under it). the signals are posted after the
// lock is dropped. (satoru)

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

    Spinlock g_lock;
    bool     g_active = false;
    bool     g_hup = false;
    KTermios g_tio;
    KWinsize g_ws = { 25, 80, 0, 0 };
    int      g_fg_pgrp = 0;

    uint8_t  g_out[OUT_SZ];
    int      g_out_h = 0, g_out_t = 0;      // head = write, tail = read (satoru)

    uint8_t  g_in[IN_SZ];
    int      g_in_h = 0, g_in_t = 0;
    int      g_rec[REC_MAX];               // byte length of each committed canonical record (satoru)
    int      g_rec_h = 0, g_rec_t = 0;
    uint8_t  g_line[LINE_MAX_];
    int      g_line_n = 0;

    inline int out_used() { return (g_out_h - g_out_t + OUT_SZ) % OUT_SZ; }
    inline int out_free() { return OUT_SZ - 1 - out_used(); }
    inline int in_used()  { return (g_in_h - g_in_t + IN_SZ) % IN_SZ; }
    inline int in_free()  { return IN_SZ - 1 - in_used(); }
    inline int rec_used() { return (g_rec_h - g_rec_t + REC_MAX) % REC_MAX; }

    inline void out_raw(uint8_t c) {
        if (out_free() <= 0) return;
        g_out[g_out_h] = c; g_out_h = (g_out_h + 1) % OUT_SZ;
    }
    // output processing (opost/onlcr/ocrnl) then into the ring. caller holds the lock. (satoru)
    inline void out_proc(uint8_t c) {
        if (g_tio.c_oflag & O_OPOST) {
            if (c == '\n' && (g_tio.c_oflag & O_ONLCR)) { out_raw('\r'); out_raw('\n'); return; }
            if (c == '\r' && (g_tio.c_oflag & O_OCRNL)) { out_raw('\n'); return; }
        }
        out_raw(c);
    }
    inline void in_push(uint8_t c) {
        if (in_free() <= 0) return;
        g_in[g_in_h] = c; g_in_h = (g_in_h + 1) % IN_SZ;
    }
    inline void echo_char(uint8_t c) {
        if (c == '\n') { out_proc('\n'); return; }
        if (c == '\t') { out_raw('\t'); return; }
        if (c < 0x20 || c == 0x7F) {
            if (g_tio.c_lflag & L_ECHOCTL) { out_raw('^'); out_raw(c == 0x7F ? '?' : (uint8_t)(c ^ 0x40)); }
            return;
        }
        out_raw(c);
    }
    // erase the last character (a whole utf-8 sequence) of the canonical line. (satoru)
    inline void line_erase_one() {
        if (g_line_n <= 0) return;
        g_line_n--;
        while (g_line_n > 0 && (g_line[g_line_n] & 0xC0) == 0x80) g_line_n--;
        if (g_tio.c_lflag & L_ECHO) {
            if (g_tio.c_lflag & L_ECHOE) { out_raw('\b'); out_raw(' '); out_raw('\b'); }
        }
    }
    // move the edited line into the readable ring as one record. (satoru)
    inline void line_commit(bool add_nl, bool eof) {
        int n = g_line_n + (add_nl ? 1 : 0);
        if (rec_used() >= REC_MAX - 1) { g_line_n = 0; return; }
        if (in_free() < n) { g_line_n = 0; return; }
        for (int i = 0; i < g_line_n; i++) in_push(g_line[i]);
        if (add_nl) in_push('\n');
        g_line_n = 0;
        (void)eof;
        g_rec[g_rec_h] = n; g_rec_h = (g_rec_h + 1) % REC_MAX;
    }
    inline void flush_input_locked() {
        g_in_h = g_in_t = 0; g_rec_h = g_rec_t = 0; g_line_n = 0;
    }
    inline void post_dirty() { Graphics::MarkUIDirty(); }
}

namespace Kpty {

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

bool Begin(int cols, int rows) {
    if (cols < 10) cols = 10; if (cols > 500) cols = 500;
    if (rows < 2) rows = 2;   if (rows > 300) rows = 300;
    SpinLockGuard g(g_lock);
    DefaultTermios(&g_tio);
    g_ws.ws_col = (uint16_t)cols; g_ws.ws_row = (uint16_t)rows; g_ws.ws_xpixel = 0; g_ws.ws_ypixel = 0;
    g_out_h = g_out_t = 0;
    flush_input_locked();
    g_fg_pgrp = 0;
    g_hup = false;
    g_active = true;
    return true;
}

void End() {
    SpinLockGuard g(g_lock);
    g_active = false;
    g_hup = true;
}

bool Active() { return __atomic_load_n(&g_active, __ATOMIC_ACQUIRE); }
bool Hungup() { return __atomic_load_n(&g_hup, __ATOMIC_ACQUIRE); }

void Hangup() {
    int pg;
    {
        SpinLockGuard g(g_lock);
        g_hup = true;
        pg = g_fg_pgrp;
    }
    if (pg > 0) LinuxSyscall::SignalGroup(pg, 1);   // SIGHUP (satoru)
}

bool MasterHasData() {
    return __atomic_load_n(&g_out_h, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_out_t, __ATOMIC_ACQUIRE);
}

int MasterRead(uint8_t* d, int max) {
    SpinLockGuard g(g_lock);
    int n = 0;
    while (n < max && g_out_t != g_out_h) {
        d[n++] = g_out[g_out_t];
        g_out_t = (g_out_t + 1) % OUT_SZ;
    }
    return n;
}

void Resize(int cols, int rows) {
    if (cols < 10) cols = 10; if (cols > 500) cols = 500;
    if (rows < 2) rows = 2;   if (rows > 300) rows = 300;
    int pg = 0;
    {
        SpinLockGuard g(g_lock);
        if (g_ws.ws_col == cols && g_ws.ws_row == rows) return;
        g_ws.ws_col = (uint16_t)cols; g_ws.ws_row = (uint16_t)rows;
        if (g_active) pg = g_fg_pgrp;
    }
    if (pg > 0) LinuxSyscall::SignalGroup(pg, 28);   // SIGWINCH (satoru)
}

// the line discipline. one byte from the terminal; signals are collected and raised
// after the lock is released. (satoru)
void MasterWrite(const uint8_t* d, int n) {
    int sig = 0, pg = 0;
    {
        SpinLockGuard g(g_lock);
        if (!g_active || g_hup) return;
        const uint32_t lf = g_tio.c_lflag;
        const bool canon = (lf & L_ICANON) != 0;
        for (int i = 0; i < n; i++) {
            uint8_t c = d[i];
            if (g_tio.c_iflag & I_ISTRIP) c &= 0x7F;
            if (c == '\r') {
                if (g_tio.c_iflag & I_IGNCR) continue;
                if (g_tio.c_iflag & I_ICRNL) c = '\n';
            } else if (c == '\n' && (g_tio.c_iflag & I_INLCR)) {
                c = '\r';
            }
            if (lf & L_ISIG) {
                int s = 0;
                if (c == g_tio.c_cc[VINTR]) s = 2;           // SIGINT (satoru)
                else if (c == g_tio.c_cc[VQUIT]) s = 3;      // SIGQUIT (satoru)
                if (s) {
                    if (!(lf & L_NOFLSH)) flush_input_locked();
                    if (lf & L_ECHO) echo_char(c);
                    sig = s; pg = g_fg_pgrp;
                    continue;
                }
                if (c == g_tio.c_cc[VSUSP]) continue;        // job control stop is not supported (satoru)
            }
            if (canon) {
                if (c == g_tio.c_cc[VERASE] && c != 0) { line_erase_one(); continue; }
                if (c == g_tio.c_cc[VKILL] && c != 0) {
                    if ((lf & L_ECHO) && (lf & L_ECHOKE)) { while (g_line_n > 0) line_erase_one(); }
                    else { g_line_n = 0; if ((lf & L_ECHO) && (lf & L_ECHOK)) out_proc('\n'); }
                    continue;
                }
                if (c == g_tio.c_cc[VWERASE] && c != 0) {
                    while (g_line_n > 0 && g_line[g_line_n - 1] == ' ') line_erase_one();
                    while (g_line_n > 0 && g_line[g_line_n - 1] != ' ') line_erase_one();
                    continue;
                }
                if (c == g_tio.c_cc[VEOF] && c != 0) {
                    if (g_line_n == 0) {                     // eof on an empty line = a zero length record (satoru)
                        if (rec_used() < REC_MAX - 1) { g_rec[g_rec_h] = 0; g_rec_h = (g_rec_h + 1) % REC_MAX; }
                    } else {
                        line_commit(false, true);
                    }
                    continue;
                }
                if (c == '\n' || (c == g_tio.c_cc[VEOL] && c != 0)) {
                    if (lf & (L_ECHO | L_ECHONL)) out_proc('\n');
                    line_commit(true, false);
                    continue;
                }
                if (g_line_n < LINE_MAX_ - 1) {
                    g_line[g_line_n++] = c;
                    if (lf & L_ECHO) echo_char(c);
                }
            } else {
                in_push(c);
                if (lf & L_ECHO) echo_char(c);
            }
        }
    }
    if (sig && pg > 0) LinuxSyscall::SignalGroup(pg, sig);
    post_dirty();
}

int SlaveRead(uint8_t* d, int max) {
    SpinLockGuard g(g_lock);
    if (max <= 0) return 0;
    if (g_tio.c_lflag & L_ICANON) {
        if (rec_used() == 0) return g_hup ? 0 : -11;
        int len = g_rec[g_rec_t];
        if (len == 0) { g_rec_t = (g_rec_t + 1) % REC_MAX; return 0; }   // eof (satoru)
        int n = len < max ? len : max;
        for (int i = 0; i < n; i++) { d[i] = g_in[g_in_t]; g_in_t = (g_in_t + 1) % IN_SZ; }
        g_rec[g_rec_t] = len - n;
        if (g_rec[g_rec_t] == 0) g_rec_t = (g_rec_t + 1) % REC_MAX;
        return n;
    }
    int avail = in_used();
    if (avail == 0) {
        if (g_hup) return 0;
        return g_tio.c_cc[VMIN] == 0 ? 0 : -11;
    }
    int n = avail < max ? avail : max;
    for (int i = 0; i < n; i++) { d[i] = g_in[g_in_t]; g_in_t = (g_in_t + 1) % IN_SZ; }
    return n;
}

int SlaveWrite(const uint8_t* d, int n) {
    int taken = 0;
    {
        SpinLockGuard g(g_lock);
        if (g_hup) return n;                      // nobody is listening: discard (satoru)
        while (taken < n && out_free() >= 2) { out_proc(d[taken]); taken++; }
    }
    if (taken > 0) post_dirty();
    return taken > 0 ? taken : (n > 0 ? -11 : 0);
}

bool SlaveReadable() {
    SpinLockGuard g(g_lock);
    if (g_hup) return true;
    if (g_tio.c_lflag & L_ICANON) return rec_used() > 0;
    return in_used() > 0 || g_tio.c_cc[VMIN] == 0;
}

int SlaveAvail() {
    SpinLockGuard g(g_lock);
    if (g_tio.c_lflag & L_ICANON) return rec_used() > 0 ? g_rec[g_rec_t] : 0;
    return in_used();
}

void GetTermios(KTermios* t) { SpinLockGuard g(g_lock); *t = g_tio; }

void SetTermios(const KTermios* t, int how) {
    SpinLockGuard g(g_lock);
    bool was_canon = (g_tio.c_lflag & L_ICANON) != 0;
    g_tio = *t;
    if (how == 2) flush_input_locked();
    bool is_canon = (g_tio.c_lflag & L_ICANON) != 0;
    if (was_canon && !is_canon) {
        // the half-edited line becomes plain readable bytes (satoru)
        for (int i = 0; i < g_line_n; i++) in_push(g_line[i]);
        g_line_n = 0;
        g_rec_h = g_rec_t = 0;
    } else if (!was_canon && is_canon) {
        int u = in_used();
        if (u > 0) { g_rec[g_rec_h] = u; g_rec_h = (g_rec_h + 1) % REC_MAX; }
    }
}

void GetWinsize(KWinsize* w) { SpinLockGuard g(g_lock); *w = g_ws; }

void SetWinsize(const KWinsize* w) {
    int pg = 0;
    {
        SpinLockGuard g(g_lock);
        if (g_ws.ws_col == w->ws_col && g_ws.ws_row == w->ws_row) return;
        g_ws = *w;
        if (g_active) pg = g_fg_pgrp;
    }
    if (pg > 0) LinuxSyscall::SignalGroup(pg, 28);
}

int ForegroundPgrp() { return __atomic_load_n(&g_fg_pgrp, __ATOMIC_ACQUIRE); }
void SetForegroundPgrp(int pg) { __atomic_store_n(&g_fg_pgrp, pg, __ATOMIC_RELEASE); }

void NoteSlaveUser(int pgid) {
    int z = 0;
    if (pgid > 0) __atomic_compare_exchange_n(&g_fg_pgrp, &z, pgid, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

void FlushInput() { SpinLockGuard g(g_lock); flush_input_locked(); }

}   // namespace Kpty

// end (satoru)
