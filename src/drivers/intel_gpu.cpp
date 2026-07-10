//  kurono os - intel integrated gpu driver
//  pci scan, bar mapping, display pipe state, generation detection
//  low mmio is identity-mapped by the boot tables, but a 64-bit gttmmaddr bar
//  can sit above that window, so map it explicitly before the first deref
#include "intel_gpu.h"
#include "serial.h"
#include "../kernel/vmm.h"   // map the (possibly high 64-bit) bar before deref (satoru)

// identity-map a bar's register window as uncached mmio before any access.
// the igpu gttmmaddr bar is 64-bit and can land above the boot identity map,
// so a register read would #pf unmapped. mirrors nvme.cpp / virtio_gpu.cpp;
// caps at 16mb (the gen register aperture fits) and is idempotent. (satoru)
static void igpu_map_bar_window(uint64_t base, uint64_t size) {
    if (base == 0) return;
    uint64_t window = size ? size : 0x1000000ULL; // default 16mb if size unknown
    if (window > 0x1000000ULL) window = 0x1000000ULL;
    uint64_t start = base & ~0xFFFULL;
    uint64_t end   = (base + window + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000ULL) {
        KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    }
}

IntelGPUInfo IntelGPU::gpu_info = {};

static inline void _igpu_out32(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %w1" : : "a"(val), "Nd"(port));
}
static inline uint32_t _igpu_in32(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint32_t IntelGPU::PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    _igpu_out32(0xCF8, addr);
    return _igpu_in32(0xCFC);
}

void IntelGPU::PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    _igpu_out32(0xCF8, addr);
    _igpu_out32(0xCFC, val);
}

uint64_t IntelGPU::ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx) {
    if (bar_idx > 5) return 0;
    uint8_t offset = 0x10 + bar_idx * 4;
    uint32_t bar_lo = PciRead(bus, dev, func, offset);
    if (bar_lo == 0 || bar_lo == 0xFFFFFFFF) return 0;
    // I/O BARs are not useful for GPU MMIO - refuse them so callers don't try to dereference them
    if (bar_lo & 1) return 0;
    uint64_t base = bar_lo & 0xFFFFFFF0;
    uint8_t type = (bar_lo >> 1) & 0x03;
    if (type == 0x02 && bar_idx < 5) {
        uint32_t bar_hi = PciRead(bus, dev, func, offset + 4);
        base |= ((uint64_t)bar_hi << 32);
    }
    return base;
}

//  register access (via identity-mapped bar0)

uint32_t IntelGPU::ReadReg(uint32_t offset) {
    if (!gpu_info.bar0) return 0;
    // refuse out-of-bounds reads - protects against driver bugs poking past BAR0
    if (gpu_info.bar0_size && offset + 4 > gpu_info.bar0_size) return 0;
    volatile uint32_t* reg = (volatile uint32_t*)((uintptr_t)gpu_info.bar0 + offset);
    uint32_t v = *reg;
    __asm__ volatile("" ::: "memory");
    return v;
}

void IntelGPU::WriteReg(uint32_t offset, uint32_t value) {
    if (!gpu_info.bar0) return;
    if (gpu_info.bar0_size && offset + 4 > gpu_info.bar0_size) return;
    volatile uint32_t* reg = (volatile uint32_t*)((uintptr_t)gpu_info.bar0 + offset);
    *reg = value;
    __asm__ volatile("sfence" ::: "memory");
}

//  generation classification

IntelGpuGen IntelGPU::ClassifyGen(uint16_t did) {
    // gen 12.7 - meteor lake / arrow lake / lunar lake
    if ((did >= 0x7D40 && did <= 0x7D67) || (did >= 0x7DD0 && did <= 0x7DDF) ||
        (did >= 0xE200 && did <= 0xE2FF) || (did >= 0x6480 && did <= 0x64FF))
        return INTEL_GEN_12_7;

    // gen 12 - tiger lake / alder lake / raptor lake
    if ((did >= 0x9A40 && did <= 0x9AF8) || (did >= 0x4680 && did <= 0x46D2) ||
        (did >= 0xA780 && did <= 0xA7AF) || (did >= 0x5690 && did <= 0x56C1))
        return INTEL_GEN_12;

    // gen 11 - ice lake
    if (did >= 0x8A50 && did <= 0x8A71)
        return INTEL_GEN_11;

    // gen 9 / 9.5 - skylake / kaby lake / coffee lake / comet lake
    if ((did >= 0x1900 && did <= 0x193D) || (did >= 0x5900 && did <= 0x593D) ||
        (did >= 0x3E90 && did <= 0x3EA8) || (did >= 0x9B00 && did <= 0x9BF6) ||
        (did >= 0x4E51 && did <= 0x4E90))
        return INTEL_GEN_9;

    // gen 8 - broadwell
    if (did >= 0x1600 && did <= 0x163D)
        return INTEL_GEN_8;

    // gen 7 / 7.5 - ivy bridge / haswell
    if ((did >= 0x0150 && did <= 0x016A) || (did >= 0x0400 && did <= 0x0426) ||
        (did >= 0x0A00 && did <= 0x0A2E) || (did >= 0x0D00 && did <= 0x0D36))
        return INTEL_GEN_7;

    // gen 6 - sandy bridge
    if (did >= 0x0100 && did <= 0x012B)
        return INTEL_GEN_6;

    return INTEL_GEN_UNKNOWN;
}

const char* IntelGPU::IdentifyDevice(uint16_t did) {
    // arrow lake / lunar lake
    if (did == 0x7D55 || did == 0x7D60) return "Intel Arc (Arrow Lake)";
    if (did == 0x6480 || did == 0x64A0) return "Intel Arc (Lunar Lake)";

    // meteor lake
    if (did >= 0x7D40 && did <= 0x7D67) return "Intel Arc (Meteor Lake)";

    // raptor lake
    if (did == 0xA780) return "Intel UHD 770 (Raptor Lake)";
    if (did >= 0xA780 && did <= 0xA7AF) return "Intel UHD (Raptor Lake)";

    // alder lake
    if (did == 0x4680) return "Intel UHD 770 (Alder Lake)";
    if (did == 0x46A6) return "Intel Arc A380 (Alder Lake)";
    if (did >= 0x4680 && did <= 0x46D2) return "Intel UHD (Alder Lake)";

    // tiger lake
    if (did == 0x9A49) return "Intel Iris Xe (Tiger Lake)";
    if (did >= 0x9A40 && did <= 0x9AF8) return "Intel Iris Xe (Tiger Lake)";

    // dg1 / dg2 (arc)
    if (did >= 0x5690 && did <= 0x56C1) return "Intel Arc A-Series (DG2)";

    // ice lake
    if (did == 0x8A52) return "Intel Iris Plus G7 (Ice Lake)";
    if (did >= 0x8A50 && did <= 0x8A71) return "Intel Iris Plus (Ice Lake)";

    // coffee lake
    if (did == 0x3E92) return "Intel UHD 630 (Coffee Lake)";
    if (did == 0x3E91) return "Intel UHD 630 (Coffee Lake)";
    if (did == 0x3E98) return "Intel UHD 630 (Coffee Lake)";
    if (did >= 0x3E90 && did <= 0x3EA8) return "Intel UHD (Coffee Lake)";

    // comet lake
    if (did == 0x9B21) return "Intel UHD 630 (Comet Lake)";
    if (did == 0x9BC4) return "Intel UHD (Comet Lake)";
    if (did >= 0x9B00 && did <= 0x9BF6) return "Intel UHD (Comet Lake)";

    // kaby lake
    if (did == 0x5912) return "Intel HD 630 (Kaby Lake)";
    if (did == 0x5916) return "Intel HD 620 (Kaby Lake)";
    if (did >= 0x5900 && did <= 0x593D) return "Intel HD (Kaby Lake)";

    // skylake
    if (did == 0x1912) return "Intel HD 530 (Skylake)";
    if (did == 0x1916) return "Intel HD 520 (Skylake)";
    if (did >= 0x1900 && did <= 0x193D) return "Intel HD (Skylake)";

    // broadwell
    if (did == 0x1616) return "Intel HD 5500 (Broadwell)";
    if (did >= 0x1600 && did <= 0x163D) return "Intel HD (Broadwell)";

    // haswell
    if (did == 0x0412) return "Intel HD 4600 (Haswell)";
    if (did == 0x0416) return "Intel HD 4600 (Haswell)";
    if (did >= 0x0400 && did <= 0x0426) return "Intel HD (Haswell)";

    // ivy bridge
    if (did == 0x0166) return "Intel HD 4000 (Ivy Bridge)";
    if (did == 0x0162) return "Intel HD 2500 (Ivy Bridge)";
    if (did >= 0x0150 && did <= 0x016A) return "Intel HD (Ivy Bridge)";

    // sandy bridge
    if (did == 0x0116) return "Intel HD 3000 (Sandy Bridge)";
    if (did == 0x0112) return "Intel HD 2000 (Sandy Bridge)";
    if (did >= 0x0100 && did <= 0x012B) return "Intel HD (Sandy Bridge)";

    return "Intel HD Graphics";
}

const char* IntelGPU::GetGenName() {
    switch (gpu_info.gen) {
        case INTEL_GEN_6:    return "Gen 6 (Sandy Bridge)";
        case INTEL_GEN_7:    return "Gen 7 (Ivy/Haswell)";
        case INTEL_GEN_8:    return "Gen 8 (Broadwell)";
        case INTEL_GEN_9:    return "Gen 9 (Skylake+)";
        case INTEL_GEN_11:   return "Gen 11 (Ice Lake)";
        case INTEL_GEN_12:   return "Gen 12 (Xe)";
        case INTEL_GEN_12_7: return "Gen 12.7 (Xe-LPG)";
        default:             return "Unknown";
    }
}

//  init: pci scan for intel igpu

void IntelGPU::Init() {
    gpu_info = {};

    SerialLogger::Log("[IntelGPU] Scanning PCI for Intel iGPU...\r\n");

    // scan pci bus 0 (igpu is almost always at 00:02.0)
    for (int bus = 0; bus < 2; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = PciRead(bus, dev, func, 0x00);
                if (id == 0xFFFFFFFF || id == 0) continue;

                uint16_t vid = id & 0xFFFF;
                uint16_t did = (id >> 16) & 0xFFFF;
                if (vid != INTEL_GPU_VENDOR_ID) continue;

                // check class = display controller (03xx)
                uint32_t class_rev = PciRead(bus, dev, func, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                if (base_class != 0x03) continue;

                // found intel gpu
                gpu_info.detected = true;
                gpu_info.vendor_id = vid;
                gpu_info.device_id = did;
                gpu_info.bus = bus;
                gpu_info.device = dev;
                gpu_info.function = func;
                gpu_info.revision = class_rev & 0xFF;
                gpu_info.gen = ClassifyGen(did);

                // read bars
                gpu_info.bar0 = ReadBAR(bus, dev, func, 0);  // gttmmaddr
                gpu_info.bar2 = ReadBAR(bus, dev, func, 2);  // gmadr

                // discover BAR0 size so register access can bounds-check
                {
                    uint32_t orig = PciRead(bus, dev, func, 0x10);
                    PciWrite(bus, dev, func, 0x10, 0xFFFFFFFF);
                    uint32_t mask = PciRead(bus, dev, func, 0x10) & 0xFFFFFFF0;
                    PciWrite(bus, dev, func, 0x10, orig);
                    if (mask) gpu_info.bar0_size = (uint64_t)(~mask) + 1;
                }

                // map the bar window before any register read (ReadPipeState
                // below) - a high 64-bit bar isn't covered by the boot identity
                // map and would #pf otherwise. (satoru)
                igpu_map_bar_window(gpu_info.bar0, gpu_info.bar0_size);

                // enable memory space + bus mastering (igpu needs master for blitter DMA)
                uint32_t cmd = PciRead(bus, dev, func, 0x04);
                uint32_t want = cmd | 0x02 | 0x04;
                if (want != cmd) PciWrite(bus, dev, func, 0x04, want);

                // accel capability: gen6+ has a working blitter; gen8+ has full xe-style 3D
                gpu_info.has_2d_accel = (gpu_info.gen >= INTEL_GEN_6);
                gpu_info.has_3d_accel = (gpu_info.gen >= INTEL_GEN_8);

                // get device name
                const char* name = IdentifyDevice(did);
                int i = 0;
                while (name[i] && i < 63) { gpu_info.name[i] = name[i]; i++; }
                gpu_info.name[i] = 0;

                // read pipe states if bar0 is valid
                if (gpu_info.bar0) {
                    ReadPipeState(0, &gpu_info.pipe_a);
                    ReadPipeState(1, &gpu_info.pipe_b);
                }

                SerialLogger::Log("[IntelGPU] Found: ");
                SerialLogger::Log(gpu_info.name);
                SerialLogger::Log(" [");
                SerialLogger::LogHex(did);
                SerialLogger::Log("] ");
                SerialLogger::Log(GetGenName());
                SerialLogger::Log("\r\n");
                SerialLogger::Log("[IntelGPU]   BAR0=0x");
                SerialLogger::LogHex((uint32_t)(gpu_info.bar0 >> 32));
                SerialLogger::LogHex((uint32_t)(gpu_info.bar0 & 0xFFFFFFFF));
                SerialLogger::Log("  BAR2=0x");
                SerialLogger::LogHex((uint32_t)(gpu_info.bar2 >> 32));
                SerialLogger::LogHex((uint32_t)(gpu_info.bar2 & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");

                if (gpu_info.pipe_a.enabled) {
                    SerialLogger::Log("[IntelGPU]   Pipe A: ");
                    SerialLogger::LogDec(gpu_info.pipe_a.width);
                    SerialLogger::Log("x");
                    SerialLogger::LogDec(gpu_info.pipe_a.height);
                    SerialLogger::Log(" surface=0x");
                    SerialLogger::LogHex((uint32_t)(gpu_info.pipe_a.surface_addr));
                    SerialLogger::Log("\r\n");
                }
                if (gpu_info.pipe_b.enabled) {
                    SerialLogger::Log("[IntelGPU]   Pipe B: ");
                    SerialLogger::LogDec(gpu_info.pipe_b.width);
                    SerialLogger::Log("x");
                    SerialLogger::LogDec(gpu_info.pipe_b.height);
                    SerialLogger::Log("\r\n");
                }

                return; // only one intel igpu expected
            }
        }
    }

    SerialLogger::Log("[IntelGPU] No Intel iGPU found\r\n");
}

bool IntelGPU::IsDetected() { return gpu_info.detected; }
const IntelGPUInfo& IntelGPU::GetInfo() { return gpu_info; }

//  display pipe state

// intel display pipe register offsets
#define _DSPCNTR(pipe)   (0x70180 + (pipe) * 0x1000)
#define _DSPSTRIDE(pipe) (0x70188 + (pipe) * 0x1000)
#define _DSPSURF(pipe)   (0x7019C + (pipe) * 0x1000)
#define _DSPSIZE(pipe)   (0x70190 + (pipe) * 0x1000)
#define _PIPECONF(pipe)  (0x70008 + (pipe) * 0x1000)

bool IntelGPU::ReadPipeState(int pipe_idx, IntelDisplayPipe* out) {
    if (!out || !gpu_info.bar0 || pipe_idx < 0 || pipe_idx > 1) return false;

    *out = {};
    uint32_t dspcntr = ReadReg(_DSPCNTR(pipe_idx));
    out->enabled = (dspcntr & (1u << 31)) != 0;

    if (out->enabled) {
        uint32_t surf = ReadReg(_DSPSURF(pipe_idx));
        out->surface_addr = (uintptr_t)(surf & 0xFFFFF000);
        out->stride = ReadReg(_DSPSTRIDE(pipe_idx));

        // dspsize: height-1 in bits 31:16, width-1 in bits 15:0
        uint32_t size_reg = ReadReg(_DSPSIZE(pipe_idx));
        out->height = ((size_reg >> 16) & 0xFFFF) + 1;
        out->width  = (size_reg & 0xFFFF) + 1;
    }

    return out->enabled;
}

uintptr_t IntelGPU::GetActiveSurfaceAddr() {
    if (gpu_info.pipe_a.enabled) return gpu_info.pipe_a.surface_addr;
    if (gpu_info.pipe_b.enabled) return gpu_info.pipe_b.surface_addr;
    return 0;
}

bool IntelGPU::IsPowerWellEnabled() {
    if (!gpu_info.bar0) return false;
    uint32_t ctl = ReadReg(0x45400);  // pwr_well_ctl
    return (ctl & 0x02) != 0;  // bit 1 = power well state
}

bool IntelGPU::HasHardwareAccel() {
    return gpu_info.detected && gpu_info.bar0 != 0 && gpu_info.has_2d_accel;
}

bool IntelGPU::BlitFillARGB(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    // submitting a blitter command stream from a kernel without a ring buffer
    // setup is unsafe - fall back to a CPU fill through the gmadr aperture
    // when one is exposed. callers detect "no accel" via HasHardwareAccel.
    if (!gpu_info.detected || !gpu_info.bar2 || !gpu_info.pipe_a.enabled) return false;
    uint32_t pw = gpu_info.pipe_a.width;
    uint32_t ph = gpu_info.pipe_a.height;
    if (x >= pw || y >= ph) return false;
    if (x + w > pw) w = pw - x;
    if (y + h > ph) h = ph - y;
    uint32_t stride_px = gpu_info.pipe_a.stride ? (gpu_info.pipe_a.stride / 4) : pw;
    volatile uint32_t* fb = (volatile uint32_t*)(uintptr_t)gpu_info.bar2;
    for (uint32_t row = 0; row < h; row++) {
        volatile uint32_t* line = fb + (y + row) * stride_px + x;
        for (uint32_t col = 0; col < w; col++) line[col] = color;
    }
    return true;
}

//  debug dump

void IntelGPU::DumpInfo(char* out, int maxo) {
    int p = 0;
    auto app = [&](const char* s) { while (*s && p < maxo - 1) out[p++] = *s++; };
    auto app_num = [&](uint32_t v) {
        char b[12]; int i = 0;
        if (v == 0) { b[i++] = '0'; }
        else { char r[12]; int ri = 0; uint32_t t = v;
            while (t) { r[ri++] = '0' + (t % 10); t /= 10; }
            while (ri--) b[i++] = r[ri];
        }
        b[i] = 0; app(b);
    };
    auto app_hex = [&](uint32_t v) {
        const char* hx = "0123456789ABCDEF";
        char b[9]; int i = 0;
        for (int s = 28; s >= 0; s -= 4) b[i++] = hx[(v >> s) & 0xF];
        b[i] = 0; app(b);
    };

    if (!gpu_info.detected) {
        app("Intel iGPU: Not detected\n");
        out[p] = 0; return;
    }

    app("Intel iGPU\n");
    app("  Name: "); app(gpu_info.name); app("\n");
    app("  Gen:  "); app(GetGenName()); app("\n");
    app("  PCI:  "); app_num(gpu_info.bus); app(":");
    app_num(gpu_info.device); app("."); app_num(gpu_info.function); app("\n");
    app("  ID:   0x"); app_hex(gpu_info.device_id); app("\n");
    app("  BAR0: 0x"); app_hex((uint32_t)(gpu_info.bar0 >> 32));
    app_hex((uint32_t)(gpu_info.bar0 & 0xFFFFFFFF)); app(" (MMIO)\n");
    app("  BAR2: 0x"); app_hex((uint32_t)(gpu_info.bar2 >> 32));
    app_hex((uint32_t)(gpu_info.bar2 & 0xFFFFFFFF)); app(" (GMADR)\n");

    if (gpu_info.pipe_a.enabled) {
        app("  Pipe A: "); app_num(gpu_info.pipe_a.width); app("x");
        app_num(gpu_info.pipe_a.height); app(" stride=");
        app_num(gpu_info.pipe_a.stride); app("\n");
    }
    if (gpu_info.pipe_b.enabled) {
        app("  Pipe B: "); app_num(gpu_info.pipe_b.width); app("x");
        app_num(gpu_info.pipe_b.height); app("\n");
    }

    out[p] = 0;
}
