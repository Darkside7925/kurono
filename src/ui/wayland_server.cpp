#include "wayland_server.h"
#include "../net/unix_socket.h"
#include "xkb_keymap_us.h"        // compiled us keymap for wl_keyboard.keymap (satoru)
#include "../drivers/serial.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "../drivers/graphics.h"     // framebuffer blit target (satoru)
#include "../kernel/heap.h"          // per-surface shadow buffers (satoru)
#include "../drivers/timer.h"        // pit-tick ms timestamps for frame cbs (satoru)
#include "window_manager.h"          // bridge surfaces to wm windows (satoru)
#include "app_icons.h"               // per-app window logo ids (satoru)
#include "../drivers/keyboard.h"     // Key enum -> linux evdev keycode map (satoru)
#include "../drivers/mouse.h"        // live pointer pos for the scroll thunk (satoru)

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
    o.subsurface_parent = 0;
    o.subsurface_x      = 0;
    o.subsurface_y      = 0;
    // free a stale shadow from a recycled slot (fresh g_clients are bss-zero,
    // so a garbage pointer is impossible on first use). (satoru)
    if (o.shadow) KernelHeap::Free(o.shadow);
    o.shadow        = nullptr;
    o.shadow_w      = 0;
    o.shadow_h      = 0;
    o.shadow_fmt    = 0;
    o.shadow_filled = false;
    o.title[0]      = 0;
    o.app_icon_hint = 0xFF;
    o.geo_x = 0; o.geo_y = 0;
    o.geo_w = 0; o.geo_h = 0;
    o.input_none      = false;
    o.region_has_rect = false;
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
    c->pointer_id_count    = 0;
    c->keyboard_id_count   = 0;
    for (int i = 0; i < Client::WL_MAX_INPUT_RES; i++) {
        c->pointer_ids[i]  = 0;
        c->keyboard_ids[i] = 0;
    }
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
    // a destroyed wl_pointer/wl_keyboard is removed from the broadcast list
    // (and the legacy first-slot alias refreshed). (satoru)
    if (o.iface == WL_POINTER) {
        int w = 0;
        for (int i = 0; i < c->pointer_id_count; i++)
            if (c->pointer_ids[i] != id) c->pointer_ids[w++] = c->pointer_ids[i];
        c->pointer_id_count = w;
        c->pointer_id = (w > 0) ? c->pointer_ids[0] : 0;
    }
    if (o.iface == WL_KEYBOARD) {
        int w = 0;
        for (int i = 0; i < c->keyboard_id_count; i++)
            if (c->keyboard_ids[i] != id) c->keyboard_ids[w++] = c->keyboard_ids[i];
        c->keyboard_id_count = w;
        c->keyboard_id = (w > 0) ? c->keyboard_ids[0] : 0;
    }
    o.in_use = false;
    o.wm_window = nullptr;
    o.pending_frame_cb = 0;
    o.damage.valid = false;
    // release the shadow with the surface (a dead slot may never be reused,
    // so waiting for clear_obj_ext would leak). (satoru)
    if (o.shadow) { KernelHeap::Free(o.shadow); o.shadow = nullptr; }
    o.shadow_w = 0; o.shadow_h = 0; o.shadow_filled = false;
    // Tighten object_high if we just freed the topmost slot.
    while (c->object_high > 1 && !c->objects[c->object_high - 1].in_use) {
        c->object_high--;
    }
}

// Append raw bytes to per-client TX scratch.  Returns false if the buffer
// would overflow - caller flushes and retries.  Header + payload are
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
// oldest if full - better than allocating.
void push_frame_cb(Client* c, uint32_t cb_id) {
    int next = (c->frame_cb_head + 1) % WL_MAX_FRAME_CB;
    if (next == c->frame_cb_tail) {
        // Ring full - drop the oldest by advancing tail.  The callback
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
        // a full match consumes the whole name: seen can reach max_len exactly
        // (an interface name passed without trailing slack), so accept seen ==
        // max_len too -- the old `< max_len` rejected every exact-length name,
        // which made firefox's wl_compositor bind look like an unknown global. (satoru)
        if ((seen <= max_len) && *a == 0 && *b == 0) return table[i].v;
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
void blit_surface_region(Client* c, Object* surf, Window* win, bool use_damage,
                         int ox = 0, int oy = 0);
void blit_child_subsurfaces(Client* c, Object* parent_surf, Window* pwin);
// forward decls: keyboard focus emitters (defined below) so a toplevel can be
// auto-focused on map - see the note in commit_surface. (satoru)
void keyboard_enter(Client* c, uint32_t kbd, uint32_t surf);
void keyboard_leave(Client* c, uint32_t kbd, uint32_t surf);
Object* find_subsurface_role(Client* c, uint32_t surf_id);   // (satoru)

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
    blit_surface_region(c, surf, win, /*use_damage=*/false,
                        -surf->geo_x, -surf->geo_y);
    // a wm repaint refills the whole content rect from the parent buffer, which
    // would re-cover any subsurface content - re-composite children on top.
    // forward-declared below; defined with commit_surface. (satoru)
    blit_child_subsurfaces(c, surf, win);
}

// input callback for bridged wayland windows: the wm delivers scroll (event 3)
// here; wheel notches become wl_pointer.axis at the live pointer position.
// char keypresses (event 2) are ignored - raw key edges go through ForwardKey
// from the desktop input path, which is what gtk/firefox actually consume. (satoru)
void wl_input_thunk(Window* win, int event, int p1, int p2) {
    (void)win;
    if (event == 3) {
        // kurono wheel delta: wheel-DOWN is positive (verified empirically via
        // qmp injection: 6x wheel-down -> [scrl] d=+2). wayland positive axis
        // also scrolls the content down, so the sign passes straight through;
        // ~32 px per notch reads like a native browser step. (satoru)
        ForwardPointerAxis(Mouse::mx, Mouse::my, 0, p1 * 32);
    } else if (event == 4) {
        // event 4 = right-click at (p1,p2). the wm only reports the click
        // edge, so synthesize the press+release pair gtk expects. (satoru)
        ForwardPointerButton(p1, p2, WL_BTN_RIGHT, true);
        ForwardPointerButton(p1, p2, WL_BTN_RIGHT, false);
    }
}

// case-insensitive substring test for toplevel meta matching (titles and
// app ids arrive in whatever case the client uses). (satoru)
bool str_has_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return true;
        if (!hay[i + 1]) break;
    }
    return false;
}

// apply xdg_toplevel.set_title / set_app_id to the toplevel's parent surface:
// store the title (used at bridge time), resolve an app-icon hint, and if the
// surface is already bridged push both onto the live wm window. is_app_id
// distinguishes the two requests - an app id ("org.mozilla.firefox") is used
// for icon resolution and as a display-name fallback, never overwriting a
// real title. (satoru)
void apply_toplevel_meta(Object* surf, const char* str, int len, bool is_app_id) {
    if (!surf || !str || len <= 0) return;
    // titles arrive as utf-8 but the wm titlebar font is ascii-only, so decode
    // and transliterate: dashes -> '-', curly quotes -> straight, ellipsis ->
    // '.', anything else multi-byte dropped. without this a page title like
    // "kurono (U+2014) Nightly" renders its raw bytes as mojibake. (satoru)
    char buf[48];
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof(buf) - 1; ) {
        unsigned char b = (unsigned char)str[i];
        if (b < 0x80) { buf[n++] = (char)b; i += 1; continue; }
        int seq = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : 2;
        if (i + seq > len) break;
        // decode the code point (enough of it to classify punctuation). (satoru)
        uint32_t cp = 0;
        if (seq == 2)      cp = ((uint32_t)(b & 0x1F) << 6)  | (str[i+1] & 0x3F);
        else if (seq == 3) cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(str[i+1] & 0x3F) << 6) | (str[i+2] & 0x3F);
        else               cp = 0;   // beyond the bmp: drop (satoru)
        char out = 0;
        if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) out = '-';
        else if (cp == 0x2018 || cp == 0x2019) out = '\'';
        else if (cp == 0x201C || cp == 0x201D) out = '"';
        else if (cp == 0x2026) out = '.';
        else if (cp == 0x00A0) out = ' ';
        if (out) buf[n++] = out;
        i += seq;
    }
    buf[n] = 0;
    if (n <= 0) return;
    // icon hint from either request: any mention of firefox pins the logo. (satoru)
    if (str_has_ci(buf, "firefox")) surf->app_icon_hint = (uint8_t)AppIcons::FIREFOX;
    if (is_app_id) {
        // display-name fallback only; a set_title wins. (satoru)
        if (!surf->title[0] && surf->app_icon_hint == (uint8_t)AppIcons::FIREFOX) {
            surf->title[0]='F'; surf->title[1]='i'; surf->title[2]='r';
            surf->title[3]='e'; surf->title[4]='f'; surf->title[5]='o';
            surf->title[6]='x'; surf->title[7]=0;
        }
    } else {
        for (int i = 0; i <= n; i++) surf->title[i] = buf[i];
    }
    // live-update an already-bridged window so later title changes (e.g. the
    // loaded page's title) show immediately. (satoru)
    if (surf->wm_window) {
        int wid = (int)(intptr_t)surf->wm_window;
        Window* win = WindowManager::GetWindow(wid);
        if (win) {
            if (surf->title[0]) WindowManager::SetTitle(wid, surf->title);
            if (surf->app_icon_hint != 0xFF) win->app_icon = (int)surf->app_icon_hint;
            WindowManager::MarkDirty(wid);
        }
    }
}

// bridge a toplevel wl_surface to a wm window the first time it is
// committed with content, so it owns an on-screen rect. returns the
// window id, or -1 if no window could be created. (satoru)
int bridge_surface_window(Client* c, Object* surf, int w, int h) {
    if (surf->wm_window) return (int)(intptr_t)surf->wm_window;
    // size the window to the VISIBLE geometry (set_window_geometry), not the
    // raw buffer - csd margins are cropped out of the blits. (satoru)
    if (surf->geo_w > 0 && surf->geo_h > 0) { w = surf->geo_w; h = surf->geo_h; }
    if (w <= 0) w = 320;
    if (h <= 0) h = 240;
    // title: prefer the client's own xdg_toplevel.set_title (stored on the
    // surface by apply_toplevel_meta); fall back to the client sd so distinct
    // untitled apps stay distinguishable in the wm. (satoru)
    char title[48];
    int t = 0;
    if (surf->title[0]) {
        while (surf->title[t] && t < 47) { title[t] = surf->title[t]; t++; }
    } else {
        const char* base = "wayland sd ";
        while (base[t] && t < 24) { title[t] = base[t]; t++; }
        int sd = c->sd; if (sd < 0) sd = 0;
        // tiny itoa (satoru)
        char num[8]; int n = 0;
        if (sd == 0) num[n++] = '0';
        else { int v = sd; char tmp[8]; int k = 0;
               while (v > 0 && k < 7) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
               while (k > 0) num[n++] = tmp[--k]; }
        for (int i = 0; i < n && t < 47; i++) title[t++] = num[i];
    }
    title[t] = 0;
    // -1,-1 lets the wm centre the window in the desktop area. (satoru)
    Window* win = WindowManager::CreateWindow(title, -1, -1, w, h);
    if (!win) return -1;
    // pin the app logo resolved from set_app_id/set_title so later title
    // changes keep it. (satoru)
    if (surf->app_icon_hint != 0xFF) win->app_icon = (int)surf->app_icon_hint;
    // install the content render callback so wm repaints keep the client
    // pixels, and remember which surface owns this window. (satoru)
    win->render = wl_render_thunk;
    win->input  = wl_input_thunk;      // scroll wheel -> wl_pointer.axis (satoru)
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
void blit_surface_region(Client* c, Object* surf, Window* win, bool use_damage,
                         int ox, int oy) {
    if (!surf || !win) return;
    // source = the surface's SHADOW when it has one (the accumulated committed
    // pixels - see update_surface_shadow); the raw pool only as a fallback for
    // surfaces that predate their first shadow fill. the shadow path deliberately
    // needs NO live wl_buffer object: once the client has release-events flowing
    // (gtk) it DESTROYS released buffers, so at wm-repaint time pending_buffer_id
    // often names a dead object - requiring it made every repaint skip the child
    // and show the parent's black (the page rendered but never displayed). (satoru)
    const uint8_t* base; uint32_t psize, sstride, soff, sfmt;
    int bw, bh;
    if (surf->shadow && surf->shadow_filled) {
        bw = surf->shadow_w; bh = surf->shadow_h;
        base    = surf->shadow;
        psize   = (uint32_t)bw * (uint32_t)bh * 4u;
        sstride = (uint32_t)bw * 4u;
        soff    = 0;
        sfmt    = surf->shadow_fmt;
    } else {
        uint32_t bid = surf->pending_buffer_id;
        if (bid == 0) return;                   // nothing attached
        Object* buf = find_obj(c, bid);
        if (!buf || buf->iface != WL_BUFFER) return;
        bw = buf->width; bh = buf->height;
        Object* pool = find_obj(c, buf->buf_pool_id);
        base  = (pool && pool->iface == WL_SHM_POOL) ? pool->pool_base : nullptr;
        psize = (pool) ? pool->pool_size : 0;
        sstride = buf->buf_stride;
        soff    = buf->buf_offset;
        sfmt    = buf->buf_format;
        if (!base) return;                      // no mapping yet - nothing to draw
    }
    if (bw <= 0 || bh <= 0) return;

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

    // bound the last byte we would touch against the source size so a short /
    // malformed pool (or a stale shadow) can never read out of bounds. (satoru)
    const uint8_t* src = base + soff
                       + (size_t)dy * sstride
                       + (size_t)dx * 4u;
    size_t last = soff
                + (size_t)(dy + dh - 1) * sstride
                + (size_t)(dx + dw) * 4u;
    if (sstride < (uint32_t)(dx + dw) * 4u) return;           // malformed stride (satoru)
    if (last > psize) return;                                 // out-of-bounds source read (satoru)

    // effective alpha = surface alpha combined with the bridged window's
    // wm opacity, so WindowManager::SetAlpha modulates client content too.
    // (satoru)
    uint32_t wm_a = WindowManager::GetAlpha(win->id);
    uint32_t eff  = ((uint32_t)surf->surf_alpha * wm_a) / 255u;
    if (eff > 255) eff = 255;
    // ox/oy shift the destination for a subsurface composited onto its parent
    // window (0,0 for a normal toplevel). the clip stays the window content rect
    // so a subsurface can't paint outside its parent. (satoru)
    blit_argb(src, sstride, dw, dh, sfmt,
              cx + ox + dx, cy + oy + dy,
              cx, cy, cw, ch, (uint8_t)eff);
    if (use_damage) Graphics::MarkDirty(cx + ox + dx, cy + oy + dy, dw, dh);
}

// find the wl_subsurface role object that wraps `surf_id` (i.e. gives that
// wl_surface the subsurface role). returns null if surf is a plain surface.
// (satoru)
Object* find_subsurface_role(Client* c, uint32_t surf_id) {
    if (surf_id == 0) return nullptr;
    for (int i = 0; i < WL_MAX_OBJECTS; i++) {
        Object& o = c->objects[i];
        if (o.in_use && o.iface == WL_SUBSURFACE && o.parent_surface_id == surf_id)
            return &o;
    }
    return nullptr;
}

// composite every subsurface parented to `parent_surf` onto `pwin` (which the
// parent already painted), each at its set_position offset, on top. called
// after a parent's own blit so the parent's (often black csd) buffer never
// covers the real content the client drew into its child surface. (satoru)
void blit_child_subsurfaces(Client* c, Object* parent_surf, Window* pwin) {
    if (!parent_surf || !pwin) return;
    for (int i = 0; i < WL_MAX_OBJECTS; i++) {
        Object& ss = c->objects[i];
        if (!ss.in_use || ss.iface != WL_SUBSURFACE) continue;
        if (ss.subsurface_parent != parent_surf->id) continue;
        Object* child = find_obj(c, ss.parent_surface_id);
        // a filled shadow is displayable even when the client has since detached
        // or DESTROYED the committed wl_buffer (gtk destroys released buffers). (satoru)
        if (!child) continue;
        if (child->pending_buffer_id == 0 && !(child->shadow && child->shadow_filled)) continue;
        blit_surface_region(c, child, pwin, /*use_damage=*/false,
                            ss.subsurface_x - parent_surf->geo_x,
                            ss.subsurface_y - parent_surf->geo_y);
    }
}

// copy the freshly-committed pixels (damage region, or the whole buffer on the
// first fill / a resize) from the client buffer into the surface's SHADOW. the
// shadow is what every blit reads, so a damage-incremental client - firefox
// SW-WR attaches a fresh mostly-ZERO buffer each frame with only the damaged
// strip painted - can never wipe the accumulated picture (the black-firefox
// bug: full blits of those zero buffers blacked out the window). (satoru)
void update_surface_shadow(Client* c, Object* surf, Object* buf) {
    int bw = buf->width, bh = buf->height;
    if (bw <= 0 || bh <= 0) return;
    Object* pool = find_obj(c, buf->buf_pool_id);
    const uint8_t* base = (pool && pool->iface == WL_SHM_POOL) ? pool->pool_base : nullptr;
    uint32_t psize = pool ? pool->pool_size : 0;
    if (!base) return;                          // no mapping yet - nothing to copy (satoru)

    // (re)allocate on first use or resize; a resize invalidates old content. (satoru)
    if (!surf->shadow || surf->shadow_w != bw || surf->shadow_h != bh) {
        if (surf->shadow) KernelHeap::Free(surf->shadow);
        surf->shadow = (uint8_t*)KernelHeap::Alloc((size_t)bw * bh * 4u);
        surf->shadow_w = bw; surf->shadow_h = bh;
        surf->shadow_filled = false;
        if (!surf->shadow) { surf->shadow_w = surf->shadow_h = 0; return; }
    }

    // region to copy: full buffer until the shadow is filled once; after that
    // honour the damage rect (a damage-less commit copies nothing - per the
    // protocol it changed nothing). (satoru)
    int dx = 0, dy = 0, dw = bw, dh = bh;
    if (surf->shadow_filled) {
        if (!surf->damage.valid) return;
        dx = surf->damage.x; dy = surf->damage.y;
        dw = surf->damage.w; dh = surf->damage.h;
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > bw) dw = bw - dx;
        if (dy + dh > bh) dh = bh - dy;
        if (dw <= 0 || dh <= 0) return;
    }
    // bound the source against the pool like the blit does. (satoru)
    if (buf->buf_stride < (uint32_t)(dx + dw) * 4u) return;
    size_t last = buf->buf_offset + (size_t)(dy + dh - 1) * buf->buf_stride
                + (size_t)(dx + dw) * 4u;
    if (last > psize) return;
    for (int row = 0; row < dh; row++) {
        const uint8_t* s = base + buf->buf_offset
                         + (size_t)(dy + row) * buf->buf_stride + (size_t)dx * 4u;
        uint8_t* d = surf->shadow + ((size_t)(dy + row) * bw + dx) * 4u;
        memcpy(d, s, (size_t)dw * 4u);
    }
    surf->shadow_fmt    = buf->buf_format;
    surf->shadow_filled = true;
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
    // accumulate this commit's pixels into the shadow BEFORE any blit - all
    // blits below (and later wm repaints) read the shadow. (satoru)
    update_surface_shadow(c, surf, buf);
    // wl_buffer::release (event opcode 0): the shadow now owns the pixels, so the
    // client may reuse this buffer immediately. WITHOUT this firefox exhausts its
    // small buffer pool after the FIRST frame and blocks forever waiting for a
    // free buffer - the frozen-first-frame browser (empty url bar, white content,
    // stale chrome) while paintCount kept climbing. only safe once the shadow is
    // filled (blits no longer read the client pool). (satoru)
    if (surf->shadow_filled)
        send_event(c, buf->id, 0, nullptr, 0);

    // if this surface is a subsurface (firefox's gtk content surface, inset
    // inside the black csd toplevel), composite it ONTO the parent window at
    // its set_position offset instead of giving it its own window behind the
    // parent - that separate-window path is exactly why the content drew but
    // the screen stayed black. (satoru)
    Object* role = find_subsurface_role(c, surf->id);
    if (role) {
        Object* psurf = find_obj(c, role->subsurface_parent);
        if (psurf && psurf->wm_window) {
            int pwid = (int)(intptr_t)psurf->wm_window;
            Window* pwin = WindowManager::GetWindow(pwid);
            if (pwin) {
                // subsurface offsets are parent-SURFACE coords; the parent's
                // geometry crop shifts them into content coords. (satoru)
                blit_surface_region(c, surf, pwin, /*use_damage=*/true,
                                    role->subsurface_x - psurf->geo_x,
                                    role->subsurface_y - psurf->geo_y);
                WindowManager::MarkDirty(pwid);
                surf->damage.valid = false;
                return;
            }
        }
        // parent not mapped yet - fall through to a temporary own-window so the
        // content isn't lost; it re-homes onto the parent on the next commit.
        // (satoru)
    }

    // ensure the surface owns an on-screen rect. (satoru)
    int wid = bridge_surface_window(c, surf, bw, bh);
    if (wid < 0) return;
    Window* win = WindowManager::GetWindow(wid);
    if (!win) { surf->wm_window = nullptr; return; }

    // AUTO-FOCUS this toplevel on map. firefox's CONTENT browsing context only
    // becomes active - and thus PAINTS (PresShell::ComputeActiveness gates on
    // bc->IsActive()) - when the window is FOCUSED. a headless boot has no
    // pointer click to focus it, so grant keyboard focus here (paired with the
    // xdg_toplevel ACTIVATED state). without it the chrome paints but the web
    // content viewport stays firefox's blank canvas. gated to real toplevels
    // (skip the tiny transient own-windows) and fired once per surface. (satoru)
    if (c->keyboard_id != 0 && c->keyboard_focus_sid != surf->id &&
        bw > 200 && bh > 200) {
        if (c->keyboard_focus_sid != 0 && find_obj(c, c->keyboard_focus_sid))
            keyboard_leave(c, c->keyboard_id, c->keyboard_focus_sid);
        c->keyboard_focus_sid = surf->id;
        keyboard_enter(c, c->keyboard_id, surf->id);
    }

    // incremental blit of the damaged region. if no pool mapping exists yet
    // (fd transport not wired) blit_surface_region is a no-op; we still mark
    // the window dirty so the wm paints its chrome and, once a mapping
    // arrives via RegisterPoolMemory(), the render thunk fills content. the
    // -geo shift crops the csd margins: buffer pixel (geo_x,geo_y) lands at
    // content (0,0). (satoru)
    blit_surface_region(c, surf, win, /*use_damage=*/true,
                        -surf->geo_x, -surf->geo_y);
    // re-composite any child subsurfaces on top so the parent's fresh (black)
    // buffer doesn't cover content the client committed into a child. (satoru)
    blit_child_subsurfaces(c, surf, win);
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

// surface-local coordinates of a global point for a bridged surface. content
// (0,0) shows buffer pixel (geo_x,geo_y) - the csd-margin crop - so the
// geometry offset is added back for the client's event coords. (satoru)
inline void global_to_surface(Object* surf, int gx, int gy, int& lx, int& ly) {
    lx = gx; ly = gy;
    if (!surf || !surf->wm_window) return;
    int wid = (int)(intptr_t)surf->wm_window;
    Window* win = WindowManager::GetWindow(wid);
    if (!win) return;
    lx = gx - win->content_x + surf->geo_x;
    ly = gy - win->content_y + surf->geo_y;
}

// resolve the INPUT TARGET under a global point: the deepest surface, not just
// the bridged toplevel. in gdk-wayland every wl_surface is its own GdkWindow
// and input is routed per-surface by the compositor - gtk does NOT synthesize
// crossings from toplevel coordinates into a subsurface. firefox draws its
// whole ui (and the scrollable page) in a gtk SUBSURFACE inset inside the csd
// toplevel, so events delivered to the toplevel never reached the widget that
// scrolls - wheel events arrived and did nothing. descend: if the point falls
// inside a child subsurface's rect (set_position offset relative to the
// parent buffer, sized by its committed shadow/buffer), target the child with
// child-local coords. (satoru)
Object* input_target_at(Client** out_client, int gx, int gy, int& lx, int& ly) {
    Client* fc = nullptr;
    Object* top = surface_at_global(&fc, gx, gy);
    if (out_client) *out_client = fc;
    if (!top || !fc) {
        lx = gx; ly = gy;
        return top;
    }
    // toplevel-buffer coords of the point. (satoru)
    int tx, ty;
    global_to_surface(top, gx, gy, tx, ty);
    lx = tx; ly = ty;
    for (int i = 0; i < WL_MAX_OBJECTS; i++) {
        Object& ss = fc->objects[i];
        if (!ss.in_use || ss.iface != WL_SUBSURFACE) continue;
        if (ss.subsurface_parent != top->id) continue;
        Object* child = find_obj(fc, ss.parent_surface_id);
        if (!child) continue;
        // honor wl_surface.set_input_region: a child with an EMPTY input
        // region is a pure output layer (firefox webrender) - input falls
        // through it to the toplevel, which is the surface gtk actually
        // listens on. (satoru)
        if (child->input_none) continue;
        int cw = 0, ch = 0;
        if (child->shadow && child->shadow_filled) { cw = child->shadow_w; ch = child->shadow_h; }
        else if (child->width > 0)                 { cw = child->width;    ch = child->height;   }
        if (cw <= 0 || ch <= 0) continue;
        int rx = tx - ss.subsurface_x;
        int ry = ty - ss.subsurface_y;
        if (rx < 0 || ry < 0 || rx >= cw || ry >= ch) continue;
        lx = rx; ly = ry;
        return child;
    }
    return top;
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
// wl_pointer.axis_source (opcode 6, v5+): 0 = wheel. gtk classifies scroll
// deltas by source; a wheel notch should arrive as source+discrete+axis in
// one frame. (satoru)
void pointer_axis_source(Client* c, uint32_t ptr) {
    Object* po = find_obj(c, ptr);
    if (!po || po->version < 5) return;
    uint8_t pl[4];
    put32(pl + 0, 0);                      // axis_source = wheel
    send_event(c, ptr, 6, pl, 4);
}
// wl_pointer.axis_discrete (opcode 8, v5+): whole wheel notches. (satoru)
void pointer_axis_discrete(Client* c, uint32_t ptr, uint32_t axis, int clicks) {
    Object* po = find_obj(c, ptr);
    if (!po || po->version < 5) return;
    uint8_t pl[8];
    put32(pl + 0, axis);
    put32(pl + 4, (uint32_t)clicks);
    send_event(c, ptr, 8, pl, 8);
}
// wl_pointer.frame (v5+) groups the preceding events into one logical
// event; harmless to send to older clients via the same opcode only if
// they negotiated v5, so gate on the resource version. (satoru)
void pointer_frame(Client* c, uint32_t ptr) {
    Object* po = find_obj(c, ptr);
    if (po && po->version >= 5) send_event(c, ptr, 5, nullptr, 0);
}

// ── seat-input wrappers ───────────────────────────────────────────────
// firefox binds the seat TWICE - gdk/gtk first, then gecko's native-layer
// compositor - so it owns two wl_pointer / wl_keyboard resources. the OLD
// single-slot tracking kept only the LAST bind (gecko's listener-less proxy)
// and starved gdk = all input dead. tracking BOTH then BROADCASTING every
// event to both looked right, but interleaving two resources' event streams
// (p0.axis, p1.source, p0.frame, ...) without a frame between them corrupts
// the wl_pointer v5 wheel + wl_keyboard sequences (gtk dropped keys + wheel;
// motion + button survived by luck). so target resource [0] ONLY: the FIRST
// seat bind is gdk (gtk initializes before gecko's native layer), and gdk is
// the proxy that actually routes input into gecko's dom - empirically the
// [0]-only motion reached the dom while broadcast axis/key did not. (satoru)
inline uint32_t live_ptr(Client* c)  { return c->pointer_id_count  > 0 ? c->pointer_ids[0]  : 0; }
inline uint32_t live_kbd(Client* c)  { return c->keyboard_id_count > 0 ? c->keyboard_ids[0] : 0; }

void ptrs_enter(Client* c, uint32_t surf, int lx, int ly) {
    uint32_t p = live_ptr(c); if (p) pointer_enter(c, p, surf, lx, ly);
}
void ptrs_leave(Client* c, uint32_t surf) {
    uint32_t p = live_ptr(c); if (p) pointer_leave(c, p, surf);
}
void ptrs_motion(Client* c, uint32_t time, int lx, int ly) {
    uint32_t p = live_ptr(c); if (p) pointer_motion(c, p, time, lx, ly);
}
void ptrs_button(Client* c, uint32_t time, uint32_t button, bool pressed) {
    uint32_t p = live_ptr(c); if (p) pointer_button(c, p, time, button, pressed);
}
void ptrs_axis(Client* c, uint32_t time, uint32_t axis, int value) {
    uint32_t p = live_ptr(c);
    if (!p) return;
    pointer_axis_source(c, p);
    pointer_axis_discrete(c, p, axis, value / 32);
    pointer_axis(c, p, time, axis, value);
}
void ptrs_frame(Client* c) {
    uint32_t p = live_ptr(c); if (p) pointer_frame(c, p);
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

// keyboard wrappers - BROADCAST to every keyboard resource. measured: the
// live gdk keyboard is NOT resource [0] (gecko's native layer binds a
// keyboard before gdk does, unlike pointers where gdk is first), so [0]-only
// sent keys to gecko's deaf proxy (kd=0). broadcasting reaches gdk whatever
// its index, and unlike the wl_pointer v5 wheel triple the simple key +
// modifiers events survive the cross-resource interleave (kd=4 confirmed).
// each key is enter/key/modifiers only - no frame to desync. (satoru)
void kbds_enter(Client* c, uint32_t surf) {
    for (int i = 0; i < c->keyboard_id_count; i++)
        keyboard_enter(c, c->keyboard_ids[i], surf);
}
void kbds_leave(Client* c, uint32_t surf) {
    for (int i = 0; i < c->keyboard_id_count; i++)
        keyboard_leave(c, c->keyboard_ids[i], surf);
}
void kbds_key(Client* c, uint32_t time, uint32_t key, bool pressed) {
    for (int i = 0; i < c->keyboard_id_count; i++)
        keyboard_key(c, c->keyboard_ids[i], time, key, pressed);
}
void kbds_modifiers(Client* c, uint32_t mods) {
    for (int i = 0; i < c->keyboard_id_count; i++)
        keyboard_modifiers(c, c->keyboard_ids[i], mods);
}

// Process one Wayland request.  All bounds checks must complete before
// any side effect.
// firefox's bundled libwayland/proxy omits the trailing object id of
// wl_registry::bind's interface-less new_id on the wire. when it's absent we
// assign the lowest free client id, which mirrors libwayland's own sequential
// id allocation so the assigned id matches what firefox subsequently uses
// (e.g. its first bind, wl_compositor, becomes obj 3 just as firefox expects). (satoru)
static uint32_t lowest_free_client_id(Client* c) {
    for (uint32_t id = 2; id < 0x01000000u; id++) {
        if (!find_obj(c, id)) return id;
    }
    return 0;
}

// append a wayland string (u32 length-incl-nul, bytes, nul, pad to 4) at *pp. (satoru)
static void put_wl_string(uint8_t* buf, int* pp, const char* s) {
    int n = 0; while (s[n]) n++;
    int withnul = n + 1;
    int padded  = (withnul + 3) & ~3;
    put32(buf + *pp, (uint32_t)withnul); *pp += 4;
    for (int i = 0; i < n; i++)      buf[*pp + i] = (uint8_t)s[i];
    for (int i = n; i < padded; i++) buf[*pp + i] = 0;
    *pp += padded;
}

// a global must announce its initial state right after bind or gtk/firefox
// stalls before it ever maps a window: wl_shm advertises pixel formats, wl_seat
// its input capabilities, wl_output its geometry+mode and a closing done. (satoru)
static void send_initial_global_events(Client* c, uint32_t id, uint16_t iface,
                                       uint16_t version) {
    if (iface == WL_SHM) {
        uint8_t f[4];
        put32(f, 0); send_event(c, id, 0, f, 4);   // wl_shm.format argb8888
        put32(f, 1); send_event(c, id, 0, f, 4);   // wl_shm.format xrgb8888
    } else if (iface == WL_SEAT) {
        uint8_t cap[4]; put32(cap, 3);             // pointer | keyboard
        send_event(c, id, 0, cap, 4);              // wl_seat.capabilities
        if (version >= 2) {
            uint8_t nm[16]; int p = 0; put_wl_string(nm, &p, "seat0");
            send_event(c, id, 1, nm, p);           // wl_seat.name
        }
    } else if (iface == WL_OUTPUT) {
        int sw = Graphics::GetWidth();
        int sh = Graphics::GetHeight();
        if (sw <= 0) sw = 1280;
        if (sh <= 0) sh = 800;
        uint8_t g[80]; int p = 0;
        put32(g + p, 0);   p += 4;                 // x
        put32(g + p, 0);   p += 4;                 // y
        put32(g + p, 300); p += 4;                 // physical_width (mm)
        put32(g + p, 200); p += 4;                 // physical_height (mm)
        put32(g + p, 0);   p += 4;                 // subpixel unknown
        put_wl_string(g, &p, "kurono");            // make
        put_wl_string(g, &p, "kurono");            // model
        put32(g + p, 0);   p += 4;                 // transform normal
        send_event(c, id, 0, g, p);                // wl_output.geometry
        uint8_t m[16];
        put32(m, 1);                               // flags: current
        put32(m + 4,  (uint32_t)sw);
        put32(m + 8,  (uint32_t)sh);
        put32(m + 12, 60000);                      // refresh (mHz)
        send_event(c, id, 1, m, 16);               // wl_output.mode
        if (version >= 2) { uint8_t s[4]; put32(s, 1); send_event(c, id, 3, s, 4); } // scale
        send_event(c, id, 2, nullptr, 0);          // wl_output.done
    }
}

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
            if (opcode == 0) {                    // bind(name, iface, [version], new_id)
                if (args_len < 8) return;
                uint32_t slen  = get32(args + 4);
                if (slen == 0 || slen > 64) return;
                int padded = ((int)slen + 3) & ~3;
                int off = 8 + padded;                 // name(4)+len(4)+string(padded)
                if (off + 4 > args_len) return;       // need at least the version word
                // wire form of the interface-less new_id is interface(string) +
                // version(u32) + id(u32). a spec-compliant client sends the id;
                // firefox's libwayland/proxy omits it, so when only the version is
                // present we assign the lowest free id (matches libwayland's own
                // allocation -> the id firefox then references). (satoru)
                uint32_t version = get32(args + off);
                uint32_t new_id  = (off + 8 <= args_len) ? get32(args + off + 4)
                                                         : lowest_free_client_id(c);
                char ifname[65];
                int copy_n = (int)slen - 1;
                if (copy_n < 0) copy_n = 0;
                if (copy_n > 64) copy_n = 64;
                for (int i = 0; i < copy_n; i++) ifname[i] = (char)args[8 + i];
                ifname[copy_n] = 0;
                uint16_t iface = iface_id_of(ifname, copy_n);
                if (iface == 0) {
                    send_display_error(c, object_id, 0, "unknown global");
                    return;
                }
                if (version == 0 || version > 32) version = 1;
                register_obj(c, new_id, iface, (uint16_t)version);
                send_initial_global_events(c, new_id, iface, (uint16_t)version);
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
                // real WL_REGION objects now: add/subtract track whether the
                // region is empty, which set_input_region reads. (previously
                // registered as WL_COMPOSITOR, so region contents were lost.) (satoru)
                register_obj(c, rid, WL_REGION, obj->version);
            }
            break;
        }
        case WL_REGION: {
            // destroy=0, add=1 (x,y,w,h), subtract=2. only emptiness matters:
            // an input region with at least one added rect accepts input. (satoru)
            if (opcode == 0) { destroy_obj(c, object_id); break; }
            if (opcode == 1 && args_len >= 16) {
                int32_t rw = (int32_t)get32(args + 8);
                int32_t rh = (int32_t)get32(args + 12);
                if (rw > 0 && rh > 0) obj->region_has_rect = true;
            }
            break;
        }
        case WL_SUBCOMPOSITOR: {
            // get_subsurface(new_id, surface, parent): give an existing wl_surface
            // the subsurface role, parented to `parent`. gtk uses this for the
            // client-side-decoration surfaces around a firefox toplevel. we don't
            // composite sub-surfaces as a separate layer yet, but the OBJECT must
            // exist - otherwise the client's later wl_subsurface requests hit an
            // "invalid object" protocol error and libwayland aborts (the crash in
            // WlLogHandler right after gtk_widget_show). register the role object +
            // remember which surface it wraps; its commits already blit through the
            // WL_SURFACE path. opcode 0 = destroy. (satoru)
            if (opcode == 1) {
                if (args_len < 12) return;
                uint32_t new_id  = get32(args);
                uint32_t surf_id = get32(args + 4);
                uint32_t parent  = get32(args + 8);
                if (!register_obj(c, new_id, WL_SUBSURFACE, obj->version)) return;
                Object* so = find_obj(c, new_id);
                if (so) {
                    so->parent_surface_id = surf_id;   // the wl_surface it roles
                    so->subsurface_parent = parent;    // the parent wl_surface
                }
            } else if (opcode == 0) {
                destroy_obj(c, object_id);
            }
            break;
        }
        case WL_SUBSURFACE: {
            // opcode 0 = destroy. opcode 1 = set_position(x,y): record the offset
            // so commit_surface can composite the wrapped surface ONTO its parent
            // window at the right place (firefox's gtk content surface is a
            // subsurface inset inside the toplevel; without this it drew into its
            // own window BEHIND the black parent = the black-window bug). the other
            // opcodes (place_above/below, set_sync/desync) stay benign no-ops.
            // (satoru)
            if (opcode == 0) { destroy_obj(c, object_id); break; }
            if (opcode == 1 && args_len >= 8) {
                Object* ss = find_obj(c, object_id);
                if (ss) {
                    ss->subsurface_x = (int32_t)get32(args);
                    ss->subsurface_y = (int32_t)get32(args + 4);
                }
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
                bool gotcm = po && UnixSocket::TakePendingControl(c->sd, &cm);
                if (gotcm) {
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
                // format) - no fd here. capture the geometry so commit can
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
                // advertise XDG_TOPLEVEL_STATE_ACTIVATED (4) so firefox treats the
                // window as FOCUSED/ACTIVE. without it the top-level CONTENT
                // browsing context stays inactive, so PresShell::ComputeActiveness()
                // returns false and the web content NEVER paints (chrome paints
                // regardless) - the blank-content-viewport symptom. a headless boot
                // has no pointer click to focus the window, so the activated state
                // must come from the configure. states is a wl_array: 4-byte length
                // then one uint32 entry. (satoru)
                // ACTIVATED only. an experiment also advertising MAXIMIZED (to
                // strip gtk's csd shadows) broke the bridge: with no decoration
                // left to draw, gtk stopped committing a buffer to the TOPLEVEL
                // surface, commit_surface never bridged a wm window, and firefox
                // ran headless-invisible. the csd margins are removed by the
                // set_window_geometry crop instead. (satoru)
                uint8_t cfg[16];
                put32(cfg + 0, 1024);
                put32(cfg + 4, 768);
                put32(cfg + 8, 4);                // states array = 4 bytes (1 entry)
                put32(cfg + 12, 4);               // XDG_TOPLEVEL_STATE_ACTIVATED
                send_event(c, tid, 0, cfg, 16);
                // xdg_surface::configure (opcode 0): serial.
                uint8_t srl[4]; put32(srl, c->serial_next++);
                send_event(c, object_id, 0, srl, 4);
            } else if (opcode == 3) {             // set_window_geometry(x,y,w,h)
                // the visible window rect inside the surface, excluding csd
                // shadow margins - store on the surface so blits crop to it
                // and the bridged window is sized to the VISIBLE rect. (satoru)
                if (args_len < 16) return;
                Object* gsurf = find_obj(c, obj->parent_surface_id);
                if (gsurf && gsurf->iface == WL_SURFACE) {
                    gsurf->geo_x = (int32_t)get32(args);
                    gsurf->geo_y = (int32_t)get32(args + 4);
                    gsurf->geo_w = (int32_t)get32(args + 8);
                    gsurf->geo_h = (int32_t)get32(args + 12);
                    if (gsurf->geo_w < 0) gsurf->geo_w = 0;
                    if (gsurf->geo_h < 0) gsurf->geo_h = 0;
                }
            } else if (opcode == 4) {             // ack_configure
                // no-op
            }
            break;
        }
        case XDG_TOPLEVEL: {
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 2 || opcode == 3) {  // set_title / set_app_id
                // wire string arg: uint32 length (includes the nul) then the
                // bytes, padded to 4. route to the toplevel's parent surface,
                // which owns the bridged wm window. (satoru)
                if (args_len < 4) return;
                uint32_t sl = get32(args);
                if (sl == 0 || sl > (uint32_t)(args_len - 4)) break;
                Object* psurf = find_obj(c, obj->parent_surface_id);
                if (psurf && psurf->iface == WL_SURFACE)
                    apply_toplevel_meta(psurf, (const char*)(args + 4),
                                        (int)sl - 1, opcode == 3);
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
                case 5: {                          // set_input_region(region)
                    // region id 0 (null) = whole surface takes input (the
                    // default). an EMPTY region = input passes through this
                    // surface entirely - firefox's webrender output layers
                    // set exactly that, and routing events to them sent every
                    // click/key into a listener-less void. (satoru)
                    if (args_len < 4) return;
                    uint32_t rid = get32(args);
                    if (rid == 0) { obj->input_none = false; break; }
                    Object* reg = find_obj(c, rid);
                    obj->input_none = (reg && reg->iface == WL_REGION)
                                        ? !reg->region_has_rect : false;
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
                // APPEND, never overwrite: firefox binds the seat twice (gdk +
                // gecko native-layer) and the old last-write-wins slot sent
                // every event to gecko's listener-less proxy - gdk never got
                // input at all (the all-input-dead bug). (satoru)
                if (c->pointer_id_count < Client::WL_MAX_INPUT_RES)
                    c->pointer_ids[c->pointer_id_count++] = pid;
                if (c->pointer_id == 0) c->pointer_id = pid;
            } else if (opcode == 1) {             // get_keyboard(new_id)
                if (args_len < 4) return;
                uint32_t kid = get32(args);
                if (!register_obj(c, kid, WL_KEYBOARD, obj->version)) return;
                if (c->keyboard_id_count < Client::WL_MAX_INPUT_RES)
                    c->keyboard_ids[c->keyboard_id_count++] = kid;
                if (c->keyboard_id == 0) c->keyboard_id = kid;
                // wl_keyboard.keymap (opcode 0): format=1 (XKB_V1) + an fd +
                // size. gdk/firefox mmap the fd and compile the keymap to
                // translate the evdev codes wl_keyboard.key carries - without
                // it typing produces nothing. the fd travels as ancillary via
                // KernelInjectMsg: the recvmsg side re-wraps the passed shm
                // backing (the embedded compiled us keymap) as a real memfd
                // the client can mmap. flush queued events first so the cmsg
                // frame lines up with THIS message. (satoru)
                {
                    tx_flush(c);
                    uint8_t km[16];
                    put32(km + 0, kid);
                    put16(km + 4, 0);                       // wl_keyboard.keymap
                    put16(km + 6, 16);                      // header+8 payload
                    put32(km + 8, 1);                       // format XKB_V1
                    put32(km + 12, g_xkb_keymap_us_len);    // size incl. nul
                    UnixSocket::ControlMsg kcm = {};
                    kcm.passed_fd_count   = 1;
                    kcm.passed_fds[0]     = 1;              // sender-fd slot, unused (satoru)
                    kcm.passed_shm_base[0] = (uint64_t)(uintptr_t)g_xkb_keymap_us;
                    kcm.passed_shm_size[0] = g_xkb_keymap_us_len;
                    if (UnixSocket::KernelInjectMsg(c->sd, km, 16, &kcm) < 0)
                        c->fatal = true;
                }
                // wl_keyboard.repeat_info (opcode 4, v4+): rate, delay. a
                // sane default keeps clients that gate on it happy. (satoru)
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
            // Buffer wedged with a partial message that can't fit - the
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
// flip completes - this is the place where wl_callback::done deliveries
// land closest to vsync, which is what GTK/Qt clients use to throttle
// their next frame.
void DispatchPendingFrames() {
    g_now_ms = Time::GetTicks();
    uint8_t pl[4];
    put32(pl, g_now_ms);                         // monotonic ms timestamp
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use) continue;
        // Liveness probe - KernelInject returns -1 if the sd has been
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
    int lx, ly;
    // deepest surface under the point (a subsurface when the pointer is over
    // one) - gtk routes input per-surface, so the target must be exact. (satoru)
    Object* surf = input_target_at(&fc, gx, gy, lx, ly);
    uint32_t time = Timer::GetRealMs();
    g_now_ms = time;

    // first, send leave to any client that previously had pointer focus on
    // a different surface (focus crossed out - including crossing between a
    // toplevel and its own subsurface). (satoru)
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use || c->pointer_id_count == 0) continue;
        uint32_t prev = c->pointer_focus_sid;
        bool still = (c == fc && surf && surf->id == prev);
        if (prev != 0 && !still) {
            if (find_obj(c, prev))
                ptrs_leave(c, prev);
            c->pointer_focus_sid = 0;
            ptrs_frame(c);
            tx_flush(c);
            if (c->fatal) drop_client(c);
        }
    }

    if (!surf || !fc || fc->pointer_id_count == 0) return;

    // stationary dedup: this forward runs every gui frame, so a parked
    // pointer used to spam 60 identical motion events a second into the
    // client (120 with two proxies) - pure startup poison for firefox.
    // only emit when the position or the target surface changed. (satoru)
    static int      s_last_gx = -1, s_last_gy = -1;
    static uint32_t s_last_sid = 0;
    bool moved = (gx != s_last_gx) || (gy != s_last_gy) || (surf->id != s_last_sid);
    s_last_gx = gx; s_last_gy = gy; s_last_sid = surf->id;

    // enter on first crossing into this surface. (satoru)
    if (fc->pointer_focus_sid != surf->id) {
        fc->pointer_focus_sid = surf->id;
        ptrs_enter(fc, surf->id, lx, ly);
        moved = true;
    }
    if (!moved) return;
    ptrs_motion(fc, time, lx, ly);
    ptrs_frame(fc);
    tx_flush(fc);
    if (fc->fatal) drop_client(fc);
}

void ForwardPointerButton(int gx, int gy, int evdev_button, bool pressed) {
    Client* fc = nullptr;
    int lx, ly;
    Object* surf = input_target_at(&fc, gx, gy, lx, ly);
    if (!surf || !fc || fc->pointer_id_count == 0) return;
    uint32_t time = Timer::GetRealMs();
    g_now_ms = time;

    // ensure the client has pointer focus before the button (a press can
    // arrive before any motion event). (satoru)
    if (fc->pointer_focus_sid != surf->id) {
        if (fc->pointer_focus_sid != 0 && find_obj(fc, fc->pointer_focus_sid))
            ptrs_leave(fc, fc->pointer_focus_sid);
        fc->pointer_focus_sid = surf->id;
        ptrs_enter(fc, surf->id, lx, ly);
    }
    ptrs_button(fc, time, (uint32_t)evdev_button, pressed);
    ptrs_frame(fc);

    // a press also moves keyboard focus to this surface's client, mirroring
    // click-to-focus. emit keyboard leave/enter as focus moves. (satoru)
    if (pressed) {
        for (int i = 0; i < WL_MAX_CLIENTS; i++) {
            Client* c = &g_clients[i];
            if (!c->in_use || c->keyboard_id_count == 0) continue;
            if (c == fc) continue;
            if (c->keyboard_focus_sid != 0) {
                if (find_obj(c, c->keyboard_focus_sid))
                    kbds_leave(c, c->keyboard_focus_sid);
                c->keyboard_focus_sid = 0;
                tx_flush(c);
                if (c->fatal) drop_client(c);
            }
        }
        if (fc->keyboard_id_count != 0 && fc->keyboard_focus_sid != surf->id) {
            fc->keyboard_focus_sid = surf->id;
            kbds_enter(fc, surf->id);
        }
    }
    tx_flush(fc);
    if (fc->fatal) drop_client(fc);
}

void ForwardPointerAxis(int gx, int gy, int axis, int value) {
    Client* fc = nullptr;
    int lx, ly;
    // deepest surface under the point: for firefox that is the gtk CONTENT
    // subsurface (the page), not the csd toplevel - gtk routes input strictly
    // per-surface, so an axis on the toplevel never scrolled the page. (satoru)
    Object* surf = input_target_at(&fc, gx, gy, lx, ly);
    if (!surf || !fc || fc->pointer_id_count == 0) return;
    uint32_t time = Timer::GetRealMs();
    g_now_ms = time;
    // establish pointer presence on the target surface first (gtk drops axis
    // events for a surface without pointer focus), then the v5 wheel triple:
    // source + discrete + axis in one frame. value is in surface pixels;
    // pointer_axis does the wl_fixed conversion (a double to_fixed once made
    // each notch ~49000px, which gtk discarded). notch = 32px. (satoru)
    if (fc->pointer_focus_sid != surf->id) {
        if (fc->pointer_focus_sid != 0 && find_obj(fc, fc->pointer_focus_sid))
            ptrs_leave(fc, fc->pointer_focus_sid);
        fc->pointer_focus_sid = surf->id;
        ptrs_enter(fc, surf->id, lx, ly);
    }
    ptrs_motion(fc, time, lx, ly);
    ptrs_axis(fc, time, (uint32_t)axis, value);
    ptrs_frame(fc);
    tx_flush(fc);
    if (fc->fatal) drop_client(fc);
}

bool IsWaylandWindow(int win_id) {
    int sd; uint32_t sid;
    return winmap_get(win_id, sd, sid);
}

int DropClientsOfPid(uint32_t pid) {
    if (pid == 0) return 0;
    int dropped = 0;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use || c->sd < 0) continue;
        UnixSocket::Credentials cr;
        if (UnixSocket::GetPeerCred(c->sd, &cr) != 0) continue;
        if (cr.pid != pid) continue;
        // destroy every surface first so bridged wm windows close with the
        // client (destroy_obj cascades window teardown + winmap cleanup). walk
        // by index with an in_use guard: destroy_obj may shrink object_high. (satoru)
        int high = c->object_high;
        for (int j = 0; j < high; j++) {
            Object& o = c->objects[j];
            if (o.in_use && o.iface == WL_SURFACE && o.wm_window)
                destroy_obj(c, o.id);
        }
        drop_client(c);
        dropped++;
    }
    return dropped;
}

void ForwardKey(int key, bool pressed, uint32_t mods) {
    uint32_t evdev = key_to_evdev(key);
    uint32_t time  = Timer::GetRealMs();
    g_now_ms = time;
    // route to whichever client currently holds keyboard focus, on EVERY
    // wl_keyboard resource it created. (satoru)
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use || c->keyboard_id_count == 0) continue;
        if (c->keyboard_focus_sid == 0) continue;
        if (!find_obj(c, c->keyboard_focus_sid)) { c->keyboard_focus_sid = 0; continue; }
        kbds_modifiers(c, mods);
        if (evdev != 0)
            kbds_key(c, time, evdev, pressed);
        tx_flush(c);
        if (c->fatal) drop_client(c);
    }
}

}  // namespace WaylandServer
