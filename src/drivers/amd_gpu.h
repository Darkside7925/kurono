#pragma once
//  kurono os  -  amd radeon gpu driver
//  pci device detection, bar mapping, display engine identification
//  supports gcn, rdna 1/2/3 architectures (rx 6000/7000/9000 series)
#include "../kernel/types.h"

#define AMD_VENDOR_ID     0x1002

// rdna 3  -  radeon rx 7000 series (navi 3x)
#define AMD_RX_7900XTX    0x744C
#define AMD_RX_7900XT     0x7448
#define AMD_RX_7800XT     0x7480
#define AMD_RX_7700XT     0x7484
#define AMD_RX_7600       0x7422

// rdna 3.5  -  radeon rx 9000 series (navi 4x)
#define AMD_RX_9070XT     0x7560
#define AMD_RX_9070       0x7564

// rdna 2  -  radeon rx 6000 series (navi 2x)
#define AMD_RX_6950XT     0x73A5
#define AMD_RX_6900XT     0x73BF
#define AMD_RX_6800XT     0x73AF
#define AMD_RX_6800       0x73AB
#define AMD_RX_6700XT     0x73DF
#define AMD_RX_6600XT     0x73FF
#define AMD_RX_6600       0x73EF

// rdna 1  -  radeon rx 5000 series (navi 1x)
#define AMD_RX_5700XT     0x731F
#define AMD_RX_5700       0x7310
#define AMD_RX_5600XT     0x7340
#define AMD_RX_5500XT     0x7360

// gcn 5  -  vega
#define AMD_VEGA_64       0x687F
#define AMD_VEGA_56       0x6863

enum AmdArch {
    AMD_ARCH_UNKNOWN  = 0,
    AMD_ARCH_GCN3,        // fiji, tonga
    AMD_ARCH_GCN4,        // polaris (rx 480/580)
    AMD_ARCH_GCN5,        // vega
    AMD_ARCH_RDNA1,       // navi 10/14 (rx 5000)
    AMD_ARCH_RDNA2,       // navi 21-24 (rx 6000)
    AMD_ARCH_RDNA3,       // navi 31-33 (rx 7000)
    AMD_ARCH_RDNA35,      // navi 4x (rx 9000)
};

enum AmdMemType {
    AMD_MEM_UNKNOWN = 0,
    AMD_MEM_GDDR5,
    AMD_MEM_GDDR6,
    AMD_MEM_HBM2,
    AMD_MEM_HBM2E,
};

enum AmdDisplayEngine {
    DCE_UNKNOWN = 0,
    DCE_8,            // gcn 1.0-1.1
    DCE_10,           // gcn 1.2
    DCE_11,           // polaris
    DCN_1,            // vega
    DCN_2,            // rdna 1
    DCN_3,            // rdna 2
    DCN_31,           // rdna 3
    DCN_32,           // rdna 3 (navi 33)
    DCN_35,           // rdna 3.5
};

#define AMDGPU_MM_INDEX     0x0000   // indirect register access index
#define AMDGPU_MM_DATA      0x0004   // indirect register access data
#define AMDGPU_GRBM_STATUS  0x8010   // graphics block busy status
#define AMDGPU_SRBM_STATUS  0x0E50   // system block busy status
#define AMDGPU_CP_RB_BASE   0x8040   // command processor ring base
#define AMDGPU_CP_RB_CNTL   0x8044   // command processor ring control
#define AMDGPU_RLC_CNTL     0xC300   // run list controller
#define AMDGPU_GRBM_SOFT_RESET  0x8020  // soft reset register

// display controller registers (dcn)
#define DCN_HUBP0_CONTROL   0x0E00
#define DCN_DPP0_CONTROL    0x0F00
#define DCN_OPP0_CONTROL    0x1000
#define DCN_OPTC0_CONTROL   0x1100

// power management
#define AMDGPU_SMC_MSG      0x0390   // smu message register
#define AMDGPU_SMC_RESP     0x0394   // smu response register
#define AMDGPU_SMC_ARG      0x0398   // smu argument register

struct AmdGPUInfo {
    bool             detected;
    uint16_t         vendor_id;
    uint16_t         device_id;
    uint8_t          bus, device, function;   // pci bdf
    uint8_t          revision;
    AmdArch          arch;
    AmdMemType       mem_type;
    AmdDisplayEngine display_engine;
    uint64_t         bar0;             // mmio base (bar0)
    uint64_t         bar0_size;        // bar0 region size
    uint64_t         vram_bar;         // vram aperture bar (bar2 or resizable)
    uint64_t         vram_size;        // detected vram size
    int              compute_units;    // cu count
    int              stream_processors;// sp count
    int              max_clock_mhz;    // maximum engine clock
    int              memory_clock_mhz; // memory clock
    int              memory_bus_width;  // bits (128/192/256/384)
    char             name[64];         // human-readable name
    char             chip_name[16];    // chip codename (e.g., "navi31")
    bool             resizable_bar;    // rebar / sam enabled
    bool             hardware_raytracing; // ray accelerator present
    bool             has_2d_accel;     // dcn/dce can do hardware blits
    bool             has_3d_accel;     // GFX engine usable for compositor
};

class AmdGPU {
public:
    // initialization  -  pci scan, bar mapping
    static bool Init();
    static bool IsAvailable();

    // gpu info
    static const AmdGPUInfo& GetInfo();
    static const char* GetArchName();
    static const char* GetDisplayEngineName();

    // register access
    static uint32_t ReadReg(uint32_t offset);
    static void     WriteReg(uint32_t offset, uint32_t value);
    static uint32_t ReadRegIdx(uint32_t offset);  // indirect access
    static void     WriteRegIdx(uint32_t offset, uint32_t value);

    // gpu reset
    static bool SoftReset();
    static bool GFXReset();

    // display engine
    static bool InitDisplay(int width, int height);
    static bool SetResolution(int width, int height, int refresh);
    static int  GetMaxRefreshRate();

    // power management
    static int  GetGPUTemperature();   // celsius
    static int  GetFanSpeedPct();      // 0-100%
    static int  GetPowerDraw();        // watts
    static bool SetPowerProfile(int profile);  // 0=balanced, 1=perf, 2=powersave

    // vram management
    static uint64_t GetVRAMTotal();
    static uint64_t GetVRAMUsed();
    static uint64_t GetVRAMFree();

    // debug
    static void DumpRegisters(char* buf, int max_len);

    // accel capability  -  compositor checks this before issuing GPU work
    static bool HasHardwareAccel();

private:
    static AmdGPUInfo info;
    static volatile uint32_t* mmio_base;

    static bool ScanPCI();
    static bool MapBAR();
    static AmdArch IdentifyArch(uint16_t device_id);
    static AmdMemType IdentifyMemType(AmdArch arch);
    static AmdDisplayEngine IdentifyDisplayEngine(AmdArch arch);
    static const char* IdentifyName(uint16_t device_id);
    static const char* IdentifyChip(uint16_t device_id);
    static int IdentifyCUs(uint16_t device_id, AmdArch arch);
    static int IdentifyBusWidth(uint16_t device_id, AmdArch arch);
    static void DetectVRAM();
    static void DetectClocks();
};
