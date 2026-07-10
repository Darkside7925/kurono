# File Map

This is the working inventory for the live project tree.

It is meant to answer one practical question quickly: *where does a given responsibility live right now?*

## 1. Top level repository map

| Path | Role |
| --- | --- |
| `README.md` | project overview |
| `STATUS.md` | development snapshot |
| `start.sh` | Linux/KVM build-and-launch helper (the path used day to day) |
| `start.ps1` | Windows/WSL (WHPX) build and launch helper |
| `create_working_boot.ps1` | boot media helper |
| `build_iso.ps1` | ISO helper script |
| `build_debian_rootfs.ps1` | Debian guest rootfs build helper |
| `fix_conflicts.py` | maintenance utility |
| `logo.h` | embedded logo pixels |
| `docs/` | project documentation |
| `src/` | live source tree |
| `tools/` | utility scripts |

## 2. `src/boot`

| File | Role |
| --- | --- |
| `efi_loader.c` | standalone EFI loader |
| `kurono_boot.asm` | early assembly boot path |
| `ap_trampoline.asm` | application-processor (SMP) real-mode → long-mode trampoline |
| `kurono_linker.ld` | main linker script |
| `multiboot_header.S` | Multiboot header |

## 3. `src/kernel`

| File | Role |
| --- | --- |
| `buddy.cpp` / `buddy.h` | buddy page allocator |
| `slab.cpp` / `slab.h` | slab allocator |
| `elf_loader.cpp` / `elf_loader.h` | ELF loader + `PT_INTERP` handoff to ld-kurono |
| `heap.cpp` | heap implementation |
| `heap.h` | heap declarations |
| `hrtimer.cpp` / `hrtimer.h` | high-resolution timer support |
| `io.h` | low level I/O helpers |
| `kurono_kernel.cpp` | boot coordinator and main loop |
| `kurono_kernel_simplified.cpp` | simplified kernel variant |
| `memory_mgr.h` | memory manager declarations |
| `multiboot.h` | Multiboot structures |
| `panic.cpp` | crash screen and bugcheck path |
| `panic.h` | panic interface |
| `pci.h` | PCI helpers |
| `pmm.cpp` | physical memory manager |
| `pmm.h` | PMM declarations |
| `system.cpp` | system helpers |
| `system.h` | system declarations |
| `time.cpp` | timekeeping |
| `time.h` | time declarations |
| `types.cpp` | type related support |
| `types.h` | core types |
| `userspace.cpp` / `userspace.h` | ring-3 entry / user-process launch helpers |
| `userspace_entry.asm` | ring-3 entry / IRETQ stubs |
| `vmm.cpp` | virtual memory or mapping support at kernel layer |
| `vmm.h` | VMM declarations |

## 4. `src/hal`

| File | Role |
| --- | --- |
| `hal.cpp` | IDT, PIC, reboot, interrupt dispatch |
| `hal.h` | HAL interface |
| `cpufreq.cpp` / `cpufreq.h` | CPUFreq governors + P-state control |
| `isr_stubs.asm` | ISR entry stubs |
| `syscall_entry.asm` | SYSCALL/`int 0x80` entry (per-CPU swapgs path) |

## 5. `src/drivers`

| File | Role |
| --- | --- |
| `ac97.cpp` | AC97 audio controller |
| `ac97.h` | AC97 interface |
| `amd_gpu.cpp` | AMD GPU support |
| `amd_gpu.h` | AMD GPU interface |
| `audio.cpp` | audio services and tone support |
| `audio.h` | audio interface |
| `audio_backend.cpp` / `audio_backend.h` | audio backend dispatch layer |
| `audio_backend_ac97.cpp` / `audio_backend_hda.cpp` / `audio_backend_sb16.cpp` / `audio_backend_pcspk.cpp` | per-device audio backends |
| `audio_dma.cpp` / `audio_dma.h` | audio DMA buffer management |
| `audio_format.cpp` / `audio_format.h` | PCM format conversion |
| `audio_mixer.cpp` / `audio_mixer.h` | per-stream software mixer |
| `audio_server.cpp` / `audio_server.h` | audio server / stream routing |
| `audio_wav.cpp` / `audio_wav.h` | WAV parse/playback helpers |
| `pulse_server.cpp` / `pulse_server.h` | PulseAudio-compatible daemon |
| `tpm.cpp` / `tpm.h` | TPM 2.0 (CRB + FIFO) |
| `bga.cpp` | Bochs graphics adapter |
| `bga.h` | BGA interface |
| `cpu_detect.cpp` | CPUID and feature discovery |
| `cpu_detect.h` | CPU detect interface |
| `display.cpp` | display setup helpers |
| `display.h` | display interface |
| `display_mgr.cpp` | display manager and mode switching |
| `display_mgr.h` | display manager interface |
| `e1000.cpp` | Intel E1000 NIC |
| `e1000.h` | E1000 interface |
| `gpu_probe.cpp` | GPU inventory and probing |
| `gpu_probe.h` | GPU probe interface |
| `graphics.cpp` | framebuffer primitives and copy paths |
| `graphics.h` | graphics interface |
| `hda.cpp` | Intel HD Audio |
| `hda.h` | HDA interface |
| `intel_gpu.cpp` | Intel GPU support |
| `intel_gpu.h` | Intel GPU interface |
| `keyboard.cpp` | PS/2 keyboard driver |
| `keyboard.h` | keyboard interface |
| `mouse.cpp` | PS/2 mouse and touchpad style driver |
| `mouse.h` | mouse interface |
| `nvidia_gpu.cpp` | NVIDIA GPU support |
| `nvidia_gpu.h` | NVIDIA GPU interface |
| `nvme.cpp` | NVMe storage driver |
| `nvme.h` | NVMe interface |
| `rtc.cpp` | real time clock |
| `rtc.h` | RTC interface |
| `serial.cpp` | COM1 serial logger |
| `serial.h` | serial interface |
| `timer.cpp` | PIT timing |
| `timer.h` | timer interface |
| `usb.cpp` | USB and xHCI logic |
| `usb.h` | USB interface |
| `virtio_gpu.cpp` | VirtIO GPU path |
| `virtio_gpu.h` | VirtIO GPU interface |

## 6. `src/ui`

| File | Role |
| --- | --- |
| `app_icons.cpp` / `app_icons.h` | drawn app/desktop icons |
| `control_center.cpp` / `control_center.h` | control-center panel |
| `desktop.cpp` | desktop shell and taskbar |
| `desktop.h` | desktop interface |
| `file_browser.cpp` | file browser UI helpers |
| `file_browser.h` | file browser interface |
| `font.cpp` | font rendering |
| `font.h` | font interface |
| `gui.cpp` | GUI drawing helpers |
| `gui.h` | GUI interface |
| `kss.cpp` / `kss.h` | KSS theme tokens + scriptable stylesheet/animation layer |
| `lockscreen.cpp` | lock screen flow |
| `lockscreen.h` | lock screen interface |
| `notification.cpp` / `notification.h` | toast/notification system |
| `perf_hud.cpp` / `perf_hud.h` | FPS / performance overlay |
| `text_layout.cpp` | text layout logic |
| `text_layout.h` | text layout interface |
| `ui_elements.cpp` | reusable UI components |
| `ui_elements.h` | UI component declarations |
| `vga_font.h` | VGA font data |
| `wallpaper.h` | wallpaper #1 declarations (pre-decoded raw RGBA) |
| `wallpaper2.h` | wallpaper #2 declarations (pre-decoded raw RGBA) |
| `wayland_server.cpp` | in-kernel Wayland compositor |
| `wayland_server.h` | Wayland compositor interface |
| `window_manager.cpp` | window manager implementation |
| `window_manager.h` | window manager interface |

## 7. `src/apps`

> Note: the GUI `conduit` app/interface lives in `src/system/` (`conduit.cpp`/`conduit.h`), not `src/apps/`. Earlier revisions of this map listed it here.

| File | Role |
| --- | --- |
| `browser.cpp` | browser app shell (deliberate placeholder tile) |
| `browser.h` | browser interface |
| `calculator.cpp` | calculator app |
| `calculator.h` | calculator interface |
| `denji_app.cpp` / `denji_app.h` | Denji windowed video-player app |
| `file_manager.cpp` | file manager app |
| `file_manager.h` | file manager interface |
| `firefox_launcher.cpp` / `firefox_launcher.h` | Firefox runtime launcher (`execve` path) |
| `kj.cpp` / `kj.h` | KJ (Kurono JavaScript) interpreter + `kj`/`node` shell commands |
| `kj_test.cpp` / `kj_test.h` | KJ self-test suite (`kurono.kjtest`, 11/11) |
| `media_player.cpp` | media player app |
| `media_player.h` | media player interface |
| `python_interp.cpp` / `python_interp.h` | Mini Python 3 interpreter |
| `settings.cpp` | settings app |
| `settings.h` | settings interface |
| `settings_mod_*.cpp` | per-tab settings modules (a11y, about, audio, devices, display, network, personalize, power, security, storage, system) |
| `system_settings.cpp` / `system_settings.h` | system-settings backing store |
| `task_manager.cpp` | task manager app |
| `task_manager.h` | task manager interface |
| `terminal.cpp` | terminal app |
| `terminal.h` | terminal interface |
| `text_editor.cpp` | text editor app |
| `text_editor.h` | text editor interface |
| `README.md` | app notes |

## 8. `src/shell`

| File | Role |
| --- | --- |
| `linux_cmds.cpp` | Linux style commands |
| `linux_cmds.h` | Linux command declarations |
| `shell.cpp` | shell core and command routing |
| `shell.h` | shell declarations |
| `windows_cmds.cpp` | Windows style commands |
| `windows_cmds.h` | Windows command declarations |

## 9. `src/fs`

| File | Role |
| --- | --- |
| `fat32.cpp` | FAT32 support |
| `fat32.h` | FAT32 declarations |
| `kvfs.cpp` | in memory virtual filesystem (runtime fs) |
| `kvfs.h` | KVFS declarations |
| `kfs.cpp` | Kurono File System - on-disk persistence filesystem |
| `kfs.h` | KFS on-disk format spec + declarations |
| `kfs_bench.cpp` / `kfs_bench.h` | KFS benchmark (`kurono.kfsbench`) |
| `persist.cpp` | persistence layer (mirrors KVFS user data into KFS) |
| `persist.h` | persistence declarations |
| `vfs.cpp` | generic VFS layer |
| `vfs.h` | VFS declarations |

## 10. `src/net`

| File | Role |
| --- | --- |
| `network.cpp` | network interface and socket management |
| `network.h` | network declarations |
| `tcpip.cpp` | protocol stack implementation (IPv4/ICMP/UDP/TCP) |
| `tcpip.h` | protocol declarations |
| `ipv6.cpp` | IPv6 stack |
| `ipv6.h` | IPv6 declarations |
| `netfilter.cpp` | 5-hook packet-filter pipeline |
| `netfilter.h` | netfilter declarations |
| `tuntap.cpp` | Tun/Tap subsystem |
| `tuntap.h` | Tun/Tap declarations |
| `unix_socket.cpp` | AF_UNIX sockets + `SCM_RIGHTS` fd-passing |
| `unix_socket.h` | AF_UNIX declarations |

## 11. `src/proc`

| File | Role |
| --- | --- |
| `scheduler.cpp` | scheduler implementation (cross-core lock, per-CPU pick) |
| `scheduler.h` | scheduler declarations |
| `smp.cpp` | multi-core: LAPIC, MADT parse, per-CPU blocks, AP bring-up |
| `smp.h` | SMP declarations |
| `kernel_processes.cpp` | built-in kernel processes (GUI, WM, daemons) |
| `kernel_processes.h` | kernel-process declarations |
| `cgroup.cpp` | cgroups v2 hierarchy |
| `cgroup.h` | cgroup declarations |
| `spinlock.h` / `kernel_locks.h` | `lock cmpxchg` spinlocks |
| `switch_to.asm` | context-switch assembly |

## 12. `src/security`

| File | Role |
| --- | --- |
| `supr.cpp` | privilege system + auth policy + escalation gate |
| `supr.h` | privilege declarations |
| `ksa.cpp` | Kurono Secure Authorization (hypervisor-backed prompts) |
| `ksa.h` | KSA declarations |

## 13. `src/packages`

| File | Role |
| --- | --- |
| `pkgmgr.cpp` | package manager |
| `pkgmgr.h` | package manager declarations |

## 14. `src/kcl`

| File | Role |
| --- | --- |
| `kcl.cpp` | KCL interpreter |
| `kcl.h` | KCL declarations |
| `kcl_test.cpp` / `kcl_test.h` | KCL self-test suite (`kurono.kcltest`, 11/11) |

## 15. `src/system`

| File | Role |
| --- | --- |
| `clipboard.cpp` / `clipboard.h` | system clipboard |
| `conduit.cpp` | conduit bridge services (also the GUI Conduit app) |
| `conduit.h` | conduit declarations |
| `dbus_server.cpp` / `dbus_server.h` | D-Bus session bus daemon |
| `gpu_driver_installer.cpp` / `gpu_driver_installer.h` | guest GPU driver installer |
| `input_manager.cpp` | input manager |
| `input_manager.h` | input declarations |
| `installer.cpp` | installer logic |
| `installer.h` | installer declarations |
| `installer_gui.cpp` / `installer_gui.h` | graphical installer / first-setup wizard (`kurono.setup=1`) |
| `kpaths.h` | canonical on-disk path layout (single source of truth) |
| `logging.cpp` | runtime logging (boot/system/serial/network/security/crash) |
| `logging.h` | runtime logging declarations |
| `runtime_layout.cpp` / `runtime_layout.h` | Linux runtime layout seeder (`/system` tree, firefox.env) |
| `screenshot.cpp` / `screenshot.h` | screen-capture support |
| `system_update.cpp` / `system_update.h` | boot-time updater / staged-rootfs flow |
| `ui_config.cpp` / `ui_config.h` | UI config (`/etc/kurono/ui.conf` theme tokens) |
| `user_mgmt.cpp` | user management |
| `user_mgmt.h` | user management declarations |
| `vconsole.cpp` / `vconsole.h` | virtual consoles |

## 16. `src/linux`

| File | Role |
| --- | --- |
| `dual_boot.cpp` | integrated Linux boot coordination |
| `dual_boot.h` | dual boot declarations |
| `ext4.cpp` | ext4 support |
| `ext4.h` | ext4 declarations |
| `ld_kurono.cpp` | in-kernel ELF64 dynamic linker (PIE/`PT_INTERP`, `DT_NEEDED`, relocations, TLS, dlopen) |
| `ld_kurono.h` | ld-kurono declarations + capability list |
| `kls.cpp` | Linux style shell helpers |
| `kls.h` | KLS declarations |
| `linux_devices.cpp` | Linux device surface |
| `linux_devices.h` | device declarations |
| `linux_drivers.cpp` | Linux driver integration |
| `linux_drivers.h` | Linux driver declarations |
| `linux_init.cpp` | init and service logic |
| `linux_init.h` | init declarations |
| `linux_kernel.cpp` | Linux personality core |
| `linux_kernel.h` | personality declarations |
| `linux_netbridge.cpp` | Linux network bridge |
| `linux_netbridge.h` | bridge declarations |
| `linux_signals.cpp` | signal handling |
| `linux_signals.h` | signal declarations |
| `linux_syscall.cpp` | syscall handling |
| `linux_syscall.h` | syscall declarations |
| `linux_syscall_x64.cpp` | x86_64 syscall-number dispatch table |
| `shared_mount.cpp` | shared mount logic |
| `shared_mount.h` | shared mount declarations |
| `user_bridge.cpp` | user bridge logic |
| `user_bridge.h` | user bridge declarations |

## 17. `src/virt`

| File | Role |
| --- | --- |
| `alpine_data.h` | embedded Alpine payload |
| `debian_data.cpp` / `debian_data.h` | embedded Debian payload |
| `ept.cpp` | EPT and NPT mapping |
| `ept.h` | EPT declarations |
| `guest_mem.cpp` | guest memory setup |
| `guest_mem.h` | guest memory declarations |
| `hypervisor.cpp` | hypervisor implementation |
| `hypervisor.h` | hypervisor declarations |
| `iommu.cpp` | IOMMU support |
| `iommu.h` | IOMMU declarations |
| `linux_boot.cpp` | guest Linux loader |
| `linux_boot.h` | guest boot declarations |
| `pci_passthrough.cpp` / `pci_passthrough.h` | PCI/GPU passthrough |
| `v9fs.cpp` | shared file protocol layer |
| `v9fs.h` | v9fs declarations |
| `vdevices.cpp` | virtual devices |
| `vdevices.h` | virtual device declarations |
| `vdisk.cpp` | virtual disk |
| `vdisk.h` | virtual disk declarations |
| `virtio_gpu_host.cpp` / `virtio_gpu_host.h` | host-side virtio-gpu device bridge |
| `vpci.cpp` / `vpci.h` | virtual PCI bus |
| `vmexit.cpp` | VM exit policy |
| `vmexit.h` | VM exit declarations |
| `vmm.cpp` | virtualization backend glue |
| `vmm.h` | backend declarations |
| `vserial.cpp` | virtual serial |
| `vserial.h` | virtual serial declarations |

## 18. `src/media`

| File | Role |
| --- | --- |
| `aac_parse.cpp` / `aac_parse.h` | AAC-LC parser |
| `codec.cpp` | codec registry and helpers |
| `codec.h` | codec declarations |
| `embedded_media.h` | embedded media declarations |
| `h264_parse.cpp` / `h264_parse.h` | H.264 parser |
| `kvid.cpp` / `kvid.h` | KVID container/player |
| `mediadecoder.cpp` | image and media decoding |
| `mediadecoder.h` | decoder declarations |
| `mp3_decoder.cpp` | MP3 decoder |
| `mp3_decoder.h` | MP3 declarations |
| `mp4_demux.cpp` / `mp4_demux.h` | MP4 demuxer |
| `video_player.cpp` / `video_player.h` | `VideoPlayer` decode/playback core |

## 19. `src/tests`

| File | Role |
| --- | --- |
| `test_suite.cpp` | boot time tests |
| `test_suite.h` | test declarations |

## 20. `src/third_party`

This folder carries the glue layer around embedded third party components used by the media and font stack (`stb_image`, `stb_truetype`, and their `*_glue.cpp` wrappers).

## 21. `src/userprogs`

Userspace test programs (cross-compiled with musl-gcc / NASM) embedded into the kernel image and launched by shell commands. Built ELF artifacts are tracked alongside their sources.

| File | Role |
| --- | --- |
| `wl_shm_test.c` / `wl_shm_test.elf` | musl Wayland client exercising the `wl_shm` + xdg-shell render path (`wltest`) |
| `pthread_test.c` / `pthread_test.elf` | pthreads (clone+futex) smoke test (`pthtest`) |
| `hello_musl.c` / `hello_musl.elf` | dynamic musl PIE used by `kurono.dyntest` |
| `dyntest.elf` | dynamic-linker bring-up artifact |
| `hello.asm` / `hello_x64.asm` | hand-written ring-3 syscall demos |
| `kpython.c` | embedded Python helper source |
| `libfoo.so` / `musl_libc.so` | shared objects for the ld-kurono 2-lib test |
| `ffmpeg.elf` | embedded ffmpeg artifact |
| `user.ld` | userspace linker script |
| `embedded_userprogs.h` | generated table of embedded user programs |

## 22. Fast lookup guide

If the question is about one of the subjects below, the first file to open should usually be this one.

| Subject | First file |
| --- | --- |
| Boot failure | `src/kernel/kurono_kernel.cpp` |
| Crash screen | `src/kernel/panic.cpp` |
| Reboot failure | `src/hal/hal.cpp` |
| Keyboard issue | `src/drivers/keyboard.cpp` |
| Mouse or touchpad issue | `src/drivers/mouse.cpp` |
| Window drag or focus issue | `src/ui/window_manager.cpp` |
| Desktop or taskbar issue | `src/ui/desktop.cpp` |
| Command routing bug | `src/shell/shell.cpp` |
| Filesystem issue | `src/fs/kvfs.cpp` and `src/fs/vfs.cpp` |
| Installer issue | `src/system/installer.cpp` |
| VM boot issue | `src/virt/hypervisor.cpp` and `src/virt/vmm.cpp` |
| Linux syscall issue | `src/linux/linux_syscall.cpp` |
| Dynamic-linker / PIE-load issue | `src/linux/ld_kurono.cpp` and `src/kernel/elf_loader.cpp` |
| Network issue | `src/net/tcpip.cpp` |
| Multi-core / AP issue | `src/proc/smp.cpp` and `src/proc/scheduler.cpp` |
| Privilege / auth-prompt issue | `src/security/supr.cpp` and `src/security/ksa.cpp` |
| Persistence / on-disk fs issue | `src/fs/persist.cpp` and `src/fs/kfs.cpp` |
| Canonical path / symlink issue | `src/system/kpaths.h` and `src/fs/kvfs.cpp` |
