# Browser (the placeholder)

`src/apps/browser.cpp` and `browser.h`. This is the GUI "Browser" tile, and it is a
deliberate placeholder, not a browser.

## What it actually is

About 70 lines. `Open()` makes a small window titled "Browser (Removed)" and prints
"Browser Removed" plus "Use terminal: `curl <url>`". No address bar, no fetch path, no
renderer.

On purpose. There used to be an in-tree HTML/HTTP client called KBrowse (~1,260 lines)
and I pulled it out. The old argument was "no C++ browser compiles for a freestanding
OS", which is no longer where this project stands.

## The real browser answer

Firefox on the Linux runtime. Not a hand-written engine.

- Real Firefox 140.11.0esr built against musl and Wayland (`cairo-gtk3-wayland`,
  software WebRender, ~174MB `libxul.so`), build rc=0.
- The Linux runtime already gives it what it wants: pthreads/futex, epoll/poll,
  `mprotect` W^X, real `fcntl` byte-range locks, AF_UNIX with `SCM_RIGHTS`, and an
  in-kernel Wayland compositor that composites real `wl_shm` clients. Including
  `wl_subsurface` compositing, which is how GTK insets Firefox's content into its
  client-side-decorated toplevel.
- The launcher (`src/apps/firefox_launcher.cpp`) really `execve`s the rootfs Firefox
  binary through the Linux syscall layer.
- Firefox composites its real chrome on the desktop. `ld-kurono` resolves libxul's
  full `.so` closure and relocates it at a multi-terabyte base. It runs a real profile
  single-process (e10s off) with threads across the secondary cores
  (`kurono.apthreads=1`, `-smp 4`), and the compositor puts its 973x743 GTK content
  subsurface on the 1025x795 CSD toplevel. Tab strip, URL bar, back/forward/reload,
  bookmark star, menus, window controls, all real pixels. The old sub-4GB pointer ABI
  cap is gone, so a high PIE base is fine now.

What's left for a page that reliably renders: the socket thread's poll-wakeup delivery
(NSPR's `PollableEvent`/eventfd against the kernel's fd readiness), plus some multicore
startup and render-timing flakiness. Not e10s, not address space, not ELF loading. The
tile stays a placeholder until page render is reliable.

## HTTP today

`curl <url>` in the terminal. Goes end to end through `src/net/tcpip.cpp`: real DNS,
TCP handshake, recv loop, FIN half-close. Fetched real pages off example.com and
wikipedia over tap+NAT. HTTPS isn't implemented, `curl` is HTTP only.

## Files

- `src/apps/browser.cpp` / `.h` the placeholder tile
- `src/apps/firefox_launcher.cpp` / `.h` the real Firefox `execve` launcher
- `src/linux/ld_kurono.cpp` the dynamic linker that loads libxul's closure
- `src/net/tcpip.cpp` the TCP/IP stack behind `curl`
- `src/ui/desktop.cpp` `LaunchBrowser()` entry point
