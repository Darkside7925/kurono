#pragma once
//  kurono os - window manager
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
    // pre-snap rect so SnapWindow(edge=3) can restore it; independent of
    // the maximize save so snap/maximize don't clobber each other. (satoru)
    int snap_saved_x, snap_saved_y, snap_saved_w, snap_saved_h;
    bool snap_saved;
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

    // titlebar traffic-light hover/press: which button the pointer is over
    // (0=close,1=min,2=max, -1=none) and the ms timestamp of the last press so
    // the render path can ease a hover-grow + press-dip through KSS::Anim. the
    // colour/scale tweens themselves live in the anim engine keyed by window id,
    // so these two fields are all the per-window state needed. (satoru)
    signed char tb_hover_btn;
    unsigned int tb_press_ms;

    // last-rendered geometry: used to damage the previous footprint
    // when the window moves/resizes/closes so the compositor below
    // can repaint just what changed.
    int last_x, last_y, last_w, last_h;
    bool had_last;

    // multi-monitor: index of the output this window currently lives on.
    // a window belongs to the monitor whose rect contains its center; the
    // wm recomputes this on move/resize. defaults to 0 (primary). (satoru)
    int monitor_id;
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
    // ── window snapping ───────────────────────────────────────────────
    // snap a window to a screen region within the wm desktop area (taskbar
    // excluded, reusing the maximize bounds). edge codes:
    //   0=left half, 1=right half, 2=maximize, 3=restore pre-snap rect,
    //   4=top-left quarter, 5=top-right, 6=bottom-left, 7=bottom-right.
    // (satoru)
    static void SnapWindow(int win_id, int edge);
    // snap the currently focused window. (satoru)
    static void SnapFocused(int edge);
    static void BringToFront(int id);
    static void Focus(int id);
    static void SetTitle(int id, const char* title);
    static void MoveWindow(int id, int x, int y);
    static void ResizeWindow(int id, int w, int h);
    static void SetVisible(int id, bool visible);
    static void MarkDirty(int id);

    // ── multi-monitor ─────────────────────────────────────────────────
    // Which output a window currently lives on (by center point). (satoru)
    static int  GetWindowMonitor(int id);
    // Reposition a window onto another monitor's rect, preserving its
    // offset within the old monitor where it fits. No-op if the index is
    // invalid or there is only one monitor. (satoru)
    static void MoveWindowToMonitor(int win_id, int monitor_index);

    // ── window context menu (right-click on the titlebar) ─────────────
    // Minimal self-contained popup menu. HandleRightClick opens it when the
    // point is on a titlebar (returns true if consumed). HandleContextMenu-
    // Click routes a subsequent left-click to the menu (returns true if the
    // menu consumed/closed). The orchestrator wires these into its
    // right-click / click dispatch. (satoru)
    static bool HandleRightClick(int mx, int my);
    static bool HandleContextMenuClick(int mx, int my);
    static bool IsContextMenuOpen();
    static void CloseContextMenu();

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
    // refresh which titlebar traffic-light button the pointer is over so the
    // render path can ease its hover/press. (satoru)
    static void UpdateTitlebarHover(int mx, int my);
    static void RenderAll();

    // query drag state for render optimization (skip shadows during drag)
    static bool IsDragging();
    // true while any window is mid open/close/minimize/restore animation.
    // The damage-gated GUI loop forces continuous rendering while this holds
    // so animations never stall mid-flight. (satoru)
    static bool HasActiveAnimations();
    static WMAction GetCurrentAction();

    // ── cached-desktop snapshot during a window drag ──────────────────
    // when a titlebar drag is in progress we compose the whole desktop
    // (wallpaper + every other window + taskbar + control center) WITHOUT
    // the dragged window ONCE into an offscreen "drag backdrop" buffer, then
    // every subsequent drag frame we just blit that backdrop back and redraw
    // the single moving window on top. that turns ~5 full-screen passes per
    // frame into one straight backdrop copy plus one small window, which is
    // what makes the drag feel smooth. correctness first: the backdrop is
    // invalidated (forcing a normal full render) the instant anything behind
    // the window could have changed. (satoru)

    // true only while a left-button TITLEBAR drag (not resize) is active - 
    // the precondition for using the fast backdrop path. (satoru)
    static bool IsWindowDragActive();
    // true when a valid backdrop snapshot exists for the current drag. (satoru)
    static bool DragBackdropReady();
    // arm the next full render to omit the dragged window so the composed
    // frame can be snapshotted as the backdrop. (satoru)
    static void BeginDragCapture();
    // copy the freshly composed (dragged-window-excluded) back buffer into
    // the backdrop buffer, allocating it lazily, and mark the backdrop ready.
    // clears the capture skip. (satoru)
    static void CaptureDragBackdrop();
    // fast drag frame: blit the backdrop over the back buffer and draw only
    // the dragged window (and its shadow) on top, damaging just the union of
    // the window's previous and current footprints. (satoru)
    static void RenderDragFast();
    // draw only the dragged window on top of the current back buffer (used on
    // the capture frame, after the backdrop snapshot, so frame 1 is complete).
    // (satoru)
    static void RenderDraggedWindowOnly();
    // drop the backdrop so the next frame falls back to a full render. called
    // on drag end and by structural changes (open/close/focus/z-order). (satoru)
    static void InvalidateDragBackdrop();

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
    // draw one settled (non-animated) window: optional shadow, body, titlebar,
    // border and content. shared by the main z-order loop and the dragged-only
    // fast path so both stay pixel-identical. (satoru)
    static void RenderWindowBody(Window* win, bool with_shadow);
    static void UpdateContentArea(Window* win);
    static WMAction HitTest(Window* win, int px, int py);
    static int  TopWindowAt(int px, int py);
    static void SortByZOrder();

    // multi-monitor: recompute win->monitor_id from its center point. (satoru)
    static void UpdateWindowMonitor(Window* win);
    // render the window context menu (called at the end of Render). (satoru)
    static void RenderContextMenu();
};
