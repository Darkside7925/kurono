# KLD - Kurono Linux Driver Compatibility Layer

Runs **unmodified upstream Linux drivers** as ordinary Kurono userspace
processes. The Kurono kernel links none of this - that invariant is the whole
point (see `feats/kld-linux-driver-compat/readme.md` for the rationale and the
full phase plan).

**Phase 1: COMPLETE.** Phase 2 (the shim proper) is the next work.

## What works today

An unmodified Linux 6.12.99 `e1000e` driver compiles, links into a container,
loads as a Kurono ring-3 process, and **fully loads and unloads as a module** -
`init_module` returns 0, it registers as a PCI driver with its real 115-entry
Intel id_table, and `cleanup_module` tears down clean. See the census below for
the exact log.

The only thing missing is a device to bind to, which needs the kernel broker.

## Quick start

```sh
cd kld
bash tools/fetch_linux.sh          # download + configure + vendor the pinned tree
make driver=e1000e                 # build the container, print the census
./build/e1000e/kld-e1000e.elf      # run it (musl-static: runs on linux too)
make driver=e1000e census          # census only
```

To run it on Kurono: the container is embedded into the image and launched by
`kurono.kld=1`.

```sh
cd src && make iso KURONO_EXTRA_CMDLINE='kurono.kld=1'
```

## Current census (e1000e, kernel 6.12.99)

```
external symbols needed : 185
IMPLEMENTED (real)      : 79
STUBBED (aborts)        : 106   <- the phase-2 backlog
MISSING (no provider)   : 0     <- container links
real coverage           : 42%
```

**The module now fully loads and unloads.** `init_module` returns 0 and
`cleanup_module` runs clean, on Kurono:

```
[10379] [INFO ] e1000e: Intel(R) PRO/1000 Network Driver
[10383] [lx_pci] registered driver 'e1000e'
[10384] [lx_pci]   matches vendor=0x8086 device=0x105e
[10396] [lx_pci]   id_table entries = 115
[10397] [lx_pci] no devices bound: PCI enumeration needs the kernel broker
[10401] [kld] init_module returned 0
[10406] [kld] container done
```

Implemented for real: data symbols, memory, sync, time/delays/timecounter, pure
computation (crc32/bitmap/strscpy/ethtool/checksum), the dev_*/netdev_* log
families, tracing no-ops, and PCI driver registration.

**What remains is not shim work.** The ~106 stubs left are overwhelmingly
PCI config space, DMA, IRQ, netdev/skb and PM - all of which need the
**kernel-side broker** (BAR mapping, MSI doorbell, DMA delegation, net class
registration). They cannot be implemented in the container alone, by design.

The census is the **single source of truth for progress**. Not vibes, not
"mostly done". `make census` after every change.

Top of the queue by reference count: `usleep_range_state`, `__const_udelay`,
`msleep`, `netdev_err`, `kfree`, `netdev_info`, `request_threaded_irq`,
`skb_put`, `system_wq`, `timecounter_*`, `pcie_capability_read_word`.
`__pci_register_driver` is what the driver hits first, so PCI + the device model
(phase 2.6) is the natural first block even though it is not the most-referenced.

## Layout

| path | what |
|---|---|
| `KLD_KERNEL_VERSION` | the pinned kernel. **Do not drift.** Bumps are a project. |
| `Makefile` | out-of-tree build harness |
| `lx_emul/lx_emul.h` | shim surface + `LX_UNIMPL` + module registry |
| `lx_emul/lx_emul.c` | logging, the abort trap, the registry |
| `lx_emul/lx_printk.c` | `printk` -> serial, with Linux level mapping. **Real, not a stub.** |
| `lx_emul/lx_main.c` | container entry: calls `init_module()` |
| `tools/fetch_linux.sh` | download + `modules_prepare` + vendor headers |
| `tools/gen_headers.sh` | synthesize generated headers (fallback path) |
| `tools/gen_stubs.sh` | emit an aborting stub per unprovided symbol |
| `tools/symcensus.sh` | the progress bar |

## Things learned building this (do not rediscover)

- **A configured kernel tree is mandatory.** `generated/autoconf.h`,
  `timeconst.h`, `bounds.h`, `asm-offsets.h` and the `asm/` wrappers only exist
  after `make modules_prepare`. Several are numeric constants derived from the
  config that decide struct layout - hand-writing them means silently guessing.
  `fetch_linux.sh` runs the kernel's own build to get them.
- **Guard `modules_prepare` on `bounds.h`, not `autoconf.h`** - `defconfig`
  creates autoconf.h by itself, so guarding on it skips the prepare step and you
  get a half-generated tree.
- **Copy Kbuild's `LINUXINCLUDE` order verbatim.** The arch `generated/` dirs
  must precede the generic ones or you get "asm/types.h: No such file".
- **`-DMODULE` is load-bearing.** Upstream's `module_init(x)` then expands to an
  `init_module()` alias, which is exactly the entry point to call. No macro
  surgery needed.
- **The stub generator must exclude three things** or it breaks: the driver's own
  cross-TU definitions (else "multiple definition"), the *previous* build's
  `lx_stubs_gen.o` (else it emits an empty file), and libc builtins.
- **Data symbols were the known hole; they are now CLOSED** in
  `lx_emul/kimpl/lx_data.c` (`jiffies`, `__this_module`, `phys_base`,
  `vmemmap_base`, `page_offset_base`, `__cpu_online_mask`, `boot_cpu_data`,
  `system_wq`, `kmalloc_caches`, `param_ops_*`). A generated stub is a zero-arg
  FUNCTION - it links for a data reference but makes the driver read code bytes
  as a value, which is silent corruption rather than a loud abort. Any new data
  symbol the census turns up must be added there, not left to the generator.
- **Compile shim implementations in KERNEL context** (`lx_emul/kimpl/*.c`, built
  with the driver's own CFLAGS and headers) so the COMPILER checks every
  prototype. A shim function with a subtly wrong signature is exactly the kind of
  bug no amount of care catches by hand.
- **`lx_emul.h` must NOT be included from kernel context** - it pulls
  `<stdint.h>`, which collides with the kernel headers on `int64_t`. Kernel-context
  files include `kimpl/lx_kshim.h` instead.
- **`__SCT__*` static-call trampolines cannot be defined in kernel context** -
  their declared static-call type conflicts with `void(void)`. They live in
  `lx_emul/lx_sc.c` (userspace context) as real no-ops.
- Build deps beyond a normal toolchain: `flex bison libelf-dev libssl-dev bc`.

## Licensing

The vendored tree and the driver sources are GPLv2 (see
`third_party/linux-<ver>/COPYING`). The container is a separate process with a
defined IPC boundary to the Kurono kernel - the same posture as NetBSD rump
kernels and Genode's DDE-Linux. The vendored tree is **not committed**; it is
reproducible from `KLD_KERNEL_VERSION` via `tools/fetch_linux.sh`. Get a real
legal opinion before distributing binaries; source-only distribution sidesteps
most of the question.
