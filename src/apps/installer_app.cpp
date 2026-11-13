// installer_app.cpp: the graphical Kurono installer. one source builds as the
// ring-3 app /system/apps/installer and as the in-kernel fallback. the drawing
// and the flow are shared; disk work goes through installer_service (kernel) or
// its unix socket (ring 3). (satoru)
#include "installer_app.h"
#include "../kui/kui.h"
#include "../system/installer_proto.h"

#if defined(KUI_BACKEND_WAYLAND)
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#else
#include "../system/installer_service.h"
#endif

namespace {

const uint64_t MIB = 1024ull * 1024ull;

int sl(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
void scpy(char* d, const char* s, int mx) {
    if (!d || mx < 1) return;
    int i = 0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
bool pin(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void fmt_size(uint64_t bytes, char* out, int mx) {
    uint64_t v = bytes;
    const char* unit = "B";
    if (v >= 1024ull * MIB * 1024ull) { v = bytes / (1024ull * MIB * 1024ull); unit = "TB"; }
    else if (v >= 1024ull * MIB) { v = bytes / (1024ull * MIB); unit = "GB"; }
    else if (v >= MIB) { v = bytes / MIB; unit = "MB"; }
    else if (v >= 1024) { v = bytes / 1024; unit = "KB"; }
    char t[24];
    int tn = 0;
    if (v == 0) t[tn++] = '0';
    while (v) { t[tn++] = (char)('0' + (v % 10)); v /= 10; }
    int p = 0;
    while (tn && p < mx - 1) out[p++] = t[--tn];
    if (p < mx - 1) out[p++] = ' ';
    while (*unit && p < mx - 1) out[p++] = *unit++;
    out[p] = 0;
}

// copy a disk model without the padding spaces ata/nvme identify leaves (satoru)
void model_cpy(char* d, const char* s, int mx) {
    scpy(d, s, mx);
    int n = sl(d);
    while (n > 0 && (d[n - 1] == ' ' || d[n - 1] == '\t')) d[--n] = 0;
}

const char* action_text(uint32_t m) {
    if (m == KIN_PLAN_CREATE) return "New GPT";
    if (m == KIN_PLAN_APPEND) return "Added to GPT";
    if (m == KIN_PLAN_CONVERT) return "MBR converted to GPT";
    if (m == KIN_PLAN_EXISTING) return "Unchanged";
    return "None";
}

#if defined(KUI_BACKEND_WAYLAND)

bool rpc(uint32_t kind, int disk, uint32_t confirm, uint32_t conv_ack, uint32_t keep_ack,
         int kfs_mb, KinReply* rep, void* out, int out_size, uint32_t os_ack = 0) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, KIN_PATH, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }
    KinReq req;
    memset(&req, 0, sizeof(req));
    req.magic = KIN_MAGIC;
    req.kind = kind;
    req.disk = disk;
    req.confirm = confirm;
    req.convert_ack = conv_ack;
    req.keep_ack = keep_ack;
    req.kfs_mb = kfs_mb;
    req.os_ack = os_ack;

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
    int wait = (kind == KIN_START) ? 75000 : 8000;
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, wait) <= 0) { close(fd); return false; }

    int got = 0;
    uint8_t* q = (uint8_t*)rep;
    while (got < (int)sizeof(*rep)) {
        ssize_t n = recv(fd, q + got, sizeof(*rep) - (size_t)got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += (int)n;
    }
    if (got != (int)sizeof(*rep) || rep->magic != KIN_REPLY_MAGIC) {
        close(fd);
        return false;
    }
    if (out && out_size > 0) {
        int tg = 0;
        uint8_t* t = (uint8_t*)out;
        while (tg < out_size) {
            ssize_t n = recv(fd, t + tg, (size_t)(out_size - tg), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            tg += (int)n;
        }
        if (tg != out_size) { close(fd); return false; }
    }
    close(fd);
    return true;
}

bool svc_list(KinList* out) {
    KinReply rep;
    return rpc(KIN_LIST, 0, 0, 0, 0, 0, &rep, out, (int)sizeof(*out)) && rep.ok;
}
bool svc_plan(int disk, KinPlan* out, char* why, int why_cap) {
    KinReply rep;
    bool t = rpc(KIN_PLAN, disk, 0, 0, 0, 0, &rep, out, (int)sizeof(*out));
    if (!t) { scpy(why, "the installer service is not answering", why_cap); return false; }
    if (!rep.ok) scpy(why, rep.why, why_cap);
    return rep.ok;
}
bool svc_status(KinStatus* out) {
    KinReply rep;
    return rpc(KIN_STATUS, 0, 0, 0, 0, 0, &rep, out, (int)sizeof(*out)) && rep.ok;
}
bool svc_start(int disk, uint32_t conv_ack, uint32_t keep_ack, uint32_t os_ack, char* why, int why_cap) {
    KinReply rep;
    if (!rpc(KIN_START, disk, KIN_CONFIRM, conv_ack, keep_ack, 0, &rep, nullptr, 0, os_ack)) {
        scpy(why, "the installer service is not answering", why_cap);
        return false;
    }
    if (!rep.ok) scpy(why, rep.why, why_cap);
    return rep.ok;
}

#else

bool svc_list(KinList* out) { return InstallerService::List(out) >= 0; }

bool svc_plan(int disk, KinPlan* out, char* why, int why_cap) {
    int rc = InstallerService::Plan(disk, 0, out);
    if (rc != 0 && out->why[0]) scpy(why, out->why, why_cap);
    return out->ok != 0;
}

bool svc_status(KinStatus* out) { return InstallerService::Status(out) == 0; }

bool svc_start(int disk, uint32_t conv_ack, uint32_t keep_ack, uint32_t os_ack, char* why, int why_cap) {
    KinReq req;
    memset(&req, 0, sizeof(req));
    req.magic = KIN_MAGIC;
    req.kind = KIN_START;
    req.disk = disk;
    req.confirm = KIN_CONFIRM;
    req.convert_ack = conv_ack;
    req.keep_ack = keep_ack;
    req.os_ack = os_ack;
    KinReply rep;
    if (InstallerService::Start(req, 0, 1000, &rep) != 0) {
        scpy(why, rep.why[0] ? rep.why : "the installer refused the request", why_cap);
        return false;
    }
    return true;
}

#endif

// ---- motion (satoru)
// springs are integrated in small fixed steps and sin is a polynomial, so no
// libm is needed on either backend. tweens take an explicit start time and
// honour reduced motion. (satoru)

bool g_reduced = false;

float clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
float ease_out(float t) { t = clamp01(t); float u = 1.0f - t; return 1.0f - u * u * u; }
float ease_in(float t) { t = clamp01(t); return t * t; }
float ease_io(float t) { t = clamp01(t); return t < 0.5f ? 4.0f * t * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) / 2.0f; }
float ease_back(float t) {
    t = clamp01(t);
    const float c1 = 1.45f, c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
float tween(uint32_t now, uint32_t t0, int delay, int dur) {
    if (g_reduced) return 1.0f;
    int el = (int)(int32_t)(now - t0) - delay;
    if (el <= 0) return 0.0f;
    if (el >= dur) return 1.0f;
    return (float)el / (float)dur;
}
float fsqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float g = x > 1.0f ? x * 0.5f : 1.0f;
    for (int i = 0; i < 14; i++) g = 0.5f * (g + x / g);
    return g;
}
float fabsf_(float v) { return v < 0.0f ? -v : v; }
int iround(float v) { return v < 0.0f ? (int)(v - 0.5f) : (int)(v + 0.5f); }

// sin on any angle: reduce to [-pi, pi], then a 7th order odd polynomial (satoru)
float fsin(float x) {
    const float PI = 3.14159265f, TAU = 6.28318531f;
    int k = (int)(x / TAU);
    x -= (float)k * TAU;
    if (x > PI) x -= TAU;
    if (x < -PI) x += TAU;
    if (x > PI / 2) x = PI - x;
    else if (x < -PI / 2) x = -PI - x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}
float fcos(float x) { return fsin(x + 1.5707963f); }

struct Spr { float x, v, t; };

void spr_set(Spr& s, float v) { s.x = v; s.v = 0.0f; s.t = v; }

// response: seconds per undamped swing; damping 1 = critical (satoru)
void spr_step(Spr& s, float response, float damping, float dt) {
    if (g_reduced) { s.x = s.t; s.v = 0.0f; return; }
    if (dt > 0.05f) dt = 0.05f;
    float w0 = 6.2831853f / response;
    float k = w0 * w0, c = 2.0f * damping * w0;
    while (dt > 0.0f) {
        float h = dt > 0.004f ? 0.004f : dt;
        float a = -k * (s.x - s.t) - c * s.v;
        s.v += a * h;
        s.x += s.v * h;
        dt -= h;
    }
    if (fabsf_(s.x - s.t) < 0.0015f && fabsf_(s.v) < 0.02f) { s.x = s.t; s.v = 0.0f; }
}

bool spr_busy(const Spr& s) { return s.x != s.t || s.v != 0.0f; }

uint32_t argb_mix(uint32_t a, uint32_t b, float t) { return kui::Mix(a, b, iround(clamp01(t) * 255.0f)); }

// ---- screens and controls (satoru)

enum Screen {
    S_WELCOME = 0,
    S_DISKS,
    S_LAYOUT,
    S_CONFIRM,
    S_PROGRESS,
    S_DONE,
    S_ERROR,
    S_COUNT,
};

const char* const kScreenName[S_COUNT] = {
    "welcome", "disks", "layout", "confirm", "progress", "done", "error",
};

// how long a page's own entrance runs; the whole content redraws until then (satoru)
const int kEntryMs[S_COUNT] = { 1500, 700, 700, 700, 900, 1600, 800 };

enum CtlId {
    C_PRIMARY = 0,
    C_BACK,
    C_RESCAN,
    C_ACK1,
    C_ACK2,
    C_ACK3,
    C_DISK0,
    C_COUNT = C_DISK0 + KIN_MAX_DISKS,
};

struct Ctl {
    int x, y, w, h;
    bool used, enabled, en_init;
    Spr hov, prs, en, chk;
};

// friendly status lines keyed by the service's real percentage marks
// (installer_disk.cpp: table 4, partition 12, format 18, loaders 22..50, data
// files 50..94, verify 97, complete 100). (satoru)
const char* status_for(uint32_t pct, bool done) {
    if (done) return "Finishing up";
    if (pct < 12) return "Preparing the disk";
    if (pct < 22) return "Formatting the partition";
    if (pct < 50) return "Copying Kurono";
    if (pct < 94) return "Setting up storage";
    return "Checking the installation";
}

struct Colors {
    uint32_t text, text2, text3, accent, accent_hi, ok, warn, danger, gold, gold2, white;
    uint32_t glass, glass_hi, line;    // translucent white or black, used with alpha (satoru)
    int glass_a, glass_hi_a, line_a;
    uint32_t track;
    int track_a;
    bool dark;
};

const int kMaxW = 1600, kMaxH = 1200;

struct InstallerApp : kui::AppHandler {
    KinList list;
    KinPlan plan;
    KinStatus st;
    int sel = 0;
    bool list_ok = false;
    bool plan_ok = false;
    char err[192];
    uint32_t t_scan = 0;
    uint32_t t_plan = 0;
    uint32_t t_poll = 0;
    bool conv_ack = false;
    bool keep_ack = false;
    bool os_ack = false;   // second confirmation for a disk that holds another os (satoru)
    bool auto_mode = false;
    uint32_t auto_t = 0;
    int auto_step = 0;

    // page flow (satoru)
    int screen = S_WELCOME;
    int from = S_WELCOME;
    int dir = 1;
    bool out_phase = false;
    uint32_t t_out = 0;
    uint32_t page_t0 = 0;
    Spr pgx;          // incoming page x offset (satoru)
    Spr dots;         // step dots position (satoru)
    Spr dotv;         // step dots visibility (satoru)
    Spr sely;         // disk selection highlight row (satoru)
    Spr grow;         // the new KURONO segment, 0..1 (satoru)
    Spr pv;           // displayed install percentage (satoru)
    Spr shake;        // error badge shake (satoru)
    uint32_t last_tick = 0;

    // install state (satoru)
    bool pending_start = false;
    bool started = false;
    bool done_seen = false;
    uint32_t done_seen_t = 0;
    uint32_t t_started = 0;
    const char* stat_cur = nullptr;
    const char* stat_prev = nullptr;
    uint32_t stat_t = 0;
    float eta_s = -1.0f;

    // input (satoru)
    Ctl ctl[C_COUNT];
    int hover = -1;
    int press = -1;
    int focus = C_PRIMARY;
    bool kbd_vis = false;
    int last_px = -1, last_py = -1;
    int order[C_COUNT];
    int order_n = 0;

    // pre-rendered pixels: the backdrop and two glow sprites (satoru)
    uint32_t* bg = nullptr;
    int bg_w = 0, bg_h = 0;
    uint32_t bg_key = 0;
    uint32_t* bg_blur = nullptr;  // the backdrop blurred, what glass refracts (satoru)
    struct Glass { uint32_t* px; int cap; uint32_t key; };
    Glass gl[C_COUNT];
    uint32_t* glow_a = nullptr;   // accent glow, premultiplied (satoru)
    uint32_t* glow_g = nullptr;   // success glow, premultiplied (satoru)
    int glow_n = 0;
    uint32_t glow_key = 0;

    // frame state used by the draw helpers (satoru)
    kui::Canvas* cv = nullptr;
    Colors K;
    uint32_t flat_bg = 0;
    bool modern = true;
    int fx = 0, fy = 0, fd = 255;
    int page_fd = 255, page_x = 0;
    int lw = 760, lh = 560;
    uint32_t now_f = 0;

    InstallerApp() : list{}, plan{}, st{} { for (int i = 0; i < C_COUNT; i++) gl[i] = Glass{ nullptr, 0, 0 }; }
    ~InstallerApp() override {
        if (bg) kui::Free(bg);
        if (bg_blur) kui::Free(bg_blur);
        for (int i = 0; i < C_COUNT; i++) if (gl[i].px) kui::Free(gl[i].px);
        if (glow_a) kui::Free(glow_a);
        if (glow_g) kui::Free(glow_g);
    }

    // ---- colours (satoru)

    void resolve_colors() {
        const kui::Theme& T = kui::CurrentTheme();
        const kui::Palette& P = T.pal;
        modern = T.style == kui::Style::Modern;
        g_reduced = T.reduced_motion;
        K.dark = P.dark;
        K.white = 0xFFFFFFFFu;
        K.text = P.dark ? 0xFFF4F5F8u : 0xFF1D1D22u;
        K.text2 = P.dark ? 0xFFA6A9B6u : 0xFF6B6E78u;
        K.text3 = P.dark ? 0xFF6E717Eu : 0xFFA0A3AEu;
        K.accent = P.accent | 0xFF000000u;
        K.accent_hi = kui::Mix(K.accent, 0xFFFFFFFFu, 40);
        K.ok = P.dark ? 0xFF34C759u : 0xFF2BB04Cu;
        K.warn = P.dark ? 0xFFFFB340u : 0xFFD98A00u;
        K.danger = P.danger | 0xFF000000u;
        K.gold = 0xFFF0C25Au;
        K.gold2 = 0xFFC78A2Cu;
        if (P.dark) {
            K.glass = 0xFFFFFFFFu; K.glass_a = 16;
            K.glass_hi = 0xFFFFFFFFu; K.glass_hi_a = 26;
            K.line = 0xFFFFFFFFu; K.line_a = 22;
            K.track = 0xFFFFFFFFu; K.track_a = 26;
        } else {
            K.glass = 0xFFFFFFFFu; K.glass_a = 170;
            K.glass_hi = 0xFFFFFFFFu; K.glass_hi_a = 235;
            K.line = 0xFF000000u; K.line_a = 18;
            K.track = 0xFF000000u; K.track_a = 22;
        }
        flat_bg = P.panel;
    }

    // ---- pre-rendered pixels (satoru)

    // the window backdrop: a soft vertical gradient lit by three wide colour
    // blooms, computed per pixel once (no banding), with a little dither (satoru)
    void build_backdrop(int w, int h) {
        uint32_t key = (uint32_t)w * 7919u + (uint32_t)h * 31u + (K.dark ? 1u : 2u) + (K.accent << 3) + (modern ? 0u : 5u);
        if (bg && key == bg_key && bg_w == w && bg_h == h) return;
        if (w <= 0 || h <= 0 || w > kMaxW || h > kMaxH) return;
        if (bg && (bg_w != w || bg_h != h)) { kui::Free(bg); bg = nullptr; }
        if (!bg) bg = (uint32_t*)kui::Alloc((size_t)w * (size_t)h * 4);
        if (!bg) return;
        bg_w = w; bg_h = h; bg_key = key;
        float top[3], bot[3];
        uint32_t t0 = K.dark ? 0xFF1E2030u : 0xFFF6F7FBu;
        uint32_t b0 = K.dark ? 0xFF111219u : 0xFFE9EBF2u;
        if (!modern) t0 = b0 = flat_bg;
        top[0] = (float)((t0 >> 16) & 255); top[1] = (float)((t0 >> 8) & 255); top[2] = (float)(t0 & 255);
        bot[0] = (float)((b0 >> 16) & 255); bot[1] = (float)((b0 >> 8) & 255); bot[2] = (float)(b0 & 255);
        struct Bloom { float x, y, r, a; uint32_t c; };
        Bloom bl[3] = {
            { 0.18f, 0.08f, 0.62f, K.dark ? 0.30f : 0.16f, K.accent },
            { 0.92f, 0.18f, 0.48f, K.dark ? 0.18f : 0.10f, 0xFF8A63FFu },
            { 0.70f, 1.05f, 0.60f, K.dark ? 0.14f : 0.12f, K.gold },
        };
        int nb = modern ? 3 : 0;
        float scale = (float)(w > h ? w : h);
        for (int y = 0; y < h; y++) {
            float ty = (float)y / (float)(h > 1 ? h - 1 : 1);
            uint32_t* row = bg + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                float c[3] = { top[0] + (bot[0] - top[0]) * ty, top[1] + (bot[1] - top[1]) * ty,
                               top[2] + (bot[2] - top[2]) * ty };
                for (int b = 0; b < nb; b++) {
                    float dx = ((float)x - bl[b].x * (float)w) / (bl[b].r * scale);
                    float dy = ((float)y - bl[b].y * (float)h) / (bl[b].r * scale);
                    float d2 = dx * dx + dy * dy;
                    if (d2 >= 1.0f) continue;
                    float f = (1.0f - d2);
                    f = f * f * bl[b].a;
                    c[0] += ((float)((bl[b].c >> 16) & 255) - c[0]) * f;
                    c[1] += ((float)((bl[b].c >> 8) & 255) - c[1]) * f;
                    c[2] += ((float)(bl[b].c & 255) - c[2]) * f;
                }
                // ordered dither, a quarter step, hides 8-bit steps in the dark gradient (satoru)
                float dth = (float)(((x & 1) ^ ((y & 1) << 1)) - 1) * 0.35f;
                int r = iround(c[0] + dth), g = iround(c[1] + dth), bb = iround(c[2] + dth);
                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                bb = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
                row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bb;
            }
        }
        build_blur();
        for (int i = 0; i < C_COUNT; i++) gl[i].key = 0;
    }

    // two separable box passes of radius 14 over the backdrop (close to a gaussian) (satoru)
    void build_blur() {
        int w = bg_w, h = bg_h;
        if (bg_blur) kui::Free(bg_blur);
        bg_blur = (uint32_t*)kui::Alloc((size_t)w * (size_t)h * 4);
        uint32_t* tmp = (uint32_t*)kui::Alloc((size_t)w * (size_t)h * 4);
        if (!bg_blur || !tmp) { if (tmp) kui::Free(tmp); return; }
        for (int i = 0; i < w * h; i++) bg_blur[i] = bg[i];
        const int R = 14;
        for (int pass = 0; pass < 2; pass++) {
            for (int dirn = 0; dirn < 2; dirn++) {
                uint32_t* src = dirn == 0 ? bg_blur : tmp;
                uint32_t* dst = dirn == 0 ? tmp : bg_blur;
                int len = dirn == 0 ? w : h, lines = dirn == 0 ? h : w;
                for (int l = 0; l < lines; l++) {
                    int sr = 0, sg = 0, sb = 0;
                    auto at = [&](int i) -> uint32_t {
                        if (i < 0) i = 0;
                        if (i >= len) i = len - 1;
                        return dirn == 0 ? src[(size_t)l * w + i] : src[(size_t)i * w + l];
                    };
                    for (int i = -R; i <= R; i++) { uint32_t c = at(i); sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255; }
                    for (int i = 0; i < len; i++) {
                        int n = 2 * R + 1;
                        uint32_t o = 0xFF000000u | ((uint32_t)(sr / n) << 16) | ((uint32_t)(sg / n) << 8) | (uint32_t)(sb / n);
                        if (dirn == 0) dst[(size_t)l * w + i] = o; else dst[(size_t)i * w + l] = o;
                        uint32_t a = at(i + R + 1), b = at(i - R);
                        sr += (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
                        sg += (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
                        sb += (int)(a & 255) - (int)(b & 255);
                    }
                }
            }
        }
        kui::Free(tmp);
    }

    uint32_t blur_at(int x, int y) const {
        const uint32_t* b = bg_blur ? bg_blur : bg;
        if (!b || bg_w <= 0) return flat_bg;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= bg_w) x = bg_w - 1;
        if (y >= bg_h) y = bg_h - 1;
        return b[(size_t)y * bg_w + x];
    }

    // liquid glass: a rounded slab showing the blurred backdrop behind it, pulled
    // in toward the edges like a lens, tinted, with a bright rim along the top,
    // a fainter one along the bottom and a soft inner glow. the pixels are cached
    // per control and rebuilt only when its rect, tint or press state changes. (satoru)
    void glass(int id, int x, int y, int w, int h, int r, uint32_t tint, float tint_amt, float press, float hover) {
        if (w <= 4 || h <= 4 || A(255) <= 0) return;
        int X = x + fx, Y = y + fy;
        if (!modern || !bg) {
            rr(x, y, w, h, r, argb_mix(K.dark ? 0xFF2A2C36u : 0xFFFFFFFFu, tint, tint_amt));
            ro(x, y, w, h, r, K.line, K.line_a + 20);
            return;
        }
        Glass& g = gl[id];
        int ta = iround(tint_amt * 40.0f), pq = iround(press * 8.0f);
        uint32_t key = (uint32_t)X * 73856093u ^ (uint32_t)Y * 19349663u ^ (uint32_t)w * 83492791u ^
                       (uint32_t)h * 2654435761u ^ tint ^ (uint32_t)(ta << 4) ^ (uint32_t)(pq << 12) ^ (uint32_t)r * 97u ^ 1u;
        if (g.key != key || !g.px) {
            if (g.cap < w * h) {
                if (g.px) kui::Free(g.px);
                g.px = (uint32_t*)kui::Alloc((size_t)w * (size_t)h * 4);
                g.cap = g.px ? w * h : 0;
            }
            if (!g.px) return;
            g.key = key;
            float hw = (float)w * 0.5f, hh = (float)h * 0.5f, rf = (float)r;
            float tr = (float)((tint >> 16) & 255), tg = (float)((tint >> 8) & 255), tb = (float)(tint & 255);
            float rim_shift = press * 0.12f;
            for (int py = 0; py < h; py++) {
                for (int px = 0; px < w; px++) {
                    float cx = (float)px + 0.5f - hw, cy = (float)py + 0.5f - hh;
                    float qx = fabsf_(cx) - (hw - rf), qy = fabsf_(cy) - (hh - rf);
                    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
                    float inside = qx > qy ? qx : qy;
                    float d = fsqrt(ox * ox + oy * oy) + (inside < 0.0f ? inside : 0.0f) - rf;
                    float cov = clamp01(0.5f - d);
                    if (cov <= 0.0f) { g.px[(size_t)py * w + px] = 0; continue; }
                    // e: 0 at the rim, 1 a radius in (satoru)
                    float e = clamp01(-d / (rf > 1.0f ? rf : 1.0f));
                    float lens = (1.0f - e) * (1.0f - e) * 0.30f;
                    int sx = X + iround((float)px - cx * lens), sy = Y + iround((float)py - cy * lens);
                    uint32_t c = blur_at(sx, sy);
                    float cr = (float)((c >> 16) & 255), cg = (float)((c >> 8) & 255), cb = (float)(c & 255);
                    float lift = K.dark ? 0.045f : 0.04f;
                    cr += (255.0f - cr) * lift; cg += (255.0f - cg) * lift; cb += (255.0f - cb) * lift;
                    cr += (tr - cr) * tint_amt; cg += (tg - cg) * tint_amt; cb += (tb - cb) * tint_amt;
                    float vy = (float)py / (float)h;
                    float edge = (1.0f - e);
                    float e2 = edge * edge;
                    float e6 = e2 * e2 * e2 * e2 * edge * edge;   // a thin rim (satoru)
                    float top = clamp01(1.0f - (vy + rim_shift) * 2.2f);
                    float bot = clamp01((vy - 0.55f) * 2.2f);
                    float glowv = e2 * e2 * (K.dark ? 0.10f : 0.07f);
                    float spec = e6 * (0.80f * top + 0.22f * bot) + glowv + top * top * 0.03f;
                    cr += (255.0f - cr) * clamp01(spec); cg += (255.0f - cg) * clamp01(spec); cb += (255.0f - cb) * clamp01(spec);
                    uint32_t a = (uint32_t)iround(cov * 255.0f);
                    uint32_t R8 = (uint32_t)iround(clamp01(cr / 255.0f) * (float)a);
                    uint32_t G8 = (uint32_t)iround(clamp01(cg / 255.0f) * (float)a);
                    uint32_t B8 = (uint32_t)iround(clamp01(cb / 255.0f) * (float)a);
                    g.px[(size_t)py * w + px] = (a << 24) | (R8 << 16) | (G8 << 8) | B8;
                }
            }
        }
        // a faint outer shadow, then the slab, then the hover sheen (satoru)
        cv->SoftShadow((float)X, (float)Y + 3.0f, (float)w, (float)h, (float)r, 10.0f, 3.0f, A(K.dark ? 70 : 34));
        cv->BlitPremul(X, Y, g.px, w, h, w, A(255));
        if (hover > 0.01f) {
            rr(x + 2, y + 1, w - 4, h / 2, r - 1, 0xFFFFFFFFu, iround(hover * 18.0f));
            ro(x, y, w, h, r, 0xFFFFFFFFu, iround(hover * 50.0f));
        }
    }

    uint32_t bg_at(int x, int y) const {
        if (!bg || bg_w <= 0) return flat_bg;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= bg_w) x = bg_w - 1;
        if (y >= bg_h) y = bg_h - 1;
        return bg[(size_t)y * bg_w + x];
    }

    // a radial glow sprite, premultiplied, smooth to the edge (satoru)
    void build_glow(uint32_t*& buf, uint32_t c) {
        int n = glow_n;
        if (!buf) buf = (uint32_t*)kui::Alloc((size_t)n * (size_t)n * 4);
        if (!buf) return;
        float R = (float)n * 0.5f;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                float dx = ((float)x + 0.5f - R) / R, dy = ((float)y + 0.5f - R) / R;
                float d2 = dx * dx + dy * dy;
                float a = d2 >= 1.0f ? 0.0f : (1.0f - d2);
                a = a * a * a;
                uint32_t A = (uint32_t)iround(a * 255.0f);
                uint32_t r = (((c >> 16) & 255) * A) / 255, g = (((c >> 8) & 255) * A) / 255, b = ((c & 255) * A) / 255;
                buf[(size_t)y * n + x] = (A << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    void build_glows() {
        uint32_t key = K.accent ^ (K.ok << 1) ^ (K.dark ? 0x55u : 0xAAu);
        if (glow_n && key == glow_key && glow_a && glow_g) return;
        glow_n = 260;
        glow_key = key;
        build_glow(glow_a, K.accent);
        build_glow(glow_g, K.ok);
    }

    void glow(uint32_t* spr, int cx, int cy, int a) {
        if (!modern || !spr || A(a) <= 0) return;
        cv->BlitPremul(cx + fx - glow_n / 2, cy + fy - glow_n / 2, spr, glow_n, glow_n, glow_n, A(a));
    }

    // ---- draw helpers: shapes fade by alpha, text fades toward the backdrop under it (satoru)

    int Rd(int r, int w, int h) const {
        if (modern || 2 * r >= w || 2 * r >= h) return r;
        return r > 4 ? 4 : r;
    }
    int A(int a) const { int v = a * fd / 255; return v < 0 ? 0 : (v > 255 ? 255 : v); }

    void rr(int x, int y, int w, int h, int r, uint32_t c, int a = 255) {
        if (w <= 0 || h <= 0 || A(a) <= 0) return;
        cv->RoundRect(x + fx, y + fy, w, h, Rd(r, w, h), c, A(a));
    }
    void rrg(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int a = 255) {
        if (w <= 0 || h <= 0 || A(a) <= 0) return;
        if (!modern) top = bot;
        cv->RoundRect(x + fx, y + fy, w, h, Rd(r, w, h), top, bot, A(a));
    }
    void ro(int x, int y, int w, int h, int r, uint32_t c, int a) {
        if (w <= 2 || h <= 2 || A(a) <= 0) return;
        cv->RoundOutline(x + fx, y + fy, w, h, Rd(r, w, h), c, A(a));
    }
    void circ(int cx, int cy, int r, uint32_t c, int a = 255) {
        if (r <= 0 || A(a) <= 0) return;
        cv->Circle(cx + fx, cy + fy, r, c, A(a));
    }
    void shadow(int x, int y, int w, int h, int r, int a) {
        if (!modern || A(a) <= 0) return;
        cv->SoftShadow((float)(x + fx), (float)(y + fy), (float)w, (float)h, (float)r, 14.0f, 5.0f, A(a));
    }
    uint32_t T(uint32_t c, int x, int y) const {
        if (fd >= 255) return c;
        return kui::Mix(bg_at(x + fx, y + fy + 8), c, fd < 0 ? 0 : fd);
    }
    int tx(int x, int y, kui::Face f, float px, const char* s, uint32_t c) {
        if (fd <= 0) return kui::MeasureWidth(f, px, s);
        return cv->Text(x + fx, y + fy, f, px, s, T(c, x, y));
    }
    void txc(int cx, int y, kui::Face f, float px, const char* s, uint32_t c) {
        if (fd <= 0) return;
        cv->TextCenter(cx + fx, y + fy, f, px, s, T(c, cx, y));
    }
    int txf(int x, int y, kui::Face f, float px, const char* s, uint32_t c, int maxw) {
        if (fd <= 0) return 0;
        return cv->TextFit(x + fx, y + fy, f, px, s, T(c, x, y), maxw);
    }
    int txr(int rx, int y, kui::Face f, float px, const char* s, uint32_t c) {
        int w = kui::MeasureWidth(f, px, s);
        tx(rx - w, y, f, px, s, c);
        return w;
    }

    // a stroke of anti-aliased dots along a polyline, drawn up to `frac` of its length (satoru)
    void stroke(const int* xs, const int* ys, int n, float frac, int r, uint32_t c, int a) {
        if (n < 2 || frac <= 0.0f || A(a) <= 0) return;
        float total = 0.0f;
        float seg[8];
        for (int i = 0; i + 1 < n && i < 8; i++) {
            float dx = (float)(xs[i + 1] - xs[i]), dy = (float)(ys[i + 1] - ys[i]);
            seg[i] = fsqrt(dx * dx + dy * dy);
            total += seg[i];
        }
        float want = total * clamp01(frac);
        float walked = 0.0f;
        for (int i = 0; i + 1 < n && i < 8; i++) {
            float L = seg[i];
            if (L <= 0.0f) continue;
            float step = r > 1 ? 0.75f : 0.5f;
            for (float d = 0.0f; d <= L; d += step) {
                if (walked + d > want) return;
                float t = d / L;
                int px = iround((float)xs[i] + (float)(xs[i + 1] - xs[i]) * t);
                int py = iround((float)ys[i] + (float)(ys[i + 1] - ys[i]) * t);
                cv->Circle(px + fx, py + fy, r, c, A(a));
            }
            walked += L;
        }
    }

    void check(int x, int y, int s, float t, uint32_t c, int r) {
        int xs[3] = { x + s * 27 / 100, x + s * 44 / 100, x + s * 74 / 100 };
        int ys[3] = { y + s * 52 / 100, y + s * 68 / 100, y + s * 35 / 100 };
        stroke(xs, ys, 3, t, r, c, 255);
    }

    // ---- controls (satoru)

    Ctl& reg(int id, int x, int y, int w, int h, bool enabled) {
        Ctl& k = ctl[id];
        k.x = x + fx; k.y = y + fy; k.w = w; k.h = h;
        k.used = true;
        k.enabled = enabled;
        if (!k.en_init) { spr_set(k.en, enabled ? 1.0f : 0.0f); k.en_init = true; }
        k.en.t = enabled ? 1.0f : 0.0f;
        if (order_n < C_COUNT) order[order_n++] = id;
        return k;
    }

    void focus_ring(int id, int x, int y, int w, int h, int r) {
        if (!kbd_vis || focus != id) return;
        ro(x - 4, y - 4, w + 8, h + 8, r + 4, K.accent, 150);
        ro(x - 3, y - 3, w + 6, h + 6, r + 3, K.accent, 220);
    }

    // pill buttons: the primary is the accent, the secondary is frosted glass (satoru)
    void button(int id, int x, int y, int w, int h, const char* label, bool primary, bool enabled) {
        Ctl& k = reg(id, x, y, w, h, enabled);
        float hv = k.hov.x, pr = k.prs.x, en = clamp01(k.en.x);
        // press squishes the pill to about 0.96 on a spring (satoru)
        float sc = 1.0f - pr * 0.04f;
        int bw = iround((float)w * sc), bh = iround((float)h * sc);
        int bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
        int r = bh / 2;
        if (primary) {
            float amt = (K.dark ? 0.62f : 0.70f) * en + 0.06f;
            glass(id, bx, by, bw, bh, r, K.accent, amt, pr, hv * en);
            uint32_t tc = argb_mix(K.text3, 0xFFFFFFFFu, en);
            int tw = kui::MeasureWidth(kui::Face::SansBold, 14.0f, label);
            tx(bx + (bw - tw) / 2, by + (bh - 18) / 2, kui::Face::SansBold, 14.0f, label, tc);
        } else {
            glass(id, bx, by, bw, bh, r, K.dark ? 0xFFFFFFFFu : 0xFFFFFFFFu, K.dark ? 0.06f : 0.45f, pr, hv);
            int tw = kui::MeasureWidth(kui::Face::Sans, 14.0f, label);
            tx(bx + (bw - tw) / 2, by + (bh - 18) / 2, kui::Face::Sans, 14.0f, label, enabled ? K.text : K.text3);
        }
        focus_ring(id, x, y, w, h, h / 2);
    }

    // a rounded toggle row: the circle fills and the check draws on a bouncy spring (satoru)
    void checkbox(int id, int x, int y, int w, const char* text, bool on) {
        int h = 46;
        Ctl& k = reg(id, x, y, w, h, true);
        k.chk.t = on ? 1.0f : 0.0f;
        float hv = k.hov.x, c = k.chk.x, cc = clamp01(c);
        glass(id, x, y, w, h, 16, K.accent, cc * 0.10f + (K.dark ? 0.0f : 0.25f), 0.0f, hv);
        ro(x, y, w, h, 16, K.accent, iround(cc * 170.0f));
        int cx = x + 26, cy = y + h / 2;
        int r = 11 + iround((c - cc) * 8.0f);
        circ(cx, cy, 11, K.line, iround((1.0f - cc) * 80.0f));
        circ(cx, cy, 10, K.dark ? 0xFF1A1B22u : 0xFFFFFFFFu, iround((1.0f - cc) * 255.0f));
        if (cc > 0.01f) circ(cx, cy, iround((float)r * (0.6f + 0.4f * cc)), K.accent, iround(cc * 255.0f));
        if (cc > 0.05f) check(cx - 9, cy - 9, 18, cc * 1.15f, 0xFFFFFFFFu, 1);
        txf(x + 50, y + (h - 18) / 2, kui::Face::Sans, 14.0f, text, K.text, w - 64);
        focus_ring(id, x, y, w, h, 16);
    }

    // ---- glyphs (satoru)

    void drive_glyph(int x, int y, int s) {
        uint32_t top = K.dark ? 0xFFA9B0C0u : 0xFFD3D7E0u;
        uint32_t bot = K.dark ? 0xFF6A7184u : 0xFF9AA1B2u;
        int h = s * 7 / 10, yy = y + (s - h) / 2;
        rrg(x, yy, s, h, h / 3, top, bot);
        rr(x + 6, yy + h / 2 - 1, s - 20, 3, 1, 0xFF000000u, 70);
        circ(x + s - 9, yy + h / 2, 3, K.ok);
    }

    // the kurono mark: a dark disc mended with a gold seam (kintsugi) (satoru)
    void logo(int cx, int cy, int R, float seam, float glint, int a) {
        if (R < 4) return;
        int sfd = fd;
        fd = fd * a / 255;
        if (modern) {
            shadow(cx - R, cy - R, 2 * R, 2 * R, R, K.dark ? 110 : 60);
        }
        rrg(cx - R, cy - R, 2 * R, 2 * R, R, 0xFF3C3F4Du, 0xFF0F1015u);
        rr(cx - R * 6 / 10, cy - R * 9 / 10, R * 12 / 10, R * 7 / 10, R * 35 / 100, 0xFFFFFFFFu, 9);
        ro(cx - R, cy - R, 2 * R, 2 * R, R, 0xFFFFFFFFu, 26);
        int xs[8], ys[8];
        const int px[8] = { -84, -52, -40, -16, -10, 14, 26, 50 };
        const int py[8] = { -52, -36, -12, -6, 18, 26, 52, 86 };
        for (int i = 0; i < 8; i++) { xs[i] = cx + px[i] * R / 100; ys[i] = cy + py[i] * R / 100; }
        int sr = R / 34 + 1;
        stroke(xs, ys, 8, seam, sr + 1, K.gold2, 60);
        stroke(xs, ys, 8, seam, sr, K.gold, 255);
        int bx[4] = { cx - 13 * R / 100, cx + 16 * R / 100, cx + 30 * R / 100, cx + 62 * R / 100 };
        int by[4] = { cy + 6 * R / 100, cy - 10 * R / 100, cy - 30 * R / 100, cy - 70 * R / 100 };
        stroke(bx, by, 4, clamp01((seam - 0.45f) / 0.55f), sr, K.gold, 220);
        if (glint > 0.0f && glint < 1.0f) {
            float t = glint * 7.0f;
            int i = (int)t;
            if (i > 6) i = 6;
            float f = t - (float)i;
            int gx = iround((float)xs[i] + (float)(xs[i + 1] - xs[i]) * f);
            int gy = iround((float)ys[i] + (float)(ys[i + 1] - ys[i]) * f);
            int ga = iround(255.0f * (1.0f - fabsf_(glint * 2.0f - 1.0f)));
            circ(gx, gy, sr + 6, 0xFFFFF4D0u, ga / 5);
            circ(gx, gy, sr + 3, 0xFFFFF4D0u, ga / 2);
            circ(gx, gy, sr + 1, 0xFFFFFFFFu, ga);
        }
        fd = sfd;
    }

    // ---- the proportional disk bar (satoru)

    uint64_t disk_sectors() {
        const KinDisk& d = list.disks[sel];
        uint32_t ss = d.sector_size ? d.sector_size : 512;
        uint64_t n = d.bytes / ss;
        if (plan.part_last_lba + 1 > n) n = plan.part_last_lba + 1;
        for (uint32_t i = 0; i < plan.parts_n && i < KIN_MAX_PARTS; i++)
            if (plan.parts[i].last_lba + 1 > n) n = plan.parts[i].last_lba + 1;
        return n ? n : 1;
    }

    uint32_t part_color(const KinPart& p) {
        if (p.flags & KIN_PART_KURONO) return K.accent;
        if (p.flags & KIN_PART_ESP) return K.dark ? 0xFF5FC8B8u : 0xFF3FAE9Fu;
        if (p.flags & KIN_PART_OTHER_OS) return K.dark ? 0xFF8B97B8u : 0xFF8090B4u;
        return K.dark ? 0xFF8A8E9Au : 0xFFA4A8B4u;
    }

    bool new_part() const {
        return plan_ok && (plan.mode == KIN_PLAN_CREATE || plan.mode == KIN_PLAN_APPEND ||
                           plan.mode == KIN_PLAN_CONVERT);
    }

    int seg_x(uint64_t lba, uint64_t total, int x, int w) {
        return x + (int)((lba * (uint64_t)w) / total);
    }

    // segments are soft pills inside a rounded track; the new one grows from its start (satoru)
    void diskbar(int x, int y, int w, int h, float g) {
        uint64_t total = disk_sectors();
        int r = h / 2;
        rr(x, y, w, h, r, K.track, K.track_a);
        int pad = 3, ir = (h - 2 * pad) / 2;
        for (uint32_t i = 0; i < plan.parts_n && i < KIN_MAX_PARTS; i++) {
            const KinPart& p = plan.parts[i];
            int sx = seg_x(p.start_lba, total, x, w), ex = seg_x(p.last_lba + 1, total, x, w);
            if (sx < x + pad) sx = x + pad;
            if (ex > x + w - pad) ex = x + w - pad;
            if (ex - sx < 2 * ir) ex = sx + 2 * ir;
            uint32_t c = part_color(p);
            rrg(sx + 1, y + pad, ex - sx - 2, h - 2 * pad, ir, argb_mix(c, 0xFFFFFFFFu, 0.12f), c);
        }
        if (new_part()) {
            int sx = seg_x(plan.part_start_lba, total, x, w), ex = seg_x(plan.part_last_lba + 1, total, x, w);
            if (sx < x + pad) sx = x + pad;
            if (ex > x + w - pad) ex = x + w - pad;
            int full = ex - sx - 2;
            float gg = g < 0.0f ? 0.0f : g;
            int cw = iround((float)full * gg);
            if (cw > full + 4) cw = full + 4;
            if (cw >= 2 * ir || (cw > 2 && gg > 0.01f)) {
                if (cw < 2 * ir) cw = 2 * ir;
                shadow(sx + 1, y + pad, cw, h - 2 * pad, ir, iround(70.0f * clamp01(gg)));
                rrg(sx + 1, y + pad, cw, h - 2 * pad, ir, K.accent_hi, K.accent);
            }
        }
    }

    // a centred row of legend chips: a dot, a name and a size (satoru)
    void legend(int cx, int y) {
        struct Item { uint32_t c; char t[48]; bool strong; uint64_t lba; };
        Item it[KIN_MAX_PARTS + 1];
        int n = 0;
        auto add = [&](uint32_t c, const char* name, uint64_t bytes, bool strong, uint64_t lba) {
            if (n >= KIN_MAX_PARTS + 1) return;
            char sz[24];
            fmt_size(bytes, sz, sizeof(sz));
            scpy(it[n].t, name, 28);
            int k = sl(it[n].t);
            it[n].t[k++] = ' ';
            it[n].t[k++] = ' ';
            for (int i = 0; sz[i] && k < 46; i++) it[n].t[k++] = sz[i];
            it[n].t[k] = 0;
            it[n].c = c;
            it[n].strong = strong;
            it[n].lba = lba;
            n++;
        };
        if (new_part()) add(K.accent, "Kurono", plan.part_bytes, true, plan.part_start_lba);
        for (uint32_t i = 0; i < plan.parts_n && i < KIN_MAX_PARTS && n < 6; i++) {
            const KinPart& p = plan.parts[i];
            char nm[32];
            if (p.label[0]) scpy(nm, p.label, sizeof(nm));
            else if (p.flags & KIN_PART_ESP) scpy(nm, "EFI", sizeof(nm));
            else {
                scpy(nm, p.fstype[0] ? p.fstype : "Other", sizeof(nm));
                for (int k = 0; nm[k]; k++) if (nm[k] >= 'a' && nm[k] <= 'z') nm[k] = (char)(nm[k] - 32);
            }
            add(part_color(p), nm, p.bytes, false, p.start_lba);
        }
        // same order as the bar: by where each partition starts on the disk (satoru)
        for (int i = 1; i < n; i++) {
            Item t = it[i];
            int j = i - 1;
            while (j >= 0 && it[j].lba > t.lba) { it[j + 1] = it[j]; j--; }
            it[j + 1] = t;
        }
        int widths[KIN_MAX_PARTS + 1];
        int total = 0;
        for (int i = 0; i < n; i++) {
            widths[i] = 16 + kui::MeasureWidth(it[i].strong ? kui::Face::SansBold : kui::Face::Sans, 13.0f, it[i].t);
            total += widths[i] + (i ? 22 : 0);
        }
        int x = cx - total / 2;
        for (int i = 0; i < n; i++) {
            circ(x + 4, y + 9, 4, it[i].c);
            tx(x + 16, y, it[i].strong ? kui::Face::SansBold : kui::Face::Sans, 13.0f, it[i].t,
               it[i].strong ? K.text : K.text2);
            x += widths[i] + 22;
        }
    }

    // ---- service state (satoru)

    void set_error(const char* why) {
        scpy(err, why ? why : "the installer could not continue", sizeof(err));
    }

    bool refresh_list() {
        KinList l;
        memset(&l, 0, sizeof(l));
        if (!svc_list(&l)) {
            list_ok = false;
            return false;
        }
        list = l;
        list_ok = true;
        if (sel >= (int)list.count) sel = list.count ? (int)list.count - 1 : 0;
        return true;
    }

    bool refresh_plan() {
        if (!list.count) return false;
        KinPlan p;
        memset(&p, 0, sizeof(p));
        char why[96];
        why[0] = 0;
        bool ok = svc_plan(list.disks[sel].index, &p, why, sizeof(why));
        plan = p;
        auto_mode = p.auto_mode != 0;
        plan_ok = ok && p.ok && p.mode != KIN_PLAN_EXISTING;
        if (p.mode == KIN_PLAN_EXISTING) {
            set_error("Kurono is already installed on this disk.");
        } else if (!plan_ok) {
            set_error(why[0] ? why : (p.why[0] ? p.why : "no install plan for this disk"));
        } else {
            err[0] = 0;
        }
        return plan_ok;
    }

    bool need_keep() const { return plan.has_other_os || plan.parts_n > 0; }
    bool ready() const {
        return plan_ok && (!need_keep() || keep_ack) && (!plan.need_convert || conv_ack) &&
               (!plan.need_os_ack || os_ack);
    }

    // ---- page flow (satoru)

    void go(kui::Window& w, int s, int d) {
        uint32_t now = kui::NowMs();
        from = screen;
        screen = s;
        dir = d;
        hover = -1;
        press = -1;
        for (int i = 0; i < C_COUNT; i++) {
            spr_set(ctl[i].hov, 0.0f);
            spr_set(ctl[i].prs, 0.0f);
            ctl[i].en_init = false;
            ctl[i].used = false;
        }
        focus = (s == S_CONFIRM) ? (need_keep() ? C_ACK1 : (plan.need_convert ? C_ACK2 : (plan.need_os_ack ? C_ACK3 : C_BACK)))
                                 : (s == S_DISKS && list.count ? C_DISK0 + sel : C_PRIMARY);
        float step = s == S_DISKS ? 0.0f : (s == S_LAYOUT ? 1.0f : 2.0f);
        dots.t = step;
        dotv.t = (s == S_DISKS || s == S_LAYOUT || s == S_CONFIRM) ? 1.0f : 0.0f;
        if (g_reduced) {
            out_phase = false;
            page_t0 = now - 10000;
            spr_set(pgx, 0.0f);
            enter_page(now);
        } else {
            out_phase = true;
            t_out = now;
        }
        kui::Log("installer", kScreenName[s]);
        w.Invalidate();
    }

    void enter_page(uint32_t now) {
        if (screen == S_LAYOUT) spr_set(grow, 0.0f);
        if (screen == S_DISKS) spr_set(sely, (float)sel);
        if (screen == S_ERROR) { spr_set(shake, 0.0f); shake.v = g_reduced ? 0.0f : 260.0f; }
        if (screen == S_PROGRESS && !started) { spr_set(pv, 0.0f); stat_cur = nullptr; stat_prev = nullptr; eta_s = -1.0f; }
        (void)now;
    }

    void next_after_disks(kui::Window& w) {
        if (!list.count) return;
        refresh_plan();
        conv_ack = false;
        keep_ack = false;
        os_ack = false;
        go(w, S_LAYOUT, 1);
    }

    void begin_install(kui::Window& w) {
        if (!ready()) return;
        pending_start = true;
        started = false;
        done_seen = false;
        memset(&st, 0, sizeof(st));
        go(w, S_PROGRESS, 1);
    }

    // the blocking part: the service asks for administrator approval here (satoru)
    void start_install(kui::Window& w) {
        if (!plan_ok || !ready()) { go(w, S_CONFIRM, -1); return; }
        char why[160];
        why[0] = 0;
        if (!svc_start(list.disks[sel].index, conv_ack ? 1u : 0u, keep_ack ? 1u : 0u,
                       os_ack ? 1u : 0u, why, sizeof(why))) {
            set_error(why);
            started = false;
            go(w, S_ERROR, 1);
            return;
        }
        memset(&st, 0, sizeof(st));
        started = true;
        t_started = kui::NowMs();
        t_poll = 0;
        w.Invalidate();
    }

    void activate(kui::Window& w, int id) {
        if (id < 0 || id >= C_COUNT || !ctl[id].used || !ctl[id].enabled) return;
        if (id >= C_DISK0) {
            int i = id - C_DISK0;
            if (i < (int)list.count && i != sel) {
                sel = i;
                plan_ok = false;
                sely.t = (float)sel;
            }
            focus = id;
            return;
        }
        switch (screen) {
        case S_WELCOME:
            if (id == C_PRIMARY) { if (!list_ok) refresh_list(); go(w, S_DISKS, 1); }
            break;
        case S_DISKS:
            if (id == C_PRIMARY && list_ok && list.count) next_after_disks(w);
            else if (id == C_BACK) go(w, S_WELCOME, -1);
            else if (id == C_RESCAN) { refresh_list(); sely.t = (float)sel; w.Invalidate(); }
            break;
        case S_LAYOUT:
            if (id == C_PRIMARY && plan_ok) { conv_ack = false; keep_ack = false; os_ack = false; go(w, S_CONFIRM, 1); }
            else if (id == C_BACK) go(w, S_DISKS, -1);
            break;
        case S_CONFIRM:
            if (id == C_ACK1) keep_ack = !keep_ack;
            else if (id == C_ACK2) conv_ack = !conv_ack;
            else if (id == C_ACK3) os_ack = !os_ack;
            else if (id == C_BACK) go(w, S_LAYOUT, -1);
            // a double click on continue must not land on install: the button
            // only answers once the page has settled (satoru)
            else if (id == C_PRIMARY && ready() && (int32_t)(kui::NowMs() - page_t0) >= 700) begin_install(w);
            break;
        case S_DONE:
            if (id == C_PRIMARY) w.Close();
            break;
        case S_ERROR:
            if (id == C_PRIMARY) w.Close();
            else if (id == C_BACK) { refresh_list(); go(w, S_DISKS, -1); }
            break;
        default: break;
        }
    }

    // ---- invalidation: ring 3 repaints only what moved, paced by a timer (satoru)

    int anim_timer = 0;
    int anim_iv = 0;
    uint32_t next_frame = 0;

    void anim_start(kui::Window& w, int iv) {
        if (anim_timer && anim_iv == iv) return;
        if (anim_timer) kui::StopTimer(anim_timer);
        anim_timer = kui::StartTimer(&w, (uint32_t)iv, true);
        anim_iv = iv;
    }
    void anim_stop() {
        if (anim_timer) kui::StopTimer(anim_timer);
        anim_timer = 0;
        anim_iv = 0;
    }

    void inv(kui::Window& w, int x, int y, int ww, int hh) {
        if (kui::IsRing3()) {
            kui::Rect r = { x - 8, y - 8, ww + 16, hh + 16 };
            w.Invalidate(r);
        } else {
            w.Invalidate();
            w.RequestFrameAt(kui::NowMs() + 15);
        }
    }
    void inv_all(kui::Window& w) { inv(w, 0, 0, lw, lh); }
    void inv_ctl(kui::Window& w, int id) {
        if (id < 0 || id >= C_COUNT || !ctl[id].used) return;
        inv(w, ctl[id].x - 6, ctl[id].y - 6, ctl[id].w + 12, ctl[id].h + 16);
    }

    // ---- the pages (satoru)

    // a staggered entrance for one element: fade and a short rise (satoru)
    void el(int delay, int dur = 460) {
        if (out_phase) { fd = page_fd; fy = 0; return; }
        float e = ease_out(tween(now_f, page_t0, delay, dur));
        fd = page_fd * iround(e * 255.0f) / 255;
        fy = iround((1.0f - e) * 12.0f);
    }
    void el_reset() { fd = page_fd; fy = 0; }

    void heading(const char* t, const char* sub, int y = 64) {
        int cx = lw / 2;
        el(0);
        txc(cx, y, kui::Face::SansBold, 26.0f, t, K.text);
        if (sub) { el(60); txc(cx, y + 40, kui::Face::Sans, 14.0f, sub, K.text2); }
    }

    // footer: a frosted Back and the accent action, centred as a pair (satoru)
    void footer(const char* primary, bool enabled, bool back = true, const char* back_label = "Back") {
        el(140);
        int bh = 40, pw = 150, bw = 110, gap = 12;
        int y = lh - 72;
        int total = back ? bw + gap + pw : pw;
        int x = lw / 2 - total / 2;
        if (back) { button(C_BACK, x, y, bw, bh, back_label, false, true); x += bw + gap; }
        button(C_PRIMARY, x, y, pw, bh, primary, true, enabled);
        el_reset();
    }

    void page_welcome() {
        int cx = lw / 2;
        uint32_t t0 = page_t0;
        float gw = out_phase ? 1.0f : ease_out(tween(now_f, t0, 0, 900));
        fd = page_fd * iround(gw * 255.0f) / 255;
        fy = 0;
        glow(glow_a, cx, 196, 120);
        float sc = out_phase ? 1.0f : ease_back(tween(now_f, t0, 60, 760));
        float la = out_phase ? 1.0f : ease_out(tween(now_f, t0, 60, 360));
        float seam = out_phase ? 1.0f : ease_io(tween(now_f, t0, 460, 760));
        float glint = out_phase ? 1.0f : tween(now_f, t0, 1150, 700);
        fd = page_fd;
        int R = iround(54.0f * (0.7f + 0.3f * sc));
        logo(cx, 196, R, seam, glint, iround(la * 255.0f));
        el(520);
        txc(cx, 284, kui::Face::SansBold, 30.0f, "Install Kurono", K.text);
        el(600);
        txc(cx, 326, kui::Face::Sans, 15.0f, "Kintsugi", K.text2);
        float bp = out_phase ? 1.0f : ease_back(tween(now_f, t0, 820, 560));
        float ba = out_phase ? 1.0f : ease_out(tween(now_f, t0, 820, 300));
        fd = page_fd * iround(ba * 255.0f) / 255;
        fy = iround((1.0f - bp) * 10.0f);
        button(C_PRIMARY, cx - 85, lh - 96, 170, 44, "Continue", true, true);
        el_reset();
    }

    void disk_row(int i, int x, int y, int w, int h, const KinDisk& d, float tint) {
        Ctl& k = reg(C_DISK0 + i, x, y, w, h, true);
        float hv = k.hov.x, pr = k.prs.x;
        int ins = iround(pr * 3.0f);
        int bx = x + ins, by = y + ins, bw = w - 2 * ins, bh = h - 2 * ins;
        glass(C_DISK0 + i, bx, by, bw, bh, 18, K.accent, tint * 0.10f + (K.dark ? 0.0f : 0.25f) * (1.0f - tint), pr, hv);
        drive_glyph(bx + 18, by + (bh - 34) / 2, 34);
        char name[64];
        model_cpy(name, d.model[0] ? d.model : d.name, sizeof(name));
        char sub[64], sz[24];
        fmt_size(d.bytes, sz, sizeof(sz));
        scpy(sub, sz, sizeof(sub));
        int n = sl(sub);
        const char* sch = d.scheme == KIN_SCHEME_NONE ? ", empty" : (d.scheme == KIN_SCHEME_MBR ? ", MBR" : (d.scheme == KIN_SCHEME_GPT ? ", GPT" : ", damaged table"));
        // a short tag for the system already on the disk, or the one in use (satoru)
        if (d.flags & KIN_DISK_RUNNING) sch = ", in use";
        else if (d.flags & KIN_DISK_WINDOWS) sch = ", has Windows";
        else if (d.flags & KIN_DISK_MAC) sch = ", has Mac";
        else if (d.flags & KIN_DISK_LINUX) sch = ", has Linux";
        for (const char* s = sch; *s && n < 60; s++) sub[n++] = *s;
        sub[n] = 0;
        txf(bx + 66, by + bh / 2 - 19, kui::Face::SansBold, 15.0f, name, K.text, bw - 130);
        txf(bx + 66, by + bh / 2 + 2, kui::Face::Sans, 13.0f, sub, K.text2, bw - 130);
        // the selection mark: an empty ring, or an accent disc with a check (satoru)
        int cx = bx + bw - 30, cy = by + bh / 2;
        circ(cx, cy, 11, K.line, iround((1.0f - tint) * 90.0f));
        if (tint > 0.02f) {
            circ(cx, cy, iround(11.0f * (0.55f + 0.45f * tint)), K.accent, iround(tint * 255.0f));
            check(cx - 8, cy - 8, 16, tint, 0xFFFFFFFFu, 1);
        }
        focus_ring(C_DISK0 + i, bx, by, bw, bh, 18);
    }

    void page_disks() {
        int cx = lw / 2;
        heading("Select a disk", nullptr);
        int w = 460, x = cx - w / 2, y = 132, rh = 68, gap = 10;
        if (!list_ok || list.count == 0) {
            el(100);
            if (!list_ok) {
                spinner(cx, y + 60, 10, K.text2);
                txc(cx, y + 92, kui::Face::Sans, 14.0f, "Looking for disks", K.text2);
            } else {
                txc(cx, y + 50, kui::Face::Sans, 15.0f, "No disks found", K.text2);
                button(C_RESCAN, cx - 60, y + 86, 120, 36, "Rescan", false, true);
            }
        } else {
            int n = (int)list.count;
            if (n > KIN_MAX_DISKS) n = KIN_MAX_DISKS;
            int maxn = (lh - 100 - y) / (rh + gap);
            if (maxn < 1) maxn = 1;
            if (n > maxn) n = maxn;
            // the selection ring slides between rows (satoru)
            el(100);
            int sy = y + iround(sely.x * (float)(rh + gap));
            ro(x - 2, sy - 2, w + 4, rh + 4, 20, K.accent, 230);
            ro(x - 1, sy - 1, w + 2, rh + 2, 19, K.accent, 255);
            for (int i = 0; i < n; i++) {
                el(100 + i * 60);
                float tint = clamp01(1.0f - fabsf_(sely.x - (float)i));
                disk_row(i, x, y + i * (rh + gap), w, rh, list.disks[i], tint);
            }
        }
        footer("Continue", list_ok && list.count > 0);
    }

    void page_layout() {
        if (list.count == 0) return;
        int cx = lw / 2;
        const KinDisk& d = list.disks[sel];
        char sub[96], sz[24];
        model_cpy(sub, d.model[0] ? d.model : d.name, 60);
        fmt_size(d.bytes, sz, sizeof(sz));
        int n = sl(sub);
        sub[n++] = ',';
        sub[n++] = ' ';
        for (int i = 0; sz[i] && n < 94; i++) sub[n++] = sz[i];
        sub[n] = 0;
        heading("Disk layout", sub);
        int w = 520, x = cx - w / 2, y = 168;
        el(90);
        diskbar(x, y, w, 46, grow.x);
        el(180);
        legend(cx, y + 66);
        el(240);
        if (plan.mode == KIN_PLAN_EXISTING) {
            txc(cx, y + 120, kui::Face::Sans, 14.0f, "Kurono is already installed here.", K.ok);
        } else if (!plan_ok) {
            char why[192];
            scpy(why, err[0] ? err : "There is no room for Kurono on this disk.", sizeof(why));
            if (why[0] >= 'a' && why[0] <= 'z') why[0] = (char)(why[0] - 32);
            txc(cx, y + 120, kui::Face::Sans, 14.0f, why, K.danger);
        } else if (plan.os_line[0]) {
            txc(cx, y + 120, kui::Face::Sans, 14.0f, plan.os_line, K.warn);
        }
        footer("Continue", plan_ok);
    }

    void kv(int cx, int y, const char* k, const char* v) {
        txr(cx - 12, y, kui::Face::Sans, 14.0f, k, K.text2);
        tx(cx + 12, y, kui::Face::Sans, 14.0f, v, K.text);
    }

    void page_confirm() {
        if (list.count == 0) return;
        int cx = lw / 2;
        const KinDisk& d = list.disks[sel];
        heading("Ready to install", nullptr);
        char dl[64], psz[24], np[48];
        model_cpy(dl, d.model[0] ? d.model : d.name, sizeof(dl));
        fmt_size(plan.part_bytes, psz, sizeof(psz));
        scpy(np, psz, sizeof(np));
        int n = sl(np);
        const char* t = ", FAT32";
        while (*t && n < 46) np[n++] = *t++;
        np[n] = 0;
        int y = 150;
        el(80);
        kv(cx, y, "Disk", dl);
        el(120);
        kv(cx, y + 28, "Partition", np);
        el(160);
        kv(cx, y + 56, "Table", action_text(plan.mode));
        int w = 440, x = cx - w / 2;
        int wy = y + 108;
        if (need_keep()) {
            el(220);
            checkbox(C_ACK1, x, wy, w, "Existing partitions stay as they are", keep_ack);
            wy += 56;
        } else {
            ctl[C_ACK1].used = false;
        }
        if (plan.need_convert) {
            el(280);
            checkbox(C_ACK2, x, wy, w, "Convert this disk from MBR to GPT", conv_ack);
            wy += 56;
        } else {
            ctl[C_ACK2].used = false;
        }
        if (plan.need_os_ack) {
            el(340);
            txc(cx, wy + 4, kui::Face::Sans, 14.0f, plan.os_line[0] ? plan.os_line : "This disk has another system on it", K.warn);
            wy += 28;
            checkbox(C_ACK3, x, wy, w, "Change it anyway", os_ack);
            wy += 56;
        } else {
            ctl[C_ACK3].used = false;
            if (plan.os_line[0]) {
                el(340);
                txc(cx, wy + 4, kui::Face::Sans, 14.0f, plan.os_line, K.warn);
            }
        }
        footer("Install", ready());
    }

    void spinner(int cx, int cy, int rad, uint32_t c) {
        float a0 = (float)(now_f % 1000u) / 1000.0f * 6.2831853f;
        for (int k = 0; k < 10; k++) {
            float a = a0 - (float)k * 0.42f;
            circ(cx + iround(fcos(a) * (float)rad), cy + iround(fsin(a) * (float)rad), 2, c, 255 - k * 24);
        }
    }

    // the waiting screen: the mark breathes in a slow glow, a spark orbits it,
    // one thin bar eases toward the real percentage, the status cross-fades (satoru)
    void page_progress() {
        int cx = lw / 2, cy = 206;
        float tsec = (float)(now_f % 600000u) / 1000.0f;
        float breath = 0.5f + 0.5f * fsin(tsec * 6.2831853f / 3.2f);
        el(0, 600);
        glow(glow_a, cx, cy, 70 + iround(breath * 110.0f));
        int R = 50 + iround(breath * 1.5f);
        logo(cx, cy, R, 1.0f, 0.0f, 255);
        // the orbit: a bright head with a fading trail (satoru)
        if (modern) {
            float a0 = tsec * 6.2831853f / 2.6f;
            float orb = (float)R + 16.0f;
            for (int k = 0; k < 12; k++) {
                float a = a0 - (float)k * 0.085f;
                int ox = cx + iround(fcos(a) * orb), oy = cy + iround(fsin(a) * orb);
                int al = 230 - k * 19;
                circ(ox, oy, k == 0 ? 3 : 2, k < 2 ? 0xFFFFF4D0u : K.gold, al);
            }
        }
        el(120);
        txc(cx, 296, kui::Face::SansBold, 24.0f, "Installing Kurono", K.text);
        // status line: the old one slides up and out while the new one comes in (satoru)
        el(200);
        int sy = 336;
        float ct = stat_t ? ease_out(tween(now_f, stat_t, 0, 420)) : 1.0f;
        int sfd = fd;
        if (stat_prev && ct < 1.0f) {
            fd = sfd * iround((1.0f - ct) * 255.0f) / 255;
            int sfy = fy;
            fy = sfy - iround(ct * 8.0f);
            txc(cx, sy, kui::Face::Sans, 14.0f, stat_prev, K.text2);
            fy = sfy;
        }
        const char* cur = stat_cur ? stat_cur : (started ? "Preparing the disk" : "Waiting for approval");
        fd = sfd * iround(ct * 255.0f) / 255;
        {
            int sfy = fy;
            fy = sfy + iround((1.0f - ct) * 8.0f);
            txc(cx, sy, kui::Face::Sans, 14.0f, cur, K.text2);
            fy = sfy;
        }
        fd = sfd;
        // the bar (satoru)
        el(260);
        int bw = 300, bx = cx - bw / 2, by = 376, bh = 6;
        rr(bx, by, bw, bh, 3, K.track, K.track_a);
        float p = clamp01(pv.x / 100.0f);
        int fw = iround((float)bw * p);
        if (started && fw > 0) {
            if (fw < bh) fw = bh;
            rrg(bx, by, fw, bh, 3, K.accent_hi, K.accent);
        } else if (!started && fd > 0) {
            // indeterminate: a short pill gliding back and forth (satoru)
            float s = 0.5f + 0.5f * fsin(tsec * 6.2831853f / 1.8f);
            int pw = 70;
            rr(bx + iround(s * (float)(bw - pw)), by, pw, bh, 3, K.accent, 220);
        }
        // the time estimate (satoru)
        el(320);
        char eta[48];
        eta[0] = 0;
        if (started && !st.done) {
            if (eta_s < 0.0f) scpy(eta, "Estimating time", sizeof(eta));
            else if (eta_s >= 90.0f) {
                int m = iround(eta_s / 60.0f);
                scpy(eta, "About ", sizeof(eta));
                int k = sl(eta);
                if (m >= 10) eta[k++] = (char)('0' + (m / 10) % 10);
                eta[k++] = (char)('0' + m % 10);
                eta[k] = 0;
                const char* t = " minutes left";
                while (*t && k < 46) eta[k++] = *t++;
                eta[k] = 0;
            } else if (eta_s >= 45.0f) scpy(eta, "About a minute left", sizeof(eta));
            else if (eta_s >= 8.0f) scpy(eta, "Less than a minute left", sizeof(eta));
            else scpy(eta, "Almost done", sizeof(eta));
        }
        if (eta[0]) txc(cx, by + 22, kui::Face::Sans, 12.5f, eta, K.text3);
        el_reset();
    }

    void page_done() {
        int cx = lw / 2, cy = 190;
        uint32_t t0 = page_t0;
        float tb = out_phase ? 1.0f : tween(now_f, t0, 60, 700);
        float sc = out_phase ? 1.0f : ease_back(tb);
        float ck = out_phase ? 1.0f : ease_io(tween(now_f, t0, 420, 460));
        float ring = out_phase ? 1.0f : tween(now_f, t0, 260, 1000);
        fd = page_fd;
        fy = 0;
        glow(glow_g, cx, cy, iround(150.0f * clamp01(tb * 1.4f)));
        if (ring > 0.0f && ring < 1.0f) {
            float e = ease_out(ring);
            int rr_ = 48 + iround(e * 70.0f);
            int a = iround((1.0f - ring) * 140.0f);
            ro(cx - rr_, cy - rr_, 2 * rr_, 2 * rr_, rr_, K.ok, a);
        }
        // a few gold sparks drifting outward (satoru)
        float pt = out_phase ? 1.0f : tween(now_f, t0, 300, 1300);
        if (pt > 0.0f && pt < 1.0f && modern) {
            for (int i = 0; i < 12; i++) {
                float ang = (float)i * 0.5236f + 0.2f;
                float sp = 80.0f + (float)((i * 37) % 5) * 14.0f;
                float e = ease_out(pt);
                int px_ = cx + iround(fcos(ang) * sp * e);
                int py_ = cy + iround(fsin(ang) * sp * e + 30.0f * pt * pt);
                circ(px_, py_, 2 + (i % 3 == 0 ? 1 : 0), i % 2 ? K.gold : 0xFFFFF0C8u, iround(230.0f * (1.0f - ease_in(pt))));
            }
        }
        int R = iround(46.0f * sc);
        if (R > 2) {
            shadow(cx - R, cy - R, 2 * R, 2 * R, R, 70);
            rrg(cx - R, cy - R, 2 * R, 2 * R, R, argb_mix(K.ok, 0xFFFFFFFFu, 0.22f), K.ok);
            if (ck > 0.0f) check(cx - R, cy - R, 2 * R, ck, 0xFFFFFFFFu, R / 15 + 1);
        }
        el(560);
        txc(cx, 270, kui::Face::SansBold, 26.0f, "Kurono is installed", K.text);
        el(640);
        txc(cx, 310, kui::Face::Sans, 14.0f, "Restart and boot from this disk.", K.text2);
        el(720);
        button(C_PRIMARY, cx - 75, lh - 72, 150, 40, "Done", true, true);
        el_reset();
    }

    void page_error() {
        int cx = lw / 2, cy = 180;
        el(0);
        int ox = iround(shake.x);
        float sc = out_phase ? 1.0f : ease_back(tween(now_f, page_t0, 0, 560));
        int R = iround(40.0f * sc);
        if (R > 2) {
            shadow(cx + ox - R, cy - R, 2 * R, 2 * R, R, 60);
            rrg(cx + ox - R, cy - R, 2 * R, 2 * R, R, argb_mix(K.danger, 0xFFFFFFFFu, 0.2f), K.danger);
            int bw = R / 6 + 1;
            rr(cx + ox - bw / 2 - 1, cy - R / 2, bw + 2, R * 6 / 10, 2, 0xFFFFFFFFu);
            circ(cx + ox, cy + R * 36 / 100, bw / 2 + 2, 0xFFFFFFFFu);
        }
        el(120);
        txc(cx, 254, kui::Face::SansBold, 24.0f, started ? "Installation stopped" : "Not installed", K.text);
        el(200);
        char why[192];
        scpy(why, err[0] ? err : st.detail, sizeof(why));
        if (why[0] >= 'a' && why[0] <= 'z') why[0] = (char)(why[0] - 32);
        if (why[0]) txc(cx, 294, kui::Face::Sans, 14.0f, why, K.text2);
        footer("Close", true, true, "Start over");
    }

    // tiny step dots: the current one stretches into a pill (satoru)
    void step_dots() {
        float vis = clamp01(dotv.x);
        if (vis <= 0.01f) return;
        fd = iround(vis * 255.0f);
        fx = 0;
        fy = 0;
        int cx = lw / 2, y = 26, gap = 7;
        float s = dots.x;
        int wv[3], total = 0;
        float cur[3];
        for (int i = 0; i < 3; i++) {
            cur[i] = clamp01(1.0f - fabsf_(s - (float)i));
            wv[i] = 7 + iround(cur[i] * 15.0f);
            total += wv[i] + (i ? gap : 0);
        }
        int x = cx - total / 2;
        for (int i = 0; i < 3; i++) {
            rr(x, y - 3, wv[i], 7, 3, argb_mix(K.text3, K.accent, cur[i]), 150 + iround(cur[i] * 105.0f));
            x += wv[i] + gap;
        }
        fd = 255;
    }

    // ---- kui callbacks (satoru)

    void OnCreate(kui::Window& w) override {
        (void)w;
        memset(&list, 0, sizeof(list));
        memset(&plan, 0, sizeof(plan));
        memset(&st, 0, sizeof(st));
        memset(ctl, 0, sizeof(ctl));
        err[0] = 0;
        t_scan = 0;
        uint32_t now = kui::NowMs();
        page_t0 = now;
        last_tick = now;
        spr_set(pgx, 0.0f);
        spr_set(dots, 0.0f);
        spr_set(dotv, 0.0f);
        spr_set(sely, 0.0f);
        spr_set(grow, 0.0f);
        spr_set(pv, 0.0f);
        spr_set(shake, 0.0f);
        kui::Log("installer", "open");
    }

    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now) override {
        (void)w;
        cv = &c;
        now_f = now;
        lw = c.Width();
        lh = c.Height();
        resolve_colors();
        build_backdrop(lw, lh);
        build_glows();
        if (bg && bg_w == lw && bg_h == lh) c.Blit(0, 0, bg, lw, lh, lw);
        else c.Clear(flat_bg);
        order_n = 0;
        for (int i = 0; i < C_COUNT; i++) ctl[i].used = false;
        fx = 0; fy = 0; fd = 255;
        step_dots();
        // one page at a time: the old one leaves, then the new one springs in (satoru)
        int pg = screen;
        if (out_phase) {
            float k = ease_in(tween(now, t_out, 0, 160));
            page_fd = iround((1.0f - k) * 255.0f);
            page_x = iround(-(float)dir * 30.0f * k);
            pg = from;
        } else {
            float k = ease_out(tween(now, page_t0, 0, 280));
            page_fd = iround(k * 255.0f);
            page_x = iround(pgx.x);
        }
        fx = page_x;
        fd = page_fd;
        fy = 0;
        int saved = screen;
        screen = pg;
        switch (pg) {
        case S_WELCOME: page_welcome(); break;
        case S_DISKS: page_disks(); break;
        case S_LAYOUT: page_layout(); break;
        case S_CONFIRM: page_confirm(); break;
        case S_PROGRESS: page_progress(); break;
        case S_DONE: page_done(); break;
        default: page_error(); break;
        }
        screen = saved;
        fx = 0; fy = 0; fd = 255;
        cv = nullptr;
    }

    int hit(int x, int y) {
        for (int i = 0; i < C_COUNT; i++) {
            const Ctl& k = ctl[i];
            if (k.used && pin(x, y, k.x, k.y, k.w, k.h)) return i;
        }
        return -1;
    }

    void move_focus(kui::Window& w, int step) {
        if (order_n <= 0) return;
        int at = -1;
        for (int i = 0; i < order_n; i++) if (order[i] == focus) at = i;
        int start = at < 0 ? (step > 0 ? -1 : order_n) : at;
        for (int n = 0; n < order_n; n++) {
            start = (start + step + order_n) % order_n;
            int id = order[start];
            if (ctl[id].used && ctl[id].enabled) { focus = id; break; }
        }
        kbd_vis = true;
        inv_all(w);
    }

    bool OnEvent(kui::Window& w, const kui::Event& e) override {
        if (e.type == kui::EventType::Resize || e.type == kui::EventType::ThemeChanged) return true;
        bool busy = out_phase || (screen == S_PROGRESS);
        if (e.type == kui::EventType::PointerMove || e.type == kui::EventType::PointerDown) {
            int h = busy ? -1 : hit(e.x, e.y);
            if (h != hover) {
                inv_ctl(w, hover);
                inv_ctl(w, h);
                hover = h;
            }
            // only a real move hides the keyboard focus ring (the wm repeats moves) (satoru)
            bool moved = e.x != last_px || e.y != last_py;
            last_px = e.x;
            last_py = e.y;
            if (kbd_vis && moved) { kbd_vis = false; inv_all(w); }
            if (e.type == kui::EventType::PointerDown && e.button == kui::Button::Left && h >= 0 &&
                ctl[h].enabled) {
                press = h;
                inv_ctl(w, h);
            }
            return false;
        }
        if (e.type == kui::EventType::PointerUp) {
            int p = press;
            press = -1;
            if (p >= 0) {
                inv_ctl(w, p);
                if (!busy && hit(e.x, e.y) == p) activate(w, p);
            }
            return false;
        }
        if (e.type == kui::EventType::Click) {
            // a click with no press seen before it (the kernel wm can send one alone) (satoru)
            if (press < 0 && !busy) {
                int h = hit(e.x, e.y);
                if (h >= 0) { activate(w, h); inv_all(w); }
            }
            return false;
        }
        if (e.type == kui::EventType::PointerLeave) {
            inv_ctl(w, hover);
            hover = -1;
            press = -1;
            return false;
        }
        if (e.type == kui::EventType::KeyDown) {
            if (busy) return false;
            int k = e.key;
            if (k == kui::K_TAB) { move_focus(w, 1); return false; }
            if (k == kui::K_BTAB) { move_focus(w, -1); return false; }
            if (screen == S_DISKS && (k == kui::K_UP || k == kui::K_DOWN) && list.count) {
                int n = (int)list.count;
                int ns = sel + (k == kui::K_DOWN ? 1 : -1);
                if (ns < 0) ns = 0;
                if (ns >= n) ns = n - 1;
                kbd_vis = true;
                activate(w, C_DISK0 + ns);
                focus = C_DISK0 + ns;
                inv_all(w);
                return false;
            }
            if (k == kui::K_ENTER || k == kui::K_SPACE) {
                // on the disk page enter on a row continues; elsewhere it presses the focused control (satoru)
                if (screen == S_DISKS && focus >= C_DISK0 && k == kui::K_ENTER) activate(w, C_PRIMARY);
                else activate(w, focus);
                inv_all(w);
                return false;
            }
            if (k == kui::K_ESC) {
                if (screen == S_DISKS) go(w, S_WELCOME, -1);
                else if (screen == S_LAYOUT) go(w, S_DISKS, -1);
                else if (screen == S_CONFIRM) go(w, S_LAYOUT, -1);
                return false;
            }
            return false;
        }
        return false;
    }

    void OnTick(kui::Window& w, uint32_t now) override {
        float dt = (float)(int32_t)(now - last_tick) / 1000.0f;
        if (dt < 0.0f) dt = 0.0f;
        last_tick = now;

        if (!list_ok) {
            if (!t_scan || (int32_t)(now - t_scan) >= 0) {
                t_scan = now + 500;
                if (refresh_list()) {
                    t_plan = now + 300;
                    if (screen == S_DISKS) inv_all(w);
                }
            }
        }
        if (list_ok && !plan_ok && t_plan && (int32_t)(now - t_plan) >= 0 &&
            screen != S_PROGRESS && screen != S_DONE) {
            t_plan = now + 2000;
            if (refresh_plan()) {
                auto_t = now + 2500;
                auto_step = 0;
            }
        }

        // page flow (satoru)
        if (out_phase && (int32_t)(now - t_out) >= 160) {
            out_phase = false;
            page_t0 = now;
            spr_set(pgx, (float)dir * 48.0f);
            pgx.t = 0.0f;
            enter_page(now);
            inv_all(w);
        }

        // the test driver walks the same screens a person would (satoru)
        if (auto_mode && plan_ok && !out_phase && screen != S_PROGRESS && screen != S_DONE &&
            screen != S_ERROR && (int32_t)(now - auto_t) >= 0) {
            switch (auto_step) {
            case 0:
                if (screen == S_WELCOME) go(w, S_DISKS, 1);
                auto_step = 1;
                auto_t = now + 1800;
                break;
            case 1:
                if (screen == S_DISKS) next_after_disks(w);
                if (screen == S_LAYOUT) auto_step = 2;
                auto_t = now + 2400;
                break;
            case 2:
                if (screen == S_LAYOUT && plan_ok) { conv_ack = false; keep_ack = false; os_ack = false; go(w, S_CONFIRM, 1); }
                auto_step = 3;
                auto_t = now + 2200;
                break;
            case 3:
                if (screen == S_CONFIRM) {
                    keep_ack = true;
                    conv_ack = plan.need_convert ? true : false;
                    inv_all(w);
                    auto_step = 4;
                    auto_t = now + 900;
                }
                break;
            case 4:
                if (screen == S_CONFIRM) begin_install(w);
                auto_step = 5;
                break;
            default: break;
            }
        }

        // the install: start once the page is up, then poll the real status (satoru)
        if (screen == S_PROGRESS && !out_phase) {
            if (pending_start && (int32_t)(now - page_t0) >= kEntryMs[S_PROGRESS] + 60) {
                pending_start = false;
                start_install(w);
            }
            if (started && screen == S_PROGRESS && (!t_poll || (int32_t)(now - t_poll) >= 0)) {
                t_poll = now + 200;
                KinStatus ns;
                memset(&ns, 0, sizeof(ns));
                if (svc_status(&ns)) st = ns;
                uint32_t pc = st.pct > 100 ? 100 : st.pct;
                pv.t = (float)pc;
                const char* s = status_for(pc, st.done != 0);
                if (s != stat_cur) { stat_prev = stat_cur; stat_cur = s; stat_t = now; }
                // remaining time from the measured rate, smoothed (satoru)
                float el_s = (float)(int32_t)(now - t_started) / 1000.0f;
                if (pc >= 6 && pc < 100 && el_s > 1.0f) {
                    float est = el_s * (float)(100 - pc) / (float)pc;
                    eta_s = eta_s < 0.0f ? est : eta_s * 0.8f + est * 0.2f;
                }
                if (st.failed) {
                    set_error(st.detail[0] ? st.detail : "the install failed");
                    go(w, S_ERROR, 1);
                } else if (st.done && !done_seen) {
                    done_seen = true;
                    done_seen_t = now;
                    pv.t = 100.0f;
                }
            }
            if (done_seen && screen == S_PROGRESS &&
                ((pv.x > 99.5f && (int32_t)(now - done_seen_t) >= 700) || (int32_t)(now - done_seen_t) >= 1800)) {
                go(w, S_DONE, 1);
            }
        }
        if (auto_mode && screen == S_DONE && !out_phase && (int32_t)(now - page_t0) >= 4000) {
            w.Close();
            return;
        }

        // springs (satoru)
        bool dots_busy = spr_busy(dots) || spr_busy(dotv);
        spr_step(dots, 0.45f, 0.72f, dt);
        spr_step(dotv, 0.4f, 1.0f, dt);
        spr_step(pgx, 0.46f, 0.80f, dt);
        bool sel_busy = spr_busy(sely);
        spr_step(sely, 0.36f, 0.72f, dt);
        if (screen == S_LAYOUT && !out_phase && (int32_t)(now - page_t0) >= 280) grow.t = 1.0f;
        bool grow_busy = spr_busy(grow);
        spr_step(grow, 0.8f, 0.66f, dt);
        bool pv_busy = spr_busy(pv);
        spr_step(pv, 0.9f, 1.0f, dt);
        bool shake_busy = spr_busy(shake);
        spr_step(shake, 0.16f, 0.22f, dt);
        bool ctl_busy[C_COUNT];
        bool any_ctl = false;
        for (int i = 0; i < C_COUNT; i++) {
            Ctl& k = ctl[i];
            k.hov.t = (hover == i && k.enabled) ? 1.0f : 0.0f;
            k.prs.t = (press == i && hover == i) ? 1.0f : 0.0f;
            bool b = spr_busy(k.hov) || spr_busy(k.prs) || spr_busy(k.en) || spr_busy(k.chk);
            spr_step(k.hov, 0.22f, 1.0f, dt);
            spr_step(k.prs, 0.16f, 0.6f, dt);
            spr_step(k.en, 0.3f, 1.0f, dt);
            spr_step(k.chk, 0.34f, 0.58f, dt);
            ctl_busy[i] = b;
            any_ctl = any_ctl || b;
        }

        // repaint only what is moving, paced by a timer so the app sleeps between
        // frames: a ring-3 app that never blocks starves the old-mode kernel procs (satoru)
        bool entering = !out_phase && screen >= 0 && screen < S_COUNT &&
                        (int32_t)(now - page_t0) < kEntryMs[screen];
        bool full = out_phase || entering || spr_busy(pgx);
        bool any = full || any_ctl || dots_busy || sel_busy || grow_busy || shake_busy || pv_busy ||
                   screen == S_PROGRESS || (screen == S_DISKS && !list_ok);
        if (!any) { anim_stop(); return; }
        int iv = (screen == S_PROGRESS && !full) ? 33 : 16;
        anim_start(w, iv);
        if ((int32_t)(now - next_frame) < 0) return;
        next_frame = now + (uint32_t)iv - 2;
        if (full) {
            inv_all(w);
            return;
        }
        for (int i = 0; i < C_COUNT; i++) if (ctl_busy[i]) inv_ctl(w, i);
        if (dots_busy) inv(w, lw / 2 - 60, 14, 120, 26);
        if (sel_busy && screen == S_DISKS) inv(w, lw / 2 - 250, 120, 500, lh - 220);
        if (grow_busy && screen == S_LAYOUT) inv(w, lw / 2 - 270, 160, 540, 64);
        if (shake_busy && screen == S_ERROR) inv(w, lw / 2 - 80, 120, 160, 120);
        // the waiting screen: the logo, its glow and orbit, the status, bar and estimate (satoru)
        if (screen == S_PROGRESS) inv(w, lw / 2 - 160, 70, 320, 350);
        if (screen == S_DISKS && !list_ok) inv(w, lw / 2 - 30, 160, 60, 60);
    }
};

kui::AppHandler* make_installer() { return new InstallerApp; }

kui::WindowSpec installer_spec() {
    kui::WindowSpec s;
    s.title = "Install Kurono";
    s.app_id = "installer";
    s.w = 760;
    s.h = 560;
    s.min_w = 640;
    s.min_h = 500;
    s.x = -1;
    s.y = -1;
    s.resizable = true;
    s.closable = true;
    s.bg = 0;
    s.app_icon = -1;
    return s;
}

}  // namespace

KUI_APP(installer, installer_spec(), make_installer)

// end (satoru)
