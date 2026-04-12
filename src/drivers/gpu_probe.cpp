//  kurono os  -  gpu probe & hybrid gpu support
//  scans pci bus for all display controllers, classifies hybrid gpu
//  topologies (optimus, powerxpress, mux, etc.), and validates/corrects
//  the framebuffer address for the gpu that actually drives the display.
//
//  this is the fix for black screens on optimus laptops:
//  the multiboot framebuffer address from grub always points to the gpu
//  that uefi gop initialized  -  on muxless optimus, that's the intel igpu.
//  but if the address is stale or the display plane was reconfigured,
//  we can read intel's dspsurf register to get the real scanout address.
#include "gpu_probe.h"
#include "serial.h"

GpuProbeResult GpuProbe::result = {};

//  pci configuration space access

static inline void _out32(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %w1" : : "a"(val), "Nd"(port));
}
static inline uint32_t _in32(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint32_t GpuProbe::PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    _out32(0xCF8, addr);
    return _in32(0xCFC);
}

void GpuProbe::PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    _out32(0xCF8, addr);
    _out32(0xCFC, val);
}

uint64_t GpuProbe::ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx) {
    uint8_t offset = 0x10 + bar_idx * 4;
    uint32_t bar_lo = PciRead(bus, dev, func, offset);
    if (bar_lo & 1) return bar_lo & 0xFFFFFFFC; // i/o bar

    uint64_t base = bar_lo & 0xFFFFFFF0;
    uint8_t type = (bar_lo >> 1) & 0x03;
    if (type == 0x02) { // 64-bit bar
        uint32_t bar_hi = PciRead(bus, dev, func, offset + 4);
        base |= ((uint64_t)bar_hi << 32);
    }
    return base;
}

uint64_t GpuProbe::GetBARSize(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx) {
    uint8_t offset = 0x10 + bar_idx * 4;
    uint32_t original = PciRead(bus, dev, func, offset);
    PciWrite(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t mask = PciRead(bus, dev, func, offset);
    PciWrite(bus, dev, func, offset, original);
    if (!(original & 1)) { // memory bar
        mask &= 0xFFFFFFF0;
        if (mask == 0) return 0;
        return ((uint64_t)(~mask)) + 1;
    }
    return 0;
}

//  intel igpu mmio register access

uint32_t GpuProbe::IntelReadMMIO(uint64_t bar0, uint32_t offset) {
    if (!bar0) return 0;
    volatile uint32_t* reg = (volatile uint32_t*)((uintptr_t)bar0 + offset);
    return *reg;
}

// read the active display surface address from intel igpu.
// tries pipe a first, then pipe b. returns 0 if no active pipe found.
uintptr_t GpuProbe::IntelGetActiveSurface(uint64_t bar0) {
    if (!bar0) return 0;

    // check pipe a  -  dspcntr_a bit 31 = plane enabled
    uint32_t dspcntr_a = IntelReadMMIO(bar0, INTEL_DSPCNTR_A);
    if (dspcntr_a & (1u << 31)) {
        uint32_t surf_a = IntelReadMMIO(bar0, INTEL_DSPSURF_A);
        // surface address is bits 31:12 (4kb aligned)
        uintptr_t fb = (uintptr_t)(surf_a & 0xFFFFF000);
        if (fb != 0) {
            SerialLogger::Log("[GpuProbe] Intel Pipe A active, DSPSURF=0x");
            SerialLogger::LogHex(surf_a);
            SerialLogger::Log("\r\n");
            return fb;
        }
    }

    // check pipe b
    uint32_t dspcntr_b = IntelReadMMIO(bar0, INTEL_DSPCNTR_B);
    if (dspcntr_b & (1u << 31)) {
        uint32_t surf_b = IntelReadMMIO(bar0, INTEL_DSPSURF_B);
        uintptr_t fb = (uintptr_t)(surf_b & 0xFFFFF000);
        if (fb != 0) {
            SerialLogger::Log("[GpuProbe] Intel Pipe B active, DSPSURF=0x");
            SerialLogger::LogHex(surf_b);
            SerialLogger::Log("\r\n");
            return fb;
        }
    }

    // on gen9+ / gen12+, pipe a plane registers are at the same offsets
    // but dspsurf might read as 0 if the gpu uses a different plane config.
    // in that case, fall back to the uefi gop address (which is usually correct).
    SerialLogger::Log("[GpuProbe] Intel: No active display plane found via DSPSURF\r\n");
    return 0;
}

//  intel igpu identification (by device id ranges)

const char* GpuProbe::IntelIdentify(uint16_t did) {
    // gen12+ (alder lake, raptor lake, meteor lake  -  12th/13th/14th gen)
    if ((did >= 0x4680 && did <= 0x46FF) || did == 0x4626 || did == 0x4628 ||
        did == 0x462A || did == 0x46A6 || did == 0x46A8 || did == 0x46AA ||
        did == 0x46B0 || did == 0x46B1 || did == 0x46B3 || did == 0x46C0 ||
        did == 0x46C1 || did == 0x46C3)
        return "Intel UHD/Iris Xe (Alder Lake)";

    if ((did >= 0xA780 && did <= 0xA7FF) || did == 0xA720 || did == 0xA721 ||
        did == 0xA7A0 || did == 0xA7A1 || did == 0xA7A8 || did == 0xA7A9 ||
        did == 0xA7AC || did == 0xA7AD)
        return "Intel UHD (Raptor Lake)";

    if (did >= 0x7D00 && did <= 0x7DFF)
        return "Intel Xe (Meteor Lake)";

    // arrow lake / lunar lake (gen 12.7+)
    if (did >= 0xE200 && did <= 0xE2FF)
        return "Intel Xe2 (Arrow/Lunar Lake)";

    // tiger lake (11th gen)
    if ((did >= 0x9A40 && did <= 0x9AFF) || did == 0x9A49 || did == 0x9A78)
        return "Intel Iris Xe (Tiger Lake)";

    // ice lake (10th gen)
    if (did >= 0x8A50 && did <= 0x8AFF)
        return "Intel Iris Plus (Ice Lake)";

    // comet lake / rocket lake (10th/11th gen desktop)
    if ((did >= 0x9B00 && did <= 0x9BFF) || (did >= 0x4C00 && did <= 0x4CFF))
        return "Intel UHD (Comet/Rocket Lake)";

    // coffee lake / whiskey lake (8th/9th gen)
    if (did >= 0x3E00 && did <= 0x3EFF)
        return "Intel UHD 630 (Coffee Lake)";

    // kaby lake (7th gen)
    if (did >= 0x5900 && did <= 0x59FF)
        return "Intel HD 630 (Kaby Lake)";

    // skylake (6th gen)
    if (did >= 0x1900 && did <= 0x19FF)
        return "Intel HD 530 (Skylake)";

    // broadwell (5th gen)
    if (did >= 0x1600 && did <= 0x16FF)
        return "Intel HD 5500 (Broadwell)";

    // haswell (4th gen)
    if (did >= 0x0400 && did <= 0x04FF)
        return "Intel HD 4600 (Haswell)";

    // ivy bridge (3rd gen)
    if (did >= 0x0150 && did <= 0x017F)
        return "Intel HD 4000 (Ivy Bridge)";

    // sandy bridge (2nd gen)
    if (did >= 0x0100 && did <= 0x012F)
        return "Intel HD 3000 (Sandy Bridge)";

    return "Intel Graphics";
}

//  vendor name helper

const char* GpuProbe::VendorName(uint16_t vid) {
    switch (vid) {
        case GPU_VENDOR_INTEL:  return "Intel";
        case GPU_VENDOR_NVIDIA: return "NVIDIA";
        case GPU_VENDOR_AMD:    return "AMD";
        case GPU_VENDOR_VMWARE: return "VMware";
        case GPU_VENDOR_QEMU:   return "QEMU/Bochs";
        case GPU_VENDOR_VIRTIO: return "VirtIO";
        case GPU_VENDOR_REDHAT: return "Red Hat";
        default:                return "Unknown";
    }
}

//  string helpers (no libc)

static void _scpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void _scat(char* d, const char* s, int max) {
    int i = 0;
    while (d[i] && i < max - 1) i++;
    while (*s && i < max - 1) { d[i++] = *s++; }
    d[i] = 0;
}

//  full pci bus scan  -  find all display controllers

void GpuProbe::ScanAll() {
    // clear previous results
    for (int i = 0; i < GPU_PROBE_MAX; i++) {
        result.gpus[i] = {};
    }
    result.count = 0;
    result.primary_idx = -1;
    result.topology = GPU_TOPO_SINGLE;
    result.fb_validated = false;
    result.validated_fb_addr = 0;

    SerialLogger::Log("[GpuProbe] Scanning PCI bus for display controllers...\r\n");

    for (int bus = 0; bus < 256 && result.count < GPU_PROBE_MAX; bus++) {
        for (int dev = 0; dev < 32 && result.count < GPU_PROBE_MAX; dev++) {
            for (int func = 0; func < 8 && result.count < GPU_PROBE_MAX; func++) {
                uint32_t id = PciRead(bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF || id == 0) {
                    // if function 0 doesn't exist, no other functions will either
                    if (func == 0) break;
                    continue;
                }

                uint16_t vendor = id & 0xFFFF;
                uint16_t device = (id >> 16) & 0xFFFF;

                // read class code (offset 0x08): [31:24]=base, [23:16]=sub, [15:8]=progif
                uint32_t class_reg = PciRead(bus, dev, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8)  & 0xFF;

                // only interested in display controllers (class 0x03)
                if (base_class != 0x03) {
                    // check multi-function
                    if (func == 0) {
                        uint32_t hdr = PciRead(bus, dev, 0, 0x0C);
                        if (!((hdr >> 16) & 0x80)) break;
                    }
                    continue;
                }

                GpuInfo& gpu = result.gpus[result.count];
                gpu.present = true;
                gpu.vendor_id = vendor;
                gpu.device_id = device;
                gpu.bus = (uint8_t)bus;
                gpu.device = (uint8_t)dev;
                gpu.function = (uint8_t)func;
                gpu.base_class = base_class;
                gpu.sub_class = sub_class;
                gpu.prog_if = prog_if;
                gpu.is_vga = (sub_class == 0x00);
                gpu.role = GPU_ROLE_UNKNOWN;

                // read bars
                gpu.bar0 = ReadBAR(bus, dev, func, 0);
                gpu.bar0_size = GetBARSize(bus, dev, func, 0);
                gpu.bar2 = ReadBAR(bus, dev, func, 2);
                gpu.bar2_size = GetBARSize(bus, dev, func, 2);

                // classify vendor and build description
                switch (vendor) {
                    case GPU_VENDOR_INTEL:
                        gpu.is_igpu = true;
                        _scpy(gpu.desc, IntelIdentify(device), sizeof(gpu.desc));
                        break;
                    case GPU_VENDOR_NVIDIA:
                        gpu.is_igpu = false;
                        _scpy(gpu.desc, "NVIDIA GPU 0x", sizeof(gpu.desc));
                        // append device id hex
                        {
                            const char* hex = "0123456789ABCDEF";
                            char hex_buf[5];
                            hex_buf[0] = hex[(device >> 12) & 0xF];
                            hex_buf[1] = hex[(device >> 8) & 0xF];
                            hex_buf[2] = hex[(device >> 4) & 0xF];
                            hex_buf[3] = hex[device & 0xF];
                            hex_buf[4] = 0;
                            _scat(gpu.desc, hex_buf, sizeof(gpu.desc));
                        }
                        break;
                    case GPU_VENDOR_AMD:
                        // amd apu igpus are class 0x0300, discrete are often 0x0300 too
                        // but apu device ids are in specific ranges
                        gpu.is_igpu = (sub_class == 0x00 && 
                                       ((device >= 0x1636 && device <= 0x16FF) || // renoir/cezanne/rembrandt apu
                                        (device >= 0x15D8 && device <= 0x15FF) || // picasso/raven ridge
                                        (device >= 0x1681 && device <= 0x16FF) || // rembrandt
                                        (device >= 0x164E && device <= 0x1681) || // raphael/phoenix
                                        (device >= 0x7640 && device <= 0x7680))); // phoenix/hawk point
                        _scpy(gpu.desc, gpu.is_igpu ? "AMD APU iGPU" : "AMD Radeon dGPU", sizeof(gpu.desc));
                        break;
                    case GPU_VENDOR_VMWARE:
                        gpu.is_igpu = false;
                        _scpy(gpu.desc, "VMware SVGA", sizeof(gpu.desc));
                        gpu.role = GPU_ROLE_VIRTUAL;
                        break;
                    case GPU_VENDOR_QEMU:
                        gpu.is_igpu = false;
                        _scpy(gpu.desc, "QEMU/Bochs VGA", sizeof(gpu.desc));
                        gpu.role = GPU_ROLE_VIRTUAL;
                        break;
                    case GPU_VENDOR_VIRTIO:
                        gpu.is_igpu = false;
                        _scpy(gpu.desc, "VirtIO GPU", sizeof(gpu.desc));
                        gpu.role = GPU_ROLE_VIRTUAL;
                        break;
                    case GPU_VENDOR_REDHAT:
                        gpu.is_igpu = false;
                        _scpy(gpu.desc, "Red Hat QXL", sizeof(gpu.desc));
                        gpu.role = GPU_ROLE_VIRTUAL;
                        break;
                    default:
                        gpu.is_igpu = false;
                        _scpy(gpu.desc, "Unknown GPU", sizeof(gpu.desc));
                        break;
                }

                SerialLogger::Log("[GpuProbe] Found: ");
                SerialLogger::Log(gpu.desc);
                SerialLogger::Log(" @ PCI ");
                SerialLogger::LogDec(bus);
                SerialLogger::Log(":");
                SerialLogger::LogDec(dev);
                SerialLogger::Log(".");
                SerialLogger::LogDec(func);
                SerialLogger::Log(" class=0x");
                SerialLogger::LogHex(((uint32_t)base_class << 8) | sub_class);
                SerialLogger::Log(" VGA=");
                SerialLogger::Log(gpu.is_vga ? "yes" : "no");
                SerialLogger::Log(" BAR0=0x");
                SerialLogger::LogHex((uint32_t)(gpu.bar0 >> 32));
                SerialLogger::LogHex((uint32_t)(gpu.bar0 & 0xFFFFFFFF));
                SerialLogger::Log(" BAR2=0x");
                SerialLogger::LogHex((uint32_t)(gpu.bar2 >> 32));
                SerialLogger::LogHex((uint32_t)(gpu.bar2 & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");

                result.count++;

                // check multi-function
                if (func == 0) {
                    uint32_t hdr = PciRead(bus, dev, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }

    SerialLogger::Log("[GpuProbe] Total display controllers found: ");
    SerialLogger::LogDec(result.count);
    SerialLogger::Log("\r\n");

    // classify the topology and assign roles
    ClassifyTopology();
    AssignRoles();
}

//  topology classification

void GpuProbe::ClassifyTopology() {
    bool has_intel = false, has_nvidia = false, has_amd_igpu = false, has_amd_dgpu = false;
    bool has_virtual = false;
    int vga_count = 0;

    for (int i = 0; i < result.count; i++) {
        const GpuInfo& g = result.gpus[i];
        if (!g.present) continue;

        if (g.vendor_id == GPU_VENDOR_INTEL) has_intel = true;
        if (g.vendor_id == GPU_VENDOR_NVIDIA) has_nvidia = true;
        if (g.vendor_id == GPU_VENDOR_AMD && g.is_igpu) has_amd_igpu = true;
        if (g.vendor_id == GPU_VENDOR_AMD && !g.is_igpu) has_amd_dgpu = true;
        if (g.role == GPU_ROLE_VIRTUAL) has_virtual = true;
        if (g.is_vga) vga_count++;
    }

    if (has_virtual && result.count == 1) {
        result.topology = GPU_TOPO_VIRTUAL;
    } else if (has_intel && has_nvidia) {
        // intel igpu + nvidia dgpu → optimus
        // on muxless: nvidia is class 0x0302 (3d controller, not vga)
        //   → panel wired to intel only
        // on muxed: both are class 0x0300 (vga compatible)
        //   → mux can route to either
        bool nvidia_is_vga = false;
        for (int i = 0; i < result.count; i++) {
            if (result.gpus[i].vendor_id == GPU_VENDOR_NVIDIA && result.gpus[i].is_vga)
                nvidia_is_vga = true;
        }
        result.topology = nvidia_is_vga ? GPU_TOPO_OPTIMUS_MUX : GPU_TOPO_OPTIMUS_MUXLESS;
    } else if (has_amd_igpu && (has_nvidia || has_amd_dgpu)) {
        result.topology = GPU_TOPO_POWERXPRESS;
    } else if (result.count >= 2 && !has_intel && !has_amd_igpu) {
        result.topology = GPU_TOPO_DUAL_DISCRETE;
    } else {
        result.topology = GPU_TOPO_SINGLE;
    }

    const char* topo_names[] = {
        "Single GPU", "Optimus (muxless)", "Optimus (MUX)", 
        "PowerXpress", "Dual Discrete", "Virtual"
    };
    SerialLogger::Log("[GpuProbe] Topology: ");
    SerialLogger::Log(topo_names[(int)result.topology]);
    SerialLogger::Log("\r\n");
}

//  role assignment  -  which gpu drives the panel?

void GpuProbe::AssignRoles() {
    result.primary_idx = -1;

    switch (result.topology) {
        case GPU_TOPO_OPTIMUS_MUXLESS:
        case GPU_TOPO_POWERXPRESS:
            // igpu drives the panel, dgpu is offload-only
            for (int i = 0; i < result.count; i++) {
                if (result.gpus[i].is_igpu) {
                    result.gpus[i].role = GPU_ROLE_PRIMARY;
                    result.primary_idx = i;
                } else if (result.gpus[i].role != GPU_ROLE_VIRTUAL) {
                    result.gpus[i].role = GPU_ROLE_SECONDARY;
                }
            }
            break;

        case GPU_TOPO_OPTIMUS_MUX:
            // with mux: both are vga. the primary is whichever has vga decode
            // enabled (pci command register bit 3) or is on bus 0.
            // uefi typically initializes the igpu for gop.
            for (int i = 0; i < result.count; i++) {
                if (result.gpus[i].is_igpu) {
                    result.gpus[i].role = GPU_ROLE_PRIMARY;
                    result.primary_idx = i;
                } else {
                    result.gpus[i].role = GPU_ROLE_SECONDARY;
                }
            }
            break;

        case GPU_TOPO_VIRTUAL:
        case GPU_TOPO_SINGLE:
            // first (only) gpu is primary
            for (int i = 0; i < result.count; i++) {
                if (result.gpus[i].present) {
                    result.gpus[i].role = GPU_ROLE_PRIMARY;
                    result.primary_idx = i;
                    break;
                }
            }
            break;

        case GPU_TOPO_DUAL_DISCRETE:
            // first vga-class device is primary
            for (int i = 0; i < result.count; i++) {
                if (result.gpus[i].is_vga) {
                    result.gpus[i].role = GPU_ROLE_PRIMARY;
                    result.primary_idx = i;
                } else {
                    result.gpus[i].role = GPU_ROLE_SECONDARY;
                }
            }
            // if no vga device found, use first gpu
            if (result.primary_idx < 0 && result.count > 0) {
                result.gpus[0].role = GPU_ROLE_PRIMARY;
                result.primary_idx = 0;
            }
            break;
    }

    if (result.primary_idx >= 0) {
        SerialLogger::Log("[GpuProbe] Primary GPU: ");
        SerialLogger::Log(result.gpus[result.primary_idx].desc);
        SerialLogger::Log("\r\n");
    } else {
        SerialLogger::Log("[GpuProbe] WARNING: No primary GPU identified!\r\n");
    }
}

//  framebuffer address validation
//  compares multiboot fb address against what the gpu hardware reports.
//  on optimus laptops, reads intel igpu's dspsurf register to cross-check.

uintptr_t GpuProbe::ValidateFramebuffer(uintptr_t mb_fb_addr, uint32_t width,
                                         uint32_t height, uint32_t pitch, uint8_t bpp) {
    result.fb_validated = true;
    result.validated_fb_addr = mb_fb_addr;

    if (result.primary_idx < 0) {
        SerialLogger::Log("[GpuProbe] Cannot validate FB  -  no primary GPU\r\n");
        return mb_fb_addr;
    }

    const GpuInfo& primary = result.gpus[result.primary_idx];

    SerialLogger::Log("[GpuProbe] Validating FB addr=0x");
    SerialLogger::LogHex((uint32_t)(mb_fb_addr >> 32));
    SerialLogger::LogHex((uint32_t)(mb_fb_addr & 0xFFFFFFFF));
    SerialLogger::Log(" against ");
    SerialLogger::Log(primary.desc);
    SerialLogger::Log("\r\n");

    if (primary.vendor_id == GPU_VENDOR_INTEL && primary.bar0 != 0) {
        SerialLogger::Log("[GpuProbe] Reading Intel iGPU display registers...\r\n");

        // read actual display surface address from mmio
        uintptr_t hw_surface = IntelGetActiveSurface(primary.bar0);

        if (hw_surface != 0) {
            // dspsurf contains an offset into the stolen memory region (gmadr / bar2).
            // on most intel gpus, the uefi gop framebuffer is at:
            //   bar2 (gmadr) + dspsurf offset
            // but dspsurf itself may already be an absolute physical address
            // depending on how uefi set it up.

            // check if dspsurf is within the gmadr aperture
            uintptr_t gmadr = (uintptr_t)primary.bar2;
            uint64_t  gmadr_size = primary.bar2_size;

            SerialLogger::Log("[GpuProbe] Intel GMADR (BAR2)=0x");
            SerialLogger::LogHex((uint32_t)(gmadr >> 32));
            SerialLogger::LogHex((uint32_t)(gmadr & 0xFFFFFFFF));
            SerialLogger::Log(" size=0x");
            SerialLogger::LogHex((uint32_t)gmadr_size);
            SerialLogger::Log("\r\n");

            // case 1: dspsurf is an offset relative to gmadr
            // (common on older gen4-gen8)
            uintptr_t fb_from_gmadr = gmadr + hw_surface;

            // case 2: dspsurf is already an absolute graphics address
            // (common on gen9+ where ggtt maps stolen to a phys region)
            uintptr_t fb_absolute = hw_surface;

            // the multiboot fb should match one of these
            bool match_gmadr = (mb_fb_addr == fb_from_gmadr);
            bool match_abs   = (mb_fb_addr == fb_absolute);
            bool match_direct = (mb_fb_addr == hw_surface);

            // also check if mb fb falls within gmadr aperture range
            // (uefi gop often places fb inside the stolen memory aperture)
            bool in_gmadr = (gmadr != 0 && gmadr_size != 0 &&
                             mb_fb_addr >= gmadr && 
                             mb_fb_addr < gmadr + gmadr_size);

            if (match_gmadr || match_abs || match_direct || in_gmadr) {
                SerialLogger::Log("[GpuProbe] FB address MATCHES Intel iGPU  -  OK\r\n");
                // all good  -  multiboot fb points to the correct gpu
            } else {
                // fb address mismatch! this is likely the optimus black screen cause.
                SerialLogger::Log("[GpuProbe] WARNING: FB address MISMATCH!\r\n");
                SerialLogger::Log("[GpuProbe]   Multiboot FB = 0x");
                SerialLogger::LogHex((uint32_t)(mb_fb_addr >> 32));
                SerialLogger::LogHex((uint32_t)(mb_fb_addr & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");
                SerialLogger::Log("[GpuProbe]   DSPSURF      = 0x");
                SerialLogger::LogHex((uint32_t)(hw_surface >> 32));
                SerialLogger::LogHex((uint32_t)(hw_surface & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");
                SerialLogger::Log("[GpuProbe]   GMADR+SURF   = 0x");
                SerialLogger::LogHex((uint32_t)(fb_from_gmadr >> 32));
                SerialLogger::LogHex((uint32_t)(fb_from_gmadr & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");

                // try the alternatives in priority order:
                // 1. if gmadr+surf is a valid physical address, use it
                if (gmadr != 0 && fb_from_gmadr != 0 && fb_from_gmadr != mb_fb_addr) {
                    SerialLogger::Log("[GpuProbe] → Trying GMADR + DSPSURF offset\r\n");
                    result.validated_fb_addr = fb_from_gmadr;
                }
                // 2. if dspsurf is already a reasonable physical address
                else if (hw_surface >= 0x80000000 && hw_surface != mb_fb_addr) {
                    SerialLogger::Log("[GpuProbe] → Trying DSPSURF as absolute address\r\n");
                    result.validated_fb_addr = hw_surface;
                }
                // 3. fall back to multiboot address (might still work)
                else {
                    SerialLogger::Log("[GpuProbe] → Keeping Multiboot FB (no better alternative)\r\n");
                    result.validated_fb_addr = mb_fb_addr;
                }
            }
        } else {
            // couldn't read dspsurf  -  trust multiboot
            SerialLogger::Log("[GpuProbe] Intel DSPSURF read returned 0  -  using Multiboot FB\r\n");
        }

        // additional intel-specific checks:
        // verify the pipe is actually running (pipeconf enabled)
        uint32_t pipeconf = IntelReadMMIO(primary.bar0, INTEL_PIPECONF_A);
        SerialLogger::Log("[GpuProbe] Intel PIPECONF_A = 0x");
        SerialLogger::LogHex(pipeconf);
        SerialLogger::Log(pipeconf & (1u << 31) ? " [ENABLED]" : " [DISABLED]");
        SerialLogger::Log("\r\n");

        // read plane stride for debugging
        uint32_t stride = IntelReadMMIO(primary.bar0, INTEL_DSPSTRIDE_A);
        SerialLogger::Log("[GpuProbe] Intel DSPSTRIDE_A = ");
        SerialLogger::LogDec(stride);
        SerialLogger::Log(" bytes (expected ");
        SerialLogger::LogDec(pitch);
        SerialLogger::Log(")\r\n");
    }
    else if (primary.vendor_id == GPU_VENDOR_NVIDIA) {
        // on nvidia-primary systems, the fb should be in bar1 (vram aperture)
        // or in stolen system memory provided by uefi gop
        if (primary.bar2 != 0) {
            uintptr_t nv_fb = (uintptr_t)primary.bar2;
            bool in_nv_vram = (mb_fb_addr >= nv_fb && 
                               mb_fb_addr < nv_fb + primary.bar2_size);
            SerialLogger::Log("[GpuProbe] NVIDIA BAR1(fb)=0x");
            SerialLogger::LogHex((uint32_t)(nv_fb >> 32));
            SerialLogger::LogHex((uint32_t)(nv_fb & 0xFFFFFFFF));
            SerialLogger::Log(in_nv_vram ? "  -  FB in VRAM ✓" : "  -  FB NOT in VRAM");
            SerialLogger::Log("\r\n");
        }
    }
    else if (primary.vendor_id == GPU_VENDOR_AMD) {
        if (primary.bar2 != 0) {
            SerialLogger::Log("[GpuProbe] AMD VRAM aperture=0x");
            SerialLogger::LogHex((uint32_t)(primary.bar2 >> 32));
            SerialLogger::LogHex((uint32_t)(primary.bar2 & 0xFFFFFFFF));
            SerialLogger::Log("\r\n");
        }
    }

    // log final decision
    if (result.validated_fb_addr != mb_fb_addr) {
        SerialLogger::Log("[GpuProbe] *** FB ADDRESS CORRECTED: 0x");
        SerialLogger::LogHex((uint32_t)(result.validated_fb_addr >> 32));
        SerialLogger::LogHex((uint32_t)(result.validated_fb_addr & 0xFFFFFFFF));
        SerialLogger::Log(" ***\r\n");
    } else {
        SerialLogger::Log("[GpuProbe] FB address validated OK (no correction needed)\r\n");
    }

    return result.validated_fb_addr;
}

//  logging

void GpuProbe::LogAll() {
    SerialLogger::Log("[GpuProbe] === GPU Inventory ===\r\n");
    for (int i = 0; i < result.count; i++) {
        const GpuInfo& g = result.gpus[i];
        if (!g.present) continue;

        SerialLogger::Log("  [");
        SerialLogger::LogDec(i);
        SerialLogger::Log("] ");

        switch (g.role) {
            case GPU_ROLE_PRIMARY:   SerialLogger::Log("[PRIMARY]   "); break;
            case GPU_ROLE_SECONDARY: SerialLogger::Log("[SECONDARY] "); break;
            case GPU_ROLE_VIRTUAL:   SerialLogger::Log("[VIRTUAL]   "); break;
            default:                 SerialLogger::Log("[UNKNOWN]   "); break;
        }

        SerialLogger::Log(g.desc);
        SerialLogger::Log("  PCI=");
        SerialLogger::LogDec(g.bus);
        SerialLogger::Log(":");
        SerialLogger::LogDec(g.device);
        SerialLogger::Log(".");
        SerialLogger::LogDec(g.function);
        SerialLogger::Log("  VID=0x");
        SerialLogger::LogHex(g.vendor_id);
        SerialLogger::Log(" DID=0x");
        SerialLogger::LogHex(g.device_id);
        SerialLogger::Log(g.is_vga ? " [VGA]" : " [3D]");
        SerialLogger::Log("\r\n");
    }

    const char* topo_names[] = {
        "Single GPU", "Optimus (muxless)", "Optimus (MUX)", 
        "PowerXpress", "Dual Discrete", "Virtual"
    };
    SerialLogger::Log("  Topology: ");
    SerialLogger::Log(topo_names[(int)result.topology]);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("[GpuProbe] === End ===\r\n");
}

//  query methods

const GpuProbeResult& GpuProbe::GetResult() { return result; }

bool GpuProbe::IsOptimus() {
    return result.topology == GPU_TOPO_OPTIMUS_MUXLESS || 
           result.topology == GPU_TOPO_OPTIMUS_MUX;
}

bool GpuProbe::IsPowerXpress() {
    return result.topology == GPU_TOPO_POWERXPRESS;
}

bool GpuProbe::IsHybrid() {
    return IsOptimus() || IsPowerXpress();
}

bool GpuProbe::HasIntelIGPU() {
    for (int i = 0; i < result.count; i++)
        if (result.gpus[i].vendor_id == GPU_VENDOR_INTEL) return true;
    return false;
}

bool GpuProbe::HasNvidiaGPU() {
    for (int i = 0; i < result.count; i++)
        if (result.gpus[i].vendor_id == GPU_VENDOR_NVIDIA) return true;
    return false;
}

bool GpuProbe::HasAmdGPU() {
    for (int i = 0; i < result.count; i++)
        if (result.gpus[i].vendor_id == GPU_VENDOR_AMD) return true;
    return false;
}

int GpuProbe::GetPrimaryGpuIndex() {
    return result.primary_idx;
}
