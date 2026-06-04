#pragma once
//  kurono os  -  nvidia gpu driver
//  pci device detection, bar mapping, and basic gpu management
//  supports geforce rtx 30xx/40xx/50xx series detection
#include "../kernel/types.h"

#define NVIDIA_VENDOR_ID   0x10DE

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

enum NvidiaArch {
    ARCH_UNKNOWN = 0,
    ARCH_AMPERE,       // rtx 30xx  (ga1xx)
    ARCH_ADA_LOVELACE, // rtx 40xx  (ad1xx)
    ARCH_BLACKWELL,    // rtx 50xx  (gb2xx)
};

enum GpuMemType {
    MEM_UNKNOWN = 0,
    MEM_GDDR6,
    MEM_GDDR6X,
    MEM_GDDR7,
};

struct NvidiaGPUInfo {
    bool        detected;
    uint16_t    vendor_id;
    uint16_t    device_id;
    uint8_t     bus, device, function;  // pci bdf
    uint8_t     revision;
    NvidiaArch  arch;
    GpuMemType  mem_type;
    uint64_t    bar0;           // mmio register base (bar0)
    uint64_t    bar1;           // framebuffer base   (bar1)
    uint64_t    bar0_size;
    uint64_t    bar1_size;
    uint32_t    vram_mb;        // estimated vram in mb
    char        name[64];       // human-readable gpu name
    bool        has_2d_accel;   // PCOPY/host-blit usable
    bool        has_3d_accel;   // graphics engine usable
};

enum GpuDriverState {
    GPU_STATE_UNINITIALIZED = 0,
    GPU_STATE_DETECTED,
    GPU_STATE_BARS_MAPPED,
    GPU_STATE_INITIALIZED,
    GPU_STATE_PASSTHROUGH,      // assigned to vm via vt-d
    GPU_STATE_ERROR,
};

//  nvidiagpu  -  static driver interface
class NvidiaGPU {
public:
    static void Init();                        // scan pci bus for nvidia gpus
    static bool IsDetected();                  // gpu found?
    static GpuDriverState GetState();

    static const NvidiaGPUInfo& GetInfo();
    static const char* GetArchName();
    static const char* GetMemTypeName();

    static uint32_t PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
    static void     PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

    static uint32_t ReadReg(uint32_t offset);  // read mmio register via bar0
    static void     WriteReg(uint32_t offset, uint32_t value);

    static bool     ResetGPU();                // full gpu reset via pmc
    static bool     EnableBusMaster();         // enable pci bus mastering
    static uint32_t GetBootDisplay();          // read nv_pmc_boot_0
    static uint32_t GetVRAMSize();             // detect vram from fb regs
    static void     PollTelemetry();           // sample live fault/interrupt state
    static bool     HasFault();                // has a sampled fault pending?
    static uint32_t GetLastFaultCode();        // last sampled fault code
    static void     ClearFault();              // clear sampled fault latch

    static bool     PrepareForPassthrough();   // detach host driver, prepare for iommu
    static bool     IsPassthroughReady();

    static void     DumpInfo(char* out, int maxo);
    static void     DumpRegisters(char* out, int maxo);

    // accel capability  -  compositor calls this to decide whether to issue GPU blits
    static bool     HasHardwareAccel();

private:
    static NvidiaGPUInfo   gpu_info;
    static GpuDriverState  state;
    static uint32_t        last_fault_code;
    static bool            has_fault;

    static void     ScanPCIBus();
    static bool     ProbeDevice(uint8_t bus, uint8_t dev, uint8_t func);
    static void     IdentifyGPU();
    static uint64_t ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);
    static uint64_t GetBARSize(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);
};
