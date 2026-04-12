//  kurono os  -  virtio gpu driver implementation
//  virtio pci modern device with 2d resource management
#include "virtio_gpu.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"

bool VirtIOGPU::detected = false;
volatile uint8_t* VirtIOGPU::bar0 = nullptr;
volatile uint8_t* VirtIOGPU::notify_base = nullptr;
uint32_t VirtIOGPU::notify_off_multiplier = 0;

VirtQueue VirtIOGPU::controlq = {};
VirtQueue VirtIOGPU::cursorq = {};

uint32_t VirtIOGPU::display_width = 0;
uint32_t VirtIOGPU::display_height = 0;
uint32_t VirtIOGPU::next_resource_id = 1;
uint32_t VirtIOGPU::fb_resource_id = 0;

uint8_t VirtIOGPU::pci_bus = 0;
uint8_t VirtIOGPU::pci_dev = 0;
uint8_t VirtIOGPU::pci_func = 0;

uint32_t VirtIOGPU::PciRead32(uint8_t offset) {
    uint32_t addr = (1 << 31) | (pci_bus << 16) | (pci_dev << 11) | (pci_func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void VirtIOGPU::PciWrite32(uint8_t offset, uint32_t val) {
    uint32_t addr = (1 << 31) | (pci_bus << 16) | (pci_dev << 11) | (pci_func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

uint16_t VirtIOGPU::PciRead16(uint8_t offset) {
    uint32_t val = PciRead32(offset & 0xFC);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

uint8_t VirtIOGPU::PciRead8(uint8_t offset) {
    uint32_t val = PciRead32(offset & 0xFC);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

bool VirtIOGPU::InitVirtQueue(int index, VirtQueue* vq) {
    if (!bar0) return false;

    // modern virtio: use common config to set up queues
    // for simplicity, we'll allocate a fixed-size queue
    vq->size = VIRTQ_SIZE;
    vq->free_head = 0;
    vq->last_used_idx = 0;
    vq->num_free = VIRTQ_SIZE;

    // allocate descriptor table
    vq->desc = (VirtQueueDesc*)KernelHeap::Alloc(4096);
    if (!vq->desc) return false;

    // allocate available ring
    vq->avail = (VirtQueueAvail*)KernelHeap::Alloc(4096);
    if (!vq->avail) return false;

    // allocate used ring
    vq->used = (VirtQueueUsed*)KernelHeap::Alloc(4096);
    if (!vq->used) return false;

    // initialize descriptor chain
    for (int i = 0; i < VIRTQ_SIZE; i++) {
        vq->desc[i].addr = 0;
        vq->desc[i].len = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next = (i + 1) % VIRTQ_SIZE;
    }

    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->flags = 0;
    vq->used->idx = 0;

    (void)index;
    return true;
}

void VirtIOGPU::NotifyQueue(int index) {
    if (notify_base) {
        *(volatile uint16_t*)(notify_base + index * notify_off_multiplier) = (uint16_t)index;
    }
}

bool VirtIOGPU::SendControlCommand(void* cmd, uint32_t cmd_size,
                                   void* resp, uint32_t resp_size) {
    VirtQueue* vq = &controlq;
    if (vq->num_free < 2) return false;

    // allocate 2 descriptors: cmd (device-readable) + resp (device-writable)
    uint16_t head = vq->free_head;
    uint16_t cmd_idx = head;
    vq->free_head = vq->desc[cmd_idx].next;
    vq->num_free--;

    uint16_t resp_idx = vq->free_head;
    vq->free_head = vq->desc[resp_idx].next;
    vq->num_free--;

    // set up command descriptor
    vq->desc[cmd_idx].addr = (uint64_t)(uintptr_t)cmd;
    vq->desc[cmd_idx].len = cmd_size;
    vq->desc[cmd_idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[cmd_idx].next = resp_idx;

    // set up response descriptor (writable by device)
    vq->desc[resp_idx].addr = (uint64_t)(uintptr_t)resp;
    vq->desc[resp_idx].len = resp_size;
    vq->desc[resp_idx].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[resp_idx].next = 0;

    // add to available ring
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = head;

    // memory barrier
    __asm__ volatile("mfence" ::: "memory");

    vq->avail->idx++;

    // notify device
    NotifyQueue(0);

    // poll for response
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile("mfence" ::: "memory");
        if (vq->used->idx != vq->last_used_idx) {
            vq->last_used_idx++;

            // free descriptors
            vq->desc[resp_idx].next = vq->free_head;
            vq->free_head = cmd_idx;
            vq->desc[cmd_idx].next = resp_idx;
            vq->num_free += 2;

            return true;
        }
        for (volatile int d = 0; d < 100; d++);
    }

    return false;
}

bool VirtIOGPU::Init() {
    detected = false;
    display_width = 0;
    display_height = 0;
    next_resource_id = 1;
    fb_resource_id = 0;

    // scan pci for virtio gpu device
    bool found = false;

    for (int bus = 0; bus < 256 && !found; bus++) {
        for (int dev = 0; dev < 32 && !found; dev++) {
            for (int func = 0; func < 8 && !found; func++) {
                uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8);

                outl(0xCF8, addr | 0x00);
                uint32_t vendor_device = inl(0xCFC);
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;

                if (vendor != VIRTIO_VENDOR_ID) continue;

                // check for virtio gpu (modern or transitional)
                if (device == VIRTIO_GPU_DEVICE_ID ||
                    (device >= 0x1000 && device <= 0x103F)) {
                    // for transitional devices, check subsystem id
                    if (device >= 0x1000 && device <= 0x103F) {
                        outl(0xCF8, addr | 0x2C);
                        uint32_t subsys = inl(0xCFC);
                        uint16_t subsys_id = (subsys >> 16) & 0xFFFF;
                        if (subsys_id != 16) continue; // gpu subsystem id is 16
                    }

                    pci_bus = bus;
                    pci_dev = dev;
                    pci_func = func;

                    outl(0xCF8, addr | 0x10);
                    uint32_t bar0_val = inl(0xCFC) & ~0xF;
                    bar0 = (volatile uint8_t*)(uintptr_t)bar0_val;

                    found = true;
                }
            }
        }
    }

    if (!found) return false;

    // enable bus mastering and memory space
    uint32_t pci_cmd = PciRead32(0x04);
    pci_cmd |= (1 << 1) | (1 << 2);
    PciWrite32(0x04, pci_cmd);

    // walk pci capabilities to find virtio config structures
    uint8_t cap_ptr = PciRead8(0x34) & 0xFC;
    volatile uint8_t* common_cfg = nullptr;

    while (cap_ptr) {
        uint8_t cap_id = PciRead8(cap_ptr);
        if (cap_id == 0x09) { // vendor-specific (virtio)
            uint8_t cfg_type = PciRead8(cap_ptr + 3);
            uint8_t bar = PciRead8(cap_ptr + 4);
            uint32_t offset = PciRead32(cap_ptr + 8);

            volatile uint8_t* bar_base = nullptr;
            if (bar == 0) bar_base = bar0;
            else {
                uint32_t bar_addr_off = 0x10 + bar * 4;
                bar_base = (volatile uint8_t*)(uintptr_t)(PciRead32(bar_addr_off) & ~0xF);
            }

            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON:
                    common_cfg = bar_base + offset;
                    break;
                case VIRTIO_PCI_CAP_NOTIFY:
                    notify_base = bar_base + offset;
                    notify_off_multiplier = PciRead32(cap_ptr + 16);
                    break;
            }
        }
        cap_ptr = PciRead8(cap_ptr + 1) & 0xFC;
    }

    // virtio device initialization sequence
    if (common_cfg) {
        // reset device
        *(volatile uint8_t*)(common_cfg + 20) = 0; // device_status = 0

        // set acknowledge
        *(volatile uint8_t*)(common_cfg + 20) = VIRTIO_STATUS_ACKNOWLEDGE;

        // set driver
        *(volatile uint8_t*)(common_cfg + 20) |= VIRTIO_STATUS_DRIVER;

        // negotiate features (accept what device offers for now)
        // feature bits 0-31
        *(volatile uint32_t*)(common_cfg + 4) = 0; // device_feature_select
        uint32_t features = *(volatile uint32_t*)(common_cfg + 0); // device_feature
        *(volatile uint32_t*)(common_cfg + 12) = 0; // driver_feature_select
        *(volatile uint32_t*)(common_cfg + 8) = features; // driver_feature

        *(volatile uint8_t*)(common_cfg + 20) |= VIRTIO_STATUS_FEATURES_OK;

        // verify features ok
        if (!(*(volatile uint8_t*)(common_cfg + 20) & VIRTIO_STATUS_FEATURES_OK)) {
            *(volatile uint8_t*)(common_cfg + 20) = VIRTIO_STATUS_FAILED;
            return false;
        }
    }

    // initialize virtqueues
    if (!InitVirtQueue(0, &controlq)) return false;
    if (!InitVirtQueue(1, &cursorq)) return false;

    // set driver_ok
    if (common_cfg) {
        *(volatile uint8_t*)(common_cfg + 20) |= VIRTIO_STATUS_DRIVER_OK;
    }

    detected = true;

    // get display info
    uint32_t w = 0, h = 0;
    if (GetDisplayInfo(0, &w, &h)) {
        display_width = w;
        display_height = h;
    } else {
        display_width = 1920;
        display_height = 1080;
    }

    return true;
}

bool VirtIOGPU::IsDetected() { return detected; }

bool VirtIOGPU::GetDisplayInfo(int scanout, uint32_t* width, uint32_t* height) {
    if (!detected) return false;

    VirtGPU_CtrlHdr cmd = {};
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    VirtGPU_DisplayInfo resp = {};

    if (!SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)))
        return false;

    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
        return false;

    if (scanout >= 0 && scanout < VIRTIO_GPU_MAX_SCANOUTS && resp.pmodes[scanout].enabled) {
        if (width)  *width = resp.pmodes[scanout].rect.width;
        if (height) *height = resp.pmodes[scanout].rect.height;
        return true;
    }

    return false;
}

uint32_t VirtIOGPU::CreateResource2D(uint32_t width, uint32_t height, uint32_t format) {
    if (!detected) return 0;

    VirtGPU_ResourceCreate2D cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = next_resource_id++;
    cmd.format = format;
    cmd.width = width;
    cmd.height = height;

    VirtGPU_CtrlHdr resp = {};
    if (!SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)))
        return 0;

    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return 0;

    return cmd.resource_id;
}

bool VirtIOGPU::AttachBacking(uint32_t resource_id, void* buffer, uint32_t size) {
    if (!detected) return false;

    // we need a buffer large enough for the command + 1 mem entry
    struct {
        VirtGPU_ResourceAttachBacking cmd;
        VirtGPU_MemEntry entry;
    } __attribute__((packed)) request = {};

    request.cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.cmd.resource_id = resource_id;
    request.cmd.nr_entries = 1;
    request.entry.addr = (uint64_t)(uintptr_t)buffer;
    request.entry.length = size;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&request, sizeof(request), &resp, sizeof(resp)) &&
           resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPU::SetScanout(int scanout_id, uint32_t resource_id,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!detected) return false;

    VirtGPU_SetScanout cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.rect.x = x;
    cmd.rect.y = y;
    cmd.rect.width = w;
    cmd.rect.height = h;
    cmd.scanout_id = scanout_id;
    cmd.resource_id = resource_id;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)) &&
           resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPU::TransferToHost2D(uint32_t resource_id,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!detected) return false;

    VirtGPU_TransferToHost2D cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.rect.x = x;
    cmd.rect.y = y;
    cmd.rect.width = w;
    cmd.rect.height = h;
    cmd.offset = 0;
    cmd.resource_id = resource_id;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)) &&
           resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPU::Flush(uint32_t resource_id,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!detected) return false;

    VirtGPU_ResourceFlush cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.rect.x = x;
    cmd.rect.y = y;
    cmd.rect.width = w;
    cmd.rect.height = h;
    cmd.resource_id = resource_id;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)) &&
           resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPU::DestroyResource(uint32_t resource_id) {
    if (!detected) return false;

    struct {
        VirtGPU_CtrlHdr hdr;
        uint32_t resource_id;
        uint32_t padding;
    } __attribute__((packed)) cmd = {};

    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = resource_id;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)) &&
           resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPU::UpdateCursor(uint32_t scanout, uint32_t resource_id,
                             uint32_t hot_x, uint32_t hot_y) {
    if (!detected) return false;

    struct {
        VirtGPU_CtrlHdr hdr;
        uint32_t pos_scanout_id;
        uint32_t pos_x, pos_y;
        uint32_t resource_id;
        uint32_t hot_x, hot_y;
        uint32_t padding;
    } __attribute__((packed)) cmd = {};

    cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos_scanout_id = scanout;
    cmd.resource_id = resource_id;
    cmd.hot_x = hot_x;
    cmd.hot_y = hot_y;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp));
}

bool VirtIOGPU::MoveCursor(uint32_t scanout, uint32_t x, uint32_t y) {
    if (!detected) return false;

    struct {
        VirtGPU_CtrlHdr hdr;
        uint32_t pos_scanout_id;
        uint32_t pos_x, pos_y;
        uint32_t resource_id;
        uint32_t hot_x, hot_y;
        uint32_t padding;
    } __attribute__((packed)) cmd = {};

    cmd.hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cmd.pos_scanout_id = scanout;
    cmd.pos_x = x;
    cmd.pos_y = y;

    VirtGPU_CtrlHdr resp = {};
    return SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp));
}

bool VirtIOGPU::PresentFramebuffer(void* pixels, uint32_t width, uint32_t height) {
    if (!detected || !pixels) return false;

    // create resource if needed
    if (fb_resource_id == 0) {
        fb_resource_id = CreateResource2D(width, height, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
        if (fb_resource_id == 0) return false;

        uint32_t size = width * height * 4;
        if (!AttachBacking(fb_resource_id, pixels, size)) return false;
        if (!SetScanout(0, fb_resource_id, 0, 0, width, height)) return false;
    }

    // transfer and flush
    if (!TransferToHost2D(fb_resource_id, 0, 0, width, height)) return false;
    if (!Flush(fb_resource_id, 0, 0, width, height)) return false;

    return true;
}

void VirtIOGPU::DumpInfo(char* out, int max_len) {
    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < max_len - 1) out[pos++] = *s++;
    };
    auto append_num = [&](uint32_t val) {
        char buf[12]; int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else { char rev[12]; int ri = 0; uint32_t tmp = val;
            while (tmp) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            while (ri--) buf[i++] = rev[ri]; }
        buf[i] = 0; append(buf);
    };

    if (!detected) {
        append("VirtIO GPU: Not detected\n");
        out[pos] = 0; return;
    }

    append("VirtIO GPU Device\n");
    append("  Display: "); append_num(display_width);
    append("x"); append_num(display_height); append("\n");
    append("  Active Resource: "); append_num(fb_resource_id); append("\n");
    append("  Next Resource ID: "); append_num(next_resource_id); append("\n");
    append("  Control Queue Size: "); append_num(VIRTQ_SIZE); append("\n");

    out[pos] = 0;
}
