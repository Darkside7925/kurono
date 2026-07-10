#pragma once
//  kurono os - intel integrated gpu driver
//  pci device detection, bar mapping, display pipe management
//  supports gen 6 (sandy bridge) through gen 12.7+ (arrow/lunar lake)
#include "../kernel/types.h"

#define INTEL_GPU_VENDOR_ID  0x8086

enum IntelGpuGen {
    INTEL_GEN_UNKNOWN = 0,
    INTEL_GEN_6,       // sandy bridge (hd 2000/3000)
    INTEL_GEN_7,       // ivy bridge (hd 2500/4000), haswell (hd 4600/5200)
    INTEL_GEN_8,       // broadwell (hd 5500/6000/iris)
    INTEL_GEN_9,       // skylake/kaby lake/coffee lake (uhd 620/630)
    INTEL_GEN_11,      // ice lake (iris plus g7)
    INTEL_GEN_12,      // tiger lake/alder lake/raptor lake (iris xe/uhd 770)
    INTEL_GEN_12_7,    // meteor lake / arrow lake (arc gpu)
};

struct IntelDisplayPipe {
    bool    enabled;
    uint32_t width;
    uint32_t height;
    uint32_t stride;        // bytes per scanline
    uintptr_t surface_addr; // physical address of display surface
};

struct IntelGPUInfo {
    bool       detected;
    uint16_t   vendor_id;
    uint16_t   device_id;
    uint8_t    bus, device, function;
    uint8_t    revision;
    IntelGpuGen gen;
    uint64_t   bar0;           // gttmmaddr - mmio registers
    uint64_t   bar0_size;
    uint64_t   bar2;           // gmadr - graphics memory aperture
    uint64_t   bar2_size;
    uint32_t   stolen_mem_mb;  // stolen memory in mb
    char       name[64];       // human-readable name
    IntelDisplayPipe pipe_a;
    IntelDisplayPipe pipe_b;
    bool       has_2d_accel;   // hardware 2D blitter / display plane scaling
    bool       has_3d_accel;   // render/compute engine usable for compositor
};

//  intelgpu - static driver interface
class IntelGPU {
public:
    static void Init();
    static bool IsDetected();
    static const IntelGPUInfo& GetInfo();
    static const char* GetGenName();

    static uint32_t ReadReg(uint32_t offset);
    static void     WriteReg(uint32_t offset, uint32_t value);

    static bool ReadPipeState(int pipe_idx, IntelDisplayPipe* out);
    static uintptr_t GetActiveSurfaceAddr();

    static bool IsPowerWellEnabled();

    // accel capability flags for the compositor
    static bool HasHardwareAccel();
    // best-effort hardware fill into the active scanout - returns false if unsupported.
    // the dst rectangle is clamped to the active pipe's resolution.
    static bool BlitFillARGB(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    static void DumpInfo(char* out, int maxo);

private:
    static IntelGPUInfo gpu_info;

    // pci helpers
    static uint32_t PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
    static void     PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
    static uint64_t ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx);

    // internal
    static IntelGpuGen ClassifyGen(uint16_t device_id);
    static const char* IdentifyDevice(uint16_t device_id);
};
