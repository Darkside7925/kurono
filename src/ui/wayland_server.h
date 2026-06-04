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
    };

    void Init();

    // Listening socket descriptor for /system/run/user/1000/wayland-0.
    int  ListenSd();

    // Per-frame helpers  -  called by the compositor render loop.
    void DispatchPendingFrames();
    void DamageAll();

    int  ClientCount();
    int  SurfaceCount();
}

#endif
