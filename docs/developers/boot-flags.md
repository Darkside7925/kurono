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
| `kurono.sched=2` | on | full-SMP scheduler: user tasks run on all online CPUs. This is the default now. |
| `kurono.sched=1` (or `=0`) | off | the old scheduler: kernel processes and program leaders on cpu 0, threads pinned |
| `kurono.maxcpus=N` | no cap | cap on online CPUs. Under `kurono.sched=1` EFI boots cap at 8 |
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
| `kurono.heapdirect=0` | on | kernel heap allocations of 256 KB and up come from page frames. `0` keeps them in the heap regions, the old way |
| `kurono.heapreserve=0` | on | background heap growth off |
| `kurono.diskguard=0` | guard on for EFI boots | turn off the bare-metal disk guard. With the guard on, every disk is read-only except the GPT partition named `KURONO`. Leave it on |
| `kurono.kmemx=0` | n/a | kill switch for KMemX (the memory compressor). It is in the kernel but does not actually compress pages yet |

## Linux layer

These are the everyday fixes from the part 3 soaks. Each one has a `=0` (or `=old`) switch
that brings back the behaviour before the fix. Use them only to A/B a bug.

| flag | default | what it does |
| --- | --- | --- |
| `kurono.zcopy=old` | new behaviour | `sendfile` and `copy_file_range` answer EINVAL, so `cat` and `cp` fall back to read/write. `old` brings back the in-kernel copy loops, which made `cat` print nothing |
| `kurono.pipeblock=0` | on | a blocking write into a full pipe waits like Linux, and a write with no reader raises SIGPIPE. `0` returns EAGAIN and no signal |
| `kurono.symlinks=old` | new behaviour | the old symlink resolution |
| `kurono.getdents=nodots` | dots listed | leave out the `.` and `..` entries in directory listings |
| `kurono.sigchld=0` | on | SIGCHLD is not sent to the parent when a child exits |
| `kurono.sigsusyield=0` | on | `rt_sigsuspend` does not park the task. The old bsp in-place wait |
| `kurono.exitsig=0` | on | `exit` ignores a pending fatal signal. On (the default) a program killed by a signal reports death by that signal. `0` brings back the bug where `kill` of a sleeping program reported exit 0 |
| `kurono.futexsig=0` | on | a caught signal does not end a `FUTEX_WAIT`. This turns the Claude Code freeze back on |
| `kurono.pidmono=0` | on | pids are the table slot plus 100 again, so a new process can get the pid of one just reaped |
| `kurono.pgrpzombie=0` | on | an exited child leaves its process group at once. This brings back "can't set tty process group" in pipelines |
| `kurono.vfcopyfix=0` | on | the old vfork stack copy-back, which read past the end of each source page |

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
| `kurono.igpu.legacytc=1` | keep a type-C port that is wired in legacy mode connected, for an external monitor on that port. Off by default |
| `kurono.vgsync=1` | go back to the blocking virtio-gpu present (the async one is default) |
| `kurono.bootanim=0` / `kurono.splash=slow` | old logo and bar instead of the boot animation |

At runtime there's a `gpuaccel` shell command to enable, disable, query and self-test the
Intel acceleration module.

## Security

| flag | default | what it does |
| --- | --- | --- |
| `kurono.wp=1` | off | sets CR0.WP on every CPU, so a kernel copy-out breaks copy-on-write instead of writing the parent's pages. Off keeps whatever WP bit each CPU booted with. Off by default |
| `kurono.ken.enforce=0` | enforcement ON | turn the admin gate off (debugging only) |
| `kurono.ksa.prompt[=cred\|nocred]` | off | show the KSA approval prompt demo at boot, with or without a password field |
| `kurono.ksa.test` | off | KSA self-test |
| `kurono.kentest=1` | off | run the ken deny-case test program |

The KSA on/off switch is not a boot flag. It is a setting in Settings, see `security/KSA.md`.

## Firefox / Linux layer

| flag | default | what it does |
| --- | --- | --- |
| `kurono.fftrace=1` | off | turn the Firefox debug probes back on (very chatty) |
| `kurono.pincpu=0` | on under `sched=1`, off under `sched=2` | don't hard-pin threads to a CPU. Only matters with `kurono.sched=1`. Debugging only |
| `kurono.apthreads=0` | on for EFI | turn off running user threads on the secondary CPUs |
| `kurono.ffjit=1\|2` | off | Firefox JIT experiment |
| `kurono.gui.run="..."` | none | launch a GUI app at startup, e.g. `firefox+https://...` |
| `kurono.cli.run="..."` | none | run a shell command at startup |

## Virtual machines

| flag | default | what it does |
| --- | --- | --- |
| `kurono.vm.smp=N` | dynamic | vCPUs for the Alpine guest, 1 to 8. Without it, a machine with 12 or more CPUs runs up to 4 vCPUs in parallel, one host thread each |
| `kurono.vm.par=1` | on for 12+ CPUs | parallel vCPUs, one kernel thread per vCPU on its own host CPU |
| `kurono.vm.fixed=1` | off | the old mode: eager guest RAM and one vCPU |
| `kurono.vm.pt=BB:DD.F[,BB:DD.F]` | none | reserve PCI devices at boot for a Kurono guest. VT-d is required. GPUs, storage controllers, bridges and NVIDIA devices are refused |
| `kurono.vmram=N` | 1024 | default guest RAM in MB. Settings shows it as a boot option until the next restart |
| `kurono.vmdisk=N` | 16 | virtual guest disk size in GB, 0 to 64. `0` is just the image |
| `kurono.kld.sup=1` | off | the KLD supervisor. Restarts a driver container that crashes or hangs, and makes `kld reload <family>` work |
| `kurono.kld.sup.hang=SECS` | 30 | seconds a KLD container may go quiet before the supervisor restarts it. 5 to 3600 |

Guest RAM is backed on first touch and returned with free page reporting, and the disks
grow on demand, so these defaults do not need flags.

## Audio

| flag | default | what it does |
| --- | --- | --- |
| `kurono.audiosplit=1` | off | run the audio pacing loop in its own kernel process (`audiopace`). Off keeps the single old process |

## Debug probes

These write to serial. Leave them off unless you are chasing the bug they name.

| flag | default | what it does |
| --- | --- | --- |
| `kurono.lktrace=1` | off | log advisory locks and `-shm` maps on the opencode and sqlite paths |
| `kurono.ubtrace=1` | off | check that a thread resumes with the rsp and callee-saved registers it left with, and log a mismatch |
| `kurono.envwatch=1` | off | watch writes to the Linux environment page. Used to find the corruptor that broke Firefox's environment |
| `kurono.pflooptest=1` | off | `[pfloop]` self-test for the demand-zero fault loop |

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
