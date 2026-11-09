//  kurono os - generic virtio 1.0 pci device model (see virtio_dev.h)
#include "virtio_dev.h"
#include "guest_mem.h"
#include "vdevices.h"
#include "../drivers/serial.h"
#include "../kernel/types.h"

#define VPC_DFSEL   0x00
#define VPC_DF      0x04
#define VPC_GFSEL   0x08
#define VPC_GF      0x0C
#define VPC_MSIXCFG 0x10
#define VPC_NUMQ    0x12
#define VPC_STATUS  0x14
#define VPC_CFGGEN  0x15
#define VPC_QSEL    0x16
#define VPC_QSIZE   0x18
#define VPC_QMSIX   0x1A
#define VPC_QEN     0x1C
#define VPC_QNOFF   0x1E
#define VPC_QDESC   0x20
#define VPC_QAVAIL  0x28
#define VPC_QUSED   0x30

#define BAR_COMMON  0x0000
#define BAR_ISR     0x1000
#define BAR_DEVCFG  0x2000
#define BAR_NOTIFY  0x3000

#define VIRTIO_F_VERSION_1 (1ULL << 32)

static const int MAX_VIRTIO_DEVS = 8;
static VirtioDev* g_devs[MAX_VIRTIO_DEVS];
static int g_ndevs = 0;

static inline void put_le(uint8_t* p, uint32_t off, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[off + i] = (uint8_t)(v >> (8 * i));
}

// pcie vendor capability (virtio_pci_cap), 16 bytes + 4 for notify
static void write_cap(VirtioDev* d, uint8_t off, uint8_t next, uint8_t cfg_type,
                      uint32_t bar_off, uint32_t len, uint8_t extra) {
    uint8_t* c = d->proto.cfg + off;
    c[0] = 0x09;                 // PCI_CAP_ID_VNDR
    c[1] = next;
    c[2] = (uint8_t)(16 + extra);
    c[3] = cfg_type;
    c[4] = 4;                    // bar 4
    c[5] = c[6] = c[7] = 0;
    put_le(c, 8, bar_off, 4);
    put_le(c, 12, len, 4);
}

static uint64_t qfield_get(VirtioDev* d, uint32_t off) {
    if (d->qsel >= (uint16_t)d->nqueues) return 0;
    VirtioQueue& q = d->q[d->qsel];
    switch (off) {
        case VPC_QSIZE:  return q.size;
        case VPC_QEN:    return q.enable;
        case VPC_QNOFF:  return d->qsel;
        case VPC_QMSIX:  return 0xFFFF;
        case VPC_QDESC:  return q.desc & 0xFFFFFFFFu;
        case VPC_QDESC + 4: return q.desc >> 32;
        case VPC_QAVAIL: return q.avail & 0xFFFFFFFFu;
        case VPC_QAVAIL + 4: return q.avail >> 32;
        case VPC_QUSED:  return q.used & 0xFFFFFFFFu;
        case VPC_QUSED + 4: return q.used >> 32;
    }
    return 0;
}

static void do_reset(VirtioDev* d) {
    d->status = 0; d->isr = 0; d->drv_features = 0; d->dfs = d->gfs = 0; d->qsel = 0;
    for (int i = 0; i < VIRTIO_DEV_MAX_Q; i++) {
        uint16_t sz = d->q[i].size ? d->q[i].size : d->queue_max;
        d->q[i] = {};
        d->q[i].size = d->queue_max;
        (void)sz;
    }
    if (d->reset) d->reset(d);
}

static bool bar_read(VPCIDevice* pd, int bar, uint32_t off, uint8_t size, uint32_t& value) {
    VirtioDev* d = (VirtioDev*)pd->user;
    if (!d || bar != 4) return false;
    value = 0;
    if (off < BAR_ISR) {
        uint32_t o = off - BAR_COMMON;
        switch (o) {
            case VPC_DFSEL: value = d->dfs; return true;
            case VPC_DF: value = d->dfs ? (uint32_t)(d->features >> 32) : (uint32_t)d->features; return true;
            case VPC_GFSEL: value = d->gfs; return true;
            case VPC_GF: value = d->gfs ? (uint32_t)(d->drv_features >> 32) : (uint32_t)d->drv_features; return true;
            case VPC_MSIXCFG: value = 0xFFFF; return true;
            case VPC_NUMQ: value = (uint32_t)d->nqueues; return true;
            case VPC_STATUS: value = d->status; return true;
            case VPC_CFGGEN: value = d->cfg_gen; return true;
            case VPC_QSEL: value = d->qsel; return true;
            default: break;
        }
        uint32_t base = o & ~3u;
        if (o >= VPC_QSIZE && o < VPC_QDESC) {
            // 16 bit fields at 0x18..0x1F
            value = (uint32_t)qfield_get(d, o & ~1u);
            if (size == 1 && (o & 1)) value >>= 8;
            value &= (size == 1) ? 0xFF : 0xFFFF;
            return true;
        }
        if (base >= VPC_QDESC && base <= VPC_QUSED + 4) { value = (uint32_t)qfield_get(d, base); return true; }
        return true;
    }
    if (off < BAR_DEVCFG) {          // isr: read clears
        value = d->isr; d->isr = 0;
        // and deasserts intx, so a level-triggered (elcr) line drops (satoru)
        if (d->irq < 8) VirtualDevices::GetMasterPIC().SetIRQ(d->irq, false);
        else VirtualDevices::GetSlavePIC().SetIRQ(d->irq - 8, false);
        return true;
    }
    if (off < BAR_NOTIFY) {
        uint32_t o = off - BAR_DEVCFG;
        for (int i = 0; i < size && i < 4; i++)
            if (o + i < sizeof(d->devcfg)) value |= (uint32_t)d->devcfg[o + i] << (8 * i);
        return true;
    }
    return true;
}

static bool bar_write(VPCIDevice* pd, int bar, uint32_t off, uint8_t size, uint32_t value) {
    VirtioDev* d = (VirtioDev*)pd->user;
    if (!d || bar != 4) return false;
    (void)size;
    if (off < BAR_ISR) {
        uint32_t o = off - BAR_COMMON;
        switch (o) {
            case VPC_DFSEL: d->dfs = value; return true;
            case VPC_GFSEL: d->gfs = value; return true;
            case VPC_GF: {
                uint64_t m = 0xFFFFFFFFull << (d->gfs ? 32 : 0);
                d->drv_features = (d->drv_features & ~m) | (((uint64_t)value << (d->gfs ? 32 : 0)) & m);
                return true;
            }
            case VPC_STATUS:
                if ((value & 0xFF) == 0) do_reset(d); else d->status = (uint8_t)value;
                return true;
            case VPC_QSEL: d->qsel = (uint16_t)value; return true;
            default: break;
        }
        if (d->qsel >= (uint16_t)d->nqueues) return true;
        VirtioQueue& q = d->q[d->qsel];
        switch (o) {
            case VPC_QSIZE: {
                uint16_t v = (uint16_t)value;
                if (v && v <= d->queue_max && (v & (v - 1)) == 0) q.size = v;
                return true;
            }
            case VPC_QEN: q.enable = (uint16_t)value; if (q.enable) q.last_avail = 0; return true;
            case VPC_QDESC: q.desc = (q.desc & ~0xFFFFFFFFull) | value; return true;
            case VPC_QDESC + 4: q.desc = (q.desc & 0xFFFFFFFFull) | ((uint64_t)value << 32); return true;
            case VPC_QAVAIL: q.avail = (q.avail & ~0xFFFFFFFFull) | value; return true;
            case VPC_QAVAIL + 4: q.avail = (q.avail & 0xFFFFFFFFull) | ((uint64_t)value << 32); return true;
            case VPC_QUSED: q.used = (q.used & ~0xFFFFFFFFull) | value; return true;
            case VPC_QUSED + 4: q.used = (q.used & 0xFFFFFFFFull) | ((uint64_t)value << 32); return true;
        }
        return true;
    }
    if (off >= BAR_NOTIFY) {
        uint32_t qid = (off - BAR_NOTIFY) / 4;
        if (qid < (uint32_t)d->nqueues && d->notify) d->notify(d, (int)qid);
        return true;
    }
    return true;
}

bool VirtioRegister(VirtioDev* d) {
    if (g_ndevs >= MAX_VIRTIO_DEVS) return false;
    d->features |= VIRTIO_F_VERSION_1;
    for (int i = 0; i < VIRTIO_DEV_MAX_Q; i++) { d->q[i] = {}; d->q[i].size = d->queue_max; }
    d->proto = {};
    d->proto.name = d->name;
    d->proto.irq_line = d->irq;
    d->proto.user = d;
    VPCI::Cfg16(&d->proto, PCI_VENDOR_ID, 0x1AF4);
    VPCI::Cfg16(&d->proto, PCI_DEVICE_ID, (uint16_t)(0x1040 + d->devtype));
    VPCI::Cfg16(&d->proto, PCI_COMMAND, 0);
    VPCI::Cfg8(&d->proto, PCI_REVISION_ID, 1);
    VPCI::Cfg8(&d->proto, PCI_PROG_IF, 0);
    VPCI::Cfg8(&d->proto, PCI_SUBCLASS, d->pci_subclass);
    VPCI::Cfg8(&d->proto, PCI_CLASS, d->pci_class);
    VPCI::Cfg16(&d->proto, PCI_SUBSYS_VEND, 0x1AF4);
    VPCI::Cfg16(&d->proto, PCI_SUBSYS_ID, d->devtype);
    d->proto.bars[4].size = VIRTIO_BAR_SIZE;
    d->proto.bars[4].is_mmio = true;
    d->proto.bars[4].is_64bit = true;
    d->proto.bars[4].prefetch = true;
    d->proto.bar_read = bar_read;
    d->proto.bar_write = bar_write;
    write_cap(d, 0x40, 0x50, 1, BAR_COMMON, 0x40, 0);     // common
    write_cap(d, 0x50, 0x60, 3, BAR_ISR, 4, 0);           // isr
    write_cap(d, 0x60, 0x70, 4, BAR_DEVCFG, 0x40, 0);     // device cfg
    write_cap(d, 0x70, 0x00, 2, BAR_NOTIFY, 0x1000, 4);   // notify (+mult)
    put_le(d->proto.cfg, 0x70 + 16, 4, 4);                // notify_off_multiplier
    d->proto.cfg[PCI_CAP_PTR] = 0x40;
    VPCI::Cfg16(&d->proto, PCI_STATUS, 0x0010);
    int slot = VPCI::RegisterDevice(&d->proto);
    if (slot < 0) return false;
    d->slot = slot;
    // RegisterDevice copied the proto into its slot; keep the user pointer valid
    VPCIDevice* canon = VPCI::GetDevice(slot);
    if (canon) canon->user = d;
    d->isr = 0; d->status = 0;
    g_devs[g_ndevs++] = d;
    SerialLogger::Log("[virtio] ");
    SerialLogger::Log(d->name);
    SerialLogger::Log(" at slot "); SerialLogger::LogDec(slot);
    SerialLogger::Log(" irq "); SerialLogger::LogDec(d->irq);
    SerialLogger::Log("\r\n");
    return true;
}

bool VirtioDriverOk(const VirtioDev* d) { return (d->status & 4) != 0; }

// host pointer for n bytes of guest memory, or null when they are not all in
// one host-contiguous piece. guest ram is 4 mb chunks that are not adjacent on
// the host, so every ring element is resolved on its own address (they are
// naturally aligned and never straddle a chunk) and bulk copies split by
// ContigBytes. (satoru)
static inline uint8_t* gp(uint64_t pa, uint32_t n) {
    uint8_t* p = GuestMemoryManager::GuestPhysToHost(pa);
    if (!p || GuestMemoryManager::ContigBytes(pa) < n) return nullptr;
    return p;
}

static inline bool rd16g(uint64_t pa, uint16_t* v) {
    uint8_t* p = gp(pa, 2);
    if (!p) return false;
    *v = *(volatile uint16_t*)p;
    return true;
}

bool VirtioHasAvail(VirtioDev* d, int qid) {
    VirtioQueue& q = d->q[qid];
    if (!q.enable || !q.avail) return false;
    uint16_t idx;
    if (!rd16g(q.avail + 2, &idx)) return false;
    return idx != q.last_avail;
}

bool VirtioPop(VirtioDev* d, int qid, VirtioChain* c) {
    VirtioQueue& q = d->q[qid];
    if (!q.enable || !q.avail || !q.used || !q.desc || !q.size) return false;
    uint16_t idx;
    if (!rd16g(q.avail + 2, &idx)) return false;
    if (idx == q.last_avail) return false;
    // a guest that publishes more than a ring of entries is broken; resync
    // instead of walking garbage (satoru)
    if ((uint16_t)(idx - q.last_avail) > q.size) { q.last_avail = idx; return false; }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint16_t head;
    if (!rd16g(q.avail + 4 + (uint64_t)(q.last_avail % q.size) * 2, &head)) return false;
    q.last_avail++;
    c->head = head; c->n = 0; c->truncated = false;
    uint16_t i = head;
    for (int guard = 0; ; guard++) {
        if (i >= q.size || guard > VIRTIO_CHAIN_MAX) { c->truncated = true; break; }
        uint8_t* dp = gp(q.desc + (uint64_t)i * 16, 16);
        if (!dp) { c->truncated = true; break; }
        uint64_t addr = *(volatile uint64_t*)dp; uint32_t len = *(volatile uint32_t*)(dp + 8);
        uint16_t fl = *(volatile uint16_t*)(dp + 12), nx = *(volatile uint16_t*)(dp + 14);
        if (fl & 4) { c->truncated = true; break; }      // indirect not offered
        if (c->n >= VIRTIO_CHAIN_MAX) { c->truncated = true; break; }
        c->addr[c->n] = addr; c->len[c->n] = len; c->write[c->n] = (fl & 2) != 0; c->n++;
        if (!(fl & 1)) break;
        i = nx;
    }
    return true;
}

void VirtioPush(VirtioDev* d, int qid, uint16_t head, uint32_t written) {
    VirtioQueue& q = d->q[qid];
    if (!q.used || !q.size) return;
    uint8_t* ip = gp(q.used + 2, 2);
    if (!ip) return;
    uint16_t idx = *(volatile uint16_t*)ip;
    uint8_t* e = gp(q.used + 4 + (uint64_t)(idx % q.size) * 8, 8);
    if (!e) return;
    *(volatile uint32_t*)e = head; *(volatile uint32_t*)(e + 4) = written;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(volatile uint16_t*)ip = (uint16_t)(idx + 1);
}

void VirtioConfigChanged(VirtioDev* d) {
    d->cfg_gen++;
    d->isr |= 2;
    d->irq_count++;
    if (d->irq < 8) VirtualDevices::GetMasterPIC().SetIRQ(d->irq, true);
    else VirtualDevices::GetSlavePIC().SetIRQ(d->irq - 8, true);
}

void VirtioRaise(VirtioDev* d) {
    d->isr |= 1;
    d->irq_count++;
    if (d->irq < 8) VirtualDevices::GetMasterPIC().SetIRQ(d->irq, true);
    else VirtualDevices::GetSlavePIC().SetIRQ(d->irq - 8, true);
}

// copies split at host chunk boundaries (satoru)
uint32_t VirtioReadChain(const VirtioChain* c, uint32_t skip, void* dst, uint32_t n) {
    uint8_t* o = (uint8_t*)dst; uint32_t done = 0;
    for (int i = 0; i < c->n && done < n; i++) {
        if (c->write[i]) continue;
        uint32_t l = c->len[i];
        if (skip >= l) { skip -= l; continue; }
        uint32_t take = l - skip; if (take > n - done) take = n - done;
        uint64_t pa = c->addr[i] + skip;
        while (take) {
            uint8_t* p = GuestMemoryManager::GuestPhysToHost(pa);
            uint32_t run = GuestMemoryManager::ContigBytes(pa);
            if (!p || !run) return done;
            if (run > take) run = take;
            memcpy(o + done, p, run);
            done += run; pa += run; take -= run;
        }
        skip = 0;
    }
    return done;
}

uint32_t VirtioWriteChain(const VirtioChain* c, uint32_t skip, const void* src, uint32_t n) {
    const uint8_t* s = (const uint8_t*)src; uint32_t done = 0;
    for (int i = 0; i < c->n && done < n; i++) {
        if (!c->write[i]) continue;
        uint32_t l = c->len[i];
        if (skip >= l) { skip -= l; continue; }
        uint32_t take = l - skip; if (take > n - done) take = n - done;
        uint64_t pa = c->addr[i] + skip;
        while (take) {
            uint8_t* p = GuestMemoryManager::GuestPhysToHost(pa);
            uint32_t run = GuestMemoryManager::ContigBytes(pa);
            if (!p || !run) return done;
            if (run > take) run = take;
            memcpy(p, s + done, run);
            done += run; pa += run; take -= run;
        }
        skip = 0;
    }
    return done;
}

void VirtioPollAll() {
    for (int i = 0; i < g_ndevs; i++) {
        VirtioDev* d = g_devs[i];
        if (!VirtioDriverOk(d) || !d->notify) continue;
        for (int q = 0; q < d->nqueues; q++)
            if (VirtioHasAvail(d, q)) d->notify(d, q);
    }
}

VirtioDev* VirtioByIndex(int i) { return (i >= 0 && i < g_ndevs) ? g_devs[i] : nullptr; }
int VirtioCount() { return g_ndevs; }
void VirtioResetAll() { g_ndevs = 0; }

uint32_t VirtioChainLen(const VirtioChain* c, bool device_writable) {
    uint32_t t = 0;
    for (int i = 0; i < c->n; i++) if (c->write[i] == device_writable) t += c->len[i];
    return t;
}

// end (satoru)
