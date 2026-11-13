//  kurono os - terminal emulator v2.0
//  improvements: tab completion, extended ansi, blinking cursor, shortcuts
#include "terminal.h"
#include "../kernel/userspace.h"   // sched=2 terminal workers: cancel probe (satoru)
#include "../proc/scheduler.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../shell/shell.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"
#include "../system/logging.h"
#include "../system/clipboard.h"
#include "../ui/kss.h"
#include "../ui/font.h"
#include "vt.h"
#include "../kui/kui_kernel.h"        // the ring-3 terminal choice (satoru)
#include "../linux/kpty.h"
#include "../linux/linux_syscall.h"   // pty mode, launch tty (satoru)
#include "../proc/kernel_processes.h"   // exec bridge cancel (satoru)
#include "../kernel/heap.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)

// theme chrome colors. seeded from the kss tokens at init so the terminal
// matches the system black/grey palette; the background and primary text
// double as the sentinels the render/escape code compares against (e.g.
// `cell->bg != T_BG`), so they live as mutable globals, not const. (satoru)
static unsigned int T_BG     = 0xFF101012;  // near-black app bg (kss .bg) (satoru)
static unsigned int T_FG     = 0xFFE6E6EA;  // primary text (kss .text) (satoru)
static unsigned int T_CURSOR = 0xFF3D7DFF;  // cursor / caret (kss accent) (satoru)
static unsigned int T_PROMPT = 0xFF3D7DFF;  // prompt accent (kss accent) (satoru)
static unsigned int T_GRAY   = 0xFF8A8A92;  // dim chrome text (kss .text_dim) (satoru)
static unsigned int T_SEL_BG = 0xFF26262B;  // selection / raised bg (kss .sel) (satoru)
// semantic ansi palette - fixed, these are color *meanings* not theme chrome. (satoru)
static const unsigned int T_GREEN    = 0xFF2ECC71;
static const unsigned int T_RED      = 0xFFE74C3C;
static const unsigned int T_YELLOW   = 0xFFF1C40F;
static const unsigned int T_MAGENTA  = 0xFF9B59B6;
static const unsigned int T_CYAN     = 0xFF1ABC9C;
// bright variants
static const unsigned int T_BRED     = 0xFFFF6B6B;
static const unsigned int T_BGREEN   = 0xFF55EFC4;
static const unsigned int T_BYELLOW  = 0xFFFEEA56;
static const unsigned int T_BBLUE    = 0xFF74B9FF;
static const unsigned int T_BMAGENTA = 0xFFA29BFE;
static const unsigned int T_BCYAN    = 0xFF81ECEC;
static const unsigned int T_BWHITE   = 0xFFFFFFFF;

// monospace grid metrics, derived from real font metrics (not a hardcoded
// 8x16). cell_pxh is the glyph height we render at; CELL_W comes from the
// measured advance of "M" at that size, CELL_H from the px height plus a
// little leading so rows don't touch. PAD_X/PAD_Y inset the grid from the
// window edges. recomputed lazily by EnsureMetrics(). (satoru)
static float cell_pxh = 16.0f;
static int   CELL_W   = 8;
static int   CELL_H   = 20;
static const int PAD_X = 8;   // inner left/right padding (px) (satoru)
static const int PAD_Y = 6;   // inner top/bottom padding (px) (satoru)
static bool  metrics_ready = false;
bool g_kbtest = false;   // kurono.kbtest=1: mirror "echo KBT" lines to serial for the keystroke harness (satoru)

// pull theme chrome from kss; safe to call repeatedly. (satoru)
static void TermSyncTheme(){
    const KSS::Theme& t = KSS::T();
    T_BG     = t.bg;
    T_FG     = t.text;
    T_CURSOR = KSS::Accent();
    T_PROMPT = KSS::Accent();
    T_GRAY   = t.text_dim;
    T_SEL_BG = t.sel;
}

// compute the monospace cell from the active font once it's loaded. measuring
// "M" gives the widest typical advance so every glyph fits its cell without
// the old cramped overlap; height = px size + leading. (satoru)
static void EnsureMetrics(){
    // re-measure when the ui style flips between smooth and pixel text.
    static bool metrics_ok = false;
    if(metrics_ready && metrics_ok == FontTTF::ok) return;
    metrics_ok = FontTTF::ok;
    float base_pxh = KSS::BodyPx();  // matches graphics' 16px body base (satoru)
    if(base_pxh < 8.0f) base_pxh = 16.0f;
    cell_pxh = base_pxh;
    int mw = 8;
    if(FontTTF::ok){
        // the scrollback is a fixed TERM_COLS grid that the window shows at 8 px
        // per column, so size the mono face to an 8 px advance. a wider cell
        // left fewer visible columns than the wrap width and clipped the tail
        // of every long line. (satoru)
        for(int k = 0; k < 3; k++){
            int m = FontTTF::MeasureMono(cell_pxh, "M");
            if(m <= 0 || m == 8) break;
            cell_pxh = cell_pxh * 8.0f / (float)m;
        }
    }
    CELL_W = mw;
    CELL_H = (int)(base_pxh * 1.30f + 0.5f);         // ~30% leading (satoru)
    if(CELL_W < 4)  CELL_W = 4;
    if(CELL_H < 10) CELL_H = 10;
    metrics_ready = true;
}

// draw one run of text at the terminal grid size via fontttf, vertically
// centered in the cell. bold = brighten + a 1px overdraw for real weight, so
// bold cells visibly read as bold and not merely lighter. (satoru)
static void TermDrawRun(int sx, int sy, const char* s, unsigned int fg, bool bold){
    if(!s || !s[0]) return;
    unsigned int draw_fg = fg;
    if(bold){
        unsigned int r=(fg>>16)&0xFF, g=(fg>>8)&0xFF, b=fg&0xFF;
        r=r+50>255?255:r+50; g=g+50>255?255:g+50; b=b+50>255?255:b+50;
        draw_fg = 0xFF000000|(r<<16)|(g<<8)|b;
    }
    // baseline offset so glyphs sit centered in CELL_H. (satoru)
    int gy = sy + (CELL_H - (int)cell_pxh) / 2;
    if(gy < sy) gy = sy;
    if(FontTTF::ok){
        // monospace face so every glyph lands on its cell. (satoru)
        FontTTF::DrawMono(sx, gy, cell_pxh, s, draw_fg);
        if(bold) FontTTF::DrawMono(sx+1, gy, cell_pxh, s, draw_fg); // faux-bold (satoru)
    } else {
        // bitmap fallback path (no ttf): route through graphics' string draw. (satoru)
        Graphics::DrawString(sx, gy, s, draw_fg, T_BG);
    }
}

static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s&&d)while(s[i]&&i<mx-1){d[i]=s[i];i++;}if(d)d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static bool sstart(const char* s,const char* p){
    if(!s||!p)return false;
    while(*p){if(*s!=*p)return false;s++;p++;}
    return true;}
static bool seq(const char* a,const char* b){
    if(!a||!b)return false;
    while(*a&&*b){if(*a!=*b)return false;a++;b++;}
    return *a==0&&*b==0;}

// per-window terminal state (satoru)
// every Terminal window owns one TermInst, found by window id in the render and
// input callbacks. instances live in a small fixed pool: each is a heap block
// allocated the first time its slot is used and then reused, never returned to
// the heap, so a late callback can never touch freed memory. closing a window
// just marks its slot free, and the next Open() re-initialises it. (satoru)
#define TERM_MAX_INST 8
struct TermInst {
    TermLine     buffer[TERM_SCROLL_BK];
    int          buf_count;
    int          scroll_offset;
    int          cursor_row;
    int          cursor_col;
    unsigned int cur_fg;
    unsigned int cur_bg;
    bool         cur_bold;

    char         input_buf[TERM_INPUT_MAX];
    int          input_len;
    int          input_cursor;

    char         history[TERM_HIST_MAX][TERM_INPUT_MAX];
    int          hist_count;
    int          hist_pos;
    char         hist_saved[TERM_INPUT_MAX];

    char         tab_matches[TERM_TAB_MAX][TERM_INPUT_MAX];
    int          tab_match_count;
    int          tab_match_idx;
    char         tab_prefix[TERM_INPUT_MAX];

    bool         shell_ready;
    char         prompt[128];
    bool         command_pending;
    bool         command_running;
    char         pending_cmd[TERM_INPUT_MAX];
    unsigned int blink_timer;
    unsigned int last_keypress_ms;
    int          smooth_scroll_q16;
    int          target_scroll_offset;

    int          wid;       // window id this instance draws into (satoru)
    int          in_use;    // slot state, 0 free / 1 live; claimed with cas (satoru)
    volatile int claimed;   // sched=2: a terminal worker owns this window's command (satoru)
    void*        worker;    // sched=2: that worker kernel proc, for ctrl+c (satoru)
    int          pty;       // this window's kpty pair, -1 when none (satoru)
    Vt*          vt;        // emulator for the pair, allocated when the window first shows a program (satoru)
    bool         pty_mode;  // the window draws the emulator instead of its text buffer (satoru)
    int          pty_off;   // lines scrolled back into the emulator history (satoru)
};

static TermInst* g_inst[TERM_MAX_INST];
static TermInst* g_last_opened = nullptr;  // target of EnqueueCommand (satoru)
static TermInst* g_exec_inst = nullptr;    // instance whose command the shell thread is running (satoru)
// sched=2 concurrent terminals: TERM_WORKERS kernel procs each run one window's
// command, so a long command in one window no longer blocks every other
// window. each worker's "exec instance" lives in its slot; exec_inst() answers
// for the calling kernel proc. (satoru)
constexpr int TERM_WORKERS = 4;
static void*     g_worker_kproc[TERM_WORKERS];
static TermInst* g_worker_inst[TERM_WORKERS];
static bool      g_worker_in_exec[TERM_WORKERS];
static volatile bool g_workers_on = false;
static int worker_slot_cur(){
    if(!g_workers_on) return -1;
    void* me=(void*)Scheduler::GetCurrentProcess();
    for(int i=0;i<TERM_WORKERS;i++) if(me && g_worker_kproc[i]==me) return i;
    return -1;
}
static TermInst* exec_inst(){ int w=worker_slot_cur(); return w>=0 ? g_worker_inst[w] : g_exec_inst; }
static bool in_exec_cur();
static bool inst_busy_exec(const TermInst* t){
    if(t==g_exec_inst) return true;
    for(int i=0;i<TERM_WORKERS;i++) if(g_worker_inst[i]==t) return true;
    return false;
}
static bool g_sink_set = false;

// pty mode state (satoru)
// every Terminal window owns one kpty pair (src/linux/kpty.cpp, the devpts model): a
// program launched from the window gets its slave side as fds 0/1/2 and the window draws the
// pair's master output through its own emulator, so any number of windows can run pty
// programs at once. with kurono.pty=0 the old model returns: ONE session (pair 0) shared by
// every program, owned by whichever window ran it first (g_legacy_owner).
static TermInst* g_legacy_owner = nullptr;
static bool g_in_exec = false;      // a shell command is running on behalf of the terminal
static bool in_exec_cur(){ int w=worker_slot_cur(); return w>=0 ? g_worker_in_exec[w] : g_in_exec; }

static bool OwnsPty(const TermInst* S){ return S && S->pty_mode; }
// the pair a window's master side talks to (satoru)
static int PairOf(const TermInst* S){ return LinuxSyscall::PtyLegacy() ? Kpty::LEGACY : S->pty; }

// find the live instance drawing into the window with this id, or null. (satoru)
static TermInst* FindInst(int wid){
    for(int i=0;i<TERM_MAX_INST;i++){
        TermInst* t=g_inst[i];
        if(t && __atomic_load_n(&t->in_use,__ATOMIC_ACQUIRE)==1 && t->wid==wid) return t;
    }
    return nullptr;
}

static TermInst* InstOfWindow(void* win_ptr){
    Window* w=(Window*)win_ptr;
    return w ? FindInst(w->id) : nullptr;
}

// free the slots of closed windows. runs on the gui side only (Open, Render,
// PtyPoll). a slot stays claimed while its shell command still runs or while it
// owns the pty, so the shell thread never loses the instance mid-command. (satoru)
static void ReapClosed(){
    for(int i=0;i<TERM_MAX_INST;i++){
        TermInst* t=g_inst[i];
        if(!t || __atomic_load_n(&t->in_use,__ATOMIC_ACQUIRE)!=1) continue;
        if(t->wid<0) continue;                       // still being opened (satoru)
        if(WindowManager::GetWindow(t->wid)) continue;
        if(t->command_running || inst_busy_exec(t) || t->pty_mode) continue;
        t->command_pending=false;
        if(t->pty>=0){ Kpty::Hangup(t->pty); Kpty::Free(t->pty); t->pty=-1; }   // the window is gone: the pair is released (satoru)
        if(t == g_last_opened) g_last_opened=nullptr;
        __atomic_store_n(&t->in_use,0,__ATOMIC_RELEASE);
    }
}

//  the shell on the window's pty (kurono.termsh=1, sched=2 and the new pty path only):
//  every Terminal window starts a real shell, busybox ash, on its own pty. kernel built-ins
//  (kpkg, firefox, claude, ...) are reached through the exec bridge: /system/bin/<name> is a
//  tiny script (ash runs a non-elf file as a script, ENOEXEC) that writes its command line to
//  /dev/kexec and waits; a launcher kernel proc runs the built-in with that terminal as stdio
//  and streams its text to the pty. busybox applets get a one-line exec stub. (satoru)
static bool g_termsh = false;
void TerminalApp::SetShellMode(bool on){ g_termsh=on; }
static const char* const APPLETS[]={
    "sleep","cat","ls","cp","mv","rm","mkdir","rmdir","ln","touch","chmod","chown","head","tail","wc","sort","uniq",
    "cut","tr","tee","grep","egrep","fgrep","sed","awk","find","xargs","du","df","free","ps","top","kill","killall",
    "id","whoami","uname","hostname","date","env","printenv","dd","od","hexdump","xxd","less","more","vi","stty","tty",
    "clear","reset","seq","yes","true","false","test","expr","basename","dirname","readlink","realpath","md5sum",
    "sha1sum","sha256sum","sha512sum","gzip","gunzip","zcat","tar","unzip","bzip2","xz","wget","nc","ping","ifconfig",
    "ip","route","nslookup","time","timeout","watch","which","uptime","setsid","patch","diff","cmp","strings","nl",
    "fold","rev","tac","paste","shuf","stat","sync","mktemp","mknod","install","dmesg","mount","umount","reboot","poweroff",nullptr};
static bool g_stubs_done=false;
static void RegisterBridgeStubs(){
    if(g_stubs_done) return;
    g_stubs_done=true;
    KVFS::Mkdirs("/system/bin");
    char path[64], body[160];
    // busybox applets first: the real program wins over a kernel built-in of the same name
    for(int i=0;APPLETS[i];i++){
        scpy(path,"/system/bin/",sizeof(path)); sapp(path,APPLETS[i],sizeof(path));
        if(KVFS::Exists(path)) continue;
        scpy(body,"#!/bin/sh\nexec busybox ",sizeof(body)); sapp(body,APPLETS[i],sizeof(body)); sapp(body," \"$@\"\n",sizeof(body));
        KVFS::WriteFile(path,body,(uint32_t)slen(body));
    }
    int n=KuronoShell::GetCommandCount();
    ShellCommand* cmds=KuronoShell::GetCommands();
    int made=0;
    for(int i=0;i<n;i++){
        const char* nm=cmds[i].name;
        if(!nm[0]) continue;
        scpy(path,"/system/bin/",sizeof(path)); sapp(path,nm,sizeof(path));
        if(KVFS::Exists(path)) continue;
        // exec 3<>/dev/kexec; run the built-in; ctrl+c cancels it; wait for it to finish
        scpy(body,"#!/bin/sh\nexec 3<>/dev/kexec\ntrap 'echo !cancel >&3; exit 130' INT\necho \"",sizeof(body));
        sapp(body,nm,sizeof(body)); sapp(body," $*\" >&3\ncat <&3 >/dev/null\n",sizeof(body));
        KVFS::WriteFile(path,body,(uint32_t)slen(body));
        made++;
    }
    SerialLogger::Log("[termsh] bridge stubs written: "); SerialLogger::LogDec(made); SerialLogger::Log("\r\n");
}

static bool g_kbin_done=false;
void TerminalApp::InstallCommandStubs(){
    if(g_kbin_done) return;
    int n=KuronoShell::GetCommandCount();
    ShellCommand* cmds=KuronoShell::GetCommands();
    if(n<=0 || !cmds) return;   // the shell is not up yet: try again on the next open (satoru)
    g_kbin_done=true;
    KVFS::Mkdirs("/system/kbin");
    char path[64], body[200];
    int made=0;
    for(int i=0;i<n;i++){
        const char* nm=cmds[i].name;
        if(!nm[0]) continue;
        bool ok=true;   // a name the shell cannot quote safely is skipped (satoru)
        for(int k=0;nm[k];k++){ char c=nm[k]; if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')) { ok=false; break; } }
        if(!ok) continue;
        scpy(path,"/system/kbin/",sizeof(path)); sapp(path,nm,sizeof(path));
        if(KVFS::Exists(path)) continue;
        scpy(body,"#!/bin/sh\nexec 3<>/dev/kexec\ntrap 'echo !cancel >&3; exit 130' INT\necho \"",sizeof(body));
        // only shell builtins: a stub that forks (cat) failed whenever fork did. (satoru)
        sapp(body,nm,sizeof(body)); sapp(body," $*\" >&3\nwhile read -r _ <&3; do :; done\n",sizeof(body));
        KVFS::WriteFile(path,body,(uint32_t)slen(body));
        KVFS::Chmod(path,0755);
        made++;
    }
    SerialLogger::Log("[terminal] kurono command stubs in /system/kbin: "); SerialLogger::LogDec(made); SerialLogger::Log("\r\n");
}

//  init / open
void TerminalApp::Init(){
    // global setup only: theme chrome + grid metrics from kss before anything
    // draws. the per-window state is set up by InitInst() for each new window,
    // so opening one terminal never resets another. (satoru)
    TermSyncTheme();
    metrics_ready = false;
    EnsureMetrics();
}

void TerminalApp::InitInst(TermInst* S){
    TermSyncTheme();
    EnsureMetrics();
    S->buf_count=0; S->scroll_offset=0;
    S->cursor_row=0; S->cursor_col=0;
    S->cur_fg=T_FG; S->cur_bg=T_BG; S->cur_bold=false;
    S->input_len=0; S->input_cursor=0;
    S->hist_count=0; S->hist_pos=-1; S->hist_saved[0]=0;
    S->tab_match_count=0; S->tab_match_idx=-1; S->tab_prefix[0]=0;
    S->blink_timer=0;
    S->last_keypress_ms=0;
    S->smooth_scroll_q16=0;
    S->target_scroll_offset=0;
    S->shell_ready=true;
    S->command_pending=false;
    S->command_running=false;
    S->pending_cmd[0]=0;

    // clear buffer
    for(int i=0;i<TERM_SCROLL_BK;i++){
        S->buffer[i].len=0;
        for(int j=0;j<TERM_COLS;j++){
            S->buffer[i].cells[j].ch=' ';
            S->buffer[i].cells[j].fg=T_FG;
            S->buffer[i].cells[j].bg=T_BG;
            S->buffer[i].cells[j].bold=false;
        }
    }

    // welcome banner
    SetColor(S, 0xFF2980B9, T_BG);
    WriteLn(S, "╔══════════════════════════════════════════════════════════╗");
    SetColor(S, 0xFF5DADE2, T_BG);
    Write(S, "\u2551  ");
    SetColor(S, 0xFFECF0F1, T_BG);
    Write(S, "Kurono OS");
    SetColor(S, T_FG, T_BG);
    Write(S, " Shell Terminal");
    SetColor(S, T_GRAY, T_BG);
    WriteLn(S, "                         ║");
    SetColor(S, T_GRAY, T_BG);
    WriteLn(S, "║  Powered by KuronoShell - 76+ commands across 3 envs   ║");
    Write(S, "║  Tab=complete  ↑↓=history  Ctrl+L=clear  Ctrl+C=cancel");
    SetColor(S, 0xFF2980B9, T_BG);
    WriteLn(S, "  ║");
    WriteLn(S, "╚══════════════════════════════════════════════════════════╝");
    SetColor(S, T_FG, T_BG);
    WriteLn(S, "");
    SetColor(S, T_GRAY, T_BG);
    WriteLn(S, "Type 'help' for commands. 'linux'/'cmd' to switch env.");
    SetColor(S, T_FG, T_BG);
    WriteLn(S, "");

    WritePrompt(S);
}

// the shell has one global output callback; it routes each chunk to the window
// whose command is running (g_exec_inst), not to whichever window opened last. (satoru)
static void TerminalShellChunkSink(void* /*udata*/, const char* data, int len) {
    TerminalApp::EmitShellChunk(data, len);
    KuronoShell::PumpUI();
}

// wheel scroll for one window: the pty owner scrolls its emulator history, every
// other window eases its own scrollback. (satoru)
static void TermWheel(TermInst* S, int p1);
static void PtyScrollInst(TermInst* S,int delta);

int TerminalApp::Open(){
    // /system/apps/terminal when kurono.r3apps (or ui.conf apps.ring3) asks for it and the
    // ELF is there; the same window callbacks below are the fallback. (satoru)
    InstallCommandStubs();   // before the ring-3 shell looks up a command (satoru)
    if(KuiKernel::LaunchRing3("terminal")) return 0;
    return OpenKernel();
}

int TerminalApp::OpenKernel(){
    TaskMem::AppScope _tms(TaskMem::APP_TERMINAL);   // kurono.taskmem=1: charge to the app (satoru)
    ReapClosed();
    // claim a free slot (allocating its block the first time it is used). (satoru)
    TermInst* S = nullptr;
    for(int i=0;i<TERM_MAX_INST && !S;i++){
        if(!g_inst[i]){
            TermInst* nb=(TermInst*)KernelHeap::Alloc(sizeof(TermInst));
            if(!nb) continue;
            nb->in_use=0; nb->wid=-1; nb->vt=nullptr; nb->pty=-1; nb->pty_mode=false; nb->pty_off=0;
            g_inst[i]=nb;
        }
        int expect=0;
        if(__atomic_compare_exchange_n(&g_inst[i]->in_use,&expect,2,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            S=g_inst[i];
    }
    if(!S){
        SerialLogger::Log("[terminal] no free window slot\r\n");
        return -1;
    }
    S->wid=-1;
    S->claimed=0; S->worker=nullptr;   // heap blocks are not zeroed (reviewer finding) (satoru)
    S->pty_mode=false; S->pty_off=0;
    S->pty = LinuxSyscall::PtyLegacy() ? -1 : Kpty::Alloc(80,25,Kpty::F_VISIBLE);   // this window's own pair (satoru)
    Init();
    InitInst(S);
    RuntimeLog::LogAppEvent("terminal", "open");
    if(!g_sink_set){ KuronoShell::SetOutputChunkCallback(TerminalShellChunkSink, nullptr); g_sink_set=true; }
    // create window via wm
    // 80 columns x 8 px + 2 x PAD_X + the 1 px borders = 658: wide enough for
    // the whole TERM_COLS grid (648 showed 78 and clipped the last two
    // characters of every full line). (satoru)
    int wid = WindowManager::CreateWindow("Terminal", -1, -1, 660, 432,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TerminalApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TerminalApp::Input(w,p1,p2,true,0);
            else if(ev==2) TerminalApp::Input(w,0,0,false,(char)p1);
            else if(ev==3) {
                TermInst* S = InstOfWindow(w);
                if(S) TermWheel(S, p1);
            }
            else if(ev==6) {
                if(p2 && p1==2) TerminalApp::PastePrimary();   // middle click (satoru)
            }
        }
    );
    if(wid < 0){
        __atomic_store_n(&S->in_use,0,__ATOMIC_RELEASE);
        return wid;
    }
    S->wid = wid;
    g_last_opened = S;
    __atomic_store_n(&S->in_use,1,__ATOMIC_RELEASE);
    if(g_termsh && S->pty>=0 && !LinuxSyscall::PtyLegacy() && g_workers_on){
        // a real shell on this window's pty (satoru)
        RegisterBridgeStubs();
        scpy(S->pending_cmd,"busybox sh",TERM_INPUT_MAX);
        S->command_pending=true;
    }
    return wid;
}

static void TermWheel(TermInst* S, int p1){
    if(OwnsPty(S)){ PtyScrollInst(S,p1); return; }
    // scroll event: p1 = scroll delta (positive = scroll up).
    // Stage on target; the next paint eases scroll_offset toward it.
    int max_scroll = S->buf_count > 25 ? S->buf_count - 25 : 0;
    S->target_scroll_offset += (p1 > 0) ? 3 : (p1 < 0 ? -3 : 0);
    if(S->target_scroll_offset < 0)
        S->target_scroll_offset = 0;
    if(S->target_scroll_offset > max_scroll)
        S->target_scroll_offset = max_scroll;
}

//  terminal buffer write
void TerminalApp::NewLine(TermInst* S){
    S->cursor_col=0;
    S->cursor_row++;
    if(S->cursor_row>=TERM_SCROLL_BK) S->cursor_row=TERM_SCROLL_BK-1;
    if(S->cursor_row>=S->buf_count) S->buf_count=S->cursor_row+1;
    // clear the new row (reset bold too, or stale weight leaks on reuse) (satoru)
    if(S->cursor_row<TERM_SCROLL_BK){
        S->buffer[S->cursor_row].len=0;
        for(int j=0;j<TERM_COLS;j++){
            S->buffer[S->cursor_row].cells[j].ch=' ';
            S->buffer[S->cursor_row].cells[j].fg=T_FG;
            S->buffer[S->cursor_row].cells[j].bg=T_BG;
            S->buffer[S->cursor_row].cells[j].bold=false;
        }
    }
}

void TerminalApp::WriteChar(TermInst* S, char c){
    if(c=='\n'){ NewLine(S); return; }
    if(c=='\r'){ S->cursor_col=0; return; }
    if(c=='\t'){
        int spc=4-(S->cursor_col%4);
        for(int i=0;i<spc;i++) WriteChar(S, ' ');
        return;
    }
    if(S->cursor_col>=TERM_COLS) NewLine(S);
    if(S->cursor_row>=TERM_SCROLL_BK) return;

    TermCell* cell=&S->buffer[S->cursor_row].cells[S->cursor_col];
    cell->ch=c;
    cell->fg=S->cur_fg;
    cell->bg=S->cur_bg;
    cell->bold=S->cur_bold;   // record weight so bold cells actually draw bold (satoru)
    S->cursor_col++;
    if(S->cursor_col>S->buffer[S->cursor_row].len)
        S->buffer[S->cursor_row].len=S->cursor_col;
}

void TerminalApp::EmitShellChunk(const char* data, int len) {
    // streamed shell output goes to the window that ran the command. a detached
    // launcher command has no terminal, so its chunks are dropped. (satoru)
    TermInst* S = exec_inst();
    if (!S && data && len > 0) {
        // the exec bridge: a built-in run for a terminal's shell streams into that pty (satoru)
        int bp = LinuxSyscall::BridgeTty();
        if (bp >= 0) Kpty::SlaveWrite(bp, (const uint8_t*)data, len);
        return;
    }
    if (!S || !data || len <= 0) return;
    SetColor(S, T_FG, T_BG);
    for (int i = 0; i < len; i++)
        WriteChar(S, data[i]);
    ScrollToBottom(S);
    // async shell output lands on the Shell thread, not the input path, so the
    // GUI gate would otherwise not see it for up to ~250ms. Mark dirty so the
    // streamed chunk paints on the next frame. (satoru, review fix)
    Graphics::MarkUIDirty();
}

void TerminalApp::Write(TermInst* S, const char* text){
    if(!text)return;
    for(int i=0;text[i];i++){
        // handle escape sequences
        if(text[i]=='\x1b' && text[i+1]=='['){
            // check for clr (clear command)
            if(text[i+2]=='C' && text[i+3]=='L' && text[i+4]=='R'){
                Clear(S);
                WritePrompt(S);
                i+=4; // skip [clr
                continue;
            }
            // parse full ansi escape sequence: \x1b[<params>m
            int j=i+2;
            // collect up to 3 params separated by ';'
            int params[4]={0,0,0,0};
            int pcount=0;
            while(pcount<4){
                int v=0;
                while(text[j]>='0'&&text[j]<='9'){v=v*10+(text[j]-'0');j++;}
                params[pcount++]=v;
                if(text[j]==';'){j++;continue;}
                break;
            }
            if(text[j]=='m'){
                for(int p=0;p<pcount;p++){
                    int code=params[p];
                    switch(code){
                        case 0:  SetColor(S, T_FG,T_BG); S->cur_bold=false; break;
                        case 1:  S->cur_bold=true;  break;
                        case 2:  S->cur_bold=false; break;
                        case 30: S->cur_fg=0xFF2D3436; break;
                        case 31: S->cur_fg=T_RED;    break;
                        case 32: S->cur_fg=T_GREEN;  break;
                        case 33: S->cur_fg=T_YELLOW; break;
                        case 34: S->cur_fg=T_PROMPT; break;
                        case 35: S->cur_fg=T_MAGENTA;break;
                        case 36: S->cur_fg=T_CYAN;   break;
                        case 37: S->cur_fg=0xFFECECEC;break;
                        case 39: S->cur_fg=T_FG;     break;
                        case 90: S->cur_fg=T_GRAY;     break;
                        case 91: S->cur_fg=T_BRED;     break;
                        case 92: S->cur_fg=T_BGREEN;   break;
                        case 93: S->cur_fg=T_BYELLOW;  break;
                        case 94: S->cur_fg=T_BBLUE;    break;
                        case 95: S->cur_fg=T_BMAGENTA; break;
                        case 96: S->cur_fg=T_BCYAN;    break;
                        case 97: S->cur_fg=T_BWHITE;   break;
                        case 40: S->cur_bg=0xFF1A1A2E; break;
                        case 41: S->cur_bg=0xFF8B0000; break;
                        case 42: S->cur_bg=0xFF006400; break;
                        case 43: S->cur_bg=0xFF8B6914; break;
                        case 44: S->cur_bg=0xFF00008B; break;
                        case 45: S->cur_bg=0xFF800080; break;
                        case 46: S->cur_bg=0xFF008B8B; break;
                        case 47: S->cur_bg=0xFF808080; break;
                        case 49: S->cur_bg=T_BG;       break;
                    }
                }
                i=j;
                continue;
            }
            // \x1b[2j = clear screen
            if(text[j]=='J' && params[0]==2){ Clear(S); WritePrompt(S); i=j; continue; }
            // \x1b[k = erase from cursor to end of line (param 0 or absent). (satoru)
            if(text[j]=='K' && params[0]==0){
                if(S->cursor_row>=0 && S->cursor_row<TERM_SCROLL_BK){
                    for(int c=S->cursor_col;c<TERM_COLS;c++){
                        S->buffer[S->cursor_row].cells[c].ch=' ';
                        S->buffer[S->cursor_row].cells[c].fg=S->cur_fg;
                        S->buffer[S->cursor_row].cells[c].bg=S->cur_bg;
                        S->buffer[S->cursor_row].cells[c].bold=false;
                    }
                    if(S->buffer[S->cursor_row].len>S->cursor_col)
                        S->buffer[S->cursor_row].len=S->cursor_col;
                }
                i=j; continue;
            }
            // unknown escape - skip \x1b[
            continue;
        }
        WriteChar(S, text[i]);
    }
}

void TerminalApp::WriteLn(TermInst* S, const char* text){
    Write(S, text);
    NewLine(S);
}

void TerminalApp::Clear(TermInst* S){
    S->buf_count=0; S->cursor_row=0; S->cursor_col=0; S->scroll_offset=0;
    for(int i=0;i<TERM_SCROLL_BK;i++){
        S->buffer[i].len=0;
        for(int j=0;j<TERM_COLS;j++){
            S->buffer[i].cells[j].ch=' ';
            S->buffer[i].cells[j].fg=T_FG;
            S->buffer[i].cells[j].bg=T_BG;
            S->buffer[i].cells[j].bold=false;
        }
    }
}

void TerminalApp::SetColor(TermInst* S, unsigned int fg,unsigned int bg){
    S->cur_fg=fg; S->cur_bg=bg;
}

void TerminalApp::SetBold(TermInst* S, bool b){
    S->cur_bold=b;
}

void TerminalApp::ScrollToBottom(TermInst* S){
    S->scroll_offset = 0;
    S->target_scroll_offset = 0;
    S->smooth_scroll_q16 = 0;
}

void TerminalApp::WritePrompt(TermInst* S){
    // use the kernel shell's prompt (reflects environment switches)
    char shell_prompt[128];
    KuronoShell::GetPrompt(shell_prompt, 128);

    // show environment indicator
    CmdEnvironment env = KuronoShell::GetEnv();
    SetColor(S, T_GRAY, T_BG);
    Write(S, "[");
    switch (env) {
        case ENV_LINUX:
            SetColor(S, T_GREEN, T_BG);
            Write(S, "linux");
            break;
        case ENV_WINDOWS:
            SetColor(S, T_CYAN, T_BG);
            Write(S, "windows");
            break;
        case ENV_DEBIAN:
            SetColor(S, 0xFFD70751, T_BG);  // Debian swirl red
            Write(S, "debian");
            break;
        default:
            SetColor(S, 0xFF3498DB, T_BG);
            Write(S, "kurono");
            break;
    }
    SetColor(S, T_GRAY, T_BG);
    Write(S, "] ");

    // write the shell-generated prompt
    SetColor(S, T_GREEN, T_BG);
    // parse the prompt for user@host:path$ format
    const char* cwd = KVFS::GetCwd();
    if(!cwd || cwd[0]==0) cwd="/";

    const char* user = KuronoShell::GetVar("USER");
    const char* host = KuronoShell::GetVar("HOSTNAME");
    if (!user) user = "user";
    if (!host) host = "kurono";

    Write(S, user);
    SetColor(S, T_GRAY, T_BG);
    Write(S, "@");
    SetColor(S, 0xFF3498DB, T_BG);
    Write(S, host);
    SetColor(S, T_GRAY, T_BG);
    Write(S, ":");
    SetColor(S, T_CYAN, T_BG);
    Write(S, cwd);
    SetColor(S, 0xFF7F8C8D, T_BG);

    // windows vs linux prompt char
    if (env == ENV_WINDOWS)
        Write(S, "> ");
    else
        Write(S, "$ ");

    SetColor(S, T_FG, T_BG);

    // store displayable prompt for scrollback
    scpy(S->prompt, "[", 128);
    sapp(S->prompt, KuronoShell::EnvName(env), 128);
    sapp(S->prompt, "] ", 128);
    sapp(S->prompt, user, 128);
    sapp(S->prompt, "@", 128);
    sapp(S->prompt, host, 128);
    sapp(S->prompt, ":", 128);
    sapp(S->prompt, cwd, 128);
    sapp(S->prompt, env == ENV_WINDOWS ? "> " : "$ ", 128);
}

//  tab completion
void TerminalApp::TabComplete(TermInst* S){
    // find start of the last word in input (after last space before cursor)
    int word_start=0;
    for(int i=0;i<S->input_cursor;i++) if(S->input_buf[i]==' ') word_start=i+1;

    int partial_len=S->input_cursor-word_start;
    if(partial_len<0) partial_len=0;
    char partial[TERM_INPUT_MAX];
    for(int i=0;i<partial_len;i++) partial[i]=S->input_buf[word_start+i];
    partial[partial_len]=0;

    // determine basename insertion point: if partial has '/', we replace
    // only the substring after the last slash so directory prefix is kept.
    int last_slash_in_partial=-1;
    for(int i=0;i<partial_len;i++) if(partial[i]=='/') last_slash_in_partial=i;
    int insert_pos = word_start + (last_slash_in_partial+1);

    // if same prefix: cycle through existing matches
    if(S->tab_match_count>0 && seq(partial, S->tab_prefix)){
        S->tab_match_idx=(S->tab_match_idx+1)%S->tab_match_count;
        const char* m=S->tab_matches[S->tab_match_idx];
        int mlen=slen(m);
        int tail_start=S->input_cursor, tail_len=S->input_len-tail_start;
        char nb[TERM_INPUT_MAX];
        for(int i=0;i<insert_pos;i++) nb[i]=S->input_buf[i];
        for(int i=0;i<mlen&&insert_pos+i<TERM_INPUT_MAX-1;i++) nb[insert_pos+i]=m[i];
        for(int i=0;i<tail_len&&insert_pos+mlen+i<TERM_INPUT_MAX-1;i++)
            nb[insert_pos+mlen+i]=S->input_buf[tail_start+i];
        int nl=insert_pos+mlen+tail_len;
        if(nl>=TERM_INPUT_MAX) nl=TERM_INPUT_MAX-1;
        nb[nl]=0;
        scpy(S->input_buf,nb,TERM_INPUT_MAX);
        S->input_len=nl; S->input_cursor=insert_pos+mlen;
        return;
    }

    // new prefix scan - list kvfs children
    scpy(S->tab_prefix,partial,TERM_INPUT_MAX);
    S->tab_match_count=0; S->tab_match_idx=-1;

    const char* cwd=KVFS::GetCwd();
    char scan_dir[KVFS_MAX_PATH];
    char file_prefix[KVFS_MAX_NAME];

    // find if partial contains '/'
    int last_slash=-1;
    for(int i=0;i<partial_len;i++) if(partial[i]=='/') last_slash=i;

    // first token (the command word) with no path separator also completes
    // against the registered shell command list; argument tokens fall through
    // to kvfs path completion below. (satoru)
    bool is_command_token = (word_start==0 && last_slash<0);
    if(is_command_token){
        ShellCommand* cmds=KuronoShell::GetCommands();
        int ccount=KuronoShell::GetCommandCount();
        for(int i=0;i<ccount&&S->tab_match_count<TERM_TAB_MAX;i++){
            const char* nm=cmds[i].name;
            if(!nm||!nm[0]) continue;
            bool match=true;
            for(int k=0;k<partial_len;k++) if(nm[k]!=partial[k]){match=false;break;}
            if(!match) continue;
            // skip if this command name is already listed (handlers can share
            // a name across environments). (satoru)
            bool dup=false;
            for(int d=0;d<S->tab_match_count;d++) if(seq(S->tab_matches[d],nm)){dup=true;break;}
            if(dup) continue;
            scpy(S->tab_matches[S->tab_match_count],nm,TERM_INPUT_MAX);
            S->tab_match_count++;
        }
    }

    if(last_slash>=0){
        int copy_n = last_slash+1;
        if(copy_n > KVFS_MAX_PATH-1) copy_n = KVFS_MAX_PATH-1;
        for(int i=0;i<copy_n;i++) scan_dir[i]=partial[i];
        scan_dir[copy_n]=0;
        int fp_i = 0;
        for(int i=last_slash+1;i<partial_len && fp_i<(int)sizeof(file_prefix)-1;i++)
            file_prefix[fp_i++]=partial[i];
        file_prefix[fp_i]=0;
        if(scan_dir[0]!='/'){
            char abs[KVFS_MAX_PATH];
            scpy(abs,cwd?cwd:"/",KVFS_MAX_PATH);
            int al=slen(abs);
            if(al>0&&abs[al-1]!='/'){abs[al]='/';abs[al+1]=0;}
            sapp(abs,scan_dir,KVFS_MAX_PATH);
            scpy(scan_dir,abs,KVFS_MAX_PATH);
        }
    } else {
        scpy(scan_dir,cwd?cwd:"/",KVFS_MAX_PATH);
        scpy(file_prefix,partial,KVFS_MAX_NAME);
    }

    KVFSNode* children[KVFS_MAX_CHILDREN];
    int n=KVFS::Listdir(scan_dir,children,KVFS_MAX_CHILDREN);
    int pfx_len=slen(file_prefix);

    for(int i=0;i<n&&S->tab_match_count<TERM_TAB_MAX;i++){
        if(!children[i]) continue;
        const char* nm=children[i]->name;
        bool match=(pfx_len==0);
        if(!match){
            match=true;
            for(int k=0;k<pfx_len;k++) if(nm[k]!=file_prefix[k]){match=false;break;}
        }
        if(match){
            // avoid listing a cwd entry already added from the command list
            // when completing the command token. (satoru)
            bool dup=false;
            if(is_command_token)
                for(int d=0;d<S->tab_match_count;d++) if(seq(S->tab_matches[d],nm)){dup=true;break;}
            if(!dup){
                scpy(S->tab_matches[S->tab_match_count],nm,TERM_INPUT_MAX);
                if(children[i]->is_dir()) sapp(S->tab_matches[S->tab_match_count],"/",TERM_INPUT_MAX);
                S->tab_match_count++;
            }
        }
    }

    if(S->tab_match_count==0) return; // no match, silent

    if(S->tab_match_count==1){
        // auto-insert unique match (preserving directory prefix)
        S->tab_match_idx=0;
        const char* m=S->tab_matches[0];
        int mlen=slen(m);
        int tail_start=S->input_cursor, tail_len=S->input_len-tail_start;
        char nb[TERM_INPUT_MAX];
        for(int i=0;i<insert_pos;i++) nb[i]=S->input_buf[i];
        for(int i=0;i<mlen&&insert_pos+i<TERM_INPUT_MAX-1;i++) nb[insert_pos+i]=m[i];
        for(int i=0;i<tail_len&&insert_pos+mlen+i<TERM_INPUT_MAX-1;i++)
            nb[insert_pos+mlen+i]=S->input_buf[tail_start+i];
        int nl=insert_pos+mlen+tail_len;
        if(nl>=TERM_INPUT_MAX) nl=TERM_INPUT_MAX-1;
        nb[nl]=0;
        scpy(S->input_buf,nb,TERM_INPUT_MAX);
        S->input_len=nl; S->input_cursor=insert_pos+mlen;
        S->tab_match_count=0; // reset so next tab re-scans
    } else {
        // show all matches then cycle on subsequent tabs
        NewLine(S);
        SetColor(S, T_CYAN,T_BG);
        for(int i=0;i<S->tab_match_count;i++){
            Write(S, S->tab_matches[i]);
            Write(S, "  ");
        }
        SetColor(S, T_FG,T_BG);
        NewLine(S);
        WritePrompt(S);
        // redraw currently typed input
        for(int i=0;i<S->input_len;i++) WriteChar(S, S->input_buf[i]);
    }
}

//  command execution
void TerminalApp::ExecuteInput(TermInst* S){
    if(S->command_pending || S->command_running) return;
    if(S->input_len==0){
        NewLine(S);
        WritePrompt(S);
        return;
    }

    // deduplicated history: skip if same as last entry
    bool is_dup = (S->hist_count > 0 && seq(S->history[S->hist_count-1], S->input_buf));
    if(!is_dup && S->hist_count < TERM_HIST_MAX){
        scpy(S->history[S->hist_count], S->input_buf, TERM_INPUT_MAX);
        S->hist_count++;
    }
    S->hist_pos=-1;
    S->hist_saved[0]=0;

    // reset tab state
    S->tab_match_count=0; S->tab_match_idx=-1;

    // echo input already visible; go to new line and queue execution so
    // the desktop loop can keep cycling outside the raw key callback.
    NewLine(S);
    scpy(S->pending_cmd, S->input_buf, TERM_INPUT_MAX);
    S->command_pending = true;
    // test hook: only lines starting "echo KBT" are mirrored to serial so a
    // harness can diff typed vs received bytes; real commands (api keys) never are.
    // off by default - a normal boot never copies typed text to serial or the disk log;
    // boot with kurono.kbtest=1 to enable it. (satoru)
    if(g_kbtest && S->input_len>=8 && S->input_buf[0]=='e' && S->input_buf[1]=='c' && S->input_buf[2]=='h' && S->input_buf[3]=='o' &&
       S->input_buf[4]==' ' && S->input_buf[5]=='K' && S->input_buf[6]=='B' && S->input_buf[7]=='T'){
        SerialLogger::Log("[kbt] "); SerialLogger::Log(S->input_buf); SerialLogger::Log("\r\n");
    }

    // reset input immediately while the command is pending
    S->input_buf[0]=0; S->input_len=0; S->input_cursor=0;
    ScrollToBottom(S);
}

// run S's queued command on the calling kernel proc; worker = its slot, or -1
// for the single shell proc. the output buffer is per caller (static per slot:
// kernel-proc stacks are small). (satoru)
void TerminalApp::RunQueued(TermInst* S, int worker){
    static char s_out[TERM_WORKERS + 1][SHELL_OUTPUT_BUF];
    char* output = s_out[worker >= 0 ? worker : TERM_WORKERS];
    if(worker>=0){ g_worker_inst[worker]=S; S->worker=g_worker_kproc[worker]; }
    else g_exec_inst = S;
    S->command_running = true;
    S->command_pending = false;

    output[0]=0;
    if(worker>=0) g_worker_in_exec[worker]=true; else g_in_exec = true;
    KuronoShell::Execute(S->pending_cmd, output, SHELL_OUTPUT_BUF);
    if(worker>=0) g_worker_in_exec[worker]=false; else g_in_exec = false;
    S->pending_cmd[0]=0;

    bool streamed = KuronoShell::TakeIncrementalOutputUsed();

    if(output[0] && !streamed){
        SetColor(S, T_FG, T_BG);
        Write(S, output);
        if(output[slen(output)-1]!='\n') NewLine(S);
    }

    S->command_running = false;
    ScrollToBottom(S);
    WritePrompt(S);
    if(worker>=0){ g_worker_inst[worker]=nullptr; S->worker=nullptr; __atomic_store_n(&S->claimed,0,__ATOMIC_RELEASE); }
    else g_exec_inst = nullptr;
    // a non-streamed command writes its whole result into the buffer here on
    // the Shell thread (e.g. ls/pwd/cat/help); nothing in the input path saw
    // it, so raise the global signal or the gate delays it up to ~250ms.
    // (satoru, review fix)
    Graphics::MarkUIDirty();
}

void TerminalApp::Tick(){
    TaskMem::AppScope _tms(TaskMem::APP_TERMINAL);   // kurono.taskmem=1: charge to the app (satoru)
    // one shell thread, so one command at a time across all windows: pick the
    // first window with a queued command and remember it as the output target. (satoru)
    if(g_workers_on) return;   // sched=2: the terminal workers run commands (satoru)
    TermInst* S = nullptr;
    for(int i=0;i<TERM_MAX_INST;i++){
        TermInst* t=g_inst[i];
        if(t && __atomic_load_n(&t->in_use,__ATOMIC_ACQUIRE)==1 && t->command_pending && !t->command_running){ S=t; break; }
    }
    if(!S) return;
    RunQueued(S, -1);
}

// only a terminal worker's own command is ever cancelled by a terminal ctrl+c:
// a desktop launcher or the shell proc waiting on a program gets false. (satoru)
static bool term_worker_cancel_probe(){
    if(KernelProcesses::BridgeCancelRequested()) return true;   // ctrl+c in a bridged shell command (satoru)
    if(worker_slot_cur()<0) return false;
    return KuronoShell::IsCommandCancelRequested();
}

// sched=2: one terminal worker kernel proc's loop body. claims the first window
// with a queued command that no other worker owns, and runs it. (satoru)
void TerminalApp::WorkerTick(int slot){
    if(slot<0 || slot>=TERM_WORKERS) return;
    if(!g_worker_kproc[slot]){
        g_worker_kproc[slot]=(void*)Scheduler::GetCurrentProcess();
        KuronoShell::RegisterWorkerContext();
        // any waiter; the probe answers with the calling worker's own flag. (satoru)
        Userspace::SetCancelProbe(nullptr, &term_worker_cancel_probe);
        __atomic_store_n(&g_workers_on, true, __ATOMIC_RELEASE);
    }
    for(int i=0;i<TERM_MAX_INST;i++){
        TermInst* t=g_inst[i];
        if(!t || __atomic_load_n(&t->in_use,__ATOMIC_ACQUIRE)!=1) continue;
        if(!t->command_pending || t->command_running) continue;
        int z=0;
        if(!__atomic_compare_exchange_n(&t->claimed,&z,1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) continue;
        if(!t->command_pending || t->command_running){ __atomic_store_n(&t->claimed,0,__ATOMIC_RELEASE); continue; }
        KuronoShell::ClearCommandCancel();
        // no command text: typed lines can hold secrets (api keys). (satoru)
        SerialLogger::Log("[termw] slot "); SerialLogger::LogDec(slot);
        SerialLogger::Log(" window "); SerialLogger::LogDec(t->wid); SerialLogger::Log(" start\r\n");
        RunQueued(t, slot);
        SerialLogger::Log("[termw] slot "); SerialLogger::LogDec(slot);
        SerialLogger::Log(" window "); SerialLogger::LogDec(t->wid); SerialLogger::Log(" done\r\n");
        return;
    }
}

bool TerminalApp::IsBusy(){
    for(int i=0;i<TERM_MAX_INST;i++){
        TermInst* t=g_inst[i];
        if(t && __atomic_load_n(&t->in_use,__ATOMIC_ACQUIRE)==1 && (t->command_pending || t->command_running)) return true;
    }
    return false;
}

// queues a command on the most recently opened terminal window (callers do
// Open() then EnqueueCommand()). (satoru)
void TerminalApp::EnqueueCommand(const char* cmd){
    TermInst* S = g_last_opened;
    if(!S || __atomic_load_n(&S->in_use,__ATOMIC_ACQUIRE)!=1) return;
    if(!cmd || !cmd[0]) return;
    if(S->command_pending || S->command_running) return;
    int i = 0;
    while(cmd[i] && i < TERM_INPUT_MAX - 1){
        S->pending_cmd[i] = cmd[i];
        i++;
    }
    S->pending_cmd[i] = 0;
    /* echo the command so users can see what's running, then queue it. */
    SetColor(S, T_FG, T_BG);
    Write(S, S->pending_cmd);
    NewLine(S);
    S->command_pending = true;
    ScrollToBottom(S);
}

void TerminalApp::HistoryUp(TermInst* S){
    if(S->hist_count==0) return;
    if(S->hist_pos<0){
        // save current in-progress input before navigating
        scpy(S->hist_saved, S->input_buf, TERM_INPUT_MAX);
        S->hist_pos=S->hist_count-1;
    } else if(S->hist_pos>0) {
        S->hist_pos--;
    }
    scpy(S->input_buf, S->history[S->hist_pos], TERM_INPUT_MAX);
    S->input_len=slen(S->input_buf);
    S->input_cursor=S->input_len;
}

void TerminalApp::HistoryDown(TermInst* S){
    if(S->hist_pos<0) return;
    S->hist_pos++;
    if(S->hist_pos>=S->hist_count){
        S->hist_pos=-1;
        // restore saved in-progress input
        scpy(S->input_buf, S->hist_saved, TERM_INPUT_MAX);
    } else {
        scpy(S->input_buf, S->history[S->hist_pos], TERM_INPUT_MAX);
    }
    S->input_len=slen(S->input_buf);
    S->input_cursor=S->input_len;
}

//  rendering callback
void TerminalApp::RenderCell(int sx,int sy,TermCell* cell){
    EnsureMetrics();
    if(cell->bg!=T_BG){
        Graphics::FillRect(sx,sy,CELL_W,CELL_H,cell->bg);
    }
    if(cell->ch>' '){
        char s[2]={cell->ch,0};
        TermDrawRun(sx,sy,s,cell->fg,cell->bold);
    }
}

static void RenderPty(TermInst* S,void* win_ptr,int cx,int cy,int cw,int ch);

void TerminalApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    ReapClosed();
    TermInst* S = InstOfWindow(win_ptr);
    if(!S) return;
    if(OwnsPty(S)){ RenderPty(S,win_ptr,cx,cy,cw,ch); return; }

    // keep theme + grid metrics current (font may finish loading after init,
    // and the theme can be re-themed live). (satoru)
    TermSyncTheme();
    EnsureMetrics();

    // ease scroll_offset toward target_scroll_offset (~3 frames to settle)
    if(S->scroll_offset != S->target_scroll_offset){
        int diff = S->target_scroll_offset - S->scroll_offset;
        int step = diff / 3;
        if(step == 0) step = (diff > 0) ? 1 : -1;
        S->scroll_offset += step;
        // self-sustain the ease: a single wheel flick marks dirty once, but the
        // animation needs ~3 frames. Request the next frame while still mid-
        // ease so the gate keeps rendering until settled. (satoru, review fix)
        Graphics::MarkUIDirty();
    }

    // fill background
    Graphics::FillRect(cx,cy,cw,ch,T_BG);

    // inset content by a few px so glyphs don't hug the window frame; all grid
    // math below is relative to this padded origin. (satoru)
    int ox = cx + PAD_X;
    int oy = cy + PAD_Y;
    int uw = cw - 2*PAD_X; if(uw < CELL_W) uw = CELL_W;
    int uh = ch - 2*PAD_Y; if(uh < CELL_H) uh = CELL_H;

    int vis_rows = uh / CELL_H;
    int vis_cols = uw / CELL_W;
    if(vis_rows < 1) vis_rows = 1;
    if(vis_cols < 1) vis_cols = 1;
    if(vis_cols>TERM_COLS) vis_cols=TERM_COLS;

    int prompt_end_col = S->cursor_col;
    int input_end_col = prompt_end_col + S->input_len;
    int input_visual_rows = input_end_col / vis_cols;
    int total_lines = S->cursor_row + input_visual_rows + 1;
    if(total_lines < S->buf_count) total_lines = S->buf_count;

    // calculate scroll to show latest lines + wrapped input
    int start_line = total_lines - vis_rows;
    if(start_line<0) start_line=0;
    start_line -= S->scroll_offset;
    if(start_line<0) start_line=0;

    // render buffer lines - batch consecutive same-bg cells into a single FillRect
    int sy=oy;
    char run_buf[TERM_COLS+1];
    for(int row=start_line; row<total_lines && sy+CELL_H<=oy+uh; row++){
        if(row<0||row>=TERM_SCROLL_BK) continue;
        int max_col = S->buffer[row].len;
        if(max_col > vis_cols) max_col = vis_cols;
        // bg pass: batched solid-color background fills
        int col = 0;
        while(col < max_col){
            unsigned int bg = S->buffer[row].cells[col].bg;
            if(bg == T_BG){ col++; continue; }
            int run_start = col;
            while(col < max_col && S->buffer[row].cells[col].bg == bg) col++;
            Graphics::FillRect(ox + run_start*CELL_W, sy,
                               (col - run_start) * CELL_W, CELL_H, bg);
        }
        // fg pass: batched consecutive same-color/bold text
        col = 0;
        while(col < max_col){
            char c0 = S->buffer[row].cells[col].ch;
            if(c0 <= ' '){ col++; continue; }
            unsigned int fg = S->buffer[row].cells[col].fg;
            bool bold = S->buffer[row].cells[col].bold;
            int run_start = col;
            int rb = 0;
            while(col < max_col && rb < TERM_COLS){
                TermCell* c = &S->buffer[row].cells[col];
                if(c->ch <= ' ' || c->fg != fg || c->bold != bold) break;
                run_buf[rb++] = c->ch;
                col++;
            }
            run_buf[rb] = 0;
            if(rb > 0)
                TermDrawRun(ox + run_start*CELL_W, sy, run_buf, fg, bold);
        }
        sy+=CELL_H;
    }

    // render the current input line at the bottom portion
    // first render what's in the current buffer row (prompt text)
    if(S->cursor_row<TERM_SCROLL_BK && S->cursor_row>=start_line){
        int prow = S->cursor_row;
        int draw_y = oy + (prow - start_line) * CELL_H;
        if(draw_y >= oy && draw_y+CELL_H <= oy+uh){
            // already rendered above
        }
    }

    // draw live input after the prompt; batch the longest contiguous run per row
    {
        int i = 0;
        char line_buf[TERM_COLS+1];
        while(i < S->input_len){
            int abs_col = prompt_end_col + i;
            int row_off = abs_col / vis_cols;
            int col = abs_col % vis_cols;
            int dx = ox + col*CELL_W;
            int dy = oy + ((S->cursor_row + row_off) - start_line) * CELL_H;
            int lb = 0;
            // gather until row wraps or input ends
            while(i < S->input_len && col < vis_cols && lb < TERM_COLS){
                line_buf[lb++] = S->input_buf[i++];
                col++;
            }
            line_buf[lb] = 0;
            if(lb > 0 && dy >= oy && dy + CELL_H <= oy + uh)
                TermDrawRun(dx, dy, line_buf, T_FG, false);
        }
    }

    // blinking cursor: pauses (always on) for ~500ms after a keypress, then blinks
    unsigned int now_ms = Timer::GetRealMs();
    unsigned int since_key = now_ms - S->last_keypress_ms;
    bool cursor_visible;
    if(since_key < 500) cursor_visible = true;
    else cursor_visible = (((now_ms - S->last_keypress_ms) / 500) % 2) == 0;
    if(cursor_visible){
        int abs_col = prompt_end_col + S->input_cursor;
        int cur_x = ox + (abs_col % vis_cols) * CELL_W;
        int cur_y = oy + ((S->cursor_row + (abs_col / vis_cols)) - start_line) * CELL_H;
        if(cur_y>=oy && cur_y+CELL_H<=oy+uh){
            // thin 2px wide blinking bar cursor in the accent color (satoru)
            Graphics::FillRect(cur_x, cur_y+2, 2, CELL_H-4, T_CURSOR);
        }
    }

    // The input cursor blinks on a time-driven 500ms cycle. When this terminal
    // is the focused window, ask for a frame at the next blink edge so the blink
    // stays a clean square wave instead of jittering on the 250ms fallback grid.
    // (this used to mark the ui dirty every frame, which kept a focused terminal
    // redrawing the whole desktop at full rate - starving whatever it launched.)
    if (win_ptr && ((Window*)win_ptr)->focused) {
        uint32_t phase = (now_ms - S->last_keypress_ms) % 500;
        Graphics::RequestFrameAt(now_ms + (500 - phase));
    }
}

//  input callback
// terminal char path counters for [hb]: 0 chars in, 1 typed into the line,
// 2 typed ahead while a command ran, 3 enter lost while busy (satoru)
volatile uint32_t g_termdbg[4] = {0};

bool TerminalApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)mx; (void)my; (void)clicked;
    TermInst* S = InstOfWindow(win_ptr);
    if(!S) return false;

    if(key==0) return false;
    __atomic_add_fetch(&g_termdbg[0], 1, __ATOMIC_RELAXED);
    // pty mode: keys arrive as raw key events through PtyKeyEvent (arrows, ctrl, alt),
    // so the char path must not double-send. (satoru)
    if(OwnsPty(S)) return true;
    // the char queue carries plain letters even with ctrl held, so the line editor's
    // ctrl codes (c u a e k l w, ctrl+shift+c/v) never arrived: map ctrl+letter here. (satoru)
    {
        const KeyboardState& ks = Keyboard::GetState();
        if(ks.ctrl && !ks.alt){
            char lc = (key>='A' && key<='Z') ? (char)(key+32) : key;
            if(lc>='a' && lc<='z') key = (char)(lc-'a'+1);
        }
    }
    S->last_keypress_ms = Timer::GetRealMs();
    if(S->command_pending || S->command_running){
        if(key==3){
            // sched=2 workers: cancel only THIS window's command; one not yet
            // claimed by a worker is simply dropped. never the global flag,
            // which other waiters read. (satoru)
            if(g_workers_on){
                if(S->worker) KuronoShell::RequestCommandCancelFor(S->worker);
                else if(S->command_pending && !S->command_running) S->command_pending=false;
            }
            else KuronoShell::RequestCommandCancel();
            SetColor(S, T_RED,T_BG);
            Write(S, "^C");
            NewLine(S);
            SetColor(S, T_FG,T_BG);
            ScrollToBottom(S);
            KuronoShell::PumpUI();
            return true;
        }
        // Type-ahead: buffer printable chars so they appear after the
        // running command finishes and the prompt re-draws.
        if(key>=32 && key<127 && S->input_len<TERM_INPUT_MAX-1){
            for(int i=S->input_len;i>S->input_cursor;i--)
                S->input_buf[i]=S->input_buf[i-1];
            S->input_buf[S->input_cursor]=key;
            S->input_len++;
            S->input_cursor++;
            S->input_buf[S->input_len]=0;
            __atomic_add_fetch(&g_termdbg[2], 1, __ATOMIC_RELAXED);
            return true;
        }
        if(key=='\n' || key=='\r') __atomic_add_fetch(&g_termdbg[3], 1, __ATOMIC_RELAXED);
        if((key==8 || key==127) && S->input_cursor>0){
            for(int i=S->input_cursor-1;i<S->input_len-1;i++)
                S->input_buf[i]=S->input_buf[i+1];
            S->input_len--;
            S->input_cursor--;
            S->input_buf[S->input_len]=0;
            return true;
        }
        return true;
    }

    if(S->scroll_offset < 0) S->scroll_offset = 0;

    // enter
    if(key=='\n' || key=='\r'){
        // write input text into the buffer so it's visible in scrollback
        for(int i=0;i<S->input_len;i++) WriteChar(S, S->input_buf[i]);
        ExecuteInput(S);
        return true;
    }

    // backspace
    if(key==8 || key==127){
        if(S->input_cursor>0){
            // shift left
            for(int i=S->input_cursor-1;i<S->input_len-1;i++)
                S->input_buf[i]=S->input_buf[i+1];
            S->input_len--;
            S->input_cursor--;
            S->input_buf[S->input_len]=0;
        }
        ScrollToBottom(S);
        return true;
    }

    // ctrl+l - clear
    if(key==12){
        Clear(S);
        S->input_buf[0]=0; S->input_len=0; S->input_cursor=0;
        WritePrompt(S);
        return true;
    }

    // ctrl+shift+c - copy the current input line to the clipboard. shift held
    // distinguishes it from the plain ctrl+c cancel below. (satoru)
    if(key==3 && Keyboard::GetState().shift){
        if(S->input_len>0){
            char saved=S->input_buf[S->input_len]; S->input_buf[S->input_len]=0;
            ClipboardManager::SetText(S->input_buf);
            S->input_buf[S->input_len]=saved;
        }
        return true;
    }

    // ctrl+shift+v - paste clipboard text into the input line at the cursor.
    // printable bytes only; a newline ends the paste without submitting. (satoru)
    if(key==22 && Keyboard::GetState().shift){
        if(ClipboardManager::HasText()){
            const char* s=ClipboardManager::GetText();
            for(int i=0;s[i] && s[i]!='\n' && s[i]!='\r';i++){
                char c=s[i];
                if(c<32 || c>=127) continue;
                if(S->input_len>=TERM_INPUT_MAX-1) break;
                for(int j=S->input_len;j>S->input_cursor;j--) S->input_buf[j]=S->input_buf[j-1];
                S->input_buf[S->input_cursor]=c;
                S->input_len++; S->input_cursor++;
                S->input_buf[S->input_len]=0;
            }
            S->tab_match_count=0;
            ScrollToBottom(S);
        }
        return true;
    }

    // ctrl+c - cancel current input
    if(key==3){
        SetColor(S, T_RED, T_BG);
        Write(S, "^C");
        SetColor(S, T_FG, T_BG);
        NewLine(S);
        S->input_buf[0]=0; S->input_len=0; S->input_cursor=0;
        S->tab_match_count=0;
        WritePrompt(S);
        return true;
    }

    // ctrl+a - move to beginning of line
    if(key==1){ S->input_cursor=0; return true; }

    // ctrl+e - move to end of line
    if(key==5){ S->input_cursor=S->input_len; return true; }

    // ctrl+k - kill to end of line
    if(key==11){ S->input_buf[S->input_cursor]=0; S->input_len=S->input_cursor; return true; }

    // ctrl+u - kill to beginning of line
    if(key==21){
        for(int i=0;i<S->input_len-S->input_cursor;i++)
            S->input_buf[i]=S->input_buf[S->input_cursor+i];
        S->input_len-=S->input_cursor; S->input_cursor=0;
        S->input_buf[S->input_len]=0;
        return true;
    }

    // ctrl+w - delete word before cursor
    if(key==23){
        while(S->input_cursor>0 && S->input_buf[S->input_cursor-1]==' '){
            for(int i=S->input_cursor-1;i<S->input_len-1;i++) S->input_buf[i]=S->input_buf[i+1];
            S->input_len--; S->input_cursor--;
        }
        while(S->input_cursor>0 && S->input_buf[S->input_cursor-1]!=' '){
            for(int i=S->input_cursor-1;i<S->input_len-1;i++) S->input_buf[i]=S->input_buf[i+1];
            S->input_len--; S->input_cursor--;
        }
        S->input_buf[S->input_len]=0;
        return true;
    }

    // up arrow (escape sequence or raw code 0x48)
    if(key==(char)0x90){
        HistoryUp(S);
        ScrollToBottom(S);
        return true;
    }
    // down arrow
    if(key==(char)0x91){
        HistoryDown(S);
        ScrollToBottom(S);
        return true;
    }
    // left arrow
    if(key==(char)0x92){
        if(S->input_cursor>0) S->input_cursor--;
        return true;
    }
    // right arrow
    if(key==(char)0x93){
        if(S->input_cursor<S->input_len) S->input_cursor++;
        return true;
    }

    // home
    if(key==(char)0x94){ S->input_cursor=0; return true; }
    // end
    if(key==(char)0x95){ S->input_cursor=S->input_len; return true; }
    // page up
    if(key==(char)0x96){
        S->scroll_offset+=10;
        int ms=(S->buf_count>25?S->buf_count-25:0);
        if(S->scroll_offset>ms) S->scroll_offset=ms;
        return true;
    }
    // page down
    if(key==(char)0x97){
        S->scroll_offset-=10;
        if(S->scroll_offset<0) S->scroll_offset=0;
        return true;
    }

    // delete
    if(key==(char)0x98){
        if(S->input_cursor<S->input_len){
            for(int i=S->input_cursor;i<S->input_len-1;i++)
                S->input_buf[i]=S->input_buf[i+1];
            S->input_len--;
            S->input_buf[S->input_len]=0;
        }
        return true;
    }

    // tab - tab completion via kvfs
    if(key=='\t'){
        TabComplete(S);
        ScrollToBottom(S);
        return true;
    }

    // printable character
    if(key>=32 && key<127 && S->input_len<TERM_INPUT_MAX-1){
        // insert at cursor
        for(int i=S->input_len;i>S->input_cursor;i--)
            S->input_buf[i]=S->input_buf[i-1];
        S->input_buf[S->input_cursor]=key;
        S->input_len++;
        S->input_cursor++;
        S->input_buf[S->input_len]=0;
        S->tab_match_count=0; // reset tab on new char
        __atomic_add_fetch(&g_termdbg[1], 1, __ATOMIC_RELAXED);
        ScrollToBottom(S);
        return true;
    }

    return false;
}

//  pty mode: vt100 screen wired to a kpty session (satoru)

// arms as "URDL" digits: 0 none, 1 light, 2 heavy, 3 double. covers u+2500..257f. (satoru)
static const char* const BOX_MIXED[64] = {
    "0110","0210","0120","0220","0011","0012","0021","0022","1100","1200","2100","2200","1001","1002","2001","2002",
    "1110","1210","2110","1120","2120","2210","1220","2220","1011","1012","2011","1021","2021","2012","1022","2022",
    "0111","0112","0211","0212","0121","0122","0221","0222","1101","1102","1201","1202","2101","2102","2201","2202",
    "1111","1112","1211","1212","2111","1121","2121","2112","2211","1122","1221","2212","1222","2122","2221","2222"
};
static const char* const BOX_DBL[29] = {
    "0303","3030","0310","0130","0330","0013","0031","0033","1300","3100","3300","1003","3001","3003",
    "1310","3130","3330","1013","3031","3033","0313","0131","0333","1303","3101","3303","1313","3131","3333"
};

static bool BoxArms(unsigned int cp, const char** out){
    static const char* const L = "0101"; static const char* const V = "1010";
    static const char* const HL = "0202"; static const char* const HV = "2020";
    if(cp>=0x250C && cp<=0x254B){ *out = BOX_MIXED[cp-0x250C]; return true; }
    if(cp>=0x2550 && cp<=0x256C){ *out = BOX_DBL[cp-0x2550]; return true; }
    switch(cp){
    case 0x2500: case 0x2504: case 0x2508: case 0x254C: *out = L; return true;
    case 0x2501: case 0x2505: case 0x2509: case 0x254D: *out = HL; return true;
    case 0x2502: case 0x2506: case 0x250A: case 0x254E: *out = V; return true;
    case 0x2503: case 0x2507: case 0x250B: case 0x254F: *out = HV; return true;
    case 0x256D: *out = "0110"; return true;
    case 0x256E: *out = "0011"; return true;
    case 0x256F: *out = "1001"; return true;
    case 0x2570: *out = "1100"; return true;
    case 0x2574: *out = "0001"; return true;
    case 0x2575: *out = "1000"; return true;
    case 0x2576: *out = "0100"; return true;
    case 0x2577: *out = "0010"; return true;
    case 0x2578: *out = "0002"; return true;
    case 0x2579: *out = "2000"; return true;
    case 0x257A: *out = "0200"; return true;
    case 0x257B: *out = "0020"; return true;
    case 0x257C: *out = "0201"; return true;
    case 0x257D: *out = "1020"; return true;
    case 0x257E: *out = "0102"; return true;
    case 0x257F: *out = "2010"; return true;
    default: return false;
    }
}

static unsigned int BlendRGB(unsigned int a, unsigned int b, int num /*0..8 of a*/){
    unsigned int r=(((a>>16)&255)*num + ((b>>16)&255)*(8-num))/8;
    unsigned int g=(((a>>8)&255)*num + ((b>>8)&255)*(8-num))/8;
    unsigned int bl=((a&255)*num + (b&255)*(8-num))/8;
    return 0xFF000000u|(r<<16)|(g<<8)|bl;
}

// draw box-drawing and block-element glyphs procedurally so they join across cells
// without depending on the font having them. returns false if cp is not handled. (satoru)
static bool DrawBlockGlyph(int x,int y,unsigned int cp,unsigned int fg,unsigned int bg){
    const int W=CELL_W, H=CELL_H;
    if(cp>=0x2580 && cp<=0x259F){
        if(cp==0x2580){ Graphics::FillRect(x,y,W,H/2,fg); return true; }
        if(cp>=0x2581 && cp<=0x2588){ int h=H*(int)(cp-0x2580)/8; Graphics::FillRect(x,y+H-h,W,h,fg); return true; }
        if(cp>=0x2589 && cp<=0x258F){ int w=W*(int)(0x2590-cp)/8; if(w<1) w=1; Graphics::FillRect(x,y,w,H,fg); return true; }
        if(cp==0x2590){ Graphics::FillRect(x+W/2,y,W-W/2,H,fg); return true; }
        if(cp>=0x2591 && cp<=0x2593){ Graphics::FillRect(x,y,W,H,BlendRGB(fg,bg,(int)(cp-0x2590)*2)); return true; }
        if(cp==0x2594){ Graphics::FillRect(x,y,W,H/8>0?H/8:1,fg); return true; }
        if(cp==0x2595){ Graphics::FillRect(x+W-W/8-1,y,W/8+1,H,fg); return true; }
        static const unsigned char Q[10]={4,8,1,13,9,7,11,2,6,14};   // ul=1 ur=2 ll=4 lr=8 (satoru)
        unsigned char m=Q[cp-0x2596];
        int hw=W/2, hh=H/2;
        if(m&1) Graphics::FillRect(x,y,hw,hh,fg);
        if(m&2) Graphics::FillRect(x+hw,y,W-hw,hh,fg);
        if(m&4) Graphics::FillRect(x,y+hh,hw,H-hh,fg);
        if(m&8) Graphics::FillRect(x+hw,y+hh,W-hw,H-hh,fg);
        return true;
    }
    const char* a=nullptr;
    if(!(cp>=0x2500 && cp<=0x257F) || !BoxArms(cp,&a)) return false;
    int cxm=x+W/2, cym=y+H/2;
    for(int k=0;k<4;k++){            // 0 up, 1 right, 2 down, 3 left
        int w=a[k]-'0';
        if(w==0) continue;
        int t=(w==2)?2:1;
        int offs[2]={0,0}; int cnt=1;
        if(w==3){ offs[0]=-2; offs[1]=1; cnt=2; t=1; }
        for(int i=0;i<cnt;i++){
            int o=offs[i];
            if(k==0||k==2){
                int lx=cxm-(t>1?1:0)+o;
                int y0=(k==0)?y:cym-1, y1=(k==0)?cym+1:y+H;
                Graphics::FillRect(lx,y0,t,y1-y0,fg);
            } else {
                int ly=cym-(t>1?1:0)+o;
                int x0=(k==3)?x:cxm-1, x1=(k==3)?cxm+1:x+W;
                Graphics::FillRect(x0,ly,x1-x0,t,fg);
            }
        }
    }
    return true;
}

static int Utf8Enc(unsigned int cp,char* o){
    if(cp<0x80){ o[0]=(char)cp; o[1]=0; return 1; }
    if(cp<0x800){ o[0]=(char)(0xC0|(cp>>6)); o[1]=(char)(0x80|(cp&63)); o[2]=0; return 2; }
    if(cp<0x10000){ o[0]=(char)(0xE0|(cp>>12)); o[1]=(char)(0x80|((cp>>6)&63)); o[2]=(char)(0x80|(cp&63)); o[3]=0; return 3; }
    o[0]=(char)(0xF0|(cp>>18)); o[1]=(char)(0x80|((cp>>12)&63)); o[2]=(char)(0x80|((cp>>6)&63)); o[3]=(char)(0x80|(cp&63)); o[4]=0; return 4;
}

static const VtCell* PtyRowPtr(TermInst* S,int r){
    Vt& vt=*S->vt;
    int off=S->pty_off;
    int hn=vt.HistCount();
    if(off>hn) off=hn;
    int v=hn-off+r;
    if(v<hn) return vt.HistLine(hn-1-v);
    return &vt.Cell(v-hn,0);
}

static void RenderPty(TermInst* S,void* win_ptr,int cx,int cy,int cw,int ch){
    Vt& g_vt=*S->vt;                 // this window's emulator (satoru)
    const int pair=PairOf(S);
    TermSyncTheme();
    EnsureMetrics();
    g_vt.SetDefaults(T_FG,T_BG);
    int ox=cx+PAD_X, oy=cy+PAD_Y;
    int cols=(cw-2*PAD_X)/CELL_W, rows=(ch-2*PAD_Y)/CELL_H;
    if(cols<20) cols=20; if(cols>VT_MAXC) cols=VT_MAXC;
    if(rows<4) rows=4;   if(rows>VT_MAXR) rows=VT_MAXR;
    if(cols!=g_vt.Cols() || rows!=g_vt.Rows()){
        g_vt.Resize(cols,rows);
        Kpty::Resize(pair,cols,rows);
    }
    // drain program output into the emulator, then send back what it asked for (satoru)
    {
        uint8_t buf[2048];
        int total=0, n;
        while(total<(1<<17) && (n=Kpty::MasterRead(pair,buf,sizeof(buf)))>0){ g_vt.Feed(buf,n); total+=n; }
        uint8_t rep[256];
        int rn;
        while((rn=g_vt.TakeReply(rep,sizeof(rep)))>0) Kpty::MasterWrite(pair,rep,rn);
        if(Kpty::MasterHasData(pair)) Graphics::MarkUIDirty();   // more than one frame's worth (satoru)
    }
    Graphics::FillRect(cx,cy,cw,ch,T_BG);
    int hn=g_vt.HistCount();
    if(S->pty_off>hn) S->pty_off=hn;
    for(int r=0;r<rows;r++){
        const VtCell* row=PtyRowPtr(S,r);
        if(!row) continue;
        int sy=oy+r*CELL_H;
        // backgrounds, batched by colour
        int c=0;
        while(c<cols){
            unsigned int bg=row[c].bg, fg=row[c].fg;
            if(row[c].attr&VA_INVERSE) bg=fg;
            if(bg==T_BG){ c++; continue; }
            int s=c;
            while(c<cols){
                unsigned int b2=row[c].bg, f2=row[c].fg;
                if(row[c].attr&VA_INVERSE) b2=f2;
                if(b2!=bg) break;
                c++;
            }
            Graphics::FillRect(ox+s*CELL_W,sy,(c-s)*CELL_W,CELL_H,bg);
        }
        // glyphs: ascii runs batched by colour, everything else per cell (satoru)
        c=0;
        while(c<cols){
            const VtCell& k=row[c];
            unsigned int cp=k.cp;
            if(cp==0||cp==' '||cp==0xFFFFFFFFu||(k.attr&VA_HIDDEN)){ c++; continue; }
            unsigned int fg=k.fg, bg=k.bg;
            if(k.attr&VA_INVERSE){ unsigned int t=fg; fg=bg; bg=t; }
            if(k.attr&VA_DIM) fg=BlendRGB(fg,bg,5);
            bool bold=(k.attr&VA_BOLD)!=0;
            if(cp<0x80){
                // one glyph per cell: the font's fractional advance drifts away from the
                // 8 px grid over a long run (a 118 column row ended ~30 px too far right
                // and clipped), so every cell is placed on the grid. (satoru)
                char s1[2]={(char)cp,0};
                TermDrawRun(ox+c*CELL_W,sy,s1,fg,bold);
                c++;
                continue;
            }
            int x=ox+c*CELL_W;
            if(!DrawBlockGlyph(x,sy,cp,fg,bg)){
                char u[8]; Utf8Enc(cp,u);
                TermDrawRun(x,sy,u,fg,bold);
            }
            c++;
        }
        // underline / strike
        for(int cc=0;cc<cols;cc++){
            unsigned char at=row[cc].attr;
            if(!(at&(VA_UNDER|VA_STRIKE))) continue;
            unsigned int fg=(at&VA_INVERSE)?row[cc].bg:row[cc].fg;
            if(at&VA_UNDER) Graphics::FillRect(ox+cc*CELL_W,sy+CELL_H-3,CELL_W,1,fg);
            if(at&VA_STRIKE) Graphics::FillRect(ox+cc*CELL_W,sy+CELL_H/2,CELL_W,1,fg);
        }
    }
    // cursor: steady block with the glyph redrawn inverted (satoru)
    if(g_vt.CursorVisible() && S->pty_off==0){
        int r=g_vt.CurRow(), c=g_vt.CurCol();
        if(r>=0 && r<rows && c>=0 && c<cols){
            int x=ox+c*CELL_W, y=oy+r*CELL_H;
            bool foc = win_ptr && ((Window*)win_ptr)->focused;
            if(foc){
                Graphics::FillRect(x,y,CELL_W,CELL_H,T_CURSOR);
                const VtCell& k=g_vt.Cell(r,c);
                if(k.cp>' ' && k.cp!=0xFFFFFFFFu){
                    if(k.cp<0x80){ char s[2]={(char)k.cp,0}; TermDrawRun(x,y,s,T_BG,(k.attr&VA_BOLD)!=0); }
                    else if(!DrawBlockGlyph(x,y,k.cp,T_BG,T_CURSOR)){ char u[8]; Utf8Enc(k.cp,u); TermDrawRun(x,y,u,T_BG,false); }
                }
            } else {
                Graphics::FillRect(x,y,CELL_W,1,T_CURSOR); Graphics::FillRect(x,y+CELL_H-1,CELL_W,1,T_CURSOR);
                Graphics::FillRect(x,y,1,CELL_H,T_CURSOR); Graphics::FillRect(x+CELL_W-1,y,1,CELL_H,T_CURSOR);
            }
        }
    }
}

bool TerminalApp::PtyActive(){
    for(int i=0;i<TERM_MAX_INST;i++){ TermInst* t=g_inst[i]; if(t && t->pty_mode) return true; }
    return false;
}

bool TerminalApp::PtyBegin(){
    // the window whose command is running takes its own pair (kurono.pty=0: the single shared
    // session, only while no other window holds it). (satoru)
    TermInst* S = exec_inst();
    if(!S && LinuxSyscall::BridgeTty()>=0) return true;   // exec bridge: the shell already owns the window, programs get stdio only (satoru)
    if(!in_exec_cur() || !S || S->wid<0 || S->pty_mode) return false;
    const bool legacy=LinuxSyscall::PtyLegacy();
    if(legacy ? g_legacy_owner!=nullptr : !Kpty::Valid(S->pty)) return false;
    const int wid = S->wid;
    Window* w=WindowManager::GetWindow(wid);
    if(!w || w->state==WIN_CLOSED) return false;
    TermSyncTheme();
    EnsureMetrics();
    // a tui wants room: grow the window to most of the screen (satoru)
    int sw=Graphics::GetWidth(), sh=Graphics::GetHeight();
    int nw=sw-60, nh=sh-110;
    if(nw>1000) nw=1000;
    if(nh>700) nh=700;
    extern int g_gui_extra_terms;   // test hook kurono.guiterms: keep windows small so several fit (satoru)
    if(g_gui_extra_terms==0 && (nw>w->w || nh>w->h)){
        WindowManager::ResizeWindow(wid, nw>w->w?nw:w->w, nh>w->h?nh:w->h);
        w=WindowManager::GetWindow(wid);
        int nx=(sw-w->w)/2, ny=36;
        if(nx<0) nx=0;
        WindowManager::MoveWindow(wid, nx, ny);
    }
    int cols=(w->content_w-2*PAD_X)/CELL_W, rows=(w->content_h-2*PAD_Y)/CELL_H;
    if(cols<20) cols=80; if(cols>VT_MAXC) cols=VT_MAXC;
    if(rows<4) rows=24;  if(rows>VT_MAXR) rows=VT_MAXR;
    if(!S->vt){ S->vt=new Vt; if(!S->vt) return false; }
    S->vt->Init(cols,rows,T_FG,T_BG);
    S->pty_off=0;
    if(legacy){ Kpty::Begin(cols,rows); g_legacy_owner=S; }
    else {
        Kpty::Reset(S->pty,cols,rows);                 // fresh termios, empty rings, no session (satoru)
        LinuxSyscall::SetTerminalTty(S->pty);          // the next program this proc launches runs on it (satoru)
    }
    S->pty_mode=true;
    Graphics::MarkUIDirty();
    return true;
}

static char PtyAscii(unsigned int cp){
    if(cp==0||cp==0xFFFFFFFFu) return ' ';
    if(cp<0x80) return (char)cp;
    if(cp>=0x2500 && cp<=0x257F){
        if(cp==0x2500||cp==0x2501||cp==0x2550) return '-';
        if(cp==0x2502||cp==0x2503||cp==0x2551) return '|';
        return '+';
    }
    if(cp>=0x2580 && cp<=0x259F) return '#';
    return '?';
}

void TerminalApp::PtyEnd(){
    TermInst* S = exec_inst();
    if(!S || !S->pty_mode) return;
    const bool legacy=LinuxSyscall::PtyLegacy();
    const int pair=PairOf(S);
    Vt& g_vt=*S->vt;
    if(legacy) Kpty::End();
    else { LinuxSyscall::SetTerminalTty(-1); Kpty::ClearSession(pair); }
    {   // final drain so nothing the program printed on its way out is lost (satoru)
        uint8_t buf[2048]; int n;
        while((n=Kpty::MasterRead(pair,buf,sizeof(buf)))>0) g_vt.Feed(buf,n);
    }
    S->pty_mode=false;
    if(legacy) g_legacy_owner=nullptr;
    // keep what was on screen: copy the visible main screen into the normal scrollback (satoru)
    if(!g_vt.AltScreen()){
        if(S->cursor_col>0) NewLine(S);
        int last=-1;
        for(int r=0;r<g_vt.Rows();r++)
            for(int c=0;c<g_vt.Cols();c++){ unsigned int cp=g_vt.Cell(r,c).cp; if(cp!=0 && cp!=' ') { last=r; break; } }
        SetColor(S, T_FG,T_BG);
        for(int r=0;r<=last;r++){
            int len=g_vt.Cols();
            while(len>0){ unsigned int cp=g_vt.Cell(r,len-1).cp; if(cp!=0 && cp!=' ') break; len--; }
            for(int c=0;c<len;c++) WriteChar(S, PtyAscii(g_vt.Cell(r,c).cp));
            NewLine(S);
        }
    }
    ScrollToBottom(S);
    Graphics::MarkUIDirty();
}

static void PtyScrollInst(TermInst* S,int delta){
    if(!S || !S->vt) return;
    S->pty_off += (delta>0)?3:(delta<0?-3:0);
    if(S->pty_off<0) S->pty_off=0;
    if(S->pty_off>S->vt->HistCount()) S->pty_off=S->vt->HistCount();
    Graphics::MarkUIDirty();
}

void TerminalApp::PtyScroll(int delta){
    Window* fw=WindowManager::GetFocusedWindow();
    PtyScrollInst(fw ? FindInst(fw->id) : nullptr, delta);
}

// middle click: paste the primary selection at the focused terminal's input.
// pty programs get the same bracketed paste the ctrl+shift+v path uses. (satoru)
void TerminalApp::PastePrimary(){
    Window* fw=WindowManager::GetFocusedWindow();
    if(!fw) return;
    TermInst* S=FindInst(fw->id);
    if(!S || !ClipboardManager::HasPrimaryText()) return;
    const char* s=ClipboardManager::GetPrimaryText();
    if(OwnsPty(S)){
        const int pair=PairOf(S);
        Vt& g_vt=*S->vt;
        if(Kpty::Hungup(pair)) return;
        if(g_vt.BracketedPaste()) Kpty::MasterWrite(pair,(const uint8_t*)"\x1b[200~",6);
        uint8_t cb[256]; int cn=0; int total=0;
        for(int i=0;s[i]&&total<8192;i++,total++){
            uint8_t ch=(uint8_t)s[i];
            if(ch=='\n') ch='\r';
            else if(ch<0x20 && ch!='\t' && ch!='\r') continue;
            cb[cn++]=ch;
            if(cn==(int)sizeof(cb)){ Kpty::MasterWrite(pair,cb,cn); cn=0; }
        }
        if(cn) Kpty::MasterWrite(pair,cb,cn);
        if(g_vt.BracketedPaste()) Kpty::MasterWrite(pair,(const uint8_t*)"\x1b[201~",6);
        S->pty_off=0;
        return;
    }
    for(int i=0;s[i] && s[i]!='\n' && s[i]!='\r';i++){
        char c=s[i];
        if(c<32 || c>=127) continue;
        if(S->input_len>=TERM_INPUT_MAX-1) break;
        for(int j=S->input_len;j>S->input_cursor;j--) S->input_buf[j]=S->input_buf[j-1];
        S->input_buf[S->input_cursor]=c;
        S->input_len++; S->input_cursor++;
        S->input_buf[S->input_len]=0;
    }
    S->tab_match_count=0;
    ScrollToBottom(S);
}

void TerminalApp::PtyPoll(){
    ReapClosed();   // gui-side: frees the slots of closed terminal windows (satoru)
    // closing a Terminal window hangs up its pair: SIGHUP then SIGCONT to the session that
    // has it as controlling tty (and no other window's session). (satoru)
    for(int i=0;i<TERM_MAX_INST;i++){
        TermInst* t=g_inst[i];
        if(!t || __atomic_load_n(&t->in_use,__ATOMIC_ACQUIRE)!=1 || t->wid<0) continue;
        Window* w=WindowManager::GetWindow(t->wid);
        if(w && w->state!=WIN_CLOSED) continue;
        if(LinuxSyscall::PtyLegacy()){
            if(t==g_legacy_owner && !Kpty::Hungup()) Kpty::Hangup();
        } else if(t->pty>=0 && !Kpty::Hungup(t->pty)){
            Kpty::Hangup(t->pty);
        }
    }
}

static int PtyCsi(uint8_t* o,int n,int num,char fin,int mod,bool app){
    o[n++]=0x1B;
    if(mod>1){ o[n++]='['; if(num==0) o[n++]='1'; else { if(num>=10){ o[n++]=(char)('0'+num/10); } o[n++]=(char)('0'+num%10); } o[n++]=';'; o[n++]=(char)('0'+mod); o[n++]=fin; return n; }
    if(num==0){ o[n++]=app?'O':'['; o[n++]=fin; return n; }
    o[n++]='['; if(num>=10) o[n++]=(char)('0'+num/10); o[n++]=(char)('0'+num%10); o[n++]=fin; return n;
}

void TerminalApp::PtyKeyEvent(int key,bool shift,bool ctrl,bool alt){
    Window* fw=WindowManager::GetFocusedWindow();
    if(!fw) return;
    TermInst* S = FindInst(fw->id);
    if(!S) return;
    const int term_wid = S->wid;
    Key k=(Key)key;
    // ctrl+alt+m: maximize / restore the terminal window from the keyboard (satoru)
    if(k==KEY_M && ctrl && alt){ WindowManager::ToggleMaximize(term_wid); return; }
    if(!OwnsPty(S)){
        // the plain shell line editor: arrows, home/end, paging and delete arrive as key
        // events (the char queue never carries them) and map to the codes Input() expects.
        char code=0;
        switch(k){
        case KEY_UP: code=(char)0x90; break;    case KEY_DOWN: code=(char)0x91; break;
        case KEY_LEFT: code=(char)0x92; break;  case KEY_RIGHT: code=(char)0x93; break;
        case KEY_HOME: code=(char)0x94; break;  case KEY_END: code=(char)0x95; break;
        case KEY_PAGEUP: code=(char)0x96; break; case KEY_PAGEDOWN: code=(char)0x97; break;
        case KEY_DELETE: code=(char)0x98; break;
        default: break;
        }
        if(code) Input(fw,0,0,false,code);
        return;
    }
    const int pair=PairOf(S);
    Vt& g_vt=*S->vt;
    if(Kpty::Hungup(pair)) return;
    uint8_t o[64]; int n=0;
    int mod=1+(shift?1:0)+(alt?2:0)+(ctrl?4:0);
    bool app=g_vt.AppCursorKeys();
    if(alt && k==KEY_TAB) return;                 // desktop alt-tab (satoru)
    if(k==KEY_LSHIFT||k==KEY_RSHIFT||k==KEY_LCTRL||k==KEY_RCTRL||k==KEY_LALT||k==KEY_RALT||
       k==KEY_LSUPER||k==KEY_RSUPER||k==KEY_CAPSLOCK||k==KEY_NUMLOCK||k==KEY_SCROLLLOCK) return;
    S->last_keypress_ms=Timer::GetRealMs();
    // scrollback paging does not reach the program (satoru)
    if(shift && !ctrl && !alt && (k==KEY_PAGEUP||k==KEY_PAGEDOWN)){
        PtyScrollInst(S,k==KEY_PAGEUP?1:-1); return;
    }
    // paste (ctrl+shift+v), bracketed when the program asked for it (satoru)
    if(ctrl && shift && k==KEY_V){
        if(ClipboardManager::HasText()){
            const char* s=ClipboardManager::GetText();
            if(g_vt.BracketedPaste()) Kpty::MasterWrite(pair,(const uint8_t*)"\x1b[200~",6);
            uint8_t cb[256]; int cn=0; int total=0;
            for(int i=0;s[i]&&total<8192;i++,total++){
                uint8_t ch=(uint8_t)s[i];
                if(ch=='\n') ch='\r';
                else if(ch<0x20 && ch!='\t' && ch!='\r') continue;
                cb[cn++]=ch;
                if(cn==(int)sizeof(cb)){ Kpty::MasterWrite(pair,cb,cn); cn=0; }
            }
            if(cn) Kpty::MasterWrite(pair,cb,cn);
            if(g_vt.BracketedPaste()) Kpty::MasterWrite(pair,(const uint8_t*)"\x1b[201~",6);
        }
        S->pty_off=0; return;
    }
    if(ctrl && shift && k==KEY_C) return;
    S->pty_off=0;
    switch(k){
    case KEY_UP:    n=PtyCsi(o,n,0,'A',mod,app); break;
    case KEY_DOWN:  n=PtyCsi(o,n,0,'B',mod,app); break;
    case KEY_RIGHT: n=PtyCsi(o,n,0,'C',mod,app); break;
    case KEY_LEFT:  n=PtyCsi(o,n,0,'D',mod,app); break;
    case KEY_HOME:  n=PtyCsi(o,n,0,'H',mod,app); break;
    case KEY_END:   n=PtyCsi(o,n,0,'F',mod,app); break;
    case KEY_INSERT:   n=PtyCsi(o,n,2,'~',mod,false); break;
    case KEY_DELETE:   n=PtyCsi(o,n,3,'~',mod,false); break;
    case KEY_PAGEUP:   n=PtyCsi(o,n,5,'~',mod,false); break;
    case KEY_PAGEDOWN: n=PtyCsi(o,n,6,'~',mod,false); break;
    case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4: {
        char fin=(char)('P'+(k-KEY_F1));
        if(mod>1) n=PtyCsi(o,n,0,fin,mod,false); else { o[n++]=0x1B; o[n++]='O'; o[n++]=(uint8_t)fin; }
        break;
    }
    case KEY_F5: n=PtyCsi(o,n,15,'~',mod,false); break;
    case KEY_F6: n=PtyCsi(o,n,17,'~',mod,false); break;
    case KEY_F7: n=PtyCsi(o,n,18,'~',mod,false); break;
    case KEY_F8: n=PtyCsi(o,n,19,'~',mod,false); break;
    case KEY_F9: n=PtyCsi(o,n,20,'~',mod,false); break;
    case KEY_F10: n=PtyCsi(o,n,21,'~',mod,false); break;
    case KEY_F11: n=PtyCsi(o,n,23,'~',mod,false); break;
    case KEY_F12: n=PtyCsi(o,n,24,'~',mod,false); break;
    case KEY_ENTER: case KEY_KP_ENTER: if(alt) o[n++]=0x1B; o[n++]='\r'; break;
    case KEY_BACKSPACE: if(alt) o[n++]=0x1B; o[n++]=ctrl?0x08:0x7F; break;
    case KEY_TAB: if(shift){ o[n++]=0x1B; o[n++]='['; o[n++]='Z'; } else { if(alt) o[n++]=0x1B; o[n++]='\t'; } break;
    case KEY_ESC: o[n++]=0x1B; break;
    default: {
        char c=Keyboard::KeyToChar(k,shift,Keyboard::GetState().caps_lock);
        if(!c) return;
        unsigned char u=(unsigned char)c;
        if(ctrl){
            if(u>='a'&&u<='z') u=(unsigned char)(u-'a'+1);
            else if(u>='A'&&u<='Z') u=(unsigned char)(u-'A'+1);
            else if(u=='['||u=='{') u=27;
            else if(u=='\\'||u=='|') u=28;
            else if(u==']'||u=='}') u=29;
            else if(u=='^'||u=='6') u=30;
            else if(u=='_'||u=='-'||u=='/') u=31;
            else if(u=='@'||u=='2'||u==' ') u=0;
            else return;
        }
        if(alt) o[n++]=0x1B;
        o[n++]=u;
        break;
    }
    }
    if(n>0) Kpty::MasterWrite(pair,o,n);
}

// end (satoru)
