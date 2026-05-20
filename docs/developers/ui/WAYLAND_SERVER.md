# Wayland Server

`src/ui/wayland_server.cpp` and `wayland_server.h` implement an in-kernel Wayland compositor that speaks the libwayland wire protocol.

## 1. What it is

The Wayland server is a compositor that listens at `/system/run/user/1000/wayland-0` (the canonical `$WAYLAND_DISPLAY`). It speaks the real libwayland binary wire protocol, not a simplified subset.

This is unusual compared to traditional Linux systems where the compositor (e.g., Mutter, wlroots) runs as a user-space process. In Kurono, the compositor runs in-kernel.

## 2. Wire protocol

The on-the-wire format for every message is:

```
uint32  object_id
uint16  opcode
uint16  size_bytes
uint8[] arguments
```

Object ID 1 is hardcoded as `wl_display`. The server parses messages in this format directly.

## 3. Registered globals

The compositor advertises the following globals via `wl_registry`:

- `wl_compositor` v5
- `wl_subcompositor` v1
- `wl_shm` v1
- `wl_output` v3
- `wl_seat` v7 (with `wl_pointer`, `wl_keyboard`, `wl_touch`)
- `wl_data_device_manager` v3
- `xdg_wm_base` v3 (with `xdg_surface`, `xdg_toplevel`, `xdg_popup`)
- `zwp_linux_dmabuf_v1` v3
- `zxdg_decoration_manager_v1` v1

## 4. Surface bridging

The intended design is that each `xdg_toplevel` surface becomes a `Window` in the Kurono window manager. Each `wl_buffer` attach becomes a blit into the framebuffer.

The bridge is currently stubbed. `DispatchPendingFrames()` and `DamageAll()` are placeholder no-ops.

## 5. Limits

The compositor has these limits:

- Up to 16 clients
- Up to 256 objects per client
- 8 KB receive buffer per client

## 6. Current status

The compositor is functional at the wire-protocol level:

- Registry and globals are advertised correctly
- Surface creation works
- `xdg_toplevel` configure events are sent

However, the surface-to-framebuffer blit bridge and input event forwarding are still stubbed. A real Wayland client can connect and negotiate surfaces, but the pixels do not yet appear on screen.

## 7. Related files

- `src/ui/window_manager.cpp`  -  intended target for `xdg_toplevel` bridge
- `src/drivers/graphics.cpp`  -  framebuffer blit target
- `src/system/input_manager.cpp`  -  input event forwarding source
