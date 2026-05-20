#include "wayland_server.h"
#include "../net/unix_socket.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"

namespace {

using namespace WaylandServer;

Client g_clients[WL_MAX_CLIENTS];
int    g_listen_sd = -1;

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

Client* client_for_sd(int sd) {
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].sd == sd) return &g_clients[i];
    }
    return nullptr;
}

Client* alloc_client(int sd) {
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            Client* c = &g_clients[i];
            c->in_use = true;
            c->sd     = sd;
            c->object_count = 1;
            c->rx_partial_len = 0;
            // Object id 1 is implicitly wl_display.
            c->objects[0].id      = 1;
            c->objects[0].iface   = WL_DISPLAY;
            c->objects[0].version = 1;
            c->objects[0].in_use  = true;
            c->objects[0].wm_window = nullptr;
            return c;
        }
    }
    return nullptr;
}

Object* find_obj(Client* c, uint32_t id) {
    for (int i = 0; i < c->object_count; i++) {
        if (c->objects[i].in_use && c->objects[i].id == id) return &c->objects[i];
    }
    return nullptr;
}

bool register_obj(Client* c, uint32_t id, uint16_t iface, uint16_t version) {
    if (c->object_count >= WL_MAX_OBJECTS) return false;
    Object& o = c->objects[c->object_count++];
    o.id = id; o.iface = iface; o.version = version;
    o.in_use = true; o.wm_window = nullptr;
    o.width = 0; o.height = 0;
    o.parent_surface_id = 0;
    return true;
}

// Send one Wayland event to the client.  Header is 8 bytes.
void send_event(int sd, uint32_t object_id, uint16_t opcode,
                const uint8_t* payload, uint16_t payload_len) {
    uint8_t hdr[8];
    put32(hdr, object_id);
    put16(hdr + 4, opcode);
    put16(hdr + 6, (uint16_t)(payload_len + 8));
    UnixSocket::KernelInject(sd, hdr, 8);
    if (payload_len > 0) UnixSocket::KernelInject(sd, payload, payload_len);
}

// Send wl_display::error event (opcode 0).
void send_display_error(int sd, uint32_t object_id, uint32_t code,
                        const char* msg) {
    uint8_t buf[256];
    int p = 0;
    put32(buf + p, object_id); p += 4;
    put32(buf + p, code);      p += 4;
    int slen = 0; while (msg[slen]) slen++;
    int padded = (slen + 1 + 3) & ~3;
    put32(buf + p, (uint32_t)(slen + 1)); p += 4;
    for (int i = 0; i < slen; i++) buf[p++] = (uint8_t)msg[i];
    while ((p % 4) || (p - 12 < padded)) { buf[p++] = 0; if (p >= (int)sizeof(buf)) break; }
    send_event(sd, 1, 0, buf, (uint16_t)p);
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

void send_global(int sd, uint32_t registry_id, const Global& g) {
    uint8_t buf[128];
    int p = 0;
    put32(buf + p, g.name); p += 4;
    int slen = 0; while (g.iface[slen]) slen++;
    int slen_with_nul = slen + 1;
    put32(buf + p, (uint32_t)slen_with_nul); p += 4;
    for (int i = 0; i < slen; i++) buf[p++] = (uint8_t)g.iface[i];
    buf[p++] = 0;
    while (p % 4) buf[p++] = 0;
    put32(buf + p, g.version); p += 4;
    // wl_registry::global = opcode 0
    send_event(sd, registry_id, 0, buf, (uint16_t)p);
}

// Map iface name → InterfaceId
uint16_t iface_id_of(const char* name) {
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
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return table[i].v;
    }
    return 0;
}

// Process one Wayland request.
void handle_request(Client* c, uint32_t object_id, uint16_t opcode,
                    const uint8_t* args, int args_len) {
    Object* obj = find_obj(c, object_id);
    if (!obj) {
        send_display_error(c->sd, object_id, 0, "invalid object");
        return;
    }
    switch (obj->iface) {
        case WL_DISPLAY: {
            if (opcode == 0) {                    // sync
                if (args_len < 4) return;
                uint32_t cb = get32(args);
                register_obj(c, cb, 0, 1);
                // wl_callback::done event opcode 0, single u32 (serial)
                uint8_t pl[4]; put32(pl, 0);
                send_event(c->sd, cb, 0, pl, 4);
            } else if (opcode == 1) {             // get_registry
                if (args_len < 4) return;
                uint32_t reg = get32(args);
                register_obj(c, reg, WL_REGISTRY, 1);
                for (int i = 0; g_globals[i].iface; i++) {
                    send_global(c->sd, reg, g_globals[i]);
                }
            }
            break;
        }
        case WL_REGISTRY: {
            if (opcode == 0) {                    // bind
                if (args_len < 12) return;
                uint32_t name = get32(args);
                uint32_t slen = get32(args + 4);
                if (slen > 64) slen = 64;
                char ifname[65];
                int i;
                for (i = 0; i < (int)slen - 1 && (int)(8 + i) < args_len; i++) {
                    ifname[i] = (char)args[8 + i];
                }
                ifname[i] = 0;
                int padded = (slen + 3) & ~3;
                int off = 8 + padded;
                if (off + 8 > args_len) return;
                uint32_t version = get32(args + off);
                uint32_t new_id  = get32(args + off + 4);
                uint16_t iface   = iface_id_of(ifname);
                if (iface) register_obj(c, new_id, iface, (uint16_t)version);
                (void)name;
            }
            break;
        }
        case WL_COMPOSITOR: {
            if (opcode == 0) {                    // create_surface
                if (args_len < 4) return;
                uint32_t sid = get32(args);
                register_obj(c, sid, WL_SURFACE, obj->version);
            }
            break;
        }
        case WL_SHM: {
            if (opcode == 0) {                    // create_pool
                if (args_len < 8) return;
                uint32_t pid = get32(args);
                register_obj(c, pid, WL_BUFFER, 1);    // pool ~ buffer parent
            }
            break;
        }
        case XDG_WM_BASE: {
            if (opcode == 2) {                    // get_xdg_surface
                if (args_len < 8) return;
                uint32_t xs = get32(args);
                uint32_t surf = get32(args + 4);
                register_obj(c, xs, XDG_SURFACE, obj->version);
                Object* o = find_obj(c, xs);
                if (o) o->parent_surface_id = surf;
            } else if (opcode == 3) {             // pong (response to ping)
                // no-op
            }
            break;
        }
        case XDG_SURFACE: {
            if (opcode == 1) {                    // get_toplevel
                if (args_len < 4) return;
                uint32_t tid = get32(args);
                register_obj(c, tid, XDG_TOPLEVEL, obj->version);
                // xdg_toplevel::configure (opcode 0): width, height,
                // states (empty array).
                uint8_t cfg[12];
                put32(cfg + 0, 1024);
                put32(cfg + 4, 768);
                put32(cfg + 8, 0);
                send_event(c->sd, tid, 0, cfg, 12);
                // xdg_surface::configure (opcode 0): serial.
                uint8_t srl[4]; put32(srl, 1);
                send_event(c->sd, object_id, 0, srl, 4);
            }
            break;
        }
        case WL_SURFACE: {
            // attach (1), damage (2), commit (6), frame (3)  -  handle commit.
            if (opcode == 6) {                    // commit
                // Schedule a frame callback for the next render tick.
            } else if (opcode == 3) {             // frame
                if (args_len < 4) return;
                uint32_t cb = get32(args);
                register_obj(c, cb, 0, 1);
                // Fire immediately; real compositor would defer to vsync.
                uint8_t pl[4]; put32(pl, 1);
                send_event(c->sd, cb, 0, pl, 4);
            }
            break;
        }
        default: break;
    }
}

void on_data(int sd, const uint8_t* data, int len, void* user) {
    (void)user;
    Client* c = client_for_sd(sd);
    if (!c) return;
    // Append to partial buffer, then drain whole messages.
    int copy = len;
    if (c->rx_partial_len + copy > (int)sizeof(c->rx_partial)) {
        copy = (int)sizeof(c->rx_partial) - c->rx_partial_len;
    }
    for (int i = 0; i < copy; i++) c->rx_partial[c->rx_partial_len + i] = data[i];
    c->rx_partial_len += copy;

    int p = 0;
    while (c->rx_partial_len - p >= 8) {
        uint32_t obj_id = get32(c->rx_partial + p);
        uint16_t op    = (uint16_t)(c->rx_partial[p + 4] | (c->rx_partial[p + 5] << 8));
        uint16_t sz    = (uint16_t)(c->rx_partial[p + 6] | (c->rx_partial[p + 7] << 8));
        if (sz < 8 || sz > 4096) {
            // Bad frame  -  discard partial.
            c->rx_partial_len = 0;
            return;
        }
        if (c->rx_partial_len - p < sz) break;
        handle_request(c, obj_id, op, c->rx_partial + p + 8, sz - 8);
        p += sz;
    }
    if (p > 0) {
        int rem = c->rx_partial_len - p;
        for (int i = 0; i < rem; i++) c->rx_partial[i] = c->rx_partial[p + i];
        c->rx_partial_len = rem;
    }
}

void on_connect(int server_sd, int new_client_sd, void* user) {
    (void)server_sd; (void)user;
    Client* c = alloc_client(new_client_sd);
    if (!c) {
        // No room; close client.
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
    for (int i = 0; i < WL_MAX_CLIENTS; i++) g_clients[i].in_use = false;

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

    // Drop a marker file so user-space tooling sees the socket.
    KVFS::WriteString("/system/run/user/1000/wayland-0.info",
        "kurono wayland compositor v1\n"
        "globals: wl_compositor v5, wl_shm v1, wl_seat v7, wl_output v3,\n"
        "         xdg_wm_base v3, zwp_linux_dmabuf_v1 v3\n");

    SerialLogger::Log("Wayland: listening on /system/run/user/1000/wayland-0\r\n");
}

int  ListenSd() { return g_listen_sd; }

void DispatchPendingFrames() {
    // Walk all surfaces and send wl_callback::done events for any
    // pending frame callbacks.  Currently fired inline in `frame`.
}

void DamageAll() {
    // Force a full redraw of every committed surface.
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
        for (int j = 0; j < g_clients[i].object_count; j++) {
            if (g_clients[i].objects[j].in_use &&
                g_clients[i].objects[j].iface == WL_SURFACE) n++;
        }
    }
    return n;
}

}  // namespace WaylandServer
