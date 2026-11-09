# Boot Kurono on real hardware (safely)

Kurono does boot on real machines. My test machine is a Lenovo Legion Pro 7 with an
Intel Arrow Lake-HX CPU. That is the one I can vouch for. Other machines might work and
might not. Read this whole page first.

**Be honest with yourself about the risk:** this is a hobby OS with a lot of drivers I
could only test on one laptop. The worst realistic outcome is that it hangs or shows a
black screen and you reboot. I have built in a disk guard to make it hard for Kurono to
write to the wrong disk, but "hard" is not "impossible". Back up anything you care about
and don't test on a machine you can't afford to fiddle with.

## What protects your disks

When Kurono boots through the EFI loader (the real-hardware path), it turns on a **disk
guard** before any storage driver starts: every disk is read-only except a GPT partition
named `KURONO`. Your Windows or Linux drive is not written to. You can turn it off with
`kurono.diskguard=0`, but please don't, unless you really know why. The NVMe driver also
guards real (non-emulated) controllers on its own.

Kurono saves its own files into that `KURONO` partition. If there isn't one, nothing gets
persisted and it just runs from RAM.

## Safest way to try it

Use a **USB stick** and don't install anything.

1. Build or download the ISO (`try-in-a-vm.md`).
2. Write it to a USB stick. The ISO is made with `grub-mkrescue`, so it's a hybrid image
   and a raw write works:
   ```bash
   sudo dd if=build/kurono.iso of=/dev/sdX bs=4M status=progress conv=fsync
   ```
   Double check `/dev/sdX` with `lsblk` first. Getting that wrong wipes the wrong disk.
   On Windows I use `flash_usb.ps1` from the repo root, which does the same raw write.
3. Reboot, open the firmware boot menu (usually F12 or similar), and pick the USB stick
   in **UEFI** mode.
4. In the GRUB menu pick **Kurono OS (EFI Direct)**. That's the path that turns the disk
   guard on and is what I run on the laptop.

If the screen is black or it hangs, try **Kurono OS (Emergency EFI)**. It boots in text
mode with a minimal setup.

## Things that matter on hardware

- **UEFI only.** Turn off Secure Boot (the loader isn't signed).
- **Keyboard and mouse:** USB works (xHCI). Hubs are new and **untested on real
  hardware**, so if you only have a keyboard behind a hub, plug it directly into the
  machine for now. PS/2 laptop keyboards and touchpads work too.
- **Graphics:** the framebuffer from the firmware is used. Intel display works,
  including more than one monitor. Hybrid laptops (Intel + NVIDIA) show output through
  the Intel GPU first. There's a long write-up on that in `../HYBRID_GPU_OPTIMUS_GUIDE.md`.
- **Network:** Intel/igc/e1000 wired cards and several Wi-Fi chips are supported to
  different degrees. Wi-Fi is the least reliable part. Wired is the safe bet.
- **Cores:** on EFI boots Kurono caps the online CPUs at 8 by default (`kurono.maxcpus` changes that). For the new all-cores scheduler add
  `kurono.sched=2`.

## GPU acceleration test build

Intel GPU acceleration is off by default because it is still being tested on real
hardware. There's a separate EFI loader for it that is never the default boot entry:

```bash
cd src && make gpu-efi        # -> build/boot/kurono_efi_gpu.efi
```

Copy that file next to the normal loader on your ESP as `EFI/KURONO/GPUTEST.EFI` and pick
it from the boot menu. It adds `kurono.wpdamage=1 kurono.igpu.accel=1
kurono.igpu.zerocopy=1`. If your screen goes wrong, reboot and pick the normal entry.
Details in `../developers/boot-flags.md`.

## If something goes wrong

- Black screen: try the Emergency EFI entry.
- Hang during boot: a serial port log is the best data. If you don't have one, a photo of
  the last text on screen still helps. Open an issue with your machine model.
- Keyboard dead at the login or the approval prompt: use `kurono.autologin=1`, and try a
  different USB port.
