# Display setup

`src/drivers/display.cpp` and `display.h` pick a display mode and get a framebuffer.
`src/drivers/display_mgr.cpp` is the multi-backend manager on top of it.

The display driver doesn't draw anything. Its job is to work out what hardware is there
and hand a usable pixel buffer to the rest of the system.

## Framebuffer sources, in order

1. Multiboot framebuffer: GRUB hands it over from EFI GOP on real hardware. On hybrid
   laptops the GPU probe validates the address.
2. VBE linear framebuffer: from BIOS-era bootloaders, covers most legacy hardware.
3. BGA, which is QEMU and VirtualBox.

Once one is accepted, the display manager picks a backend from the GPU probe results and
passes the address, dimensions and pitch to the graphics driver.

## Backend order

1. VirtIO GPU, if detected. 2D resource management plus scanout, the best path in a VM.
2. BGA, for QEMU/Bochs.
3. Native: Intel, NVIDIA or AMD, whichever the GPU probe called primary.

## Panic

After a mode is set, the display driver tells panic the framebuffer parameters, so a
later crash draws in the right place.

## Modes

10 predefined modes, 640x480 up to 3840x2160, switchable at runtime through
`SetResolution()`. A settings change goes through here, which re-initialises and tells
the rest of the system.

## When it breaks

| Symptom | Cause |
| --- | --- |
| screen stays black | no framebuffer source, check bootloader config and the GPU probe |
| boots at the wrong resolution | VBE picked the wrong mode index, or EDID wasn't read |
| works in a VM but not on metal | BGA only, the GPU probe and VBE never got set up |
| black screen on an Optimus laptop | the GRUB framebuffer points at the wrong GPU, probe validation failed |
| a resolution change crashes | panic's framebuffer wasn't updated after the switch |

## Files

- `src/drivers/display_mgr.cpp` multi-backend display manager
- `src/drivers/gpu_probe.cpp` GPU detection and hybrid topology
- `src/drivers/graphics.cpp` where the framebuffer parameters end up
- `src/drivers/bga.cpp` the VM path
- `src/drivers/virtio_gpu.cpp` the VirtIO path
- `src/kernel/panic.cpp` the framebuffer update target
