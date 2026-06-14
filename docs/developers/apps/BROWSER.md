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
  (`--disable-jit`, `cairo-gtk3-wayland`, ~174 MB `libxul.so`), build rc=0.
- The Linux runtime already provides what it targets: pthreads/`futex`,
  `epoll`/`poll`, `mprotect` W^X, AF_UNIX + `SCM_RIGHTS`, and an in-kernel Wayland
  compositor that composites real `wl_shm` clients.
- The launcher (`src/apps/firefox_launcher.cpp`) does a real `execve` of the
  rootfs Firefox binary through the Linux syscall/runtime layer.
- **The Gecko engine loads and runs on-device.** [ld-kurono](../linux/LD_KURONO.md)
  resolves libxul's full `.so` dependency closure and loads + relocates `libxul`
  (130 MB+) at a multi-terabyte base; XPCOM and Gecko's own application code
  execute, and Firefox's child processes spawn (via `clone3`). The syscall
  layer's old sub-4 GB pointer-ABI cap is **lifted**  -  user mappings span the full
  canonical 64-bit user range, so a high PIE base is no longer a blocker.

**What's left to render a Firefox window** (the active frontier, *not* shipped):
the **e10s multiprocess IPC** path between the parent and content processes, plus
a **musl symbol/threading** issue still being chased down. This is no longer an
address-space or ELF-loading limitation. The GUI tile stays a placeholder until
the window renders.

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
