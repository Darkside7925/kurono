#pragma once
//  kurono os - text editor application
#include "../kui/kui.h"
#include "editor/editor_text.h"
#include "editor/editor_highlight.h"
#include "editor/editor_regex.h"

#define ED_MAX_TABS   16
#define ED_PATH_MAX   256
#define ED_SIDE_MAX   192
#define ED_FIND_MAX   128
#define ED_DLG_ENTRIES 160

// the editor is one window with any number of open documents (tabs), a file sidebar, a find and
// replace bar and modal prompts. each document's text, cursor, undo history and highlight cache
// live in ed::Doc, so any number of editor windows stay independent. (satoru)

enum EdDialog {
    ED_DLG_NONE = 0,
    ED_DLG_OPEN,
    ED_DLG_SAVEAS,
    ED_DLG_FOLDER,
    ED_DLG_GOTO,
    ED_DLG_UNSAVED,
    ED_DLG_HELP
};

enum EdDialogAction {
    ED_ACT_NONE = 0,
    ED_ACT_CLOSE_TAB,
    ED_ACT_CLOSE_WINDOW
};

struct EdColors {
    uint32_t bg, panel, panel2, panel3, line, text, text2, accent, sel, curline;
    uint32_t gutter_bg, gutter_text, gutter_active;
    uint32_t tab_bg, tab_active, tab_text, tab_dim, tab_hover;
    uint32_t keyword, type, string, comment, number, preproc, func, op, punct, heading, link;
    uint32_t modified, match, match_cur, match_line, bracket, err, scrim, button, button_text;
    bool dark, modern;
};

struct EdInput {
    char text[ED_PATH_MAX];
    int  len;
    int  cursor;
};

class TextEditorApp : public kui::AppHandler {
public:
    TextEditorApp();
    ~TextEditorApp() override;

    static int  Open();
    static int  OpenFile(const char* path);
    bool LoadFile(const char* path);

    void OnCreate(kui::Window& w) override;
    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now_ms) override;
    bool OnEvent(kui::Window& w, const kui::Event& e) override;
    bool OnClose(kui::Window& w) override;

private:
    // ---- documents (satoru)
    ed::Doc docs[ED_MAX_TABS];
    int ntabs;
    int active;
    ed::Regex re;
    bool re_ready;

    ed::Doc* D() { return &docs[active]; }
    ed::Doc* Dn(int i) { return (i >= 0 && i < ntabs) ? &docs[i] : nullptr; }
    void AddDoc(const char* path);
    void OpenPath(kui::Window& w, const char* path);
    void RequestExit(kui::Window& w);
    void CloseDoc(int idx);
    bool LoadInto(int idx, const char* path);
    bool SaveDoc(int idx, const char* path, bool save_as);
    bool SaveDocContent(int idx, const char* path);
    void SyncTitle(kui::Window& w);
    int  FindDocByPath(const char* path);

    // ---- theme and layout (satoru)
    EdColors col;
    float px_ui, px_mono;
    int line_h, char_w, menu_h, tab_h, status_h, sidebar_w, row_h, pad;
    int mono_ascent;
    kui::Face mono_face;
    kui::Face ui_face;
    kui::Face ui_bold;
    void UpdateTheme();
    void UpdateMetrics();

    // layout of the last frame (content local) (satoru)
    int ed_x, ed_y, ed_w, ed_h;          // text viewport inside the window
    int gutter_w;
    int tabbar_y, body_y;
    int side_x, side_y, side_w, side_h;
    uint32_t bg_applied;
    int fb_y, fb_h, fb_qx, fb_qy, fb_qw, fb_rx, fb_rw;
    int fb_regex_x, fb_case_x, fb_prev_x, fb_next_x, fb_close_x;

    // ---- pointer and focus (satoru)
    kui::Window* win;
    int cw, ch;
    int mx, my;
    bool pointer_inside;
    bool dragging;
    bool drag_in_text;
    uint32_t last_key_ms;
    bool focused;
    uint32_t frame_now;
    bool wrap;
    bool show_numbers;
    int  zoom_pct;
    int  goal_col;

    // ---- menu (satoru)
    int menu_open;                        // -1 none, 0 File, 1 Edit, 2 View, 3 Help
    int menu_hover;                       // row under the pointer
    int menu_x[4], menu_y[4], menu_w[4], menu_n[4];
    void DrawMenuBar(kui::Canvas& c);
    void DrawDropdown(kui::Canvas& c);
    bool MenuClick(int x, int y);
    void RunMenuAction(int menu, int item);
    void ResetMenuRects();

    // ---- tabs (satoru)
    int tab_x[ED_MAX_TABS], tab_w[ED_MAX_TABS], plus_x, plus_w;
    void DrawTabBar(kui::Canvas& c);
    int  TabAt(int x, int y, bool* close);
    bool close_hover[ED_MAX_TABS];

    // ---- sidebar (satoru)
    bool sidebar;
    char folder[ED_PATH_MAX];
    kui::DirEntry side_entries[ED_SIDE_MAX];
    int side_count;
    int side_sel;
    int side_scroll;
    uint32_t side_loaded_ms;
    void DrawSidebar(kui::Canvas& c);
    void LoadFolder(const char* path);
    bool SidebarClick(int x, int y);

    // ---- find and replace (satoru)
    bool find_open;
    bool find_replace;
    bool find_regex;
    bool find_case;
    int  find_field;                      // 0 query, 1 replacement
    EdInput find_q, find_repl;
    EdInput* find_cur() { return find_field == 1 ? &find_repl : &find_q; }
    bool find_valid;
    char find_err[72];
    int  match_a, match_b;
    bool have_match;
    bool find_wrapped;
    char status_msg[96];
    uint32_t status_ms;
    void Status(kui::Canvas& c, int x, int y, int w);
    void FindOpen(bool replace);
    void FindClose();
    void FindCompile();
    bool FindNext(bool backward, bool select);
    int  FindOnLine(const ed::Doc* d, int row, int from, int* a, int* b);
    void ReplaceCurrent();
    void ReplaceAll();
    void SetStatus(const char* msg, uint32_t now);

    // ---- dialogs (satoru)
    int  dlg;
    int  dlg_action;
    int  dlg_target;                      // document index for close/save prompts
    bool close_window_after;
    EdInput dlg_input;
    char dlg_dir[ED_PATH_MAX];
    char dlg_msg[96];
    int  dlg_sel;
    int  dlg_scroll;
    int  dlg_field;                       // 0 input, 1 list
    kui::DirEntry dlg_entries[ED_DLG_ENTRIES];
    int  dlg_count;
    struct DlgLayout {
        int x, y, w, h;
        int in_x, in_y, in_w, in_h;
        int list_x, list_y, list_w, list_h;
        int btn_x[4], btn_y[4], btn_w[4], btn_h;
        int nbtn;
    } dl;
    void OpenDialog(int kind, int target, int action);
    void CloseDialog();
    void DialogAccept(kui::Window& w);
    void DialogCancel();
    void DialogLoadDir(const char* dir);
    void LayoutDialog(int content_w, int content_h);
    void DialogDraw(kui::Canvas& c);
    bool DialogClick(kui::Window& w, int x, int y, int clicks);
    bool DialogKey(kui::Window& w, const kui::Event& e, bool* redraw);
    void PromptCloseDoc(kui::Window& w, int idx, bool window_after);
    void AfterSaveAction(kui::Window& w);

    // ---- editing (satoru)
    void InsertTyped(kui::Window& w, const char* s, int n);
    void InsertNewlineAuto();
    void DoBackspace();
    void DoDelete();
    void DoTab(bool shift);
    void IndentSelection(bool unindent);
    void CopySel();
    void CopySelTo(bool primary);         // primary selection for middle click (satoru)
    void CutSel();
    void PasteClip();
    void PastePrimary();                  // middle click (satoru)
    void SelectAll();
    void Undo();
    void Redo();
    void MoveCaret(int pos, bool extend);
    void MoveLines(int delta, bool extend);
    void MoveWords(int dir, bool extend);
    void GotoLine(kui::Window& w);
    void EnsureCaretVisible();

    // ---- bracket matching (satoru)
    int  bracket_a, bracket_b;
    void FindBrackets();

    // ---- drawing helpers (satoru)
    void DrawEditor(kui::Canvas& c);
    void DrawFindBar(kui::Canvas& c);
    void DrawStatus(kui::Canvas& c);
    void DrawScrollbar(kui::Canvas& c, int x, int y, int w, int h);
    int  LineDisplayCols(const ed::Doc* d, int row, bool* has_tab);
    int  CursorDisplayCol(const ed::Doc* d);
    int  RowVisualCount(const ed::Doc* d, int row, int max_cols);
    int  RowOfPos(const ed::Doc* d, int pos) { return ed::edx_line_of(d, pos); }
    int  TextTopY() { return body_y + tab_h; }
    void PositionToXY(int row, int disp_col, int* px, int* py, bool* visible);
    int  XYToPos(int x, int y);
    uint32_t TokColor(int kind);
    void DrawInput(kui::Canvas& c, EdInput* in, int x, int y, int w, int h, bool focused_field);
    bool InputKey(EdInput* in, const kui::Event& e);
    bool InputClick(EdInput* in, int x, int y, int w);
    void InputSet(EdInput* in, const char* s);
    void Button(kui::Canvas& c, int x, int y, int w, int h, const char* label, bool primary, bool hover);
    bool In(int x, int y, int w, int h, int px, int py) { return px >= x && py >= y && px < x + w && py < y + h; }
    int  UiWidth(const char* s) { return kui::MeasureWidth(ui_face, px_ui, s); }
    int  MonoWidth(const char* s) { return kui::MeasureWidth(mono_face, px_mono, s); }
    void UiText(kui::Canvas& c, int x, int y, const char* s, uint32_t color) { c.Text(x, y, ui_face, px_ui, s, color); }
    void UiTextBold(kui::Canvas& c, int x, int y, const char* s, uint32_t color) { c.Text(x, y, ui_bold, px_ui, s, color); }
    int  UiTextFit(kui::Canvas& c, int x, int y, const char* s, uint32_t color, int maxw) { return c.TextFit(x, y, ui_face, px_ui, s, color, maxw); }

    // ---- misc (satoru)
    bool ctrl_key;
    bool shift_key;
    bool HandleShortcut(kui::Window& w, int key, bool ctrl, bool shift);
    void ApplyAutoIndent();               // kept for symmetry, used by InsertNewlineAuto (satoru)
};

// end (satoru)
