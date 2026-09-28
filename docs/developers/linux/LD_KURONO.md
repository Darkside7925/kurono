# ld-kurono

`src/linux/ld_kurono.cpp` / `ld_kurono.h`. A dynamic linker built straight into the kernel.

There is no `ld.so` binary on disk. When `execve` loads an ET_DYN ELF that declares `PT_INTERP`
(for example `/lib64/ld-linux-x86-64.so.2`), the ELF loader (`src/kernel/elf_loader.cpp`) hands the
image to `LdKurono::ExecPIE()` instead of trying to map it as a static binary. The path-translation
layer rewrites interpreter requests to `/kurono/system/lib/ld-kurono.so` for anything that asks, and
a marker file gets dropped there so `file(1)` and `ldd`-style scanners see something.

This is the piece that lets real dynamic musl PIEs run on the OS, and the prerequisite for Firefox,
which is a musl PIE plus an ~80 library closure.

## The load path

When the loader sees `PT_INTERP`, `ExecPIE` does the full dynamic bring-up:

1. Pick a per-segment ASLR base (RDTSC-derived entropy) in the user range, page-aligned and
   span-clamped, and map the PIE's `PT_LOAD` segments.
2. Recursively resolve `DT_NEEDED` with circular-dependency tracking and SONAME-based dedup. Search
   order: `DT_RPATH`/`DT_RUNPATH`, then `LD_LIBRARY_PATH` (ignored for setuid), then `/system/lib`,
   `/system/lib/kurono`, `/system/lib/x86_64-linux-gnu`, `/apps/lib`, `/system/local/lib`,
   `/home/user/.local/lib`, all resolving through the `/kurono` compat symlinks. References to
   `ld-linux*` or `ld-kurono.so` are short-circuited, since the linker is the kernel.
3. Apply relocations, enforce `PT_GNU_RELRO`, set up static TLS, build the SysV auxv stack frame.
4. Return the entry point to jump to. Per the current design (commit `feba825`) that is musl's own
   linker entry, the interp's `_start`, so musl runs its real `__dls3` dso-list init rather than
   being bypassed.

The kernel then enters ring 3 with the right user CR3, FS base and stack.

## Symbol resolution

- GNU_HASH first (bloom filter, bucket, chain), SYSV hash as fallback, linear scan as a last resort
- binding and visibility honoured: `STB_GLOBAL`, `STB_WEAK`, `STB_LOCAL`, and `STV_DEFAULT`,
  `STV_HIDDEN`, `STV_PROTECTED`, `STV_INTERNAL`
- versioned lookup through `DT_VERSYM`, `DT_VERDEF`, `DT_VERNEED`

## Relocations

The full x86-64 set, 24 distinct `case R_X86_64_*` arms: `NONE`, `64`, `PC32`, `PC64`, `PLT32`,
`GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX`, `32`, `32S`, `GLOB_DAT`, `JUMP_SLOT`, `RELATIVE`,
`IRELATIVE` (IFUNC resolvers), `COPY`, `TPOFF32/64`, `DTPMOD64`, `DTPOFF32/64`, `TLSDESC`, `TLSGD`,
`TLSLD`, `GOTTPOFF`. Binding is eager, matching `DT_BIND_NOW` and `LD_BIND_NOW=1`. After relocations
the RELRO region is re-protected read-only (`PTE_USER | PTE_NX`).

## TLS, vDSO, auxv, constructors

- static TLS: variant-2 layout, monotonic per-module offsets, with `arch_prctl(ARCH_SET_FS)` wired
  through the syscall layer. The main-thread TLS and thread pointer get installed for dynamic PIEs,
  and the user FS base is programmed in `UserspaceEnter`.
- vDSO: a 4KB ELF64 stub is synthesised (exporting `__vdso_clock_gettime`, `__vdso_gettimeofday`,
  `__vdso_time`, `__vdso_getcpu` as `syscall` trampolines) and can be mapped with `MapVDSO`. Honest
  note: as of commit `919820b` the linker deliberately does not advertise it
  (`AT_SYSINFO_EHDR=0`), so musl falls back to direct `syscall`, which avoided a bring-up issue. The
  stub code is still there, the auxv just doesn't point at it.
- auxv builder: pushes `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_ENTRY`,
  `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`, `AT_SECURE`, `AT_RANDOM` (16 bytes of RDTSC entropy),
  `AT_HWCAP`/`AT_HWCAP2`, `AT_CLKTCK=100`, `AT_PLATFORM="x86_64"`, `AT_EXECFN`, `AT_SYSINFO_EHDR`.
- constructors: `DT_INIT` and `DT_INIT_ARRAY` run in dependency order through a hand-emitted
  user-mode trampoline page that preserves the SysV `(argc, argv, envp)` registers between calls,
  then tail-jumps to the program entry.

## dlopen family

`Dlopen`, `Dlclose`, `Dlsym`, `Dlvsym`, `Dladdr` and `Dlerror` are implemented with all the
`RTLD_*` flags (`LAZY`, `NOW`, `GLOBAL`, `LOCAL`, `NOLOAD`, `DEEPBIND`, `NODELETE`) plus the
`RTLD_DEFAULT` and `RTLD_NEXT` pseudo-handles. `LD_DEBUG`
(`all`/`libs`/`symbols`/`reloc`/`files`/`versions`/`bindings`) logs to serial and
`/system/log/ldso.log`. `LD_PRELOAD` is honoured, dropped for setuid/setgid. An `r_debug`
rendezvous plus a `_dl_debug_state` hook lets a future GDB attach rescan the loaded library list on
each `Dlopen`/`Dlclose`.

## Verification, current state

- `dyntest` (the `kurono.dyntest` cmdline token, or the `dyntest` shell command) was the first real
  exercise of the dynamic path: it loads `/usr/bin/dyntest`, a musl PIE with `PT_INTERP`, through
  ld-kurono, recurses into `libc.musl-x86_64.so.1` from `/system/lib`, relocates, sets up TLS and
  auxv, and runs. The boot gate reports `DYNTEST_END rc=0`, also surfaced by `kurono.logcheck`,
  which proves the dynamic load and musl-libc resolution path.
- file-backed `mmap` of `.so` segments landed (commits `e3a5952`, `df45ed1`), so musl can map its
  shared-object segments from a regular file.
- Firefox's Gecko engine loads through ld-kurono and composites its real chrome on the desktop. A
  real Firefox 140.11.0esr (musl and Wayland, software WebRender, 174MB `libxul.so`) is loaded
  on-device: ld-kurono resolves libxul's full `.so` closure and loads and relocates libxul (130MB+)
  at a multi-terabyte base, XPCOM plus Gecko's own application code runs, and Firefox paints its tab
  strip, URL bar, navigation controls and menus through the compositor's `wl_subsurface` path,
  single-process (e10s off) with threads across the secondary cores. The sub-4GB user-pointer ABI cap
  that used to block a high PIE base is lifted, so the syscall layer spans the full canonical 64-bit
  user range (see [LINUX_SYSCALL.md](LINUX_SYSCALL.md)). What remains for a reliably rendered web
  page is the socket thread's poll-wakeup delivery and some render-timing flakiness, not the dynamic
  loader path.

## Limits

From `ld_kurono.h`: 256 libs per process, 1024 global, 64 `DT_NEEDED` deps, 32 search paths, 16
preloads.

## Files

- `src/linux/ld_kurono.cpp` / `.h` the linker. The header comment is the authoritative capability
  list.
- `src/kernel/elf_loader.cpp` `PT_INTERP` detection and the handoff to `ExecPIE`
- `src/linux/linux_syscall.cpp` `execve`, file-backed `mmap`, `arch_prctl`, and path translation to
  the interpreter name
- `src/kernel/userspace.cpp` `UserspaceEnter`, which programs the user FS base
- `src/kernel/kurono_kernel.cpp` the `kurono.dyntest` boot gate
