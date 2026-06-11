// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — File Manager Application Implementation
// ═══════════════════════════════════════════════════════════════════════════
#include "file_manager.h"
#include "../ui/window_manager.h"
#include "../fs/kvfs.h"
#include "../drivers/graphics.h"

// ────────────── colours ──────────────
static const unsigned int FM_BG        = 0xFF0F0F1A;
static const unsigned int FM_TOOLBAR   = 0xFF16162B;
static const unsigned int FM_PATH_BG   = 0xFF1A1A2E;
static const unsigned int FM_SEL       = 0xFF2C3E50;
static const unsigned int FM_HOVER     = 0xFF1E1E35;
static const unsigned int FM_TEXT      = 0xFFD0D0D0;
static const unsigned int FM_DIM       = 0xFF888888;
static const unsigned int FM_BLUE      = 0xFF3498DB;
static const unsigned int FM_GREEN     = 0xFF2ECC71;
static const unsigned int FM_AMBER     = 0xFFF39C12;
static const unsigned int FM_WHITE     = 0xFFFFFFFF;
static const unsigned int FM_BTN       = 0xFF2C3E50;
static const unsigned int FM_BTN_HVR   = 0xFF34495E;
static const unsigned int FM_BORDER    = 0xFF333355;
static const unsigned int FM_STATUS    = 0xFF0D0D18;

static const int ROW_H = 24;
static const int GRID_CELL = 80;

// ────────────── helpers ──────────────
static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static bool seq(const char*a,const char*b){
    int i=0;while(a[i]&&b[i]){if(a[i]!=b[i])return false;i++;}return a[i]==b[i];}
static void int_to_str(int v,char*b,int mx){
    if(mx<2){b[0]=0;return;}if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}

// ────────────── static data ──────────────
char        FileManagerApp::current_path[FM_MAX_PATH] = "/home/user";
FMEntry     FileManagerApp::entries[FM_MAX_ENTRIES];
int         FileManagerApp::entry_count   = 0;
int         FileManagerApp::selected_index = -1;
int         FileManagerApp::scroll_offset  = 0;
FMViewMode  FileManagerApp::view_mode      = FM_VIEW_LIST;
bool        FileManagerApp::show_hidden    = false;
bool        FileManagerApp::context_menu_open = false;
int         FileManagerApp::context_menu_x = 0;
int         FileManagerApp::context_menu_y = 0;
int         FileManagerApp::context_menu_idx = -1;
char        FileManagerApp::clipboard_path[FM_MAX_PATH] = "";
char        FileManagerApp::clipboard_name[FM_NAME_MAX] = "";
bool        FileManagerApp::clipboard_has_item = false;
bool        FileManagerApp::rename_mode = false;
char        FileManagerApp::rename_buf[FM_NAME_MAX] = "";
int         FileManagerApp::rename_cursor = 0;

// ═══════════════════════════════════════════════════════════════════════════
//  Init / Open
// ═══════════════════════════════════════════════════════════════════════════
void FileManagerApp::Init(){
    scpy(current_path, "/home/user", FM_MAX_PATH);
    entry_count=0; selected_index=-1; scroll_offset=0;
    view_mode=FM_VIEW_LIST; show_hidden=false;
    Refresh();
}

int FileManagerApp::Open(){
    Init();
    int wid = WindowManager::CreateWindow("Files", -1, -1, 600, 420,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            FileManagerApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) FileManagerApp::Input(w,p1,p2,true,0);
            else if(ev==2) FileManagerApp::Input(w,0,0,false,(char)p1);
        }
    );
    return wid;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Navigation
// ═══════════════════════════════════════════════════════════════════════════
void FileManagerApp::Refresh(){
    entry_count=0;
    selected_index=-1;
    scroll_offset=0;

    // Read directory from KVFS
    KVFSNode* dir = KVFS::ResolvePath(current_path);
    if(!dir || dir->type != KVFS_DIR) return;

    for(int i=0;i<dir->child_count && entry_count<FM_MAX_ENTRIES;i++){
        KVFSNode* child = dir->children[i];
        if(!child) continue;
        // Skip hidden files (starting with '.')
        if(!show_hidden && child->name[0]=='.') continue;

        FMEntry* e = &entries[entry_count];
        scpy(e->name, child->name, FM_NAME_MAX);
        e->is_dir = (child->type == KVFS_DIR);
        e->size = child->size;
        e->permissions = child->perms.mode;
        e->selected = false;
        entry_count++;
    }

    // Sort: directories first, then alphabetical
    for(int i=0;i<entry_count-1;i++){
        for(int j=i+1;j<entry_count;j++){
            bool swap=false;
            if(!entries[i].is_dir && entries[j].is_dir) swap=true;
            else if(entries[i].is_dir==entries[j].is_dir){
                // Alpha compare
                for(int k=0;;k++){
                    char a=entries[i].name[k], b=entries[j].name[k];
                    if(a>='A'&&a<='Z') a+=32;
                    if(b>='A'&&b<='Z') b+=32;
                    if(a>b){swap=true;break;}
                    if(a<b) break;
                    if(!a) break;
                }
            }
            if(swap){
                FMEntry tmp=entries[i];
                entries[i]=entries[j];
                entries[j]=tmp;
            }
        }
    }
}

void FileManagerApp::NavigateTo(const char* path){
    scpy(current_path, path, FM_MAX_PATH);
    Refresh();
}

void FileManagerApp::GoUp(){
    // Find last '/'
    int len=slen(current_path);
    if(len<=1) return; // already at root
    int last=len-1;
    if(current_path[last]=='/') last--;
    while(last>0 && current_path[last]!='/') last--;
    if(last==0) { current_path[0]='/'; current_path[1]=0; }
    else { current_path[last]=0; }
    Refresh();
}

void FileManagerApp::GoHome(){
    scpy(current_path, "/home/user", FM_MAX_PATH);
    Refresh();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Actions
// ═══════════════════════════════════════════════════════════════════════════
void FileManagerApp::OpenEntry(int idx){
    if(idx<0||idx>=entry_count) return;
    FMEntry* e=&entries[idx];
    if(e->is_dir){
        char newpath[FM_MAX_PATH];
        scpy(newpath, current_path, FM_MAX_PATH);
        if(!seq(current_path,"/")) sapp(newpath,"/",FM_MAX_PATH);
        sapp(newpath, e->name, FM_MAX_PATH);
        NavigateTo(newpath);
    }
    // For files, could open in text editor — placeholder
}

void FileManagerApp::DeleteEntry(int idx){
    if(idx<0||idx>=entry_count) return;
    char fullpath[FM_MAX_PATH];
    scpy(fullpath, current_path, FM_MAX_PATH);
    if(!seq(current_path,"/")) sapp(fullpath,"/",FM_MAX_PATH);
    sapp(fullpath, entries[idx].name, FM_MAX_PATH);
    KVFS::Unlink(fullpath);
    Refresh();
}

void FileManagerApp::CreateFolder(){
    char path[FM_MAX_PATH];
    scpy(path, current_path, FM_MAX_PATH);
    sapp(path, "/New Folder", FM_MAX_PATH);
    KVFS::Mkdir(path);
    Refresh();
}

void FileManagerApp::CreateFile(){
    char path[FM_MAX_PATH];
    scpy(path, current_path, FM_MAX_PATH);
    sapp(path, "/untitled.txt", FM_MAX_PATH);
    KVFS::CreateFile(path);
    Refresh();
}

void FileManagerApp::CopyEntry(int idx){
    if(idx<0 || idx>=entry_count) return;
    scpy(clipboard_path, current_path, FM_MAX_PATH);
    if(!seq(current_path, "/")) sapp(clipboard_path, "/", FM_MAX_PATH);
    sapp(clipboard_path, entries[idx].name, FM_MAX_PATH);
    scpy(clipboard_name, entries[idx].name, FM_NAME_MAX);
    clipboard_has_item = true;
}

void FileManagerApp::PasteEntry(){
    if(!clipboard_has_item) return;
    // Build destination path
    char dest[FM_MAX_PATH];
    scpy(dest, current_path, FM_MAX_PATH);
    if(!seq(current_path, "/")) sapp(dest, "/", FM_MAX_PATH);
    sapp(dest, clipboard_name, FM_MAX_PATH);

    // Read source, create destination, write data
    KVFS::CreateFile(dest);

    // Try to copy content
    char buf[4096];
    int bytes_read = KVFS::ReadFile(clipboard_path, buf, 4096);
    if(bytes_read > 0){
        KVFS::WriteFile(dest, buf, bytes_read);
    }
    Refresh();
}

void FileManagerApp::RenameEntry(int idx){
    if(idx<0 || idx>=entry_count) return;
    rename_mode = true;
    scpy(rename_buf, entries[idx].name, FM_NAME_MAX);
    rename_cursor = slen(rename_buf);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Rendering
// ═══════════════════════════════════════════════════════════════════════════
void FileManagerApp::RenderToolbar(int x,int y,int w){
    Graphics::FillRect(x,y,w,32,FM_TOOLBAR);
    Graphics::DrawLine(x,y+32,x+w,y+32,FM_BORDER);

    // Back button
    Graphics::FillRoundedRect(x+4,y+4,28,24,4,FM_BTN);
    Graphics::DrawString(x+10,y+8,"<",FM_WHITE,0xFF000000);

    // Home button
    Graphics::FillRoundedRect(x+36,y+4,28,24,4,FM_BTN);
    Graphics::DrawString(x+42,y+8,"H",FM_WHITE,0xFF000000);

    // Refresh button
    Graphics::FillRoundedRect(x+68,y+4,28,24,4,FM_BTN);
    Graphics::DrawString(x+74,y+8,"R",FM_WHITE,0xFF000000);

    // New folder
    Graphics::FillRoundedRect(x+104,y+4,56,24,4,FM_BTN);
    Graphics::DrawString(x+108,y+8,"+Dir",FM_GREEN,0xFF000000);

    // New file
    Graphics::FillRoundedRect(x+164,y+4,56,24,4,FM_BTN);
    Graphics::DrawString(x+168,y+8,"+File",FM_BLUE,0xFF000000);

    // View toggle
    const char* vl = (view_mode==FM_VIEW_LIST) ? "List" : "Grid";
    Graphics::FillRoundedRect(x+w-56,y+4,52,24,4,FM_BTN);
    Graphics::DrawString(x+w-48,y+8,vl,FM_TEXT,0xFF000000);
}

void FileManagerApp::RenderPathBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,24,FM_PATH_BG);
    Graphics::DrawString(x+8,y+4,current_path,FM_BLUE,0xFF000000);
    Graphics::DrawLine(x,y+24,x+w,y+24,FM_BORDER);
}

void FileManagerApp::RenderEntryIcon(int x,int y,FMEntry* e){
    if(e->is_dir){
        // Folder icon
        Graphics::FillRoundedRect(x,y+2,16,12,2,FM_AMBER);
        Graphics::FillRect(x,y+2,8,4,0xFFD35400);
    } else {
        // File icon
        Graphics::FillRect(x+2,y,12,16,0xFF95A5A6);
        Graphics::FillRect(x+2,y,12,2,0xFF7F8C8D);
    }
}

void FileManagerApp::RenderFileList(int x,int y,int w,int h){
    int visible = h / ROW_H;
    for(int i=0;i<visible && (i+scroll_offset)<entry_count;i++){
        int idx = i + scroll_offset;
        FMEntry* e = &entries[idx];
        int ry = y + i * ROW_H;

        // Selection / hover bg
        if(idx==selected_index){
            Graphics::FillRect(x,ry,w,ROW_H,FM_SEL);
        } else if(i%2==1){
            Graphics::FillRect(x,ry,w,ROW_H,0xFF121225);
        }

        // Icon
        RenderEntryIcon(x+8, ry+4, e);

        // Name
        unsigned int name_clr = e->is_dir ? FM_AMBER : FM_TEXT;
        char tname[40]; scpy(tname, e->name, 38);
        Graphics::DrawString(x+32, ry+4, tname, name_clr, 0xFF000000);

        // Size
        if(!e->is_dir){
            char sz[16]; int_to_str(e->size, sz, 16);
            sapp(sz, " B", 16);
            Graphics::DrawString(x+w-100, ry+4, sz, FM_DIM, 0xFF000000);
        } else {
            Graphics::DrawString(x+w-100, ry+4, "<DIR>", FM_DIM, 0xFF000000);
        }

        // Permissions
        char perm[8];
        perm[0] = (e->permissions & 0x100) ? 'r' : '-';
        perm[1] = (e->permissions & 0x080) ? 'w' : '-';
        perm[2] = (e->permissions & 0x040) ? 'x' : '-';
        perm[3] = 0;
        Graphics::DrawString(x+w-40, ry+4, perm, FM_DIM, 0xFF000000);
    }

    // Scrollbar
    if(entry_count > visible){
        int sb_h = (visible * h) / entry_count;
        int sb_y = y + (scroll_offset * h) / entry_count;
        Graphics::FillRoundedRect(x+w-6, sb_y, 4, sb_h, 2, FM_BORDER);
    }
}

void FileManagerApp::RenderFileGrid(int x,int y,int w,int h){
    int cols = w / GRID_CELL;
    if(cols<1) cols=1;
    int vis_rows = h / GRID_CELL;

    for(int i=0;i<entry_count;i++){
        int r = i / cols;
        int c = i % cols;
        if(r < scroll_offset) continue;
        int dr = r - scroll_offset;
        if(dr >= vis_rows) break;

        FMEntry* e = &entries[i];
        int gx = x + c * GRID_CELL + 4;
        int gy = y + dr * GRID_CELL + 4;

        // Selection
        if(i==selected_index){
            Graphics::FillRoundedRect(gx,gy,GRID_CELL-8,GRID_CELL-8,6,FM_SEL);
        }

        // Larger icon
        unsigned int ic = e->is_dir ? FM_AMBER : FM_BLUE;
        Graphics::FillRoundedRect(gx+16,gy+4,40,36,6,ic);

        // Label
        char lbl[12]; scpy(lbl, e->name, 11);
        int tw = slen(lbl)*8;
        int tx = gx + (GRID_CELL-8)/2 - tw/2;
        Graphics::DrawString(tx, gy+46, lbl, FM_TEXT, 0xFF000000);
    }
}

void FileManagerApp::RenderStatusBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,20,FM_STATUS);
    Graphics::DrawLine(x,y,x+w,y,FM_BORDER);

    char info[64]={0};
    int_to_str(entry_count, info, 64);
    sapp(info, " items", 64);
    Graphics::DrawString(x+8, y+3, info, FM_DIM, 0xFF000000);
}

void FileManagerApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;
    Graphics::FillRect(cx,cy,cw,ch,FM_BG);

    int y=cy;
    RenderToolbar(cx,y,cw);   y+=33;
    RenderPathBar(cx,y,cw);   y+=25;

    int list_h = ch - 33 - 25 - 20;
    if(view_mode==FM_VIEW_LIST)
        RenderFileList(cx, y, cw, list_h);
    else
        RenderFileGrid(cx, y, cw, list_h);

    RenderStatusBar(cx, cy+ch-20, cw);

    if(context_menu_open) RenderContextMenu(cx, cy);
}

void FileManagerApp::RenderContextMenu(int ox, int oy){
    const int ITEM_H = 22;
    const int MENU_W = 140;
    const char* items[] = {"Open","Copy","Paste","Rename","Delete","---","New Folder","New File"};
    const int ITEM_COUNT = 8;

    int px = ox + context_menu_x;
    int py = oy + context_menu_y;

    // Background + border
    Graphics::FillRect(px, py, MENU_W, ITEM_COUNT*ITEM_H + 4, 0xFF2D2D30);
    Graphics::DrawRect(px, py, MENU_W, ITEM_COUNT*ITEM_H + 4, 0xFF555555);

    for(int i=0;i<ITEM_COUNT;i++){
        int iy = py + 2 + i * ITEM_H;
        if(items[i][0]=='-'){
            // Separator
            Graphics::FillRect(px+8, iy+10, MENU_W-16, 1, 0xFF555555);
        } else {
            // Dim paste if clipboard empty
            uint32_t col = 0xFFCCCCCC;
            if(i==2 && !clipboard_has_item) col = 0xFF666666;
            Graphics::DrawString(px+12, iy+3, items[i], col, 0xFF000000);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Input
// ═══════════════════════════════════════════════════════════════════════════
bool FileManagerApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)win_ptr;

    Window* w = (Window*)win_ptr;
    // mx, my are already content-local (0,0 = top-left of content area)

    // --- Handle right-click (event code 4 sent as key=4) ---
    if(key == 4 && !clicked){
        // Right-click at mx,my => open context menu
        context_menu_open = true;
        context_menu_x = mx;
        context_menu_y = my;
        // Determine which entry was right-clicked
        int list_y = 33 + 25;
        context_menu_idx = -1;
        if(my >= list_y){
            if(view_mode==FM_VIEW_LIST){
                int row = (my - list_y) / ROW_H + scroll_offset;
                if(row>=0 && row<entry_count) context_menu_idx = row;
            } else {
                int cw = w->w - 2;
                int cols = cw / GRID_CELL;
                if(cols<1) cols=1;
                int gcol = mx / GRID_CELL;
                int grow = (my - list_y) / GRID_CELL + scroll_offset;
                int idx = grow*cols + gcol;
                if(idx>=0 && idx<entry_count) context_menu_idx = idx;
            }
        }
        if(context_menu_idx >= 0) selected_index = context_menu_idx;
        return true;
    }

    // --- Handle context menu clicks ---
    if(clicked && context_menu_open){
        const int ITEM_H = 22;
        const int MENU_W = 140;
        const int ITEM_COUNT = 8;
        int px = context_menu_x;
        int py = context_menu_y;

        if(mx >= px && mx < px+MENU_W && my >= py && my < py+ITEM_COUNT*ITEM_H+4){
            int item = (my - py - 2) / ITEM_H;
            context_menu_open = false;
            switch(item){
                case 0: // Open
                    if(context_menu_idx>=0) OpenEntry(context_menu_idx);
                    break;
                case 1: // Copy
                    if(context_menu_idx>=0) CopyEntry(context_menu_idx);
                    break;
                case 2: // Paste
                    PasteEntry();
                    break;
                case 3: // Rename
                    if(context_menu_idx>=0) RenameEntry(context_menu_idx);
                    break;
                case 4: // Delete
                    if(context_menu_idx>=0) DeleteEntry(context_menu_idx);
                    break;
                // 5 = separator
                case 6: // New Folder
                    CreateFolder();
                    break;
                case 7: // New File
                    CreateFile();
                    break;
            }
            return true;
        }
        // Click outside menu -> close it
        context_menu_open = false;
        return true;
    }

    // --- Handle rename mode keyboard input ---
    if(rename_mode){
        if(key=='\n' || key=='\r'){
            // Apply rename
            if(rename_cursor > 0 && context_menu_idx >= 0 && context_menu_idx < entry_count){
                char old_path[FM_MAX_PATH], new_path[FM_MAX_PATH];
                scpy(old_path, current_path, FM_MAX_PATH);
                if(!seq(current_path, "/")) sapp(old_path, "/", FM_MAX_PATH);
                sapp(old_path, entries[context_menu_idx].name, FM_MAX_PATH);

                scpy(new_path, current_path, FM_MAX_PATH);
                if(!seq(current_path, "/")) sapp(new_path, "/", FM_MAX_PATH);
                sapp(new_path, rename_buf, FM_MAX_PATH);

                // Delete old, create new with same content
                char tmp[4096];
                int rlen = KVFS::ReadFile(old_path, tmp, 4096);
                KVFS::Unlink(old_path);
                KVFS::CreateFile(new_path);
                if(rlen > 0) KVFS::WriteFile(new_path, tmp, rlen);
                Refresh();
            }
            rename_mode = false;
            return true;
        }
        if(key==27){ rename_mode = false; return true; } // Escape
        if(key==8){ // Backspace
            if(rename_cursor > 0){ rename_buf[--rename_cursor] = 0; }
            return true;
        }
        if(key >= 32 && key < 127 && rename_cursor < FM_NAME_MAX-1){
            rename_buf[rename_cursor++] = key;
            rename_buf[rename_cursor] = 0;
            return true;
        }
        return true;
    }

    if(clicked){
        // Toolbar buttons (at top of content)
        if(my >= 0 && my < 32){
            if(mx>=4 && mx<32){ GoUp(); return true; }
            if(mx>=36 && mx<64){ GoHome(); return true; }
            if(mx>=68 && mx<96){ Refresh(); return true; }
            if(mx>=104 && mx<160){ CreateFolder(); return true; }
            if(mx>=164 && mx<220){ CreateFile(); return true; }
            // View toggle
            int cw = w->w - 2;
            if(mx>=cw-56 && mx<cw){
                view_mode = (view_mode==FM_VIEW_LIST) ? FM_VIEW_GRID : FM_VIEW_LIST;
                return true;
            }
            return true;
        }

        // File list area
        int list_y = 33 + 25;
        int list_h = w->h - WM_TITLEBAR_H - 1 - 33 - 25 - 20;
        if(my >= list_y && my < list_y + list_h){
            if(view_mode==FM_VIEW_LIST){
                int row = (my - list_y) / ROW_H + scroll_offset;
                if(row>=0 && row<entry_count){
                    selected_index=row;
                    return true;
                }
            } else {
                int cw = w->w - 2;
                int cols = cw / GRID_CELL;
                if(cols<1) cols=1;
                int gcol = mx / GRID_CELL;
                int grow = (my - list_y) / GRID_CELL + scroll_offset;
                int idx = grow*cols + gcol;
                if(idx>=0 && idx<entry_count){
                    selected_index=idx;
                    return true;
                }
            }
        }
    }

    // Keyboard
    if(key=='\n' || key=='\r'){
        if(selected_index>=0) OpenEntry(selected_index);
        return true;
    }
    if(key==8){ GoUp(); return true; } // Backspace = go up
    if(key=='d' || key=='D'){
        if(selected_index>=0) DeleteEntry(selected_index);
        return true;
    }
    if(key=='c' || key=='C'){
        if(selected_index>=0) CopyEntry(selected_index);
        return true;
    }
    if(key=='p' || key=='P'){
        PasteEntry();
        return true;
    }
    if(key=='r' || key=='R'){
        if(selected_index>=0) RenameEntry(selected_index);
        return true;
    }
    if(key=='n'){ CreateFolder(); return true; }
    if(key=='h'){
        show_hidden = !show_hidden;
        Refresh();
        return true;
    }
    if(key=='v'){
        view_mode = (view_mode==FM_VIEW_LIST) ? FM_VIEW_GRID : FM_VIEW_LIST;
        return true;
    }

    // Arrow navigation
    if(key==(char)0x48 && selected_index>0){ selected_index--; return true; }
    if(key==(char)0x50 && selected_index<entry_count-1){ selected_index++; return true; }

    return false;
}
