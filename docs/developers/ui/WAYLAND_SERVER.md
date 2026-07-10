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

## 4. Surface bridging (working)

Each `xdg_toplevel` surface becomes a real `Window` in the Kurono window manager,
and each committed `wl_shm` buffer is blitted into that window - this is
**implemented, not stubbed**:

- `commit_surface()` → `bridge_surface_window()` calls
  `WindowManager::CreateWindow()` the first time a toplevel commits, then
  `blit_surface_region()` → `blit_argb()` copies the client's pixels (a
  `wl_shm` buffer fd-passed via `SCM_RIGHTS`) into the window's framebuffer rect,
  honouring damage.
- `DispatchPendingFrames()` and `DamageAll()` are real (paced from the desktop
  redraw), not no-ops.
- Pointer events are forwarded back to the focused client every frame via
  `ForwardPointerMotion()` / `ForwardPointerButton()` (called from `desktop.cpp`).
- **`wl_subsurface` compositing** is implemented: a client can inset a child
  surface into a parent at a `set_position` offset (`blit_child_subsurfaces`,
  `find_subsurface_role`), and the compositor composites the child onto the
  parent window at that offset. This is what a real GTK app needs - GTK renders
  its content into a subsurface inset inside a client-side-decorated toplevel - 
  and is what lets **Firefox composite its full browser chrome** (a 973×743 GTK
  content subsurface onto a 1025×795 CSD toplevel). A companion fix makes
  `UnixSocket::TakePendingControl` hand back one fd per call so every `wl_shm`
  pool's memfd maps individually.

This was proven with a real **musl-gcc**-compiled client,
`src/userprogs/wl_shm_test.c`, embedded in the kernel and launched by the
`wltest` shell command, and - at full scale - by Firefox 140.11.0esr painting
its real browser chrome through the subsurface path.

## 5. Limits

The compositor has these limits:

- Up to 16 clients
- Up to 256 objects per client
- 8 KB receive buffer per client

## 6. Current status

The compositor goes well past the wire protocol - the full `wl_shm` + xdg-shell
render path works:

- Registry and globals advertised correctly; surface creation + `xdg_toplevel`
  configure events work.
- A committed `wl_shm` buffer (fd-passed via `SCM_RIGHTS`) is blitted into a real
  WM window with damage tracking, and pointer enter/leave/motion/button events are
  forwarded back to the focused client. Verified end to end with a musl-compiled
  client (`wl_shm_test`, via `wltest`).

**Honest gaps that remain:**

- **Keyboard-to-client forwarding** (`ForwardKey`) is implemented but **not yet
  wired into the input loop**, so the focused client doesn't receive keystrokes
  yet.
- **`zwp_linux_dmabuf` GPU buffers are advertised but not composited** - only
  `wl_shm` software buffers are blitted today.

## 7. Related files

- `src/ui/window_manager.cpp` - intended target for `xdg_toplevel` bridge
- `src/drivers/graphics.cpp` - framebuffer blit target
- `src/system/input_manager.cpp` - input event forwarding source
