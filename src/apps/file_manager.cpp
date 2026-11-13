//  kurono os - file manager (multi-tab, dual-pane, bookmarks, trash, search)
#include "file_manager.h"
#include "../kui/kui.h"           // the toolkit: one source for the kernel and the ring 3 build (satoru)

// everything below the toolkit that only the kernel build has (open-with into the other built-in
// apps, kvfs node details, the desktop icon refresh, the app event log). the ring 3 build compiles
// the same file with these replaced by plain syscalls, see the host layer further down. (satoru)
#if !defined(KUI_BACKEND_WAYLAND)
#include "../fs/kvfs.h"
#include "../system/logging.h"
#include "../drivers/serial.h"
#include "../ui/desktop.h"
#include "text_editor.h"
#include "media_player.h"
#include "denji_app.h"
#include "terminal.h"
#include "../kcl/kcl.h"
#include "../ui/kss.h"            // classic chrome tokens (satoru)
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)
#include "../kernel/pmm.h"      // lazy files-window tables (satoru)
#include "../media/mediadecoder.h"   // png/jpeg thumbnails (satoru)
#include "../fs/fat32.h"         // mounted removable volume in the drives group (satoru)
#include "../linux/ext4.h"       // mounted linux partition in the drives group (satoru)
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
// the ring 3 build carries its own stb_image, the kernel one calls MediaDecoder (satoru)
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_MALLOC malloc
#define STBI_REALLOC realloc
#define STBI_FREE free
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#endif

// the files window tables are pmm-backed in the kernel build and malloc-backed
// in the ring 3 build. (satoru)
#if defined(KUI_BACKEND_WAYLAND)
#define FM_TABLE_ALLOC(n) malloc(n)
#define FM_TABLE_FREE(p,n) free(p)
#else
#define FM_TABLE_ALLOC(n) PMM::AllocBytes(n)
#define FM_TABLE_FREE(p,n) PMM::FreeBytes(p,n)
#endif

// ── color palette ──────────────────────────────────────────────────────
// re-pointed onto the shared kss theme so files matches the black/grey chrome
// of settings: one accent, neutral surfaces/text/borders, no rainbow. these are
// macros (not statics) so every draw reads the live theme tokens after kss init,
// and a ui.conf theme reload retints the app for free. the former multicolor
// file-type hues all collapse to the single accent (folders) or dim text
// (everything else). (satoru)
#define FM_BG        (fm_tok().bg)
#define FM_TOOLBAR   (fm_tok().header)
#define FM_PATH_BG   (fm_tok().surface)
#define FM_SIDE_BG   (fm_tok().surface)
#define FM_TAB_BG    (fm_tok().bg)
#define FM_TAB_ACT   (fm_tok().surface_hi)
#define FM_SEL       (fm_tok().accent)
#define FM_SEL_INACT (fm_tok().surface_hi)
#define FM_HOVER     (fm_tok().surface_hi)
#define FM_TEXT      (fm_tok().text)
#define FM_DIM       (fm_tok().text_dim)
#define FM_ACCENT    (fm_tok().accent)
// folders/active carry the one accent; all other former hues read as dim text. (satoru)
#define FM_BLUE      (fm_tok().text_dim)
#define FM_GREEN     (fm_tok().text_dim)
#define FM_AMBER     (fm_tok().accent)
#define FM_PURPLE    (fm_tok().text_dim)
#define FM_PINK      (fm_tok().text_dim)
#define FM_CYAN      (fm_tok().text_dim)
#define FM_RED       (fm_tok().text_dim)
#define FM_WHITE     (fm_tok().white)
#define FM_BTN       (fm_tok().surface)
#define FM_BTN_HVR   (fm_tok().surface_hi)
#define FM_BORDER    (fm_tok().border)
#define FM_BORDER_HI (fm_tok().accent)
#define FM_STATUS    (fm_tok().header)
#define FM_FOCUS_BAR (fm_tok().accent)

// ── toolkit shims ─────────────────────────────────────────────────────
// the canvas of the frame being drawn; the draw helpers below read it so the render code keeps its
// old shape (x, y, w, h, colour). valid only inside OnFrame. (satoru)
static kui::Canvas* g_cv = nullptr;
static int g_mx = -1, g_my = -1;   // pointer in window-local pixels, -1 when outside (hover states) (satoru)

#define FM_BODY_PX 16.0f   // what KSS::BodyPx() returned (satoru)

// the chrome tokens the classic style reads. the kernel build takes them from the live kss theme
// (identical to before); the ring 3 build derives them from the kui palette. (satoru)
struct FmTok { uint32_t bg, surface, surface_hi, sel, header, text, text_dim, heading, border, accent, on, off, track, white; };
static FmTok fm_tok(){
    FmTok t;
#if !defined(KUI_BACKEND_WAYLAND)
    const KSS::Theme& k = KSS::T();
    t.bg=k.bg; t.surface=k.surface; t.surface_hi=k.surface_hi; t.sel=k.sel; t.header=k.header; t.text=k.text;
    t.text_dim=k.text_dim; t.heading=k.heading; t.border=k.border; t.accent=k.accent; t.on=k.on; t.off=k.off;
    t.track=k.track; t.white=k.white;
#else
    const kui::Palette& p = kui::CurrentTheme().pal;
    t.bg=p.panel; t.surface=p.panel2; t.surface_hi=p.hover; t.sel=p.accent; t.header=p.panel2; t.text=p.text;
    t.text_dim=p.text2; t.heading=p.text; t.border=p.line; t.accent=p.accent; t.on=p.accent; t.off=p.text2;
    t.track=p.line; t.white=0xFFFFFFFFu;
#endif
    return t;
}
static bool fm_modern(){ return kui::CurrentTheme().style == kui::Style::Modern; }

// Graphics::DrawString drew inter at the body size (16 px, 20 px on 1440p+, times the font scale); the
// theme's ui_scale carries that factor. (satoru)
static float fm_ds_px(){ return FM_BODY_PX * (float)kui::CurrentTheme().ui_scale / 100.0f; }
static int fm_ds(int x,int y,const char* s,uint32_t c){ return g_cv->Text(x,y,kui::Face::Sans,fm_ds_px(),s,c); }
static int fm_dt(int x,int y,float px,const char* s,uint32_t c,bool bold=false){
    return g_cv->Text(x,y,bold?kui::Face::SansBold:kui::Face::Sans,px,s,c);
}
static int fm_mw(float px,const char* s){ return kui::MeasureWidth(kui::Face::Sans,px,s); }

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
// join a directory and a name, tolerating a trailing slash on the dir (satoru)
static void fm_join(char* dst,int max,const char* dir,const char* name){
    scpy(dst,dir,max);
    int n=slen(dst);
    if(n>0 && dst[n-1]!='/') sapp(dst,"/",max);
    sapp(dst,name,max);
}
static const char* fm_basename(const char* p);
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
#if !defined(KUI_BACKEND_WAYLAND)
    if(path_under_desktop(path)) Desktop::RefreshFiles();
#else
    if(path_under_desktop(path)) kui::Launch::RefreshDesktop();   // desktop rescans over the launcher socket (satoru)
#endif
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

// ── host layer ────────────────────────────────────────────────────────
// the few things kui::Fs does not cover (mtime, permissions and owner for the date sort and the
// properties dialog, recursive delete, copy of a tree) and the hand-off to the other built-in apps.
// the kernel build keeps calling kvfs directly, so behaviour is exactly what it was; the ring 3
// build reaches the same data through stat(2) / readdir(3). (satoru)
struct FmNode { char name[FM_NAME_MAX]; bool dir; bool link; int size; int mode; int uid; int gid; unsigned int modified; };

#if !defined(KUI_BACKEND_WAYLAND)
static bool fm_fat_path(const char* path,const char** sub){
    if(!path) return false;
    const char* p="fat32:";
    for(int i=0;p[i];i++) if(path[i]!=p[i]) return false;
    *sub = path + 6;
    if((*sub)[0]==0) *sub="/";
    return true;
}
// the fat32 driver only offers a text listing, so parse one line per entry. (satoru)
static int fm_fat_readdir(const char* sub,FmNode* out,int max){
    char* buf=(char*)kui::Alloc(8192);
    if(!buf) return -1;
    int n=FAT32::ListDir(sub,buf,8192);
    if(n<0){ kui::Free(buf); return -1; }
    int cnt=0, i=0;
    while(i<n && cnt<max){
        int e=i; while(e<n && buf[e]!='\n') e++;
        int nm_start=i; int nm_end=e;
        while(nm_end>nm_start && (buf[nm_end-1]==' ' || buf[nm_end-1]=='\r')) nm_end--;
        // the size / <DIR> column follows two spaces
        int col=nm_end;
        while(col>nm_start && buf[col-1]!=' ') col--;
        int name_end=col;
        while(name_end>nm_start && buf[name_end-1]==' ') name_end--;
        FmNode* o=&out[cnt];
        for(int z=0;z<FM_NAME_MAX-1;z++) o->name[z]=0;
        int L=name_end-nm_start;
        if(L>FM_NAME_MAX-1) L=FM_NAME_MAX-1;
        for(int z=0;z<L;z++) o->name[z]=buf[nm_start+z];
        o->name[L]=0;
        o->dir=false; o->link=false; o->size=0; o->mode=0644; o->uid=0; o->gid=0; o->modified=0;
        if(col>nm_start && buf[col]=='<'){
            o->dir=true;
        } else {
            int sz=0; bool any=false;
            for(int z=col;z<nm_end;z++){
                if(buf[z]>='0'&&buf[z]<='9'){ sz=sz*10+(buf[z]-'0'); any=true; }
                else if(any) break;
            }
            o->size=sz;
        }
        if(o->name[0]) cnt++;
        i=e+1;
    }
    kui::Free(buf);
    return cnt;
}
static bool fm_stat(const char* path, FmNode* o){
    const char* fsub;
    if(fm_fat_path(path,&fsub)){
        if(seq(fsub,"/")){ scpy(o->name,"Removable",FM_NAME_MAX); o->dir=true; o->link=false; o->size=0; o->mode=0755; o->uid=0; o->gid=0; o->modified=0; return true; }
        // a file on the volume; the listing of its parent has the size (satoru)
        char parent[FM_MAX_PATH]; scpy(parent,fsub,FM_MAX_PATH);
        int last=-1; for(int i=0;parent[i];i++) if(parent[i]=='/') last=i;
        if(last<=0) scpy(parent,"/",FM_MAX_PATH); else parent[last]=0;
        FmNode* kids=(FmNode*)kui::Alloc(sizeof(FmNode)*64);
        if(!kids) return false;
        int cnt=fm_fat_readdir(parent,kids,64);
        bool found=false;
        const char* nm=fm_basename(fsub);
        for(int i=0;i<cnt;i++) if(seq(kids[i].name,nm)){ *o=kids[i]; found=true; break; }
        kui::Free(kids);
        return found;
    }
    KVFSNode* n = KVFS::ResolvePath(path);
    if(!n) return false;
    scpy(o->name,n->name,FM_NAME_MAX);
    o->dir = n->is_dir(); o->link = (n->type==KVFS_SYMLINK);
    o->size = n->size; o->mode = n->perms.mode; o->uid = n->perms.uid; o->gid = n->perms.gid;
    o->modified = n->modified;
    return true;
}
// fills out[] with the children of a directory, -1 when path is not one. (satoru)
static int fm_readdir(const char* path, FmNode* out, int max){
    const char* fsub;
    if(fm_fat_path(path,&fsub)) return fm_fat_readdir(fsub,out,max);
    KVFSNode* dir = KVFS::ResolvePath(path);
    if(!dir || dir->type != KVFS_DIR) return -1;
    int cnt=0;
    for(int i=0;i<dir->child_count && cnt<max;i++){
        KVFSNode* c = dir->children[i];
        if(!c) continue;
        FmNode* o=&out[cnt++];
        scpy(o->name,c->name,FM_NAME_MAX);
        o->dir = (c->type==KVFS_DIR); o->link = (c->type==KVFS_SYMLINK);
        o->size = c->size; o->mode = c->perms.mode; o->uid = c->perms.uid; o->gid = c->perms.gid;
        o->modified = c->modified;
    }
    return cnt;
}
static void fm_rmtree(const char* path){
    KVFSNode* nd = KVFS::ResolvePath(path);
    if(nd && nd->is_dir()) KVFS::RmTree(path);
    else KVFS::Unlink(path);
}
static bool fm_create_file(const char* path){ return KVFS::CreateFile(path)>=0; }
static bool fm_mkdir(const char* path){ return KVFS::Mkdir(path)>=0; }
// rename inside one folder: a file is copied under the new name and unlinked (the per-file ceiling
// applies), a directory node is renamed in place and its parent's name hash rebuilt. (satoru)
static void fm_rename(const char* old_p, const char* new_p, const char* new_name){
    KVFSNode* n = KVFS::ResolvePath(old_p);
    if(n && !n->is_dir()){
        // heap-allocate the copy buffer sized to the file (a big stack
        // frame overflows the GUI process stack). cap at the per-file
        // ceiling so renaming large files doesn't truncate them. (satoru)
        uint32_t need = n->size + 1;
        if(need > KVFS_MAX_FILE_SIZE) need = KVFS_MAX_FILE_SIZE;
        char* buf = (char*)kui::Alloc(need);
        if(buf){
            int len = KVFS::ReadFile(old_p,buf,need);
            KVFS::Unlink(old_p);
            KVFS::CreateFile(new_p);
            if(len>0) KVFS::WriteFile(new_p,buf,len);
            kui::Free(buf);
        }
    } else if(n && n->is_dir()){
        // mkdir new and recursive copy is too complex; just rename the node in place
        // by mutating its name.
        int i=0; while(new_name[i] && i<KVFS_MAX_NAME-1){ n->name[i]=new_name[i]; i++; } n->name[i]=0;
        // the parent indexes children by name in a hash table; rebuild it
        // so the renamed dir is reachable by its new name (and the old
        // name stops resolving to it). (satoru)
        if(n->parent) n->parent->rebuild_hash();
    }
}
static void fm_log_event(const char* what, const char* arg){ RuntimeLog::LogAppEvent("files",what,arg); }
// hand a file to another built-in app. these are C++ objects in the gui task today. (satoru)
enum FmOpenKind { FO_TEXT, FO_PLAYER, FO_KVID };
static void fm_open_with(FmOpenKind k, const char* path){
    if(k==FO_TEXT) TextEditorApp::OpenFile(path);
    else if(k==FO_PLAYER) MediaPlayerApp::Open(path);
    else DenjiApp::OpenFile(path);
}
static void fm_run_kcl(const char* cmd){ TerminalApp::Open(); TerminalApp::EnqueueCommand(cmd); }
#else
static bool fm_stat(const char* path, FmNode* o){
    struct stat st;
    if(lstat(path,&st)!=0) return false;
    const char* base=path; for(const char* p=path;*p;++p) if(*p=='/' && p[1]) base=p+1;
    scpy(o->name,base,FM_NAME_MAX);
    o->dir = S_ISDIR(st.st_mode); o->link = S_ISLNK(st.st_mode);
    o->size = (int)st.st_size; o->mode = (int)(st.st_mode & 07777); o->uid = (int)st.st_uid; o->gid = (int)st.st_gid;
    o->modified = (unsigned int)st.st_mtime;
    return true;
}
static int fm_readdir(const char* path, FmNode* out, int max){
    DIR* d = opendir(path);
    if(!d) return -1;
    int cnt=0;
    while(cnt<max){
        struct dirent* de = readdir(d);
        if(!de) break;
        if(de->d_name[0]=='.' && (de->d_name[1]==0 || (de->d_name[1]=='.' && de->d_name[2]==0))) continue;
        char full[FM_MAX_PATH]; full[0]=0;
        sapp(full,path,FM_MAX_PATH); if(!seq(path,"/")) sapp(full,"/",FM_MAX_PATH); sapp(full,de->d_name,FM_MAX_PATH);
        FmNode* o=&out[cnt];
        if(!fm_stat(full,o)) continue;
        scpy(o->name,de->d_name,FM_NAME_MAX);
        cnt++;
    }
    closedir(d);
    return cnt;
}
static void fm_rmtree(const char* path){
    FmNode n;
    if(fm_stat(path,&n) && n.dir && !n.link){
        FmNode* kids = (FmNode*)kui::Alloc(sizeof(FmNode)*FM_MAX_ENTRIES);
        if(kids){
            int c = fm_readdir(path,kids,FM_MAX_ENTRIES);
            for(int i=0;i<c;i++){
                char sub[FM_MAX_PATH]; sub[0]=0;
                sapp(sub,path,FM_MAX_PATH); if(!seq(path,"/")) sapp(sub,"/",FM_MAX_PATH); sapp(sub,kids[i].name,FM_MAX_PATH);
                fm_rmtree(sub);
            }
            kui::Free(kids);
        }
        rmdir(path);
    } else unlink(path);
}
static bool fm_create_file(const char* path){ return kui::Fs::WriteFile(path,"",0); }
static bool fm_mkdir(const char* path){ return mkdir(path,0755)==0; }
static void fm_rename(const char* old_p, const char* new_p, const char*){ kui::Fs::Rename(old_p,new_p); }
static void fm_log_event(const char* what, const char* arg){ (void)arg; kui::Log("files",what); }
// ring 3 hands the request to the desktop launcher, which opens the same registered apps (satoru)
enum FmOpenKind { FO_TEXT, FO_PLAYER, FO_KVID };
static void fm_open_with(FmOpenKind k, const char* path){
    if(k==FO_TEXT) kui::Launch::App("texteditor",path);
    else if(k==FO_PLAYER) kui::Launch::App("mediaplayer",path);
    else kui::Launch::Open(path);   // kvid: the desktop maps it to the video player (satoru)
}
static void fm_run_kcl(const char* cmd){
    // the caller builds "kcl <path>": run the script itself, not the command line (satoru)
    const char* p = cmd;
    while(*p && *p!=' ') p++;
    while(*p==' ') p++;
    kui::Launch::RunScript(p);
}
#endif

// ── chunked io, mounts, trash metadata ────────────────────────────────
// progress copy works on descriptors so one file can be moved in slices
// across frames instead of blocking the frame that started it. (satoru)
#if !defined(KUI_BACKEND_WAYLAND)
static int  fm_open_read(const char* p){ return KVFS::Open(p,1); }
static int  fm_read_chunk(int fd, void* b, int n){ return KVFS::Read(fd,b,(uint32_t)n); }
static int  fm_open_write(const char* p){ return KVFS::Open(p,2); }
static int  fm_write_chunk(int fd, const void* b, int n){ return KVFS::Write(fd,b,(uint32_t)n); }
static void fm_close_fd(int fd){ KVFS::Close(fd); }
#else
static int  fm_open_read(const char* p){ return open(p,O_RDONLY); }
static int  fm_read_chunk(int fd, void* b, int n){ return (int)read(fd,b,(size_t)n); }
static int  fm_open_write(const char* p){ return open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); }
static int  fm_write_chunk(int fd, const void* b, int n){ return (int)write(fd,b,(size_t)n); }
static void fm_close_fd(int fd){ if(fd>=0) close(fd); }
#endif

// read a whole file into a kui allocation, caller frees. (satoru)
static char* fm_read_all(const char* path, int* out_len, int cap){
#if !defined(KUI_BACKEND_WAYLAND)
    const char* fsub;
    if(fm_fat_path(path,&fsub)){
        int fs=FAT32::GetFileSize(fsub);
        if(fs<=0 || fs>cap) return nullptr;
        char* fb=(char*)kui::Alloc((size_t)fs);
        if(!fb) return nullptr;
        int rn=FAT32::ReadFile(fsub,fb,(uint32_t)fs);
        if(rn<=0){ kui::Free(fb); return nullptr; }
        *out_len=rn;
        return fb;
    }
#endif
    int sz = kui::Fs::FileSize(path);
    if(sz <= 0 || sz > cap) return nullptr;
    char* buf = (char*)kui::Alloc((size_t)sz);
    if(!buf) return nullptr;
    int fd = fm_open_read(path);
    if(fd < 0){ kui::Free(buf); return nullptr; }
    int got = 0;
    while(got < sz){
        int n = fm_read_chunk(fd, buf+got, sz-got);
        if(n <= 0) break;
        got += n;
    }
    fm_close_fd(fd);
    if(got <= 0){ kui::Free(buf); return nullptr; }
    *out_len = got;
    return buf;
}

// mount points the sidebar can browse. the kernel asks the live drivers;
// the ring 3 build probes the same paths through the linux abi. (satoru)
struct FmDrive { char label[24]; char path[FM_MAX_PATH]; char kind[12]; };
#if !defined(KUI_BACKEND_WAYLAND)
static int fm_drives(FmDrive* out, int max){
    int n = 0;
    if(n < max){ scpy(out[n].label,"Kurono",24); scpy(out[n].path,"/",FM_MAX_PATH); scpy(out[n].kind,"kvfs",12); n++; }
    if(Ext4::IsMounted() && n < max){
        scpy(out[n].label,"Linux root",24); scpy(out[n].path,"/linux",FM_MAX_PATH); scpy(out[n].kind,"ext4",12); n++;
    }
    if(FAT32::IsMounted() && n < max){
        scpy(out[n].label,"Removable",24); scpy(out[n].path,"fat32:/",FM_MAX_PATH); scpy(out[n].kind,"fat32",12); n++;
    }
    // mounts done by linux programs (ntfs-3g, user binds) land under /mnt or /media
    static const char* roots[] = {"/mnt","/media"};
    for(int r=0;r<2 && n<max;r++){
        FmNode* kids = (FmNode*)kui::Alloc(sizeof(FmNode)*16);
        if(!kids) continue;
        int cnt = fm_readdir(roots[r],kids,16);
        for(int i=0;i<cnt && n<max;i++){
            if(!kids[i].dir) continue;
            char p[FM_MAX_PATH]; fm_join(p,FM_MAX_PATH,roots[r],kids[i].name);
            scpy(out[n].label,kids[i].name,24);
            scpy(out[n].path,p,FM_MAX_PATH);
            scpy(out[n].kind,"mount",12);
            n++;
        }
        kui::Free(kids);
    }
    return n;
}
#else
static int fm_drives(FmDrive* out, int max){
    int n = 0;
    if(n < max){ scpy(out[n].label,"Kurono",24); scpy(out[n].path,"/",FM_MAX_PATH); scpy(out[n].kind,"kvfs",12); n++; }
    if(kui::Fs::IsDir("/linux") && n < max){
        scpy(out[n].label,"Linux root",24); scpy(out[n].path,"/linux",FM_MAX_PATH); scpy(out[n].kind,"ext4",12); n++;
    }
    static const char* roots[] = {"/mnt","/media"};
    for(int r=0;r<2 && n<max;r++){
        kui::DirEntry* kids = (kui::DirEntry*)kui::Alloc(sizeof(kui::DirEntry)*16);
        if(!kids) continue;
        int cnt = kui::Fs::ListDir(roots[r],kids,16);
        for(int i=0;i<cnt && n<max;i++){
            if(!kids[i].dir) continue;
            char p[FM_MAX_PATH]; fm_join(p,FM_MAX_PATH,roots[r],kids[i].name);
            scpy(out[n].label,kids[i].name,24);
            scpy(out[n].path,p,FM_MAX_PATH);
            scpy(out[n].kind,"mount",12);
            n++;
        }
        kui::Free(kids);
    }
    return n;
}
#endif

// a small sidecar next to a trashed item remembers where it came from. (satoru)
static void fm_trashinfo_path(const char* trash_path, char* out, int mx){
    scpy(out,trash_path,mx); sapp(out,".trashinfo",mx);
}
static void fm_write_trashinfo(const char* trash_path, const char* origin){
#if !defined(KUI_BACKEND_WAYLAND)
    char ip[FM_MAX_PATH]; fm_trashinfo_path(trash_path,ip,FM_MAX_PATH);
    KVFS::WriteString(ip,origin);
#else
    char ip[FM_MAX_PATH]; fm_trashinfo_path(trash_path,ip,FM_MAX_PATH);
    kui::Fs::WriteFile(ip,origin,(int)slen(origin));
#endif
}
static bool fm_read_trashinfo(const char* trash_path, char* origin, int mx){
    char ip[FM_MAX_PATH]; fm_trashinfo_path(trash_path,ip,FM_MAX_PATH);
    origin[0]=0;
#if !defined(KUI_BACKEND_WAYLAND)
    return KVFS::ReadString(ip,origin,mx) > 0;
#else
    int n = kui::Fs::ReadFile(ip,origin,mx-1);
    if(n <= 0) return false;
    origin[n]=0;
    return true;
#endif
}

// ── extension classification ──────────────────────────────────────────
static const char* MEDIA_EXT[]={"mp4","wav","mp3","ogg","flac","avi","mkv","webm","m4a","opus"};
static const char* VIDEO_EXT[]={"kvid","mp4","mkv","avi","webm","mov","m4v"};
static const char* IMG_EXT[]={"png","jpg","jpeg","bmp","gif","ppm","tga","ico","webp"};

static bool is_media_file(const char* n){return ext_in(n,MEDIA_EXT,sizeof(MEDIA_EXT)/sizeof(MEDIA_EXT[0]));}
static bool is_video_file(const char* n){return ext_in(n,VIDEO_EXT,sizeof(VIDEO_EXT)/sizeof(VIDEO_EXT[0]));}
static bool is_image_file(const char* n){return ext_in(n,IMG_EXT,sizeof(IMG_EXT)/sizeof(IMG_EXT[0]));}
// a .kcl script is double-click-runnable through the kcl interpreter. (satoru)
static const char* KCL_EXT[]={"kcl"};
static bool is_kcl_file(const char* n){return ext_in(n,KCL_EXT,sizeof(KCL_EXT)/sizeof(KCL_EXT[0]));}

static unsigned int icon_color_for(const FMEntry* e){
    // monochrome theming: folders take the single accent, files all read as dim
    // text so the type palette no longer competes with the chrome. (satoru)
    if(e->is_dir) return FM_ACCENT;
    return FM_DIM;
}

// ── image thumbnails ──────────────────────────────────────────────────
// a small fixed cache of 48x48 argb thumbnails. decoding happens in the draw
// path but at most a couple of images per frame, so a folder full of pngs
// fills in over a few frames instead of stalling one. (satoru)
#define FM_THUMB_MAX   32
#define FM_THUMB_SZ    48
#define FM_THUMB_FILE  (16*1024*1024)
struct FmThumb {
    char path[FM_MAX_PATH];
    unsigned int stamp;
    uint32_t px[FM_THUMB_SZ*FM_THUMB_SZ];
    bool ok;
};
// 32 thumbnails, ~9 kb each; only a Files window that shows images needs
// them. allocated on the first thumbnail and freed with the last window. (satoru)
static FmThumb* g_thumbs = nullptr;
static int  g_thumb_next = 0;
static int  g_thumb_budget = 0;
static bool g_thumb_pending = false;

// 24/32 bpp uncompressed bmp, which is what the kernel screenshot command
// writes. returns an argb top-down buffer. (satoru)
static uint32_t* fm_decode_bmp(const unsigned char* d, int n, int* ow, int* oh){
    if(n < 54 || d[0]!='B' || d[1]!='M') return nullptr;
    int off = d[10] | (d[11]<<8) | (d[12]<<16) | (d[13]<<24);
    int w = d[18] | (d[19]<<8) | (d[20]<<16) | (d[21]<<24);
    int h = d[22] | (d[23]<<8) | (d[24]<<16) | (d[25]<<24);
    int bpp = d[28] | (d[29]<<8);
    int comp = d[30] | (d[31]<<8) | (d[32]<<16) | (d[33]<<24);
    if(w <= 0 || w > 8192 || h == 0 || h < -8192 || h > 8192) return nullptr;
    if((bpp != 24 && bpp != 32) || comp != 0) return nullptr;
    bool top_down = h < 0;
    if(top_down) h = -h;
    if(h <= 0 || h > 8192) return nullptr;
    int stride = ((w*bpp + 31)/32)*4;
    if(off < 0 || off + stride*h > n) return nullptr;
    uint32_t* out = (uint32_t*)kui::Alloc((size_t)w*h*4);
    if(!out) return nullptr;
    for(int y=0;y<h;y++){
        int sy = top_down ? y : (h-1-y);
        const unsigned char* row = d + off + sy*stride;
        uint32_t* dst = out + (size_t)y*w;
        for(int x=0;x<w;x++){
            unsigned char b = row[x*(bpp/8)+0];
            unsigned char g = row[x*(bpp/8)+1];
            unsigned char r = row[x*(bpp/8)+2];
            dst[x] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
        }
    }
    *ow = w; *oh = h;
    return out;
}

// decode an image into rgba and shrink it into the square cache slot. (satoru)
static bool fm_decode_thumb_px(const char* path, uint32_t* dst){
    int len = 0;
    char* buf = fm_read_all(path,&len,FM_THUMB_FILE);
    if(!buf) return false;
    int w=0,h=0;
    uint32_t* src = nullptr;
#if !defined(KUI_BACKEND_WAYLAND)
    MediaDecoder::Image img = MediaDecoder::DecodeMemory((const uint8_t*)buf,len);
    if(img.valid){
        src = (uint32_t*)kui::Alloc((size_t)img.width*img.height*4);
        if(src){
            const unsigned char* s = img.data;
            for(int i=0;i<img.width*img.height;i++){
                src[i] = ((uint32_t)s[i*4+3]<<24) | ((uint32_t)s[i*4]<<16) |
                         ((uint32_t)s[i*4+1]<<8) | (uint32_t)s[i*4+2];
            }
            w = img.width; h = img.height;
        }
        MediaDecoder::FreeImage(img);
    }
#else
    {
        int comp = 0;
        unsigned char* rgba = stbi_load_from_memory((const unsigned char*)buf,len,&w,&h,&comp,4);
        if(rgba){
            src = (uint32_t*)kui::Alloc((size_t)w*h*4);
            if(src){
                for(int i=0;i<w*h;i++){
                    src[i] = ((uint32_t)rgba[i*4+3]<<24) | ((uint32_t)rgba[i*4]<<16) |
                             ((uint32_t)rgba[i*4+1]<<8) | (uint32_t)rgba[i*4+2];
                }
            }
            stbi_image_free(rgba);
        }
    }
#endif
    if(!src){
        src = fm_decode_bmp((const unsigned char*)buf,len,&w,&h);
        if(!src){ kui::Free(buf); return false; }
    }
    kui::Free(buf);
    for(int i=0;i<FM_THUMB_SZ*FM_THUMB_SZ;i++) dst[i] = 0;
    if(w <= 0 || h <= 0){ kui::Free(src); return false; }
    int scale = (w > h) ? w : h;
    int dw = (w*FM_THUMB_SZ)/scale;
    int dh = (h*FM_THUMB_SZ)/scale;
    if(dw < 1) dw = 1;
    if(dh < 1) dh = 1;
    int ox = (FM_THUMB_SZ-dw)/2, oy = (FM_THUMB_SZ-dh)/2;
    for(int y=0;y<dh;y++){
        int sy = (y*h)/dh;
        for(int x=0;x<dw;x++){
            int sx = (x*w)/dw;
            dst[(oy+y)*FM_THUMB_SZ + ox+x] = src[(size_t)sy*w+sx];
        }
    }
    kui::Free(src);
    return true;
}

// find or build the thumbnail of one entry. (satoru)
static const uint32_t* fm_thumb_get(const char* full,int stamp){
    if(!g_thumbs){
        g_thumbs = (FmThumb*)FM_TABLE_ALLOC(sizeof(FmThumb)*FM_THUMB_MAX);
        if(!g_thumbs) return nullptr;
        for(unsigned char* p=(unsigned char*)g_thumbs,*e=p+sizeof(FmThumb)*FM_THUMB_MAX;p<e;p++) *p=0;
    }
    for(int i=0;i<FM_THUMB_MAX;i++){
        FmThumb* t = &g_thumbs[i];
        if(t->ok && t->stamp==(unsigned int)stamp && seq(t->path,full)) return t->px;
        if(t->ok && t->path[0] && seq(t->path,full) && t->stamp!=(unsigned int)stamp){
            t->ok = false;   // stale after an edit or replace (satoru)
        }
    }
    if(g_thumb_budget <= 0){ g_thumb_pending = true; return nullptr; }
    g_thumb_budget--;
    FmThumb* t = &g_thumbs[g_thumb_next];
    g_thumb_next = (g_thumb_next+1)%FM_THUMB_MAX;
    scpy(t->path,full,FM_MAX_PATH);
    t->stamp = (unsigned int)stamp;
    t->ok = fm_decode_thumb_px(full,t->px);
    return t->ok ? t->px : nullptr;
}

// draw an entry's picture: the cached image if it has one, else the generic
// drawn icon. returns true when a thumbnail was drawn. (satoru)
bool FileManagerApp::DrawEntryIcon(int pane,int idx,int x,int y,int sz){
    FMEntry* e = &EntriesOf(pane)[idx];
    if(!e->is_dir && is_image_file(e->name)){
        char full[FM_MAX_PATH];
        fm_join(full,FM_MAX_PATH,Active(pane)->path,e->name);
        const uint32_t* th = fm_thumb_get(full,(int)(e->modified ^ (unsigned)e->size));
        if(th){
            g_cv->BlitScaled({x,y,sz,sz},th,FM_THUMB_SZ,FM_THUMB_SZ,FM_THUMB_SZ);
            g_cv->RoundOutline(x-1,y-1,sz+2,sz+2,3,FM_BORDER,120);
            return true;
        }
    }
    RenderEntryIcon(x,y,sz,e);
    return false;
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
FMInst*  FileManagerApp::cur = nullptr;
int      FileManagerApp::win_id = -1;   // id of the most recently opened Files window (satoru)
char     FileManagerApp::clipboard_paths[FM_CLIP_MAX][FM_MAX_PATH];
char     FileManagerApp::clipboard_names[FM_CLIP_MAX][FM_NAME_MAX];
int      FileManagerApp::clipboard_count = 0;
bool     FileManagerApp::clipboard_has_item = false;
bool     FileManagerApp::clipboard_cut = false;
char     FileManagerApp::recent[FM_RECENT_MAX][FM_MAX_PATH];
int      FileManagerApp::recent_count = 0;

static const uint32_t DOUBLE_CLICK_MS = 500;
static const int FM_RCLICK = -4;   // Input() key value for a right click (a ctrl-d is ascii 4) (satoru)

// per-window state (satoru)
// every Files window owns one FMInst, held by its FMApp handler. the frame and event
// callbacks point cur at it; the helpers below then read and write through cur.
// the clipboard and the recent list stay shared on purpose, so copy in one window
// pastes in another. (satoru)
#define FM_MAX_INST 8
struct FMInst {
    FMTab tabs[FM_MAX_TABS];
    int   tab_count;
    int   active_tab;        // left pane active tab
    int   active_tab_right;  // right pane active tab (when dual-pane)
    bool  dual_pane;
    int   focused_pane;      // 0=left, 1=right

    FMEntry entries_l[FM_MAX_ENTRIES];
    FMEntry entries_r[FM_MAX_ENTRIES];
    int     entry_count_l;
    int     entry_count_r;

    bool context_menu_open;
    int  context_menu_x, context_menu_y;
    int  context_menu_pane;
    int  context_menu_idx;

    bool properties_open;
    char properties_path[FM_MAX_PATH];

    bool open_with_open;      // the open with chooser overlay (satoru)
    char open_with_path[FM_MAX_PATH];

    // drag state: armed on a press over a row, a drag starts after 6 px. (satoru)
    bool drag_armed;
    bool dragging;
    int  drag_pane;
    int  drag_idx;
    int  drag_x0, drag_y0, drag_x, drag_y;
    bool drag_copy;
    bool drag_on_item;        // hovering a folder row (drop into it) (satoru)
    char drag_dest[FM_MAX_PATH];

    bool sort_menu_open;

    bool rename_mode;
    int  rename_pane;
    int  rename_target;
    char rename_buf[FM_NAME_MAX];
    int  rename_cursor;
    bool rename_fresh;      // first typed key replaces the old name (satoru)

    // double-click detection per pane
    int      last_click_idx[2];
    uint32_t last_click_time[2];
    // remembered last viewport for hit testing in Input()
    int s_last_left_pane_x, s_last_left_pane_y, s_last_left_pane_w, s_last_left_pane_h;
    int s_last_right_pane_x, s_last_right_pane_y, s_last_right_pane_w, s_last_right_pane_h;

    FmDrive drives[FM_DRIVE_MAX];   // sidebar drives, rescanned a few times a minute (satoru)
    int     drive_count;
    uint32_t drives_stamp;

    uint32_t bg_set;   // window body colour last handed to the toolkit, 0 = none yet (satoru)
    int  wid;      // unused since the toolkit port, kept so the slot layout is unchanged (satoru)
    bool used;     // slot is live (satoru)
};

// the per-window Files state is ~82 kb each; the 8-slot table exists only
// while a Files window is open. (satoru)
static FMInst* g_fm = nullptr;

// ── copy / move jobs ──────────────────────────────────────────────────
// a paste or a trash expands into a flat list of directory creations and
// file copies up front. the app moves one slice per tick and draws a bar,
// so a large folder no longer freezes the window. (satoru)
struct FmJob {
    char src[FM_MAX_PATH];
    char dst[FM_MAX_PATH];
    bool dir;
    bool remove_src;
    bool done;
};
struct FmJobs {
    FmJob jobs[FM_JOB_MAX];
    int   count;
    int   next;
    int   files_total;
    int   files_done;
    long long bytes_total;
    long long bytes_done;
    bool  active;
    bool  moving;
    bool  failed;
    int   fd_r, fd_w;
    long long cur_size;
    long long cur_done;
    char  label[FM_NAME_MAX];
    FMInst* owner;
};
static FmJobs g_jobs;
static char g_copy_buf[64*1024];   // job slice, kept off the stack (satoru)


// a slot is freed by the handler's destructor, which the toolkit runs once the window is gone: closing a
// Files window drops its tabs, selection and menus. (satoru)

FMTab* FileManagerApp::Active(int pane){
    int idx = (pane==1) ? cur->active_tab_right : cur->active_tab;
    if(idx<0||idx>=cur->tab_count) idx=0;
    return &cur->tabs[idx];
}
FMEntry* FileManagerApp::EntriesOf(int pane){ return (pane==1)?cur->entries_r:cur->entries_l; }
int      FileManagerApp::EntryCount(int pane){ return (pane==1)?cur->entry_count_r:cur->entry_count_l; }

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
    fm_join(dst,max,dir,name);
}

// ── multi selection ───────────────────────────────────────────────────
bool FileManagerApp::HasSel(int pane,int idx){
    if(idx<0 || idx>=FM_MAX_ENTRIES) return false;
    return Active(pane)->sel[idx] != 0;
}
int FileManagerApp::SelCount(int pane){
    int n=0; FMTab* t=Active(pane);
    for(int i=0;i<FM_MAX_ENTRIES;i++) if(t->sel[i]) n++;
    return n;
}
void FileManagerApp::SelClear(int pane){
    FMTab* t=Active(pane);
    for(int i=0;i<FM_MAX_ENTRIES;i++) t->sel[i]=0;
    t->sel_count=0; t->sel_anchor=-1;
}
void FileManagerApp::SelAll(int pane){
    FMTab* t=Active(pane);
    int n=EntryCount(pane);
    for(int i=0;i<FM_MAX_ENTRIES;i++) t->sel[i]=(i<n)?1:0;
    t->sel_count=n; t->sel_anchor=0;
    if(n>0) t->selected=n-1;
}
void FileManagerApp::SelSetOne(int pane,int idx){
    SelClear(pane);
    FMTab* t=Active(pane);
    if(idx>=0 && idx<EntryCount(pane)){
        t->sel[idx]=1; t->sel_count=1; t->sel_anchor=idx; t->selected=idx;
    }
}
void FileManagerApp::SelToggle(int pane,int idx){
    FMTab* t=Active(pane);
    if(idx<0 || idx>=EntryCount(pane)) return;
    if(t->sel[idx]){ t->sel[idx]=0; if(t->sel_count>0) t->sel_count--; }
    else { t->sel[idx]=1; t->sel_count++; }
    t->sel_anchor=idx; t->selected=idx;
}
void FileManagerApp::SelRange(int pane,int from,int to){
    FMTab* t=Active(pane);
    int n=EntryCount(pane);
    if(from<0) from=0;
    if(to<0) to=0;
    if(from>=n) from=n-1;
    if(to>=n) to=n-1;
    if(from>to){ int x=from; from=to; to=x; }
    SelClear(pane);
    for(int i=from;i<=to;i++) t->sel[i]=1;
    t->sel_count=(to>=from && n>0)?(to-from+1):0;
    t->sel_anchor=from; t->selected=to;
}
// names survive a resort or a refresh, indices do not. (satoru)
static char g_sel_names[FM_MAX_ENTRIES][FM_NAME_MAX];
static int  g_sel_saved;
static char g_sel_cursor[FM_NAME_MAX];
static bool g_sel_has_cursor;
static void sel_capture(FMTab* t, FMEntry* e, int n){
    g_sel_saved=0; g_sel_has_cursor=false;
    for(int i=0;i<n && g_sel_saved<FM_MAX_ENTRIES;i++){
        if(t->sel[i] && e[i].name[0]){
            scpy(g_sel_names[g_sel_saved],e[i].name,FM_NAME_MAX);
            g_sel_saved++;
        }
    }
    if(t->selected>=0 && t->selected<n && e[t->selected].name[0]){
        scpy(g_sel_cursor,e[t->selected].name,FM_NAME_MAX);
        g_sel_has_cursor=true;
    }
}
static void sel_apply(FMTab* t, FMEntry* e, int n){
    for(int i=0;i<FM_MAX_ENTRIES;i++) t->sel[i]=0;
    t->sel_count=0; t->sel_anchor=-1;
    for(int k=0;k<g_sel_saved;k++){
        for(int i=0;i<n;i++){
            if(seq(e[i].name,g_sel_names[k])){ t->sel[i]=1; t->sel_count++; if(t->sel_anchor<0) t->sel_anchor=i; break; }
        }
    }
    if(g_sel_has_cursor){
        t->selected=-1;
        for(int i=0;i<n;i++) if(seq(e[i].name,g_sel_cursor)){ t->selected=i; break; }
    }
}

static void sort_entries(FMTab* t, FMEntry* e, int n){
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

void FileManagerApp::SortPane(int pane){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    sel_capture(t,e,n);
    sort_entries(t,e,n);
    sel_apply(t,e,n);
}

void FileManagerApp::RefreshPane(int pane){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int& cnt = (pane==1)?cur->entry_count_r:cur->entry_count_l;
    sel_capture(t,e,cnt);   // keep the selection by name over the rescan (satoru)
    cnt=0;
    FmNode* kids = (FmNode*)kui::Alloc(sizeof(FmNode)*FM_MAX_ENTRIES);   // too big for the stack (satoru)
    int total = kids ? fm_readdir(t->path,kids,FM_MAX_ENTRIES) : -1;
    if(total < 0){ if(kids) kui::Free(kids); t->selected=-1; t->scroll=0; SelClear(pane); return; }
    for(int i=0;i<total && cnt<FM_MAX_ENTRIES;i++){
        FmNode* c = &kids[i];
        if(!t->show_hidden && c->name[0]=='.') continue;
        // the trash sidecar is bookkeeping, never a row (satoru)
        {
            int nl=slen(c->name);
            if(nl>10 && seq(c->name+nl-10,".trashinfo")) continue;
        }
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
        en->is_dir = c->dir;
        en->size = c->size;
        en->permissions = c->mode;
        en->modified = c->modified;
        cnt++;
    }
    kui::Free(kids);
    sort_entries(t,e,cnt);
    if(cnt == 0){ t->selected=-1; t->scroll=0; }
    else {
        sel_apply(t,e,cnt);
        if(t->selected >= cnt) t->selected = cnt-1;
        if(t->selected < -1) t->selected = -1;
    }
}
void FileManagerApp::RefreshActive(){ RefreshPane(cur->focused_pane); }
void FileManagerApp::RefreshBoth(){ RefreshPane(0); if(cur->dual_pane) RefreshPane(1); }

void FileManagerApp::NotifyFilesystemChanged(const char* path){
    (void)path;
    // refresh every open Files window; this can be called from inside one window's
    // own input handler, so put the current instance back afterwards. (satoru)
    FMInst* saved=cur;
    if(!g_fm) return;
    for(int i=0;i<FM_MAX_INST;i++){
        if(!g_fm[i].used || g_fm[i].tab_count<=0) continue;
        cur=&g_fm[i];
        RefreshBoth();
    }
    cur=saved;
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
    t->selected=-1; t->scroll=0; t->search[0]=0; t->search_cursor=0; t->search_edit=false;
    SelClear(pane);
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
    SelClear(pane);
    RefreshPane(pane);
}
void FileManagerApp::GoForwardPane(int pane){
    FMTab* t = Active(pane);
    if(t->history_pos+1 >= t->history_count) return;
    t->history_pos++;
    scpy(t->path, t->history[t->history_pos], FM_MAX_PATH);
    t->selected=-1; t->scroll=0;
    SelClear(pane);
    RefreshPane(pane);
}

// ── trash ─────────────────────────────────────────────────────────────
void FileManagerApp::EnsureTrashDir(){
    kui::Fs::Mkdirs("/home/user/.Trash");
}

static const char* fm_basename(const char* p){
    const char* b=p;
    for(const char* q=p; q && *q; ++q) if(*q=='/' && q[1]) b=q+1;
    return b;
}

// true when a name is on disk or already reserved by a queued job. (satoru)
static bool fm_name_taken(const char* p){
    if(kui::Fs::Exists(p)) return true;
    for(int i=0;i<g_jobs.count;i++) if(seq(g_jobs.jobs[i].dst,p)) return true;
    return false;
}

// a free name in dir, keeping the extension. suffix is " - copy" or "" for -N. (satoru)
static void fm_unique_name(const char* dir,const char* name,const char* suffix,char* out){
    fm_join(out,FM_MAX_PATH,dir,name);
    if(!fm_name_taken(out)) return;
    int dot=-1;
    for(int i=0;name[i];i++) if(name[i]=='.') dot=i;
    char base[FM_NAME_MAX]; char ext[FM_NAME_MAX];
    if(dot>0){
        int bl=dot; if(bl>FM_NAME_MAX-1) bl=FM_NAME_MAX-1;
        for(int i=0;i<bl;i++) base[i]=name[i];
        base[bl]=0;
        scpy(ext,name+dot,FM_NAME_MAX);
    }
    else { scpy(base,name,FM_NAME_MAX); ext[0]=0; }
    for(int n=1;n<999;n++){
        char nb[8]; int_to_str(n,nb,8);
        char cand[FM_NAME_MAX];
        if(suffix && *suffix){
            scpy(cand,base,FM_NAME_MAX); sapp(cand,suffix,FM_NAME_MAX);
            if(n>1){ sapp(cand," ",FM_NAME_MAX); sapp(cand,nb,FM_NAME_MAX); }
        } else {
            scpy(cand,base,FM_NAME_MAX); sapp(cand,"-",FM_NAME_MAX); sapp(cand,nb,FM_NAME_MAX);
        }
        sapp(cand,ext,FM_NAME_MAX);
        fm_join(out,FM_MAX_PATH,dir,cand);
        if(!fm_name_taken(out)) return;
    }
}

// ── copy / move job queue ─────────────────────────────────────────────
static bool fm_job_add(const char* src,const char* dst,bool dir,bool remove_src){
    if(g_jobs.count >= FM_JOB_MAX) return false;
    FmJob* j=&g_jobs.jobs[g_jobs.count++];
    scpy(j->src,src,FM_MAX_PATH);
    scpy(j->dst,dst,FM_MAX_PATH);
    j->dir=dir; j->remove_src=remove_src; j->done=false;
    return true;
}

// flatten a tree into jobs: directories first, then their files, then the
// source removal. returns files added, -1 when the job list is full. (satoru)
static int fm_enqueue_tree(const char* src,const char* dst,bool remove_src,int depth){
    if(depth>24) return -1;
    FmNode st;
    if(!fm_stat(src,&st)) return -1;
    if(st.dir && !st.link){
        if(!fm_job_add(src,dst,true,false)) return -1;
        FmNode* kids=(FmNode*)kui::Alloc(sizeof(FmNode)*FM_MAX_ENTRIES);
        if(!kids) return -1;
        int cnt=fm_readdir(src,kids,FM_MAX_ENTRIES);
        int added = 0;
        for(int i=0;i<cnt;i++){
            char cs[FM_MAX_PATH], cd[FM_MAX_PATH];
            fm_join(cs,FM_MAX_PATH,src,kids[i].name);
            fm_join(cd,FM_MAX_PATH,dst,kids[i].name);
            int a=fm_enqueue_tree(cs,cd,false,depth+1);
            if(a<0){ kui::Free(kids); return -1; }
            added += a;
        }
        kui::Free(kids);
        if(remove_src){
            if(!fm_job_add(src,dst,true,true)) return -1;
        }
        return added;
    }
    if(!fm_job_add(src,dst,false,remove_src)) return -1;
    g_jobs.files_total++;
    g_jobs.bytes_total += st.size > 0 ? st.size : 0;
    return 1;
}

static void fm_jobs_begin(FMInst* owner,bool moving){
    g_jobs.active=true; g_jobs.moving=moving; g_jobs.failed=false;
    g_jobs.next=0; g_jobs.files_done=0; g_jobs.fd_r=-1; g_jobs.fd_w=-1;
    g_jobs.cur_size=0; g_jobs.cur_done=0; g_jobs.label[0]=0;
    g_jobs.owner=owner;
}

static void fm_jobs_reset(){
    g_jobs.active=false;
    if(g_jobs.fd_r>=0){ fm_close_fd(g_jobs.fd_r); g_jobs.fd_r=-1; }
    if(g_jobs.fd_w>=0){ fm_close_fd(g_jobs.fd_w); g_jobs.fd_w=-1; }
    g_jobs.count=0; g_jobs.next=0; g_jobs.files_total=0; g_jobs.files_done=0;
    g_jobs.bytes_total=0; g_jobs.bytes_done=0;
    g_jobs.owner=nullptr;
}

bool FileManagerApp::JobsActive(){ return g_jobs.active; }

void FileManagerApp::CancelJobs(){
    if(!g_jobs.active) return;
    // drop the half written destination of the file in flight (satoru)
    if(g_jobs.fd_w>=0 && g_jobs.next<g_jobs.count){
        fm_close_fd(g_jobs.fd_w); g_jobs.fd_w=-1;
        kui::Fs::Remove(g_jobs.jobs[g_jobs.next].dst);
    }
    fm_jobs_reset();
    if(cur){ RefreshBoth(); }
}

void FileManagerApp::PumpJobs(kui::Window* w){
    if(!g_jobs.active) return;
    int budget = 32*1024;   // one slice per tick keeps the bar moving and the ui live (satoru)
    bool changed=false;
    while(g_jobs.active && budget>0 && g_jobs.next < g_jobs.count){
        FmJob* j=&g_jobs.jobs[g_jobs.next];
        if(j->dir){
            if(j->remove_src) fm_rmtree(j->src);
            else fm_mkdir(j->dst);
            j->done=true; g_jobs.next++; changed=true;
            continue;
        }
        if(g_jobs.fd_r<0){
            g_jobs.fd_r=fm_open_read(j->src);
            g_jobs.fd_w=(g_jobs.fd_r>=0) ? fm_open_write(j->dst) : -1;
            g_jobs.cur_size = (long long)kui::Fs::FileSize(j->src);
            g_jobs.cur_done=0;
            scpy(g_jobs.label,fm_basename(j->src),FM_NAME_MAX);
            if(g_jobs.fd_r<0 || g_jobs.fd_w<0){
                if(g_jobs.fd_r>=0){ fm_close_fd(g_jobs.fd_r); g_jobs.fd_r=-1; }
                if(g_jobs.fd_w>=0){ fm_close_fd(g_jobs.fd_w); g_jobs.fd_w=-1; }
                g_jobs.failed=true; j->done=true; g_jobs.next++;
                changed=true;
                continue;
            }
        }
        int want = budget < (int)sizeof(g_copy_buf) ? budget : (int)sizeof(g_copy_buf);
        int n=fm_read_chunk(g_jobs.fd_r,g_copy_buf,want);
        if(n < 0){
            fm_close_fd(g_jobs.fd_r); g_jobs.fd_r=-1;
            fm_close_fd(g_jobs.fd_w); g_jobs.fd_w=-1;
            kui::Fs::Remove(j->dst);
            g_jobs.failed=true; j->done=true; g_jobs.next++;
            changed=true;
            continue;
        }
        if(n == 0){
            fm_close_fd(g_jobs.fd_r); g_jobs.fd_r=-1;
            fm_close_fd(g_jobs.fd_w); g_jobs.fd_w=-1;
            if(j->remove_src) kui::Fs::Remove(j->src);
            j->done=true; g_jobs.next++; g_jobs.files_done++;
            changed=true;
            continue;
        }
        int off=0, wr_ok=1;
        while(off<n){
            int wn=fm_write_chunk(g_jobs.fd_w,g_copy_buf+off,n-off);
            if(wn<=0){ wr_ok=0; break; }
            off += wn;
        }
        if(!wr_ok){
            fm_close_fd(g_jobs.fd_r); g_jobs.fd_r=-1;
            fm_close_fd(g_jobs.fd_w); g_jobs.fd_w=-1;
            kui::Fs::Remove(j->dst);
            g_jobs.failed=true; j->done=true; g_jobs.next++;
            changed=true;
            continue;
        }
        g_jobs.bytes_done += n;
        g_jobs.cur_done += n;
        budget -= n;
    }
    if(g_jobs.next >= g_jobs.count && g_jobs.fd_r<0){
        bool failed=g_jobs.failed;
        bool moving=g_jobs.moving;
        fm_jobs_reset();
        RefreshBoth();
        if(w){
            w->Notify(failed?3:1, failed?"Files":"Files",
                      failed?"Some items could not be copied":(moving?"Move complete":"Copy complete"));
        }
        return;
    }
    if(changed && w) w->Invalidate();
    if(w) w->RequestAnimationFrame();
}

void FileManagerApp::MoveToTrash(const char* full_path){
    EnsureTrashDir();
    if(!full_path||!*full_path) return;
    if(seq(full_path,"/")) return;
    // build trash destination using the filename + a small disambiguator
    const char* nm = fm_basename(full_path);
    if(!*nm) return;
    char dst[FM_MAX_PATH];
    // collision: append (n)
    fm_unique_name("/home/user/.Trash",nm,nullptr,dst);
    fm_write_trashinfo(dst,full_path);
    if(!g_jobs.active){
        g_jobs.count=0; g_jobs.files_total=0; g_jobs.bytes_total=0; g_jobs.bytes_done=0;
        fm_jobs_begin(cur,true);
    }
    fm_enqueue_tree(full_path,dst,true,0);
    fm_log_event("trash",full_path);
    if(!g_jobs.active) RefreshBoth();
}

void FileManagerApp::EmptyTrash(){
    EnsureTrashDir();
    // collect names then unlink each
    FmNode* kids = (FmNode*)kui::Alloc(sizeof(FmNode)*64);
    if(!kids) return;
    int cnt = fm_readdir("/home/user/.Trash",kids,64);
    for(int i=0;i<cnt;i++){
        if(kids[i].name[0]=='.' && kids[i].name[1]==0) continue;
        char p[FM_MAX_PATH]; JoinPath(p,FM_MAX_PATH,"/home/user/.Trash",kids[i].name);
        fm_rmtree(p);
    }
    kui::Free(kids);
    fm_log_event("empty-trash","");
}
void FileManagerApp::RestoreFromTrash(const char* trash_name){
    char src[FM_MAX_PATH]; JoinPath(src,FM_MAX_PATH,"/home/user/.Trash",trash_name);
    StartRestore(src);
    fm_log_event("restore",trash_name);
}

void FileManagerApp::StartRestore(const char* path){
    char origin[FM_MAX_PATH];
    if(!fm_read_trashinfo(path,origin,FM_MAX_PATH) || !origin[0]){
        char nm[FM_NAME_MAX]; scpy(nm,fm_basename(path),FM_NAME_MAX);
        fm_join(origin,FM_MAX_PATH,"/home/user",nm);
    }
    char ip[FM_MAX_PATH]; fm_trashinfo_path(path,ip,FM_MAX_PATH);
    kui::Fs::Remove(ip);
    char dst[FM_MAX_PATH];
    const char* nm=fm_basename(origin);
    const char* dir;
    char dtmp[FM_MAX_PATH];
    int cut=0, last=-1;
    for(int i=0;origin[i];i++) if(origin[i]=='/'){ last=i; }
    if(last>0){ for(int i=0;i<last;i++) dtmp[i]=origin[i]; dtmp[last]=0; dir=dtmp; }
    else dir="/home/user";
    fm_unique_name(dir,nm,nullptr,dst);
    if(!g_jobs.active){
        g_jobs.count=0; g_jobs.files_total=0; g_jobs.bytes_total=0; g_jobs.bytes_done=0;
        fm_jobs_begin(cur,true);
    }
    fm_enqueue_tree(path,dst,true,0);
    (void)cut;
    if(!g_jobs.active) RefreshBoth();
}

void FileManagerApp::StartTrashSelection(int pane){
    FMTab* t=Active(pane);
    FMEntry* e=EntriesOf(pane);
    int n=EntryCount(pane);
    if(t->sel_count<=0){
        if(t->selected>=0 && t->selected<n) { char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,e[t->selected].name); MoveToTrash(fp); }
    } else {
        for(int i=0;i<n;i++) if(t->sel[i]){
            char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,e[i].name);
            MoveToTrash(fp);
        }
    }
    SelClear(pane);
    t->selected=-1;
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
    if(cur->tab_count>=FM_MAX_TABS) return -1;
    FMTab* t = &cur->tabs[cur->tab_count];
    scpy(t->path, path?path:"/home/user", FM_MAX_PATH);
    t->selected=-1; t->scroll=0;
    t->history_count=1; t->history_pos=0;
    scpy(t->history[0], t->path, FM_MAX_PATH);
    t->sort_mode = FM_SORT_NAME;
    t->sort_desc = false;
    t->show_hidden = false;
    t->search[0]=0; t->search_cursor=0; t->search_edit=false;
    t->address_edit=false; t->address_fresh=false; t->address_buf[0]=0; t->address_cursor=0;
    t->view_mode = FM_VIEW_LIST;
    for(int si=0;si<FM_MAX_ENTRIES;si++) t->sel[si]=0;
    t->sel_count=0; t->sel_anchor=-1;
    t->used = true;
    int idx = cur->tab_count++;
    cur->active_tab = idx;
    RefreshPane(0);
    return idx;
}
void FileManagerApp::CloseTab(int idx){
    if(cur->tab_count<=1) return;
    if(idx<0||idx>=cur->tab_count) return;
    for(int i=idx;i<cur->tab_count-1;i++) cur->tabs[i] = cur->tabs[i+1];
    cur->tab_count--;
    if(cur->active_tab>=cur->tab_count) cur->active_tab=cur->tab_count-1;
    if(cur->active_tab_right>=cur->tab_count) cur->active_tab_right=cur->tab_count-1;
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
    fm_log_event("open-entry",full);
    if(is_kcl_file(e[idx].name)){
        // double-click a .kcl script: open the terminal and run it through the
        // kcl interpreter so its output (and any errors) are visible. (satoru)
        fm_log_event("run-kcl",full);
        char cmd[FM_MAX_PATH+8];
        int ci=0; const char* pfx="kcl ";
        for(int s=0; pfx[s] && ci<(int)sizeof(cmd)-1; s++) cmd[ci++]=pfx[s];
        for(int s=0; full[s] && ci<(int)sizeof(cmd)-1; s++) cmd[ci++]=full[s];
        cmd[ci]=0;
        fm_run_kcl(cmd);
        return;
    }
    if(is_video_file(e[idx].name)){
        // play through the native kvid player. real-time h264 decode isn't
        // feasible on a freestanding kernel, so a non-kvid video plays its
        // host-transcoded sibling "<name>.kvid" if one exists; otherwise we
        // fall back to the media player's (audio) view. (satoru)
        const char* dot=nullptr; for(const char* p=full;*p;++p) if(*p=='.') dot=p;
        if(dot && (istricmp(dot+1,"kvid")==0 || istricmp(dot+1,"kvd")==0)){
            fm_open_with(FO_KVID,full);
        } else {
            char kv[FM_MAX_PATH];
            int blen = dot ? (int)(dot-full) : 0;
            if(!dot){ while(full[blen]) blen++; }
            int k=0; while(k<blen && k<FM_MAX_PATH-6){ kv[k]=full[k]; k++; }
            const char* suf=".kvid"; for(int s=0; suf[s] && k<FM_MAX_PATH-1; s++) kv[k++]=suf[s];
            kv[k]=0;
            if(kui::Fs::Exists(kv)) fm_open_with(FO_KVID,kv);
            else fm_open_with(FO_PLAYER,full);
        }
    }
    else if(is_media_file(e[idx].name))      fm_open_with(FO_PLAYER,full);
    else if(is_image_file(e[idx].name)) fm_open_with(FO_PLAYER,full);
    else                                 fm_open_with(FO_TEXT,full);
}

void FileManagerApp::DeleteEntryPane(int pane,int idx,bool permanent){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    if(idx<0||idx>=n) return;
    bool multi = HasSel(pane,idx) && t->sel_count>1;
    if(permanent){
        // permanent delete has no progress bar; do it now, multi included (satoru)
        for(int i=0;i<n;i++){
            if(multi ? !t->sel[i] : i!=idx) continue;
            char full[FM_MAX_PATH]; JoinPath(full,FM_MAX_PATH,t->path,e[i].name);
            fm_rmtree(full);
            fm_log_event("delete-permanent",full);
            notify_desktop_path_changed(full);
        }
        // clear selection - the index just got invalidated
        SelClear(pane);
        t->selected=-1;
        RefreshBoth();
        return;
    }
    if(multi) StartTrashSelection(pane);
    else {
        char full[FM_MAX_PATH]; JoinPath(full,FM_MAX_PATH,t->path,e[idx].name);
        MoveToTrash(full);
        SelClear(pane);
        t->selected=-1;
    }
    notify_desktop_path_changed(t->path);
}

void FileManagerApp::CreateFolderPane(int pane){
    FMTab* t = Active(pane);
    char p[FM_MAX_PATH]; JoinPath(p,FM_MAX_PATH,t->path,"New Folder");
    int n=1;
    char base[FM_MAX_PATH]; scpy(base,p,FM_MAX_PATH);
    while(kui::Fs::Exists(p) && n<999){
        scpy(p,base,FM_MAX_PATH);
        sapp(p," ",FM_MAX_PATH);
        char nb[8]; int_to_str(n,nb,8);
        sapp(p,nb,FM_MAX_PATH);
        n++;
    }
    fm_mkdir(p);
    RefreshBoth();
    notify_desktop_path_changed(p);
}
void FileManagerApp::CreateFilePane(int pane){
    FMTab* t = Active(pane);
    char p[FM_MAX_PATH]; JoinPath(p,FM_MAX_PATH,t->path,"untitled.txt");
    int n=1;
    char base[FM_MAX_PATH]; scpy(base,p,FM_MAX_PATH);
    while(kui::Fs::Exists(p) && n<999){
        // strip ".txt", append "-N.txt"
        scpy(p,t->path,FM_MAX_PATH);
        if(!seq(t->path,"/")) sapp(p,"/",FM_MAX_PATH);
        sapp(p,"untitled-",FM_MAX_PATH);
        char nb[8]; int_to_str(n,nb,8);
        sapp(p,nb,FM_MAX_PATH);
        sapp(p,".txt",FM_MAX_PATH);
        n++;
    }
    fm_create_file(p);
    RefreshBoth();
    notify_desktop_path_changed(p);
}

void FileManagerApp::CopySelection(int pane,int idx,bool cut){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    if(idx<0||idx>=n) return;
    clipboard_count = 0;
    bool use_sel = t->sel_count>0;
    for(int i=0;i<n && clipboard_count<FM_CLIP_MAX;i++){
        if(use_sel){ if(!t->sel[i]) continue; }
        else if(i!=idx) continue;
        JoinPath(clipboard_paths[clipboard_count],FM_MAX_PATH,t->path,e[i].name);
        scpy(clipboard_names[clipboard_count],e[i].name,FM_NAME_MAX);
        clipboard_count++;
    }
    clipboard_has_item = clipboard_count>0;
    clipboard_cut = cut;
}
void FileManagerApp::CopyEntryPane(int pane,int idx,bool cut){ CopySelection(pane,idx,cut); }

void FileManagerApp::PastePane(int pane){
    if(!clipboard_has_item || g_jobs.active) return;
    FMTab* t = Active(pane);
    char dst[FM_MAX_PATH];
    int queued=0;
    g_jobs.count=0; g_jobs.files_total=0; g_jobs.bytes_total=0; g_jobs.bytes_done=0;
    bool started=false;
    for(int i=0;i<clipboard_count;i++){
        const char* nm=fm_basename(clipboard_paths[i]);
        // pasting a cut item into its own folder is a no-op (satoru)
        char parent[FM_MAX_PATH]; scpy(parent,clipboard_paths[i],FM_MAX_PATH);
        int last=-1; for(int k=0;parent[k];k++) if(parent[k]=='/') last=k;
        if(last>0) parent[last]=0; else parent[0]=0;
        if(clipboard_cut && seq(parent,t->path)) continue;
        fm_unique_name(t->path,nm,clipboard_cut?nullptr:" - copy",dst);
        if(!started){
            fm_jobs_begin(cur,clipboard_cut);
            started=true;
        }
        int a=fm_enqueue_tree(clipboard_paths[i],dst,clipboard_cut,0);
        if(a<0){ g_jobs.failed=true; break; }
        queued += a;
    }
    bool moving = clipboard_cut;
    if(clipboard_cut){ clipboard_has_item=false; clipboard_cut=false; clipboard_count=0; }
    if(!started){ RefreshBoth(); return; }
    fm_log_event(moving?"move":"copy",t->path);
    if(queued==0 && !g_jobs.failed){ fm_jobs_reset(); RefreshBoth(); }
}

static int fm_sidebar_drives(FMInst* in,FmDrive** out);
// ── drag and drop ─────────────────────────────────────────────────────
// the drop target is recomputed from the same geometry the render and input
// passes use. empty pane space means "this folder", a folder row means that
// folder, and the sidebar means the place or drive under the pointer. (satoru)
static bool fm_drag_target(FMInst* in,int mx,int my,char* out,bool* on_item){
    out[0]=0; *on_item=false;
    int main_x=SIDEBAR_W;
    int main_y=TAB_BAR_H+TOOLBAR_H;
    if(mx<SIDEBAR_W && my>=main_y){
        int by=main_y+24;
        for(int i=0;i<BOOKMARK_COUNT;i++){
            if(my>=by && my<by+22){ scpy(out,BOOKMARKS[i].path,FM_MAX_PATH); return true; }
            by += 22;
        }
        FmDrive* dv=nullptr; int dn=fm_sidebar_drives(in,&dv);
        by += 8+6+18;
        for(int i=0;i<dn;i++){
            if(my>=by && my<by+22){ scpy(out,dv[i].path,FM_MAX_PATH); return true; }
            by += 22;
        }
        return false;
    }
    int cw=in?0:0; (void)cw;
    // panes: width comes from the window, passed in through the last layout (satoru)
    int main_w = in->s_last_left_pane_w + (in->dual_pane ? in->s_last_right_pane_w : 0);
    if(main_w<=0) return false;
    int half = in->dual_pane ? main_w/2 : main_w;
    int pane = (in->dual_pane && mx>=main_x+half) ? 1 : 0;
    int pane_x = (pane==1) ? main_x+half : main_x;
    int pane_w = (pane==1) ? main_w-half : half;
    FMTab* t = (pane==1) ? &in->tabs[in->active_tab_right] : &in->tabs[in->active_tab];
    int list_y = main_y+2+PATH_BAR_H;
    if(my>=main_y && my<list_y){ scpy(out,t->path,FM_MAX_PATH); return true; }
    FMEntry* e = (pane==1) ? in->entries_r : in->entries_l;
    int n = (pane==1) ? in->entry_count_r : in->entry_count_l;
    int idx=-1;
    if(my>=list_y){
        int rel_x=mx-pane_x, rel_y=my-list_y;
        if(t->view_mode==FM_VIEW_GRID){
            int cols=pane_w/GRID_CELL; if(cols<1) cols=1;
            int gc=rel_x/GRID_CELL, gr=rel_y/GRID_CELL+t->scroll;
            if(gc>=0&&gc<cols&&gr>=0){ int ii=gr*cols+gc; if(ii>=0&&ii<n) idx=ii; }
        } else {
            int row=rel_y/ROW_H+t->scroll;
            if(row>=0&&row<n) idx=row;
        }
    }
    if(idx>=0){
        if(!e[idx].is_dir) return false;
        fm_join(out,FM_MAX_PATH,t->path,e[idx].name);
        *on_item=true;
        return true;
    }
    // pane content but no row: the pane's own folder
    int main_h=my; (void)main_h;
    scpy(out,t->path,FM_MAX_PATH);
    return true;
}

// move or copy every dragged item into dest. dest == the trash folder routes
// through MoveToTrash so the sidecar keeps the original path. (satoru)
static void fm_drag_drop(FMInst* in,const char* dest,bool copy){
    if(!dest || !*dest) return;
    FMTab* t = &in->tabs[in->drag_pane==1?in->active_tab_right:in->active_tab];
    FMEntry* e = in->drag_pane==1 ? in->entries_r : in->entries_l;
    int n = in->drag_pane==1 ? in->entry_count_r : in->entry_count_l;
    bool is_trash = seq(dest,"/home/user/.Trash");
    bool started=false;
    int moved=0;
    for(int i=0;i<n;i++){
        bool take = (t->sel_count>0) ? (t->sel[i]!=0) : (i==in->drag_idx);
        if(!take) continue;
        char src[FM_MAX_PATH]; fm_join(src,FM_MAX_PATH,t->path,e[i].name);
        // dropping an item into its own parent is a no-op (satoru)
        char parent[FM_MAX_PATH]; scpy(parent,t->path,FM_MAX_PATH);
        if(seq(dest,parent)) continue;
        // a folder cannot be dropped into itself or below itself (satoru)
        int dl=slen(dest), sl=slen(src);
        bool inside=false;
        if(dl>sl && sl>0){
            int k=0; while(k<sl && src[k]==dest[k]) k++;
            if(k==sl && dest[sl]=='/') inside=true;
        }
        if(inside) continue;
        if(is_trash){ FileManagerApp::MoveToTrash(src); moved++; continue; }
        char dst[FM_MAX_PATH];
        fm_unique_name(dest,e[i].name,copy?" - copy":nullptr,dst);
        if(!started){ fm_jobs_begin(in,copy?false:true); started=true; }
        fm_enqueue_tree(src,dst,!copy,0);
        moved++;
    }
    if(started && moved>0) in->tabs[in->drag_pane==1?in->active_tab_right:in->active_tab].selected=-1;
}

// ── init / open ───────────────────────────────────────────────────────
// the toolkit handler of one Files window: it owns a slot of g_fm and points cur at it around every
// callback, so the helpers above keep working through cur. (satoru)
class FMApp : public kui::AppHandler {
public:
    explicit FMApp(FMInst* i) : inst(i) {}
    ~FMApp() override {
        if(inst){
            if(g_jobs.owner==inst) fm_jobs_reset();
            inst->used=false;
            // last Files window gone: give its state and thumbnail cache back.
            bool any=false;
            if(g_fm) for(int i=0;i<FM_MAX_INST;i++) if(g_fm[i].used){ any=true; break; }
            if(!any){
                if(FileManagerApp::cur && FileManagerApp::cur>=g_fm && FileManagerApp::cur<g_fm+FM_MAX_INST) FileManagerApp::cur=nullptr;
                if(g_fm){ FM_TABLE_FREE(g_fm,sizeof(FMInst)*FM_MAX_INST); g_fm=nullptr; }
                if(g_thumbs){ FM_TABLE_FREE(g_thumbs,sizeof(FmThumb)*FM_THUMB_MAX); g_thumbs=nullptr; }
                g_thumb_next=0;
            }
        }
    }
    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now_ms) override;
    bool OnEvent(kui::Window& w, const kui::Event& e) override;
    void OnTick(kui::Window& w, uint32_t now_ms) override;
    FMInst* inst;
};

// content size = the old 920 x 560 window minus the 1 px borders and the 36 px title bar (satoru)
static const kui::WindowSpec kFilesSpec = { "Files", "files", 918, 523, 480, 320, -1, -1, true, true, 0, -1 };

// a fresh window state in a free slot, nullptr when all slots are taken. (satoru)
kui::AppHandler* FileManagerApp::Make(const char* nav){
    if(!g_fm){
        g_fm = (FMInst*)FM_TABLE_ALLOC(sizeof(FMInst)*FM_MAX_INST);
        if(!g_fm){ kui::Log("files","no memory for a window"); return nullptr; }
        for(unsigned char* p=(unsigned char*)g_fm,*e=p+sizeof(FMInst)*FM_MAX_INST;p<e;p++) *p=0;
    }
    FMInst* f=nullptr;
    for(int i=0;i<FM_MAX_INST;i++) if(!g_fm[i].used){ f=&g_fm[i]; break; }
    if(!f){
        kui::Log("files","no free window slot");
        return nullptr;
    }
    FMInst* saved=cur;
    // fresh state for the new window; the slot is live from here on. (satoru)
    for(unsigned char* p=(unsigned char*)f, *e=p+sizeof(FMInst); p<e; p++) *p=0;
    f->wid=-1;
    f->context_menu_idx=-1;
    f->rename_target=-1;
    f->last_click_idx[0]=-1; f->last_click_idx[1]=-1;
    f->used=true;
    cur=f;
    NewTab("/home/user");
    if(nav && *nav) NavigateToPane(0, nav, true);
    cur=saved;    // Open can run inside another window's handler (satoru)
    return new FMApp(f);
}
static kui::AppHandler* MakeFiles(){ return FileManagerApp::Make(nullptr); }

void FileManagerApp::Init(){
    EnsureTrashDir();
}

#if !defined(KUI_BACKEND_WAYLAND)
int FileManagerApp::Open(){ return OpenAt(nullptr); }
int FileManagerApp::OpenAt(const char* path){
    TaskMem::AppScope _tms(TaskMem::APP_FILES);   // kurono.taskmem=1: charge to the app (satoru)
    Init();
    kui::AppHandler* a = FileManagerApp::Make(path);
    if(!a) return -1;
    kui::Window* w = kui::OpenWindow(kFilesSpec, a);
    if(!w){ delete a; return -1; }
    win_id = 1;   // the toolkit hides the kernel window id; settings only resets this to -1 (satoru)
    fm_log_event("open","");
    return 1;
}
#endif

// ── rendering ─────────────────────────────────────────────────────────
// ── modern chrome (ui.style modern) ─────────────────────────────────────
// the same surfaces and accent as the settings app, light or dark from
// ui.appearance: a quiet toolbar of rounded icon buttons, a sidebar with an
// accent pill on the current place, list rows with drawn folder/document
// icons and a rounded selection. geometry (hit boxes) is unchanged. (satoru)
struct FmPal { uint32_t bg, side, bar, line, text, dim, hover, sel, sel_text, folder, folder2, doc, doc_line; bool dark; };
static FmPal fm_pal(){
    const kui::Palette& P = kui::CurrentTheme().pal;
    FmPal f;
    f.dark = P.dark;
    if (P.dark) {
        f.bg = 0xFF1C1C21; f.side = 0xFF232329; f.bar = 0xFF26262C; f.line = 0xFF34343C;
        f.text = 0xFFECEDF2; f.dim = 0xFF9EA0AB; f.hover = 0xFF2E2E36;
        f.folder = 0xFF5AA9FF; f.folder2 = 0xFF3B86E6; f.doc = 0xFFE8EAF0; f.doc_line = 0xFFB0B4C0;
    } else {
        f.bg = 0xFFFFFFFF; f.side = 0xFFF3F3F6; f.bar = 0xFFF7F7F9; f.line = 0xFFE2E2E8;
        f.text = 0xFF1C1C1E; f.dim = 0xFF7A7B83; f.hover = 0xFFEDEDF1;
        f.folder = 0xFF6CB4FF; f.folder2 = 0xFF4A92EC; f.doc = 0xFFFFFFFF; f.doc_line = 0xFFC2C6D0;
    }
    f.sel = P.accent; f.sel_text = 0xFFFFFFFF;
    return f;
}
static bool fm_hot(int x,int y,int w,int h){
    return g_mx>=x && g_mx<x+w && g_my>=y && g_my<y+h;
}
// small vector glyphs for the toolbar (satoru)
static void fm_glyph(int kind,int cx,int cy,uint32_t c){
    switch(kind){
    case 0: case 1: { // back / forward chevrons (satoru)
        int d = kind==0 ? -1 : 1;
        for(int k=0;k<5;k++){ g_cv->RoundRect(cx + d*2 - d*k - 1, cy - k - 1, 2, 2, 1, c, 255);
                              g_cv->RoundRect(cx + d*2 - d*k - 1, cy + k - 1, 2, 2, 1, c, 255); }
        break; }
    case 2: // up arrow (satoru)
        g_cv->RoundRect(cx-1, cy-4, 2, 10, 1, c, 255);
        for(int k=0;k<5;k++){ g_cv->RoundRect(cx-k-1, cy-5+k, 2, 2, 1, c, 255); g_cv->RoundRect(cx+k-1, cy-5+k, 2, 2, 1, c, 255); }
        break;
    case 3: // home (satoru)
        for(int k=0;k<6;k++){ g_cv->RoundRect(cx-k-1, cy-6+k, 2, 2, 1, c, 255); g_cv->RoundRect(cx+k-1, cy-6+k, 2, 2, 1, c, 255); }
        g_cv->RoundOutline(cx-4, cy-1, 9, 7, 1, c, 255);
        g_cv->RoundOutline(cx-3, cy, 7, 5, 1, c, 120);
        break;
    case 4: { // refresh: an open ring with a head (satoru)
        for(int a=0;a<28;a++){
            if(a>22) continue;
            float t = (float)a/28.0f*6.2832f;
            float sx = 0, cxx = 1; // sin/cos via a small series (satoru)
            float t2 = t; while(t2>3.14159f) t2-=6.28318f;
            float x2 = t2*t2;
            sx = t2*(1-x2/6*(1-x2/20*(1-x2/42)));
            cxx = 1-x2/2*(1-x2/12*(1-x2/30));
            g_cv->RoundRect(cx + (int)(5.0f*cxx) - 1, cy + (int)(5.0f*sx) - 1, 2, 2, 1, c, 220);
        }
        g_cv->RoundRect(cx+4, cy-5, 4, 2, 1, c, 255); g_cv->RoundRect(cx+6, cy-5, 2, 4, 1, c, 255);
        break; }
    }
}
// drawn folder / document icons (satoru)
static void fm_icon(int x,int y,int sz,bool dir,const FmPal& P){
    if(dir){
        int r = sz/6 > 2 ? sz/6 : 2;
        g_cv->RoundRect(x, y + sz/8, sz/2, sz/4, r/2+1, P.folder2, P.folder2, 255);
        g_cv->RoundRect(x, y + sz/4, sz, (sz*3)/4 - sz/16, r, P.folder, P.folder2, 255);
        g_cv->RoundRect(x+1, y + sz/4 + 1, sz-2, 1, 0, 0xFFFFFFFFu, 70);
    } else {
        int w = (sz*3)/4, ox = x + (sz-w)/2;
        g_cv->RoundRect(ox, y, w, sz, 2, P.doc, P.doc, 255);
        g_cv->RoundOutline(ox, y, w, sz, 2, P.dark ? 0xFF000000u : 0xFF9AA0AE, P.dark ? 80 : 120);
        for(int k=0;k<3 && sz>=12;k++) g_cv->RoundRect(ox+3, y+4+k*(sz/5), w-6, 1, 0, P.doc_line, 255);
    }
}

void FileManagerApp::RenderTabBar(int x,int y,int w){
    if(fm_modern()){
        FmPal P = fm_pal();
        g_cv->FillRect(x,y,w,TAB_BAR_H,P.bar);
        int tx=x+4, avail=w-8-28;
        int per = avail/(cur->tab_count>0?cur->tab_count:1);
        if(per>180) per=180;
        if(per<60) per=60;
        for(int i=0;i<cur->tab_count;i++){
            bool act = (i==cur->active_tab) || (cur->dual_pane && i==cur->active_tab_right);
            if(act) g_cv->RoundRect(tx,y+3,per-4,TAB_BAR_H-5,7,P.bg,P.bg,255);
            else if(fm_hot(tx,y+3,per-4,TAB_BAR_H-5)) g_cv->RoundRect(tx,y+3,per-4,TAB_BAR_H-5,7,P.hover,P.hover,255);
            const char* p = cur->tabs[i].path; int len=slen(p); const char* base=p;
            for(int k=len-1;k>=0;k--) if(p[k]=='/'){ base=p+k+1; break; }
            if(!*base) base="/";
            char title[20]; scpy(title,base,18);
            fm_dt(tx+10,y+5,13.5f,title,act?P.text:P.dim,act);
            fm_dt(tx+per-18,y+5,13.5f,"x",P.dim);
            tx += per;
        }
        bool hp = fm_hot(tx+2,y+3,24,TAB_BAR_H-6);
        if(hp) g_cv->RoundRect(tx+2,y+3,24,TAB_BAR_H-6,6,P.hover,P.hover,255);
        g_cv->RoundRect(tx+9,y+TAB_BAR_H/2,10,2,1,P.dim,255);
        g_cv->RoundRect(tx+13,y+TAB_BAR_H/2-4,2,10,1,P.dim,255);
        return;
    }
    g_cv->FillRect(x,y,w,TAB_BAR_H,FM_TAB_BG);
    int tx=x+4;
    int max_tab_w=160;
    int avail = w - 8 - 28; // reserve "+" button
    int per = avail / (cur->tab_count>0?cur->tab_count:1);
    if(per>max_tab_w) per=max_tab_w;
    if(per<60) per=60;
    for(int i=0;i<cur->tab_count;i++){
        bool act = (i==cur->active_tab) || (cur->dual_pane && i==cur->active_tab_right);
        unsigned int bg = act ? FM_TAB_ACT : FM_TAB_BG;
        g_cv->FillRect(tx,y+2,per-2,TAB_BAR_H-2,bg);
        if(act) g_cv->FillRect(tx,y+TAB_BAR_H-2,per-2,2,FM_FOCUS_BAR);
        // tab title = basename of path
        const char* p = cur->tabs[i].path;
        int len=slen(p);
        const char* base=p;
        for(int k=len-1;k>=0;k--) if(p[k]=='/'){ base=p+k+1; break; }
        if(!*base) base="/";
        char title[20]; scpy(title,base,18);
        fm_ds(tx+8,y+6,title, act?FM_TEXT:FM_DIM);
        // close button "x" on right of each tab
        fm_ds(tx+per-14,y+6,"x", FM_DIM);
        tx += per;
    }
    // "+" new tab button
    g_cv->FillRect(tx+4,y+2,22,TAB_BAR_H-4,FM_TAB_BG);
    fm_ds(tx+10,y+6,"+",FM_ACCENT);
}

void FileManagerApp::RenderToolbar(int x,int y,int w){
    if(fm_modern()){
        FmPal P = fm_pal();
        g_cv->FillRect(x,y,w,TOOLBAR_H,P.bar);
        g_cv->FillRect(x,y+TOOLBAR_H-1,w,1,P.line);
        struct MB { int x; int w; int glyph; const char* label; bool on; };
        MB b[] = {
            {4,28,0,nullptr,false},{36,28,1,nullptr,false},{68,28,2,nullptr,false},{100,28,3,nullptr,false},
            {132,28,4,nullptr,false},{168,56,-1,"New Folder",false},{228,56,-1,"New File",false},
            {288,56,-1,"Sort",cur->sort_menu_open},{348,56,-1,"View",false},{408,72,-1,"Dual Pane",cur->dual_pane},
            {488,28,-1,".*",Active(cur->focused_pane)->show_hidden},
            {528,60,-1,"Search",Active(cur->focused_pane)->search_edit},
        };
        for(int i=0;i<12;i++){
            int bx=x+b[i].x, by=y+4, bw=b[i].w, bh=TOOLBAR_H-8;
            bool hot = fm_hot(bx,by,bw,bh);
            if(b[i].on) g_cv->RoundRect(bx,by,bw,bh,7,P.sel,P.sel,P.dark?70:45);
            else if(hot) g_cv->RoundRect(bx,by,bw,bh,7,P.hover,P.hover,255);
            uint32_t ink = b[i].on ? P.sel : P.text;
            if(b[i].glyph>=0) fm_glyph(b[i].glyph, bx+bw/2, by+bh/2, ink);
            else {
                // labels shrink to fit the classic hit box (satoru)
                float px = 13.0f;
                while(px > 10.0f && fm_mw(px,b[i].label) > bw-8) px -= 0.5f;
                int tw = fm_mw(px,b[i].label);
                fm_dt(bx+(bw-tw)/2, by+(bh-(int)(px+3))/2, px, b[i].label, ink);
            }
        }
        return;
    }
    g_cv->FillRect(x,y,w,TOOLBAR_H,FM_TOOLBAR);
    g_cv->DrawLine(x,y+TOOLBAR_H,x+w,y+TOOLBAR_H,FM_BORDER);
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
        {408,72,"Dual",cur->dual_pane?FM_ACCENT:FM_TEXT},
        {488,28,"H",FM_TEXT},     // hidden toggle (Ctrl+H)
        {528,60,"Find",FM_TEXT},
    };
    for(int i=0;i<12;i++){
        g_cv->FillRoundedRect(x+btns[i].x,y+4,btns[i].w,TOOLBAR_H-8,4,FM_BTN);
        fm_ds(x+btns[i].x+6,y+8,btns[i].label,btns[i].color);
    }
}

// the sidebar drives rescan on a slow clock, so a mount made in another
// app shows up without rebuilding the window. (satoru)
static int fm_sidebar_drives(FMInst* in,FmDrive** out){
    uint32_t now=kui::NowMs();
    if(in->drives_stamp==0 || now-in->drives_stamp>2000){
        in->drive_count=fm_drives(in->drives,FM_DRIVE_MAX);
        in->drives_stamp=now ? now : 1;
    }
    *out=in->drives;
    return in->drive_count;
}

void FileManagerApp::RenderSidebar(int x,int y,int w,int h){
    bool modern=fm_modern();
    FmPal P; if(modern) P=fm_pal();
    FMTab* a = Active(cur->focused_pane);
    if(modern){
        g_cv->FillRect(x,y,w,h,P.side);
        g_cv->FillRect(x+w-1,y,1,h,P.line);
        fm_dt(x+12,y+8,11.5f,"Places",P.dim,true);
    } else {
        g_cv->FillRect(x,y,w,h,FM_SIDE_BG);
        g_cv->DrawLine(x+w,y,x+w,y+h,FM_BORDER);
        fm_ds(x+10,y+6,"PLACES",FM_DIM);
    }
    int by = y+24;
    for(int i=0;i<BOOKMARK_COUNT;i++){
        bool current = seq(a->path, BOOKMARKS[i].path);
        if(modern){
            if(current) g_cv->RoundRect(x+6,by,w-12,22,6,P.sel,P.sel,P.dark?60:38);
            else if(fm_hot(x+6,by,w-12,22)) g_cv->RoundRect(x+6,by,w-12,22,6,P.hover,P.hover,255);
            fm_icon(x+12,by+4,14,true,P);
            char lbl[20]; scpy(lbl,BOOKMARKS[i].label,18);
            fm_dt(x+32,by+3,13.5f,lbl,P.text,current);
        } else {
            if(current) g_cv->FillRect(x+2,by,w-4,22,FM_HOVER);
            g_cv->FillRect(x+10,by+7,8,8, current?fm_tok().accent:fm_tok().text_dim);
            char lbl[20]; scpy(lbl,BOOKMARKS[i].label,18);
            fm_ds(x+24,by+5,lbl, current?FM_TEXT:FM_DIM);
        }
        by += 22;
    }
    // drives: every mounted volume we can browse (satoru)
    FmDrive* dv=nullptr; int dn=fm_sidebar_drives(cur,&dv);
    by += 8;
    if(modern){
        g_cv->FillRect(x+10,by,w-20,1,P.line); by += 6;
        fm_dt(x+12,by,11.5f,"Drives",P.dim,true); by += 18;
    } else {
        g_cv->DrawLine(x+8,by,x+w-8,by,FM_BORDER); by += 6;
        fm_ds(x+10,by,"DRIVES",FM_DIM); by += 18;
    }
    for(int i=0;i<dn;i++){
        bool current = seq(a->path, dv[i].path);
        if(modern){
            if(current) g_cv->RoundRect(x+6,by,w-12,22,6,P.sel,P.sel,P.dark?60:38);
            else if(fm_hot(x+6,by,w-12,22)) g_cv->RoundRect(x+6,by,w-12,22,6,P.hover,P.hover,255);
            int gx=x+12, gy=by+4;
            g_cv->RoundRect(gx,gy+2,14,11,2,P.dim,P.dim,220);
            g_cv->RoundRect(gx+3,gy+5,8,2,1,P.side,P.side,255);
            fm_dt(x+32,by+2,13.0f,dv[i].label,P.text,current);
            fm_dt(x+32,by+15-2,10.0f,dv[i].kind,P.dim);
        } else {
            if(current) g_cv->FillRect(x+2,by,w-4,22,FM_HOVER);
            g_cv->FillRoundedRect(x+10,by+6,10,9,1,current?fm_tok().accent:fm_tok().text_dim);
            char lbl[20]; scpy(lbl,dv[i].label,18);
            fm_ds(x+24,by+5,lbl, current?FM_TEXT:FM_DIM);
        }
        by += 22;
    }
    // recent
    by += 8;
    if(modern){
        g_cv->FillRect(x+10,by,w-20,1,P.line); by += 6;
        fm_dt(x+12,by,11.5f,"Recent",P.dim,true); by += 18;
    } else {
        g_cv->DrawLine(x+8,by,x+w-8,by,FM_BORDER); by += 6;
        fm_ds(x+10,by,"RECENT",FM_DIM); by += 18;
    }
    int rmax = (y+h - by) / 20;
    if(rmax>recent_count) rmax=recent_count;
    if(rmax>6) rmax=6;
    for(int i=0;i<rmax;i++){
        const char* p = recent[i];
        int len=slen(p); const char* base=p;
        for(int k=len-1;k>=0;k--) if(p[k]=='/'){ base=p+k+1; break; }
        char nm[18]; scpy(nm,base,16);
        if(fm_hot(x+6,by,w-12,20)) { if(modern) g_cv->RoundRect(x+6,by,w-12,20,6,P.hover,P.hover,255); }
        if(modern){ fm_icon(x+12,by+3,14,false,P); fm_dt(x+32,by+1,13.0f,nm,P.dim); }
        else { fm_ds(x+12,by+2,nm,FM_DIM); }
        by += 20;
    }
}

// ── breadcrumb geometry ───────────────────────────────────────────────
// the render pass and the click hit test both build the same crumb list, so
// a segment is clickable exactly where it is drawn. (satoru)
struct FmCrumb { int x, w, a, b; };   // path[0..b) is the target of this crumb (satoru)
static int fm_crumbs(FMTab* t,int x,bool modern,FmCrumb* out,int max){
    int n=0, cx=x+(modern?10:8);
    int px = modern?13.5f:FM_BODY_PX;
    bool fat = (t->path[0]=='f' && t->path[1]=='a' && t->path[2]=='t');
    if(fat){
        if(n<max){ out[n].x=cx; out[n].w=fm_mw(px,"Removable"); out[n].a=0; out[n].b=7; n++; }
        cx += fm_mw(px,"Removable") + (modern?12:10);
        int i=7;
        while(t->path[i]=='/') i++;
        while(t->path[i] && n<max){
            int j=i; while(t->path[j] && t->path[j]!='/') j++;
            int sl=j-i; if(sl>38) sl=38;
            char seg[40]; for(int k=0;k<sl;k++) seg[k]=t->path[i+k]; seg[sl]=0;
            out[n].x=cx; out[n].w=fm_mw(px,seg); out[n].a=i; out[n].b=j; n++;
            cx += out[n-1].w + (modern?12:10);
            i=j; while(t->path[i]=='/') i++;
        }
        return n;
    }
    if(n<max){ out[n].x=cx; out[n].w=fm_mw(px,"/"); out[n].a=0; out[n].b=1; n++; }
    cx += fm_mw(px,"/") + (modern?12:10);
    int i=1;
    while(t->path[i] && n<max){
        int j=i; while(t->path[j] && t->path[j]!='/') j++;
        int sl=j-i; if(sl>38) sl=38;
        char seg[40]; for(int k=0;k<sl;k++) seg[k]=t->path[i+k]; seg[sl]=0;
        out[n].x=cx; out[n].w=fm_mw(px,seg); out[n].a=i; out[n].b=j; n++;
        cx += out[n-1].w + (modern?12:10);
        i=j; if(t->path[i]=='/') i++;
    }
    return n;
}
// -1 when the click is in the blank part (address edit), else nav prefix length. (satoru)
static int fm_crumb_hit(FMTab* t,int x,int w,int mx,bool modern,char* nav,int navmax){
    (void)w;
    FmCrumb cr[24];
    int n=fm_crumbs(t,x,modern,cr,24);
    for(int i=0;i<n;i++){
        if(mx>=cr[i].x && mx<cr[i].x+cr[i].w+2){
            int b=cr[i].b;
            if(b>=navmax) b=navmax-1;
            for(int k=0;k<b;k++) nav[k]=t->path[k];
            nav[b]=0;
            if(b==0) scpy(nav,"/",navmax);
            return b;
        }
    }
    return -1;
}

void FileManagerApp::RenderPathBar(int pane,int x,int y,int w){
    FMTab* t = Active(pane);
    bool modern = fm_modern() && !t->address_edit;
    FmPal P; if(modern) P=fm_pal();
    if(modern){
        g_cv->FillRect(x,y,w,PATH_BAR_H,P.bg);
        g_cv->FillRect(x,y+PATH_BAR_H-1,w,1,P.line);
        bool show_search = t->search_edit || t->search[0];
        int sw = show_search ? 178 : 0;
        FmCrumb cr[24];
        int n=fm_crumbs(t,x,true,cr,24);
        int search_left = x+w-sw-6;
        for(int i=0;i<n;i++){
            if(cr[i].x+cr[i].w > search_left) break;
            char seg[40]; int sl=cr[i].b-cr[i].a; if(sl>38) sl=38;
            bool fat_root = (i==0 && t->path[0]=='f');
            if(fat_root) scpy(seg,"Removable",40);
            else { for(int k=0;k<sl;k++) seg[k]=t->path[cr[i].a+k]; seg[sl]=0; }
            bool last = (i==n-1);
            fm_dt(cr[i].x,y+4,13.5f,seg,last?P.text:P.dim,last);
            if(!last){
                int sx=cr[i].x+cr[i].w+5;
                for(int k=0;k<4;k++){ g_cv->RoundRect(sx+k-1,y+PATH_BAR_H/2-4+k-1,2,2,1,P.dim,200);
                                      g_cv->RoundRect(sx+k-1,y+PATH_BAR_H/2+4-k-1,2,2,1,P.dim,200); }
            }
        }
        if(show_search){
            g_cv->RoundRect(search_left,y+3,sw,PATH_BAR_H-6,8,t->search_edit?P.hover:P.bg,P.bg,255);
            if(t->search_edit) g_cv->RoundOutline(search_left,y+3,sw,PATH_BAR_H-6,8,P.sel,150);
            g_cv->DrawCircle(search_left+13,y+PATH_BAR_H/2,5,P.dim);
            g_cv->FillRect(search_left+17,y+PATH_BAR_H/2+3,5,2,P.dim);
            const char* shown = t->search[0]?t->search:(t->search_edit?"Search":"");
            fm_dt(search_left+26,y+4,13.0f,shown,t->search[0]?P.text:P.dim);
            if(t->search_edit){
                char cur[FM_SEARCH_MAX]; int cn=t->search_cursor;
                if(cn>FM_SEARCH_MAX-1) cn=FM_SEARCH_MAX-1;
                for(int k=0;k<cn;k++) cur[k]=t->search[k];
                cur[cn]=0;
                int cx2=search_left+26+fm_mw(13.0f,cur);
                g_cv->FillRect(cx2,y+5,1,PATH_BAR_H-10,P.sel);
            }
        }
        return;
    }
    // classic path bar
    g_cv->FillRect(x,y,w,PATH_BAR_H,FM_PATH_BG);
    if(t->address_edit){
        // show editable buffer with text cursor
        g_cv->DrawRect(x+2,y+2,w-4,PATH_BAR_H-4,FM_BORDER_HI);
        fm_ds(x+8,y+5,t->address_buf,FM_TEXT);
        // cursor - measure the text up to the caret rather than assuming 8px
        // glyphs (the old fixed-font math drifted under fontttf). (satoru)
        char acur[FM_MAX_PATH];
        int an=t->address_cursor; if(an>FM_MAX_PATH-1) an=FM_MAX_PATH-1;
        for(int k=0;k<an;k++) acur[k]=t->address_buf[k];
        acur[an]=0;
        int cx = x+8 + fm_mw(FM_BODY_PX, acur);
        g_cv->FillRect(cx,y+4,1,PATH_BAR_H-8,FM_TEXT);
    } else {
        // breadcrumb segments
        bool show_search = t->search_edit || t->search[0];
        int sw = show_search ? 178 : 0;
        FmCrumb cr[24];
        int n=fm_crumbs(t,x,false,cr,24);
        int search_left = x+w-sw-4;
        fm_ds(x+8,y+5,"/",FM_DIM);
        // walk segments
        for(int i=0;i<n;i++){
            if(cr[i].x+cr[i].w > search_left) break;
            char seg[40]; int sl=cr[i].b-cr[i].a; if(sl>38) sl=38;
            bool fat_root = (i==0 && t->path[0]=='f');
            if(fat_root) scpy(seg,"Removable",40);
            else { for(int k=0;k<sl;k++) seg[k]=t->path[cr[i].a+k]; seg[sl]=0; }
            if(!fat_root) fm_ds(cr[i].x,y+5,seg,FM_TEXT);
            if(i<n-1) fm_ds(cr[i].x+cr[i].w+3,y+5,"/",FM_DIM);
        }
        // search box on right
        if(show_search){
            g_cv->FillRect(search_left,y+3,sw,PATH_BAR_H-6,FM_HOVER);
            if(t->search_edit) g_cv->DrawRect(search_left,y+3,sw,PATH_BAR_H-6,FM_BORDER_HI);
            fm_ds(search_left+6,y+5,t->search[0]?"S:":"search:",FM_DIM);
            fm_ds(search_left+22,y+5,t->search,FM_AMBER);
        }
    }
    g_cv->DrawLine(x,y+PATH_BAR_H,x+w,y+PATH_BAR_H,FM_BORDER);
    if(!t->address_edit && t->search_edit){
        // caret in the classic search box (satoru)
        char cur[FM_SEARCH_MAX]; int cn=t->search_cursor;
        if(cn>FM_SEARCH_MAX-1) cn=FM_SEARCH_MAX-1;
        for(int k=0;k<cn;k++) cur[k]=t->search[k];
        cur[cn]=0;
        int cx=x+w-178-4+22+fm_mw(FM_BODY_PX,cur);
        g_cv->FillRect(cx,y+4,1,PATH_BAR_H-8,FM_TEXT);
    }
}

void FileManagerApp::RenderEntryIcon(int x,int y,int sz,FMEntry* e){
    unsigned int c = icon_color_for(e);
    if(e->is_dir){
        g_cv->FillRoundedRect(x,y+sz/4,sz,(sz*3)/4,3,c);
        // folder tab: a subtle darker token instead of the old orange. (satoru)
        g_cv->FillRect(x,y+sz/4,sz/2,sz/8,FM_BORDER);
    } else {
        g_cv->FillRoundedRect(x+sz/8,y,(sz*3)/4,sz,2,c);
        // fold corner
        g_cv->FillRect(x+(sz*5)/8,y,sz/4,sz/4,0xFF000000);
        // ext label for files >=24 px
        if(sz>=24){
            const char* e2 = fname_ext(e->name);
            if(e2[0]){
                char up[5]; int k=0;
                while(e2[k] && k<3){ char ch=e2[k]; if(ch>='a'&&ch<='z') ch-=32; up[k]=ch; k++; }
                up[k]=0;
                fm_ds(x+sz/4,y+sz/2-4,up,FM_WHITE);
            }
        }
    }
}

void FileManagerApp::RenderFileList(int pane,int x,int y,int w,int h){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    int visible = h / ROW_H;
    bool focused = (pane==cur->focused_pane);
    for(int i=0;i<visible && (i+t->scroll)<n;i++){
        int idx = i + t->scroll;
        FMEntry* ent = &e[idx];
        int ry = y + i*ROW_H;
        const bool modern = fm_modern();
        FmPal P; if(modern) P = fm_pal();
        bool selected = (idx==t->selected) || t->sel[idx];
        if(modern){
            // rounded selection inset from the edges, a soft hover, no zebra (satoru)
            if(selected) g_cv->RoundRect(x+4,ry+1,w-8,ROW_H-2,6, focused?P.sel:P.hover, focused?P.sel:P.hover, 255);
            else if(fm_hot(x+4,ry+1,w-8,ROW_H-2)) g_cv->RoundRect(x+4,ry+1,w-8,ROW_H-2,6,P.hover,P.hover,255);
        } else if(selected){
            g_cv->FillRect(x,ry,w,ROW_H, focused?FM_SEL:FM_SEL_INACT);
        } else if(i%2){
            g_cv->FillRect(x,ry,w,ROW_H,fm_tok().surface);
        }
        if(cur->rename_mode && cur->rename_pane==pane && cur->rename_target==idx){
            // draw editable name
            g_cv->FillRect(x+30,ry+2,w-160,ROW_H-4,FM_HOVER);
            fm_ds(x+34,ry+5,cur->rename_buf,FM_TEXT);
            // caret position measured through fontttf, not rename_cursor*8. (satoru)
            char rcur[FM_NAME_MAX];
            int rn=cur->rename_cursor; if(rn>FM_NAME_MAX-1) rn=FM_NAME_MAX-1;
            for(int k=0;k<rn;k++) rcur[k]=cur->rename_buf[k];
            rcur[rn]=0;
            int cx = x+34 + fm_mw(FM_BODY_PX, rcur);
            g_cv->FillRect(cx,ry+4,1,ROW_H-8,FM_TEXT);
        } else if(modern){
            DrawEntryIcon(pane,idx,x+10,ry+3,15);
            char tname[44]; scpy(tname,ent->name,42);
            fm_dt(x+32,ry+3,13.5f,tname, selected?P.sel_text:P.text);
        } else {
            // small icon
            DrawEntryIcon(pane,idx,x+8,ry+5,12);
            char tname[44]; scpy(tname,ent->name,42);
            fm_ds(x+30,ry+5,tname, selected?FM_WHITE:(ent->is_dir?FM_AMBER:FM_TEXT));
        }
        if(modern){
            bool sel = selected && focused;
            uint32_t dc = sel ? 0xFFE6EEFF : P.dim;
            char szs[16];
            if(ent->is_dir) scpy(szs,"--",16); else size_to_human(ent->size,szs,16);
            fm_dt(x+w-130,ry+3,13.5f,szs,dc);
            fm_dt(x+w-70,ry+3,13.5f,fname_ext(ent->name),dc);
            continue;
        }
        // size column
        char sz[16];
        if(ent->is_dir) scpy(sz,"--",16);
        else size_to_human(ent->size,sz,16);
        fm_ds(x+w-130,ry+5,sz,FM_DIM);
        // ext column
        fm_ds(x+w-70,ry+5,fname_ext(ent->name),FM_DIM);
        // perms
        char perm[4];
        perm[0]=(ent->permissions&0400)?'r':'-';
        perm[1]=(ent->permissions&0200)?'w':'-';
        perm[2]=(ent->permissions&0100)?'x':'-';
        perm[3]=0;
        fm_ds(x+w-32,ry+5,perm,FM_DIM);
    }
    if(n>visible && n>0){
        int sb_h = (visible*h)/n; if(sb_h<8) sb_h=8;
        int sb_y = y + (t->scroll*h)/n;
        g_cv->FillRoundedRect(x+w-4,sb_y,3,sb_h,2,FM_BORDER_HI);
    }
}

void FileManagerApp::RenderFileGrid(int pane,int x,int y,int w,int h){
    FMTab* t = Active(pane);
    FMEntry* e = EntriesOf(pane);
    int n = EntryCount(pane);
    int cols = w/GRID_CELL; if(cols<1) cols=1;
    int vis_rows = h/GRID_CELL;
    bool focused = (pane==cur->focused_pane);
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
        bool selected = (i==t->selected) || t->sel[i];
        bool modern = fm_modern();
        FmPal P; if(modern) P=fm_pal();
        if(selected){
            if(modern) g_cv->RoundRect(gx,gy,GRID_CELL-8,GRID_CELL-8,8,focused?P.sel:P.hover,focused?P.sel:P.hover,255);
            else g_cv->FillRoundedRect(gx,gy,GRID_CELL-8,GRID_CELL-8,6,focused?FM_SEL:FM_SEL_INACT);
        } else if(modern && fm_hot(gx,gy,GRID_CELL-8,GRID_CELL-8)){
            g_cv->RoundRect(gx,gy,GRID_CELL-8,GRID_CELL-8,8,P.hover,P.hover,255);
        }
        DrawEntryIcon(pane,i,gx+(GRID_CELL-8)/2-20, gy+8, 40);
        char lbl[14]; scpy(lbl,ent->name,12);
        // center using the measured label width, not len*8. (satoru)
        int tw = fm_mw(FM_BODY_PX, lbl);
        int tx = gx + (GRID_CELL-8)/2 - tw/2;
        if(modern) fm_dt(tx, gy+GRID_CELL-22, 13.0f, lbl, selected?P.sel_text:P.text);
        else fm_ds(tx, gy+GRID_CELL-22, lbl, selected?FM_WHITE:FM_TEXT);
    }
}

void FileManagerApp::RenderFileColumns(int pane,int x,int y,int w,int h){
    // single-column for now (data: name + size). Wider columns view.
    RenderFileList(pane,x,y,w,h);
}

void FileManagerApp::RenderPane(int pane,int x,int y,int w,int h){
    bool focused = (pane==cur->focused_pane);
    // modern: the focus bar only marks the active side of a dual pane (satoru)
    bool bar = focused && (!fm_modern() || cur->dual_pane);
    if(bar) g_cv->FillRect(x,y,w,2,FM_FOCUS_BAR);
    int top = bar ? y+2 : y;
    RenderPathBar(pane, x, top, w);
    int list_y = top + PATH_BAR_H;
    int list_h = (y+h) - list_y;
    FMTab* t = Active(pane);
    if(t->view_mode==FM_VIEW_GRID)         RenderFileGrid(pane,x,list_y,w,list_h);
    else if(t->view_mode==FM_VIEW_COLUMNS) RenderFileColumns(pane,x,list_y,w,list_h);
    else                                   RenderFileList(pane,x,list_y,w,list_h);
    if(pane==0){
        cur->s_last_left_pane_x=x; cur->s_last_left_pane_y=list_y;
        cur->s_last_left_pane_w=w; cur->s_last_left_pane_h=list_h;
    } else {
        cur->s_last_right_pane_x=x; cur->s_last_right_pane_y=list_y;
        cur->s_last_right_pane_w=w; cur->s_last_right_pane_h=list_h;
    }
}

void FileManagerApp::RenderStatusBar(int x,int y,int w){
    const bool modern = fm_modern();
    if(modern){ FmPal P = fm_pal(); g_cv->FillRect(x,y,w,STATUS_H,P.bar); g_cv->FillRect(x,y,w,1,P.line); }
    else { g_cv->FillRect(x,y,w,STATUS_H,FM_STATUS); g_cv->DrawLine(x,y,x+w,y,FM_BORDER); }
    FMTab* t = Active(cur->focused_pane);
    int n = EntryCount(cur->focused_pane);
    int sc = SelCount(cur->focused_pane);
    char info[192]; info[0]=0;
    char nb[16]; int_to_str(n,nb,16); sapp(info,nb,192); sapp(info," items",192);
    if(sc>1){
        sapp(info,"  |  ",192); int_to_str(sc,nb,16); sapp(info,nb,192); sapp(info," selected",192);
        int total=0; FMEntry* se=EntriesOf(cur->focused_pane);
        for(int i=0;i<n;i++) if(t->sel[i] && !se[i].is_dir) total+=se[i].size;
        char sz[16]; size_to_human(total,sz,16); sapp(info,"  ",192); sapp(info,sz,192);
    } else if(t->selected>=0 && t->selected<n){
        FMEntry* e = &EntriesOf(cur->focused_pane)[t->selected];
        sapp(info,"  |  ",192);
        sapp(info,e->name,192);
        if(!e->is_dir){
            char sz[16]; size_to_human(e->size,sz,16);
            sapp(info,"  ",192); sapp(info,sz,192);
        }
    }
    if(t->search[0]){ sapp(info,"  |  search: ",192); sapp(info,t->search,192); }
    if(modern){
        FmPal P = fm_pal();
        fm_dt(x+10,y+2,12.5f,info,P.dim);
        if(clipboard_has_item){
            char cb[96]; cb[0]=0;
            sapp(cb,clipboard_cut?"Cut: ":"Copied: ",96);
            if(clipboard_count>1){ char nb[8]; int_to_str(clipboard_count,nb,8); sapp(cb,nb,96); sapp(cb," items",96); }
            else sapp(cb,clipboard_names[0],96);
            fm_dt(x+w-260,y+2,12.5f,cb,P.dim);
        }
        return;
    }
    fm_ds(x+8,y+3,info,FM_DIM);
    if(clipboard_has_item){
        const char* lbl = clipboard_cut?"[CUT] ":"[COPY] ";
        fm_ds(x+w-260,y+3,lbl,FM_AMBER);
        if(clipboard_count>1){ char nb[8]; int_to_str(clipboard_count,nb,8); fm_ds(x+w-200,y+3,nb,FM_DIM); fm_ds(x+w-184,y+3," items",FM_DIM); }
        else fm_ds(x+w-200,y+3,clipboard_names[0],FM_DIM);
    }
}

// context menu items. the input handler switches on these indices. (satoru)
static const char* const FM_CTX_ITEMS[] = {
    "Open","Open With...","---","Copy","Cut","Paste","Rename","Move to Trash",
    "Delete Permanently","---","Properties","New Folder","New File","---",
    "Select All","Restore","Empty Trash","Refresh"
};
static const int FM_CTX_N = 18;
static const int FM_CTX_ITEM_H = 22;
static const int FM_CTX_W = 186;
static const int FM_CTX_MARGIN = 2;

static void fm_menu_place(FMInst* in,int mx,int my,int cw,int ch){
    int mh=FM_CTX_N*FM_CTX_ITEM_H+FM_CTX_MARGIN*2;
    if(mx+FM_CTX_W > cw-4) mx = cw-4-FM_CTX_W;
    if(my+mh > ch-4) my = ch-4-mh;
    if(mx<4) mx=4;
    if(my<4) my=4;
    in->context_menu_x=mx; in->context_menu_y=my;
}
static bool fm_in_trash(const char* path){
    static const char* t="/home/user/.Trash";
    for(int i=0;t[i];i++) if(path[i]!=t[i]) return false;
    return path[slen(t)]==0 || path[slen(t)]=='/';
}

void FileManagerApp::RenderContextMenu(int ox,int oy){
    int px = ox + cur->context_menu_x;
    int py = oy + cur->context_menu_y;
    bool modern=fm_modern();
    FmPal P; if(modern) P=fm_pal();
    int mh=FM_CTX_N*FM_CTX_ITEM_H+FM_CTX_MARGIN*2;
    int chh=g_cv->Height();
    if(py<0) py=0;
    if(py+mh > oy+chh) py = oy+chh-mh;   // the menu is clamped when opened (satoru)
    if(modern) g_cv->RoundRect(px,py,FM_CTX_W,mh,8,P.bar,P.bar,250);
    else g_cv->FillRect(px,py,FM_CTX_W,mh,fm_tok().surface);
    g_cv->DrawRect(px,py,FM_CTX_W,mh,FM_BORDER);
    bool has_item = cur->context_menu_idx>=0;
    bool in_trash = fm_in_trash(Active(cur->context_menu_pane)->path);
    for(int i=0;i<FM_CTX_N;i++){
        int iy = py + FM_CTX_MARGIN + i*FM_CTX_ITEM_H;
        if(FM_CTX_ITEMS[i][0]=='-'){
            g_cv->FillRect(px+8,iy+FM_CTX_ITEM_H/2,FM_CTX_W-16,1,FM_BORDER);
            continue;
        }
        bool enabled=true;
        if(i==0||i==1||i==3||i==4||i==6||i==7||i==8||i==10) enabled=has_item;
        if(i==5) enabled=clipboard_has_item && !g_jobs.active;
        if(i==15) enabled=in_trash && has_item;
        if(i==16) enabled=in_trash;
        if(enabled && fm_hot(px+4,iy,FM_CTX_W-8,FM_CTX_ITEM_H-2)){
            if(modern) g_cv->RoundRect(px+4,iy,FM_CTX_W-8,FM_CTX_ITEM_H-2,5,P.hover,P.hover,255);
            else g_cv->FillRect(px+4,iy,FM_CTX_W-8,FM_CTX_ITEM_H-2,FM_HOVER);
        }
        fm_ds(px+14,iy+4,FM_CTX_ITEMS[i],enabled?FM_TEXT:fm_tok().off);
    }
}

void FileManagerApp::RenderSortMenu(int ox,int oy){
    const int ITEM_H=22, MENU_W=140;
    const char* items[] = {"Name","Size","Type","Date","---","Ascending","Descending"};
    const int N=7;
    int px = ox + 288;
    int py = oy + TAB_BAR_H + TOOLBAR_H + 4;
    g_cv->FillRect(px,py,MENU_W,N*ITEM_H+4,fm_tok().surface);
    g_cv->DrawRect(px,py,MENU_W,N*ITEM_H+4,FM_BORDER);
    FMTab* t = Active(cur->focused_pane);
    for(int i=0;i<N;i++){
        int iy = py + 2 + i*ITEM_H;
        if(items[i][0]=='-'){
            g_cv->FillRect(px+6,iy+10,MENU_W-12,1,FM_BORDER);
        } else {
            bool checked=false;
            if(i<4) checked = (t->sort_mode==i);
            else if(i==5) checked = !t->sort_desc;
            else if(i==6) checked = t->sort_desc;
            if(checked) fm_ds(px+8,iy+4,"*",FM_ACCENT);
            fm_ds(px+22,iy+4,items[i],FM_TEXT);
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
    bool credited = fm_path_has_ssstik(cur->properties_path);
    int dw=320, dh = credited ? 268 : 220;
    int dx = cx + (cw-dw)/2, dy = cy + (ch-dh)/2;
    g_cv->FillRectAlpha(cx,cy,cw,ch,180,0xFF000000);
    g_cv->FillRoundedRect(dx,dy,dw,dh,8,fm_tok().surface);
    g_cv->DrawRect(dx,dy,dw,dh,FM_BORDER);
    fm_ds(dx+12,dy+10,"Properties",FM_TEXT);
    g_cv->DrawLine(dx,dy+32,dx+dw,dy+32,FM_BORDER);
    FmNode nv; FmNode* n = fm_stat(cur->properties_path,&nv) ? &nv : nullptr;
    if(!n){
        fm_ds(dx+12,dy+44,"(missing)",FM_RED);
    } else {
        char line[160]; line[0]=0;
        sapp(line,"Name: ",160); sapp(line,n->name,160);
        fm_ds(dx+12,dy+44,line,FM_TEXT);
        line[0]=0; sapp(line,"Path: ",160); sapp(line,cur->properties_path,160);
        fm_ds(dx+12,dy+62,line,FM_TEXT);
        line[0]=0; sapp(line,"Type: ",160);
        sapp(line, n->dir?"Directory":(n->link?"Symlink":"File"),160);
        fm_ds(dx+12,dy+80,line,FM_TEXT);
        line[0]=0; sapp(line,"Size: ",160);
        char szb[16]; size_to_human(n->size,szb,16); sapp(line,szb,160);
        fm_ds(dx+12,dy+98,line,FM_TEXT);
        line[0]=0; sapp(line,"Perms: ",160);
        char p[5]={0};
        p[0]=(n->mode&0400)?'r':'-';
        p[1]=(n->mode&0200)?'w':'-';
        p[2]=(n->mode&0100)?'x':'-';
        p[3]=0;
        sapp(line,p,160);
        fm_ds(dx+12,dy+116,line,FM_TEXT);
        line[0]=0; sapp(line,"UID: ",160);
        char ub[8]; int_to_str(n->uid,ub,8); sapp(line,ub,160);
        sapp(line,"  GID: ",160);
        int_to_str(n->gid,ub,8); sapp(line,ub,160);
        fm_ds(dx+12,dy+134,line,FM_TEXT);
        line[0]=0; sapp(line,"Modified (s since boot): ",160);
        char tb[16]; int_to_str((int)n->modified,tb,16); sapp(line,tb,160);
        fm_ds(dx+12,dy+152,line,FM_TEXT);
        if(credited){
            // creator attribution for the imported tiktok video. (satoru)
            g_cv->DrawLine(dx+12,dy+166,dx+dw-12,dy+166,FM_BORDER);
            fm_ds(dx+12,dy+172,"Credit: neitux.vfx",FM_WHITE);
            fm_ds(dx+12,dy+190,"Source: tiktok.com/@neitux.vfx",FM_TEXT);
            fm_ds(dx+12,dy+208,"video/7393775041478954246",FM_TEXT);
        }
    }
    // close button
    g_cv->FillRoundedRect(dx+dw-72,dy+dh-30,60,22,4,FM_BTN);
    fm_ds(dx+dw-60,dy+dh-26,"Close",FM_WHITE);
}

// small overlay listing the built-in apps a file can be handed to. (satoru)
static const char* const FM_OPEN_WITH[] = { "Text Editor", "Media Player", "Video Player", "Terminal", "Cancel" };
static const int FM_OPEN_WITH_N = 5;

void FileManagerApp::RenderOpenWith(int cx,int cy,int cw,int ch){
    const int dw=270, item_h=34, dh=44+FM_OPEN_WITH_N*item_h;
    int dx=cx+(cw-dw)/2, dy=cy+(ch-dh)/2;
    g_cv->FillRectAlpha(cx,cy,cw,ch,150,0xFF000000);
    g_cv->FillRoundedRect(dx,dy,dw,dh,8,fm_tok().surface);
    g_cv->DrawRect(dx,dy,dw,dh,FM_BORDER);
    fm_ds(dx+14,dy+12,"Open With",FM_TEXT);
    char nm[FM_NAME_MAX]; scpy(nm,fm_basename(cur->open_with_path),FM_NAME_MAX);
    fm_ds(dx+dw-14-fm_mw(FM_BODY_PX,nm),dy+12,nm,FM_DIM);
    g_cv->DrawLine(dx,dy+38,dx+dw,dy+38,FM_BORDER);
    for(int i=0;i<FM_OPEN_WITH_N;i++){
        int iy=dy+44+i*item_h;
        bool hot=fm_hot(dx+8,iy,dw-16,item_h-4);
        if(i==FM_OPEN_WITH_N-1) g_cv->DrawLine(dx+12,iy-2,dx+dw-12,iy-2,FM_BORDER);
        if(hot) g_cv->FillRoundedRect(dx+8,iy,dw-16,item_h-6,5,FM_HOVER);
        fm_ds(dx+20,iy+8,FM_OPEN_WITH[i],i==FM_OPEN_WITH_N-1?FM_DIM:FM_TEXT);
    }
}

void FileManagerApp::RenderProgress(int cx,int cy,int cw,int ch){
    if(!g_jobs.active || g_jobs.owner!=cur) return;
    const int dw=330, dh=64;
    int dx=cx+cw-dw-12, dy=cy+ch-STATUS_H-dh-10;
    FmPal P=fm_pal();
    if(fm_modern()) g_cv->RoundRect(dx,dy,dw,dh,10,P.bar,P.bar,245);
    else g_cv->FillRect(dx,dy,dw,dh,fm_tok().surface);
    g_cv->DrawRect(dx,dy,dw,dh,FM_BORDER);
    char head[64]; head[0]=0;
    sapp(head,g_jobs.moving?"Moving ":"Copying ",64);
    char nb[16]; int_to_str(g_jobs.files_done,nb,16); sapp(head,nb,64);
    sapp(head," of ",64); int_to_str(g_jobs.files_total,nb,16); sapp(head,nb,64);
    fm_ds(dx+12,dy+8,head,FM_TEXT);
    fm_ds(dx+12,dy+26,g_jobs.label[0]?g_jobs.label:"...",FM_DIM);
    int pct=0;
    if(g_jobs.bytes_total>0) pct=(int)((g_jobs.bytes_done*100)/g_jobs.bytes_total);
    else if(g_jobs.files_total>0) pct=(g_jobs.files_done*100)/g_jobs.files_total;
    if(pct<0) pct=0;
    if(pct>100) pct=100;
    int bx=dx+12, by=dy+44, bw=dw-24, bh=8;
    g_cv->FillRoundedRect(bx,by,bw,bh,4,FM_BORDER);
    if(pct>0) g_cv->FillRoundedRect(bx,by,(bw*pct)/100,bh,4,FM_ACCENT);
    char pb[8]; int_to_str(pct,pb,8); sapp(pb,"%",8);
    fm_ds(dx+dw-52,dy+8,pb,FM_DIM);
    // cancel button
    if(fm_hot(dx+dw-62,dy+24,50,20)){
        g_cv->FillRoundedRect(dx+dw-62,dy+24,50,20,4,FM_BTN_HVR);
        fm_ds(dx+dw-52,dy+27,"Cancel",FM_TEXT);
    } else fm_ds(dx+dw-52,dy+27,"Cancel",FM_DIM);
}

void FileManagerApp::Render(kui::Window& win, kui::Canvas& c){
    g_cv = &c;
    g_thumb_budget = 2;   // at most two decodes per frame (satoru)
    g_thumb_pending = false;
    const int cx=0, cy=0, cw=c.Width(), ch=c.Height();
    if(fm_modern()){
        // the window body (and so the unified titlebar) takes the app surface (satoru)
        FmPal P = fm_pal();
        if(cur->bg_set != P.bar){ cur->bg_set = P.bar; win.SetBackground(P.bar); }   // only on a theme change (satoru)
        g_cv->FillRect(cx,cy,cw,ch,P.bg);
    } else
    g_cv->FillRect(cx,cy,cw,ch,FM_BG);
    RenderTabBar(cx,cy,cw);
    RenderToolbar(cx,cy+TAB_BAR_H,cw);
    RenderSidebar(cx,cy+TAB_BAR_H+TOOLBAR_H, SIDEBAR_W, ch-TAB_BAR_H-TOOLBAR_H-STATUS_H);
    int main_x = cx+SIDEBAR_W;
    int main_y = cy+TAB_BAR_H+TOOLBAR_H;
    int main_w = cw-SIDEBAR_W;
    int main_h = ch-TAB_BAR_H-TOOLBAR_H-STATUS_H;
    if(cur->dual_pane){
        int half = main_w/2;
        RenderPane(0, main_x, main_y, half, main_h);
        // split between panes is structural chrome -> hairline border. (satoru)
        g_cv->DrawLine(main_x+half, main_y, main_x+half, main_y+main_h, FM_BORDER);
        RenderPane(1, main_x+half, main_y, main_w-half, main_h);
    } else {
        RenderPane(0, main_x, main_y, main_w, main_h);
        // clear right pane bounds so click handling ignores
        cur->s_last_right_pane_w = 0;
    }
    RenderStatusBar(cx, cy+ch-STATUS_H, cw);
    if(cur->sort_menu_open) RenderSortMenu(cx,cy);
    if(cur->context_menu_open) RenderContextMenu(cx,cy);
    if(cur->properties_open) RenderProperties(cx,cy,cw,ch);
    if(cur->open_with_open) RenderOpenWith(cx,cy,cw,ch);
    if(cur->dragging){
        FMTab* dt=Active(cur->drag_pane);
        FMEntry* de=EntriesOf(cur->drag_pane);
        int cnt=0; char nm[FM_NAME_MAX]; nm[0]=0;
        if(cur->drag_idx>=0 && cur->drag_idx<FM_MAX_ENTRIES && dt->sel[cur->drag_idx] && dt->sel_count>0){
            cnt=dt->sel_count;
            for(int i=0;i<FM_MAX_ENTRIES;i++) if(dt->sel[i] && i<EntryCount(cur->drag_pane)){ scpy(nm,de[i].name,FM_NAME_MAX); break; }
        } else if(cur->drag_idx>=0 && cur->drag_idx<EntryCount(cur->drag_pane)){ cnt=1; scpy(nm,de[cur->drag_idx].name,FM_NAME_MAX); }
        char lbl[96]; lbl[0]=0;
        if(cnt>1){ char nb[8]; int_to_str(cnt,nb,8); scpy(lbl,nb,96); sapp(lbl," items",96); }
        else scpy(lbl,nm,96);
        int tw=fm_mw(FM_BODY_PX,lbl);
        int gx=g_mx+12, gy=g_my+10;
        if(gx+tw+22>cw) gx=cw-tw-26;
        if(gy+26>ch) gy=ch-30;
        g_cv->FillRoundedRect(gx,gy,tw+22,24,6,fm_tok().surface);
        g_cv->DrawRect(gx,gy,tw+22,24,cur->drag_copy?FM_ACCENT:FM_BORDER);
        fm_ds(gx+8,gy+4,lbl,FM_TEXT);
    }
    RenderProgress(cx,cy,cw,ch);
    if(g_thumb_pending) win.RequestAnimationFrame();   // finish the visible thumbnails (satoru)
}

// ── input ─────────────────────────────────────────────────────────────
bool FileManagerApp::Input(kui::Window& w,int mx,int my,bool clicked,int key){
    const kui::Size cs = w.ContentSize();
    int cw_local = cs.w;
    int ch_local = cs.h;

    // progress overlay cancel button is the topmost control while a job runs (satoru)
    if(g_jobs.active && g_jobs.owner==cur){
        if(clicked){
            int dw=330, dh=64;
            int dx=cw_local-dw-12, dy=ch_local-STATUS_H-dh-10;
            if(mx>=dx+dw-62 && mx<dx+dw-12 && my>=dy+24 && my<dy+44){ CancelJobs(); return true; }
        }
        if(key==27){ CancelJobs(); return true; }
    }

    // open with chooser
    if(cur->open_with_open){
        if(clicked){
            const int dw=270, item_h=34, dh=44+FM_OPEN_WITH_N*item_h;
            int dx=(cw_local-dw)/2, dy=(ch_local-dh)/2;
            if(mx>=dx && mx<dx+dw && my>=dy+44 && my<dy+dh-20){
                int it=(my-dy-44)/item_h;
                if(it==FM_OPEN_WITH_N-1){ cur->open_with_open=false; return true; }
                const char* path=cur->open_with_path;
                bool is_dir=kui::Fs::IsDir(path);
                if(!is_dir){
                    if(it==0) fm_open_with(FO_TEXT,path);
                    else if(it==1) fm_open_with(FO_PLAYER,path);
                    else if(it==2) fm_open_with(FO_KVID,path);
                    else {
                        char cmd[FM_MAX_PATH+8]; int ci=0;
                        const char* pfx="kcl ";
                        for(int q=0;pfx[q]&&ci<(int)sizeof(cmd)-1;q++) cmd[ci++]=pfx[q];
                        for(int q=0;path[q]&&ci<(int)sizeof(cmd)-1;q++) cmd[ci++]=path[q];
                        cmd[ci]=0;
                        fm_run_kcl(cmd);
                    }
                    PushRecent(path);
                }
                cur->open_with_open=false;
                return true;
            }
            if(mx<dx||mx>dx+dw||my<dy||my>dy+dh) cur->open_with_open=false;
        }
        if(key==27) cur->open_with_open=false;
        return true;
    }

    // properties dialog
    if(cur->properties_open){
        if(clicked){
            int dw=320, dh = fm_path_has_ssstik(cur->properties_path) ? 268 : 220;
            int dx = (cw_local-dw)/2, dy = (ch_local-dh)/2;
            if(mx>=dx+dw-72 && mx<=dx+dw-12 && my>=dy+dh-30 && my<=dy+dh-8){
                cur->properties_open=false; return true;
            }
            // click outside dialog also closes
            if(mx<dx||mx>dx+dw||my<dy||my>dy+dh){
                cur->properties_open=false; return true;
            }
        }
        if(key==27){ cur->properties_open=false; return true; }
        return true;
    }

    // sort menu
    if(cur->sort_menu_open && clicked){
        const int ITEM_H=22, MENU_W=140;
        int px = 288;
        int py = TAB_BAR_H + TOOLBAR_H + 4;
        if(mx>=px && mx<px+MENU_W && my>=py && my<py+7*ITEM_H+4){
            int it = (my-py-2)/ITEM_H;
            FMTab* t = Active(cur->focused_pane);
            cur->sort_menu_open=false;
            if(it>=0 && it<4){ t->sort_mode=it; SortPane(cur->focused_pane); }
            else if(it==5){ t->sort_desc=false; SortPane(cur->focused_pane); }
            else if(it==6){ t->sort_desc=true;  SortPane(cur->focused_pane); }
            return true;
        }
        cur->sort_menu_open=false;
        return true;
    }

    // context menu
    if(cur->context_menu_open && clicked){
        const int N=FM_CTX_N;
        int px = cur->context_menu_x, py = cur->context_menu_y;
        int mh=N*FM_CTX_ITEM_H+FM_CTX_MARGIN*2;
        g_mx=0; // no hover while a menu owns the click (satoru)
        if(mx>=px && mx<px+FM_CTX_W && my>=py && my<py+mh){
            int it = (my-py-FM_CTX_MARGIN)/FM_CTX_ITEM_H;
            cur->context_menu_open=false;
            int pane=cur->context_menu_pane;
            int idx=cur->context_menu_idx;
            FMTab* t=Active(pane);
            bool has_item=idx>=0;
            bool in_trash=fm_in_trash(t->path);
            char fp[FM_MAX_PATH];
            if(has_item) JoinPath(fp,FM_MAX_PATH,t->path,EntriesOf(pane)[idx].name);
            switch(it){
                case 0: if(has_item) OpenEntryPane(pane,idx); break;
                case 1: if(has_item && !EntriesOf(pane)[idx].is_dir){
                            scpy(cur->open_with_path,fp,FM_MAX_PATH);
                            cur->open_with_open=true;
                        } break;
                case 3: if(has_item) CopyEntryPane(pane,idx,false); break;
                case 4: if(has_item) CopyEntryPane(pane,idx,true);  break;
                case 5: PastePane(pane); break;
                case 6: if(has_item){
                            cur->rename_mode=true; cur->rename_pane=pane;
                            cur->rename_target=idx;
                            FMEntry* e=&EntriesOf(pane)[idx];
                            scpy(cur->rename_buf,e->name,FM_NAME_MAX);
                            cur->rename_cursor=slen(cur->rename_buf);
                            cur->rename_fresh=true;
                        } break;
                case 7: if(has_item) DeleteEntryPane(pane,idx,false); break;
                case 8: if(has_item) DeleteEntryPane(pane,idx,true);  break;
                case 10: if(has_item){
                            scpy(cur->properties_path,fp,FM_MAX_PATH);
                            cur->properties_open=true;
                        } break;
                case 11: CreateFolderPane(pane); break;
                case 12: CreateFilePane(pane); break;
                case 14: SelAll(pane); break;
                case 15: if(has_item && in_trash) StartRestore(fp); break;
                case 16: if(in_trash){ EmptyTrash(); RefreshBoth(); } break;
                case 17: RefreshBoth(); break;
            }
            return true;
        }
        cur->context_menu_open=false;
        return true;
    }

    if(key==27 && cur->context_menu_open){ cur->context_menu_open=false; return true; }
    if(key==27 && cur->sort_menu_open){ cur->sort_menu_open=false; return true; }

    // address bar editing
    {
        FMTab* t = Active(cur->focused_pane);
        if(t->address_edit){
            if(key=='\n' || key=='\r'){
                NavigateToPane(cur->focused_pane, t->address_buf, true);
                t->address_edit=false; return true;
            }
            if(key==27){ t->address_edit=false; return true; }
            if(key==8){
                t->address_fresh=false;
                if(t->address_cursor>0){ t->address_buf[--t->address_cursor]=0; }
                return true;
            }
            if(key>=32 && key<127 && t->address_cursor<FM_MAX_PATH-1){
                if(t->address_fresh){ t->address_fresh=false; t->address_cursor=0; t->address_buf[0]=0; }
                t->address_buf[t->address_cursor++]=key;
                t->address_buf[t->address_cursor]=0;
                return true;
            }
            // allow click outside address bar to commit-cancel
            if(clicked){ t->address_edit=false; }
        }
    }

    // rename inline
    if(cur->rename_mode){
        if(key=='\n'||key=='\r'){
            // reject invalid names (empty, contains '/' or '..')
            if(cur->rename_buf[0]==0){ cur->rename_mode=false; return true; }
            for(int ri=0;cur->rename_buf[ri];ri++){
                if(cur->rename_buf[ri]=='/'){ cur->rename_mode=false; return true; }
            }
            if(cur->rename_buf[0]=='.' && cur->rename_buf[1]=='.' && (cur->rename_buf[2]==0 || cur->rename_buf[2]=='/'))
                { cur->rename_mode=false; return true; }
            if(cur->rename_target<0 || cur->rename_target>=EntryCount(cur->rename_pane)){
                cur->rename_mode=false; return true;
            }
            FMTab* t = Active(cur->rename_pane);
            FMEntry* e = &EntriesOf(cur->rename_pane)[cur->rename_target];
            char old_p[FM_MAX_PATH], new_p[FM_MAX_PATH];
            JoinPath(old_p,FM_MAX_PATH,t->path,e->name);
            JoinPath(new_p,FM_MAX_PATH,t->path,cur->rename_buf);
            fm_rename(old_p,new_p,cur->rename_buf);
            cur->rename_mode=false;
            RefreshBoth();
            notify_desktop_path_changed(old_p);
            notify_desktop_path_changed(new_p);
            return true;
        }
        if(key==27){ cur->rename_mode=false; return true; }
        if(key==8){ if(cur->rename_cursor>0){ cur->rename_buf[--cur->rename_cursor]=0; } return true; }
        if(key>=32 && key<127 && cur->rename_cursor<FM_NAME_MAX-1){
            cur->rename_buf[cur->rename_cursor++]=key;
            cur->rename_buf[cur->rename_cursor]=0;
            return true;
        }
        return true;
    }

    // search field editing: live filter of the active pane (satoru)
    {
        FMTab* t = Active(cur->focused_pane);
        if(t->search_edit){
            if(key=='\n'||key=='\r'){ t->search_edit=false; return true; }
            if(key==27){ t->search_edit=false; t->search[0]=0; t->search_cursor=0; RefreshPane(cur->focused_pane); return true; }
            if(key==8){
                if(t->search_cursor>0){ t->search[--t->search_cursor]=0; RefreshPane(cur->focused_pane); }
                return true;
            }
            if(key==kui::K_DEL && t->search[0]==0) return true;
            if(key>=32 && key<127 && t->search_cursor<FM_SEARCH_MAX-1){
                t->search[t->search_cursor++]=key;
                t->search[t->search_cursor]=0;
                RefreshPane(cur->focused_pane);
                return true;
            }
            if(clicked) t->search_edit=false;   // click elsewhere leaves the field
            else return true;
        }
    }

    // mouse clicks
    if(clicked){
        // tab bar
        if(my>=0 && my<TAB_BAR_H){
            // tab strip layout match RenderTabBar
            int avail = cw_local - 8 - 28;
            int per = avail / (cur->tab_count>0?cur->tab_count:1);
            if(per>160) per=160;
            if(per<60) per=60;
            int tx=4;
            for(int i=0;i<cur->tab_count;i++){
                if(mx>=tx && mx<tx+per){
                    if(mx>=tx+per-18 && mx<tx+per){
                        // close
                        CloseTab(i);
                    } else {
                        if(cur->focused_pane==1) cur->active_tab_right=i;
                        else                cur->active_tab=i;
                        RefreshPane(cur->focused_pane);
                    }
                    return true;
                }
                tx += per;
            }
            // "+" button
            if(mx>=tx+4 && mx<tx+26){
                NewTab(Active(cur->focused_pane)->path);
                return true;
            }
            return true;
        }
        // toolbar
        if(my>=TAB_BAR_H && my<TAB_BAR_H+TOOLBAR_H){
            int rx = mx;
            int ry = my-TAB_BAR_H;
            (void)ry;
            if(rx>=  4 && rx< 32){ GoBackPane(cur->focused_pane); return true; }
            if(rx>= 36 && rx< 64){ GoForwardPane(cur->focused_pane); return true; }
            if(rx>= 68 && rx< 96){ GoUpPane(cur->focused_pane); return true; }
            if(rx>=100 && rx<128){ NavigateToPane(cur->focused_pane,"/home/user",true); return true; }
            if(rx>=132 && rx<160){ RefreshBoth(); return true; }
            if(rx>=168 && rx<224){ CreateFolderPane(cur->focused_pane); return true; }
            if(rx>=228 && rx<284){ CreateFilePane(cur->focused_pane); return true; }
            if(rx>=288 && rx<344){ cur->sort_menu_open=!cur->sort_menu_open; return true; }
            if(rx>=348 && rx<404){
                FMTab* t = Active(cur->focused_pane);
                t->view_mode = (FMViewMode)(((int)t->view_mode+1)%3);
                return true;
            }
            if(rx>=408 && rx<480){
                cur->dual_pane = !cur->dual_pane;
                if(cur->dual_pane && cur->tab_count<2){
                    NewTab(Active(0)->path);
                    cur->active_tab_right = cur->tab_count-1;
                }
                RefreshBoth();
                return true;
            }
            if(rx>=488 && rx<516){
                FMTab* t = Active(cur->focused_pane);
                t->show_hidden = !t->show_hidden;
                RefreshBoth();
                return true;
            }
            if(rx>=528 && rx<588){
                FMTab* t = Active(cur->focused_pane);
                t->search_edit=true; t->address_edit=false;
                t->search_cursor=slen(t->search);
                return true;
            }
            return true;
        }
        // sidebar
        if(mx<SIDEBAR_W && my>=TAB_BAR_H+TOOLBAR_H){
            int by = TAB_BAR_H+TOOLBAR_H + 24;
            for(int i=0;i<BOOKMARK_COUNT;i++){
                if(my>=by && my<by+22){
                    NavigateToPane(cur->focused_pane, BOOKMARKS[i].path, true);
                    return true;
                }
                by += 22;
            }
            FmDrive* dv=nullptr; int dn=fm_sidebar_drives(cur,&dv);
            by += 8+6+18;
            for(int i=0;i<dn;i++){
                if(my>=by && my<by+22){
                    NavigateToPane(cur->focused_pane, dv[i].path, true);
                    return true;
                }
                by += 22;
            }
            // recent click
            by += 8+6+18;
            for(int i=0;i<recent_count && i<6;i++){
                if(my>=by && my<by+20){
                    if(kui::Fs::Exists(recent[i]) && !kui::Fs::IsDir(recent[i])){
                        if(is_media_file(recent[i])||is_image_file(recent[i])||is_video_file(recent[i])) fm_open_with(is_video_file(recent[i])?FO_KVID:FO_PLAYER,recent[i]);
                        else if(is_kcl_file(recent[i])) { char cmd[FM_MAX_PATH+8]; int ci=0; const char* pfx="kcl "; for(int q=0;pfx[q]&&ci<(int)sizeof(cmd)-1;q++) cmd[ci++]=pfx[q]; for(int q=0;recent[i][q]&&ci<(int)sizeof(cmd)-1;q++) cmd[ci++]=recent[i][q]; cmd[ci]=0; fm_run_kcl(cmd); }
                        else fm_open_with(FO_TEXT,recent[i]);
                    }
                    return true;
                }
                by += 20;
            }
            return true;
        }
        // path bar click → enter address-edit mode for whichever pane
        int main_x = SIDEBAR_W;
        int main_w = cw_local - SIDEBAR_W;
        int main_y = TAB_BAR_H+TOOLBAR_H;
        if(my>=main_y && my<main_y+PATH_BAR_H+2){
            int half = cur->dual_pane ? main_w/2 : main_w;
            int pane = (cur->dual_pane && mx>=main_x+half) ? 1 : 0;
            cur->focused_pane = pane;
            FMTab* t = Active(pane);
            int pane_x_off = (pane==1) ? main_x+half : main_x;
            int pane_w     = (pane==1) ? main_w-half : (cur->dual_pane?half:main_w);
            int sb_left = pane_x_off + pane_w - 178 - 6;
            // if click in search box area (last 140px), focus search instead
            if((t->search_edit || t->search[0]) && mx>=sb_left){
                t->search_edit=true; t->search_cursor=slen(t->search);
                t->address_edit=false;
                return true;
            }
            char nav[FM_MAX_PATH]; nav[0]=0;
            int hit=fm_crumb_hit(t,pane_x_off,pane_w,mx,fm_modern(),nav,FM_MAX_PATH);
            if(hit>=0 && nav[0]){ NavigateToPane(pane,nav,true); return true; }
            t->address_edit=true; t->search_edit=false; t->address_fresh=true;
            scpy(t->address_buf, t->path, FM_MAX_PATH);
            t->address_cursor = slen(t->address_buf);
            return true;
        }
        // pane content area
        int main_h = ch_local - TAB_BAR_H - TOOLBAR_H - STATUS_H;
        if(my>=main_y && my<main_y+main_h){
            int half = cur->dual_pane ? main_w/2 : main_w;
            int pane = (cur->dual_pane && mx>=main_x+half) ? 1 : 0;
            cur->focused_pane = pane;
            FMTab* t = Active(pane);
            int pane_x_off = (pane==1) ? main_x+half : main_x;
            int pane_w     = (pane==1) ? main_w-half : (cur->dual_pane?half:main_w);
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
            if(key==FM_RCLICK){
                // right-click
                cur->context_menu_open=true;
                fm_menu_place(cur,mx,my,cw_local,ch_local);
                cur->context_menu_pane = pane;
                cur->context_menu_idx = row_idx;
                if(row_idx>=0 && !HasSel(pane,row_idx)) SelSetOne(pane,row_idx);
                return true;
            }
            uint8_t mods = kui::ModsHeld();
            if(row_idx>=0){
                uint32_t now = kui::NowMs();
                if((mods & kui::M_CTRL) && (mods & kui::M_SHIFT)){
                    SelRange(pane,t->sel_anchor>=0?t->sel_anchor:row_idx,row_idx);
                    return true;
                }
                if(mods & kui::M_CTRL){ SelToggle(pane,row_idx); return true; }
                if(mods & kui::M_SHIFT){
                    SelRange(pane,t->sel_anchor>=0?t->sel_anchor:row_idx,row_idx);
                    return true;
                }
                if(row_idx==cur->last_click_idx[pane] && (now-cur->last_click_time[pane])<DOUBLE_CLICK_MS){
                    OpenEntryPane(pane,row_idx);
                    cur->last_click_idx[pane]=-1; cur->last_click_time[pane]=0;
                    cur->drag_armed=false;
                } else {
                    SelSetOne(pane,row_idx);
                    cur->last_click_idx[pane] = row_idx;
                    cur->last_click_time[pane] = now;
                    cur->drag_armed=true; cur->drag_pane=pane; cur->drag_idx=row_idx;
                    cur->drag_x0=mx; cur->drag_y0=my; cur->dragging=false;
                    cur->drag_copy=(mods & kui::M_CTRL)!=0;
                }
            } else {
                if(!(mods & kui::M_CTRL)){ SelClear(pane); t->selected=-1; }
            }
            return true;
        }
    }

    // raw right-click event without earlier ctxmenu state
    if(key==FM_RCLICK && !clicked){
        int main_x = SIDEBAR_W;
        int main_w = cw_local - SIDEBAR_W;
        int main_y = TAB_BAR_H+TOOLBAR_H;
        int half = cur->dual_pane ? main_w/2 : main_w;
        int pane = (cur->dual_pane && mx>=main_x+half) ? 1 : 0;
        cur->focused_pane = pane;
        FMTab* t = Active(pane); (void)t;
        cur->context_menu_open=true;
        fm_menu_place(cur,mx,my,cw_local,ch_local);
        cur->context_menu_pane = pane;
        cur->context_menu_idx = -1;
        // hit-test entry like above
        int pane_x_off = (pane==1) ? main_x+half : main_x;
        int pane_w     = (pane==1) ? main_w-half : (cur->dual_pane?half:main_w);
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
                    if(idx>=0 && idx<EntryCount(pane)){ cur->context_menu_idx=idx; tt->selected=idx; }
                }
            } else {
                int row = rel_y/ROW_H + tt->scroll;
                if(row>=0 && row<EntryCount(pane)){ cur->context_menu_idx=row; tt->selected=row; }
            }
        }
        return true;
    }

    // keyboard shortcuts
    if(key){
        const bool k_ctrl  = (kui::ModsHeld() & kui::M_CTRL)  != 0;
        const bool k_shift = (kui::ModsHeld() & kui::M_SHIFT) != 0;
        FMTab* t = Active(cur->focused_pane);
        int pane = cur->focused_pane;
        // letter keys w/ ctrl
        if(k_ctrl){
            if(key=='c'||key=='C'){ CopySelection(pane,t->selected,false); return true; }
            if(key=='x'||key=='X'){ CopySelection(pane,t->selected,true);  return true; }
            if(key=='v'||key=='V'){ PastePane(pane); return true; }
            if(key=='a'||key=='A'){ SelAll(pane); return true; }
            if(key=='h'||key=='H'){ t->show_hidden=!t->show_hidden; RefreshBoth(); return true; }
            if(key=='t'||key=='T'){ NewTab(t->path); return true; }
            if(key=='w'||key=='W'){ CloseTab(pane==1?cur->active_tab_right:cur->active_tab); return true; }
            if(key=='l'||key=='L'){ t->address_edit=true; t->search_edit=false; t->address_fresh=true; scpy(t->address_buf,t->path,FM_MAX_PATH); t->address_cursor=slen(t->address_buf); return true; }
            if(key=='f'||key=='F'){ t->search_edit=!t->search_edit; t->address_edit=false; t->search_cursor=slen(t->search); return true; }
            if(key=='i'||key=='I'){ if(t->selected>=0){ char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,EntriesOf(pane)[t->selected].name); scpy(cur->properties_path,fp,FM_MAX_PATH); cur->properties_open=true; } return true; }
            if(key=='d'||key=='D'){ cur->dual_pane=!cur->dual_pane; if(cur->dual_pane && cur->tab_count<2){ NewTab(t->path); cur->active_tab_right=cur->tab_count-1; } RefreshBoth(); return true; }
            return true;
        }
        // F-key shortcuts
        if(key==kui::K_F1+1 && t->selected>=0){
            cur->rename_mode=true; cur->rename_pane=pane; cur->rename_target=t->selected;
            FMEntry* e=&EntriesOf(pane)[t->selected];
            scpy(cur->rename_buf,e->name,FM_NAME_MAX);
            cur->rename_cursor=slen(cur->rename_buf);
            cur->rename_fresh=true;
            return true;
        }
        if(key==kui::K_F1+2){ t->view_mode=(FMViewMode)(((int)t->view_mode+1)%3); return true; }   // f3 cycles list/grid (satoru)
        if(key==kui::K_F1+4){ RefreshBoth(); return true; }
        if(key==kui::K_F1+7 && t->selected>=0 && fm_in_trash(t->path)){   // f8 restore (satoru)
            char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,EntriesOf(pane)[t->selected].name);
            StartRestore(fp);
            return true;
        }
        if(key==kui::K_F1+8 && t->selected>=0){   // f9 open with (satoru)
            char fp[FM_MAX_PATH]; JoinPath(fp,FM_MAX_PATH,t->path,EntriesOf(pane)[t->selected].name);
            if(!EntriesOf(pane)[t->selected].is_dir){ scpy(cur->open_with_path,fp,FM_MAX_PATH); cur->open_with_open=true; }
            return true;
        }
        if(key==kui::K_F1+9){   // f10 opens the context menu on the cursor row (satoru)
            if(t->selected>=0){
                cur->context_menu_open=true;
                cur->context_menu_pane=pane;
                cur->context_menu_idx=t->selected;
                int my2=cur->s_last_left_pane_y+(t->selected-t->scroll)*ROW_H;
                if(my2<80) my2=80;
                fm_menu_place(cur,160,my2,cw_local,ch_local);
            }
            return true;
        }
        if(key==kui::K_DEL){
            if(t->sel_count>0) DeleteEntryPane(pane,t->selected>=0?t->selected:0,k_shift);
            else if(t->selected>=0) DeleteEntryPane(pane,t->selected,k_shift);
            return true;
        }
        if(key=='\n'||key=='\r'){ if(t->selected>=0) OpenEntryPane(pane,t->selected); return true; }
        if(key==8){ GoBackPane(pane); return true; }     // backspace = back
        if(key=='/'){ // start search
            t->search[0]=0; t->search_cursor=0; t->search_edit=true; t->address_edit=false;
            RefreshPane(pane);
            return true;
        }
        if(key==27){ // escape clears search
            if(t->search_edit || t->search[0]){ t->search_edit=false; t->search[0]=0; t->search_cursor=0; RefreshPane(pane); return true; }
            SelClear(pane); t->selected=-1; return true;
        }
        if(key==9){ // Tab swaps focused pane
            if(cur->dual_pane){ cur->focused_pane = cur->focused_pane?0:1; return true; }
        }
        // arrow keys (raw scancodes)
        if(key==kui::K_UP || key==kui::K_DOWN){
            int n=EntryCount(pane);
            if(n<=0) return true;
            int next=t->selected;
            if(key==kui::K_UP){ if(next<0) next=n-1; else if(next>0) next--; }
            else { if(next<0) next=0; else if(next<n-1) next++; }
            if(k_shift){
                if(t->sel_anchor<0) t->sel_anchor=(t->selected>=0?t->selected:next);
                SelRange(pane,t->sel_anchor,next);
            } else {
                SelSetOne(pane,next);
            }
            if(t->selected<t->scroll) t->scroll=t->selected<0?0:t->selected;
            // keep visible: nudge scroll if selection runs past the viewport
            int rows = cur->s_last_left_pane_h>0 ? cur->s_last_left_pane_h/ROW_H : 12;
            if(t->selected - t->scroll >= rows) t->scroll = t->selected - rows + 1;
            return true;
        }
        if(key==kui::K_LEFT){ if(pane==1 && cur->dual_pane) cur->focused_pane=0; return true; }
        if(key==kui::K_RIGHT){ if(pane==0 && cur->dual_pane) cur->focused_pane=1; return true; }
        if(key==kui::K_HOME && EntryCount(pane)>0){ SelSetOne(pane,0); t->scroll=0; return true; }
        if(key==kui::K_END && EntryCount(pane)>0){ SelSetOne(pane,EntryCount(pane)-1); return true; }
    }
    return false;
}

// ── toolkit glue ──────────────────────────────────────────────────────
void FMApp::OnFrame(kui::Window& w, kui::Canvas& c, uint32_t){
    FMInst* saved = FileManagerApp::cur;
    FileManagerApp::cur = inst;
    FileManagerApp::Render(w, c);
    g_cv = nullptr;
    FileManagerApp::cur = saved;
}

bool FMApp::OnEvent(kui::Window& w, const kui::Event& e){
    using kui::EventType;
    FMInst* saved = FileManagerApp::cur;
    FileManagerApp::cur = inst;
    bool redraw = false;
    switch(e.type){
    case EventType::PointerMove: {
        g_mx = e.x; g_my = e.y;
        if(inst->drag_armed && !inst->dragging){
            int dx=e.x-inst->drag_x0, dy=e.y-inst->drag_y0;
            if(dx<0) dx=-dx;
            if(dy<0) dy=-dy;
            if(dx>6 || dy>6){ inst->dragging=true; inst->drag_dest[0]=0; }
        }
        if(inst->dragging){
            inst->drag_x=e.x; inst->drag_y=e.y;
            bool on=false; char dest[FM_MAX_PATH];
            fm_drag_target(inst,e.x,e.y,dest,&on);
            scpy(inst->drag_dest,dest,FM_MAX_PATH);
            inst->drag_on_item=on;
            if(e.mods & kui::M_CTRL) inst->drag_copy=true;
        }
        redraw = true; break; }
    case EventType::PointerUp: {
        redraw = true;
        if(inst->dragging){
            bool on=false; char dest[FM_MAX_PATH];
            fm_drag_target(inst,e.x,e.y,dest,&on);
            if(dest[0]) fm_drag_drop(inst,dest,inst->drag_copy);
            inst->dragging=false; inst->drag_armed=false;
        }
        break; }
    case EventType::PointerLeave: g_mx = -1;  g_my = -1;  redraw = true; break;
    case EventType::Click:        // left press, the old wm event 1 (satoru)
        g_mx = e.x; g_my = e.y;
        FileManagerApp::Input(w, e.x, e.y, true, 0); redraw = true; break;
    case EventType::PointerDown:  // right press, the old wm event 4 (satoru)
        if(e.button == kui::Button::Right){ inst->drag_armed=false; inst->dragging=false; FileManagerApp::Input(w, e.x, e.y, false, FM_RCLICK); redraw = true; }
        break;
    case EventType::Scroll:       // the old event 3 passed notches, positive = wheel up, the same sign kui uses (satoru)
        FileManagerApp::Scroll(e.scroll_steps); redraw = true; break;
    case EventType::KeyDown: {
        int k = e.key;
        if(k==kui::K_SHIFT || k==kui::K_CTRL || k==kui::K_ALT || k==kui::K_SUPER) break;   // modifiers alone are not keys (satoru)
        if(k==kui::K_ENTER) k = '\n';
        else if((e.mods & kui::M_CTRL) && k>=1 && k<=26) k = 'a' + k - 1;   // ctrl-c may arrive as 3 (satoru)
        FileManagerApp::Input(w, 0, 0, false, k);
        redraw = true;
        break; }
    case EventType::Resize:
    case EventType::ThemeChanged:
    case EventType::Focus:
    case EventType::Blur:         redraw = true; break;
    default: break;
    }
    FileManagerApp::cur = saved;
    return redraw;
}

void FMApp::OnTick(kui::Window& w, uint32_t){
    if(!g_jobs.active || g_jobs.owner!=inst) return;
    FMInst* saved = FileManagerApp::cur;
    FileManagerApp::cur = inst;
    FileManagerApp::PumpJobs(&w);
    FileManagerApp::cur = saved;
}

KUI_APP(files, kFilesSpec, MakeFiles)

