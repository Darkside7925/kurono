# What is Kurono?

Kurono is an operating system I wrote from scratch for 64-bit x86 PCs. It is not a Linux
distro and it is not based on Linux code. The kernel, the desktop, the apps, the network
stack and most of the drivers are my own. Where it needs Linux software, it pretends to be
Linux well enough that real Linux programs run on it.

The release I'm working toward is **Kurono Kintsugi, Version 1**. It isn't out yet. What
you get today is part 3 of the major update: the infrastructure and support work that
comes before it. It is not the final version, and some features stay behind flags until
they stop breaking things.

## What works today

**It boots on real hardware.** My main test machine is a Lenovo Legion Pro 7 (Intel Arrow
Lake-HX). It boots there through EFI. For virtual machines there is a GRUB ISO.

**It has a desktop.** Compositing window manager, menu bar and dock, lock screen, a
control center (Wi-Fi, volume, brightness, night light, power actions, media controls),
notifications, window snapping, Alt+Tab with live thumbnails and Super+D for show desktop.
There is one system clipboard shared by native apps, Wayland apps and the Alpine guest,
with a Super+V history popup.

**It has its own apps.** Calculator, text editor (tabs, syntax highlighting, regex find
and replace), files, task manager, media player, video player, terminal and settings. Most
of them can run as separate ring-3 programs built on the kui toolkit, which is the
direction everything is going. There are also in-kernel fallback versions.

**It runs Linux programs.** The Linux ABI layer runs unmodified Firefox (it loads real
websites, but heavy sites are slow and sometimes flaky), BusyBox, and a set of recovery
tools like `ntfsfix` and `e2fsck`. It also runs OpenCode, a terminal coding tool, and
Claude Code. A freeze that used to hit Claude Code in long sessions is fixed, and a
2 h 50 min session with heavy tool use had no faults. Linux apps talk to Kurono through a
built-in Wayland server.

**It has its own hypervisor.** Alpine Linux runs inside it and can launch apps that show
up on the Kurono desktop. On machines with 12 or more CPUs, Alpine gets up to four vCPUs
that run in parallel. Debian 13 also runs as a guest.

**It has a real security model.** Programs run as a normal user. Anything that touches
system paths, raw disks, other users' processes or the driver broker asks for approval.
The approval prompt (KSA) is drawn from inside an isolated hypervisor context so a
compromised kernel can't fake a click. `amdo` is the privilege command. It used to be
`ken`, and `ken`, `sudo` and `supr` still work as aliases.

**It has real drivers.** NVMe, SATA/AHCI, USB (xHCI, including hubs), Intel and igc and
e1000 network cards, Intel display with multiple monitors, HDA and USB audio.

**It can run unmodified Linux drivers.** KLD loads a Linux driver as an untrusted ring-3
process that talks to the hardware only through a kernel broker. I've done e1000e. Intel
Wi-Fi (iwlwifi, for the BE200 card) runs the same way now.

**It uses all the cores by default.** Scheduler mode 2 is the default. On EFI boots every
CPU the firmware lists comes online. `/proc` is real now, so `ps` and `top` work.

## What is not ready

I'm listing these so nobody is surprised.

- **Intel GPU acceleration** (hardware cursor plane, zero-copy present, damage-only
  wallpaper) exists behind flags and is **off by default**. I'm still testing it on real
  hardware. On my laptop an earlier version of it broke the desktop, so I pulled it from
  the default boot line.
- **USB hub support** is new. It is written and works against the spec in my tests, but
  I haven't tested it on real hardware yet.
- **KMemX** (memory compression) is in the kernel but does not actually compress pages in
  a real boot yet.
- **Firefox** is the heaviest thing I run. It works, but it is slow under software
  rendering and some heavy pages hang or stutter.
- **ARM and 32-bit** are not supported. This is x86_64 only for now.
- **Installer:** there is a setup entry in the boot menu, but a polished "install to disk
  and boot from it" story is still on the roadmap.
- **Windows apps** don't run. That is a far-future idea (see `../roadmap.md`).

## Why "Kintsugi"?

Kintsugi is the Japanese art of repairing broken pottery with gold, so the cracks become
part of the piece. This release is basically me going back through everything that was
cracked (scheduler, memory bugs, input, security) and fixing it properly instead of
working around it.

## Next

Try it: `try-in-a-vm.md`.
