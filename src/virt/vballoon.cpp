//  kurono os - virtio-balloon host device (see vballoon.h)
#include "vballoon.h"
#include "virtio_dev.h"
#include "hypervisor.h"
#include "../drivers/serial.h"

namespace {
    VirtioDev g_dev;
    bool g_have = false;
    uint64_t g_released = 0;
    uint32_t g_reports = 0;

    const int Q_REPORT = 2;                 // inflate 0, deflate 1, reporting 2 (stats and hints are off)
    const uint64_t F_REPORTING = 1ULL << 5; // VIRTIO_BALLOON_F_REPORTING

    void on_notify(VirtioDev* d, int qid) {
        VirtioChain c;
        int n = 0;
        if (qid == Q_REPORT) {
            while (VirtioPop(d, qid, &c)) {
                g_reports++;
                for (int i = 0; i < c.n; i++) g_released += Hypervisor::ReleaseGuestRam(c.addr[i], c.len[i]);
                VirtioPush(d, qid, c.head, 0);
                n++;
            }
        } else {
            // inflate / deflate are not offered; complete anything the guest queues anyway
            while (VirtioPop(d, qid, &c)) { VirtioPush(d, qid, c.head, 0); n++; }
        }
        if (n) VirtioRaise(d);
    }
}

bool VBalloon::Init(uint8_t irq) {
    g_dev = {};
    g_dev.name = "virtio-balloon";
    g_dev.devtype = 5;
    g_dev.pci_class = 0x00; g_dev.pci_subclass = 0xFF;
    g_dev.irq = irq;
    g_dev.nqueues = 3;
    g_dev.queue_max = 128;
    g_dev.features = F_REPORTING;
    g_dev.devcfg_len = 16;                  // num_pages, actual, free_page_hint_cmd_id, poison_val: all 0
    g_dev.notify = on_notify;
    g_released = 0; g_reports = 0;
    g_have = VirtioRegister(&g_dev);
    return g_have;
}

uint64_t VBalloon::ReleasedBytes() { return g_released; }
uint32_t VBalloon::Reports() { return g_reports; }

// end (satoru)
