# Linux syscall layer

`src/linux/linux_syscall.cpp`, `linux_syscall_x64.cpp` and `linux_syscall.h`. Kurono's
in-kernel Linux system-call layer.

Not a command shim. It's a real process, memory, fd and syscall runtime that lets
musl-compiled Linux binaries run in ring 3 on Kurono.

When a ring-3 process does a `syscall` or `int 0x80`, the HAL entry path routes it into
`LinuxSyscall::Dispatch()`, which maps the x86-64 syscall number onto a Kurono implementation.
The layer owns the Linux process model (per-process fd table, `brk` heap, cwd, signal
disposition), backs file I/O on KVFS, and translates Linux paths into the canonical `/kurono`
tree on the way in.

## Coverage

`linux_syscall.h` defines 177 `LSYS_*` numbers across the x86-64 ABI, and
`linux_syscall.cpp` implements about 155 of them for real, as distinct `case LSYS_*` arms. Not
stubs.

| Group | Syscalls |
| --- | --- |
| File I/O | `read`, `write`, `open`/`openat`, `close`, `lseek`, `dup`, `dup2`, `ioctl`, `writev`, `getdents64`, `stat`, `fstat`, `statx`, `access` |
| FS namespace | `getcwd`, `chdir`, `mkdir`, `rmdir`, `unlink`, with paths translated into `/kurono` |
| Process | `fork`, `clone`/`clone3`, `execve`/`execveat`, `waitpid`, `exit`, `exit_group`, `getpid`, `getuid`/`getgid` |
| Memory | `brk` (lazy), `mmap` (anonymous and file-backed), `munmap` with region split, `mprotect` (W^X plus region split) |
| Time | `nanosleep`, `clock_gettime`, `clock_nanosleep` |
| TLS | `arch_prctl(ARCH_SET_FS)`, `set_thread_area`, `set_tid_address` |
| Identity | `uname`, which reports `Linux 6.8.0-kurono` |

## The set a GUI app blocks on

A real GUI app blocks on a handful of syscalls that are easy to stub wrongly. These are the
real thing, which is what makes the Wayland render path work:

- `futex` (`FUTEX_WAIT` / `FUTEX_WAKE`), real blocking and wakeups. The multithreaded pthread
  gate test reliably reaches `counter=2000`.
- `clone` / `CLONE_THREAD`, threads sharing one address space, preemptively switched on the
  user-thread path.
- `epoll_create1` / `epoll_ctl` / `epoll_wait` and `poll` / `ppoll`, event loops backed by
  eventfd, timerfd and sockets.
- `mprotect`, W^X with region splitting.
- `memfd_create` plus file-backed `mmap`, the shared-memory backing for `wl_shm`.
- `sendmsg` / `recvmsg` carrying `SCM_RIGHTS`. A Wayland client fd-passes a `wl_shm` buffer to
  the compositor end to end.

## Process and memory model

- up to 16 concurrent Linux processes, 64 fds each
- lazy `brk` heap and anonymous `mmap` regions, with recoverable user page faults and
  copy-on-write address-space cloning behind `fork`
- full 64-bit user address space. The old sub-4GB pointer ABI cap is gone: `mmap`/`brk` and
  PIE load bases run up to `USER_SPACE_TOP` (`0x0000_7FFF_FFFF_FFFF`), and the syscall ABI
  widens user pointers, lengths and offsets to 64-bit so a multi-terabyte mapping round trips
  intact. That's what lets Firefox's libxul load and relocate at a multi-terabyte base. (The
  kernel still identity-maps low physical memory for its own access, which is a
  physical-mapping detail, not a user-address limit.)

## Path translation

`ResolvePath()` rewrites Linux paths into the canonical tree before any KVFS op. `/usr/lib*`
and `/lib*` go through the compat symlinks into `/kurono/...`, `/proc`, `/dev`, `/run` and
`/tmp` map under `/kurono/runtime`, and `/etc` maps to `/kurono/system/config`. See
[KVFS.md](../fs/KVFS.md) and [../system/LOGGING.md](../system/LOGGING.md).

## Dynamic linking

When `execve` loads an ET_DYN ELF that declares `PT_INTERP`, the ELF loader hands the image to
the in-kernel dynamic linker instead of mapping it as a static binary. The linker resolves
`DT_NEEDED` (musl's `libc.musl-x86_64.so.1`), applies relocations, sets up TLS and the SysV
auxv stack, and enters the program. See [LD_KURONO.md](LD_KURONO.md).

## Unknown syscalls

An unrecognised number gets logged to serial and returns `-ENOSYS`. Heavier Linux workloads
that want a full kernel go through the hypervisor instead, as a real Linux guest. See
[../virt/HYPERVISOR.md](../virt/HYPERVISOR.md).

## Files

- `src/linux/linux_syscall.cpp`, `linux_syscall_x64.cpp`, `.h` the dispatch table and handlers
- `src/linux/linux_kernel.cpp` the personality coordinator that activates the layer
- `src/linux/ld_kurono.cpp` where PIEs get handed off
- `src/net/unix_socket.cpp` AF_UNIX plus `SCM_RIGHTS` fd passing
- `src/virt/hypervisor.cpp` the full hardware-virtualization path for heavier guests
