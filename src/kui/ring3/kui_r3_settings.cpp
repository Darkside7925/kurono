// kui_r3_settings.cpp: the ring-3 side of Settings. it reuses the kernel's own shell,
// control kit and page sources unchanged and gives them two backends: drawing goes
// into the kui canvas, and every kernel read or write goes through the settings
// service over a unix socket. no kernel global is touched directly. (satoru)
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include "../kui_internal.h"
#include "../../system/settings_proto.h"
#include "../../system/ui_config.h"
#include "../../system/logging.h"
#include "../../system/user_mgmt.h"
#include "../../system/system_update.h"
#include "../../kernel/types.h"
#include "../../ui/night_light.h"   // r3 night light shims (satoru)
#include "../../kernel/heap.h"
#define private public   // the pmm counters are read-only inputs here, never changed (satoru)
#include "../../kernel/pmm.h"
#undef private
#include "../../kernel/taskmem.h"
#include "../../kernel/kmemx.h"
#include "../../drivers/graphics.h"
#include "../../drivers/timer.h"
#include "../../drivers/mouse.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/display_mgr.h"
#include "../../drivers/intel_modeset.h"
#include "../../drivers/outputs.h"
#include "../../drivers/cpu_detect.h"
#include "../../drivers/gpu_probe.h"
#include "../../drivers/intel_gpu.h"
#include "../../drivers/nvidia_gpu.h"
#include "../../drivers/amd_gpu.h"
#include "../../drivers/audio.h"
#include "../../drivers/audio_server.h"
#include "../../drivers/audio_backend.h"
#include "../../drivers/audio_mixer.h"
#include "../../drivers/hda.h"
#include "../../drivers/blockdev.h"
#include "../../drivers/usb.h"
#include "../../drivers/disklog.h"
#include "../../drivers/serial.h"
#include "../../net/network.h"
#include "../../net/dhcp.h"
#include "../../drivers/e1000.h"
#include "../../hal/cpufreq.h"
#include "../../ui/window_manager.h"
#include "../../ui/desktop.h"
#include "../../ui/style.h"
#include "../../ui/frame_pacer.h"
#include "../../ui/modern_draw.h"
#include "../../ui/ui_elements.h"
#include "../../ui/font.h"
#include "../../fs/kvfs.h"
#include "../../fs/persist.h"
#include "../../linux/kls.h"
#include "../../linux/linux_kernel.h"
#include "../../linux/linux_drivers.h"
#include "../../packages/pkgmgr.h"
#include "../../virt/vm_host.h"
#include "../../virt/vmm.h"
#include "../../virt/hypervisor.h"
#include "../../proc/scheduler.h"
#include "../../security/ken.h"
#include "../../apps/system_settings.h"
#include "../../apps/alpine_term.h"

namespace {

// ---- the config and info snapshots the service sent (satoru)
struct Ent { char key[KSS_KEY_MAX]; char val[KSS_VAL_MAX]; };
Ent g_cfg[KSS_ENTRY_MAX];  int g_cfg_n = 0;
Ent g_info[KSS_INFO_MAX];  int g_info_n = 0;
bool g_loaded = false;

const char* find_ent(const Ent* t, int n, const char* key) {
    for (int i = 0; i < n; i++) if (strcmp(t[i].key, key) == 0) return t[i].val;
    return nullptr;
}
const char* info_str(const char* key, const char* def) {
    const char* v = find_ent(g_info, g_info_n, key);
    return v ? v : def;
}
int info_int(const char* key, int def) {
    const char* v = find_ent(g_info, g_info_n, key);
    if (!v) return def;
    return (int)strtol(v, nullptr, 0);
}
long long info_ll(const char* key, long long def) {
    const char* v = find_ent(g_info, g_info_n, key);
    if (!v) return def;
    return (long long)strtoll(v, nullptr, 0);
}
const char* cfg_get(const char* key, const char* def) {
    const char* v = find_ent(g_cfg, g_cfg_n, key);
    return v ? v : def;
}
int cfg_int(const char* key, int def) {
    const char* v = find_ent(g_cfg, g_cfg_n, key);
    if (!v) return def;
    return (int)strtol(v, nullptr, 0);
}
bool cfg_bool(const char* key, bool def) {
    const char* v = find_ent(g_cfg, g_cfg_n, key);
    if (!v) return def;
    return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}
uint32_t cfg_color(const char* key, uint32_t def) {
    const char* v = find_ent(g_cfg, g_cfg_n, key);
    if (!v) return def;
    return (uint32_t)strtoul(v, nullptr, 0);
}
void cfg_put(const char* key, const char* val) {
    for (int i = 0; i < g_cfg_n; i++) {
        if (strcmp(g_cfg[i].key, key) == 0) {
            strncpy(g_cfg[i].val, val, KSS_VAL_MAX - 1);
            g_cfg[i].val[KSS_VAL_MAX - 1] = 0;
            return;
        }
    }
    if (g_cfg_n >= KSS_ENTRY_MAX) return;
    Ent* e = &g_cfg[g_cfg_n++];
    strncpy(e->key, key, KSS_KEY_MAX - 1);
    e->key[KSS_KEY_MAX - 1] = 0;
    strncpy(e->val, val, KSS_VAL_MAX - 1);
    e->val[KSS_VAL_MAX - 1] = 0;
}

// ---- rpc (satoru)
bool rpc(const KssReq& req, KssReply* rep, KssTable* table) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, KSS_PATH, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }
    const uint8_t* p = (const uint8_t*)&req;
    int left = (int)sizeof(req);
    while (left > 0) {
        ssize_t n = send(fd, p, (size_t)left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        p += n;
        left -= (int)n;
    }
    int wait = (req.kind == KSS_ACTION) ? 30000 : 5000;
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, wait) <= 0) { close(fd); return false; }
    uint8_t* q = (uint8_t*)rep;
    int need = (int)sizeof(*rep);
    int got = 0;
    while (got < need) {
        ssize_t n = recv(fd, q + got, (size_t)(need - got), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += (int)n;
    }
    if (got != need || rep->magic != KSS_REPLY_MAGIC) { close(fd); return false; }
    if (table && rep->ok) {
        uint8_t* t = (uint8_t*)table;
        int tn = (int)sizeof(*table);
        int tg = 0;
        while (tg < tn) {
            ssize_t n = recv(fd, t + tg, (size_t)(tn - tg), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            tg += (int)n;
        }
        if (tg != tn || table->magic != KSS_TABLE_MAGIC) { close(fd); return false; }
    }
    close(fd);
    return rep->ok != 0;
}

KssReq req_init(uint32_t kind) {
    KssReq r;
    memset(&r, 0, sizeof(r));
    r.magic = KSS_MAGIC;
    r.kind = kind;
    return r;
}
void req_str(char* d, int mx, const char* s) {
    if (!s) { d[0] = 0; return; }
    strncpy(d, s, (size_t)(mx - 1));
    d[mx - 1] = 0;
}

bool set_key(const char* key, const char* val) {
    KssReq r = req_init(KSS_SET);
    req_str(r.key, sizeof(r.key), key);
    req_str(r.val, sizeof(r.val), val);
    KssReply rep;
    if (!rpc(r, &rep, nullptr)) return false;
    cfg_put(key, val);
    return true;
}

void action(const char* name, int32_t ival, uint32_t uval, const char* arg,
            const char* key, const char* val, const int32_t* a = nullptr,
            const int32_t* b = nullptr, uint32_t onmask = 0) {
    KssReq r = req_init(KSS_ACTION);
    req_str(r.action, sizeof(r.action), name);
    req_str(r.arg, sizeof(r.arg), arg);
    req_str(r.key, sizeof(r.key), key);
    req_str(r.val, sizeof(r.val), val);
    r.ival = ival;
    r.uval = uval;
    r.onmask = onmask;
    if (a) for (int i = 0; i < 4; i++) r.a[i] = a[i];
    if (b) for (int i = 0; i < 4; i++) r.b[i] = b[i];
    KssReply rep;
    rpc(r, &rep, nullptr);
}

// the same, but the service's verdict comes back (account changes). (satoru)
bool action_ok(const char* name, uint32_t uval, const char* key, const char* val) {
    KssReq r = req_init(KSS_ACTION);
    req_str(r.action, sizeof(r.action), name);
    req_str(r.key, sizeof(r.key), key);
    req_str(r.val, sizeof(r.val), val);
    r.uval = uval;
    KssReply rep;
    rep.ok = 0;
    if (!rpc(r, &rep, nullptr)) return false;
    volatile char* v = (volatile char*)r.val;
    for (unsigned i = 0; i < sizeof(r.val); i++) v[i] = 0;
    return rep.ok != 0;
}

// ---- drawing target for the kit (satoru)
kui::raster::Target* g_target = nullptr;
kui::Window* g_kwin = nullptr;
bool g_alive = false;
bool g_focused = true;

struct ClipSave { int x0, y0, x1, y1; };
ClipSave g_clip[32];
int g_clip_sp = 0;

void target_reset(kui::raster::Target* t) { g_target = t; g_clip_sp = 0; }

}  // namespace

// ---- settings bridge entry points (satoru)
namespace SettingsBridge {

bool Refresh() {
    KssTable* t = (KssTable*)malloc(sizeof(KssTable));
    if (!t) return false;
    KssReply rep;
    g_cfg_n = 0;
    for (uint32_t start = 0; start < KSS_INFO_MAX; ) {
        KssReq r = req_init(KSS_GET_CFG);
        r.uval = start;
        if (!rpc(r, &rep, t)) break;
        for (uint32_t i = 0; i < t->count && g_cfg_n < KSS_ENTRY_MAX; i++) {
            Ent* e = &g_cfg[g_cfg_n++];
            strncpy(e->key, t->entries[i].key, KSS_KEY_MAX - 1); e->key[KSS_KEY_MAX - 1] = 0;
            strncpy(e->val, t->entries[i].val, KSS_VAL_MAX - 1); e->val[KSS_VAL_MAX - 1] = 0;
        }
        if (t->count < KSS_ENTRY_MAX) break;
        start += t->count;
    }
    g_info_n = 0;
    for (uint32_t start = 0; start < KSS_INFO_MAX; ) {
        KssReq r = req_init(KSS_GET_INFO);
        r.uval = start;
        if (!rpc(r, &rep, t)) break;
        for (uint32_t i = 0; i < t->count && g_info_n < KSS_INFO_MAX; i++) {
            Ent* e = &g_info[g_info_n++];
            strncpy(e->key, t->entries[i].key, KSS_KEY_MAX - 1); e->key[KSS_KEY_MAX - 1] = 0;
            strncpy(e->val, t->entries[i].val, KSS_VAL_MAX - 1); e->val[KSS_VAL_MAX - 1] = 0;
        }
        if (t->count < KSS_ENTRY_MAX) break;
        start += t->count;
    }
    free(t);
    PMM::usable_frames = (uint64_t)info_ll("mem.total", 0) / PAGE_SIZE;
    PMM::used_frames = (uint64_t)info_ll("mem.used", 0) / PAGE_SIZE;
    PMM::total_frames = PMM::usable_frames;
    // the account list the users page reads straight from UserManager::users. (satoru)
    {
        int un = info_int("user.count", 0);
        if (un > UserManager::MAX_USERS) un = UserManager::MAX_USERS;
        for (int i = 0; i < un; i++) {
            User& u = UserManager::users[i];
            memset(&u, 0, sizeof(u));
            char k[32];
            snprintf(k, sizeof(k), "user.%d.name", i);
            strncpy(u.username, info_str(k, ""), sizeof(u.username) - 1);
            snprintf(k, sizeof(k), "user.%d.display", i);
            strncpy(u.display_name, info_str(k, ""), sizeof(u.display_name) - 1);
            snprintf(k, sizeof(k), "user.%d.admin", i);
            u.is_admin = info_int(k, 0) != 0;
        }
        UserManager::user_count = un;
    }
    g_loaded = true;
    FontTTF::SetActive(UIStyle::Modern());
    return true;
}

bool Loaded() { return g_loaded; }

int NodeCount() { return info_int("fs.nodes", 0); }

}  // namespace SettingsBridge

// ---- ui config: local snapshot, every write goes to the service (satoru)
const char* UIConfig::Path() { return "/system/etc/kurono/ui.conf"; }
const char* UIConfig::DefaultFile() { return ""; }
void UIConfig::Init() {}
void UIConfig::Load() {}
bool UIConfig::Reload() { SettingsBridge::Refresh(); return true; }
uint32_t UIConfig::Version() { return 1; }
uint32_t UIConfig::Color(const char* key, uint32_t fallback) { return cfg_color(key, fallback); }
int UIConfig::Int(const char* key, int fallback) { return cfg_int(key, fallback); }
bool UIConfig::Bool(const char* key, bool fallback) { return cfg_bool(key, fallback); }
const char* UIConfig::Str(const char* key, const char* fallback) { return cfg_get(key, fallback); }
void UIConfig::Set(const char* key, const char* value, bool persist) {
    (void)persist;
    if (key && value) set_key(key, value);
}
void UIConfig::SetColor(const char* key, uint32_t argb, bool persist) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08X", argb);
    Set(key, b, persist);
}
void UIConfig::SetInt(const char* key, int v, bool persist) {
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    Set(key, b, persist);
}
bool UIConfig::Save() { return true; }

// ---- kernel services the pages read (satoru)
namespace {
struct Nin { int x, y, w, h; };
}

size_t KernelHeap::GetUsed() { return (size_t)info_ll("heap.used", 0); }
size_t KernelHeap::GetFree() {
    long long t = info_ll("heap.total", 0), u = info_ll("heap.used", 0);
    return t > u ? (size_t)(t - u) : 0;
}
size_t KernelHeap::GetTotal() { return (size_t)info_ll("heap.total", 0); }

uint64_t PMM::usable_frames = 0;
uint64_t PMM::total_frames = 0;
uint64_t PMM::used_frames = 0;

uint32_t Timer::GetRealMs() { return kui::NowMs(); }

int Mouse::mx = -1;
int Mouse::my = -1;
bool Mouse::IsLeftDown() { return false; }

void RuntimeLog::LogAppEvent(const char*, const char*, const char*) {}
TaskMem::AppScope::AppScope(uint32_t) { armed = false; }
TaskMem::AppScope::~AppScope() {}
volatile bool TaskMem::g_on = false;

void Keyboard::SetStickyKeys(bool) {}
void Keyboard::SetSlowKeys(uint32_t) {}
void Keyboard::SetBounceKeys(uint32_t) {}
void Keyboard::SetScreenReader(bool) {}
bool Widget::high_contrast_mode = false;

// ---- graphics: the kit and the display page draw through the kui canvas (satoru)
uint8_t* Graphics::GetBuffer() { return g_target ? (uint8_t*)g_target->px : nullptr; }
int Graphics::GetWidth() { return info_int("screen.w", 1024); }
int Graphics::GetHeight() { return info_int("screen.h", 768); }
uint32_t Graphics::GetPitch() { return g_target ? (uint32_t)(g_target->pitch * 4) : 0; }
uint8_t Graphics::GetBpp() { return 32; }
// ring 3 has no damage scissor: the kit clamps to the screen as before (satoru)
void Graphics::ScissorBounds(int& x0, int& y0, int& x1, int& y1) { x0 = 0; y0 = 0; x1 = GetWidth(); y1 = GetHeight(); }
int Graphics::GetUIScale() { return cfg_int("display.ui_scale", info_int("screen.scale", 100)); }
void Graphics::RequestUIScale(int pct) { action("ui.scale", pct, 0, nullptr, nullptr, nullptr); }
bool Graphics::MultiActive() { return info_int("out.active", 0) != 0; }
void Graphics::RequestOutputConfig(int primary, const bool* on, const int* ax, const int* ay) {
    int32_t a[4] = { 0, 0, 0, 0 }, b[4] = { 0, 0, 0, 0 };
    uint32_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (on && on[i]) mask |= 1u << i;
        if (ax) a[i] = ax[i];
        if (ay) b[i] = ay[i];
    }
    action("display.config", primary, 0, nullptr, nullptr, nullptr, a, b, mask);
}
void Graphics::ApplyPendingOutputOp() {}
bool Graphics::TakeOutputReloc(int*, int*, int*, int*) { return false; }
void Graphics::SetHighContrast(bool on) { action("a11y.contrast", 0, on ? 1 : 0, nullptr, nullptr, nullptr); }
void Graphics::SetColorFilter(int mode) { action("a11y.filter", mode, 0, nullptr, nullptr, nullptr); }
void Graphics::SetBrightness(int pct) { action("display.brightness", pct, 0, nullptr, nullptr, nullptr); }
void Graphics::MarkDirty(int, int, int, int) { if (g_kwin) g_kwin->Invalidate(); }
void Graphics::MarkUIDirty() { if (g_kwin) g_kwin->Invalidate(); }
void Graphics::RequestFrameAt(uint32_t ms) { if (g_kwin) g_kwin->RequestFrameAt(ms); }
void Graphics::FillRectAlpha(int x, int y, int w, int h, uint8_t a, uint32_t c) {
    if (g_target) kui::raster::FillRectAlpha(*g_target, x, y, w, h, a, c);
}
void Graphics::FillRectRounded(int x, int y, int w, int h, int r, uint32_t c) {
    if (g_target) kui::raster::FillRectRounded(*g_target, x, y, w, h, r, c);
}
void Graphics::PushClipRect(int x, int y, int w, int h) {
    if (!g_target) return;
    if (g_clip_sp < 32) g_clip[g_clip_sp] = { g_target->x0, g_target->y0, g_target->x1, g_target->y1 };
    g_clip_sp++;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < g_target->x0) x0 = g_target->x0;
    if (y0 < g_target->y0) y0 = g_target->y0;
    if (x1 > g_target->x1) x1 = g_target->x1;
    if (y1 > g_target->y1) y1 = g_target->y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    g_target->x0 = x0; g_target->y0 = y0; g_target->x1 = x1; g_target->y1 = y1;
}
void Graphics::PopClipRect() {
    if (!g_target || g_clip_sp <= 0) return;
    g_clip_sp--;
    g_target->x0 = g_clip[g_clip_sp].x0;
    g_target->y0 = g_clip[g_clip_sp].y0;
    g_target->x1 = g_clip[g_clip_sp].x1;
    g_target->y1 = g_clip[g_clip_sp].y1;
}
void Graphics::DrawString(int x, int y, const char* s, uint32_t fg, uint32_t) {
    if (g_target && s) kui::raster::Bitmap8x16(*g_target, x, y, 16.0f, s, 1 << 30, fg);
}

uint32_t MDraw::Mix(uint32_t a, uint32_t b, int t) { return kui::raster::Mix(a, b, t); }
void MDraw::RoundRect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha) {
    if (g_target) kui::raster::RoundRect(*g_target, x, y, w, h, r, top, bottom, alpha);
}
void MDraw::RoundOutline(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    if (g_target) kui::raster::RoundOutline(*g_target, x, y, w, h, r, c, alpha);
}
void MDraw::Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha) {
    if (g_target) kui::raster::Shadow(*g_target, x, y, w, h, r, spread, dy, alpha);
}

// ---- window manager: one kui window, one dummy kernel Window for the shell (satoru)
namespace {
Window g_win;
WindowRenderFunc g_render_cb = nullptr;
WindowInputFunc g_input_cb = nullptr;

struct SettingsHandler : kui::AppHandler {
    void OnCreate(kui::Window& w) override {
        (void)w;
        g_alive = true;
        g_focused = true;
    }
    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t) override {
        // live pages (wi-fi scan list, devices, battery) read the info
        // snapshot: renew it at most every 1.5 s while frames are drawn. (satoru)
        static uint32_t last_refresh = 0;
        uint32_t now = kui::NowMs();
        if (now - last_refresh > 1500u) { last_refresh = now; SettingsBridge::Refresh(); }
        kui::Canvas::Impl* ci = c.impl();
        target_reset(&ci->t);
        kui::bk::SetTextTarget(&ci->t);
        if (g_render_cb) g_render_cb(&g_win, 0, 0, c.Width(), c.Height());
        kui::bk::SetTextTarget(nullptr);
        g_target = nullptr;
        (void)w;
    }
    bool OnEvent(kui::Window& w, const kui::Event& e) override {
        (void)w;
        switch (e.type) {
        case kui::EventType::PointerMove:
            Mouse::mx = e.x; Mouse::my = e.y;
            if (g_input_cb) g_input_cb(&g_win, 5, e.x, e.y);
            return true;
        case kui::EventType::PointerDown:
            if (e.button != kui::Button::Left) return false;
            Mouse::mx = e.x; Mouse::my = e.y;
            if (g_input_cb) g_input_cb(&g_win, 6, 0, 1);
            return true;
        case kui::EventType::PointerUp:
            if (e.button != kui::Button::Left) return false;
            if (g_input_cb) g_input_cb(&g_win, 6, 0, 0);
            return true;
        case kui::EventType::Click:
            if (e.button != kui::Button::Left) return false;
            Mouse::mx = e.x; Mouse::my = e.y;
            if (g_input_cb) g_input_cb(&g_win, 1, e.x, e.y);
            return true;
        case kui::EventType::Scroll:
            if (g_input_cb) g_input_cb(&g_win, 3, e.scroll_steps, 0);
            return true;
        case kui::EventType::KeyDown: {
            int k = e.key;
            bool shift = (e.mods & kui::M_SHIFT) != 0;
            if (k == kui::K_BS || k == kui::K_ENTER || k == kui::K_TAB || k == kui::K_BTAB ||
                k == kui::K_ESC || k == kui::K_UP || k == kui::K_DOWN || k == kui::K_LEFT ||
                k == kui::K_RIGHT || k == kui::K_HOME || k == kui::K_END || k == kui::K_PGUP ||
                k == kui::K_PGDN || k == kui::K_DEL || (k >= 32 && k <= 126)) {
                SystemSettings::KeyCode(k, shift);
            }
            return true;
        }
        case kui::EventType::Focus:
            g_focused = true;
            return true;
        case kui::EventType::Blur:
            g_focused = false;
            return true;
        default:
            break;
        }
        return false;
    }
    void OnTick(kui::Window& w, uint32_t now) override {
        (void)w;
        static uint32_t last = 0;
        if (now - last < 500u) return;
        last = now;
        char page[32];
        int n = kui::Fs::ReadFile("/system/run/kui/settings-page", page, (int)sizeof(page) - 1);
        if (n <= 0) return;
        if (n >= (int)sizeof(page)) n = (int)sizeof(page) - 1;
        page[n] = 0;
        int k = 0;
        while (page[k] && page[k] != '\n' && page[k] != '\r') k++;
        page[k] = 0;
        kui::Fs::Remove("/system/run/kui/settings-page");
        if (page[0]) SystemSettings::OpenPage(page);
    }
    bool OnClose(kui::Window& w) override {
        (void)w;
        g_alive = false;
        return true;
    }
};
}

int WindowManager::CreateWindow(const char* title, int x, int y, int w, int h,
                                WindowRenderFunc render, WindowInputFunc input) {
    if (g_kwin) return g_win.id;
    g_render_cb = render;
    g_input_cb = input;
    memset(&g_win, 0, sizeof(g_win));
    g_win.id = 1;
    g_win.state = WIN_NORMAL;
    g_win.visible = true;
    g_win.focused = true;
    g_win.resizable = true;
    g_win.closable = true;
    g_win.content_x = 0; g_win.content_y = 0;
    g_win.content_w = w; g_win.content_h = h;
    kui::WindowSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.title = title ? title : "Settings";
    spec.app_id = "settings";
    spec.w = w; spec.h = h;
    spec.min_w = 620; spec.min_h = 420;
    spec.x = x; spec.y = y;
    spec.resizable = true;
    spec.closable = true;
    spec.app_icon = -1;
    spec.bg = 0xFF18181B;
    g_kwin = kui::OpenWindow(spec, new SettingsHandler());
    if (!g_kwin) { g_alive = false; return -1; }
    return g_win.id;
}

Window* WindowManager::GetWindow(int id) {
    if (id == 1 && g_alive) return &g_win;
    return nullptr;
}
Window* WindowManager::GetFocusedWindow() {
    if (g_alive && g_focused) return &g_win;
    return nullptr;
}
void WindowManager::Focus(int id) { (void)id; if (g_kwin) g_kwin->Raise(); }
void WindowManager::BringToFront(int id) { (void)id; if (g_kwin) g_kwin->Raise(); }
void WindowManager::ReloadFromConfig() { action("wm.reload", 0, 0, nullptr, nullptr, nullptr); }

// ---- desktop, style, taskbar (satoru)
void UIStyle::Set(bool modern) {
    cfg_put("ui.style", modern ? "modern" : "classic");
    action("ui.style", 0, modern ? 1 : 0, nullptr, nullptr, nullptr);
    FontTTF::SetActive(modern);
}
bool UIStyle::Modern() { return !strcmp(cfg_get("ui.style", "modern"), "modern"); }
void UIStyle::SetMinimalIcons(bool minimal) {
    cfg_put("ui.icons", minimal ? "minimal" : "colorful");
    action("ui.icons", 0, minimal ? 1 : 0, nullptr, nullptr, nullptr);
}
bool UIStyle::MinimalIcons() { return !strcmp(cfg_get("ui.icons", "colorful"), "minimal"); }
void UIStyle::Apply() {}
void Desktop::ReloadFromConfig() { action("desktop.reload", 0, 0, nullptr, nullptr, nullptr); }
void Desktop::SetWallpaper(unsigned int c) { action("wallpaper.color", 0, c, nullptr, nullptr, nullptr); }
bool Desktop::ApplyBuiltinWallpaper(int idx) {
    action("wallpaper.builtin", idx, 0, nullptr, nullptr, nullptr);
    return true;
}
const uint8_t* Desktop::BuiltinWallpaperRGBA(int idx, int* w, int* h) {
    static uint8_t* cache[2] = { nullptr, nullptr };
    static int cache_n[2] = { 0, 0 };
    if (idx < 0 || idx > 1) return nullptr;
    if (!cache[idx]) {
        char path[64];
        snprintf(path, sizeof(path), "/system/share/kurono/wallpaper%d.rgba", idx);
        int sz = kui::Fs::FileSize(path);
        if (sz <= 0) return nullptr;
        uint8_t* p = (uint8_t*)kui::Alloc((size_t)sz);
        if (!p) return nullptr;
        if (kui::Fs::ReadFile(path, p, sz) != sz) { kui::Free(p); return nullptr; }
        cache[idx] = p;
        cache_n[idx] = sz;
    }
    if (w) *w = 960;
    if (h) *h = cache_n[idx] / (960 * 4);
    return cache[idx];
}
void DesktopEnvironment::ReloadFromConfig() { action("deskenv.reload", 0, 0, nullptr, nullptr, nullptr); }
void Taskbar::ReloadFromConfig() { action("taskbar.reload", 0, 0, nullptr, nullptr, nullptr); }

// ---- battery (satoru)
#include "../../system/battery.h"
void Battery::Tick() {}
bool Battery::Present() { return info_int("bat.present", 0) != 0; }
int Battery::Percent() { return info_int("bat.pct", -1); }
Battery::State Battery::GetState() { return (Battery::State)info_int("bat.state", 0); }
int Battery::Minutes() { return info_int("bat.min", -1); }
void Battery::Describe(char* out, int cap) { strncpy(out, info_str("bat.desc", ""), (size_t)cap - 1); out[cap - 1] = 0; }

// ---- kernel slots + recovery restart (satoru)
#include "../../system/kernel_slots.h"
#include "../../system/recovery_restart.h"
void KernelSlots::Describe(char* out, int cap) {
    strncpy(out, info_str("boot.slots", ""), (size_t)cap - 1);
    out[cap - 1] = 0;
}
void RecoveryRestart() { action("boot.recovery", 0, 0, nullptr, nullptr, nullptr); }

// ---- audio devices (satoru)
#include "../../system/audio_devices.h"
int AudioDevices::OutputCount() { return info_int("audio.out.count", 0); }
bool AudioDevices::OutputInfo(int i, char* name, int ncap, char* label, int lcap, bool* active, bool* ready) {
    if (i < 0 || i >= OutputCount()) return false;
    char k[40];
    snprintf(k, sizeof(k), "audio.out.%d.name", i);
    if (name) { strncpy(name, info_str(k, ""), (size_t)ncap - 1); name[ncap - 1] = 0; }
    snprintf(k, sizeof(k), "audio.out.%d.label", i);
    if (label) { strncpy(label, info_str(k, ""), (size_t)lcap - 1); label[lcap - 1] = 0; }
    snprintf(k, sizeof(k), "audio.out.%d.active", i);
    if (active) *active = info_int(k, 0) != 0;
    snprintf(k, sizeof(k), "audio.out.%d.ready", i);
    if (ready) *ready = info_int(k, 0) != 0;
    return true;
}
bool AudioDevices::SelectOutput(const char* name) {
    bool ok = action_ok("audio.output", 0, name, nullptr);
    SettingsBridge::Refresh();
    return ok;
}
// the service remembers the level when the slider commit arrives. (satoru)
void AudioDevices::RememberVolume(int v) { action("audio.volume", v, 1, nullptr, nullptr, nullptr); }
int AudioDevices::InputCount() { return 0; }

// ---- audio and power (satoru)
int AudioMixer::GetMasterVolume() { return info_int("audio.volume", 75); }
void AudioMixer::SetMasterVolume(int v) { action("audio.volume", v, 0, nullptr, nullptr, nullptr); }
void AudioMixer::SetMasterMute(bool m) { action("audio.mute", 0, m ? 1 : 0, nullptr, nullptr, nullptr); }
bool Audio::IsAvailable() { return info_int("audio.available", 0) != 0; }
void Audio::SetMasterVolume(int v) { (void)v; }
void Audio::SetMuted(bool m) { (void)m; }
// recovery actions go to the kernel through the settings service; the page
// reads the result back from the info snapshot it already refreshes. (satoru)
bool AudioServer::RestartActive() {
    action("audio.restart", 0, 0, nullptr, nullptr, nullptr);
    return info_int("audio.restart_ok", 0) != 0;
}
int AudioServer::StopAllStreams() {
    action("audio.stop", 0, 0, nullptr, nullptr, nullptr);
    return info_int("audio.stopped", 0);
}
AudioServer::ServerStatus AudioServer::GetStatus() {
    static ServerStatus st;
    static char backend[32];
    strncpy(backend, info_str("audio.backend", "none"), sizeof(backend) - 1);
    backend[sizeof(backend) - 1] = 0;
    st.backend_name = backend;
    st.backend_ready = info_int("audio.ready", 0) != 0;
    st.backend_rate = (uint32_t)info_int("audio.rate", 48000);
    st.active_streams = info_int("audio.streams", 0);
    st.master_volume = info_int("audio.volume", 75);
    st.master_muted = info_int("audio.muted", 0) != 0;
    st.total_periods_submitted = 0;
    return st;
}
AudioBackend* AudioServer::ActiveBackend() { return nullptr; }
void AudioServer::PlayTone(int, int, int) { action("audio.tone", 0, 0, nullptr, nullptr, nullptr); }
bool HDAudio::IsDetected() { return info_int("audio.hda", 0) != 0; }
int HDAudio::GetCodecCount() { return info_int("audio.codecs", 0); }
uint32_t HDAudio::GetCodecVendor(int) { return 0; }

int CPUFreq::CPUCount() { return info_int("cpu.count", 1); }
const CPUFreq::CPUInfo* CPUFreq::GetCPU(uint32_t id) {
    static CPUInfo ci[32];
    static bool done = false;
    if (!done) {
        done = true;
        int n = info_int("cpu.count", 1);
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            char k[32];
            snprintf(k, sizeof(k), "cpu.%d.present", i); ci[i].present = info_int(k, 0) != 0;
            snprintf(k, sizeof(k), "cpu.%d.base", i); ci[i].base_mhz = (uint16_t)info_int(k, 0);
            snprintf(k, sizeof(k), "cpu.%d.turbo", i); ci[i].turbo_mhz = (uint16_t)info_int(k, 0);
            snprintf(k, sizeof(k), "cpu.%d.cur", i); ci[i].cur_mhz = (uint16_t)info_int(k, 0);
            snprintf(k, sizeof(k), "cpu.%d.gov", i); ci[i].governor = (Governor)info_int(k, 2);
            snprintf(k, sizeof(k), "cpu.%d.pstates", i); ci[i].pstate_count = (uint8_t)info_int(k, 0);
            snprintf(k, sizeof(k), "cpu.%d.load", i); ci[i].recent_load_pct = (uint8_t)info_int(k, 0);
            ci[i].vendor = 2;
        }
    }
    if (id >= 32) return nullptr;
    return &ci[id];
}
CPUFreq::Governor CPUFreq::GetGovernor(uint32_t id) {
    const CPUInfo* ci = GetCPU(id);
    return ci ? ci->governor : GOV_ONDEMAND;
}
bool CPUFreq::SetGovernor(uint32_t, Governor) { return true; }

// ---- storage (satoru)
int BlockDev::Count() { return info_int("disk.count", 0); }
BlockDevice* BlockDev::Get(int idx) {
    static BlockDevice d[16];
    static bool done = false;
    if (!done) {
        done = true;
        int n = info_int("disk.count", 0);
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) {
            char k[40];
            snprintf(k, sizeof(k), "disk.%d.name", i); strncpy(d[i].name, info_str(k, ""), 15); d[i].name[15] = 0;
            snprintf(k, sizeof(k), "disk.%d.driver", i); strncpy(d[i].driver, info_str(k, ""), 15); d[i].driver[15] = 0;
            snprintf(k, sizeof(k), "disk.%d.model", i); strncpy(d[i].model, info_str(k, ""), 63); d[i].model[63] = 0;
            snprintf(k, sizeof(k), "disk.%d.bytes", i);
            long long bytes = info_ll(k, 0);
            d[i].capacity_lba = bytes > 0 ? (uint64_t)bytes / 512 : 0;
            d[i].sector_size = 512;
            snprintf(k, sizeof(k), "disk.%d.removable", i); d[i].removable = info_int(k, 0) != 0;
            d[i].present = d[i].name[0] != 0;
        }
    }
    return idx >= 0 && idx < 16 ? &d[idx] : nullptr;
}
uint32_t KVFS::DiskUsage(const char*) { return (uint32_t)info_ll("fs.used", 0); }
KVFSNode* KVFS::GetRoot() { return nullptr; }
bool PersistStore::Available() { return info_int("persist", 0) != 0; }

// ---- processor, gpu, display (satoru)
const char* CPUDetect::GetVendorName() { return info_str("cpu.vendor", "Unknown"); }
const char* CPUDetect::GetBrandString() { return info_str("cpu.brand", "Processor"); }
int CPUDetect::GetCoreCount() { return info_int("cpu.cores", 1); }
int CPUDetect::GetThreadCount() { return info_int("cpu.threads", 1); }
int CPUDetect::GetBaseMHz() { return info_int("cpu.base_mhz", 0); }
bool CPUDetect::HasSSE2() { return info_int("cpu.sse2", 1) != 0; }
bool CPUDetect::HasAVX() { return info_int("cpu.avx", 0) != 0; }
bool CPUDetect::HasAVX2() { return info_int("cpu.avx2", 0) != 0; }
bool CPUDetect::HasAVX512() { return info_int("cpu.avx512", 0) != 0; }
bool CPUDetect::HasAES() { return info_int("cpu.aes", 0) != 0; }
bool CPUDetect::HasRDRAND() { return info_int("cpu.rdrand", 0) != 0; }

const GpuProbeResult& GpuProbe::GetResult() {
    static GpuProbeResult r;
    static bool done = false;
    if (!done) {
        done = true;
        r.count = info_int("gpu.count", 0);
        if (r.count > GPU_PROBE_MAX) r.count = GPU_PROBE_MAX;
        r.primary_idx = info_int("gpu.primary", 0);
        r.topology = GPU_TOPO_SINGLE;
        for (int i = 0; i < r.count; i++) {
            char k[40];
            snprintf(k, sizeof(k), "gpu.%d.vendor", i); r.gpus[i].vendor_id = (uint16_t)info_int(k, 0);
            snprintf(k, sizeof(k), "gpu.%d.device", i); r.gpus[i].device_id = (uint16_t)info_int(k, 0);
            snprintf(k, sizeof(k), "gpu.%d.igpu", i); r.gpus[i].is_igpu = info_int(k, 0) != 0;
            snprintf(k, sizeof(k), "gpu.%d.desc", i); strncpy(r.gpus[i].desc, info_str(k, ""), 47); r.gpus[i].desc[47] = 0;
            r.gpus[i].present = true;
        }
    }
    return r;
}
bool IntelGPU::IsDetected() { return info_int("gpu.intel", 0) != 0; }
const char* IntelGPU::GetGenName() { return info_str("gpu.intel.gen", ""); }
bool NvidiaGPU::IsDetected() { return info_int("gpu.nvidia", 0) != 0; }
const char* NvidiaGPU::GetArchName() { return info_str("gpu.nvidia.arch", ""); }
bool AmdGPU::IsAvailable() { return info_int("gpu.amd", 0) != 0; }
const char* AmdGPU::GetArchName() { return info_str("gpu.amd.arch", ""); }

DisplayBackend DisplayManager::GetBackend() { return DISPLAY_BACKEND_BGA; }
const char* DisplayManager::GetBackendName() { return info_str("display.backend", "Framebuffer"); }
uint32_t DisplayManager::GetWidth() { return (uint32_t)info_int("display.w", info_int("screen.w", 1024)); }
uint32_t DisplayManager::GetHeight() { return (uint32_t)info_int("display.h", info_int("screen.h", 768)); }
uint8_t DisplayManager::GetBrightness() { return (uint8_t)info_int("display.brightness", 255); }
VSyncMode DisplayManager::GetVSync() { return info_int("display.vsync", 1) ? VSYNC_ON : VSYNC_OFF; }
// night light: the kernel owns the tint and the schedule; the page reads the
// saved keys and asks the service to change them. (satoru)
namespace NightLight {
static void put_int(const char* key, int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); cfg_put(key, b); }
bool IsOn() { return cfg_int("display.night_light", 0) != 0; }
void Set(bool on) { action("nightlight.on", 0, on ? 1 : 0, nullptr, nullptr, nullptr); put_int("display.night_light", on ? 1 : 0); }
void Toggle() { Set(!IsOn()); }
int  Strength() { return cfg_int("display.night_strength", 70); }
void SetStrength(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    action("nightlight.strength", pct, 0, nullptr, nullptr, nullptr);
    put_int("display.night_strength", pct);
}
bool ScheduleOn() { return cfg_int("display.night_schedule", 0) != 0; }
void SetSchedule(bool on) {
    action("nightlight.schedule", 0, on ? 1 : 0, nullptr, nullptr, nullptr);
    put_int("display.night_schedule", on ? 1 : 0);
    SettingsBridge::Refresh();   // the schedule may have switched the tint (satoru)
}
int  FromMinutes() { return cfg_int("display.night_from", 20 * 60); }
int  ToMinutes() { return cfg_int("display.night_to", 7 * 60); }
void Init() {}
void Tick() {}
}  // namespace NightLight

void DisplayManager::SetBrightness(uint8_t level) { action("display.brightness", (level * 100) / 255, 0, nullptr, nullptr, nullptr); }
void DisplayManager::SetVSync(VSyncMode mode) { action("display.vsync", 0, mode != VSYNC_OFF ? 1 : 0, nullptr, nullptr, nullptr); }
bool DisplayManager::SetResolution(uint32_t w, uint32_t h, uint8_t) {
    action("display.resolution", (int32_t)w, h, nullptr, nullptr, nullptr);
    return true;
}
bool DisplayManager::GetSupportedModes(DisplayMode* modes, int max_count, int* count) {
    int n = info_int("mode.count", 0);
    if (n > max_count) n = max_count;
    if (n > DISPLAY_MODE_COUNT) n = DISPLAY_MODE_COUNT;
    for (int i = 0; i < n; i++) {
        char k[40];
        snprintf(k, sizeof(k), "mode.%d.w", i); modes[i].width = (uint32_t)info_int(k, 0);
        snprintf(k, sizeof(k), "mode.%d.h", i); modes[i].height = (uint32_t)info_int(k, 0);
        snprintf(k, sizeof(k), "mode.%d.bpp", i); modes[i].bpp = (uint8_t)info_int(k, 32);
        snprintf(k, sizeof(k), "mode.%d.refresh", i); modes[i].refresh_rate = (uint8_t)info_int(k, 60);
        modes[i].interlaced = false;
        modes[i].name = "";
    }
    if (count) *count = n;
    return n > 0;
}
const MonitorInfo& DisplayManager::GetMonitorInfo() {
    static MonitorInfo mi;
    static bool done = false;
    if (!done) {
        done = true;
        memset(&mi, 0, sizeof(mi));
        mi.connected = true;
        strncpy(mi.name, "Display", sizeof(mi.name) - 1);
        mi.native_width = GetWidth();
        mi.native_height = GetHeight();
        mi.max_width = mi.native_width;
        mi.max_height = mi.native_height;
        mi.max_refresh = 60;
    }
    return mi;
}

int Outputs::Total() { return info_int("out.count", 1); }
bool Outputs::Multi() { return info_int("out.multi", 0) != 0; }
const Output* Outputs::Get(int i) {
    static Output o[OUTPUT_MAX];
    static bool done = false;
    if (!done) {
        done = true;
        int n = info_int("out.count", 1);
        if (n > OUTPUT_MAX) n = OUTPUT_MAX;
        for (int k = 0; k < n; k++) {
            char key[40];
            snprintf(key, sizeof(key), "out.%d.name", k); strncpy(o[k].name, info_str(key, "Display"), 15); o[k].name[15] = 0;
            snprintf(key, sizeof(key), "out.%d.present", k); o[k].present = info_int(key, 1) != 0;
            snprintf(key, sizeof(key), "out.%d.connected", k); o[k].connected = info_int(key, 1) != 0;
            snprintf(key, sizeof(key), "out.%d.x", k); o[k].x = info_int(key, 0);
            snprintf(key, sizeof(key), "out.%d.y", k); o[k].y = info_int(key, 0);
            snprintf(key, sizeof(key), "out.%d.w", k); o[k].w = info_int(key, (int)DisplayManager::GetWidth());
            snprintf(key, sizeof(key), "out.%d.h", k); o[k].h = info_int(key, (int)DisplayManager::GetHeight());
            snprintf(key, sizeof(key), "out.%d.pw", k); o[k].pw = info_int(key, o[k].w);
            snprintf(key, sizeof(key), "out.%d.ph", k); o[k].ph = info_int(key, o[k].h);
            snprintf(key, sizeof(key), "out.%d.ax", k); o[k].ax = info_int(key, 0);
            snprintf(key, sizeof(key), "out.%d.ay", k); o[k].ay = info_int(key, 0);
        }
    }
    return i >= 0 && i < OUTPUT_MAX ? &o[i] : nullptr;
}

uint32_t FramePacer::Mode() { return (uint32_t)info_int("fpc.mode", 0); }
uint32_t FramePacer::CapHz() { return (uint32_t)info_int("fpc.cap", 60); }
uint32_t FramePacer::CurrentHz() { return (uint32_t)info_int("fpc.current", 60); }
uint32_t FramePacer::CmdlineHz() { return (uint32_t)info_int("fpc.cmdline", 0); }
uint32_t FramePacer::PanelHz() { return (uint32_t)info_int("fpc.panel", 60); }
const char* FramePacer::PanelSource() { return info_str("fpc.source", "default"); }
void FramePacer::SetMode(uint32_t hz) {
    action("framepacer.mode", 0, hz, nullptr, nullptr, nullptr);
}
void FramePacer::ReloadConfig() {}
void FramePacer::OutputsChanged() {}

// ---- external monitor refresh (the service owns the real hardware switch) ----
int IntelModeset::Rates(uint32_t* hz, int max) {
    if (!hz || max <= 0) return 0;
    const char* v = info_str("dp.rates", "");
    int n = 0;
    while (*v && n < max) {
        char* end = nullptr;
        long r = strtol(v, &end, 10);
        if (end == v) break;
        hz[n++] = (uint32_t)r;
        v = end;
        while (*v == ',' || *v == ' ') v++;
    }
    return n;
}
uint32_t IntelModeset::CurrentHz() { return (uint32_t)info_int("dp.hz", 0); }
uint32_t IntelModeset::CurrentMilliHz() { return CurrentHz() * 1000u; }
int IntelModeset::SetRate(char*, int, uint32_t hz) {
    action("intelmodeset.rate", 0, hz, nullptr, nullptr, nullptr);
    return 1;
}
int IntelModeset::ApplySavedRate() { return 0; }
void IntelModeset::PublishOutputRate() {}

// ---- network (satoru)
const char* WiFi::WirelessChipName() { return info_str("wifi.chip", ""); }
WiFiState WiFi::GetState() { return (WiFiState)info_int("wifi.state", WIFI_OFF); }
WiFi::LinkKind WiFi::DetectedLink() { return info_int("wifi.detected", 0) ? LINK_WIFI : LINK_ETHERNET; }
const char* WiFi::StateString() {
    static const char* names[] = { "Off", "Scanning", "Connecting", "Connected", "Disconnected", "Error" };
    int s = info_int("wifi.state", 0);
    return s >= 0 && s < 6 ? names[s] : "Off";
}
int WiFi::GetNetworkCount() { return info_int("wifi.count", 0); }
WiFiNetwork* WiFi::GetNetworks() {
    static WiFiNetwork n[NET_MAX_WIFI_NETS];
    // rebuilt on every call: the scan list changes while the page is open. (satoru)
    {
        int c = info_int("wifi.count", 0);
        if (c > NET_MAX_WIFI_NETS) c = NET_MAX_WIFI_NETS;
        for (int i = 0; i < c; i++) {
            char k[40];
            snprintf(k, sizeof(k), "wifi.%d.ssid", i); strncpy(n[i].ssid, info_str(k, ""), NET_MAX_SSID - 1); n[i].ssid[NET_MAX_SSID - 1] = 0;
            snprintf(k, sizeof(k), "wifi.%d.sig", i); n[i].signal_strength = info_int(k, -70);
            snprintf(k, sizeof(k), "wifi.%d.sec", i); n[i].security = (WiFiSecurity)info_int(k, 0);
            snprintf(k, sizeof(k), "wifi.%d.conn", i); n[i].connected = info_int(k, 0) != 0;
        }
    }
    return n;
}
bool WiFi::Enable() { action("wifi.enable", 0, 1, nullptr, nullptr, nullptr); return true; }
bool WiFi::Disable() { action("wifi.disable", 0, 0, nullptr, nullptr, nullptr); return true; }
bool WiFi::Scan() { action("wifi.scan", 0, 0, nullptr, nullptr, nullptr); return true; }
bool WiFi::Disconnect() { action("wifi.disconnect", 0, 0, nullptr, nullptr, nullptr); return true; }
bool WiFi::Connect(const char* ssid, const char* pass) {
    action("wifi.connect", 0, 0, nullptr, ssid, pass);
    return true;
}
WiFiNetwork* WiFi::GetConnectedNetwork() {
    int n = WiFi::GetNetworkCount();
    WiFiNetwork* w = WiFi::GetNetworks();
    for (int i = 0; i < n; i++) if (w[i].connected) return &w[i];
    return nullptr;
}
int WiFi::GetSignalStrength() { return info_int("wifi.signal", -100); }
int WiFi::SignalBars() { return info_int("wifi.bars", 0); }
bool WiFi::AutoReconnect() { return info_int("wifi.autoreconnect", 1) != 0; }
void WiFi::SetAutoReconnect(bool) {}
void WiFi::Tick() {}
int WiFi::SavedCount() { return info_int("wifi.saved.count", 0); }
bool WiFi::SavedAt(int i, char* ssid, int cap, int* security, bool* autojoin) {
    if (i < 0 || i >= SavedCount() || cap < 1) return false;
    char k[40];
    snprintf(k, sizeof(k), "wifi.saved.%d.ssid", i);
    strncpy(ssid, info_str(k, ""), (size_t)cap - 1); ssid[cap - 1] = 0;
    snprintf(k, sizeof(k), "wifi.saved.%d.sec", i);
    if (security) *security = info_int(k, 3);
    snprintf(k, sizeof(k), "wifi.saved.%d.auto", i);
    if (autojoin) *autojoin = info_int(k, 1) != 0;
    return true;
}
bool WiFi::Forget(const char* ssid) { return action_ok("wifi.forget", 0, ssid, nullptr); }
bool WiFi::SetAutoJoin(const char* ssid, bool on) { return action_ok("wifi.autojoin", on ? 1 : 0, ssid, nullptr); }
bool WiFi::KldBridge() { return info_int("wifi.kld", 0) != 0; }
int Network::GetInterfaceCount() { return info_int("net.count", 0); }
NetworkInterface* Network::GetInterfaces() {
    static NetworkInterface n[NET_MAX_INTERFACES];
    static bool done = false;
    if (!done) {
        done = true;
        int c = info_int("net.count", 0);
        if (c > NET_MAX_INTERFACES) c = NET_MAX_INTERFACES;
        for (int i = 0; i < c; i++) {
            char k[40];
            snprintf(k, sizeof(k), "net.%d.name", i); strncpy(n[i].name, info_str(k, ""), 15); n[i].name[15] = 0;
            snprintf(k, sizeof(k), "net.%d.type", i); n[i].type = (NICType)info_int(k, NIC_NONE);
            snprintf(k, sizeof(k), "net.%d.state", i); n[i].state = (NICState)info_int(k, NIC_DOWN);
            snprintf(k, sizeof(k), "net.%d.ip", i); {
                const char* ip = info_str(k, "0.0.0.0");
                unsigned a = 0, b = 0, c2 = 0, d = 0;
                sscanf(ip, "%u.%u.%u.%u", &a, &b, &c2, &d);
                n[i].ip.bytes[0] = (unsigned char)a; n[i].ip.bytes[1] = (unsigned char)b;
                n[i].ip.bytes[2] = (unsigned char)c2; n[i].ip.bytes[3] = (unsigned char)d;
            }
            snprintf(k, sizeof(k), "net.%d.gw", i); {
                const char* ip = info_str(k, "0.0.0.0");
                unsigned a = 0, b = 0, c2 = 0, d = 0;
                sscanf(ip, "%u.%u.%u.%u", &a, &b, &c2, &d);
                n[i].gateway.bytes[0] = (unsigned char)a; n[i].gateway.bytes[1] = (unsigned char)b;
                n[i].gateway.bytes[2] = (unsigned char)c2; n[i].gateway.bytes[3] = (unsigned char)d;
            }
            snprintf(k, sizeof(k), "net.%d.dns", i); {
                const char* ip = info_str(k, "0.0.0.0");
                unsigned a = 0, b = 0, c2 = 0, d = 0;
                sscanf(ip, "%u.%u.%u.%u", &a, &b, &c2, &d);
                n[i].dns.bytes[0] = (unsigned char)a; n[i].dns.bytes[1] = (unsigned char)b;
                n[i].dns.bytes[2] = (unsigned char)c2; n[i].dns.bytes[3] = (unsigned char)d;
            }
            snprintf(k, sizeof(k), "net.%d.rx_packets", i); n[i].rx_packets = (unsigned)info_int(k, 0);
            snprintf(k, sizeof(k), "net.%d.tx_packets", i); n[i].tx_packets = (unsigned)info_int(k, 0);
            snprintf(k, sizeof(k), "net.%d.rx_bytes", i); n[i].rx_bytes = (unsigned)info_int(k, 0);
            snprintf(k, sizeof(k), "net.%d.tx_bytes", i); n[i].tx_bytes = (unsigned)info_int(k, 0);
        }
    }
    return n;
}
NetworkInterface* Network::GetInterface(const char* name) {
    NetworkInterface* n = GetInterfaces();
    int c = GetInterfaceCount();
    for (int i = 0; i < c && i < NET_MAX_INTERFACES; i++) if (!strcmp(n[i].name, name)) return &n[i];
    return nullptr;
}
void Network::IPToString(IPv4Address ip, char* out, int max) {
    if (!out || max <= 0) return;
    snprintf(out, (size_t)max, "%u.%u.%u.%u", ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
}
IPv4Address Network::MakeIP(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
    IPv4Address ip;
    ip.bytes[0] = a; ip.bytes[1] = b; ip.bytes[2] = c; ip.bytes[3] = d;
    return ip;
}
void Network::GetDNSServers(uint32_t* primary, uint32_t* secondary) {
    if (primary) *primary = (uint32_t)info_int("net.dns1", 0);
    if (secondary) *secondary = (uint32_t)info_int("net.dns2", 0);
}

// ---- dhcp status read through the settings service (satoru)
bool Dhcp::IsBound() { return info_int("dhcp.bound", 0) != 0; }
const char* Dhcp::StateString() { return info_str("dhcp.state", "idle"); }
uint32_t Dhcp::LeaseSeconds() { return 0; }
uint32_t Dhcp::LeaseRemainingSeconds() { return (uint32_t)info_int("dhcp.lease_left", 0); }
uint32_t Dhcp::GetServer() { return (uint32_t)info_int("dhcp.server", 0); }
uint32_t Dhcp::GetPrimaryDNS() { return (uint32_t)info_int("dhcp.dns1", 0); }
uint32_t Dhcp::GetSecondaryDNS() { return (uint32_t)info_int("dhcp.dns2", 0); }

// ---- nic counters for the status rows (satoru)
bool E1000::IsDetected() { return info_int("net.detected", 0) != 0; }
uint32_t E1000::GetRxBytes() { return (uint32_t)info_int("net.rx_bytes", 0); }
uint32_t E1000::GetTxBytes() { return (uint32_t)info_int("net.tx_bytes", 0); }
uint32_t E1000::GetRxCount() { return (uint32_t)info_int("net.rx_packets", 0); }
uint32_t E1000::GetTxCount() { return (uint32_t)info_int("net.tx_packets", 0); }

// ---- drivers, usb, users, sessions (satoru)
LinuxDriver* LinuxDriverFramework::GetDrivers() {
    static LinuxDriver d[48];
    static bool done = false;
    if (!done) {
        done = true;
        int n = info_int("drv.count", 0);
        if (n > 48) n = 48;
        for (int i = 0; i < n; i++) {
            char k[40];
            snprintf(k, sizeof(k), "drv.%d.name", i); strncpy(d[i].name, info_str(k, ""), LDRV_MAX_NAME - 1);
            snprintf(k, sizeof(k), "drv.%d.desc", i); strncpy(d[i].description, info_str(k, ""), LDRV_MAX_DESC - 1);
            snprintf(k, sizeof(k), "drv.%d.ver", i); strncpy(d[i].version, info_str(k, ""), sizeof(d[i].version) - 1);
            snprintf(k, sizeof(k), "drv.%d.cat", i); d[i].category = (LinuxDriverCategory)info_int(k, LDRV_CAT_OTHER);
            snprintf(k, sizeof(k), "drv.%d.state", i); d[i].state = (LinuxDriverState)info_int(k, LDRV_UNLOADED);
            snprintf(k, sizeof(k), "drv.%d.bound", i); d[i].bound = info_int(k, 0) != 0;
        }
    }
    return d;
}
int LinuxDriverFramework::GetDriverCount() { return info_int("drv.count", 0); }
bool USB::IsDetected() { return info_int("usb.detected", 0) != 0; }
int USB::GetPortCount() { return info_int("usb.ports", 0); }
int USB::GetDeviceCount() { return info_int("usb.count", 0); }
const USBDeviceInfo* USB::GetDevice(int i) {
    static USBDeviceInfo d[16];
    static bool done = false;
    if (!done) {
        done = true;
        int n = info_int("usb.count", 0);
        if (n > 16) n = 16;
        for (int k = 0; k < n; k++) {
            char key[40];
            snprintf(key, sizeof(key), "usb.%d.conn", k); d[k].connected = info_int(key, 0) != 0;
            snprintf(key, sizeof(key), "usb.%d.man", k); strncpy(d[k].manufacturer, info_str(key, ""), sizeof(d[k].manufacturer) - 1);
            snprintf(key, sizeof(key), "usb.%d.prod", k); strncpy(d[k].product, info_str(key, ""), sizeof(d[k].product) - 1);
            snprintf(key, sizeof(key), "usb.%d.port", k); d[k].port = (uint8_t)info_int(key, 0);
            snprintf(key, sizeof(key), "usb.%d.speed", k); d[k].speed = (uint8_t)info_int(key, 0);
        }
    }
    return i >= 0 && i < 16 ? &d[i] : nullptr;
}
const char* USB::SpeedName(uint8_t speed) {
    switch (speed) {
    case 1: return "Low speed";
    case 2: return "Full speed";
    case 3: return "High speed";
    case 4: return "SuperSpeed";
    default: return "Unknown";
    }
}
User UserManager::users[16];
int UserManager::user_count = 0;
int UserManager::current_user = 0;
int UserManager::GetUserCount() { return info_int("user.count", 0); }
int UserManager::GetCurrentUserIndex() { return info_int("user.cur", 0); }
const char* UserManager::GetCurrentUsername() {
    static char b[32];
    int i = GetCurrentUserIndex();
    snprintf(b, sizeof(b), "user.%d.name", i);
    return info_str(b, "user");
}
const char* UserManager::GetCurrentDisplayName() {
    static char b[32];
    int i = GetCurrentUserIndex();
    snprintf(b, sizeof(b), "user.%d.display", i);
    return info_str(b, "");
}
bool UserManager::IsUsernameValid(const char* u) {
    if (!u) return false;
    int n = (int)strlen(u);
    if (n < 3 || n > 31) return false;
    if (!(u[0] >= 'a' && u[0] <= 'z')) return false;
    for (int i = 0; i < n; i++) {
        char c = u[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}
bool UserManager::IsUsernameTaken(const char* u) {
    int n = GetUserCount();
    for (int i = 0; i < n; i++) {
        char k[32];
        snprintf(k, sizeof(k), "user.%d.name", i);
        if (!strcmp(info_str(k, ""), u)) return true;
    }
    return false;
}
bool UserManager::AddUser(const char* username, const char* password) {
    action("user.add", 0, 0, nullptr, username, password);
    return true;
}
bool UserManager::SetAdmin(const char* username, bool admin) {
    return action_ok("user.setadmin", admin ? 1 : 0, username, nullptr);
}
// old and new travel as "old\nnew" in one value field (128 bytes). (satoru)
bool UserManager::ChangePassword(const char* username, const char* old_pw, const char* new_pw) {
    char v[KSS_VAL_MAX];
    int p = 0;
    for (int i = 0; old_pw && old_pw[i] && p < KSS_VAL_MAX - 2; i++) v[p++] = old_pw[i];
    v[p++] = '\n';
    for (int i = 0; new_pw && new_pw[i] && p < KSS_VAL_MAX - 1; i++) v[p++] = new_pw[i];
    v[p] = 0;
    bool ok = action_ok("user.passwd", 0, username, v);
    volatile char* w = v;
    for (int i = 0; i < KSS_VAL_MAX; i++) w[i] = 0;
    return ok;
}
// the ksa switch: the kernel service runs the real KEN::SetKsaEnabled; the service's reason
// text comes back through err. the password rides in the value field and is wiped. (satoru)
bool KEN::IsKsaEnabled() { return info_int("ksa.on", 1) != 0; }
bool KEN::SetKsaEnabled(int, bool enable, const char* password, char* err, int err_max) {
    KssReq r = req_init(KSS_ACTION);
    req_str(r.action, sizeof(r.action), "ksa.set");
    req_str(r.val, sizeof(r.val), password);
    r.uval = enable ? 1 : 0;
    KssReply rep;
    rep.ok = 0;
    rep.why[0] = 0;
    bool got = rpc(r, &rep, nullptr);
    volatile char* v = (volatile char*)r.val;
    for (unsigned i = 0; i < sizeof(r.val); i++) v[i] = 0;
    if (err && err_max > 0) {
        const char* m = got ? rep.why : "the settings service did not answer";
        int i = 0;
        while (m[i] && i < err_max - 1) { err[i] = m[i]; i++; }
        err[i] = 0;
    }
    return got && rep.ok != 0;
}
int KEN::GetCurrentSession() { return info_int("ken.level", -1) >= 0 ? 0 : -1; }
KENLevel KEN::GetLevel(int) { return (KENLevel)info_int("ken.level", 0); }
KENSession* KEN::GetSession(int i) {
    static KENSession s[KEN_MAX_SESSIONS];
    static bool done = false;
    if (!done) {
        done = true;
        int n = info_int("ken.sessions", 0);
        if (n > KEN_MAX_SESSIONS) n = KEN_MAX_SESSIONS;
        for (int k = 0; k < n; k++) s[k].active = true;
    }
    return i >= 0 && i < KEN_MAX_SESSIONS ? &s[i] : nullptr;
}

// ---- system, packages, kmemx (satoru)
KLSState KLS::GetState() { return (KLSState)info_int("kls.state", KLS_STOPPED); }
void KLS::Start() { action("kls.start", 0, 0, nullptr, nullptr, nullptr); }
void KLS::Stop() { action("kls.stop", 0, 0, nullptr, nullptr, nullptr); }
const char* KLS::GetKernelVersion() { return info_str("kls.kernel", "unknown"); }
int KLS::LinuxProcessCount() { return info_int("kls.procs", 0); }
KLSConfig* KLS::GetConfig() {
    static KLSConfig c;
    static bool done = false;
    if (!done) {
        done = true;
        strncpy(c.hostname, info_str("hostname", "kurono"), sizeof(c.hostname) - 1);
        strncpy(c.linux_root, "/", sizeof(c.linux_root) - 1);
        strncpy(c.default_shell, "/bin/sh", sizeof(c.default_shell) - 1);
        c.share_home = true;
        c.share_tmp = true;
        c.auto_mount_ext4 = true;
    }
    return &c;
}
const char* LinuxKernel::GetVersion() { return info_str("linux.version", "4.19"); }
bool SystemUpdate::HasPendingUpdate() { return info_int("update.pending", 0) != 0; }
const char* PackageManager::GetLastSyncMessage() { return info_str("pkg.sync", "Never"); }
const char* PackageManager::GetRepositoryHost() { return info_str("pkg.repo", "kurono.satorut.com"); }
int PackageManager::InstalledCount() { return info_int("pkg.installed", 0); }
int PackageManager::AvailableCount() { return info_int("pkg.available", 0); }
int PackageManager::GetPendingUpdateCount() { return info_int("pkg.pending", 0); }
bool PackageManager::SyncRepository() { action("pkg.sync", 0, 0, nullptr, nullptr, nullptr); return true; }

bool KMemX::IsEnabled() { return info_int("kmemx.enabled", 0) != 0; }
int KMemX::PoolPct() { return info_int("kmemx.pool_pct", 20); }
int KMemX::Threshold() { return info_int("kmemx.threshold", 6); }
bool KMemX::Enable() { action("kmemx.enable", 0, 1, nullptr, nullptr, nullptr); return true; }
int KMemX::Disable() { action("kmemx.disable", 0, 0, nullptr, nullptr, nullptr); return 0; }
bool KMemX::SetPoolPct(int pct) { action("kmemx.pool", pct, 0, nullptr, nullptr, nullptr); return true; }
void KMemX::SetThreshold(int gen) { action("kmemx.threshold", gen, 0, nullptr, nullptr, nullptr); }
void KMemX::WriteConfFile() {}
const KMemX::Stats& KMemX::GetStats() {
    static Stats s;
    s.pool_used = (uint64_t)info_ll("kmemx.pool_used", 0);
    s.pool_bytes = (uint64_t)info_ll("kmemx.pool_bytes", 0);
    s.bytes_saved = (uint64_t)info_ll("kmemx.saved", 0);
    return s;
}
int KMemX::RatioX100() { return info_int("kmemx.ratio", 100); }

// ---- the linux vm (satoru)
static const VMHost::DistroDesc kAlpine = {
    "alpine", "Alpine Linux", "3.20", "ALPINE  IMG", "INITRD  IMG", "ALPINE  IMG",
    "ALPINE.IMG", 1024, 0, "A small, fast Linux"
};
static const VMHost::DistroDesc kDebian = {
    "debian", "Debian", "12", "DEBIAN  IMG", "INITRD  IMG", "DEBIAN  IMG",
    "DEBIAN.IMG", 2048, 0, "A full desktop Linux"
};
int VMHost::DistroCount() { return info_int("vm.distro_count", 2); }
const VMHost::DistroDesc* VMHost::GetDistro(int i) {
    if (i == 0) return &kAlpine;
    if (i == 1) return &kDebian;
    return nullptr;
}
int VMHost::DistroIndex(const char* id) {
    if (id && !strcmp(id, "debian")) return 1;
    if (id && !strcmp(id, "alpine")) return 0;
    return -1;
}
bool VMHost::Enabled() { return cfg_bool("linux.enabled", info_int("vm.enabled", 1) != 0); }
int VMHost::SelectedDistro() {
    const char* d = cfg_get("linux.distro", nullptr);
    if (d) return DistroIndex(d);
    int i = info_int("vm.distro", 0);
    return i == 1 ? 1 : 0;
}
uint32_t VMHost::ConfiguredRamMb() {
    int v = cfg_int("linux.ram_mb", info_int("vm.ram_mb", 1024));
    return v > 0 ? (uint32_t)v : 1024;
}
int VMHost::DistroInstalled(int i) {
    char k[24];
    snprintf(k, sizeof(k), "vmd.%d.inst", i);
    return info_int(k, i == 0 ? 0 : 1);
}
bool VMHost::RamFromCmdline() { return info_int("vm.ram_cmdline", 0) != 0; }
void VMHost::AutostartAtBoot() {}
void VMHost::GetInfo(Info* out) {
    if (!out) return;
    static char err[64];
    memset(out, 0, sizeof(*out));
    out->state = info_int("vm.live_state", info_int("vm.state", 0));
    out->distro = info_int("vm.live_distro", info_int("vm.distro", 0));
    out->uptime_ms = (uint32_t)info_int("vm.uptime", 0);
    out->ram_mb = (uint32_t)info_int("vm.live_ram", 0);
    out->agent = info_int("vm.agent_live", 0) != 0;
    out->start_pending = info_int("vm.start_pending", 0) != 0;
    out->loop_stopped = info_int("vm.loop_stopped", 0) != 0;
    strncpy(err, info_str("vm.error", ""), sizeof(err) - 1);
    out->error = err;
}
void VMHost::Restart() { action("vm.restart", 0, 0, nullptr, nullptr, nullptr); }
bool VMHost::Start(int, uint32_t) { action("vm.start", 0, 0, nullptr, nullptr, nullptr); return true; }
void VMHost::Stop() { action("vm.stop", 0, 0, nullptr, nullptr, nullptr); }
void VMHost::Kill() { action("vm.kill", 0, 0, nullptr, nullptr, nullptr); }
VMHost::State VMHost::GetState() { return (State)info_int("vm.live_state", info_int("vm.state", 0)); }
const char* VMHost::StateName() { return "Stopped"; }
const char* VMHost::DistroName() { return GetDistro(SelectedDistro())->name; }
const char* VMHost::LastError() { return info_str("vm.error", ""); }
uint32_t VMHost::UptimeMs() { return (uint32_t)info_int("vm.uptime", 0); }
uint64_t VMHost::SliceCount() { return 0; }
uint32_t VMHost::Generation() { return (uint32_t)info_int("vm.generation", 0); }
bool VMHost::AgentReady() { return info_int("vm.agent_live", 0) != 0; }
bool VMHost::AgentSend(const char* line) { action("vm.agent", 0, 0, line, nullptr, nullptr); return true; }
int VMHost::AgentLastLine(char*, int) { return 0; }
int VMHost::Exec(const char*, char*, int, uint32_t, int*) { return -1; }
bool VMHost::WaitAgent(uint32_t) { return false; }
int VMHost::Status(char* out, int max) { if (out && max) out[0] = 0; return 0; }
bool VMM::IsSupported() { return info_int("vm.hv", 0) != 0; }
bool Hypervisor::IsAvailable() { return info_int("vm.hv", 0) != 0; }
int DiskLog::GuestImage(const char*, uint64_t*, uint64_t* bytes) {
    if (bytes) *bytes = (uint64_t)info_ll("vm.img_bytes", 0);
    return info_int("vm.img_why", 1);
}
int AlpineTerminal::Open() { action("open.alpine", 0, 0, nullptr, nullptr, nullptr); return 0; }

// ---- scheduler and kernel-only helpers (satoru)
bool IntelModeset::GetOutput(uint64_t* phys, int* w, int* h, uint32_t* pitch) {
    if (!info_int("dp.lit", 0)) return false;
    if (phys) *phys = (uint64_t)info_ll("dp.phys", 0);
    if (w) *w = info_int("dp.w", 0);
    if (h) *h = info_int("dp.h", 0);
    if (pitch) *pitch = (uint32_t)info_int("dp.pitch", 0);
    return true;
}
bool Scheduler::IsPreemptiveKernelSchedulerActive() { return false; }
Process* Scheduler::SpawnKernelProcess(const char*, KernelProcessEntry, ProcessPriorityTier, uint32_t, uint32_t) { return nullptr; }
void Scheduler::SleepMs(uint32_t) {}

// end (satoru)
