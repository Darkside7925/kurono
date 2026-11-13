#include "wayland_server.h"
#include "../proc/spinlock.h"   // frame-callback ring lock (satoru)
static bool g_wl_childalpha = false;   // kurono.wl.childalpha=1 (satoru)
static bool g_wl_deco = false;         // kurono.wl.deco=1 (satoru)
static bool g_wl_fullsize = false;     // kurono.wl.fullsize=1 (satoru)
// wl_shm reference census (memcensus line): pool/buffer objects and bytes. the
// memfd-side counts come from LinuxSyscall::ShmKinds. (satoru)
static uint32_t g_pp_pools_live = 0, g_pp_pools_made = 0;
static uint32_t g_pp_bufs_live = 0,  g_pp_bufs_made = 0;
static uint64_t g_pp_pool_kb = 0, g_pp_buf_kb = 0;
static bool g_wl_poolfree = false;   // kurono.wl.poolfree=1 (satoru)
static bool g_wl_scdirect = false;   // kurono.wl.scdirect=1: blit commits straight into the back buffer under gfx.scissor, as before (satoru)
#include "../net/unix_socket.h"
#include "../linux/linux_syscall.h"   // ShmUnpinPin, wl_shm pin release (satoru)
#include "../system/key_layouts.h"   // settings keyboard layout keymaps (satoru)
#include "xkb_keymap_us.h"        // compiled us keymap for wl_keyboard.keymap (satoru)
#include "../drivers/serial.h"
#include "../drivers/disklog.h"       // eager log off at first paint (satoru)
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "../drivers/graphics.h"     // framebuffer blit target (satoru)
#include "../drivers/outputs.h"      // wl_output reports the primary output (satoru)
#include "../kernel/heap.h"          // per-surface shadow buffers (satoru)
#include "../kernel/pmm.h"           // page-aligned keymap backing
#include "../drivers/timer.h"        // pit-tick ms timestamps for frame cbs (satoru)
extern volatile bool g_wltrace;      // kurono.wltrace=1 (linux_syscall.cpp) (satoru)
#include "window_manager.h"          // bridge surfaces to wm windows (satoru)
#include "app_icons.h"               // per-app window logo ids (satoru)
#include "../drivers/keyboard.h"     // Key enum -> linux evdev keycode map (satoru)
#include "../drivers/mouse.h"        // live pointer pos for the scroll thunk (satoru)
#include "../system/clipboard.h"     // system clipboard store + history (satoru)

// the client mmaps the keymap fd from offset 0, and a memfd maps whole pages
// from its base, so the backing must start on a page. the embedded array does
// not, and clients read the bytes in front of it ("unexpected non-ASCII
// character"). copy it once into page-aligned memory shared by every client.
static const uint8_t* keymap_backing(uint32_t* len) {
    // the layout picked in settings: a page-aligned compiled keymap. (satoru)
    if (const uint8_t* k = KeyLayouts::WaylandKeymap(len)) return k;
    *len = g_xkb_keymap_us_len;
    static uint8_t* page = nullptr;
    if (!page) {
        uint8_t* p = (uint8_t*)PMM::AllocBytes(g_xkb_keymap_us_len);
        if (!p) return g_xkb_keymap_us;
        for (uint32_t i = 0; i < g_xkb_keymap_us_len; i++) p[i] = g_xkb_keymap_us[i];
        page = p;
    }
    return page;
}

extern volatile bool g_wltrace;   // kurono.wltrace=1 (satoru)
namespace {

using namespace WaylandServer;

// per-connection state is ~96 kb (the object table dominates). clients come
// and go with apps, so the slots are pointers allocated on connect and freed
// on disconnect; an idle compositor keeps only the 16-entry pointer array. (satoru)
// the client table is chunked: 16-slot pointer chunks allocated on demand and never moved,
// so a Client* and its slot index stay valid; g_ncap is the slot count of the allocated chunks (satoru)
constexpr int CL_CHUNK = 16;
Client** g_cl_chunks[WL_MAX_CLIENTS / CL_CHUNK];
int      g_ncap = 0;
Client*  g_cl_null = nullptr;
inline Client** cl_ref(int i) {
    if (i < 0 || i >= g_ncap) { g_cl_null = nullptr; return &g_cl_null; }
    return &g_cl_chunks[i / CL_CHUNK][i % CL_CHUNK];
}
bool cl_grow() {
    int n = g_ncap / CL_CHUNK;
    if (n >= WL_MAX_CLIENTS / CL_CHUNK) return false;
    Client** ch = (Client**)KernelHeap::Alloc(sizeof(Client*) * CL_CHUNK);
    if (!ch) return false;
    memset(ch, 0, sizeof(Client*) * CL_CHUNK);
    g_cl_chunks[n] = ch;
    g_ncap += CL_CHUNK;
    return true;
}
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
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (c && c->in_use && c->sd == sd) return c;
    }
    return nullptr;
}

// client-controlled sizes are bounded here. a surface or buffer edge longer
// than WL_MAX_EDGE, a buffer above WL_MAX_BUF_PX pixels (a 64 MiB shadow) or
// all shadows together above WL_MAX_SHADOW_TOTAL bytes is refused, so a client
// cannot exhaust the kernel heap with 1 GiB shadows. (satoru)
constexpr int32_t  WL_MAX_EDGE         = 16384;
constexpr uint64_t WL_MAX_BUF_PX       = 16ull * 1024 * 1024;
constexpr uint64_t WL_MAX_SHADOW_TOTAL = 512ull * 1024 * 1024;
uint64_t g_shadow_bytes = 0;

// free a surface shadow and give its bytes back to the global budget. (satoru)
inline void shadow_release(Object& o) {
    if (o.shadow) {
        KernelHeap::Free(o.shadow);
        uint64_t b = (uint64_t)o.shadow_w * (uint64_t)o.shadow_h * 4u;
        g_shadow_bytes = (g_shadow_bytes > b) ? g_shadow_bytes - b : 0;
    }
    o.shadow = nullptr;
    o.shadow_w = 0; o.shadow_h = 0;
}

// clamp a client int32 into [lo, hi]. (satoru)
inline int32_t wl_clamp(int64_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : (int32_t)v);
}

// Zero the extended (phase-3) bookkeeping on a fresh object slot so a
// reused slot never inherits a stale pool pointer / buffer geometry. (satoru)
inline void clear_obj_ext(Object& o) {
    o.pool_base         = nullptr;
    o.pool_size         = 0;
    o.pool_map_size     = 0;
    o.buf_pool_id       = 0;
    o.buf_offset        = 0;
    o.buf_stride        = 0;
    o.buf_format        = 0;
    o.buf_base          = nullptr;
    o.buf_base_size     = 0;
    o.pin_slot = 0; o.pin_pin = 0;
    o.opq_valid         = false;
    o.opq_x = 0; o.opq_y = 0; o.opq_w = 0; o.opq_h = 0;
    o.reg_x0 = 0; o.reg_y0 = 0; o.reg_x1 = 0; o.reg_y1 = 0;
    o.reg_adds = 0; o.reg_subs = 0;
    o.commit_n = 0; o.commit_first_ms = 0; o.commit_last_ms = 0;
    o.pending_buffer_id = 0;
    o.attach_pending    = false;
    o.attach_x          = 0;
    o.attach_y          = 0;
    o.surf_alpha        = 255;
    o.mapped            = false;
    o.subsurface_parent = 0;
    o.subsurface_x      = 0;
    o.subsurface_y      = 0;
    // free a stale shadow from a recycled slot (fresh g_clients are bss-zero,
    // so a garbage pointer is impossible on first use). (satoru)
    shadow_release(o);
    o.shadow_fmt    = 0;
    o.shadow_filled = false;
    o.title[0]      = 0;
    o.app_icon_hint = 0xFF;
    o.geo_x = 0; o.geo_y = 0;
    o.geo_w = 0; o.geo_h = 0;
    o.input_none      = false;
    o.region_has_rect = false;
    o.popup_obj = 0; o.popup_parent_sid = 0;
    o.popup_x = 0; o.popup_y = 0; o.popup_w = 0; o.popup_h = 0;
    o.popup_xs = 0; o.popup_seq = 0; o.popup_grab = false;
    o.pos_w = 0; o.pos_h = 0;
    o.pos_ax = 0; o.pos_ay = 0; o.pos_aw = 0; o.pos_ah = 0;
    o.pos_ox = 0; o.pos_oy = 0;
    o.pos_anchor = 0; o.pos_gravity = 0; o.pos_cadj = 0;
    o.tl_parent_sid = 0; o.tl_last_w = 0; o.tl_last_h = 0;
    o.tl_cfg_w = 0; o.tl_cfg_h = 0; o.tl_close_ms = 0;
}

void clip_client_dropped(int sd);   // selection / offer cleanup, defined below (satoru)

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
    c->commit_n = 0; c->commit_first_ms = 0; c->commit_last_ms = 0;
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
    c->data_device_count   = 0;
    c->primary_device_count = 0;
    for (int i = 0; i < Client::WL_MAX_DATA_DEV; i++) {
        c->data_device_ids[i] = 0;
        c->primary_device_ids[i] = 0;
    }
    c->clip_offer_id       = 0;
    c->primary_offer_id    = 0;
    c->clip_server_id_next = 0;
    // Object id 1 = wl_display, always live. the previous client's shadows
    // are freed here: slots past the new object_high would otherwise keep
    // them forever. (satoru)
    for (int i = 0; i < WL_MAX_OBJECTS; i++) {
        c->objects[i].in_use = false;
        shadow_release(c->objects[i]);
    }
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
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (c && c->in_use && c->sd == sd) {
            clip_client_dropped(sd);   // stale connection's clipboard objects (satoru)
            reset_client_state(c, sd);
            return c;
        }
    }
    for (int i = 0; i < g_ncap; i++) {
        if (!(*cl_ref(i))) {
            Client* c = (Client*)KernelHeap::Alloc(sizeof(Client));
            if (!c) return nullptr;
            memset(c, 0, sizeof(Client));
            c->sd = -1;
            (*cl_ref(i)) = c;
        }
        Client* c = (*cl_ref(i));
        if (!c->in_use) {
            reset_client_state(c, sd);
            return c;
        }
    }
    // table full: add a chunk and take its first slot (satoru)
    int first = g_ncap;
    if (!cl_grow()) return nullptr;
    Client* c = (Client*)KernelHeap::Alloc(sizeof(Client));
    if (!c) return nullptr;
    memset(c, 0, sizeof(Client));
    c->sd = -1;
    (*cl_ref(first)) = c;
    reset_client_state(c, sd);
    return c;
}

void log_commit_census(Client* c) {
    if (!c || !g_wltrace) return;
    if (c->commit_n >= 2) {
        uint32_t span = c->commit_last_ms - c->commit_first_ms;
        SerialLogger::Log("Wayland: client commits="); SerialLogger::LogDec((int)c->commit_n);
        SerialLogger::Log(" span_ms="); SerialLogger::LogDec((int)span);
        SerialLogger::Log(" fps="); SerialLogger::LogDec(span ? (int)(((uint64_t)(c->commit_n - 1) * 1000ull) / span) : 0);
        SerialLogger::Log("\r\n");
    }
    for (int i = 0; i < c->object_high; i++) {
        Object& o = c->objects[i];
        if (!o.in_use || o.iface != WL_SURFACE || o.commit_n < 2) continue;
        uint32_t span = o.commit_last_ms - o.commit_first_ms;
        SerialLogger::Log("Wayland: surf="); SerialLogger::LogDec((int)o.id);
        SerialLogger::Log(" commits="); SerialLogger::LogDec((int)o.commit_n);
        SerialLogger::Log(" span_ms="); SerialLogger::LogDec((int)span);
        SerialLogger::Log(" fps="); SerialLogger::LogDec(span ? (int)(((uint64_t)(o.commit_n - 1) * 1000ull) / span) : 0);
        SerialLogger::Log("\r\n");
    }
}

// true while any live client still names this exact SCM_RIGHTS pass (a pool or
// one of its buffers carries the token). the last object drops the pass pin;
// slot reuse cannot alias because the pin id is unique per pass. (satoru)
bool pin_token_refed(uint32_t slot, uint32_t pin) {
    if (!pin) return false;
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use) continue;
        for (int j = 0; j < c->object_high; j++) {
            Object& o = c->objects[j];
            if (!o.in_use) continue;
            if ((o.iface == WL_SHM_POOL || o.iface == WL_BUFFER) &&
                o.pin_slot == slot && o.pin_pin == pin) return true;
        }
    }
    return false;
}
void pool_unpin_token(uint32_t slot, uint32_t pin) {
    if (!g_wl_poolfree || !pin) return;
    if (pin_token_refed(slot, pin)) return;
    LinuxSyscall::ShmUnpinPin(slot, pin);
}
// a dropped client names no more pools: release every pass it held. its own
// objects are skipped because c->in_use is already false. (satoru)
void pool_release_client(Client* c) {
    if (!g_wl_poolfree || !c) return;
    for (int i = 0; i < c->object_high; i++) {
        Object& o = c->objects[i];
        if (!o.in_use) continue;
        if (o.iface == WL_SHM_POOL || o.iface == WL_BUFFER)
            pool_unpin_token(o.pin_slot, o.pin_pin);
    }
}

// subtract a closing client's live shm objects from the census. called once at
// each drop path; protocol destroys already decremented their own objects. (satoru)
void pool_account_client(Client* c) {
    if (!c) return;
    for (int i = 0; i < c->object_high; i++) {
        Object& o = c->objects[i];
        if (!o.in_use) continue;
        if (o.iface == WL_SHM_POOL) {
            uint64_t b = o.pool_map_size ? o.pool_map_size : o.pool_size;
            if (g_pp_pools_live) g_pp_pools_live--;
            g_pp_pool_kb = (g_pp_pool_kb > (b >> 10)) ? g_pp_pool_kb - (b >> 10) : 0;
        } else if (o.iface == WL_BUFFER) {
            uint64_t b = (uint64_t)o.buf_stride * (uint64_t)(o.height > 0 ? o.height : 0);
            if (g_pp_bufs_live) g_pp_bufs_live--;
            g_pp_buf_kb = (g_pp_buf_kb > (b >> 10)) ? g_pp_buf_kb - (b >> 10) : 0;
        }
    }
}

void drop_client(Client* c) {
    log_commit_census(c);
    if (!c || !c->in_use) return;
    pool_account_client(c);
    int sd = c->sd;
    c->in_use = false;
    pool_release_client(c);
    c->sd = -1;
    clip_client_dropped(sd);   // sources/offers/pulls/writes of this connection (satoru)
    // a dropped client's shadows went unfreed until its slot was reused. (satoru)
    for (int i = 0; i < WL_MAX_OBJECTS; i++) shadow_release(c->objects[i]);
    c->tx_len = 0;
    c->rx_partial_len = 0;
    UnixSocket::Close(sd);
    // the slot's 96 kb goes back to the heap; the next client allocates its own.
    for (int i = 0; i < g_ncap; i++) {
        if ((*cl_ref(i)) == c) { (*cl_ref(i)) = nullptr; break; }
    }
    KernelHeap::Free(c);
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

void send_event(Client* c, uint32_t object_id, uint16_t opcode, const uint8_t* payload, uint16_t payload_len);   // defined below (satoru)
// kurono.wltrace=1: protocol trace kept in RAM. printing each message live
// changed the timing enough to hide the black-window race (6/6 good boots with
// it vs ~50% black without), so record now and print on demand. (satoru)
struct WltEnt { uint32_t ms; uint8_t dir; uint8_t sd; uint16_t iface; uint32_t id; uint16_t op; uint32_t a0, a1; };
constexpr uint32_t WLT_SLOTS = 4096;
WltEnt g_wlt[WLT_SLOTS];
uint32_t g_wlt_n = 0;
inline uint32_t get32(const uint8_t* p);
void wlt_note(int dir, int sd, uint32_t id, uint16_t iface, uint16_t op, uint32_t a0, uint32_t a1) {
    if (g_wlt_n >= WLT_SLOTS) return;   // keep the first window of the session (satoru)
    WltEnt* e = &g_wlt[g_wlt_n++];
    e->ms = Timer::GetRealMs(); e->dir = (uint8_t)dir; e->sd = (uint8_t)sd; e->iface = iface;
    e->id = id; e->op = op; e->a0 = a0; e->a1 = a1;
}
void mark_tree_root_dirty(Client* c, Object* surf);   // defined with the surface-tree helpers (satoru)
void destroy_obj(Client* c, uint32_t id) {
    int slot = find_obj_slot(c, id);
    if (slot < 0) return;
    Object& o = c->objects[slot];
    // a subsurface or popup leaving the tree: repaint the window it was
    // composited onto, or a hidden tooltip / closed menu stays on screen. (satoru)
    if (o.iface == WL_SUBSURFACE) {
        Object* wrapped = find_obj(c, o.parent_surface_id);
        if (wrapped) mark_tree_root_dirty(c, wrapped);
    } else if (o.iface == XDG_POPUP) {
        Object* ps = find_obj(c, o.parent_surface_id);
        if (ps && ps->iface == WL_SURFACE && ps->popup_obj == id) {
            mark_tree_root_dirty(c, ps);
            ps->popup_obj = 0;
            ps->popup_grab = false;
        }
    }
    // Cascade: children whose parent is this surface get unparented.
    if (o.iface == WL_SURFACE) {
        if (!o.wm_window) mark_tree_root_dirty(c, &o);
        for (int i = 0; i < c->object_high; i++) {
            if (c->objects[i].in_use &&
                c->objects[i].parent_surface_id == id) {
                c->objects[i].parent_surface_id = 0;
            }
            // a recycled id must not inherit this surface's subsurfaces or
            // popups. (satoru)
            if (c->objects[i].in_use && c->objects[i].iface == WL_SUBSURFACE &&
                c->objects[i].subsurface_parent == id)
                c->objects[i].subsurface_parent = 0;
            if (c->objects[i].in_use && c->objects[i].iface == WL_SURFACE &&
                c->objects[i].popup_parent_sid == id) {
                c->objects[i].popup_obj = 0;
                c->objects[i].popup_parent_sid = 0;
            }
        }
        // drop focus references BEFORE closing the window: the close refocuses
        // another window, which reaches set_keyboard_focus. (satoru)
        if (c->pointer_focus_sid  == id) c->pointer_focus_sid  = 0;
        if (c->keyboard_focus_sid == id) c->keyboard_focus_sid = 0;
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
    // wl_display.delete_id: the client keeps a client-made id as a zombie until it
    // gets this, and only then may reuse it. without it every late request on the
    // id looked like a protocol error. server-made ids (>= 0xff000000) never get one. (satoru)
    if (id < 0xFF000000u && !c->fatal) {
        uint8_t did[4]; put32(did, id);
        send_event(c, 1, 1, did, 4);
    }
    o.in_use = false;
    o.wm_window = nullptr;
    o.pending_frame_cb = 0;
    o.damage.valid = false;
    // release the shadow with the surface (a dead slot may never be reused,
    // so waiting for clear_obj_ext would leak). (satoru)
    shadow_release(o);
    o.shadow_filled = false;
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
    if (g_wltrace) {   // kurono.wltrace=1: silent ring, dumped later (satoru)
        Object* eo = find_obj(c, object_id);
        wlt_note(1, c->sd, object_id, eo ? eo->iface : 0xFFFF, opcode,
                 payload_len >= 4 ? get32(payload) : 0, payload_len >= 8 ? get32(payload + 4) : 0);
    }
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
// the frame-callback ring is touched from the client's sendmsg (on_data, any
// cpu) and from the bsp desktop render (DispatchPendingFrames), so it gets its
// own lock. leaf lock: nothing called under it takes another. (satoru)
Spinlock g_fcb_lock;

void push_frame_cb(Client* c, uint32_t cb_id) {
    SpinLockGuard g(g_fcb_lock);
    int next = (c->frame_cb_head + 1) % WL_MAX_FRAME_CB;
    if (next == c->frame_cb_tail) {
        // full: squeeze out the zeroed holes commit-time delivery leaves before
        // dropping anything - a still-pending callback (gecko's ReadyToDraw on
        // the toplevel) must not be the one thrown away. (satoru)
        uint32_t keep[WL_MAX_FRAME_CB]; int nk = 0;
        for (int i = c->frame_cb_tail; i != c->frame_cb_head; i = (i + 1) % WL_MAX_FRAME_CB)
            if (c->frame_cb[i]) keep[nk++] = c->frame_cb[i];
        c->frame_cb_tail = 0; c->frame_cb_head = 0;
        for (int k = 0; k < nk; k++) c->frame_cb[c->frame_cb_head++] = keep[k];
        next = (c->frame_cb_head + 1) % WL_MAX_FRAME_CB;
    }
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
    { 10, "zwp_primary_selection_device_manager_v1", 1 },
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
        { "zwp_primary_selection_device_manager_v1", ZWP_PRIMARY_MGR },
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
    // clamp with 64-bit math into [0, WL_MAX_EDGE]: raw client values made
    // dx + dw overflow in the shadow copy and blit, skipping their clamps and
    // turning a damage rect into a kernel heap write ~8 GB past the shadow.
    // gdk's "everything" damage (0,0,INT32_MAX,INT32_MAX) stays full. (satoru)
    {
        int32_t x0 = wl_clamp(x, 0, WL_MAX_EDGE);
        int32_t y0 = wl_clamp(y, 0, WL_MAX_EDGE);
        int32_t x1 = wl_clamp((int64_t)x + w, 0, WL_MAX_EDGE);
        int32_t y1 = wl_clamp((int64_t)y + h, 0, WL_MAX_EDGE);
        if (x1 <= x0 || y1 <= y0) return;
        x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    }
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
WlWinMap g_winmap[WL_MAX_CLIENTS * 4];

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
                         int ox = 0, int oy = 0, bool src_alpha = false);
void blit_child_subsurfaces(Client* c, Object* parent_surf, Window* pwin);
// forward decls: keyboard focus emitters (defined below) so a toplevel can be
// auto-focused on map - see the note in commit_surface. (satoru)
void keyboard_enter(Client* c, uint32_t kbd, uint32_t surf);
void keyboard_leave(Client* c, uint32_t kbd, uint32_t surf);
Object* find_subsurface_role(Client* c, uint32_t surf_id);   // (satoru)
void set_keyboard_focus(Client* c, uint32_t sid);           // (satoru)
bool has_toplevel_role(Client* c, uint32_t sid);            // (satoru)

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
    // the wm resized the window (drag, maximize, snap, full screen): tell the
    // client its new content size (audit P3). not when the change is just the
    // wm following the client's own size (tl_last), so this cannot loop. states
    // stay ACTIVATED only: advertising MAXIMIZED once broke firefox's bridge. (satoru)
    if (surf->tl_cfg_w > 0 && win->content_w > 0 && win->content_h > 0 &&
        (win->content_w != surf->tl_cfg_w || win->content_h != surf->tl_cfg_h) &&
        (win->content_w != surf->tl_last_w || win->content_h != surf->tl_last_h)) {
        surf->tl_cfg_w = win->content_w; surf->tl_cfg_h = win->content_h;
        for (int i = 0; i < c->object_high; i++) {
            Object& t = c->objects[i];
            if (!t.in_use || t.iface != XDG_TOPLEVEL || t.parent_surface_id != surf->id) continue;
            uint8_t cfg[16];
            put32(cfg + 0, (uint32_t)win->content_w);
            put32(cfg + 4, (uint32_t)win->content_h);
            put32(cfg + 8, 4); put32(cfg + 12, 4);   // states: ACTIVATED
            send_event(c, t.id, 0, cfg, 16);
            if (t.popup_xs && find_obj(c, t.popup_xs)) {
                uint8_t srl[4]; put32(srl, c->serial_next++);
                send_event(c, t.popup_xs, 0, srl, 4);
            }
            tx_flush(c);   // a fatal flush is dropped by the dispatch loop (satoru)
            break;
        }
    }
    blit_surface_region(c, surf, win, /*use_damage=*/false,
                        -surf->geo_x, -surf->geo_y, g_wl_childalpha);
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
    if (is_app_id && surf->app_icon_hint == 0xFF) {
        // kurono's own ring-3 apps (kui) name themselves by app id: give them
        // their dock/titlebar logo. exact names only - a guest app id such as
        // "foot" would hit IdForName's one-letter fallbacks. (satoru)
        static const char* const kKuiIds[] = { "calculator", "texteditor", "files", "terminal",
                                               "taskmgr", "taskmanager", "settings", "mediaplayer" };
        for (unsigned k = 0; k < sizeof(kKuiIds) / sizeof(kKuiIds[0]); k++) {
            const char* a = kKuiIds[k]; int q = 0;
            while (a[q] && buf[q] == a[q]) q++;
            if (a[q] || buf[q]) continue;
            char cap[16]; int m = 0;
            for (; a[m] && m < 15; m++) cap[m] = a[m];
            cap[m] = 0;
            cap[0] = (char)(cap[0] - 'a' + 'A');
            surf->app_icon_hint = (uint8_t)AppIcons::IdForName(cap);
            break;
        }
    }
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
            if (surf->app_icon_hint != 0xFF)
                WindowManager::SetAppIcon(wid, (int)surf->app_icon_hint);
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
    // the wm size is the OUTER size: titlebar and border come on top of the
    // client's content, else they ate its bottom 37 px (a gtk dialog lost its
    // buttons, firefox its status line). capped to the desktop. -1,-1 lets the
    // wm centre the window; a dialog (set_parent) is centred over its parent. (satoru)
    int ow = w + 2 * WM_BORDER_W, oh = h + WM_TITLEBAR_H + WM_BORDER_W;
    int sw = WindowManager::GetScreenWidth(), dh = WindowManager::GetDesktopH();
    if (sw > 0 && ow > sw) ow = sw;
    if (dh > 0 && oh > dh) oh = dh;
    int ox = -1, oy = -1;
    if (surf->tl_parent_sid) {
        Object* ps = find_obj(c, surf->tl_parent_sid);
        Window* pw = (ps && ps->wm_window) ? WindowManager::GetWindow((int)(intptr_t)ps->wm_window) : nullptr;
        if (pw) {
            ox = pw->x + (pw->w - ow) / 2;
            oy = pw->y + (pw->h - oh) / 2;
            if (ox < 0) ox = 0;
            if (oy < WindowManager::GetDesktopY()) oy = WindowManager::GetDesktopY();
        }
    }
    surf->tl_last_w = w; surf->tl_last_h = h;
    // a dialog that sized itself (0x0 configure) bigger than the desktop - gtk's
    // file chooser picked 1124x859 on a 1024x768 screen - gets one configure
    // capped to the desktop content area and re-lays out to fit. (satoru)
    {
        int maxw = (sw > 0 ? sw : ow) - 2 * WM_BORDER_W;
        int maxh = (dh > 0 ? dh : oh) - WM_TITLEBAR_H - WM_BORDER_W;
        if (surf->tl_parent_sid && (w > maxw || h > maxh) && maxw > 0 && maxh > 0) {   // dialogs only (satoru)
            for (int i = 0; i < c->object_high; i++) {
                Object& t = c->objects[i];
                if (!t.in_use || t.iface != XDG_TOPLEVEL || t.parent_surface_id != surf->id) continue;
                uint8_t cfg[16];
                put32(cfg + 0, (uint32_t)(w > maxw ? maxw : w));
                put32(cfg + 4, (uint32_t)(h > maxh ? maxh : h));
                put32(cfg + 8, 4); put32(cfg + 12, 4);   // states: ACTIVATED
                send_event(c, t.id, 0, cfg, 16);
                if (t.popup_xs && find_obj(c, t.popup_xs)) {
                    uint8_t srl[4]; put32(srl, c->serial_next++);
                    send_event(c, t.popup_xs, 0, srl, 4);
                }
                break;
            }
        }
    }
    Window* win = WindowManager::CreateWindow(title, ox, oy, ow, oh);
    if (!win) return -1;
    SerialLogger::Log("[wl] window "); SerialLogger::LogDec(win->id);
    SerialLogger::Log(" '"); SerialLogger::Log(title); SerialLogger::Log("' ");
    SerialLogger::LogDec(w); SerialLogger::Log("x"); SerialLogger::LogDec(h);
    SerialLogger::Log("\r\n");
    // pin the app logo resolved from set_app_id/set_title so later title
    // changes keep it. (satoru)
    if (surf->app_icon_hint != 0xFF)
        WindowManager::SetAppIcon(win->id, (int)surf->app_icon_hint);
    // install the content render callback so wm repaints keep the client
    // pixels, and remember which surface owns this window. (satoru)
    win->render = wl_render_thunk;
    win->input  = wl_input_thunk;      // scroll wheel -> wl_pointer.axis (satoru)
    surf->wm_window = (void*)(intptr_t)win->id;
    surf->mapped = true;
    winmap_set(win->id, c->sd, surf->id);
    // the content rect the window starts with: only a later wm resize (drag,
    // maximize, snap, full screen) sends a configure, see wl_render_thunk. (satoru)
    surf->tl_cfg_w = win->content_w; surf->tl_cfg_h = win->content_h;
    return win->id;
}

// blit `bw x bh` argb pixels from `src` (row stride `stride` bytes) into
// the window content rect at (dst_x,dst_y), clipped to (clip_*). only the
// intersection with the damage rect is touched. alpha is the per-surface
// alpha (255 = use source as-is). writes into the graphics back buffer in
// its native layout, then marks the touched region dirty. (satoru)
// premultiplied ARGB over an opaque destination: the wayland spec says shm
// argb8888 pixels are premultiplied, so out = src + dst*(1-a) (NOT src*a). (satoru)
static inline uint32_t premul_over(uint32_t s, uint32_t d, uint32_t a) {
    uint32_t inv = 255u - a;
    uint32_t r = ((s >> 16) & 0xFFu) + ((((d >> 16) & 0xFFu) * inv + 127u) / 255u);
    uint32_t g = ((s >> 8)  & 0xFFu) + ((((d >> 8)  & 0xFFu) * inv + 127u) / 255u);
    uint32_t b = ( s        & 0xFFu) + ((( d        & 0xFFu) * inv + 127u) / 255u);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// blit `bw x bh` argb pixels from `src` (row stride `stride` bytes) into
// the window content rect at (dst_x,dst_y), clipped to (clip_*). only the
// intersection with the damage rect is touched. alpha is the per-surface
// alpha (255 = use source as-is). src_premul enables the wayland ARGB rules
// (premultiplied per-pixel alpha; the (opx,opy,opw,oph) rect in source coords
// is fully opaque and skips the blend). xrgb8888 is always opaque. (satoru)
void blit_argb(const uint8_t* src, uint32_t stride, int bw, int bh,
               uint32_t fmt, int dst_x, int dst_y,
               int clip_x, int clip_y, int clip_w, int clip_h,
               uint8_t surf_alpha, bool src_premul,
               int opx, int opy, int opw, int oph) {
    if (!src || bw <= 0 || bh <= 0) return;
    // the damage scissor (kurono.gfx.scissor=1) bounds the clip too; without a
    // compositor pass this is the whole screen and changes nothing. (satoru)
    {
        int bx0, by0, bx1, by1;
        Graphics::ScissorBounds(bx0, by0, bx1, by1);
        int cx1 = clip_x + clip_w, cy1 = clip_y + clip_h;
        if (clip_x < bx0) clip_x = bx0;
        if (clip_y < by0) clip_y = by0;
        if (cx1 > bx1) cx1 = bx1;
        if (cy1 > by1) cy1 = by1;
        clip_w = cx1 - clip_x;
        clip_h = cy1 - clip_y;
        if (clip_w <= 0 || clip_h <= 0) return;
    }
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
                if (!src_premul) {
                    if (surf_alpha < 255) native = Graphics::BlendColors(native, Graphics::ReadPixel(px, py), surf_alpha);
                    Graphics::DrawPixel(px, py, native);
                    continue;
                }
                if (fmt != WL_SHM_ARGB8888) {
                    if (surf_alpha < 255) native = Graphics::BlendColors(native, Graphics::ReadPixel(px, py), surf_alpha);
                    Graphics::DrawPixel(px, py, native);
                    continue;
                }
                bool opaque = (opw > 0 && oph > 0 &&
                               col >= opx && col < opx + opw && row >= opy && row < opy + oph);
                uint32_t a = (native >> 24) & 0xFFu;
                if (surf_alpha < 255) {
                    uint32_t sa = surf_alpha;
                    native = ((a * sa / 255u) << 24) |
                             ((((native >> 16) & 0xFFu) * sa / 255u) << 16) |
                             ((((native >> 8) & 0xFFu) * sa / 255u) << 8) |
                             ((native & 0xFFu) * sa / 255u);
                    a = a * sa / 255u;
                }
                if (opaque || a == 255) { Graphics::DrawPixel(px, py, native); continue; }
                if (a == 0) continue;
                Graphics::DrawPixel(px, py, premul_over(native, Graphics::ReadPixel(px, py), a));
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
            if (!src_premul) {
                // default path: per-window opacity modulates the client pixels
                // exactly like the per-pixel path below does. (satoru)
                if (surf_alpha < 255) *d = Graphics::BlendColors(native, *d, surf_alpha);
                else                  *d = native;
                continue;
            }
            if (fmt != WL_SHM_ARGB8888) {
                // childalpha flag on, opaque format: still honour the window
                // opacity (parity with the per-pixel path). (satoru)
                if (surf_alpha < 255) *d = Graphics::BlendColors(native, *d, surf_alpha);
                else                  *d = native;
                continue;
            }
            bool opaque = (opw > 0 && oph > 0 &&
                           col >= opx && col < opx + opw && row >= opy && row < opy + oph);
            uint32_t a = (native >> 24) & 0xFFu;
            if (surf_alpha < 255) {
                uint32_t sa = surf_alpha;
                native = ((a * sa / 255u) << 24) |
                         ((((native >> 16) & 0xFFu) * sa / 255u) << 16) |
                         ((((native >> 8) & 0xFFu) * sa / 255u) << 8) |
                         ((native & 0xFFu) * sa / 255u);
                a = a * sa / 255u;
            }
            if (opaque || a == 255) { *d = native; continue; }
            if (a == 0) continue;
            *d = premul_over(native, *d, a);
        }
    }
}

// blit a surface's attached buffer into its window content rect. when
// use_damage is true, only the surface's accumulated damage rect is
// painted (incremental commit path); otherwise the whole buffer is painted
// (full wm repaint path). returns true if any pixels were written. (satoru)
void blit_surface_region(Client* c, Object* surf, Window* win, bool use_damage,
                         int ox, int oy, bool src_alpha) {
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
        // F6: a dropped client's pools are unpinned (kurono.wl.poolfree), so its backing
        // may be gone. with no filled shadow there is nothing safe to read: skip it.
        // (this runs on the pump thread, the same one that drops clients.) (satoru)
        if (g_wl_poolfree && !c->in_use) return;
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
    if (g_wltrace) {
        static int s_bl = 0;
        if (s_bl < 24) {
            s_bl++;
            SerialLogger::Log("[wld] win="); SerialLogger::LogDec(win->id);
            SerialLogger::Log(" surf="); SerialLogger::LogDec((int)surf->id);
            SerialLogger::Log(" cr="); SerialLogger::LogDec(cx); SerialLogger::Log(","); SerialLogger::LogDec(cy);
            SerialLogger::Log(","); SerialLogger::LogDec(cw); SerialLogger::Log(","); SerialLogger::LogDec(ch);
            SerialLogger::Log(" bw="); SerialLogger::LogDec(bw); SerialLogger::Log("x"); SerialLogger::LogDec(bh);
            SerialLogger::Log(" sh="); SerialLogger::LogDec(surf->shadow_filled ? 1 : 0);
            SerialLogger::Log(" dam="); SerialLogger::LogDec(use_damage ? 1 : 0);
            SerialLogger::Log("\r\n");
        }
    }

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
    // kurono.gfx.scissor=1, outside a compositor pass (a commit): mark the damage and
    // let the window's render thunk blit it inside the pass, in stacking order. a blit
    // here went straight into the back buffer over any window stacked above this one;
    // a frame repaints that, but a move-by-copy of the window on top first carried the
    // foreign pixels to its new place, where nothing repaints them. (satoru)
    if (Graphics::ScissorEnabled() && !g_wl_scdirect) {
        int gx, gy, gw, gh;
        if (!Graphics::GetScissor(gx, gy, gw, gh)) {
            int x0 = cx + ox + dx, y0 = cy + oy + dy, x1 = x0 + dw, y1 = y0 + dh;
            if (x0 < cx) x0 = cx;
            if (y0 < cy) y0 = cy;
            if (x1 > cx + cw) x1 = cx + cw;
            if (y1 > cy + ch) y1 = cy + ch;
            if (x1 > x0 && y1 > y0) Graphics::MarkDirty(x0, y0, x1 - x0, y1 - y0);
            return;
        }
    }

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
    // the opaque region is in buffer coords; shift it into the blit's source
    // coords (the src already starts at the damage rect's dx,dy). only argb
    // surfaces use it. (satoru)
    int opx = 0, opy = 0, opw = 0, oph = 0;
    if (src_alpha && sfmt == WL_SHM_ARGB8888 && surf->opq_valid) {
        opx = surf->opq_x - dx; opy = surf->opq_y - dy;
        opw = surf->opq_w; oph = surf->opq_h;
    }
    blit_argb(src, sstride, dw, dh, sfmt,
              cx + ox + dx, cy + oy + dy,
              cx, cy, cw, ch, (uint8_t)eff, src_alpha, opx, opy, opw, oph);
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
// true while `surf` holds a live xdg_popup role. (satoru)
bool popup_live(Client* c, Object* surf) {
    if (!surf || !surf->popup_obj) return false;
    Object* po = find_obj(c, surf->popup_obj);
    return po && po->iface == XDG_POPUP;
}

// true when an xdg_toplevel gives `sid` the toplevel role. only such surfaces
// own a wm window; a role-less surface (a gtk cursor surface, a dnd icon) has
// nothing to show. (satoru)
bool has_toplevel_role(Client* c, uint32_t sid) {
    if (sid == 0) return false;
    for (int i = 0; i < c->object_high; i++) {
        Object& o = c->objects[i];
        if (o.in_use && o.iface == XDG_TOPLEVEL && o.parent_surface_id == sid) return true;
    }
    return false;
}

// one step up the surface tree: the parent wl_surface of a subsurface or a
// popup, and this surface's BUFFER origin in the parent's buffer coords. a
// popup's configured x/y is relative to the parent's window geometry and its
// own buffer starts geo_x/geo_y before its geometry. (satoru)
Object* surface_tree_parent(Client* c, Object* surf, int& bx, int& by, bool& is_popup) {
    is_popup = false;
    Object* role = find_subsurface_role(c, surf->id);
    if (role) {
        Object* p = find_obj(c, role->subsurface_parent);
        if (!p || p->iface != WL_SURFACE || p == surf) return nullptr;
        bx = role->subsurface_x; by = role->subsurface_y;
        return p;
    }
    if (popup_live(c, surf)) {
        Object* p = find_obj(c, surf->popup_parent_sid);
        if (!p || p->iface != WL_SURFACE || p == surf) return nullptr;
        bx = p->geo_x + surf->popup_x - surf->geo_x;
        by = p->geo_y + surf->popup_y - surf->geo_y;
        is_popup = true;
        return p;
    }
    return nullptr;
}

// walk up to the surface that owns the wm window. ox/oy = content coords of
// surf's buffer origin in that window; in_popup = the chain crosses a popup.
// null when the chain is not (yet) mapped. tooltips are a subsurface of the
// toplevel with gecko's own subsurface inside, so the walk must be deeper than
// one level - the old one-level lookup gave that inner surface a stray window
// of its own (the laptop's "random extra window", 288x58 'wayland sd N'). (satoru)
Object* surface_tree_root(Client* c, Object* surf, int& ox, int& oy, bool& in_popup) {
    int ax = 0, ay = 0;
    in_popup = false;
    Object* s = surf;
    for (int d = 0; d < 8 && s; d++) {
        if (s->wm_window) { ox = ax - s->geo_x; oy = ay - s->geo_y; return s; }
        int bx = 0, by = 0; bool pp = false;
        Object* p = surface_tree_parent(c, s, bx, by, pp);
        if (!p) return nullptr;
        if (pp) in_popup = true;
        ax += bx; ay += by;
        s = p;
    }
    return nullptr;
}

// the tree changed shape (a popup or subsurface went away): repaint the window
// it was composited onto, or its pixels stay on screen. (satoru)
void mark_tree_root_dirty(Client* c, Object* surf) {
    if (!surf) return;
    int ox, oy; bool ip;
    Object* root = surface_tree_root(c, surf, ox, oy, ip);
    if (root && root->wm_window) WindowManager::MarkDirty((int)(intptr_t)root->wm_window);
}

// composite everything stacked on `parent` (whose buffer origin sits at content
// coords pox/poy): its subsurfaces, each with its own subtree, then its live
// popups oldest first, so a popup is above the whole tree it belongs to. (satoru)
void blit_surface_children(Client* c, Object* parent, Window* win, int pox, int poy, int depth) {
    if (depth > 6) return;
    for (int i = 0; i < c->object_high; i++) {
        Object& ss = c->objects[i];
        if (!ss.in_use || ss.iface != WL_SUBSURFACE) continue;
        if (ss.subsurface_parent != parent->id) continue;
        Object* child = find_obj(c, ss.parent_surface_id);
        if (!child || child == parent) continue;
        int ox = pox + ss.subsurface_x, oy = poy + ss.subsurface_y;
        // a filled shadow is displayable even when the client has since detached
        // or DESTROYED the committed wl_buffer (gtk destroys released buffers). (satoru)
        if (child->pending_buffer_id != 0 || (child->shadow && child->shadow_filled))
            blit_surface_region(c, child, win, /*use_damage=*/false, ox, oy, g_wl_childalpha);
        blit_surface_children(c, child, win, ox, oy, depth + 1);
    }
    uint32_t last = 0;
    for (int guard = 0; guard < 16; guard++) {
        Object* best = nullptr;
        for (int i = 0; i < c->object_high; i++) {
            Object& o = c->objects[i];
            if (!o.in_use || o.iface != WL_SURFACE || !o.popup_obj) continue;
            if (o.popup_parent_sid != parent->id || o.popup_seq <= last) continue;
            if (best && o.popup_seq >= best->popup_seq) continue;
            if (!popup_live(c, &o)) continue;
            if (find_subsurface_role(c, o.id)) continue;   // drawn as a subsurface already (satoru)
            best = &o;
        }
        if (!best) break;
        last = best->popup_seq;
        int ox = pox + parent->geo_x + best->popup_x - best->geo_x;
        int oy = poy + parent->geo_y + best->popup_y - best->geo_y;
        if (best->shadow && best->shadow_filled)
            blit_surface_region(c, best, win, /*use_damage=*/false, ox, oy, g_wl_childalpha);
        blit_surface_children(c, best, win, ox, oy, depth + 1);
    }
}

void blit_child_subsurfaces(Client* c, Object* parent_surf, Window* pwin) {
    if (!parent_surf || !pwin) return;
    blit_surface_children(c, parent_surf, pwin, -parent_surf->geo_x, -parent_surf->geo_y, 0);
}

// a child surface that committed before its parent mapped got a temporary
// own-window. until now it re-homed only on the child's NEXT commit; a child
// that never commits again (a static tooltip or a content surface waiting for
// a frame callback) kept a stray 'wayland sd N' window. now that the parent is
// mapped, close every temp window whose tree root is this surface; the render
// pass composites those children onto the parent. (satoru)
void rehome_temp_children(Client* c, Object* root_surf) {
    if (!c || !root_surf || !root_surf->wm_window) return;
    int closed = 0;
    for (int i = 0; i < c->object_high; i++) {
        Object& ch = c->objects[i];
        if (!ch.in_use || ch.iface != WL_SURFACE || ch.id == root_surf->id || !ch.wm_window)
            continue;
        int ox = 0, oy = 0; bool ip = false;
        if (surface_tree_root(c, &ch, ox, oy, ip) != root_surf) continue;
        int wid = (int)(intptr_t)ch.wm_window;
        WindowManager::CloseWindow(wid);
        winmap_clear(wid);
        ch.wm_window = nullptr;
        closed++;
        static int s_logs = 0;
        if (s_logs < 8) {
            s_logs++;
            SerialLogger::Log("[wl] rehome child sid="); SerialLogger::LogDec((int)ch.id);
            SerialLogger::Log(" into window "); SerialLogger::LogDec((int)(intptr_t)root_surf->wm_window);
            SerialLogger::Log("\r\n");
        }
    }
    if (closed) WindowManager::MarkDirty((int)(intptr_t)root_surf->wm_window);
}

// xdg_positioner placement on one axis. side -1 = left/top, 1 = right/bottom,
// 0 = centre, for both the anchor point on the anchor rect and the direction
// the popup grows from it (gravity). (satoru)
int popup_axis_pos(int a0, int alen, int aside, int gside, int off, int len) {
    int pt = aside < 0 ? a0 : (aside > 0 ? a0 + alen : a0 + alen / 2);
    int p  = gside < 0 ? pt - len : (gside > 0 ? pt : pt - len / 2);
    return p + off;
}

// place, then apply the constraint adjustments in the order the protocol
// gives them (flip, slide, resize) against [lo, hi). hi <= lo = no bounds. (satoru)
void popup_axis(int a0, int alen, int aside, int gside, int off, int lo, int hi,
                bool flip, bool slide, bool resize, int& pos, int& len) {
    pos = popup_axis_pos(a0, alen, aside, gside, off, len);
    if (hi <= lo) return;
    if (pos >= lo && pos + len <= hi) return;
    if (flip) {
        int fp = popup_axis_pos(a0, alen, -aside, -gside, -off, len);
        if (fp >= lo && fp + len <= hi) { pos = fp; return; }
    }
    if (slide) {
        if (pos + len > hi) pos = hi - len;
        if (pos < lo) pos = lo;
    }
    if (resize) {
        if (pos < lo) { len -= lo - pos; pos = lo; }
        if (pos + len > hi) len = hi - pos;
        if (len < 1) len = 1;
    }
}

// anchor / gravity enum -> per-axis side: none 0, top 1, bottom 2, left 3,
// right 4, top_left 5, bottom_left 6, top_right 7, bottom_right 8. (satoru)
inline int xdg_side_x(uint32_t e) { return (e == 3 || e == 5 || e == 6) ? -1 : (e == 4 || e == 7 || e == 8) ? 1 : 0; }
inline int xdg_side_y(uint32_t e) { return (e == 1 || e == 5 || e == 7) ? -1 : (e == 2 || e == 6 || e == 8) ? 1 : 0; }

void wlpop_log(const char* what, Object* s, int a, int b, int c2, int d);   // (satoru)

// compute the popup's geometry from a positioner. the compositor's
// constraint area is the root window's content: a popup is composited onto
// that window, so anything outside it would be clipped away. (satoru)
void popup_place(Client* c, Object* psurf, Object* pos, Object* parent) {
    int w = pos->pos_w > 0 ? pos->pos_w : 1;
    int h = pos->pos_h > 0 ? pos->pos_h : 1;
    int lox = 0, hix = 0, loy = 0, hiy = 0;
    int pox = 0, poy = 0; bool ip = false;
    Object* root = surface_tree_root(c, parent, pox, poy, ip);
    if (root && root->wm_window) {
        Window* rw = WindowManager::GetWindow((int)(intptr_t)root->wm_window);
        int W = root->geo_w > 0 ? root->geo_w : (rw ? rw->content_w : 0);
        int H = root->geo_h > 0 ? root->geo_h : (rw ? rw->content_h : 0);
        // the parent's window-geometry origin in root content coords. (satoru)
        int gx = pox + parent->geo_x, gy = poy + parent->geo_y;
        lox = -gx; hix = W - gx; loy = -gy; hiy = H - gy;
    }
    uint32_t ca = pos->pos_cadj;
    int x = 0, y = 0;
    popup_axis(pos->pos_ax, pos->pos_aw, xdg_side_x(pos->pos_anchor), xdg_side_x(pos->pos_gravity),
               pos->pos_ox, lox, hix, (ca & 4) != 0, (ca & 1) != 0, (ca & 16) != 0, x, w);
    popup_axis(pos->pos_ay, pos->pos_ah, xdg_side_y(pos->pos_anchor), xdg_side_y(pos->pos_gravity),
               pos->pos_oy, loy, hiy, (ca & 8) != 0, (ca & 2) != 0, (ca & 32) != 0, y, h);
    psurf->popup_x = x; psurf->popup_y = y;
    psurf->popup_w = w; psurf->popup_h = h;
    wlpop_log("place", psurf, x, y, w, h);
}

// bounded popup log: where each popup was placed, first 32 a boot. (satoru)
int g_wlpop_logs = 0;
void wlpop_log(const char* what, Object* s, int a, int b, int c2, int d) {
    if (g_wlpop_logs >= 32 || !s) return;
    g_wlpop_logs++;
    SerialLogger::Log("[wlpop] "); SerialLogger::Log(what);
    SerialLogger::Log(" sid="); SerialLogger::LogDec((int)s->id);
    SerialLogger::Log(" "); SerialLogger::LogDec(a); SerialLogger::Log(",");
    SerialLogger::LogDec(b); SerialLogger::Log(" "); SerialLogger::LogDec(c2);
    SerialLogger::Log("x"); SerialLogger::LogDec(d);
    SerialLogger::Log(" geo="); SerialLogger::LogDec(s->geo_x); SerialLogger::Log(",");
    SerialLogger::LogDec(s->geo_y); SerialLogger::Log(" ");
    SerialLogger::LogDec(s->geo_w); SerialLogger::Log("x"); SerialLogger::LogDec(s->geo_h);
    SerialLogger::Log("\r\n");
}

// xdg_popup.configure (+ repositioned for a reposition request) and the
// closing xdg_surface.configure. (satoru)
void popup_send_configure(Client* c, Object* popup, Object* psurf, bool repos, uint32_t token) {
    if (repos) { uint8_t tk[4]; put32(tk, token); send_event(c, popup->id, 2, tk, 4); }
    uint8_t pc[16];
    put32(pc + 0,  (uint32_t)psurf->popup_x);
    put32(pc + 4,  (uint32_t)psurf->popup_y);
    put32(pc + 8,  (uint32_t)psurf->popup_w);
    put32(pc + 12, (uint32_t)psurf->popup_h);
    send_event(c, popup->id, 0, pc, 16);
    if (popup->popup_xs && find_obj(c, popup->popup_xs)) {
        uint8_t srl[4]; put32(srl, c->serial_next++);
        send_event(c, popup->popup_xs, 0, srl, 4);
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
    const uint8_t* base = buf->buf_base;
    uint32_t psize = buf->buf_base_size;
    if (!base) {
        Object* pool = find_obj(c, buf->buf_pool_id);   // pre-create_buffer path (satoru)
        base = (pool && pool->iface == WL_SHM_POOL) ? pool->pool_base : nullptr;
        psize = pool ? pool->pool_size : 0;
    }
    if (!base) return;                          // no mapping yet - nothing to copy (satoru)

    // (re)allocate on first use or resize; a resize invalidates old content. (satoru)
    if (!surf->shadow || surf->shadow_w != bw || surf->shadow_h != bh) {
        shadow_release(*surf);
        surf->shadow_filled = false;
        uint64_t need = (uint64_t)bw * (uint64_t)bh * 4u;
        if ((uint64_t)bw * (uint64_t)bh > WL_MAX_BUF_PX) return;
        if (g_shadow_bytes + need > WL_MAX_SHADOW_TOTAL) {
            static int s_budget_logs = 0;
            if (s_budget_logs < 4) {
                s_budget_logs++;
                SerialLogger::Log("[wl] shadow budget full, surface not shown\r\n");
            }
            return;
        }
        surf->shadow = (uint8_t*)KernelHeap::Alloc((size_t)need);
        if (!surf->shadow) return;
        surf->shadow_w = bw; surf->shadow_h = bh;
        g_shadow_bytes += need;
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
    bool fresh = surf->attach_pending;
    surf->attach_pending = false;
    if (!fresh) {
        // no attach since the last commit: the content did not change, and the
        // released buffer is the client's again. nothing to read. a temporary
        // own-window still re-homes once its parent has mapped. (satoru)
        if (surf->wm_window) {
            Object* hr = find_subsurface_role(c, surf->id);
            Object* hp = hr ? find_obj(c, hr->subsurface_parent) : nullptr;
            if (hp && hp != surf && hp->wm_window) {
                int twid = (int)(intptr_t)surf->wm_window;
                WindowManager::CloseWindow(twid);
                winmap_clear(twid);
                surf->wm_window = nullptr;
                WindowManager::MarkDirty((int)(intptr_t)hp->wm_window);
            }
        }
        surf->damage.valid = false;
        return;
    }
    if (bid == 0) {
        // attach(null) + commit unmaps the surface. for a subsurface or popup
        // drop its picture and repaint what it covered; a toplevel keeps the
        // old behaviour (its window goes with the xdg_toplevel). (satoru)
        if (!surf->wm_window && surf->shadow) {
            mark_tree_root_dirty(c, surf);
            shadow_release(*surf);
            surf->shadow_filled = false;
            mark_tree_root_dirty(c, surf);
        }
        surf->damage.valid = false;
        return;
    }
    Object* buf = find_obj(c, bid);
    if (!buf || buf->iface != WL_BUFFER) return;
    int bw = buf->width;
    int bh = buf->height;
    if (bw <= 0 || bh <= 0) return;
    // accumulate this commit's pixels into the shadow BEFORE any blit - all
    // blits below (and later wm repaints) read the shadow. (satoru)
    update_surface_shadow(c, surf, buf);
    if (g_wltrace) {
        static int s_cm = 0;
        if (s_cm < 24) {
            s_cm++;
            uint32_t px0 = 0xFFFFFFFFu, pxc = 0xFFFFFFFFu;
            const uint8_t* b0 = buf->buf_base ? buf->buf_base + buf->buf_offset : nullptr;
            if (b0) { px0 = *(const uint32_t*)b0;
                      pxc = *(const uint32_t*)(b0 + (uint64_t)(buf->height / 2) * buf->buf_stride + (uint64_t)(buf->width / 2) * 4u); }
            SerialLogger::Log("[wlc] surf="); SerialLogger::LogDec((int)surf->id);
            SerialLogger::Log(" buf="); SerialLogger::LogDec((int)buf->id);
            SerialLogger::Log(" dam="); SerialLogger::LogDec(surf->damage.valid ? 1 : 0);
            SerialLogger::Log(" filled="); SerialLogger::LogDec(surf->shadow_filled ? 1 : 0);
            SerialLogger::Log(" px0="); SerialLogger::LogHex(px0);
            SerialLogger::Log(" pxc="); SerialLogger::LogHex(pxc);
            if (surf->shadow && surf->shadow_w > 1 && surf->shadow_h > 1) {
                const uint32_t* sh = (const uint32_t*)surf->shadow;
                SerialLogger::Log(" shc="); SerialLogger::LogHex(sh[(surf->shadow_h/2)*(surf->shadow_w) + surf->shadow_w/2]);
                SerialLogger::Log(" shw="); SerialLogger::LogDec(surf->shadow_w);
                SerialLogger::Log(" shh="); SerialLogger::LogDec(surf->shadow_h);
            }
            SerialLogger::Log(" geo="); SerialLogger::LogDec(surf->geo_x); SerialLogger::Log(","); SerialLogger::LogDec(surf->geo_y);
            SerialLogger::Log(","); SerialLogger::LogDec(surf->geo_w); SerialLogger::Log(","); SerialLogger::LogDec(surf->geo_h);
            SerialLogger::Log("\r\n");
        }
    }
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
                // re-homed: drop the temporary own-window (stray dock entry). (satoru)
                if (surf->wm_window) {
                    int twid = (int)(intptr_t)surf->wm_window;
                    WindowManager::CloseWindow(twid);
                    winmap_clear(twid);
                    surf->wm_window = nullptr;
                }
                // subsurface offsets are parent-SURFACE coords; the parent's
                // geometry crop shifts them into content coords. (satoru)
                blit_surface_region(c, surf, pwin, /*use_damage=*/true,
                                    role->subsurface_x - psurf->geo_x,
                                    role->subsurface_y - psurf->geo_y, g_wl_childalpha);
                {   // census marker: the first real content commit (gecko's content
                    // subsurface composited onto its toplevel). KWPAINT alone also fires
                    // for the empty csd frame of a window that stays black. (satoru)
                    static int s_kwcontent = 0;
                    static Client* s_kwc_client = nullptr;
                    if (s_kwc_client != c) { s_kwc_client = c; s_kwcontent = 0; }
                    if (s_kwcontent < 2 && surf->shadow_filled) {
                        s_kwcontent++;
                        SerialLogger::Log("[KWCONTENT] sid="); SerialLogger::LogDec((int)surf->id);
                        SerialLogger::Log(" "); SerialLogger::LogDec(bw);
                        SerialLogger::Log("x"); SerialLogger::LogDec(bh);
                        SerialLogger::Log("\r\n");
                    }
                }
                WindowManager::MarkDirty(pwid);
                surf->damage.valid = false;
                return;
            }
        }
        // unreachable: a subsurface/popup commit never bridges a window, the
        // role/popup block below keeps its shadow until the tree has a root. (satoru)
    }
    // deeper trees (gecko's surface inside a tooltip subsurface) and popups
    // (menus): the shadow already holds the pixels, so repaint the window the
    // tree is composited onto; the render thunk walks the whole tree. never a
    // window of their own - that was the stray 'wayland sd N' window, and the
    // old direct-child temporary window left one behind when the child never
    // committed again. an unmapped tree keeps its shadows and waits. (satoru)
    if (role || popup_live(c, surf)) {
        int tox = 0, toy = 0; bool tip = false;
        Object* troot = surface_tree_root(c, surf, tox, toy, tip);
        if (troot && troot != surf && troot->wm_window) {
            WindowManager::MarkDirty((int)(intptr_t)troot->wm_window);
            surf->damage.valid = false;
            return;
        }
        // no window in the tree yet (the parent toplevel has not mapped):
        // keep the shadow and wait for the parent's commit to composite the
        // whole tree. the old fallback gave this child a temporary own-window,
        // which is exactly the stray 'wayland sd N' window when the child
        // never committed again. never create one for a subsurface/popup. (satoru)
        surf->damage.valid = false;
        return;
    } else if (!surf->wm_window && !has_toplevel_role(c, surf->id)) {
        // a role-less surface (gtk's cursor surface, a drag icon) is never
        // shown as a window. (satoru)
        surf->damage.valid = false;
        return;
    }

    // ensure the surface owns an on-screen rect. (satoru)
    bool newly_bridged = (surf->wm_window == nullptr);
    int wid = bridge_surface_window(c, surf, bw, bh);
    if (wid < 0) return;
    Window* win = WindowManager::GetWindow(wid);
    if (!win) { surf->wm_window = nullptr; return; }
    // the client resized itself (a gtk dialog settling on its natural size):
    // follow it. only on a CHANGE of the client's size, so a wm maximize of a
    // client that keeps its size is not undone on the next frame. (satoru)
    if (!newly_bridged && win->state == WIN_NORMAL) {
        int cw = surf->geo_w > 0 ? surf->geo_w : bw;
        int ch = surf->geo_h > 0 ? surf->geo_h : bh;
        if (cw != surf->tl_last_w || ch != surf->tl_last_h) {
            surf->tl_last_w = cw; surf->tl_last_h = ch;
            surf->tl_cfg_w = cw; surf->tl_cfg_h = ch;
            int ow = cw + 2 * WM_BORDER_W, oh = ch + WM_TITLEBAR_H + WM_BORDER_W;
            int sw = WindowManager::GetScreenWidth(), dh = WindowManager::GetDesktopH();
            if (sw > 0 && ow > sw) ow = sw;
            if (dh > 0 && oh > dh) oh = dh;
            WindowManager::ResizeWindow(wid, ow, oh);
            int dy = WindowManager::GetDesktopY();
            if (dh > 0 && win->y + win->h > dy + dh) {
                int ny = dy + dh - win->h;
                WindowManager::MoveWindow(wid, win->x, ny < dy ? dy : ny);
            }
        }
    }

    // the parent is mapped now: fold any child that committed early (and got a
    // temporary own-window) back into this window instead of leaving a stray. (satoru)
    rehome_temp_children(c, surf);

    // first-paint serial marker: until now a paint was only provable by a qmp
    // screenshot (KW:NativeShow comes from the firefox image and fires before
    // any pixels hit the compositor). a big toplevel commit reaching here = a
    // real client window with content on screen, so the reliability harness
    // can classify PAINT vs STALL from serial alone. first few commits only. (satoru)
    {
        static int s_kwpaint_lines = 0;
        if (s_kwpaint_lines < 4 && bw > 400 && bh > 300) {
            if (s_kwpaint_lines == 0) {
                // first window painted: the eager disk log (turned on at firefox launch) has
                // done its job, hand back to the 1 s worker. (satoru)
                SerialLogger::Log("[fftl] first window painted, eager disk log OFF\r\n");
                DiskLog::SetEager(false);
            }
            s_kwpaint_lines++;
            UnixSocket::Credentials kcr;
            uint32_t kpid = (UnixSocket::GetPeerCred(c->sd, &kcr) == 0) ? kcr.pid : 0;
            SerialLogger::Log("[KWPAINT] pid="); SerialLogger::LogDec((int)kpid);
            SerialLogger::Log(" "); SerialLogger::LogDec(bw);
            SerialLogger::Log("x"); SerialLogger::LogDec(bh);
            SerialLogger::Log("\r\n");
        }
    }

    // AUTO-FOCUS this toplevel on map. firefox's CONTENT browsing context only
    // becomes active - and thus PAINTS (PresShell::ComputeActiveness gates on
    // bc->IsActive()) - when the window is FOCUSED. a headless boot has no
    // pointer click to focus it, so grant keyboard focus here (paired with the
    // xdg_toplevel ACTIVATED state). without it the chrome paints but the web
    // content viewport stays firefox's blank canvas. fired once per surface,
    // when it is first bridged (newly_bridged): doing it on every commit stole
    // focus back from any other client on each frame and churned leave/enter
    // after every click. every toplevel gets it, including tiny 1x1 helper
    // surfaces: wl-copy creates one and only calls set_selection after its
    // keyboard enter, so skipping small windows broke the guest clipboard. (satoru)
    if (newly_bridged && has_toplevel_role(c, surf->id)) {
        set_keyboard_focus(c, surf->id);
    }

    // incremental blit of the damaged region. if no pool mapping exists yet
    // (fd transport not wired) blit_surface_region is a no-op; we still mark
    // the window dirty so the wm paints its chrome and, once a mapping
    // arrives via RegisterPoolMemory(), the render thunk fills content. the
    // -geo shift crops the csd margins: buffer pixel (geo_x,geo_y) lands at
    // content (0,0). (satoru)
    blit_surface_region(c, surf, win, /*use_damage=*/true,
                        -surf->geo_x, -surf->geo_y, g_wl_childalpha);
    // re-composite any child subsurfaces on top so the parent's fresh (black)
    // buffer doesn't cover content the client committed into a child. (satoru)
    // under kurono.gfx.scissor=1 the blit above only marked the committed damage, and
    // the pass repaints the window (children included) inside it. marking the whole
    // window on every commit made a hover in a big window a full-screen frame (the
    // files window is 65% of a 1024x768 screen) and a 16 mb flip. a first commit, or
    // one without damage, still marks the window. (satoru)
    bool precise = Graphics::ScissorEnabled() && !g_wl_scdirect && !newly_bridged && surf->damage.valid;
    if (precise) {
        WindowManager::InvalidateDragBackdrop();
    } else {
        blit_child_subsurfaces(c, surf, win);
        WindowManager::MarkDirty(wid);
    }
    surf->damage.valid = false;
}

// deliver this surface's pending frame callback right after its commit so
// the client may submit its next frame. timestamp = current pit tick in
// ms (Timer::GetRealMs). the callback id is also scrubbed from the
// per-client ring so DispatchPendingFrames doesn't fire it twice. (satoru)
void deliver_surface_frame(Client* c, Object* surf) {
    // deliver every frame callback this surface requested before this commit,
    // in request order - not just the last one. the old code fired only the
    // newest (pending_frame_cb) and left older ones to the render-loop drain,
    // which fired them later, out of order, with a different clock - and even
    // for surfaces that had not committed. gtk and gecko both hang callbacks on
    // the toplevel: gdk's frame-clock callback and gecko's ReadyToDraw callback
    // (WaylandSurface::MapLocked asks wl_surface_frame(parent)). when gecko's
    // landed late and out of order, its initial-draw handshake never finished
    // and the window stayed black (~50% of boots). (satoru)
    surf->pending_frame_cb = 0;
    uint32_t now = Timer::GetRealMs();
    g_now_ms = now;
    uint8_t pl[4]; put32(pl, now);
    SpinLockGuard g(g_fcb_lock);   // (satoru)
    for (int i = c->frame_cb_tail; i != c->frame_cb_head;
         i = (i + 1) % WL_MAX_FRAME_CB) {
        uint32_t cb = c->frame_cb[i];
        if (cb == 0) continue;
        Object* o = find_obj(c, cb);
        if (!o || o->iface != WL_CALLBACK) { c->frame_cb[i] = 0; continue; }
        if (o->parent_surface_id != surf->id) continue;
        c->frame_cb[i] = 0;
        send_event(c, cb, 0, pl, 4);          // wl_callback::done
        destroy_obj(c, cb);                    // one-shot
    }
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
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use) continue;
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
    // an open popup (menu) of this window is on top of everything in it: the
    // newest popup whose geometry holds the point takes the event, or a
    // subsurface inside it that accepts input. (satoru)
    {
        int cpx = tx - top->geo_x, cpy = ty - top->geo_y;   // root content coords
        Object* hit = nullptr; int hox = 0, hoy = 0;
        for (int i = 0; i < fc->object_high; i++) {
            Object& p = fc->objects[i];
            if (!p.in_use || p.iface != WL_SURFACE || !p.popup_obj) continue;
            if (hit && p.popup_seq < hit->popup_seq) continue;
            if (!popup_live(fc, &p)) continue;
            if (!(p.shadow && p.shadow_filled)) continue;   // not drawn, takes no input (satoru)
            int ox = 0, oy = 0; bool ip = false;
            if (surface_tree_root(fc, &p, ox, oy, ip) != top) continue;
            int rx = cpx - (ox + p.geo_x), ry = cpy - (oy + p.geo_y);
            if (rx < 0 || ry < 0 || rx >= p.popup_w || ry >= p.popup_h) continue;
            hit = &p; hox = ox; hoy = oy;
        }
        if (hit) {
            int bx = cpx - hox, by = cpy - hoy;   // popup buffer coords (satoru)
            lx = bx; ly = by;
            for (int i = 0; i < fc->object_high; i++) {
                Object& ss = fc->objects[i];
                if (!ss.in_use || ss.iface != WL_SUBSURFACE || ss.subsurface_parent != hit->id) continue;
                Object* child = find_obj(fc, ss.parent_surface_id);
                if (!child || child->input_none) continue;
                int cw = 0, ch = 0;
                if (child->shadow && child->shadow_filled) { cw = child->shadow_w; ch = child->shadow_h; }
                else if (child->width > 0)                 { cw = child->width;    ch = child->height;   }
                int rx = bx - ss.subsurface_x, ry = by - ss.subsurface_y;
                if (cw <= 0 || ch <= 0 || rx < 0 || ry < 0 || rx >= cw || ry >= ch) continue;
                lx = rx; ly = ry;
                return child;
            }
            return hit;
        }
    }
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

// the one keyboard focus: (client, toplevel surface). every change sends leave
// to the previous owner first, so keys never reach two clients at once (map
// auto-focus used to grab focus without telling the other client, and
// ForwardKey fed both). focus always names a TOPLEVEL: gdk ignores enter on a
// surface that is not one of its windows (gecko's content subsurface), so a
// leave/enter pair onto such a surface would leave gdk with no focus. (satoru)
void set_keyboard_focus(Client* c, uint32_t sid) {
    for (int i = 0; i < g_ncap; i++) {
        Client* o = (*cl_ref(i));
        if (!o || !o->in_use || o == c || o->keyboard_focus_sid == 0) continue;
        if (find_obj(o, o->keyboard_focus_sid)) kbds_leave(o, o->keyboard_focus_sid);
        o->keyboard_focus_sid = 0;
        // flush only: a failed flush marks it fatal and the dispatch loop drops
        // it. dropping here could free a client whose request is mid-dispatch
        // (a destroy closes its window, the wm refocuses, we land here). (satoru)
        tx_flush(o);
    }
    if (!c || c->keyboard_focus_sid == sid) return;
    if (c->keyboard_focus_sid != 0 && find_obj(c, c->keyboard_focus_sid))
        kbds_leave(c, c->keyboard_focus_sid);
    c->keyboard_focus_sid = sid;
    if (sid) kbds_enter(c, sid);   // a keyboard bound later gets its enter at bind (satoru)
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
        // multi-monitor: the one wl_output is the primary screen, not the whole
        // virtual desktop, so clients size/maximize to a real screen. (satoru)
        int ox = 0, oy = 0;
        if (Outputs::Multi()) {
            const Output* po = Outputs::Get(0);
            if (po && po->connected && po->w > 0 && po->h > 0) { sw = po->w; sh = po->h; ox = po->x; oy = po->y; }
        }
        uint8_t g[80]; int p = 0;
        put32(g + p, (uint32_t)ox);   p += 4;      // x
        put32(g + p, (uint32_t)oy);   p += 4;      // y
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

// ── data device / primary selection ──────────────────────────────────
// a selection owner is asked to write every supported mime into a pipe as
// soon as it takes the selection; the compositor keeps that snapshot in the
// system clipboard and serves every consumer from there. this makes native
// apps, wayland clients and the alpine bridge share one store, and keeps a
// copy alive when its owner exits. (satoru)

constexpr int CLIP_MAX_MIMES    = 8;
constexpr int CLIP_MIME_NAME    = 48;
constexpr int CLIP_MAX_SOURCES  = 64;
constexpr int CLIP_MAX_OFFERS   = 128;
constexpr int CLIP_MAX_PULLS    = 16;
constexpr int CLIP_MAX_WRITES   = 16;
constexpr uint32_t CLIP_PULL_MS = 3000;
constexpr uint32_t CLIP_WRITE_MS = 5000;

struct ClipMimeList {
    int  count;
    char name[CLIP_MAX_MIMES][CLIP_MIME_NAME];
};

struct ClipSourceInfo {
    bool     used;
    int      sd;
    uint32_t id;
    uint16_t iface;
    bool     primary;
    ClipMimeList mimes;
};

struct ClipOfferInfo {
    bool     used;
    int      sd;
    uint32_t id;
    bool     primary;
    uint32_t serial;
    ClipMimeList mimes;
};

struct ClipSelection {
    bool     active;
    bool     system;
    int      sd;
    uint32_t source_id;
    uint16_t source_iface;
    uint32_t serial;
    ClipMimeList mimes;
};

struct ClipPull {
    bool     used;
    int      owner_sd;
    uint32_t source_id;
    uint16_t source_iface;
    bool     primary;
    uint32_t serial;
    char     mime[CLIP_MIME_NAME];
    int      read_sd;
    uint8_t* buf;
    uint32_t len, cap;
    uint32_t deadline;
};

struct ClipWrite {
    bool     used;
    int      sd;
    uint8_t* data;
    uint32_t len, off;
    uint32_t deadline;
};

ClipSourceInfo g_csources[CLIP_MAX_SOURCES];
ClipOfferInfo  g_coffers[CLIP_MAX_OFFERS];
ClipSelection  g_csel, g_psel;
ClipPull       g_pulls[CLIP_MAX_PULLS];
ClipWrite      g_writes[CLIP_MAX_WRITES];
int            g_pulls_left[2] = { 0, 0 };
uint32_t       g_csel_serial = 1;
uint32_t       g_psel_serial = 1;
Spinlock       g_clip_lock;

bool cstr_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

bool clip_is_text_mime(const char* m) {
    return cstr_eq(m, "text/plain") || cstr_eq(m, "text/plain;charset=utf-8") ||
           cstr_eq(m, "UTF8_STRING");
}

bool clip_supported_mime(const char* m) {
    return clip_is_text_mime(m) || cstr_eq(m, "text/html") || cstr_eq(m, "image/png");
}

void clip_mimes_clear(ClipMimeList& l) { l.count = 0; }

bool clip_mimes_add(ClipMimeList& l, const char* m) {
    if (!m || !m[0]) return false;
    for (int i = 0; i < l.count; i++) if (cstr_eq(l.name[i], m)) return false;
    if (l.count >= CLIP_MAX_MIMES) return false;
    int n = 0;
    while (m[n] && n < CLIP_MIME_NAME - 1) { l.name[l.count][n] = m[n]; n++; }
    l.name[l.count][n] = 0;
    l.count++;
    return true;
}

ClipSelection& clip_sel(bool primary) { return primary ? g_psel : g_csel; }
uint32_t& clip_serial(bool primary) { return primary ? g_psel_serial : g_csel_serial; }
int clip_sel_index(bool primary) { return primary ? 1 : 0; }

ClipSourceInfo* clip_find_source(int sd, uint32_t id) {
    for (int i = 0; i < CLIP_MAX_SOURCES; i++)
        if (g_csources[i].used && g_csources[i].sd == sd && g_csources[i].id == id)
            return &g_csources[i];
    return nullptr;
}

ClipOfferInfo* clip_find_offer(int sd, uint32_t id) {
    for (int i = 0; i < CLIP_MAX_OFFERS; i++)
        if (g_coffers[i].used && g_coffers[i].sd == sd && g_coffers[i].id == id)
            return &g_coffers[i];
    return nullptr;
}

void clip_remove_source(int sd, uint32_t id) {
    ClipSourceInfo* s = clip_find_source(sd, id);
    if (s) { s->used = false; s->sd = -1; s->id = 0; }
}

void clip_remove_offer(int sd, uint32_t id) {
    ClipOfferInfo* o = clip_find_offer(sd, id);
    if (o) { o->used = false; o->sd = -1; o->id = 0; }
}

void clip_free_pull(ClipPull& p) {
    if (p.read_sd >= 0) UnixSocket::Close(p.read_sd);
    if (p.buf) KernelHeap::Free(p.buf);
    p.buf = nullptr;
    p.len = p.cap = 0;
    p.read_sd = -1;
    p.used = false;
}

void clip_cancel_pulls(bool primary, uint32_t serial) {
    for (int i = 0; i < CLIP_MAX_PULLS; i++) {
        ClipPull& p = g_pulls[i];
        if (!p.used || p.primary != primary) continue;
        if (serial && p.serial != serial) continue;
        clip_free_pull(p);
    }
    g_pulls_left[clip_sel_index(primary)] = 0;
}

void clip_pump_writes();

void clip_finish_pull(ClipPull& p) {
    ClipSelection& sel = clip_sel(p.primary);
    bool current = sel.active && !sel.system && sel.serial == p.serial;
    SerialLogger::Log("[clip] pull done mime="); SerialLogger::Log(p.mime);
    SerialLogger::Log(" len="); SerialLogger::LogDec((int)p.len);
    SerialLogger::Log(" current="); SerialLogger::LogDec(current ? 1 : 0);
    SerialLogger::Log("\r\n");
    if (current && p.len > 0)
        ClipboardManager::AddMimeFor(p.primary, p.mime, p.buf, p.len);
    clip_free_pull(p);
    if (!current) return;
    int idx = clip_sel_index(p.primary);
    if (g_pulls_left[idx] > 0) g_pulls_left[idx]--;
    if (g_pulls_left[idx] == 0)
        ClipboardManager::CommitReplace(p.primary, false);
}

void clip_pump_pulls() {
    uint32_t now = Timer::GetRealMs();
    for (int i = 0; i < CLIP_MAX_PULLS; i++) {
        ClipPull& p = g_pulls[i];
        if (!p.used) continue;
        bool done = false;
        for (int guard = 0; guard < 8 && !done; guard++) {
            uint8_t tmp[4096];
            int n = UnixSocket::Recv(p.read_sd, tmp, (int)sizeof(tmp), 0);
            if (n > 0) {
                if (p.len + (uint32_t)n > ClipboardManager::MAX_BLOB) { done = true; break; }
                if (p.len + (uint32_t)n > p.cap) {
                    uint32_t ncap = p.cap ? p.cap : 65536;
                    while (ncap < p.len + (uint32_t)n) ncap *= 2;
                    if (ncap > ClipboardManager::MAX_BLOB) ncap = ClipboardManager::MAX_BLOB;
                    uint8_t* nb = (uint8_t*)KernelHeap::Alloc(ncap);
                    if (!nb) { done = true; break; }
                    for (uint32_t k = 0; k < p.len; k++) nb[k] = p.buf[k];
                    if (p.buf) KernelHeap::Free(p.buf);
                    p.buf = nb;
                    p.cap = ncap;
                }
                for (int k = 0; k < n; k++) p.buf[p.len + k] = tmp[k];
                p.len += (uint32_t)n;
                continue;
            }
            if (n == -11) break;             // EAGAIN: owner still writing (satoru)
            done = true;                     // EOF (0) or error
        }
        if (!done && now > p.deadline) done = true;
        if (done) clip_finish_pull(p);
    }
}

void clip_queue_write(int dest_sd, uint8_t* data, uint32_t len) {
    if (dest_sd < 0) { if (data) KernelHeap::Free(data); return; }
    if (!data || len == 0) {
        if (data) KernelHeap::Free(data);
        UnixSocket::Close(dest_sd);
        return;
    }
    for (int i = 0; i < CLIP_MAX_WRITES; i++) {
        ClipWrite& w = g_writes[i];
        if (w.used) continue;
        w.used = true;
        w.sd = dest_sd;
        w.data = data;
        w.len = len;
        w.off = 0;
        w.deadline = Timer::GetRealMs() + CLIP_WRITE_MS;
        return;
    }
    KernelHeap::Free(data);
    UnixSocket::Close(dest_sd);
}

void clip_pump_writes() {
    uint32_t now = Timer::GetRealMs();
    for (int i = 0; i < CLIP_MAX_WRITES; i++) {
        ClipWrite& w = g_writes[i];
        if (!w.used) continue;
        bool done = false;
        for (int guard = 0; guard < 8 && !done; guard++) {
            int n = UnixSocket::Send(w.sd, w.data + w.off, (int)(w.len - w.off), 0);
            if (n > 0) {
                w.off += (uint32_t)n;
                if (w.off >= w.len) done = true;
                continue;
            }
            if (n == -11) break;             // ring full: next frame (satoru)
            done = true;                     // EPIPE / closed
        }
        if (!done && now > w.deadline) done = true;
        if (done) {
            UnixSocket::Close(w.sd);
            KernelHeap::Free(w.data);
            w.data = nullptr;
            w.used = false;
        }
    }
}

void clip_mimes_from_system(bool primary, ClipMimeList& out) {
    clip_mimes_clear(out);
    if (primary) {
        if (ClipboardManager::HasPrimaryText()) clip_mimes_add(out, "text/plain;charset=utf-8");
        return;
    }
    if (ClipboardManager::HasText()) clip_mimes_add(out, "text/plain;charset=utf-8");
    uint32_t n = 0;
    if (ClipboardManager::GetData("text/html", &n) && n) clip_mimes_add(out, "text/html");
    if (ClipboardManager::GetData("image/png", &n) && n) clip_mimes_add(out, "image/png");
}

void clip_send_device_selection(Client* c, bool primary, uint32_t offer_id) {
    uint8_t pl[4];
    put32(pl, offer_id);
    if (primary) {
        for (int i = 0; i < c->primary_device_count; i++)
            send_event(c, c->primary_device_ids[i], 1, pl, 4);   // selection
    } else {
        for (int i = 0; i < c->data_device_count; i++)
            send_event(c, c->data_device_ids[i], 5, pl, 4);      // selection
    }
}

void clip_broadcast_locked(bool primary) {
    ClipSelection& sel = clip_sel(primary);
    for (int ci = 0; ci < g_ncap; ci++) {
        Client* c = (*cl_ref(ci));
        if (!c || !c->in_use) continue;
        uint32_t old = primary ? c->primary_offer_id : c->clip_offer_id;
        if (old) {
            clip_remove_offer(c->sd, old);
            destroy_obj(c, old);
            if (primary) c->primary_offer_id = 0; else c->clip_offer_id = 0;
        }
        if (!sel.active) {
            clip_send_device_selection(c, primary, 0);
            tx_flush(c);
            continue;
        }
        // the owner already knows what it copied; it gets no offer. (satoru)
        if (!sel.system && sel.sd == c->sd) continue;
        int dev_count = primary ? c->primary_device_count : c->data_device_count;
        if (dev_count <= 0) continue;
        uint32_t dev = primary ? c->primary_device_ids[0] : c->data_device_ids[0];
        uint32_t oid = 0xFF000000u | (c->clip_server_id_next++ & 0x00FFFFFFu);
        uint16_t iface = primary ? ZWP_PRIMARY_OFFER : WL_DATA_OFFER;
        if (!register_obj(c, oid, iface, 3)) continue;
        ClipOfferInfo* oi = nullptr;
        for (int i = 0; i < CLIP_MAX_OFFERS; i++) {
            if (!g_coffers[i].used) { oi = &g_coffers[i]; break; }
        }
        if (!oi) { destroy_obj(c, oid); continue; }
        oi->used = true;
        oi->sd = c->sd;
        oi->id = oid;
        oi->primary = primary;
        oi->serial = sel.serial;
        oi->mimes = sel.mimes;
        if (primary) c->primary_offer_id = oid; else c->clip_offer_id = oid;

        uint8_t ev[128];
        put32(ev, oid);
        send_event(c, dev, 0, ev, 4);           // data_offer
        for (int i = 0; i < sel.mimes.count; i++) {
            int p = 0;
            put_wl_string(ev, &p, sel.mimes.name[i]);
            send_event(c, oid, 0, ev, (uint16_t)p);   // offer(mime)
        }
        clip_send_device_selection(c, primary, oid);
        tx_flush(c);
    }
}

void clip_promote_system_locked(bool primary) {
    ClipSelection& sel = clip_sel(primary);
    clip_cancel_pulls(primary, 0);
    ClipboardManager::CommitReplace(primary, false);
    ClipMimeList mimes;
    clip_mimes_from_system(primary, mimes);
    sel.active = mimes.count > 0;
    sel.system = true;
    sel.sd = 0;
    sel.source_id = 0;
    sel.source_iface = 0;
    sel.serial = ++clip_serial(primary);
    sel.mimes = mimes;
    clip_broadcast_locked(primary);
}

// ask the owner for one mime over a fresh socketpair; the other end rides
// wl_data_source.send so the owner's write lands back in the pull buffer. (satoru)
bool clip_start_pull_locked(bool primary, const char* mime) {
    ClipSelection& sel = clip_sel(primary);
    int slot = -1;
    for (int i = 0; i < CLIP_MAX_PULLS; i++) if (!g_pulls[i].used) { slot = i; break; }
    if (slot < 0) return false;
    int ra = -1, wb = -1;
    if (UnixSocket::Pair(UnixSocket::UNIX_SOCK_STREAM, &ra, &wb) < 0) return false;

    uint8_t ev[128];
    put32(ev, sel.source_id);
    uint16_t op = primary ? 0 : 1;              // primary source send / data source send
    put16(ev + 4, op);
    int p = 8;
    put_wl_string(ev, &p, mime);
    put16(ev + 6, (uint16_t)p);

    UnixSocket::ControlMsg cm = {};
    cm.passed_fd_count = 1;
    cm.passed_fds[0] = -1;
    cm.passed_is_socket[0] = true;
    cm.passed_sd[0] = wb;
    UnixSocket::Retain(wb);   // the ref that rides the SCM_RIGHTS message (satoru)
    int r = UnixSocket::KernelInjectMsg(sel.sd, ev, (uint16_t)p, &cm);
    // the pair's own ref is the kernel's, not the client's: drop it now that
    // the message carries the retained one. on failure nothing was delivered,
    // so drop the in-flight ref as well. leaking it held the socket slot open
    // forever, one per clipboard pull. (satoru)
    UnixSocket::Close(wb);
    if (r <= 0) {
        UnixSocket::Close(wb);
        UnixSocket::Close(ra);
        return false;
    }
    ClipPull& pull = g_pulls[slot];
    pull.used = true;
    pull.owner_sd = sel.sd;
    pull.source_id = sel.source_id;
    pull.source_iface = sel.source_iface;
    pull.primary = primary;
    pull.serial = sel.serial;
    int mn = 0;
    while (mime[mn] && mn < CLIP_MIME_NAME - 1) { pull.mime[mn] = mime[mn]; mn++; }
    pull.mime[mn] = 0;
    pull.read_sd = ra;
    pull.buf = nullptr;
    pull.len = pull.cap = 0;
    pull.deadline = Timer::GetRealMs() + CLIP_PULL_MS;
    SerialLogger::Log("[clip] pull start src="); SerialLogger::LogDec((int)sel.source_id);
    SerialLogger::Log(" mime="); SerialLogger::Log(mime);
    SerialLogger::Log("\r\n");
    return true;
}

// pick at most one text spelling plus html and png. (satoru)
int clip_pick_mimes(const ClipMimeList& in, char out[3][CLIP_MIME_NAME]) {
    int n = 0;
    const char* text = nullptr;
    bool html = false, png = false;
    for (int i = 0; i < in.count; i++) {
        const char* m = in.name[i];
        if (clip_is_text_mime(m)) {
            if (!text || cstr_eq(m, "text/plain;charset=utf-8")) text = m;
        } else if (cstr_eq(m, "text/html")) html = true;
        else if (cstr_eq(m, "image/png")) png = true;
    }
    if (text) {
        int k = 0; while (text[k] && k < CLIP_MIME_NAME - 1) { out[n][k] = text[k]; k++; }
        out[n][k] = 0; n++;
    }
    if (html) { const char* m = "text/html"; int k = 0; while (m[k]) { out[n][k] = m[k]; k++; } out[n][k] = 0; n++; }
    if (png)  { const char* m = "image/png"; int k = 0; while (m[k]) { out[n][k] = m[k]; k++; } out[n][k] = 0; n++; }
    return n;
}

void clip_set_selection_locked(Client* c, bool primary, uint32_t source_id) {
    ClipSelection& sel = clip_sel(primary);
    uint32_t& serial = clip_serial(primary);
    ClipSourceInfo* src = source_id ? clip_find_source(c->sd, source_id) : nullptr;
    if (source_id && (!src || src->primary != primary)) return;

    // the old owner is cancelled; it may still hold a source object. (satoru)
    if (sel.active && !sel.system && (sel.sd != c->sd || sel.source_id != source_id)) {
        Client* oc = client_for_sd(sel.sd);
        if (oc) {
            uint16_t op = primary ? 1 : 2;       // primary source cancelled / data source cancelled
            send_event(oc, sel.source_id, op, nullptr, 0);
            tx_flush(oc);
        }
    }
    clip_cancel_pulls(primary, 0);
    ClipboardManager::BeginReplace(primary);

    serial++;
    sel.serial = serial;
    sel.sd = c->sd;
    sel.source_id = source_id;
    sel.source_iface = src ? src->iface : 0;
    sel.system = false;
    sel.active = source_id != 0;
    clip_mimes_clear(sel.mimes);
    if (src) sel.mimes = src->mimes;

    char picked[3][CLIP_MIME_NAME];
    int np = sel.active ? clip_pick_mimes(sel.mimes, picked) : 0;
    int idx = clip_sel_index(primary);
    g_pulls_left[idx] = np;
    for (int i = 0; i < np; i++) {
        if (!clip_start_pull_locked(primary, picked[i])) g_pulls_left[idx]--;
    }
    SerialLogger::Log("[clip] selection sd="); SerialLogger::LogDec(c->sd);
    SerialLogger::Log(" src="); SerialLogger::LogDec((int)source_id);
    SerialLogger::Log(" mimes="); SerialLogger::LogDec(sel.mimes.count);
    SerialLogger::Log(" primary="); SerialLogger::LogDec(primary ? 1 : 0);
    SerialLogger::Log("\r\n");
    if (g_pulls_left[idx] <= 0) ClipboardManager::CommitReplace(primary, false);
    clip_broadcast_locked(primary);
}

void clip_offer_receive(Client* c, uint32_t offer_id, const uint8_t* args, int args_len, bool primary) {
    char mime[CLIP_MIME_NAME];
    mime[0] = 0;
    if (args_len >= 4) {
        uint32_t sl = get32(args);
        if (sl > 0 && sl <= (uint32_t)args_len - 4 && sl < CLIP_MIME_NAME) {
            uint32_t n = sl - 1;
            for (uint32_t i = 0; i < n; i++) mime[i] = (char)args[4 + i];
            mime[n] = 0;
        }
    }
    UnixSocket::ControlMsg cm = {};
    bool got = UnixSocket::TakePendingControl(c->sd, &cm);

    uint8_t* data = nullptr;
    uint32_t len = 0;
    {
        SpinLockGuard g(g_clip_lock);
        ClipOfferInfo* oi = clip_find_offer(c->sd, offer_id);
        ClipSelection& sel = clip_sel(primary);
        if (oi && oi->primary == primary && sel.active && oi->serial == sel.serial && mime[0])
            ClipboardManager::CopyData(mime, &data, &len);
        SerialLogger::Log("[clip] recv offer="); SerialLogger::LogDec((int)offer_id);
        SerialLogger::Log(" mime="); SerialLogger::Log(mime);
        SerialLogger::Log(" len="); SerialLogger::LogDec((int)len);
        SerialLogger::Log("\r\n");
    }

    if (!got || cm.passed_fd_count <= 0) {
        if (data) KernelHeap::Free(data);
        return;
    }
    if (cm.passed_shm_base[0]) {
        uint8_t* base = (uint8_t*)(uintptr_t)cm.passed_shm_base[0];
        uint32_t cap = (uint32_t)cm.passed_shm_size[0];
        uint32_t n = (data && len < cap) ? len : (data ? cap : 0);
        for (uint32_t i = 0; i < n; i++) base[i] = data[i];
        if (data) KernelHeap::Free(data);
        return;
    }
    if (cm.passed_is_socket[0]) {
        int dest = cm.passed_sd[0];
        UnixSocket::Retain(dest);   // the sender's in-flight ref is dropped when sendmsg returns (satoru)
        SpinLockGuard g(g_clip_lock);
        clip_queue_write(dest, data, len);
        return;
    }
    if (data) KernelHeap::Free(data);
}

void clip_client_dropped(int sd) {
    SpinLockGuard g(g_clip_lock);
    for (int i = 0; i < CLIP_MAX_PULLS; i++)
        if (g_pulls[i].used && g_pulls[i].owner_sd == sd) clip_free_pull(g_pulls[i]);
    for (int i = 0; i < CLIP_MAX_SOURCES; i++)
        if (g_csources[i].used && g_csources[i].sd == sd) g_csources[i].used = false;
    for (int i = 0; i < CLIP_MAX_OFFERS; i++)
        if (g_coffers[i].used && g_coffers[i].sd == sd) g_coffers[i].used = false;
    // the copy survives its owner: the eager pull left a snapshot in the
    // system clipboard, so promote it instead of clearing. (satoru)
    if (g_csel.active && !g_csel.system && g_csel.sd == sd) clip_promote_system_locked(false);
    if (g_psel.active && !g_psel.system && g_psel.sd == sd) clip_promote_system_locked(true);
}

void clip_pump_all() {
    SpinLockGuard g(g_clip_lock);
    clip_pump_pulls();
    clip_pump_writes();
}

void handle_request(Client* c, uint32_t object_id, uint16_t opcode,
                    const uint8_t* args, int args_len) {
    if (c->fatal) return;
    Object* obj = find_obj(c, object_id);
    if (!obj) {
        // no delete_id exists here, and the server can drop an object before the client
        // sees it (cascades, window close), so a late request is a race, not a client bug.
        // a protocol error makes firefox crash on purpose: log and ignore instead. (satoru)
        static int s_bad_obj_logs = 0;
        if (s_bad_obj_logs < 16) {
            s_bad_obj_logs++;
            SerialLogger::Log("[wl] ignored request on unknown object ");
            SerialLogger::LogDec((int)object_id);
            SerialLogger::Log(" op ");
            SerialLogger::LogDec((int)opcode);
            SerialLogger::Log("\r\n");
        }
        return;
    }
    if (g_wltrace) {   // kurono.wltrace=1: silent ring, dumped later (satoru)
        wlt_note(0, c->sd, object_id, obj->iface, opcode,
                 args_len >= 4 ? get32(args) : 0, args_len >= 8 ? get32(args + 4) : 0);
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
            // destroy=0, add=1 (x,y,w,h), subtract=2. emptiness drives the
            // input-region rule; the bbox + add/sub counts let set_opaque_region
            // snapshot a single-rect opaque area for the ARGB blend. (satoru)
            if (opcode == 0) { destroy_obj(c, object_id); break; }
            if (opcode == 1 && args_len >= 16) {
                int32_t rx = (int32_t)get32(args);
                int32_t ry = (int32_t)get32(args + 4);
                int32_t rw = (int32_t)get32(args + 8);
                int32_t rh = (int32_t)get32(args + 12);
                if (rw > 0 && rh > 0) {
                    // w/h are int32 and the sum can wrap: compute the far edge
                    // in 64 bits, then clamp into the bbox field range. (satoru)
                    int64_t rx1 = (int64_t)rx + rw;
                    int64_t ry1 = (int64_t)ry + rh;
                    if (rx1 > INT32_MAX) rx1 = INT32_MAX;
                    if (rx1 < INT32_MIN) rx1 = INT32_MIN;
                    if (ry1 > INT32_MAX) ry1 = INT32_MAX;
                    if (ry1 < INT32_MIN) ry1 = INT32_MIN;
                    obj->region_has_rect = true;
                    if (obj->reg_adds == 0) {
                        obj->reg_x0 = rx; obj->reg_y0 = ry;
                        obj->reg_x1 = (int32_t)rx1; obj->reg_y1 = (int32_t)ry1;
                    } else {
                        if (rx < obj->reg_x0) obj->reg_x0 = rx;
                        if (ry < obj->reg_y0) obj->reg_y0 = ry;
                        if (rx1 > obj->reg_x1) obj->reg_x1 = (int32_t)rx1;
                        if (ry1 > obj->reg_y1) obj->reg_y1 = (int32_t)ry1;
                    }
                    obj->reg_adds++;
                }
            } else if (opcode == 2 && args_len >= 16) {
                obj->reg_subs++;
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
                // both must be live wl_surfaces and different; no self-parent. (satoru)
                {
                    Object* s0 = find_obj(c, surf_id);
                    Object* p0 = find_obj(c, parent);
                    if (!s0 || !p0 || s0->iface != WL_SURFACE || p0->iface != WL_SURFACE ||
                        surf_id == parent) return;
                }
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
                    ss->subsurface_x = wl_clamp((int32_t)get32(args), -WL_MAX_EDGE, WL_MAX_EDGE);
                    ss->subsurface_y = wl_clamp((int32_t)get32(args + 4), -WL_MAX_EDGE, WL_MAX_EDGE);
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
                if (g_wltrace) {
                    SerialLogger::Log("[wlp] create_pool sd="); SerialLogger::LogDec(c->sd);
                    SerialLogger::Log(" id="); SerialLogger::LogDec((int)pid);
                    SerialLogger::Log(" sz="); SerialLogger::LogDec((int)size);
                    SerialLogger::Log(" cm="); SerialLogger::LogDec(gotcm ? 1 : 0); SerialLogger::Log("\r\n");
                }
                if (gotcm) {
                    for (int i = 0; i < cm.passed_fd_count; i++) {
                        if (!cm.passed_shm_base[i]) continue;
                        po->pool_base = (const uint8_t*)(uintptr_t)cm.passed_shm_base[i];
                        uint64_t msz64 = (uint64_t)cm.passed_shm_size[i];
                        uint32_t msz = msz64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)msz64;
                        po->pool_size = (size && size <= msz) ? size : msz;
                        po->pool_map_size = msz;   // the real backing (satoru)
                        po->pin_slot = cm.passed_shm_slot[i];
                        po->pin_pin  = cm.passed_shm_pin[i];
                        break;
                    }
                }
                if (po) {
                    g_pp_pools_live++; g_pp_pools_made++;
                    uint64_t b = po->pool_map_size ? po->pool_map_size : po->pool_size;
                    g_pp_pool_kb += b >> 10;
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
                // a shadow per buffer size is kernel heap: cap the pixel count,
                // and reject a stride shorter than a row or a buffer that ends
                // past the pool's real backing (64-bit math). (satoru)
                if ((uint64_t)bw * (uint64_t)bh > WL_MAX_BUF_PX) return;
                if (stride != 0 && (uint64_t)stride < (uint64_t)bw * 4u) return;
                {
                    uint64_t st  = stride ? stride : (uint64_t)bw * 4u;
                    uint64_t end = (uint64_t)offset + st * (uint64_t)bh;
                    if (obj->pool_base && end > obj->pool_map_size) return;
                }
                if (!register_obj(c, bid, WL_BUFFER, 1)) return;
                Object* bo = find_obj(c, bid);
                if (bo) {
                    bo->buf_pool_id = object_id;  // this pool owns the buffer
                    bo->buf_offset  = offset;
                    bo->width       = bw;
                    bo->height      = bh;
                    bo->buf_stride  = stride ? stride : (uint32_t)bw * 4u;
                    bo->buf_format  = format;
                    // keep the pool mapping alive with the buffer: mesa destroys
                    // the wl_shm_pool immediately after create_buffer. (satoru)
                    bo->buf_base      = obj->pool_base;
                    bo->buf_base_size = obj->pool_map_size;
                    bo->pin_slot      = obj->pin_slot;   // the pool's pass
                    bo->pin_pin       = obj->pin_pin;
                    g_pp_bufs_live++; g_pp_bufs_made++;
                    g_pp_buf_kb += ((uint64_t)bo->buf_stride * (uint64_t)bh) >> 10;
                    if (g_wltrace) {
                        SerialLogger::Log("[wlb] buf="); SerialLogger::LogDec((int)bid);
                        SerialLogger::Log(" "); SerialLogger::LogDec(bw); SerialLogger::Log("x"); SerialLogger::LogDec(bh);
                        SerialLogger::Log(" st="); SerialLogger::LogDec((int)bo->buf_stride);
                        SerialLogger::Log(" fmt="); SerialLogger::LogDec((int)format);
                        SerialLogger::Log(" base="); SerialLogger::LogHex64((uint64_t)(uintptr_t)bo->buf_base);
                        SerialLogger::Log(" psz="); SerialLogger::LogDec((int)bo->buf_base_size); SerialLogger::Log("\r\n");
                    }
                }
            } else if (opcode == 1) {             // destroy
                {
                    uint64_t b = obj->pool_map_size ? obj->pool_map_size : obj->pool_size;
                    if (g_pp_pools_live) g_pp_pools_live--;
                    g_pp_pool_kb = (g_pp_pool_kb > (b >> 10)) ? g_pp_pool_kb - (b >> 10) : 0;
                }
                {
                    uint32_t pslot = obj->pin_slot, ppin = obj->pin_pin;
                    destroy_obj(c, object_id);
                    pool_unpin_token(pslot, ppin);
                }
            } else if (opcode == 2) {             // resize
                // resize(size): grow the pool hint. (satoru)
                // never past the mapped backing: pool_base was bound at create
                // and does not grow, and every bounds check reads pool_size. (satoru)
                if (args_len >= 4) {
                    uint32_t nsize = get32(args);
                    if (obj->pool_base && nsize > obj->pool_map_size) nsize = obj->pool_map_size;
                    if (nsize > obj->pool_size) obj->pool_size = nsize;
                }
            }
            break;
        }
        case WL_BUFFER: {
            if (opcode == 0) {                    // destroy
                {
                    uint64_t b = (uint64_t)obj->buf_stride * (uint64_t)(obj->height > 0 ? obj->height : 0);
                    if (g_pp_bufs_live) g_pp_bufs_live--;
                    g_pp_buf_kb = (g_pp_buf_kb > (b >> 10)) ? g_pp_buf_kb - (b >> 10) : 0;
                }
                uint32_t bslot = obj->pin_slot, bpin = obj->pin_pin;
                // no surface may keep naming a dead buffer: libwayland reuses the
                // id for the next new object. (satoru)
                for (int i = 0; i < c->object_high; i++) {
                    Object& so = c->objects[i];
                    if (so.in_use && so.iface == WL_SURFACE && so.pending_buffer_id == object_id) {
                        so.pending_buffer_id = 0;
                        so.attach_pending = false;
                    }
                }
                destroy_obj(c, object_id);
                pool_unpin_token(bslot, bpin);
            }
            break;
        }
        case WL_DATA_DEVICE_MGR: {
            if (opcode == 0) {                    // create_data_source(new_id)
                if (args_len < 4) return;
                uint32_t sid = get32(args);
                if (!register_obj(c, sid, WL_DATA_SOURCE, obj->version)) return;
                ClipSourceInfo* si = nullptr;
                for (int i = 0; i < CLIP_MAX_SOURCES; i++)
                    if (!g_csources[i].used) { si = &g_csources[i]; break; }
                if (!si) { destroy_obj(c, sid); return; }
                si->used = true; si->sd = c->sd; si->id = sid;
                si->iface = WL_DATA_SOURCE; si->primary = false;
                clip_mimes_clear(si->mimes);
            } else if (opcode == 1) {             // get_data_device(new_id, seat)
                if (args_len < 8) return;
                uint32_t did = get32(args);
                if (!register_obj(c, did, WL_DATA_DEVICE, obj->version)) return;
                if (c->data_device_count < Client::WL_MAX_DATA_DEV)
                    c->data_device_ids[c->data_device_count++] = did;
                SpinLockGuard g(g_clip_lock);
                clip_broadcast_locked(false);   // a new device learns the current selection (satoru)
            }
            break;
        }
        case WL_DATA_DEVICE: {
            if (opcode == 0) {                    // start_drag: no dnd in this compositor
                break;
            } else if (opcode == 1) {             // set_selection(source, serial)
                if (args_len < 8) return;
                uint32_t sid = get32(args);
                SerialLogger::Log("[clip] set_selection src="); SerialLogger::LogDec((int)sid);
                SerialLogger::Log(" sd="); SerialLogger::LogDec(c->sd); SerialLogger::Log("\r\n");
                SpinLockGuard g(g_clip_lock);
                clip_set_selection_locked(c, false, sid);
            } else if (opcode == 2) {             // release
                int w = 0;
                for (int i = 0; i < c->data_device_count; i++)
                    if (c->data_device_ids[i] != object_id) c->data_device_ids[w++] = c->data_device_ids[i];
                c->data_device_count = w;
                destroy_obj(c, object_id);
            }
            break;
        }
        case WL_DATA_SOURCE: {
            if (opcode == 0 && args_len >= 4) {   // offer(mime)
                uint32_t sl = get32(args);
                if (sl == 0 || sl > (uint32_t)args_len - 4) return;
                char mime[CLIP_MIME_NAME];
                uint32_t n = sl - 1;
                if (n >= CLIP_MIME_NAME) n = CLIP_MIME_NAME - 1;
                for (uint32_t i = 0; i < n; i++) mime[i] = (char)args[4 + i];
                mime[n] = 0;
                SpinLockGuard g(g_clip_lock);
                ClipSourceInfo* si = clip_find_source(c->sd, object_id);
                if (si) clip_mimes_add(si->mimes, mime);
            } else if (opcode == 1) {             // destroy
                SpinLockGuard g(g_clip_lock);
                ClipSelection& sel = g_csel;
                if (sel.active && !sel.system && sel.sd == c->sd && sel.source_id == object_id)
                    clip_set_selection_locked(c, false, 0);
                clip_remove_source(c->sd, object_id);
                destroy_obj(c, object_id);
            } else if (opcode == 2 && args_len >= 4) {   // set_actions
                // drag actions only; no-op for selections. (satoru)
            }
            break;
        }
        case WL_DATA_OFFER: {
            if (opcode == 1) {                    // receive(mime, fd)
                clip_offer_receive(c, object_id, args, args_len, false);
            } else if (opcode == 2) {             // destroy
                SpinLockGuard g(g_clip_lock);
                clip_remove_offer(c->sd, object_id);
                if (c->clip_offer_id == object_id) c->clip_offer_id = 0;
                destroy_obj(c, object_id);
            }
            // accept=0, finish=3, set_actions=4: dnd only. (satoru)
            break;
        }
        case ZXDG_DECORATION_MGR: {
            if (!g_wl_deco) break;   // default: same as the old unimplemented global (satoru)
            // xdg-decoration v1: kurono's wm draws the titlebar and border for
            // every bridged toplevel, so tell clients the decorations are
            // server-side. without the configure event libdecor waits for a
            // mode that never arrives. (satoru)
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 1) {             // get_toplevel_decoration(new_id, toplevel)
                if (args_len < 8) return;
                uint32_t did = get32(args);
                uint32_t tid = get32(args + 4);
                if (!register_obj(c, did, ZXDG_TOPLEVEL_DECORATION, 1)) return;
                Object* d = find_obj(c, did);
                if (d) d->parent_surface_id = tid;
                uint8_t mode[4]; put32(mode, 1);  // mode_server_side
                send_event(c, did, 0, mode, 4);
            }
            break;
        }
        case ZXDG_TOPLEVEL_DECORATION: {
            // destroy=0, set_mode=1, unset_mode=2. the mode is fixed to
            // server-side; answer every request with that mode. (satoru)
            if (opcode == 0) { destroy_obj(c, object_id); break; }
            if (opcode == 1 || opcode == 2) {
                uint8_t mode[4]; put32(mode, 1);
                send_event(c, object_id, 0, mode, 4);
            }
            break;
        }
        case ZWP_PRIMARY_MGR: {
            if (opcode == 0) {                    // create_source(new_id)
                if (args_len < 4) return;
                uint32_t sid = get32(args);
                if (!register_obj(c, sid, ZWP_PRIMARY_SOURCE, obj->version)) return;
                ClipSourceInfo* si = nullptr;
                for (int i = 0; i < CLIP_MAX_SOURCES; i++)
                    if (!g_csources[i].used) { si = &g_csources[i]; break; }
                if (!si) { destroy_obj(c, sid); return; }
                si->used = true; si->sd = c->sd; si->id = sid;
                si->iface = ZWP_PRIMARY_SOURCE; si->primary = true;
                clip_mimes_clear(si->mimes);
            } else if (opcode == 1) {             // get_device(new_id, seat)
                if (args_len < 8) return;
                uint32_t did = get32(args);
                if (!register_obj(c, did, ZWP_PRIMARY_DEVICE, obj->version)) return;
                if (c->primary_device_count < Client::WL_MAX_DATA_DEV)
                    c->primary_device_ids[c->primary_device_count++] = did;
                SpinLockGuard g(g_clip_lock);
                clip_broadcast_locked(true);
            } else if (opcode == 2) {             // destroy
                destroy_obj(c, object_id);
            }
            break;
        }
        case ZWP_PRIMARY_DEVICE: {
            if (opcode == 0) {                    // set_selection(source, serial)
                if (args_len < 8) return;
                uint32_t sid = get32(args);
                SpinLockGuard g(g_clip_lock);
                clip_set_selection_locked(c, true, sid);
            } else if (opcode == 1) {             // destroy
                int w = 0;
                for (int i = 0; i < c->primary_device_count; i++)
                    if (c->primary_device_ids[i] != object_id) c->primary_device_ids[w++] = c->primary_device_ids[i];
                c->primary_device_count = w;
                destroy_obj(c, object_id);
            }
            break;
        }
        case ZWP_PRIMARY_SOURCE: {
            if (opcode == 0 && args_len >= 4) {   // offer(mime)
                uint32_t sl = get32(args);
                if (sl == 0 || sl > (uint32_t)args_len - 4) return;
                char mime[CLIP_MIME_NAME];
                uint32_t n = sl - 1;
                if (n >= CLIP_MIME_NAME) n = CLIP_MIME_NAME - 1;
                for (uint32_t i = 0; i < n; i++) mime[i] = (char)args[4 + i];
                mime[n] = 0;
                SpinLockGuard g(g_clip_lock);
                ClipSourceInfo* si = clip_find_source(c->sd, object_id);
                if (si) clip_mimes_add(si->mimes, mime);
            } else if (opcode == 1) {             // destroy
                SpinLockGuard g(g_clip_lock);
                ClipSelection& sel = g_psel;
                if (sel.active && !sel.system && sel.sd == c->sd && sel.source_id == object_id)
                    clip_set_selection_locked(c, true, 0);
                clip_remove_source(c->sd, object_id);
                destroy_obj(c, object_id);
            }
            break;
        }
        case ZWP_PRIMARY_OFFER: {
            if (opcode == 0) {                    // receive(mime, fd)
                clip_offer_receive(c, object_id, args, args_len, true);
            } else if (opcode == 1) {             // destroy
                SpinLockGuard g(g_clip_lock);
                clip_remove_offer(c->sd, object_id);
                if (c->primary_offer_id == object_id) c->primary_offer_id = 0;
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
            } else if (opcode == 1) {             // create_positioner(new_id)
                if (args_len < 4) return;
                register_obj(c, get32(args), XDG_POSITIONER, obj->version);
            } else if (opcode == 3) {             // pong
                // no-op
            }
            break;
        }
        case XDG_POSITIONER: {
            // placement rules only; read at get_popup / reposition time. (satoru)
            if (opcode == 0) { destroy_obj(c, object_id); break; }
            if (opcode == 1 && args_len >= 8) {            // set_size(w, h)
                obj->pos_w = wl_clamp((int32_t)get32(args), 1, WL_MAX_EDGE);
                obj->pos_h = wl_clamp((int32_t)get32(args + 4), 1, WL_MAX_EDGE);
            } else if (opcode == 2 && args_len >= 16) {    // set_anchor_rect(x, y, w, h)
                obj->pos_ax = wl_clamp((int32_t)get32(args), -WL_MAX_EDGE, WL_MAX_EDGE);
                obj->pos_ay = wl_clamp((int32_t)get32(args + 4), -WL_MAX_EDGE, WL_MAX_EDGE);
                obj->pos_aw = wl_clamp((int32_t)get32(args + 8), 0, WL_MAX_EDGE);
                obj->pos_ah = wl_clamp((int32_t)get32(args + 12), 0, WL_MAX_EDGE);
            } else if (opcode == 3 && args_len >= 4) {     // set_anchor
                obj->pos_anchor = get32(args);
            } else if (opcode == 4 && args_len >= 4) {     // set_gravity
                obj->pos_gravity = get32(args);
            } else if (opcode == 5 && args_len >= 4) {     // set_constraint_adjustment
                obj->pos_cadj = get32(args);
            } else if (opcode == 6 && args_len >= 8) {     // set_offset(x, y)
                obj->pos_ox = wl_clamp((int32_t)get32(args), -WL_MAX_EDGE, WL_MAX_EDGE);
                obj->pos_oy = wl_clamp((int32_t)get32(args + 4), -WL_MAX_EDGE, WL_MAX_EDGE);
            }
            // 7 set_reactive, 8 set_parent_size, 9 set_parent_configure: no-ops. (satoru)
            break;
        }
        case XDG_POPUP: {
            Object* psurf = find_obj(c, obj->parent_surface_id);
            if (opcode == 0) {                    // destroy: unmap the popup (satoru)
                if (psurf && psurf->iface == WL_SURFACE && psurf->popup_obj == object_id) {
                    mark_tree_root_dirty(c, psurf);
                    psurf->popup_obj = 0;
                    psurf->popup_grab = false;
                }
                destroy_obj(c, object_id);
            } else if (opcode == 1) {             // grab(seat, serial)
                if (psurf && psurf->iface == WL_SURFACE) psurf->popup_grab = true;
            } else if (opcode == 2 && args_len >= 8) {   // reposition(positioner, token)
                Object* pos = find_obj(c, get32(args));
                Object* par = psurf ? find_obj(c, psurf->popup_parent_sid) : nullptr;
                if (pos && pos->iface == XDG_POSITIONER && psurf && par && par->iface == WL_SURFACE) {
                    mark_tree_root_dirty(c, psurf);
                    popup_place(c, psurf, pos, par);
                    popup_send_configure(c, obj, psurf, true, get32(args + 4));
                    mark_tree_root_dirty(c, psurf);
                }
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
                SerialLogger::Log("[wl] get_toplevel sd="); SerialLogger::LogDec(c->sd);
                SerialLogger::Log("\r\n");
                Object* tl = find_obj(c, tid);
                if (tl) { tl->parent_surface_id = obj->parent_surface_id; tl->popup_xs = object_id; }
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
                uint32_t cfgw = 1024, cfgh = 768;
                if (g_wl_fullsize) {
                    int sw0 = Graphics::GetWidth(), sh0 = Graphics::GetHeight();
                    if (sw0 > 0 && sh0 > 0) { cfgw = (uint32_t)sw0; cfgh = (uint32_t)sh0; }
                }
                put32(cfg + 0, cfgw);
                put32(cfg + 4, cfgh);
                put32(cfg + 8, 4);                // states array = 4 bytes (1 entry)
                put32(cfg + 12, 4);               // XDG_TOPLEVEL_STATE_ACTIVATED
                send_event(c, tid, 0, cfg, 16);
                // xdg_surface::configure (opcode 0): serial.
                uint8_t srl[4]; put32(srl, c->serial_next++);
                send_event(c, object_id, 0, srl, 4);
            } else if (opcode == 2) {             // get_popup(new_id, parent xdg_surface, positioner)
                // menus and dropdowns. the popup is composited onto the window of
                // the tree it belongs to (see surface_tree_root), placed by the
                // positioner and constrained to that window. (satoru)
                if (args_len < 12) return;
                uint32_t pid = get32(args);
                Object* pxs  = find_obj(c, get32(args + 4));
                Object* pos  = find_obj(c, get32(args + 8));
                Object* surf = find_obj(c, obj->parent_surface_id);
                if (!register_obj(c, pid, XDG_POPUP, obj->version)) return;
                Object* po = find_obj(c, pid);
                if (!po || !surf || surf->iface != WL_SURFACE) return;
                po->parent_surface_id = surf->id;
                po->popup_xs = object_id;
                Object* par = (pxs && pxs->iface == XDG_SURFACE) ? find_obj(c, pxs->parent_surface_id) : nullptr;
                if (!par || par->iface != WL_SURFACE || par == surf) {
                    // no parent (allowed by the protocol only with another
                    // role mechanism): tell the client it is dismissed. (satoru)
                    send_event(c, pid, 1, nullptr, 0);
                    return;
                }
                static uint32_t s_popup_seq = 0;
                surf->popup_obj = pid;
                surf->popup_parent_sid = par->id;
                surf->popup_seq = ++s_popup_seq;
                surf->popup_grab = false;
                if (pos && pos->iface == XDG_POSITIONER) popup_place(c, surf, pos, par);
                else { surf->popup_x = 0; surf->popup_y = 0; surf->popup_w = 1; surf->popup_h = 1; }
                popup_send_configure(c, po, surf, false, 0);
            } else if (opcode == 3) {             // set_window_geometry(x,y,w,h)
                // the visible window rect inside the surface, excluding csd
                // shadow margins - store on the surface so blits crop to it
                // and the bridged window is sized to the VISIBLE rect. (satoru)
                if (args_len < 16) return;
                Object* gsurf = find_obj(c, obj->parent_surface_id);
                if (gsurf && gsurf->iface == WL_SURFACE) {
                    // clamped: geo_w/h size the wm window and geo_x/y feed
                    // signed blit coordinates. (satoru)
                    gsurf->geo_x = wl_clamp((int32_t)get32(args), 0, WL_MAX_EDGE);
                    gsurf->geo_y = wl_clamp((int32_t)get32(args + 4), 0, WL_MAX_EDGE);
                    gsurf->geo_w = wl_clamp((int32_t)get32(args + 8), 0, WL_MAX_EDGE);
                    gsurf->geo_h = wl_clamp((int32_t)get32(args + 12), 0, WL_MAX_EDGE);
                }
            } else if (opcode == 4) {             // ack_configure
                // no-op
            }
            break;
        }
        case XDG_TOPLEVEL: {
            if (opcode == 0) {                    // destroy
                destroy_obj(c, object_id);
            } else if (opcode == 1 || opcode == 7) {   // set_parent(parent) / set_max_size(w, h)
                // a dialog (set_parent) or a window that caps its own size is
                // sized by the client: re-configure it 0x0 before it maps, instead
                // of the 1024x768 every toplevel gets (a gtk "save changes?" dialog
                // was handed 1024x768 and never showed). (satoru)
                Object* ts = find_obj(c, obj->parent_surface_id);
                if (!ts || ts->iface != WL_SURFACE) break;
                bool own_size = false;
                if (opcode == 1 && args_len >= 4) {
                    Object* ptl = find_obj(c, get32(args));
                    Object* psurf = (ptl && ptl->iface == XDG_TOPLEVEL) ? find_obj(c, ptl->parent_surface_id) : nullptr;
                    ts->tl_parent_sid = (psurf && psurf != ts) ? psurf->id : 0;
                    own_size = ts->tl_parent_sid != 0;
                } else if (opcode == 7 && args_len >= 8) {
                    int32_t mw = (int32_t)get32(args), mh = (int32_t)get32(args + 4);
                    own_size = (mw > 0 && mw < 1024) || (mh > 0 && mh < 768);
                }
                if (own_size && !ts->wm_window) {
                    uint8_t cfg[16];
                    put32(cfg + 0, 0); put32(cfg + 4, 0);
                    put32(cfg + 8, 4); put32(cfg + 12, 4);   // states: ACTIVATED
                    send_event(c, object_id, 0, cfg, 16);
                    if (obj->popup_xs && find_obj(c, obj->popup_xs)) {
                        uint8_t srl[4]; put32(srl, c->serial_next++);
                        send_event(c, obj->popup_xs, 0, srl, 4);
                    }
                }
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
                    obj->attach_pending = true;
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
                    // remember which surface asked: the callback belongs to that
                    // surface's next commit (wayland spec), see deliver_surface_frame. (satoru)
                    if (Object* cbo = find_obj(c, cb)) cbo->parent_surface_id = object_id;
                    obj->pending_frame_cb = cb;
                    push_frame_cb(c, cb);
                    break;
                }
                case 4: {                          // set_opaque_region(region)
                    // snapshot a single-add region: the blit may skip per-pixel
                    // blending there (firefox/gtk mark the whole toplevel opaque).
                    // a multi-rect or subtractive region just disables the
                    // shortcut; the alpha channel still decides. (satoru)
                    if (args_len < 4) return;
                    uint32_t rid = get32(args);
                    obj->opq_valid = false;
                    if (rid == 0) break;
                    Object* reg = find_obj(c, rid);
                    if (reg && reg->iface == WL_REGION && reg->reg_adds == 1 &&
                        reg->reg_subs == 0 && reg->reg_x1 > reg->reg_x0 &&
                        reg->reg_y1 > reg->reg_y0) {
                        obj->opq_x = reg->reg_x0; obj->opq_y = reg->reg_y0;
                        obj->opq_w = reg->reg_x1 - reg->reg_x0;
                        obj->opq_h = reg->reg_y1 - reg->reg_y0;
                        obj->opq_valid = true;
                    }
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
                    if (g_wltrace) {
                        uint32_t cnow = Timer::GetRealMs();
                        if (obj->commit_n == 0) obj->commit_first_ms = cnow;
                        obj->commit_last_ms = cnow;
                        obj->commit_n++;
                        if (c->commit_n == 0) c->commit_first_ms = cnow;
                        c->commit_last_ms = cnow;
                        c->commit_n++;
                    }
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
                bool kb_enter_after = (c->keyboard_focus_sid != 0);   // focus came first (satoru)
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
                    uint32_t km_len = 0;
                    const uint8_t* km_base = keymap_backing(&km_len);
                    put32(km + 0, kid);
                    put16(km + 4, 0);                       // wl_keyboard.keymap
                    put16(km + 6, 16);                      // header+8 payload
                    put32(km + 8, 1);                       // format XKB_V1
                    put32(km + 12, km_len);                 // size incl. nul
                    UnixSocket::ControlMsg kcm = {};
                    kcm.passed_fd_count   = 1;
                    kcm.passed_fds[0]     = 1;              // sender-fd slot, unused (satoru)
                    kcm.passed_shm_base[0] = (uint64_t)(uintptr_t)km_base;
                    kcm.passed_shm_size[0] = km_len;
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
                if (kb_enter_after && find_obj(c, c->keyboard_focus_sid))
                    keyboard_enter(c, kid, c->keyboard_focus_sid);   // (satoru)
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
    g_now_ms = Timer::GetRealMs();   // one clock for every frame-callback path (satoru)
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
    void SetChildAlpha(bool on) { g_wl_childalpha = on; }
    void SetScissorDirectBlit(bool on) { g_wl_scdirect = on; }
    void SetDecoration(bool on) { g_wl_deco = on; }
    void SetFullsize(bool on) { g_wl_fullsize = on; }
    void SetPoolFree(bool on) { g_wl_poolfree = on; }
    void PoolCensus(int* pools_live, int* pools_made, uint64_t* pool_kb,
                    int* bufs_live, int* bufs_made, uint64_t* buf_kb,
                    uint64_t* shadow_kb) {
        if (pools_live) *pools_live = (int)g_pp_pools_live;
        if (pools_made) *pools_made = (int)g_pp_pools_made;
        if (pool_kb)    *pool_kb = g_pp_pool_kb;
        if (bufs_live)  *bufs_live = (int)g_pp_bufs_live;
        if (bufs_made)  *bufs_made = (int)g_pp_bufs_made;
        if (buf_kb)     *buf_kb = g_pp_buf_kb;
        if (shadow_kb)  *shadow_kb = g_shadow_bytes >> 10;
    }

void Init() {
    if (g_ncap == 0) cl_grow();   // first chunk up front (satoru)
    for (int i = 0; i < g_ncap; i++) (*cl_ref(i)) = nullptr;

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
    ClipboardManager::SetChangeHook([](bool primary) {
        WaylandServer::NotifySystemClipboardChanged(primary);
    });

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
    // same clock as the commit-time delivery: Time::GetTicks() is not ms since
    // boot, so callbacks fired here carried a wildly different timestamp. (satoru)
    g_now_ms = Timer::GetRealMs();
    clip_pump_all();   // drain selection pulls / pending offer writes (satoru)
    uint8_t pl[4];
    put32(pl, g_now_ms);                         // monotonic ms timestamp
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use) continue;
        // Liveness probe - KernelInject returns -1 if the sd has been
        // closed beneath us (no on_disconnect callback exists).
        // a zero-length inject succeeds even after the peer closed, so ask the
        // socket layer directly: a dead client must drop its surfaces here. (satoru)
        if (UnixSocket::PeerClosed(c->sd) || UnixSocket::KernelInject(c->sd, "", 0) < 0) {
            // drop_client also closes the server end of the connection and frees the
            // slot: clearing in_use alone left that socket in use forever, one per
            // app run, until the unix table and its 256 service slots ran out. (satoru)
            drop_client(c);
            continue;
        }
        // a frame callback fires after its surface's commit (delivered there,
        // in order). what is still queued belongs to a surface that has not
        // committed since asking, so it stays queued (spec: it must not fire
        // yet). only drop dead entries. callbacks with no recorded surface keep
        // the old behaviour. (satoru)
        {
            SpinLockGuard g(g_fcb_lock);   // (satoru)
            uint32_t keep[WL_MAX_FRAME_CB]; int nk = 0;
            while (c->frame_cb_tail != c->frame_cb_head) {
                uint32_t cb = c->frame_cb[c->frame_cb_tail];
                c->frame_cb_tail = (c->frame_cb_tail + 1) % WL_MAX_FRAME_CB;
                if (cb == 0) continue;
                Object* o = find_obj(c, cb);
                if (!o || o->iface != WL_CALLBACK) continue;
                Object* so = o->parent_surface_id ? find_obj(c, o->parent_surface_id) : nullptr;
                if (so && so->iface == WL_SURFACE) { if (nk < WL_MAX_FRAME_CB - 1) keep[nk++] = cb; continue; }
                if (o->parent_surface_id) continue;   // its surface is gone: never fires (satoru)
                send_event(c, cb, 0, pl, 4);
                destroy_obj(c, cb);
            }
            // re-queue in order (lock already held; nk < ring size). (satoru)
            for (int k = 0; k < nk; k++) {
                c->frame_cb[c->frame_cb_head] = keep[k];
                c->frame_cb_head = (c->frame_cb_head + 1) % WL_MAX_FRAME_CB;
            }
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

void NotifySystemClipboardChanged(bool primary) {
    SpinLockGuard g(g_clip_lock);
    SerialLogger::Log("[clip] system changed primary="); SerialLogger::LogDec(primary ? 1 : 0);
    SerialLogger::Log("\r\n");
    ClipSelection& sel = clip_sel(primary);
    if (sel.active && !sel.system) {
        Client* oc = client_for_sd(sel.sd);
        if (oc) {
            uint16_t op = primary ? 1 : 2;
            send_event(oc, sel.source_id, op, nullptr, 0);
            tx_flush(oc);
        }
    }
    clip_cancel_pulls(primary, 0);
    ClipMimeList mimes;
    clip_mimes_from_system(primary, mimes);
    sel.active = mimes.count > 0;
    sel.system = true;
    sel.sd = 0;
    sel.source_id = 0;
    sel.source_iface = 0;
    sel.serial = ++clip_serial(primary);
    sel.mimes = mimes;
    clip_broadcast_locked(primary);
}

bool ClipboardGetForOffer(int sd, uint32_t offer_id, const char* mime,
                          uint8_t** out, uint32_t* out_len) {
    if (out) *out = nullptr;
    if (out_len) *out_len = 0;
    SpinLockGuard g(g_clip_lock);
    ClipOfferInfo* oi = clip_find_offer(sd, offer_id);
    if (!oi || !mime) return false;
    ClipSelection& sel = clip_sel(oi->primary);
    if (!sel.active || oi->serial != sel.serial) return false;
    return ClipboardManager::CopyData(mime, out, out_len);
}

void DamageAll() {
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use) continue;
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
    for (int i = 0; i < g_ncap; i++) if ((*cl_ref(i)) && (*cl_ref(i))->in_use) n++;
    return n;
}

int SurfaceCount() {
    int n = 0;
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use) continue;
        for (int j = 0; j < c->object_high; j++) {
            if (c->objects[j].in_use &&
                c->objects[j].iface == WL_SURFACE) n++;
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
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use || c->pointer_id_count == 0) continue;
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
    // a press outside a client with a grabbing popup (another app or the
    // desktop) dismisses its popups, newest first, like any wayland
    // compositor; presses inside the same client are left to the client,
    // which rolls its own menus up. (satoru)
    if (pressed) {
        for (int ci = 0; ci < g_ncap; ci++) {
            Client* c = (*cl_ref(ci));
            if (!c || !c->in_use || c == fc) continue;
            bool sent = false;
            for (int guard = 0; guard < 16; guard++) {
                Object* top = nullptr;
                for (int i = 0; i < c->object_high; i++) {
                    Object& p = c->objects[i];
                    if (!p.in_use || p.iface != WL_SURFACE || !p.popup_grab) continue;
                    if (!popup_live(c, &p)) { p.popup_grab = false; continue; }
                    if (!top || p.popup_seq > top->popup_seq) top = &p;
                }
                if (!top) break;
                top->popup_grab = false;
                send_event(c, top->popup_obj, 1, nullptr, 0);   // xdg_popup.popup_done
                sent = true;
            }
            if (sent) { tx_flush(c); if (c->fatal) drop_client(c); }
        }
    }
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
        // focus goes to the clicked surface's TOPLEVEL (see set_keyboard_focus);
        // a click in a subsurface, tooltip or popup of the focused window
        // therefore changes nothing - firefox drives its menus from the
        // window's own key events. (satoru)
        int pox = 0, poy = 0; bool in_popup = false;
        Object* froot = surface_tree_root(fc, surf, pox, poy, in_popup);
        if (froot) set_keyboard_focus(fc, froot->id);
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

bool RequestClose(int win_id) {
    int sd; uint32_t sid;
    if (!winmap_get(win_id, sd, sid)) return false;
    Client* c = client_for_sd(sd);
    if (!c) return false;
    Object* s = find_obj(c, sid);
    if (!s || s->iface != WL_SURFACE || (int)(intptr_t)s->wm_window != win_id) return false;
    uint32_t now = Timer::GetRealMs();
    if (s->tl_close_ms && now - s->tl_close_ms < 5000) return false;   // asked already: force (satoru)
    for (int i = 0; i < c->object_high; i++) {
        Object& t = c->objects[i];
        if (!t.in_use || t.iface != XDG_TOPLEVEL || t.parent_surface_id != sid) continue;
        send_event(c, t.id, 1, nullptr, 0);   // xdg_toplevel.close (satoru)
        SerialLogger::Log("[wl] close requested, window "); SerialLogger::LogDec(win_id);
        SerialLogger::Log("\r\n");
        tx_flush(c);
        s->tl_close_ms = now ? now : 1;
        return true;
    }
    return false;
}

void NotifyWmFocus(int win_id) {
    int sd; uint32_t sid;
    // a native window: nothing to do - keys only reach ForwardKey while a
    // wayland window has the wm focus. (satoru)
    if (!winmap_get(win_id, sd, sid)) return;
    Client* c = client_for_sd(sd);
    if (!c) return;
    Object* s = find_obj(c, sid);
    // the surface must still own this window: a dead client's sd can be reused
    // by a new connection with an unrelated surface under the same id. (satoru)
    if (!s || s->iface != WL_SURFACE || (int)(intptr_t)s->wm_window != win_id) return;
    set_keyboard_focus(c, sid);
    tx_flush(c);   // a fatal flush is dropped by the dispatch loop (satoru)
}

bool IsWaylandWindow(int win_id) {
    int sd; uint32_t sid;
    return winmap_get(win_id, sd, sid);
}

// one connection by its server-side sd: the vm bridge's guest clients all
// share the vm host's pid, so they close one by one when a guest app goes. (satoru)
int DropClientOfSd(int sd) {
    if (sd < 0) return 0;
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use || c->sd != sd) continue;
        int high = c->object_high;
        for (int j = 0; j < high; j++) {
            Object& o = c->objects[j];
            if (o.in_use && o.iface == WL_SURFACE && o.wm_window)
                destroy_obj(c, o.id);
        }
        drop_client(c);
        return 1;
    }
    return 0;
}

int DropClientsOfPid(uint32_t pid) {
    if (pid == 0) return 0;
    int dropped = 0;
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use || c->sd < 0) continue;
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
    for (int i = 0; i < g_ncap; i++) {
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use || c->keyboard_id_count == 0) continue;
        if (c->keyboard_focus_sid == 0) continue;
        if (!find_obj(c, c->keyboard_focus_sid)) { c->keyboard_focus_sid = 0; continue; }
        kbds_modifiers(c, mods);
        if (evdev != 0)
            kbds_key(c, time, evdev, pressed);
        tx_flush(c);
        if (c->fatal) drop_client(c);
    }
}

void DumpProtocolTrace() {   // (satoru)
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    for (int i = 0; i < g_ncap; i++) {   // unread bytes on each client socket (satoru)
        Client* c = (*cl_ref(i));
        if (!c || !c->in_use) continue;
        SerialLogger::Log("[wlpend] sd="); SerialLogger::LogDec(c->sd);
        SerialLogger::Log(" client_unread="); SerialLogger::LogDec(UnixSocket::PeerPendingBytes(c->sd));
        SerialLogger::Log(" server_unread="); SerialLogger::LogDec(UnixSocket::PendingBytes(c->sd));
        SerialLogger::Log(" tx_len="); SerialLogger::LogDec(c->tx_len);
        SerialLogger::Log("\r\n");
    }
    UnixSocket::DumpPending();
    uint32_t n = g_wlt_n;
    for (uint32_t i = 0; i < n; i++) {
        const WltEnt* e = &g_wlt[i];
        SerialLogger::Log(e->dir ? "[wle] t=" : "[wlr] t="); SerialLogger::LogDec((int)e->ms);
        SerialLogger::Log(" sd="); SerialLogger::LogDec((int)e->sd);
        SerialLogger::Log(" id="); SerialLogger::LogDec((int)e->id);
        SerialLogger::Log(" if="); SerialLogger::LogDec(e->iface == 0xFFFF ? -1 : (int)e->iface);
        SerialLogger::Log(" op="); SerialLogger::LogDec((int)e->op);
        SerialLogger::Log(" a0="); SerialLogger::LogDec((int)e->a0);
        SerialLogger::Log(" a1="); SerialLogger::LogDec((int)e->a1);
        SerialLogger::Log("\r\n");
    }
    g_wlt_n = WLT_SLOTS;   // dump once (satoru)
}
}  // namespace WaylandServer
