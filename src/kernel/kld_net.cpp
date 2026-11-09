//  KLD network backend: makes a Linux net_device running in a KLD driver
//  container the backend of Kurono's eth0.
//
//  The container owns the hardware (an unmodified upstream driver, e1000e
//  today). Kurono's TCP/IP stack keeps talking to the E1000:: facade exactly as
//  it does for the native e1000 and igc backends; when KldNet is the backend,
//  that facade forwards here:
//
//    TX  E1000::Send -> copy into a TX slot of the shared ring -> bump the
//        doorbell -> futex-wake the container's xmit thread (by physical key).
//        The container builds an sk_buff and calls the driver's
//        ndo_start_xmit.
//    RX  the driver's napi_gro_receive/netif_receive_skb copy the frame into an
//        RX slot; the container then issues NET_RX_KICK and this file drains the
//        ring into TCPStack under g_net_lock, in that syscall. The periodic
//        E1000::Poll() paths drain it too, so nothing depends on the kick.
//
//  The ring frames are broker-owned and read through the kernel identity map,
//  never through a user pointer. Lengths written by the container are
//  validated before use, and every frame is copied into a kernel buffer before
//  the stack parses it, so a container rewriting a slot mid-parse cannot change
//  what the stack already validated. (satoru)
//
//  end (satoru)

#include "kld_broker.h"
#include "../drivers/serial.h"
#include "../proc/spinlock.h"
#include "../proc/kernel_locks.h"
#include "../net/tcpip.h"
#include "../linux/linux_netbridge.h"
#include "../net/network.h"
#include "../net/dhcp.h"

extern int LinuxSyscallFutexWakePhys(uint64_t phys_key, int max);

namespace KldNet {

namespace {

bool      g_wanted = false;
volatile bool g_attached = false;
volatile bool g_carrier = false;
uint64_t  g_as = 0;
uint64_t  g_ring_phys = 0;
RingHeader* g_hdr = nullptr;
uint8_t*  g_tx_base = nullptr;
uint8_t*  g_rx_base = nullptr;
uint8_t   g_mac[6] = {0};
void (*g_handler)(const uint8_t*, uint16_t) = nullptr;
Spinlock  g_tx_lock;
uint32_t  g_tx_count = 0, g_rx_count = 0, g_tx_bytes = 0, g_rx_bytes = 0;
uint32_t  g_rx_bad = 0;
uint8_t   g_rx_copy[KLD_NET_SLOT_BYTES];

}  // namespace

void SetWanted(bool on) { g_wanted = on; }
bool Wanted() { return g_wanted; }
bool Attached() { return g_attached; }
bool LinkUp() {
    bool up = g_attached && g_carrier;
    // one-shot: the first time anyone observes the kld link as up. (satoru)
    static bool s_seen = false;
    if (up && !s_seen) { s_seen = true; SerialLogger::Log("[kldnet] link-up observed by the stack\r\n"); }
    return up;
}
void GetMAC(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = g_mac[i]; }
void SetPacketHandler(void (*h)(const uint8_t*, uint16_t)) { g_handler = h; }
uint32_t TxCount() { return g_tx_count; }
uint32_t RxCount() { return g_rx_count; }
uint32_t TxBytes() { return g_tx_bytes; }
uint32_t RxBytes() { return g_rx_bytes; }
uint32_t RxDrops() { return g_hdr ? g_hdr->rx_drops : 0; }

int64_t Attach(uint64_t address_space, uint64_t ring_phys, const uint8_t mac[6]) {
    if (g_attached) return KldBroker::KLD_EBUSY;
    RingHeader* h = (RingHeader*)(uintptr_t)ring_phys;
    // the broker zeroed these frames; the kernel initializes the header so the
    // container cannot pick the geometry. (satoru)
    h->magic = KLD_NET_MAGIC;
    h->tx_slots = KLD_NET_TX_SLOTS;
    h->rx_slots = KLD_NET_RX_SLOTS;
    h->slot_bytes = KLD_NET_SLOT_BYTES;
    h->tx_prod = h->tx_cons = h->rx_prod = h->rx_cons = 0;
    h->tx_doorbell = 0;
    g_hdr = h;
    g_tx_base = (uint8_t*)(uintptr_t)(ring_phys + 4096);
    g_rx_base = g_tx_base + KLD_NET_TX_SLOTS * KLD_NET_SLOT_BYTES;
    g_ring_phys = ring_phys;
    g_as = address_space;
    for (int i = 0; i < 6; i++) g_mac[i] = mac[i];
    g_carrier = false;
    __atomic_store_n(&g_attached, true, __ATOMIC_RELEASE);
    // TCPStack captured a mac at boot (before the container existed). (satoru)
    TCPStack::SetMAC(g_mac);
    SerialLogger::Log("[kldnet] eth0 backend = KLD container, mac ");
    const char* hx = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char b[4] = { hx[g_mac[i] >> 4], hx[g_mac[i] & 15], (char)(i < 5 ? ':' : 0), 0 };
        SerialLogger::Log(b);
    }
    SerialLogger::Log("\r\n");
    return KldBroker::KLD_OK;
}

void Detach(uint64_t address_space) {
    if (!g_attached || g_as != address_space) return;
    SpinLockGuard g(g_tx_lock);
    __atomic_store_n(&g_attached, false, __ATOMIC_RELEASE);
    g_carrier = false;
    g_hdr = nullptr;
    SerialLogger::Log("[kldnet] eth0 backend detached (container gone)\r\n");
}

int64_t SetCarrier(uint64_t address_space, bool on) {
    if (!g_attached || g_as != address_space) return KldBroker::KLD_EPERM;
    if (g_carrier != on) {
        g_carrier = on;
        SerialLogger::Log(on ? "[kldnet] carrier ON (from the linux driver)\r\n"
                             : "[kldnet] carrier OFF (from the linux driver)\r\n");
    }
    return KldBroker::KLD_OK;
}

bool Send(const uint8_t* data, uint16_t len) {
    if (!g_attached || !data || len == 0 || len > KLD_NET_SLOT_BYTES) return false;
    uint64_t wake = 0;
    {
        SpinLockGuard g(g_tx_lock);
        RingHeader* h = g_hdr;
        if (!h) return false;
        uint32_t prod = h->tx_prod;
        uint32_t cons = __atomic_load_n(&h->tx_cons, __ATOMIC_ACQUIRE);
        if (prod - cons >= KLD_NET_TX_SLOTS) {
            h->tx_drops++;
            return false;
        }
        uint32_t slot = prod % KLD_NET_TX_SLOTS;
        uint8_t* dst = g_tx_base + slot * KLD_NET_SLOT_BYTES;
        for (uint16_t i = 0; i < len; i++) dst[i] = data[i];
        h->tx_len[slot] = len;
        __atomic_store_n(&h->tx_prod, prod + 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&h->tx_doorbell, 1u, __ATOMIC_RELEASE);
        wake = g_ring_phys + __builtin_offsetof(RingHeader, tx_doorbell);
        g_tx_count++;
        g_tx_bytes += len;
    }
    LinuxSyscallFutexWakePhys(wake, 1);
    return true;
}

// drain the rx ring into the handler. callers hold g_net_lock (the same rule as
// the native E1000::Poll). (satoru)
void Poll() {
    if (!__atomic_load_n(&g_attached, __ATOMIC_ACQUIRE)) return;
    RingHeader* h = g_hdr;
    if (!h) return;
    uint32_t budget = KLD_NET_RX_SLOTS;
    while (budget--) {
        uint32_t cons = h->rx_cons;
        uint32_t prod = __atomic_load_n(&h->rx_prod, __ATOMIC_ACQUIRE);
        if (cons == prod) break;
        if (prod - cons > KLD_NET_RX_SLOTS) {
            // a container that lies about its producer index gets resynced, not
            // trusted. (satoru)
            g_rx_bad++;
            __atomic_store_n(&h->rx_cons, prod, __ATOMIC_RELEASE);
            break;
        }
        uint32_t slot = cons % KLD_NET_RX_SLOTS;
        uint32_t len = h->rx_len[slot];
        if (len >= 14 && len <= KLD_NET_SLOT_BYTES) {
            const uint8_t* src = g_rx_base + slot * KLD_NET_SLOT_BYTES;
            for (uint32_t i = 0; i < len; i++) g_rx_copy[i] = src[i];
            g_rx_count++;
            g_rx_bytes += len;
            if (g_handler) g_handler(g_rx_copy, (uint16_t)len);
        } else {
            g_rx_bad++;
        }
        __atomic_store_n(&h->rx_cons, cons + 1, __ATOMIC_RELEASE);
    }
}

int64_t RxKick(uint64_t address_space) {
    if (!g_attached || g_as != address_space) return KldBroker::KLD_EPERM;
    {
        SpinLockCpuGuard g(g_net_lock);
        if (TCPStack::IsUp()) TCPStack::Tick();   // drains via E1000::Poll + timers (satoru)
        else Poll();
    }
    // hand the drained rx to any parked poller now, outside the net lock (the
    // same order the network process uses). (satoru)
    LinuxNetBridge::WakeRxPollers();
    return KldBroker::KLD_OK;
}

// one iteration of the network kernel process's loop body, driven from a
// preemptive ring-3 thread of the container. WHY: the runner enters the
// container from a cooperative kernel process, and while the container's
// leader occupies the bsp the cooperative kernel processes - including the
// network one that calls RefreshLink/Dhcp::Tick/TCPStack::Tick - stop getting
// turns (measured: link up at 73.7 s, the stack never observed it). that is the
// known cooperative-scheduler starvation class and belongs to the scheduler
// work; this keeps eth0 alive meanwhile at the same 10 ms cadence. dhcp and the
// link refresh run under g_net_lock here, so two tickers cannot interleave the
// dhcp state machine from this side. (satoru)
int64_t Tick(uint64_t address_space) {
    if (!g_attached || g_as != address_space) return KldBroker::KLD_EPERM;
    {
        SpinLockCpuGuard g(g_net_lock);
        if (TCPStack::IsUp()) TCPStack::Tick();
        else Poll();
    }
    LinuxNetBridge::WakeRxPollers();
    {
        SpinLockCpuGuard g(g_net_lock);
        Network::RefreshLink();
        Dhcp::Tick();
    }
    WiFi::Tick();
    LinuxNetBridge::RefreshFromKernel();
    return KldBroker::KLD_OK;
}

}  // namespace KldNet

// end (satoru)
