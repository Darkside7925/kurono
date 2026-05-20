#include "vconsole.h"
#include "../drivers/serial.h"

namespace {
    VConsole::Console g_consoles[VConsole::VC_COUNT];
    int g_active = VConsole::VC_GUI;        // boot into GUI

    inline void wipe_cell(VConsole::Cell& c) {
        c.ch = ' '; c.fg = 7; c.bg = 0;
    }

    void newline(VConsole::Console& vc) {
        vc.cur_row++;
        vc.cur_col = 0;
        if (vc.cur_row >= VConsole::VC_SCROLLBACK) {
            vc.cur_row = VConsole::VC_SCROLLBACK - 1;
            // shift up by one row
            for (int r = 0; r < VConsole::VC_SCROLLBACK - 1; r++) {
                for (int c = 0; c < VConsole::VC_COLS; c++) {
                    vc.rows[r][c] = vc.rows[r + 1][c];
                }
            }
            for (int c = 0; c < VConsole::VC_COLS; c++) wipe_cell(vc.rows[vc.cur_row][c]);
        }
    }
}

namespace VConsole {

void Init(int initial_active) {
    for (int i = 0; i < VC_COUNT; i++) {
        Console& vc = g_consoles[i];
        for (int r = 0; r < VC_SCROLLBACK; r++)
            for (int c = 0; c < VC_COLS; c++) wipe_cell(vc.rows[r][c]);
        vc.top = 0; vc.bottom = 0;
        vc.cur_row = 0; vc.cur_col = 0;
        vc.fg = 7; vc.bg = 0;
    }
    if (initial_active < 0 || initial_active >= VC_COUNT) initial_active = VC_GUI;
    g_active = initial_active;

    // Welcome banner on each tty.
    const char* greet[] = {
        "Kurono OS tty1  -  System log\r\n",
        "Kurono OS tty2  -  Secondary console\r\n",
        "Kurono OS tty3  -  Free console\r\n",
        "Kurono OS tty4  -  Free console\r\n",
        "Kurono OS tty5  -  Free console\r\n",
        "Kurono OS tty6  -  Free console\r\n",
        "Kurono OS tty7  -  Graphical session (Ctrl+Alt+F1..F6 to switch)\r\n",
    };
    for (int i = 0; i < VC_COUNT; i++) WriteStr(i, greet[i]);
    if (g_active == VC_GUI) {
        SerialLogger::Log("VConsole: 7 virtual consoles ready (active=tty7/GUI)\r\n");
    } else {
        char log[48] = "VConsole: 7 virtual consoles ready (active=tty";
        int li = 0;
        while (log[li]) li++;
        log[li++] = (char)('1' + g_active);
        log[li++] = ')';
        log[li++] = '\r';
        log[li++] = '\n';
        log[li] = 0;
        SerialLogger::Log(log);
    }
}

int Active() { return g_active; }

bool Switch(int idx) {
    if (idx < 0 || idx >= VC_COUNT) return false;
    if (idx == g_active) return true;
    g_active = idx;
    char log[40] = "VConsole: switched to tty";
    int li = 25;
    log[li++] = (char)('1' + idx);
    log[li++] = '\r'; log[li++] = '\n'; log[li] = 0;
    SerialLogger::Log(log);
    return true;
}

Console* Get(int idx) {
    if (idx < 0 || idx >= VC_COUNT) return nullptr;
    return &g_consoles[idx];
}

void WriteChar(int idx, char c) {
    Console* vc = Get(idx);
    if (!vc) return;
    if (c == '\n') { newline(*vc); return; }
    if (c == '\r') { vc->cur_col = 0; return; }
    if (c == '\t') {
        int next = (vc->cur_col + 8) & ~7;
        while (vc->cur_col < next && vc->cur_col < VC_COLS) {
            vc->rows[vc->cur_row][vc->cur_col].ch = ' ';
            vc->rows[vc->cur_row][vc->cur_col].fg = vc->fg;
            vc->rows[vc->cur_row][vc->cur_col].bg = vc->bg;
            vc->cur_col++;
        }
        return;
    }
    if (c == '\b') {
        if (vc->cur_col > 0) vc->cur_col--;
        vc->rows[vc->cur_row][vc->cur_col].ch = ' ';
        return;
    }
    if (vc->cur_col >= VC_COLS) newline(*vc);
    Cell& cell = vc->rows[vc->cur_row][vc->cur_col];
    cell.ch = c; cell.fg = vc->fg; cell.bg = vc->bg;
    vc->cur_col++;
}

void WriteStr(int idx, const char* s) {
    if (!s) return;
    while (*s) WriteChar(idx, *s++);
}

}  // namespace VConsole
