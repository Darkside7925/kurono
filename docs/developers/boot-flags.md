# Boot flags

Kurono reads its options from the kernel command line (`kurono.*` tokens). With GRUB, press
`e` on the entry and append them to the `multiboot2` line. With the EFI loader the
command line is compiled in (`efi kurono.autologin=1` for the normal one, see
`src/Makefile`); to change it for the EFI loader, rebuild.

Flags are plain tokens. `kurono.foo=1` turns something on, `kurono.foo=0` turns it off,
`kurono.foo=value` sets a value. Every flag below I checked against the source
(mostly `src/kernel/kurono_kernel.cpp`). Defaults are the defaults of the current tree.

## The ones you'll actually use

| flag | default | what it does |
| --- | --- | --- |
| `kurono.autologin=1` | off (on in the EFI loader line) | skip the login screen |
| `kurono.sched=2` | off | full-SMP scheduler: user tasks run on all online CPUs. Needed for Firefox to feel right on many cores |
| `kurono.maxcpus=N` | 8 on EFI boots | cap on online CPUs |
| `kurono.r3apps=a,b,c` | none | run those kui apps as separate ring-3 programs. Names: `calculator`, `texteditor`, `files`, `taskmgr`, `mediaplayer`, `denji` (video player), `terminal`, `settings`. `1` or `all` means every app. The value is cut at 47 characters |
| `kurono.uiscale=NNN` | 100 | UI scale in percent, 100 to 300 |
| `kurono.hz=N` | panel rate | fixed compositor refresh cap, 60 to 360 |
| `kurono.text=1` | off | text-mode console (what the emergency entry uses) |
| `kurono.taskmem=1` | off | per-task kernel heap accounting. Shows up in the Task Manager |

`kurono.r3apps` can also be set persistently with the `apps.ring3` key in `ui.conf`.
Apps not listed run as the in-kernel version of the same source.

## Memory and storage

| flag | default | what it does |
| --- | --- | --- |
| `kurono.memcap=N` | no cap | limit the memory the physical allocator manages, in GB. `kurono.memcap=16` is the old 16 GB cap |
| `kurono.diskguard=0` | guard on for EFI boots | turn off the bare-metal disk guard. With the guard on, every disk is read-only except the GPT partition named `KURONO`. Leave it on |
| `kurono.kmemx=0` | n/a | kill switch for KMemX (the memory compressor). It is in the kernel but does not actually compress pages yet |

## Graphics (GPU acceleration is experimental)

These are **all off by default** and stay off in the normal EFI loader. They exist for
the test loader built by `make gpu-efi`, which uses:

`kurono.autologin=1 kurono.uiscale=100 kurono.wpdamage=1 kurono.igpu.accel=1 kurono.igpu.zerocopy=1`

| flag | what it does |
| --- | --- |
| `kurono.igpu.accel=1` | Intel display-engine acceleration. Right now that means the hardware cursor plane. It validates scanout page-table entries first and self-tests |
| `kurono.igpu.accel.noptefb=1` | turn off the fallback that maps the framebuffer tail when firmware didn't |
| `kurono.igpu.zerocopy=1` | zero-copy present. Needs `igpu.accel` and `wpdamage`. Renders into a RAM buffer and flushes damage |
| `kurono.wpdamage=1` | wallpaper is only redrawn where damaged, instead of every frame |
| `kurono.multimon=0` | disable the multi-monitor path, single framebuffer only |
| `kurono.vgsync=1` | go back to the blocking virtio-gpu present (the async one is default) |
| `kurono.bootanim=0` / `kurono.splash=slow` | old logo and bar instead of the boot animation |

At runtime there's a `gpuaccel` shell command to enable, disable, query and self-test the
Intel acceleration module.

## Security

| flag | default | what it does |
| --- | --- | --- |
| `kurono.ken.enforce=0` | enforcement ON | turn the admin gate off (debugging only) |
| `kurono.ksa.prompt[=cred\|nocred]` | off | show the KSA approval prompt demo at boot, with or without a password field |
| `kurono.ksa.test` | off | KSA self-test |
| `kurono.kentest=1` | off | run the ken deny-case test program |

## Firefox / Linux layer

| flag | default | what it does |
| --- | --- | --- |
| `kurono.fftrace=1` | off | turn the Firefox debug probes back on (very chatty) |
| `kurono.pincpu=0` | pinning on | don't hard-pin threads to a CPU. Debugging only, it is slower and less stable |
| `kurono.apthreads=0` | on for EFI | turn off running user threads on the secondary CPUs |
| `kurono.ffjit=1\|2` | off | Firefox JIT experiment |
| `kurono.gui.run="..."` | none | launch a GUI app at startup, e.g. `firefox+https://...` |
| `kurono.cli.run="..."` | none | run a shell command at startup |

## Self-tests and test hooks

Each prints to serial. Many have a `.poweroff` variant that halts afterward.

`kurono.kmemxtest`, `kurono.kcltest`, `kurono.kjtest`, `kurono.klstest`,
`kurono.kdf.test`, `kurono.kinit.test`, `kurono.kfstest`, `kurono.dyntest`,
`kurono.wifitest`, `kurono.pmmtest=1`, `kurono.usbtest`, `kurono.nettest=1`,
`kurono.audiotest`, `kurono.findtest`, `kurono.taskmem.bench=1`.

Test hooks: `kurono.kui.open=<app>` opens a kui app once the desktop is up,
`kurono.guiterms=N` opens extra terminals, `kurono.setup=1` boots the setup flow.

## Advanced scheduler tuning (sched=2)

Only touch these if you're debugging the scheduler. Each is a one-binary A/B switch:
`kurono.schedrel=0` (early on_cpu release), `kurono.schedlaunch=0`, `kurono.schedcpus=0`,
`kurono.schedpoll=0`, `kurono.schedtrace=1`, `kurono.schedterm=0`, `kurono.kstackguard=0`.
