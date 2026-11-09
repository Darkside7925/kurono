#pragma once
//  kurono os - file manager (multi-tab, dual-pane, bookmarks, trash, search)

#define FM_MAX_ENTRIES   256
#define FM_MAX_PATH      256
#define FM_NAME_MAX      64
#define FM_MAX_TABS      8
#define FM_MAX_HISTORY   16
#define FM_SEARCH_MAX    48
#define FM_RECENT_MAX    16
#define FM_CLIP_MAX      64     // items one cut/copy can hold (satoru)
#define FM_JOB_MAX       128    // files one paste or trash expands into (satoru)
#define FM_DRIVE_MAX     8      // mounts shown in the sidebar drives group (satoru)

struct FMEntry {
    char name[FM_NAME_MAX];
    bool is_dir;
    int  size;
    int  permissions;
    unsigned int modified;   // seconds since boot from KVFSNode
};

enum FMViewMode {
    FM_VIEW_LIST    = 0,
    FM_VIEW_GRID    = 1,
    FM_VIEW_COLUMNS = 2,
};

enum FMSortMode {
    FM_SORT_NAME = 0,
    FM_SORT_SIZE = 1,
    FM_SORT_TYPE = 2,
    FM_SORT_DATE = 3,
};

struct FMTab {
    char path[FM_MAX_PATH];
    int  selected;
    int  scroll;
    char history[FM_MAX_HISTORY][FM_MAX_PATH];
    int  history_count;     // number of valid history entries
    int  history_pos;       // index of "current" position
    int  sort_mode;         // FMSortMode
    bool sort_desc;
    bool show_hidden;
    char search[FM_SEARCH_MAX];
    int  search_cursor;
    bool search_edit;       // the search field has the keyboard (satoru)
    bool address_edit;      // true when typing a path in the address bar
    bool address_fresh;     // first typed key replaces the shown path (satoru)
    char address_buf[FM_MAX_PATH];
    int  address_cursor;
    FMViewMode view_mode; 
    unsigned char sel[FM_MAX_ENTRIES];   // multi selection, indexed like the entries (satoru)
    int  sel_count;
    int  sel_anchor;        // shift-click and shift-arrow origin (satoru)
    bool used; // wthr ths tab slot is in use
};

struct FMInst;   // one per Files window, defined in file_manager.cpp (satoru)
class FMApp;     // the toolkit handler of one window, defined in file_manager.cpp (satoru)
namespace kui { class AppHandler; class Window; class Canvas; }   // src/kui/kui.h (satoru)

class FileManagerApp {
public:
    static void Init();
    static int  Open();
    static int  OpenAt(const char* path);
    static void NotifyFilesystemChanged(const char* path);
    static int  win_id;

    // window callbacks, called by FMApp (the kui handler) with the window's state selected (satoru)
    static void Render(kui::Window& win, kui::Canvas& c);
    static bool Input(kui::Window& win, int mx, int my, bool clicked, int key);   // key: ascii or a kui::Key, -4 = right click (satoru)
    static void Scroll(int dz);   // mouse-wheel scroll of the active pane (satoru)
    static kui::AppHandler* Make(const char* nav);   // new window state in a free slot, nullptr when full (satoru)

    // trash subsystem
    static void EnsureTrashDir();
    static void MoveToTrash(const char* full_path);
    static void EmptyTrash();
    static void RestoreFromTrash(const char* trash_name);

    // recent files
    static void PushRecent(const char* full_path);

    // tabs
    static int  NewTab(const char* path);
    static void CloseTab(int idx);

private:
    // per-window state (tabs, panes, menus, rename) lives in an FMInst defined in
    // file_manager.cpp; Select() points cur at the one a callback is for. the
    // clipboard and the recent list below are shared by all windows. (satoru)
    static FMInst* cur;
    friend class FMApp;

    // clipboard
    // clipboard: one or more items, so cut/copy of a multi selection works (satoru)
    static char clipboard_paths[FM_CLIP_MAX][FM_MAX_PATH];
    static char clipboard_names[FM_CLIP_MAX][FM_NAME_MAX];
    static int  clipboard_count;
    static bool clipboard_has_item;
    static bool clipboard_cut;       // true = move on paste

    // recent
    static char recent[FM_RECENT_MAX][FM_MAX_PATH];
    static int  recent_count;

    // helpers
    static FMTab*    Active(int pane);
    static FMEntry*  EntriesOf(int pane);
    static int       EntryCount(int pane);
    static void      RefreshPane(int pane);
    static void      RefreshActive();   // refresh the focused pane
    static void      RefreshBoth();
    static void      SortPane(int pane);
    static void      JoinPath(char* dst, int max, const char* dir, const char* name);
    static void      NavigateToPane(int pane, const char* path, bool record_history);
    static void      GoUpPane(int pane);
    static void      GoBackPane(int pane);
    static void      GoForwardPane(int pane);
    static void      OpenEntryPane(int pane, int idx);
    static void      DeleteEntryPane(int pane, int idx, bool permanent);
    static void      CreateFolderPane(int pane);
    static void      CreateFilePane(int pane);
    static void      CopyEntryPane(int pane, int idx, bool cut);
    static void      CopySelection(int pane, int idx, bool cut);   // multi-item clip fill (satoru)
    static void      PastePane(int pane);
    static void      StartTrashSelection(int pane);
    static void      StartRestore(const char* path);
    static bool      JobsActive();
    static void      PumpJobs(kui::Window* w);
    static void      CancelJobs();

    // rendering
    static void RenderToolbar(int x, int y, int w);
    static void RenderTabBar(int x, int y, int w);
    static void RenderSidebar(int x, int y, int w, int h);
    static void RenderPathBar(int pane, int x, int y, int w);
    static void RenderPane(int pane, int x, int y, int w, int h);
    static void RenderFileList(int pane, int x, int y, int w, int h);
    static void RenderFileGrid(int pane, int x, int y, int w, int h);
    static void RenderFileColumns(int pane, int x, int y, int w, int h);
    static void RenderStatusBar(int x, int y, int w);
    static void RenderEntryIcon(int x, int y, int sz, FMEntry* e);
    static bool DrawEntryIcon(int pane, int idx, int x, int y, int sz);   // image thumb or glyph (satoru)
    static void RenderContextMenu(int ox, int oy);
    static void RenderSortMenu(int ox, int oy);
    static void RenderProperties(int cx, int cy, int cw, int ch);
    static void RenderOpenWith(int cx, int cy, int cw, int ch);
    static void RenderProgress(int cx, int cy, int cw, int ch);

    // multi selection (satoru)
    static bool HasSel(int pane, int idx);
    static int  SelCount(int pane);
    static void SelClear(int pane);
    static void SelAll(int pane);
    static void SelToggle(int pane, int idx);
    static void SelRange(int pane, int from, int to);
    static void SelSetOne(int pane, int idx);
    static void SelRemapAfterSort(int pane, char (*names)[FM_NAME_MAX], int n);

    // hit testing helpers
    static int HitTestEntry(int pane, int rel_x, int rel_y, int pane_w, int pane_h);
};
