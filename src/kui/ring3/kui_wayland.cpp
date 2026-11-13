// kui_wayland.cpp: the ring-3 backend of libkurono-ui, a small hand-written wayland client (satoru)
// wl_display / wl_registry / wl_compositor / wl_shm (memfd pools) / xdg_wm_base + xdg_toplevel / wl_seat (satoru)
// pointer and keyboard / wl_output, over the unix socket, no libwayland. an app draws into a wl_shm (satoru)
// buffer through the same kui::Canvas as in the kernel and commits only the damaged rect; kurono's (satoru)
// wayland server bridges the toplevel into a normal wm window (title bar, motion, dock, focus). (satoru)
// the loop sleeps in poll() on the display socket with the next timer / frame deadline as timeout, so an (satoru)
// idle app costs nothing. (satoru)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "../kui_internal.h"

namespace kui {
bool ThemeReload();
void FontsLoad();
}

namespace {

using namespace kui;

// ---- connection (satoru)

int g_fd = -1;
bool g_dead = false;
uint8_t g_rx[16384];
int g_rx_len = 0;
uint8_t g_tx[16384];
int g_tx_len = 0;
uint32_t g_next_id = 2;          // 1 is wl_display (satoru)

enum Kind : uint8_t {
    K_NONE, K_REGISTRY, K_CALLBACK_SYNC, K_CALLBACK_FRAME, K_COMPOSITOR, K_SHM, K_WMBASE, K_SEAT, K_OUTPUT,
    K_POINTER, K_KEYBOARD, K_SURFACE, K_XDGSURF, K_TOPLEVEL, K_POOL, K_BUFFER,
    K_DDM, K_DDEV, K_DSOURCE, K_DOFFER, K_PDM, K_PDEV, K_PSOURCE, K_POFFER
};

struct Obj { uint32_t id; Kind kind; int win; int buf; };
Obj g_objs[256];

// data offer proxies announced by the server. their ids are server-allocated
// (>= 0xff000000), so they are registered directly instead of via new_id. (satoru)
struct KOffer {
    bool     used;
    uint32_t id;
    bool     primary;
    int      mime_count;
    char     mimes[8][48];
};
KOffer g_koffers[32];
int g_rx_fds[16];
int g_rx_fd_n = 0;

uint32_t new_id(Kind k, int win = -1, int buf = -1) {
    uint32_t id = g_next_id++;
    for (int i = 0; i < 256; i++) if (g_objs[i].kind == K_NONE) { g_objs[i] = Obj{ id, k, win, buf }; return id; }
    return id;   // table full: events for it are ignored (satoru)
}
Obj* obj_of(uint32_t id) {
    for (int i = 0; i < 256; i++) if (g_objs[i].kind != K_NONE && g_objs[i].id == id) return &g_objs[i];
    return nullptr;
}
void obj_drop(uint32_t id) { if (Obj* o = obj_of(id)) o->kind = K_NONE; }

// register a server-allocated object id (data offers, >= 0xff000000). (satoru)
void obj_add_id(uint32_t id, Kind k) {
    for (int i = 0; i < 256; i++)
        if (g_objs[i].kind == K_NONE) { g_objs[i] = Obj{ id, k, -1, -1 }; return; }
}

KOffer* koffer_find(uint32_t id) {
    for (int i = 0; i < 32; i++) if (g_koffers[i].used && g_koffers[i].id == id) return &g_koffers[i];
    return nullptr;
}
KOffer* koffer_new(uint32_t id, bool primary) {
    for (int i = 0; i < 32; i++) {
        if (g_koffers[i].used) continue;
        g_koffers[i].used = true;
        g_koffers[i].id = id;
        g_koffers[i].primary = primary;
        g_koffers[i].mime_count = 0;
        return &g_koffers[i];
    }
    return nullptr;
}
void koffer_remove(uint32_t id) {
    KOffer* o = koffer_find(id);
    if (o) { o->used = false; o->id = 0; o->mime_count = 0; }
}
void koffer_add_mime(KOffer* o, const char* m) {
    if (!o || !m || !m[0] || o->mime_count >= 8) return;
    for (int i = 0; i < o->mime_count; i++) if (!strcmp(o->mimes[i], m)) return;
    strncpy(o->mimes[o->mime_count], m, 47);
    o->mimes[o->mime_count][47] = 0;
    o->mime_count++;
}
const char* koffer_pick_text(KOffer* o) {
    if (!o) return nullptr;
    const char* plain = nullptr;
    for (int i = 0; i < o->mime_count; i++) {
        if (!strcmp(o->mimes[i], "text/plain;charset=utf-8")) return o->mimes[i];
        if (!strcmp(o->mimes[i], "text/plain") || !strcmp(o->mimes[i], "UTF8_STRING")) plain = o->mimes[i];
    }
    return plain;
}
int pop_rx_fd() {
    if (g_rx_fd_n <= 0) return -1;
    int fd = g_rx_fds[0];
    memmove(g_rx_fds, g_rx_fds + 1, sizeof(int) * (size_t)(g_rx_fd_n - 1));
    g_rx_fd_n--;
    return fd;
}
// copy `data` into a pipe-style fd the wayland server reads back. (satoru)
void write_all_fd(int fd, const char* data, int len) {
    if (fd < 0) return;
    int off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, (size_t)(len - off));
        if (n > 0) { off += (int)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { fd, POLLOUT, 0 };
            poll(&p, 1, 100);
            continue;
        }
        break;
    }
    close(fd);
}

bool flush_tx(int pass_fd = -1) {
    int off = 0;
    bool fd_sent = pass_fd < 0;
    while (off < g_tx_len && !g_dead) {
        struct iovec iov = { g_tx + off, (size_t)(g_tx_len - off) };
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = &iov; m.msg_iovlen = 1;
        char cbuf[CMSG_SPACE(sizeof(int))];
        if (!fd_sent) {
            memset(cbuf, 0, sizeof(cbuf));
            m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
            struct cmsghdr* cm = CMSG_FIRSTHDR(&m);
            cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));
        }
        ssize_t r = sendmsg(g_fd, &m, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {   // kurono sockets are non-blocking (satoru)
                struct pollfd p = { g_fd, POLLOUT, 0 };
                poll(&p, 1, 100);
                continue;
            }
            g_dead = true;
            return false;
        }
        fd_sent = true;
        off += (int)r;
    }
    g_tx_len = 0;
    return !g_dead;
}

// queue one request: header + 32-bit words (strings are pre-encoded by the caller) (satoru)
void send_req(uint32_t obj, uint16_t op, const uint32_t* w, int nw, int pass_fd = -1) {
    int size = 8 + nw * 4;
    if (pass_fd >= 0 && g_tx_len) flush_tx();
    if (g_tx_len + size > (int)sizeof(g_tx)) flush_tx();
    uint32_t hdr[2] = { obj, ((uint32_t)size << 16) | op };
    memcpy(g_tx + g_tx_len, hdr, 8);
    if (nw) memcpy(g_tx + g_tx_len + 8, w, (size_t)nw * 4);
    g_tx_len += size;
    if (pass_fd >= 0) flush_tx(pass_fd);   // the fd rides on this message's bytes (satoru)
}

// encode a wayland string into words: length (with nul), bytes, pad to 4 (satoru)
int put_str(uint32_t* w, const char* s) {
    uint32_t len = (uint32_t)strlen(s) + 1;
    w[0] = len;
    memset(&w[1], 0, (len + 3) & ~3u);
    memcpy(&w[1], s, len - 1);
    return 1 + (int)((len + 3) / 4);
}

// ---- globals (satoru)

uint32_t g_compositor, g_shm, g_wmbase, g_seat, g_output, g_pointer, g_keyboard;
uint32_t g_seat_name, g_seat_ver, g_out_name;
uint32_t g_comp_name, g_shm_name, g_wm_name;
uint32_t g_ddm_name, g_pdm_name;
uint32_t g_ddm, g_ddev, g_pdm, g_pdev;
uint32_t g_clip_serial = 1;
uint32_t g_clip_source, g_primary_source;
uint32_t g_clip_offer, g_primary_offer;
char*    g_clip_own = nullptr;
char*    g_primary_own = nullptr;
char*    g_clip_fetch = nullptr;
char     g_clip_fetch_buf[256 * 1024];   // no malloc on the fetch path (satoru)
bool g_synced = false;
int g_screen_w = 0, g_screen_h = 0;


// ---- windows (satoru)

const int MAXW = 8;

struct Buf {
    uint32_t id, pool;
    int fd;
    uint32_t* px;
    int w, h;
    size_t bytes;
    bool busy;
};

struct Timer1 { bool used; int id; int win; uint32_t interval, next; bool repeat; };
Timer1 g_timers[32];
int g_timer_next = 1;

int g_pointer_win = -1, g_kbd_win = -1;
int g_px = 0, g_py = 0;
uint8_t g_mods = 0;
bool g_keys[256];
bool g_quit = false;
int g_exit = 0;

// extra fds the app wants RunLoop to wake on (a terminal's pty master). the (satoru)
// wake only re-runs OnTick; the app drains the fd itself. (satoru)
const int MAXWATCH = 8;
int g_watch[MAXWATCH] = { -1, -1, -1, -1, -1, -1, -1, -1 };

bool watch_add(int fd) {
    if (fd < 0) return false;
    for (int i = 0; i < MAXWATCH; i++) if (g_watch[i] == fd) return true;
    for (int i = 0; i < MAXWATCH; i++) if (g_watch[i] < 0) { g_watch[i] = fd; return true; }
    return false;
}
void watch_del(int fd) {
    for (int i = 0; i < MAXWATCH; i++) if (g_watch[i] == fd) g_watch[i] = -1;
}

}  // namespace

namespace kui {

struct Window::Impl {
    bool used;
    int idx;
    uint32_t surface, xsurf, toplevel;
    Buf buf[2];
    int cur;                         // buffer holding the last presented frame, -1 none (satoru)
    int w, h;                        // content size (satoru)
    int pend_w, pend_h;              // size from the last toplevel configure (satoru)
    bool configured, mapped;
    uint32_t frame_cb;               // pending frame callback, 0 = may commit (satoru)
    uint32_t frame_ms;               // when it was asked, a lost callback must not wedge us (satoru)
    bool dirty;
    int dx0, dy0, dx1, dy1;          // accumulated damage (satoru)
    uint32_t frame_at;
    bool focused, fullscreen, maximized;
    char app_id[24];
    Window pub;
};

}  // namespace kui

namespace {

kui::Window::Impl g_win[MAXW];

void damage(kui::Window::Impl* s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 <= x || y1 <= y) return;
    if (!s->dirty) { s->dx0 = x; s->dy0 = y; s->dx1 = x1; s->dy1 = y1; s->dirty = true; return; }
    if (x < s->dx0) s->dx0 = x;
    if (y < s->dy0) s->dy0 = y;
    if (x1 > s->dx1) s->dx1 = x1;
    if (y1 > s->dy1) s->dy1 = y1;
}
void damage_all(kui::Window::Impl* s) { damage(s, 0, 0, s->w, s->h); }

void buf_free(Buf& b) {
    if (b.id) { send_req(b.id, 0, nullptr, 0); obj_drop(b.id); }          // wl_buffer.destroy (satoru)
    if (b.pool) { send_req(b.pool, 1, nullptr, 0); obj_drop(b.pool); }    // wl_shm_pool.destroy (satoru)
    if (b.px) munmap(b.px, b.bytes);
    if (b.fd >= 0) close(b.fd);
    b = Buf{ 0, 0, -1, nullptr, 0, 0, 0, false };
}

// a buffer of the window's size in its own memfd pool (pools never grow on kurono) (satoru)
bool buf_make(kui::Window::Impl* s, int i) {
    Buf& b = s->buf[i];
    buf_free(b);
    size_t bytes = (size_t)s->w * (size_t)s->h * 4;
    int fd = (int)syscall(SYS_memfd_create, "kui-shm", 0);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)bytes) < 0) { close(fd); return false; }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return false; }
    b.fd = fd; b.px = (uint32_t*)p; b.bytes = bytes; b.w = s->w; b.h = s->h; b.busy = false;
    b.pool = new_id(K_POOL, s->idx, i);
    uint32_t a[2] = { b.pool, (uint32_t)bytes };
    send_req(g_shm, 0, a, 2, fd);                                           // wl_shm.create_pool (satoru)
    b.id = new_id(K_BUFFER, s->idx, i);
    uint32_t c[6] = { b.id, 0, (uint32_t)s->w, (uint32_t)s->h, (uint32_t)s->w * 4, 1 };   // xrgb8888 (satoru)
    send_req(b.pool, 0, c, 6);                                              // create_buffer (satoru)
    return true;
}

void ev_init(Event& e, EventType t) {
    memset(&e, 0, sizeof(e));
    e.type = t; e.mods = g_mods; e.x = g_px; e.y = g_py; e.time_ms = NowMs();
}

void deliver(kui::Window::Impl* s, const Event& e) {
    if (!s || !s->used || !s->pub.handler) return;
    if (s->pub.handler->OnEvent(s->pub, e) && s->used) damage_all(s);
}

void win_destroy(kui::Window::Impl* s) {
    if (!s->used) return;
    for (int i = 0; i < 32; i++) if (g_timers[i].used && g_timers[i].win == s->idx) g_timers[i].used = false;
    for (int i = 0; i < 2; i++) buf_free(s->buf[i]);
    if (s->toplevel) { send_req(s->toplevel, 0, nullptr, 0); obj_drop(s->toplevel); }
    if (s->xsurf) { send_req(s->xsurf, 0, nullptr, 0); obj_drop(s->xsurf); }
    if (s->surface) { send_req(s->surface, 0, nullptr, 0); obj_drop(s->surface); }
    flush_tx();
    if (g_pointer_win == s->idx) g_pointer_win = -1;
    if (g_kbd_win == s->idx) g_kbd_win = -1;
    AppHandler* h = s->pub.handler;
    s->used = false;
    s->pub.handler = nullptr;
    delete h;
}

// draw the damaged rect into a free buffer and commit it (satoru)
void render(kui::Window::Impl* s) {
    if (!s->used || !s->configured || s->frame_cb || !s->dirty || s->w <= 0 || s->h <= 0) return;
    int bi = -1;
    for (int i = 0; i < 2; i++) {
        int k = s->cur < 0 ? i : (s->cur + i) % 2;   // prefer the buffer holding the last frame (satoru)
        if (s->buf[k].px && !s->buf[k].busy && s->buf[k].w == s->w && s->buf[k].h == s->h) { bi = k; break; }
    }
    if (bi < 0) {
        for (int i = 0; i < 2; i++) {
            if (s->buf[i].busy && s->buf[i].w == s->w && s->buf[i].h == s->h) continue;
            if (buf_make(s, i)) { bi = i; break; }
        }
        if (bi < 0) return;   // both busy: the release event will bring us back (satoru)
    }
    Buf& b = s->buf[bi];
    if (s->cur >= 0 && s->cur != bi) {
        Buf& o = s->buf[s->cur];
        if (o.px && o.w == b.w && o.h == b.h) memcpy(b.px, o.px, b.bytes);   // carry the old frame over (satoru)
        else damage_all(s);
    } else if (s->cur < 0) {
        damage_all(s);
    }
    int x0 = s->dx0, y0 = s->dy0, x1 = s->dx1, y1 = s->dy1;
    s->dirty = false;
    Canvas::Impl ci;
    CanvasInit(ci, b.px, b.w, b.w, b.h, 0, 0, s->w, s->h, x0, y0, x1, y1);
    Canvas c(&ci);
    s->pub.handler->OnFrame(s->pub, c, NowMs());
    uint32_t at[3] = { b.id, 0, 0 };
    send_req(s->surface, 1, at, 3);                                          // attach (satoru)
    uint32_t dm[4] = { (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
    send_req(s->surface, 9, dm, 4);                                          // damage_buffer (satoru)
    s->frame_cb = new_id(K_CALLBACK_FRAME, s->idx);
    s->frame_ms = NowMs();
    send_req(s->surface, 3, &s->frame_cb, 1);                                // frame (satoru)
    send_req(s->surface, 6, nullptr, 0);                                     // commit (satoru)
    flush_tx();
    b.busy = true;
    s->cur = bi;
    s->mapped = true;
}

// ---- input mapping: evdev key codes, us layout (satoru)

const char kPlain[] = "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
const char kShift[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

int special_of(uint32_t k, bool shift) {
    switch (k) {
    case 1: return K_ESC;   case 14: return K_BS;   case 15: return shift ? K_BTAB : K_TAB;
    case 28: case 96: return K_ENTER;
    case 103: return K_UP;  case 108: return K_DOWN; case 105: return K_LEFT; case 106: return K_RIGHT;
    case 102: return K_HOME; case 107: return K_END; case 104: return K_PGUP; case 109: return K_PGDN;
    case 111: return K_DEL;
    case 42: case 54: return K_SHIFT;  case 29: case 97: return K_CTRL;
    case 56: case 100: return K_ALT;   case 125: case 126: return K_SUPER;
    case 98: return '/'; case 55: return '*'; case 74: return '-'; case 78: return '+';
    default: break;
    }
    if (k >= 59 && k <= 68) return K_F1 + (int)(k - 59);
    if (k == 87) return K_F1 + 10;
    if (k == 88) return K_F1 + 11;
    // keypad digits (numlock assumed on, like the kernel's KeyToChar) (satoru)
    static const char kp[] = { '7', '8', '9', 0, '4', '5', '6', 0, '1', '2', '3', '0', '.' };
    if (k >= 71 && k <= 83 && kp[k - 71]) return kp[k - 71];
    return 0;
}

void on_key(uint32_t code, bool pressed) {
    if (code < 256) g_keys[code] = pressed;
    bool shift = g_keys[42] || g_keys[54];
    kui::Window::Impl* s = (g_kbd_win >= 0) ? &g_win[g_kbd_win] : nullptr;
    if (!s || !s->used) return;
    int key = special_of(code, shift);
    char ch = 0;
    if (!key && code < sizeof(kPlain) - 1) {
        ch = shift ? kShift[code] : kPlain[code];
        bool caps = (g_mods & 0x80) != 0;   // caps lock latched in our private bit (satoru)
        if (caps && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        else if (caps && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (!pressed && code < sizeof(kPlain) - 1) ch = kPlain[code];   // key ups carry the unshifted char (satoru)
        key = (unsigned char)ch;
    }
    if (!key) return;
    Event e;
    ev_init(e, pressed ? EventType::KeyDown : EventType::KeyUp);
    e.mods = g_mods & 0x0F;
    e.key = key;
    if (pressed && ch >= 32 && ch < 127) { e.text[0] = ch; e.text[1] = 0; }
    deliver(s, e);
}

void pointer_button(uint32_t btn, bool pressed) {
    kui::Window::Impl* s = (g_pointer_win >= 0) ? &g_win[g_pointer_win] : nullptr;
    if (!s || !s->used) return;
    Event e;
    ev_init(e, pressed ? EventType::PointerDown : EventType::PointerUp);
    e.mods = g_mods & 0x0F;
    e.button = btn == 0x110 ? Button::Left : btn == 0x111 ? Button::Right : btn == 0x112 ? Button::Middle : Button::None;
    deliver(s, e);
    if (pressed && e.button == Button::Left && s->used) {
        Event c;
        ev_init(c, EventType::Click);
        c.mods = g_mods & 0x0F;
        c.button = Button::Left;
        c.clicks = ClickCount(g_px, g_py, c.time_ms);
        deliver(s, c);
    }
}

int win_of_surface(uint32_t sid) {
    for (int i = 0; i < MAXW; i++) if (g_win[i].used && g_win[i].surface == sid) return i;
    return -1;
}

void theme_check() {
    if (!ThemeReload()) return;
    for (int i = 0; i < MAXW; i++) {
        if (!g_win[i].used) continue;
        Event e; ev_init(e, EventType::ThemeChanged);
        deliver(&g_win[i], e);
        if (g_win[i].used) damage_all(&g_win[i]);
    }
}

inline uint32_t rd(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// copy a wayland string argument (u32 len incl nul, bytes) out of an event. (satoru)
bool rd_str(const uint8_t* a, int n, char* out, int max) {
    if (!out || max <= 0) return false;
    out[0] = 0;
    if (n < 4) return false;
    uint32_t len = rd(a);
    if (len == 0 || len > (uint32_t)n - 4) return false;
    uint32_t c = len - 1;
    if (c > (uint32_t)(max - 1)) c = (uint32_t)(max - 1);
    for (uint32_t i = 0; i < c; i++) out[i] = (char)a[4 + i];
    out[c] = 0;
    return true;
}

// ---- event dispatch (satoru)

void dispatch(uint32_t id, uint16_t op, const uint8_t* a, int n) {
    if (id == 1) {
        if (op == 0 && n >= 8) {   // wl_display.error (satoru)
            fprintf(stderr, "[kui] wayland error obj=%u code=%u %s\n", rd(a), rd(a + 4), n > 12 ? (const char*)(a + 12) : "");
            g_dead = true;
        }
        return;   // delete_id: our ids are never reused (satoru)
    }
    Obj* o = obj_of(id);
    if (!o) return;
    switch (o->kind) {
    case K_REGISTRY:
        if (op == 0 && n >= 12) {
            uint32_t name = rd(a), len = rd(a + 4);
            const char* iface = (const char*)(a + 8);
            uint32_t ver = (n >= 12 + (int)((len + 3) & ~3u)) ? rd(a + 8 + ((len + 3) & ~3u)) : 1;
            if (!strcmp(iface, "wl_compositor")) g_comp_name = name;
            else if (!strcmp(iface, "wl_shm")) g_shm_name = name;
            else if (!strcmp(iface, "xdg_wm_base")) g_wm_name = name;
            else if (!strcmp(iface, "wl_seat")) { g_seat_name = name; g_seat_ver = ver; }
            else if (!strcmp(iface, "wl_output") && !g_out_name) g_out_name = name;
            else if (!strcmp(iface, "wl_data_device_manager")) g_ddm_name = name;
            else if (!strcmp(iface, "zwp_primary_selection_device_manager_v1")) g_pdm_name = name;
        }
        break;
    case K_CALLBACK_SYNC:
        if (op == 0) { g_synced = true; obj_drop(id); }
        break;
    case K_CALLBACK_FRAME:
        if (op == 0) {
            if (o->win >= 0 && g_win[o->win].frame_cb == id) g_win[o->win].frame_cb = 0;
            obj_drop(id);
        }
        break;
    case K_WMBASE:
        if (op == 0 && n >= 4) { uint32_t s = rd(a); send_req(g_wmbase, 3, &s, 1); }   // ping -> pong (satoru)
        break;
    case K_SEAT:
        if (op == 0 && n >= 4) {
            uint32_t caps = rd(a);
            if ((caps & 1) && !g_pointer) { g_pointer = new_id(K_POINTER); send_req(g_seat, 0, &g_pointer, 1); }
            if ((caps & 2) && !g_keyboard) { g_keyboard = new_id(K_KEYBOARD); send_req(g_seat, 1, &g_keyboard, 1); }
        }
        break;
    case K_OUTPUT:
        if (op == 1 && n >= 16 && (rd(a) & 1)) { g_screen_w = (int)rd(a + 4); g_screen_h = (int)rd(a + 8); }   // mode, current (satoru)
        break;
    case K_BUFFER:
        if (op == 0 && o->win >= 0 && o->buf >= 0) g_win[o->win].buf[o->buf].busy = false;   // release (satoru)
        break;
    case K_XDGSURF:
        if (op == 0 && n >= 4 && o->win >= 0) {
            kui::Window::Impl* s = &g_win[o->win];
            uint32_t serial = rd(a);
            send_req(s->xsurf, 4, &serial, 1);                                  // ack_configure (satoru)
            // the first configure's size is the server's default guess (1024x768); a kui app keeps its (satoru)
            // spec size until it is mapped. after that a configure is a real wm resize. (satoru)
            if (s->mapped && s->pend_w > 0 && s->pend_h > 0 && (s->pend_w != s->w || s->pend_h != s->h)) {
                s->w = s->pend_w; s->h = s->pend_h;
                Event e; ev_init(e, EventType::Resize); e.w = s->w; e.h = s->h;
                deliver(s, e);
                s->cur = -1;
            }
            s->configured = true;
            damage_all(s);
        }
        break;
    case K_TOPLEVEL:
        if (o->win < 0) break;
        if (op == 0 && n >= 8) {
            kui::Window::Impl* s = &g_win[o->win];
            s->pend_w = (int)rd(a); s->pend_h = (int)rd(a + 4);
            uint32_t alen = n >= 12 ? rd(a + 8) : 0;
            bool fs = false, mx = false;
            for (uint32_t k = 0; k + 4 <= alen && 12 + (int)k + 4 <= n; k += 4) {
                uint32_t st = rd(a + 12 + k);
                if (st == 1) mx = true;
                if (st == 2) fs = true;
            }
            s->fullscreen = fs; s->maximized = mx;
        } else if (op == 1) {   // close: the wm close button (satoru)
            kui::Window::Impl* s = &g_win[o->win];
            if (s->used && (!s->pub.handler || s->pub.handler->OnClose(s->pub))) win_destroy(s);
        }
        break;
    case K_POINTER: {
        if (op == 0 && n >= 16) {          // enter(serial, surface, x, y) (satoru)
            g_pointer_win = win_of_surface(rd(a + 4));
            g_px = (int)rd(a + 8) / 256; g_py = (int)rd(a + 12) / 256;
            if (g_pointer_win >= 0) { Event e; ev_init(e, EventType::PointerMove); deliver(&g_win[g_pointer_win], e); }
        } else if (op == 1) {              // leave (satoru)
            if (g_pointer_win >= 0) { Event e; ev_init(e, EventType::PointerLeave); deliver(&g_win[g_pointer_win], e); }
            g_pointer_win = -1;
        } else if (op == 2 && n >= 12) {   // motion(time, x, y) (satoru)
            g_px = (int32_t)rd(a + 4) / 256; g_py = (int32_t)rd(a + 8) / 256;
            if (g_pointer_win >= 0) { Event e; ev_init(e, EventType::PointerMove); deliver(&g_win[g_pointer_win], e); }
        } else if (op == 3 && n >= 16) {   // button(serial, time, button, state) (satoru)
            pointer_button(rd(a + 8), rd(a + 12) != 0);
        } else if (op == 4 && n >= 12 && rd(a + 4) == 0) {   // vertical axis, 32 px per notch (satoru)
            if (g_pointer_win >= 0) {
                float v = (float)(int32_t)rd(a + 8) / 256.0f / 32.0f;
                Event e; ev_init(e, EventType::Scroll);
                e.dy = v;
                e.scroll_steps = (int)(v < 0 ? v - 0.5f : v + 0.5f);
                deliver(&g_win[g_pointer_win], e);
            }
        }
        break;
    }
    case K_KEYBOARD:
        if (op == 1 && n >= 8) {           // enter(serial, surface, keys) (satoru)
            g_kbd_win = win_of_surface(rd(a + 4));
            theme_check();                  // settings may have changed the theme meanwhile (satoru)
            if (g_kbd_win >= 0) {
                kui::Window::Impl* s = &g_win[g_kbd_win];
                s->focused = true;
                Event e; ev_init(e, EventType::Focus); deliver(s, e);
            }
        } else if (op == 2 && n >= 8) {    // leave (satoru)
            int w = win_of_surface(rd(a + 4));
            if (w >= 0) { g_win[w].focused = false; Event e; ev_init(e, EventType::Blur); deliver(&g_win[w], e); }
            if (g_kbd_win == w) g_kbd_win = -1;
            memset(g_keys, 0, sizeof(g_keys));
        } else if (op == 3 && n >= 16) {   // key(serial, time, key, state) (satoru)
            on_key(rd(a + 8), rd(a + 12) != 0);
        } else if (op == 4 && n >= 20) {   // modifiers(serial, depressed, latched, locked, group) (satoru)
            uint32_t dep = rd(a + 4), lock = rd(a + 12);
            uint8_t m = 0;
            if (dep & 1) m |= M_SHIFT;
            if (dep & 4) m |= M_CTRL;
            if (dep & 8) m |= M_ALT;
            if (dep & 64) m |= M_SUPER;
            if (lock & 2) m |= 0x80;
            g_mods = m;
        } else if (op == 0) {              // keymap(fd): kui maps keys itself (satoru)
            int fd = pop_rx_fd();
            if (fd >= 0) close(fd);
        }
        break;
    case K_DDEV:
        if (op == 0 && n >= 4) {           // data_offer(new_id)
            uint32_t oid = rd(a);
            obj_add_id(oid, K_DOFFER);
            koffer_new(oid, false);
        } else if (op == 5 && n >= 4) {    // selection(offer)
            uint32_t oid = rd(a);
            if (oid != g_clip_offer) {
                if (g_clip_offer) { koffer_remove(g_clip_offer); obj_drop(g_clip_offer); }
                g_clip_offer = oid;
            }
        }
        break;
    case K_PDEV:
        if (op == 0 && n >= 4) {           // data_offer(new_id)
            uint32_t oid = rd(a);
            obj_add_id(oid, K_POFFER);
            koffer_new(oid, true);
        } else if (op == 1 && n >= 4) {    // selection(offer)
            uint32_t oid = rd(a);
            if (oid != g_primary_offer) {
                if (g_primary_offer) { koffer_remove(g_primary_offer); obj_drop(g_primary_offer); }
                g_primary_offer = oid;
            }
        }
        break;
    case K_DOFFER:
    case K_POFFER:
        if (op == 0) {                     // offer(mime)
            char mime[64];
            if (rd_str(a, n, mime, sizeof(mime))) koffer_add_mime(koffer_find(id), mime);
        }
        break;
    case K_DSOURCE:
        if (op == 1) {                     // send(mime, fd)
            int fd = pop_rx_fd();
            if (fd >= 0) write_all_fd(fd, g_clip_own ? g_clip_own : "", g_clip_own ? (int)strlen(g_clip_own) : 0);
        } else if (op == 2) {              // cancelled: another app owns the selection now (satoru)
            free(g_clip_own); g_clip_own = nullptr;
        }
        break;
    case K_PSOURCE:
        if (op == 0) {                     // send(mime, fd)
            int fd = pop_rx_fd();
            if (fd >= 0) write_all_fd(fd, g_primary_own ? g_primary_own : "", g_primary_own ? (int)strlen(g_primary_own) : 0);
        } else if (op == 1) {              // cancelled (satoru)
            free(g_primary_own); g_primary_own = nullptr;
        }
        break;
    default:
        break;
    }
}

void read_events() {
    for (;;) {
        struct iovec iov = { g_rx + g_rx_len, sizeof(g_rx) - (size_t)g_rx_len };
        char cbuf[CMSG_SPACE(sizeof(int) * 8)];
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = &iov; m.msg_iovlen = 1;
        m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
        ssize_t r = recvmsg(g_fd, &m, MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) g_dead = true;
            break;
        }
        if (r == 0) { g_dead = true; break; }   // the server dropped us (task manager kill, wm gone) (satoru)
        // fds ride with the first byte of the message that carries them; queue
        // them in order so a data_source.send / keymap handler can claim one. (satoru)
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < nfd; i++) {
                int f; memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                if (g_rx_fd_n < 16) g_rx_fds[g_rx_fd_n++] = f; else close(f);
            }
        }
        g_rx_len += (int)r;
        int off = 0;
        while (g_rx_len - off >= 8) {
            uint32_t id = rd(g_rx + off), hw = rd(g_rx + off + 4);
            int size = (int)(hw >> 16);
            if (size < 8 || size > (int)sizeof(g_rx)) { g_dead = true; break; }
            if (off + size > g_rx_len) break;
            dispatch(id, (uint16_t)(hw & 0xFFFF), g_rx + off + 8, size - 8);
            off += size;
        }
        if (off) { memmove(g_rx, g_rx + off, (size_t)(g_rx_len - off)); g_rx_len -= off; }
        if (g_rx_len == 0) {   // no half-read message can still owe an fd (satoru)
            while (g_rx_fd_n > 0) close(g_rx_fds[--g_rx_fd_n]);
        }
        if (g_rx_len == (int)sizeof(g_rx)) { g_dead = true; break; }
    }
    flush_tx();
}

bool roundtrip() {
    g_synced = false;
    uint32_t cb = new_id(K_CALLBACK_SYNC);
    send_req(1, 0, &cb, 1);   // wl_display.sync (satoru)
    flush_tx();
    for (int i = 0; i < 400 && !g_synced && !g_dead; i++) {
        struct pollfd p = { g_fd, POLLIN, 0 };
        poll(&p, 1, 25);
        read_events();
    }
    return g_synced;
}

void bind(uint32_t name, const char* iface, uint32_t ver, uint32_t id) {
    uint32_t w[24];
    int k = 0;
    w[k++] = name;
    k += put_str(&w[k], iface);
    w[k++] = ver;
    w[k++] = id;
    send_req(2, 0, w, k);
}

bool connect_display() {
    if (g_fd >= 0) return !g_dead;
    const char* dir = getenv("XDG_RUNTIME_DIR");
    const char* dsp = getenv("WAYLAND_DISPLAY");
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%s", dir && dir[0] ? dir : "/system/run/user/1000",
             dsp && dsp[0] ? dsp : "wayland-0");
    g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_fd < 0) return false;
    if (connect(g_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        fprintf(stderr, "[kui] cannot reach the compositor at %s\n", sa.sun_path);
        return false;
    }
    g_objs[0] = Obj{ 2, K_REGISTRY, -1, -1 };
    g_next_id = 3;
    uint32_t reg = 2;
    send_req(1, 1, &reg, 1);   // get_registry (satoru)
    if (!roundtrip() || !g_comp_name || !g_shm_name || !g_wm_name) {
        fprintf(stderr, "[kui] compositor globals missing\n");
        return false;
    }
    g_compositor = new_id(K_COMPOSITOR); bind(g_comp_name, "wl_compositor", 4, g_compositor);
    g_shm = new_id(K_SHM);               bind(g_shm_name, "wl_shm", 1, g_shm);
    g_wmbase = new_id(K_WMBASE);         bind(g_wm_name, "xdg_wm_base", 1, g_wmbase);
    if (g_seat_name) { g_seat = new_id(K_SEAT); bind(g_seat_name, "wl_seat", g_seat_ver < 5 ? g_seat_ver : 5, g_seat); }
    if (g_out_name) { g_output = new_id(K_OUTPUT); bind(g_out_name, "wl_output", 2, g_output); }
    if (g_seat) {
        if (g_ddm_name) {
            g_ddm = new_id(K_DDM);
            bind(g_ddm_name, "wl_data_device_manager", 3, g_ddm);
            g_ddev = new_id(K_DDEV);
            uint32_t a[2] = { g_ddev, g_seat };
            send_req(g_ddm, 1, a, 2);                 // get_data_device
        }
        if (g_pdm_name) {
            g_pdm = new_id(K_PDM);
            bind(g_pdm_name, "zwp_primary_selection_device_manager_v1", 1, g_pdm);
            g_pdev = new_id(K_PDEV);
            uint32_t a[2] = { g_pdev, g_seat };
            send_req(g_pdm, 1, a, 2);                 // get_device
        }
    }
    flush_tx();
    return true;
}

}  // namespace

namespace kui {

// ---- clipboard through wl_data_device / zwp_primary_selection (satoru)

static void clip_offer_text_mimes(uint32_t source) {
    static const char* mimes[3] = {
        "text/plain;charset=utf-8", "text/plain", "UTF8_STRING"
    };
    for (int i = 0; i < 3; i++) {
        uint32_t w[16];
        int k = put_str(w, mimes[i]);
        send_req(source, 0, w, k);
    }
}

static bool clip_set(bool primary, const char* s) {
    if (!connect_display()) return false;
    if (primary ? (!g_pdm || !g_pdev) : (!g_ddm || !g_ddev)) return false;
    char** own = primary ? &g_primary_own : &g_clip_own;
    free(*own);
    *own = strdup(s ? s : "");
    if (!*own) return false;
    uint32_t* src = primary ? &g_primary_source : &g_clip_source;
    if (*src) {
        send_req(*src, 1, nullptr, 0);   // destroy the previous source
        obj_drop(*src);
        *src = 0;
    }
    *src = new_id(primary ? K_PSOURCE : K_DSOURCE);
    send_req(primary ? g_pdm : g_ddm, 0, src, 1);   // create_source(new_id)
    clip_offer_text_mimes(*src);
    uint32_t a[2] = { *src, g_clip_serial++ };
    send_req(primary ? g_pdev : g_ddev, primary ? 0 : 1, a, 2);
    flush_tx();
    return true;
}

static const char* clip_get(bool primary) {
    if (!connect_display()) return "";
    read_events();
    uint32_t oid = primary ? g_primary_offer : g_clip_offer;
    KOffer* o = koffer_find(oid);
    const char* mime = koffer_pick_text(o);
    char* own = primary ? g_primary_own : g_clip_own;
    if (!mime) {
        if (own && own[0]) return own;
        return "";
    }
    int fds[2];
    if (pipe(fds) < 0) return "";
    uint32_t w[16];
    int k = put_str(w, mime);
    send_req(oid, primary ? 0 : 1, w, k, fds[1]);   // receive(mime, fd)
    close(fds[1]);

    int cap = (int)sizeof(g_clip_fetch_buf), len = 0;
    char* buf = g_clip_fetch_buf;
    uint32_t deadline = NowMs() + 1500;
    for (;;) {
        int32_t left = (int32_t)(deadline - NowMs());
        if (left < 0) left = 0;
        struct pollfd pf[2];
        pf[0].fd = g_fd;      pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = fds[0];    pf[1].events = POLLIN; pf[1].revents = 0;
        int r = poll(pf, 2, left);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (len + 4096 > cap) break;   // bounded static fetch buffer (satoru)
            ssize_t n = read(fds[0], buf + len, (size_t)(cap - len));
            if (n > 0) { len += (int)n; continue; }
            if (n == 0) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (pf[0].revents & POLLIN) read_events();
    }
    close(fds[0]);
    buf[len] = 0;
    g_clip_fetch = buf;
    return g_clip_fetch;
}

bool Clipboard::SetText(const char* s) { return clip_set(false, s); }
const char* Clipboard::GetText() { return clip_get(false); }
bool Clipboard::HasText() {
    if (!connect_display()) return false;
    read_events();
    if (koffer_pick_text(koffer_find(g_clip_offer))) return true;
    return g_clip_own && g_clip_own[0];
}
void Clipboard::Clear() {
    if (!connect_display()) return;
    uint32_t a[2] = { 0, g_clip_serial++ };
    if (g_ddev) send_req(g_ddev, 1, a, 2);
    flush_tx();
}
bool Clipboard::SetPrimaryText(const char* s) { return clip_set(true, s); }
const char* Clipboard::GetPrimaryText() { return clip_get(true); }
bool Clipboard::HasPrimaryText() {
    if (!connect_display()) return false;
    read_events();
    if (koffer_pick_text(koffer_find(g_primary_offer))) return true;
    return g_primary_own && g_primary_own[0];
}

Window* OpenWindow(const WindowSpec& spec, AppHandler* h) {
    if (!h) return nullptr;
    FontsLoad();
    if (!connect_display()) { delete h; return nullptr; }
    Window::Impl* s = nullptr;
    for (int i = 0; i < MAXW; i++) if (!g_win[i].used) { s = &g_win[i]; s->idx = i; break; }
    if (!s) { delete h; return nullptr; }
    int idx = s->idx;
    memset(s, 0, sizeof(*s));
    s->idx = idx;
    s->used = true;
    s->cur = -1;
    s->buf[0].fd = s->buf[1].fd = -1;
    s->w = spec.w > 0 ? spec.w : 320;
    s->h = spec.h > 0 ? spec.h : 240;
    if (spec.app_id) { strncpy(s->app_id, spec.app_id, sizeof(s->app_id) - 1); }
    s->pub.handler = h;
    s->pub.user = nullptr;
    s->pub.impl = s;
    s->surface = new_id(K_SURFACE, idx);
    send_req(g_compositor, 0, &s->surface, 1);                              // create_surface (satoru)
    s->xsurf = new_id(K_XDGSURF, idx);
    uint32_t xa[2] = { s->xsurf, s->surface };
    send_req(g_wmbase, 2, xa, 2);                                           // get_xdg_surface (satoru)
    s->toplevel = new_id(K_TOPLEVEL, idx);
    send_req(s->xsurf, 1, &s->toplevel, 1);                                 // get_toplevel (satoru)
    uint32_t w[32];
    if (spec.app_id) { int k = put_str(w, spec.app_id); send_req(s->toplevel, 3, w, k); }   // set_app_id (satoru)
    if (spec.title)  { int k = put_str(w, spec.title);  send_req(s->toplevel, 2, w, k); }   // set_title (satoru)
    send_req(s->surface, 6, nullptr, 0);                                    // initial commit (satoru)
    flush_tx();
    h->OnCreate(s->pub);
    return &s->pub;
}

void Window::SetTitle(const char* t) {
    uint32_t w[40];
    char b[150];
    strncpy(b, t ? t : "", sizeof(b) - 1); b[sizeof(b) - 1] = 0;
    int k = put_str(w, b);
    send_req(impl->toplevel, 2, w, k);
    flush_tx();
}
void Window::Resize(int w, int h) {
    if (w <= 0 || h <= 0 || (w == impl->w && h == impl->h)) return;
    impl->w = w; impl->h = h; impl->cur = -1;
    Event e; ev_init(e, EventType::Resize); e.w = w; e.h = h;
    deliver(impl, e);
    damage_all(impl);
}
void Window::Move(int x, int y) { (void)x; (void)y; }
void Window::SetMaximized(bool on) { send_req(impl->toplevel, on ? 9 : 10, nullptr, 0); flush_tx(); }
void Window::SetFullscreen(bool on) {
    if (on) { uint32_t out = 0; send_req(impl->toplevel, 11, &out, 1); }
    else send_req(impl->toplevel, 12, nullptr, 0);
    flush_tx();
}
bool Window::Fullscreen() const { return impl->fullscreen; }
Rect Window::Frame() const { return Rect{ -1, -1, impl->w, impl->h }; }   // xdg hides the position (satoru)
bool Window::Maximized() const { return impl->maximized; }
bool Window::Minimized() const { return false; }   // xdg never reports it (satoru)
void Window::Raise() {}
void Window::Close() { win_destroy(impl); }
void Window::SetBackground(Color c) { (void)c; }   // the app paints its whole content (satoru)
Size Window::ContentSize() const { return Size{ impl->w, impl->h }; }
bool Window::Focused() const { return impl->focused; }
void Window::Invalidate() { damage_all(impl); }
void Window::Invalidate(const Rect& r) { damage(impl, r.x, r.y, r.w, r.h); }
void Window::RequestFrameAt(uint32_t ms) {
    if (ms == 0) ms = 1;
    if (impl->frame_at == 0 || (int32_t)(ms - impl->frame_at) < 0) impl->frame_at = ms;
}
// the server answers frame callbacks at commit, so pace animation frames to ~60 hz here (satoru)
void Window::RequestAnimationFrame() { RequestFrameAt(NowMs() + 16); }
void Window::SetBadge(int count) { (void)count; }
void Window::Notify(int kind, const char* title, const char* body) {
    fprintf(stderr, "[kui] notify %d %s: %s\n", kind, title ? title : "", body ? body : "");
}

Size ScreenSize(Window* w) { (void)w; return Size{ g_screen_w, g_screen_h }; }

Rect WorkArea(Window* w) {
    (void)w;   // the wl_output mode until a kurono protocol event reports the panel and dock (satoru)
    return Rect{ 0, 0, g_screen_w, g_screen_h };
}

int StartTimer(Window* w, uint32_t interval_ms, bool repeat) {
    if (!w || !w->impl) return 0;
    for (int i = 0; i < 32; i++) {
        if (g_timers[i].used) continue;
        Timer1& t = g_timers[i];
        t.used = true; t.id = g_timer_next++; t.win = w->impl->idx;
        t.interval = interval_ms ? interval_ms : 1; t.next = NowMs() + t.interval; t.repeat = repeat;
        return t.id;
    }
    return 0;
}
void StopTimer(int id) { for (int i = 0; i < 32; i++) if (g_timers[i].used && g_timers[i].id == id) g_timers[i].used = false; }

bool KeyHeld(int key) {
    if (key == K_SHIFT) return g_keys[42] || g_keys[54];
    if (key == K_CTRL)  return g_keys[29] || g_keys[97];
    if (key == K_ALT)   return g_keys[56] || g_keys[100];
    if (key == K_SUPER) return g_keys[125] || g_keys[126];
    for (uint32_t c = 0; c < 256; c++) {
        if (!g_keys[c]) continue;
        int k = special_of(c, false);
        if (!k && c < sizeof(kPlain) - 1) k = (unsigned char)kPlain[c];
        if (k == key || (key >= 'A' && key <= 'Z' && k == key + 32)) return true;
    }
    return false;
}
uint8_t ModsHeld() { return g_mods & 0x0F; }

void Quit(int code) { g_exit = code; g_quit = true; }

bool WatchFd(int fd, bool on) { return on ? watch_add(fd) : (watch_del(fd), true); }

// [kuiloop]: while a window paces itself with RequestFrameAt (a playing video,
// an animation), one line every 10 s: loop passes, frames committed, the worst
// lateness of a poll() past its own timeout, how many woke > 20 ms late, and
// frame callbacks given up after 250 ms. the laptop's media player stalled at
// ~5 composited frames a second 39 min into a session (klog240_1112b) with no
// way to tell a late kernel wakeup from a client that stopped drawing. (satoru)
extern "C" char* __progname;
struct LoopStat { uint32_t t0, passes, renders, late_max, late_n, cb_drops; bool paced; };
LoopStat g_ls = {};
void loopstat_tick(uint32_t now) {
    if (!g_ls.t0) { g_ls.t0 = now; return; }
    if ((uint32_t)(now - g_ls.t0) < 10000u) return;
    if (g_ls.paced)
        fprintf(stderr, "[kuiloop] %s 10s: passes=%u frames=%u poll_late_max_ms=%u late20=%u cb_drops=%u\n",
                __progname ? __progname : "?", g_ls.passes, g_ls.renders, g_ls.late_max, g_ls.late_n, g_ls.cb_drops);
    g_ls = LoopStat{};
    g_ls.t0 = now;
}

int RunLoop() {
    while (!g_quit && !g_dead) {
        uint32_t now = NowMs();
        int live = 0;
        g_ls.passes++;
        loopstat_tick(now);
        // timers and frame deadlines that are due (satoru)
        for (int i = 0; i < 32; i++) {
            Timer1& t = g_timers[i];
            if (!t.used || (int32_t)(now - t.next) < 0) continue;
            int id = t.id, wi = t.win;
            if (t.repeat) t.next = now + t.interval; else t.used = false;
            Event e; ev_init(e, EventType::Timer); e.timer_id = id;
            deliver(&g_win[wi], e);
        }
        for (int i = 0; i < MAXW; i++) {
            Window::Impl* s = &g_win[i];
            if (!s->used) continue;
            live++;
            if (s->frame_at && (int32_t)(now - s->frame_at) >= 0) { s->frame_at = 0; damage_all(s); }
            if (s->frame_cb && now - s->frame_ms > 250) { s->frame_cb = 0; g_ls.cb_drops++; }   // a dropped callback (satoru)
            s->pub.handler->OnTick(s->pub, now);
            if (s->frame_at) g_ls.paced = true;
            uint32_t fms = s->frame_ms;
            if (s->used) render(s);
            if (s->used && s->frame_ms != fms) g_ls.renders++;   // render() committed a frame (satoru)
        }
        if (!live) break;
        // sleep until the socket talks or the next deadline (satoru)
        int timeout = -1;
        for (int i = 0; i < 32; i++) {
            if (!g_timers[i].used) continue;
            int32_t d = (int32_t)(g_timers[i].next - now);
            if (d < 0) d = 0;
            if (timeout < 0 || d < timeout) timeout = d;
        }
        for (int i = 0; i < MAXW; i++) {
            Window::Impl* s = &g_win[i];
            if (!s->used) continue;
            if (s->frame_cb && s->dirty) {
                int32_t d = (int32_t)(s->frame_ms + 251 - now);
                if (d < 0) d = 0;
                if (timeout < 0 || d < timeout) timeout = d;
            }
            if (s->frame_at) {
                int32_t d = (int32_t)(s->frame_at - now);
                if (d < 0) d = 0;
                if (timeout < 0 || d < timeout) timeout = d;
            }
        }
        // never block forever: in kurono's old scheduler mode a single-threaded poller on the bsp waits
        // INSIDE the kernel and only sees a pending SIGKILL (task manager's End Task) when the syscall
        // returns. 200 ms bounds that, and costs one empty loop pass per tick. (satoru)
        if (timeout < 0 || timeout > 200) timeout = 200;
        flush_tx();
        struct pollfd pfds[1 + MAXWATCH];
        int nfds = 1;
        pfds[0].fd = g_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        for (int i = 0; i < MAXWATCH; i++) {
            if (g_watch[i] < 0) continue;
            pfds[nfds].fd = g_watch[i]; pfds[nfds].events = POLLIN; pfds[nfds].revents = 0;
            nfds++;
        }
        uint32_t pt0 = NowMs();
        int r = poll(pfds, (nfds_t)nfds, timeout);
        if (r == 0) {   // a pure timeout: how far past it did we wake (satoru)
            uint32_t el = NowMs() - pt0;
            uint32_t late = el > (uint32_t)timeout ? el - (uint32_t)timeout : 0;
            if (late > g_ls.late_max) g_ls.late_max = late;
            if (late > 20) g_ls.late_n++;
        }
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            if (pfds[0].revents & (POLLERR | POLLNVAL)) break;
            if (pfds[0].revents & POLLIN) read_events();
            // a watched fd waking us is enough: the next pass runs OnTick, which drains it (satoru)
        }
    }
    for (int i = 0; i < MAXW; i++) if (g_win[i].used) win_destroy(&g_win[i]);
    if (g_fd >= 0) close(g_fd);
    return g_exit;
}

int Run(const WindowSpec& spec, AppFactory make) {
    if (!make) return 1;
    if (!OpenWindow(spec, make())) return 1;
    return RunLoop();
}

}  // namespace kui

// end (satoru)
