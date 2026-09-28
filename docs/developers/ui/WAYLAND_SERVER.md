# Wayland server

`src/ui/wayland_server.cpp` and `wayland_server.h`. An in-kernel Wayland compositor that speaks
the real libwayland wire protocol.

It listens on `/system/run/user/1000/wayland-0`, the canonical `$WAYLAND_DISPLAY`. Real
libwayland binary wire protocol, not a simplified subset.

The unusual bit: on Linux the compositor (Mutter, wlroots) is a userspace process. Here it runs
in-kernel.

## Wire format

```
uint32  object_id
uint16  opcode
uint16  size_bytes
uint8[] arguments
```

Object ID 1 is hardcoded as `wl_display`. The server parses messages in this format directly.

## Globals

Advertised through `wl_registry`:

- `wl_compositor` v5
- `wl_subcompositor` v1
- `wl_shm` v1
- `wl_output` v3
- `wl_seat` v7, with `wl_pointer`, `wl_keyboard`, `wl_touch`
- `wl_data_device_manager` v3
- `xdg_wm_base` v3, with `xdg_surface`, `xdg_toplevel`, `xdg_popup`
- `zwp_linux_dmabuf_v1` v3
- `zxdg_decoration_manager_v1` v1

## Surface bridging

Each `xdg_toplevel` surface becomes a real `Window` in the window manager, and each committed
`wl_shm` buffer gets blitted into that window. Implemented, not stubbed:

- `commit_surface()` then `bridge_surface_window()` calls `WindowManager::CreateWindow()` the
  first time a toplevel commits, then `blit_surface_region()` and `blit_argb()` copy the
  client's pixels, a `wl_shm` buffer fd-passed through `SCM_RIGHTS`, into the window's
  framebuffer rect, honouring damage.
- `DispatchPendingFrames()` and `DamageAll()` are real, paced from the desktop redraw.
- Pointer events go back to the focused client every frame through `ForwardPointerMotion()` and
  `ForwardPointerButton()`, called from `desktop.cpp`.
- `wl_subsurface` compositing works: a client can inset a child surface into a parent at a
  `set_position` offset (`blit_child_subsurfaces`, `find_subsurface_role`), and the compositor
  puts the child on the parent window at that offset. That's what a real GTK app needs, since
  GTK renders its content into a subsurface inset inside a client-side-decorated toplevel, and
  it's what lets Firefox composite its full chrome: a 973x743 GTK content subsurface on a
  1025x795 CSD toplevel. A companion fix makes `UnixSocket::TakePendingControl` hand back one fd
  per call, so every `wl_shm` pool's memfd maps individually.

Proven with a real musl-gcc-compiled client, `src/userprogs/wl_shm_test.c`, embedded in the
kernel and launched by the `wltest` command, and at full scale by Firefox 140.11.0esr painting
its chrome through the subsurface path.

## Limits

- up to 16 clients
- up to 256 objects per client
- 8KB receive buffer per client

## What's still missing

- keyboard-to-client forwarding (`ForwardKey`) is implemented but not wired into the input loop
  yet, so the focused client doesn't receive keystrokes yet
- `zwp_linux_dmabuf` GPU buffers are advertised but not composited. Only `wl_shm` software
  buffers get blitted today.

## Files

- `src/ui/window_manager.cpp` the `xdg_toplevel` bridge target
- `src/drivers/graphics.cpp` the framebuffer blit target
- `src/system/input_manager.cpp` the input event source
