//  kurono os - virtio-gpu host device emulator
//
//  presents a virtio 1.0+ pci gpu to the guest vm so the alpine/debian
//  guest can load its in-tree virtio_gpu drm driver and render to the
//  kurono compositor.
//
//  layout (single-bar modern virtio):
//      bar4 (mmio, 64-bit, prefetchable, 64 kb total)
//          0x0000 - 0x0FFF  common cfg     (virtio_pci_common_cfg)
//          0x1000 - 0x1FFF  isr cfg        (1 byte)
//          0x2000 - 0x2FFF  device cfg     (virtio_gpu_config)
//          0x3000 - 0x3FFF  notify region  (per-queue doorbells)
//
//  pci capabilities chained in cfg space at offset 0x40 describe the
//  bar offsets to the linux virtio_pci modern driver.
//
//  supported virtio-gpu commands (subset adequate for fbdev/drm):
//      VIRTIO_GPU_CMD_GET_DISPLAY_INFO
//      VIRTIO_GPU_CMD_RESOURCE_CREATE_2D
//      VIRTIO_GPU_CMD_RESOURCE_UNREF
//      VIRTIO_GPU_CMD_SET_SCANOUT
//      VIRTIO_GPU_CMD_RESOURCE_FLUSH
//      VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D
//      VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING
//      VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING
//      VIRTIO_GPU_CMD_GET_EDID
#pragma once
#include "../kernel/types.h"
#include "vpci.h"

constexpr int    VIRTIO_GPU_MAX_RES       = 32;
constexpr int    VIRTIO_GPU_MAX_SCANOUTS  = 1;     // single virtual display
constexpr int    VIRTIO_GPU_MAX_QUEUES    = 2;     // controlq + cursorq
constexpr int    VIRTIO_GPU_MAX_BACKINGS  = 16;    // sg list per resource
constexpr int    VIRTIO_GPU_BAR_SIZE      = 0x10000; // 64 kb

// vendor / device ids
constexpr uint16_t VIRTIO_VENDOR_ID       = 0x1AF4;       // red hat
constexpr uint16_t VIRTIO_GPU_DEVICE_ID   = 0x1050;       // modern: 0x1040 + dev type 16
constexpr uint16_t VIRTIO_GPU_DEVICE_TYPE = 16;           // gpu

// virtqueue split-ring constants
constexpr int VIRTQ_DESC_F_NEXT     = 1;
constexpr int VIRTQ_DESC_F_WRITE    = 2;
constexpr int VIRTQ_DESC_F_INDIRECT = 4;

struct VirtQDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct VirtQAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];   // length = queue_size
};

struct VirtQUsedElem {
    uint32_t id;       // index of head desc
    uint32_t len;      // bytes written
};

struct VirtQUsed {
    uint16_t flags;
    uint16_t idx;
    VirtQUsedElem ring[]; // length = queue_size
};

struct VirtQGpu {
    uint16_t size;             // selected queue size
    uint16_t enable;
    uint64_t desc_addr;        // guest physical
    uint64_t avail_addr;
    uint64_t used_addr;
    uint16_t last_avail_idx;   // host's view of next avail idx to consume
};

struct VGpuBacking {
    uint64_t addr;
    uint32_t length;
};

struct VGpuResource {
    bool     in_use;
    uint32_t id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint8_t* host_pixels;      // RGBA8 buffer, host heap
    uint32_t host_pixels_size; // bytes allocated for host_pixels
    VGpuBacking backings[VIRTIO_GPU_MAX_BACKINGS];
    int      backing_count;
};

struct VGpuScanout {
    uint32_t resource_id;
    uint32_t x, y, width, height;
    bool     enabled;
};

class VirtIOGPUHost {
public:
    static bool Init();              // registers a vpci device

    // called from the hypervisor tick loop - checks all virtqueues for
    // newly-submitted requests and processes them.
    static void ProcessQueues();

    // present any pending scanout to kurono compositor.  returns true if
    // the host framebuffer was actually updated (so display layer can
    // re-blit).
    static bool PresentIfDirty();

    // shell debug
    static int  GetWidth();
    static int  GetHeight();
    static int  ResourceCount();
    static uint32_t FrameCount();

    static bool IsRegistered();

private:
    static VPCIDevice         dev;
    static bool               registered;

    // device state
    static uint8_t            common_cfg[0x1000];
    static uint8_t            isr_status;
    static uint8_t            device_cfg[0x1000];
    static uint8_t            notify_region[0x1000];

    // common cfg working state
    static uint32_t           device_feature_select;
    static uint64_t           device_features;
    static uint32_t           driver_feature_select;
    static uint64_t           driver_features;
    static uint16_t           queue_select;
    static uint8_t            device_status;
    static uint16_t           config_generation;
    static uint16_t           config_msix_vector;
    static uint16_t           queue_msix_vector;
    static uint32_t           num_queues;

    // virtqueues
    static VirtQGpu           queues[VIRTIO_GPU_MAX_QUEUES];

    // resources + scanouts
    static VGpuResource       resources[VIRTIO_GPU_MAX_RES];
    static VGpuScanout        scanouts[VIRTIO_GPU_MAX_SCANOUTS];

    // present accounting
    static bool               scanout_dirty;
    static uint32_t           frame_count;

    // bar callbacks
    static bool BarRead (VPCIDevice* d, int bar, uint32_t off,
                          uint8_t size, uint32_t& value);
    static bool BarWrite(VPCIDevice* d, int bar, uint32_t off,
                          uint8_t size, uint32_t value);

    // common-cfg field i/o (offsets defined in cpp)
    static uint32_t ReadCommonCfg (uint32_t off, uint8_t size);
    static void     WriteCommonCfg(uint32_t off, uint8_t size, uint32_t value);

    // queue notify - guest tells us a virtqueue has new requests
    static void NotifyQueue(int qid);

    // process a single virtio-gpu command pulled from the controlq
    static uint32_t ProcessCommand(VirtQGpu& q, uint16_t head_desc,
                                     uint32_t* out_resp_len);

    // resource helpers
    static VGpuResource* FindRes(uint32_t id);
    static VGpuResource* AllocRes(uint32_t id);
    static void          FreeRes (VGpuResource* r);

    // copy guest-scattered backing into host_pixels for a region
    static void TransferToHost(VGpuResource* r, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h, uint64_t guest_off);

    // pull bytes from a guest sg list (resource backings) starting at offset
    static void ReadBackingBytes(VGpuResource* r, uint64_t offset,
                                   uint8_t* dst, uint32_t bytes);

    // present scanout 0 onto kurono compositor
    static void PresentScanout0();

    // capability list setup
    static void BuildPCICapabilities();
};
