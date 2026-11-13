# The kloop dev loop

`src/virt/kloop.cpp` and `kloop.h`, with the guest side in `tools/selfhost/` (`kloop_build.sh`,
`kxfer.c`, `guest_build.sh`). `vm kloop` builds a Kurono kernel inside the Alpine guest, then boots
that kernel as a guest. The whole loop runs from one command on Kurono.

```
vm kloop [clean] [boot secs] [guest MB] [guest cmdline...]
```

The defaults are a 60 second boot and a 2048 MB guest. `clean` throws away the guest's build tree.

## What it does

1. The source tarball has to be in place. The loop reads `/home/user/vmshare/src.tar`. If that
   file is missing, it takes `KSRC.TAR` from the KURONO partition root and copies it in.
2. The Alpine VM starts, if it is not already up.
3. `kxfer.c` is pasted into the guest and the toolchain is installed. apk is retried while another
   apk holds the guest's database lock.
4. The guest unpacks the source into `/root/kb` and builds `kurono_base.elf` there. The build is
   incremental: `kloop_build.sh` copies only the files whose content changed with `rsync -c`, so
   make sees just those. A changed header, Makefile, linker script or asm include forces a clean
   build, and so does `vm kloop clean`.
5. The guest pushes back `kguest.elf` (debug stripped) and the build log.
6. The build guest stops. The new kernel boots as a Kurono guest for the given number of seconds.
   Its com1 streams into `kguest.log`, and the screen is saved as `kguest.png`.
7. The guest is killed, and a summary comes back to the shell. The summary is also written to
   `kloop.log`.

The loop runs in the caller's shell context and blocks it until the loop finishes. Its last line
reports how long the whole run took.

## The source tarball

The tarball must hold these paths:

```
src/  Alpine/  kld/  tools/  logo.h  pciids.h
```

Put it at `/home/user/vmshare/src.tar`, or as `KSRC.TAR` in the KURONO partition root. The
share is the v9fs directory `/home/user/vmshare`, the only host directory a guest sees.

## Output files

All of these land in `/home/user/vmshare`:

| File | What |
| --- | --- |
| `kmake.log` | the guest build log |
| `kguest.elf` | the built kernel, debug stripped |
| `kguest.log` | the guest kernel's serial log, live |
| `kguest.png` | the guest screen |
| `kloop.log` | the summary of each run |

## Build time

The first build in a fresh guest is a full build. The guest keeps its tree in `/root/kb` on the
guest disk, so the next run only rebuilds what changed. A repeat run took about 76 seconds in the
tests for part 3 of the major update.

## Files

- `src/virt/kloop.cpp` the loop and its guest steps
- `src/virt/kloop.h` the entry point, `KLoop::Run()`
- `tools/selfhost/kloop_build.sh` the incremental guest build
- `tools/selfhost/kxfer.c` the guest-side transfer tool
- `tools/selfhost/guest_build.sh` the guest build helper
- `src/virt/v9fs.cpp` the share behind `/home/user/vmshare`
- `src/shell/linux_cmds.cpp` the `vm kloop` argument parsing
