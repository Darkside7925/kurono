#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Terminal Emulator Application
// ═══════════════════════════════════════════════════════════════════════════

#define TERM_COLS      80
#define TERM_ROWS      25
#define TERM_HIST_MAX  256
#define TERM_INPUT_MAX 256
#define TERM_SCROLL_BK 512

struct TermCell {
    char      ch;
    unsigned int fg;
    unsigned int bg;
};

struct TermLine {
    TermCell cells[TERM_COLS];
    int len;
};

class TerminalApp {
public:
    static void Init();
    static int  Open();       // creates & shows a window, returns wid

    // Window callbacks
    static void Render(void* win, int x, int y, int w, int h);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

    // Terminal writing API (used by shell output)
    static void Write(const char* text);
    static void WriteChar(char c);
    static void WriteLn(const char* text);
    static void Clear();
    static void SetColor(unsigned int fg, unsigned int bg);

    // Scroll state (public for WM scroll event forwarding)
    static int buf_count;
    static int scroll_offset;

private:
    static TermLine buffer[TERM_SCROLL_BK];
    static int cursor_row;      // current row in buffer
    static int cursor_col;      // current col in buffer
    static unsigned int cur_fg;
    static unsigned int cur_bg;

    // Input line
    static char input_buf[TERM_INPUT_MAX];
    static int  input_len;
    static int  input_cursor;

    // Command history
    static char history[TERM_HIST_MAX][TERM_INPUT_MAX];
    static int  hist_count;
    static int  hist_pos;

    // Shell integration
    static bool shell_ready;
    static char prompt[64];

    static void ExecuteInput();
    static void HistoryUp();
    static void HistoryDown();
    static void NewLine();
    static void ScrollToBottom();
    static void RenderCell(int sx, int sy, TermCell* cell);
    static void WritePrompt();
};
