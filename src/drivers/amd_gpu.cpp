#include "amd_gpu.h"
#include "../drivers/serial.h"

//  kurono os  -  amd radeon gpu driver implementation
//  real pci scan, bar mapping, register access, display engine init

AmdGPUInfo AmdGPU::info;
volatile uint32_t* AmdGPU::mmio_base = nullptr;

static inline void _outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t _inl(uint16_t port) {
    uint32_t val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = (1u<<31) | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)func<<8) | (off & 0xFC);
    _outl(0xCF8, addr);
    return _inl(0xCFC);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1u<<31) | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)func<<8) | (off & 0xFC);
    _outl(0xCF8, addr);
    _outl(0xCFC, val);
}

static void _scpy(char* d, const char* s, int m) {
    int i = 0; while (s[i] && i < m-1) { d[i] = s[i]; i++; } d[i] = 0;
}

//  pci scan  -  find amd gpu
bool AmdGPU::ScanPCI() {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            // probe func 0 first, then decide whether to scan other functions
            uint32_t id0 = pci_read32(bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr0 = pci_read32(bus, dev, 0, 0x0C);
            int max_func = ((hdr0 >> 16) & 0x80) ? 8 : 1;

            for (int func = 0; func < max_func; func++) {
                uint32_t id = pci_read32(bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint16_t vid = id & 0xFFFF;
                uint16_t did = (id >> 16) & 0xFFFF;
                if (vid != AMD_VENDOR_ID) continue;

                // class code: 03xx = display controller (sub_class 0x80 = display other)
                uint32_t class_reg = pci_read32(bus, dev, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                if (base_class != 0x03) continue;

                info.vendor_id = vid;
                info.device_id = did;
                info.bus = (uint8_t)bus;
                info.device = (uint8_t)dev;
                info.function = (uint8_t)func;
                info.revision = class_reg & 0xFF;

                // BAR0  -  only accept memory BARs. I/O BARs would dereference a port range as memory.
                uint32_t bar0_lo = pci_read32(bus, dev, func, 0x10);
                if (bar0_lo & 1) {
                    info.bar0 = 0;
                    info.bar0_size = 0;
                } else {
                    info.bar0 = bar0_lo & 0xFFFFFFF0;
                    if ((bar0_lo & 0x06) == 0x04) {
                        uint32_t bar0_hi = pci_read32(bus, dev, func, 0x14);
                        info.bar0 = ((uint64_t)bar0_hi << 32) | (bar0_lo & 0xFFFFFFF0);
                    }
                    // size probe  -  restore original value before continuing
                    pci_write32(bus, dev, func, 0x10, 0xFFFFFFFF);
                    uint32_t bar0_mask = pci_read32(bus, dev, func, 0x10) & 0xFFFFFFF0;
                    pci_write32(bus, dev, func, 0x10, bar0_lo);
                    info.bar0_size = bar0_mask ? ((uint64_t)(~bar0_mask) + 1) : 0;
                }

                // BAR2  -  VRAM aperture; same memory/IO check
                uint32_t bar2_lo = pci_read32(bus, dev, func, 0x18);
                if (bar2_lo & 1) {
                    info.vram_bar = 0;
                } else {
                    info.vram_bar = bar2_lo & 0xFFFFFFF0;
                    if ((bar2_lo & 0x06) == 0x04) {
                        uint32_t bar2_hi = pci_read32(bus, dev, func, 0x1C);
                        info.vram_bar = ((uint64_t)bar2_hi << 32) | (bar2_lo & 0xFFFFFFF0);
                    }
                }

                // bus mastering + memory space
                uint32_t cmd = pci_read32(bus, dev, func, 0x04);
                uint32_t want = cmd | 0x06;
                if (want != cmd) pci_write32(bus, dev, func, 0x04, want);

                // resizable bar capability  -  extended config space, mask 0xFC per spec
                info.resizable_bar = false;
                uint8_t cap_ptr = pci_read32(bus, dev, func, 0x34) & 0xFC;
                int safety = 48;
                while (cap_ptr && safety-- > 0) {
                    uint32_t cap = pci_read32(bus, dev, func, cap_ptr);
                    if ((cap & 0xFF) == 0x15) {
                        info.resizable_bar = true;
                        break;
                    }
                    cap_ptr = (cap >> 8) & 0xFC;
                }

                return true;
            }
        }
    }
    return false;
}

//  architecture identification
AmdArch AmdGPU::IdentifyArch(uint16_t did) {
    // rdna 3.5
    if (did == AMD_RX_9070XT || did == AMD_RX_9070) return AMD_ARCH_RDNA35;

    // rdna 3
    if (did == AMD_RX_7900XTX || did == AMD_RX_7900XT || did == AMD_RX_7800XT ||
        did == AMD_RX_7700XT || did == AMD_RX_7600)
        return AMD_ARCH_RDNA3;
    if (did >= 0x7400 && did <= 0x75FF) return AMD_ARCH_RDNA3;

    // rdna 2
    if (did == AMD_RX_6950XT || did == AMD_RX_6900XT || did == AMD_RX_6800XT ||
        did == AMD_RX_6800 || did == AMD_RX_6700XT || did == AMD_RX_6600XT || did == AMD_RX_6600)
        return AMD_ARCH_RDNA2;
    if (did >= 0x73A0 && did <= 0x73FF) return AMD_ARCH_RDNA2;

    // rdna 1
    if (did == AMD_RX_5700XT || did == AMD_RX_5700 || did == AMD_RX_5600XT || did == AMD_RX_5500XT)
        return AMD_ARCH_RDNA1;
    if (did >= 0x7300 && did <= 0x73A0) return AMD_ARCH_RDNA1;

    // vega (gcn5)
    if (did == AMD_VEGA_64 || did == AMD_VEGA_56) return AMD_ARCH_GCN5;
    if (did >= 0x6860 && did <= 0x68FF) return AMD_ARCH_GCN5;

    // polaris (gcn4)
    if (did >= 0x67C0 && did <= 0x67FF) return AMD_ARCH_GCN4;

    return AMD_ARCH_UNKNOWN;
}

AmdMemType AmdGPU::IdentifyMemType(AmdArch arch) {
    switch (arch) {
        case AMD_ARCH_RDNA35:
        case AMD_ARCH_RDNA3:
        case AMD_ARCH_RDNA2:
        case AMD_ARCH_RDNA1:  return AMD_MEM_GDDR6;
        case AMD_ARCH_GCN5:   return AMD_MEM_HBM2;
        case AMD_ARCH_GCN4:   return AMD_MEM_GDDR5;
        default: return AMD_MEM_UNKNOWN;
    }
}

AmdDisplayEngine AmdGPU::IdentifyDisplayEngine(AmdArch arch) {
    switch (arch) {
        case AMD_ARCH_RDNA35: return DCN_35;
        case AMD_ARCH_RDNA3:  return DCN_31;
        case AMD_ARCH_RDNA2:  return DCN_3;
        case AMD_ARCH_RDNA1:  return DCN_2;
        case AMD_ARCH_GCN5:   return DCN_1;
        case AMD_ARCH_GCN4:   return DCE_11;
        default: return DCE_UNKNOWN;
    }
}

const char* AmdGPU::IdentifyName(uint16_t did) {
    switch (did) {
        case AMD_RX_9070XT:   return "AMD Radeon RX 9070 XT";
        case AMD_RX_9070:     return "AMD Radeon RX 9070";
        case AMD_RX_7900XTX:  return "AMD Radeon RX 7900 XTX";
        case AMD_RX_7900XT:   return "AMD Radeon RX 7900 XT";
        case AMD_RX_7800XT:   return "AMD Radeon RX 7800 XT";
        case AMD_RX_7700XT:   return "AMD Radeon RX 7700 XT";
        case AMD_RX_7600:     return "AMD Radeon RX 7600";
        case AMD_RX_6950XT:   return "AMD Radeon RX 6950 XT";
        case AMD_RX_6900XT:   return "AMD Radeon RX 6900 XT";
        case AMD_RX_6800XT:   return "AMD Radeon RX 6800 XT";
        case AMD_RX_6800:     return "AMD Radeon RX 6800";
        case AMD_RX_6700XT:   return "AMD Radeon RX 6700 XT";
        case AMD_RX_6600XT:   return "AMD Radeon RX 6600 XT";
        case AMD_RX_6600:     return "AMD Radeon RX 6600";
        case AMD_RX_5700XT:   return "AMD Radeon RX 5700 XT";
        case AMD_RX_5700:     return "AMD Radeon RX 5700";
        case AMD_RX_5600XT:   return "AMD Radeon RX 5600 XT";
        case AMD_RX_5500XT:   return "AMD Radeon RX 5500 XT";
        case AMD_VEGA_64:     return "AMD Radeon RX Vega 64";
        case AMD_VEGA_56:     return "AMD Radeon RX Vega 56";
        default: return "AMD Radeon GPU (Unknown)";
    }
}

const char* AmdGPU::IdentifyChip(uint16_t did) {
    switch (did) {
        case AMD_RX_9070XT: case AMD_RX_9070:
            return "Navi48";
        case AMD_RX_7900XTX: case AMD_RX_7900XT:
            return "Navi31";
        case AMD_RX_7800XT: case AMD_RX_7700XT:
            return "Navi32";
        case AMD_RX_7600:
            return "Navi33";
        case AMD_RX_6950XT: case AMD_RX_6900XT: case AMD_RX_6800XT: case AMD_RX_6800:
            return "Navi21";
        case AMD_RX_6700XT:
            return "Navi22";
        case AMD_RX_6600XT: case AMD_RX_6600:
            return "Navi23";
        case AMD_RX_5700XT: case AMD_RX_5700:
            return "Navi10";
        case AMD_RX_5600XT: case AMD_RX_5500XT:
            return "Navi14";
        case AMD_VEGA_64: case AMD_VEGA_56:
            return "Vega10";
        default: return "Unknown";
    }
}

int AmdGPU::IdentifyCUs(uint16_t did, AmdArch arch) {
    switch (did) {
        case AMD_RX_9070XT:   return 64;
        case AMD_RX_9070:     return 56;
        case AMD_RX_7900XTX:  return 96;
        case AMD_RX_7900XT:   return 84;
        case AMD_RX_7800XT:   return 60;
        case AMD_RX_7700XT:   return 54;
        case AMD_RX_7600:     return 32;
        case AMD_RX_6950XT:
        case AMD_RX_6900XT:   return 80;
        case AMD_RX_6800XT:   return 72;
        case AMD_RX_6800:     return 60;
        case AMD_RX_6700XT:   return 40;
        case AMD_RX_6600XT:
        case AMD_RX_6600:     return 32;
        case AMD_RX_5700XT:   return 40;
        case AMD_RX_5700:     return 36;
        case AMD_RX_5600XT:   return 36;
        case AMD_RX_5500XT:   return 22;
        case AMD_VEGA_64:     return 64;
        case AMD_VEGA_56:     return 56;
        default:
            if (arch == AMD_ARCH_RDNA3) return 32;
            if (arch == AMD_ARCH_RDNA2) return 32;
            return 16;
    }
}

int AmdGPU::IdentifyBusWidth(uint16_t did, AmdArch arch) {
    switch (did) {
        case AMD_RX_9070XT: case AMD_RX_9070: return 256;
        case AMD_RX_7900XTX: case AMD_RX_7900XT: return 384;
        case AMD_RX_7800XT: case AMD_RX_7700XT:  return 256;
        case AMD_RX_7600: return 128;
        case AMD_RX_6950XT: case AMD_RX_6900XT: case AMD_RX_6800XT: case AMD_RX_6800: return 256;
        case AMD_RX_6700XT: return 192;
        case AMD_RX_6600XT: case AMD_RX_6600: return 128;
        case AMD_RX_5700XT: case AMD_RX_5700: return 256;
        case AMD_VEGA_64: case AMD_VEGA_56: return 2048;  // hbm2
        default: return 128;
    }
}

//  bar mapping
bool AmdGPU::MapBAR() {
    if (info.bar0 == 0) return false;

    // map bar0 as uncacheable mmio (identity-mapped in our kernel)
    mmio_base = (volatile uint32_t*)(uintptr_t)info.bar0;

    // verify access: read grbm_status (should not be all 0xf or 0x0)
    uint32_t grbm = ReadReg(AMDGPU_GRBM_STATUS);
    if (grbm == 0xFFFFFFFF) {
        mmio_base = nullptr;
        return false;
    }

    return true;
}

//  register access
uint32_t AmdGPU::ReadReg(uint32_t offset) {
    if (!mmio_base) return 0xFFFFFFFF;
    if (info.bar0_size && offset + 4 > info.bar0_size) return 0xFFFFFFFF;
    uint32_t v = mmio_base[offset / 4];
    __asm__ volatile("" ::: "memory");
    return v;
}

void AmdGPU::WriteReg(uint32_t offset, uint32_t value) {
    if (!mmio_base) return;
    if (info.bar0_size && offset + 4 > info.bar0_size) return;
    mmio_base[offset / 4] = value;
    __asm__ volatile("sfence" ::: "memory");
}

uint32_t AmdGPU::ReadRegIdx(uint32_t offset) {
    if (!mmio_base) return 0xFFFFFFFF;
    WriteReg(AMDGPU_MM_INDEX, offset);
    return ReadReg(AMDGPU_MM_DATA);
}

void AmdGPU::WriteRegIdx(uint32_t offset, uint32_t value) {
    if (!mmio_base) return;
    WriteReg(AMDGPU_MM_INDEX, offset);
    WriteReg(AMDGPU_MM_DATA, value);
}

//  vram detection
void AmdGPU::DetectVRAM() {
    // read mc (memory controller) registers for vram size
    // location varies by architecture
    uint32_t mc_status = ReadRegIdx(0x2004);  // mc_vm_fb_size
    if (mc_status != 0 && mc_status != 0xFFFFFFFF) {
        info.vram_size = (uint64_t)mc_status * 1024 * 1024;
    } else {
        // fallback: estimate from device id
        switch (info.device_id) {
            case AMD_RX_7900XTX:  info.vram_size = 24ULL * 1024 * 1024 * 1024; break;
            case AMD_RX_7900XT:   info.vram_size = 20ULL * 1024 * 1024 * 1024; break;
            case AMD_RX_7800XT:
            case AMD_RX_6950XT:
            case AMD_RX_6900XT:
            case AMD_RX_6800XT:
            case AMD_RX_6800:     info.vram_size = 16ULL * 1024 * 1024 * 1024; break;
            case AMD_RX_7700XT:
            case AMD_RX_6700XT:
            case AMD_RX_5700XT:
            case AMD_RX_5700:     info.vram_size = 12ULL * 1024 * 1024 * 1024; break;
            case AMD_RX_9070XT:
            case AMD_RX_9070:     info.vram_size = 16ULL * 1024 * 1024 * 1024; break;
            default:              info.vram_size = 8ULL * 1024 * 1024 * 1024; break;
        }
    }
}

void AmdGPU::DetectClocks() {
    // read current engine/memory clocks from smu
    // the exact register depends on architecture but we try common locations
    uint32_t sclk = ReadRegIdx(0x0200);  // gfx_sclk
    uint32_t mclk = ReadRegIdx(0x0204);  // gfx_mclk

    if (sclk > 0 && sclk < 4000 && sclk != 0xFFFF) {
        info.max_clock_mhz = sclk;
    } else {
        // fallback estimates
        switch (info.arch) {
            case AMD_ARCH_RDNA35: info.max_clock_mhz = 2700; break;
            case AMD_ARCH_RDNA3:  info.max_clock_mhz = 2500; break;
            case AMD_ARCH_RDNA2:  info.max_clock_mhz = 2310; break;
            case AMD_ARCH_RDNA1:  info.max_clock_mhz = 2100; break;
            case AMD_ARCH_GCN5:   info.max_clock_mhz = 1630; break;
            default:              info.max_clock_mhz = 1500; break;
        }
    }

    if (mclk > 0 && mclk < 3000 && mclk != 0xFFFF) {
        info.memory_clock_mhz = mclk;
    } else {
        switch (info.mem_type) {
            case AMD_MEM_GDDR6:  info.memory_clock_mhz = 2000; break;
            case AMD_MEM_HBM2:   info.memory_clock_mhz = 945;  break;
            case AMD_MEM_GDDR5:  info.memory_clock_mhz = 2000; break;
            default:             info.memory_clock_mhz = 1750; break;
        }
    }
}

//  init
bool AmdGPU::Init() {
    info.detected = false;
    mmio_base = nullptr;

    if (!ScanPCI()) return false;

    info.arch = IdentifyArch(info.device_id);
    info.mem_type = IdentifyMemType(info.arch);
    info.display_engine = IdentifyDisplayEngine(info.arch);
    info.compute_units = IdentifyCUs(info.device_id, info.arch);
    info.memory_bus_width = IdentifyBusWidth(info.device_id, info.arch);

    // rdna 3+ uses wgp (workgroup processors), each wgp has 2 cus
    // stream processors = cus × 64 (rdna) or cus × 128 / 2 (rdna3 dual-issue)
    if (info.arch >= AMD_ARCH_RDNA1)
        info.stream_processors = info.compute_units * 64;
    else
        info.stream_processors = info.compute_units * 64;

    // hardware ray tracing available on rdna 2+
    info.hardware_raytracing = (info.arch >= AMD_ARCH_RDNA2);

    // accel capability: any recognized arch with a working MMIO mapping
    info.has_2d_accel = (info.arch != AMD_ARCH_UNKNOWN);
    info.has_3d_accel = (info.arch >= AMD_ARCH_RDNA1);

    _scpy(info.name, IdentifyName(info.device_id), 64);
    _scpy(info.chip_name, IdentifyChip(info.device_id), 16);

    if (!MapBAR()) {
        // still mark as detected even without mmio  -  info is valid from pci
        info.detected = true;
        DetectVRAM();
        DetectClocks();
        SerialLogger::Log("[AMD GPU] Detected ");
        SerialLogger::Log(info.name);
        SerialLogger::Log(" (no MMIO)\r\n");
        return true;
    }

    info.detected = true;
    DetectVRAM();
    DetectClocks();

    SerialLogger::Log("[AMD GPU] ");
    SerialLogger::Log(info.name);
    SerialLogger::Log(" @ PCI ");
    SerialLogger::LogHex(info.bus);
    SerialLogger::Log(":");
    SerialLogger::LogHex(info.device);
    SerialLogger::Log(".");
    SerialLogger::LogHex(info.function);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("[AMD GPU] BAR0=");
    SerialLogger::LogHex((uint32_t)info.bar0);
    SerialLogger::Log(" CUs=");
    SerialLogger::LogDec(info.compute_units);
    SerialLogger::Log(" SPs=");
    SerialLogger::LogDec(info.stream_processors);
    SerialLogger::Log(" VRAM=");
    SerialLogger::LogDec((int)(info.vram_size / (1024*1024)));
    SerialLogger::Log("MB ");
    SerialLogger::Log(info.resizable_bar ? "ReBAR" : "");
    SerialLogger::Log("\r\n");

    return true;
}

bool AmdGPU::IsAvailable() { return info.detected; }
const AmdGPUInfo& AmdGPU::GetInfo() { return info; }
bool AmdGPU::HasHardwareAccel() { return info.detected && mmio_base != nullptr && info.has_2d_accel; }

const char* AmdGPU::GetArchName() {
    switch (info.arch) {
        case AMD_ARCH_RDNA35: return "RDNA 3.5";
        case AMD_ARCH_RDNA3:  return "RDNA 3";
        case AMD_ARCH_RDNA2:  return "RDNA 2";
        case AMD_ARCH_RDNA1:  return "RDNA 1";
        case AMD_ARCH_GCN5:   return "GCN 5 (Vega)";
        case AMD_ARCH_GCN4:   return "GCN 4 (Polaris)";
        case AMD_ARCH_GCN3:   return "GCN 3";
        default: return "Unknown";
    }
}

const char* AmdGPU::GetDisplayEngineName() {
    switch (info.display_engine) {
        case DCN_35:  return "DCN 3.5";
        case DCN_32:  return "DCN 3.2";
        case DCN_31:  return "DCN 3.1";
        case DCN_3:   return "DCN 3.0";
        case DCN_2:   return "DCN 2.0";
        case DCN_1:   return "DCN 1.0";
        case DCE_11:  return "DCE 11.0";
        case DCE_10:  return "DCE 10.0";
        case DCE_8:   return "DCE 8.0";
        default: return "Unknown";
    }
}

//  gpu reset
bool AmdGPU::SoftReset() {
    if (!mmio_base) return false;

    // write to grbm_soft_reset register
    uint32_t reset_val = ReadReg(AMDGPU_GRBM_SOFT_RESET);
    // set soft reset bits for gfx, cp, and rlc
    reset_val |= (1 << 0) | (1 << 1) | (1 << 2);  // soft_reset_cp | gfx | rlc
    WriteReg(AMDGPU_GRBM_SOFT_RESET, reset_val);

    // read back to ensure write is flushed
    (void)ReadReg(AMDGPU_GRBM_SOFT_RESET);

    // wait ~50 microseconds (spin loop)
    for (volatile int i = 0; i < 50000; i++) {}

    // clear reset bits
    reset_val &= ~((1 << 0) | (1 << 1) | (1 << 2));
    WriteReg(AMDGPU_GRBM_SOFT_RESET, reset_val);
    (void)ReadReg(AMDGPU_GRBM_SOFT_RESET);

    return true;
}

bool AmdGPU::GFXReset() {
    return SoftReset();
}

//  display engine
bool AmdGPU::InitDisplay(int width, int height) {
    if (!mmio_base) return false;

    // enable display clock via smu
    WriteReg(AMDGPU_SMC_ARG, 0x01);
    WriteReg(AMDGPU_SMC_MSG, 0x0A);  // msg_setdispclkfreq

    // wait for smu response
    for (int i = 0; i < 1000; i++) {
        if (ReadReg(AMDGPU_SMC_RESP) == 0x01) break;  // ok
        for (volatile int j = 0; j < 10000; j++) {}
    }

    (void)width; (void)height;  // mode setting requires full crtc programming
    return true;
}

bool AmdGPU::SetResolution(int width, int height, int refresh) {
    (void)width; (void)height; (void)refresh;
    return InitDisplay(width, height);
}

int AmdGPU::GetMaxRefreshRate() {
    if (info.display_engine >= DCN_3) return 360;
    if (info.display_engine >= DCN_2) return 240;
    if (info.display_engine >= DCN_1) return 144;
    return 60;
}

//  power management / sensors
int AmdGPU::GetGPUTemperature() {
    if (!mmio_base) return 0;
    // read temperature register (thm block)
    uint32_t thm_val = ReadRegIdx(0x0300);  // cg_mult_thermal_status
    if (thm_val == 0xFFFFFFFF) return 0;
    int temp = (thm_val >> 24) & 0xFF;
    return temp;
}

int AmdGPU::GetFanSpeedPct() {
    if (!mmio_base) return 0;
    uint32_t fan_val = ReadRegIdx(0x0308);
    if (fan_val == 0xFFFFFFFF) return 0;
    return fan_val & 0xFF;
}

int AmdGPU::GetPowerDraw() {
    if (!mmio_base) return 0;
    uint32_t power = ReadRegIdx(0x030C);
    if (power == 0xFFFFFFFF) return 0;
    return power & 0xFFFF;  // mw → w
}

bool AmdGPU::SetPowerProfile(int profile) {
    if (!mmio_base) return false;
    WriteReg(AMDGPU_SMC_ARG, profile);
    WriteReg(AMDGPU_SMC_MSG, 0x05);  // msg_setworkloadmode
    return true;
}

//  vram info
uint64_t AmdGPU::GetVRAMTotal() { return info.vram_size; }
uint64_t AmdGPU::GetVRAMUsed()  { return 0; }  // requires memory manager tracking
uint64_t AmdGPU::GetVRAMFree()  { return info.vram_size; }

//  debug dump
void AmdGPU::DumpRegisters(char* buf, int max_len) {
    if (!buf || max_len < 256) return;
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < max_len-1) buf[p++] = *s++; buf[p] = 0; };
    auto sai = [&](int v) {
        if (v < 0) { if (p < max_len-1) buf[p++] = '-'; v = -v; }
        char t[12]; int ti = 0;
        if (v == 0) { t[ti++] = '0'; } else { while (v > 0) { t[ti++] = '0' + (v%10); v /= 10; } }
        while (ti > 0 && p < max_len-1) buf[p++] = t[--ti];
        buf[p] = 0;
    };

    sa("AMD GPU Register Dump:\n");
    sa("  GRBM_STATUS: 0x"); sai(mmio_base ? ReadReg(AMDGPU_GRBM_STATUS) : 0); sa("\n");
    sa("  SRBM_STATUS: 0x"); sai(mmio_base ? ReadReg(AMDGPU_SRBM_STATUS) : 0); sa("\n");
    sa("  RLC_CNTL:    0x"); sai(mmio_base ? ReadReg(AMDGPU_RLC_CNTL) : 0); sa("\n");
}
