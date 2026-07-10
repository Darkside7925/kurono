#pragma once
//  kurono os - virtio gpu driver
//  implements virtio gpu specification for 2d/3d accelerated rendering
//  used for guest ↔ host display bridge in virtualized environments
#include "../kernel/types.h"

#define VIRTIO_VENDOR_ID        0x1AF4
#define VIRTIO_GPU_DEVICE_ID    0x1050  // virtio gpu (modern)
#define VIRTIO_GPU_LEGACY_ID    0x1040  // virtio transitional base + 0x10

#define VIRTIO_PCI_CAP_COMMON   1
#define VIRTIO_PCI_CAP_NOTIFY   2
#define VIRTIO_PCI_CAP_ISR      3
#define VIRTIO_PCI_CAP_DEVICE   4
#define VIRTIO_PCI_CAP_PCI      5

#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FAILED       128

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET             0x0109
#define VIRTIO_GPU_CMD_GET_EDID               0x010A

// 3d commands
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207

// cursor commands
#define VIRTIO_GPU_CMD_UPDATE_CURSOR           0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR             0x0301

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103
#define VIRTIO_GPU_RESP_OK_EDID                0x1104
#define VIRTIO_GPU_RESP_ERR_UNSPEC             0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY      0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT    0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE   0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT    0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER  0x1205

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM  3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM  4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM  67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM  68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM  121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM  134

#define VIRTIO_GPU_MAX_SCANOUTS  16

struct VirtGPU_CtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct VirtGPU_Rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct VirtGPU_DisplayOne {
    VirtGPU_Rect rect;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct VirtGPU_DisplayInfo {
    VirtGPU_CtrlHdr hdr;
    VirtGPU_DisplayOne pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct VirtGPU_ResourceCreate2D {
    VirtGPU_CtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct VirtGPU_MemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct VirtGPU_ResourceAttachBacking {
    VirtGPU_CtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // followed by virtgpu_mementry array
} __attribute__((packed));

struct VirtGPU_SetScanout {
    VirtGPU_CtrlHdr hdr;
    VirtGPU_Rect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct VirtGPU_TransferToHost2D {
    VirtGPU_CtrlHdr hdr;
    VirtGPU_Rect rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct VirtGPU_ResourceFlush {
    VirtGPU_CtrlHdr hdr;
    VirtGPU_Rect rect;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct VirtQueueDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2
#define VIRTQ_DESC_F_INDIRECT 4

struct VirtQueueAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct VirtQueueUsedElem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct VirtQueueUsed {
    uint16_t flags;
    uint16_t idx;
    VirtQueueUsedElem ring[];
} __attribute__((packed));

#define VIRTQ_SIZE 64

struct VirtQueue {
    VirtQueueDesc*  desc;
    VirtQueueAvail* avail;
    VirtQueueUsed*  used;
    uint16_t size;
    uint16_t free_head;
    uint16_t last_used_idx;
    uint16_t num_free;
};

class VirtIOGPU {
public:
    static bool Init();
    static bool IsDetected();

    // display info
    static bool GetDisplayInfo(int scanout, uint32_t* width, uint32_t* height);

    // 2d resource management
    static uint32_t CreateResource2D(uint32_t width, uint32_t height, uint32_t format);
    static bool AttachBacking(uint32_t resource_id, void* buffer, uint32_t size);
    static bool SetScanout(int scanout_id, uint32_t resource_id,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    static bool TransferToHost2D(uint32_t resource_id,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    static bool Flush(uint32_t resource_id,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    static bool DestroyResource(uint32_t resource_id);

    // cursor
    static bool UpdateCursor(uint32_t scanout, uint32_t resource_id,
                            uint32_t hot_x, uint32_t hot_y);
    static bool MoveCursor(uint32_t scanout, uint32_t x, uint32_t y);

    // full framebuffer update (convenience)
    static bool PresentFramebuffer(void* pixels, uint32_t width, uint32_t height);

    static void DumpInfo(char* out, int max_len);

private:
    static bool detected;
    static volatile uint8_t* bar0;
    static volatile uint8_t* notify_base;
    static uint32_t notify_off_multiplier;

    // virtqueues (0=controlq, 1=cursorq)
    static VirtQueue controlq;
    static VirtQueue cursorq;

    // display state
    static uint32_t display_width;
    static uint32_t display_height;
    static uint32_t next_resource_id;
    static uint32_t fb_resource_id;

    // pci location
    static uint8_t pci_bus, pci_dev, pci_func;

    static bool InitVirtQueue(int index, VirtQueue* vq);
    static bool SendControlCommand(void* cmd, uint32_t cmd_size,
                                   void* resp, uint32_t resp_size);
    static void NotifyQueue(int index);

    // pci config space helpers
    static uint32_t PciRead32(uint8_t offset);
    static void PciWrite32(uint8_t offset, uint32_t val);
    static uint16_t PciRead16(uint8_t offset);
    static uint8_t PciRead8(uint8_t offset);
};
