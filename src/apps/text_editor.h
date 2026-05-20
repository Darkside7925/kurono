#pragma once
//  kurono os  -  text editor application

#define ED_MAX_LINES   2048
#define ED_LINE_MAX    256
#define ED_PATH_MAX    256

struct EditorLine {
    char text[ED_LINE_MAX];
    int  len;
};

class TextEditorApp {
public:
    static void Init();
    static int  Open();
    static int  OpenFile(const char* path);

    // window callbacks
    static void Render(void* win, int x, int y, int w, int h);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

    // file operations
    static bool LoadFile(const char* path);
    static bool SaveFile();
    static bool SaveFileAs(const char* path);

private:
    static char file_path[ED_PATH_MAX];
    static EditorLine lines[ED_MAX_LINES];
    static int line_count;
    static int cursor_row;
    static int cursor_col;
    static int scroll_x;
    static int scroll_y;
    static int sel_start_row, sel_start_col;
    static int sel_end_row, sel_end_col;
    static bool has_selection;
    static bool modified;
    static bool show_line_numbers;

    // editing
    static void InsertChar(char c);
    static void DeleteChar();
    static void Backspace();
    static void InsertNewline();
    static void DeleteLine(int row);

    // cursor movement
    static void MoveCursorUp();
    static void MoveCursorDown();
    static void MoveCursorLeft();
    static void MoveCursorRight();
    static void MoveCursorHome();
    static void MoveCursorEnd();
    static void PageUp(int vis_rows);
    static void PageDown(int vis_rows);

    // render helpers
    static void RenderMenuBar(int x, int y, int w);
    static void RenderMenuDropdown(int x, int y);
    static void RenderGutter(int x, int y, int h, int vis_rows);
    static void RenderContent(int x, int y, int w, int h);
    static void RenderStatusBar(int x, int y, int w);
    static void RenderCursor(int cx, int cy, int content_x, int content_y);

    // menu state
    static int  menu_open_idx;       // -1 = none, 0=File, 1=Edit, 2=View
    static int  menu_x, menu_y;      // dropdown anchor position
};
