#include "wayland_server.h"
#include "../net/unix_socket.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"

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
    return true;
}

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
    }
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
                if (args_len < 8) return;
                uint32_t pid = get32(args);
                register_obj(c, pid, WL_SHM_POOL, 1);
            }
            break;
        }
        case WL_SHM_POOL: {
            if (opcode == 0) {                    // create_buffer
                if (args_len < 24) return;
                uint32_t bid = get32(args);
                register_obj(c, bid, WL_BUFFER, 1);
            } else if (opcode == 1) {             // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 2) {             // resize
                // no-op (size validation handled at attach time)
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
                    // no-op; rendering hooks live in the WM bridge.
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
                    obj->damage.valid    = false; // consume accumulated damage
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

}  // namespace WaylandServer
