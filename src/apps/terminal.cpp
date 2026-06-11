// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Terminal Emulator Application Implementation
// ═══════════════════════════════════════════════════════════════════════════
#include "terminal.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../shell/shell.h"
#include "../drivers/graphics.h"

// ────────────── colours ──────────────
static const unsigned int T_BG      = 0xFF0A0A18;
static const unsigned int T_FG      = 0xFFD0D0D0;
static const unsigned int T_PROMPT  = 0xFF3498DB;
static const unsigned int T_GREEN   = 0xFF2ECC71;
static const unsigned int T_RED     = 0xFFE74C3C;
static const unsigned int T_YELLOW  = 0xFFF1C40F;
static const unsigned int T_CURSOR  = 0xFFFFFFFF;
static const unsigned int T_SEL_BG  = 0xFF2C3E50;

static const int CELL_W = 8;   // pixels per character
static const int CELL_H = 16;  // pixels per line

// ────────────── helpers ──────────────
static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}

// ────────────── static data ──────────────
TermLine     TerminalApp::buffer[TERM_SCROLL_BK];
int          TerminalApp::buf_count     = 0;
int          TerminalApp::scroll_offset = 0;
int          TerminalApp::cursor_row    = 0;
int          TerminalApp::cursor_col    = 0;
unsigned int TerminalApp::cur_fg        = T_FG;
unsigned int TerminalApp::cur_bg        = T_BG;

char         TerminalApp::input_buf[TERM_INPUT_MAX];
int          TerminalApp::input_len     = 0;
int          TerminalApp::input_cursor  = 0;

char         TerminalApp::history[TERM_HIST_MAX][TERM_INPUT_MAX];
int          TerminalApp::hist_count    = 0;
int          TerminalApp::hist_pos      = -1;

bool         TerminalApp::shell_ready   = false;
char         TerminalApp::prompt[64];

// ═══════════════════════════════════════════════════════════════════════════
//  Init / Open
// ═══════════════════════════════════════════════════════════════════════════
void TerminalApp::Init(){
    buf_count=0; scroll_offset=0;
    cursor_row=0; cursor_col=0;
    cur_fg=T_FG; cur_bg=T_BG;
    input_len=0; input_cursor=0;
    hist_count=0; hist_pos=-1;
    shell_ready=true;

    // Clear buffer
    for(int i=0;i<TERM_SCROLL_BK;i++){
        buffer[i].len=0;
        for(int j=0;j<TERM_COLS;j++){
            buffer[i].cells[j].ch=' ';
            buffer[i].cells[j].fg=T_FG;
            buffer[i].cells[j].bg=T_BG;
        }
    }

    // Welcome banner
    SetColor(T_PROMPT, T_BG);
    WriteLn("╔══════════════════════════════════════════════════════════╗");
    WriteLn("║          Kurono OS Terminal — v1.0                      ║");
    WriteLn("║  Type 'help' for commands. Ctrl+L to clear.            ║");
    WriteLn("╚══════════════════════════════════════════════════════════╝");
    SetColor(T_FG, T_BG);
    WriteLn("");

    WritePrompt();
}

int TerminalApp::Open(){
    Init();
    // Create window via WM
    int wid = WindowManager::CreateWindow("Terminal", -1, -1, 648, 432,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TerminalApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TerminalApp::Input(w,p1,p2,true,0);
            else if(ev==2) TerminalApp::Input(w,0,0,false,(char)p1);
            else if(ev==3) {
                // Scroll event: p1 = scroll delta (positive = scroll up)
                if(p1 > 0) {
                    // Scroll up (show earlier content)
                    TerminalApp::scroll_offset += 3;
                    int max_scroll = TerminalApp::buf_count > 25 ? TerminalApp::buf_count - 25 : 0;
                    if(TerminalApp::scroll_offset > max_scroll)
                        TerminalApp::scroll_offset = max_scroll;
                } else if(p1 < 0) {
                    // Scroll down (towards latest)
                    TerminalApp::scroll_offset -= 3;
                    if(TerminalApp::scroll_offset < 0)
                        TerminalApp::scroll_offset = 0;
                }
            }
        }
    );
    return wid;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Terminal buffer write
// ═══════════════════════════════════════════════════════════════════════════
void TerminalApp::NewLine(){
    cursor_col=0;
    cursor_row++;
    if(cursor_row>=TERM_SCROLL_BK) cursor_row=TERM_SCROLL_BK-1;
    if(cursor_row>=buf_count) buf_count=cursor_row+1;
    // Clear the new row
    if(cursor_row<TERM_SCROLL_BK){
        buffer[cursor_row].len=0;
        for(int j=0;j<TERM_COLS;j++){
            buffer[cursor_row].cells[j].ch=' ';
            buffer[cursor_row].cells[j].fg=T_FG;
            buffer[cursor_row].cells[j].bg=T_BG;
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
    cursor_col++;
    if(cursor_col>buffer[cursor_row].len)
        buffer[cursor_row].len=cursor_col;
}

void TerminalApp::Write(const char* text){
    if(!text)return;
    for(int i=0;text[i];i++){
        // Handle escape sequences
        if(text[i]=='\x1b' && text[i+1]=='['){
            // Check for CLR (clear command)
            if(text[i+2]=='C' && text[i+3]=='L' && text[i+4]=='R'){
                Clear();
                WritePrompt();
                i+=4; // skip [CLR
                continue;
            }
            // Check for color codes: \x1b[33m (yellow), \x1b[0m (reset)
            int j=i+2;
            int code=0;
            while(text[j]>='0' && text[j]<='9'){ code=code*10+(text[j]-'0'); j++; }
            if(text[j]=='m'){
                switch(code){
                    case 0:  SetColor(T_FG, T_BG); break;   // reset
                    case 31: SetColor(T_RED, T_BG); break;   // red
                    case 32: SetColor(T_GREEN, T_BG); break; // green
                    case 33: SetColor(T_YELLOW, T_BG); break;// yellow
                    case 34: SetColor(T_PROMPT, T_BG); break;// blue
                    case 36: SetColor(0xFF1ABC9C, T_BG); break;// cyan
                }
                i=j;
                continue;
            }
            // Unknown escape - skip \x1b[
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
        }
    }
}

void TerminalApp::SetColor(unsigned int fg,unsigned int bg){
    cur_fg=fg; cur_bg=bg;
}

void TerminalApp::ScrollToBottom(){
    int vis_rows= 25; // approximate
    scroll_offset = (buf_count > vis_rows) ? buf_count - vis_rows : 0;
}

void TerminalApp::WritePrompt(){
    KuronoShell::GetPrompt(prompt, 64);
    SetColor(T_PROMPT, T_BG);
    Write(prompt);
    SetColor(T_FG, T_BG);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command execution
// ═══════════════════════════════════════════════════════════════════════════
void TerminalApp::ExecuteInput(){
    if(input_len==0){
        NewLine();
        WritePrompt();
        return;
    }

    // Record in local history
    if(hist_count<TERM_HIST_MAX){
        scpy(history[hist_count], input_buf, TERM_INPUT_MAX);
        hist_count++;
    }
    hist_pos=-1;

    // Echo input already visible; go to new line
    NewLine();

    // Execute through shell
    char output[SHELL_OUTPUT_BUF];
    output[0]=0;
    KuronoShell::Execute(input_buf, output, SHELL_OUTPUT_BUF);

    // Print output
    if(output[0]){
        SetColor(T_FG, T_BG);
        Write(output);
        if(output[slen(output)-1]!='\n') NewLine();
    }

    // Reset input
    input_buf[0]=0; input_len=0; input_cursor=0;
    ScrollToBottom();
    WritePrompt();
}

void TerminalApp::HistoryUp(){
    if(hist_count==0) return;
    if(hist_pos<0) hist_pos=hist_count-1;
    else if(hist_pos>0) hist_pos--;
    scpy(input_buf, history[hist_pos], TERM_INPUT_MAX);
    input_len=slen(input_buf);
    input_cursor=input_len;
}

void TerminalApp::HistoryDown(){
    if(hist_pos<0) return;
    hist_pos++;
    if(hist_pos>=hist_count){
        hist_pos=-1;
        input_buf[0]=0; input_len=0; input_cursor=0;
    } else {
        scpy(input_buf, history[hist_pos], TERM_INPUT_MAX);
        input_len=slen(input_buf);
        input_cursor=input_len;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Rendering callback
// ═══════════════════════════════════════════════════════════════════════════
void TerminalApp::RenderCell(int sx,int sy,TermCell* cell){
    if(cell->bg!=T_BG){
        Graphics::FillRect(sx,sy,CELL_W,CELL_H,cell->bg);
    }
    if(cell->ch>' '){
        char s[2]={cell->ch,0};
        Graphics::DrawString(sx,sy,s,cell->fg,0xFF000000);
    }
}

void TerminalApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;

    // Fill background
    Graphics::FillRect(cx,cy,cw,ch,T_BG);

    int vis_rows = ch / CELL_H;
    int vis_cols = cw / CELL_W;
    if(vis_cols>TERM_COLS) vis_cols=TERM_COLS;

    // Calculate scroll to show latest lines + input
    int total_lines = buf_count;
    int start_line = total_lines - vis_rows + 1; // +1 for input line
    if(start_line<0) start_line=0;
    start_line -= scroll_offset;
    if(start_line<0) start_line=0;

    // Render buffer lines
    int sy=cy;
    for(int row=start_line; row<total_lines && sy+CELL_H<=cy+ch-CELL_H; row++){
        if(row<0||row>=TERM_SCROLL_BK) continue;
        for(int col=0;col<vis_cols && col<buffer[row].len;col++){
            RenderCell(cx+col*CELL_W, sy, &buffer[row].cells[col]);
        }
        sy+=CELL_H;
    }

    // Render the current input line at the bottom portion
    // First render what's in the current buffer row (prompt text)
    if(cursor_row<TERM_SCROLL_BK && cursor_row>=start_line){
        int prow = cursor_row;
        int draw_y = cy + (prow - start_line) * CELL_H;
        if(draw_y >= cy && draw_y+CELL_H <= cy+ch){
            // Already rendered above
        }
    }

    // Render the input buffer after prompt
    int input_y = sy;
    if(input_y+CELL_H>cy+ch) input_y=cy+ch-CELL_H;

    // Show current prompt + input on the active line
    // The prompt was written via Write() calls into buffer[cursor_row]
    // Now draw the user-typed text right after cursor_col of the buffer
    int prompt_end_col = cursor_col;
    for(int i=0;i<input_len && prompt_end_col+i<vis_cols;i++){
        char s[2]={input_buf[i],0};
        // Determine row position for this — it's on the current cursor row
        int dx=cx+(prompt_end_col+i)*CELL_W;
        int dy=cy+(cursor_row-start_line)*CELL_H;
        if(dy>=cy && dy+CELL_H<=cy+ch){
            Graphics::DrawString(dx,dy,s,T_FG,0xFF000000);
        }
    }

    // Blinking cursor
    unsigned int ticks = 0; // Time::GetTicks() — use if available
    bool cursor_visible = true; //(ticks / 500) % 2 == 0;
    if(cursor_visible){
        int cur_x = cx + (prompt_end_col + input_cursor) * CELL_W;
        int cur_y = cy + (cursor_row - start_line) * CELL_H;
        if(cur_y>=cy && cur_y+CELL_H<=cy+ch){
            Graphics::FillRect(cur_x, cur_y, 2, CELL_H, T_CURSOR);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Input callback
// ═══════════════════════════════════════════════════════════════════════════
bool TerminalApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)win_ptr; (void)mx; (void)my; (void)clicked;

    if(key==0) return false;

    // Enter
    if(key=='\n' || key=='\r'){
        // Write input text into the buffer so it's visible in scrollback
        for(int i=0;i<input_len;i++) WriteChar(input_buf[i]);
        ExecuteInput();
        return true;
    }

    // Backspace
    if(key==8 || key==127){
        if(input_cursor>0){
            // Shift left
            for(int i=input_cursor-1;i<input_len-1;i++)
                input_buf[i]=input_buf[i+1];
            input_len--;
            input_cursor--;
            input_buf[input_len]=0;
        }
        return true;
    }

    // Ctrl+L — clear
    if(key==12){
        Clear();
        input_buf[0]=0; input_len=0; input_cursor=0;
        WritePrompt();
        return true;
    }

    // Ctrl+C — cancel current input
    if(key==3){
        SetColor(T_RED, T_BG);
        Write("^C");
        SetColor(T_FG, T_BG);
        NewLine();
        input_buf[0]=0; input_len=0; input_cursor=0;
        WritePrompt();
        return true;
    }

    // Up arrow (escape sequence or raw code 0x48)
    if(key==(char)0x48){
        HistoryUp();
        return true;
    }
    // Down arrow
    if(key==(char)0x50){
        HistoryDown();
        return true;
    }
    // Left arrow
    if(key==(char)0x4B){
        if(input_cursor>0) input_cursor--;
        return true;
    }
    // Right arrow
    if(key==(char)0x4D){
        if(input_cursor<input_len) input_cursor++;
        return true;
    }

    // Home
    if(key==(char)0x47){ input_cursor=0; return true; }
    // End
    if(key==(char)0x4F){ input_cursor=input_len; return true; }

    // Delete
    if(key==(char)0x53){
        if(input_cursor<input_len){
            for(int i=input_cursor;i<input_len-1;i++)
                input_buf[i]=input_buf[i+1];
            input_len--;
            input_buf[input_len]=0;
        }
        return true;
    }

    // Tab — simple filename completion placeholder
    if(key=='\t'){
        // TODO: implement tab completion via KVFS
        return true;
    }

    // Printable character
    if(key>=32 && key<127 && input_len<TERM_INPUT_MAX-1){
        // Insert at cursor
        for(int i=input_len;i>input_cursor;i--)
            input_buf[i]=input_buf[i-1];
        input_buf[input_cursor]=key;
        input_len++;
        input_cursor++;
        input_buf[input_len]=0;
        return true;
    }

    return false;
}
