#pragma once
//  kurono os - gpu probe & hybrid gpu support
//  early pci scan for all display controllers (intel igpu, nvidia dgpu,
//  amd apu/dgpu). detects optimus/powerxpress hybrid setups and identifies
//  which gpu owns the display panel.
//
//  critical for laptops:
//  - nvidia optimus (muxless): igpu drives panel, grub fb → igpu stolen mem
//  - amd powerxpress: apu drives panel, discrete gpu is offload-only
//  - advanced optimus / mux: either gpu may drive panel
//  - single-gpu desktops: one vga controller, straightforward
//
//  this module must run before graphics::init so we can validate/fix the
//  framebuffer address that multiboot/grub reported.
#include "../kernel/types.h"

// pci class codes for display controllers
#define PCI_CLASS_VGA_COMPATIBLE   0x0300   // vga-compatible controller
#define PCI_CLASS_XGA_CONTROLLER   0x0301   // xga controller
#define PCI_CLASS_3D_CONTROLLER    0x0302   // 3d controller (not vga)
#define PCI_CLASS_DISPLAY_OTHER    0x0380   // other display controller

// gpu vendor ids
#define GPU_VENDOR_INTEL   0x8086
#define GPU_VENDOR_NVIDIA  0x10DE
#define GPU_VENDOR_AMD     0x1002
#define GPU_VENDOR_VMWARE  0x15AD  // vmware svga
#define GPU_VENDOR_QEMU    0x1234  // qemu/bochs vga
#define GPU_VENDOR_VIRTIO  0x1AF4  // virtio gpu
#define GPU_VENDOR_REDHAT  0x1B36  // red hat qxl

// maximum gpus we track
#define GPU_PROBE_MAX  8

// gpu topology role
enum GpuRole {
    GPU_ROLE_UNKNOWN = 0,
    GPU_ROLE_PRIMARY,      // drives the display panel (gets the framebuffer)
    GPU_ROLE_SECONDARY,    // offload-only (nvidia in optimus, amd dgpu in hybrid)
    GPU_ROLE_VIRTUAL,      // virtual/emulated gpu (qemu, vmware, etc.)
};

// hybrid gpu topology
enum GpuTopology {
    GPU_TOPO_SINGLE = 0,         // single gpu (desktop or single-gpu laptop)
    GPU_TOPO_OPTIMUS_MUXLESS,    // intel igpu + nvidia dgpu, no mux (panel → igpu only)
    GPU_TOPO_OPTIMUS_MUX,        // intel + nvidia with mux switch
    GPU_TOPO_POWERXPRESS,        // amd apu + amd/nvidia discrete
    GPU_TOPO_DUAL_DISCRETE,      // two discrete gpus (workstation)
    GPU_TOPO_VIRTUAL,            // running under hypervisor with virtual gpu
};

// per-gpu information discovered during pci scan
struct GpuInfo {
    bool        present;
    uint16_t    vendor_id;
    uint16_t    device_id;
    uint8_t     bus, device, function;
    uint8_t     base_class;      // 0x03
    uint8_t     sub_class;       // 0x00=vga, 0x02=3d, etc.
    uint8_t     prog_if;
    uint64_t    bar0;            // mmio registers (intel: gttmmaddr, nv: nv mmio)
    uint64_t    bar0_size;
    uint64_t    bar2;            // vram aperture (intel: gmadr, nv: bar1)
    uint64_t    bar2_size;
    GpuRole     role;
    bool        is_igpu;         // true for intel integrated / amd apu
    bool        is_vga;          // sub_class == 0x00 (vga-compatible)
    char        desc[48];        // human-readable description
};

// probe result - full system gpu inventory
struct GpuProbeResult {
    GpuInfo     gpus[GPU_PROBE_MAX];
    int         count;
    int         primary_idx;     // index of the gpu driving the display (-1 if unknown)
    GpuTopology topology;

    // framebuffer validation
    bool        fb_validated;
    uintptr_t   validated_fb_addr;  // corrected fb address (or same if ok)
};

//  intel igpu display surface register offsets
//  used to read/verify the active framebuffer address from intel gpu mmio.
//  these are exposed via pci bar0 (gttmmaddr).

// pipe a display plane registers (gen 4+)
#define INTEL_DSPCNTR_A     0x70180  // display plane a control
#define INTEL_DSPSTRIDE_A   0x70188  // display plane a stride
#define INTEL_DSPSURF_A     0x7019C  // display plane a surface base address
#define INTEL_DSPOFFSET_A   0x701A4  // display plane a offset

// pipe b display plane registers
#define INTEL_DSPCNTR_B     0x71180
#define INTEL_DSPSTRIDE_B   0x71188
#define INTEL_DSPSURF_B     0x7119C

// skl+ (gen9+) universal plane registers - pipe a, plane 0
#define INTEL_PLANE_CTL_A    0x70180  // plane control
#define INTEL_PLANE_STRIDE_A 0x70188  // plane stride (in 64b units)
#define INTEL_PLANE_SURF_A   0x7019C  // plane surface address (bits 31:12 → phys addr)
#define INTEL_PLANE_SIZE_A   0x70190  // plane size (height:16 | width:16)

// pipe config
#define INTEL_PIPECONF_A    0x70008  // pipe a configuration
#define INTEL_PIPECONF_B    0x71008

// framebuffer compression (fbc)
#define INTEL_FBC_CTL       0x43208

// power management / display power
#define INTEL_PWR_WELL_CTL  0x45400

class GpuProbe {
public:
    // full pci scan for display controllers. call once at early boot.
    static void ScanAll();

    // get the probe result
    static const GpuProbeResult& GetResult();

    // validate a multiboot-provided framebuffer address against the actual
    // hardware. returns the correct fb address (may differ on optimus).
    static uintptr_t ValidateFramebuffer(uintptr_t mb_fb_addr, uint32_t width,
                                         uint32_t height, uint32_t pitch, uint8_t bpp);

    // log all discovered gpus to serial output
    static void LogAll();

    // log diagnostic info to the early framebuffer text renderer
    static void LogToEarlyFB();

    // check topology
    static bool IsOptimus();       // intel igpu + nvidia dgpu
    static bool IsPowerXpress();   // amd apu + discrete gpu
    static bool IsHybrid();        // any multi-gpu laptop topology
    static bool HasIntelIGPU();
    static bool HasNvidiaGPU();
    static bool HasAmdGPU();
    static int  GetPrimaryGpuIndex();

private:
    static GpuProbeResult result;

    // pci helpers (duplicated per-module to avoid cross-driver deps)
    static uint32_t PciRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
    static void     PciWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
    static uint64_t ReadBAR(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx);
    static uint64_t GetBARSize(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx);

    // intel igpu-specific
    static uint32_t IntelReadMMIO(uint64_t bar0, uint32_t offset);
    static uintptr_t IntelGetActiveSurface(uint64_t bar0);
    static const char* IntelIdentify(uint16_t device_id);

    // classification
    static void ClassifyTopology();
    static void AssignRoles();
    static const char* VendorName(uint16_t vid);
};
