#pragma once
//  kurono os - pseudo terminal table (satoru)
//
//  a table of pty pairs, the devpts model: every pair has its own output and input
//  rings, termios, window size, controlling session, foreground process group,
//  hangup state and its own lock. the master side belongs to whoever created the
//  pair (a Terminal window in the kernel, or a program that opened /dev/ptmx); the
//  slave side is what programs get as fds 0/1/2 (LFD_PTY_SLAVE, /dev/pts/N). the line
//  discipline (canonical + raw, echo, isig, icrnl, onlcr) runs on the master write.
//  signals from the line discipline go to the foreground process group of THAT pair
//  only. pair 0 is the legacy single session of the old console model, kept behind
//  kurono.pty=0 for A/B: it is what the LFD_CONSOLE fds read and write there, and the
//  Begin/End/MasterRead/... functions without a pair argument act on it. (satoru)

#include "../kernel/types.h"

struct KTermios {                 // the linux x86_64 kernel `struct termios` (36 bytes) (satoru)
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct KWinsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

namespace Kpty {
    constexpr int MAX_PAIRS   = 32;
    constexpr int LEGACY      = 0;          // the old single session pair (satoru)

    // pair flags (satoru)
    constexpr int F_VISIBLE   = 1;          // shows up as /dev/pts/N (satoru)
    constexpr int F_CAPTURE   = 2;          // kernel capture: raw output, never blocks the writer (satoru)
    constexpr int F_LOCKED    = 4;          // ptmx pair before unlockpt (TIOCSPTLCK) (satoru)
    constexpr int F_USERMASTER= 8;          // the master is a user ptmx fd, not the kernel (satoru)

    // table ------------------------------------------------------------------
    void Init();
    int  Alloc(int cols, int rows, int flags);   // -1 when the table is full (satoru)
    void Free(int t);                            // the kernel master lets go; frees once no fd uses it (satoru)
    bool Valid(int t);
    bool Visible(int t);                         // openable as /dev/pts/N (satoru)
    bool Locked(int t);
    bool IsCapture(int t);
    void SetLocked(int t, bool on);
    void Reset(int t, int cols, int rows);       // fresh termios, empty rings, no session (satoru)
    int  CountAlive();
    void SlaveRef(int t, int delta);             // open slave fds (satoru)
    void MasterRef(int t, int delta);            // open user master fds (satoru)
    int  SlaveRefs(int t);
    bool SlaveWasOpened(int t);

    // master side ------------------------------------------------------------
    void MasterWrite(int t, const uint8_t* d, int n);   // keystrokes / pasted text (satoru)
    int  MasterRead(int t, uint8_t* d, int max);        // program output (and echo) (satoru)
    bool MasterHasData(int t);
    void Resize(int t, int cols, int rows);             // updates winsize and raises SIGWINCH (satoru)
    void Hangup(int t);                                 // terminal went away (satoru)
    bool Hungup(int t);
    int  MasterAvail(int t);

    // slave side -------------------------------------------------------------
    int  SlaveRead(int t, uint8_t* d, int max);         // >0 bytes, 0 eof, -11 would block (satoru)
    int  SlaveWrite(int t, const uint8_t* d, int n);    // bytes taken, -11 ring full, -5 hung up (satoru)
    bool SlaveReadable(int t);
    bool SlaveWritable(int t);
    bool MasterReadable(int t);                         // output waiting or the slave side closed (satoru)
    bool MasterSlaveGone(int t);
    int  SlaveAvail(int t);

    void GetTermios(int t, KTermios* tio);
    void SetTermios(int t, const KTermios* tio, int how); // how: 0 now, 1 drain, 2 flush input (satoru)
    void GetWinsize(int t, KWinsize* w);
    void SetWinsize(int t, const KWinsize* w);
    void FlushInput(int t);
    void FlushOutput(int t);
    void DefaultTermios(KTermios* tio);

    // controlling session / foreground group ---------------------------------
    int  ForegroundPgrp(int t);
    void SetForegroundPgrp(int t, int pg);
    int  SessionOf(int t);                       // controlling session id, 0 = none (satoru)
    void SetSession(int t, int sid, int pgrp);   // make t the controlling tty of session sid (satoru)
    void ClearSession(int t);                    // disassociate (satoru)
    int  FindBySession(int sid);                 // the pair controlling session sid, -1 none (satoru)

    // legacy single session (pair 0): the old console model ------------------
    bool Begin(int cols, int rows);
    void End();
    bool Active();
    void MasterWrite(const uint8_t* d, int n);
    int  MasterRead(uint8_t* d, int max);
    bool MasterHasData();
    void Resize(int cols, int rows);
    void Hangup();
    bool Hungup();
    int  SlaveRead(uint8_t* d, int max);
    int  SlaveWrite(const uint8_t* d, int n);
    bool SlaveReadable();
    int  SlaveAvail();
    void GetTermios(KTermios* tio);
    void SetTermios(const KTermios* tio, int how);
    void GetWinsize(KWinsize* w);
    void SetWinsize(const KWinsize* w);
    int  ForegroundPgrp();
    void SetForegroundPgrp(int pg);
    void NoteSlaveUser(int pgid);
    void FlushInput();
}

// end (satoru)
