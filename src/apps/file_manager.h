#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — File Manager Application
// ═══════════════════════════════════════════════════════════════════════════

#define FM_MAX_ENTRIES   128
#define FM_MAX_PATH      256
#define FM_NAME_MAX      64

struct FMEntry {
    char name[FM_NAME_MAX];
    bool is_dir;
    int  size;
    int  permissions;
    bool selected;
};

enum FMViewMode {
    FM_VIEW_LIST = 0,
    FM_VIEW_GRID = 1,
};

class FileManagerApp {
public:
    static void Init();
    static int  Open();

    // Window callbacks
    static void Render(void* win, int x, int y, int w, int h);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

private:
    static char current_path[FM_MAX_PATH];
    static FMEntry entries[FM_MAX_ENTRIES];
    static int entry_count;
    static int selected_index;
    static int scroll_offset;
    static FMViewMode view_mode;
    static bool show_hidden;

    // Navigation
    static void NavigateTo(const char* path);
    static void GoUp();
    static void GoHome();
    static void Refresh();

    // Actions
    static void OpenEntry(int idx);
    static void DeleteEntry(int idx);
    static void CreateFolder();
    static void CreateFile();
    static void CopyEntry(int idx);
    static void PasteEntry();
    static void RenameEntry(int idx);

    // Context menu
    static bool  context_menu_open;
    static int   context_menu_x, context_menu_y;
    static int   context_menu_idx;     // Entry index for context menu
    static char  clipboard_path[FM_MAX_PATH];
    static char  clipboard_name[FM_NAME_MAX];
    static bool  clipboard_has_item;
    static bool  rename_mode;
    static char  rename_buf[FM_NAME_MAX];
    static int   rename_cursor;

    // Render helpers
    static void RenderToolbar(int x, int y, int w);
    static void RenderPathBar(int x, int y, int w);
    static void RenderFileList(int x, int y, int w, int h);
    static void RenderFileGrid(int x, int y, int w, int h);
    static void RenderStatusBar(int x, int y, int w);
    static void RenderEntryIcon(int x, int y, FMEntry* e);
    static void RenderContextMenu(int ox, int oy);
};
