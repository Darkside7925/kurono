# File Map

This is the working inventory for the live project tree.

It is meant to answer one practical question quickly: *where does a given responsibility live right now?*

## 1. Top level repository map

| Path | Role |
| --- | --- |
| `README.md` | project overview |
| `STATUS.md` | development snapshot |
| `start.ps1` | build and launch helper |
| `create_working_boot.ps1` | boot media helper |
| `build_iso.ps1` | ISO helper script |
| `flash_usb.ps1` | USB deployment helper |
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
| `kurono_linker.ld` | main linker script |
| `multiboot_header.S` | Multiboot header |

## 3. `src/kernel`

| File | Role |
| --- | --- |
| `heap.cpp` | heap implementation |
| `heap.h` | heap declarations |
| `io.h` | low level I/O helpers |
| `kurono_kernel.cpp` | boot coordinator and main loop |
| `kurono_kernel.cpp.bak` | historical backup |
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
| `vmm.cpp` | virtual memory or mapping support at kernel layer |
| `vmm.h` | VMM declarations |

## 4. `src/hal`

| File | Role |
| --- | --- |
| `hal.cpp` | IDT, PIC, reboot, interrupt dispatch |
| `hal.h` | HAL interface |

## 5. `src/drivers`

| File | Role |
| --- | --- |
| `ac97.cpp` | AC97 audio controller |
| `ac97.h` | AC97 interface |
| `amd_gpu.cpp` | AMD GPU support |
| `amd_gpu.h` | AMD GPU interface |
| `audio.cpp` | audio services and tone support |
| `audio.h` | audio interface |
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
| `desktop.cpp` | desktop shell and taskbar |
| `desktop.h` | desktop interface |
| `file_browser.cpp` | file browser UI helpers |
| `file_browser.h` | file browser interface |
| `font.cpp` | font rendering |
| `font.h` | font interface |
| `gui.cpp` | GUI drawing helpers |
| `gui.h` | GUI interface |
| `lockscreen.cpp` | lock screen flow |
| `lockscreen.h` | lock screen interface |
| `text_layout.cpp` | text layout logic |
| `text_layout.h` | text layout interface |
| `ui_elements.cpp` | reusable UI components |
| `ui_elements.h` | UI component declarations |
| `vga_font.h` | VGA font data |
| `wallpaper.h` | wallpaper declarations |
| `wayland_server.cpp` | in-kernel Wayland compositor |
| `wayland_server.h` | Wayland compositor interface |
| `window_manager.cpp` | window manager implementation |
| `window_manager.h` | window manager interface |

## 7. `src/apps`

| File | Role |
| --- | --- |
| `browser.cpp` | browser app shell |
| `browser.h` | browser interface |
| `calculator.cpp` | calculator app |
| `calculator.h` | calculator interface |
| `conduit.cpp` | conduit app |
| `conduit.h` | conduit interface |
| `file_manager.cpp` | file manager app |
| `file_manager.h` | file manager interface |
| `media_player.cpp` | media player app |
| `media_player.h` | media player interface |
| `settings.cpp` | settings app |
| `settings.h` | settings interface |
| `task_manager.cpp` | task manager app |
| `task_manager.h` | task manager interface |
| `terminal.cpp` | terminal app |
| `terminal.h` | terminal interface |
| `text_editor.cpp` | text editor app |
| `text_editor.h` | text editor interface |
| `README.md` | app notes |
| `syncthing-windows-setup.exe` | bundled external installer artifact |

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
| `kvfs.cpp` | in memory virtual filesystem |
| `kvfs.h` | KVFS declarations |
| `vfs.cpp` | generic VFS layer |
| `vfs.h` | VFS declarations |

## 10. `src/net`

| File | Role |
| --- | --- |
| `network.cpp` | network interface and socket management |
| `network.h` | network declarations |
| `tcpip.cpp` | protocol stack implementation |
| `tcpip.h` | protocol declarations |

## 11. `src/proc`

| File | Role |
| --- | --- |
| `scheduler.cpp` | scheduler implementation |
| `scheduler.h` | scheduler declarations |

## 12. `src/security`

| File | Role |
| --- | --- |
| `supr.cpp` | privilege system |
| `supr.h` | privilege declarations |

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

## 15. `src/system`

| File | Role |
| --- | --- |
| `conduit.cpp` | conduit bridge services |
| `conduit.h` | conduit declarations |
| `input_manager.cpp` | input manager |
| `input_manager.h` | input declarations |
| `installer.cpp` | installer logic |
| `installer.h` | installer declarations |
| `logging.cpp` | runtime logging |
| `logging.h` | runtime logging declarations |
| `user_mgmt.cpp` | user management |
| `user_mgmt.h` | user management declarations |

## 16. `src/linux`

| File | Role |
| --- | --- |
| `dual_boot.cpp` | integrated Linux boot coordination |
| `dual_boot.h` | dual boot declarations |
| `ext4.cpp` | ext4 support |
| `ext4.h` | ext4 declarations |
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
| `shared_mount.cpp` | shared mount logic |
| `shared_mount.h` | shared mount declarations |
| `user_bridge.cpp` | user bridge logic |
| `user_bridge.h` | user bridge declarations |

## 17. `src/virt`

| File | Role |
| --- | --- |
| `alpine_data.h` | embedded Alpine payload |
| `debian_data.h` | embedded Debian payload |
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
| `v9fs.cpp` | shared file protocol layer |
| `v9fs.h` | v9fs declarations |
| `vdevices.cpp` | virtual devices |
| `vdevices.h` | virtual device declarations |
| `vdisk.cpp` | virtual disk |
| `vdisk.h` | virtual disk declarations |
| `vmexit.cpp` | VM exit policy |
| `vmexit.h` | VM exit declarations |
| `vmm.cpp` | virtualization backend glue |
| `vmm.h` | backend declarations |
| `vserial.cpp` | virtual serial |
| `vserial.h` | virtual serial declarations |

## 18. `src/media`

| File | Role |
| --- | --- |
| `codec.cpp` | codec registry and helpers |
| `codec.h` | codec declarations |
| `embedded_media.h` | embedded media declarations |
| `mediadecoder.cpp` | image and media decoding |
| `mediadecoder.h` | decoder declarations |
| `mp3_decoder.cpp` | MP3 decoder |
| `mp3_decoder.h` | MP3 declarations |

## 19. `src/tests`

| File | Role |
| --- | --- |
| `test_suite.cpp` | boot time tests |
| `test_suite.h` | test declarations |

## 20. `src/third_party`

This folder carries the glue layer around embedded third party components used by the media and font stack.

## 21. Fast lookup guide

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
| Network issue | `src/net/tcpip.cpp` |
