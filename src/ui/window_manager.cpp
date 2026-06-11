#include "window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Window Manager Implementation
// ═══════════════════════════════════════════════════════════════════════════

Window WindowManager::windows[WM_MAX_WINDOWS];
int WindowManager::window_count = 0;
int WindowManager::next_id = 1;
int WindowManager::focused_id = -1;
int WindowManager::screen_width = 1024;
int WindowManager::screen_height = 768;
WMAction WindowManager::current_action = WM_NONE;
int WindowManager::action_window_id = -1;
int WindowManager::drag_offset_x = 0;
int WindowManager::drag_offset_y = 0;
bool WindowManager::prev_mouse_down = false;
bool WindowManager::mouse_is_down = false;

static int wmlen(const char* s) { int n=0; while(s[n]) n++; return n; }
static void wmcpy(char* d, const char* s, int m) {
    int i=0; while(s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}

// Kurono UI color scheme — modern glassmorphism
#define COL_TITLE_BG       0xFF1C1C2E  // Frosted dark
#define COL_TITLE_FOCUSED  0xFF22223A  // Focused: deeper purple-blue
#define COL_TITLE_GRAD     0xFF2A2A48  // Gradient bottom for focused
#define COL_TITLE_TEXT     0xFFF0F0F5
#define COL_TITLE_TEXT_DIM 0xFF888898
#define COL_CLOSE_BTN      0xFFFF5F57  // macOS red
#define COL_MAX_BTN        0xFF28C840  // macOS green
#define COL_MIN_BTN        0xFFFFBD2E  // macOS amber
#define COL_BORDER         0xFF303050
#define COL_BORDER_FOCUS   0xFF6C8CFF  // Vibrant blue accent
#define COL_SHADOW         0x30000000
#define COL_WIN_BG         0xFF121218  // Dark background
#define COL_SEPARATOR      0xFF2A2A40  // Titlebar/content separator

// ── Init ─────────────────────────────────────────────────────────────────

void WindowManager::Init(int sw, int sh) {
    screen_width = sw;
    screen_height = sh;
    window_count = 0;
    next_id = 1;
    focused_id = -1;
    current_action = WM_NONE;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        windows[i].state = WIN_CLOSED;
        windows[i].id = 0;
    }

    SerialLogger::Log("WindowManager: Initialized\r\n");
}

// ── Window lifecycle ─────────────────────────────────────────────────────

Window* WindowManager::CreateWindow(const char* title, int x, int y, int w, int h) {
    // Find free slot
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state == WIN_CLOSED) { slot = i; break; }
    }
    if (slot == -1) return nullptr;

    Window* win = &windows[slot];
    win->id = next_id++;
    wmcpy(win->title, title, 64);
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->saved_x = x; win->saved_y = y;
    win->saved_w = w; win->saved_h = h;
    win->state = WIN_NORMAL;
    win->visible = true;
    win->focused = false;
    win->resizable = true;
    win->closable = true;
    win->has_titlebar = true;
    win->z_order = window_count;
    win->bg_color = COL_WIN_BG;
    win->title_color = COL_TITLE_BG;
    win->border_color = COL_BORDER;
    win->render = nullptr;
    win->input = nullptr;
    win->user_data = nullptr;
    win->dirty = true;

    UpdateContentArea(win);

    if (window_count < WM_MAX_WINDOWS) window_count++;
    Focus(win->id);

    return win;
}

void WindowManager::CloseWindow(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    win->state = WIN_CLOSED;
    win->visible = false;
    if (focused_id == id) focused_id = -1;
}

void WindowManager::CloseAll() {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED) {
            windows[i].state = WIN_CLOSED;
            windows[i].visible = false;
        }
    }
    focused_id = -1;
}

void WindowManager::DestroyWindow(int id) {
    CloseWindow(id);
}

Window* WindowManager::GetWindow(int id) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].id == id && windows[i].state != WIN_CLOSED)
            return &windows[i];
    }
    return nullptr;
}

Window* WindowManager::GetFocusedWindow() {
    return GetWindow(focused_id);
}

// ── Window operations ────────────────────────────────────────────────────

void WindowManager::Minimize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    win->state = WIN_MINIMIZED;
    win->visible = false;
}

void WindowManager::Maximize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->state != WIN_MAXIMIZED) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    win->x = 0; win->y = GetDesktopY();
    win->w = screen_width; win->h = GetDesktopH();
    win->state = WIN_MAXIMIZED;
    win->visible = true;
    win->dirty = true;
    UpdateContentArea(win);
}

void WindowManager::Restore(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    win->x = win->saved_x; win->y = win->saved_y;
    win->w = win->saved_w; win->h = win->saved_h;
    win->state = WIN_NORMAL;
    win->visible = true;
    win->dirty = true;
    UpdateContentArea(win);
}

void WindowManager::ToggleMaximize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->state == WIN_MAXIMIZED) Restore(id);
    else Maximize(id);
}

void WindowManager::BringToFront(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    int max_z = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED && windows[i].z_order > max_z)
            max_z = windows[i].z_order;
    }
    win->z_order = max_z + 1;
}

void WindowManager::Focus(int id) {
    // Unfocus old
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].focused) {
            windows[i].focused = false;
            windows[i].dirty = true;
        }
    }
    Window* win = GetWindow(id);
    if (win) {
        win->focused = true;
        win->dirty = true;
        focused_id = id;
        BringToFront(id);
    }
}

void WindowManager::SetTitle(int id, const char* title) {
    Window* win = GetWindow(id);
    if (win) { wmcpy(win->title, title, 64); win->dirty = true; }
}

void WindowManager::MoveWindow(int id, int x, int y) {
    Window* win = GetWindow(id);
    if (win) { win->x = x; win->y = y; win->dirty = true; UpdateContentArea(win); }
}

void WindowManager::ResizeWindow(int id, int w, int h) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (w < WM_MIN_WIDTH) w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    win->w = w; win->h = h;
    win->dirty = true;
    UpdateContentArea(win);
}

void WindowManager::SetVisible(int id, bool visible) {
    Window* win = GetWindow(id);
    if (win) { win->visible = visible; win->dirty = true; }
}

void WindowManager::MarkDirty(int id) {
    Window* win = GetWindow(id);
    if (win) win->dirty = true;
}

// ── Query ────────────────────────────────────────────────────────────────

int WindowManager::GetWindowCount() {
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED) count++;
    }
    return count;
}

Window* WindowManager::GetWindows() { return windows; }
int WindowManager::GetScreenWidth() { return screen_width; }
int WindowManager::GetScreenHeight() { return screen_height; }
int WindowManager::GetDesktopY() { return 0; }
int WindowManager::GetDesktopH() { return screen_height - 40; } // 40px taskbar

int WindowManager::GetFocusedIndex() { return focused_id; }

void WindowManager::SetDesktopArea(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w;
    // Primarily adjusts the desktop height (taskbar offset)
    // Store for use in GetDesktopH
    screen_height = y + h + (screen_height - (y + h));
    // In practice just used to inform the WM of the taskbar region — 
    // the 40px offset is already baked in for now.
    (void)h;
}

bool WindowManager::IsPointInWindow(Window* win, int px, int py) {
    if (!win || !win->visible || win->state == WIN_MINIMIZED) return false;
    return px >= win->x && px < win->x + win->w &&
           py >= win->y && py < win->y + win->h;
}

void WindowManager::UpdateContentArea(Window* win) {
    if (win->has_titlebar) {
        win->content_x = win->x + WM_BORDER_W;
        win->content_y = win->y + WM_TITLEBAR_H;
        win->content_w = win->w - WM_BORDER_W * 2;
        win->content_h = win->h - WM_TITLEBAR_H - WM_BORDER_W;
    } else {
        win->content_x = win->x;
        win->content_y = win->y;
        win->content_w = win->w;
        win->content_h = win->h;
    }
}

// ── Hit testing ──────────────────────────────────────────────────────────

WMAction WindowManager::HitTest(Window* win, int px, int py) {
    if (!win) return WM_NONE;
    int r = 6; // Resize border sensitivity

    bool top    = (py >= win->y && py < win->y + r);
    bool bottom = (py >= win->y + win->h - r && py < win->y + win->h);
    bool left   = (px >= win->x && px < win->x + r);
    bool right  = (px >= win->x + win->w - r && px < win->x + win->w);

    if (win->resizable) {
        if (top && left)   return WM_RESIZE_TL;
        if (top && right)  return WM_RESIZE_TR;
        if (bottom && left)  return WM_RESIZE_BL;
        if (bottom && right) return WM_RESIZE_BR;
        if (top)    return WM_RESIZE_T;
        if (bottom) return WM_RESIZE_B;
        if (left)   return WM_RESIZE_L;
        if (right)  return WM_RESIZE_R;
    }

    // Titlebar drag
    if (py >= win->y && py < win->y + WM_TITLEBAR_H &&
        px >= win->x && px < win->x + win->w) {
        return WM_DRAG;
    }

    return WM_NONE;
}

int WindowManager::TopWindowAt(int px, int py) {
    int best_id = -1, best_z = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED && windows[i].visible &&
            windows[i].state != WIN_MINIMIZED) {
            if (IsPointInWindow(&windows[i], px, py) && windows[i].z_order > best_z) {
                best_z = windows[i].z_order;
                best_id = windows[i].id;
            }
        }
    }
    return best_id;
}

void WindowManager::SortByZOrder() {
    // Simple bubble sort for rendering order
    for (int i = 0; i < WM_MAX_WINDOWS - 1; i++) {
        for (int j = 0; j < WM_MAX_WINDOWS - i - 1; j++) {
            if (windows[j].z_order > windows[j+1].z_order) {
                Window tmp = windows[j];
                windows[j] = windows[j+1];
                windows[j+1] = tmp;
            }
        }
    }
}

// ── Update (input handling) ──────────────────────────────────────────────

void WindowManager::Update(int mouse_x, int mouse_y, bool mouse_down, bool mouse_clicked) {
    // Handle ongoing drag/resize
    if (current_action != WM_NONE && mouse_down) {
        Window* win = GetWindow(action_window_id);
        if (win) {
            switch (current_action) {
                case WM_DRAG:
                    win->x = mouse_x - drag_offset_x;
                    win->y = mouse_y - drag_offset_y;
                    // Clamp
                    if (win->y < 0) win->y = 0;
                    if (win->y > screen_height - 20) win->y = screen_height - 20;
                    UpdateContentArea(win);
                    win->dirty = true;
                    break;
                case WM_RESIZE_BR:
                    win->w = mouse_x - win->x;
                    win->h = mouse_y - win->y;
                    if (win->w < WM_MIN_WIDTH) win->w = WM_MIN_WIDTH;
                    if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
                    UpdateContentArea(win);
                    win->dirty = true;
                    break;
                case WM_RESIZE_R:
                    win->w = mouse_x - win->x;
                    if (win->w < WM_MIN_WIDTH) win->w = WM_MIN_WIDTH;
                    UpdateContentArea(win);
                    win->dirty = true;
                    break;
                case WM_RESIZE_B:
                    win->h = mouse_y - win->y;
                    if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
                    UpdateContentArea(win);
                    win->dirty = true;
                    break;
                case WM_RESIZE_BL: {
                    int new_x = mouse_x;
                    int new_w = (win->x + win->w) - new_x;
                    if (new_w >= WM_MIN_WIDTH) { win->x = new_x; win->w = new_w; }
                    win->h = mouse_y - win->y;
                    if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
                    UpdateContentArea(win); win->dirty = true;
                    break;
                }
                default: break;
            }
        }
    }

    if (!mouse_down) {
        current_action = WM_NONE;
        action_window_id = -1;
    }

    // Handle new clicks
    if (mouse_clicked) {
        int top_id = TopWindowAt(mouse_x, mouse_y);
        if (top_id > 0) {
            Focus(top_id);
            Window* win = GetWindow(top_id);
            if (win) {
                // macOS traffic light buttons on LEFT side
                int btn_cy = win->y + WM_TITLEBAR_H / 2;
                int btn_start_x = win->x + 14;
                int btn_r = 8; // click radius slightly larger than visual

                // Close button
                if (win->closable) {
                    int dx = mouse_x - btn_start_x;
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r*btn_r) {
                        CloseWindow(top_id);
                        return;
                    }
                }

                // Minimize button
                {
                    int dx = mouse_x - (btn_start_x + 22);
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r*btn_r) {
                        Minimize(top_id);
                        return;
                    }
                }

                // Maximize button
                {
                    int dx = mouse_x - (btn_start_x + 44);
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r*btn_r) {
                        ToggleMaximize(top_id);
                        return;
                    }
                }

                // Titlebar double-click → maximize
                // Start drag/resize
                WMAction action = HitTest(win, mouse_x, mouse_y);
                if (action != WM_NONE) {
                    current_action = action;
                    action_window_id = top_id;
                    drag_offset_x = mouse_x - win->x;
                    drag_offset_y = mouse_y - win->y;
                }

                // Pass click to window input handler
                if (win->input && mouse_y > win->y + WM_TITLEBAR_H) {
                    int local_x = mouse_x - win->content_x;
                    int local_y = mouse_y - win->content_y;
                    win->input(win, 1, local_x, local_y); // event 1 = click
                }
            }
        }
    }

    prev_mouse_down = mouse_down;
}

// ── Render ───────────────────────────────────────────────────────────────

void WindowManager::RenderShadow(Window* win) {
    int sz = win->focused ? WM_SHADOW_SIZE + 3 : WM_SHADOW_SIZE;
    // Multi-layer soft shadow
    for (int i = 1; i <= sz; i++) {
        unsigned int alpha = (unsigned int)((win->focused ? 20 : 12) * (sz - i + 1));
        if (alpha > 0xFF) alpha = 0xFF;
        unsigned int col = alpha << 24;
        // Bottom shadow (wider)
        Graphics::FillRect(win->x + i, win->y + win->h + i, win->w - i, 1, col);
        // Right shadow
        Graphics::FillRect(win->x + win->w + i, win->y + i + 4, 1, win->h - 4, col);
        // Left shadow (subtle)
        if (i <= 3) Graphics::FillRect(win->x - i, win->y + i + 4, 1, win->h - 4, col >> 1);
    }
}

void WindowManager::RenderTitlebar(Window* win) {
    unsigned int bg = win->focused ? COL_TITLE_FOCUSED : COL_TITLE_BG;
    unsigned int text_col = win->focused ? COL_TITLE_TEXT : COL_TITLE_TEXT_DIM;

    // Titlebar background with rounded top corners
    Graphics::FillRoundedRect(win->x, win->y, win->w, WM_TITLEBAR_H, WM_CORNER_RADIUS, bg);
    // Bottom half (square corners to merge with body)
    Graphics::FillRect(win->x, win->y + WM_CORNER_RADIUS, win->w, WM_TITLEBAR_H - WM_CORNER_RADIUS, bg);

    // Subtle gradient overlay for focused windows
    if (win->focused) {
        for (int row = 0; row < 4; row++) {
            uint8_t a = (uint8_t)(30 - row * 6);
            Graphics::FillRect(win->x + 1, win->y + WM_TITLEBAR_H - 4 + row, win->w - 2, 1,
                              (a << 24) | 0x000000);
        }
    }

    // Separator line
    Graphics::FillRect(win->x, win->y + WM_TITLEBAR_H, win->w, 1, COL_SEPARATOR);

    // macOS-style traffic light buttons (LEFT side)
    int btn_y = win->y + WM_TITLEBAR_H / 2;
    int btn_r = 6;
    int btn_start_x = win->x + 14;

    // Close (red)
    if (win->closable) {
        Graphics::FillCircle(btn_start_x, btn_y, btn_r, COL_CLOSE_BTN);
        if (win->focused) {
            // X mark
            Graphics::DrawLine(btn_start_x - 2, btn_y - 2, btn_start_x + 2, btn_y + 2, 0x80000000);
            Graphics::DrawLine(btn_start_x + 2, btn_y - 2, btn_start_x - 2, btn_y + 2, 0x80000000);
        }
    }

    // Minimize (yellow)
    Graphics::FillCircle(btn_start_x + 22, btn_y, btn_r, COL_MIN_BTN);
    if (win->focused) {
        Graphics::FillRect(btn_start_x + 19, btn_y, 6, 1, 0x80000000);
    }

    // Maximize (green)
    Graphics::FillCircle(btn_start_x + 44, btn_y, btn_r, COL_MAX_BTN);
    if (win->focused) {
        // Diagonal arrows
        Graphics::DrawLine(btn_start_x + 42, btn_y - 2, btn_start_x + 46, btn_y + 2, 0x80000000);
        Graphics::DrawLine(btn_start_x + 46, btn_y - 2, btn_start_x + 42, btn_y + 2, 0x80000000);
    }

    // Title text — centered in remaining space
    int title_area_start = btn_start_x + 62;
    int title_area_end = win->x + win->w - 10;
    int title_w = wmlen(win->title) * 8;
    int title_x = title_area_start + (title_area_end - title_area_start - title_w) / 2;
    if (title_x < title_area_start) title_x = title_area_start;
    Graphics::DrawString(title_x, win->y + (WM_TITLEBAR_H - 12) / 2, win->title, text_col, 0x00000000);

    // Focused accent glow on top border
    if (win->focused) {
        Graphics::FillRect(win->x + WM_CORNER_RADIUS, win->y, win->w - 2 * WM_CORNER_RADIUS, 1, COL_BORDER_FOCUS);
    }
}

void WindowManager::Render() {
    // Render windows in z-order (lowest first)
    // Build sorted index
    int indices[WM_MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED && windows[i].visible &&
            windows[i].state != WIN_MINIMIZED)
            indices[count++] = i;
    }

    // Sort by z-order
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (windows[indices[j]].z_order > windows[indices[j+1]].z_order) {
                int tmp = indices[j]; indices[j] = indices[j+1]; indices[j+1] = tmp;
            }
        }
    }

    // Render each window
    for (int i = 0; i < count; i++) {
        Window* win = &windows[indices[i]];

        // Shadow
        RenderShadow(win);

        // Window body — smooth rounded rectangle
        unsigned int border = win->focused ? COL_BORDER_FOCUS : COL_BORDER;

        // Full body background with rounded corners
        Graphics::FillRoundedRect(win->x, win->y, win->w, win->h, WM_CORNER_RADIUS, win->bg_color);

        // Titlebar (draws over the top portion)
        if (win->has_titlebar) RenderTitlebar(win);

        // Subtle border
        Graphics::DrawRect(win->x, win->y, win->w, win->h, border);

        // Content
        if (win->render) {
            win->render(win, win->content_x, win->content_y, win->content_w, win->content_h);
        }

        win->dirty = false;
    }
}

// ── Split mouse event API ────────────────────────────────────────────────

bool WindowManager::HandleMouseDown(int mx, int my) {
    mouse_is_down = true;
    int top_id = TopWindowAt(mx, my);
    if (top_id > 0) {
        Focus(top_id);
        Window* win = GetWindow(top_id);
        if (!win) return true;

        // macOS traffic light buttons on LEFT side
        int btn_cy = win->y + WM_TITLEBAR_H / 2;
        int btn_start_x = win->x + 14;
        int btn_r = 8;

        if (win->closable) {
            int dx = mx - btn_start_x;
            int dy = my - btn_cy;
            if (dx*dx + dy*dy <= btn_r*btn_r) { CloseWindow(top_id); return true; }
        }
        {
            int dx = mx - (btn_start_x + 22);
            int dy = my - btn_cy;
            if (dx*dx + dy*dy <= btn_r*btn_r) { Minimize(top_id); return true; }
        }
        {
            int dx = mx - (btn_start_x + 44);
            int dy = my - btn_cy;
            if (dx*dx + dy*dy <= btn_r*btn_r) { ToggleMaximize(top_id); return true; }
        }
        WMAction action = HitTest(win, mx, my);
        if (action != WM_NONE) {
            current_action = action;
            action_window_id = top_id;
            drag_offset_x = mx - win->x;
            drag_offset_y = my - win->y;
        }
        if (win->input && my > win->y + WM_TITLEBAR_H) {
            win->input(win, 1, mx - win->content_x, my - win->content_y);
        }
        return true;
    }
    return false;
}

void WindowManager::HandleMouseMove(int mx, int my) {
    if (!mouse_is_down || current_action == WM_NONE) return;
    Window* win = GetWindow(action_window_id);
    if (!win) return;

    switch (current_action) {
        case WM_DRAG:
            win->x = mx - drag_offset_x;
            win->y = my - drag_offset_y;
            if (win->y < 0) win->y = 0;
            if (win->y > screen_height - 20) win->y = screen_height - 20;
            UpdateContentArea(win);
            win->dirty = true;
            break;
        case WM_RESIZE_BR:
            win->w = mx - win->x; win->h = my - win->y;
            if (win->w < WM_MIN_WIDTH) win->w = WM_MIN_WIDTH;
            if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
            UpdateContentArea(win); win->dirty = true;
            break;
        case WM_RESIZE_R:
            win->w = mx - win->x;
            if (win->w < WM_MIN_WIDTH) win->w = WM_MIN_WIDTH;
            UpdateContentArea(win); win->dirty = true;
            break;
        case WM_RESIZE_B:
            win->h = my - win->y;
            if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
            UpdateContentArea(win); win->dirty = true;
            break;
        case WM_RESIZE_BL: {
            int new_x = mx;
            int new_w = (win->x + win->w) - new_x;
            if (new_w >= WM_MIN_WIDTH) { win->x = new_x; win->w = new_w; }
            win->h = my - win->y;
            if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
            UpdateContentArea(win); win->dirty = true;
            break;
        }
        default: break;
    }
}

void WindowManager::HandleMouseUp(int mx, int my) {
    (void)mx; (void)my;
    mouse_is_down = false;
    current_action = WM_NONE;
    action_window_id = -1;
}

void WindowManager::RenderAll() {
    Render();
}

int WindowManager::CreateWindow(const char* title, int x, int y, int w, int h,
    WindowRenderFunc render_func, WindowInputFunc input_func) {
    Window* win = CreateWindow(title, x, y, w, h);
    if (!win) return -1;
    win->render = render_func;
    win->input = input_func;
    return win->id;
}
