# Kurono OS roadmap

This is the honest, living plan for where Kurono is and where it is going. It is
deliberately blunt about what works, what half-works, and what is not started.
Dates are not promised; the ordering reflects priority, not a schedule.

Status legend: **[done]** shipped and verified, **[wip]** actively in progress,
**[next]** queued, **[later]** wanted but not started.

---

## Where it is today (the foundation, mostly done)

These are built and verified enough to build on. They still have rough edges.

- **[done] Boot + install.** Multiboot2 kernel, standalone EFI loaders, emergency
  recovery boot, GRUB menu, graphical installer / first-run wizard, FAT32/ext4
  install targets. Day-to-day dev runs on QEMU/KVM; bare metal is the target.
- **[done] Desktop.** Compositing window manager, lock screen, control center,
  notifications, the KSS theme + animation engine, the KJ scripting interpreter,
  a settings app, terminal, files, editor, calculator, task manager, media player.
- **[done] Linux runtime (KLS).** A Linux syscall layer, the in-kernel `ld-kurono`
  dynamic linker, real `CLONE_THREAD` threads + `futex`, `epoll`/`poll`,
  `mprotect` with W^X, `memfd` + file-backed `mmap`, `SCM_RIGHTS` fd-passing, and
  an in-kernel Wayland compositor that composites real musl-compiled Wayland
  clients on screen.
- **[done] Networking.** Full TCP/IP stack over Intel E1000; `curl`/HTTP works end
  to end (verified against real sites); a multi-segment TCP send window.
- **[done] Storage.** NVMe driver, KFS on-disk filesystem that survives reboot,
  KVFS as the runtime filesystem.
- **[done] SMP.** Multi-core bring-up, secondary cores run real ring-3 user
  processes, per-AP LAPIC-timer preemption.
- **[done] Audio, GPU, hypervisor, security.** HDA/AC97 audio; virtio-gpu
  accelerated display plus a multi-backend display manager; a Type-1 hypervisor;
  KSA hypervisor-backed privilege prompts; `supr` escalation; a real memory-safety
  audit pass.

---

## Now: the active frontier

### Firefox: from painted chrome to a reliably painted page

A real Firefox 140.11.0esr (musl + Wayland, software WebRender) **loads, runs its
Gecko engine, and now composites its real browser chrome on the Kurono desktop**  - 
tab strip, URL bar, back/forward/reload, bookmark star, account/extensions/
hamburger menus, window controls. It runs **single-process** (e10s off), so the
old "the content process is blocked in the e10s IPC path" framing is retired. The
headline goal now is a **reliably rendered web page**: navigation reaches necko,
and the socket thread's poll-wakeup is the active fix.

- **[done] `wl_subsurface` compositing** (`src/ui/wayland_server.cpp`). GTK renders
  Firefox's UI into a 973×743 content subsurface inset inside a 1025×795
  client-side-decorated toplevel; the compositor now composites child subsurfaces
  onto the parent at their `set_position` offsets. Plus a one-fd-per-call
  `SCM_RIGHTS` `TakePendingControl` fix so every `wl_shm` pool maps. This is what
  got the chrome to paint.
- **[done] SMP futex waiter-slot ownership** (`src/linux/linux_syscall.cpp`). Two
  abort paths in `futex_enqueue_and_block` cleared their wait-queue slot
  unconditionally; a cross-CPU `FUTEX_WAKE` could consume and re-assign the slot in
  that window, so the blind clear stranded the new owner blocked forever. Both
  sites now verify slot ownership first  -  this killed the "startup stalls at
  thread-init, no crash" heisenbug.
- **[done] TLB shootdown ack-wait** (`src/proc/smp.cpp`). `BroadcastTlbFlush` gave
  up in sub-millisecond time while a peer core could sit interrupts-off ~8.7 ms in
  a serial write, so a stale instruction translation survived the JIT's W^X
  `mprotect` flip and produced per-boot-random wild-jump `#GP`s. Bounded to a
  ~20 ms wall-clock wait; the JIT stays enabled.
- **[done] Process-reap use-after-free** (`src/proc/scheduler.cpp`).
  `DestroyProcess` freed the `Process` struct while a CPU was still unwinding on
  it; the recycled block came back as a path buffer and the CPU `iretq`'d through
  ASCII. Now leaks the struct/stack (bounded) when the context is still live.
- **[done] Serial→KVFS log-mirror deferral** (`src/system/logging.cpp`). The mirror
  synchronously wrote KVFS from the exception-dump path, and the recursive
  log/vfs/heap locks let a fault re-enter a half-mutated heap and corrupt live
  memory (the dump→nested-fault→triple-fault cascades). `MirrorSerial` now only
  stages into a static ring; the `LoggingProcess` flushes from process context.
- **[done] Real `fcntl` byte-range locks** (`src/linux/linux_syscall.cpp`).
  `F_GETLK`/`F_SETLK`/`F_SETLKW` are real advisory locks now, not an always-grant
  stub  -  which had let two SQLite connections both "own" a WAL lock and fail with
  `SQLITE_PROTOCOL`, wedging Places init and NSS's `cert9.db` open.
- **[done] `readlink` errno + socket/pipe `read`/`write`.** `readlink` on an
  existing non-symlink now returns POSIX `-EINVAL` (was `-ENOENT`, which broke
  Rust `fs::canonicalize` for every dir); `sys_read`/`sys_write` gained the missing
  `LFD_SOCKET`/`LFD_PIPE` cases (a pipe `write()` was returning `-EBADF`, which
  broke NSPR's `PollableEvent` and left the socket thread wake-less).
- **[wip] Socket-thread poll-wakeup.** With the pipe `write()` fixed, the socket
  thread's `PollableEvent` self-test passes, but the thread still parks: it polls
  its wakeup fd for `POLLIN` and never sees it readable while the dispatch signals
  land. Narrowed to how NSPR's socketpair/eventfd wakeup maps to the kernel fd
  readiness (the 8-byte dispatch writes point at an eventfd); an in-kernel
  socketpair self-test proves the primitive itself delivers correctly. Closing
  this is what unblocks the first painted page.
- **[next] Render-timing reliability.** Some boots reach the window but the
  software WebRender frame doesn't land before the screenshot, or startup stalls
  before window-create. Once the socket wakeup is closed, tighten the remaining
  multicore startup-timing flakiness so a good boot is the common case.
- **[later] Keyboard-to-client forwarding.** Pointer events already reach Wayland
  clients; the keyboard handler exists but is not yet wired into the input loop.
- **[later] GPU buffers.** `zwp_linux_dmabuf` is advertised but only `wl_shm`
  software buffers are composited today.

### Verify the band-corruption fix
Re-test the freestanding stb image decoder on a large image. If clean, wallpapers
no longer need to ship as pre-decoded raw RGBA.

---

## Next: turning the corner toward usable

- **[next] WiFi association.** The hardware layer detects and identifies the card
  and the 802.11 + WPA2 software stack is in place (crypto self-verified), but
  there is no association yet and no DHCP client. Needs: a DHCP client in the
  TCP/IP stack, and per-vendor radio drivers finished on **real hardware** (QEMU
  emulates no WiFi NIC).
- **[next] ext4 write path.** Reads work; the write path allocates bad blocks, so
  persistence currently bypasses ext4 and uses the raw KFS store. Fixing ext4
  write-back makes the installer targets and guest images fully read-write.
- **[next] Real signal delivery.** Signals are largely accepted-and-ignored stubs.
  Real delivery (handlers, masks, `rt_sigreturn`) is needed by many Linux apps.
- **[next] SMP thread spreading + load balancing.** APs run user *processes*;
  spreading a single process's `clone` threads across cores and balancing load is
  the remaining scheduler work.
- **[next] Hypervisor guest boot off nested VMX.** Alpine/Debian guest boot is
  implemented but needs nested VT-x exposed to Kurono; under plain nested KVM the
  guest VM-entry fails. Document and detect the requirement clearly.

---

## Later: daily-driver and beyond

- **[later] Browser usability.** Once Firefox paints: tabs, real input, sustained
  browsing. This is the single biggest daily-driver unlock.
- **[later] App ecosystem.** More native apps, and running more unmodified Linux
  apps through the runtime (the Wayland + musl + syscall surface already proven by
  Firefox is the foundation).
- **[later] Package manager maturity.** A reliable repo index and more packages.
- **[later] Real GPU acceleration.** Beyond virtio-gpu software composite: dmabuf
  buffers, and the Intel/NVIDIA/AMD backends past detection.
- **[later] KFS as a mounted root.** Today KFS is the persistence layer under
  KVFS; promoting it to a fully mounted root filesystem is the long-term target.
- **[later] Power management.** Suspend/resume, CPU frequency, battery.
- **[later] Finish the security audit.** The hardening pass fixed about 20 of ~68
  triaged issues; the rest (including concurrency and performance items) are open.

---

## How releases land here

Most subsystems were built up over time in a local `feats/` tree and are being
debugged and released as each one becomes solid, which is why features arrive in
bursts. A lot of the OS is "built and almost working." The work in front of it is
largely debugging and finishing, not building from nothing. See the project-status
note in the README.
