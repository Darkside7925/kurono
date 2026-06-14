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

**What's left to actually run it on-device** (the active frontier, *not* shipped):
bringing libxul's full `.so` dependency closure onto the OS and loading it through
[ld-kurono](../linux/LD_KURONO.md), and lifting the **<4 GB user-pointer ABI
limit** in the syscall layer. The GUI tile stays a placeholder until then.

## 3. The working HTTP path today

For HTTP from the OS today, use **`curl <url>`** in the terminal  -  it goes end to
end through the custom TCP/IP stack (`src/net/tcpip.cpp`): real DNS, TCP
handshake, recv loop, and FIN half-close, verified fetching real pages off
example.com / wikipedia over tap+NAT. HTTPS/TLS is not implemented; `curl` is
HTTP-only.

## 4. Related files

- `src/apps/browser.cpp` / `.h`  -  the placeholder tile
- `src/apps/firefox_launcher.cpp` / `.h`  -  the real Firefox `execve` launcher
- `src/linux/ld_kurono.cpp`  -  the dynamic linker that will load libxul's closure
- `src/net/tcpip.cpp`  -  the TCP/IP stack behind `curl`
- `src/ui/desktop.cpp`  -  `LaunchBrowser()` entry point
