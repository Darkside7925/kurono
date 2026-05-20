//  kurono os  -  text editor application implementation
#include "text_editor.h"
#include "../ui/window_manager.h"
#include "../fs/kvfs.h"
#include "../drivers/graphics.h"
#include "../system/logging.h"

static const unsigned int ED_BG        = 0xFF0A0A18;
static const unsigned int ED_GUTTER    = 0xFF12122A;
static const unsigned int ED_LINE_NUM  = 0xFF555588;
static const unsigned int ED_CUR_LINE  = 0xFF141430;
static const unsigned int ED_TEXT      = 0xFFD4D4D4;
static const unsigned int ED_KEYWORD   = 0xFF569CD6;
static const unsigned int ED_STRING    = 0xFFCE9178;
static const unsigned int ED_COMMENT   = 0xFF6A9955;
static const unsigned int ED_CURSOR    = 0xFFFFFFFF;
static const unsigned int ED_SEL_BG    = 0xFF264F78;
static const unsigned int ED_MENU_BG   = 0xFF1A1A2E;
static const unsigned int ED_MENU_TXT  = 0xFFCCCCCC;
static const unsigned int ED_STATUS_BG = 0xFF007ACC;
static const unsigned int ED_STATUS_TX = 0xFFFFFFFF;
static const unsigned int ED_BORDER    = 0xFF333355;
static const unsigned int ED_MODIFIED  = 0xFFE74C3C;

static const int CHAR_W = 8;
static const int CHAR_H = 16;
static const int GUTTER_W = 48;
static const int MENU_H = 24;
static const int STATUS_H = 20;

static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static void int_to_str(int v,char*b,int mx){
    if(mx<2){b[0]=0;return;}if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}

char       TextEditorApp::file_path[ED_PATH_MAX] = "";
EditorLine TextEditorApp::lines[ED_MAX_LINES];
int        TextEditorApp::line_count      = 1;
int        TextEditorApp::cursor_row      = 0;
int        TextEditorApp::cursor_col      = 0;
int        TextEditorApp::scroll_x        = 0;
int        TextEditorApp::scroll_y        = 0;
int        TextEditorApp::sel_start_row   = 0;
int        TextEditorApp::sel_start_col   = 0;
int        TextEditorApp::sel_end_row     = 0;
int        TextEditorApp::sel_end_col     = 0;
bool       TextEditorApp::has_selection   = false;
bool       TextEditorApp::modified        = false;
bool       TextEditorApp::show_line_numbers = true;
int        TextEditorApp::menu_open_idx   = -1;
int        TextEditorApp::menu_x          = 0;
int        TextEditorApp::menu_y          = 0;

//  init / open
void TextEditorApp::Init(){
    file_path[0]=0;
    line_count=1;
    cursor_row=0; cursor_col=0;
    scroll_x=0; scroll_y=0;
    has_selection=false;
    modified=false;
    show_line_numbers=true;

    for(int i=0;i<ED_MAX_LINES;i++){
        lines[i].text[0]=0;
        lines[i].len=0;
    }
}

int TextEditorApp::Open(){
    Init();
    RuntimeLog::LogAppEvent("editor", "open");
    int wid = WindowManager::CreateWindow("Text Editor", -1, -1, 640, 480,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TextEditorApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TextEditorApp::Input(w,p1,p2,true,0);
            else if(ev==2) TextEditorApp::Input(w,0,0,false,(char)p1);
        }
    );
    return wid;
}

int TextEditorApp::OpenFile(const char* path){
    Init();
    LoadFile(path);
    RuntimeLog::LogAppEvent("editor", "open-file", path);
    int wid = WindowManager::CreateWindow("Text Editor", -1, -1, 640, 480,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TextEditorApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TextEditorApp::Input(w,p1,p2,true,0);
            else if(ev==2) TextEditorApp::Input(w,0,0,false,(char)p1);
        }
    );
    return wid;
}

//  file i/o
bool TextEditorApp::LoadFile(const char* path){
    scpy(file_path, path, ED_PATH_MAX);
    char content[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(path, content, KVFS_MAX_CONTENT);
    if(sz<=0){
        line_count=1;
        lines[0].text[0]=0; lines[0].len=0;
        return false;
    }

    // parse into lines
    line_count=0;
    int col=0;
    for(int i=0;i<sz && line_count<ED_MAX_LINES;i++){
        if(content[i]=='\n'){
            lines[line_count].text[col]=0;
            lines[line_count].len=col;
            line_count++;
            col=0;
        } else if(content[i]!='\r'){
            if(col<ED_LINE_MAX-1){
                lines[line_count].text[col++]=content[i];
            }
        }
    }
    // last line if no trailing newline
    if(col>0 || line_count==0){
        lines[line_count].text[col]=0;
        lines[line_count].len=col;
        line_count++;
    }

    cursor_row=0; cursor_col=0;
    scroll_x=0; scroll_y=0;
    modified=false;
    return true;
}

bool TextEditorApp::SaveFile(){
    if(file_path[0]==0) return false;
    return SaveFileAs(file_path);
}

bool TextEditorApp::SaveFileAs(const char* path){
    // assemble content
    char content[KVFS_MAX_CONTENT];
    int pos=0;
    for(int i=0;i<line_count && pos<KVFS_MAX_CONTENT-2;i++){
        for(int j=0;j<lines[i].len && pos<KVFS_MAX_CONTENT-2;j++){
            content[pos++]=lines[i].text[j];
        }
        if(i<line_count-1) content[pos++]='\n';
    }
    content[pos]=0;

    int err = KVFS::WriteString(path, content);
    if(err==KVFS_OK){
        scpy(file_path, path, ED_PATH_MAX);
        modified=false;
        RuntimeLog::LogAppEvent("editor", "save-file", path);
        return true;
    }
    return false;
}

//  editing operations
void TextEditorApp::InsertChar(char c){
    if(cursor_row<0||cursor_row>=line_count) return;
    EditorLine* ln=&lines[cursor_row];
    if(ln->len>=ED_LINE_MAX-1) return;

    // insert at cursor position
    for(int i=ln->len;i>cursor_col;i--)
        ln->text[i]=ln->text[i-1];
    ln->text[cursor_col]=c;
    ln->len++;
    ln->text[ln->len]=0;
    cursor_col++;
    modified=true;
}

void TextEditorApp::InsertNewline(){
    if(line_count>=ED_MAX_LINES) return;

    // shift lines down
    for(int i=line_count;i>cursor_row+1;i--)
        lines[i]=lines[i-1];
    line_count++;

    // split current line at cursor
    EditorLine* cur=&lines[cursor_row];
    EditorLine* next=&lines[cursor_row+1];
    int rest=cur->len - cursor_col;
    for(int i=0;i<rest;i++)
        next->text[i]=cur->text[cursor_col+i];
    next->text[rest]=0;
    next->len=rest;

    cur->text[cursor_col]=0;
    cur->len=cursor_col;

    cursor_row++;
    cursor_col=0;
    modified=true;
}

void TextEditorApp::DeleteChar(){
    // delete character at cursor (like del key)
    if(cursor_row<0||cursor_row>=line_count) return;
    EditorLine* ln=&lines[cursor_row];
    if(cursor_col<ln->len){
        for(int i=cursor_col;i<ln->len-1;i++)
            ln->text[i]=ln->text[i+1];
        ln->len--;
        ln->text[ln->len]=0;
        modified=true;
    } else if(cursor_row<line_count-1){
        // join with next line
        EditorLine* next=&lines[cursor_row+1];
        int space=ED_LINE_MAX-1-ln->len;
        int copy=next->len; if(copy>space) copy=space;
        for(int i=0;i<copy;i++)
            ln->text[ln->len+i]=next->text[i];
        ln->len+=copy;
        ln->text[ln->len]=0;
        DeleteLine(cursor_row+1);
        modified=true;
    }
}

void TextEditorApp::Backspace(){
    if(cursor_col>0){
        cursor_col--;
        DeleteChar();
    } else if(cursor_row>0){
        cursor_col=lines[cursor_row-1].len;
        cursor_row--;
        DeleteChar();
    }
}

void TextEditorApp::DeleteLine(int row){
    if(row<0||row>=line_count) return;
    for(int i=row;i<line_count-1;i++)
        lines[i]=lines[i+1];
    line_count--;
    if(line_count==0){
        line_count=1;
        lines[0].text[0]=0; lines[0].len=0;
    }
}

//  cursor movement
void TextEditorApp::MoveCursorUp(){
    if(cursor_row>0){
        cursor_row--;
        if(cursor_col>lines[cursor_row].len)
            cursor_col=lines[cursor_row].len;
    }
}
void TextEditorApp::MoveCursorDown(){
    if(cursor_row<line_count-1){
        cursor_row++;
        if(cursor_col>lines[cursor_row].len)
            cursor_col=lines[cursor_row].len;
    }
}
void TextEditorApp::MoveCursorLeft(){
    if(cursor_col>0) cursor_col--;
    else if(cursor_row>0){ cursor_row--; cursor_col=lines[cursor_row].len; }
}
void TextEditorApp::MoveCursorRight(){
    if(cursor_col<lines[cursor_row].len) cursor_col++;
    else if(cursor_row<line_count-1){ cursor_row++; cursor_col=0; }
}
void TextEditorApp::MoveCursorHome(){ cursor_col=0; }
void TextEditorApp::MoveCursorEnd(){ cursor_col=lines[cursor_row].len; }
void TextEditorApp::PageUp(int vr){
    cursor_row-=vr; if(cursor_row<0) cursor_row=0;
    if(cursor_col>lines[cursor_row].len) cursor_col=lines[cursor_row].len;
}
void TextEditorApp::PageDown(int vr){
    cursor_row+=vr; if(cursor_row>=line_count) cursor_row=line_count-1;
    if(cursor_col>lines[cursor_row].len) cursor_col=lines[cursor_row].len;
}

//  rendering
void TextEditorApp::RenderMenuBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,MENU_H,ED_MENU_BG);
    Graphics::DrawLine(x,y+MENU_H,x+w,y+MENU_H,ED_BORDER);

    // menu items  -  highlight the open menu
    unsigned int file_clr = (menu_open_idx == 0) ? ED_SEL_BG : ED_MENU_BG;
    unsigned int edit_clr = (menu_open_idx == 1) ? ED_SEL_BG : ED_MENU_BG;
    unsigned int view_clr = (menu_open_idx == 2) ? ED_SEL_BG : ED_MENU_BG;
    Graphics::FillRect(x+6, y+2, 40, MENU_H-4, file_clr);
    Graphics::FillRect(x+46, y+2, 40, MENU_H-4, edit_clr);
    Graphics::FillRect(x+86, y+2, 40, MENU_H-4, view_clr);
    Graphics::DrawString(x+8,  y+4, "File", ED_MENU_TXT, 0xFF000000);
    Graphics::DrawString(x+48, y+4, "Edit", ED_MENU_TXT, 0xFF000000);
    Graphics::DrawString(x+88, y+4, "View", ED_MENU_TXT, 0xFF000000);

    // title / filename
    if(file_path[0]){
        int last=slen(file_path)-1;
        while(last>0 && file_path[last]!='/') last--;
        const char* fname = &file_path[last ? last+1 : 0];
        char title[64]; scpy(title, fname, 60);
        if(modified) sapp(title, " *", 64);
        int tw=slen(title)*8;
        Graphics::DrawString(x+w/2-tw/2, y+4, title, ED_STATUS_TX, 0xFF000000);
    } else {
        const char* t = modified ? "Untitled *" : "Untitled";
        int tw=slen(t)*8;
        Graphics::DrawString(x+w/2-tw/2, y+4, t, ED_STATUS_TX, 0xFF000000);
    }
}

void TextEditorApp::RenderMenuDropdown(int ox,int oy){
    if(menu_open_idx < 0 || menu_open_idx > 2) return;

    static const char* file_items[] = {"Save  (Ctrl+S)", "Save As...", "Open...", nullptr};
    static const char* edit_items[] = {"Toggle Line Numbers  (Ctrl+L)", "Go to Top  (Ctrl+G)", nullptr};
    static const char* view_items[] = {"Toggle Line Numbers", nullptr};
    const char** items = nullptr;
    if(menu_open_idx == 0) items = file_items;
    else if(menu_open_idx == 1) items = edit_items;
    else items = view_items;

    int count = 0;
    while(items[count]) count++;
    if(count == 0) return;

    int dw = 220;
    int dh = count * 22 + 4;
    int dx = ox + menu_x;
    int dy = oy + menu_y;

    Graphics::FillRoundedRect(dx, dy, dw, dh, 4, 0xFF1E1E32);
    Graphics::DrawRect(dx, dy, dw, dh, ED_BORDER);

    for(int i=0;i<count;i++){
        int iy = dy + 2 + i * 22;
        Graphics::DrawString(dx+8, iy+3, items[i], ED_MENU_TXT, 0xFF000000);
    }
}

void TextEditorApp::RenderGutter(int x,int y,int h,int vis_rows){
    Graphics::FillRect(x,y,GUTTER_W,h,ED_GUTTER);
    Graphics::DrawLine(x+GUTTER_W-1,y,x+GUTTER_W-1,y+h,ED_BORDER);

    for(int i=0;i<vis_rows && (i+scroll_y)<line_count;i++){
        int ln_num = i + scroll_y + 1;
        char num[8]; int_to_str(ln_num, num, 8);
        int tw=slen(num)*8;
        unsigned int clr = ((i+scroll_y)==cursor_row) ? ED_TEXT : ED_LINE_NUM;
        Graphics::DrawString(x+GUTTER_W-4-tw, y+i*CHAR_H, num, clr, 0xFF000000);
    }
}

void TextEditorApp::RenderContent(int x,int y,int w,int h){
    int vis_rows = h / CHAR_H;
    int vis_cols = w / CHAR_W;

    // ensure cursor is visible
    if(cursor_row < scroll_y) scroll_y = cursor_row;
    if(cursor_row >= scroll_y + vis_rows) scroll_y = cursor_row - vis_rows + 1;
    if(cursor_col < scroll_x) scroll_x = cursor_col;
    if(cursor_col >= scroll_x + vis_cols) scroll_x = cursor_col - vis_cols + 1;

    for(int i=0;i<vis_rows && (i+scroll_y)<line_count;i++){
        int row_idx = i + scroll_y;
        int ry = y + i * CHAR_H;

        // current line highlight
        if(row_idx==cursor_row){
            Graphics::FillRect(x, ry, w, CHAR_H, ED_CUR_LINE);
        }

        EditorLine* ln = &lines[row_idx];
        for(int j=scroll_x; j<ln->len && (j-scroll_x)<vis_cols; j++){
            char s[2]={ln->text[j],0};
            int dx=x+(j-scroll_x)*CHAR_W;
            Graphics::DrawString(dx, ry, s, ED_TEXT, 0xFF000000);
        }
    }

    // cursor
    int cx = x + (cursor_col - scroll_x) * CHAR_W;
    int cy_c = y + (cursor_row - scroll_y) * CHAR_H;
    if(cx>=x && cx<x+w && cy_c>=y && cy_c<y+h){
        Graphics::FillRect(cx, cy_c, 2, CHAR_H, ED_CURSOR);
    }
}

void TextEditorApp::RenderStatusBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,STATUS_H, modified ? ED_MODIFIED : ED_STATUS_BG);

    // position info
    char pos[32]={0};
    sapp(pos,"Ln ",32);
    char n[8]; int_to_str(cursor_row+1,n,8); sapp(pos,n,32);
    sapp(pos,", Col ",32);
    int_to_str(cursor_col+1,n,8); sapp(pos,n,32);
    Graphics::DrawString(x+8, y+3, pos, ED_STATUS_TX, 0xFF000000);

    // line count
    char lc[32]={0};
    int_to_str(line_count,n,8); sapp(lc,n,32);
    sapp(lc," lines",32);
    int tw=slen(lc)*8;
    Graphics::DrawString(x+w-tw-8, y+3, lc, ED_STATUS_TX, 0xFF000000);

    // mode
    Graphics::DrawString(x+w/2-16, y+3, "UTF-8", ED_STATUS_TX, 0xFF000000);
}

void TextEditorApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;
    Graphics::FillRect(cx,cy,cw,ch,ED_BG);

    int y=cy;
    RenderMenuBar(cx,y,cw);  y+=MENU_H+1;

    // Draw dropdown on top of everything if open
    RenderMenuDropdown(cx, cy);

    int content_h = ch - MENU_H - 1 - STATUS_H;
    int vis_rows = content_h / CHAR_H;

    if(show_line_numbers){
        RenderGutter(cx, y, content_h, vis_rows);
        RenderContent(cx+GUTTER_W, y, cw-GUTTER_W, content_h);
    } else {
        RenderContent(cx, y, cw, content_h);
    }

    RenderStatusBar(cx, cy+ch-STATUS_H, cw);
}

//  input
bool TextEditorApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)win_ptr;

    // mouse click
    if(clicked){
        // If menu dropdown is open, check dropdown items first
        if(menu_open_idx >= 0){
            int count = 0;
            if(menu_open_idx == 0) count = 3;  // File: Save, Save As, Open
            else if(menu_open_idx == 1) count = 2;  // Edit: Line Numbers, Go Top
            else count = 1;  // View: Toggle Line Numbers

            int dw = 220;
            int dh = count * 22 + 4;
            if(mx >= menu_x && mx < menu_x + dw &&
               my >= menu_y && my < menu_y + dh){
                int item = (my - menu_y - 2) / 22;
                if(item >= 0 && item < count){
                    menu_open_idx = -1;
                    // Dispatch based on which menu was open
                    if(count == 3){  // File menu
                        if(item == 0) SaveFile();
                        else if(item == 2) { /* Open  -  placeholder */ }
                    } else if(count == 2){  // Edit menu
                        if(item == 0) show_line_numbers = !show_line_numbers;
                    } else {  // View menu
                        if(item == 0) show_line_numbers = !show_line_numbers;
                    }
                    return true;
                }
            }
            menu_open_idx = -1;
            return true;
        }

        // Check menu bar clicks (File/Edit/View)
        if(my >= 0 && my < MENU_H){
            if(mx >= 6 && mx < 48){ menu_open_idx = 0; menu_x = 6; menu_y = MENU_H; return true; }
            if(mx >= 46 && mx < 88){ menu_open_idx = 1; menu_x = 46; menu_y = MENU_H; return true; }
            if(mx >= 86 && mx < 128){ menu_open_idx = 2; menu_x = 86; menu_y = MENU_H; return true; }
        }

        // mx, my are already content-local (0,0 = top-left of content area)
        int text_y = MENU_H + 1;
        int text_x = (show_line_numbers ? GUTTER_W : 0);

        if(mx >= text_x && my >= text_y){
            int col = (mx - text_x) / CHAR_W + scroll_x;
            int row = (my - text_y) / CHAR_H + scroll_y;
            if(row>=0 && row<line_count){
                cursor_row=row;
                cursor_col=col;
                if(cursor_col>lines[cursor_row].len)
                    cursor_col=lines[cursor_row].len;
            }
            menu_open_idx = -1;
            return true;
        }

        menu_open_idx = -1;
        return true;
    }

    if(key==0) return false;

    // enter
    if(key=='\n'||key=='\r'){ InsertNewline(); return true; }

    // backspace
    if(key==8||key==127){ Backspace(); return true; }

    // tab
    if(key=='\t'){
        for(int i=0;i<4;i++) InsertChar(' ');
        return true;
    }

    // ctrl+s  -  save
    if(key==19){ SaveFile(); return true; }

    // ctrl+l  -  toggle line numbers
    if(key==12){ show_line_numbers=!show_line_numbers; return true; }

    // ctrl+g  -  go to line (just go to top as simple impl)
    if(key==7){ cursor_row=0; cursor_col=0; return true; }

    // arrows
    if(key==(char)0x48){ MoveCursorUp(); return true; }
    if(key==(char)0x50){ MoveCursorDown(); return true; }
    if(key==(char)0x4B){ MoveCursorLeft(); return true; }
    if(key==(char)0x4D){ MoveCursorRight(); return true; }

    // home/end
    if(key==(char)0x47){ MoveCursorHome(); return true; }
    if(key==(char)0x4F){ MoveCursorEnd(); return true; }

    // delete
    if(key==(char)0x53){ DeleteChar(); return true; }

    // page up/down
    if(key==(char)0x49){ PageUp(20); return true; }
    if(key==(char)0x51){ PageDown(20); return true; }

    // printable
    if(key>=32 && key<127){ InsertChar(key); return true; }

    return false;
}
