// kui_kernel.cpp: the kernel backend of libkurono-ui (satoru)
// a kui window is a normal WindowManager window whose render/input callbacks are two thunks here. the (satoru)
// thunks translate wm events 1..6 into kui::Event, build a Canvas over the graphics back buffer at the (satoru)
// content origin, and call the app's AppHandler. KuiKernel::Pump (once per gui loop pass) runs timers, (satoru)
// frame requests, focus / pointer-leave edges and cleans up after closed windows. (satoru)
#include "kui_internal.h"
#include "kui_kernel.h"
#include "../kernel/heap.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../ui/window_manager.h"
#include "../ui/modern_draw.h"
#include "../ui/style.h"
#include "../ui/font.h"
#include "../ui/notification.h"
#include "../system/ui_config.h"
#include "../system/clipboard.h"
#include "../fs/kvfs.h"

extern "C" int tt_ascent_face(int face, float pixel_height);
extern "C" {
    extern const uint8_t _binary_modern_icons_bin_start[] __attribute__((weak));
    extern const uint8_t _binary_modern_icons_bin_end[]   __attribute__((weak));
}

namespace kui {

// ---- misc (satoru)

void* Alloc(size_t n) { return KernelHeap::Alloc(n); }
void  Free(void* p) { if (p) KernelHeap::Free(p); }
uint32_t NowMs() { return Timer::GetRealMs(); }
bool IsRing3() { return false; }
bool WatchFd(int fd, bool on) { (void)fd; (void)on; return false; }   // wm-driven: no loop to wake (satoru)
void Log(const char* tag, const char* msg) {
    SerialLogger::Log("[kui] "); SerialLogger::Log(tag ? tag : "?");
    SerialLogger::Log(": "); SerialLogger::Log(msg ? msg : ""); SerialLogger::Log("\r\n");
}

// ---- theme (satoru)

static Theme g_theme;
static uint32_t g_theme_ms = 0;
static bool g_theme_ok = false;

const Theme& CurrentTheme() {
    uint32_t now = Timer::GetRealMs();
    if (!g_theme_ok || now - g_theme_ms > 250u) {
        g_theme_ms = now; g_theme_ok = true;
        const MDraw::Palette& P = MDraw::Pal();
        g_theme.pal.panel = P.panel; g_theme.pal.panel2 = P.panel2; g_theme.pal.line = P.line;
        g_theme.pal.text = P.text; g_theme.pal.text2 = P.text2; g_theme.pal.hover = P.hover;
        g_theme.pal.danger = P.danger; g_theme.pal.accent = P.accent; g_theme.pal.dark = P.dark;
        g_theme.appearance = P.dark ? Appearance::Dark : Appearance::Light;
        g_theme.style = UIStyle::Modern() ? Style::Modern : Style::Classic;
        g_theme.minimal_icons = UIStyle::MinimalIcons();
        g_theme.reduced_motion = UIConfig::Bool("compositor.reduced_motion", false);
        g_theme.high_contrast = UIConfig::Bool("a11y.high_contrast", false);
        g_theme.ui_scale = Graphics::GetUIScale();
        if (g_theme.ui_scale <= 0) g_theme.ui_scale = 100;
    }
    return g_theme;
}

// ---- text through FontTTF (satoru)

namespace bk {
bool FontsOk() { return FontTTF::ok; }

int TextWidth(Face f, float px, const char* s) {
    switch (f) {
    case Face::SansBold:   return FontTTF::MeasureText(px, s, true);
    case Face::Mono:       return FontTTF::MeasureMono(px, s);
    case Face::Bitmap8x16: return raster::Bitmap8x16Width(px, s, 1 << 30);
    default:               return FontTTF::MeasureText(px, s, false);
    }
}

int TextAscent(Face f, float px) {
    if (!FontTTF::ok || f == Face::Bitmap8x16) return 0;
    if (f == Face::Mono) return tt_ascent_face(FontTTF::HasMono() ? 2 : 0, px);
    return FontTTF::Ascent(px, f == Face::SansBold);
}

int TextDraw(Canvas::Impl& ci, int ax, int atop, Face f, float px, const char* s, Color c) {
    if (ci.t.x1 <= ci.t.x0 || ci.t.y1 <= ci.t.y0) return TextWidth(f, px, s);
    Graphics::PushClipRect(ci.t.x0, ci.t.y0, ci.t.x1 - ci.t.x0, ci.t.y1 - ci.t.y0);
    int adv;
    switch (f) {
    case Face::SansBold:   adv = FontTTF::DrawText(ax, atop, px, s, c, true); break;
    case Face::Mono:       adv = FontTTF::DrawMono(ax, atop, px, s, c); break;
    case Face::Bitmap8x16: adv = raster::Bitmap8x16(ci.t, ax, atop, px, s, 1 << 30, c); break;
    default:               adv = FontTTF::DrawText(ax, atop, px, s, c, false); break;
    }
    Graphics::PopClipRect();
    return adv;
}

const uint8_t* IconBlob(size_t* n) {
    if (!_binary_modern_icons_bin_start || !_binary_modern_icons_bin_end) { *n = 0; return nullptr; }
    *n = (size_t)(_binary_modern_icons_bin_end - _binary_modern_icons_bin_start);
    return _binary_modern_icons_bin_start;
}
}  // namespace bk

// ---- windows (satoru)

static const int KUI_MAX_WIN = 16;
static const int KUI_MAX_TIMERS = 32;

struct Window::Impl {
    bool    used;
    int     wid;                      // WindowManager id (satoru)
    char    app_id[24];
    int     last_w, last_h;           // content size at the last frame, for Resize (satoru)
    bool    focused;                  // last focus state delivered (satoru)
    bool    inside;                   // pointer was over the content (satoru)
    int     px, py;                   // last pointer position, content-local (satoru)
    uint32_t frame_at;                // pending frame deadline, 0 = none (satoru)
    bool    swallow_event1;           // event 1 for a press already delivered from event 6 (satoru)
    bool    event1_valid;             // an event 1 click arrived for the press being handled (satoru)
    uint32_t event1_ms;               // when it arrived, for the event-order check below (satoru)
    int     event1_x, event1_y;
    Window  pub;                      // what the app sees (satoru)
};

static Window::Impl g_win[KUI_MAX_WIN];

struct KTimer { bool used; int id; int slot; uint32_t interval, next; bool repeat; };
static KTimer g_timers[KUI_MAX_TIMERS];
static int g_timer_next = 1;
static Window::Impl* g_cur = nullptr;   // window whose callback is running, for Quit (satoru)

static Window::Impl* slot_of(::Window* kw) {
    if (!kw || !kw->user_data) return nullptr;
    Window::Impl* s = (Window::Impl*)kw->user_data;
    if (s < g_win || s >= g_win + KUI_MAX_WIN || !s->used || s->wid != kw->id) return nullptr;
    return s;
}

static void mark(Window::Impl* s) {
    WindowManager::MarkDirty(s->wid);
    Graphics::MarkUIDirty();
}

static uint8_t mods_now() {
    const KeyboardState& st = Keyboard::GetState();
    uint8_t m = 0;
    if (st.shift) m |= M_SHIFT;
    if (st.ctrl)  m |= M_CTRL;
    if (st.alt)   m |= M_ALT;
    if (st.super) m |= M_SUPER;
    return m;
}

static void ev_init(Event& e, EventType t, Window::Impl* s) {
    for (size_t i = 0; i < sizeof(e); i++) ((char*)&e)[i] = 0;
    e.type = t;
    e.mods = mods_now();
    e.x = s ? s->px : 0; e.y = s ? s->py : 0;
    e.time_ms = Timer::GetRealMs();
}

static void deliver(Window::Impl* s, const Event& e) {
    if (!s || !s->used || !s->pub.handler) return;
    Window::Impl* prev = g_cur;
    g_cur = s;
    bool redraw = s->pub.handler->OnEvent(s->pub, e);
    g_cur = prev;
    if (redraw && s->used) mark(s);
}

static void release_slot(Window::Impl* s) {
    if (!s->used) return;
    for (int i = 0; i < KUI_MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].slot == (int)(s - g_win)) g_timers[i].used = false;
    AppHandler* h = s->pub.handler;
    s->used = false;
    s->pub.handler = nullptr;
    if (h) { h->OnClose(s->pub); delete h; }
}

static void render_thunk(::Window* kw, int cx, int cy, int cw, int ch) {
    Window::Impl* s = slot_of(kw);
    if (!s || !s->pub.handler) return;
    if (cw != s->last_w || ch != s->last_h) {
        bool first = s->last_w < 0;
        s->last_w = cw; s->last_h = ch;
        if (!first) { Event e; ev_init(e, EventType::Resize, s); e.w = cw; e.h = ch; deliver(s, e); }
    }
    if (Graphics::GetBpp() != 32 || !Graphics::GetBuffer()) return;
    int bx0 = cx, by0 = cy, bx1 = cx + cw, by1 = cy + ch;
    int gx, gy, gw, gh;
    if (Graphics::GetClip(gx, gy, gw, gh)) {
        if (gx > bx0) bx0 = gx;
        if (gy > by0) by0 = gy;
        if (gx + gw < bx1) bx1 = gx + gw;
        if (gy + gh < by1) by1 = gy + gh;
    }
    Canvas::Impl ci;
    CanvasInit(ci, (uint32_t*)Graphics::GetBuffer(), (int)(Graphics::GetPitch() / 4),
               Graphics::GetWidth(), Graphics::GetHeight(), cx, cy, cw, ch, bx0, by0, bx1, by1);
    Canvas c(&ci);
    Window::Impl* prev = g_cur;
    g_cur = s;
    s->pub.handler->OnFrame(s->pub, c, Timer::GetRealMs());
    g_cur = prev;
}

static Button btn_of(int b) { return b == 0 ? Button::Left : b == 1 ? Button::Right : b == 2 ? Button::Middle : Button::None; }

static void input_thunk(::Window* kw, int event, int p1, int p2) {
    Window::Impl* s = slot_of(kw);
    if (!s) return;
    Event e;
    switch (event) {
    case 1:   // click: a left press on the content, local coords (satoru)
        s->px = p1; s->py = p2;
        if (s->swallow_event1) {   // the press already produced its Click from event 6 (satoru)
            s->swallow_event1 = false;
            s->event1_valid = false;
            break;
        }
        ev_init(e, EventType::Click, s);
        e.button = Button::Left;
        e.clicks = ClickCount(p1, p2, e.time_ms);
        deliver(s, e);
        s->event1_valid = true;
        s->event1_ms = e.time_ms;
        s->event1_x = p1; s->event1_y = p2;
        break;
    case 2: { // a character from the keyboard queue (satoru)
        int ch = (unsigned char)(char)p1;
        if (ch == '\t') break;                       // tab comes through KeyEvent (shift+tab) (satoru)
        ev_init(e, EventType::KeyDown, s);
        if (ch == '\n' || ch == '\r') e.key = K_ENTER;
        else if (ch == '\b') e.key = K_BS;
        else { e.key = ch; if (ch >= 32) { e.text[0] = (char)ch; e.text[1] = 0; } }
        deliver(s, e);
        break;
    }
    case 3:   // wheel, positive = down (satoru)
        ev_init(e, EventType::Scroll, s);
        e.dy = (float)p1;
        e.scroll_steps = p1;
        deliver(s, e);
        break;
    case 5:   // pointer move, local coords (satoru)
        s->px = p1; s->py = p2; s->inside = true;
        ev_init(e, EventType::PointerMove, s);
        deliver(s, e);
        break;
    case 6:   // button edge: p1 = button, p2 = pressed (satoru)
        if (kw) { s->px = Mouse::mx - kw->content_x; s->py = Mouse::my - kw->content_y; }
        if (p2 && p1 == 0) {   // left press: PointerDown then Click, so the order is Down, Click, Up (satoru)
            ev_init(e, EventType::PointerDown, s);
            e.button = Button::Left;
            deliver(s, e);
            // the wm's event 1 for this press arrives after this (or after the up on a fast click): (satoru)
            // emit the Click here and drop the later event 1 for it. (satoru)
            bool dup = s->event1_valid && (uint32_t)(e.time_ms - s->event1_ms) <= 8 &&
                       s->event1_x == s->px && s->event1_y == s->py;
            s->event1_valid = false;
            if (!dup) {
                Event c; ev_init(c, EventType::Click, s);
                c.button = Button::Left;
                c.clicks = ClickCount(s->px, s->py, c.time_ms);
                deliver(s, c);
                s->swallow_event1 = true;
            }
            break;
        }
        ev_init(e, p2 ? EventType::PointerDown : EventType::PointerUp, s);
        e.button = btn_of(p1);
        deliver(s, e);
        break;
    default:  // 4 (right click) duplicates the event 6 edges (satoru)
        break;
    }
}

Window* OpenWindow(const WindowSpec& spec, AppHandler* h) {
    if (!h) return nullptr;
    Window::Impl* s = nullptr;
    for (int i = 0; i < KUI_MAX_WIN; i++) if (!g_win[i].used) { s = &g_win[i]; break; }
    if (!s) { delete h; return nullptr; }
    int ow = spec.w + 2 * WM_BORDER_W, oh = spec.h + WM_TITLEBAR_H + WM_BORDER_W;
    int wid = WindowManager::CreateWindow(spec.title ? spec.title : "", spec.x, spec.y, ow, oh,
                                          render_thunk, input_thunk);
    ::Window* kw = WindowManager::GetWindow(wid);
    if (!kw) { delete h; return nullptr; }
    s->used = true;
    s->wid = wid;
    int k = 0;
    if (spec.app_id) while (spec.app_id[k] && k < 23) { s->app_id[k] = spec.app_id[k]; k++; }
    s->app_id[k] = 0;
    s->last_w = -1; s->last_h = -1;
    s->focused = kw->focused;
    s->inside = false;
    s->px = s->py = 0;
    s->frame_at = 0;
    s->pub.handler = h;
    s->pub.user = nullptr;
    s->pub.impl = s;
    kw->user_data = s;
    kw->resizable = spec.resizable;
    kw->closable = spec.closable;
    if (spec.bg) kw->bg_color = spec.bg;
    if (spec.app_icon >= 0) kw->app_icon = spec.app_icon;
    Window::Impl* prev = g_cur;
    g_cur = s;
    h->OnCreate(s->pub);
    g_cur = prev;
    return &s->pub;
}

int Run(const WindowSpec& spec, AppFactory make) {
    if (!make) return -1;
    return OpenWindow(spec, make()) ? 0 : -1;
}

// the kernel backend is driven by the window manager, so there is no loop to run. (satoru)
int RunLoop() { return 0; }

void Quit(int code) {
    (void)code;
    if (!g_cur) return;
    char app[24];
    for (int i = 0; i < 24; i++) app[i] = g_cur->app_id[i];
    for (int i = 0; i < KUI_MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        bool same = true;
        for (int j = 0; j < 24; j++) { if (g_win[i].app_id[j] != app[j]) { same = false; break; } if (!app[j]) break; }
        if (same) WindowManager::CloseWindow(g_win[i].wid);
    }
}

void Window::SetTitle(const char* t) { WindowManager::SetTitle(impl->wid, t ? t : ""); }
void Window::Resize(int w, int h) {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    if (!kw) return;
    int ox = kw->x, oy = kw->y, ow = kw->w, oh = kw->h;
    WindowManager::ResizeWindow(impl->wid, w + 2 * WM_BORDER_W, h + WM_TITLEBAR_H + WM_BORDER_W);
    WindowManager::AnimateGeometry(impl->wid, ox, oy, ow, oh, kw->has_titlebar);   // the wm morphs it (satoru)
    mark(impl);
}
void Window::SetFullscreen(bool on) {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    if (kw && ((kw->state == WIN_FULLSCREEN) != on)) WindowManager::ToggleFullscreen(impl->wid);
}
bool Window::Fullscreen() const {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    return kw && kw->state == WIN_FULLSCREEN;
}
Rect Window::Frame() const {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    if (!kw) return Rect{ 0, 0, 0, 0 };
    return Rect{ kw->x, kw->y, kw->w, kw->h };
}
bool Window::Maximized() const {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    return kw && kw->state == WIN_MAXIMIZED;
}
bool Window::Minimized() const {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    return kw && kw->state == WIN_MINIMIZED;
}
void Window::Move(int x, int y) { WindowManager::MoveWindow(impl->wid, x, y); }
void Window::SetMaximized(bool on) {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    if (kw && ((kw->state == WIN_MAXIMIZED) != on)) WindowManager::ToggleMaximize(impl->wid);
}
void Window::Raise() { WindowManager::Focus(impl->wid); }
void Window::Close() { WindowManager::CloseWindow(impl->wid); }
void Window::SetBackground(Color c) {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    if (kw && c) { kw->bg_color = c; mark(impl); }
}
Size Window::ContentSize() const {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    if (!kw) return Size{ 0, 0 };
    return Size{ kw->content_w, kw->content_h };
}
bool Window::Focused() const {
    ::Window* kw = WindowManager::GetWindow(impl->wid);
    return kw && kw->focused;
}
void Window::Invalidate() { mark(impl); }
void Window::Invalidate(const Rect& r) { (void)r; mark(impl); }   // the wm repaints whole contents (satoru)
void Window::RequestFrameAt(uint32_t ms) {
    if (ms == 0) ms = 1;
    if (impl->frame_at == 0 || (int32_t)(ms - impl->frame_at) < 0) impl->frame_at = ms;
    Graphics::RequestFrameAt(ms);
}
void Window::RequestAnimationFrame() { RequestFrameAt(Timer::GetRealMs() + 1); }
void Window::SetBadge(int count) { (void)count; }
void Window::Notify(int kind, const char* title, const char* body) {
    NotificationManager::Post(title ? title : "", body ? body : "", kind, 4000);
}

// ---- timers (satoru)

int StartTimer(Window* w, uint32_t interval_ms, bool repeat) {
    if (!w || !w->impl) return 0;
    for (int i = 0; i < KUI_MAX_TIMERS; i++) {
        if (g_timers[i].used) continue;
        KTimer& t = g_timers[i];
        t.used = true; t.id = g_timer_next++; t.slot = (int)(w->impl - g_win);
        t.interval = interval_ms ? interval_ms : 1; t.next = Timer::GetRealMs() + t.interval; t.repeat = repeat;
        Graphics::RequestFrameAt(t.next);
        return t.id;
    }
    return 0;
}

void StopTimer(int id) {
    for (int i = 0; i < KUI_MAX_TIMERS; i++) if (g_timers[i].used && g_timers[i].id == id) g_timers[i].used = false;
}

}  // namespace kui

// kernel Key -> kui key code; 0 = not a special the char queue misses (satoru)
static int kui_special(int k, bool shift) {
    switch ((::Key)k) {
    case KEY_UP: return kui::K_UP;       case KEY_DOWN: return kui::K_DOWN;
    case KEY_LEFT: return kui::K_LEFT;   case KEY_RIGHT: return kui::K_RIGHT;
    case KEY_HOME: return kui::K_HOME;   case KEY_END: return kui::K_END;
    case KEY_PAGEUP: return kui::K_PGUP; case KEY_PAGEDOWN: return kui::K_PGDN;
    case KEY_DELETE: return kui::K_DEL;  case KEY_ESC: return kui::K_ESC;
    case KEY_TAB: return shift ? kui::K_BTAB : kui::K_TAB;
    case KEY_LSHIFT: case KEY_RSHIFT: return kui::K_SHIFT;
    case KEY_LCTRL: case KEY_RCTRL: return kui::K_CTRL;
    case KEY_LALT: case KEY_RALT: return kui::K_ALT;
    case KEY_LSUPER: case KEY_RSUPER: return kui::K_SUPER;
    default: break;
    }
    if (k >= (int)KEY_F1 && k <= (int)KEY_F12) return kui::K_F1 + (k - (int)KEY_F1);
    return 0;
}

// kui key code -> kernel Key, for KeyHeld (satoru)
static int kernel_key(int key) {
    if (key >= 'a' && key <= 'z') return (int)KEY_A + (key - 'a');
    if (key >= 'A' && key <= 'Z') return (int)KEY_A + (key - 'A');
    if (key >= '0' && key <= '9') return (int)KEY_0 + (key - '0');
    switch (key) {
    case kui::K_UP: return KEY_UP;       case kui::K_DOWN: return KEY_DOWN;
    case kui::K_LEFT: return KEY_LEFT;   case kui::K_RIGHT: return KEY_RIGHT;
    case kui::K_HOME: return KEY_HOME;   case kui::K_END: return KEY_END;
    case kui::K_PGUP: return KEY_PAGEUP; case kui::K_PGDN: return KEY_PAGEDOWN;
    case kui::K_DEL: return KEY_DELETE;  case kui::K_ESC: return KEY_ESC;
    case kui::K_TAB: return KEY_TAB;     case kui::K_ENTER: return KEY_ENTER;
    case kui::K_BS: return KEY_BACKSPACE; case kui::K_SPACE: return KEY_SPACE;
    case kui::K_SHIFT: return KEY_LSHIFT; case kui::K_CTRL: return KEY_LCTRL;
    case kui::K_ALT: return KEY_LALT;    case kui::K_SUPER: return KEY_LSUPER;
    default: break;
    }
    if (key >= kui::K_F1 && key < kui::K_F1 + 12) return (int)KEY_F1 + (key - kui::K_F1);
    return 0;
}

namespace kui {

bool KeyHeld(int key) {
    int k = kernel_key(key);
    if (!k) return false;
    if (key == K_SHIFT) return Keyboard::IsKeyDown(KEY_LSHIFT) || Keyboard::IsKeyDown(KEY_RSHIFT);
    if (key == K_CTRL)  return Keyboard::IsKeyDown(KEY_LCTRL) || Keyboard::IsKeyDown(KEY_RCTRL);
    if (key == K_ALT)   return Keyboard::IsKeyDown(KEY_LALT) || Keyboard::IsKeyDown(KEY_RALT);
    return Keyboard::IsKeyDown((::Key)k);
}

uint8_t ModsHeld() { return mods_now(); }

// ---- clipboard (satoru)

namespace Clipboard {
bool SetText(const char* s) { ClipboardManager::SetText(s ? s : ""); return true; }
const char* GetText() { const char* t = ClipboardManager::GetText(); return t ? t : ""; }
bool HasText() { return ClipboardManager::HasText(); }
void Clear() { ClipboardManager::Clear(); }
bool SetPrimaryText(const char* s) { ClipboardManager::SetPrimaryText(s ? s : ""); return true; }
const char* GetPrimaryText() { const char* t = ClipboardManager::GetPrimaryText(); return t ? t : ""; }
bool HasPrimaryText() { return ClipboardManager::HasPrimaryText(); }
}

// ---- files (satoru)

namespace Fs {
int  ReadFile(const char* p, void* buf, int max) { int n = KVFS::ReadFile(p, buf, max < 0 ? 0 : (uint32_t)max); return n < 0 ? -1 : n; }
bool WriteFile(const char* p, const void* d, int len) { return KVFS::WriteFile(p, d, len < 0 ? 0 : (uint32_t)len) >= 0; }
int  FileSize(const char* p) { return KVFS::Exists(p) ? KVFS::GetFileSize(p) : -1; }
bool Exists(const char* p) { return KVFS::Exists(p); }
bool IsDir(const char* p) { return KVFS::IsDir(p); }
bool Mkdirs(const char* p) { return KVFS::Mkdirs(p) >= 0; }
bool Remove(const char* p) { return KVFS::IsDir(p) ? KVFS::Rmdir(p) >= 0 : KVFS::Unlink(p) >= 0; }
bool Rename(const char* a, const char* b) { return KVFS::Move(a, b) >= 0; }
int  ListDir(const char* p, DirEntry* out, int max) {
    static KVFSNode* nodes[256];
    if (max > 256) max = 256;
    int n = KVFS::Listdir(p, nodes, max);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (nodes[i]->name[k] && k < 63) { out[i].name[k] = nodes[i]->name[k]; k++; }
        out[i].name[k] = 0;
        out[i].dir = nodes[i]->type == KVFS_DIR;
        out[i].size = nodes[i]->size;
    }
    return n;
}
}

Size ScreenSize(Window* w) {
    (void)w;   // one logical desktop: the wm screen (satoru)
    return Size{ WindowManager::GetScreenWidth(), WindowManager::GetScreenHeight() };
}

Rect WorkArea(Window* w) {
    (void)w;   // one logical desktop: the area between the menu bar and the dock (satoru)
    return Rect{ 0, WindowManager::GetDesktopY(), WindowManager::GetScreenWidth(), WindowManager::GetDesktopH() };
}

// ---- the pump (satoru)

void PumpWindows() {
    uint32_t now = Timer::GetRealMs();
    for (int i = 0; i < KUI_MAX_WIN; i++) {
        Window::Impl* s = &g_win[i];
        if (!s->used) continue;
        ::Window* kw = WindowManager::GetWindow(s->wid);
        // closed, or its close motion started: the app is done with it (satoru)
        if (!kw || kw->state == WIN_CLOSED || kw->anim_kind == 2 || kw->user_data != s) { release_slot(s); continue; }
        if (kw->focused != s->focused) {
            s->focused = kw->focused;
            Event e; ev_init(e, s->focused ? EventType::Focus : EventType::Blur, s);
            deliver(s, e);
        }
        if (s->inside) {
            int mx = Mouse::mx, my = Mouse::my;
            bool in = mx >= kw->content_x && mx < kw->content_x + kw->content_w &&
                      my >= kw->content_y && my < kw->content_y + kw->content_h &&
                      kw->state != WIN_MINIMIZED;
            if (!in) {
                s->inside = false;
                Event e; ev_init(e, EventType::PointerLeave, s);
                deliver(s, e);
            }
        }
        if (s->pub.handler) {
            Window::Impl* prev = g_cur;
            g_cur = s;
            s->pub.handler->OnTick(s->pub, now);
            g_cur = prev;
        }
        if (s->used && s->frame_at && (int32_t)(now - s->frame_at) >= 0) {
            s->frame_at = 0;
            mark(s);
        }
    }
    for (int i = 0; i < KUI_MAX_TIMERS; i++) {
        KTimer& t = g_timers[i];
        if (!t.used || (int32_t)(now - t.next) < 0) continue;
        Window::Impl* s = &g_win[t.slot];
        int id = t.id;
        if (t.repeat) { t.next = now + t.interval; Graphics::RequestFrameAt(t.next); }
        else t.used = false;
        if (!s->used) { t.used = false; continue; }
        Event e; ev_init(e, EventType::Timer, s);
        e.timer_id = id;
        deliver(s, e);
    }
}

void ThemeChangedAll() {
    for (int i = 0; i < KUI_MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        Event e; ev_init(e, EventType::ThemeChanged, &g_win[i]);
        deliver(&g_win[i], e);
        if (g_win[i].used) mark(&g_win[i]);
    }
}

void KeyToFocused(int key, bool pressed, bool shift, int raw) {
    (void)key;
    ::Window* fw = WindowManager::GetFocusedWindow();
    Window::Impl* s = slot_of(fw);
    if (!s) return;
    int code = kui_special(raw, shift);
    if (pressed && !code) return;     // printable presses arrive as wm char events (satoru)
    if (!pressed && !code) {
        char ch = Keyboard::KeyToChar((::Key)raw, false, false);
        if (raw == (int)KEY_ENTER) code = K_ENTER;
        else if (raw == (int)KEY_BACKSPACE) code = K_BS;
        else if (ch) code = (unsigned char)ch;
        else return;
    }
    Event e; ev_init(e, pressed ? EventType::KeyDown : EventType::KeyUp, s);
    e.key = code;
    deliver(s, e);
}

}  // namespace kui

// end (satoru)
