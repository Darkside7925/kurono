#pragma once
//  kurono os - single-session pseudo terminal (satoru)
//
//  the in-kernel linux layer gives every ring-3 process the same three console fds
//  (LFD_CONSOLE). a kpty session turns that console into the slave side of a real
//  tty: a termios line discipline (canonical + raw, echo, isig, icrnl, onlcr), a
//  window size, a foreground process group and blocking reads. the master side is
//  owned by the desktop terminal: it writes encoded key bytes with MasterWrite and
//  drains program output with MasterRead. one session at a time (the terminal that
//  launched the program). with no session active the console keeps its old
//  capture-and-return behaviour. (satoru)

#include "../kernel/types.h"

struct KTermios {                 // the linux x86_64 kernel `struct termios` (36 bytes) (satoru)
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct KWinsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

namespace Kpty {
    // session lifetime (terminal side). Begin resets termios to the cooked default. (satoru)
    bool Begin(int cols, int rows);
    void End();                       // program finished: slave side detaches, output stays drainable
    bool Active();

    // master side
    void MasterWrite(const uint8_t* d, int n);   // keystrokes / pasted text
    int  MasterRead(uint8_t* d, int max);        // program output (and echo)
    bool MasterHasData();
    void Resize(int cols, int rows);             // updates winsize and raises SIGWINCH
    void Hangup();                               // terminal went away: slave reads see EOF

    // slave side (console fd hooks in linux_syscall.cpp)
    int  SlaveRead(uint8_t* d, int max);         // >0 bytes, 0 eof, -11 would block
    int  SlaveWrite(const uint8_t* d, int n);    // bytes taken, -11 when the ring is full
    bool SlaveReadable();
    int  SlaveAvail();
    bool Hungup();

    void GetTermios(KTermios* t);
    void SetTermios(const KTermios* t, int how); // how: 0 now, 1 drain, 2 flush input
    void GetWinsize(KWinsize* w);
    void SetWinsize(const KWinsize* w);
    int  ForegroundPgrp();
    void SetForegroundPgrp(int pg);
    void NoteSlaveUser(int pgid);                // first process to touch the tty becomes foreground
    void FlushInput();
    void DefaultTermios(KTermios* t);            // used for TCGETS when no session is active
}

// end (satoru)
