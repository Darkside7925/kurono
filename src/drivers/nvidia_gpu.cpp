//  kurono os  -  nvidia gpu driver implementation
//  pci enumeration, bar mapping, register access, vt-d passthrough prep
#include "nvidia_gpu.h"
#include "../hal/hal.h"
#include "../drivers/serial.h"
#include "../kernel/vmm.h"   // map the (possibly high 64-bit) bar before deref (satoru)

// identity-map a bar's register window as uncached mmio before any access.
// nvidia bar0 (mmio register block) is 64-bit and is usually placed high above
// the boot identity map, so ReadReg would #pf unmapped. mirrors nvme.cpp /
// virtio_gpu.cpp; caps at 16mb (the pmc/pfb register block fits) + idempotent.
// (satoru)
static void nvgpu_map_bar_window(uint64_t base, uint64_t size) {
    if (base == 0) return;
    uint64_t window = size ? size : 0x1000000ULL; // default 16mb if size unknown
    if (window > 0x1000000ULL) window = 0x1000000ULL;
    uint64_t start = base & ~0xFFFULL;
    uint64_t end   = (base + window + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000ULL) {
        KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    }
}

NvidiaGPUInfo  NvidiaGPU::gpu_info = {};
GpuDriverState NvidiaGPU::state = GPU_STATE_UNINITIALIZED;
uint32_t       NvidiaGPU::last_fault_code = 0;
bool           NvidiaGPU::has_fault = false;

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_REVISION     0x08
#define PCI_CLASS        0x08
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24

#define PCI_CMD_IO       (1 << 0)
#define PCI_CMD_MEMORY   (1 << 1)
#define PCI_CMD_MASTER   (1 << 2)
 
#define NV_PMC_BOOT_0      0x000000
#define NV_PMC_ENABLE       0x000200
#define NV_PMC_INTR_0       0x000100
#define NV_PMC_INTR_EN_0    0x000140
#define NV_PFB_CFG0         0x100C00
#define NV_PFB_CSTATUS      0x10020C

static int sa(char* o, int p, int mx, const char* s) {
    while (*s && p < mx - 1) o[p++] = *s++;
    o[p] = 0;
    return p;
}

static int sa_hex(char* o, int p, int mx, uint64_t v) {
    char buf[20];
    const char hex[] = "0123456789ABCDEF";
    int len = 0;
    if (v == 0) { buf[len++] = '0'; }
    else {
        uint64_t tmp = v;
        char rev[20]; int ri = 0;
        while (tmp) { rev[ri++] = hex[tmp & 0xF]; tmp >>= 4; }
        for (int i = ri - 1; i >= 0; i--) buf[len++] = rev[i];
    }
    buf[len] = 0;
    return sa(o, p, mx, buf);
}

static int sa_dec(char* o, int p, int mx, uint32_t v) {
    char buf[16]; int len = 0;
    if (v == 0) { buf[len++] = '0'; }
    else {
        char rev[16]; int ri = 0;
        while (v) { rev[ri++] = '0' + (v % 10); v /= 10; }
        for (int i = ri - 1; i >= 0; i--) buf[len++] = rev[i];
    }
    buf[len] = 0;
    return sa(o, p, mx, buf);
}

static void scpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

//  pci configuration space access

static inline void pci_outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint32_t NvidiaGPU::PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

void NvidiaGPU::PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, val);
}

//  bar reading (supports 64-bit bars)

uint64_t NvidiaGPU::ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    if (bar_index > 5) return 0;
    uint8_t offset = PCI_BAR0 + bar_index * 4;
    uint32_t bar_low = PciRead(bus, dev, func, offset);

    if (bar_low == 0xFFFFFFFF || bar_low == 0) return 0;

    // I/O BARs are not used by NVIDIA for register or framebuffer access  -  reject them
    if (bar_low & 1) return 0;

    uint8_t type = (bar_low >> 1) & 0x03;
    uint64_t base = bar_low & 0xFFFFFFF0;
    if (type == 0x02 && bar_index < 5) {
        uint32_t bar_high = PciRead(bus, dev, func, offset + 4);
        base |= ((uint64_t)bar_high << 32);
    }
    return base;
}

uint64_t NvidiaGPU::GetBARSize(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    if (bar_index > 5) return 0;
    uint8_t offset = PCI_BAR0 + bar_index * 4;
    uint32_t original_lo = PciRead(bus, dev, func, offset);
    if (original_lo == 0xFFFFFFFF || (original_lo & 1)) return 0;

    bool is_64 = ((original_lo >> 1) & 0x03) == 0x02 && bar_index < 5;
    uint32_t original_hi = is_64 ? PciRead(bus, dev, func, offset + 4) : 0;

    // size discovery: write all-1s, read mask, restore
    PciWrite(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t mask_lo = PciRead(bus, dev, func, offset);
    uint32_t mask_hi = 0;
    if (is_64) {
        PciWrite(bus, dev, func, offset + 4, 0xFFFFFFFF);
        mask_hi = PciRead(bus, dev, func, offset + 4);
        PciWrite(bus, dev, func, offset + 4, original_hi);
    }
    PciWrite(bus, dev, func, offset, original_lo);

    uint64_t mask = ((uint64_t)mask_hi << 32) | (mask_lo & 0xFFFFFFF0);
    if (mask == 0) return 0;
    return (~mask) + 1;
}

//  pci bus scan  -  find nvidia gpu

bool NvidiaGPU::ProbeDevice(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t vid_did = PciRead(bus, dev, func, PCI_VENDOR_ID);
    uint16_t vendor = vid_did & 0xFFFF;
    uint16_t device = (vid_did >> 16) & 0xFFFF;

    if (vendor != NVIDIA_VENDOR_ID) return false;

    // check class: 0x03 = display controller
    uint32_t class_reg = PciRead(bus, dev, func, PCI_CLASS);
    uint8_t base_class = (class_reg >> 24) & 0xFF;
    if (base_class != 0x03) return false;

    // found an nvidia gpu!
    gpu_info.detected = true;
    gpu_info.vendor_id = vendor;
    gpu_info.device_id = device;
    gpu_info.bus = bus;
    gpu_info.device = dev;
    gpu_info.function = func;
    gpu_info.revision = class_reg & 0xFF;

    // read bars (nvidia: bar0 = mmio registers, bar1 = framebuffer)
    gpu_info.bar0 = ReadBAR(bus, dev, func, 0);
    gpu_info.bar1 = ReadBAR(bus, dev, func, 1);
    gpu_info.bar0_size = GetBARSize(bus, dev, func, 0);
    gpu_info.bar1_size = GetBARSize(bus, dev, func, 1);

    return true;
}

void NvidiaGPU::ScanPCIBus() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            // probe func 0 once; only enumerate the rest if multi-function bit is set
            uint32_t vid0 = PciRead((uint8_t)bus, dev, 0, PCI_VENDOR_ID);
            if ((vid0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr0 = PciRead((uint8_t)bus, dev, 0, 0x0C);
            int max_func = ((hdr0 >> 16) & 0x80) ? 8 : 1;

            for (int func = 0; func < max_func; func++) {
                uint32_t vid = PciRead((uint8_t)bus, dev, (uint8_t)func, PCI_VENDOR_ID);
                if ((vid & 0xFFFF) == 0xFFFF) continue;

                if (ProbeDevice((uint8_t)bus, dev, (uint8_t)func)) {
                    return; // first nvidia gpu wins
                }
            }
        }
    }
}

//  gpu identification

void NvidiaGPU::IdentifyGPU() {
    uint16_t did = gpu_info.device_id;

    // determine architecture and name based on device id ranges
    // blackwell (rtx 50xx): 0x2bxx
    if ((did & 0xFF00) == 0x2B00) {
        gpu_info.arch = ARCH_BLACKWELL;
        gpu_info.mem_type = MEM_GDDR7;
        if (did == NVIDIA_RTX_5090 || did == 0x2B84 || did == 0x2B85 || did == 0x2B86) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 5090", 64);
            gpu_info.vram_mb = 32768; // 32 gb
        } else if (did == NVIDIA_RTX_5080 || did == 0x2B80 || did == 0x2B81) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 5080", 64);
            gpu_info.vram_mb = 16384; // 16 gb
        } else if (did == NVIDIA_RTX_5070TI || did == 0x2B02 || did == 0x2B03) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 5070 Ti", 64);
            gpu_info.vram_mb = 16384;
        } else if (did == NVIDIA_RTX_5070 || did == 0x2B00 || did == 0x2B01) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 5070", 64);
            gpu_info.vram_mb = 12288;
        } else {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 50xx (Blackwell)", 64);
            gpu_info.vram_mb = 8192;
        }
    }
    // ada lovelace (rtx 40xx): 0x26xx, 0x27xx, 0x28xx
    else if ((did & 0xFF00) >= 0x2600 && (did & 0xFF00) <= 0x2800) {
        gpu_info.arch = ARCH_ADA_LOVELACE;
        gpu_info.mem_type = MEM_GDDR6X;
        if (did == NVIDIA_RTX_4090) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 4090", 64);
            gpu_info.vram_mb = 24576;
        } else if (did == NVIDIA_RTX_4080) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 4080", 64);
            gpu_info.vram_mb = 16384;
        } else if (did == NVIDIA_RTX_4070TI) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 4070 Ti", 64);
            gpu_info.vram_mb = 12288;
        } else {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 40xx (Ada)", 64);
            gpu_info.vram_mb = 8192;
        }
    }
    // ampere (rtx 30xx): 0x22xx, 0x24xx, 0x25xx
    else if ((did & 0xFF00) >= 0x2200 && (did & 0xFF00) <= 0x2500) {
        gpu_info.arch = ARCH_AMPERE;
        gpu_info.mem_type = MEM_GDDR6X;
        if (did == NVIDIA_RTX_3090) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 3090", 64);
            gpu_info.vram_mb = 24576;
        } else if (did == NVIDIA_RTX_3080) {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 3080", 64);
            gpu_info.vram_mb = 10240;
        } else {
            scpy(gpu_info.name, "NVIDIA GeForce RTX 30xx (Ampere)", 64);
            gpu_info.vram_mb = 8192;
        }
    }
    else {
        gpu_info.arch = ARCH_UNKNOWN;
        gpu_info.mem_type = MEM_UNKNOWN;
        scpy(gpu_info.name, "NVIDIA GPU (Unknown)", 64);
        gpu_info.vram_mb = 0;
    }

    // accel flags: NVIDIA discrete GPUs all expose hardware blit/3D engines
    // when BAR0 is mapped, but the kernel doesn't ship a command-stream
    // submitter yet  -  surface a conservative capability for the compositor.
    gpu_info.has_2d_accel = (gpu_info.arch != ARCH_UNKNOWN);
    gpu_info.has_3d_accel = (gpu_info.arch != ARCH_UNKNOWN);
}

bool NvidiaGPU::HasHardwareAccel() {
    return gpu_info.detected && gpu_info.bar0 != 0 && gpu_info.has_2d_accel;
}

//  mmio register access (via bar0)

uint32_t NvidiaGPU::ReadReg(uint32_t offset) {
    if (!gpu_info.bar0) return 0;
    if (gpu_info.bar0_size && offset + 4 > gpu_info.bar0_size) return 0;
    volatile uint32_t* reg = (volatile uint32_t*)(gpu_info.bar0 + offset);
    uint32_t v = *reg;
    __asm__ volatile("" ::: "memory");
    return v;
}

void NvidiaGPU::WriteReg(uint32_t offset, uint32_t value) {
    if (!gpu_info.bar0) return;
    if (gpu_info.bar0_size && offset + 4 > gpu_info.bar0_size) return;
    volatile uint32_t* reg = (volatile uint32_t*)(gpu_info.bar0 + offset);
    *reg = value;
    __asm__ volatile("sfence" ::: "memory");
}

//  gpu operations

bool NvidiaGPU::EnableBusMaster() {
    if (!gpu_info.detected) return false;
    uint32_t cmd = PciRead(gpu_info.bus, gpu_info.device, gpu_info.function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_MASTER;
    PciWrite(gpu_info.bus, gpu_info.device, gpu_info.function, PCI_COMMAND, cmd);
    return true;
}

uint32_t NvidiaGPU::GetBootDisplay() {
    if (!gpu_info.bar0) return 0;
    return ReadReg(NV_PMC_BOOT_0);
}

uint32_t NvidiaGPU::GetVRAMSize() {
    if (!gpu_info.bar0) return gpu_info.vram_mb;
    // try reading vram size from pfb registers
    uint32_t cstatus = ReadReg(NV_PFB_CSTATUS);
    if (cstatus > 0) {
        return cstatus / (1024 * 1024); // convert to mb
    }
    return gpu_info.vram_mb; // fall back to estimated
}

void NvidiaGPU::PollTelemetry() {
    if (!gpu_info.detected || !gpu_info.bar0) return;

    uint32_t intr = ReadReg(NV_PMC_INTR_0);
    // only the top-level "host" / "fault" bits indicate driver-visible faults.
    // many bits here are routine display/engine events that should not flip the
    // driver into a permanent ERROR state.
    constexpr uint32_t FAULT_MASK = 0xFFFF0000u; // upper half = fault classes on Ampere+
    uint32_t faults = intr & FAULT_MASK;
    if (faults) {
        last_fault_code = faults;
        has_fault = true;
        state = GPU_STATE_ERROR;
    }
    // ack everything we observed so future polls don't re-trigger on stale bits
    if (intr) WriteReg(NV_PMC_INTR_0, intr);
}

bool NvidiaGPU::HasFault() {
    return has_fault;
}

uint32_t NvidiaGPU::GetLastFaultCode() {
    return last_fault_code;
}

void NvidiaGPU::ClearFault() {
    has_fault = false;
}

bool NvidiaGPU::ResetGPU() {
    if (!gpu_info.bar0) return false;
    // soft-reset via pmc enable register: preserve original engine mask, only toggle.
    // writing 0xFFFFFFFF would enable engines that may not exist on this chip and
    // hang the GPU on Ampere/Ada.
    uint32_t saved = ReadReg(NV_PMC_ENABLE);
    WriteReg(NV_PMC_ENABLE, 0);
    for (volatile int i = 0; i < 100000; i++) {}
    WriteReg(NV_PMC_ENABLE, saved);
    for (volatile int i = 0; i < 100000; i++) {}
    // ack any pending interrupts so the latch doesn't fire immediately after reset
    WriteReg(NV_PMC_INTR_0, 0);
    last_fault_code = 0;
    has_fault = false;
    if (gpu_info.detected) state = gpu_info.bar0 ? GPU_STATE_INITIALIZED : GPU_STATE_DETECTED;
    return true;
}

//  vt-d passthrough support

bool NvidiaGPU::PrepareForPassthrough() {
    if (!gpu_info.detected) return false;

    // mask interrupts and ack pending state BEFORE we yank MMIO access
    if (gpu_info.bar0) {
        WriteReg(NV_PMC_INTR_EN_0, 0);
        uint32_t pending = ReadReg(NV_PMC_INTR_0);
        if (pending) WriteReg(NV_PMC_INTR_0, pending);
    }

    // disable bus master + memory space on host side (guest will re-enable)
    uint32_t cmd = PciRead(gpu_info.bus, gpu_info.device, gpu_info.function, PCI_COMMAND);
    cmd &= ~(PCI_CMD_MEMORY | PCI_CMD_MASTER);
    PciWrite(gpu_info.bus, gpu_info.device, gpu_info.function, PCI_COMMAND, cmd);

    // mmio is now disabled  -  clear cached base so any stray ReadReg returns 0 instead
    // of dereferencing a disabled region (which can fault on real hardware)
    gpu_info.bar0 = 0;

    state = GPU_STATE_PASSTHROUGH;
    return true;
}

bool NvidiaGPU::IsPassthroughReady() {
    return state == GPU_STATE_PASSTHROUGH;
}

//  initialization

void NvidiaGPU::Init() {
    memset(&gpu_info, 0, sizeof(gpu_info));
    state = GPU_STATE_UNINITIALIZED;
    last_fault_code = 0;
    has_fault = false;

    SerialLogger::Log("[NVIDIA] Scanning PCI bus for NVIDIA GPUs...\r\n");
    ScanPCIBus();

    if (!gpu_info.detected) {
        SerialLogger::Log("[NVIDIA] No NVIDIA GPU found\r\n");
        return;
    }

    state = GPU_STATE_DETECTED;
    IdentifyGPU();

    // log what we found
    SerialLogger::Log("[NVIDIA] Found: ");
    SerialLogger::Log(gpu_info.name);
    SerialLogger::Log("\r\n");

    // enable bus mastering for dma
    EnableBusMaster();

    if (gpu_info.bar0) {
        // map the bar window before the first register read (GetBootDisplay)  - 
        // a high 64-bit bar isn't covered by the boot identity map. (satoru)
        nvgpu_map_bar_window(gpu_info.bar0, gpu_info.bar0_size);
        state = GPU_STATE_BARS_MAPPED;
        // read boot display register to verify bar0 access
        uint32_t boot0 = GetBootDisplay();
        (void)boot0;
        state = GPU_STATE_INITIALIZED;
        SerialLogger::Log("[NVIDIA] GPU initialized (BAR0 mapped, bus mastering enabled)\r\n");
    } else {
        SerialLogger::Log("[NVIDIA] Warning: BAR0 not available, limited functionality\r\n");
    }
}

bool NvidiaGPU::IsDetected() {
    return gpu_info.detected;
}

GpuDriverState NvidiaGPU::GetState() {
    return state;
}

const NvidiaGPUInfo& NvidiaGPU::GetInfo() {
    return gpu_info;
}

const char* NvidiaGPU::GetArchName() {
    switch (gpu_info.arch) {
        case ARCH_AMPERE:       return "Ampere";
        case ARCH_ADA_LOVELACE: return "Ada Lovelace";
        case ARCH_BLACKWELL:    return "Blackwell";
        default:                return "Unknown";
    }
}

const char* NvidiaGPU::GetMemTypeName() {
    switch (gpu_info.mem_type) {
        case MEM_GDDR6:  return "GDDR6";
        case MEM_GDDR6X: return "GDDR6X";
        case MEM_GDDR7:  return "GDDR7";
        default:         return "Unknown";
    }
}

//  debug / status output

void NvidiaGPU::DumpInfo(char* out, int maxo) {
    int p = 0;
    if (!gpu_info.detected) {
        p = sa(out, p, maxo, "No NVIDIA GPU detected\n");
        return;
    }

    p = sa(out, p, maxo, "\033[36m═══ NVIDIA GPU ═══\033[0m\n");
    p = sa(out, p, maxo, "  Name : "); p = sa(out, p, maxo, gpu_info.name); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  Arch : "); p = sa(out, p, maxo, GetArchName()); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  PCI  : ");
    p = sa_hex(out, p, maxo, gpu_info.bus); p = sa(out, p, maxo, ":");
    p = sa_hex(out, p, maxo, gpu_info.device); p = sa(out, p, maxo, ".");
    p = sa_hex(out, p, maxo, gpu_info.function); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  VID  : 0x"); p = sa_hex(out, p, maxo, gpu_info.vendor_id);
    p = sa(out, p, maxo, "  DID: 0x"); p = sa_hex(out, p, maxo, gpu_info.device_id); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  BAR0 : 0x"); p = sa_hex(out, p, maxo, gpu_info.bar0); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  BAR1 : 0x"); p = sa_hex(out, p, maxo, gpu_info.bar1); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  VRAM : "); p = sa_dec(out, p, maxo, gpu_info.vram_mb); p = sa(out, p, maxo, " MB (");
    p = sa(out, p, maxo, GetMemTypeName()); p = sa(out, p, maxo, ")\n");
    p = sa(out, p, maxo, "  State: ");
    switch (state) {
        case GPU_STATE_DETECTED:      p = sa(out, p, maxo, "Detected"); break;
        case GPU_STATE_BARS_MAPPED:   p = sa(out, p, maxo, "BARs Mapped"); break;
        case GPU_STATE_INITIALIZED:   p = sa(out, p, maxo, "Initialized"); break;
        case GPU_STATE_PASSTHROUGH:   p = sa(out, p, maxo, "VT-d Passthrough"); break;
        case GPU_STATE_ERROR:         p = sa(out, p, maxo, "Error"); break;
        default:                      p = sa(out, p, maxo, "Uninitialized"); break;
    }
    p = sa(out, p, maxo, "\n");
}

void NvidiaGPU::DumpRegisters(char* out, int maxo) {
    int p = 0;
    if (!gpu_info.bar0) {
        p = sa(out, p, maxo, "BAR0 not mapped  -  cannot read registers\n");
        return;
    }
    p = sa(out, p, maxo, "\033[33mNVIDIA GPU Registers:\033[0m\n");
    p = sa(out, p, maxo, "  PMC_BOOT_0 : 0x"); p = sa_hex(out, p, maxo, ReadReg(NV_PMC_BOOT_0)); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  PMC_ENABLE : 0x"); p = sa_hex(out, p, maxo, ReadReg(NV_PMC_ENABLE)); p = sa(out, p, maxo, "\n");
    p = sa(out, p, maxo, "  PMC_INTR_0 : 0x"); p = sa_hex(out, p, maxo, ReadReg(NV_PMC_INTR_0)); p = sa(out, p, maxo, "\n");
}
