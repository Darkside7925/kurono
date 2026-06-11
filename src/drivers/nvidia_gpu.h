#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — NVIDIA GPU Driver
//  PCI device detection, BAR mapping, and basic GPU management
//  Supports GeForce RTX 30xx/40xx/50xx series detection
// ═══════════════════════════════════════════════════════════════════════════
#include "../kernel/types.h"

// ── NVIDIA PCI Vendor ID ──
#define NVIDIA_VENDOR_ID   0x10DE

// ── Known Device IDs (subset) ──
#define NVIDIA_RTX_5090    0x2B85
#define NVIDIA_RTX_5080    0x2B80
#define NVIDIA_RTX_5070TI  0x2B02
#define NVIDIA_RTX_5070    0x2B00
#define NVIDIA_RTX_4090    0x2684
#define NVIDIA_RTX_4080    0x2704
#define NVIDIA_RTX_4070TI  0x2782
#define NVIDIA_RTX_4070    0x2786
#define NVIDIA_RTX_3090    0x2204
#define NVIDIA_RTX_3080    0x2206
#define NVIDIA_RTX_3070    0x2484

// ── GPU Architecture Generation ──
enum NvidiaArch {
    ARCH_UNKNOWN = 0,
    ARCH_AMPERE,       // RTX 30xx  (GA1xx)
    ARCH_ADA_LOVELACE, // RTX 40xx  (AD1xx)
    ARCH_BLACKWELL,    // RTX 50xx  (GB2xx)
};

// ── GPU Memory Type ──
enum GpuMemType {
    MEM_UNKNOWN = 0,
    MEM_GDDR6,
    MEM_GDDR6X,
    MEM_GDDR7,
};

// ── GPU Information Structure ──
struct NvidiaGPUInfo {
    bool        detected;
    uint16_t    vendor_id;
    uint16_t    device_id;
    uint8_t     bus, device, function;  // PCI BDF
    uint8_t     revision;
    NvidiaArch  arch;
    GpuMemType  mem_type;
    uint64_t    bar0;           // MMIO register base (BAR0)
    uint64_t    bar1;           // Framebuffer base   (BAR1)
    uint64_t    bar0_size;
    uint64_t    bar1_size;
    uint32_t    vram_mb;        // Estimated VRAM in MB
    char        name[64];       // Human-readable GPU name
};

// ── GPU Driver State ──
enum GpuDriverState {
    GPU_STATE_UNINITIALIZED = 0,
    GPU_STATE_DETECTED,
    GPU_STATE_BARS_MAPPED,
    GPU_STATE_INITIALIZED,
    GPU_STATE_PASSTHROUGH,      // Assigned to VM via VT-d
    GPU_STATE_ERROR,
};

// ═══════════════════════════════════════════════════════════════════════════
//  NvidiaGPU — Static driver interface
// ═══════════════════════════════════════════════════════════════════════════
class NvidiaGPU {
public:
    // ── Lifecycle ──
    static void Init();                        // Scan PCI bus for NVIDIA GPUs
    static bool IsDetected();                  // GPU found?
    static GpuDriverState GetState();

    // ── GPU Information ──
    static const NvidiaGPUInfo& GetInfo();
    static const char* GetArchName();
    static const char* GetMemTypeName();

    // ── PCI Configuration ──
    static uint32_t PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
    static void     PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

    // ── BAR Access ──
    static uint32_t ReadReg(uint32_t offset);  // Read MMIO register via BAR0
    static void     WriteReg(uint32_t offset, uint32_t value);

    // ── GPU Management ──
    static bool     ResetGPU();                // Full GPU reset via PMC
    static bool     EnableBusMaster();         // Enable PCI bus mastering
    static uint32_t GetBootDisplay();          // Read NV_PMC_BOOT_0
    static uint32_t GetVRAMSize();             // Detect VRAM from FB regs

    // ── VT-d Passthrough Support ──
    static bool     PrepareForPassthrough();   // Detach host driver, prepare for IOMMU
    static bool     IsPassthroughReady();

    // ── Debug / Status ──
    static void     DumpInfo(char* out, int maxo);
    static void     DumpRegisters(char* out, int maxo);

private:
    static NvidiaGPUInfo   gpu_info;
    static GpuDriverState  state;

    // ── Internal helpers ──
    static void     ScanPCIBus();
    static bool     ProbeDevice(uint8_t bus, uint8_t dev, uint8_t func);
    static void     IdentifyGPU();
    static uint64_t ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);
    static uint64_t GetBARSize(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);
};
