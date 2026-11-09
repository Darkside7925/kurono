#include "clipboard_ui.h"
#include "window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../system/clipboard.h"

namespace {

constexpr int CLIP_UI_W       = 600;
constexpr int CLIP_UI_HEADER  = 38;
constexpr int CLIP_UI_ROW     = 44;
constexpr int CLIP_UI_FOOTER  = 26;
constexpr int CLIP_UI_MAXROWS = 9;

constexpr uint32_t CU_BG     = 0xFF1E1F22;
constexpr uint32_t CU_EDGE   = 0xFF3A3C42;
constexpr uint32_t CU_SEL    = 0xFF33363C;
constexpr uint32_t CU_TEXT   = 0xFFDADCE0;
constexpr uint32_t CU_TITLE  = 0xFFE8EAED;
constexpr uint32_t CU_DIM    = 0xFF8A8F98;

int g_wid = -1;
int g_prev_focus = -1;
int g_sel = 0;
int g_top = 0;

int rows_visible(int ch) {
    int rows = (ch - CLIP_UI_HEADER - CLIP_UI_FOOTER) / CLIP_UI_ROW;
    if (rows < 1) rows = 1;
    if (rows > CLIP_UI_MAXROWS) rows = CLIP_UI_MAXROWS;
    return rows;
}

int row_at(int ch, int y) {
    if (y < CLIP_UI_HEADER) return -1;
    int r = (y - CLIP_UI_HEADER) / CLIP_UI_ROW;
    if (r < 0 || r >= rows_visible(ch)) return -1;
    return g_top + r;
}

void activate(int index) {
    ClipboardManager::HistoryActivate(index);
    ClipboardUI::Close();
}

void render_cb(Window* win, int cx, int cy, int cw, int ch) {
    Graphics::FillRect(cx, cy, cw, ch, CU_BG);
    Graphics::DrawRect(cx, cy, cw, ch, CU_EDGE);
    Graphics::DrawString(cx + 16, cy + 11, "Clipboard history", CU_TITLE, CU_BG);
    Graphics::DrawString(cx + cw - 76, cy + 11, "Super+V", CU_DIM, CU_BG);

    int count = ClipboardManager::HistoryCount();
    int rows = rows_visible(ch);
    int max_top = count > rows ? count - rows : 0;
    if (g_top > max_top) g_top = max_top;
    if (g_top < 0) g_top = 0;

    if (count == 0) {
        Graphics::DrawString(cx + 16, cy + CLIP_UI_HEADER + 14,
                             "no history yet - copy something first", CU_DIM, CU_BG);
    } else {
        for (int r = 0; r < rows; r++) {
            int i = g_top + r;
            if (i >= count) break;
            int y = cy + CLIP_UI_HEADER + r * CLIP_UI_ROW;
            if (i == g_sel) Graphics::FillRect(cx + 8, y, cw - 16, CLIP_UI_ROW - 4, CU_SEL);
            char buf[96];
            uint32_t len = 0;
            if (!ClipboardManager::HistoryTextAt(i, buf, (int)sizeof(buf), &len)) continue;
            for (int k = 0; buf[k]; k++) if ((unsigned char)buf[k] < 32) buf[k] = ' ';
            Graphics::DrawString(cx + 16, y + 12, buf, CU_TEXT, CU_SEL);
        }
    }
    Graphics::DrawString(cx + 16, cy + ch - 20,
                         "Enter: paste   Esc: close   Wheel: scroll", CU_DIM, CU_BG);
}

void input_cb(Window* win, int ev, int p1, int p2) {
    (void)win;
    int ch = win ? win->content_h : 0;
    if (ev == 1) {
        int i = row_at(ch, p2);
        if (i >= 0 && i < ClipboardManager::HistoryCount()) { g_sel = i; activate(i); }
    } else if (ev == 3) {
        int count = ClipboardManager::HistoryCount();
        int rows = rows_visible(ch);
        int max_top = count > rows ? count - rows : 0;
        g_top += (p1 > 0) ? 1 : -1;
        if (g_top < 0) g_top = 0;
        if (g_top > max_top) g_top = max_top;
        Graphics::MarkUIDirty();
    } else if (ev == 5) {
        int i = row_at(ch, p2);
        if (i >= 0 && i != g_sel) { g_sel = i; Graphics::MarkUIDirty(); }
    }
}

void layout() {
    Window* w = WindowManager::GetWindow(g_wid);
    if (!w) return;
    int count = ClipboardManager::HistoryCount();
    int rows = count < 1 ? 1 : (count > CLIP_UI_MAXROWS ? CLIP_UI_MAXROWS : count);
    int h = CLIP_UI_HEADER + rows * CLIP_UI_ROW + CLIP_UI_FOOTER;
    int sw = WindowManager::GetScreenWidth();
    int sh = WindowManager::GetScreenHeight();
    int x = (sw - CLIP_UI_W) / 2;
    int y = (sh - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    WindowManager::MoveWindow(g_wid, x, y);
    WindowManager::ResizeWindow(g_wid, CLIP_UI_W, h);
}

void ensure_window() {
    if (g_wid > 0) {
        Window* w = WindowManager::GetWindow(g_wid);
        if (w && w->state != WIN_CLOSED) return;
    }
    g_wid = WindowManager::CreateWindow("Clipboard", -1, -1, CLIP_UI_W, 200,
                                        render_cb, input_cb);
    Window* w = WindowManager::GetWindow(g_wid);
    if (!w) return;
    w->has_titlebar = false;
    w->resizable = false;
    w->closable = false;
    w->visible = false;
}

}  // namespace

namespace ClipboardUI {

bool IsOpen() {
    Window* w = WindowManager::GetWindow(g_wid);
    return w && w->state != WIN_CLOSED && w->visible;
}

void Toggle() {
    if (IsOpen()) { Close(); return; }
    // remember the app focus BEFORE creating the popup: CreateWindow focuses
    // the new window itself. (satoru)
    Window* fw = WindowManager::GetFocusedWindow();
    ensure_window();
    if (g_wid < 0) return;
    g_prev_focus = (fw && fw->id != g_wid) ? fw->id : -1;
    g_sel = 0;
    g_top = 0;
    layout();
    WindowManager::SetVisible(g_wid, true);
    WindowManager::Focus(g_wid);
    Graphics::MarkUIDirty();
}

void Close() {
    if (!IsOpen()) return;
    WindowManager::SetVisible(g_wid, false);
    int prev = g_prev_focus;
    g_prev_focus = -1;
    if (prev > 0 && WindowManager::GetWindow(prev)) WindowManager::Focus(prev);
    Graphics::MarkUIDirty();
}

bool KeyEvent(int key, bool shift, bool ctrl, bool alt) {
    (void)shift; (void)ctrl; (void)alt;
    if (!IsOpen()) return false;
    int count = ClipboardManager::HistoryCount();
    int rows = rows_visible(WindowManager::GetWindow(g_wid)->content_h);
    if (key == KEY_ESC) { Close(); return true; }
    if (key == KEY_UP && count > 0) {
        if (g_sel > 0) g_sel--;
        if (g_sel < g_top) g_top = g_sel;
        Graphics::MarkUIDirty();
        return true;
    }
    if (key == KEY_DOWN && count > 0) {
        if (g_sel + 1 < count) g_sel++;
        if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
        Graphics::MarkUIDirty();
        return true;
    }
    if (key == KEY_PAGEUP && count > 0) {
        g_sel -= rows; if (g_sel < 0) g_sel = 0;
        g_top -= rows; if (g_top < 0) g_top = 0;
        Graphics::MarkUIDirty();
        return true;
    }
    if (key == KEY_PAGEDOWN && count > 0) {
        g_sel += rows; if (g_sel >= count) g_sel = count - 1;
        g_top += rows;
        int max_top = count > rows ? count - rows : 0;
        if (g_top > max_top) g_top = max_top;
        Graphics::MarkUIDirty();
        return true;
    }
    if ((key == KEY_ENTER || key == KEY_KP_ENTER) && count > 0) {
        activate(g_sel);
        return true;
    }
    return true;   // modal while open (satoru)
}

}  // namespace ClipboardUI

// end (satoru)
