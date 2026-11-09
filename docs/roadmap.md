# Kurono roadmap

Where Kurono is going, and what I'm honest about not having yet. Dates aren't promised.
The order is priority, not a schedule. I work on this alone, around other things.

Status tags: **[done]** shipped and tested, **[wip]** being worked on, **[next]**
queued, **[later]** wanted but not started.

## Where it is right now: Kurono Kintsugi, Version 1 (part 2)

Kintsugi is the release where I go back and fix what was cracked. It's in progress. This
push is part 2. What has landed so far:

- **[done] Boots on real hardware.** Lenovo Legion Pro 7 (Arrow Lake-HX) via EFI, with a
  disk guard so it only writes to the `KURONO` partition. GRUB ISO for VMs.
- **[done] Full-SMP scheduler mode** (`kurono.sched=2`, up to all cores) and a real
  per-task `/proc`, so `ps`, `top` and the Task Manager show real data.
- **[done] Linux ABI layer.** Runs unmodified Firefox (real sites load), BusyBox and the
  recovery tools. Wayland server with a system clipboard shared across native apps,
  Wayland clients and the Alpine guest, plus Super+V history.
- **[done] Apps as ring-3 programs** through kui: calculator, text editor (tabs, syntax
  highlighting, regex find), files, task manager, media player, video player, terminal,
  settings (via a settings service). In-kernel fallbacks stay.
- **[done] New desktop pieces.** Control center, window snapping, Alt+Tab, show desktop,
  per-app window memory, files app polish (thumbnails, drives, multi-select, drag and
  drop, trash restore, search, open-with).
- **[done] Security.** KSA approval prompt in an isolated hypervisor context, `ken`
  (sudo-like) with `sudo`/`supr` aliases, and an admin gate on system paths, raw disks,
  cross-user signals, the driver broker and more. Ring-3 programs run as a normal user.
- **[done] Networking reliability.** Proper DHCP renew/rebind/expiry, DNS fallback, TCP
  retransmit timing, Wi-Fi reconnect.
- **[done] Drivers.** NVMe, AHCI/SATA, xHCI USB, Intel/igc/e1000 NICs, Intel display with
  multi-monitor, HDA and USB audio. KLD runs unmodified Linux drivers (e1000e) in a ring-3
  sandbox.
- **[done] Alpine Linux in my own hypervisor**, launching apps onto the desktop, with
  clipboard and audio bridged.
- **[wip] USB hubs.** Hub and transaction-translator support is written. Hardware test
  is pending.
- **[wip] Debian guest.** Still being tested.
- **[wip] Firefox on heavy sites.** Works, but slow and flaky on heavy pages.
- **[wip] Intel GPU acceleration.** Hardware cursor plane, zero-copy present, damage-only
  wallpaper. Behind `kurono.igpu.accel`, `kurono.igpu.zerocopy` and `kurono.wpdamage`,
  **off by default**, still being tested on the laptop.
- **[wip] KMemX.** The memory compressor exists but doesn't compress anything in
  practice yet.
- **[later] ARM and 32-bit.** Not supported.

## Near term

Goal: something I can use every day without it surprising me.

- **[next] Daily-driver stability.** Fewer hangs and crashes on real hardware, the
  remaining scheduler and memory races, Firefox reliability on heavy sites, less CPU
  burn when idle.
- **[next] Finish and ship Kintsugi V1.** Remaining parts, final testing on the laptop,
  release notes.
- **[next] GPU acceleration, for real.** Get the hardware cursor and zero-copy path
  solid on the laptop, make them safe to enable by default, then look past it
  (plane composition, eventually command submission).
- **[next] Installer.** A proper install-to-disk flow: partitioning, the `KURONO`
  partition, EFI boot entry, first-run setup. Today you run it from an ISO or USB.
- **[next] Hardware coverage.** Test USB hubs on real hardware, more Wi-Fi chips, more
  laptops than mine.

## Mid term

- **[later] Package store.** `kpkg` works, but it's basic. I want a proper store with a
  signed index, versions, dependencies and a GUI.
- **[later] KLD expansion.** More Linux drivers running in the sandbox (Wi-Fi, GPU
  pieces, audio), and an IOMMU-backed isolation story so a driver can't DMA anywhere.
- **[later] Move services out of the kernel.** The settings service, audio server,
  clipboard and compositor pieces are still in ring 0. The kui apps are already out. The
  plan is to push more of these into ring-3 services with narrow interfaces.
- **[later] Stronger KSA.** More operations behind the hypervisor-isolated prompt, a
  clearer audit trail, and tighter policy for what ring-3 programs can ask for.
- **[later] ext4 write path and KFS as a mounted root.** So the installer targets and
  guest images are fully read-write.
- **[later] Power management.** Suspend/resume, CPU frequency, battery. (CPU frequency
  work hung the laptop at boot once, so that's being done carefully.)
- **[later] Real signal delivery** and more of the Linux ABI so more unmodified apps run.

## Long term

- **[later] Low RAM goal.** A lean base system under 50 MB of RAM. It boots to a desktop
  on 512 MB today; the plan is to get the base way under that by moving services out of
  the kernel and trimming what's loaded at boot.
- **[later] Windows app support**, someday. Probably a compatibility layer in the same
  spirit as the Linux one. Not started, and a very big job.
- **[later] ARM and 32-bit ports.** x86_64 only right now. ARM first, 32-bit second.
- **[later] More native apps** and a better app story for third parties.

## How to read this

Things marked [done] mean I tested them in a VM and on my laptop where relevant. They
can still have bugs. Things marked [wip] are in the tree but I won't tell you they work
until I've seen them work on real hardware.

For the details of what's in each area, see `developers/architecture.md`.
