//  kurono os - virtio-console host device (see vcon.h)
#include "vcon.h"
#include "virtio_dev.h"
#include "guest_mem.h"
#include "../drivers/serial.h"
#include "../proc/spinlock.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"

namespace {
    const uint32_t IN_RING = 262144;          // host -> guest, bytes waiting for rx buffers (bridge frames, keymaps) (satoru)

    struct Con {
        VirtioDev dev;
        bool      up;
        uint8_t*  in;                          // 256 kb, allocated with the vm (satoru)
        volatile uint32_t in_w, in_r;         // in_w: writers (locked), in_r: vm thread
        Spinlock  in_lock;
        uint16_t  cols, rows;
        volatile bool size_dirty;
        uint64_t  bytes_out, bytes_in;
        void    (*sink)(int idx, const uint8_t* data, int n);
        bool    (*flow)(int idx);              // nullptr = always drain (satoru)
    };
    Con g_con[GuestConsole::MAX_CONSOLES];

    int index_of(VirtioDev* d) {
        for (int i = 0; i < GuestConsole::MAX_CONSOLES; i++) if (&g_con[i].dev == d) return i;
        return -1;
    }

    // guest -> host: every transmit chain is consumed at once (the guest's hvc
    // put_chars busy-waits for it), its bytes go to the sink (satoru)
    void drain_tx(int idx) {
        Con& c = g_con[idx];
        VirtioChain ch;
        int n = 0;
        // the sink may ask us to hold the queue (the audio bridge does while
        // its ring is full): leaving the buffers pending blocks the guest. (satoru)
        while ((!c.flow || c.flow(idx)) && VirtioPop(&c.dev, 1, &ch)) {
            for (int i = 0; i < ch.n; i++) {
                if (ch.write[i]) continue;
                uint32_t len = ch.len[i], off = 0;
                while (off < len) {
                    uint64_t gpa = ch.addr[i] + off;
                    uint8_t* p = GuestMemoryManager::GuestPhysToHost(gpa);
                    uint32_t run = GuestMemoryManager::ContigBytes(gpa);
                    if (!p || run == 0) break;
                    if (run > len - off) run = len - off;
                    c.bytes_out += run;
                    if (c.sink) c.sink(idx, p, (int)run);
                    off += run;
                }
            }
            VirtioPush(&c.dev, 1, ch.head, 0);
            n++;
        }
        if (n) VirtioRaise(&c.dev);
    }

    // host -> guest: fill posted receive buffers from the input ring (satoru)
    void fill_rx(int idx) {
        Con& c = g_con[idx];
        if (!VirtioDriverOk(&c.dev)) return;
        int n = 0;
        while (c.in_r != c.in_w && VirtioHasAvail(&c.dev, 0)) {
            VirtioChain ch;
            if (!VirtioPop(&c.dev, 0, &ch)) break;
            uint32_t written = 0;
            for (int i = 0; i < ch.n && c.in_r != c.in_w; i++) {
                if (!ch.write[i]) continue;
                uint32_t len = ch.len[i], off = 0;
                while (off < len && c.in_r != c.in_w) {
                    uint64_t gpa = ch.addr[i] + off;
                    uint8_t* p = GuestMemoryManager::GuestPhysToHost(gpa);
                    uint32_t run = GuestMemoryManager::ContigBytes(gpa);
                    if (!p || run == 0) { off = len; break; }
                    if (run > len - off) run = len - off;
                    uint32_t k = 0;
                    while (k < run && c.in_r != c.in_w) {
                        p[k++] = c.in[c.in_r % IN_RING];
                        __atomic_store_n(&c.in_r, c.in_r + 1, __ATOMIC_RELEASE);
                    }
                    off += k; written += k;
                    if (k < run) break;
                }
            }
            c.bytes_in += written;
            VirtioPush(&c.dev, 0, ch.head, written);
            n++;
        }
        if (n) VirtioRaise(&c.dev);
    }

    void on_notify(VirtioDev* d, int qid) {
        int idx = index_of(d);
        if (idx < 0) return;
        if (qid == 1) drain_tx(idx);
        else if (qid == 0) fill_rx(idx);
    }

    void on_reset(VirtioDev* d) {
        int idx = index_of(d);
        if (idx >= 0) g_con[idx].size_dirty = true;
    }

    void put_size(Con& c) {
        c.dev.devcfg[0] = (uint8_t)c.cols; c.dev.devcfg[1] = (uint8_t)(c.cols >> 8);
        c.dev.devcfg[2] = (uint8_t)c.rows; c.dev.devcfg[3] = (uint8_t)(c.rows >> 8);
    }
}

bool GuestConsole::Init(int idx, uint8_t irq, const char* name) {
    if (idx < 0 || idx >= MAX_CONSOLES) return false;
    Con& c = g_con[idx];
    if (!c.in) {
        c.in = (uint8_t*)KernelHeap::Alloc(IN_RING);
        if (!c.in) return false;
        for (uint32_t i = 0; i < IN_RING; i++) c.in[i] = 0;
    }
    void (*sink)(int, const uint8_t*, int) = c.sink;
    bool (*flow)(int) = c.flow;
    uint16_t cols = c.cols ? c.cols : 80, rows = c.rows ? c.rows : 25;
    c.dev = {};
    c.dev.name = name;
    c.dev.devtype = 3;                       // virtio console
    c.dev.pci_class = 0x07; c.dev.pci_subclass = 0x80;
    c.dev.irq = irq;
    c.dev.nqueues = 2;
    c.dev.queue_max = 64;
    c.dev.features = (1ULL << 0);            // VIRTIO_CONSOLE_F_SIZE
    c.cols = cols; c.rows = rows;
    put_size(c);
    c.dev.devcfg[4] = 1;                     // max_nr_ports (unused without multiport)
    c.dev.devcfg_len = 12;
    c.dev.notify = on_notify;
    c.dev.reset = on_reset;
    c.sink = sink;
    c.flow = flow;
    c.bytes_out = c.bytes_in = 0;
    c.size_dirty = false;
    c.up = VirtioRegister(&c.dev);
    return c.up;
}

void GuestConsole::Poll(int idx) {
    if (idx < 0 || idx >= MAX_CONSOLES) return;
    Con& c = g_con[idx];
    if (!c.up || !VirtioDriverOk(&c.dev)) return;
    drain_tx(idx);
    fill_rx(idx);
    if (c.size_dirty) {
        c.size_dirty = false;
        put_size(c);
        VirtioConfigChanged(&c.dev);
    }
}

int GuestConsole::Write(int idx, const void* data, int n) {
    if (idx < 0 || idx >= MAX_CONSOLES || n <= 0) return 0;
    Con& c = g_con[idx];
    if (!c.in) return 0;
    const uint8_t* s = (const uint8_t*)data;
    SpinLockGuard g(c.in_lock);
    int k = 0;
    while (k < n) {
        uint32_t r = __atomic_load_n(&c.in_r, __ATOMIC_ACQUIRE);
        if (c.in_w - r >= IN_RING) break;
        c.in[c.in_w % IN_RING] = s[k++];
        __atomic_store_n(&c.in_w, c.in_w + 1, __ATOMIC_RELEASE);
    }
    return k;
}

int GuestConsole::InputSpace(int idx) {
    if (idx < 0 || idx >= MAX_CONSOLES) return 0;
    Con& c = g_con[idx];
    return c.in ? (int)(IN_RING - (c.in_w - c.in_r)) : 0;
}

bool GuestConsole::DriverReady(int idx) {
    if (idx < 0 || idx >= MAX_CONSOLES) return false;
    return g_con[idx].up && VirtioDriverOk(&g_con[idx].dev);
}

void GuestConsole::SetSize(int idx, int cols, int rows) {
    if (idx < 0 || idx >= MAX_CONSOLES || cols < 2 || rows < 2) return;
    Con& c = g_con[idx];
    if (c.cols == cols && c.rows == rows) return;
    c.cols = (uint16_t)cols; c.rows = (uint16_t)rows;
    c.size_dirty = true;     // applied by Poll on the vm thread
}

void GuestConsole::ResetRings(int idx) {
    if (idx < 0 || idx >= MAX_CONSOLES) return;
    Con& c = g_con[idx];
    SpinLockGuard g(c.in_lock);
    c.in_r = c.in_w = 0;
}

// the vm is gone: hand the four 256 kb input rings back. Init allocates them
// again on the next start. (satoru)
void GuestConsole::ReleaseAll() {
    for (int i = 0; i < MAX_CONSOLES; i++) {
        Con& c = g_con[i];
        if (c.in) { KernelHeap::Free(c.in); c.in = nullptr; }
        c.in_w = c.in_r = 0;
        c.dev = {};
        c.up = false;
    }
}

uint64_t GuestConsole::BytesOut(int idx) { return (idx >= 0 && idx < MAX_CONSOLES) ? g_con[idx].bytes_out : 0; }
uint64_t GuestConsole::BytesIn(int idx)  { return (idx >= 0 && idx < MAX_CONSOLES) ? g_con[idx].bytes_in : 0; }

void GuestConsole::SetSink(int idx, void (*sink)(int idx, const uint8_t* data, int n)) {
    if (idx >= 0 && idx < GuestConsole::MAX_CONSOLES) g_con[idx].sink = sink;
}

void GuestConsole::SetFlow(int idx, bool (*ready)(int idx)) {
    if (idx >= 0 && idx < GuestConsole::MAX_CONSOLES) g_con[idx].flow = ready;
}

// end (satoru)
