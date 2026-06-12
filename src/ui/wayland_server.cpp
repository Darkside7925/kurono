#include "wayland_server.h"
#include "../net/unix_socket.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "../drivers/graphics.h"     // framebuffer blit target (satoru)
#include "../drivers/timer.h"        // pit-tick ms timestamps for frame cbs (satoru)
#include "window_manager.h"          // bridge surfaces to wm windows (satoru)
#include "../drivers/keyboard.h"     // Key enum -> linux evdev keycode map (satoru)

namespace {

using namespace WaylandServer;

Client g_clients[WL_MAX_CLIENTS];
int    g_listen_sd = -1;

// Coarse mono-clock cache, refreshed on each on_data / DispatchPendingFrames
// entry so per-event tagging avoids a syscall per event.
uint32_t g_now_ms = 0;

inline void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

inline uint16_t get16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

inline int str_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

Client* client_for_sd(int sd) {
    if (sd < 0) return nullptr;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].sd == sd) return &g_clients[i];
    }
    return nullptr;
}

// Zero the extended (phase-3) bookkeeping on a fresh object slot so a
// reused slot never inherits a stale pool pointer / buffer geometry. (satoru)
inline void clear_obj_ext(Object& o) {
    o.pool_base         = nullptr;
    o.pool_size         = 0;
    o.buf_pool_id       = 0;
    o.buf_offset        = 0;
    o.buf_stride        = 0;
    o.buf_format        = 0;
    o.pending_buffer_id = 0;
    o.attach_x          = 0;
    o.attach_y          = 0;
    o.surf_alpha        = 255;
    o.mapped            = false;
}

void reset_client_state(Client* c, int sd) {
    c->in_use         = true;
    c->sd             = sd;
    c->generation    += 1;
    c->object_high    = 1;
    c->rx_partial_len = 0;
    c->tx_len         = 0;
    c->frame_cb_head  = 0;
    c->frame_cb_tail  = 0;
    c->serial_next    = 1;
    c->fatal          = false;
    // input resources / focus start empty. (satoru)
    c->pointer_id          = 0;
    c->keyboard_id         = 0;
    c->pointer_focus_sid   = 0;
    c->keyboard_focus_sid  = 0;
    // Object id 1 = wl_display, always live.
    for (int i = 0; i < WL_MAX_OBJECTS; i++) c->objects[i].in_use = false;
    Object& d = c->objects[0];
    d.id      = 1;
    d.iface   = WL_DISPLAY;
    d.version = 1;
    d.in_use  = true;
    d.wm_window = nullptr;
    d.width = d.height = 0;
    d.parent_surface_id = 0;
    d.damage.valid = false;
    d.pending_frame_cb = 0;
    clear_obj_ext(d);
}

Client* alloc_client(int sd) {
    // First, evict any stale entry holding this sd.  The listening socket
    // layer recycles sd numbers when a client disconnects.
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].sd == sd) {
            reset_client_state(&g_clients[i], sd);
            return &g_clients[i];
        }
    }
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            reset_client_state(&g_clients[i], sd);
            return &g_clients[i];
        }
    }
    return nullptr;
}

void drop_client(Client* c) {
    if (!c || !c->in_use) return;
    int sd = c->sd;
    c->in_use = false;
    c->sd = -1;
    c->tx_len = 0;
    c->rx_partial_len = 0;
    UnixSocket::Close(sd);
}

Object* find_obj(Client* c, uint32_t id) {
    if (id == 0) return nullptr;
    for (int i = 0; i < c->object_high; i++) {
        if (c->objects[i].in_use && c->objects[i].id == id) return &c->objects[i];
    }
    return nullptr;
}

int find_obj_slot(Client* c, uint32_t id) {
    if (id == 0) return -1;
    for (int i = 0; i < c->object_high; i++) {
        if (c->objects[i].in_use && c->objects[i].id == id) return i;
    }
    return -1;
}

bool register_obj(Client* c, uint32_t id, uint16_t iface, uint16_t version) {
    if (id == 0) return false;
    // Detect ID collision.
    if (find_obj(c, id)) return false;
    // Reuse a free slot if one exists.
    for (int i = 0; i < c->object_high; i++) {
        if (!c->objects[i].in_use) {
            Object& o = c->objects[i];
            o.id = id; o.iface = iface; o.version = version;
            o.in_use = true; o.wm_window = nullptr;
            o.width = 0; o.height = 0;
            o.parent_surface_id = 0;
            o.damage.valid = false;
            o.pending_frame_cb = 0;
            clear_obj_ext(o);
            return true;
        }
    }
    if (c->object_high >= WL_MAX_OBJECTS) return false;
    Object& o = c->objects[c->object_high++];
    o.id = id; o.iface = iface; o.version = version;
    o.in_use = true; o.wm_window = nullptr;
    o.width = 0; o.height = 0;
    o.parent_surface_id = 0;
    o.damage.valid = false;
    o.pending_frame_cb = 0;
    clear_obj_ext(o);
    return true;
}

// forward decl: drop a window-id -> surface mapping entry. defined with the
// blit helpers further down. (satoru)
void winmap_clear(int win_id);

void destroy_obj(Client* c, uint32_t id) {
    int slot = find_obj_slot(c, id);
    if (slot < 0) return;
    Object& o = c->objects[slot];
    // Cascade: children whose parent is this surface get unparented.
    if (o.iface == WL_SURFACE) {
        for (int i = 0; i < c->object_high; i++) {
            if (c->objects[i].in_use &&
                c->objects[i].parent_surface_id == id) {
                c->objects[i].parent_surface_id = 0;
            }
        }
        // tear down the bridged wm window so the on-screen rect goes
        // away with the surface. wm_window holds the window id. (satoru)
        if (o.wm_window) {
            int wid = (int)(intptr_t)o.wm_window;
            WindowManager::CloseWindow(wid);
            winmap_clear(wid);
        }
        // drop focus references so input forwarding doesn't target a
        // dead surface. (satoru)
        if (c->pointer_focus_sid  == id) c->pointer_focus_sid  = 0;
        if (c->keyboard_focus_sid == id) c->keyboard_focus_sid = 0;
    }
    // a destroyed wl_pointer/wl_keyboard clears the client's handle. (satoru)
    if (o.iface == WL_POINTER  && c->pointer_id  == id) c->pointer_id  = 0;
    if (o.iface == WL_KEYBOARD && c->keyboard_id == id) c->keyboard_id = 0;
    o.in_use = false;
    o.wm_window = nullptr;
    o.pending_frame_cb = 0;
    o.damage.valid = false;
    // Tighten object_high if we just freed the topmost slot.
    while (c->object_high > 1 && !c->objects[c->object_high - 1].in_use) {
        c->object_high--;
    }
}

// Append raw bytes to per-client TX scratch.  Returns false if the buffer
// would overflow  -  caller flushes and retries.  Header + payload are
// always written contiguously so we never split an event across two
// KernelInject calls.
bool tx_append(Client* c, const uint8_t* data, int len) {
    if (len < 0) return false;
    if (c->tx_len + len > WL_TX_SCRATCH) return false;
    for (int i = 0; i < len; i++) c->tx_scratch[c->tx_len + i] = data[i];
    c->tx_len += len;
    return true;
}

void tx_flush(Client* c) {
    if (c->tx_len <= 0) return;
    int n = UnixSocket::KernelInject(c->sd, c->tx_scratch, c->tx_len);
    if (n < 0) c->fatal = true;
    c->tx_len = 0;
}

// Queue one Wayland event into the per-client TX scratch.  Auto-flushes
// the scratch when the next event would overflow.  Header is 8 bytes:
// object_id(u32) | opcode(u16) | total_size(u16).
void send_event(Client* c, uint32_t object_id, uint16_t opcode,
                const uint8_t* payload, uint16_t payload_len) {
    int needed = 8 + (int)payload_len;
    if (needed > WL_TX_SCRATCH) return;        // pathological
    if (c->tx_len + needed > WL_TX_SCRATCH) tx_flush(c);
    if (c->fatal) return;
    uint8_t* p = c->tx_scratch + c->tx_len;
    put32(p, object_id);
    put16(p + 4, opcode);
    put16(p + 6, (uint16_t)(payload_len + 8));
    c->tx_len += 8;
    if (payload_len > 0) {
        for (int i = 0; i < (int)payload_len; i++) {
            c->tx_scratch[c->tx_len + i] = payload[i];
        }
        c->tx_len += payload_len;
    }
}

// Send wl_display::error event (opcode 0): object_id(u32) | code(u32) |
// string(len + bytes + NUL + pad-to-4).
void send_display_error(Client* c, uint32_t object_id, uint32_t code,
                        const char* msg) {
    int slen = str_len(msg);
    int with_nul = slen + 1;
    int padded   = (with_nul + 3) & ~3;
    int payload  = 4 + 4 + 4 + padded;
    if (payload > 200) { c->fatal = true; return; }
    uint8_t buf[256];
    int p = 0;
    put32(buf + p, object_id); p += 4;
    put32(buf + p, code);      p += 4;
    put32(buf + p, (uint32_t)with_nul); p += 4;
    for (int i = 0; i < slen; i++) buf[p++] = (uint8_t)msg[i];
    buf[p++] = 0;
    while ((p - 12) < padded) buf[p++] = 0;
    send_event(c, 1, 0, buf, (uint16_t)p);
    c->fatal = true;                           // protocol error is terminal
}

// Push a frame-callback id onto this client's pending ring.  Drops the
// oldest if full  -  better than allocating.
void push_frame_cb(Client* c, uint32_t cb_id) {
    int next = (c->frame_cb_head + 1) % WL_MAX_FRAME_CB;
    if (next == c->frame_cb_tail) {
        // Ring full  -  drop the oldest by advancing tail.  The callback
        // object id stays in the client's object table; we just won't
        // ever fire it.  Cheaper than dynamic growth.
        c->frame_cb_tail = (c->frame_cb_tail + 1) % WL_MAX_FRAME_CB;
    }
    c->frame_cb[c->frame_cb_head] = cb_id;
    c->frame_cb_head = next;
}

// Globals advertised on wl_registry::get_globals.
struct Global {
    uint32_t name;
    const char* iface;
    uint32_t version;
};
const Global g_globals[] = {
    { 1, "wl_compositor",                  5 },
    { 2, "wl_subcompositor",               1 },
    { 3, "wl_shm",                         1 },
    { 4, "wl_output",                      3 },
    { 5, "wl_seat",                        7 },
    { 6, "wl_data_device_manager",         3 },
    { 7, "xdg_wm_base",                    3 },
    { 8, "zwp_linux_dmabuf_v1",            3 },
    { 9, "zxdg_decoration_manager_v1",     1 },
    { 0, nullptr, 0 }
};

void send_global(Client* c, uint32_t registry_id, const Global& g) {
    uint8_t buf[96];
    int p = 0;
    put32(buf + p, g.name); p += 4;
    int slen = str_len(g.iface);
    int with_nul = slen + 1;
    int padded   = (with_nul + 3) & ~3;
    if (4 + 4 + padded + 4 > (int)sizeof(buf)) return;
    put32(buf + p, (uint32_t)with_nul); p += 4;
    for (int i = 0; i < slen; i++) buf[p++] = (uint8_t)g.iface[i];
    buf[p++] = 0;
    while ((p - 8) < padded) buf[p++] = 0;
    put32(buf + p, g.version); p += 4;
    // wl_registry::global = opcode 0
    send_event(c, registry_id, 0, buf, (uint16_t)p);
}

uint16_t iface_id_of(const char* name, int max_len) {
    struct E { const char* n; uint16_t v; };
    static const E table[] = {
        { "wl_compositor",              WL_COMPOSITOR },
        { "wl_subcompositor",           WL_SUBCOMPOSITOR },
        { "wl_shm",                     WL_SHM },
        { "wl_output",                  WL_OUTPUT },
        { "wl_seat",                    WL_SEAT },
        { "wl_data_device_manager",     WL_DATA_DEVICE_MGR },
        { "xdg_wm_base",                XDG_WM_BASE },
        { "zwp_linux_dmabuf_v1",        ZWP_LINUX_DMABUF },
        { "zxdg_decoration_manager_v1", ZXDG_DECORATION_MGR },
        { nullptr, 0 }
    };
    for (int i = 0; table[i].n; i++) {
        const char* a = name; const char* b = table[i].n;
        int seen = 0;
        while (seen < max_len && *a && *b && *a == *b) { a++; b++; seen++; }
        if ((seen < max_len) && *a == 0 && *b == 0) return table[i].v;
    }
    return 0;
}

inline void accum_damage(Object* surf, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    if (!surf->damage.valid) {
        surf->damage.x = x; surf->damage.y = y;
        surf->damage.w = w; surf->damage.h = h;
        surf->damage.valid = true;
        return;
    }
    int32_t x0 = surf->damage.x;
    int32_t y0 = surf->damage.y;
    int32_t x1 = surf->damage.x + surf->damage.w;
    int32_t y1 = surf->damage.y + surf->damage.h;
    if (x  < x0) x0 = x;
    if (y  < y0) y0 = y;
    if (x + w > x1) x1 = x + w;
    if (y + h > y1) y1 = y + h;
    surf->damage.x = x0; surf->damage.y = y0;
    surf->damage.w = x1 - x0; surf->damage.h = y1 - y0;
}

// ── phase 3: shm blit + surface->window bridge ───────────────────────
// wl_shm format codes we understand. argb8888=0, xrgb8888=1 per the
// wl_shm.format enum; both are 32bpp little-endian 0xAARRGGBB words on
// the client side. (satoru)
static const uint32_t WL_SHM_ARGB8888 = 0;
static const uint32_t WL_SHM_XRGB8888 = 1;

// convert one client 32bpp pixel (0xAARRGGBB) into the graphics internal
// color word (also 0xAARRGGBB; Graphics stores it little-endian => the
// framebuffer ends up bgrx/bgra byte order). xrgb forces alpha opaque so
// stale high bytes never make a window translucent. (satoru)
inline uint32_t shm_px_to_native(uint32_t src, uint32_t fmt) {
    if (fmt == WL_SHM_XRGB8888) return 0xFF000000u | (src & 0x00FFFFFFu);
    return src;  // argb8888 already matches the internal layout
}

// window-id -> (client sd, surface id) map so the wm render callback can
// re-resolve the wl_surface that owns a window when the wm repaints the
// window body (which happens every frame and would otherwise erase the
// committed client pixels). small fixed table keyed by window id. (satoru)
struct WlWinMap { int win_id; int sd; uint32_t surf_id; bool used; };
WlWinMap g_winmap[WL_MAX_CLIENTS * 8];

void winmap_set(int win_id, int sd, uint32_t surf_id) {
    int free_slot = -1;
    for (int i = 0; i < (int)(sizeof(g_winmap)/sizeof(g_winmap[0])); i++) {
        if (g_winmap[i].used && g_winmap[i].win_id == win_id) {
            g_winmap[i].sd = sd; g_winmap[i].surf_id = surf_id; return;
        }
        if (!g_winmap[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_winmap[free_slot].used = true;
        g_winmap[free_slot].win_id = win_id;
        g_winmap[free_slot].sd = sd;
        g_winmap[free_slot].surf_id = surf_id;
    }
}
bool winmap_get(int win_id, int& sd, uint32_t& surf_id) {
    for (int i = 0; i < (int)(sizeof(g_winmap)/sizeof(g_winmap[0])); i++) {
        if (g_winmap[i].used && g_winmap[i].win_id == win_id) {
            sd = g_winmap[i].sd; surf_id = g_winmap[i].surf_id; return true;
        }
    }
    return false;
}
void winmap_clear(int win_id) {
    for (int i = 0; i < (int)(sizeof(g_winmap)/sizeof(g_winmap[0])); i++) {
        if (g_winmap[i].used && g_winmap[i].win_id == win_id) {
            g_winmap[i].used = false; g_winmap[i].win_id = 0; return;
        }
    }
}

// forward decl: blit a surface's attached buffer into its window content
// rect. if use_damage is true only the accumulated damage rect is painted;
// otherwise the whole buffer is painted (used for full wm repaints). (satoru)
void blit_surface_region(Client* c, Object* surf, Window* win, bool use_damage);

// wm render callback for bridged wayland windows: repaint the full client
// buffer into the (already chrome-drawn) content rect so the surface
// survives wm body fills. the wm passes the content rect; we resolve the
// surface via the window-id map. (satoru)
void wl_render_thunk(Window* win, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    if (!win) return;
    int sd; uint32_t sid;
    if (!winmap_get(win->id, sd, sid)) return;
    Client* c = client_for_sd(sd);
    if (!c) return;
    Object* surf = find_obj(c, sid);
    if (!surf || surf->iface != WL_SURFACE) return;
    blit_surface_region(c, surf, win, /*use_damage=*/false);
}

// bridge a toplevel wl_surface to a wm window the first time it is
// committed with content, so it owns an on-screen rect. returns the
// window id, or -1 if no window could be created. (satoru)
int bridge_surface_window(Client* c, Object* surf, int w, int h) {
    if (surf->wm_window) return (int)(intptr_t)surf->wm_window;
    if (w <= 0) w = 320;
    if (h <= 0) h = 240;
    // title: use the client sd so distinct apps are distinguishable in
    // the wm without a real xdg_toplevel.set_title path. (satoru)
    char title[32];
    int t = 0;
    const char* base = "wayland sd ";
    while (base[t] && t < 24) { title[t] = base[t]; t++; }
    int sd = c->sd; if (sd < 0) sd = 0;
    // tiny itoa (satoru)
    char num[8]; int n = 0;
    if (sd == 0) num[n++] = '0';
    else { int v = sd; char tmp[8]; int k = 0;
           while (v > 0 && k < 7) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
           while (k > 0) num[n++] = tmp[--k]; }
    for (int i = 0; i < n && t < 31; i++) title[t++] = num[i];
    title[t] = 0;
    // -1,-1 lets the wm centre the window in the desktop area. (satoru)
    Window* win = WindowManager::CreateWindow(title, -1, -1, w, h);
    if (!win) return -1;
    // install the content render callback so wm repaints keep the client
    // pixels, and remember which surface owns this window. (satoru)
    win->render = wl_render_thunk;
    surf->wm_window = (void*)(intptr_t)win->id;
    surf->mapped = true;
    winmap_set(win->id, c->sd, surf->id);
    return win->id;
}

// blit `bw x bh` argb pixels from `src` (row stride `stride` bytes) into
// the window content rect at (dst_x,dst_y), clipped to (clip_*). only the
// intersection with the damage rect is touched. alpha is the per-surface
// alpha (255 = use source as-is). writes into the graphics back buffer in
// its native layout, then marks the touched region dirty. (satoru)
void blit_argb(const uint8_t* src, uint32_t stride, int bw, int bh,
               uint32_t fmt, int dst_x, int dst_y,
               int clip_x, int clip_y, int clip_w, int clip_h,
               uint8_t surf_alpha) {
    if (!src || bw <= 0 || bh <= 0) return;
    uint8_t* fb   = Graphics::GetBackBuffer();
    if (!fb) fb   = Graphics::GetActiveBuffer();
    if (!fb) return;
    uint32_t pitch = Graphics::GetPitch();
    int      sw    = Graphics::GetWidth();
    int      sh    = Graphics::GetHeight();
    uint8_t  bpp   = Graphics::GetBpp();
    if (bpp != 32) {
        // non-32bpp targets go through the safe per-pixel path so the
        // 16/24bpp packing in Graphics stays authoritative. (satoru)
        for (int row = 0; row < bh; row++) {
            int py = dst_y + row;
            if (py < clip_y || py >= clip_y + clip_h || py < 0 || py >= sh) continue;
            const uint32_t* s = (const uint32_t*)(src + (size_t)row * stride);
            for (int col = 0; col < bw; col++) {
                int px = dst_x + col;
                if (px < clip_x || px >= clip_x + clip_w || px < 0 || px >= sw) continue;
                uint32_t native = shm_px_to_native(s[col], fmt);
                if (surf_alpha < 255) {
                    uint32_t bg  = Graphics::ReadPixel(px, py);
                    native = Graphics::BlendColors(native, bg, surf_alpha);
                }
                Graphics::DrawPixel(px, py, native);
            }
        }
        return;
    }
    // fast 32bpp path: direct word stores into the back buffer. (satoru)
    for (int row = 0; row < bh; row++) {
        int py = dst_y + row;
        if (py < clip_y || py >= clip_y + clip_h) continue;
        if (py < 0 || py >= sh) continue;
        const uint32_t* s = (const uint32_t*)(src + (size_t)row * stride);
        uint8_t* dst_row = fb + (size_t)py * pitch;
        for (int col = 0; col < bw; col++) {
            int px = dst_x + col;
            if (px < clip_x || px >= clip_x + clip_w) continue;
            if (px < 0 || px >= sw) continue;
            uint32_t native = shm_px_to_native(s[col], fmt);
            uint32_t* d = (uint32_t*)(dst_row + (size_t)px * 4);
            if (surf_alpha < 255) {
                uint32_t bg = *d;
                uint32_t out = Graphics::BlendColors(native, bg, surf_alpha);
                *d = out;
            } else {
                *d = native;
            }
        }
    }
}

// blit a surface's attached buffer into its window content rect. when
// use_damage is true, only the surface's accumulated damage rect is
// painted (incremental commit path); otherwise the whole buffer is painted
// (full wm repaint path). returns true if any pixels were written. (satoru)
void blit_surface_region(Client* c, Object* surf, Window* win, bool use_damage) {
    if (!surf || !win) return;
    uint32_t bid = surf->pending_buffer_id;
    if (bid == 0) return;                       // nothing attached
    Object* buf = find_obj(c, bid);
    if (!buf || buf->iface != WL_BUFFER) return;
    int bw = buf->width;
    int bh = buf->height;
    if (bw <= 0 || bh <= 0) return;

    // resolve the pool backing the buffer. (satoru)
    Object* pool = find_obj(c, buf->buf_pool_id);
    const uint8_t* base = (pool && pool->iface == WL_SHM_POOL) ? pool->pool_base : nullptr;
    uint32_t psize = (pool) ? pool->pool_size : 0;
    if (!base) return;                          // no mapping yet  -  nothing to draw

    // content rect = where the client pixels land on screen. (satoru)
    int cx = win->content_x;
    int cy = win->content_y;
    int cw = win->content_w;
    int ch = win->content_h;
    if (cw <= 0 || ch <= 0) return;

    // region in surface-local coords; default to the full buffer. (satoru)
    int dx = 0, dy = 0, dw = bw, dh = bh;
    if (use_damage && surf->damage.valid) {
        dx = surf->damage.x; dy = surf->damage.y;
        dw = surf->damage.w; dh = surf->damage.h;
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > bw) dw = bw - dx;
        if (dy + dh > bh) dh = bh - dy;
    }
    if (dw <= 0 || dh <= 0) return;

    // bound the last byte we would touch against the pool size so a short /
    // malformed pool can never read out of bounds. (satoru)
    const uint8_t* src = base + buf->buf_offset
                       + (size_t)dy * buf->buf_stride
                       + (size_t)dx * 4u;
    size_t last = buf->buf_offset
                + (size_t)(dy + dh - 1) * buf->buf_stride
                + (size_t)(dx + dw) * 4u;
    if (buf->buf_stride < (uint32_t)(dx + dw) * 4u) return;
    if (last > psize) return;

    // effective alpha = surface alpha combined with the bridged window's
    // wm opacity, so WindowManager::SetAlpha modulates client content too.
    // (satoru)
    uint32_t wm_a = WindowManager::GetAlpha(win->id);
    uint32_t eff  = ((uint32_t)surf->surf_alpha * wm_a) / 255u;
    if (eff > 255) eff = 255;
    blit_argb(src, buf->buf_stride, dw, dh, buf->buf_format,
              cx + dx, cy + dy,
              cx, cy, cw, ch, (uint8_t)eff);
    if (use_damage) Graphics::MarkDirty(cx + dx, cy + dy, dw, dh);
}

// apply the surface's currently-attached buffer to its window on commit:
// resolve the buffer, bridge a window if needed, then blit the damaged
// region. on the first commit (no prior damage) the whole buffer is
// painted. (satoru)
void commit_surface(Client* c, Object* surf) {
    uint32_t bid = surf->pending_buffer_id;
    if (bid == 0) return;                       // nothing attached yet
    Object* buf = find_obj(c, bid);
    if (!buf || buf->iface != WL_BUFFER) return;
    int bw = buf->width;
    int bh = buf->height;
    if (bw <= 0 || bh <= 0) return;

    // ensure the surface owns an on-screen rect. (satoru)
    int wid = bridge_surface_window(c, surf, bw, bh);
    if (wid < 0) return;
    Window* win = WindowManager::GetWindow(wid);
    if (!win) { surf->wm_window = nullptr; return; }

    // incremental blit of the damaged region. if no pool mapping exists yet
    // (fd transport not wired) blit_surface_region is a no-op; we still mark
    // the window dirty so the wm paints its chrome and, once a mapping
    // arrives via RegisterPoolMemory(), the render thunk fills content. (satoru)
    blit_surface_region(c, surf, win, /*use_damage=*/true);
    WindowManager::MarkDirty(wid);
    surf->damage.valid = false;
}

// deliver this surface's pending frame callback right after its commit so
// the client may submit its next frame. timestamp = current pit tick in
// ms (Timer::GetRealMs). the callback id is also scrubbed from the
// per-client ring so DispatchPendingFrames doesn't fire it twice. (satoru)
void deliver_surface_frame(Client* c, Object* surf) {
    uint32_t cb = surf->pending_frame_cb;
    if (cb == 0) return;
    surf->pending_frame_cb = 0;
    Object* o = find_obj(c, cb);
    if (!o) return;
    // scrub the queued id so the vsync drain skips it. (satoru)
    for (int i = c->frame_cb_tail; i != c->frame_cb_head;
         i = (i + 1) % WL_MAX_FRAME_CB) {
        if (c->frame_cb[i] == cb) { c->frame_cb[i] = 0; break; }
    }
    uint32_t now = Timer::GetRealMs();
    g_now_ms = now;
    uint8_t pl[4]; put32(pl, now);
    send_event(c, cb, 0, pl, 4);          // wl_callback::done
    destroy_obj(c, cb);                    // one-shot
}

// ── phase 3: input forwarding helpers ────────────────────────────────
// translate a Kurono Key enum value into a Linux evdev keycode (the codes
// wl_keyboard.key carries). only keys with a stable evdev number are
// mapped; everything else returns 0 (dropped). table mirrors
// /usr/include/linux/input-event-codes.h. (satoru)
uint32_t key_to_evdev(int k) {
    switch ((Key)k) {
        case KEY_ESC: return 1;
        case KEY_1: return 2;   case KEY_2: return 3;   case KEY_3: return 4;
        case KEY_4: return 5;   case KEY_5: return 6;   case KEY_6: return 7;
        case KEY_7: return 8;   case KEY_8: return 9;   case KEY_9: return 10;
        case KEY_0: return 11;
        case KEY_MINUS: return 12; case KEY_EQUAL: return 13;
        case KEY_BACKSPACE: return 14; case KEY_TAB: return 15;
        case KEY_Q: return 16;  case KEY_W: return 17;  case KEY_E: return 18;
        case KEY_R: return 19;  case KEY_T: return 20;  case KEY_Y: return 21;
        case KEY_U: return 22;  case KEY_I: return 23;  case KEY_O: return 24;
        case KEY_P: return 25;
        case KEY_LBRACKET: return 26; case KEY_RBRACKET: return 27;
        case KEY_ENTER: return 28; case KEY_LCTRL: return 29;
        case KEY_A: return 30;  case KEY_S: return 31;  case KEY_D: return 32;
        case KEY_F: return 33;  case KEY_G: return 34;  case KEY_H: return 35;
        case KEY_J: return 36;  case KEY_K: return 37;  case KEY_L: return 38;
        case KEY_SEMICOLON: return 39; case KEY_QUOTE: return 40;
        case KEY_GRAVE: return 41; case KEY_LSHIFT: return 42;
        case KEY_BACKSLASH: return 43;
        case KEY_Z: return 44;  case KEY_X: return 45;  case KEY_C: return 46;
        case KEY_V: return 47;  case KEY_B: return 48;  case KEY_N: return 49;
        case KEY_M: return 50;
        case KEY_COMMA: return 51; case KEY_PERIOD: return 52;
        case KEY_SLASH: return 53; case KEY_RSHIFT: return 54;
        case KEY_KP_MULTIPLY: return 55; case KEY_LALT: return 56;
        case KEY_SPACE: return 57; case KEY_CAPSLOCK: return 58;
        case KEY_F1: return 59;  case KEY_F2: return 60;  case KEY_F3: return 61;
        case KEY_F4: return 62;  case KEY_F5: return 63;  case KEY_F6: return 64;
        case KEY_F7: return 65;  case KEY_F8: return 66;  case KEY_F9: return 67;
        case KEY_F10: return 68; case KEY_NUMLOCK: return 69;
        case KEY_SCROLLLOCK: return 70;
        case KEY_KP_7: return 71; case KEY_KP_8: return 72; case KEY_KP_9: return 73;
        case KEY_KP_SUBTRACT: return 74;
        case KEY_KP_4: return 75; case KEY_KP_5: return 76; case KEY_KP_6: return 77;
        case KEY_KP_ADD: return 78;
        case KEY_KP_1: return 79; case KEY_KP_2: return 80; case KEY_KP_3: return 81;
        case KEY_KP_0: return 82; case KEY_KP_DECIMAL: return 83;
        case KEY_F11: return 87; case KEY_F12: return 88;
        case KEY_KP_ENTER: return 96; case KEY_RCTRL: return 97;
        case KEY_KP_DIVIDE: return 98; case KEY_RALT: return 100;
        case KEY_HOME: return 102; case KEY_UP: return 103;
        case KEY_PAGEUP: return 104; case KEY_LEFT: return 105;
        case KEY_RIGHT: return 106; case KEY_END: return 107;
        case KEY_DOWN: return 108; case KEY_PAGEDOWN: return 109;
        case KEY_INSERT: return 110; case KEY_DELETE: return 111;
        case KEY_LSUPER: return 125; case KEY_RSUPER: return 126;
        case KEY_MENU: return 127;
        default: return 0;
    }
}

// find the topmost mapped wl_surface (across all clients) whose on-screen
// content rect contains the global point, returning the owning client and
// surface. "topmost" follows wm z-order. used to route pointer events.
// returns nullptr if the point hits no wayland surface. (satoru)
Object* surface_at_global(Client** out_client, int gx, int gy) {
    Object* best = nullptr;
    Client* best_c = nullptr;
    int best_z = -0x7FFFFFFF;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use) continue;
        for (int j = 0; j < c->object_high; j++) {
            Object& o = c->objects[j];
            if (!o.in_use || o.iface != WL_SURFACE || !o.wm_window) continue;
            int wid = (int)(intptr_t)o.wm_window;
            Window* win = WindowManager::GetWindow(wid);
            if (!win || !win->visible || win->state == WIN_MINIMIZED) continue;
            if (gx <  win->content_x || gx >= win->content_x + win->content_w ||
                gy <  win->content_y || gy >= win->content_y + win->content_h)
                continue;
            if (win->z_order >= best_z) {
                best_z = win->z_order;
                best   = &o;
                best_c = c;
            }
        }
    }
    if (out_client) *out_client = best_c;
    return best;
}

// surface-local coordinates of a global point for a bridged surface. (satoru)
inline void global_to_surface(Object* surf, int gx, int gy, int& lx, int& ly) {
    lx = gx; ly = gy;
    if (!surf || !surf->wm_window) return;
    int wid = (int)(intptr_t)surf->wm_window;
    Window* win = WindowManager::GetWindow(wid);
    if (!win) return;
    lx = gx - win->content_x;
    ly = gy - win->content_y;
}

// wl_fixed_t is signed 24.8 fixed point: whole pixels << 8. (satoru)
inline int32_t to_fixed(int v) { return (int32_t)(v << 8); }

// ── wl_pointer event emitters (server->client opcodes) ───────────────
// enter=0, leave=1, motion=2, button=3, axis=4, frame=5. each takes the
// already-resolved pointer resource id. (satoru)
void pointer_enter(Client* c, uint32_t ptr, uint32_t surf, int lx, int ly) {
    uint8_t pl[16];
    put32(pl + 0, c->serial_next++);
    put32(pl + 4, surf);
    put32(pl + 8,  (uint32_t)to_fixed(lx));
    put32(pl + 12, (uint32_t)to_fixed(ly));
    send_event(c, ptr, 0, pl, 16);
}
void pointer_leave(Client* c, uint32_t ptr, uint32_t surf) {
    uint8_t pl[8];
    put32(pl + 0, c->serial_next++);
    put32(pl + 4, surf);
    send_event(c, ptr, 1, pl, 8);
}
void pointer_motion(Client* c, uint32_t ptr, uint32_t time, int lx, int ly) {
    uint8_t pl[12];
    put32(pl + 0, time);
    put32(pl + 4, (uint32_t)to_fixed(lx));
    put32(pl + 8, (uint32_t)to_fixed(ly));
    send_event(c, ptr, 2, pl, 12);
}
void pointer_button(Client* c, uint32_t ptr, uint32_t time,
                    uint32_t button, bool pressed) {
    uint8_t pl[16];
    put32(pl + 0, c->serial_next++);
    put32(pl + 4, time);
    put32(pl + 8, button);
    put32(pl + 12, pressed ? 1u : 0u);     // wl_pointer.button_state
    send_event(c, ptr, 3, pl, 16);
}
void pointer_axis(Client* c, uint32_t ptr, uint32_t time,
                  uint32_t axis, int value) {
    uint8_t pl[12];
    put32(pl + 0, time);
    put32(pl + 4, axis);                   // 0=vertical_scroll,1=horizontal
    put32(pl + 8, (uint32_t)to_fixed(value));
    send_event(c, ptr, 4, pl, 12);
}
// wl_pointer.frame (v5+) groups the preceding events into one logical
// event; harmless to send to older clients via the same opcode only if
// they negotiated v5, so gate on the resource version. (satoru)
void pointer_frame(Client* c, uint32_t ptr) {
    Object* po = find_obj(c, ptr);
    if (po && po->version >= 5) send_event(c, ptr, 5, nullptr, 0);
}

// ── wl_keyboard event emitters ───────────────────────────────────────
// enter=1, leave=2, key=3, modifiers=4. (satoru)
void keyboard_enter(Client* c, uint32_t kbd, uint32_t surf) {
    uint8_t pl[12];
    put32(pl + 0, c->serial_next++);
    put32(pl + 4, surf);
    put32(pl + 8, 0);                      // empty pressed-keys array
    send_event(c, kbd, 1, pl, 12);
}
void keyboard_leave(Client* c, uint32_t kbd, uint32_t surf) {
    uint8_t pl[8];
    put32(pl + 0, c->serial_next++);
    put32(pl + 4, surf);
    send_event(c, kbd, 2, pl, 8);
}
void keyboard_key(Client* c, uint32_t kbd, uint32_t time,
                  uint32_t key, bool pressed) {
    uint8_t pl[16];
    put32(pl + 0, c->serial_next++);
    put32(pl + 4, time);
    put32(pl + 8, key);                    // linux evdev keycode
    put32(pl + 12, pressed ? 1u : 0u);     // wl_keyboard.key_state
    send_event(c, kbd, 3, pl, 16);
}
void keyboard_modifiers(Client* c, uint32_t kbd, uint32_t mods) {
    // translate our compact bitmask into xkb mod masks. the standard
    // xkb base layout uses bit0=Shift, bit2=Control, bit3=Mod1(Alt),
    // bit6=Mod4(Logo/meta). (satoru)
    uint32_t depressed = 0;
    if (mods & WaylandServer::WL_MOD_SHIFT) depressed |= (1u << 0);
    if (mods & WaylandServer::WL_MOD_CTRL)  depressed |= (1u << 2);
    if (mods & WaylandServer::WL_MOD_ALT)   depressed |= (1u << 3);
    if (mods & WaylandServer::WL_MOD_META)  depressed |= (1u << 6);
    uint8_t pl[20];
    put32(pl + 0,  c->serial_next++);
    put32(pl + 4,  depressed);             // mods_depressed
    put32(pl + 8,  0);                     // mods_latched
    put32(pl + 12, 0);                     // mods_locked
    put32(pl + 16, 0);                     // group
    send_event(c, kbd, 4, pl, 20);
}

// Process one Wayland request.  All bounds checks must complete before
// any side effect.
void handle_request(Client* c, uint32_t object_id, uint16_t opcode,
                    const uint8_t* args, int args_len) {
    if (c->fatal) return;
    Object* obj = find_obj(c, object_id);
    if (!obj) {
        send_display_error(c, object_id, 0, "invalid object");
        return;
    }
    switch (obj->iface) {
        case WL_DISPLAY: {
            if (opcode == 0) {                    // sync(new_id callback)
                if (args_len < 4) { send_display_error(c, object_id, 1, "short sync"); return; }
                uint32_t cb = get32(args);
                if (!register_obj(c, cb, WL_CALLBACK, 1)) {
                    send_display_error(c, object_id, 2, "callback alloc");
                    return;
                }
                uint8_t pl[4]; put32(pl, c->serial_next++);
                send_event(c, cb, 0, pl, 4);      // wl_callback::done
                destroy_obj(c, cb);               // one-shot
            } else if (opcode == 1) {             // get_registry(new_id)
                if (args_len < 4) { send_display_error(c, object_id, 1, "short get_registry"); return; }
                uint32_t reg = get32(args);
                if (!register_obj(c, reg, WL_REGISTRY, 1)) {
                    send_display_error(c, object_id, 2, "registry alloc");
                    return;
                }
                for (int i = 0; g_globals[i].iface; i++) {
                    send_global(c, reg, g_globals[i]);
                }
            }
            break;
        }
        case WL_REGISTRY: {
            if (opcode == 0) {                    // bind(name, iface_str, version, new_id)
                if (args_len < 8) return;
                /* name = get32(args + 0); */
                uint32_t slen = get32(args + 4);
                if (slen == 0 || slen > 64) return;
                int padded = ((int)slen + 3) & ~3;
                if (8 + padded + 8 > args_len) return;
                char ifname[65];
                int copy_n = (int)slen - 1;
                if (copy_n < 0) copy_n = 0;
                if (copy_n > 64) copy_n = 64;
                for (int i = 0; i < copy_n; i++) ifname[i] = (char)args[8 + i];
                ifname[copy_n] = 0;
                int off = 8 + padded;
                uint32_t version = get32(args + off);
                uint32_t new_id  = get32(args + off + 4);
                uint16_t iface   = iface_id_of(ifname, copy_n);
                if (iface == 0) {
                    send_display_error(c, object_id, 0, "unknown global");
                    return;
                }
                if (version == 0 || version > 32) version = 1;
                register_obj(c, new_id, iface, (uint16_t)version);
            }
            break;
        }
        case WL_COMPOSITOR: {
            if (opcode == 0) {                    // create_surface(new_id)
                if (args_len < 4) return;
                uint32_t sid = get32(args);
                register_obj(c, sid, WL_SURFACE, obj->version);
            } else if (opcode == 1) {             // create_region(new_id)
                if (args_len < 4) return;
                uint32_t rid = get32(args);
                register_obj(c, rid, WL_COMPOSITOR, obj->version);
            }
            break;
        }
        case WL_SHM: {
            if (opcode == 0) {                    // create_pool(new_id, fd, size)
                // fd travels in scm_rights ancillary data (0 bytes on the
                // wire) so the on-wire args are: new_id(u32), size(u32). (satoru)
                if (args_len < 8) return;
                uint32_t pid  = get32(args);
                uint32_t size = get32(args + 4);
                if (!register_obj(c, pid, WL_SHM_POOL, 1)) return;
                Object* po = find_obj(c, pid);
                if (po) po->pool_size = size;     // hint until mapping lands
                // the pool fd arrived as SCM_RIGHTS ancillary; the kernel
                // resolved that memfd to its shm backing. bind it now so blits
                // read the client's real pixels instead of a null pool. (satoru)
                UnixSocket::ControlMsg cm;
                if (po && UnixSocket::TakePendingControl(c->sd, &cm)) {
                    for (int i = 0; i < cm.passed_fd_count; i++) {
                        if (!cm.passed_shm_base[i]) continue;
                        po->pool_base = (const uint8_t*)(uintptr_t)cm.passed_shm_base[i];
                        uint32_t msz = (uint32_t)cm.passed_shm_size[i];
                        po->pool_size = (size && size <= msz) ? size : msz;
                        break;
                    }
                }
            }
            break;
        }
        case WL_SHM_POOL: {
            if (opcode == 0) {                    // create_buffer
                // create_buffer(new_id, offset, width, height, stride,
                // format)  -  no fd here. capture the geometry so commit can
                // locate the pixels inside the parent pool. (satoru)
                if (args_len < 24) return;
                uint32_t bid    = get32(args);
                uint32_t offset = get32(args + 4);
                int32_t  bw     = (int32_t)get32(args + 8);
                int32_t  bh     = (int32_t)get32(args + 12);
                uint32_t stride = get32(args + 16);
                uint32_t format = get32(args + 20);
                if (bw < 0 || bh < 0) return;
                if (bw > 16384 || bh > 16384) return;   // sanity bound
                if (!register_obj(c, bid, WL_BUFFER, 1)) return;
                Object* bo = find_obj(c, bid);
                if (bo) {
                    bo->buf_pool_id = object_id;  // this pool owns the buffer
                    bo->buf_offset  = offset;
                    bo->width       = bw;
                    bo->height      = bh;
                    bo->buf_stride  = stride ? stride : (uint32_t)bw * 4u;
                    bo->buf_format  = format;
                }
            } else if (opcode == 1) {             // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 2) {             // resize
                // resize(size): grow the pool hint. (satoru)
                if (args_len >= 4) {
                    uint32_t nsize = get32(args);
                    if (nsize > obj->pool_size) obj->pool_size = nsize;
                }
            }
            break;
        }
        case WL_BUFFER: {
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            }
            break;
        }
        case XDG_WM_BASE: {
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 2) {             // get_xdg_surface(new_id, surface)
                if (args_len < 8) return;
                uint32_t xs   = get32(args);
                uint32_t surf = get32(args + 4);
                if (!find_obj(c, surf)) return;
                register_obj(c, xs, XDG_SURFACE, obj->version);
                Object* o = find_obj(c, xs);
                if (o) o->parent_surface_id = surf;
            } else if (opcode == 3) {             // pong
                // no-op
            }
            break;
        }
        case XDG_SURFACE: {
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 1) {             // get_toplevel(new_id)
                if (args_len < 4) return;
                uint32_t tid = get32(args);
                register_obj(c, tid, XDG_TOPLEVEL, obj->version);
                Object* tl = find_obj(c, tid);
                if (tl) tl->parent_surface_id = obj->parent_surface_id;
                // xdg_toplevel::configure (opcode 0): width, height, states.
                uint8_t cfg[16];
                put32(cfg + 0, 1024);
                put32(cfg + 4, 768);
                put32(cfg + 8, 0);                // empty states array length
                send_event(c, tid, 0, cfg, 12);
                // xdg_surface::configure (opcode 0): serial.
                uint8_t srl[4]; put32(srl, c->serial_next++);
                send_event(c, object_id, 0, srl, 4);
            } else if (opcode == 4) {             // ack_configure
                // no-op
            }
            break;
        }
        case XDG_TOPLEVEL: {
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            }
            break;
        }
        case WL_SURFACE: {
            switch (opcode) {
                case 0: {                          // destroy
                    destroy_obj(c, object_id);
                    break;
                }
                case 1: {                          // attach(buffer, x, y)
                    // record the buffer to apply on the next commit. buffer
                    // id 0 means "detach" (client wants no content). the
                    // x/y is the surface-relative attach offset. (satoru)
                    if (args_len < 12) return;
                    uint32_t bufid = get32(args);
                    obj->attach_x = (int32_t)get32(args + 4);
                    obj->attach_y = (int32_t)get32(args + 8);
                    obj->pending_buffer_id = bufid;
                    break;
                }
                case 2: {                          // damage(x, y, w, h)
                    if (args_len < 16) return;
                    int32_t x = (int32_t)get32(args);
                    int32_t y = (int32_t)get32(args + 4);
                    int32_t w = (int32_t)get32(args + 8);
                    int32_t h = (int32_t)get32(args + 12);
                    accum_damage(obj, x, y, w, h);
                    break;
                }
                case 3: {                          // frame(new_id callback)
                    if (args_len < 4) return;
                    uint32_t cb = get32(args);
                    if (!register_obj(c, cb, WL_CALLBACK, 1)) return;
                    obj->pending_frame_cb = cb;
                    push_frame_cb(c, cb);
                    break;
                }
                case 6: {                          // commit
                    // apply the attached buffer: blit damaged region into
                    // the bridged window's content rect (consumes damage).
                    // (satoru)
                    commit_surface(c, obj);
                    // deliver this surface's queued frame callback so the
                    // client is unblocked to render its next frame. uses the
                    // current pit tick as the millisecond timestamp. (satoru)
                    deliver_surface_frame(c, obj);
                    break;
                }
                case 9: {                          // damage_buffer
                    if (args_len < 16) return;
                    int32_t x = (int32_t)get32(args);
                    int32_t y = (int32_t)get32(args + 4);
                    int32_t w = (int32_t)get32(args + 8);
                    int32_t h = (int32_t)get32(args + 12);
                    accum_damage(obj, x, y, w, h);
                    break;
                }
                default: break;
            }
            break;
        }
        case WL_SEAT: {
            if (opcode == 0) {                    // get_pointer(new_id)
                if (args_len < 4) return;
                uint32_t pid = get32(args);
                if (!register_obj(c, pid, WL_POINTER, obj->version)) return;
                c->pointer_id = pid;
            } else if (opcode == 1) {             // get_keyboard(new_id)
                if (args_len < 4) return;
                uint32_t kid = get32(args);
                if (!register_obj(c, kid, WL_KEYBOARD, obj->version)) return;
                c->keyboard_id = kid;
                // wl_keyboard.repeat_info (opcode 4, v4+): rate, delay. a
                // sane default keeps clients that gate on it happy. no
                // keymap event is sent because that requires passing an fd
                // via ancillary data which the in-kernel tx path cannot do;
                // key events still carry raw linux evdev codes. (satoru)
                if (obj->version >= 4) {
                    uint8_t ri[8];
                    put32(ri + 0, 25);            // 25 keys/sec
                    put32(ri + 4, 400);           // 400 ms delay
                    send_event(c, kid, 5, ri, 8); // wl_keyboard.repeat_info=5
                }
            } else if (opcode == 2) {             // get_touch(new_id)
                if (args_len < 4) return;
                uint32_t tid = get32(args);
                register_obj(c, tid, WL_TOUCH, obj->version);
            } else if (opcode == 3) {             // release
                destroy_obj(c, object_id);
            }
            break;
        }
        case WL_POINTER: {
            if (opcode == 1) {                    // release
                destroy_obj(c, object_id);
            }
            // opcode 0 = set_cursor: ignored (compositor draws cursor). (satoru)
            break;
        }
        case WL_KEYBOARD: {
            if (opcode == 0) {                    // release
                destroy_obj(c, object_id);
            }
            break;
        }
        case WL_CALLBACK: {
            // Clients don't send requests on callbacks; ignore.
            break;
        }
        default:
            break;
    }
}

void on_data(int sd, const uint8_t* data, int len, void* user) {
    (void)user;
    if (len <= 0) return;
    g_now_ms = Time::GetTicks();
    Client* c = client_for_sd(sd);
    if (!c) return;

    int written = 0;
    while (written < len) {
        int space = (int)sizeof(c->rx_partial) - c->rx_partial_len;
        if (space <= 0) {
            // Buffer wedged with a partial message that can't fit  -  the
            // peer is sending garbage.  Disconnect.
            drop_client(c);
            return;
        }
        int n = len - written;
        if (n > space) n = space;
        for (int i = 0; i < n; i++) {
            c->rx_partial[c->rx_partial_len + i] = data[written + i];
        }
        c->rx_partial_len += n;
        written += n;

        int p = 0;
        while (c->rx_partial_len - p >= 8) {
            uint32_t obj_id = get32(c->rx_partial + p);
            uint16_t op     = get16(c->rx_partial + p + 4);
            uint16_t sz     = get16(c->rx_partial + p + 6);
            if (sz < 8 || (sz & 3) != 0 || sz > 4096) {
                drop_client(c);
                return;
            }
            if (c->rx_partial_len - p < sz) break;
            handle_request(c, obj_id, op,
                           c->rx_partial + p + 8, sz - 8);
            if (c->fatal) {
                tx_flush(c);
                drop_client(c);
                return;
            }
            p += sz;
        }
        if (p > 0) {
            int rem = c->rx_partial_len - p;
            for (int i = 0; i < rem; i++) {
                c->rx_partial[i] = c->rx_partial[p + i];
            }
            c->rx_partial_len = rem;
        }
    }

    tx_flush(c);
    if (c->fatal) drop_client(c);
}

void on_connect(int server_sd, int new_client_sd, void* user) {
    (void)server_sd; (void)user;
    Client* c = alloc_client(new_client_sd);
    if (!c) {
        UnixSocket::Close(new_client_sd);
        return;
    }
    SerialLogger::Log("Wayland: client connected, sd=");
    SerialLogger::LogDec(new_client_sd);
    SerialLogger::Log("\r\n");
}

}  // namespace

namespace WaylandServer {

void Init() {
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        g_clients[i].in_use     = false;
        g_clients[i].sd         = -1;
        g_clients[i].generation = 0;
        g_clients[i].tx_len     = 0;
        g_clients[i].rx_partial_len = 0;
    }

    g_listen_sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (g_listen_sd < 0) {
        SerialLogger::Log("Wayland: socket alloc failed\r\n");
        return;
    }
    if (UnixSocket::Bind(g_listen_sd,
                         "/system/run/user/1000/wayland-0") < 0) {
        SerialLogger::Log("Wayland: bind failed\r\n");
        return;
    }
    UnixSocket::Listen(g_listen_sd, 16);
    UnixSocket::RegisterServer(g_listen_sd, on_connect, on_data, nullptr);

    KVFS::WriteString("/system/run/user/1000/wayland-0.info",
        "kurono wayland compositor v1\n"
        "globals: wl_compositor v5, wl_shm v1, wl_seat v7, wl_output v3,\n"
        "         xdg_wm_base v3, zwp_linux_dmabuf_v1 v3\n");

    SerialLogger::Log("Wayland: listening on /system/run/user/1000/wayland-0\r\n");
}

int  ListenSd() { return g_listen_sd; }

// Called by the compositor render loop right after the framebuffer
// flip completes  -  this is the place where wl_callback::done deliveries
// land closest to vsync, which is what GTK/Qt clients use to throttle
// their next frame.
void DispatchPendingFrames() {
    g_now_ms = Time::GetTicks();
    uint8_t pl[4];
    put32(pl, g_now_ms);                         // monotonic ms timestamp
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use) continue;
        // Liveness probe  -  KernelInject returns -1 if the sd has been
        // closed beneath us (no on_disconnect callback exists).
        if (UnixSocket::KernelInject(c->sd, "", 0) < 0) {
            c->in_use = false; c->sd = -1; c->tx_len = 0;
            continue;
        }
        while (c->frame_cb_tail != c->frame_cb_head) {
            uint32_t cb = c->frame_cb[c->frame_cb_tail];
            c->frame_cb_tail = (c->frame_cb_tail + 1) % WL_MAX_FRAME_CB;
            if (cb == 0) continue;
            // The callback may have been destroyed via parent surface
            // destruction since it was queued.
            Object* o = find_obj(c, cb);
            if (!o) continue;
            send_event(c, cb, 0, pl, 4);
            destroy_obj(c, cb);
        }
        // Clear pending_frame_cb refs that pointed into the drained ring.
        for (int j = 0; j < c->object_high; j++) {
            if (c->objects[j].in_use &&
                c->objects[j].iface == WL_SURFACE &&
                c->objects[j].pending_frame_cb != 0 &&
                !find_obj(c, c->objects[j].pending_frame_cb)) {
                c->objects[j].pending_frame_cb = 0;
            }
        }
        tx_flush(c);
        if (c->fatal) drop_client(c);
    }
}

void DamageAll() {
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use) continue;
        for (int j = 0; j < c->object_high; j++) {
            Object& o = c->objects[j];
            if (!o.in_use || o.iface != WL_SURFACE) continue;
            o.damage.x = 0; o.damage.y = 0;
            o.damage.w = (o.width  > 0) ? o.width  : 1024;
            o.damage.h = (o.height > 0) ? o.height : 768;
            o.damage.valid = true;
        }
    }
}

int ClientCount() {
    int n = 0;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) if (g_clients[i].in_use) n++;
    return n;
}

int SurfaceCount() {
    int n = 0;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) continue;
        for (int j = 0; j < g_clients[i].object_high; j++) {
            if (g_clients[i].objects[j].in_use &&
                g_clients[i].objects[j].iface == WL_SURFACE) n++;
        }
    }
    return n;
}

// ── shm pool backing registration ────────────────────────────────────
void RegisterPoolMemory(int sd, uint32_t pool_id,
                        const uint8_t* base, uint32_t size) {
    Client* c = client_for_sd(sd);
    if (!c) return;
    Object* pool = find_obj(c, pool_id);
    if (!pool || pool->iface != WL_SHM_POOL) return;
    pool->pool_base = base;
    pool->pool_size = size;          // authoritative size from the mapping
}

// ── input forwarding entry points ─────────────────────────────────────
void ForwardPointerMotion(int gx, int gy) {
    Client* fc = nullptr;
    Object* surf = surface_at_global(&fc, gx, gy);
    uint32_t time = Timer::GetRealMs();
    g_now_ms = time;

    // first, send leave to any client that previously had pointer focus on
    // a different surface (focus crossed out). (satoru)
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use || c->pointer_id == 0) continue;
        uint32_t prev = c->pointer_focus_sid;
        bool still = (c == fc && surf && surf->id == prev);
        if (prev != 0 && !still) {
            if (find_obj(c, prev))
                pointer_leave(c, c->pointer_id, prev);
            c->pointer_focus_sid = 0;
            pointer_frame(c, c->pointer_id);
            tx_flush(c);
            if (c->fatal) drop_client(c);
        }
    }

    if (!surf || !fc || fc->pointer_id == 0) return;

    int lx, ly;
    global_to_surface(surf, gx, gy, lx, ly);

    // enter on first crossing into this surface. (satoru)
    if (fc->pointer_focus_sid != surf->id) {
        fc->pointer_focus_sid = surf->id;
        pointer_enter(fc, fc->pointer_id, surf->id, lx, ly);
    }
    pointer_motion(fc, fc->pointer_id, time, lx, ly);
    pointer_frame(fc, fc->pointer_id);
    tx_flush(fc);
    if (fc->fatal) drop_client(fc);
}

void ForwardPointerButton(int gx, int gy, int evdev_button, bool pressed) {
    Client* fc = nullptr;
    Object* surf = surface_at_global(&fc, gx, gy);
    if (!surf || !fc || fc->pointer_id == 0) return;
    uint32_t time = Timer::GetRealMs();
    g_now_ms = time;

    int lx, ly;
    global_to_surface(surf, gx, gy, lx, ly);
    // ensure the client has pointer focus before the button (a press can
    // arrive before any motion event). (satoru)
    if (fc->pointer_focus_sid != surf->id) {
        fc->pointer_focus_sid = surf->id;
        pointer_enter(fc, fc->pointer_id, surf->id, lx, ly);
    }
    pointer_button(fc, fc->pointer_id, time, (uint32_t)evdev_button, pressed);
    pointer_frame(fc, fc->pointer_id);

    // a press also moves keyboard focus to this surface's client, mirroring
    // click-to-focus. emit keyboard leave/enter as focus moves. (satoru)
    if (pressed) {
        for (int i = 0; i < WL_MAX_CLIENTS; i++) {
            Client* c = &g_clients[i];
            if (!c->in_use || c->keyboard_id == 0) continue;
            if (c == fc) continue;
            if (c->keyboard_focus_sid != 0) {
                if (find_obj(c, c->keyboard_focus_sid))
                    keyboard_leave(c, c->keyboard_id, c->keyboard_focus_sid);
                c->keyboard_focus_sid = 0;
                tx_flush(c);
                if (c->fatal) drop_client(c);
            }
        }
        if (fc->keyboard_id != 0 && fc->keyboard_focus_sid != surf->id) {
            fc->keyboard_focus_sid = surf->id;
            keyboard_enter(fc, fc->keyboard_id, surf->id);
        }
    }
    tx_flush(fc);
    if (fc->fatal) drop_client(fc);
}

void ForwardPointerAxis(int gx, int gy, int axis, int value) {
    Client* fc = nullptr;
    Object* surf = surface_at_global(&fc, gx, gy);
    if (!surf || !fc || fc->pointer_id == 0) return;
    uint32_t time = Timer::GetRealMs();
    g_now_ms = time;
    pointer_axis(fc, fc->pointer_id, time, (uint32_t)axis, value);
    pointer_frame(fc, fc->pointer_id);
    tx_flush(fc);
    if (fc->fatal) drop_client(fc);
}

void ForwardKey(int key, bool pressed, uint32_t mods) {
    uint32_t evdev = key_to_evdev(key);
    uint32_t time  = Timer::GetRealMs();
    g_now_ms = time;
    // route to whichever client currently holds keyboard focus. (satoru)
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use || c->keyboard_id == 0) continue;
        if (c->keyboard_focus_sid == 0) continue;
        if (!find_obj(c, c->keyboard_focus_sid)) { c->keyboard_focus_sid = 0; continue; }
        keyboard_modifiers(c, c->keyboard_id, mods);
        if (evdev != 0)
            keyboard_key(c, c->keyboard_id, time, evdev, pressed);
        tx_flush(c);
        if (c->fatal) drop_client(c);
    }
}

}  // namespace WaylandServer
