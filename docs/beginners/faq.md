# FAQ

**Is this Linux?**
No. The kernel is mine. It has a Linux ABI layer so it can run Linux programs, the same
way Wine runs Windows programs on Linux, except at the kernel level.

**Why does it run Firefox?**
Because I wanted a real test. If a modern browser runs, the syscall layer, threading,
memory management, sockets and the Wayland server are all doing something right. It
works, loads real sites, and is still slow and flaky on heavy ones. Software rendering
and a young scheduler are the reasons.

**Does it run on real hardware?**
Yes, on my Lenovo Legion Pro 7 (Arrow Lake-HX) through EFI. Other machines are untested.
See `boot-on-hardware.md`.

**Will it hurt my other OS or my files?**
The EFI boot path makes every disk read-only except a partition named `KURONO`. That is
the safety net. Still, don't test with data you can't lose.

**Can I install it?**
There's a setup entry in the boot menu, but a proper installer is a roadmap item, not
something I'd call finished. For now use the ISO, a VM, or a USB stick.

**Does it run Windows apps?**
No. Maybe someday (`../roadmap.md`).

**Does it run on ARM, or 32-bit?**
No. x86_64 only. Ports are on the long-term list.

**How much RAM does it need?**
It boots to the desktop on 512 MB. Firefox and Alpine want a few GB. A long-term goal is
a lean base system under 50 MB.

**What is Alpine doing in there?**
Kurono has its own hypervisor (Intel VT-x). It boots an Alpine Linux guest and can launch
Alpine apps whose windows show up on the Kurono desktop. Debian is still being tested.

**What is `ken`? What is KSA?**
`ken` is the sudo of Kurono: `ken <command>`. When something needs admin rights, KSA
shows an approval prompt. The prompt is drawn from an isolated hypervisor context, so
even a compromised kernel can't click "Allow" for you. `sudo` and `supr` work as aliases.

**What is KLD?**
Kurono Linux Drivers. It runs unmodified Linux driver code (I've done e1000e) as a
sandboxed ring-3 process. The kernel doesn't link any Linux code, the driver talks to
hardware only through a broker in the kernel.

**What are the `kurono.*` flags?**
Boot options you add on the GRUB line. See `../developers/boot-flags.md`.

**Why is the GPU acceleration off?**
Because on my laptop an earlier version made the desktop worse and I don't ship things I
can't verify on real hardware. It's behind flags and off by default until it's solid.

**Can I contribute?**
Yes. Issues with a serial log are the most useful thing. If you want to read code, start
with `FIRST_STEPS.md`.

**Why the name?**
Kurono is just the name. The release is called Kintsugi after the pottery repair art:
fixing the cracks instead of hiding them.
