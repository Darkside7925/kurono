#ifndef KURONO_SYSTEM_VCONSOLE_H
#define KURONO_SYSTEM_VCONSOLE_H

#include "../kernel/types.h"

// Linux-style virtual consoles.
//
// Pressing Ctrl+Alt+F1..F6 from the GUI session switches the active
// console.  Each console keeps its own scrollback buffer and ANSI cursor
// position; consoles 1..6 are TTYs, console 7 is reserved for the GUI.
//
// Switching is cooperative  -  the keyboard handler calls VConsole::Switch()
// which sets the active index; the next GUI frame reads the active index
// via VConsole::Active() and renders either the framebuffer compositor
// (tty7) or the corresponding text buffer.

namespace VConsole {

    static const int VC_COUNT       = 7;     // tty1..tty7
    static const int VC_GUI         = 6;     // index of GUI console (tty7)
    static const int VC_COLS        = 120;
    static const int VC_ROWS        = 40;
    static const int VC_SCROLLBACK  = 200;

    struct Cell {
        char     ch;
        unsigned char fg;
        unsigned char bg;
    };

    struct Console {
        Cell rows[VC_SCROLLBACK][VC_COLS];   // ring buffer
        int  top;                            // row index of oldest line
        int  bottom;                         // row index past newest line
        int  cur_row;                        // cursor (row offset from top)
        int  cur_col;
        unsigned char fg;
        unsigned char bg;
    };

    void Init(int initial_active = VC_GUI);

    // Active console index (0..VC_COUNT-1).  GUI is VC_GUI.
    int  Active();
    bool Switch(int idx);

    // Append a NUL-terminated string to the active console (or specific).
    void WriteStr(int idx, const char* s);
    void WriteChar(int idx, char c);

    Console* Get(int idx);
}

#endif
