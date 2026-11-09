// kui.h: libkurono-ui, one app source for two backends (phase 1 of separate_everything_plan.md) (satoru)
// KUI_BACKEND_KERNEL (default): the app is drawn by the in-kernel window manager, like today. (satoru)
// KUI_BACKEND_WAYLAND: the app is a ring-3 musl-static program that draws into a wl_shm buffer and (satoru)
// presents through kurono's own wayland server, so it gets kurono title bars, motion, dock and focus. (satoru)
// (satoru)
// version rule: KUI_VERSION 1 is frozen (plus the agreed additions marked in agent_kui.md). later versions only ADD functions, enum values at the end and (satoru)
// trailing struct fields; nothing here changes meaning or signature, so phase 2 ports never break. (satoru)
// (satoru)
// rules for both backends: no exceptions, no rtti, no std library in app code, no heap in the draw path, (satoru)
// plain function pointers. all coordinates are window-content-local physical pixels: (0,0) is the top (satoru)
// left of the content area, below the title bar. the kernel backend adds content_x / content_y itself. (satoru)
// (satoru)
// what each item replaces (real call sites in src/apps, see scratchpad/kui_interface_draft.h) (satoru)
//   kui::Window           <- WindowManager::CreateWindow / CloseWindow / ResizeWindow / SetTitle / ... (satoru)
//   Window::Invalidate    <- Graphics::MarkDirty / MarkUIDirty / WindowManager::MarkDirty (satoru)
//   AppHandler::OnFrame   <- WindowRenderFunc and the static App::Draw() hooks (satoru)
//   AppHandler::OnEvent   <- WindowInputFunc events 1..6, polled Mouse:: / Keyboard:: state, the (satoru)
//                            per-app InputManager key hooks (SystemSettings::KeyEvent, PtyKeyEvent) (satoru)
//   kui::Canvas           <- Graphics:: primitives, MDraw::, WMMotion::SoftShadow, FontTTF::, SKit pieces (satoru)
//   kui::CurrentTheme     <- MDraw::Pal, UIStyle::Modern / MinimalIcons, ui.appearance (satoru)
//   kui::NowMs / timers   <- Timer::GetRealMs and hand-rolled "now - t0 < 240" checks (satoru)
//   kui::Clipboard        <- ClipboardManager (system/clipboard.h) (satoru)
//   kui::Fs               <- KVFS:: calls in apps (ring 3 uses the linux abi with the same kvfs paths) (satoru)

#pragma once
#include <stdint.h>
#include <stddef.h>

#define KUI_VERSION 1

namespace kui {

// ---- basic types (satoru)

typedef uint32_t Color;                       // 0xaarrggbb, the packing Graphics uses (satoru)

struct Rect { int x, y, w, h; };              // window-local rectangle (satoru)

struct Size { int w, h; };                    // a width and height pair (satoru)

// ---- theme (satoru)

enum class Appearance : uint8_t { Light, Dark };   // ui.appearance (satoru)

enum class Style : uint8_t { Modern, Classic };    // ui.style (satoru)

struct Palette {                              // the same fields as MDraw::Palette, so kit code ports by rename (satoru)
    Color panel, panel2, line, text, text2, hover, danger, accent;   // shell surface colours (satoru)
    bool  dark;                               // true when the dark palette is active (satoru)
};

struct Theme {
    Appearance appearance;                    // light or dark (satoru)
    Style      style;                         // modern or classic chrome (satoru)
    bool       minimal_icons;                 // ui.icons minimal vs colorful (satoru)
    bool       reduced_motion;                // animations off or reduced (satoru)
    bool       high_contrast;                 // accessibility high contrast flag (satoru)
    int        ui_scale;                      // percent, 100 = 1x (satoru)
    Palette    pal;                           // resolved colours for this appearance (satoru)
};

const Theme& CurrentTheme();                  // kernel: UIConfig via MDraw::Pal; ring 3: the theme file / env (satoru)
Color Mix(Color a, Color b, int t);           // opaque mix, t 0..255, 0 = a; exactly MDraw::Mix (satoru)

// ---- fonts and text (satoru)

enum class Face : uint8_t { Sans, SansBold, Mono, Bitmap8x16 };   // inter, inter semibold, dejavu mono, vga 8x16 (satoru)

struct TextMetrics { int width, ascent, descent, line_h; };      // measured extent of a string (satoru)

bool FontsReady();                            // false when no ttf face is loaded: every face draws 8x16 (satoru)
TextMetrics Measure(Face f, float px, const char* s);            // FontTTF::MeasureText / Measure / MeasureMono (satoru)
int  MeasureWidth(Face f, float px, const char* s);              // just the width, the common case (satoru)
int  MeasureN(Face f, float px, const char* s, int n);           // width of the first n bytes, for carets (satoru)
int  FitChars(Face f, float px, const char* s, int maxw);        // bytes that fit in maxw, for ellipsis and wrap (satoru)
int  Ascent(Face f, float px);                                    // FontTTF::Ascent (satoru)
// integer-size twins for files built with -mno-sse (the video app): px in whole pixels (satoru)
int  MeasureWidthI(Face f, int px, const char* s);
int  AscentI(Face f, int px);

// ---- input (satoru)

enum Key : int {                              // printables arrive as their ascii code, specials as below (satoru)
    K_BS = 8, K_TAB = 9, K_ENTER = 10, K_ESC = 27, K_SPACE = 32, K_DEL_ASCII = 127,   // ascii controls (satoru)
    K_UP = 0x90, K_DOWN = 0x91, K_LEFT = 0x92, K_RIGHT = 0x93,   // arrows, same codes as SKit::Keys (satoru)
    K_HOME = 0x94, K_END = 0x95, K_PGUP = 0x96, K_PGDN = 0x97, K_DEL = 0x98, K_BTAB = 0x99,   // navigation, shift+tab (satoru)
    K_F1 = 0xA0,                              // f1..f12 are K_F1 + 0..11 (satoru)
    K_SHIFT = 0xB0, K_CTRL = 0xB1, K_ALT = 0xB2, K_SUPER = 0xB3,  // modifier keys, KeyDown/KeyUp only (satoru)
};

enum Mod : uint8_t { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4, M_SUPER = 8 };   // modifier bits (satoru)

enum class Button : uint8_t { None, Left, Right, Middle };   // pointer buttons (satoru)

enum class EventType : uint8_t {
    KeyDown,                                  // key pressed or auto-repeated, key + text filled (satoru)
    KeyUp,                                    // key released (satoru)
    PointerMove,                              // pointer moved over the content (wm event 5) (satoru)
    PointerDown,                              // a button went down over the content (wm event 6, 4) (satoru)
    PointerUp,                                // a button came up (wm event 6); delivered even outside (satoru)
    Click,                                    // left PRESS on the content, right after PointerDown, like the (satoru)
                                              // kernel wm's event 1 always did; clicks counts double clicks (satoru)
    Scroll,                                   // wheel (wm event 3): dy in notches, positive = content down (satoru)
    PointerLeave,                             // pointer left the content area, clear hover (satoru)
    Focus,                                    // window gained keyboard focus (satoru)
    Blur,                                     // window lost keyboard focus (satoru)
    Resize,                                   // content size changed, w and h filled; a frame follows (satoru)
    ThemeChanged,                             // appearance, accent or scale changed; a frame follows (satoru)
    Timer,                                    // a StartTimer deadline fired, timer_id filled (satoru)
};

struct Event {
    EventType type;                           // which of the above (satoru)
    uint8_t   mods;                           // Mod bits held at the time of the event (satoru)
    Button    button;                         // pointer down, up and click (satoru)
    uint8_t   clicks;                         // Click: 1 single, 2 double (two within 400 ms and 10 px) (satoru)
    int       x, y;                           // pointer position, content-local (also on key events) (satoru)
    int       key;                            // Key or printable code for key events (satoru)
    char      text[8];                        // utf-8 text the key produced, empty for specials (satoru)
    float     dx, dy;                         // scroll amount in notches (satoru)
    int       w, h;                           // new content size for Resize (satoru)
    int       timer_id;                       // which timer fired for Timer (satoru)
    uint32_t  time_ms;                        // event time in NowMs units (satoru)
    int       scroll_steps;                   // Scroll: dy rounded to whole notches, for -mno-sse callers (satoru)
};

bool    KeyHeld(int key);                     // current key state (letters/digits as ascii, specials as Key) (satoru)
uint8_t ModsHeld();                           // current Mod bits (satoru)

// ---- drawing (satoru)

class Canvas {                                // valid only inside OnFrame, clipped to the content and the damage (satoru)
public:
    int  Width() const;                       // content width in pixels (satoru)
    int  Height() const;                      // content height in pixels (satoru)
    Rect Damage() const;                      // the part being repainted; drawing outside it is clipped away (satoru)

    // clip stack, intersects with the parent clip; every primitive and text honours it (satoru)
    void PushClip(int x, int y, int w, int h);       // Graphics::PushClipRect / SKit::PushClip (satoru)
    void PopClip();                                  // Graphics::PopClipRect / SKit::PopClip (satoru)

    // classic primitives; colours with alpha < 0xF0 blend like Graphics does (satoru)
    void Clear(Color c);                                              // fill the whole content (satoru)
    void FillRect(int x, int y, int w, int h, Color c);               // Graphics::FillRect (satoru)
    void FillRectAlpha(int x, int y, int w, int h, uint8_t a, Color c);   // Graphics::FillRectAlpha (satoru)
    void FillRoundedRect(int x, int y, int w, int h, int r, Color c); // Graphics::FillRoundedRect (aliased) (satoru)
    void DrawRect(int x, int y, int w, int h, Color c);               // one-pixel outline, Graphics::DrawRect (satoru)
    void DrawLine(int x0, int y0, int x1, int y1, Color c, int thickness = 1);   // Graphics::DrawLine (satoru)
    void FillCircle(int cx, int cy, int r, Color c);                  // Graphics::FillCircle (satoru)
    void DrawCircle(int cx, int cy, int r, Color c);                  // outline, Graphics::DrawCircle (satoru)

    // anti-aliased modern primitives, alpha 0..255 (satoru)
    void RoundRect(int x, int y, int w, int h, int r, Color top, Color bottom, int alpha);   // MDraw::RoundRect gradient (satoru)
    void RoundRect(int x, int y, int w, int h, int r, Color c, int alpha);   // flat MDraw::RoundRect (satoru)
    void RoundOutline(int x, int y, int w, int h, int r, Color c, int alpha);   // MDraw::RoundOutline (satoru)
    void Circle(int cx, int cy, int r, Color c, int alpha);           // MDraw::Circle (satoru)
    void Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha);   // MDraw::Shadow / SKit::Shadow layered (satoru)
    void SoftShadow(float x, float y, float w, float h, float r, float blur, float dy, int alpha);   // WMMotion::SoftShadow (satoru)

    // images, argb rows of stride_px pixels (satoru)
    void Blit(int x, int y, const uint32_t* argb, int w, int h, int stride_px);   // opaque copy, alpha ignored (satoru)
    void BlitAlpha(int x, int y, const uint32_t* argb, int w, int h, int stride_px, int alpha);   // straight alpha * alpha/255 (satoru)
    void BlitPremul(int x, int y, const uint32_t* argb, int w, int h, int stride_px, int alpha);  // premultiplied source (satoru)
    void BlitScaled(const Rect& dst, const uint32_t* argb, int w, int h, int stride_px);   // bilinear, video and thumbnails (satoru)
    void BlitScaledFast(const Rect& dst, const uint32_t* argb, int w, int h, int stride_px);   // nearest, full-rate video (satoru)

    // shared kit pieces (satoru)
    void AppTile(int app_id, int x, int y, int size);                 // MDraw::AppTile, app_id = AppIcons::Id (satoru)
    void Menu(int x, int y, int w, int top, int item_h, const char* const* items, int n, int hover, float t);   // MDraw::Menu (satoru)
    void Toast(int x, int y, int w, int h, int kind, const char* title, const char* body, int alpha);   // MDraw::Toast (satoru)

    // text: y is the TOP of the line box (FontTTF::DrawText), returns the advance in pixels (satoru)
    int  Text(int x, int y, Face f, float px, const char* s, Color c);          // DrawText / DrawMono / DrawString (satoru)
    int  TextN(int x, int y, Face f, float px, const char* s, int n, Color c);  // first n bytes only (satoru)
    void TextCenter(int cx, int y, Face f, float px, const char* s, Color c);   // FontTTF::DrawStringCenter (satoru)
    int  TextFit(int x, int y, Face f, float px, const char* s, Color c, int maxw);   // ellipsised, SKit::TextFit (satoru)

    // integer twins for -mno-sse callers (satoru)
    int  TextI(int x, int y, Face f, int px, const char* s, Color c);
    void TextCenterI(int cx, int y, Face f, int px, const char* s, Color c);
    int  TextFitI(int x, int y, Face f, int px, const char* s, Color c, int maxw);
    void SoftShadowI(int x, int y, int w, int h, int r, int blur, int dy, int alpha);

    // backend plumbing, apps never touch it (satoru)
    struct Impl;                              // kernel: screen target + content origin; ring 3: canvas pixels (satoru)
    explicit Canvas(Impl* impl) : impl_(impl) {}
    Impl* impl() const { return impl_; }
private:
    Impl* impl_;                              // backend state for this frame (satoru)
};

// ---- windows (satoru)

struct WindowSpec {
    const char* title;                        // initial title, copied (satoru)
    const char* app_id;                       // dock and config identity, e.g. "calculator" (satoru)
    int   w, h;                               // initial CONTENT size (the kernel adds title bar + border) (satoru)
    int   min_w, min_h;                       // smallest content size (satoru)
    int   x, y;                               // placement hint, -1 lets the shell choose (satoru)
    bool  resizable;                          // resize edges and maximize (satoru)
    bool  closable;                           // the close button (satoru)
    Color bg;                                 // window body colour behind the content, 0 = theme panel (satoru)
    int   app_icon;                           // AppIcons::Id for title bar and dock, -1 = derive from title (satoru)
};

class AppHandler;

class Window {                                // one toplevel: a kernel Window or a wl_surface + xdg_toplevel (satoru)
public:
    void SetTitle(const char* title);         // WindowManager::SetTitle / xdg_toplevel.set_title (satoru)
    void Resize(int w, int h);                // content size; ring 3 commits a new buffer (satoru)
    void Move(int x, int y);                  // kernel only, a no-op on ring 3 (xdg has no move) (satoru)
    void SetMaximized(bool on);               // WindowManager::ToggleMaximize / xdg_toplevel.set_maximized (satoru)
    void Raise();                             // WindowManager::Focus; ring 3: no-op until xdg_activation (satoru)
    void Close();                             // close now, OnClose is NOT asked (satoru)
    void SetBackground(Color c);              // body colour behind the content (theme changes) (satoru)
    void SetFullscreen(bool on);              // a request: the wm animates it (kernel ToggleFullscreen, xdg set_fullscreen) (satoru)
    bool Fullscreen() const;
    Rect Frame() const;                       // outer position and size; ring 3 has no position (-1,-1,w,h) (satoru)
    bool Maximized() const;                   // the wm maximized state (satoru)
    bool Minimized() const;                   // the wm minimized state, ring 3 always false (satoru)
    Size ContentSize() const;                 // current drawable size (satoru)
    bool Focused() const;                     // keyboard focus (satoru)

    void Invalidate();                        // redraw everything on the next frame (satoru)
    void Invalidate(const Rect& r);           // redraw only r (wl_surface.damage_buffer on ring 3) (satoru)
    void RequestFrameAt(uint32_t ms);         // a frame once NowMs() >= ms (animations, caret blink) (satoru)
    void RequestAnimationFrame();             // a frame as soon as the compositor can take one (satoru)

    void SetBadge(int count);                 // dock badge; no-op until the kurono protocol extension lands (satoru)
    void Notify(int kind, const char* title, const char* body);   // shell toast; kind 0 info 1 ok 2 warn 3 error (satoru)

    AppHandler* handler;                      // the app object driving this window (satoru)
    void* user;                               // free for the app (satoru)
    struct Impl;                              // backend state (satoru)
    Impl* impl;                               // owned by the backend (satoru)
};

// ---- time and timers (satoru)

uint32_t NowMs();                             // monotonic ms: Timer::GetRealMs / clock_gettime(MONOTONIC) (satoru)
int   StartTimer(Window* w, uint32_t interval_ms, bool repeat);   // delivers EventType::Timer, returns id > 0 (satoru)
void  StopTimer(int timer_id);                // cancel, safe on a dead id (satoru)
float Ease(uint32_t t0, int delay_ms, int dur_ms);   // out-cubic 0..1 tween from t0, same curve as SKit::Ease (satoru)
int   EaseQ8(uint32_t t0, int delay_ms, int dur_ms); // the same tween as 0..256, for -mno-sse callers (satoru)

// ---- clipboard (satoru)

namespace Clipboard {
    bool        SetText(const char* utf8);    // kernel: ClipboardManager; ring 3: the compositor's data device (satoru)
    const char* GetText();                    // "" when empty, valid until the next call (satoru)
    bool        HasText();
    void        Clear();
    // primary selection: middle click. ring 3 uses zwp_primary_selection. (satoru)
    bool        SetPrimaryText(const char* utf8);
    const char* GetPrimaryText();
    bool        HasPrimaryText();
}

// ---- files: kvfs paths in both backends (satoru)
// ring 3 goes through the linux abi, which passes /home, /apps, /system and /boot through unchanged; (satoru)
// keep app data there (/etc, /tmp, /var and /usr are remapped under /system for linux programs). (satoru)

struct DirEntry { char name[64]; bool dir; uint32_t size; };

namespace Fs {
    int  ReadFile(const char* path, void* buf, int max);         // bytes read, -1 on error (satoru)
    bool WriteFile(const char* path, const void* data, int len); // create or replace (satoru)
    int  FileSize(const char* path);                              // -1 when missing (satoru)
    bool Exists(const char* path);
    bool IsDir(const char* path);
    bool Mkdirs(const char* path);
    bool Remove(const char* path);                                // a file or an empty directory (satoru)
    bool Rename(const char* from, const char* to);
    int  ListDir(const char* path, DirEntry* out, int max);       // entries filled, -1 on error (satoru)
}

// ---- launching registered apps and opening files (additive to v1) (satoru)
// every call is validated against the app registry and the path rules; neither backend can run (satoru)
// an arbitrary command. ring 3 asks the desktop over a small unix socket request. (satoru)

namespace Launch {
    bool App(const char* name, const char* arg = nullptr);   // launch a registered app, optional path (satoru)
    bool Open(const char* path);                              // open a file with its default app (satoru)
    bool RunScript(const char* path);                         // run a .kcl script in a terminal (satoru)
    bool RefreshDesktop();                                    // desktop icons changed under /home/user/Desktop (satoru)
}

// ---- audio: one or more streams of interleaved s16 pcm (satoru)
// kernel: the hda/ac97 audio server; ring 3: the pulse compatible server over its unix socket. (satoru)

namespace Audio {
    int  Open(int rate, int channels);        // stream id > 0, -1 when there is no audio (satoru)
    int  Write(int id, const int16_t* frames, int nframes);   // non-blocking, returns frames taken (satoru)
    int  Queued(int id);                      // frames buffered and not played yet (satoru)
    void Pause(int id, bool on);
    void SetVolume(int id, int pct);          // 0..100 (satoru)
    void Close(int id);
    const char* BackendName();                // "hda", "ac97", "none"; ring 3 "pulse" or "none" (satoru)
}

// ---- misc (satoru)

Size  ScreenSize(Window* w);                  // the output the window is on, nullptr = primary (satoru)
Rect  WorkArea(Window* w);                    // usable desktop rect minus the menu bar and the dock (satoru)

void* Alloc(size_t n);                        // KernelHeap / malloc; new and delete work too (satoru)
void  Free(void* p);
void  Log(const char* tag, const char* msg);  // serial log on the kernel, stderr on ring 3 (satoru)
bool  IsRing3();                              // which backend this build is (satoru)
// wake RunLoop when `fd` becomes readable (a pty master, a child pipe). ring 3 only: the kernel (satoru)
// backend is driven by the wm and returns false. false also when the watch table is full. (satoru)
bool  WatchFd(int fd, bool on);

// ---- the app (satoru)

class AppHandler {                            // what an app implements, one instance per window (satoru)
public:
    virtual ~AppHandler() {}                  // deleted by the backend after the window is gone (satoru)
    virtual void OnCreate(Window& w) { (void)w; }                    // window exists (satoru)
    virtual void OnFrame(Window& w, Canvas& c, uint32_t now_ms) = 0; // draw the content (all of it, or Damage()) (satoru)
    virtual bool OnEvent(Window& w, const Event& e) { (void)w; (void)e; return false; }   // true = redraw all (satoru)
    virtual void OnTick(Window& w, uint32_t now_ms) { (void)w; (void)now_ms; }   // each loop pass, keep it cheap (satoru)
    virtual bool OnClose(Window& w) { (void)w; return true; }        // false keeps it open where the backend can (satoru)
};

typedef AppHandler* (*AppFactory)();          // builds a handler, one per window (satoru)

// kernel backend: opens the window and returns at once; the window manager drives the callbacks. (satoru)
// ring 3: connects, opens the window, then sleeps in poll(display fd, next deadline) until the last (satoru)
// window closes, and returns the exit code. the same app code either way. (satoru)
int     Run(const WindowSpec& spec, AppFactory make);   // open the first window and run (satoru)
int     RunLoop();                            // run until every window is gone; for apps that opened their own (satoru)
Window* OpenWindow(const WindowSpec& spec, AppHandler* h);   // more windows from inside an app (satoru)
void    Quit(int code);                       // close every window of this app and leave Run (satoru)

}  // namespace kui (satoru)

// entry point glue, the only per-backend line in an app source (satoru)
#if defined(KUI_BACKEND_WAYLAND)
#define KUI_APP(name, spec, factory) \
    int main(int, char**) { return kui::Run((spec), (factory)); }
#else
#define KUI_APP(name, spec, factory) \
    int kui_launch_##name() { return kui::Run((spec), (factory)); }
#endif

// porting an app (the calculator is the worked example, src/apps/calculator.cpp): (satoru)
//   struct Calc : kui::AppHandler { (satoru)
//       void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now) override { c.Clear(kui::CurrentTheme().pal.panel); } (satoru)
//       bool OnEvent(kui::Window& w, const kui::Event& e) override { return e.type == kui::EventType::Click; } (satoru)
//   }; (satoru)
//   static kui::AppHandler* MakeCalc() { return new Calc; } (satoru)
//   static const kui::WindowSpec kSpec = { "Calculator", "calculator", 298, 433, 238, 320, -1, -1, true, true, 0, -1 }; (satoru)
//   KUI_APP(calculator, kSpec, MakeCalc) (satoru)

// end (satoru)
