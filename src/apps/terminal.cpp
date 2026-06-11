//  kurono os  -  terminal emulator v2.0
//  improvements: tab completion, extended ansi, blinking cursor, shortcuts
#include "terminal.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../shell/shell.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../fs/kvfs.h"
#include "../system/logging.h"
#include "../system/clipboard.h"
#include "../ui/kss.h"
#include "../ui/font.h"

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
// semantic ansi palette  -  fixed, these are color *meanings* not theme chrome. (satoru)
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
    if(metrics_ready) return;
    cell_pxh = KSS::BodyPx();        // matches graphics' 16px body base (satoru)
    if(cell_pxh < 8.0f) cell_pxh = 16.0f;
    int mw = FontTTF::ok ? FontTTF::Measure(cell_pxh, "M") : 0;
    if(mw <= 0) mw = (int)(cell_pxh * 0.5f + 0.5f);  // sane fallback (satoru)
    CELL_W = mw;
    CELL_H = (int)(cell_pxh * 1.30f + 0.5f);         // ~30% leading (satoru)
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
        FontTTF::DrawString(sx, gy, cell_pxh, s, draw_fg);
        if(bold) FontTTF::DrawString(sx+1, gy, cell_pxh, s, draw_fg); // faux-bold (satoru)
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

TermLine     TerminalApp::buffer[TERM_SCROLL_BK];
int          TerminalApp::buf_count     = 0;
int          TerminalApp::scroll_offset = 0;
int          TerminalApp::cursor_row    = 0;
int          TerminalApp::cursor_col    = 0;
unsigned int TerminalApp::cur_fg        = T_FG;
unsigned int TerminalApp::cur_bg        = T_BG;
bool         TerminalApp::cur_bold      = false;

char         TerminalApp::input_buf[TERM_INPUT_MAX];
int          TerminalApp::input_len     = 0;
int          TerminalApp::input_cursor  = 0;

char         TerminalApp::history[TERM_HIST_MAX][TERM_INPUT_MAX];
int          TerminalApp::hist_count    = 0;
int          TerminalApp::hist_pos      = -1;
char         TerminalApp::hist_saved[TERM_INPUT_MAX];

char         TerminalApp::tab_matches[TERM_TAB_MAX][TERM_INPUT_MAX];
int          TerminalApp::tab_match_count = 0;
int          TerminalApp::tab_match_idx   = -1;
char         TerminalApp::tab_prefix[TERM_INPUT_MAX];

bool         TerminalApp::shell_ready   = false;
char         TerminalApp::prompt[128];
bool         TerminalApp::command_pending = false;
bool         TerminalApp::command_running = false;
char         TerminalApp::pending_cmd[TERM_INPUT_MAX];
unsigned int TerminalApp::blink_timer   = 0;
unsigned int TerminalApp::last_keypress_ms = 0;
int          TerminalApp::smooth_scroll_q16 = 0;
int          TerminalApp::target_scroll_offset = 0;

//  init / open
void TerminalApp::Init(){
    // seed theme chrome + grid metrics from kss before anything draws. (satoru)
    TermSyncTheme();
    metrics_ready = false;
    EnsureMetrics();
    buf_count=0; scroll_offset=0;
    cursor_row=0; cursor_col=0;
    cur_fg=T_FG; cur_bg=T_BG; cur_bold=false;
    input_len=0; input_cursor=0;
    hist_count=0; hist_pos=-1; hist_saved[0]=0;
    tab_match_count=0; tab_match_idx=-1; tab_prefix[0]=0;
    blink_timer=0;
    last_keypress_ms=0;
    smooth_scroll_q16=0;
    target_scroll_offset=0;
    shell_ready=true;
    command_pending=false;
    command_running=false;
    pending_cmd[0]=0;

    // clear buffer
    for(int i=0;i<TERM_SCROLL_BK;i++){
        buffer[i].len=0;
        for(int j=0;j<TERM_COLS;j++){
            buffer[i].cells[j].ch=' ';
            buffer[i].cells[j].fg=T_FG;
            buffer[i].cells[j].bg=T_BG;
            buffer[i].cells[j].bold=false;
        }
    }

    // welcome banner
    SetColor(0xFF2980B9, T_BG);
    WriteLn("╔══════════════════════════════════════════════════════════╗");
    SetColor(0xFF5DADE2, T_BG);
    Write  ("\u2551  ");
    SetColor(0xFFECF0F1, T_BG);
    Write  ("Kurono OS");
    SetColor(T_FG, T_BG);
    Write  (" Shell Terminal");
    SetColor(T_GRAY, T_BG);
    WriteLn("                         ║");
    SetColor(T_GRAY, T_BG);
    WriteLn("║  Powered by KuronoShell  -  76+ commands across 3 envs   ║");
    Write  ("║  Tab=complete  ↑↓=history  Ctrl+L=clear  Ctrl+C=cancel");
    SetColor(0xFF2980B9, T_BG);
    WriteLn("  ║");
    WriteLn("╚══════════════════════════════════════════════════════════╝");
    SetColor(T_FG, T_BG);
    WriteLn("");
    SetColor(T_GRAY, T_BG);
    WriteLn("Type 'help' for commands. 'linux'/'cmd' to switch env.");
    SetColor(T_FG, T_BG);
    WriteLn("");

    WritePrompt();
}

static void TerminalShellChunkSink(void* /*udata*/, const char* data, int len) {
    TerminalApp::EmitShellChunk(data, len);
    KuronoShell::PumpUI();
}

int TerminalApp::Open(){
    Init();
    RuntimeLog::LogAppEvent("terminal", "open");
    KuronoShell::SetOutputChunkCallback(TerminalShellChunkSink, nullptr);
    // create window via wm
    int wid = WindowManager::CreateWindow("Terminal", -1, -1, 648, 432,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TerminalApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TerminalApp::Input(w,p1,p2,true,0);
            else if(ev==2) TerminalApp::Input(w,0,0,false,(char)p1);
            else if(ev==3) {
                // scroll event: p1 = scroll delta (positive = scroll up).
                // Stage on target; the next paint eases scroll_offset toward it.
                int max_scroll = TerminalApp::buf_count > 25 ? TerminalApp::buf_count - 25 : 0;
                TerminalApp::target_scroll_offset += (p1 > 0) ? 3 : (p1 < 0 ? -3 : 0);
                if(TerminalApp::target_scroll_offset < 0)
                    TerminalApp::target_scroll_offset = 0;
                if(TerminalApp::target_scroll_offset > max_scroll)
                    TerminalApp::target_scroll_offset = max_scroll;
            }
        }
    );
    return wid;
}

//  terminal buffer write
void TerminalApp::NewLine(){
    cursor_col=0;
    cursor_row++;
    if(cursor_row>=TERM_SCROLL_BK) cursor_row=TERM_SCROLL_BK-1;
    if(cursor_row>=buf_count) buf_count=cursor_row+1;
    // clear the new row (reset bold too, or stale weight leaks on reuse) (satoru)
    if(cursor_row<TERM_SCROLL_BK){
        buffer[cursor_row].len=0;
        for(int j=0;j<TERM_COLS;j++){
            buffer[cursor_row].cells[j].ch=' ';
            buffer[cursor_row].cells[j].fg=T_FG;
            buffer[cursor_row].cells[j].bg=T_BG;
            buffer[cursor_row].cells[j].bold=false;
        }
    }
}

void TerminalApp::WriteChar(char c){
    if(c=='\n'){ NewLine(); return; }
    if(c=='\r'){ cursor_col=0; return; }
    if(c=='\t'){
        int spc=4-(cursor_col%4);
        for(int i=0;i<spc;i++) WriteChar(' ');
        return;
    }
    if(cursor_col>=TERM_COLS) NewLine();
    if(cursor_row>=TERM_SCROLL_BK) return;

    TermCell* cell=&buffer[cursor_row].cells[cursor_col];
    cell->ch=c;
    cell->fg=cur_fg;
    cell->bg=cur_bg;
    cell->bold=cur_bold;   // record weight so bold cells actually draw bold (satoru)
    cursor_col++;
    if(cursor_col>buffer[cursor_row].len)
        buffer[cursor_row].len=cursor_col;
}

void TerminalApp::EmitShellChunk(const char* data, int len) {
    if (!data || len <= 0) return;
    SetColor(T_FG, T_BG);
    for (int i = 0; i < len; i++)
        WriteChar(data[i]);
    ScrollToBottom();
    // async shell output lands on the Shell thread, not the input path, so the
    // GUI gate would otherwise not see it for up to ~250ms. Mark dirty so the
    // streamed chunk paints on the next frame. (satoru, review fix)
    Graphics::MarkUIDirty();
}

void TerminalApp::Write(const char* text){
    if(!text)return;
    for(int i=0;text[i];i++){
        // handle escape sequences
        if(text[i]=='\x1b' && text[i+1]=='['){
            // check for clr (clear command)
            if(text[i+2]=='C' && text[i+3]=='L' && text[i+4]=='R'){
                Clear();
                WritePrompt();
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
                        case 0:  SetColor(T_FG,T_BG); cur_bold=false; break;
                        case 1:  cur_bold=true;  break;
                        case 2:  cur_bold=false; break;
                        case 30: cur_fg=0xFF2D3436; break;
                        case 31: cur_fg=T_RED;    break;
                        case 32: cur_fg=T_GREEN;  break;
                        case 33: cur_fg=T_YELLOW; break;
                        case 34: cur_fg=T_PROMPT; break;
                        case 35: cur_fg=T_MAGENTA;break;
                        case 36: cur_fg=T_CYAN;   break;
                        case 37: cur_fg=0xFFECECEC;break;
                        case 39: cur_fg=T_FG;     break;
                        case 90: cur_fg=T_GRAY;     break;
                        case 91: cur_fg=T_BRED;     break;
                        case 92: cur_fg=T_BGREEN;   break;
                        case 93: cur_fg=T_BYELLOW;  break;
                        case 94: cur_fg=T_BBLUE;    break;
                        case 95: cur_fg=T_BMAGENTA; break;
                        case 96: cur_fg=T_BCYAN;    break;
                        case 97: cur_fg=T_BWHITE;   break;
                        case 40: cur_bg=0xFF1A1A2E; break;
                        case 41: cur_bg=0xFF8B0000; break;
                        case 42: cur_bg=0xFF006400; break;
                        case 43: cur_bg=0xFF8B6914; break;
                        case 44: cur_bg=0xFF00008B; break;
                        case 45: cur_bg=0xFF800080; break;
                        case 46: cur_bg=0xFF008B8B; break;
                        case 47: cur_bg=0xFF808080; break;
                        case 49: cur_bg=T_BG;       break;
                    }
                }
                i=j;
                continue;
            }
            // \x1b[2j = clear screen
            if(text[j]=='J' && params[0]==2){ Clear(); WritePrompt(); i=j; continue; }
            // \x1b[k = erase from cursor to end of line (param 0 or absent). (satoru)
            if(text[j]=='K' && params[0]==0){
                if(cursor_row>=0 && cursor_row<TERM_SCROLL_BK){
                    for(int c=cursor_col;c<TERM_COLS;c++){
                        buffer[cursor_row].cells[c].ch=' ';
                        buffer[cursor_row].cells[c].fg=cur_fg;
                        buffer[cursor_row].cells[c].bg=cur_bg;
                        buffer[cursor_row].cells[c].bold=false;
                    }
                    if(buffer[cursor_row].len>cursor_col)
                        buffer[cursor_row].len=cursor_col;
                }
                i=j; continue;
            }
            // unknown escape - skip \x1b[
            continue;
        }
        WriteChar(text[i]);
    }
}

void TerminalApp::WriteLn(const char* text){
    Write(text);
    NewLine();
}

void TerminalApp::Clear(){
    buf_count=0; cursor_row=0; cursor_col=0; scroll_offset=0;
    for(int i=0;i<TERM_SCROLL_BK;i++){
        buffer[i].len=0;
        for(int j=0;j<TERM_COLS;j++){
            buffer[i].cells[j].ch=' ';
            buffer[i].cells[j].fg=T_FG;
            buffer[i].cells[j].bg=T_BG;
            buffer[i].cells[j].bold=false;
        }
    }
}

void TerminalApp::SetColor(unsigned int fg,unsigned int bg){
    cur_fg=fg; cur_bg=bg;
}

void TerminalApp::SetBold(bool b){
    cur_bold=b;
}

void TerminalApp::ScrollToBottom(){
    scroll_offset = 0;
    target_scroll_offset = 0;
    smooth_scroll_q16 = 0;
}

void TerminalApp::WritePrompt(){
    // use the kernel shell's prompt (reflects environment switches)
    char shell_prompt[128];
    KuronoShell::GetPrompt(shell_prompt, 128);

    // show environment indicator
    CmdEnvironment env = KuronoShell::GetEnv();
    SetColor(T_GRAY, T_BG);
    Write("[");
    switch (env) {
        case ENV_LINUX:
            SetColor(T_GREEN, T_BG);
            Write("linux");
            break;
        case ENV_WINDOWS:
            SetColor(T_CYAN, T_BG);
            Write("windows");
            break;
        case ENV_DEBIAN:
            SetColor(0xFFD70751, T_BG);  // Debian swirl red
            Write("debian");
            break;
        default:
            SetColor(0xFF3498DB, T_BG);
            Write("kurono");
            break;
    }
    SetColor(T_GRAY, T_BG);
    Write("] ");

    // write the shell-generated prompt
    SetColor(T_GREEN, T_BG);
    // parse the prompt for user@host:path$ format
    const char* cwd = KVFS::GetCwd();
    if(!cwd || cwd[0]==0) cwd="/";

    const char* user = KuronoShell::GetVar("USER");
    const char* host = KuronoShell::GetVar("HOSTNAME");
    if (!user) user = "user";
    if (!host) host = "kurono";

    Write(user);
    SetColor(T_GRAY, T_BG);
    Write("@");
    SetColor(0xFF3498DB, T_BG);
    Write(host);
    SetColor(T_GRAY, T_BG);
    Write(":");
    SetColor(T_CYAN, T_BG);
    Write(cwd);
    SetColor(0xFF7F8C8D, T_BG);

    // windows vs linux prompt char
    if (env == ENV_WINDOWS)
        Write("> ");
    else
        Write("$ ");

    SetColor(T_FG, T_BG);

    // store displayable prompt for scrollback
    scpy(prompt, "[", 128);
    sapp(prompt, KuronoShell::EnvName(env), 128);
    sapp(prompt, "] ", 128);
    sapp(prompt, user, 128);
    sapp(prompt, "@", 128);
    sapp(prompt, host, 128);
    sapp(prompt, ":", 128);
    sapp(prompt, cwd, 128);
    sapp(prompt, env == ENV_WINDOWS ? "> " : "$ ", 128);
}

//  tab completion
void TerminalApp::TabComplete(){
    // find start of the last word in input (after last space before cursor)
    int word_start=0;
    for(int i=0;i<input_cursor;i++) if(input_buf[i]==' ') word_start=i+1;

    int partial_len=input_cursor-word_start;
    if(partial_len<0) partial_len=0;
    char partial[TERM_INPUT_MAX];
    for(int i=0;i<partial_len;i++) partial[i]=input_buf[word_start+i];
    partial[partial_len]=0;

    // determine basename insertion point: if partial has '/', we replace
    // only the substring after the last slash so directory prefix is kept.
    int last_slash_in_partial=-1;
    for(int i=0;i<partial_len;i++) if(partial[i]=='/') last_slash_in_partial=i;
    int insert_pos = word_start + (last_slash_in_partial+1);

    // if same prefix: cycle through existing matches
    if(tab_match_count>0 && seq(partial, tab_prefix)){
        tab_match_idx=(tab_match_idx+1)%tab_match_count;
        const char* m=tab_matches[tab_match_idx];
        int mlen=slen(m);
        int tail_start=input_cursor, tail_len=input_len-tail_start;
        char nb[TERM_INPUT_MAX];
        for(int i=0;i<insert_pos;i++) nb[i]=input_buf[i];
        for(int i=0;i<mlen&&insert_pos+i<TERM_INPUT_MAX-1;i++) nb[insert_pos+i]=m[i];
        for(int i=0;i<tail_len&&insert_pos+mlen+i<TERM_INPUT_MAX-1;i++)
            nb[insert_pos+mlen+i]=input_buf[tail_start+i];
        int nl=insert_pos+mlen+tail_len;
        if(nl>=TERM_INPUT_MAX) nl=TERM_INPUT_MAX-1;
        nb[nl]=0;
        scpy(input_buf,nb,TERM_INPUT_MAX);
        input_len=nl; input_cursor=insert_pos+mlen;
        return;
    }

    // new prefix scan  -  list kvfs children
    scpy(tab_prefix,partial,TERM_INPUT_MAX);
    tab_match_count=0; tab_match_idx=-1;

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
        for(int i=0;i<ccount&&tab_match_count<TERM_TAB_MAX;i++){
            const char* nm=cmds[i].name;
            if(!nm||!nm[0]) continue;
            bool match=true;
            for(int k=0;k<partial_len;k++) if(nm[k]!=partial[k]){match=false;break;}
            if(!match) continue;
            // skip if this command name is already listed (handlers can share
            // a name across environments). (satoru)
            bool dup=false;
            for(int d=0;d<tab_match_count;d++) if(seq(tab_matches[d],nm)){dup=true;break;}
            if(dup) continue;
            scpy(tab_matches[tab_match_count],nm,TERM_INPUT_MAX);
            tab_match_count++;
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

    for(int i=0;i<n&&tab_match_count<TERM_TAB_MAX;i++){
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
                for(int d=0;d<tab_match_count;d++) if(seq(tab_matches[d],nm)){dup=true;break;}
            if(!dup){
                scpy(tab_matches[tab_match_count],nm,TERM_INPUT_MAX);
                if(children[i]->is_dir()) sapp(tab_matches[tab_match_count],"/",TERM_INPUT_MAX);
                tab_match_count++;
            }
        }
    }

    if(tab_match_count==0) return; // no match, silent

    if(tab_match_count==1){
        // auto-insert unique match (preserving directory prefix)
        tab_match_idx=0;
        const char* m=tab_matches[0];
        int mlen=slen(m);
        int tail_start=input_cursor, tail_len=input_len-tail_start;
        char nb[TERM_INPUT_MAX];
        for(int i=0;i<insert_pos;i++) nb[i]=input_buf[i];
        for(int i=0;i<mlen&&insert_pos+i<TERM_INPUT_MAX-1;i++) nb[insert_pos+i]=m[i];
        for(int i=0;i<tail_len&&insert_pos+mlen+i<TERM_INPUT_MAX-1;i++)
            nb[insert_pos+mlen+i]=input_buf[tail_start+i];
        int nl=insert_pos+mlen+tail_len;
        if(nl>=TERM_INPUT_MAX) nl=TERM_INPUT_MAX-1;
        nb[nl]=0;
        scpy(input_buf,nb,TERM_INPUT_MAX);
        input_len=nl; input_cursor=insert_pos+mlen;
        tab_match_count=0; // reset so next tab re-scans
    } else {
        // show all matches then cycle on subsequent tabs
        NewLine();
        SetColor(T_CYAN,T_BG);
        for(int i=0;i<tab_match_count;i++){
            Write(tab_matches[i]);
            Write("  ");
        }
        SetColor(T_FG,T_BG);
        NewLine();
        WritePrompt();
        // redraw currently typed input
        for(int i=0;i<input_len;i++) WriteChar(input_buf[i]);
    }
}

//  command execution
void TerminalApp::ExecuteInput(){
    if(command_pending || command_running) return;
    if(input_len==0){
        NewLine();
        WritePrompt();
        return;
    }

    // deduplicated history: skip if same as last entry
    bool is_dup = (hist_count > 0 && seq(history[hist_count-1], input_buf));
    if(!is_dup && hist_count < TERM_HIST_MAX){
        scpy(history[hist_count], input_buf, TERM_INPUT_MAX);
        hist_count++;
    }
    hist_pos=-1;
    hist_saved[0]=0;

    // reset tab state
    tab_match_count=0; tab_match_idx=-1;

    // echo input already visible; go to new line and queue execution so
    // the desktop loop can keep cycling outside the raw key callback.
    NewLine();
    scpy(pending_cmd, input_buf, TERM_INPUT_MAX);
    command_pending = true;

    // reset input immediately while the command is pending
    input_buf[0]=0; input_len=0; input_cursor=0;
    ScrollToBottom();
}

void TerminalApp::Tick(){
    if(!command_pending || command_running) return;

    command_running = true;
    command_pending = false;

    char output[SHELL_OUTPUT_BUF];
    output[0]=0;
    KuronoShell::Execute(pending_cmd, output, SHELL_OUTPUT_BUF);
    pending_cmd[0]=0;

    bool streamed = KuronoShell::TakeIncrementalOutputUsed();

    if(output[0] && !streamed){
        SetColor(T_FG, T_BG);
        Write(output);
        if(output[slen(output)-1]!='\n') NewLine();
    }

    command_running = false;
    ScrollToBottom();
    WritePrompt();
    // a non-streamed command writes its whole result into the buffer here on
    // the Shell thread (e.g. ls/pwd/cat/help); nothing in the input path saw
    // it, so raise the global signal or the gate delays it up to ~250ms.
    // (satoru, review fix)
    Graphics::MarkUIDirty();
}

bool TerminalApp::IsBusy(){
    return command_pending || command_running;
}

void TerminalApp::EnqueueCommand(const char* cmd){
    if(!cmd || !cmd[0]) return;
    if(command_pending || command_running) return;
    int i = 0;
    while(cmd[i] && i < TERM_INPUT_MAX - 1){
        pending_cmd[i] = cmd[i];
        i++;
    }
    pending_cmd[i] = 0;
    /* echo the command so users can see what's running, then queue it. */
    SetColor(T_FG, T_BG);
    Write(pending_cmd);
    NewLine();
    command_pending = true;
    ScrollToBottom();
}

void TerminalApp::HistoryUp(){
    if(hist_count==0) return;
    if(hist_pos<0){
        // save current in-progress input before navigating
        scpy(hist_saved, input_buf, TERM_INPUT_MAX);
        hist_pos=hist_count-1;
    } else if(hist_pos>0) {
        hist_pos--;
    }
    scpy(input_buf, history[hist_pos], TERM_INPUT_MAX);
    input_len=slen(input_buf);
    input_cursor=input_len;
}

void TerminalApp::HistoryDown(){
    if(hist_pos<0) return;
    hist_pos++;
    if(hist_pos>=hist_count){
        hist_pos=-1;
        // restore saved in-progress input
        scpy(input_buf, hist_saved, TERM_INPUT_MAX);
    } else {
        scpy(input_buf, history[hist_pos], TERM_INPUT_MAX);
    }
    input_len=slen(input_buf);
    input_cursor=input_len;
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

void TerminalApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;

    // keep theme + grid metrics current (font may finish loading after init,
    // and the theme can be re-themed live). (satoru)
    TermSyncTheme();
    EnsureMetrics();

    // ease scroll_offset toward target_scroll_offset (~3 frames to settle)
    if(scroll_offset != target_scroll_offset){
        int diff = target_scroll_offset - scroll_offset;
        int step = diff / 3;
        if(step == 0) step = (diff > 0) ? 1 : -1;
        scroll_offset += step;
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

    int prompt_end_col = cursor_col;
    int input_end_col = prompt_end_col + input_len;
    int input_visual_rows = input_end_col / vis_cols;
    int total_lines = cursor_row + input_visual_rows + 1;
    if(total_lines < buf_count) total_lines = buf_count;

    // calculate scroll to show latest lines + wrapped input
    int start_line = total_lines - vis_rows;
    if(start_line<0) start_line=0;
    start_line -= scroll_offset;
    if(start_line<0) start_line=0;

    // render buffer lines  -  batch consecutive same-bg cells into a single FillRect
    int sy=oy;
    char run_buf[TERM_COLS+1];
    for(int row=start_line; row<total_lines && sy+CELL_H<=oy+uh; row++){
        if(row<0||row>=TERM_SCROLL_BK) continue;
        int max_col = buffer[row].len;
        if(max_col > vis_cols) max_col = vis_cols;
        // bg pass: batched solid-color background fills
        int col = 0;
        while(col < max_col){
            unsigned int bg = buffer[row].cells[col].bg;
            if(bg == T_BG){ col++; continue; }
            int run_start = col;
            while(col < max_col && buffer[row].cells[col].bg == bg) col++;
            Graphics::FillRect(ox + run_start*CELL_W, sy,
                               (col - run_start) * CELL_W, CELL_H, bg);
        }
        // fg pass: batched consecutive same-color/bold text
        col = 0;
        while(col < max_col){
            char c0 = buffer[row].cells[col].ch;
            if(c0 <= ' '){ col++; continue; }
            unsigned int fg = buffer[row].cells[col].fg;
            bool bold = buffer[row].cells[col].bold;
            int run_start = col;
            int rb = 0;
            while(col < max_col && rb < TERM_COLS){
                TermCell* c = &buffer[row].cells[col];
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
    if(cursor_row<TERM_SCROLL_BK && cursor_row>=start_line){
        int prow = cursor_row;
        int draw_y = oy + (prow - start_line) * CELL_H;
        if(draw_y >= oy && draw_y+CELL_H <= oy+uh){
            // already rendered above
        }
    }

    // draw live input after the prompt; batch the longest contiguous run per row
    {
        int i = 0;
        char line_buf[TERM_COLS+1];
        while(i < input_len){
            int abs_col = prompt_end_col + i;
            int row_off = abs_col / vis_cols;
            int col = abs_col % vis_cols;
            int dx = ox + col*CELL_W;
            int dy = oy + ((cursor_row + row_off) - start_line) * CELL_H;
            int lb = 0;
            // gather until row wraps or input ends
            while(i < input_len && col < vis_cols && lb < TERM_COLS){
                line_buf[lb++] = input_buf[i++];
                col++;
            }
            line_buf[lb] = 0;
            if(lb > 0 && dy >= oy && dy + CELL_H <= oy + uh)
                TermDrawRun(dx, dy, line_buf, T_FG, false);
        }
    }

    // blinking cursor: pauses (always on) for ~500ms after a keypress, then blinks
    unsigned int now_ms = Timer::GetRealMs();
    unsigned int since_key = now_ms - last_keypress_ms;
    bool cursor_visible;
    if(since_key < 500) cursor_visible = true;
    else cursor_visible = (((now_ms - last_keypress_ms) / 500) % 2) == 0;
    if(cursor_visible){
        int abs_col = prompt_end_col + input_cursor;
        int cur_x = ox + (abs_col % vis_cols) * CELL_W;
        int cur_y = oy + ((cursor_row + (abs_col / vis_cols)) - start_line) * CELL_H;
        if(cur_y>=oy && cur_y+CELL_H<=oy+uh){
            // thin 2px wide blinking bar cursor in the accent color (satoru)
            Graphics::FillRect(cur_x, cur_y+2, 2, CELL_H-4, T_CURSOR);
        }
    }

    // The input cursor blinks on a time-driven 500ms cycle. When this terminal
    // is the focused window, self-sustain frames so the blink stays a clean
    // square wave instead of jittering on the 250ms gate fallback grid. Scoped
    // to focus so a background terminal still lets the GUI idle. (review fix)
    if (win_ptr && ((Window*)win_ptr)->focused) Graphics::MarkUIDirty();
}

//  input callback
bool TerminalApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)win_ptr; (void)mx; (void)my; (void)clicked;

    if(key==0) return false;
    last_keypress_ms = Timer::GetRealMs();
    if(command_pending || command_running){
        if(key==3){
            KuronoShell::RequestCommandCancel();
            SetColor(T_RED,T_BG);
            Write("^C");
            NewLine();
            SetColor(T_FG,T_BG);
            ScrollToBottom();
            KuronoShell::PumpUI();
            return true;
        }
        // Type-ahead: buffer printable chars so they appear after the
        // running command finishes and the prompt re-draws.
        if(key>=32 && key<127 && input_len<TERM_INPUT_MAX-1){
            for(int i=input_len;i>input_cursor;i--)
                input_buf[i]=input_buf[i-1];
            input_buf[input_cursor]=key;
            input_len++;
            input_cursor++;
            input_buf[input_len]=0;
            return true;
        }
        if((key==8 || key==127) && input_cursor>0){
            for(int i=input_cursor-1;i<input_len-1;i++)
                input_buf[i]=input_buf[i+1];
            input_len--;
            input_cursor--;
            input_buf[input_len]=0;
            return true;
        }
        return true;
    }

    if(scroll_offset < 0) scroll_offset = 0;

    // enter
    if(key=='\n' || key=='\r'){
        // write input text into the buffer so it's visible in scrollback
        for(int i=0;i<input_len;i++) WriteChar(input_buf[i]);
        ExecuteInput();
        return true;
    }

    // backspace
    if(key==8 || key==127){
        if(input_cursor>0){
            // shift left
            for(int i=input_cursor-1;i<input_len-1;i++)
                input_buf[i]=input_buf[i+1];
            input_len--;
            input_cursor--;
            input_buf[input_len]=0;
        }
        ScrollToBottom();
        return true;
    }

    // ctrl+l  -  clear
    if(key==12){
        Clear();
        input_buf[0]=0; input_len=0; input_cursor=0;
        WritePrompt();
        return true;
    }

    // ctrl+shift+c  -  copy the current input line to the clipboard. shift held
    // distinguishes it from the plain ctrl+c cancel below. (satoru)
    if(key==3 && Keyboard::GetState().shift){
        if(input_len>0){
            char saved=input_buf[input_len]; input_buf[input_len]=0;
            ClipboardManager::SetText(input_buf);
            input_buf[input_len]=saved;
        }
        return true;
    }

    // ctrl+shift+v  -  paste clipboard text into the input line at the cursor.
    // printable bytes only; a newline ends the paste without submitting. (satoru)
    if(key==22 && Keyboard::GetState().shift){
        if(ClipboardManager::HasText()){
            const char* s=ClipboardManager::GetText();
            for(int i=0;s[i] && s[i]!='\n' && s[i]!='\r';i++){
                char c=s[i];
                if(c<32 || c>=127) continue;
                if(input_len>=TERM_INPUT_MAX-1) break;
                for(int j=input_len;j>input_cursor;j--) input_buf[j]=input_buf[j-1];
                input_buf[input_cursor]=c;
                input_len++; input_cursor++;
                input_buf[input_len]=0;
            }
            tab_match_count=0;
            ScrollToBottom();
        }
        return true;
    }

    // ctrl+c  -  cancel current input
    if(key==3){
        SetColor(T_RED, T_BG);
        Write("^C");
        SetColor(T_FG, T_BG);
        NewLine();
        input_buf[0]=0; input_len=0; input_cursor=0;
        tab_match_count=0;
        WritePrompt();
        return true;
    }

    // ctrl+a  -  move to beginning of line
    if(key==1){ input_cursor=0; return true; }

    // ctrl+e  -  move to end of line
    if(key==5){ input_cursor=input_len; return true; }

    // ctrl+k  -  kill to end of line
    if(key==11){ input_buf[input_cursor]=0; input_len=input_cursor; return true; }

    // ctrl+u  -  kill to beginning of line
    if(key==21){
        for(int i=0;i<input_len-input_cursor;i++)
            input_buf[i]=input_buf[input_cursor+i];
        input_len-=input_cursor; input_cursor=0;
        input_buf[input_len]=0;
        return true;
    }

    // ctrl+w  -  delete word before cursor
    if(key==23){
        while(input_cursor>0 && input_buf[input_cursor-1]==' '){
            for(int i=input_cursor-1;i<input_len-1;i++) input_buf[i]=input_buf[i+1];
            input_len--; input_cursor--;
        }
        while(input_cursor>0 && input_buf[input_cursor-1]!=' '){
            for(int i=input_cursor-1;i<input_len-1;i++) input_buf[i]=input_buf[i+1];
            input_len--; input_cursor--;
        }
        input_buf[input_len]=0;
        return true;
    }

    // up arrow (escape sequence or raw code 0x48)
    if(key==(char)0x48){
        HistoryUp();
        ScrollToBottom();
        return true;
    }
    // down arrow
    if(key==(char)0x50){
        HistoryDown();
        ScrollToBottom();
        return true;
    }
    // left arrow
    if(key==(char)0x4B){
        if(input_cursor>0) input_cursor--;
        return true;
    }
    // right arrow
    if(key==(char)0x4D){
        if(input_cursor<input_len) input_cursor++;
        return true;
    }

    // home
    if(key==(char)0x47){ input_cursor=0; return true; }
    // end
    if(key==(char)0x4F){ input_cursor=input_len; return true; }
    // page up
    if(key==(char)0x49){
        scroll_offset+=10;
        int ms=(buf_count>25?buf_count-25:0);
        if(scroll_offset>ms) scroll_offset=ms;
        return true;
    }
    // page down
    if(key==(char)0x51){
        scroll_offset-=10;
        if(scroll_offset<0) scroll_offset=0;
        return true;
    }

    // delete
    if(key==(char)0x53){
        if(input_cursor<input_len){
            for(int i=input_cursor;i<input_len-1;i++)
                input_buf[i]=input_buf[i+1];
            input_len--;
            input_buf[input_len]=0;
        }
        return true;
    }

    // tab  -  tab completion via kvfs
    if(key=='\t'){
        TabComplete();
        ScrollToBottom();
        return true;
    }

    // printable character
    if(key>=32 && key<127 && input_len<TERM_INPUT_MAX-1){
        // insert at cursor
        for(int i=input_len;i>input_cursor;i--)
            input_buf[i]=input_buf[i-1];
        input_buf[input_cursor]=key;
        input_len++;
        input_cursor++;
        input_buf[input_len]=0;
        tab_match_count=0; // reset tab on new char
        ScrollToBottom();
        return true;
    }

    return false;
}
