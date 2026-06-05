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

    // Pending frame callbacks per client  -  drained on vsync.
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
        bool     fatal;          // protocol error  -  drop on next dispatch

        // input: ids of the wl_pointer / wl_keyboard resources this client
        // created via wl_seat.get_pointer / get_keyboard (0 = none). a
        // single seat is assumed so we track one of each. (satoru)
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

    // Per-frame helpers  -  called by the compositor render loop.
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
    void ForwardPointerAxis(int gx, int gy, int axis, int value);
    void ForwardKey(int key, bool pressed, uint32_t mods);

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
