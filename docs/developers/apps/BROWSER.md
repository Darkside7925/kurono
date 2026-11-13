# Browser

`src/apps/browser.cpp` and `browser.h`. This is the old "Browser" tile, and it's not a
browser. It opens a small window titled "Browser (Removed)" that says the in-tree browser
is gone and points you at `curl <url>` in the terminal. It's about 70 lines and stays only so
old shortcuts don't break.

There used to be an in-tree HTML/HTTP client called KBrowse (~1,260 lines). I pulled it out
once real Firefox started working, because a hand-written engine was never going to catch
up.

## The real browser is Firefox

Firefox 140 ESR (musl + Wayland, software WebRender, a ~174 MB `libxul.so`) runs on the
Linux layer, unmodified. The desktop's Firefox icon and `kurono.gui.run=@firefox` both go
through `DesktopEnvironment::LaunchFirefox()`, which starts it detached, with no terminal
window.

- It installs from `FIREFOX.TAR` on the KURONO partition, so no network is needed, or
  downloads it with `kpkg`.
- It runs multi-process (e10s). Content processes are real forked processes, talking to the
  parent over Unix sockets with fd passing.
- `ld-kurono` resolves libxul's whole `.so` closure and relocates it at a high base.
- The in-kernel Wayland compositor shows its windows, including the GTK content subsurface
  inside the client-side-decorated toplevel, popups and menus.
- HTTPS works through Firefox's own NSS. Real sites load.
- It can run at the same time as Claude Code. A vfork bug used to corrupt Firefox's
  environment when Claude had fragmented memory first, and that's fixed in part 3.

## HTTP from the terminal

`curl <url>` works for quick fetches. It's a small shim over busybox `wget`, which handles
HTTPS too.

## Files

- `src/apps/browser.cpp` / `.h`: the old tile
- `src/apps/firefox_launcher.cpp` / `.h`: the Firefox launcher
- `src/packages/pkgmgr.cpp`: installing Firefox from `FIREFOX.TAR` or the network
- `src/linux/ld_kurono.cpp`: the dynamic linker that loads libxul's closure
- `src/ui/wayland_server.cpp`: the compositor side
- `src/ui/desktop.cpp`: `LaunchBrowser()` and `LaunchFirefox()`
