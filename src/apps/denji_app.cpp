// kurono os - media / video player app (window glue around VideoPlayer)
// =====================================================================
// layout: the whole content area is the picture; a floating control bar fades
// in on mouse move and out after a couple of idle seconds while playing.
// every animation is a function of the real clock (kui::NowMs), never
// of how many frames the compositor happened to draw. (satoru)
#include "denji_app.h"
#include "../media/video_player.h"
#include "../kui/kui.h"   // window, canvas, events, files and time come from kui (satoru)
#include "media_text.h"
#include "../media/mplat.h"
// the demo clips and the page allocator are kernel side; ring 3 plays files from the vfs. (satoru)
#ifndef KUI_BACKEND_WAYLAND
#include "../media/embedded_media.h"
#include "../drivers/mouse.h"      // kernel only: drag safety net in HandleEvent (satoru)
#include "../ui/window_manager.h"   // kernel only: the usable desktop rectangle for FitWindowToVideo (satoru)
#include "../system/logging.h"
#include "../system/kasset.h"
#include "../kernel/pmm.h"
#endif

namespace DenjiApp {

namespace {

// ---- palette (matches the task manager / settings) ----
const uint32_t C_BG     = 0xFF1B1B1D;
const uint32_t C_PANEL  = 0xFF26262A;
const uint32_t C_HOVER  = 0xFF303036;
const uint32_t C_ACCENT = 0xFF3B82F6;
const uint32_t C_TEXT   = 0xFFE6E6EA;
const uint32_t C_DIM    = 0xFF8E8E96;
const uint32_t C_VIDEO  = 0xFF08080C;
const uint32_t C_WHITE  = 0xFFFFFFFF;

// ---- timing ----
const uint32_t kHideAfterMs  = 2600;   // controls fade out after this much idle time while playing
const uint32_t kFadeMs       = 170;    // full fade in or out
const uint32_t kFlashMs      = 460;    // centre play / pause pulse
const uint32_t kToastMs      = 1100;
const uint32_t kDoubleClickMs = 350;
const int32_t  kSeekStepMs   = 5000;
const int32_t  kSeekBigMs    = 10000;

enum Mode { M_NONE = 0, M_LIBRARY, M_PLAYER };
enum PendKind { P_NONE = 0, P_SAMPLE, P_FILE, P_BUFFER };
enum SrcKind { S_NONE = 0, S_SAMPLE0, S_SAMPLE1, S_FILE, S_BUFFER };
enum Drag { D_NONE = 0, D_SEEK, D_VOL };

struct Entry {
    char     name[44];
    char     path[112];   // empty for the demo clips
    int      sample;      // -1 for a file, else the demo clip index
    uint32_t size;
};

const int kMaxEntries = 24;

// ---- state (single instance app) ----
VideoPlayer::State g_st;
Mode  g_mode      = M_NONE;
kui::Window* g_w  = nullptr;    // the open window, null when closed (satoru)
kui::Canvas* g_c  = nullptr;    // the canvas of the frame being drawn (satoru)
bool  g_loaded    = false;      // g_st holds an opened video
char  g_title[64];
char  g_error[96];

PendKind g_pend_kind = P_NONE;
int      g_pend_sample = 0;
char     g_pend_path[128];
const uint8_t* g_pend_data = nullptr;
uint32_t g_pend_size = 0;
int      g_pend_defer = 0;

SrcKind  g_src = S_NONE;
uint8_t* g_file_buf = nullptr;  // owned copy of a vfs file, null otherwise
size_t   g_file_alloc = 0;      // bytes handed to PMM for it
bool     g_file_pmm = false;

// player ui
uint64_t g_last_act   = 0;      // last pointer / key activity
uint64_t g_last_paint = 0;
int      g_ctl_alpha  = 0;      // 0..255 current control opacity
int      g_mx = -1, g_my = -1;  // pointer, content coordinates
Drag     g_drag       = D_NONE;
bool     g_drag_resume = false;
uint64_t g_drag_seek_ms = 0;
uint64_t g_flash_t0   = 0;
int      g_flash_kind = 0;      // 0 play, 1 pause
char     g_toast[32];
uint64_t g_toast_t0   = 0;
uint64_t g_click_t0   = 0;
int      g_click_x = -100, g_click_y = -100;
int      g_volume = 100;        // survives reopen
uint64_t g_last_key_ms = 0;     // when kui last delivered a key press, so held-arrow repeat does not double up (satoru)
bool     g_muted  = false;

// what the draw functions need to know about the content area (origin is always 0,0 with a canvas) (satoru)
struct View { int content_x, content_y, content_w, content_h; };
inline kui::Canvas& C() { return *g_c; }
inline bool IsFull() { return g_w && g_w->Fullscreen(); }
inline void Dirty() { if (g_w) g_w->Invalidate(); }

// library
Entry g_ent[kMaxEntries];
int   g_ent_count = 0;
int   g_lib_scroll = 0;
int   g_lib_sel = 0;

// ---- small helpers ----
int Slen(const char* s) { int n = 0; while (s && s[n]) ++n; return n; }
void Scpy(char* d, const char* s, int max) {
    int i = 0; while (i + 1 < max && s && s[i]) { d[i] = s[i]; ++i; } d[i] = 0;
}
bool EndsWithNoCase(const char* s, const char* suf) {
    int sl = Slen(s), el = Slen(suf);
    if (el > sl) return false;
    for (int i = 0; i < el; i++) {
        char a = s[sl - el + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}
const char* Basename(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}
int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
uint64_t Now() {   // kui::NowMs widened so it does not wrap (satoru)
    static uint32_t last = 0;
    static uint64_t hi = 0;
    uint32_t n = kui::NowMs();
    if (n < last) hi += 1ull << 32;
    last = n;
    return hi | n;
}

int FmtTime(char* out, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    int n = 0;
    char tmp[12];
    auto put = [&](uint32_t v, int pad) {
        int k = 0;
        do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k < pad) tmp[k++] = '0';
        while (k) out[n++] = tmp[--k];
    };
    if (h) { put(h, 1); out[n++] = ':'; put(m, 2); }
    else   { put(m, 1); }
    out[n++] = ':';
    put(sec, 2);
    out[n] = 0;
    return n;
}

void FmtSize(char* out, int max, uint32_t bytes) {
    uint32_t tenth = (uint32_t)(((uint64_t)bytes * 10ull) / (1024ull * 1024ull));
    char tmp[24]; int n = 0;
    uint32_t whole = tenth / 10, frac = tenth % 10;
    char rev[12]; int rn = 0;
    do { rev[rn++] = (char)('0' + whole % 10); whole /= 10; } while (whole && rn < 10);
    while (rn) tmp[n++] = rev[--rn];
    tmp[n++] = '.'; tmp[n++] = (char)('0' + frac); tmp[n++] = ' '; tmp[n++] = 'M'; tmp[n++] = 'B'; tmp[n] = 0;
    Scpy(out, tmp, max);
}

// text goes through media_text (float font sizes can't cross from this
// no-sse unit). y is the top of the line. (satoru)
int TextW(const char* s, int px) { return MediaText::Width(s, px); }
void Text(int x, int y, const char* s, uint32_t c, int px = 14, bool bold = false) {
    MediaText::Draw(C(), x, y, s, c, px, bold);
}
int TextH(int px) { return MediaText::Height(px); }

// right-pointing triangle, 4x4 supersampled so the slanted edges are smooth.
// bounding box x,y,w,h; alpha 0..255. (satoru)
void PlayTri(int x, int y, int w, int h, uint32_t col, int a) {
    if (a <= 0 || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int cnt = 0;
            for (int sy = 0; sy < 4; sy++) {
                int v = 8 * j + 2 * sy + 1;
                int d = v - 4 * h; if (d < 0) d = -d;
                for (int sx = 0; sx < 4; sx++) {
                    int u = 8 * i + 2 * sx + 1;
                    if (u * h <= 2 * w * (4 * h - d)) cnt++;
                }
            }
            if (cnt) C().FillRectAlpha(x + i, y + j, 1, 1, (uint8_t)(a * cnt / 16), col);
        }
    }
}

void PauseIcon(int cx, int cy, int h, uint32_t col, int a) {
    int bw = h / 3 > 3 ? h / 3 : 3;
    int gap = bw;
    C().RoundRect(cx - gap / 2 - bw, cy - h / 2, bw, h, 1, col, a);
    C().RoundRect(cx + gap / 2 + (gap & 1), cy - h / 2, bw, h, 1, col, a);
}

// ---- memory ownership ----
void FreeFileBuffer() {
    if (!g_file_buf) return;
#ifndef KUI_BACKEND_WAYLAND
    if (g_file_pmm) PMM::FreeBytes(g_file_buf, g_file_alloc);
    else
#endif
    kui::Free(g_file_buf);
    g_file_buf = nullptr;
    g_file_alloc = 0;
    g_file_pmm = false;
}

// stop playback and give back everything the current video holds: the audio
// stream, decode buffers, the vfs copy, and the on-demand asset payload. (satoru)
void StopMedia() {
    VideoPlayer::Close(g_st);
    g_loaded = false;
    FreeFileBuffer();
#ifndef KUI_BACKEND_WAYLAND
    if (g_src == S_SAMPLE0) KAsset::Release("DENJI   KVD");
    if (g_src == S_SAMPLE1) KAsset::Release("SSSTIK  KVD");
#endif
    g_src = S_NONE;
    g_pend_kind = P_NONE;
    g_pend_data = nullptr;
    g_pend_size = 0;
    g_drag = D_NONE;
    g_error[0] = 0;
}

// ---- fullscreen ----
// a request: the window manager animates the change (and gives the title bar back on the way out). (satoru)
void SetFullscreen(bool on) {
    if (!g_w || g_w->Fullscreen() == on) return;
    g_w->SetFullscreen(on);
    Dirty();
}

// ---- toast / flash ----
void Toast(const char* s) {
    Scpy(g_toast, s, (int)sizeof(g_toast));
    g_toast_t0 = Now();
    Dirty();
}
void ToastVolume() {
    char t[32]; int n = 0;
    const char* p = g_muted ? "Muted" : "Volume ";
    while (*p) t[n++] = *p++;
    if (!g_muted) {
        int v = g_volume;
        char rev[4]; int rn = 0;
        do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (rn) t[n++] = rev[--rn];
        t[n++] = '%';
    }
    t[n] = 0;
    Toast(t);
}
void Flash(int kind) { g_flash_kind = kind; g_flash_t0 = Now(); }
void Touch() { g_last_act = Now(); Dirty(); }

// ---- layout of the control bar (content coordinates) ----
struct Lay {
    int bx, by, bw, bh;
    int play_cx, play_cy, play_r;
    int el_x, tot_x, time_w;
    int seek_x, seek_y, seek_w;
    int vol_icon_x, vol_x, vol_w;
    int lib_x, fs_x, btn;
    bool show_vol;
};

Lay MakeLay(int cw, int ch) {
    Lay l;
    l.bh = 52;
    l.bw = cw - 28;
    if (l.bw > 780) l.bw = 780;
    if (l.bw < 120) l.bw = cw - 8;
    l.bx = (cw - l.bw) / 2;
    int margin = ch >= 200 ? 14 : 4;
    l.by = ch - l.bh - margin;
    if (l.by < 2) l.by = 2;
    l.btn = 28;
    l.play_r = 18;
    l.play_cx = l.bx + 14 + l.play_r;
    l.play_cy = l.by + l.bh / 2;
    l.time_w = TextW("0:00:00", 13) + 2;
    l.el_x = l.play_cx + l.play_r + 12;
    l.seek_x = l.el_x + l.time_w + 8;
    l.fs_x = l.bx + l.bw - 14 - l.btn;
    l.lib_x = l.fs_x - 6 - l.btn;
    l.show_vol = l.bw >= 470;
    l.vol_w = l.show_vol ? 64 : 0;
    l.vol_x = l.lib_x - 10 - l.vol_w;
    l.vol_icon_x = (l.show_vol ? l.vol_x - 6 : l.lib_x - 6) - 22;
    l.tot_x = l.vol_icon_x - 8 - l.time_w;
    l.seek_w = l.tot_x - 8 - l.seek_x;
    if (l.seek_w < 20) l.seek_w = 20;
    l.seek_y = l.by + l.bh / 2;
    return l;
}

bool InRect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// ---- library ----
void AddEntry(const char* name, const char* path, int sample, uint32_t size) {
    if (g_ent_count >= kMaxEntries) return;
    Entry& e = g_ent[g_ent_count++];
    Scpy(e.name, name, (int)sizeof(e.name));
    Scpy(e.path, path ? path : "", (int)sizeof(e.path));
    e.sample = sample;
    e.size = size;
}

bool IsVideoName(const char* n) {
    return EndsWithNoCase(n, ".kvid") || EndsWithNoCase(n, ".kvd") ||
           EndsWithNoCase(n, ".mp4") || EndsWithNoCase(n, ".m4v") ||
           EndsWithNoCase(n, ".mov");
}

void ScanLibrary() {
    g_ent_count = 0;
#ifndef KUI_BACKEND_WAYLAND
    if (EmbeddedMedia::HasDenjiKVID())  AddEntry("Denji.kvid", "", 0, EmbeddedMedia::DenjiKVIDSize());
    if (EmbeddedMedia::HasSsstikKVID()) AddEntry("ssstik.kvid", "", 1, EmbeddedMedia::SsstikKVIDSize());
#endif
    static const char* dirs[] = { "/home/user/Videos", "/home/user/Documents", "/home/user/Downloads" };
    for (int d = 0; d < 3; d++) {
        static kui::DirEntry kids[48];
        int n = kui::Fs::ListDir(dirs[d], kids, 48);
        for (int i = 0; i < n; i++) {
            const kui::DirEntry* k = &kids[i];
            if (k->dir || !IsVideoName(k->name)) continue;
            char full[128]; int fn = 0;
            for (const char* p = dirs[d]; *p && fn < 100; ++p) full[fn++] = *p;
            full[fn++] = '/';
            for (const char* p = k->name; *p && fn < 126; ++p) full[fn++] = *p;
            full[fn] = 0;
            AddEntry(k->name, full, -1, k->size);
        }
    }
    if (g_lib_sel >= g_ent_count) g_lib_sel = 0;
    g_lib_scroll = 0;
}

// ---- window plumbing ----
// the window is a kui handler: frames and events go to the functions below, and when the window goes away
// however it was closed (title bar, task manager, resolution change) the player is torn down. (satoru)
bool OnFrameImpl(kui::Window& w, kui::Canvas& c, uint32_t now_ms);
bool HandleEvent(kui::Window& w, const kui::Event& e);
void WindowGone(kui::Window* who);

struct DenjiWindow : kui::AppHandler {
    kui::Window* self = nullptr;
    void OnCreate(kui::Window& w) override { self = &w; g_w = &w; }
    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now) override { OnFrameImpl(w, c, now); }
    bool OnEvent(kui::Window& w, const kui::Event& e) override { return HandleEvent(w, e); }
#if defined(KUI_BACKEND_WAYLAND)
    // the kernel gui loop calls BackgroundTick once per pass; ring 3 has no such loop. (satoru)
    void OnTick(kui::Window& w, uint32_t now) override { (void)w; (void)now; BackgroundTick(); }
#endif
    ~DenjiWindow() override { WindowGone(self); }
};

// w and h are the old total window size; kui wants the content size (no 1 px borders, no 36 px title bar) (satoru)
bool EnsureWindow(int w, int h, const char* title) {
    if (g_w) return true;
    kui::WindowSpec spec = { title, "media", w - 2, h - 37, 138, 63, -1, -1, true, true, 0, -1 };
    g_w = kui::OpenWindow(spec, new DenjiWindow);
    if (!g_w) {
        MPlat::Log("[media] CreateWindow failed\r\n");
        return false;
    }
    g_ctl_alpha = 255;
    g_last_act = Now();
    return true;
}

void SetWindowTitle(const char* t) {
    Scpy(g_title, t, (int)sizeof(g_title));
    if (g_w) g_w->SetTitle(g_title);
}

void BeginLoad(PendKind kind, const char* title) {
    if (!EnsureWindow(640, 460, title)) return;
    if (IsFull()) SetFullscreen(false);
    StopMedia();
    g_pend_kind = kind;
    g_pend_defer = 2;
    g_mode = M_PLAYER;
    g_ctl_alpha = 255;
    g_last_act = Now();
    SetWindowTitle(title);
    g_w->Raise();
    Dirty();
}

// fit the window to the picture (once, when a video is first shown). the old code kept the window's
// centre; kui has no position query, so it is centred on the screen (it opens centred anyway). (satoru)
void FitWindowToVideo() {
    if (!g_w || IsFull()) return;
    int vw = VideoPlayer::FrameWidth(g_st), vh = VideoPlayer::FrameHeight(g_st);
    if (vw <= 0 || vh <= 0) return;
    // the usable desktop (below the menu bar, above the dock): the sizing and centring maths want it, kui
    // only knows the screen. kernel side asks the window manager, ring 3 will fall back to the screen. (satoru)
    kui::Size sc = kui::ScreenSize(g_w);
    int top = 0, desk_h = sc.h;
#ifndef KUI_BACKEND_WAYLAND
    sc.w = WindowManager::GetScreenWidth();
    top = WindowManager::GetDesktopY();
    desk_h = WindowManager::GetDesktopH();
#endif
    int area_w = sc.w * 7 / 10;
    int area_h = desk_h * 8 / 10 - 36;
    if (area_w < 320) area_w = 320;
    if (area_h < 240) area_h = 240;
    int cw = area_w, chh = (int)((int64_t)cw * vh / vw);
    if (chh > area_h) { chh = area_h; cw = (int)((int64_t)chh * vw / vh); }
    if (cw < 420) cw = 420;           // keep room for the control bar
    if (chh < 240) chh = 240;
    g_w->Resize(cw, chh);
    // the old window (640x460, opened centred) kept its centre when it resized: same integer maths (satoru)
    int ox = (sc.w - 640) / 2, oy = top + (desk_h - 460) / 2;
    int nx = ox + 320 - (cw + 2) / 2, ny = oy + 230 - (chh + 37) / 2;
    if (nx < 0) nx = 0;
    if (ny < top) ny = top;
    g_w->Move(nx, ny);
}

// finish the deferred load: runs on the second painted frame so the window
// (with its loading card) is on screen while the payload is read. (satoru)
void FinishLoad() {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    bool ok = true;
    if (g_pend_kind == P_SAMPLE) {
#ifndef KUI_BACKEND_WAYLAND
        if (g_pend_sample == 0) {
            data = EmbeddedMedia::DenjiKVIDData();
            size = EmbeddedMedia::DenjiKVIDSize();
            g_src = S_SAMPLE0;
        } else {
            data = EmbeddedMedia::SsstikKVIDData();
            size = EmbeddedMedia::SsstikKVIDSize();
            g_src = S_SAMPLE1;
        }
        if (!data || size < 16) { Scpy(g_error, "This clip is not on the KURONO partition.", (int)sizeof(g_error)); ok = false; }
#else
        Scpy(g_error, "This clip is not on the KURONO partition.", (int)sizeof(g_error));
        ok = false;
#endif
    } else if (g_pend_kind == P_FILE) {
        int sz = kui::Fs::FileSize(g_pend_path);
        if (sz <= 16) { Scpy(g_error, "The file is empty or unreadable.", (int)sizeof(g_error)); ok = false; }
        else {
#ifndef KUI_BACKEND_WAYLAND
            size_t alloc = ((size_t)sz + 4095) & ~(size_t)4095;
            uint8_t* buf = (uint8_t*)PMM::AllocBytes(alloc);
#else
            size_t alloc = (size_t)sz;
            uint8_t* buf = (uint8_t*)kui::Alloc(alloc);
#endif
            if (!buf) { Scpy(g_error, "Not enough memory to open this file.", (int)sizeof(g_error)); ok = false; }
            else {
                int got = kui::Fs::ReadFile(g_pend_path, buf, sz);
                bool kvid = got >= 16 && buf[0] == 'K' && buf[1] == 'V' && buf[2] == 'I' && buf[3] == 'D';
                bool mp4 = got >= 16 && buf[4] == 'f' && buf[5] == 't' && buf[6] == 'y' && buf[7] == 'p';
                if (!kvid && !mp4) {
#ifndef KUI_BACKEND_WAYLAND
                    PMM::FreeBytes(buf, alloc);
#else
                    kui::Free(buf);
#endif
                    Scpy(g_error, "This is not a kvid or mp4 video.", (int)sizeof(g_error));
                    ok = false;
                } else {
                    g_file_buf = buf; g_file_alloc = alloc;
#ifndef KUI_BACKEND_WAYLAND
                    g_file_pmm = true;
#endif
                    data = buf; size = (uint32_t)got;
                    g_src = S_FILE;
                }
            }
        }
    } else if (g_pend_kind == P_BUFFER) {
        data = g_pend_data; size = g_pend_size;
        g_src = S_BUFFER;
        if (!data || size < 16) { Scpy(g_error, "Nothing to play.", (int)sizeof(g_error)); ok = false; }
    } else {
        return;
    }
    g_pend_kind = P_NONE;
    if (ok && !VideoPlayer::Open(data, size, g_st)) {
        Scpy(g_error, "Unrecognised video format.", (int)sizeof(g_error));
        ok = false;
    }
    if (!ok) {
        MPlat::Log("[media] open failed: ");
        MPlat::Log(g_error);
        MPlat::Log("\r\n");
        FreeFileBuffer();
#ifndef KUI_BACKEND_WAYLAND
        if (g_src == S_SAMPLE0) KAsset::Release("DENJI   KVD");
        if (g_src == S_SAMPLE1) KAsset::Release("SSSTIK  KVD");
#endif
        g_src = S_NONE;
        return;
    }
    g_loaded = true;
    VideoPlayer::SetVolume(g_st, g_volume);
    VideoPlayer::SetMuted(g_st, g_muted);
    FitWindowToVideo();
    VideoPlayer::Play(g_st);
    g_last_act = Now();
#ifndef KUI_BACKEND_WAYLAND
    RuntimeLog::LogAppEvent("media", "open", g_title);
#endif
    MPlat::Log("[media] playing ");
    MPlat::Log(g_title);
    MPlat::Log(" dur_ms=");
    MPlat::LogDec((int)VideoPlayer::DurationMs(g_st));
    MPlat::Log("\r\n");
}

// full teardown. window_gone: the wm already retired / is retiring the window. (satoru)
void Teardown(bool window_gone) {
    if (g_w && !window_gone) { kui::Window* w = g_w; g_w = nullptr; w->Close(); }
    g_w = nullptr;
    StopMedia();
    g_mode = M_NONE;
    g_ctl_alpha = 0;
    g_flash_t0 = 0;
    g_toast_t0 = 0;
}

void WindowGone(kui::Window* who) {
    if (g_w != who && g_w != nullptr) return;     // an older window must not tear down a newer one (satoru)
    g_w = nullptr;
    Teardown(true);
}

// ---- transport helpers for input ----
void TogglePlay() {
    if (!g_loaded) return;
    bool was_playing = VideoPlayer::IsPlaying(g_st);
    VideoPlayer::TogglePause(g_st);
    Flash(was_playing ? 1 : 0);
}

void SeekBy(int32_t delta) {
    if (!g_loaded) return;
    VideoPlayer::SeekRelativeMs(g_st, delta);
    char t[16]; int n = 0;
    t[n++] = delta < 0 ? '-' : '+';
    int s = (delta < 0 ? -delta : delta) / 1000;
    char rev[6]; int rn = 0;
    do { rev[rn++] = (char)('0' + s % 10); s /= 10; } while (s);
    while (rn) t[n++] = rev[--rn];
    t[n++] = 's'; t[n] = 0;
    Toast(t);
}

void ChangeVolume(int delta) {
    g_volume = Clampi(g_volume + delta, 0, 100);
    if (delta > 0) g_muted = false;
    if (g_loaded) { VideoPlayer::SetVolume(g_st, g_volume); VideoPlayer::SetMuted(g_st, g_muted); }
    ToastVolume();
}

void ToggleMute() {
    g_muted = !g_muted;
    if (g_loaded) VideoPlayer::SetMuted(g_st, g_muted);
    ToastVolume();
}

void GoLibrary() {
    if (IsFull()) SetFullscreen(false);
    StopMedia();
    ScanLibrary();
    g_mode = M_LIBRARY;
    SetWindowTitle("Media");
    Dirty();
}

// ---- drawing: library ----
void DrawLibrary(const View* w) {
    int x = w->content_x, y = w->content_y, cw = w->content_w, ch = w->content_h;
    C().FillRect(x, y, cw, ch, C_BG);
    Text(x + 24, y + 20, "Media", C_TEXT, 22, true);
    Text(x + 24, y + 50, "Pick a video to play", C_DIM, 13);

    int row_h = 58, top = y + 82;
    int visible = (ch - 82 - 12) / row_h;
    if (visible < 1) visible = 1;
    if (g_ent_count == 0) {
        C().RoundRect(x + 24, top, cw - 48, 96, 12, C_PANEL, 255);
        Text(x + 44, top + 22, "No videos found", C_TEXT, 15, true);
        Text(x + 44, top + 48, "Put .kvid files in /home/user/Videos", C_DIM, 13);
        return;
    }
    if (g_lib_scroll > g_ent_count - visible) g_lib_scroll = g_ent_count - visible;
    if (g_lib_scroll < 0) g_lib_scroll = 0;
    C().PushClip(x, top - 4, cw, ch - 82 + 4);
    for (int i = 0; i < visible && g_lib_scroll + i < g_ent_count; i++) {
        int idx = g_lib_scroll + i;
        const Entry& e = g_ent[idx];
        int ry = top + i * row_h;
        bool hover = InRect(g_mx, g_my, 16, ry - y, cw - 32, row_h - 6);
        bool sel = idx == g_lib_sel;
        C().RoundRect(x + 16, ry, cw - 32, row_h - 6, 12,
                         (hover || sel) ? C_HOVER : C_PANEL, 255);
        if (sel) C().RoundOutline(x + 16, ry, cw - 32, row_h - 6, 12, C_ACCENT, 200);
        C().RoundRect(x + 28, ry + 8, 36, 36, 10, 0xFF4F8FFF, 0xFF2F5FD0, 255);
        PlayTri(x + 40, ry + 18, 13, 16, C_WHITE, 255);
        Text(x + 76, ry + 9, e.name, C_TEXT, 14, true);
        char meta[64]; int mn = 0;
        const char* kind = e.sample >= 0 ? "Demo clip   " : "File   ";
        while (*kind) meta[mn++] = *kind++;
        char sz[16]; FmtSize(sz, sizeof(sz), e.size);
        for (const char* p = sz; *p; ++p) meta[mn++] = *p;
        meta[mn] = 0;
        Text(x + 76, ry + 29, meta, C_DIM, 12);
    }
    C().PopClip();
}

// ---- drawing: player overlays ----
void DrawSeekAndTimes(int ox, int oy, const Lay& l, int a) {
    uint32_t pos = VideoPlayer::PositionMs(g_st), dur = VideoPlayer::DurationMs(g_st);
    char t1[16], t2[16];
    FmtTime(t1, pos); FmtTime(t2, dur);
    int th = TextH(13);
    int cy = oy + l.play_cy;
    Text(ox + l.el_x, cy - th / 2, t1, C_TEXT, 13);
    Text(ox + l.tot_x + l.time_w - TextW(t2, 13), cy - th / 2, t2, C_DIM, 13);

    bool hot = g_drag == D_SEEK || InRect(g_mx, g_my, l.seek_x - 6, l.seek_y - 12, l.seek_w + 12, 24);
    int th_bar = hot ? 6 : 4;
    int sy = cy - th_bar / 2;
    C().RoundRect(ox + l.seek_x, sy, l.seek_w, th_bar, th_bar / 2, 0xFF55555C, 255);
    int fill = dur ? (int)(((uint64_t)l.seek_w * pos) / dur) : 0;
    if (fill > 0) C().RoundRect(ox + l.seek_x, sy, fill < th_bar ? th_bar : fill, th_bar, th_bar / 2, C_ACCENT, 255);
    if (hot) C().Circle(ox + l.seek_x + fill, cy, 7, C_WHITE, 255);

    if (hot && g_mx >= 0 && a > 200) {
        int rel = Clampi(g_mx - l.seek_x, 0, l.seek_w);
        char tt[16];
        FmtTime(tt, (uint32_t)(((uint64_t)dur * rel) / (l.seek_w ? l.seek_w : 1)));
        int tw = TextW(tt, 12) + 16;
        int tx = Clampi(ox + l.seek_x + rel - tw / 2, ox + 4, ox + l.bx + l.bw - tw);
        C().RoundRect(tx, oy + l.by - 30, tw, 22, 8, 0xFF0E0E10, 235);
        Text(tx + 8, oy + l.by - 30 + (22 - TextH(12)) / 2, tt, C_TEXT, 12);
    }
}

int ISqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

void DrawVolumeIcon(int x, int cy, int a, bool muted, int vol) {
    uint32_t c = muted ? C_DIM : C_TEXT;
    C().FillRectAlpha(x, cy - 3, 4, 6, (uint8_t)a, c);          // speaker body
    for (int k = 0; k < 6; k++) {                                       // flared cone
        int half = 3 + k;
        C().FillRectAlpha(x + 4 + k, cy - half, 1, 2 * half, (uint8_t)a, c);
    }
    if (muted || vol == 0) {
        C().DrawLine(x + 13, cy - 4, x + 20, cy + 4, 0xFFE5484D, 2);
        C().DrawLine(x + 20, cy - 4, x + 13, cy + 4, 0xFFE5484D, 2);
    } else {
        int arcs = vol > 66 ? 3 : (vol > 33 ? 2 : 1);
        for (int k = 1; k <= arcs; k++) {                               // sound waves as arcs
            int r = 3 + 3 * k;
            int lim = r * 3 / 5;
            for (int dy = -lim; dy <= lim; dy++) {
                int dx = ISqrt(r * r - dy * dy);
                C().FillRectAlpha(x + 9 + dx, cy + dy, 2, 1, (uint8_t)a, c);
            }
        }
    }
}

void DrawFullscreenIcon(int x, int cy, bool full, int a) {
    int s = 14, y = cy - s / 2, t = 5;
    // top-left, top-right, bottom-left, bottom-right brackets, pointing out
    // when entering fullscreen and in when leaving it. (satoru)
    int o = full ? 3 : 0;
    C().FillRectAlpha(x + o,         y + o,         t, 2, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + o,         y + o,         2, t, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + s - t - o, y + o,         t, 2, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + s - 2 - o, y + o,         2, t, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + o,         y + s - 2 - o, t, 2, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + o,         y + s - t - o, 2, t, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + s - t - o, y + s - 2 - o, t, 2, (uint8_t)a, C_TEXT);
    C().FillRectAlpha(x + s - 2 - o, y + s - t - o, 2, t, (uint8_t)a, C_TEXT);
}

void DrawListIcon(int x, int cy, int a) {
    for (int i = 0; i < 3; i++) {
        C().FillRectAlpha(x, cy - 6 + i * 5, 3, 2, (uint8_t)a, C_TEXT);
        C().FillRectAlpha(x + 6, cy - 6 + i * 5, 10, 2, (uint8_t)a, C_TEXT);
    }
}

void DrawControls(const View* w, int a) {
    if (a <= 0) return;
    int ox = w->content_x;
    int oy = w->content_y + (255 - a) * 10 / 255;      // slides up while it fades in
    Lay l = MakeLay(w->content_w, w->content_h);
    int by = oy + l.by;

    // soft scrim behind the bar keeps white text readable on bright frames (satoru)
    int scrim_h = l.bh + 56;
    int scrim_y = w->content_y + w->content_h - scrim_h;
    for (int k = 0; k < 8; k++) {
        int y0 = scrim_y + k * scrim_h / 8, y1 = scrim_y + (k + 1) * scrim_h / 8;
        C().FillRectAlpha(ox, y0, w->content_w, y1 - y0, (uint8_t)(a * (k + 1) * 11 / 255), 0xFF000000);
    }
    C().Shadow(ox + l.bx, by, l.bw, l.bh, 16, 14, 4, a * 110 / 255);
    C().RoundRect(ox + l.bx, by, l.bw, l.bh, 16, 0xFF26262A, 0xFF1B1B1D, a * 235 / 255);
    C().RoundOutline(ox + l.bx, by, l.bw, l.bh, 16, 0xFFFFFFFF, a * 28 / 255);
    if (a < 110) return;             // text and glyphs appear once the plate is mostly there

    int cy = oy + l.play_cy;
    bool playing = VideoPlayer::IsPlaying(g_st);
    bool hov = InRect(g_mx, g_my, l.play_cx - l.play_r, l.play_cy - l.play_r, l.play_r * 2, l.play_r * 2);
    C().Circle(ox + l.play_cx, cy, l.play_r, hov ? 0xFF5B97FF : C_ACCENT, 255);
    if (playing) PauseIcon(ox + l.play_cx, cy, 14, C_WHITE, 255);
    else PlayTri(ox + l.play_cx - 5, cy - 8, 14, 16, C_WHITE, 255);

    DrawSeekAndTimes(ox, oy, l, a);

    if (l.show_vol) {
        bool hv = g_drag == D_VOL || InRect(g_mx, g_my, l.vol_x - 6, l.play_cy - 12, l.vol_w + 12, 24);
        int tb = hv ? 6 : 4;
        C().RoundRect(ox + l.vol_x, cy - tb / 2, l.vol_w, tb, tb / 2, 0xFF55555C, 255);
        int vf = g_muted ? 0 : l.vol_w * g_volume / 100;
        if (vf > 0) C().RoundRect(ox + l.vol_x, cy - tb / 2, vf < tb ? tb : vf, tb, tb / 2, C_TEXT, 255);
        if (hv) C().Circle(ox + l.vol_x + vf, cy, 6, C_WHITE, 255);
    }
    DrawVolumeIcon(ox + l.vol_icon_x, cy, 255, g_muted, g_volume);
    DrawListIcon(ox + l.lib_x + 6, cy, 255);
    DrawFullscreenIcon(ox + l.fs_x + 7, cy, IsFull(), 255);
}

void DrawFlash(const View* w) {
    if (!g_flash_t0) return;
    uint64_t dt = Now() - g_flash_t0;
    if (dt >= kFlashMs) { g_flash_t0 = 0; return; }
    int t = (int)(dt * 256 / kFlashMs);              // 0..255
    int cx = w->content_x + w->content_w / 2, cy = w->content_y + w->content_h / 2;
    int r = 30 + t * 24 / 256;
    int a = (256 - t) * 150 / 256;
    C().Circle(cx, cy, r, 0xFF000000, a);
    int ia = (256 - t) * 255 / 256;
    if (g_flash_kind == 1) PauseIcon(cx, cy, 22, C_WHITE, ia);
    else PlayTri(cx - 8, cy - 12, 22, 24, C_WHITE, ia);
}

void DrawToast(const View* w) {
    if (!g_toast_t0) return;
    uint64_t dt = Now() - g_toast_t0;
    if (dt >= kToastMs) { g_toast_t0 = 0; return; }
    int a = 235;
    if (dt > kToastMs - 250) a = (int)(235 * (kToastMs - dt) / 250);
    int tw = TextW(g_toast, 14) + 28;
    int x = w->content_x + (w->content_w - tw) / 2;
    int y = w->content_y + 18;
    C().RoundRect(x, y, tw, 32, 16, 0xFF0E0E10, a);
    if (a > 120) Text(x + 14, y + (32 - TextH(14)) / 2, g_toast, C_TEXT, 14);
}

void DrawCenterCard(const View* w, const char* l1, const char* l2, bool spinner) {
    int x = w->content_x, y = w->content_y, cw = w->content_w, ch = w->content_h;
    C().FillRect(x, y, cw, ch, C_VIDEO);
    int pw = cw - 48 < 360 ? cw - 48 : 360;
    if (pw < 120) pw = 120;
    int ph = 100;
    int px = x + (cw - pw) / 2, py = y + (ch - ph) / 2;
    C().RoundRect(px, py, pw, ph, 14, C_PANEL, 255);
    Text(px + 22, py + 22, l1, C_TEXT, 15, true);
    if (l2) Text(px + 22, py + 50, l2, C_DIM, 13);
    if (spinner) {
        // three dots chasing each other, driven by the clock (satoru)
        int ph3 = (int)((Now() / 180) % 3);
        for (int i = 0; i < 3; i++)
            C().Circle(px + 26 + i * 16, py + ph - 18, 4, i == ph3 ? C_ACCENT : 0xFF55555C, 255);
    }
}

// target opacity of the control bar right now (satoru)
int ControlsTarget() {
    if (!g_loaded) return 255;
    if (!VideoPlayer::IsPlaying(g_st)) return 255;
    if (g_drag != D_NONE) return 255;
    return (Now() - g_last_act) < kHideAfterMs ? 255 : 0;
}

void DrawPlayer(const View* w) {
    int x = w->content_x, y = w->content_y, cw = w->content_w, ch = w->content_h;
    uint64_t now = Now();
    uint64_t dt = g_last_paint ? now - g_last_paint : 0;
    g_last_paint = now;
    if (dt > 120) dt = 120;

    if (g_pend_kind != P_NONE) {
        DrawCenterCard(w, "Loading...", g_title, true);
        if (g_pend_defer > 0) { g_pend_defer--; Dirty(); return; }
        FinishLoad();
        Dirty();
        return;
    }
    if (!g_loaded) {
        DrawCenterCard(w, g_error[0] ? "Can't play this video" : "Nothing to play",
                       g_error[0] ? g_error : "Press the list button for the library.", false);
        return;
    }

    VideoPlayer::PumpDecode(g_st);
    VideoPlayer::DrawVideo(g_st, C(), x, y, cw, ch);

    // fade the control bar toward its target at a fixed speed per real ms (satoru)
    int target = ControlsTarget();
    int step = (int)(dt * 255 / kFadeMs);
    if (step < 1) step = 1;
    if (g_ctl_alpha < target) g_ctl_alpha = g_ctl_alpha + step > target ? target : g_ctl_alpha + step;
    else if (g_ctl_alpha > target) g_ctl_alpha = g_ctl_alpha - step < target ? target : g_ctl_alpha - step;

    if (VideoPlayer::IsEnded(g_st)) {
        int cx = x + cw / 2, cy = y + ch / 2 - 14;
        C().Circle(cx, cy, 34, 0xFF000000, 170);
        C().RoundOutline(cx - 34, cy - 34, 68, 68, 34, C_WHITE, 120);
        PlayTri(cx - 9, cy - 13, 24, 26, C_WHITE, 255);
        const char* rp = "Replay";
        Text(cx - TextW(rp, 14) / 2, cy + 44, rp, C_TEXT, 14, true);
    }
    DrawFlash(w);
    if (IsFull() || g_ctl_alpha > 0) {
        // title strip while the controls are up (the window has no titlebar in fullscreen)
        if (IsFull() && g_ctl_alpha > 110) {
            C().FillRectAlpha(x, y, cw, 52, (uint8_t)(g_ctl_alpha * 110 / 255), 0xFF000000);
            Text(x + 20, y + 16, g_title, C_TEXT, 16, true);
        }
    }
    DrawControls(w, g_ctl_alpha);
    DrawToast(w);
}

bool OnFrameImpl(kui::Window& win, kui::Canvas& c, uint32_t now_ms) {
    (void)now_ms;
    g_c = &c;
    View v = { 0, 0, c.Width(), c.Height() };
    const View* w = &v;
    if (g_mode == M_LIBRARY) DrawLibrary(w);
    else if (g_mode == M_PLAYER) DrawPlayer(w);
    else C().FillRect(w->content_x, w->content_y, w->content_w, w->content_h, C_BG);
    g_c = nullptr;
    (void)win;
    return true;
}

// ---- input ----
void StartSeekDrag(const Lay& l, int px) {
    g_drag = D_SEEK;
    g_drag_resume = VideoPlayer::IsPlaying(g_st);
    if (g_drag_resume) VideoPlayer::Pause(g_st);
    uint32_t dur = VideoPlayer::DurationMs(g_st);
    int rel = Clampi(px - l.seek_x, 0, l.seek_w);
    VideoPlayer::SeekMs(g_st, (uint32_t)(((uint64_t)dur * rel) / (l.seek_w ? l.seek_w : 1)));
    g_drag_seek_ms = Now();
}

void UpdateDrag(const View* w, int px) {
    Lay l = MakeLay(w->content_w, w->content_h);
    if (g_drag == D_SEEK) {
        uint64_t now = Now();
        if (now - g_drag_seek_ms < 30) return;       // cap decode work while scrubbing
        g_drag_seek_ms = now;
        uint32_t dur = VideoPlayer::DurationMs(g_st);
        int rel = Clampi(px - l.seek_x, 0, l.seek_w);
        VideoPlayer::SeekMs(g_st, (uint32_t)(((uint64_t)dur * rel) / (l.seek_w ? l.seek_w : 1)));
    } else if (g_drag == D_VOL) {
        int rel = Clampi(px - l.vol_x, 0, l.vol_w);
        g_volume = l.vol_w ? rel * 100 / l.vol_w : 0;
        g_muted = false;
        VideoPlayer::SetVolume(g_st, g_volume);
        VideoPlayer::SetMuted(g_st, false);
    }
}

void EndDrag(const View* w) {
    if (g_drag == D_NONE) return;
    if (g_drag == D_SEEK && g_loaded) {
        // land exactly where the pointer is, then resume if it was playing
        UpdateDrag(w, g_mx);
        g_drag_seek_ms = 0;
        Lay l = MakeLay(w->content_w, w->content_h);
        uint32_t dur = VideoPlayer::DurationMs(g_st);
        int rel = Clampi(g_mx - l.seek_x, 0, l.seek_w);
        VideoPlayer::SeekMs(g_st, (uint32_t)(((uint64_t)dur * rel) / (l.seek_w ? l.seek_w : 1)));
        if (g_drag_resume) VideoPlayer::Play(g_st);
    }
    g_drag = D_NONE;
    Touch();
}

void LibraryClick(const View* w, int lx, int ly) {
    int row_h = 58, top = 82;
    if (ly < top) return;
    int idx = g_lib_scroll + (ly - top) / row_h;
    if (idx < 0 || idx >= g_ent_count) return;
    (void)w; (void)lx;
    g_lib_sel = idx;
    const Entry& e = g_ent[idx];
    if (e.sample >= 0) OpenSample(e.sample);
    else OpenFile(e.path);
}

void PlayerClick(const View* w, int lx, int ly) {
    if (!g_loaded) return;
    Lay l = MakeLay(w->content_w, w->content_h);
    bool bar_up = g_ctl_alpha > 110;
    if (bar_up && InRect(lx, ly, l.bx, l.by, l.bw, l.bh)) {
        int dxp = lx - l.play_cx, dyp = ly - l.play_cy;
        if (dxp * dxp + dyp * dyp <= (l.play_r + 4) * (l.play_r + 4)) { TogglePlay(); return; }
        if (InRect(lx, ly, l.seek_x - 6, l.seek_y - 14, l.seek_w + 12, 28)) { StartSeekDrag(l, lx); return; }
        if (l.show_vol && InRect(lx, ly, l.vol_x - 6, l.play_cy - 14, l.vol_w + 12, 28)) {
            g_drag = D_VOL; UpdateDrag(w, lx); return;
        }
        if (InRect(lx, ly, l.vol_icon_x - 4, l.play_cy - 14, 30, 28)) { ToggleMute(); return; }
        if (InRect(lx, ly, l.lib_x, l.play_cy - 14, l.btn, 28)) { GoLibrary(); return; }
        if (InRect(lx, ly, l.fs_x - 2, l.play_cy - 14, l.btn + 4, 28)) { SetFullscreen(!IsFull()); return; }
        return;                                       // plate background: swallow the click
    }
    // the picture: click toggles play, double click toggles fullscreen
    uint64_t now = Now();
    bool dbl = (now - g_click_t0) < kDoubleClickMs &&
               (lx - g_click_x) * (lx - g_click_x) + (ly - g_click_y) * (ly - g_click_y) < 100;
    g_click_t0 = now; g_click_x = lx; g_click_y = ly;
    if (dbl) {
        TogglePlay();                                 // undo the first click's toggle
        g_click_t0 = 0;
        SetFullscreen(!IsFull());
        return;
    }
    TogglePlay();
}

// one key. arrows, home and end come through kui as the same 0x90.. codes the old char queue never
// carried, so the key-state polling in the tick is gone. (satoru)
void KeyPressed(const View* w, int k) {
    if (g_mode == M_LIBRARY) {
        if (k == 0x90 && g_lib_sel > 0) { g_lib_sel--; if (g_lib_sel < g_lib_scroll) g_lib_scroll = g_lib_sel; }
        if (k == 0x91 && g_lib_sel + 1 < g_ent_count) {
            g_lib_sel++;
            int visible = (w->content_h - 94) / 58;
            if (visible < 1) visible = 1;
            if (g_lib_sel >= g_lib_scroll + visible) g_lib_scroll = g_lib_sel - visible + 1;
        }
        if ((k == '\n' || k == '\r') && g_lib_sel < g_ent_count) {
            const Entry& e = g_ent[g_lib_sel];
            if (e.sample >= 0) OpenSample(e.sample); else OpenFile(e.path);
        }
        Dirty();
        return;
    }
    if (g_mode != M_PLAYER) return;
    Touch();
    if (!g_loaded) {
        if (k == 27 && IsFull()) SetFullscreen(false);
        if (k == 'b' || k == 'B' || k == 8 || k == 127) GoLibrary();   // leave the error card (satoru)
        return;
    }
    if (k == ' ' || k == 'k' || k == 'K') TogglePlay();
    else if (k == 0x92) SeekBy(-kSeekStepMs);
    else if (k == 0x93) SeekBy(kSeekStepMs);
    else if (k == 'j' || k == 'J') SeekBy(-kSeekBigMs);
    else if (k == 'l' || k == 'L') SeekBy(kSeekBigMs);
    else if (k == 0x90) ChangeVolume(5);
    else if (k == 0x91) ChangeVolume(-5);
    else if (k == 'm' || k == 'M') ToggleMute();
    else if (k == 'f' || k == 'F') SetFullscreen(!IsFull());
    else if (k == 27) { if (IsFull()) SetFullscreen(false); }
    else if (k == 'r' || k == 'R' || k == 0x94) { VideoPlayer::SeekMs(g_st, 0); Toast("Restart"); }
    else if (k == 0x95) VideoPlayer::SeekMs(g_st, VideoPlayer::DurationMs(g_st));
    else if (k == 'b' || k == 'B' || k == 8 || k == 127) GoLibrary();
    else if (k >= '0' && k <= '9') {
        uint32_t dur = VideoPlayer::DurationMs(g_st);
        VideoPlayer::SeekMs(g_st, (uint32_t)((uint64_t)dur * (uint32_t)(k - '0') / 10));
    }
}

bool HandleEvent(kui::Window& win, const kui::Event& e) {
    kui::Size cs = win.ContentSize();
    View v = { 0, 0, cs.w, cs.h };
    const View* w = &v;
    int a = e.x, b = e.y;
    switch (e.type) {
    case kui::EventType::PointerMove:
        if (g_mode == M_LIBRARY) { g_mx = a; g_my = b; Dirty(); return true; }
        if (g_mode != M_PLAYER) return false;
        {
            bool moved = a != g_mx || b != g_my;
            g_mx = a; g_my = b;
            if (moved) Touch();
            if (g_drag != D_NONE) {
#ifndef KUI_BACKEND_WAYLAND
                // release-before-Click ordering of a very fast click: trust the button state (satoru)
                if (!Mouse::IsLeftDown()) EndDrag(w);
                else
#endif
                if (g_loaded) UpdateDrag(w, a);
            }
        }
        return true;
    case kui::EventType::PointerUp:
        if (g_mode == M_PLAYER) EndDrag(w);    // a release ends a seek or volume drag, wherever it happens
        return true;
    case kui::EventType::PointerLeave:
        g_mx = -1; g_my = -1;
        return true;
    case kui::EventType::Click:
        g_mx = a; g_my = b;
        if (g_mode == M_LIBRARY) { LibraryClick(w, a, b); return true; }
        if (g_mode != M_PLAYER) return false;
        Touch();
        PlayerClick(w, a, b);
        return true;
    case kui::EventType::Scroll: {
        int st = e.scroll_steps;       // whole notches, so this file never reads a float (satoru)
        if (g_mode == M_LIBRARY) { g_lib_scroll -= st; if (g_lib_scroll < 0) g_lib_scroll = 0; Dirty(); return true; }
        if (g_mode != M_PLAYER) return false;
        if (g_loaded) ChangeVolume(st > 0 ? 5 : -5);
        Touch();
        return true;
    }
    case kui::EventType::KeyDown: {
        int k = e.key;
        if (k >= 0xB0 && k <= 0xB3) return false;     // modifier presses on their own do nothing (satoru)
        g_last_key_ms = Now();
        KeyPressed(w, k & 0xFF);
        return true;
    }
    default:
        break;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------
// public api
// ---------------------------------------------------------------------
void Open() { OpenSample(0); }

void OpenSample(int which) {
#ifndef KUI_BACKEND_WAYLAND
    bool has = which == 1 ? EmbeddedMedia::HasSsstikKVID() : EmbeddedMedia::HasDenjiKVID();
#else
    bool has = false;
#endif
    if (!has) {
        MPlat::Log("[media] demo clip not on the KURONO partition\r\n");
        if (g_w) g_w->Raise();
        else OpenLibrary();
        return;
    }
    g_pend_sample = which == 1 ? 1 : 0;
    BeginLoad(P_SAMPLE, which == 1 ? "ssstik.kvid" : "Denji.kvid");
}

void OpenLibrary() {
    if (!EnsureWindow(560, 520, "Media")) return;
    if (g_mode == M_PLAYER && g_loaded) { g_w->Raise(); return; }
    if (IsFull()) SetFullscreen(false);
    StopMedia();
    ScanLibrary();
    g_mode = M_LIBRARY;
    SetWindowTitle("Media");
    g_w->Raise();
    Dirty();
}

void OpenBuffer(const uint8_t* data, uint32_t size, const char* title) {
    if (!data || size == 0) return;
    g_pend_data = data;
    g_pend_size = size;
    BeginLoad(P_BUFFER, title ? title : "Video");
    // BeginLoad -> StopMedia cleared the pending buffer, set it again
    g_pend_data = data;
    g_pend_size = size;
}

bool OpenFile(const char* path) {
    if (!path || !path[0]) return false;
    int sz = kui::Fs::FileSize(path);
    if (sz <= 16) return false;
    Scpy(g_pend_path, path, (int)sizeof(g_pend_path));
    BeginLoad(P_FILE, Basename(path));
    Scpy(g_pend_path, path, (int)sizeof(g_pend_path));
    return g_w != nullptr;
}

void Close() {
    if (g_w) Teardown(false);
}

bool IsOpen() {
    return g_w != nullptr;
}

bool NeedsFrames() {
    if (!g_w) return false;
    if (g_pend_kind != P_NONE) return true;
    if (g_mode != M_PLAYER || !g_loaded) return false;
    if (g_drag != D_NONE) return true;
    if (g_ctl_alpha != ControlsTarget()) return true;
    uint64_t now = Now();
    if (g_flash_t0 && now - g_flash_t0 < kFlashMs) return true;
    if (g_toast_t0 && now - g_toast_t0 < kToastMs) return true;
    return false;
}

void BackgroundTick() {
    if (!g_w) return;
    uint64_t now = Now();
    {
        // arrows repeat while held: the first press arrives as a kui key event, and a held arrow repeats
        // after 380 ms, then every 90 ms, unless the backend already sends its own repeats. (satoru)
        static const int kCodes[4] = { kui::K_UP, kui::K_DOWN, kui::K_LEFT, kui::K_RIGHT };
        static bool was[4];
        static uint64_t rep[4];
        bool focused = g_w->Focused();
        for (int i = 0; i < 4; i++) {
            bool d = focused && kui::KeyHeld(kCodes[i]);
            if (d && !was[i]) rep[i] = now + 380;
            else if (d && now >= rep[i] && now - g_last_key_ms > 150) {
                kui::Size cs = g_w->ContentSize();
                View v = { 0, 0, cs.w, cs.h };
                KeyPressed(&v, kCodes[i]);
                rep[i] = now + 90;
            }
            was[i] = d;
        }
    }
    if (g_mode != M_PLAYER || !g_loaded) return;

    VideoPlayer::PumpAudio(g_st);

    // reached the end while nobody was painting (minimized): settle the state (satoru)
    if (VideoPlayer::IsPlaying(g_st) &&
        VideoPlayer::PositionMs(g_st) >= VideoPlayer::DurationMs(g_st)) {
        VideoPlayer::PumpDecode(g_st);
        Dirty();
    }

    // ask for a frame exactly when the next video frame is due, and when the
    // controls should start fading. the gui loop's clock is Timer::GetRealMs
    // so convert the distance, not the absolute time. (satoru)
    if (VideoPlayer::IsPlaying(g_st)) {
        uint32_t pos = VideoPlayer::PositionMs(g_st);
        uint32_t cur = g_st.kvid_cur_frame;
        uint32_t wait = 8;
        if (g_st.kind == VideoPlayer::SRC_KVID && cur + 1 < g_st.kvid.hdr.frame_count) {
            uint64_t due_ms = g_st.kvid.index[cur + 1].dts_us / 1000ull;
            wait = due_ms > pos ? (uint32_t)(due_ms - pos) : 0;
        }
        if (wait < 1) wait = 1;
        if (wait > 40) wait = 40;
        g_w->RequestFrameAt(kui::NowMs() + wait);
        if (g_ctl_alpha > 0) {
            uint64_t hide_at = g_last_act + kHideAfterMs;
            if (hide_at > now) g_w->RequestFrameAt(kui::NowMs() + (uint32_t)(hide_at - now));
            else Dirty();
        }
    }
}

} // namespace DenjiApp

#if defined(KUI_BACKEND_WAYLAND)
// ring 3: open the library (or a file named on the command line) and run the kui loop. (satoru)
int main(int argc, char** argv) {
    if (argc > 1 && argv[1] && argv[1][0]) DenjiApp::OpenFile(argv[1]);
    else DenjiApp::OpenLibrary();
    return kui::RunLoop();
}
#else
// in-kernel fallback used by the app registry (kui_launch.cpp). (satoru)
int kui_launch_denji() { DenjiApp::OpenLibrary(); return 0; }
#endif

// end (satoru)
