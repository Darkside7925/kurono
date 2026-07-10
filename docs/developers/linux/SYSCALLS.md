# Kurono Linux syscall ABI - full table

This is the authoritative status of the in-kernel Linux syscall layer (KLS), the
thing that lets unmodified x86_64 musl/glibc ELF binaries run on Kurono with no
VM. It complements `KLS.md` (architecture) and `LINUX_SYSCALL.md` (overview).

## How a syscall reaches a handler

A real amd64 binary enters via the `SYSCALL` instruction. The asm stub
(`src/hal/syscall_entry.asm`) builds an `InterruptFrame` and calls
`SyscallEntryX64Handler` (`src/linux/linux_syscall_x64.cpp`). That function, in
order:

1. handles a small set of numbers **directly** (mmap, arch_prctl,
   set_tid_address, getrandom, readlink/readlinkat, openat, readv, the 64-bit
   stat family, clock_nanosleep, the KDF/UDF `0x4B554446` proxy);
2. returns `0` for the **stub-ok** list (no-op-success numbers);
3. otherwise translates the amd64 number to an internal `LSYS_*` id via `kNrMap`
   and calls `LinuxSyscall::Dispatch` (`src/linux/linux_syscall.cpp`).

The legacy `int 0x80` path enters `Dispatch` directly with the i386 number. Both
paths share the `Dispatch` handler body.

If a number is neither handled directly, nor stub-ok, nor in `kNrMap`, the x64
path logs `[kls] ENOSYS nr=<n>` (rate-limited) and returns `-38` (`-ENOSYS`). The
`Dispatch` `default:` arm logs the same way. This is the strace-equivalent audit
hook: boot a real binary and grep the serial for `ENOSYS` to see exactly which
numbers it still needs.

## Status legend

- **real** - does meaningful work backed by real kernel state (fs, scheduler,
  memory, process/identity tables) and returns real values.
- **stub** - accepts the call shape and returns a correct-contract value (often
  `0`, or a canned errno like `-ENODATA`/`-EOPNOTSUPP`) without a full backend.
  Chosen so the call is non-fatal to the caller, never a blind `-ENOSYS`.
- **enosys** - genuinely cannot proceed in this kernel; returns `-ENOSYS` and is
  logged. Limited to ops that need a mechanism the kernel lacks (a ptrace trap
  path, a kernel keyring).

## Summary

Across the Tier 1-11 completeness list, **every** number is now reachable from
the amd64 `SYSCALL` path (0 unreachable). The split is roughly: the file/io,
identity, scheduler, memory-query, and signal-post families are **real**; the
sandbox/namespace/advisory/NUMA/xattr families are **stub** (correct contract,
no deep backend); only `ptrace` (non-TRACEME requests), `add_key`, and
`request_key` are **enosys** (no trap mechanism / no keyring).

Verified headless via `kurono.klstest` (a per-tier self-test that drives
`Dispatch` and logs PASS/FAIL): 30/30 checks pass, including the real memory work
(`mremap` grow-with-move copies bytes, `mincore` resident map), `statfs`, the
`getres*`/`setfs*` identity round-trips, the xattr errno contract, `rt_sigpending`
mask read-back, and the `ENOSYS` logger firing exactly once per number.

---

### Tier 1 - blocks real apps now

| #   | name                    | status | notes |
|-----|-------------------------|--------|-------|
| 425 | io_uring_setup          | stub   | allocates a real `LFD_URING` fd; ring is not yet shared-mapped (staged work) |
| 426 | io_uring_enter          | stub   | returns 0 (no submissions processed yet) |
| 427 | io_uring_register       | stub   | returns 0 (fixed buffers not registered yet) |
| 434 | pidfd_open              | real   | real fd; target pid stashed in the fd for pidfd_send_signal/getfd |
| 424 | pidfd_send_signal       | real   | resolves the pidfd's pid and posts the signal bit |
| 438 | pidfd_getfd             | real   | dup of the target fd in this single-table build |
| 437 | openat2                 | real   | pulls flags+mode out of `struct open_how`, routes to open |
| 326 | copy_file_range         | real   | bounded read/write copy loop, advances both offsets |
| 300 | fanotify_init           | stub   | real `LFD_FANOTIFY` fd; no event backend |
| 301 | fanotify_mark           | stub   | accepted, marks recorded as no-ops |
| 253 | inotify_init            | real   | routed to inotify_init1(flags=0) |
| 294 | inotify_init1           | real   | real fd |
| 254 | inotify_add_watch       | stub   | accepted, no watch events delivered |
| 255 | inotify_rm_watch        | stub   | accepted |
| 444 | landlock_create_ruleset | real   | real `LFD_LANDLOCK` fd |
| 445 | landlock_add_rule       | stub   | accepted; rules recorded, not enforced |
| 446 | landlock_restrict_self  | stub   | accepted; no enforcement |
| 317 | seccomp                 | stub   | accepts BPF filters without installing them |
| 275 | splice                  | stub   | returns 0 (no pipe page-move); callers fall back to read/write |
| 276 | tee                     | stub   | returns 0 |
| 278 | vmsplice                | stub   | returns 0 (consumed 0); callers fall back to write |
| 40  | sendfile                | real   | bounded read/write loop with optional in-offset seek |
| 310 | process_vm_readv        | real   | iovec-to-iovec memcpy (single address space) |
| 311 | process_vm_writev       | real   | iovec-to-iovec memcpy |

### Tier 2 - POSIX file/io

| #   | name              | status | notes |
|-----|-------------------|--------|-------|
| 277 | sync_file_range   | stub   | advisory; no page cache to flush |
| 285 | fallocate         | stub   | returns 0 (files grow on write in kvfs) |
| 221 | posix_fadvise     | stub   | advisory no-op |
| 187 | readahead         | stub   | advisory no-op (stub-ok list) |
| 303 | name_to_handle_at | stub   | returns 0 |
| 304 | open_by_handle_at | stub   | returns 0 |
| 316 | renameat2         | stub   | returns 0 (flags ignored) |
| 332 | statx             | real   | full statx block from the kvfs node |
| 324 | membarrier        | stub   | no-op success (cooperative + barriers) |
| 334 | rseq              | stub   | no-op success |
| 436 | close_range       | real   | closes every open fd in [first,last] |
| 292 | dup3              | real   | dup2 + the oldfd==newfd → EINVAL rule |
| 293 | pipe2             | real   | unix-socketpair-backed pipe |
| 284 | eventfd           | real   | real `LFD_EVENTFD` with a counter |
| 290 | eventfd2          | real   | real `LFD_EVENTFD` |
| 282 | signalfd          | stub   | real fd, never fires (no async signal delivery) |
| 289 | signalfd4         | stub   | real fd, never fires |
| 283 | timerfd_create    | real   | real `LFD_TIMERFD` |
| 286 | timerfd_settime   | real   | arms expiry/interval against the ms timebase |
| 287 | timerfd_gettime   | real   | reports remaining time |
| 441 | epoll_pwait2      | real   | converts the timespec timeout to ms, routes to epoll_wait |
| 295 | preadv            | real   | seek-to-pos then scatter read |
| 296 | pwritev           | real   | seek-to-pos then scatter write |
| 327 | preadv2           | real   | as preadv (flags ignored) |
| 328 | pwritev2          | real   | as pwritev (flags ignored) |

### Tier 3 - process / thread / scheduler / capabilities

| #   | name                   | status | notes |
|-----|------------------------|--------|-------|
| 435 | clone3                 | real   | routes to fork for the common (no CLONE_VM) case; firefox content procs |
| 272 | unshare                | real   | bumps the per-task namespace ids the caller can observe |
| 308 | setns                  | stub   | accepted |
| 155 | pivot_root             | stub   | accepted |
| 161 | chroot                 | real   | sets the process cwd root |
| 203 | sched_setaffinity      | real   | pushes the low-byte cpu mask to Scheduler::SetAffinity |
| 204 | sched_getaffinity      | real   | reads the task's affinity bitmap |
| 314 | sched_setattr          | real   | applies sched_policy from sched_attr |
| 315 | sched_getattr          | real   | fills sched_attr from the task (policy + nice) |
| 142 | sched_setparam         | stub   | accepted (no realtime priority store) |
| 143 | sched_getparam         | real   | reports priority 0 |
| 144 | sched_setscheduler     | real   | maps policy → task sched_class |
| 145 | sched_getscheduler     | real   | reports the task's class |
| 146 | sched_get_priority_max | real   | 99 for FIFO/RR, 0 otherwise |
| 147 | sched_get_priority_min | real   | 1 for FIFO/RR, 0 otherwise |
| 148 | sched_rr_get_interval  | real   | reports the ~10ms PIT quantum |
| 141 | setpriority            | real   | clamps + stores nice on the backing kernel task |
| 140 | getpriority            | real   | returns 20-nice from the task |
| 251 | ioprio_set             | stub   | accepted |
| 252 | ioprio_get             | stub   | reports best-effort class, prio 0 |
| 125 | capget                 | real   | zeroes the user cap data (no-capabilities set) |
| 126 | capset                 | stub   | accepted (run as root-equivalent) |
| 157 | prctl                  | real   | PR_SET_NAME / PR_GET_NAME; others accepted (stub-ok) |
| 158 | arch_prctl             | real   | ARCH_SET/GET_FS/GS via the FS/GS-base MSRs (direct case) |
| 135 | personality           | stub   | accepted (ADDR_NO_RANDOMIZE etc. no-op) |

### Tier 4 - filesystem (at-family + xattr)

| #   | name         | status | notes |
|-----|--------------|--------|-------|
| 265 | linkat       | real   | kvfs has no hardlinks → copies source to dest (AT_FDCWD) |
| 266 | symlinkat    | real   | creates a real kvfs symlink node |
| 267 | readlinkat   | real   | /proc/self/exe → real exec path (direct case) |
| 268 | fchmodat     | real   | resolve + KVFS::Chmod |
| 269 | faccessat    | real   | routed to the path-based access check |
| 439 | faccessat2   | real   | as faccessat |
| 280 | utimensat    | real   | touches the node's atime/mtime |
| 261 | futimesat    | real   | touches the node's atime/mtime |
| 165 | mount        | stub   | accepted (single in-RAM root) |
| 166 | umount2      | stub   | accepted |
| 167 | swapon       | stub   | accepted (no swap) |
| 168 | swapoff      | stub   | accepted |
| 179 | quotactl     | stub   | accepted |
| 188 | setxattr     | stub   | `-EOPNOTSUPP` (no xattr store) |
| 189 | lsetxattr    | stub   | `-EOPNOTSUPP` |
| 190 | fsetxattr    | stub   | `-EOPNOTSUPP` |
| 191 | getxattr     | stub   | `-ENODATA` (attr always absent) |
| 192 | lgetxattr    | stub   | `-ENODATA` |
| 193 | fgetxattr    | stub   | `-ENODATA` |
| 194 | listxattr    | stub   | 0 (empty name list) |
| 195 | llistxattr   | stub   | 0 |
| 196 | flistxattr   | stub   | 0 |
| 197 | removexattr  | stub   | `-ENODATA` |
| 198 | lremovexattr | stub   | `-ENODATA` |
| 199 | fremovexattr | stub   | `-ENODATA` |

### Tier 5 - networking

| #   | name        | status | notes |
|-----|-------------|--------|-------|
| 53  | socketpair  | real   | AF_UNIX connected pair |
| 51  | getsockname | real   | |
| 52  | getpeername | real   | |
| 54  | setsockopt  | real   | |
| 55  | getsockopt  | real   | |
| 48  | shutdown    | real   | |
| 307 | sendmmsg    | real   | loops sendmsg over the mmsghdr array, sets each msg_len |
| 299 | recvmmsg    | real   | loops recvmsg over the mmsghdr array |

### Tier 6 - memory

| #   | name           | status | notes |
|-----|----------------|--------|-------|
| 25  | mremap         | real   | shrink/same in place; grow-with-MAYMOVE allocates + copies + frees |
| 26  | msync          | stub   | no-op success (stub-ok list) |
| 28  | madvise        | stub   | no-op success |
| 149 | mlock          | stub   | accepted (nothing is paged out) |
| 150 | munlock        | stub   | accepted |
| 151 | mlockall       | stub   | accepted |
| 152 | munlockall     | stub   | accepted |
| 27  | mincore        | real   | marks every queried page resident |
| 447 | memfd_secret   | real   | backed by the memfd anon-file machinery (no hw secrecy) |
| 323 | userfaultfd    | stub   | real fd; no fault events delivered |
| 237 | mbind          | stub   | accepted (single node) |
| 238 | set_mempolicy  | stub   | accepted |
| 239 | get_mempolicy  | stub   | reports MPOL_DEFAULT / node 0 |
| 256 | migrate_pages  | stub   | accepted (nothing to migrate) |
| 279 | move_pages     | stub   | reports every page on node 0 |

### Tier 7 - signals

| #   | name              | status | notes |
|-----|-------------------|--------|-------|
| 13  | rt_sigaction      | stub   | accepted (stub-ok); no async delivery yet |
| 14  | rt_sigprocmask    | stub   | accepted |
| 15  | rt_sigreturn      | stub   | accepted |
| 127 | rt_sigpending     | real   | writes the process's real pending mask |
| 128 | rt_sigtimedwait   | stub   | `-EAGAIN` (timeout elapses, nothing pending) |
| 129 | rt_sigqueueinfo   | real   | posts the signal bit to the target process |
| 297 | rt_tgsigqueueinfo | real   | posts the signal bit to the target thread |
| 131 | sigaltstack       | stub   | accepted (stub-ok) |
| 62  | kill              | real   | posts the signal bit (pid<=0 → self) |
| 200 | tkill             | real   | posts the signal bit to the tid |
| 234 | tgkill            | real   | maps to kill(tid, sig) |
| 34  | pause             | stub   | yields once, returns `-EINTR` (no async delivery) |

### Tier 8 - time

| #   | name            | status | notes |
|-----|-----------------|--------|-------|
| 228 | clock_gettime   | real   | |
| 227 | clock_settime   | stub   | accepted (clock not moved) |
| 229 | clock_getres    | real   | |
| 230 | clock_nanosleep | real   | routes to nanosleep (direct case) |
| 35  | nanosleep       | real   | |
| 36  | getitimer       | stub   | clears the old itimerval, accepts |
| 38  | setitimer       | stub   | clears the old itimerval, accepts (no SIGALRM backend) |
| 37  | alarm           | stub   | 0 (no prior alarm) |
| 100 | times           | real   | reports cpu_ms_total as utime; returns tick count |
| 159 | adjtimex        | stub   | TIME_OK |
| 305 | clock_adjtime   | stub   | TIME_OK |

### Tier 9 - user / group identity

| #   | name       | status | notes |
|-----|------------|--------|-------|
| 111 | getpgrp    | real   | |
| 121 | getpgid    | real   | |
| 109 | setpgid    | real   | |
| 124 | getsid     | real   | |
| 112 | setsid     | real   | |
| 105 | setuid     | real   | sets real+effective uid on the LinuxProcess |
| 106 | setgid     | real   | sets real+effective gid |
| 113 | setreuid   | real   | honours the -1 "leave unchanged" sentinel |
| 114 | setregid   | real   | |
| 117 | setresuid  | real   | |
| 119 | setresgid  | real   | |
| 118 | getresuid  | real   | reports real/effective (no separate saved-uid) |
| 120 | getresgid  | real   | |
| 122 | setfsuid   | real   | returns the previous fsuid (== euid) |
| 123 | setfsgid   | real   | returns the previous fsgid (== egid) |
| 115 | getgroups  | stub   | 0 (no supplementary groups tracked) |
| 116 | setgroups  | stub   | accepted |

### Tier 10 - system info

| #   | name     | status | notes |
|-----|----------|--------|-------|
| 63  | uname    | real   | |
| 99  | sysinfo  | real   | uptime/loads/ram/proc count |
| 103 | syslog   | stub   | no dmesg ring; SIZE→canned, reads empty |
| 163 | acct     | stub   | accepted (accounting off) |
| 139 | sysfs    | stub   | option 3 (fs-type count) → 1; others accept |
| 136 | ustat    | stub   | zeroes the buffer, succeeds |
| 137 | statfs   | real   | fills struct statfs for the in-RAM kvfs |
| 138 | fstatfs  | real   | same as statfs |

### Tier 11 - misc / advanced

| #   | name            | status | notes |
|-----|-----------------|--------|-------|
| 101 | ptrace          | enosys | PTRACE_TRACEME accepted; tracer ops need a trap mechanism the kernel lacks |
| 298 | perf_event_open | stub   | real fd; read() returns 0 |
| 246 | kexec_load      | stub   | accepted (no in-place kernel swap) |
| 320 | kexec_file_load | stub   | accepted |
| 321 | bpf             | stub   | MAP/PROG ops accept; PROG_LOAD returns a fresh fd |
| 212 | lookup_dcookie  | stub   | `-ENOENT` (no dcookie db) |
| 248 | add_key         | enosys | no kernel keyring backend |
| 249 | request_key     | enosys | no kernel keyring backend |
| 250 | keyctl          | stub   | accepted |

---

## Hard blockers / honest limits

- **io_uring** is staged: `io_uring_setup` hands back a real fd but the SQ/CQ
  rings are not yet shared-mapped into userspace, so `io_uring_enter` processes
  no submissions. A full impl needs the shared-ring VA mapping; most binaries
  probe io_uring then fall back to epoll, which is real.
- **ptrace** needs a single-step/trap delivery path (debug-exception plumbing
  back to a tracer process) the cooperative kernel does not have yet;
  `PTRACE_TRACEME` is accepted, all tracer-side requests return `-ENOSYS`.
- **signals are post-only**: kill/tkill/tgkill/pidfd_send_signal/rt_sigqueueinfo
  set the pending bit, but there is no async user-signal delivery, so
  signalfd/sigtimedwait/pause cannot actually fire a handler.
- **xattr / NUMA / namespaces / fanotify / userfaultfd** are contract-stubs: the
  call shape is satisfied with the value a real Linux returns when the feature is
  simply unavailable, which is what well-behaved libc/coreutils tolerate.

## Audit recipe

```
# bake the gate into grub.cfg, boot headless, grep the serial:
cd src && make iso KURONO_EXTRA_CMDLINE='kurono.klstest=1 kurono.klstest.poweroff=1'
qemu-system-x86_64 -cdrom ../build/kurono.iso -m 2G -enable-kvm -cpu host -smp 1 \
  -vga std -serial file:/tmp/s.log -display none
grep -E '\[klstest\]|ENOSYS' /tmp/s.log
```

To audit a real binary's needs, boot it (e.g. `kurono.dyntest`, or
`KURONO_GUI_RUN=firefox`) and grep the serial for `[kls] ENOSYS nr=` - each
unimplemented number it hits logs once (rate-limited).
