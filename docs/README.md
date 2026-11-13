# Kurono OS docs

Kurono is a from-scratch x86_64 operating system I'm writing in C++17 (plus a bit of
NASM and C). It has its own kernel, desktop, shell, drivers, a Linux compatibility layer
that runs real Linux programs, and a small hypervisor. I'm working on it as a solo
student developer, so these docs are written the way I'd explain it to a friend.

The release I'm working toward is **Kurono Kintsugi, Version 1**. It isn't out yet.
This push is part 3 of the major update: the infrastructure and support work that comes
before Kintsugi. It is not the final version, and some parts are held back because of
bugs and issues, so expect rough edges.

## What part 3 adds

OpenCode and Claude Code run natively, a Debian 13 guest boots, Alpine can use up to four
vCPUs in parallel on bigger machines, Intel Wi-Fi (BE200) works through KLD, the
privilege command is now `amdo`, KSA can be switched off for now, and a round of
everyday Linux fixes landed (cat and cp, pipes, symlinks, file times, directory listings).

## How much code is actually in here

`src/` is about 130,000 lines of hand-written freestanding C++17, NASM and C. A raw
`wc -l` says far more than that, but most of the difference is wallpaper pixel data
sitting in `src/ui/wallpaper.h` and `src/ui/wallpaper2.h`. That is not code. The vendored
stb glue in `src/third_party/` is a bit more on top.

## New here

Start with these, in order:

1. `beginners/README.md` the short intro and reading order
2. `beginners/what-is-kurono.md` what works today and what doesn't
3. `beginners/try-in-a-vm.md` the safe way to try it (QEMU)
4. `beginners/boot-on-hardware.md` booting on a real machine without losing data, and the
   root files the KURONO partition needs
5. `beginners/faq.md` the questions I get asked the most
6. `beginners/FIRST_STEPS.md`, `PROJECT_TOUR.md`, `BUILD_AND_RUN.md` for finding your way
   around the source tree

## Here to change code

1. `developers/README.md` index of everything under `developers/`
2. `developers/build.md` building, the ISO, the EFI loaders, test runs
3. `developers/architecture.md` the big picture: kernel, Linux layer, Wayland, kui apps,
   KSA and amdo, hypervisor, KLD
4. `developers/boot-flags.md` every boot flag I'd tell you about, and its default
5. `developers/kui.md` the app toolkit and how apps run in ring 3
6. `developers/how-it-works/BOOT_TO_DESKTOP.md` and `SUBSYSTEMS.md`
7. `developers/reference/FILE_MAP.md` keep it open while you read code

## By topic

- **Display / graphics:** `developers/drivers/GPU_PROBE.md`, `DISPLAY_MGR.md`,
  `GRAPHICS.md`, `developers/ui/WAYLAND_SERVER.md`, `WINDOW_MANAGER.md`, and the older
  `HYBRID_GPU_OPTIMUS_GUIDE.md` (hybrid laptop graphics and framebuffer ownership).
- **Storage and paths:** `developers/fs/KVFS.md`, `KFS.md`, `developers/system/LOGGING.md`,
  `developers/drivers/NVME.md`.
- **Linux runtime:** `developers/linux/LINUX_SYSCALL.md`, `LD_KURONO.md`,
  `developers/net/NETWORK.md`.
- **Security:** `developers/security/KEN.md`, `KSA.md`.
- **Scheduler and multi-core:** `developers/proc/SCHEDULER.md`, `SMP.md`.
- **Virtualization:** `developers/virt/HYPERVISOR.md`, `EPT.md`, `VMEXIT.md`.

## Where the project is going

`roadmap.md` has the near, mid and long term plan.

## A note on how current each page is

Everything here was updated for part 3, but a few of the older per-file pages under
`developers/` may still lag behind the code. When a page and the source disagree, the
source wins. If you find a page that is wrong, tell me.
