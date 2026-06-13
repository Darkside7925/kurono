# ld-kurono  -  In-Kernel ELF64 Dynamic Linker

`src/linux/ld_kurono.cpp` / `ld_kurono.h` implement **ld-kurono**, a dynamic
linker built directly into the kernel. There is no separate `ld.so` binary on
disk: when `execve` loads an ET_DYN ELF that declares `PT_INTERP` (e.g.
`/lib64/ld-linux-x86-64.so.2`), the ELF loader (`src/kernel/elf_loader.cpp`) hands
the image to `LdKurono::ExecPIE()` instead of trying to map it as a static binary.
The path-translation layer rewrites interpreter requests to
`/kurono/system/lib/ld-kurono.so` for any code that asks, and a marker file is
dropped there so `file(1)` / `ldd`-style scanners can see it.

This is the piece that lets **real dynamic musl PIEs run on the OS**, and it is the
prerequisite for eventually running Firefox (a musl PIE plus an ~80-library
closure).

## 1. Load path (`ExecPIE`)

When the loader detects `PT_INTERP`, `ExecPIE` does the full dynamic bring-up:

1. Pick a per-segment **ASLR** base (RDTSC-derived entropy) in the user range,
   page-aligned and span-clamped, and map the PIE's `PT_LOAD` segments.
2. Recursively resolve **`DT_NEEDED`** with circular-dependency tracking and
   SONAME-based deduplication. Search order: `DT_RPATH`/`DT_RUNPATH` →
   `LD_LIBRARY_PATH` (ignored for setuid) → `/system/lib` →
   `/system/lib/kurono` → `/system/lib/x86_64-linux-gnu` → `/apps/lib` →
   `/home/user/.local/lib` (all resolving through the `/kurono` compat symlinks).
   References to `ld-linux*` / `ld-kurono.so` are short-circuited  -  the linker
   *is* the kernel.
3. Apply relocations (see §3), enforce `PT_GNU_RELRO`, set up **static TLS**, and
   build the SysV **auxv** stack frame.
4. Return the entry point to jump to. Per the current design (commit
   `feba825`) this is **musl's own linker entry** (the interp's `_start`), so
   musl runs its real `__dls3` dso-list init rather than being bypassed.

The kernel then enters ring 3 with the correct user CR3, FS base, and stack.

## 2. Symbol resolution

- **GNU_HASH** primary path (bloom filter + bucket + chain), **SYSV hash**
  fallback, linear scan as a last resort.
- Binding + visibility honoured: `STB_GLOBAL` / `STB_WEAK` / `STB_LOCAL`,
  `STV_DEFAULT` / `STV_HIDDEN` / `STV_PROTECTED` / `STV_INTERNAL`.
- **Versioned** lookup via `DT_VERSYM` / `DT_VERDEF` / `DT_VERNEED`.

## 3. Relocations

The full x86-64 set  -  **23 relocation types**: `NONE`, `64`, `PC32`, `PC64`,
`PLT32`, `GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX`, `32`, `32S`, `GLOB_DAT`,
`JUMP_SLOT`, `RELATIVE`, `IRELATIVE` (IFUNC resolvers), `COPY`, `TPOFF32/64`,
`DTPMOD64`, `DTPOFF32/64`, `TLSDESC`, `TLSGD`, `TLSLD`, `GOTTPOFF`. Binding is
eager (matching `DT_BIND_NOW` / `LD_BIND_NOW=1` semantics). After relocations the
RELRO region is re-protected read-only (`PTE_USER | PTE_NX`).

## 4. TLS, vDSO, auxv, constructors

- **Static TLS**  -  variant-2 layout, monotonic per-module offset assignment, with
  `arch_prctl(ARCH_SET_FS)` wired through the syscall layer (the main-thread TLS +
  thread pointer are installed for dynamic PIEs; the user FS base is programmed in
  `UserspaceEnter`).
- **vDSO**  -  a 4 KB ELF64 stub is synthesized (exporting `__vdso_clock_gettime`,
  `__vdso_gettimeofday`, `__vdso_time`, `__vdso_getcpu` as `syscall` trampolines)
  and can be mapped via `MapVDSO`. **Honest note:** as of commit `919820b` the
  linker deliberately does **not advertise** the synthesized vDSO
  (`AT_SYSINFO_EHDR=0`)  -  musl falls back to direct `syscall`, which avoided a
  bring-up issue. The stub code remains; the auxv just doesn't point at it.
- **Auxv builder**  -  pushes `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`, `AT_PAGESZ`,
  `AT_BASE`, `AT_ENTRY`, `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`, `AT_SECURE`,
  `AT_RANDOM` (16 bytes of RDTSC entropy), `AT_HWCAP`/`AT_HWCAP2`,
  `AT_CLKTCK=100`, `AT_PLATFORM="x86_64"`, `AT_EXECFN`, `AT_SYSINFO_EHDR`.
- **Constructors**  -  `DT_INIT` / `DT_INIT_ARRAY` run in dependency order via a
  hand-emitted user-mode trampoline page that preserves the SysV `(argc, argv,
  envp)` registers between calls, then tail-jumps to the program entry.

## 5. Runtime API (`dlopen` family)

`Dlopen` / `Dlclose` / `Dlsym` / `Dlvsym` / `Dladdr` / `Dlerror` are implemented
with all `RTLD_*` flags (`LAZY` / `NOW` / `GLOBAL` / `LOCAL` / `NOLOAD` /
`DEEPBIND` / `NODELETE`) plus the `RTLD_DEFAULT` / `RTLD_NEXT` pseudo-handles.
`LD_DEBUG` (`all`/`libs`/`symbols`/`reloc`/`files`/`versions`/`bindings`) logs to
serial and `/system/log/ldso.log`; `LD_PRELOAD` is honoured (dropped for
setuid/setgid). An `r_debug` rendezvous + `_dl_debug_state` hook lets a future GDB
attach rescan the loaded library list on each `Dlopen`/`Dlclose`.

## 6. Verification  -  current honest state

- **`dyntest`** (`kurono.dyntest` cmdline token, or the `dyntest` shell command) is
  the first real exercise of the dynamic path: it loads `/usr/bin/dyntest`  -  a
  musl PIE with `PT_INTERP`  -  through ld-kurono, recurses into
  `libc.musl-x86_64.so.1` from `/system/lib`, relocates, sets up TLS + auxv, and
  runs. The boot gate reports **`DYNTEST_END rc=0`** (also surfaced by
  `kurono.logcheck`), proving the dynamic-load + musl-libc resolution path.
- **File-backed `mmap` of `.so` segments** landed (commits `e3a5952`, `df45ed1`):
  musl can `mmap` its shared-object segments from a regular file.
- **Firefox is NOT yet confirmed running on-device.** A real Firefox 140.11.0esr
  is cross-compiled against musl + Wayland (174 MB `libxul.so`), but two items
  remain: (1) bringing libxul's full `.so` dependency closure onto the OS and
  loading it through ld-kurono, and (2) lifting the **<4 GB user-pointer ABI
  limit** in the syscall layer (see [LINUX_SYSCALL.md](LINUX_SYSCALL.md) §3).

## 7. Limits (`ld_kurono.h`)

256 libs per process, 1024 global, 64 `DT_NEEDED` deps, 32 search paths, 16
preloads.

## 8. Related files

- `src/linux/ld_kurono.cpp` / `.h`  -  the linker (the `.h` header comment is the
  authoritative capability list)
- `src/kernel/elf_loader.cpp`  -  `PT_INTERP` detection + handoff to `ExecPIE`
- `src/linux/linux_syscall.cpp`  -  `execve`, `mmap` (file-backed), `arch_prctl`,
  path translation to the interpreter name
- `src/kernel/userspace.cpp`  -  `UserspaceEnter` (programs the user FS base)
- `src/kernel/kurono_kernel.cpp`  -  the `kurono.dyntest` boot gate
