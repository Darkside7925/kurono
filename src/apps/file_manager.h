#pragma once
//  kurono os  -  file manager (multi-tab, dual-pane, bookmarks, trash, search)

#define FM_MAX_ENTRIES   256
#define FM_MAX_PATH      256
#define FM_NAME_MAX      64
#define FM_MAX_TABS      8
#define FM_MAX_HISTORY   16
#define FM_SEARCH_MAX    48
#define FM_RECENT_MAX    16

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
    bool address_edit;      // true when typing a path in the address bar
    char address_buf[FM_MAX_PATH];
    int  address_cursor;
    FMViewMode view_mode; 
    bool used; // wthr ths tab slot is in use
};

class FileManagerApp {
public:
    static void Init();
    static int  Open();
    static int  OpenAt(const char* path);
    static void NotifyFilesystemChanged(const char* path);
    static int  win_id;

    // window callbacks
    static void Render(void* win, int x, int y, int w, int h);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

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
    // tabs
    static FMTab tabs[FM_MAX_TABS];
    static int   tab_count;
    static int   active_tab;        // left pane active tab
    static int   active_tab_right;  // right pane active tab (when dual-pane)
    static bool  dual_pane;
    static int   focused_pane;      // 0=left, 1=right

    // entry buffers (one per pane)
    static FMEntry entries_l[FM_MAX_ENTRIES];
    static FMEntry entries_r[FM_MAX_ENTRIES];
    static int     entry_count_l;
    static int     entry_count_r;

    // clipboard
    static char clipboard_path[FM_MAX_PATH];
    static char clipboard_name[FM_NAME_MAX];
    static bool clipboard_has_item;
    static bool clipboard_cut;       // true = move on paste

    // context menu
    static bool context_menu_open;
    static int  context_menu_x, context_menu_y;
    static int  context_menu_pane;
    static int  context_menu_idx;

    // properties dialog
    static bool properties_open;
    static char properties_path[FM_MAX_PATH];

    // sort menu
    static bool sort_menu_open;

    // rename inline
    static bool rename_mode;
    static int  rename_pane;
    static int  rename_target;
    static char rename_buf[FM_NAME_MAX];
    static int  rename_cursor;

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
    static void      PastePane(int pane);

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
    static void RenderContextMenu(int ox, int oy);
    static void RenderSortMenu(int ox, int oy);
    static void RenderProperties(int cx, int cy, int cw, int ch);

    // hit testing helpers
    static int HitTestEntry(int pane, int rel_x, int rel_y, int pane_w, int pane_h);
};
