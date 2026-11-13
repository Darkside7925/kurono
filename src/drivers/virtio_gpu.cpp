//  kurono os - virtio gpu driver implementation
//  virtio pci modern device with 2d resource management
#include "virtio_gpu.h"
#include "../kernel/kimage.h"   // command buffers may be high-linked kernel bss (satoru)
#include "../kernel/heap.h"
#include "../kernel/io.h"
#include "../kernel/vmm.h"   // map the high 64-bit bar before dereferencing it (satoru)
#include "serial.h"          // [vgtime] present-phase instrument (satoru)

// present-phase timing instrument. the present costs ~4.2ms/frame (measured:
// caps virtio at ~236fps vs ~2030fps on the plain framebuffer) but we do not
// know WHERE that goes - submit cost, the host's transfer, or the flush. a
// naive async restructure was already tried and made things WORSE, so measure
// the phases before restructuring again. split reported: build+submit, then
// the wait for the FIRST completion (the transfer - once that retires the host
// owns a copy and the guest backing is free to overwrite), then the extra wait
// for the SECOND (the flush/display). (satoru)
static inline uint64_t vg_rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ── async present state (one frame in flight) ───────────────────────────────
// the present submits and returns; DrainPresent() retires the frame just
// before the compositor next writes the framebuffer. exactly one frame may be
// outstanding, so the ring can never grow and the descriptors are always
// reclaimed. (satoru)
static bool     s_present_inflight = false;
static uint16_t s_present_expected = 0;
static uint16_t s_present_head_a   = 0;
static uint16_t s_present_head_b   = 0;
static uint64_t s_present_submit_cycles = 0;
// ASYNC BY DEFAULT (kurono.vgsync=1 restores the blocking present).
// the present submits and returns; DrainPresent() collects it at the top of
// Graphics::SwapBuffers, immediately before the first write to fb_addr.
//
// MEASURED, and the useful metric was NOT throughput. present cost drops
// 1169us -> 52us (22x), but fps is unchanged at the 60fps cap, unchanged with
// full-screen transfers, and unchanged with host headroom - because the 1.17ms
// is not idle latency the guest could reclaim, it is qemu doing real work on
// the same physical cpus (composite measured 2.58ms -> 3.7ms, cancelling it).
//
// the win is LATENCY. a 1.17ms non-yielding busy-poll blocks every other
// cooperative kernel process. probing with the kmemx worker's 10ms sleep, the
// gap it actually gets: sync 10.69ms vs async 10.52ms mean over ~3000 samples,
// two runs per arm, no overlap between arms. that is a 24% cut in scheduling
// overshoot (685us -> 522us), and worst-case gaps trend lower too. on a
// cooperative scheduler that is the property that matters. (satoru)
static bool     s_present_sync = false;

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

// per-queue notification offset captured from the common-config queue_notify_off
// field. without this, modern virtio devices ignore writes to notify_base.
static uint16_t s_ctrlq_notify_off = 0;
static uint16_t s_cursorq_notify_off = 1;
static volatile uint8_t* s_common_cfg = nullptr;
static bool s_indirect_supported = false;

// scratch indirect descriptor table (single in-flight cmd at a time on the
// control queue keeps this safe). aligned for the device.
static VirtQueueDesc* s_indirect_table = nullptr;

#define VIRTIO_F_INDIRECT_DESC_BIT 28

uint32_t VirtIOGPU::PciRead32(uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)pci_bus << 16) | ((uint32_t)pci_dev << 11) | ((uint32_t)pci_func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void VirtIOGPU::PciWrite32(uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)pci_bus << 16) | ((uint32_t)pci_dev << 11) | ((uint32_t)pci_func << 8) | (offset & 0xFC);
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

// resolve a PCI BAR index to a base pointer, properly handling 64-bit and I/O
// BARs. previous version treated I/O BARs as memory and stripped the wrong
// alignment bits - this caused garbage MMIO mappings on real hardware.
static volatile uint8_t* resolve_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx) {
    if (bar_idx > 5) return nullptr;
    uint8_t off = 0x10 + bar_idx * 4;
    uint32_t addr_lo;
    {
        uint32_t cfg = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC);
        outl(0xCF8, cfg);
        addr_lo = inl(0xCFC);
    }
    if (addr_lo & 1) return nullptr; // I/O BAR - not usable for virtio modern config
    uint64_t base = (uint64_t)(addr_lo & 0xFFFFFFF0);
    uint8_t type = (addr_lo >> 1) & 0x03;
    if (type == 0x02 && bar_idx < 5) {
        uint8_t off_hi = off + 4;
        uint32_t cfg = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off_hi & 0xFC);
        outl(0xCF8, cfg);
        uint32_t hi = inl(0xCFC);
        base |= ((uint64_t)hi << 32);
    }
    if (base == 0) return nullptr;

    // a 64-bit bar is often placed high above the boot identity map (qemu puts
    // virtio modern config bars around 0x3_8000_0000), so dereferencing the
    // base would #pf. identity-map ~4mb of it as uncached mmio first -- that
    // covers the common/notify/isr/device config regions. cheap + idempotent
    // (MapPage just rewrites the pte if already mapped). (satoru)
    for (uint64_t p = base & ~0xFFFULL; p < (base & ~0xFFFULL) + 0x400000ULL; p += 0x1000ULL) {
        KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    }

    return (volatile uint8_t*)(uintptr_t)base;
}

bool VirtIOGPU::InitVirtQueue(int index, VirtQueue* vq) {
    if (!bar0) return false;

    vq->size = VIRTQ_SIZE;
    vq->free_head = 0;
    vq->last_used_idx = 0;
    vq->num_free = VIRTQ_SIZE;

    vq->desc = (VirtQueueDesc*)KernelHeap::Alloc(4096);
    vq->avail = (VirtQueueAvail*)KernelHeap::Alloc(4096);
    vq->used = (VirtQueueUsed*)KernelHeap::Alloc(4096);

    if (!vq->desc || !vq->avail || !vq->used) {
        if (vq->desc) KernelHeap::Free(vq->desc);
        if (vq->avail) KernelHeap::Free(vq->avail);
        if (vq->used) KernelHeap::Free(vq->used);
        vq->desc = nullptr; vq->avail = nullptr; vq->used = nullptr;
        return false;
    }

    // zero the rings so the device sees a clean ground state
    uint8_t* d = (uint8_t*)vq->desc;  for (int i = 0; i < 4096; i++) d[i] = 0;
    uint8_t* a = (uint8_t*)vq->avail; for (int i = 0; i < 4096; i++) a[i] = 0;
    uint8_t* u = (uint8_t*)vq->used;  for (int i = 0; i < 4096; i++) u[i] = 0;

    for (int i = 0; i < VIRTQ_SIZE; i++) {
        vq->desc[i].next = (uint16_t)((i + 1) % VIRTQ_SIZE);
    }

    // program queue addresses + size into the common config so the device knows
    // where to find the rings. without this, no commands ever complete.
    if (s_common_cfg) {
        volatile uint16_t* queue_select   = (volatile uint16_t*)(s_common_cfg + 22);
        volatile uint16_t* queue_size     = (volatile uint16_t*)(s_common_cfg + 24);
        volatile uint16_t* queue_msix     = (volatile uint16_t*)(s_common_cfg + 26);
        volatile uint16_t* queue_enable   = (volatile uint16_t*)(s_common_cfg + 28);
        volatile uint16_t* queue_notify   = (volatile uint16_t*)(s_common_cfg + 30);
        volatile uint64_t* queue_desc_lo  = (volatile uint64_t*)(s_common_cfg + 32);
        volatile uint64_t* queue_avail_lo = (volatile uint64_t*)(s_common_cfg + 40);
        volatile uint64_t* queue_used_lo  = (volatile uint64_t*)(s_common_cfg + 48);

        *queue_select = (uint16_t)index;
        __asm__ volatile("mfence" ::: "memory");

        uint16_t max_size = *queue_size;
        if (max_size == 0) return false;
        uint16_t use_size = (max_size < VIRTQ_SIZE) ? max_size : (uint16_t)VIRTQ_SIZE;
        vq->size = use_size;
        vq->num_free = use_size;
        for (int i = 0; i < use_size; i++) {
            vq->desc[i].next = (uint16_t)((i + 1) % use_size);
        }

        *queue_size    = use_size;
        *queue_msix    = 0xFFFF; // no MSI-X
        *queue_desc_lo  = KernelPhysOf(vq->desc);
        *queue_avail_lo = KernelPhysOf(vq->avail);
        *queue_used_lo  = KernelPhysOf(vq->used);
        __asm__ volatile("mfence" ::: "memory");

        uint16_t notify_off = *queue_notify;
        if (index == 0) s_ctrlq_notify_off = notify_off;
        else            s_cursorq_notify_off = notify_off;

        *queue_enable = 1;
        __asm__ volatile("mfence" ::: "memory");
    }

    return true;
}

void VirtIOGPU::NotifyQueue(int index) {
    if (!notify_base) return;
    // modern virtio: notify address = notify_base + queue_notify_off * notify_off_multiplier
    uint32_t mult = notify_off_multiplier ? notify_off_multiplier : 2;
    uint16_t off = (index == 0) ? s_ctrlq_notify_off : s_cursorq_notify_off;
    volatile uint16_t* ptr = (volatile uint16_t*)(notify_base + (uint32_t)off * mult);
    __asm__ volatile("sfence" ::: "memory");
    *ptr = (uint16_t)index;
}

bool VirtIOGPU::SendControlCommand(void* cmd, uint32_t cmd_size,
                                   void* resp, uint32_t resp_size) {
    VirtQueue* vq = &controlq;
    if (!vq->desc || !vq->avail || !vq->used) return false;

    // try the indirect-descriptor fast path when the device negotiated it.
    // this collapses every command into a single direct ring slot.
    if (s_indirect_supported && s_indirect_table && vq->num_free >= 1) {
        uint16_t head = vq->free_head;
        vq->free_head = vq->desc[head].next;
        vq->num_free--;

        s_indirect_table[0].addr = KernelPhysOf(cmd);
        s_indirect_table[0].len  = cmd_size;
        s_indirect_table[0].flags = VIRTQ_DESC_F_NEXT;
        s_indirect_table[0].next = 1;
        s_indirect_table[1].addr = KernelPhysOf(resp);
        s_indirect_table[1].len  = resp_size;
        s_indirect_table[1].flags = VIRTQ_DESC_F_WRITE;
        s_indirect_table[1].next = 0;

        vq->desc[head].addr = KernelPhysOf(s_indirect_table);
        vq->desc[head].len  = sizeof(VirtQueueDesc) * 2;
        vq->desc[head].flags = VIRTQ_DESC_F_INDIRECT;
        vq->desc[head].next = 0;

        __asm__ volatile("mfence" ::: "memory");
        uint16_t avail_slot = vq->avail->idx % vq->size;
        vq->avail->ring[avail_slot] = head;
        __asm__ volatile("mfence" ::: "memory");
        vq->avail->idx++;
        __asm__ volatile("mfence" ::: "memory");
        NotifyQueue(0);

        for (int i = 0; i < 2000000; i++) {
            __asm__ volatile("mfence" ::: "memory");
            uint16_t cur = vq->used->idx;
            if (cur != vq->last_used_idx) {
                vq->last_used_idx = cur;
                vq->desc[head].next = vq->free_head;
                vq->free_head = head;
                vq->num_free++;
                return true;
            }
            for (volatile int d = 0; d < 16; d++);
        }
        // submission timeout: reclaim slot so the queue does not leak
        vq->desc[head].next = vq->free_head;
        vq->free_head = head;
        vq->num_free++;
        return false;
    }

    if (vq->num_free < 2) return false;

    uint16_t cmd_idx = vq->free_head;
    vq->free_head = vq->desc[cmd_idx].next;
    vq->num_free--;

    uint16_t resp_idx = vq->free_head;
    vq->free_head = vq->desc[resp_idx].next;
    vq->num_free--;

    vq->desc[cmd_idx].addr  = KernelPhysOf(cmd);
    vq->desc[cmd_idx].len   = cmd_size;
    vq->desc[cmd_idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[cmd_idx].next  = resp_idx;

    vq->desc[resp_idx].addr  = KernelPhysOf(resp);
    vq->desc[resp_idx].len   = resp_size;
    vq->desc[resp_idx].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[resp_idx].next  = 0;

    __asm__ volatile("mfence" ::: "memory");
    uint16_t avail_slot = vq->avail->idx % vq->size;
    vq->avail->ring[avail_slot] = cmd_idx;
    __asm__ volatile("mfence" ::: "memory");
    vq->avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    NotifyQueue(0);

    for (int i = 0; i < 2000000; i++) {
        __asm__ volatile("mfence" ::: "memory");
        uint16_t cur = vq->used->idx;
        if (cur != vq->last_used_idx) {
            vq->last_used_idx = cur;
            vq->desc[resp_idx].next = vq->free_head;
            vq->free_head = cmd_idx;
            vq->desc[cmd_idx].next = resp_idx;
            vq->num_free += 2;
            return true;
        }
        for (volatile int d = 0; d < 16; d++);
    }

    // reclaim both descriptors on timeout so the ring keeps moving
    vq->desc[resp_idx].next = vq->free_head;
    vq->free_head = cmd_idx;
    vq->desc[cmd_idx].next = resp_idx;
    vq->num_free += 2;
    return false;
}

bool VirtIOGPU::Init() {
    detected = false;
    display_width = 0;
    display_height = 0;
    next_resource_id = 1;
    fb_resource_id = 0;
    s_common_cfg = nullptr;
    s_indirect_supported = false;
    s_indirect_table = nullptr;

    bool found = false;

    for (int bus = 0; bus < 256 && !found; bus++) {
        for (int dev = 0; dev < 32 && !found; dev++) {
            // probe func 0 first to check vendor + multi-function
            uint32_t addr0 = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11);
            outl(0xCF8, addr0);
            uint32_t vd0 = inl(0xCFC);
            if ((vd0 & 0xFFFF) == 0xFFFF) continue;
            outl(0xCF8, addr0 | 0x0C);
            uint32_t hdr0 = inl(0xCFC);
            bool multi = ((hdr0 >> 16) & 0x80) != 0;
            int max_func = multi ? 8 : 1;

            for (int func = 0; func < max_func && !found; func++) {
                uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8);

                outl(0xCF8, addr | 0x00);
                uint32_t vendor_device = inl(0xCFC);
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;

                if (vendor != VIRTIO_VENDOR_ID) continue;

                bool is_gpu = false;
                if (device == VIRTIO_GPU_DEVICE_ID) {
                    is_gpu = true;
                } else if (device >= 0x1000 && device <= 0x103F) {
                    outl(0xCF8, addr | 0x2C);
                    uint32_t subsys = inl(0xCFC);
                    uint16_t subsys_id = (subsys >> 16) & 0xFFFF;
                    if (subsys_id == 16) is_gpu = true;
                }
                if (!is_gpu) continue;

                pci_bus = (uint8_t)bus;
                pci_dev = (uint8_t)dev;
                pci_func = (uint8_t)func;

                // walk all BARs; the first memory-mapped one becomes our fallback.
                bar0 = nullptr;
                for (uint8_t b = 0; b < 6 && !bar0; b++) {
                    volatile uint8_t* p = resolve_bar(pci_bus, pci_dev, pci_func, b);
                    if (p) bar0 = p;
                }

                found = true;
            }
        }
    }

    if (!found) return false;

    // enable bus mastering + memory space
    uint32_t pci_cmd = PciRead32(0x04);
    pci_cmd |= (1u << 1) | (1u << 2);
    PciWrite32(0x04, pci_cmd);

    // walk PCI capabilities to find virtio config structures
    uint8_t cap_ptr = PciRead8(0x34) & 0xFC;
    volatile uint8_t* common_cfg = nullptr;
    int safety = 48;

    while (cap_ptr && safety-- > 0) {
        uint8_t cap_id = PciRead8(cap_ptr);
        if (cap_id == 0x09) { // vendor-specific (virtio)
            uint8_t cfg_type = PciRead8(cap_ptr + 3);
            uint8_t bar      = PciRead8(cap_ptr + 4);
            uint32_t offset  = PciRead32(cap_ptr + 8);

            volatile uint8_t* bar_base = resolve_bar(pci_bus, pci_dev, pci_func, bar);
            if (bar_base) {
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
        }
        cap_ptr = PciRead8(cap_ptr + 1) & 0xFC;
    }

    s_common_cfg = common_cfg;

    // modern virtio requires the common config capability; without it we
    // cannot program rings, so reject the device instead of pretending to work
    if (!common_cfg) return false;

    if (common_cfg) {
        volatile uint8_t*  device_status     = common_cfg + 20;
        volatile uint32_t* device_feature_sel = (volatile uint32_t*)(common_cfg + 0);
        volatile uint32_t* device_feature     = (volatile uint32_t*)(common_cfg + 4);
        volatile uint32_t* driver_feature_sel = (volatile uint32_t*)(common_cfg + 8);
        volatile uint32_t* driver_feature     = (volatile uint32_t*)(common_cfg + 12);

        // reset device, then wait for status to read back 0
        *device_status = 0;
        __asm__ volatile("mfence" ::: "memory");
        for (int i = 0; i < 1000 && *device_status != 0; i++) {
            for (volatile int d = 0; d < 1000; d++);
        }

        *device_status = VIRTIO_STATUS_ACKNOWLEDGE;
        __asm__ volatile("mfence" ::: "memory");
        *device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
        __asm__ volatile("mfence" ::: "memory");

        // negotiate features: keep only the bits we understand
        *device_feature_sel = 0;
        __asm__ volatile("mfence" ::: "memory");
        uint32_t feats_lo = *device_feature;
        *device_feature_sel = 1;
        __asm__ volatile("mfence" ::: "memory");
        uint32_t feats_hi = *device_feature;

        uint32_t want_lo = feats_lo & (1u << VIRTIO_F_INDIRECT_DESC_BIT);
        uint32_t want_hi = feats_hi & (1u << 0); // VIRTIO_F_VERSION_1 (bit 32)
        s_indirect_supported = (want_lo & (1u << VIRTIO_F_INDIRECT_DESC_BIT)) != 0;

        *driver_feature_sel = 0;
        __asm__ volatile("mfence" ::: "memory");
        *driver_feature = want_lo;
        *driver_feature_sel = 1;
        __asm__ volatile("mfence" ::: "memory");
        *driver_feature = want_hi;

        *device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
        __asm__ volatile("mfence" ::: "memory");

        if (!(*device_status & VIRTIO_STATUS_FEATURES_OK)) {
            *device_status = VIRTIO_STATUS_FAILED;
            return false;
        }
    }

    if (!InitVirtQueue(0, &controlq)) return false;
    if (!InitVirtQueue(1, &cursorq)) {
        // cursor queue is optional - free the control queue + tear down on hard fail
        // but cursor failure shouldn't bring the device down. just disable cursor ops.
        cursorq.desc = nullptr;
    }

    if (s_indirect_supported) {
        s_indirect_table = (VirtQueueDesc*)KernelHeap::Alloc(sizeof(VirtQueueDesc) * 8);
        if (!s_indirect_table) s_indirect_supported = false;
    }

    if (common_cfg) {
        volatile uint8_t* device_status = common_cfg + 20;
        *device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                         VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
        __asm__ volatile("mfence" ::: "memory");
    }

    detected = true;

    uint32_t w = 0, h = 0;
    if (GetDisplayInfo(0, &w, &h) && w && h) {
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
    cmd.resource_id = next_resource_id;
    cmd.format = format;
    cmd.width = width;
    cmd.height = height;

    VirtGPU_CtrlHdr resp = {};
    if (!SendControlCommand(&cmd, sizeof(cmd), &resp, sizeof(resp)))
        return 0;

    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return 0;

    next_resource_id++;
    return cmd.resource_id;
}

bool VirtIOGPU::AttachBacking(uint32_t resource_id, void* buffer, uint32_t size) {
    if (!detected) return false;

    struct {
        VirtGPU_ResourceAttachBacking cmd;
        VirtGPU_MemEntry entry;
    } __attribute__((packed)) request = {};

    request.cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.cmd.resource_id = resource_id;
    request.cmd.nr_entries = 1;
    request.entry.addr = KernelPhysOf(buffer);
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
    cmd.scanout_id = (uint32_t)scanout_id;
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
    return PresentFramebufferRect(pixels, width, height, 0, 0, width, height);
}

void VirtIOGPU::SetSyncPresent(bool on) { s_present_sync = on; }

void VirtIOGPU::DrainPresent() {
    if (!s_present_inflight) return;
    VirtQueue* vq = &controlq;
    uint64_t t0 = vg_rdtsc();
    for (int i = 0; i < 2000000; i++) {
        __asm__ volatile("mfence" ::: "memory");
        if ((int16_t)(vq->used->idx - s_present_expected) >= 0) {
            vq->last_used_idx = s_present_expected;
            break;
        }
        for (volatile int d = 0; d < 16; d++);
    }
    uint64_t waited = vg_rdtsc() - t0;
    // reclaim regardless of timeout - a leaked descriptor pair would wedge the
    // ring, which is worse than a dropped frame. (satoru)
    vq->desc[s_present_head_a].next = vq->free_head;
    vq->free_head = s_present_head_a;
    vq->num_free++;
    vq->desc[s_present_head_b].next = vq->free_head;
    vq->free_head = s_present_head_b;
    vq->num_free++;
    s_present_inflight = false;

    // prove the overlap actually works: `waitdrain` is what is LEFT of the
    // transfer wait after the compositor's work overlapped it. it was 1127us
    // synchronously; if async is doing its job this is ~0. (satoru)
    static uint32_t s_n = 0;
    static uint64_t s_sub = 0, s_wait = 0;
    s_sub  += s_present_submit_cycles;
    s_wait += waited;
    if (++s_n >= 120) {
        SerialLogger::Log("[vgtime] per-frame kcycles submit=");
        SerialLogger::LogDec((int)(s_sub / s_n / 1000));
        SerialLogger::Log(" waitdrain=");
        SerialLogger::LogDec((int)(s_wait / s_n / 1000));
        SerialLogger::Log(" n=");
        SerialLogger::LogDec((int)s_n);
        SerialLogger::Log("\r\n");
        s_n = 0; s_sub = 0; s_wait = 0;
    }
}

// PERF NOTE (measured 2026-07-27): the virtio present's synchronous wait below
// is ~4.2ms/frame and caps this backend at ~236fps, while the plain framebuffer
// path does ~2030fps - the "accelerated" backend is 8.6x SLOWER, and the cost
// is the host ROUND-TRIP, not the pixels. an async restructure (submit here,
// retire at the top of the next present) was implemented and measured: it made
// things slightly WORSE (~210fps), because one frame of overlap only hides the
// composite. the real fix needs multiple frames genuinely in flight, i.e. more
// than one scanout resource to write into, plus an interrupt/fence instead of a
// busy-poll. see the memory note framerate-virtio-is-the-bottleneck. (satoru)

bool VirtIOGPU::PresentFramebufferRect(void* pixels, uint32_t width, uint32_t height,
                                       uint32_t dx, uint32_t dy,
                                       uint32_t dw, uint32_t dh) {
    if (!detected || !pixels) return false;
    uint64_t t_present_start = vg_rdtsc();   // [vgtime] phase instrument (satoru)
    // clamp the damage box; a degenerate one means "everything". (satoru)
    if (dw == 0 || dh == 0 || dx >= width || dy >= height) {
        dx = 0; dy = 0; dw = width; dh = height;
    }
    if (dx + dw > width)  dw = width  - dx;
    if (dy + dh > height) dh = height - dy;

    if (fb_resource_id == 0) {
        fb_resource_id = CreateResource2D(width, height, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
        if (fb_resource_id == 0) return false;

        uint32_t size = width * height * 4;
        if (!AttachBacking(fb_resource_id, pixels, size)) {
            DestroyResource(fb_resource_id);
            fb_resource_id = 0;
            return false;
        }
        if (!SetScanout(0, fb_resource_id, 0, 0, width, height)) {
            DestroyResource(fb_resource_id);
            fb_resource_id = 0;
            return false;
        }
    }

    // pipelined submission: post xfer and flush into the ring back-to-back
    // and only poll for the final used-idx update. saves one notify-and-wait
    // round trip per frame, which is the dominant cost at 60+ Hz.
    if (s_indirect_supported && s_indirect_table && controlq.num_free >= 2) {
        // MUST be persistent: the present is async, so the host reads these
        // command buffers AFTER we return. as stack locals they would be a
        // use-after-scope and the host would build the frame from whatever
        // reused that stack. safe as file-scope because exactly one frame is
        // ever in flight and the only caller is the gui loop under g_fb_lock.
        // (satoru)
        static VirtGPU_TransferToHost2D xfer;
        static VirtGPU_ResourceFlush    flush;
        static VirtGPU_CtrlHdr          resp_xfer;
        static VirtGPU_CtrlHdr          resp_flush;
        xfer = {};
        xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        xfer.rect.x = dx;
        xfer.rect.y = dy;
        xfer.rect.width = dw;
        xfer.rect.height = dh;
        // offset is where THIS rect's pixels start inside the attached backing,
        // not the rect origin - the backing is one linear width-strided buffer.
        // a sub-rect transfer with offset 0 would upload the wrong rows. (satoru)
        xfer.offset = ((uint64_t)dy * (uint64_t)width + (uint64_t)dx) * 4ull;
        xfer.resource_id = fb_resource_id;

        flush = {};
        flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        flush.rect.x = dx;
        flush.rect.y = dy;
        flush.rect.width = dw;
        flush.rect.height = dh;
        flush.resource_id = fb_resource_id;

        resp_xfer = {};
        resp_flush = {};

        VirtQueue* vq = &controlq;

        uint16_t head_a = vq->free_head;
        vq->free_head = vq->desc[head_a].next;
        vq->num_free--;
        uint16_t head_b = vq->free_head;
        vq->free_head = vq->desc[head_b].next;
        vq->num_free--;

        // two separate indirect tables: [0],[1] = xfer pair (based at &[0]);
        // [4],[5] = flush pair (based at &[4]). the `next` field of an indirect
        // descriptor is an index RELATIVE to its own table base, so the flush
        // head's next must be 1 (-> [5]), NOT the absolute 5 - pointing at 5 in a
        // 2-entry table is out of range and makes qemu reject the chain
        // ("Desc next is 5"), which breaks the virtqueue and freezes the desktop
        // once a real display drains the queue. (satoru)
        s_indirect_table[0] = { KernelPhysOf(&xfer),       sizeof(xfer),       VIRTQ_DESC_F_NEXT, 1 };
        s_indirect_table[1] = { KernelPhysOf(&resp_xfer),  sizeof(resp_xfer),  VIRTQ_DESC_F_WRITE, 0 };
        s_indirect_table[4] = { KernelPhysOf(&flush),      sizeof(flush),      VIRTQ_DESC_F_NEXT, 1 };
        s_indirect_table[5] = { KernelPhysOf(&resp_flush), sizeof(resp_flush), VIRTQ_DESC_F_WRITE, 0 };

        vq->desc[head_a].addr = KernelPhysOf(&s_indirect_table[0]);
        vq->desc[head_a].len  = sizeof(VirtQueueDesc) * 2;
        vq->desc[head_a].flags = VIRTQ_DESC_F_INDIRECT;
        vq->desc[head_a].next = 0;

        vq->desc[head_b].addr = KernelPhysOf(&s_indirect_table[4]);
        vq->desc[head_b].len  = sizeof(VirtQueueDesc) * 2;
        vq->desc[head_b].flags = VIRTQ_DESC_F_INDIRECT;
        vq->desc[head_b].next = 0;

        __asm__ volatile("mfence" ::: "memory");
        uint16_t s0 = vq->avail->idx % vq->size;
        vq->avail->ring[s0] = head_a;
        __asm__ volatile("" ::: "memory");
        uint16_t s1 = (vq->avail->idx + 1) % vq->size;
        vq->avail->ring[s1] = head_b;
        __asm__ volatile("mfence" ::: "memory");
        vq->avail->idx += 2;
        __asm__ volatile("mfence" ::: "memory");
        NotifyQueue(0);

        // MEASURED: making this async (submit here, retire at the top of the
        // next present) did NOT help - virtio went ~236fps -> ~210fps. one
        // frame of overlap only hides the COMPOSITE (~0.5ms at fpsbench
        // rates) while the host round-trip is ~4.2ms, so there is nothing to
        // overlap with and the bookkeeping costs a little. the win needs
        // several frames genuinely in flight, which needs more than one
        // scanout resource to write into - a real change, not a reordering.
        // keeping the simple synchronous wait until that exists. (satoru)
        // ASYNC: do not wait here. MEASURED phase split (2883MHz, fpsbench):
        // submit 38us, wait-for-transfer 1127us, wait-for-flush 4.5us. the
        // whole cost is the transfer wait, and the compositor spends ~1.9ms
        // building the NEXT frame before it touches this buffer again - which
        // is longer than the 1.13ms wait. so hand the frame to the host and
        // let DrainPresent() collect it at the top of the next SwapBuffers,
        // by which point it is already done and the wait costs ~0.
        //
        // an earlier attempt put the drain at the top of the next PRESENT,
        // which does NOT work: that is after the composite AND after the copy
        // into this buffer, i.e. too late to be safe and too early to be free.
        // the drain has to sit immediately before the first write to the
        // framebuffer. (satoru)
        s_present_inflight = true;
        s_present_expected = (uint16_t)(vq->last_used_idx + 2);
        s_present_head_a   = head_a;
        s_present_head_b   = head_b;
        s_present_submit_cycles = vg_rdtsc() - t_present_start;
        // a/b arm: drain right here to reproduce the old blocking behaviour. (satoru)
        if (s_present_sync) DrainPresent();
        return true;
    }

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
    append("  Control Queue Size: "); append_num(controlq.size); append("\n");
    append("  Indirect Descriptors: "); append(s_indirect_supported ? "yes\n" : "no\n");

    out[pos] = 0;
}
