//  kurono os - generic virtio 1.0 pci device model (modern transport only)
//
//  one VirtioDev per device (blk, vsock, ...). each gets its own pci slot, a
//  64 kb bar4 (common / isr / device cfg / notify) and its own legacy pic
//  irq line, delivered edge-style: every completed buffer pulses the line.
//  device specific code supplies a notify callback and drives the queues with
//  VirtioPop / VirtioPush. everything runs on the vm thread between guest
//  entries, so no locking is needed. (satoru)
#pragma once
#include "../kernel/types.h"
#include "vpci.h"

constexpr int VIRTIO_DEV_MAX_Q    = 4;
constexpr int VIRTIO_CHAIN_MAX    = 24;     // descriptors followed per request
constexpr uint32_t VIRTIO_BAR_SIZE = 0x10000;

struct VirtioQueue {
    uint16_t size;          // negotiated (power of two)
    uint16_t enable;
    uint64_t desc, avail, used;
    uint16_t last_avail;    // next avail index the host will consume
};

struct VirtioChain {
    uint16_t head;
    int      n;             // descriptors
    uint64_t addr[VIRTIO_CHAIN_MAX];
    uint32_t len[VIRTIO_CHAIN_MAX];
    bool     write[VIRTIO_CHAIN_MAX];   // true = device writes this buffer
    bool     truncated;                 // longer than VIRTIO_CHAIN_MAX or malformed (satoru)
};

struct VirtioDev {
    // filled in by the device before VirtioRegister
    const char* name;
    uint16_t    devtype;        // virtio device id (2 = blk, 19 = vsock ...)
    uint8_t     pci_class, pci_subclass;
    uint8_t     irq;            // legacy pic line (5, 9, 10, 11)
    int         nqueues;
    uint16_t    queue_max;      // max queue size offered (power of two)
    uint64_t    features;       // device feature bits (VERSION_1 added here)
    uint8_t     devcfg[64];
    uint32_t    devcfg_len;
    void      (*notify)(VirtioDev* d, int qid);
    void      (*reset)(VirtioDev* d);
    void*       priv;

    // runtime
    int         slot;
    VPCIDevice  proto;
    uint8_t     isr;
    uint8_t     status;
    uint32_t    dfs, gfs;
    uint64_t    drv_features;
    uint16_t    qsel;
    uint8_t     cfg_gen;
    VirtioQueue q[VIRTIO_DEV_MAX_Q];
    uint32_t    irq_count;
};

bool VirtioRegister(VirtioDev* d);
bool VirtioDriverOk(const VirtioDev* d);                 // status has DRIVER_OK
bool VirtioPop(VirtioDev* d, int qid, VirtioChain* c);   // next available request
void VirtioPush(VirtioDev* d, int qid, uint16_t head, uint32_t written);
void VirtioRaise(VirtioDev* d);                          // pulse the irq line
void VirtioConfigChanged(VirtioDev* d);                  // isr bit 1 + irq (satoru)
// total bytes of the device-writable (true) or device-readable (false) buffers (satoru)
uint32_t VirtioChainLen(const VirtioChain* c, bool device_writable);
bool VirtioHasAvail(VirtioDev* d, int qid);
// copy between a chain's buffers (device-readable / device-writable) and host memory
uint32_t VirtioReadChain(const VirtioChain* c, uint32_t skip, void* dst, uint32_t n);
uint32_t VirtioWriteChain(const VirtioChain* c, uint32_t skip, const void* src, uint32_t n);
// poll all queues for work the guest queued without a doorbell (safety net)
void VirtioPollAll();
VirtioDev* VirtioByIndex(int i);
int VirtioCount();
void VirtioResetAll();

// end (satoru)
