#pragma once
//  kurono os  -  window manager
//  compositing window manager with z-ordering, drag, resize, close

#define WM_MAX_WINDOWS    32
#define WM_TITLEBAR_H     36
#define WM_BORDER_W        1
#define WM_MIN_WIDTH      140
#define WM_MIN_HEIGHT     100
#define WM_SHADOW_SIZE      6
#define WM_CORNER_RADIUS   10

enum WindowState {
    WIN_CLOSED = 0,
    WIN_NORMAL,
    WIN_MINIMIZED,
    WIN_MAXIMIZED,
    WIN_FULLSCREEN
};

enum WMAction {
    WM_NONE = 0,
    WM_DRAG,
    WM_RESIZE_TL, WM_RESIZE_TR, WM_RESIZE_BL, WM_RESIZE_BR,
    WM_RESIZE_T, WM_RESIZE_B, WM_RESIZE_L, WM_RESIZE_R
};

// forward declaration
class Window;

// callback for rendering window content
typedef void (*WindowRenderFunc)(Window* win, int x, int y, int w, int h);
// callback for handling input
typedef void (*WindowInputFunc)(Window* win, int event, int param1, int param2);

struct Window {
    int id;
    char title[64];
    int x, y, w, h;           // position and size
    int saved_x, saved_y;     // pre-maximize position
    int saved_w, saved_h;     // pre-maximize size
    WindowState state;
    bool visible;
    bool focused;
    bool resizable;
    bool closable;
    bool has_titlebar;
    int z_order;
    unsigned int bg_color;
    unsigned int title_color;
    unsigned int border_color;

    // content area (excludes titlebar and borders)
    int content_x, content_y, content_w, content_h;

    // callbacks
    WindowRenderFunc render;
    WindowInputFunc  input;
    void* user_data;

    // dirty tracking
    bool dirty;

    // ── compositor: per-window animation + opacity ────────────────────
    // anim_kind: 0=none, 1=open, 2=close, 3=minimize, 4=restore.
    // anim_start_ms is in Timer::GetRealMs() units; phase = clamp01
    // ((now - start) / duration).  CLOSE / MINIMIZE animations defer
    // the actual state transition until phase reaches 1.0 so the
    // window remains visible (with shrinking outline) during the
    // transition.  alpha is a user-controllable post-pass overlay.
    unsigned int anim_start_ms;
    unsigned short anim_duration_ms;
    unsigned char anim_kind;
    unsigned char alpha;            // 0..255
    short anchor_x, anchor_y;       // taskbar target for minimize anim
};

class WindowManager {
public:
    static void Init(int screen_w, int screen_h);
    static void Update(int mouse_x, int mouse_y, bool mouse_down, bool mouse_clicked);
    static void Render();

    // window lifecycle
    static Window* CreateWindow(const char* title, int x, int y, int w, int h);
    static void CloseWindow(int id);
    static void CloseAll();
    static void DestroyWindow(int id);
    static Window* GetWindow(int id);
    static Window* GetFocusedWindow();

    // window ops
    static void Minimize(int id);
    static void Maximize(int id);
    static void Restore(int id);
    static void ToggleMaximize(int id);
    static void BringToFront(int id);
    static void Focus(int id);
    static void SetTitle(int id, const char* title);
    static void MoveWindow(int id, int x, int y);
    static void ResizeWindow(int id, int w, int h);
    static void SetVisible(int id, bool visible);
    static void MarkDirty(int id);

    // query
    static int  GetWindowCount();
    static Window* GetWindows();
    static int  GetScreenWidth();
    static int  GetScreenHeight();
    static bool IsPointInWindow(Window* win, int px, int py);

    // desktop area (excluding taskbar)
    static int GetDesktopY();
    static int GetDesktopH();
    static int GetFocusedIndex();
    static void SetDesktopArea(int x, int y, int w, int h);

    // split mouse event api (used by desktopenvironment)
    static bool HandleMouseDown(int mx, int my);
    static void HandleMouseMove(int mx, int my);
    static void HandleMouseUp(int mx, int my);
    static void HandlePointerMove(int mx, int my);
    static void HandlePointerButton(int mx, int my, int button, bool pressed);
    static void RenderAll();

    // query drag state for render optimization (skip shadows during drag)
    static bool IsDragging();
    static WMAction GetCurrentAction();

    // create window with render/input callbacks, returns window id
    static int CreateWindow(const char* title, int x, int y, int w, int h,
        WindowRenderFunc render, WindowInputFunc input);

    // ── compositor configuration / animation ──────────────────────────
    // Re-read display.* and compositor.* keys from UIConfig and apply
    // to Graphics frame pacing + animation defaults.  Called by the
    // shell `kurono reload` command and by Settings.
    static void ReloadFromConfig();

    // Advance per-window animation phase, retire windows whose CLOSE
    // animation finished, hide windows whose MINIMIZE animation
    // finished.  Safe to call every frame; uses Timer::GetRealMs.
    static void TickAnimations();

    // Set per-window alpha (0..255).  255 = fully opaque.
    static void SetAlpha(int id, unsigned char a);
    static unsigned char GetAlpha(int id);

    // Set the taskbar anchor point used as the target of the minimize
    // animation for a window.  Defaults to the screen-bottom centre.
    static void SetTaskbarAnchor(int id, int x, int y);

private:
    static Window windows[WM_MAX_WINDOWS];
    static int window_count;
    static int next_id;
    static int focused_id;
    static int screen_width, screen_height;

    // drag state
    static WMAction current_action;
    static int action_window_id;
    static int drag_offset_x, drag_offset_y;
    static bool prev_mouse_down;
    static bool mouse_is_down;

    static void RenderTitlebar(Window* win);
    static void RenderShadow(Window* win);
    static void UpdateContentArea(Window* win);
    static WMAction HitTest(Window* win, int px, int py);
    static int  TopWindowAt(int px, int py);
    static void SortByZOrder();
};
