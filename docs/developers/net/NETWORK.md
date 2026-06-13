# Network Stack

`src/net/network.cpp` / `network.h` and `src/net/tcpip.cpp` / `tcpip.h` implement
Kurono's custom TCP/IP stack  -  a real from-scratch stack on top of the E1000 NIC,
not a host-guest shortcut.

## 1. Architecture

The network stack is a two-file core.

**`network.cpp`**  -  Ethernet II framing and reception from the E1000 driver, ARP
resolution, and the interface-management layer. It is the boundary between
hardware and protocol.

**`tcpip.cpp`**  -  IPv4, ICMP, UDP, and TCP on top of the frames `network.cpp`
provides. Surrounding files add IPv6 (`ipv6.cpp`), the packet-filter pipeline
(`netfilter.cpp`), the Tun/Tap subsystem (`tuntap.cpp`), and AF_UNIX local sockets
(`unix_socket.cpp`).

## 2. Supported protocols

| Protocol | Status |
| --- | --- |
| Ethernet II | Frame encapsulation + parsing |
| ARP | Request/reply, 32-entry cache |
| IPv4 | Header build/parse, **inbound checksum validation** with drop logs |
| IPv6 | Stack present, echo + neighbor-discovery plumbing |
| ICMP | Echo request/reply (ping) with RTT measurement |
| UDP | Send/receive, `SendTo` / `RecvFrom` |
| TCP | Full **11-state machine** (`CLOSED` → `TIME_WAIT`), 3-way handshake, FIN teardown |
| DNS | UDP-based resolver |
| DHCP | DISCOVER/OFFER/REQUEST/ACK; obtains IP at boot |

**Correctness note:** Internet checksums are computed from network-order bytes
(IPv4 + TCP pseudo-header, covering the real TCP header length including options),
and inbound IPv4/TCP checksums are validated with explicit serial drop logs  -  the
fix for the earlier "ARP works but TCP stalls" behavior.

## 3. DHCP at boot

On startup, if an E1000 NIC is detected, the stack sends a DHCP DISCOVER. The
assigned IP, gateway, and DNS server are stored and used for subsequent
connections. Routing is configurable via `SetIP` / `SetSubnetMask` / `SetGateway`
/ `SetDNS`.

## 4. Socket API and limits

A BSD-style socket interface is provided:

```cpp
Socket(), Bind(), Connect(), Listen(), Accept(), Send(), Recv(), Close()
```

Socket types: `SOCK_STREAM` (TCP), `SOCK_DGRAM` (UDP), `SOCK_RAW`. Limits: up to
16 sockets, 8 KB RX/TX buffers, 1460-byte MSS. A `NetStats` struct tracks
per-protocol RX/TX/error/drop counters.

## 5. HTTP today

`curl <url>` is the working HTTP path end to end  -  verified by fetching real pages
off `example.com` and Wikipedia over tap+NAT. The fixes that got it working were a
recv loop that no longer gives up early, the `FIN_WAIT` half-close path, and
ephemeral-port selection. (The OS's `curl` is HTTP-only  -  no TLS.) The GUI
"Browser" tile is a placeholder; see the README's browser note.

## 6. Packet filtering (netfilter)

`netfilter.cpp` provides a 5-hook pipeline (`PRE_ROUTING`, `LOCAL_IN`, `FORWARD`,
`LOCAL_OUT`, `POST_ROUTING`) with a table+chain+rule model and `ACCEPT` / `DROP` /
`REJECT` verdicts, matching on IPv4 src/dst, port ranges, protocol, and interface.
It is wired into the `tcpip.cpp` send/receive paths.

## 7. Logging

The TCP stack calls `RuntimeLog::LogNetwork` (`/kurono/var/log/network.log`) on
connection establishment and RST. See [../system/LOGGING.md](../system/LOGGING.md).

## 8. Common problems

| Problem | Likely cause |
| --- | --- |
| No network at boot | E1000 not detected on PCI bus |
| DHCP fails | No DHCP server on the network segment |
| `ping` no response | ARP not resolving gateway MAC |
| TCP stalls under double-NAT | Run with single-NAT (`-netdev user` under KVM); WSL's double-NAT drops guest TCP |

## 9. Related files

- `src/drivers/e1000.cpp`  -  Ethernet frame source
- `src/net/tcpip.cpp` / `network.cpp`  -  IPv4/ICMP/UDP/TCP + Ethernet/ARP
- `src/net/ipv6.cpp`  -  IPv6 stack
- `src/net/netfilter.cpp`  -  packet-filter pipeline
- `src/net/unix_socket.cpp`  -  AF_UNIX sockets + `SCM_RIGHTS` fd-passing
- `src/shell/linux_cmds.cpp`  -  `ping`, `ifconfig`, `ip`, `ss`, `curl`, `wget`
