# Linux Syscall Layer

`src/linux/linux_syscall.cpp` / `linux_syscall_x64.cpp` and `linux_syscall.h`
implement Kurono's in-kernel Linux system-call layer. This is more than a command
shim: it is a real process / memory / file-descriptor / syscall runtime that lets
native musl-compiled Linux binaries run on Kurono in ring 3.

## 1. What it does

When a ring-3 Linux process executes a `syscall` instruction (or `int 0x80`), the
HAL entry path routes it into `LinuxSyscall::Dispatch()`, which maps the Linux
x86-64 syscall number to a Kurono implementation. The layer owns the Linux process
model (per-process fd table, brk heap, cwd, signal disposition), backs file I/O on
KVFS, and translates Linux paths into the canonical `/kurono` tree on the way in.

## 2. Coverage

`linux_syscall.h` defines **177 `LSYS_*` syscall numbers** across the x86-64 ABI
(symbolic names + their numeric synonyms), and `linux_syscall.cpp` implements
**~155 of them for real** (distinct `case LSYS_*` arms)  -  not stubbed  -  covering
the surface a command-line or GUI program needs:

| Group | Syscalls |
| --- | --- |
| File I/O | `read`, `write`, `open`/`openat`, `close`, `lseek`, `dup`, `dup2`, `ioctl`, `writev`, `getdents64`, `stat`, `fstat`, `statx`, `access` |
| FS namespace | `getcwd`, `chdir`, `mkdir`, `rmdir`, `unlink` (paths translated to `/kurono`) |
| Process | `fork`, `clone`/`clone3`, `execve`/`execveat`, `waitpid`, `exit`, `exit_group`, `getpid`, `getuid`/`getgid` |
| Memory | `brk` (lazy), `mmap` (anonymous + **file-backed**), `munmap` (with region split), `mprotect` (W^X + region split) |
| Time | `nanosleep`, `clock_gettime`, `clock_nanosleep` |
| TLS | `arch_prctl(ARCH_SET_FS)`, `set_thread_area`, `set_tid_address` |
| Identity | `uname` (reports `Linux 6.8.0-kurono`) |

### The GUI-blocking set (implemented, not stubbed)

A real GUI app blocks on a handful of syscalls that are easy to stub wrongly.
These are implemented for real, which is what makes the Wayland render path work:

- **`futex`** (`FUTEX_WAIT` / `FUTEX_WAKE`)  -  real blocking + wakeups. The
  multithreaded pthread gate test (`pthread_test`) reliably reaches `counter=2000`.
- **`clone` / `CLONE_THREAD`**  -  threads sharing one address space, preemptively
  switched on the user-thread path.
- **`epoll_create1` / `epoll_ctl` / `epoll_wait`** and **`poll` / `ppoll`**  - 
  event loops backed by eventfd, timerfd, and sockets.
- **`mprotect`**  -  W^X enforcement with region splitting.
- **`memfd_create`** + file-backed **`mmap`**  -  shared-memory backing for `wl_shm`.
- **`sendmsg` / `recvmsg`** carrying **`SCM_RIGHTS`**  -  file-descriptor passing.
  A Wayland client fd-passes a `wl_shm` buffer to the compositor end to end.

## 3. Process & memory model

- Up to 16 concurrent Linux processes, 64 file descriptors each.
- A lazy `brk` heap and anonymous `mmap` regions, with recoverable user page
  faults and copy-on-write address-space cloning behind `fork`.
- **Full 64-bit user address space.** The old sub-4 GB pointer-ABI cap is gone:
  `mmap`/`brk` and PIE load bases run up to `USER_SPACE_TOP`
  (`0x0000_7FFF_FFFF_FFFF`, the canonical user-half top), and the syscall ABI
  widens user pointers/lengths/offsets to 64-bit so a high (multi-TB) mapping
  round-trips intact. This is what lets Firefox's libxul load and relocate at a
  multi-terabyte base. (The kernel still identity-maps low *physical* memory for
  its own access  -  a physical-mapping detail, not a user-address limit.)

## 4. Path translation

`ResolvePath()` rewrites Linux paths into Kurono's canonical tree before any KVFS
op  -  e.g. `/usr/lib*` and `/lib*` resolve through the compat symlinks into
`/kurono/...`, `/proc` / `/dev` / `/run` / `/tmp` map under `/kurono/runtime`, and
`/etc` maps to `/kurono/system/config`. See [KVFS.md](../fs/KVFS.md) §3 and
[../system/LOGGING.md](../system/LOGGING.md) §1 for the layout.

## 5. Dynamic linking

When `execve` loads an ET_DYN ELF that declares `PT_INTERP`, the ELF loader hands
the image to the in-kernel dynamic linker rather than mapping it as a static
binary. The linker resolves `DT_NEEDED` (e.g. musl's `libc.musl-x86_64.so.1`),
applies relocations, sets up TLS + the SysV auxv stack, and enters the program.
See **[LD_KURONO.md](LD_KURONO.md)**.

## 6. Unsupported syscalls

An unrecognized syscall number is logged to serial and returns `-ENOSYS`. Heavier
Linux workloads that need a full kernel run instead through the hypervisor path (a
real Linux guest)  -  see [../virt/HYPERVISOR.md](../virt/HYPERVISOR.md).

## 7. Related files

- `src/linux/linux_syscall.cpp` / `linux_syscall_x64.cpp` / `.h`  -  the dispatch table + handlers
- `src/linux/linux_kernel.cpp`  -  personality coordinator that activates the layer
- `src/linux/ld_kurono.cpp`  -  the dynamic linker the loader hands PIEs to
- `src/net/unix_socket.cpp`  -  AF_UNIX sockets + `SCM_RIGHTS` fd-passing
- `src/virt/hypervisor.cpp`  -  full hardware-virtualization path for heavier guests
