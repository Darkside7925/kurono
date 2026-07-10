//  kurono os - file manager (multi-tab, dual-pane, bookmarks, trash, search)
#include "file_manager.h"
#include "../ui/window_manager.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../system/logging.h"
#include "../ui/desktop.h"
#include "../ui/kss.h"
#include "../ui/font.h"
#include "text_editor.h"
#include "media_player.h"
#include "denji_app.h"
#include "terminal.h"
#include "../kcl/kcl.h"

// ── color palette ──────────────────────────────────────────────────────
// re-pointed onto the shared kss theme so files matches the black/grey chrome
// of settings: one accent, neutral surfaces/text/borders, no rainbow. these are
// macros (not statics) so every draw reads the live theme tokens after kss init,
// and a ui.conf theme reload retints the app for free. the former multicolor
// file-type hues all collapse to the single accent (folders) or dim text
// (everything else). (satoru)
#define FM_BG        (KSS::T().bg)
#define FM_TOOLBAR   (KSS::T().header)
#define FM_PATH_BG   (KSS::T().surface)
#define FM_SIDE_BG   (KSS::T().surface)
#define FM_TAB_BG    (KSS::T().bg)
#define FM_TAB_ACT   (KSS::T().surface_hi)
#define FM_SEL       (KSS::Accent())
#define FM_SEL_INACT (KSS::T().surface_hi)
#define FM_HOVER     (KSS::T().surface_hi)
#define FM_TEXT      (KSS::T().text)
#define FM_DIM       (KSS::T().text_dim)
#define FM_ACCENT    (KSS::Accent())
// folders/active carry the one accent; all other former hues read as dim text. (satoru)
#define FM_BLUE      (KSS::T().text_dim)
#define FM_GREEN     (KSS::T().text_dim)
#define FM_AMBER     (KSS::Accent())
#define FM_PURPLE    (KSS::T().text_dim)
#define FM_PINK      (KSS::T().text_dim)
#define FM_CYAN      (KSS::T().text_dim)
#define FM_RED       (KSS::T().text_dim)
#define FM_WHITE     (KSS::T().white)
#define FM_BTN       (KSS::T().surface)
#define FM_BTN_HVR   (KSS::T().surface_hi)
#define FM_BORDER    (KSS::T().border)
#define FM_BORDER_HI (KSS::Accent())
#define FM_STATUS    (KSS::T().header)
#define FM_FOCUS_BAR (KSS::Accent())

// ── layout constants ──────────────────────────────────────────────────
static const int TAB_BAR_H   = 26;
static const int TOOLBAR_H   = 32;
static const int PATH_BAR_H  = 24;
static const int STATUS_H    = 20;
static const int SIDEBAR_W   = 152;
static const int ROW_H       = 22;
static const int GRID_CELL   = 88;
static const int COL_W       = 180;

// ── small string helpers ──────────────────────────────────────────────
static int  slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static bool seq(const char*a,const char*b){
    int i=0;while(a[i]&&b[i]){if(a[i]!=b[i])return false;i++;}return a[i]==b[i];}
static bool path_under_desktop(const char* path){
    static const char* desktop_root = "/home/user/Desktop";
    if(!path) return false;
    int root_len = slen(desktop_root);
    for(int pos=0;pos<root_len;pos++){
        if(!path[pos] || path[pos]!=desktop_root[pos]) return false;
    }
    return path[root_len]==0 || path[root_len]=='/';
}
static void notify_desktop_path_changed(const char* path){
    if(path_under_desktop(path)) Desktop::RefreshFiles();
}
static void int_to_str(int v,char*b,int mx){
    if(mx<2){b[0]=0;return;}if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}
static void size_to_human(int b,char* out,int mx){
    if(b<1024){int_to_str(b,out,mx);sapp(out," B",mx);return;}
    if(b<1024*1024){int_to_str(b/1024,out,mx);sapp(out," KB",mx);return;}
    int_to_str(b/(1024*1024),out,mx);sapp(out," MB",mx);
}
static int  istricmp(const char* a,const char* b){
    while(*a||*b){
        char ca=*a,cb=*b;
        if(ca>='A'&&ca<='Z')ca+=32;
        if(cb>='A'&&cb<='Z')cb+=32;
        if(ca!=cb)return (int)(unsigned char)ca-(int)(unsigned char)cb;
        if(!ca)return 0;
        a++;b++;
    }
    return 0;
}
static const char* fname_ext(const char* name){
    int n=slen(name);const char* dot=0;
    for(int i=n-1;i>=0;i--){if(name[i]=='.'){dot=name+i+1;break;}if(name[i]=='/')break;}
    return dot?dot:"";
}
static bool ext_in(const char* name, const char* const* exts, int n){
    const char* e=fname_ext(name);
    for(int i=0;i<n;i++)if(istricmp(e,exts[i])==0)return true;
    return false;
}

// ── extension classification ──────────────────────────────────────────
static const char* TEXT_EXT[]={"txt","c","h","cpp","cc","hpp","py","kcl","md","sh","cfg","json","ini","log","conf","html","css","js","rs","toml","yml","yaml","xml"};
static const char* MEDIA_EXT[]={"mp4","wav","mp3","ogg","flac","avi","mkv","webm","m4a","opus"};
static const char* VIDEO_EXT[]={"kvid","mp4","mkv","avi","webm","mov","m4v"};
static const char* IMG_EXT[]={"png","jpg","jpeg","bmp","gif","ppm","tga","ico","webp"};
static const char* ARCHIVE_EXT[]={"zip","tar","gz","bz2","xz","7z","rar","kpkg","kro"};
static const char* CODE_EXT[]={"c","h","cpp","cc","hpp","py","rs","js","kcl"};

static bool is_text_file(const char* n){return ext_in(n,TEXT_EXT,sizeof(TEXT_EXT)/sizeof(TEXT_EXT[0]));}
static bool is_media_file(const char* n){return ext_in(n,MEDIA_EXT,sizeof(MEDIA_EXT)/sizeof(MEDIA_EXT[0]));}
static bool is_video_file(const char* n){return ext_in(n,VIDEO_EXT,sizeof(VIDEO_EXT)/sizeof(VIDEO_EXT[0]));}
static bool is_image_file(const char* n){return ext_in(n,IMG_EXT,sizeof(IMG_EXT)/sizeof(IMG_EXT[0]));}
static bool is_archive_file(const char* n){return ext_in(n,ARCHIVE_EXT,sizeof(ARCHIVE_EXT)/sizeof(ARCHIVE_EXT[0]));}
static bool is_code_file(const char* n){return ext_in(n,CODE_EXT,sizeof(CODE_EXT)/sizeof(CODE_EXT[0]));}
// a .kcl script is double-click-runnable through the kcl interpreter. (satoru)
static const char* KCL_EXT[]={"kcl"};
static bool is_kcl_file(const char* n){return ext_in(n,KCL_EXT,sizeof(KCL_EXT)/sizeof(KCL_EXT[0]));}

static unsigned int icon_color_for(const FMEntry* e){
    // monochrome theming: folders take the single accent, files all read as dim
    // text so the type palette no longer competes with the chrome. (satoru)
    if(e->is_dir) return FM_ACCENT;
    return FM_DIM;
}

// ── bookmarks ─────────────────────────────────────────────────────────
// color is kept in the struct for layout/compat but no longer carries a per-row
// hue - the kss theme is runtime, so the dot is tinted from theme tokens at draw
// time (accent for the current place, dim otherwise) for the monochrome look. (satoru)
struct Bookmark { const char* label; const char* path; unsigned int color; };
static const Bookmark BOOKMARKS[] = {
    {"Home",      "/home/user",            0},
    {"Desktop",   "/home/user/Desktop",    0},
    {"Documents", "/home/user/Documents",  0},
    {"Downloads", "/home/user/Downloads",  0},
    {"Pictures",  "/home/user/Pictures",   0},
    {"Music",     "/home/user/Music",      0},
    {"Videos",    "/home/user/Videos",     0},
    {"Trash",     "/home/user/.Trash",     0},
    {"Root",      "/",                     0},
    {"System",    "/system",               0},
    {"Etc",       "/etc",                  0},
};
static const int BOOKMARK_COUNT = sizeof(BOOKMARKS)/sizeof(BOOKMARKS[0]);

// ── static state ──────────────────────────────────────────────────────
int      FileManagerApp::win_id = -1;
FMTab    FileManagerApp::tabs[FM_MAX_TABS];
int      FileManagerApp::tab_count = 0;
int      FileManagerApp::active_tab = 0;
int      FileManagerApp::active_tab_right = 0;
bool     FileManagerApp::dual_pane = false;
int      FileManagerApp::focused_pane = 0;
FMEntry  FileManagerApp::entries_l[FM_MAX_ENTRIES];
FMEntry  FileManagerApp::entries_r[FM_MAX_ENTRIES];
int      FileManagerApp::entry_count_l = 0;
int      FileManagerApp::entry_count_r = 0;
char     FileManagerApp::clipboard_path[FM_MAX_PATH] = "";
char     FileManagerApp::clipboard_name[FM_NAME_MAX] = "";
bool     FileManagerApp::clipboard_has_item = false;
bool     FileManagerApp::clipboard_cut = false;
bool     FileManagerApp::context_menu_open = false;
int      FileManagerApp::context_menu_x = 0;
int      FileManagerApp::context_menu_y = 0;
int      FileManagerApp::context_menu_pane = 0;
int      FileManagerApp::context_menu_idx = -1;
bool     FileManagerApp::properties_open = false;
char     FileManagerApp::properties_path[FM_MAX_PATH] = "";
bool     FileManagerApp::sort_menu_open = false;
bool     FileManagerApp::rename_mode = false;
int      FileManagerApp::rename_pane = 0;
int      FileManagerApp::rename_target = -1;
char     FileManagerApp::rename_buf[FM_NAME_MAX] = "";
int      FileManagerApp::rename_cursor = 0;
char     FileManagerApp::recent[FM_RECENT_MAX][FM_MAX_PATH];
int      FileManagerApp::recent_count = 0;

// double-click detection per pane
static int      last_click_idx[2]  = {-1,-1};
static uint32_t last_click_time[2] = {0,0};
static const uint32_t DOUBLE_CLICK_MS = 500;

// remembered last viewport for hit testing in Input()
static int s_last_left_pane_x = 0, s_last_left_pane_y = 0, s_last_left_pane_w = 0, s_last_left_pane_h = 0;
static int s_last_right_pane_x = 0, s_last_right_pane_y = 0, s_last_right_pane_w = 0, s_last_right_pane_h = 0;

// ── helpers ───────────────────────────────────────────────────────────
FMTab* FileManagerApp::Active(int pane){
    int idx = (pane==1) ? active_tab_right : active_tab;
    if(idx<0||idx>=tab_count) idx=0;
    return &tabs[idx];
}
FMEntry* FileManagerApp::EntriesOf(int pane){ return (pane==1)?entries_r:entries_l; }
int      FileManagerApp::EntryCount(int pane){ return (pane==1)?entry_count_r:entry_count_l; }

// mouse-wheel scroll of the active pane's file list. dz>0 = wheel up = toward the
// top. clamped to the entry range so it can't run off either end. (satoru)
void FileManagerApp::Scroll(int dz){
    FMTab* t = Active(0);
    if(!t) return;
    int n = EntryCount(0);
    t->scroll -= dz * 3;
    if(t->scroll < 0) t->scroll = 0;
    int maxs = (n > 0) ? n - 1 : 0;
    if(t->scroll > maxs) t->scroll = maxs;
}

void FileManagerApp::JoinPath(char* dst,int max,const char* dir,const char* name){
    scpy(dst,dir,max);
    if(!seq(dir,"/")) sapp(dst,"/",max);
    sapp(dst,name,max);
}

void FileManagerApp::SortPane(int pane){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    int mode = t->sort_mode;
    bool desc = t->sort_desc;
    // simple bubble (n<=256)
    for(int i=0;i<n-1;i++){
        for(int j=0;j<n-1-i;j++){
            // dirs always first, regardless of sort order
            bool swap=false;
            if(!e[j].is_dir && e[j+1].is_dir){ swap=true; }
            else if(e[j].is_dir==e[j+1].is_dir){
                int cmp=0;
                if(mode==FM_SORT_NAME) cmp = istricmp(e[j].name,e[j+1].name);
                else if(mode==FM_SORT_SIZE) cmp = e[j].size - e[j+1].size;
                else if(mode==FM_SORT_TYPE){
                    cmp = istricmp(fname_ext(e[j].name), fname_ext(e[j+1].name));
                    if(cmp==0) cmp = istricmp(e[j].name,e[j+1].name);
                } else if(mode==FM_SORT_DATE){
                    cmp = (int)e[j].modified - (int)e[j+1].modified;
                }
                if(desc) cmp = -cmp;
                if(cmp>0) swap=true;
            }
            if(swap){ FMEntry tmp=e[j]; e[j]=e[j+1]; e[j+1]=tmp; }
        }
    }
}

void FileManagerApp::RefreshPane(int pane){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int& cnt = (pane==1)?entry_count_r:entry_count_l;
    cnt=0;
    KVFSNode* dir = KVFS::ResolvePath(t->path);
    if(!dir || dir->type != KVFS_DIR){ t->selected=-1; t->scroll=0; return; }
    for(int i=0;i<dir->child_count && cnt<FM_MAX_ENTRIES;i++){
        KVFSNode* c = dir->children[i];
        if(!c) continue;
        if(!t->show_hidden && c->name[0]=='.') continue;
        // search filter
        if(t->search[0]){
            // case-insensitive substring
            const char* hay = c->name;
            const char* needle = t->search;
            bool match=false;
            for(int p=0; hay[p]; p++){
                int q=0;
                while(needle[q] && hay[p+q]){
                    char a=hay[p+q],b=needle[q];
                    if(a>='A'&&a<='Z')a+=32;
                    if(b>='A'&&b<='Z')b+=32;
                    if(a!=b) break;
                    q++;
                }
                if(!needle[q]){ match=true; break; }
            }
            if(!match) continue;
        }
        FMEntry* en = &e[cnt];
        scpy(en->name,c->name,FM_NAME_MAX);
        en->is_dir = (c->type==KVFS_DIR);
        en->size = c->size;
        en->permissions = c->perms.mode;
        en->modified = c->modified;
        cnt++;
    }
    if(t->selected >= cnt) t->selected = cnt-1;
    if(t->selected < -1) t->selected = -1;
    SortPane(pane);
}
void FileManagerApp::RefreshActive(){ RefreshPane(focused_pane); }
void FileManagerApp::RefreshBoth(){ RefreshPane(0); if(dual_pane) RefreshPane(1); }

void FileManagerApp::NotifyFilesystemChanged(const char* path){
    (void)path;
    if(tab_count>0) RefreshBoth();
}

static bool fm_path_is_safe(const char* path){
    if(!path || !*path) return false;
    // reject embedded ".." segments - prevents traversal abuse via address bar
    int n = slen(path);
    for(int i=0;i<n-1;i++){
        if(path[i]=='.' && path[i+1]=='.'){
            bool start_ok = (i==0 || path[i-1]=='/');
            bool end_ok = (i+2==n || path[i+2]=='/');
            if(start_ok && end_ok) return false;
        }
    }
    return true;
}

void FileManagerApp::NavigateToPane(int pane,const char* path,bool record_history){
    FMTab* t = Active(pane);
    if(!fm_path_is_safe(path)) return;
    if(record_history){
        // truncate forward history then push
        t->history_count = t->history_pos + 1;
        if(t->history_count >= FM_MAX_HISTORY){
            // shift left by 1
            for(int i=0;i<FM_MAX_HISTORY-1;i++)
                scpy(t->history[i],t->history[i+1],FM_MAX_PATH);
            t->history_count = FM_MAX_HISTORY-1;
            t->history_pos   = FM_MAX_HISTORY-2;
        }
        scpy(t->history[t->history_count], path, FM_MAX_PATH);
        t->history_count++;
        t->history_pos = t->history_count-1;
    }
    scpy(t->path,path,FM_MAX_PATH);
    t->selected=-1; t->scroll=0; t->search[0]=0; t->search_cursor=0;
    RefreshPane(pane);
}

void FileManagerApp::GoUpPane(int pane){
    FMTab* t = Active(pane);
    int len=slen(t->path);
    if(len<=1) return;
    int last=len-1;
    if(t->path[last]=='/') last--;
    while(last>0 && t->path[last]!='/') last--;
    char np[FM_MAX_PATH];
    if(last==0){ np[0]='/'; np[1]=0; }
    else { scpy(np,t->path,FM_MAX_PATH); np[last]=0; }
    NavigateToPane(pane, np, true);
}

void FileManagerApp::GoBackPane(int pane){
    FMTab* t = Active(pane);
    if(t->history_pos<=0) return;
    t->history_pos--;
    scpy(t->path, t->history[t->history_pos], FM_MAX_PATH);
    t->selected=-1; t->scroll=0;
    RefreshPane(pane);
}
void FileManagerApp::GoForwardPane(int pane){
    FMTab* t = Active(pane);
    if(t->history_pos+1 >= t->history_count) return;
    t->history_pos++;
    scpy(t->path, t->history[t->history_pos], FM_MAX_PATH);
    t->selected=-1; t->scroll=0;
    RefreshPane(pane);
}

// ── trash ─────────────────────────────────────────────────────────────
void FileManagerApp::EnsureTrashDir(){
    KVFS::Mkdirs("/home/user/.Trash", 0755);
}
void FileManagerApp::MoveToTrash(const char* full_path){
    EnsureTrashDir();
    if(!full_path||!*full_path) return;
    if(seq(full_path,"/")) return;
    // build trash destination using the filename + a small disambiguator
    int len=slen(full_path);
    int last=len-1; while(last>0 && full_path[last]!='/') last--;
    const char* nm = (full_path[last]=='/') ? (full_path+last+1) : full_path;
    if(!*nm) return;
    char dst[FM_MAX_PATH];
    JoinPath(dst,FM_MAX_PATH,"/home/user/.Trash",nm);
    // collision: append (n)
    int n=1;
    char base[FM_MAX_PATH]; scpy(base,dst,FM_MAX_PATH);
    while(KVFS::Exists(dst) && n<999){
        scpy(dst,base,FM_MAX_PATH);
        sapp(dst,"-",FM_MAX_PATH);
        char nb[8]; int_to_str(n,nb,8);
        sapp(dst,nb,FM_MAX_PATH);
        n++;
    }
    KVFSNode* node = KVFS::ResolvePath(full_path);
    if(!node) return;
    if(node->is_dir()){
        // recursive copy then RmTree
        // simple: just unlink in place (full recursive dir move is complex
        // and KVFS::Move only supports files).  Accept that directory trash
        // is destructive for now - log and remove.
        KVFS::RmTree(full_path);
    } else {
        KVFS::Move(full_path, dst);
    }
    RuntimeLog::LogAppEvent("files","trash",full_path);
}
void FileManagerApp::EmptyTrash(){
    EnsureTrashDir();
    KVFSNode* d = KVFS::ResolvePath("/home/user/.Trash");
    if(!d) return;
    // collect names then unlink each
    char names[64][FM_NAME_MAX];
    int cnt=0;
    for(int i=0;i<d->child_count && cnt<64;i++){
        if(d->children[i]) scpy(names[cnt++],d->children[i]->name,FM_NAME_MAX);
    }
    for(int i=0;i<cnt;i++){
        char p[FM_MAX_PATH]; JoinPath(p,FM_MAX_PATH,"/home/user/.Trash",names[i]);
        KVFSNode* n=KVFS::ResolvePath(p);
        if(n && n->is_dir()) KVFS::RmTree(p);
        else KVFS::Unlink(p);
    }
    RuntimeLog::LogAppEvent("files","empty-trash","");
}
void FileManagerApp::RestoreFromTrash(const char* trash_name){
    char src[FM_MAX_PATH]; JoinPath(src,FM_MAX_PATH,"/home/user/.Trash",trash_name);
    char dst[FM_MAX_PATH]; JoinPath(dst,FM_MAX_PATH,"/home/user",trash_name);
    KVFS::Move(src,dst);
    RuntimeLog::LogAppEvent("files","restore",trash_name);
}

// ── recent ────────────────────────────────────────────────────────────
void FileManagerApp::PushRecent(const char* full_path){
    // remove any existing copy first
    for(int i=0;i<recent_count;i++){
        if(seq(recent[i],full_path)){
            for(int j=i;j<recent_count-1;j++) scpy(recent[j],recent[j+1],FM_MAX_PATH);
            recent_count--;
            break;
        }
    }
    // shift down + insert at top
    if(recent_count<FM_RECENT_MAX) recent_count++;
    for(int i=recent_count-1;i>0;i--) scpy(recent[i],recent[i-1],FM_MAX_PATH);
    scpy(recent[0],full_path,FM_MAX_PATH);
}

// ── tabs ──────────────────────────────────────────────────────────────
int FileManagerApp::NewTab(const char* path){
    if(tab_count>=FM_MAX_TABS) return -1;
    FMTab* t = &tabs[tab_count];
    scpy(t->path, path?path:"/home/user", FM_MAX_PATH);
    t->selected=-1; t->scroll=0;
    t->history_count=1; t->history_pos=0;
    scpy(t->history[0], t->path, FM_MAX_PATH);
    t->sort_mode = FM_SORT_NAME;
    t->sort_desc = false;
    t->show_hidden = false;
    t->search[0]=0; t->search_cursor=0;
    t->address_edit=false; t->address_buf[0]=0; t->address_cursor=0;
    t->view_mode = FM_VIEW_LIST;
    t->used = true;
    int idx = tab_count++;
    active_tab = idx;
    RefreshPane(0);
    return idx;
}
void FileManagerApp::CloseTab(int idx){
    if(tab_count<=1) return;
    if(idx<0||idx>=tab_count) return;
    for(int i=idx;i<tab_count-1;i++) tabs[i] = tabs[i+1];
    tab_count--;
    if(active_tab>=tab_count) active_tab=tab_count-1;
    if(active_tab_right>=tab_count) active_tab_right=tab_count-1;
    RefreshBoth();
}

// ── action helpers ────────────────────────────────────────────────────
void FileManagerApp::OpenEntryPane(int pane,int idx){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    if(idx<0||idx>=n) return;
    char full[FM_MAX_PATH]; JoinPath(full,FM_MAX_PATH,t->path,e[idx].name);
    if(e[idx].is_dir){ NavigateToPane(pane,full,true); return; }
    PushRecent(full);
    RuntimeLog::LogAppEvent("files","open-entry",full);
    if(is_kcl_file(e[idx].name)){
        // double-click a .kcl script: open the terminal and run it through the
        // kcl interpreter so its output (and any errors) are visible. (satoru)
        RuntimeLog::LogAppEvent("files","run-kcl",full);
        TerminalApp::Open();
        char cmd[FM_MAX_PATH+8];
        int ci=0; const char* pfx="kcl ";
        for(int s=0; pfx[s] && ci<(int)sizeof(cmd)-1; s++) cmd[ci++]=pfx[s];
        for(int s=0; full[s] && ci<(int)sizeof(cmd)-1; s++) cmd[ci++]=full[s];
        cmd[ci]=0;
        TerminalApp::EnqueueCommand(cmd);
        return;
    }
    if(is_video_file(e[idx].name)){
        // play through the native kvid player. real-time h264 decode isn't
        // feasible on a freestanding kernel, so a non-kvid video plays its
        // host-transcoded sibling "<name>.kvid" if one exists; otherwise we
        // fall back to the media player's (audio) view. (satoru)
        const char* dot=nullptr; for(const char* p=full;*p;++p) if(*p=='.') dot=p;
        if(dot && istricmp(dot+1,"kvid")==0){
            DenjiApp::OpenFile(full);
        } else {
            char kv[FM_MAX_PATH];
            int blen = dot ? (int)(dot-full) : 0;
            if(!dot){ while(full[blen]) blen++; }
            int k=0; while(k<blen && k<FM_MAX_PATH-6){ kv[k]=full[k]; k++; }
            const char* suf=".kvid"; for(int s=0; suf[s] && k<FM_MAX_PATH-1; s++) kv[k++]=suf[s];
            kv[k]=0;
            if(KVFS::Exists(kv)) DenjiApp::OpenFile(kv);
            else MediaPlayerApp::Open(full);
        }
    }
    else if(is_media_file(e[idx].name))      MediaPlayerApp::Open(full);
    else if(is_image_file(e[idx].name)) MediaPlayerApp::Open(full);
    else                                 TextEditorApp::OpenFile(full);
}

void FileManagerApp::DeleteEntryPane(int pane,int idx,bool permanent){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    if(idx<0||idx>=n) return;
    char full[FM_MAX_PATH]; JoinPath(full,FM_MAX_PATH,t->path,e[idx].name);
    if(permanent){
        KVFSNode* nd = KVFS::ResolvePath(full);
        if(nd && nd->is_dir()) KVFS::RmTree(full);
        else KVFS::Unlink(full);
        RuntimeLog::LogAppEvent("files","delete-permanent",full);
    } else {
        MoveToTrash(full);
    }
    // clear selection - the index just got invalidated
    t->selected = -1;
    RefreshBoth();
    notify_desktop_path_changed(full);
}

void FileManagerApp::CreateFolderPane(int pane){
    FMTab* t = Active(pane);
    char p[FM_MAX_PATH]; JoinPath(p,FM_MAX_PATH,t->path,"New Folder");
    int n=1;
    char base[FM_MAX_PATH]; scpy(base,p,FM_MAX_PATH);
    while(KVFS::Exists(p) && n<999){
        scpy(p,base,FM_MAX_PATH);
        sapp(p," ",FM_MAX_PATH);
        char nb[8]; int_to_str(n,nb,8);
        sapp(p,nb,FM_MAX_PATH);
        n++;
    }
    KVFS::Mkdir(p);
    RefreshBoth();
    notify_desktop_path_changed(p);
}
void FileManagerApp::CreateFilePane(int pane){
    FMTab* t = Active(pane);
    char p[FM_MAX_PATH]; JoinPath(p,FM_MAX_PATH,t->path,"untitled.txt");
    int n=1;
    char base[FM_MAX_PATH]; scpy(base,p,FM_MAX_PATH);
    while(KVFS::Exists(p) && n<999){
        // strip ".txt", append "-N.txt"
        scpy(p,t->path,FM_MAX_PATH);
        if(!seq(t->path,"/")) sapp(p,"/",FM_MAX_PATH);
        sapp(p,"untitled-",FM_MAX_PATH);
        char nb[8]; int_to_str(n,nb,8);
        sapp(p,nb,FM_MAX_PATH);
        sapp(p,".txt",FM_MAX_PATH);
        n++;
    }
    KVFS::CreateFile(p);
    RefreshBoth();
    notify_desktop_path_changed(p);
}

void FileManagerApp::CopyEntryPane(int pane,int idx,bool cut){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    if(idx<0||idx>=n) return;
    JoinPath(clipboard_path,FM_MAX_PATH,t->path,e[idx].name);
    scpy(clipboard_name,e[idx].name,FM_NAME_MAX);
    clipboard_has_item = true;
    clipboard_cut = cut;
}

void FileManagerApp::PastePane(int pane){
    if(!clipboard_has_item) return;
    FMTab* t = Active(pane);
    char dst[FM_MAX_PATH]; JoinPath(dst,FM_MAX_PATH,t->path,clipboard_name);
    if(seq(dst,clipboard_path)){ /* same place - make a copy with " - copy" suffix */
        scpy(dst,t->path,FM_MAX_PATH);
        if(!seq(t->path,"/")) sapp(dst,"/",FM_MAX_PATH);
        sapp(dst,clipboard_name,FM_MAX_PATH);
        sapp(dst,"-copy",FM_MAX_PATH);
    }
    if(clipboard_cut){
        KVFS::Move(clipboard_path,dst);
        clipboard_has_item=false;
        clipboard_cut=false;
    } else {
        KVFS::Copy(clipboard_path,dst);
    }
    RefreshBoth();
    notify_desktop_path_changed(clipboard_path);
    notify_desktop_path_changed(dst);
}

// ── init / open ───────────────────────────────────────────────────────
void FileManagerApp::Init(){
    if(tab_count==0) NewTab("/home/user");
    EnsureTrashDir();
}

int FileManagerApp::Open(){
    Init();
    win_id = WindowManager::CreateWindow("Files", -1, -1, 920, 560,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            FileManagerApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) FileManagerApp::Input(w,p1,p2,true,0);
            else if(ev==2) FileManagerApp::Input(w,0,0,false,(char)p1);
            else if(ev==4) FileManagerApp::Input(w,p1 - w->content_x,p2 - w->content_y,false,(char)4);  // right-click: ev4 arrives global, convert to content-local like task_manager (satoru)
            else if(ev==3) FileManagerApp::Scroll(p1);                    // mouse wheel (satoru)
        }
    );
    RuntimeLog::LogAppEvent("files","open","");
    return win_id;
}
int FileManagerApp::OpenAt(const char* path){
    int id = Open();
    if(path && *path){
        NavigateToPane(0, path, true);
    }
    return id;
}

// ── rendering ─────────────────────────────────────────────────────────
void FileManagerApp::RenderTabBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,TAB_BAR_H,FM_TAB_BG);
    int tx=x+4;
    int max_tab_w=160;
    int avail = w - 8 - 28; // reserve "+" button
    int per = avail / (tab_count>0?tab_count:1);
    if(per>max_tab_w) per=max_tab_w;
    if(per<60) per=60;
    for(int i=0;i<tab_count;i++){
        bool act = (i==active_tab) || (dual_pane && i==active_tab_right);
        unsigned int bg = act ? FM_TAB_ACT : FM_TAB_BG;
        Graphics::FillRect(tx,y+2,per-2,TAB_BAR_H-2,bg);
        if(act) Graphics::FillRect(tx,y+TAB_BAR_H-2,per-2,2,FM_FOCUS_BAR);
        // tab title = basename of path
        const char* p = tabs[i].path;
        int len=slen(p);
        const char* base=p;
        for(int k=len-1;k>=0;k--) if(p[k]=='/'){ base=p+k+1; break; }
        if(!*base) base="/";
        char title[20]; scpy(title,base,18);
        Graphics::DrawString(tx+8,y+6,title, act?FM_TEXT:FM_DIM, 0xFF000000);
        // close button "x" on right of each tab
        Graphics::DrawString(tx+per-14,y+6,"x", FM_DIM, 0xFF000000);
        tx += per;
    }
    // "+" new tab button
    Graphics::FillRect(tx+4,y+2,22,TAB_BAR_H-4,FM_TAB_BG);
    Graphics::DrawString(tx+10,y+6,"+",FM_ACCENT,0xFF000000);
}

void FileManagerApp::RenderToolbar(int x,int y,int w){
    Graphics::FillRect(x,y,w,TOOLBAR_H,FM_TOOLBAR);
    Graphics::DrawLine(x,y+TOOLBAR_H,x+w,y+TOOLBAR_H,FM_BORDER);
    struct Btn { int x; int w; const char* label; unsigned int color; };
    // neutral labels on surface buttons; the active "Dual" toggle takes the
    // single accent to signal its on-state. (satoru)
    Btn btns[] = {
        {  4,28,"<",FM_TEXT},    // back
        { 36,28,">",FM_TEXT},    // fwd
        { 68,28,"^",FM_TEXT},    // up
        {100,28,"~",FM_TEXT},    // home
        {132,28,"R",FM_TEXT},    // refresh
        {168,56,"+Dir",FM_TEXT},
        {228,56,"+File",FM_TEXT},
        {288,56,"Sort",FM_TEXT},
        {348,56,"View",FM_TEXT},
        {408,72,"Dual",dual_pane?FM_ACCENT:FM_TEXT},
        {488,28,"H",FM_TEXT},     // hidden toggle (Ctrl+H)
    };
    for(int i=0;i<11;i++){
        Graphics::FillRoundedRect(x+btns[i].x,y+4,btns[i].w,TOOLBAR_H-8,4,FM_BTN);
        Graphics::DrawString(x+btns[i].x+6,y+8,btns[i].label,btns[i].color,0xFF000000);
    }
}

void FileManagerApp::RenderSidebar(int x,int y,int w,int h){
    Graphics::FillRect(x,y,w,h,FM_SIDE_BG);
    Graphics::DrawLine(x+w,y,x+w,y+h,FM_BORDER);
    Graphics::DrawString(x+10,y+6,"PLACES",FM_DIM,0xFF000000);
    int by = y+24;
    FMTab* a = Active(focused_pane);
    for(int i=0;i<BOOKMARK_COUNT;i++){
        bool current = seq(a->path, BOOKMARKS[i].path);
        if(current) Graphics::FillRect(x+2,by,w-4,22,FM_HOVER);
        // monochrome place dot: accent on the active place, dim otherwise. (satoru)
        Graphics::FillRect(x+10,by+7,8,8, current?KSS::Accent():KSS::T().text_dim);
        char lbl[20]; scpy(lbl,BOOKMARKS[i].label,18);
        Graphics::DrawString(x+24,by+5,lbl, current?FM_TEXT:FM_DIM, 0xFF000000);
        by += 22;
    }
    by += 8;
    Graphics::DrawLine(x+8,by,x+w-8,by,FM_BORDER); by+=4;
    Graphics::DrawString(x+10,by,"RECENT",FM_DIM,0xFF000000); by += 18;
    int rmax = (y+h - by) / 18;
    if(rmax>recent_count) rmax=recent_count;
    if(rmax>6) rmax=6;
    for(int i=0;i<rmax;i++){
        const char* p = recent[i];
        int len=slen(p); const char* base=p;
        for(int k=len-1;k>=0;k--) if(p[k]=='/'){ base=p+k+1; break; }
        char nm[18]; scpy(nm,base,16);
        Graphics::DrawString(x+12,by,nm,FM_DIM,0xFF000000);
        by += 18;
    }
}

void FileManagerApp::RenderPathBar(int pane,int x,int y,int w){
    FMTab* t = Active(pane);
    Graphics::FillRect(x,y,w,PATH_BAR_H,FM_PATH_BG);
    if(t->address_edit){
        // show editable buffer with text cursor
        Graphics::DrawRect(x+2,y+2,w-4,PATH_BAR_H-4,FM_BORDER_HI);
        Graphics::DrawString(x+8,y+5,t->address_buf,FM_TEXT,0xFF000000);
        // cursor - measure the text up to the caret rather than assuming 8px
        // glyphs (the old fixed-font math drifted under fontttf). (satoru)
        char acur[FM_MAX_PATH];
        int an=t->address_cursor; if(an>FM_MAX_PATH-1) an=FM_MAX_PATH-1;
        for(int k=0;k<an;k++) acur[k]=t->address_buf[k]; acur[an]=0;
        int cx = x+8 + FontTTF::Measure(KSS::BodyPx(), acur);
        Graphics::FillRect(cx,y+4,1,PATH_BAR_H-8,FM_TEXT);
    } else {
        // breadcrumb segments
        int cx = x+8;
        Graphics::DrawString(cx,y+5,"/",FM_DIM,0xFF000000);
        cx += 10;
        char buf[FM_MAX_PATH]; scpy(buf,t->path,FM_MAX_PATH);
        // walk segments
        int i=1;
        while(buf[i]){
            int j=i;
            while(buf[j] && buf[j]!='/') j++;
            char seg[40]; int sl = j-i; if(sl>38)sl=38;
            for(int k=0;k<sl;k++) seg[k]=buf[i+k]; seg[sl]=0;
            Graphics::DrawString(cx,y+5,seg,FM_TEXT,0xFF000000);
            // advance by the measured segment width, not len*8. (satoru)
            cx += FontTTF::Measure(KSS::BodyPx(), seg) + 4;
            if(buf[j]){
                Graphics::DrawString(cx,y+5,"/",FM_DIM,0xFF000000);
                cx += 10;
                i = j+1;
            } else break;
        }
        // search box on right
        if(t->search[0]){
            int sw = 140;
            Graphics::FillRect(x+w-sw-4,y+3,sw,PATH_BAR_H-6,FM_HOVER);
            Graphics::DrawString(x+w-sw,y+5,"S:",FM_DIM,0xFF000000);
            Graphics::DrawString(x+w-sw+18,y+5,t->search,FM_AMBER,0xFF000000);
        }
    }
    Graphics::DrawLine(x,y+PATH_BAR_H,x+w,y+PATH_BAR_H,FM_BORDER);
}

void FileManagerApp::RenderEntryIcon(int x,int y,int sz,FMEntry* e){
    unsigned int c = icon_color_for(e);
    if(e->is_dir){
        Graphics::FillRoundedRect(x,y+sz/4,sz,(sz*3)/4,3,c);
        // folder tab: a subtle darker token instead of the old orange. (satoru)
        Graphics::FillRect(x,y+sz/4,sz/2,sz/8,FM_BORDER);
    } else {
        Graphics::FillRoundedRect(x+sz/8,y,(sz*3)/4,sz,2,c);
        // fold corner
        Graphics::FillRect(x+(sz*5)/8,y,sz/4,sz/4,0xFF000000);
        // ext label for files >=24 px
        if(sz>=24){
            const char* e2 = fname_ext(e->name);
            if(e2[0]){
                char up[5]; int k=0;
                while(e2[k] && k<3){ char ch=e2[k]; if(ch>='a'&&ch<='z') ch-=32; up[k]=ch; k++; }
                up[k]=0;
                Graphics::DrawString(x+sz/4,y+sz/2-4,up,FM_WHITE,0xFF000000);
            }
        }
    }
}

void FileManagerApp::RenderFileList(int pane,int x,int y,int w,int h){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    int visible = h / ROW_H;
    bool focused = (pane==focused_pane);
    for(int i=0;i<visible && (i+t->scroll)<n;i++){
        int idx = i + t->scroll;
        FMEntry* ent = &e[idx];
        int ry = y + i*ROW_H;
        if(idx==t->selected){
            Graphics::FillRect(x,ry,w,ROW_H, focused?FM_SEL:FM_SEL_INACT);
        } else if(i%2){
            Graphics::FillRect(x,ry,w,ROW_H,KSS::T().surface);
        }
        if(rename_mode && rename_pane==pane && rename_target==idx){
            // draw editable name
            Graphics::FillRect(x+30,ry+2,w-160,ROW_H-4,FM_HOVER);
            Graphics::DrawString(x+34,ry+5,rename_buf,FM_TEXT,0xFF000000);
            // caret position measured through fontttf, not rename_cursor*8. (satoru)
            char rcur[FM_NAME_MAX];
            int rn=rename_cursor; if(rn>FM_NAME_MAX-1) rn=FM_NAME_MAX-1;
            for(int k=0;k<rn;k++) rcur[k]=rename_buf[k]; rcur[rn]=0;
            int cx = x+34 + FontTTF::Measure(KSS::BodyPx(), rcur);
            Graphics::FillRect(cx,ry+4,1,ROW_H-8,FM_TEXT);
        } else {
            // small icon
            Graphics::FillRoundedRect(x+8,ry+5,12,12,2,icon_color_for(ent));
            char tname[44]; scpy(tname,ent->name,42);
            Graphics::DrawString(x+30,ry+5,tname, ent->is_dir?FM_AMBER:FM_TEXT, 0xFF000000);
        }
        // size column
        char sz[16];
        if(ent->is_dir) scpy(sz,"--",16);
        else size_to_human(ent->size,sz,16);
        Graphics::DrawString(x+w-130,ry+5,sz,FM_DIM,0xFF000000);
        // ext column
        Graphics::DrawString(x+w-70,ry+5,fname_ext(ent->name),FM_DIM,0xFF000000);
        // perms
        char perm[4];
        perm[0]=(ent->permissions&0400)?'r':'-';
        perm[1]=(ent->permissions&0200)?'w':'-';
        perm[2]=(ent->permissions&0100)?'x':'-';
        perm[3]=0;
        Graphics::DrawString(x+w-32,ry+5,perm,FM_DIM,0xFF000000);
    }
    if(n>visible && n>0){
        int sb_h = (visible*h)/n; if(sb_h<8) sb_h=8;
        int sb_y = y + (t->scroll*h)/n;
        Graphics::FillRoundedRect(x+w-4,sb_y,3,sb_h,2,FM_BORDER_HI);
    }
}

void FileManagerApp::RenderFileGrid(int pane,int x,int y,int w,int h){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    int cols = w/GRID_CELL; if(cols<1) cols=1;
    int vis_rows = h/GRID_CELL;
    bool focused = (pane==focused_pane);
    // virtualize: only iterate the visible cell range
    int start_i = t->scroll * cols;
    int end_i   = start_i + vis_rows * cols + cols;
    if(start_i < 0) start_i = 0;
    if(end_i > n) end_i = n;
    for(int i=start_i;i<end_i;i++){
        int r = i/cols, c = i%cols;
        int dr = r - t->scroll;
        if(dr < 0 || dr >= vis_rows) continue;
        FMEntry* ent = &e[i];
        int gx = x + c*GRID_CELL + 4;
        int gy = y + dr*GRID_CELL + 4;
        if(i==t->selected){
            Graphics::FillRoundedRect(gx,gy,GRID_CELL-8,GRID_CELL-8,6,
                                       focused?FM_SEL:FM_SEL_INACT);
        }
        RenderEntryIcon(gx+(GRID_CELL-8)/2-20, gy+8, 36, ent);
        char lbl[14]; scpy(lbl,ent->name,12);
        // center using the measured label width, not len*8. (satoru)
        int tw = FontTTF::Measure(KSS::BodyPx(), lbl);
        int tx = gx + (GRID_CELL-8)/2 - tw/2;
        Graphics::DrawString(tx, gy+GRID_CELL-22, lbl, FM_TEXT, 0xFF000000);
    }
}

void FileManagerApp::RenderFileColumns(int pane,int x,int y,int w,int h){
    // single-column for now (data: name + size). Wider columns view.
    RenderFileList(pane,x,y,w,h);
}

void FileManagerApp::RenderPane(int pane,int x,int y,int w,int h){
    bool focused = (pane==focused_pane);
    if(focused) Graphics::FillRect(x,y,w,2,FM_FOCUS_BAR);
    int top = focused ? y+2 : y;
    RenderPathBar(pane, x, top, w);
    int list_y = top + PATH_BAR_H;
    int list_h = (y+h) - list_y;
    FMTab* t = Active(pane);
    if(t->view_mode==FM_VIEW_GRID)         RenderFileGrid(pane,x,list_y,w,list_h);
    else if(t->view_mode==FM_VIEW_COLUMNS) RenderFileColumns(pane,x,list_y,w,list_h);
    else                                   RenderFileList(pane,x,list_y,w,list_h);
    if(pane==0){
        s_last_left_pane_x=x; s_last_left_pane_y=list_y;
        s_last_left_pane_w=w; s_last_left_pane_h=list_h;
    } else {
        s_last_right_pane_x=x; s_last_right_pane_y=list_y;
        s_last_right_pane_w=w; s_last_right_pane_h=list_h;
    }
}

void FileManagerApp::RenderStatusBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,STATUS_H,FM_STATUS);
    Graphics::DrawLine(x,y,x+w,y,FM_BORDER);
    FMTab* t = Active(focused_pane);
    int n = EntryCount(focused_pane);
    char info[96]; info[0]=0;
    char nb[16]; int_to_str(n,nb,16); sapp(info,nb,96); sapp(info," items",96);
    if(t->selected>=0 && t->selected<n){
        FMEntry* e = &EntriesOf(focused_pane)[t->selected];
        sapp(info,"  |  ",96);
        sapp(info,e->name,96);
        if(!e->is_dir){
            char sz[16]; size_to_human(e->size,sz,16);
            sapp(info,"  ",96); sapp(info,sz,96);
        }
    }
    Graphics::DrawString(x+8,y+3,info,FM_DIM,0xFF000000);
    if(clipboard_has_item){
        const char* lbl = clipboard_cut?"[CUT] ":"[COPY] ";
        Graphics::DrawString(x+w-260,y+3,lbl,FM_AMBER,0xFF000000);
        Graphics::DrawString(x+w-200,y+3,clipboard_name,FM_DIM,0xFF000000);
    }
}

void FileManagerApp::RenderContextMenu(int ox,int oy){
    const int ITEM_H=22;
    const int MENU_W=180;
    const char* items[] = {
        "Open","Open With Editor","Open With Player","Copy","Cut","Paste",
        "Rename","Move to Trash","Delete Permanently","Properties","---",
        "New Folder","New File","---","Empty Trash","Refresh"
    };
    const int N=16;
    int px = ox + context_menu_x;
    int py = oy + context_menu_y;
    Graphics::FillRect(px,py,MENU_W,N*ITEM_H+4,KSS::T().surface);
    Graphics::DrawRect(px,py,MENU_W,N*ITEM_H+4,FM_BORDER);
    for(int i=0;i<N;i++){
        int iy = py + 2 + i*ITEM_H;
        if(items[i][0]=='-'){
            Graphics::FillRect(px+6,iy+10,MENU_W-12,1,FM_BORDER);
        } else {
            uint32_t col = FM_TEXT;
            if((i==5) && !clipboard_has_item) col = KSS::T().off;  // paste disabled (satoru)
            Graphics::DrawString(px+12,iy+4,items[i],col,0xFF000000);
        }
    }
}

void FileManagerApp::RenderSortMenu(int ox,int oy){
    const int ITEM_H=22, MENU_W=140;
    const char* items[] = {"Name","Size","Type","Date","---","Ascending","Descending"};
    const int N=7;
    int px = ox + 288;
    int py = oy + TAB_BAR_H + TOOLBAR_H + 4;
    Graphics::FillRect(px,py,MENU_W,N*ITEM_H+4,KSS::T().surface);
    Graphics::DrawRect(px,py,MENU_W,N*ITEM_H+4,FM_BORDER);
    FMTab* t = Active(focused_pane);
    for(int i=0;i<N;i++){
        int iy = py + 2 + i*ITEM_H;
        if(items[i][0]=='-'){
            Graphics::FillRect(px+6,iy+10,MENU_W-12,1,FM_BORDER);
        } else {
            bool checked=false;
            if(i<4) checked = (t->sort_mode==i);
            else if(i==5) checked = !t->sort_desc;
            else if(i==6) checked = t->sort_desc;
            if(checked) Graphics::DrawString(px+8,iy+4,"*",FM_ACCENT,0xFF000000);
            Graphics::DrawString(px+22,iy+4,items[i],FM_TEXT,0xFF000000);
        }
    }
}

// the imported ssstik video carries a creator credit shown in its properties
// for attribution (tiktok @neitux.vfx). detect it by path so the render and the
// close-button hit-test agree on the (taller) dialog height. (satoru)
static bool fm_path_has_ssstik(const char* path){
    for (const char* a = path; *a; ++a) {
        const char* b = "ssstik"; const char* c = a;
        while (*b && *c && *b == *c) { ++b; ++c; }
        if (!*b) return true;
    }
    return false;
}

void FileManagerApp::RenderProperties(int cx,int cy,int cw,int ch){
    bool credited = fm_path_has_ssstik(properties_path);
    int dw=320, dh = credited ? 268 : 220;
    int dx = cx + (cw-dw)/2, dy = cy + (ch-dh)/2;
    Graphics::FillRectAlpha(cx,cy,cw,ch,180,0xFF000000);
    Graphics::FillRoundedRect(dx,dy,dw,dh,8,KSS::T().surface);
    Graphics::DrawRect(dx,dy,dw,dh,FM_BORDER);
    Graphics::DrawString(dx+12,dy+10,"Properties",FM_TEXT,0xFF000000);
    Graphics::DrawLine(dx,dy+32,dx+dw,dy+32,FM_BORDER);
    KVFSNode* n = KVFS::ResolvePath(properties_path);
    if(!n){
        Graphics::DrawString(dx+12,dy+44,"(missing)",FM_RED,0xFF000000);
    } else {
        char line[160]; line[0]=0;
        sapp(line,"Name: ",160); sapp(line,n->name,160);
        Graphics::DrawString(dx+12,dy+44,line,FM_TEXT,0xFF000000);
        line[0]=0; sapp(line,"Path: ",160); sapp(line,properties_path,160);
        Graphics::DrawString(dx+12,dy+62,line,FM_TEXT,0xFF000000);
        line[0]=0; sapp(line,"Type: ",160);
        sapp(line, n->is_dir()?"Directory":(n->type==KVFS_SYMLINK?"Symlink":"File"),160);
        Graphics::DrawString(dx+12,dy+80,line,FM_TEXT,0xFF000000);
        line[0]=0; sapp(line,"Size: ",160);
        char szb[16]; size_to_human(n->size,szb,16); sapp(line,szb,160);
        Graphics::DrawString(dx+12,dy+98,line,FM_TEXT,0xFF000000);
        line[0]=0; sapp(line,"Perms: ",160);
        char p[5]={0};
        p[0]=(n->perms.mode&0400)?'r':'-';
        p[1]=(n->perms.mode&0200)?'w':'-';
        p[2]=(n->perms.mode&0100)?'x':'-';
        p[3]=0;
        sapp(line,p,160);
        Graphics::DrawString(dx+12,dy+116,line,FM_TEXT,0xFF000000);
        line[0]=0; sapp(line,"UID: ",160);
        char ub[8]; int_to_str(n->perms.uid,ub,8); sapp(line,ub,160);
        sapp(line,"  GID: ",160);
        int_to_str(n->perms.gid,ub,8); sapp(line,ub,160);
        Graphics::DrawString(dx+12,dy+134,line,FM_TEXT,0xFF000000);
        line[0]=0; sapp(line,"Modified (s since boot): ",160);
        char tb[16]; int_to_str((int)n->modified,tb,16); sapp(line,tb,160);
        Graphics::DrawString(dx+12,dy+152,line,FM_TEXT,0xFF000000);
        if(credited){
            // creator attribution for the imported tiktok video. (satoru)
            Graphics::DrawLine(dx+12,dy+166,dx+dw-12,dy+166,FM_BORDER);
            Graphics::DrawString(dx+12,dy+172,"Credit: neitux.vfx",FM_WHITE,0xFF000000);
            Graphics::DrawString(dx+12,dy+190,"Source: tiktok.com/@neitux.vfx",FM_TEXT,0xFF000000);
            Graphics::DrawString(dx+12,dy+208,"video/7393775041478954246",FM_TEXT,0xFF000000);
        }
    }
    // close button
    Graphics::FillRoundedRect(dx+dw-72,dy+dh-30,60,22,4,FM_BTN);
    Graphics::DrawString(dx+dw-60,dy+dh-26,"Close",FM_WHITE,0xFF000000);
}

void FileManagerApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;
    Graphics::FillRect(cx,cy,cw,ch,FM_BG);
    RenderTabBar(cx,cy,cw);
    RenderToolbar(cx,cy+TAB_BAR_H,cw);
    RenderSidebar(cx,cy+TAB_BAR_H+TOOLBAR_H, SIDEBAR_W, ch-TAB_BAR_H-TOOLBAR_H-STATUS_H);
    int main_x = cx+SIDEBAR_W;
    int main_y = cy+TAB_BAR_H+TOOLBAR_H;
    int main_w = cw-SIDEBAR_W;
    int main_h = ch-TAB_BAR_H-TOOLBAR_H-STATUS_H;
    if(dual_pane){
        int half = main_w/2;
        RenderPane(0, main_x, main_y, half, main_h);
        // split between panes is structural chrome -> hairline border. (satoru)
        Graphics::DrawLine(main_x+half, main_y, main_x+half, main_y+main_h, FM_BORDER);
        RenderPane(1, main_x+half, main_y, main_w-half, main_h);
    } else {
        RenderPane(0, main_x, main_y, main_w, main_h);
        // clear right pane bounds so click handling ignores
        s_last_right_pane_w = 0;
    }
    RenderStatusBar(cx, cy+ch-STATUS_H, cw);
    if(sort_menu_open) RenderSortMenu(cx,cy);
    if(context_menu_open) RenderContextMenu(cx,cy);
    if(properties_open) RenderProperties(cx,cy,cw,ch);
}

// ── input ─────────────────────────────────────────────────────────────
bool FileManagerApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    Window* w = (Window*)win_ptr;
    int cw_local = w->content_w;
    int ch_local = w->content_h;

    // properties dialog
    if(properties_open){
        if(clicked){
            int dw=320, dh = fm_path_has_ssstik(properties_path) ? 268 : 220;
            int dx = (cw_local-dw)/2, dy = (ch_local-dh)/2;
            if(mx>=dx+dw-72 && mx<=dx+dw-12 && my>=dy+dh-30 && my<=dy+dh-8){
                properties_open=false; return true;
            }
            // click outside dialog also closes
            if(mx<dx||mx>dx+dw||my<dy||my>dy+dh){
                properties_open=false; return true;
            }
        }
        if(key==27){ properties_open=false; return true; }
        return true;
    }

    // sort menu
    if(sort_menu_open && clicked){
        const int ITEM_H=22, MENU_W=140;
        int px = 288;
        int py = TAB_BAR_H + TOOLBAR_H + 4;
        if(mx>=px && mx<px+MENU_W && my>=py && my<py+7*ITEM_H+4){
            int it = (my-py-2)/ITEM_H;
            FMTab* t = Active(focused_pane);
            sort_menu_open=false;
            if(it>=0 && it<4){ t->sort_mode=it; SortPane(focused_pane); }
            else if(it==5){ t->sort_desc=false; SortPane(focused_pane); }
            else if(it==6){ t->sort_desc=true;  SortPane(focused_pane); }
            return true;
        }
        sort_menu_open=false;
        return true;
    }

    // context menu
    if(context_menu_open && clicked){
        const int ITEM_H=22, MENU_W=180, N=16;
        int px = context_menu_x, py = context_menu_y;
        if(mx>=px && mx<px+MENU_W && my>=py && my<py+N*ITEM_H+4){
            int it = (my-py-2)/ITEM_H;
            context_menu_open=false;
            switch(it){
                case 0: if(context_menu_idx>=0) OpenEntryPane(context_menu_pane,context_menu_idx); break;
                case 1: { // open with editor
                    if(context_menu_idx>=0){
                        FMTab* t=Active(context_menu_pane);
                        FMEntry* e=&EntriesOf(context_menu_pane)[context_menu_idx];
                        char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,e->name);
                        if(!e->is_dir){ TextEditorApp::OpenFile(fp); PushRecent(fp); }
                    }
                } break;
                case 2: { // open with player
                    if(context_menu_idx>=0){
                        FMTab* t=Active(context_menu_pane);
                        FMEntry* e=&EntriesOf(context_menu_pane)[context_menu_idx];
                        char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,e->name);
                        if(!e->is_dir){ MediaPlayerApp::Open(fp); PushRecent(fp); }
                    }
                } break;
                case 3: if(context_menu_idx>=0) CopyEntryPane(context_menu_pane,context_menu_idx,false); break;
                case 4: if(context_menu_idx>=0) CopyEntryPane(context_menu_pane,context_menu_idx,true);  break;
                case 5: PastePane(context_menu_pane); break;
                case 6: if(context_menu_idx>=0){
                            rename_mode=true; rename_pane=context_menu_pane;
                            rename_target=context_menu_idx;
                            FMEntry* e=&EntriesOf(context_menu_pane)[context_menu_idx];
                            scpy(rename_buf,e->name,FM_NAME_MAX);
                            rename_cursor=slen(rename_buf);
                        } break;
                case 7: if(context_menu_idx>=0) DeleteEntryPane(context_menu_pane,context_menu_idx,false); break;
                case 8: if(context_menu_idx>=0) DeleteEntryPane(context_menu_pane,context_menu_idx,true);  break;
                case 9: if(context_menu_idx>=0){
                            FMTab* t=Active(context_menu_pane);
                            FMEntry* e=&EntriesOf(context_menu_pane)[context_menu_idx];
                            JoinPath(properties_path,FM_MAX_PATH,t->path,e->name);
                            properties_open=true;
                        } break;
                // 10: separator
                case 11: CreateFolderPane(context_menu_pane); break;
                case 12: CreateFilePane(context_menu_pane); break;
                // 13: separator
                case 14: EmptyTrash(); RefreshBoth(); break;
                case 15: RefreshBoth(); break;
            }
            return true;
        }
        context_menu_open=false;
        return true;
    }

    // address bar editing
    {
        FMTab* t = Active(focused_pane);
        if(t->address_edit){
            if(key=='\n' || key=='\r'){
                NavigateToPane(focused_pane, t->address_buf, true);
                t->address_edit=false; return true;
            }
            if(key==27){ t->address_edit=false; return true; }
            if(key==8){
                if(t->address_cursor>0){ t->address_buf[--t->address_cursor]=0; }
                return true;
            }
            if(key>=32 && key<127 && t->address_cursor<FM_MAX_PATH-1){
                t->address_buf[t->address_cursor++]=key;
                t->address_buf[t->address_cursor]=0;
                return true;
            }
            // allow click outside address bar to commit-cancel
            if(clicked){ t->address_edit=false; }
        }
    }

    // rename inline
    if(rename_mode){
        if(key=='\n'||key=='\r'){
            // reject invalid names (empty, contains '/' or '..')
            if(rename_buf[0]==0){ rename_mode=false; return true; }
            for(int ri=0;rename_buf[ri];ri++){
                if(rename_buf[ri]=='/'){ rename_mode=false; return true; }
            }
            if(rename_buf[0]=='.' && rename_buf[1]=='.' && (rename_buf[2]==0 || rename_buf[2]=='/'))
                { rename_mode=false; return true; }
            if(rename_target<0 || rename_target>=EntryCount(rename_pane)){
                rename_mode=false; return true;
            }
            FMTab* t = Active(rename_pane);
            FMEntry* e = &EntriesOf(rename_pane)[rename_target];
            char old_p[FM_MAX_PATH], new_p[FM_MAX_PATH];
            JoinPath(old_p,FM_MAX_PATH,t->path,e->name);
            JoinPath(new_p,FM_MAX_PATH,t->path,rename_buf);
            // crude rename: copy + unlink (KVFS::Move handles files only; for dirs skip)
            KVFSNode* n = KVFS::ResolvePath(old_p);
            if(n && !n->is_dir()){
                // heap-allocate the copy buffer sized to the file (a big stack
                // frame overflows the GUI process stack). cap at the per-file
                // ceiling so renaming large files doesn't truncate them. (satoru)
                uint32_t need = n->size + 1;
                if(need > KVFS_MAX_FILE_SIZE) need = KVFS_MAX_FILE_SIZE;
                char* buf = (char*)KernelHeap::Alloc(need);
                if(buf){
                    int len = KVFS::ReadFile(old_p,buf,need);
                    KVFS::Unlink(old_p);
                    KVFS::CreateFile(new_p);
                    if(len>0) KVFS::WriteFile(new_p,buf,len);
                    KernelHeap::Free(buf);
                }
            } else if(n && n->is_dir()){
                // mkdir new and recursive copy is too complex; just rename the node in place
                // by mutating its name.
                int i=0; while(rename_buf[i] && i<KVFS_MAX_NAME-1){ n->name[i]=rename_buf[i]; i++; } n->name[i]=0;
                // the parent indexes children by name in a hash table; rebuild it
                // so the renamed dir is reachable by its new name (and the old
                // name stops resolving to it). (satoru)
                if(n->parent) n->parent->rebuild_hash();
            }
            rename_mode=false;
            RefreshBoth();
            notify_desktop_path_changed(old_p);
            notify_desktop_path_changed(new_p);
            return true;
        }
        if(key==27){ rename_mode=false; return true; }
        if(key==8){ if(rename_cursor>0){ rename_buf[--rename_cursor]=0; } return true; }
        if(key>=32 && key<127 && rename_cursor<FM_NAME_MAX-1){
            rename_buf[rename_cursor++]=key;
            rename_buf[rename_cursor]=0;
            return true;
        }
        return true;
    }

    // search-mode editing: when active pane has a search filter that grew via '/' key,
    // typed chars append; backspace removes; Esc clears.
    {
        FMTab* t = Active(focused_pane);
        // search edit only continues if the user pressed '/' which sets search to ""
        // We treat search as "live" if address_edit is false and search has trailing-cursor.
        // Simplification: when search_cursor>=0 and key is printable + not addressed elsewhere, append.
        if(!clicked && t->search_cursor>0 && key>=32 && key<127 && key!='/'){
            if(t->search_cursor < FM_SEARCH_MAX-1){
                t->search[t->search_cursor++]=key;
                t->search[t->search_cursor]=0;
                RefreshPane(focused_pane);
                return true;
            }
        }
    }

    // mouse clicks
    if(clicked){
        // tab bar
        if(my>=0 && my<TAB_BAR_H){
            // tab strip layout match RenderTabBar
            int avail = cw_local - 8 - 28;
            int per = avail / (tab_count>0?tab_count:1);
            if(per>160) per=160; if(per<60) per=60;
            int tx=4;
            for(int i=0;i<tab_count;i++){
                if(mx>=tx && mx<tx+per){
                    if(mx>=tx+per-18 && mx<tx+per){
                        // close
                        CloseTab(i);
                    } else {
                        if(focused_pane==1) active_tab_right=i;
                        else                active_tab=i;
                        RefreshPane(focused_pane);
                    }
                    return true;
                }
                tx += per;
            }
            // "+" button
            if(mx>=tx+4 && mx<tx+26){
                NewTab(Active(focused_pane)->path);
                return true;
            }
            return true;
        }
        // toolbar
        if(my>=TAB_BAR_H && my<TAB_BAR_H+TOOLBAR_H){
            int rx = mx;
            int ry = my-TAB_BAR_H;
            (void)ry;
            if(rx>=  4 && rx< 32){ GoBackPane(focused_pane); return true; }
            if(rx>= 36 && rx< 64){ GoForwardPane(focused_pane); return true; }
            if(rx>= 68 && rx< 96){ GoUpPane(focused_pane); return true; }
            if(rx>=100 && rx<128){ NavigateToPane(focused_pane,"/home/user",true); return true; }
            if(rx>=132 && rx<160){ RefreshBoth(); return true; }
            if(rx>=168 && rx<224){ CreateFolderPane(focused_pane); return true; }
            if(rx>=228 && rx<284){ CreateFilePane(focused_pane); return true; }
            if(rx>=288 && rx<344){ sort_menu_open=!sort_menu_open; return true; }
            if(rx>=348 && rx<404){
                FMTab* t = Active(focused_pane);
                t->view_mode = (FMViewMode)(((int)t->view_mode+1)%3);
                return true;
            }
            if(rx>=408 && rx<480){
                dual_pane = !dual_pane;
                if(dual_pane && tab_count<2){
                    NewTab(Active(0)->path);
                    active_tab_right = tab_count-1;
                }
                RefreshBoth();
                return true;
            }
            if(rx>=488 && rx<516){
                FMTab* t = Active(focused_pane);
                t->show_hidden = !t->show_hidden;
                RefreshBoth();
                return true;
            }
            return true;
        }
        // sidebar
        if(mx<SIDEBAR_W && my>=TAB_BAR_H+TOOLBAR_H){
            int by = TAB_BAR_H+TOOLBAR_H + 24;
            for(int i=0;i<BOOKMARK_COUNT;i++){
                if(my>=by && my<by+22){
                    NavigateToPane(focused_pane, BOOKMARKS[i].path, true);
                    return true;
                }
                by += 22;
            }
            // recent click
            by += 8 + 4 + 18;
            for(int i=0;i<recent_count && i<6;i++){
                if(my>=by && my<by+18){
                    if(KVFS::IsFile(recent[i])){
                        if(is_media_file(recent[i])||is_image_file(recent[i])) MediaPlayerApp::Open(recent[i]);
                        else TextEditorApp::OpenFile(recent[i]);
                    }
                    return true;
                }
                by += 18;
            }
            return true;
        }
        // path bar click → enter address-edit mode for whichever pane
        int main_x = SIDEBAR_W;
        int main_w = cw_local - SIDEBAR_W;
        int main_y = TAB_BAR_H+TOOLBAR_H;
        if(my>=main_y && my<main_y+PATH_BAR_H+2){
            int half = dual_pane ? main_w/2 : main_w;
            int pane = (dual_pane && mx>=main_x+half) ? 1 : 0;
            focused_pane = pane;
            FMTab* t = Active(pane);
            // if click in search box area (last 140px), focus search instead
            int pane_x_off = (pane==1) ? main_x+half : main_x;
            int pane_w     = (pane==1) ? main_w-half : (dual_pane?half:main_w);
            int sb_left = pane_x_off + pane_w - 144;
            if(mx>=sb_left){
                t->search[0]=0; t->search_cursor=0;
                // mark search as live by setting cursor=0 → next chars append
                // (clicked field is considered "search active" while focused; user
                // types chars, Esc clears.)
                return true;
            }
            t->address_edit=true;
            scpy(t->address_buf, t->path, FM_MAX_PATH);
            t->address_cursor = slen(t->address_buf);
            return true;
        }
        // pane content area
        int main_h = ch_local - TAB_BAR_H - TOOLBAR_H - STATUS_H;
        if(my>=main_y && my<main_y+main_h){
            int half = dual_pane ? main_w/2 : main_w;
            int pane = (dual_pane && mx>=main_x+half) ? 1 : 0;
            focused_pane = pane;
            FMTab* t = Active(pane);
            int pane_x_off = (pane==1) ? main_x+half : main_x;
            int pane_w     = (pane==1) ? main_w-half : (dual_pane?half:main_w);
            int focus_off  = 2;
            int list_y     = main_y + focus_off + PATH_BAR_H;
            int row_idx = -1;
            if(my>=list_y){
                int rel_x = mx - pane_x_off;
                int rel_y = my - list_y;
                if(t->view_mode==FM_VIEW_GRID){
                    int cols = pane_w/GRID_CELL; if(cols<1) cols=1;
                    int gc = rel_x/GRID_CELL;
                    int gr = rel_y/GRID_CELL + t->scroll;
                    if(gc>=0 && gc<cols && gr>=0){
                        int idx = gr*cols+gc;
                        if(idx>=0 && idx<EntryCount(pane)) row_idx=idx;
                    }
                } else {
                    int row = rel_y/ROW_H + t->scroll;
                    if(row>=0 && row<EntryCount(pane)) row_idx=row;
                }
            }
            if(key==4){
                // right-click
                context_menu_open=true;
                context_menu_x = mx;
                context_menu_y = my;
                context_menu_pane = pane;
                context_menu_idx = row_idx;
                if(row_idx>=0) t->selected = row_idx;
                return true;
            }
            if(row_idx>=0){
                uint32_t now = Timer::GetRealMs();
                if(row_idx==last_click_idx[pane] && (now-last_click_time[pane])<DOUBLE_CLICK_MS){
                    OpenEntryPane(pane,row_idx);
                    last_click_idx[pane]=-1; last_click_time[pane]=0;
                } else {
                    t->selected = row_idx;
                    last_click_idx[pane] = row_idx;
                    last_click_time[pane] = now;
                }
            } else {
                t->selected = -1;
            }
            return true;
        }
    }

    // raw right-click event without earlier ctxmenu state
    if(key==4 && !clicked){
        int main_x = SIDEBAR_W;
        int main_w = cw_local - SIDEBAR_W;
        int main_y = TAB_BAR_H+TOOLBAR_H;
        int half = dual_pane ? main_w/2 : main_w;
        int pane = (dual_pane && mx>=main_x+half) ? 1 : 0;
        focused_pane = pane;
        FMTab* t = Active(pane); (void)t;
        context_menu_open=true;
        context_menu_x = mx; context_menu_y = my;
        context_menu_pane = pane;
        context_menu_idx = -1;
        // hit-test entry like above
        int pane_x_off = (pane==1) ? main_x+half : main_x;
        int pane_w     = (pane==1) ? main_w-half : (dual_pane?half:main_w);
        int list_y = main_y + 2 + PATH_BAR_H;
        if(my>=list_y){
            int rel_x = mx - pane_x_off;
            int rel_y = my - list_y;
            FMTab* tt = Active(pane);
            if(tt->view_mode==FM_VIEW_GRID){
                int cols = pane_w/GRID_CELL; if(cols<1) cols=1;
                int gc = rel_x/GRID_CELL;
                int gr = rel_y/GRID_CELL + tt->scroll;
                if(gc>=0 && gc<cols && gr>=0){
                    int idx = gr*cols+gc;
                    if(idx>=0 && idx<EntryCount(pane)){ context_menu_idx=idx; tt->selected=idx; }
                }
            } else {
                int row = rel_y/ROW_H + tt->scroll;
                if(row>=0 && row<EntryCount(pane)){ context_menu_idx=row; tt->selected=row; }
            }
        }
        return true;
    }

    // keyboard shortcuts
    if(key){
        const KeyboardState& ks = Keyboard::GetState();
        FMTab* t = Active(focused_pane);
        // letter keys w/ ctrl
        if(ks.ctrl){
            if(key=='c'||key=='C'){ if(t->selected>=0) CopyEntryPane(focused_pane,t->selected,false); return true; }
            if(key=='x'||key=='X'){ if(t->selected>=0) CopyEntryPane(focused_pane,t->selected,true);  return true; }
            if(key=='v'||key=='V'){ PastePane(focused_pane); return true; }
            if(key=='h'||key=='H'){ t->show_hidden=!t->show_hidden; RefreshBoth(); return true; }
            if(key=='t'||key=='T'){ NewTab(t->path); return true; }
            if(key=='w'||key=='W'){ CloseTab(focused_pane==1?active_tab_right:active_tab); return true; }
            if(key=='l'||key=='L'){ t->address_edit=true; scpy(t->address_buf,t->path,FM_MAX_PATH); t->address_cursor=slen(t->address_buf); return true; }
            if(key=='f'||key=='F'){ t->search[0]=0; t->search_cursor=0; return true; }
            if(key=='d'||key=='D'){ dual_pane=!dual_pane; if(dual_pane && tab_count<2){ NewTab(t->path); active_tab_right=tab_count-1; } RefreshBoth(); return true; }
            return true;
        }
        // F-key shortcuts
        if(Keyboard::IsKeyPressed(KEY_F2) && t->selected>=0){
            rename_mode=true; rename_pane=focused_pane; rename_target=t->selected;
            FMEntry* e=&EntriesOf(focused_pane)[t->selected];
            scpy(rename_buf,e->name,FM_NAME_MAX);
            rename_cursor=slen(rename_buf);
            return true;
        }
        if(Keyboard::IsKeyPressed(KEY_F5)){ RefreshBoth(); return true; }
        if(Keyboard::IsKeyPressed(KEY_DELETE) && t->selected>=0){
            DeleteEntryPane(focused_pane,t->selected, ks.shift);
            return true;
        }
        if(key=='\n'||key=='\r'){ if(t->selected>=0) OpenEntryPane(focused_pane,t->selected); return true; }
        if(key==8){ GoBackPane(focused_pane); return true; }     // backspace = back
        if(key=='/'){ // start search
            t->search[0]=0; t->search_cursor=0;
            return true;
        }
        if(key==27){ // escape clears search
            if(t->search[0]){ t->search[0]=0; t->search_cursor=0; RefreshPane(focused_pane); return true; }
        }
        if(key==9){ // Tab swaps focused pane
            if(dual_pane){ focused_pane = focused_pane?0:1; return true; }
        }
        // arrow keys (raw scancodes)
        if(key==(char)0x48){
            if(t->selected>0) t->selected--;
            else if(t->selected<0 && EntryCount(focused_pane)>0) t->selected=0;
            if(t->selected<t->scroll) t->scroll=t->selected<0?0:t->selected;
            return true;
        }
        if(key==(char)0x50){
            int n = EntryCount(focused_pane);
            if(t->selected<n-1) t->selected++;
            // keep visible: nudge scroll if selection runs past the viewport
            int approx_rows = 12;
            if(t->selected - t->scroll >= approx_rows) t->scroll = t->selected - approx_rows + 1;
            return true;
        }
        if(key==(char)0x4B){ if(focused_pane==1 && dual_pane) focused_pane=0; return true; }
        if(key==(char)0x4D){ if(focused_pane==0 && dual_pane) focused_pane=1; return true; }
    }
    return false;
}
