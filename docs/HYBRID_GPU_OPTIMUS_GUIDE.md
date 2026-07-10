# Hybrid GPU & NVIDIA Optimus - Bare-Metal OS Developer Guide

> **Target hardware**: MSI Thin 15 B13VE - Intel i5-13420H (Raptor Lake, UHD Graphics) + NVIDIA RTX 4050 Laptop (Ada Lovelace AD107). Muxless Optimus design.

> **Target OS**: Kurono OS - boots via GRUB Multiboot1, receives framebuffer from bootloader, runs in 64-bit long mode.

---

## Table of Contents

1. [NVIDIA Optimus Architecture](#1-nvidia-optimus-architecture)
2. [UEFI GOP and Optimus](#2-uefi-gop-and-optimus)
3. [Common Failure Modes (Black Screen)](#3-common-failure-modes-black-screen)
4. [PCI Device Detection](#4-pci-device-detection)
5. [Intel iGPU Framebuffer (BAR Layout & Registers)](#5-intel-igpu-framebuffer-bar-layout--registers)
6. [NVIDIA GPU Framebuffer (BAR Layout)](#6-nvidia-gpu-framebuffer-bar-layout)
7. [Muxless vs Muxed vs Advanced Optimus](#7-muxless-vs-muxed-vs-advanced-optimus)
8. [What Linux Does](#8-what-linux-does)
9. [Solutions for Bare-Metal OS Developers](#9-solutions-for-bare-metal-os-developers)
10. [MSI Thin 15 B13VE Specifics](#10-msi-thin-15-b13ve-specifics)
11. [AMD Hybrid Graphics (PowerXpress / Enduro)](#11-amd-hybrid-graphics-powerxpress--enduro)
12. [Apple Silicon / ARM Considerations](#12-apple-silicon--arm-considerations)
13. [Code Patterns for Kurono OS](#13-code-patterns-for-kurono-os)

---

## 1. NVIDIA Optimus Architecture

### 1.1 What Is Optimus?

NVIDIA Optimus is a **hybrid GPU power management technology** that pairs an Intel (or AMD) integrated GPU (iGPU) with an NVIDIA discrete GPU (dGPU). The goal is battery life: the dGPU powers down when not needed, and the iGPU handles display output.

### 1.2 Muxless Optimus - How It Works

On virtually all modern Optimus laptops (2013+), the design is **muxless**:

```
┌──────────────────────────────────────────────────────┐
│                    LAPTOP                             │
│                                                       │
│  ┌──────────┐    PCIe copy     ┌──────────────────┐  │
│  │ NVIDIA   │ ──────────────── │ Intel iGPU       │  │
│  │ dGPU     │  (render result) │                  │  │
│  │          │                  │  Display Engine   │──│──► LCD Panel
│  │ VRAM     │                  │  (pipe/plane)     │  │
│  │ (GDDR6)  │                  │  Framebuffer      │  │
│  └──────────┘                  │  (stolen memory)  │  │
│                                └──────────────────┘  │
│                                                       │
│  The LCD panel is PHYSICALLY WIRED to the iGPU only.  │
│  There is NO electrical path from dGPU to the panel.  │
└──────────────────────────────────────────────────────┘
```

**Key architectural facts:**

1. **The iGPU ALWAYS owns the display panel**. The LCD's eDP/LVDS connector is wired to the Intel GPU's display engine. There is no MUX chip.

2. **The dGPU has no display outputs connected to the internal panel**. On some laptops, external ports (HDMI, USB-C/DP) may be wired directly to the dGPU, but the built-in screen is always iGPU.

3. **The dGPU renders to its own VRAM** (GDDR6/GDDR6X). When an application uses the dGPU, it renders frames into dGPU VRAM.

4. **The rendered frame is copied to the iGPU's framebuffer** over the PCIe bus. On Linux, this is done via DMA (PRIME). On Windows, the NVIDIA driver handles this transparently.

5. **The iGPU's display controller scans out the framebuffer to the panel**. The iGPU's display pipe reads from its framebuffer (in system RAM / stolen memory) and sends pixel data to the LCD.

### 1.3 What This Means for a Bare-Metal OS

When GRUB or UEFI gives you a framebuffer address via Multiboot:

- **That address points to the iGPU's framebuffer**, NOT the dGPU's VRAM.
- The iGPU's display engine is already configured (by firmware) to scan out from that address.
- **Writing pixels to that address makes them appear on screen** - because the iGPU hardware is continuously reading from it.
- The NVIDIA dGPU is typically powered down or in a low-power state. It is **irrelevant** for basic display output.
- **You do NOT need the NVIDIA GPU to display anything on the laptop screen.** The Intel iGPU alone is sufficient.

---

## 2. UEFI GOP and Optimus

### 2.1 Which GPU Does UEFI GOP Initialize?

On muxless Optimus laptops:

- **UEFI GOP initializes the Intel iGPU**. Period.
- The UEFI firmware contains an Intel GOP driver (embedded in the UEFI ROM or loaded from the Intel VBIOS option ROM).
- The NVIDIA dGPU either:
  - Has its own VBIOS/GOP driver loaded (for external displays), or
  - Is left uninitialized / in a low-power state.
- **The GOP framebuffer address reported by UEFI is always the iGPU's framebuffer.**

### 2.2 What Framebuffer Address Does GOP Report?

The EFI Graphics Output Protocol reports:

```
EFI_GRAPHICS_OUTPUT_PROTOCOL.Mode->FrameBufferBase = <physical address>
EFI_GRAPHICS_OUTPUT_PROTOCOL.Mode->FrameBufferSize = <size in bytes>
```

This address is the **iGPU's stolen memory aperture** - a region of system RAM that Intel reserves for GPU use. On modern Intel GPUs:

- Typically in the range `0x80000000` - `0xFFFFFFFF` (below 4 GB) or sometimes above 4 GB.
- The exact address is configured by the firmware in the iGPU's PCI BARs and the Graphics Stolen Memory Base Register (BDSM/BGSM).
- This is **identity-mapped** by the firmware - `physical address == address you write to`.

### 2.3 GRUB Multiboot1 and the Framebuffer

When GRUB boots a Multiboot1 kernel with `set gfxpayload=1920x1080x32`:

1. GRUB calls UEFI GOP (or VBE on legacy BIOS) to set the mode.
2. GRUB stores the framebuffer info in the Multiboot Info Structure (offset 88 - 100):
   - `framebuffer_addr` (uint64_t at offset 88) - the physical base address
   - `framebuffer_pitch` (uint32_t at offset 96) - bytes per scanline
   - `framebuffer_width` (uint32_t at offset 100)
   - `framebuffer_height` (uint32_t at offset 104)
   - `framebuffer_bpp` (uint8_t at offset 108)
   - `framebuffer_type` (uint8_t at offset 109) - 1 = RGB, 2 = text
3. **This framebuffer address comes from the iGPU's GOP driver** on Optimus laptops.
4. Your kernel reads this and writes pixels. It works because the iGPU display engine is scanning out from this address.

---

## 3. Common Failure Modes (Black Screen)

### 3.1 "I Get a Black Screen on My Optimus Laptop"

Common causes, from most to least likely:

#### A. Framebuffer Caching (MOST COMMON - Your OS Already Handles This!)

**Problem**: The framebuffer physical address is in MMIO space (uncacheable by default). With identity mapping and Write-Back (WB) caching, CPU writes go to the CPU cache but **never reach the GPU's memory controller**.

**Symptom**: Works in QEMU/Bochs, black screen on real hardware.

**Solution**: Mark the framebuffer pages as **Write-Combining (WC)** via the PAT (Page Attribute Table). Your `remap_fb_writecombining()` in `graphics.cpp` already does this correctly:
- PAT entry 1 = WC (0x01)
- Set PWT=1, PCD=0, PAT=0 on the 2MB page table entries
- Use `movntdq` (non-temporal stores) for buffer copies

```
✅ Kurono OS already handles this correctly via:
   - remap_fb_writecombining() in graphics.cpp
   - fb_copy_nt() using movntdq + sfence
   - PAT programming in kurono_boot.asm
```

#### B. Wrong Framebuffer Address

**Problem**: The OS reads a stale or incorrect framebuffer address.

Scenarios:
- Multiboot info struct is in memory that gets overwritten during kernel init
- The address was read incorrectly (endianness, 32-bit truncation of 64-bit address)
- A page table misconfiguration makes the virtual address map to the wrong physical address

**Solution**: Read `framebuffer_addr` as a full uint64_t early, before any memory initialization. Verify by reading back from the framebuffer (pixels should match what you wrote).

#### C. Firmware Framebuffer Becomes "Stale"

**Problem**: Some firmware implementations set up the framebuffer using a temporary GOP mode that becomes invalid if you reinitialize certain hardware.

Triggers:
- Reprogramming the PCI command register of the iGPU (disabling memory space)
- Performing a PCI bus reset that affects the iGPU
- Writing to the iGPU's display registers (DSPSURF, DSPCNTR, etc.) without understanding the full programming sequence
- Disabling the iGPU's display pipe

**Solution**: **Don't touch the iGPU's PCI configuration or display registers.** Just use the framebuffer address as-is.

#### D. Writing to the Wrong GPU's VRAM

**Problem**: If you accidentally write to the NVIDIA dGPU's BAR1 (VRAM aperture) instead of the iGPU's framebuffer, those pixels go to the dGPU's VRAM - which is **not connected to the display**.

**Symptom**: Your writes succeed (no fault), but nothing appears on screen.

**Detection**:
```
iGPU framebuffer: typically 0x80000000 - 0xE0000000 range (system memory / stolen memory)
dGPU BAR1 (VRAM): typically above 0x100000000 (>4GB) or a separate PCI MMIO range
```

**Solution**: Only write to the address provided by Multiboot/GOP. Don't try to use the NVIDIA GPU for display on the laptop panel.

#### E. iGPU Display Pipe Disabled

**Problem**: Something disabled the iGPU's display pipe or plane after boot.

This can happen if:
- You write 0 to DSPCNTR (Display Plane Control) - disables the plane
- You write 0 to PIPECONF - disables the pipe
- You issue a GPU reset via GDRST (Graphics Device Reset)

**Solution**: Don't write to Intel GPU registers unless you understand the full modeset sequence. If you need to check the current state, READ the registers but don't WRITE them.

#### F. Backlight Off

**Problem**: The backlight is controlled separately from the display engine. If the firmware's backlight control is disrupted, the screen appears black even though pixels are being displayed.

**Detection**: Shine a flashlight at the screen. If you can faintly see your UI, the backlight is off.

**Solution**: The backlight is typically controlled via:
- Intel GPU register `BLC_PWM_CTL` (offset `0x61254` on older gens, `0xC8254` on newer)
- ACPI `_BCM` (Brightness Control Method)
- EC (Embedded Controller) commands

---

## 4. PCI Device Detection

### 4.1 PCI Configuration Space Layout

Every PCI device has a 256-byte configuration space (or 4096 bytes for PCIe extended config):

```
Offset  Size  Field
──────  ────  ─────
0x00    2     Vendor ID
0x02    2     Device ID
0x04    2     Command Register
0x06    2     Status Register
0x08    1     Revision ID
0x09    1     Programming Interface (Prog IF)
0x0A    1     Subclass Code
0x0B    1     Class Code
0x0C    1     Cache Line Size
0x0D    1     Latency Timer
0x0E    1     Header Type (bit 7 = multi-function)
0x0F    1     BIST
0x10    4     BAR0 (Base Address Register 0)
0x14    4     BAR1
0x18    4     BAR2
0x1C    4     BAR3
0x20    4     BAR4
0x24    4     BAR5
0x28    4     CardBus CIS Pointer
0x2C    2     Subsystem Vendor ID
0x2E    2     Subsystem Device ID
0x30    4     Expansion ROM Base
0x34    1     Capabilities Pointer
0x3C    1     Interrupt Line
0x3D    1     Interrupt Pin
0x3E    1     Min Grant
0x3F    1     Max Latency
```

### 4.2 Reading Class Code (Offset 0x08 - 0x0B)

The class code is a 3-byte value at offset 0x09 - 0x0B:

```
Offset 0x0B: Class Code      (e.g., 0x03 = Display Controller)
Offset 0x0A: Subclass Code   (e.g., 0x00 = VGA Compatible)
Offset 0x09: Prog IF         (e.g., 0x00 = VGA)
```

Reading a 32-bit DWORD at offset 0x08 gives: `[RevisionID][ProgIF][Subclass][Class]`

So `(PCI::Read32(bus, dev, func, 0x08) >> 8) & 0xFFFFFF` gives `ClassCode:Subclass:ProgIF`.

**VGA Controllers**: Class = `0x03`, Subclass = `0x00` → combined `0x0300xx`

### 4.3 PCI Bus Enumeration Code Pattern

```cpp
// Scan all PCI buses for VGA controllers (class 0x0300)
struct PCIDevice {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint32_t class_code;  // class:subclass:progif (24 bits)
};

#define MAX_GPUS 8
static PCIDevice gpus[MAX_GPUS];
static int gpu_count = 0;

void ScanForGPUs() {
    gpu_count = 0;
    for (int bus = 0; bus < 256 && gpu_count < MAX_GPUS; bus++) {
        for (int dev = 0; dev < 32 && gpu_count < MAX_GPUS; dev++) {
            int max_func = 1;
            // Check if multi-function device
            uint8_t header = PCI::Read8(bus, dev, 0, 0x0E);
            if (header & 0x80) max_func = 8;

            for (int func = 0; func < max_func && gpu_count < MAX_GPUS; func++) {
                uint16_t vendor = PCI::GetVendor(bus, dev, func);
                if (vendor == 0xFFFF) continue;  // No device

                uint32_t class_code = PCI::GetClassCode(bus, dev, func);
                uint8_t base_class = (class_code >> 16) & 0xFF;
                uint8_t sub_class  = (class_code >> 8) & 0xFF;

                // Display controller: class 0x03
                // VGA compatible: subclass 0x00
                // 3D controller:  subclass 0x02 (dGPU on some Optimus configs)
                if (base_class == 0x03) {
                    gpus[gpu_count].bus = bus;
                    gpus[gpu_count].dev = dev;
                    gpus[gpu_count].func = func;
                    gpus[gpu_count].vendor_id = vendor;
                    gpus[gpu_count].device_id = PCI::GetDevice(bus, dev, func);
                    gpus[gpu_count].class_code = class_code;
                    gpu_count++;
                }
            }
        }
    }
}
```

### 4.4 Typical PCI IDs for GPUs

#### Intel Integrated Graphics

| Vendor ID | Device ID | Name | Generation |
|-----------|-----------|------|------------|
| `0x8086` | `0xA780` | RPL-S UHD 770 | Raptor Lake (13th gen) |
| `0x8086` | `0xA788` | RPL-H UHD Graphics | Raptor Lake (13th gen) - **your i5-13420H** |
| `0x8086` | `0xA789` | RPL-H UHD Graphics | Raptor Lake (13th gen) |
| `0x8086` | `0x4680` | ADL-S UHD 770 | Alder Lake (12th gen) |
| `0x8086` | `0x46A6` | ADL-P Iris Xe | Alder Lake (12th gen) |
| `0x8086` | `0x9A49` | TGL Iris Xe | Tiger Lake (11th gen) |
| `0x8086` | `0x3E92` | CFL UHD 630 | Coffee Lake (8th/9th gen) |
| `0x8086` | `0x5917` | KBL UHD 620 | Kaby Lake (7th gen) |
| `0x8086` | `0x1912` | SKL HD 530 | Skylake (6th gen) |
| `0x8086` | `0x56A5` | Arc A770 | Alchemist (discrete) |
| `0x8086` | `0x56A0` | Arc A750 | Alchemist (discrete) |

All Intel iGPUs share vendor `0x8086` and class code `0x0300` (VGA compatible controller).

#### NVIDIA Discrete GPUs

| Vendor ID | Device ID | Name | Architecture |
|-----------|-----------|------|-------------|
| `0x10DE` | `0x2860` | RTX 4050 Laptop | Ada Lovelace (AD107) - **your GPU** |
| `0x10DE` | `0x2684` | RTX 4090 | Ada Lovelace (AD102) |
| `0x10DE` | `0x2704` | RTX 4080 | Ada Lovelace (AD103) |
| `0x10DE` | `0x2786` | RTX 4070 | Ada Lovelace (AD104) |
| `0x10DE` | `0x2204` | RTX 3090 | Ampere (GA102) |
| `0x10DE` | `0x2206` | RTX 3080 | Ampere (GA102) |
| `0x10DE` | `0x2484` | RTX 3070 | Ampere (GA104) |
| `0x10DE` | `0x2B85` | RTX 5090 | Blackwell (GB202) |
| `0x10DE` | `0x2B00` | RTX 5070 | Blackwell (GB205) |

**Important**: On Optimus laptops, the NVIDIA GPU often appears with **PCI class `0x0302`** (3D controller) instead of `0x0300` (VGA compatible). This is because:
- The NVIDIA GPU has **no VGA-compatible display output** connected
- It is registered as a "3D controller" (compute/render only) in PCI config
- Only the Intel iGPU is class `0x0300` (VGA compatible)

This is a deliberate firmware distinction. When enumerating GPUs, check for both `0x0300` AND `0x0302`.

#### AMD GPUs

| Vendor ID | Device ID | Name | Type |
|-----------|-----------|------|------|
| `0x1002` | `0x164E` | Raphael iGPU (RDNA2) | Integrated (Ryzen 7000) |
| `0x1002` | `0x15BF` | Barcelo iGPU (Vega) | Integrated (Ryzen 5000) |
| `0x1002` | `0x1638` | Cezanne iGPU (Vega) | Integrated (Ryzen 5000) |
| `0x1002` | `0x744C` | RX 7900 XTX | Discrete (RDNA3) |
| `0x1002` | `0x7480` | RX 7800 XT | Discrete (RDNA3) |
| `0x1002` | `0x73BF` | RX 6900 XT | Discrete (RDNA2) |

---

## 5. Intel iGPU Framebuffer (BAR Layout & Registers)

### 5.1 PCI BAR Layout

Intel integrated GPUs have the following BAR layout:

```
BAR0 (offset 0x10): GTTMMADR - Graphics Translation Table + MMIO registers
    Size: 16 MB (most gens) or 4 MB
    This is where GPU control registers live.
    Physical address example: 0x6000_0000

BAR1 (offset 0x14): [part of BAR0 if 64-bit]

BAR2 (offset 0x18): GMADR - Graphics Memory Aperture (Aperture/Stolen Memory)
    Size: 256 MB or 512 MB (configurable in BIOS)
    This is the aperture through which CPU can access GPU-visible memory.
    The framebuffer is mapped within this region.
    Physical address example: 0x8000_0000

BAR3 (offset 0x1C): [part of BAR2 if 64-bit]

BAR4 (offset 0x20): I/O Base (legacy, rarely used)
```

**Note**: BAR0 and BAR2 are typically **64-bit BARs**, so BAR0 occupies offsets 0x10+0x14, and BAR2 occupies 0x18+0x1C.

### 5.2 Key MMIO Registers (via BAR0)

These registers control the display pipeline. All offsets are relative to BAR0 base.

#### Display Pipe Registers

Intel GPUs have "pipes" (A, B, C) and "planes" that feed into them:

```
Register            Pipe A          Pipe B          Pipe C
────────            ──────          ──────          ──────
PIPECONF            0x70008         0x71008         0x72008
PIPESRC             0x6001C         0x6101C         0x6201C
HTOTAL              0x60000         0x61000         0x62000
HBLANK              0x60004         0x61004         0x62004
HSYNC               0x60008         0x61008         0x62008
VTOTAL              0x6000C         0x6100C         0x6200C
VBLANK              0x60010         0x61010         0x62010
VSYNC               0x60014         0x61014         0x62014
```

#### Display Plane Registers (Gen9+ / Skylake and later - "Universal Planes")

Starting from Skylake (Gen9), Intel uses a new plane register layout:

```
Register                Plane 1 Pipe A      Plane 1 Pipe B
────────                ──────────────      ──────────────
PLANE_CTL               0x70180             0x71180
PLANE_SURF              0x7019C             0x7119C
PLANE_STRIDE            0x70188             0x71188
PLANE_SIZE              0x70190             0x71190
PLANE_OFFSET            0x701A4             0x711A4
PLANE_POS               0x7018C             0x7118C
```

#### Older Generations (Haswell, Broadwell - Gen7.5/Gen8)

```
Register        Pipe A          Pipe B
────────        ──────          ──────
DSPCNTR         0x70180         0x71180     // Display Plane Control
DSPSTRIDE       0x70188         0x71188     // Display Plane Stride
DSPSURF         0x7019C         0x7119C     // Display Plane Surface Base Address
DSPTILEOFF      0x701A4         0x711A4     // Tile offset
DSPLINOFF       0x70184         0x71184     // Linear offset
```

#### Key Register Descriptions

**DSPCNTR / PLANE_CTL (0x70180)**
```
Bit 31:     Plane Enable (1 = enabled, CRITICAL - if 0, plane is disabled = black screen)
Bits 29:26: Pixel Format
              0010 = XRGB 8:8:8:8 (32bpp, no alpha)
              0101 = XBGR 8:8:8:8
              0110 = XRGB 2:10:10:10
Bit 10:     Tiling mode (0 = linear, 1 = X-tiled)
```

**DSPSURF / PLANE_SURF (0x7019C)**
```
Bits 31:12: Surface base address (4KB aligned physical address)
            This is THE address the display engine reads pixels from.
            On boot, this matches the GOP/Multiboot framebuffer address.
            
Reading this register tells you where the display is currently scanning from.
```

**DSPSTRIDE / PLANE_STRIDE (0x70188)**
```
Bits 15:6:  Stride in 64-byte (cacheline) units
            Actual stride in bytes = (value >> 6) * 64
            For 1920x32bpp: stride = 7680 bytes = 120 cachelines → value = 120 << 6 = 0x1E00
```

**PIPECONF (0x70008)**
```
Bit 31:     Pipe Enable (1 = enabled)
Bit 21:     Pipe state (read-only, 1 = active)
            If this reads 0, the pipe is disabled - nothing is being scanned out.
```

### 5.3 Reading the Active Framebuffer Address

To find what address the iGPU is currently displaying from:

```cpp
// Example: Read the current framebuffer surface address from Pipe A, Plane 1
// Assumes iGPU BAR0 is already identity-mapped (which it is in Kurono's 16GB identity map)

uint64_t GetIntelActiveFBAddress(uint64_t bar0_base) {
    volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)bar0_base;
    
    // Read PLANE_CTL (Pipe A, Plane 1) - check if display plane is enabled
    uint32_t plane_ctl = mmio[0x70180 / 4];
    if (!(plane_ctl & (1u << 31))) {
        // Plane disabled - try Pipe B
        plane_ctl = mmio[0x71180 / 4];
        if (!(plane_ctl & (1u << 31))) {
            return 0;  // No active plane found
        }
        // Read PLANE_SURF for Pipe B
        return (uint64_t)(mmio[0x7119C / 4]) & 0xFFFFF000ULL;
    }
    
    // Read PLANE_SURF for Pipe A - gives 4KB-aligned physical address
    uint32_t surf = mmio[0x7019C / 4];
    return (uint64_t)(surf & 0xFFFFF000);
}

// Check if the Multiboot FB address matches what the iGPU is actually displaying
bool VerifyFramebufferAddress(uint64_t multiboot_fb, uint64_t bar0_base) {
    uint64_t active_fb = GetIntelActiveFBAddress(bar0_base);
    if (active_fb == 0) return false;  // Can't determine
    return (multiboot_fb & 0xFFFFF000) == (active_fb & 0xFFFFF000);
}
```

### 5.4 Graphics Stolen Memory

Intel iGPUs use "stolen memory" - a region of system RAM reserved by the BIOS for GPU use. The CPU cannot use this memory for general allocation. The framebuffer lives in stolen memory.

The base of stolen memory is stored in:
- **BDSM** (Base Data of Stolen Memory): PCI config offset `0x5C` on the Host Bridge (bus 0, device 0, function 0)
- **BGSM** (Base of Graphics Stolen Memory): PCI config offset `0xB4` on the iGPU device

```cpp
// Read the graphics stolen memory base (Gen8+)
uint32_t GetStolenMemoryBase() {
    // Read from Host Bridge (00:00.0), offset 0x5C
    // Bits 31:20 = base address (1MB aligned)
    return PCI::Read32(0, 0, 0, 0x5C) & 0xFFF00000;
}
```

---

## 6. NVIDIA GPU Framebuffer (BAR Layout)

### 6.1 PCI BAR Layout

NVIDIA GPUs have a consistent BAR layout across generations:

```
BAR0 (offset 0x10): MMIO Registers
    Size: 16 MB - 32 MB
    Contains all GPU control registers (PMC, PFIFO, PGRAPH, PFB, PDISP, etc.)
    Physical address example: 0xA100_0000
    64-bit BAR (occupies 0x10 + 0x14)

BAR1 (offset 0x18): VRAM Aperture (Framebuffer)
    Size: 256 MB - 16 GB (depends on GPU model and firmware config)
    Provides CPU access to GPU VRAM
    Physical address example: 0x4000_0000_0000 (typically above 4GB on modern systems)
    64-bit BAR (occupies 0x18 + 0x1C)

BAR2/3 (offset 0x20): I/O ports (legacy, 128 bytes)
    Rarely used.

BAR5 (offset 0x24): NV_RAMIN - Instance memory / PRAMIN aperture
    Size: 16 MB - 64 MB
    Used for page tables, channel descriptors, etc.
```

### 6.2 Key NVIDIA MMIO Registers (via BAR0)

Documented by the nouveau project (envytools):

```
Register Range      Block           Description
──────────────      ─────           ───────────
0x000000 - 0x000FFF   NV_PMC          Master Control (boot ID, interrupts, enable)
0x001000 - 0x001FFF   NV_PBUS         Bus Control
0x002000 - 0x003FFF   NV_PFIFO        FIFO / Command Submission
0x009000 - 0x009FFF   NV_PMASTER      Master (Turing+)
0x060000 - 0x06FFFF   NV_PDISPLAY     Display Engine
0x088000 - 0x089FFF   NV_PPCI         PCI config mirror
0x100000 - 0x100FFF   NV_PFB          Framebuffer/Memory Interface
0x109000 - 0x109FFF   NV_PKFUSE       Key Fuse (chip identity)
0x610000 - 0x61FFFF   NV_PDISP_DAC    Display output registers
0x640000 - 0x64FFFF   NV_PDISP_SOR    Serial Output Resource (DP/HDMI/eDP)
```

#### Essential registers:

**NV_PMC_BOOT_0 (0x000000)**
```
Reads the chip identification. Example for Ada Lovelace:
  Bits 31:20 = Chip ID (0x190 for AD102, 0x1A7 for AD107)
  Bits 3:0   = Stepping/revision
```

**NV_PFB_CSTATUS (0x10020C)**
```
Reports physical VRAM size in bytes.
For RTX 4050 Laptop (6GB GDDR6): reads 0x18000000 (384 MB units internally)
```

**NV_PMC_ENABLE (0x000200)**
```
Engine enable register. Individual bits enable/disable GPU sub-engines.
Bit 0: PFIFO, Bit 12: PGRAPH, etc.
Writing 0 disables engines; writing back enables them.
```

### 6.3 Nouveau / Envytools Documentation

The `envytools` project (https://envytools.readthedocs.io/) is the primary reference for NVIDIA GPU registers. Key resources:

- **MMIO register map**: `envytools/rnndb/` contains XML register definitions
- **Memory management**: `nv50_pmc.xml`, `nv50_pfb.xml`
- **Display engine**: `nv50_pdisp.xml` (NV50+), extended for Kepler/Maxwell/Pascal/Turing/Ampere/Ada
- **GPU identification**: `pmc/boot.xml`

For Ada Lovelace (AD10x), much of the register space is similar to Turing/Ampere but with newer engine blocks. Nouveau support for Ada is limited.

### 6.4 Why You Should NOT Use NVIDIA BAR1 for Display

On muxless Optimus:
- BAR1 maps to NVIDIA VRAM (GDDR6)
- The NVIDIA GPU has **no display output connected to the laptop panel**
- Writing pixels to BAR1 puts them in NVIDIA VRAM, but nobody reads them for display
- The display pipe on the NVIDIA GPU doesn't have a connected LCD panel
- **You would need to implement the entire PCIe copy mechanism** (like PRIME) to transfer those pixels to the Intel iGPU's framebuffer

---

## 7. Muxless vs Muxed vs Advanced Optimus

### 7.1 Muxless Optimus (Most Common - Your MSI Laptop)

```
LCD ←── eDP ←── Intel iGPU display engine
                    ↑
              copies from iGPU framebuffer (system RAM)
                    ↑
              DMA copy from dGPU VRAM (when using dGPU)
                    ↑
              NVIDIA dGPU renders to VRAM

External ports (HDMI/DP/USB-C):
  Some → wired to Intel iGPU
  Some → wired to NVIDIA dGPU directly
```

- **No physical MUX chip**
- iGPU always drives display, dGPU renders offscreen
- Pixel data copied over PCIe bus (adds ~1ms latency per frame)
- dGPU can be fully powered off (D3cold) when not rendering
- **For bare-metal OS**: Just use the iGPU. Ignore the NVIDIA GPU for display.

### 7.2 Muxed Optimus (Rare, Older Laptops 2010 - 2014)

```
LCD ←── eDP ←── [MUX Chip] ←─┬── Intel iGPU
                               └── NVIDIA dGPU
```

- **Physical MUX chip** (e.g., NXP CBTL06141) routes the display signal
- BIOS/ACPI controls which GPU drives the display
- `vga_switcheroo` in Linux controls the MUX
- Switching GPUs causes a brief screen blank
- **For bare-metal OS**: Check ACPI `_DSM` methods for GPU switching. The firmware may boot with either GPU.

### 7.3 Advanced Optimus / Dynamic Display Switch (DDS)

```
LCD ←── eDP ←── [Software-Controlled MUX] ←─┬── Intel iGPU
                                               └── NVIDIA dGPU
```

- **Software-controlled MUX** (newer high-end laptops, 2020+)
- NVIDIA driver can switch the MUX at runtime (no reboot needed)
- When MUX routes to dGPU: direct dGPU-to-display path (lower latency)
- When MUX routes to iGPU: standard Optimus (power saving)
- The MUX is controlled via ACPI `_DSM` methods on the dGPU
- **For bare-metal OS**: Since Advanced Optimus defaults to muxless mode at boot, the iGPU framebuffer is still the correct one to use. Switching to dGPU mode requires implementing the full ACPI _DSM calls and NVIDIA display initialization.

### 7.4 How to Detect Which Type You Have

```cpp
enum OptimusType {
    OPTIMUS_NONE,           // Single GPU
    OPTIMUS_MUXLESS,        // iGPU + dGPU, no MUX
    OPTIMUS_MUXED,          // iGPU + dGPU + physical MUX
    OPTIMUS_ADVANCED,       // iGPU + dGPU + software MUX
};

OptimusType DetectOptimusType() {
    // Step 1: Count GPUs
    int intel_count = 0, nvidia_count = 0;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].vendor_id == 0x8086 && (gpus[i].class_code >> 16) == 0x03)
            intel_count++;
        if (gpus[i].vendor_id == 0x10DE)
            nvidia_count++;
    }
    
    if (nvidia_count == 0) return OPTIMUS_NONE;
    if (intel_count == 0) return OPTIMUS_NONE;  // dGPU only laptop
    
    // Step 2: Check NVIDIA GPU subclass
    // 0x0300 = VGA compatible (has display output)
    // 0x0302 = 3D controller (no display output = muxless)
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].vendor_id == 0x10DE) {
            uint16_t subclass = (gpus[i].class_code >> 8) & 0xFFFF;
            if (subclass == 0x0302) {
                return OPTIMUS_MUXLESS;  // dGPU is "3D controller only"
            }
            // subclass 0x0300 = dGPU has VGA outputs = possibly muxed
            // Need ACPI to distinguish muxed from Advanced Optimus
            return OPTIMUS_MUXED;  // Conservative default
        }
    }
    
    return OPTIMUS_MUXLESS;  // Default assumption for modern laptops
}
```

---

## 8. What Linux Does

### 8.1 Boot Sequence on Optimus

1. **UEFI GOP** sets up framebuffer via Intel iGPU
2. **GRUB** uses `efifb`/`simplefb` from GOP framebuffer
3. **Linux early boot** uses `efifb`/`simplefb` driver - just writes to the GOP framebuffer address
4. **`i915` driver loads** for Intel iGPU:
   - Performs full KMS (Kernel Mode Setting) modesetting
   - Takes over the display pipe from firmware
   - Allocates its own framebuffer in stolen memory or GTT
   - Programs display pipe registers (DSPSURF, DSPCNTR, PIPECONF, etc.)
   - Registers DRM framebuffer device (`/dev/dri/card0`)
5. **`nouveau` or `nvidia` driver loads** for NVIDIA dGPU:
   - Maps BAR0 (MMIO) and BAR1 (VRAM)
   - Initializes GPU engine (PFIFO, PGRAPH, etc.)
   - Does NOT touch display - the dGPU has no connected outputs on muxless
   - Registers as a "render-only" DRM device (`/dev/dri/renderD128`)

### 8.2 Key Linux Subsystems

#### DRM/KMS (Direct Rendering Manager / Kernel Mode Setting)

The primary display abstraction in Linux. Handles:
- Modesetting (display resolution, timing)
- Framebuffer management
- Page flipping / VSync
- Multi-GPU coordination

#### vga_switcheroo

Handles GPU switching on **muxed** systems:
- `/sys/kernel/debug/vgaswitcheroo/switch`
- Controls `D3cold` power states for dGPU
- Routes display through physical MUX chip

Not used on muxless Optimus (the MUX doesn't exist).

#### DRM PRIME / Render Offload

Handles the frame copy mechanism on **muxless** Optimus:
1. Application requests rendering on NVIDIA GPU
2. `nouveau`/`nvidia` renders to dGPU VRAM framebuffer
3. Frame is exported as a **DMA-BUF** (shared memory handle)
4. `i915` imports the DMA-BUF
5. Frame is copied/blitted to iGPU framebuffer via PCIe DMA
6. iGPU display engine scans it out to the panel

#### Environment Variables for PRIME Offloading

```bash
# Run application on NVIDIA GPU with nouveau:
DRI_PRIME=1 ./application

# Run application on NVIDIA GPU with proprietary driver:
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia ./application
```

### 8.3 The `i915` Driver

The Intel iGPU driver. Key source files (Linux kernel):
- `drivers/gpu/drm/i915/display/intel_display.c` - modesetting
- `drivers/gpu/drm/i915/display/intel_fb.c` - framebuffer management
- `drivers/gpu/drm/i915/gt/intel_ggtt.c` - Graphics Global Translation Table
- `drivers/gpu/drm/i915/i915_reg.h` - ALL register definitions (invaluable reference!)

The `i915_reg.h` file contains thousands of register definitions. Key ones:

```c
#define _DSPASURF    0x7019C   /* Display A Surface Base Address */
#define _DSPBSURF    0x7119C   /* Display B Surface Base Address */
#define _DSPACNTR    0x70180   /* Display A Control */
#define _DSPBCNTR    0x71180   /* Display B Control */
#define _PIPEACONF   0x70008   /* Pipe A Config */
#define _PIPEBCONF   0x71008   /* Pipe B Config */
```

### 8.4 Implications for Kurono OS

**You don't need to replicate what Linux does.** Linux performs full KMS modesetting because it needs to:
- Change resolutions dynamically
- Support multiple monitors
- Handle hotplug
- Support GPU acceleration and compositing

For a bare-metal OS that just needs pixels on screen:
1. **Use the GRUB-provided framebuffer** - it's already set up correctly by the iGPU firmware
2. **Don't reinitialize the display pipeline** - the firmware already did it
3. **Mark the framebuffer as Write-Combining** - you already do this
4. **Use non-temporal stores for buffer operations** - you already do this

---

## 9. Solutions for Bare-Metal OS Developers

### 9.1 The Golden Rule

> **Don't fight the firmware. Use what it gives you.**

On Optimus laptops, the simplest and most reliable approach:

1. Boot via GRUB/UEFI
2. GRUB sets a graphics mode via GOP
3. Read the framebuffer address from Multiboot info
4. Mark those pages as Write-Combining
5. Write pixels
6. Done

### 9.2 Defensive Framebuffer Initialization

```cpp
bool InitFramebufferSafe(multiboot_info_t* mbi) {
    // 1. Verify multiboot framebuffer info is present
    if (!(mbi->flags & (1u << 12))) return false;
    if (mbi->framebuffer_addr == 0) return false;
    if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0) return false;
    if (mbi->framebuffer_type == 2) return false;  // text mode, not graphical
    
    uint64_t fb_addr  = mbi->framebuffer_addr;
    uint32_t fb_width = mbi->framebuffer_width;
    uint32_t fb_height = mbi->framebuffer_height;
    uint32_t fb_pitch = mbi->framebuffer_pitch;
    uint8_t  fb_bpp   = mbi->framebuffer_bpp;
    
    // 2. Sanity check the address
    if (fb_addr < 0x100000) return false;  // Below 1MB = definitely wrong
    if (fb_bpp != 32 && fb_bpp != 24) return false;  // Unexpected format
    
    // 3. Scan PCI for VGA controllers and log what we find
    ScanForGPUs();
    for (int i = 0; i < gpu_count; i++) {
        SerialLogger::Log("GPU found: vendor=0x");
        SerialLogger::LogHex(gpus[i].vendor_id);
        SerialLogger::Log(" device=0x");
        SerialLogger::LogHex(gpus[i].device_id);
        SerialLogger::Log(" class=0x");
        SerialLogger::LogHex(gpus[i].class_code);
        SerialLogger::Log("\r\n");
    }
    
    // 4. Identify the active display GPU (the one GOP used)
    //    On muxless Optimus: always Intel (0x8086) with class 0x0300
    bool found_igpu = false;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].vendor_id == 0x8086 && ((gpus[i].class_code >> 8) & 0xFFFF) == 0x0300) {
            found_igpu = true;
            SerialLogger::Log("Active display GPU: Intel iGPU\r\n");
            
            // 5. Optionally verify: read Intel iGPU DSPSURF to confirm FB address
            uint64_t bar0 = ReadBAR64(gpus[i].bus, gpus[i].dev, gpus[i].func, 0);
            if (bar0 != 0) {
                volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)bar0;
                uint32_t dspsurf = mmio[0x7019C / 4];
                SerialLogger::Log("  iGPU DSPSURF=0x");
                SerialLogger::LogHex(dspsurf);
                SerialLogger::Log("  Multiboot FB=0x");
                SerialLogger::LogHex((uint32_t)fb_addr);
                SerialLogger::Log("\r\n");
                // These should match (at least the upper bits)
            }
            break;
        }
    }
    
    // 6. Remap framebuffer pages to Write-Combining
    uint32_t fb_size = fb_pitch * fb_height;
    remap_fb_writecombining(fb_addr, fb_size);
    
    // 7. Initialize graphics subsystem with the framebuffer
    Graphics::Init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    
    // 8. Clear to a known color (dark blue) - if this appears, we're working
    Graphics::Clear(0x001428);
    
    return true;
}
```

### 9.3 Checklist: Why Is My Screen Black?

```
□ Is the multiboot magic correct (0x2BADB002)?
□ Is bit 12 set in multiboot flags (framebuffer info present)?
□ Is framebuffer_addr non-zero?
□ Is framebuffer_type == 1 (RGB, not text mode)?
□ Is the framebuffer address identity-mapped in your page tables?
□ Are the framebuffer pages marked Write-Combining (not Write-Back)?
□ Are you using non-temporal stores (movntdq) or explicit cache flushes?
□ Did you issue `sfence` or `mfence` after writing to the framebuffer?
□ Did you accidentally overwrite the multiboot info struct in memory?
□ Did you accidentally write to NVIDIA BAR1 instead of the iGPU's framebuffer?
□ Did you accidentally disable the iGPU by writing to its PCI config?
□ Is the backlight still on? (Shine a flashlight at the screen to check)
□ Are you clearing the framebuffer to a non-black color to test?
□ Check serial output - does the framebuffer address look reasonable?
```

### 9.4 Advanced: Re-reading the Framebuffer If It Becomes Stale

In rare cases, the firmware framebuffer address may become invalid (e.g., after a GPU fault or if the firmware used a temporary buffer). Here's how to recover:

```cpp
// Find the Intel iGPU and read its current surface address
uint64_t RecoverFramebufferAddress() {
    // Find Intel iGPU
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].vendor_id != 0x8086) continue;
        if (((gpus[i].class_code >> 8) & 0xFFFF) != 0x0300) continue;
        
        // Read BAR0 (MMIO base)
        uint64_t bar0 = ReadBAR64(gpus[i].bus, gpus[i].dev, gpus[i].func, 0);
        if (bar0 == 0) continue;
        
        volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)bar0;
        
        // Check Pipe A first
        uint32_t pipe_conf = mmio[0x70008 / 4];
        if (pipe_conf & (1u << 31)) {
            // Pipe A enabled - read its surface address
            uint32_t surf = mmio[0x7019C / 4] & 0xFFFFF000;
            if (surf != 0) return (uint64_t)surf;
        }
        
        // Try Pipe B
        pipe_conf = mmio[0x71008 / 4];
        if (pipe_conf & (1u << 31)) {
            uint32_t surf = mmio[0x7119C / 4] & 0xFFFFF000;
            if (surf != 0) return (uint64_t)surf;
        }
        
        // Try Pipe C
        pipe_conf = mmio[0x72008 / 4];
        if (pipe_conf & (1u << 31)) {
            uint32_t surf = mmio[0x7219C / 4] & 0xFFFFF000;
            if (surf != 0) return (uint64_t)surf;
        }
    }
    
    return 0;  // Failed to find active framebuffer
}
```

### 9.5 ACPI Methods for Display Detection

If you have an ACPI parser, you can use standard ACPI methods to detect connected displays:

- **`_DOD` (Display Output Devices)**: Enumerates all connected display outputs
- **`_DCS` (Display Current Status)**: Reports if a display is connected and active
- **`_DGS` (Display Graphics State)**: Reports current display state
- **`_DSS` (Display Switch State)**: Controls display output routing

These are defined in the ACPI spec §B.4 (Display-Specific Methods) and require a full ACPI interpreter (AML execution). Not recommended for early OS development.

---

## 10. MSI Thin 15 B13VE Specifics

### 10.1 Hardware Configuration

| Component | Detail |
|-----------|--------|
| CPU | Intel Core i5-13420H (Raptor Lake, 8C/12T) |
| iGPU | Intel UHD Graphics (RPL-H, 48 EU) |
| iGPU PCI ID | `8086:A7A0` or `8086:A788` (depends on SKU stepping) |
| iGPU PCI Class | `0x0300` (VGA Compatible Controller) |
| dGPU | NVIDIA GeForce RTX 4050 Laptop (6GB GDDR6) |
| dGPU PCI ID | `10DE:2860` (AD107) |
| dGPU PCI Class | `0x0302` (3D Controller) - **NOT 0x0300** |
| Display | 15.6" FHD (1920x1080), connected via eDP to iGPU |
| Optimus Type | **Muxless** - no MUX chip |
| HDMI Port | Routed through NVIDIA dGPU (common on MSI laptops) |
| USB-C/DP | If present, may route through iGPU or dGPU (check `lspci`) |

### 10.2 PCI Topology (Expected)

```
00:00.0 Host Bridge: Intel Corporation Raptor Lake-H [8086:A700]
00:02.0 VGA compatible controller: Intel Corporation Raptor Lake-P [Iris Xe Graphics] [8086:A788]
01:00.0 3D controller: NVIDIA Corporation AD107M [GeForce RTX 4050] [10DE:2860]
```

Note bus `01` for the NVIDIA GPU - it's behind a PCIe bridge.

### 10.3 Framebuffer Behavior

On this laptop:
- UEFI firmware initializes Intel UHD Graphics via GOP
- GOP reports a framebuffer in the Intel iGPU's stolen memory aperture
- GRUB receives this framebuffer and passes it to your kernel via Multiboot
- The NVIDIA RTX 4050 is **irrelevant for the internal display**
- External HDMI may require NVIDIA driver initialization (not needed for basic OS work)

### 10.4 Expected Multiboot Info Values

When booting at 1920x1080x32:
```
framebuffer_addr:   0x80000000 - 0xC0000000 range (typical)
                    (could also be above 4GB on some firmware)
framebuffer_width:  1920
framebuffer_height: 1080
framebuffer_pitch:  7680 (1920 × 4 bytes)
framebuffer_bpp:    32
framebuffer_type:   1 (RGB direct color)
```

### 10.5 BIOS/UEFI Settings That Affect Display

MSI BIOS settings to check:
- **Graphics Configuration → Optimus**: Should be "MSHybrid" (default). "Discrete Only" may not work on muxless designs.
- **Secure Boot**: Disable for custom OS (you probably already did this)
- **CSM**: Disable (use pure UEFI for best GOP support)
- **DVMT Pre-Allocated**: Controls Intel iGPU stolen memory size. Set to 64MB or higher for high-resolution framebuffers.

---

## 11. AMD Hybrid Graphics (PowerXpress / Enduro)

### 11.1 Overview

AMD's equivalent to Optimus has gone through several names:
- **ATI PowerXpress** (2008 - 2012) - muxed design
- **AMD Enduro** (2012 - 2015) - muxless, similar to Optimus
- **AMD SmartShift** (2020+) - dynamic power sharing between AMD APU + AMD dGPU

### 11.2 Architecture

```
Scenario 1: AMD APU + NVIDIA dGPU
  LCD ←── AMD Vega/RDNA iGPU ←── (render copy) ←── NVIDIA dGPU

Scenario 2: AMD APU + AMD dGPU
  LCD ←── AMD Vega/RDNA iGPU ←── (render copy) ←── AMD RDNA dGPU

Scenario 3: Intel CPU + AMD dGPU (rare in laptops, common on older models)
  LCD ←── Intel iGPU ←── (render copy) ←── AMD dGPU
```

### 11.3 Key Differences from NVIDIA Optimus

| Aspect | NVIDIA Optimus | AMD Enduro |
|--------|---------------|------------|
| iGPU vendor | Intel (or AMD) | AMD (APU) |
| dGPU class | `0x0302` (3D controller) | `0x0300` or `0x0302` |
| Display driver | Intel i915 | AMD amdgpu |
| Render copy | PRIME (DMA-BUF) | PRIME (DMA-BUF) |
| Power control | ACPI _DSM | ACPI _ATPX |
| MUX control | ACPI _DSM | ACPI _ATPX |

### 11.4 AMD APU Display Registers

AMD APUs (Ryzen integrated graphics) use the DCN (Display Core Next) display engine:

- BAR0: MMIO registers (including display engine)
- The framebuffer is in system memory (GART/GTT mapped)
- Display pipe registers are in BAR0 at offsets like:
  - CRTC: `0x1B9C0` - `0x1B9FF` (varies by DCN version)
  - Hub control: `0x0E00` - `0x0FFF` (HUBP - Hub Pipeline)
  - Surface address: programmed via HUBP registers
  
AMD's register documentation is largely undocumented publicly. The `amdgpu` kernel driver source (`drivers/gpu/drm/amd/display/dc/`) is the best reference.

### 11.5 For Bare-Metal OS Developers

The approach is identical to Intel Optimus:
1. Use the UEFI GOP framebuffer from the APU
2. Don't touch the dGPU for display
3. Mark framebuffer as WC
4. Write pixels

---

## 12. Apple Silicon / ARM Considerations

### 12.1 Apple Silicon (M1/M2/M3/M4)

Apple Silicon Macs have a fundamentally different GPU architecture:

- **Unified Memory Architecture (UMA)**: CPU and GPU share the same physical memory pool
- **No separate VRAM**: The GPU renders directly in system memory
- **No PCIe GPU**: The GPU is on the SoC die, connected via internal fabric
- **Custom display controller**: Not PCIe-based, uses Apple's proprietary DCP (Display Controller Processor)
- **No standard UEFI GOP**: Apple uses its own boot process (iBoot → XNU). The Asahi Linux project has reverse-engineered the display initialization.

**For OS developers**: Apple Silicon requires completely custom display initialization. No PCI, no VBE, no standard UEFI. See the Asahi Linux project for documentation.

### 12.2 Qualcomm Snapdragon (ARM Windows Laptops)

- GPU: Qualcomm Adreno (integrated)
- Display: Qualcomm DPU (Display Processing Unit)
- Framebuffer: Via UEFI GOP (if compliant) or DT-defined
- Hybrid GPU: Not applicable (single GPU)

### 12.3 NVIDIA Tegra (Jetson, Nintendo Switch)

- GPU: NVIDIA GPU on SoC (shared memory with CPU)
- Display: NVIDIA DC (Display Controller), not the same as desktop display engine
- Framebuffer: Through DRM/KMS in Linux, or Tegra-specific register programming
- No Optimus - single GPU

**Bottom line for ARM**: Each SoC vendor has a completely different display subsystem. There is no universal "PCI VGA" or "UEFI GOP" guarantee. Stick with x86 if you want standards-based display initialization.

---

## 13. Code Patterns for Kurono OS

This section provides concrete code that integrates with your existing Kurono OS architecture.

### 13.1 GPU Enumeration Extension

Add to your existing PCI scanning:

```cpp
// pci_gpu.h - GPU detection for Optimus/hybrid systems

#pragma once
#include "../kernel/types.h"
#include "../kernel/pci.h"

#define GPU_VENDOR_INTEL   0x8086
#define GPU_VENDOR_NVIDIA  0x10DE
#define GPU_VENDOR_AMD     0x1002

#define PCI_CLASS_VGA        0x0300  // VGA compatible controller
#define PCI_CLASS_3D         0x0302  // 3D controller (Optimus dGPU)
#define PCI_CLASS_DISPLAY    0x0380  // Display controller (other)

struct DetectedGPU {
    uint8_t  bus, dev, func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t pci_class;     // class:subclass (16 bits)
    uint64_t bar0;          // MMIO base
    uint64_t bar2;          // Aperture / VRAM (Intel BAR2 or NVIDIA BAR1)
    bool     is_display_gpu; // true if this GPU drives the display
    const char* description;
};

enum HybridGPUType {
    HYBRID_NONE,         // Single GPU or no GPU
    HYBRID_MUXLESS,      // Muxless Optimus (iGPU drives display)
    HYBRID_MUXED,        // Muxed (either GPU can drive display)
    HYBRID_ADVANCED,     // Advanced Optimus (software MUX)
};

class GPUDetector {
public:
    static void ScanAll();
    static int GetGPUCount();
    static const DetectedGPU* GetGPU(int index);
    static const DetectedGPU* GetDisplayGPU();  // Returns the GPU driving the display
    static HybridGPUType GetHybridType();
    static void DumpInfo();  // Log all GPU info to serial

private:
    static DetectedGPU gpus[8];
    static int count;
    static HybridGPUType hybrid_type;
};
```

### 13.2 Reading 64-bit BARs

```cpp
// Read a 64-bit BAR (handles the two-DWORD encoding)
static uint64_t ReadBAR64(uint8_t bus, uint8_t dev, uint8_t func, int bar_index) {
    uint32_t bar_low = PCI::GetBAR(bus, dev, func, bar_index);
    
    if (bar_low & 1) {
        // I/O BAR - should not happen for GPU MMIO
        return bar_low & 0xFFFFFFFC;
    }
    
    uint8_t type = (bar_low >> 1) & 0x03;
    uint64_t base = bar_low & 0xFFFFFFF0;
    
    if (type == 0x02) {
        // 64-bit BAR: next BAR register contains upper 32 bits
        uint32_t bar_high = PCI::GetBAR(bus, dev, func, bar_index + 1);
        base |= ((uint64_t)bar_high << 32);
    }
    
    return base;
}
```

### 13.3 Intel iGPU Framebuffer Verification

```cpp
// Verify that the Multiboot framebuffer address matches the Intel iGPU's
// active display surface. Returns true if verified, false if mismatch or
// unable to check.
bool VerifyIntelFBAddress(uint64_t multiboot_fb_addr) {
    // Find Intel iGPU
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].vendor_id != 0x8086) continue;
        if ((gpus[i].pci_class & 0xFF00) != 0x0300) continue;
        
        uint64_t bar0 = gpus[i].bar0;
        if (bar0 == 0 || bar0 > 0xFFFFFFFF) {
            // BAR0 not in our identity-mapped range
            // Would need to set up page table mapping first
            return false;
        }
        
        volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)bar0;
        
        // Try each pipe (A, B, C) - check PIPECONF first, then read PLANE_SURF
        const uint32_t pipe_offsets[] = {0x70000, 0x71000, 0x72000};
        for (int p = 0; p < 3; p++) {
            uint32_t pipeconf = mmio[(pipe_offsets[p] + 0x08) / 4];
            if (!(pipeconf & (1u << 31))) continue;  // Pipe not enabled
            
            // Read PLANE_SURF for this pipe's primary plane
            uint32_t surf = mmio[(pipe_offsets[p] + 0x19C) / 4];
            uint64_t active_fb = (uint64_t)(surf & 0xFFFFF000);
            
            if ((multiboot_fb_addr & 0xFFFFF000) == active_fb) {
                SerialLogger::Log("Intel FB verified: Multiboot FB matches DSPSURF\r\n");
                return true;
            } else {
                SerialLogger::Log("Intel FB MISMATCH: Multiboot=0x");
                SerialLogger::LogHex((uint32_t)multiboot_fb_addr);
                SerialLogger::Log(" DSPSURF=0x");
                SerialLogger::LogHex((uint32_t)active_fb);
                SerialLogger::Log("\r\n");
                // Could use active_fb as the real address
            }
        }
    }
    return false;
}
```

### 13.4 Complete Intel iGPU Register Map Reference

For OS developers who need to read/debug Intel GPU state. All offsets relative to BAR0.

```
═══════════════════════════════════════════════════════════════════
 INTEL GEN9+ (Skylake through Raptor Lake) REGISTER REFERENCE
 For Kurono OS bare-metal debugging
═══════════════════════════════════════════════════════════════════

── GPU IDENTITY ────────────────────────────────────────────────
0x902C    TIMESTAMP_LOW       Render engine timestamp (low)
0x9030    TIMESTAMP_HIGH      Render engine timestamp (high)

── DISPLAY PIPES ───────────────────────────────────────────────
                    Pipe A      Pipe B      Pipe C
HTOTAL              0x60000     0x61000     0x62000
HBLANK              0x60004     0x61004     0x62004
HSYNC               0x60008     0x61008     0x62008
VTOTAL              0x6000C     0x6100C     0x6200C
VBLANK              0x60010     0x61010     0x62010
VSYNC               0x60014     0x61014     0x62014
PIPESRC             0x6001C     0x6101C     0x6201C     (source image size)
PIPECONF            0x70008     0x71008     0x72008     (bit31=enable)
PIPESTATUS          0x70024     0x71024     0x72024     (VBlank/frame status)

── DISPLAY PLANES (Universal Planes, Gen9+) ────────────────────
Plane 1 per pipe:
                    Pipe A      Pipe B      Pipe C
PLANE_CTL           0x70180     0x71180     0x72180     (bit31=enable, bits29:26=format)
PLANE_LINOFF        0x70184     0x71184     0x72184     (linear offset in bytes)
PLANE_STRIDE        0x70188     0x71188     0x72188     (stride in 64B chunks)
PLANE_POS           0x7018C     0x7118C     0x7218C     (x,y position on pipe)
PLANE_SIZE          0x70190     0x71190     0x72190     (height:width in pixels)
PLANE_SURF          0x7019C     0x7119C     0x7219C     (surface phys addr, 4KB aligned)
PLANE_TILEOFF       0x701A4     0x711A4     0x721A4     (tile offset x,y)

Plane 2 per pipe (cursor or secondary):
                    Pipe A      Pipe B      Pipe C
PLANE2_CTL          0x70280     0x71280     0x72280
PLANE2_SURF         0x7029C     0x7129C     0x7229C

── CURSOR ──────────────────────────────────────────────────────
                    Pipe A      Pipe B      Pipe C
CUR_CTL             0x70080     0x71080     0x72080
CUR_BASE            0x70084     0x71084     0x72084
CUR_POS             0x70088     0x71088     0x72088

── DISPLAY PORT / eDP ──────────────────────────────────────────
DP_CTL_A            0x64000     (DDI A - usually eDP for laptop panel)
DP_CTL_B            0x64100     (DDI B)
DP_CTL_C            0x64200     (DDI C)
DP_AUX_CTL_A        0x64010     (AUX channel control)
DDI_BUF_CTL_A       0x64000     (DDI Buffer Control)

── BACKLIGHT ───────────────────────────────────────────────────
BLC_PWM_CTL         0xC8250     (backlight PWM control - Gen9+)
BLC_PWM_DATA        0xC8254     (backlight PWM duty cycle)

── POWER / CLOCKING ────────────────────────────────────────────
PWR_WELL_CTL        0x45400     (power well control)
FUSE_STATUS         0x42000     (fuse strap status, tells number of pipes)
```

### 13.5 Debugging Framebuffer Issues via Serial

Add this diagnostic function to dump all relevant display state:

```cpp
void DumpDisplayDiagnostics(uint64_t mb_fb_addr, uint32_t mb_fb_width,
                            uint32_t mb_fb_height, uint32_t mb_fb_pitch) {
    SerialLogger::Log("\r\n=== DISPLAY DIAGNOSTICS ===\r\n");
    
    // 1. Multiboot framebuffer info
    SerialLogger::Log("Multiboot FB: addr=0x");
    SerialLogger::LogHex((uint32_t)(mb_fb_addr >> 32));
    SerialLogger::LogHex((uint32_t)(mb_fb_addr & 0xFFFFFFFF));
    SerialLogger::Log(" ");
    SerialLogger::LogDec(mb_fb_width);
    SerialLogger::Log("x");
    SerialLogger::LogDec(mb_fb_height);
    SerialLogger::Log(" pitch=");
    SerialLogger::LogDec(mb_fb_pitch);
    SerialLogger::Log("\r\n");
    
    // 2. PCI GPU enumeration
    SerialLogger::Log("\r\nPCI Display devices:\r\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor = PCI::GetVendor(bus, dev, func);
                if (vendor == 0xFFFF) { if (func == 0) break; continue; }
                
                uint32_t class_code = PCI::GetClassCode(bus, dev, func);
                uint8_t base_class = (class_code >> 16) & 0xFF;
                if (base_class != 0x03) continue;
                
                uint16_t device_id = PCI::GetDevice(bus, dev, func);
                SerialLogger::Log("  ");
                SerialLogger::LogHex(bus); SerialLogger::Log(":");
                SerialLogger::LogHex(dev); SerialLogger::Log(".");
                SerialLogger::LogHex(func);
                SerialLogger::Log(" [");
                SerialLogger::LogHex(vendor); SerialLogger::Log(":");
                SerialLogger::LogHex(device_id); SerialLogger::Log("]");
                SerialLogger::Log(" class=0x");
                SerialLogger::LogHex(class_code);
                
                // Read BARs
                uint64_t bar0 = ReadBAR64(bus, dev, func, 0);
                SerialLogger::Log(" BAR0=0x");
                SerialLogger::LogHex((uint32_t)(bar0 >> 32));
                SerialLogger::LogHex((uint32_t)(bar0 & 0xFFFFFFFF));
                
                if (vendor == 0x8086) {
                    uint64_t bar2 = ReadBAR64(bus, dev, func, 2);
                    SerialLogger::Log(" BAR2(GMADR)=0x");
                    SerialLogger::LogHex((uint32_t)(bar2 >> 32));
                    SerialLogger::LogHex((uint32_t)(bar2 & 0xFFFFFFFF));
                }
                if (vendor == 0x10DE) {
                    uint64_t bar1 = ReadBAR64(bus, dev, func, 1);
                    SerialLogger::Log(" BAR1(VRAM)=0x");
                    SerialLogger::LogHex((uint32_t)(bar1 >> 32));
                    SerialLogger::LogHex((uint32_t)(bar1 & 0xFFFFFFFF));
                    
                    // Note class code
                    uint8_t sub = (class_code >> 8) & 0xFF;
                    if (sub == 0x02) SerialLogger::Log(" [3D CTRL - no display]");
                    else if (sub == 0x00) SerialLogger::Log(" [VGA - has display]");
                }
                
                SerialLogger::Log("\r\n");
                
                // If Intel iGPU, dump display pipe status
                if (vendor == 0x8086 && bar0 != 0 && bar0 < 0x100000000ULL) {
                    volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)bar0;
                    for (int pipe = 0; pipe < 3; pipe++) {
                        uint32_t base = 0x70000 + pipe * 0x1000;
                        uint32_t pipeconf = mmio[(base + 0x08) / 4];
                        uint32_t plane_ctl = mmio[(base + 0x180) / 4];
                        uint32_t plane_surf = mmio[(base + 0x19C) / 4];
                        uint32_t plane_stride = mmio[(base + 0x188) / 4];
                        uint32_t plane_size = mmio[(base + 0x190) / 4];
                        
                        char pipe_name = 'A' + pipe;
                        SerialLogger::Log("    Pipe ");
                        SerialLogger::Log(&pipe_name);  // crude, see below
                        SerialLogger::Log(": PIPECONF=0x");
                        SerialLogger::LogHex(pipeconf);
                        SerialLogger::Log(pipeconf & (1u<<31) ? " [ON]" : " [OFF]");
                        SerialLogger::Log(" PLANE_CTL=0x");
                        SerialLogger::LogHex(plane_ctl);
                        SerialLogger::Log(plane_ctl & (1u<<31) ? " [ON]" : " [OFF]");
                        SerialLogger::Log(" SURF=0x");
                        SerialLogger::LogHex(plane_surf);
                        SerialLogger::Log(" STRIDE=0x");
                        SerialLogger::LogHex(plane_stride);
                        SerialLogger::Log(" SIZE=0x");
                        SerialLogger::LogHex(plane_size);
                        SerialLogger::Log("\r\n");
                    }
                }
                
                if (func == 0) {
                    uint8_t hdr = PCI::Read8(bus, dev, 0, 0x0E);
                    if (!(hdr & 0x80)) break; // Not multi-function
                }
            }
        }
    }
    
    SerialLogger::Log("=== END DIAGNOSTICS ===\r\n\r\n");
}
```

---

## Quick Reference Card

```
╔═══════════════════════════════════════════════════════════════════╗
║  OPTIMUS BARE-METAL CHEAT SHEET                                  ║
╠═══════════════════════════════════════════════════════════════════╣
║                                                                   ║
║  Rule #1: The iGPU drives the display. Always.                    ║
║  Rule #2: Use the GRUB/Multiboot framebuffer address as-is.       ║
║  Rule #3: Mark FB pages Write-Combining (PAT).                    ║
║  Rule #4: Use non-temporal stores (movntdq + sfence).             ║
║  Rule #5: Don't touch GPU registers unless you know exactly why.  ║
║  Rule #6: Log EVERYTHING to serial for debugging.                 ║
║                                                                   ║
║  PCI Detection:                                                   ║
║    Intel iGPU: vendor=0x8086, class=0x0300                        ║
║    NVIDIA dGPU: vendor=0x10DE, class=0x0302 (muxless)             ║
║    AMD iGPU: vendor=0x1002, class=0x0300                          ║
║                                                                   ║
║  Intel BAR Layout:                                                ║
║    BAR0 = MMIO registers (16MB)                                   ║
║    BAR2 = Graphics aperture / stolen memory (256-512MB)           ║
║                                                                   ║
║  Intel Display Registers (BAR0 + offset):                         ║
║    PIPECONF:   0x70008 (Pipe A), bit31=enable                     ║
║    PLANE_CTL:  0x70180 (Pipe A Plane 1), bit31=enable             ║
║    PLANE_SURF: 0x7019C (Pipe A Plane 1), FB physical address      ║
║                                                                   ║
║  NVIDIA BAR Layout:                                               ║
║    BAR0 = MMIO registers (16-32MB)                                ║
║    BAR1 = VRAM aperture (NOT connected to display on muxless!)    ║
║                                                                   ║
║  Black Screen Debug:                                              ║
║    1. Check serial output for FB address                          ║
║    2. Verify WC remapping happened                                ║
║    3. Try wbinvd after writes (desperate measure)                 ║
║    4. Flashlight test (backlight issue?)                          ║
║    5. Read DSPSURF - does it match Multiboot FB?                  ║
║                                                                   ║
╚═══════════════════════════════════════════════════════════════════╝
```

---

## References

1. **Intel Open Source HD Graphics Programmer's Reference** (PRM)
   - Volume 12: Display Engine
   - Available at: https://01.org/linuxgraphics/documentation
   - Contains ALL register definitions for Gen9+ display engine

2. **Envytools** - NVIDIA GPU documentation project
   - https://envytools.readthedocs.io/
   - Register database: `rnndb/` directory

3. **Linux kernel source**
   - `drivers/gpu/drm/i915/i915_reg.h` - Intel register definitions
   - `drivers/gpu/drm/i915/display/` - Intel display pipeline code
   - `drivers/gpu/drm/nouveau/` - NVIDIA open-source driver
   - `drivers/gpu/drm/amd/display/dc/` - AMD display core

4. **Multiboot Specification**
   - https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
   - Framebuffer fields at offset 88 - 109

5. **UEFI Specification** - Graphics Output Protocol (GOP)
   - Chapter 12.9: Graphics Output Protocol

6. **PCI Local Bus Specification** - Configuration space layout
   - Class codes: Appendix D

7. **OSDev Wiki**
   - https://wiki.osdev.org/Intel_HD_Graphics
   - https://wiki.osdev.org/PCI
   - https://wiki.osdev.org/NVIDIA

---

*Document created for Kurono OS development. Last updated: 2026-04-03.*
