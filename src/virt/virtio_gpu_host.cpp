//  kurono os - virtio-gpu host device implementation
//
//  see virtio_gpu_host.h for design.
#include "virtio_gpu_host.h"
#include "guest_mem.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"
#include "../linux/linux_devices.h"

// ----------------------------------------------------------------- statics
VPCIDevice    VirtIOGPUHost::dev;
bool          VirtIOGPUHost::registered = false;
uint8_t       VirtIOGPUHost::common_cfg [0x1000];
uint8_t       VirtIOGPUHost::isr_status = 0;
uint8_t       VirtIOGPUHost::device_cfg [0x1000];
uint8_t       VirtIOGPUHost::notify_region[0x1000];

uint32_t      VirtIOGPUHost::device_feature_select = 0;
uint64_t      VirtIOGPUHost::device_features       = 0;
uint32_t      VirtIOGPUHost::driver_feature_select = 0;
uint64_t      VirtIOGPUHost::driver_features       = 0;
uint16_t      VirtIOGPUHost::queue_select          = 0;
uint8_t       VirtIOGPUHost::device_status         = 0;
uint16_t      VirtIOGPUHost::config_generation     = 0;
uint16_t      VirtIOGPUHost::config_msix_vector    = 0xFFFF;
uint16_t      VirtIOGPUHost::queue_msix_vector     = 0xFFFF;
uint32_t      VirtIOGPUHost::num_queues            = VIRTIO_GPU_MAX_QUEUES;

VirtQGpu      VirtIOGPUHost::queues   [VIRTIO_GPU_MAX_QUEUES];
VGpuResource  VirtIOGPUHost::resources[VIRTIO_GPU_MAX_RES];
VGpuScanout   VirtIOGPUHost::scanouts [VIRTIO_GPU_MAX_SCANOUTS];
bool          VirtIOGPUHost::scanout_dirty = false;
uint32_t      VirtIOGPUHost::frame_count   = 0;

// ----------------------------------------------------------------- spec defs

// virtio_pci_common_cfg field offsets (per virtio 1.0 spec, sec 4.1.4.3)
constexpr uint32_t VPCC_DEVICE_FEATURE_SELECT  = 0x00; // 4
constexpr uint32_t VPCC_DEVICE_FEATURE         = 0x04; // 4
constexpr uint32_t VPCC_DRIVER_FEATURE_SELECT  = 0x08; // 4
constexpr uint32_t VPCC_DRIVER_FEATURE         = 0x0C; // 4
constexpr uint32_t VPCC_CONFIG_MSIX_VECTOR     = 0x10; // 2
constexpr uint32_t VPCC_NUM_QUEUES             = 0x12; // 2
constexpr uint32_t VPCC_DEVICE_STATUS          = 0x14; // 1
constexpr uint32_t VPCC_CONFIG_GENERATION      = 0x15; // 1
constexpr uint32_t VPCC_QUEUE_SELECT           = 0x16; // 2
constexpr uint32_t VPCC_QUEUE_SIZE             = 0x18; // 2
constexpr uint32_t VPCC_QUEUE_MSIX_VECTOR      = 0x1A; // 2
constexpr uint32_t VPCC_QUEUE_ENABLE           = 0x1C; // 2
constexpr uint32_t VPCC_QUEUE_NOTIFY_OFF       = 0x1E; // 2
constexpr uint32_t VPCC_QUEUE_DESC_LO          = 0x20; // 4
constexpr uint32_t VPCC_QUEUE_DESC_HI          = 0x24; // 4
constexpr uint32_t VPCC_QUEUE_AVAIL_LO         = 0x28; // 4
constexpr uint32_t VPCC_QUEUE_AVAIL_HI         = 0x2C; // 4
constexpr uint32_t VPCC_QUEUE_USED_LO          = 0x30; // 4
constexpr uint32_t VPCC_QUEUE_USED_HI          = 0x34; // 4

// bar4 layout
constexpr uint32_t BAR_COMMON_OFF   = 0x0000;
constexpr uint32_t BAR_COMMON_LEN   = 0x0100;
constexpr uint32_t BAR_ISR_OFF      = 0x1000;
constexpr uint32_t BAR_ISR_LEN      = 0x0001;
constexpr uint32_t BAR_DEVCFG_OFF   = 0x2000;
constexpr uint32_t BAR_DEVCFG_LEN   = 0x0010;
constexpr uint32_t BAR_NOTIFY_OFF   = 0x3000;
constexpr uint32_t BAR_NOTIFY_LEN   = 0x1000;
constexpr uint32_t BAR_NOTIFY_OFF_MULT = 4; // bytes between queue notify slots

// virtio-gpu device cfg (sec 5.7.4)
struct VirtIOGPUConfig {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};

// virtio-gpu commands (sec 5.7.6.7)
enum VirtIOGPUCtrlType : uint32_t {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO       = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF         = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT            = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH         = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING= 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING= 0x0107,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO        = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET             = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID               = 0x010A,
    VIRTIO_GPU_CMD_UPDATE_CURSOR          = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR            = 0x0301,
    VIRTIO_GPU_RESP_OK_NODATA             = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO       = 0x1101,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO        = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET             = 0x1103,
    VIRTIO_GPU_RESP_OK_EDID               = 0x1104,
    VIRTIO_GPU_RESP_ERR_UNSPEC            = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY     = 0x1201,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID= 0x1203,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID = 0x1204,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER = 0x1205,
};

struct VirtIOGPUCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  pad[3];
};

struct VirtIOGPURect {
    uint32_t x, y, width, height;
};

struct VirtIOGPUResourceCreate2D {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct VirtIOGPUResourceUnref {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t pad;
};

struct VirtIOGPUSetScanout {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct VirtIOGPUResourceFlush {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint32_t resource_id;
    uint32_t pad;
};

struct VirtIOGPUTransferToHost2D {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t pad;
};

struct VirtIOGPUResourceAttachBacking {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // followed by nr_entries virtio_gpu_mem_entry
};

struct VirtIOGPUMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t pad;
};

struct VirtIOGPUDisplayOne {
    VirtIOGPURect r;
    uint32_t enabled;
    uint32_t flags;
};

struct VirtIOGPURespDisplayInfo {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPUDisplayOne pmodes[16];
};

// pci capability ids (virtio)
constexpr uint8_t PCI_CAP_ID_VNDR             = 0x09;
constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG   = 1;
constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG   = 2;
constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG      = 3;
constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG   = 4;
constexpr uint8_t VIRTIO_PCI_CAP_PCI_CFG      = 5;

struct VirtioPCICap {
    uint8_t cap_vndr;        // PCI_CAP_ID_VNDR
    uint8_t cap_next;        // pointer to next cap
    uint8_t cap_len;          // sizeof(VirtioPCICap) [+ extras]
    uint8_t cfg_type;         // VIRTIO_PCI_CAP_*
    uint8_t bar;              // bar index
    uint8_t pad[3];
    uint32_t offset;          // offset within bar
    uint32_t length;          // length of structure
};

// ----------------------------------------------------------------- helpers
static inline uint32_t bytes_per_pixel_for_format(uint32_t fmt) {
    // virtio_gpu_formats: B8G8R8A8_UNORM (1), B8G8R8X8 (2), A8R8G8B8 (3),
    // X8R8G8B8 (4), R8G8B8A8 (67), X8B8G8R8 (68), A8B8G8R8 (121), R8G8B8X8 (134)
    (void)fmt;
    return 4; // all supported formats are 32 bpp
}

void VirtIOGPUHost::BuildPCICapabilities() {
    // place caps at cfg offset 0x40 onward.  Linker layout:
    //   0x40: COMMON_CFG  -> bar 4 off 0x0000 len 0x100
    //   0x50: ISR_CFG     -> bar 4 off 0x1000 len 0x001
    //   0x60: DEVICE_CFG  -> bar 4 off 0x2000 len 0x010
    //   0x70: NOTIFY_CFG  -> bar 4 off 0x3000 len 0x1000  (+ 4-byte mult)

    auto write_cap = [](uint8_t off, uint8_t next, uint8_t cfg_type,
                          uint32_t bar_off, uint32_t bar_len,
                          uint8_t extra_len /*=0*/) {
        VirtioPCICap c{};
        c.cap_vndr = PCI_CAP_ID_VNDR;
        c.cap_next = next;
        c.cap_len  = (uint8_t)(sizeof(VirtioPCICap) + extra_len);
        c.cfg_type = cfg_type;
        c.bar      = 4;
        c.offset   = bar_off;
        c.length   = bar_len;
        for (uint32_t i = 0; i < sizeof(VirtioPCICap); i++) {
            VirtIOGPUHost::dev.cfg[off + i] = ((uint8_t*)&c)[i];
        }
    };

    write_cap(0x40, 0x50, VIRTIO_PCI_CAP_COMMON_CFG,
              BAR_COMMON_OFF, BAR_COMMON_LEN, 0);
    write_cap(0x50, 0x60, VIRTIO_PCI_CAP_ISR_CFG,
              BAR_ISR_OFF, BAR_ISR_LEN, 0);
    write_cap(0x60, 0x70, VIRTIO_PCI_CAP_DEVICE_CFG,
              BAR_DEVCFG_OFF, BAR_DEVCFG_LEN, 0);
    // notify cfg has trailing notify_off_multiplier (uint32_t)
    write_cap(0x70, 0x00, VIRTIO_PCI_CAP_NOTIFY_CFG,
              BAR_NOTIFY_OFF, BAR_NOTIFY_LEN, 4);
    uint32_t mult = BAR_NOTIFY_OFF_MULT;
    for (int i = 0; i < 4; i++) {
        VirtIOGPUHost::dev.cfg[0x70 + sizeof(VirtioPCICap) + i] =
            (uint8_t)((mult >> (i * 8)) & 0xFF);
    }

    // capability list pointer in std cfg
    VirtIOGPUHost::dev.cfg[PCI_CAP_PTR] = 0x40;
    // status: capabilities bit
    VPCI::Cfg16(&VirtIOGPUHost::dev, PCI_STATUS, 0x0010);
}

// ----------------------------------------------------------------- init
bool VirtIOGPUHost::Init() {
    if (registered) return true;

    for (int i = 0; i < VIRTIO_GPU_MAX_RES; i++)     resources[i].in_use = false;
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        scanouts[i] = {};
    }
    for (int i = 0; i < VIRTIO_GPU_MAX_QUEUES; i++) {
        queues[i] = {};
        queues[i].size = 256;
    }
    for (uint32_t i = 0; i < sizeof(common_cfg); i++)    common_cfg[i] = 0;
    for (uint32_t i = 0; i < sizeof(device_cfg); i++)    device_cfg[i] = 0;
    for (uint32_t i = 0; i < sizeof(notify_region); i++) notify_region[i] = 0;

    // device cfg: 1 scanout
    VirtIOGPUConfig* cfg = (VirtIOGPUConfig*)device_cfg;
    cfg->num_scanouts = VIRTIO_GPU_MAX_SCANOUTS;
    cfg->num_capsets  = 0;

    // device features (modern virtio + VIRGL=0).  bit 32 = VIRTIO_F_VERSION_1
    device_features = (1ULL << 32);
    // VIRTIO_GPU_F_EDID = 1
    device_features |= (1ULL << 1);

    // build pci device
    dev = {};
    dev.name     = "virtio-gpu-host";
    dev.irq_line = 11;
    VPCI::Cfg16(&dev, PCI_VENDOR_ID,    VIRTIO_VENDOR_ID);
    VPCI::Cfg16(&dev, PCI_DEVICE_ID,    VIRTIO_GPU_DEVICE_ID);
    VPCI::Cfg16(&dev, PCI_COMMAND,      0); // guest sets PCI_CMD_MEM
    VPCI::Cfg8 (&dev, PCI_REVISION_ID,  1);  // virtio modern (>= 1)
    VPCI::Cfg8 (&dev, PCI_PROG_IF,      0);
    VPCI::Cfg8 (&dev, PCI_SUBCLASS,     0x00);
    VPCI::Cfg8 (&dev, PCI_CLASS,        0x03); // display controller
    VPCI::Cfg16(&dev, PCI_SUBSYS_VEND,  VIRTIO_VENDOR_ID);
    VPCI::Cfg16(&dev, PCI_SUBSYS_ID,    VIRTIO_GPU_DEVICE_TYPE);

    // bar4 is the virtio modern region (mmio, 64-bit, prefetchable, 64 kb)
    dev.bars[4].size     = VIRTIO_GPU_BAR_SIZE;
    dev.bars[4].is_mmio  = true;
    dev.bars[4].is_64bit = true;
    dev.bars[4].prefetch = true;

    dev.bar_read   = &VirtIOGPUHost::BarRead;
    dev.bar_write  = &VirtIOGPUHost::BarWrite;
    dev.cfg_notify = nullptr;

    BuildPCICapabilities();

    int slot = VPCI::RegisterDevice(&dev);
    if (slot < 0) {
        SerialLogger::Log("VirtIOGPUHost: failed to register pci device\r\n");
        return false;
    }
    // VPCI::RegisterDevice copies the device into its slot - pick that up
    // so subsequent state queries reflect the canonical instance.
    VPCIDevice* canonical = VPCI::GetDevice(slot);
    if (canonical) {
        // re-point our dev struct to the canonical slot for cfg fields the
        // bar callbacks may want to read.  Note: bars[].base on `dev` is
        // unused after registration; the real values live on `canonical`.
        dev = *canonical;
    }

    registered = true;
    SerialLogger::Log("VirtIOGPUHost: ready (1 scanout, ");
    SerialLogger::LogDec(VIRTIO_GPU_MAX_RES);
    SerialLogger::Log(" resources, bar4=");
    SerialLogger::LogHex((uint32_t)dev.bars[4].base);
    SerialLogger::Log(")\r\n");
    return true;
}

bool VirtIOGPUHost::IsRegistered() { return registered; }
int  VirtIOGPUHost::GetWidth()      { return 1024; }
int  VirtIOGPUHost::GetHeight()     { return 768;  }
uint32_t VirtIOGPUHost::FrameCount(){ return frame_count; }
int  VirtIOGPUHost::ResourceCount() {
    int n = 0;
    for (int i = 0; i < VIRTIO_GPU_MAX_RES; i++) if (resources[i].in_use) n++;
    return n;
}

// ----------------------------------------------------------------- bar i/o

bool VirtIOGPUHost::BarRead(VPCIDevice*, int bar, uint32_t off,
                              uint8_t size, uint32_t& value) {
    if (bar != 4) return false;
    value = 0;

    if (off >= BAR_COMMON_OFF && off < BAR_COMMON_OFF + BAR_COMMON_LEN) {
        value = ReadCommonCfg(off - BAR_COMMON_OFF, size);
        return true;
    }
    if (off >= BAR_ISR_OFF && off < BAR_ISR_OFF + BAR_ISR_LEN) {
        value = isr_status;
        isr_status = 0; // read-clear
        return true;
    }
    if (off >= BAR_DEVCFG_OFF && off < BAR_DEVCFG_OFF + BAR_DEVCFG_LEN) {
        uint32_t doff = off - BAR_DEVCFG_OFF;
        for (int i = 0; i < size && doff + i < BAR_DEVCFG_LEN; i++) {
            value |= ((uint32_t)device_cfg[doff + i]) << (i * 8);
        }
        return true;
    }
    if (off >= BAR_NOTIFY_OFF && off < BAR_NOTIFY_OFF + BAR_NOTIFY_LEN) {
        // notify region reads return 0
        return true;
    }
    return true; // swallow
}

bool VirtIOGPUHost::BarWrite(VPCIDevice*, int bar, uint32_t off,
                               uint8_t size, uint32_t value) {
    if (bar != 4) return false;

    if (off >= BAR_COMMON_OFF && off < BAR_COMMON_OFF + BAR_COMMON_LEN) {
        WriteCommonCfg(off - BAR_COMMON_OFF, size, value);
        return true;
    }
    if (off >= BAR_ISR_OFF && off < BAR_ISR_OFF + BAR_ISR_LEN) {
        isr_status = (uint8_t)(value & 0xFF);
        return true;
    }
    if (off >= BAR_DEVCFG_OFF && off < BAR_DEVCFG_OFF + BAR_DEVCFG_LEN) {
        uint32_t doff = off - BAR_DEVCFG_OFF;
        for (int i = 0; i < size && doff + i < BAR_DEVCFG_LEN; i++) {
            device_cfg[doff + i] = (uint8_t)((value >> (i * 8)) & 0xFF);
        }
        return true;
    }
    if (off >= BAR_NOTIFY_OFF && off < BAR_NOTIFY_OFF + BAR_NOTIFY_LEN) {
        // notify_off_multiplier is 4 - qid = (off - notify_off) / 4
        uint32_t qid = (off - BAR_NOTIFY_OFF) / BAR_NOTIFY_OFF_MULT;
        if (qid < (uint32_t)num_queues) {
            NotifyQueue((int)qid);
        }
        return true;
    }
    return true; // swallow
}

// ----------------------------------------------------------------- common cfg

uint32_t VirtIOGPUHost::ReadCommonCfg(uint32_t off, uint8_t size) {
    auto extract = [&](uint64_t v) -> uint32_t {
        if (size == 1) return (uint32_t)(v & 0xFF);
        if (size == 2) return (uint32_t)(v & 0xFFFF);
        return (uint32_t)v;
    };

    switch (off) {
        case VPCC_DEVICE_FEATURE_SELECT: return device_feature_select;
        case VPCC_DEVICE_FEATURE: {
            uint32_t hi = (uint32_t)(device_features >> 32);
            uint32_t lo = (uint32_t)(device_features & 0xFFFFFFFFu);
            return device_feature_select ? hi : lo;
        }
        case VPCC_DRIVER_FEATURE_SELECT: return driver_feature_select;
        case VPCC_DRIVER_FEATURE: {
            uint32_t hi = (uint32_t)(driver_features >> 32);
            uint32_t lo = (uint32_t)(driver_features & 0xFFFFFFFFu);
            return driver_feature_select ? hi : lo;
        }
        case VPCC_CONFIG_MSIX_VECTOR: return extract(config_msix_vector);
        case VPCC_NUM_QUEUES:         return extract(num_queues);
        case VPCC_DEVICE_STATUS:      return extract(device_status);
        case VPCC_CONFIG_GENERATION:  return extract(config_generation);
        case VPCC_QUEUE_SELECT:       return extract(queue_select);
        case VPCC_QUEUE_SIZE:         {
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return extract(queues[queue_select].size);
            return 0;
        }
        case VPCC_QUEUE_MSIX_VECTOR:  return extract(queue_msix_vector);
        case VPCC_QUEUE_ENABLE:       {
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return extract(queues[queue_select].enable);
            return 0;
        }
        case VPCC_QUEUE_NOTIFY_OFF:   return extract(queue_select);
        case VPCC_QUEUE_DESC_LO:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return (uint32_t)(queues[queue_select].desc_addr & 0xFFFFFFFFu);
            return 0;
        case VPCC_QUEUE_DESC_HI:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return (uint32_t)(queues[queue_select].desc_addr >> 32);
            return 0;
        case VPCC_QUEUE_AVAIL_LO:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return (uint32_t)(queues[queue_select].avail_addr & 0xFFFFFFFFu);
            return 0;
        case VPCC_QUEUE_AVAIL_HI:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return (uint32_t)(queues[queue_select].avail_addr >> 32);
            return 0;
        case VPCC_QUEUE_USED_LO:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return (uint32_t)(queues[queue_select].used_addr & 0xFFFFFFFFu);
            return 0;
        case VPCC_QUEUE_USED_HI:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                return (uint32_t)(queues[queue_select].used_addr >> 32);
            return 0;
    }
    return 0;
}

void VirtIOGPUHost::WriteCommonCfg(uint32_t off, uint8_t size, uint32_t value) {
    (void)size;
    switch (off) {
        case VPCC_DEVICE_FEATURE_SELECT: device_feature_select = value; break;
        case VPCC_DRIVER_FEATURE_SELECT: driver_feature_select = value; break;
        case VPCC_DRIVER_FEATURE: {
            uint64_t mask = ((uint64_t)0xFFFFFFFFu) <<
                            (driver_feature_select ? 32 : 0);
            uint64_t v    = ((uint64_t)value) <<
                            (driver_feature_select ? 32 : 0);
            driver_features = (driver_features & ~mask) | v;
            break;
        }
        case VPCC_CONFIG_MSIX_VECTOR: config_msix_vector = (uint16_t)value; break;
        case VPCC_DEVICE_STATUS:      device_status = (uint8_t)(value & 0xFF); break;
        case VPCC_QUEUE_SELECT:       queue_select  = (uint16_t)value; break;
        case VPCC_QUEUE_SIZE:         {
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].size = (uint16_t)value;
            break;
        }
        case VPCC_QUEUE_MSIX_VECTOR:  queue_msix_vector = (uint16_t)value; break;
        case VPCC_QUEUE_ENABLE:       {
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].enable = (uint16_t)value;
            break;
        }
        case VPCC_QUEUE_DESC_LO:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].desc_addr =
                    (queues[queue_select].desc_addr & 0xFFFFFFFF00000000ULL) | value;
            break;
        case VPCC_QUEUE_DESC_HI:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].desc_addr =
                    (queues[queue_select].desc_addr & 0x00000000FFFFFFFFULL) |
                    ((uint64_t)value << 32);
            break;
        case VPCC_QUEUE_AVAIL_LO:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].avail_addr =
                    (queues[queue_select].avail_addr & 0xFFFFFFFF00000000ULL) | value;
            break;
        case VPCC_QUEUE_AVAIL_HI:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].avail_addr =
                    (queues[queue_select].avail_addr & 0x00000000FFFFFFFFULL) |
                    ((uint64_t)value << 32);
            break;
        case VPCC_QUEUE_USED_LO:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].used_addr =
                    (queues[queue_select].used_addr & 0xFFFFFFFF00000000ULL) | value;
            break;
        case VPCC_QUEUE_USED_HI:
            if (queue_select < VIRTIO_GPU_MAX_QUEUES)
                queues[queue_select].used_addr =
                    (queues[queue_select].used_addr & 0x00000000FFFFFFFFULL) |
                    ((uint64_t)value << 32);
            break;
    }
}

// ----------------------------------------------------------------- queues

void VirtIOGPUHost::NotifyQueue(int qid) {
    if (qid < 0 || qid >= VIRTIO_GPU_MAX_QUEUES) return;
    VirtQGpu& q = queues[qid];
    if (!q.enable || !q.desc_addr || !q.avail_addr || !q.used_addr) return;

    // read avail.idx
    uint16_t avail_idx = 0;
    GuestMemoryManager::ReadGuestPhys(q.avail_addr + 2, &avail_idx, 2);

    while (q.last_avail_idx != avail_idx) {
        // ring entry @ avail_addr + 4 + (last_avail_idx % size) * 2
        uint16_t head = 0;
        uint64_t ring_off = q.avail_addr + 4 +
                            ((uint64_t)(q.last_avail_idx % q.size)) * 2;
        GuestMemoryManager::ReadGuestPhys(ring_off, &head, 2);

        uint32_t resp_len = 0;
        if (qid == 0) {
            resp_len = ProcessCommand(q, head, &resp_len);
        }
        // cursorq (qid=1): consume but do nothing useful

        // write used ring entry
        uint64_t used_elem_off = q.used_addr + 4 +
                                  ((uint64_t)(q.last_avail_idx % q.size)) * 8;
        VirtQUsedElem ue;
        ue.id  = head;
        ue.len = resp_len;
        GuestMemoryManager::WriteGuestPhys(used_elem_off, &ue, sizeof(ue));

        q.last_avail_idx++;
        // bump used.idx
        GuestMemoryManager::WriteGuestPhys(q.used_addr + 2, &q.last_avail_idx, 2);
    }

    // raise virtio isr (used buffer notification = bit 0)
    isr_status |= 0x01;
}

// ----------------------------------------------------------------- cmd dispatch

VGpuResource* VirtIOGPUHost::FindRes(uint32_t id) {
    for (int i = 0; i < VIRTIO_GPU_MAX_RES; i++) {
        if (resources[i].in_use && resources[i].id == id) return &resources[i];
    }
    return nullptr;
}
VGpuResource* VirtIOGPUHost::AllocRes(uint32_t id) {
    if (FindRes(id)) return nullptr;
    for (int i = 0; i < VIRTIO_GPU_MAX_RES; i++) {
        if (!resources[i].in_use) {
            resources[i] = {};
            resources[i].in_use = true;
            resources[i].id = id;
            return &resources[i];
        }
    }
    return nullptr;
}
void VirtIOGPUHost::FreeRes(VGpuResource* r) {
    if (!r) return;
    if (r->host_pixels) {
        KernelHeap::Free(r->host_pixels);
        r->host_pixels = nullptr;
    }
    r->in_use = false;
}

void VirtIOGPUHost::ReadBackingBytes(VGpuResource* r, uint64_t offset,
                                       uint8_t* dst, uint32_t bytes) {
    uint32_t written = 0;
    uint64_t cursor = 0;
    for (int i = 0; i < r->backing_count && written < bytes; i++) {
        uint64_t entry_start = cursor;
        uint64_t entry_end   = cursor + r->backings[i].length;
        if (offset + written >= entry_end) {
            cursor = entry_end;
            continue;
        }
        uint64_t pos = offset + written;
        uint64_t skip = (pos > entry_start) ? (pos - entry_start) : 0;
        uint64_t avail = r->backings[i].length - skip;
        uint32_t take = bytes - written;
        if ((uint64_t)take > avail) take = (uint32_t)avail;
        GuestMemoryManager::ReadGuestPhys(r->backings[i].addr + skip,
                                            dst + written, take);
        written += take;
        cursor = entry_end;
    }
}

void VirtIOGPUHost::TransferToHost(VGpuResource* r, uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h, uint64_t guest_off) {
    if (!r || !r->host_pixels) return;
    // reject out-of-range / overflowing rects up front (64-bit to dodge
    // wraparound). without this a guest x >= width made (width - x) underflow
    // below into a huge `copy`, smashing host_pixels past its end (oob write).
    // (satoru)
    if (x >= r->width || y >= r->height ||
        (uint64_t)x + w > r->width || (uint64_t)y + h > r->height) {
        return;
    }
    uint32_t bpp = bytes_per_pixel_for_format(r->format);
    uint32_t row_bytes = w * bpp;
    for (uint32_t row = 0; row < h; row++) {
        if (y + row >= r->height) break;
        uint64_t src = guest_off + (uint64_t)row * r->width * bpp;
        uint8_t* dst = r->host_pixels + ((y + row) * r->width + x) * bpp;
        uint32_t copy = row_bytes;
        if (x + w > r->width) copy = (r->width - x) * bpp;
        ReadBackingBytes(r, src, dst, copy);
    }
}

// walk a chained desc list starting at `head`.  collect in-pointers into
// `in_addrs/in_lens` and out-pointers separately.  bound by 16 entries.
struct DescChain {
    uint64_t in_addr [16]; uint32_t in_len [16]; int n_in  = 0;
    uint64_t out_addr[16]; uint32_t out_len[16]; int n_out = 0;
};

static void walk_chain(VirtQGpu& q, uint16_t head, DescChain& dc) {
    uint16_t idx = head;
    for (int safety = 0; safety < 64; safety++) {
        VirtQDesc d{};
        uint64_t off = q.desc_addr + (uint64_t)idx * 16;
        GuestMemoryManager::ReadGuestPhys(off, &d, sizeof(d));
        bool is_write = (d.flags & VIRTQ_DESC_F_WRITE) != 0;
        if (is_write) {
            if (dc.n_out < 16) {
                dc.out_addr[dc.n_out] = d.addr;
                dc.out_len [dc.n_out] = d.len;
                dc.n_out++;
            }
        } else {
            if (dc.n_in < 16) {
                dc.in_addr[dc.n_in] = d.addr;
                dc.in_len [dc.n_in] = d.len;
                dc.n_in++;
            }
        }
        if (!(d.flags & VIRTQ_DESC_F_NEXT)) break;
        idx = d.next;
    }
}

// helper: read first `bytes` from in-pointer chain into a contiguous buffer
static uint32_t read_in(const DescChain& dc, void* dst, uint32_t bytes) {
    uint8_t* p = (uint8_t*)dst;
    uint32_t copied = 0;
    for (int i = 0; i < dc.n_in && copied < bytes; i++) {
        uint32_t take = dc.in_len[i];
        if (copied + take > bytes) take = bytes - copied;
        GuestMemoryManager::ReadGuestPhys(dc.in_addr[i], p + copied, take);
        copied += take;
    }
    return copied;
}

// helper: write to out chain
static uint32_t write_out(const DescChain& dc, const void* src, uint32_t bytes) {
    const uint8_t* p = (const uint8_t*)src;
    uint32_t written = 0;
    for (int i = 0; i < dc.n_out && written < bytes; i++) {
        uint32_t take = dc.out_len[i];
        if (written + take > bytes) take = bytes - written;
        GuestMemoryManager::WriteGuestPhys(dc.out_addr[i], p + written, take);
        written += take;
    }
    return written;
}

uint32_t VirtIOGPUHost::ProcessCommand(VirtQGpu& q, uint16_t head,
                                          uint32_t* out_resp_len) {
    DescChain dc;
    walk_chain(q, head, dc);
    if (dc.n_in == 0 || dc.in_len[0] < sizeof(VirtIOGPUCtrlHdr)) {
        return 0;
    }

    VirtIOGPUCtrlHdr hdr{};
    GuestMemoryManager::ReadGuestPhys(dc.in_addr[0], &hdr, sizeof(hdr));

    auto respond_simple = [&](uint32_t type) -> uint32_t {
        VirtIOGPUCtrlHdr resp{};
        resp.type     = type;
        resp.flags    = 0;
        resp.fence_id = hdr.fence_id;
        resp.ctx_id   = hdr.ctx_id;
        write_out(dc, &resp, sizeof(resp));
        if (out_resp_len) *out_resp_len = sizeof(resp);
        return sizeof(resp);
    };

    switch (hdr.type) {
        case VIRTIO_GPU_CMD_GET_DISPLAY_INFO: {
            VirtIOGPURespDisplayInfo r{};
            r.hdr.type     = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
            r.hdr.fence_id = hdr.fence_id;
            r.hdr.ctx_id   = hdr.ctx_id;
            r.pmodes[0].r.x = 0; r.pmodes[0].r.y = 0;
            r.pmodes[0].r.width  = (uint32_t)GetWidth();
            r.pmodes[0].r.height = (uint32_t)GetHeight();
            r.pmodes[0].enabled  = 1;
            r.pmodes[0].flags    = 0;
            write_out(dc, &r, sizeof(r));
            if (out_resp_len) *out_resp_len = sizeof(r);
            return sizeof(r);
        }
        case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: {
            VirtIOGPUResourceCreate2D req{};
            read_in(dc, &req, sizeof(req));
            VGpuResource* r = AllocRes(req.resource_id);
            if (!r) return respond_simple(VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
            r->format = req.format;
            r->width  = req.width;
            r->height = req.height;
            uint32_t bpp = bytes_per_pixel_for_format(r->format);
            r->host_pixels_size = r->width * r->height * bpp;
            r->host_pixels = (uint8_t*)KernelHeap::Alloc(r->host_pixels_size);
            if (!r->host_pixels) {
                FreeRes(r);
                return respond_simple(VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
            }
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
            VirtIOGPUResourceUnref req{};
            read_in(dc, &req, sizeof(req));
            FreeRes(FindRes(req.resource_id));
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
            VirtIOGPUResourceAttachBacking req{};
            read_in(dc, &req, sizeof(req));
            VGpuResource* r = FindRes(req.resource_id);
            if (!r) return respond_simple(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
            // entries follow the request struct in the same in-buffer
            uint32_t entries = req.nr_entries;
            if (entries > VIRTIO_GPU_MAX_BACKINGS) entries = VIRTIO_GPU_MAX_BACKINGS;
            r->backing_count = 0;
            // they may be in subsequent in-descriptors or at offset
            // sizeof(req) inside the first.  read sequentially across the
            // chain by re-walking from byte sizeof(req).
            // Rebuild into a flat scratch first.
            uint32_t need = entries * sizeof(VirtIOGPUMemEntry);
            uint8_t scratch[VIRTIO_GPU_MAX_BACKINGS * sizeof(VirtIOGPUMemEntry)];
            uint32_t cursor = 0;
            uint32_t copied = 0;
            for (int i = 0; i < dc.n_in && copied < need; i++) {
                uint32_t skip = 0;
                if (cursor < sizeof(req)) {
                    uint32_t s = sizeof(req) - cursor;
                    if (s > dc.in_len[i]) {
                        cursor += dc.in_len[i];
                        continue;
                    }
                    skip = s;
                    cursor += s;
                }
                uint32_t avail = dc.in_len[i] - skip;
                uint32_t take = need - copied;
                if (take > avail) take = avail;
                GuestMemoryManager::ReadGuestPhys(dc.in_addr[i] + skip,
                                                    scratch + copied, take);
                copied += take;
                cursor += avail;
            }
            VirtIOGPUMemEntry* mes = (VirtIOGPUMemEntry*)scratch;
            for (uint32_t i = 0; i < entries; i++) {
                r->backings[i].addr   = mes[i].addr;
                r->backings[i].length = mes[i].length;
            }
            r->backing_count = (int)entries;
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
            VirtIOGPUResourceUnref req{};
            read_in(dc, &req, sizeof(req));
            VGpuResource* r = FindRes(req.resource_id);
            if (r) r->backing_count = 0;
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: {
            VirtIOGPUTransferToHost2D req{};
            read_in(dc, &req, sizeof(req));
            VGpuResource* r = FindRes(req.resource_id);
            if (!r) return respond_simple(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
            TransferToHost(r, req.r.x, req.r.y, req.r.width, req.r.height,
                            req.offset);
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_SET_SCANOUT: {
            VirtIOGPUSetScanout req{};
            read_in(dc, &req, sizeof(req));
            if (req.scanout_id >= VIRTIO_GPU_MAX_SCANOUTS) {
                return respond_simple(VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
            }
            VGpuScanout& s = scanouts[req.scanout_id];
            s.resource_id = req.resource_id;
            s.x = req.r.x; s.y = req.r.y;
            s.width = req.r.width; s.height = req.r.height;
            s.enabled = (req.resource_id != 0);
            scanout_dirty = true;
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_RESOURCE_FLUSH: {
            VirtIOGPUResourceFlush req{};
            read_in(dc, &req, sizeof(req));
            scanout_dirty = true;
            PresentScanout0();
            return respond_simple(VIRTIO_GPU_RESP_OK_NODATA);
        }
        case VIRTIO_GPU_CMD_GET_EDID: {
            // minimal 128-byte edid block claiming 1024x768 @ 60 hz preferred
            struct {
                VirtIOGPUCtrlHdr hdr;
                uint32_t size;
                uint32_t pad;
                uint8_t  edid[1024];
            } resp{};
            resp.hdr.type     = VIRTIO_GPU_RESP_OK_EDID;
            resp.hdr.fence_id = hdr.fence_id;
            resp.hdr.ctx_id   = hdr.ctx_id;
            resp.size = 128;
            // edid 1.4 header
            uint8_t hdr8[8] = {0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0};
            for (int i = 0; i < 8; i++) resp.edid[i] = hdr8[i];
            // checksum
            uint8_t sum = 0;
            for (int i = 0; i < 127; i++) sum += resp.edid[i];
            resp.edid[127] = (uint8_t)(0u - sum);
            write_out(dc, &resp, sizeof(resp));
            if (out_resp_len) *out_resp_len = sizeof(resp);
            return sizeof(resp);
        }
        default:
            return respond_simple(VIRTIO_GPU_RESP_ERR_UNSPEC);
    }
}

// ----------------------------------------------------------------- presentation

void VirtIOGPUHost::PresentScanout0() {
    if (!scanouts[0].enabled) return;
    VGpuResource* r = FindRes(scanouts[0].resource_id);
    if (!r || !r->host_pixels) return;

    uint32_t w = scanouts[0].width;
    uint32_t h = scanouts[0].height;
    if (w == 0 || h == 0 || w > r->width || h > r->height) return;

    uint32_t bpp = bytes_per_pixel_for_format(r->format);
    uint32_t pitch = r->width * bpp;
    const uint8_t* src = r->host_pixels +
                          (scanouts[0].y * r->width + scanouts[0].x) * bpp;
    LinuxDeviceBridge::BlitFramebufferRect(src, pitch, w, h, 0, 0);
    frame_count++;
    scanout_dirty = false;
}

void VirtIOGPUHost::ProcessQueues() {
    // virtqueues are processed lazily on guest notify (NotifyQueue),
    // but a tick-driven sweep keeps things flowing in case the guest
    // never re-notifies after the initial enable.
    if (!registered) return;
    for (int i = 0; i < VIRTIO_GPU_MAX_QUEUES; i++) {
        VirtQGpu& q = queues[i];
        if (!q.enable || !q.avail_addr) continue;
        uint16_t avail_idx = 0;
        GuestMemoryManager::ReadGuestPhys(q.avail_addr + 2, &avail_idx, 2);
        if (avail_idx != q.last_avail_idx) NotifyQueue(i);
    }
}

bool VirtIOGPUHost::PresentIfDirty() {
    if (!scanout_dirty) return false;
    PresentScanout0();
    return true;
}
