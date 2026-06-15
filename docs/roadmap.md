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

### Firefox: from a rendered window to a painted page

A real Firefox 140.11.0esr (cross-compiled against musl + Wayland) **loads, runs
its Gecko engine, and now maps a real window on the Kurono desktop**. The window
is currently blank because the content process is blocked before its first paint,
in Firefox's multiprocess (e10s) IPC path. Closing that path is the headline goal.

- **[done] Restartable SSE-store demand-zero fix** (`src/hal/hal.cpp`). A ring-3
  `movups` store faulting onto a fresh demand-zero page was losing its first 16
  bytes because the fault handler clobbered the user's `%xmm0`. Fixed by
  `fxsave`/`fxrstor` of the user FPU/SSE around the ring-3 page-fault path. This is
  what got the window to render. It very likely also fixes the long-standing
  large-image "band corruption" bug; that should be re-tested.
- **[done] Cross-process (shared) futex** (`src/linux/linux_syscall.cpp`). Futexes
  were keyed by `(address_space, virtual_address)`, so a `FUTEX_WAKE` in the
  Firefox parent could never match a `FUTEX_WAIT` in a content process (different
  address space). They are now also keyed by the **physical page** backing the
  word, so a futex in shared memory wakes across processes. The same-process
  thread path is unchanged.
- **[next] Cross-process AF_UNIX data transfer.** The parent and content processes
  talk over a `socketpair`. The bytes (and `SCM_RIGHTS` fds) have to move between
  two distinct processes' endpoints, not just client-to-compositor.
- **[next] Fork fd-inheritance + child shared-memory re-map.** A forked/exec'd
  content process must inherit (or be passed) the parent's memfd and shared
  socket, and its `mmap(MAP_SHARED)` of that memfd must resolve to the **same
  physical frames** the parent allocated.
- **[next] Growable shared-memory segments** for the IPC message rings.
- **[later] Keyboard-to-client forwarding.** Pointer events already reach Wayland
  clients; the keyboard handler exists but is not yet wired into the input loop.
- **[later] GPU buffers.** `zwp_linux_dmabuf` is advertised but only `wl_shm`
  software buffers are composited today.

### Verify the band-corruption fix
Re-test the freestanding stb image decoder on a large image now that the SSE
demand-zero bug is fixed. If clean, wallpapers no longer need to ship as
pre-decoded raw RGBA.

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
