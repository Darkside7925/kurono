#ifndef KURONO_UI_WAYLAND_SERVER_H
#define KURONO_UI_WAYLAND_SERVER_H

#include "../kernel/types.h"

// Wayland compositor server.
//
// Listens at /system/run/user/1000/wayland-0 (the canonical
// $WAYLAND_DISPLAY socket) and speaks the wire protocol described by
// libwayland.  Per RFC the on-the-wire format of every message is:
//
//   uint32  object_id
//   uint16  opcode
//   uint16  size_bytes
//   uint8[] arguments              (size - 8 bytes)
//
// Object id 1 is hardcoded as wl_display.  We register the following
// globals on first wl_registry::get_globals() call:
//
//   wl_compositor          v5
//   wl_subcompositor       v1
//   wl_shm                 v1
//   wl_output              v3
//   wl_seat                v7   (with wl_pointer/wl_keyboard/wl_touch)
//   wl_data_device_manager v3
//   xdg_wm_base            v3   (xdg_surface, xdg_toplevel, xdg_popup)
//   zwp_linux_dmabuf_v1    v3
//   zxdg_decoration_manager_v1 v1
//
// Surfaces are bridged into the Kurono window manager: each xdg_toplevel
// becomes a `WMWindow` and each wl_buffer attach becomes a blit into
// the BGA framebuffer.
//
// The compositor process model is in-kernel: the listening socket has a
// kernel-side data callback (UnixSocket::RegisterServer) so the wire
// protocol is parsed in this module without forking a user-space
// compositor binary.

namespace WaylandServer {

    static const int WL_MAX_CLIENTS  = 16;
    static const int WL_MAX_OBJECTS  = 256;
    static const int WL_MAX_GLOBALS  = 16;

    // Outbound TX scratch per client.  Sized so that an entire batch of
    // wl_registry::global events (the worst inline burst) coalesces into
    // a single KernelInject call.
    static const int WL_TX_SCRATCH   = 4096;

    // Pending frame callbacks per client - drained on vsync.
    static const int WL_MAX_FRAME_CB = 64;

    enum InterfaceId : uint16_t {
        WL_DISPLAY            = 1,
        WL_REGISTRY           = 2,
        WL_COMPOSITOR         = 3,
        WL_SUBCOMPOSITOR      = 4,
        WL_SHM                = 5,
        WL_OUTPUT             = 6,
        WL_SEAT               = 7,
        WL_DATA_DEVICE_MGR    = 8,
        XDG_WM_BASE           = 9,
        ZWP_LINUX_DMABUF      = 10,
        ZXDG_DECORATION_MGR   = 11,
        WL_SURFACE            = 12,
        WL_BUFFER             = 13,
        WL_POINTER            = 14,
        WL_KEYBOARD           = 15,
        WL_TOUCH              = 16,
        XDG_SURFACE           = 17,
        XDG_TOPLEVEL          = 18,
        WL_CALLBACK           = 19,
        WL_SHM_POOL           = 20,
        WL_SUBSURFACE         = 21,
        WL_REGION             = 22,
    };

    struct DamageRect {
        int32_t x, y, w, h;
        bool    valid;
    };

    struct Object {
        uint32_t id;
        uint16_t iface;
        uint16_t version;
        bool     in_use;
        uint32_t parent_surface_id;
        uint32_t subsurface_parent;   // for WL_SUBSURFACE: the parent wl_surface id (satoru)
        int32_t  subsurface_x, subsurface_y;  // wl_subsurface.set_position offset in parent (satoru)
        // For wl_surface objects: the bound Kurono WMWindow handle.
        void*    wm_window;
        int      width, height;
        // Accumulated damage bounding box for wl_surface, reset on commit.
        DamageRect damage;
        // For wl_surface: id of the pending frame callback (0 if none).
        uint32_t pending_frame_cb;

        // shm pool backing: base pointer + byte size of the mapped pool.
        // populated by RegisterPoolMemory() once the fd transport supplies
        // a kernel-visible mapping; nullptr means "not yet mappable". (satoru)
        const uint8_t* pool_base;
        uint32_t       pool_size;
        // for wl_buffer: geometry into its parent pool. (satoru)
        uint32_t buf_pool_id;
        uint32_t buf_offset;
        uint32_t buf_stride;
        uint32_t buf_format;     // wl_shm format code (0 = ARGB8888)
        // for wl_surface: the buffer id attached via wl_surface.attach,
        // applied on the next commit, plus the attach hotspot offset and a
        // per-surface alpha (0..255, 255 = opaque). (satoru)
        uint32_t pending_buffer_id;
        int32_t  attach_x, attach_y;
        uint8_t  surf_alpha;
        bool     mapped;         // true once bridged to a wm window. (satoru)
        // per-surface SHADOW buffer: server-side accumulation of the client's
        // committed pixels. damage-incremental clients (firefox SW-WR) attach a
        // FRESH mostly-zero buffer each frame and paint only the damaged strip
        // into it; blitting the raw attached buffer therefore wipes the window
        // with zeros (the black-firefox bug). on commit the damaged region is
        // copied client-buffer -> shadow, and every blit reads the SHADOW, so a
        // full repaint (wm render thunk / damage-less commit) always shows the
        // accumulated picture. raw client pixel format, stride = shadow_w*4. (satoru)
        uint8_t* shadow;
        int32_t  shadow_w, shadow_h;
        uint32_t shadow_fmt;     // wl_shm format of the shadow pixels (satoru)
        bool     shadow_filled;  // first full copy done - partial copies valid after (satoru)
        // for wl_surface: window title from xdg_toplevel.set_title (stored on
        // the toplevel's parent surface, since the surface owns the bridged wm
        // window) plus an app-icon id resolved from set_app_id/set_title so the
        // wm titlebar + taskbar can show the app's own logo. 0xFF = no hint. (satoru)
        char     title[48];
        uint8_t  app_icon_hint;
        // for wl_surface: xdg_surface.set_window_geometry - the visible window
        // rect within the surface buffer (excludes csd shadow margins). blits
        // shift by -geo_x/-geo_y and the window is sized geo_w x geo_h, so the
        // margins never paint (they showed as a black band around firefox).
        // geo_w == 0 means never set (use the full buffer). (satoru)
        int32_t  geo_x, geo_y;
        int32_t  geo_w, geo_h;
        // wl_surface.set_input_region: true when the client set an EMPTY
        // input region = "input passes through me". firefox's webrender
        // output subsurfaces do exactly this - they are pure output layers
        // with no event listener, so pointer/keyboard events targeted at
        // them vanish inside the client (the every-input-dead-in-firefox
        // bug). hit-testing must skip such surfaces and fall through to the
        // real gtk toplevel underneath. (satoru)
        bool     input_none;
        // for WL_REGION objects: whether any wl_region.add arrived - an
        // empty region on set_input_region is the pass-through marker. (satoru)
        bool     region_has_rect;
    };

    struct Client {
        bool     in_use;
        int      sd;
        uint32_t generation;     // bumped each (re)allocation, detects stale sd reuse
        Object   objects[WL_MAX_OBJECTS];
        int      object_high;    // highest slot ever used + 1 (lookup bound)
        uint8_t  rx_partial[8192];
        int      rx_partial_len;
        uint8_t  tx_scratch[WL_TX_SCRATCH];
        int      tx_len;
        uint32_t frame_cb[WL_MAX_FRAME_CB];
        int      frame_cb_head;
        int      frame_cb_tail;
        uint32_t serial_next;
        bool     fatal;          // protocol error - drop on next dispatch

        // input: ids of ALL wl_pointer / wl_keyboard resources this client
        // created via wl_seat.get_pointer / get_keyboard. firefox binds the
        // seat TWICE (gtk/gdk once, gecko's native-layer compositor once) and
        // the old single-slot tracking kept only the LAST bind - every input
        // event then went to gecko's listener-less proxy and gdk never saw a
        // thing (the all-input-dead-in-firefox bug). a real compositor
        // broadcasts each event to every resource of the seat. slot 0 kept as
        // pointer_id/keyboard_id aliases via helpers in the cpp. (satoru)
        static const int WL_MAX_INPUT_RES = 8;
        uint32_t pointer_ids[WL_MAX_INPUT_RES];
        int      pointer_id_count;
        uint32_t keyboard_ids[WL_MAX_INPUT_RES];
        int      keyboard_id_count;
        // legacy single-id views: first live resource (0 = none). still used
        // by the quick has-a-pointer checks. (satoru)
        uint32_t pointer_id;
        uint32_t keyboard_id;
        // id of the wl_surface currently holding pointer / keyboard focus
        // for this client, so we can emit leave before the next enter. (satoru)
        uint32_t pointer_focus_sid;
        uint32_t keyboard_focus_sid;
    };

    void Init();

    // Listening socket descriptor for /system/run/user/1000/wayland-0.
    int  ListenSd();

    // Per-frame helpers - called by the compositor render loop.
    void DispatchPendingFrames();
    void DamageAll();

    int  ClientCount();
    int  SurfaceCount();

    // ── shm pool backing registration ─────────────────────────────────
    // The in-kernel compositor cannot see a client's wl_shm pool until the
    // SCM_RIGHTS fd that backs it is resolved to a kernel-visible mapping.
    // That resolution lives in the socket / memfd layer (outside this
    // module); once it has a base pointer + size it calls this to attach
    // the mapping to the pool object so commit-time blits read real pixels.
    // sd = client socket descriptor, pool_id = wl_shm_pool object id. The
    // pointer must stay valid for the pool's lifetime. (satoru)
    void RegisterPoolMemory(int sd, uint32_t pool_id,
                            const uint8_t* base, uint32_t size);

    // ── input forwarding entry points ─────────────────────────────────
    // Called by the input system (mouse / keyboard pump) to deliver host
    // input to whichever Wayland surface currently sits under the pointer
    // / holds keyboard focus.  Coordinates are global (screen) pixels;
    // surface-local translation happens inside.  buttons use Linux evdev
    // codes (BTN_LEFT=0x110 ...); `key` is a Kurono Key enum value (see
    // drivers/keyboard.h) which is translated to a Linux evdev keycode on
    // the wire; mods is a shift/ctrl/alt/meta bitmask. (satoru)
    void ForwardPointerMotion(int gx, int gy);
    void ForwardPointerButton(int gx, int gy, int evdev_button, bool pressed);
    // axis 0 = vertical scroll; value is in surface pixels (converted to
    // wl_fixed on the wire). positive scrolls the content down. (satoru)
    void ForwardPointerAxis(int gx, int gy, int axis, int value);
    void ForwardKey(int key, bool pressed, uint32_t mods);

    // true when the wm window id belongs to a bridged wayland surface - the
    // desktop input path uses this to route raw key edges to ForwardKey
    // instead of the kurono char pipeline. (satoru)
    bool IsWaylandWindow(int win_id);

    // drop every client whose socket peer is the given (killed) process:
    // closes its bridged wm windows and frees the client slot, so a task
    // manager kill visibly closes the app instead of leaving a frozen husk
    // on screen. returns the number of clients dropped. (satoru)
    int DropClientsOfPid(uint32_t pid);

    // Linux evdev button codes for ForwardPointerButton(). (satoru)
    static const int WL_BTN_LEFT   = 0x110;
    static const int WL_BTN_RIGHT  = 0x111;
    static const int WL_BTN_MIDDLE = 0x112;

    // modifier bitmask bits for ForwardKey()'s `mods` argument. (satoru)
    static const uint32_t WL_MOD_SHIFT = 1u << 0;
    static const uint32_t WL_MOD_CTRL  = 1u << 1;
    static const uint32_t WL_MOD_ALT   = 1u << 2;
    static const uint32_t WL_MOD_META  = 1u << 3;
}

#endif
