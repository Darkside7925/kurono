# Dynamic VM

How the background guest uses host memory, disk and CPUs. The pieces are in `src/virt/guest_mem.cpp`,
`vballoon.cpp`, `vblk.cpp`, `vm_host.cpp` and `hypervisor.cpp`. "Dynamic" is the default mode. The
old mode, with the whole guest RAM allocated up front and one vCPU, is `kurono.vm.fixed=1`.

## Lazy guest RAM

High guest RAM is split into 2 MB chunks (`GUEST_HI_CHUNK_LAZY`). A chunk gets host frames when
the guest, or a device model, first touches it. Until then it costs nothing on the host.

If no 2 MB contiguous host block is free for a chunk, the chunk is backed by 512 separate 4 KB
pages instead. The VM keeps running. `kurono.vm.splitmem=1` forces that for every chunk and
`kurono.vm.splitmem=2` for the odd chunks, for tests.

Lazy RAM is off with `kurono.vm.fixed=1`. It is also off for a `vm kurono` guest that has
passthrough devices reserved, because those need their memory pinned.

## Free page reporting

`vballoon.cpp` is a virtio-balloon device with only the free page reporting queue. Inflate and
deflate are not offered. The guest reports 2 MB aligned runs of free pages, and every lazy chunk
that lies wholly inside a reported run goes back to the host. The count of released bytes is
kept per VM.

The guest needs the `virtio_balloon` module. In lazy mode the guest agent loads it with
`modprobe virtio_balloon` once it is up.

## Guest RAM size

The size of the next start comes from, in order:

1. `kurono.vmram=N` on the command line (128 to 3008 MB)
2. the `linux.ram_mb` setting, if it is 128 MB or more
3. in dynamic mode, half of the host RAM, capped at 3008 MB, when that is at least the default
4. the default, 1024 MB

Guest RAM never goes above 3008 MB. The cap keeps the guest below the 0xC0000000 vPCI window.

## The idle vCPU

An idle guest executes HLT. The VM host thread takes the exit, advances past the HLT, and sleeps
until an interrupt is pending instead of spinning on the host CPU. A slice that ends idle makes
the host thread sleep for 1 ms. Parallel vCPUs wait with `sti; hlt` on their own host CPU.

The CPUID leaf that advertises MWAIT is hidden from the guest. Linux would idle in MWAIT, and an
MWAIT that returned at once kept the host CPU at 100 percent. The guest uses HLT.

## Chunked guest disks

The guest disk is one image plus chunk files.

- `ALPINE.IMG` or `DEBIAN.IMG` is the first part of the disk.
- The virtual size comes from `linux.disk_gb` (default 16, at most 64) or `kurono.vmdisk=N`.
  `kurono.vmdisk=0` means just the image.
- The rest is made of 64 MB chunk files named after the image (`ALPINE.000`, `ALPINE.001`, and so
  on, or `DEBIAN.000` ...). They sit in the KURONO partition root and are created on the first
  write to them. A chunk that does not exist reads as zeros.
- Once the network is up, the guest grows its ext4 onto `/dev/vda` with `resize2fs`, installing
  `e2fsprogs-extra` on Alpine or `e2fsprogs` on Debian first. The log is `/var/log/kurono-resize.log`.

## Parallel vCPUs

Parallel vCPUs run the Alpine guest on more than one host CPU. Each vCPU has its own kernel
thread on its own service CPU, a big VM lock around the exit handlers and pumps, and per-CPU
VMXON and VMCS state.

- With no setting, a machine with 12 or more CPUs runs `min(4, (cpus - 4) / 2)` vCPUs, and turns
  the parallel threads on. Smaller machines stay at one vCPU.
- `kurono.vm.smp=N` sets the vCPU count, from 1 to 8. `kurono.vm.smp=1` keeps one vCPU.
- `kurono.vm.par=1` turns the parallel threads on. It needs more than one vCPU.
- `kurono.vm.fixed=1` forces one vCPU and the old fixed RAM mode.

Debian stays at one vCPU, because its cloud kernel has no MP table. A `vm kurono` guest and a
`vm kloop` build guest stay at one vCPU as well.

## Files

- `src/virt/guest_mem.cpp` and `guest_mem.h` lazy high RAM chunks, the 4 KB fallback, the RAM cap
- `src/virt/vballoon.cpp` and `vballoon.h` free page reporting
- `src/virt/vblk.cpp` and `vblk.h` the chunked virtio-blk disk
- `src/virt/vm_host.cpp` the RAM, disk and vCPU choices above
- `src/virt/hypervisor.cpp` the parallel vCPU run loop and idle sleep
