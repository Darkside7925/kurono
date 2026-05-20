#ifndef KURONO_NET_TUNTAP_H
#define KURONO_NET_TUNTAP_H

#include "network.h"

// TUN/TAP virtual network devices.
//
// TUN  (NIC_TUN):  userspace I/O is L3 IP packets.  Used by VPN clients
//                  (WireGuard, OpenVPN) and IPv6 transition tunnels.
// TAP  (NIC_TAP):  userspace I/O is full Ethernet frames.  Used by qemu
//                  bridged networking, container veth pairs.
//
// Each open of /dev/net/tun returns a fresh fd; the fd's inode keeps a
// pair of bounded ring buffers:
//   - rx_ring:  packets the kernel pushed in (read by userspace)
//   - tx_ring:  packets userspace wrote (forwarded into kernel netstack)
//
// All buffers are statically allocated  -  no kernel heap dependency.

namespace TunTap {

    static const int  TUNTAP_MAX_DEVS    = 8;
    static const int  TUNTAP_RING_SLOTS  = 32;
    static const int  TUNTAP_MTU         = 1500;
    static const int  TUNTAP_FRAME_MAX   = 1518;     // ethernet w/o FCS

    struct Packet {
        unsigned short len;
        unsigned char  data[TUNTAP_FRAME_MAX];
    };

    struct Device {
        bool          in_use;
        bool          is_tap;       // true = NIC_TAP, false = NIC_TUN
        bool          persist;      // IFF_PERSIST: survives last close
        bool          up;
        int           refcount;     // # of fds open on this device
        char          name[16];     // "tun0", "tap0", ...
        unsigned int  flags;        // raw IFF_* bits the user requested
        IPv4Address   ip;
        IPv4Address   netmask;
        MACAddress    mac;          // for TAP

        // RX: kernel -> userspace.  Userspace read() drains.
        Packet        rx_ring[TUNTAP_RING_SLOTS];
        unsigned int  rx_head;
        unsigned int  rx_tail;
        unsigned int  rx_drops;

        // TX: userspace -> kernel.  Network::Tick() drains and forwards.
        Packet        tx_ring[TUNTAP_RING_SLOTS];
        unsigned int  tx_head;
        unsigned int  tx_tail;
        unsigned int  tx_drops;

        // accounting (mirrored into NetworkInterface stats on Tick)
        unsigned int  rx_packets, rx_bytes;
        unsigned int  tx_packets, tx_bytes;
    };

    // ---- one-time init ----
    void Init();

    // ---- device lifecycle (called from /dev/net/tun open/close) ----
    // Allocate (or reuse persistent) device matching the requested name
    // and flags.  Returns dev id (>=0) or -1 on exhaustion.
    int  Open(const char* req_name, bool tap, bool persist, unsigned int flags);
    void Close(int dev_id);

    Device* Get(int dev_id);
    int     Count();

    // ---- I/O ----
    // userspace read: pop one packet from rx_ring into buf.  Returns
    // bytes copied, 0 if ring empty (caller should re-poll), -1 on error.
    int  Read(int dev_id, unsigned char* buf, int max);

    // userspace write: push one packet into tx_ring for the kernel to
    // forward.  Returns bytes accepted, -1 on error.
    int  Write(int dev_id, const unsigned char* buf, int len);

    // kernel-side: enqueue a packet for userspace to consume.  Returns
    // true on success, false if ring full.
    bool Inject(int dev_id, const unsigned char* buf, int len);

    // periodic forwarder  -  drains tx_rings into Network::SendPacket so
    // that VPN-style userspace daemons can route through the real NIC.
    void Tick();

    // up/down + ip configuration (called from ifconfig/ip syscall paths)
    bool SetUp(int dev_id, bool up);
    bool SetIP(int dev_id, IPv4Address ip, IPv4Address mask);
    bool SetMAC(int dev_id, const MACAddress& mac);
}

#endif
