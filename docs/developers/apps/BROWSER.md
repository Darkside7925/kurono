# Browser App

`src/apps/browser.cpp` and `browser.h` implement the GUI "Browser" tile  -  which
is a **deliberate placeholder**, not a working browser.

## 1. What it actually is

The current `browser.cpp` is a ~70-line stub. `Open()` creates a small window
titled **"Browser (Removed)"** that just shows the text **"Browser Removed"** and
**"Use terminal: `curl <url>`"**. There is no address bar, no fetch path, and no
renderer in this file.

This is intentional. An earlier in-tree HTML/HTTP client (the ~1,260-line
"KBrowse") was removed and replaced with this stub  -  see `STATUS.md` →
*Browser Removed*. The historical reasoning ("no C++ browser compiles for a
freestanding OS") is no longer the project's stance.

## 2. The real browser strategy

Kurono's browser answer is **Firefox on the Linux runtime**, not a hand-written
freestanding engine:

- A real **Firefox 140.11.0esr** is cross-compiled against **musl + Wayland**
  (`cairo-gtk3-wayland`, software WebRender, ~174 MB `libxul.so`), build rc=0.
- The Linux runtime already provides what it targets: pthreads/`futex`,
  `epoll`/`poll`, `mprotect` W^X, real `fcntl` byte-range locks, AF_UNIX +
  `SCM_RIGHTS`, and an in-kernel Wayland compositor that composites real `wl_shm`
  clients  -  including **`wl_subsurface` compositing** (a child surface composited
  onto its parent at its `set_position` offset), which is what GTK uses to inset
  Firefox's content into its client-side-decorated toplevel.
- The launcher (`src/apps/firefox_launcher.cpp`) does a real `execve` of the
  rootfs Firefox binary through the Linux syscall/runtime layer.
- **Firefox composites its real browser chrome on the desktop.**
  [ld-kurono](../linux/LD_KURONO.md) resolves libxul's full `.so` closure and
  loads + relocates `libxul` at a multi-terabyte base; Firefox runs a real
  profile **single-process** (e10s off) with its threads dispatched across the
  secondary cores (`kurono.apthreads=1`, `-smp 4`), and the compositor composites
  its 973×743 GTK content subsurface onto the 1025×795 CSD toplevel  -  tab strip,
  URL bar, back/forward/reload, bookmark star, account/extensions/hamburger menus,
  window controls, all real pixels (`0xFFF9F9FB` ARGB8888). The syscall layer's
  old sub-4 GB pointer-ABI cap is **lifted**, so a high PIE base is no longer a
  blocker.

**What's left for a reliably rendered web page** (the active frontier, *not* fully
shipped): navigation reaches necko, and the current fix is the **socket thread's
poll-wakeup delivery** (NSPR's `PollableEvent`/eventfd wakeup vs the kernel's fd
readiness), plus some multicore startup/render-timing flakiness. This is no longer
an e10s, address-space, or ELF-loading limitation  -  Firefox runs single-process
and the chrome already paints. The GUI tile stays a placeholder until the page
render is reliable.

## 3. The working HTTP path today

For HTTP from the OS today, use **`curl <url>`** in the terminal  -  it goes end to
end through the custom TCP/IP stack (`src/net/tcpip.cpp`): real DNS, TCP
handshake, recv loop, and FIN half-close, verified fetching real pages off
example.com / wikipedia over tap+NAT. HTTPS/TLS is not implemented; `curl` is
HTTP-only.

## 4. Related files

- `src/apps/browser.cpp` / `.h`  -  the placeholder tile
- `src/apps/firefox_launcher.cpp` / `.h`  -  the real Firefox `execve` launcher
- `src/linux/ld_kurono.cpp`  -  the dynamic linker that loads libxul's closure
- `src/net/tcpip.cpp`  -  the TCP/IP stack behind `curl`
- `src/ui/desktop.cpp`  -  `LaunchBrowser()` entry point
