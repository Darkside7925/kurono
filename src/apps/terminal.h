#pragma once
//  kurono os - terminal emulator v2.0
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

struct TermInst;   // one per Terminal window, defined in terminal.cpp (satoru)

class TerminalApp {
public:
    /** global setup (theme, grid metrics); per-window state is made by Open(). */
    static void Init();
    // kurono.termsh=1: every window starts busybox sh on its pty and reaches kernel built-ins through /dev/kexec (satoru)
    static void SetShellMode(bool on);
    static int  Open();          // ring 3 when kurono.r3apps asks for it, else the kernel build (satoru)
    static int  OpenKernel();    // the in-kernel build only (scripts, fallback) (satoru)
    static void Tick();
    // sched=2: body of terminal worker kernel proc `slot` (0..3): runs one window's
    // queued command, so windows run commands concurrently. (satoru)
    static void WorkerTick(int slot);
    static bool IsBusy();
    /** Programmatic command injection (e.g. GUI autorun): targets the window opened last. */
    static void EnqueueCommand(const char* cmd);

    // window callbacks
    static void Render(void* win, int cx, int cy, int cw, int ch);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

    /** Raw bytes for cooperative shell streaming (no ANSI parse). Goes to the
        window whose command the shell thread is running. */
    static void EmitShellChunk(const char* data, int len);

    // pty mode (satoru): while a ring-3 program (claude, busybox sh) runs from a shell
    // command, the terminal is a vt100 screen wired to a kpty session. PtyBegin is called
    // on the shell thread from inside the command; the window input path feeds the
    // master side and Render drains program output into the emulator.
    static bool PtyBegin();
    static void PtyEnd();
    static bool PtyActive();
    static void PtyKeyEvent(int key, bool shift, bool ctrl, bool alt);
    static void PtyPoll();
    static void PtyScroll(int delta);
    static void PastePrimary();   // middle click: primary selection into the focused terminal (satoru)

private:
    // per-window state lives in a TermInst (see terminal.cpp); the private helpers
    // below take the instance they act on. (satoru)
    static void InitInst(TermInst* S);
    static void ExecuteInput(TermInst* S);
    static void RunQueued(TermInst* S, int worker);   // sched=2 worker or the shell proc (satoru)
    static void HistoryUp(TermInst* S);
    static void HistoryDown(TermInst* S);
    static void NewLine(TermInst* S);
    static void ScrollToBottom(TermInst* S);
    static void RenderCell(int sx, int sy, TermCell* cell);
    static void WritePrompt(TermInst* S);
    static void TabComplete(TermInst* S);
    static void Write(TermInst* S, const char* text);
    static void WriteChar(TermInst* S, char c);
    static void WriteLn(TermInst* S, const char* text);
    static void Clear(TermInst* S);
    static void SetColor(TermInst* S, unsigned int fg, unsigned int bg);
    static void SetBold(TermInst* S, bool b);
};
