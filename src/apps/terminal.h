#pragma once
//  kurono os  -  terminal emulator v2.0
//  improved: tab completion, full ansi colors, blink cursor, more shortcuts

#define TERM_COLS         80
#define TERM_ROWS         25
#define TERM_HIST_MAX     1024
#define TERM_INPUT_MAX    512
#define TERM_SCROLL_BK    2048
#define TERM_TAB_MAX      32
#define SHELL_OUTPUT_BUF  8192

struct TermCell {
    char         ch;
    unsigned int fg;
    unsigned int bg;
    bool         bold;
};

struct TermLine {
    TermCell cells[TERM_COLS];
    int len;
};

class TerminalApp {
public:
    static void Init();
    static int  Open();
    static void Tick();
    static bool IsBusy();
    /** Programmatic command injection (e.g. GUI autorun). */
    static void EnqueueCommand(const char* cmd);

    // window callbacks
    static void Render(void* win, int cx, int cy, int cw, int ch);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

    // terminal writing api
    static void Write(const char* text);
    /** Raw bytes for cooperative shell streaming (no ANSI parse). */
    static void EmitShellChunk(const char* data, int len);
    static void WriteChar(char c);
    static void WriteLn(const char* text);
    static void Clear();
    static void SetColor(unsigned int fg, unsigned int bg);
    static void SetBold(bool b);

    // scroll state (public for wm scroll event forwarding)
    static int buf_count;
    static int scroll_offset;

private:
    static TermLine buffer[TERM_SCROLL_BK];
    static int cursor_row;
    static int cursor_col;
    static unsigned int cur_fg;
    static unsigned int cur_bg;
    static bool cur_bold;

    // input line
    static char input_buf[TERM_INPUT_MAX];
    static int  input_len;
    static int  input_cursor;

    // command history
    static char history[TERM_HIST_MAX][TERM_INPUT_MAX];
    static int  hist_count;
    static int  hist_pos;
    static char hist_saved[TERM_INPUT_MAX]; // saves in-progress input during history nav

    // tab completion
    static char tab_matches[TERM_TAB_MAX][TERM_INPUT_MAX];
    static int  tab_match_count;
    static int  tab_match_idx;
    static char tab_prefix[TERM_INPUT_MAX];

    // shell integration
    static bool shell_ready;
    static char prompt[128];
    static bool command_pending;
    static bool command_running;
    static char pending_cmd[TERM_INPUT_MAX];

    // aesthetic state
    static unsigned int blink_timer; // milliseconds for cursor blink

    static void ExecuteInput();
    static void HistoryUp();
    static void HistoryDown();
    static void NewLine();
    static void ScrollToBottom();
    static void RenderCell(int sx, int sy, TermCell* cell);
    static void WritePrompt();
    static void TabComplete();
    static void HandleAnsiEscape(const char* seq, int len);
    static int  WriteAnsiEscaped(const char* text, int start);
};
