# Network stack

`src/net/network.cpp` / `network.h` and `src/net/tcpip.cpp` / `tcpip.h`. A real from-scratch
TCP/IP stack on top of the E1000, not a host-guest shortcut.

## Two files at the core

**`network.cpp`** does Ethernet II framing and reception from the E1000 driver, ARP
resolution, and the interface-management layer. It's the boundary between hardware and
protocol.

**`tcpip.cpp`** does IPv4, ICMP, UDP and TCP on top of those frames. Around them:
`ipv6.cpp` for IPv6, `netfilter.cpp` for the packet-filter pipeline, `tuntap.cpp` for Tun/Tap,
and `unix_socket.cpp` for AF_UNIX local sockets.

## Protocols

| Protocol | State |
| --- | --- |
| Ethernet II | frame encapsulation and parsing |
| ARP | request/reply, 32-entry cache |
| IPv4 | header build/parse, inbound checksum validation with explicit drop logs |
| IPv6 | stack present, echo and neighbour-discovery plumbing |
| ICMP | echo request/reply with RTT measurement |
| UDP | send/receive, `SendTo` and `RecvFrom` |
| TCP | full 11-state machine (`CLOSED` to `TIME_WAIT`), 3-way handshake, FIN teardown |
| DNS | UDP-based resolver |
| DHCP | DISCOVER/OFFER/REQUEST/ACK, gets an IP at boot |

On checksums: computed from network-order bytes, IPv4 plus the TCP pseudo-header, covering
the real TCP header length including options. Inbound IPv4 and TCP checksums are validated
with explicit serial drop logs. That was the fix for the old "ARP works but TCP stalls"
behaviour.

## DHCP

At startup, if there's an E1000, the stack sends a DISCOVER. The assigned IP, gateway and DNS
server get stored and used from then on. Routing is configurable through `SetIP`,
`SetSubnetMask`, `SetGateway` and `SetDNS`.

## Sockets

BSD-style: `Socket()`, `Bind()`, `Connect()`, `Listen()`, `Accept()`, `Send()`, `Recv()`,
`Close()`. Types are `SOCK_STREAM` (TCP), `SOCK_DGRAM` (UDP) and `SOCK_RAW`.

Limits: 16 sockets, 8KB RX/TX buffers, 1460-byte MSS. A `NetStats` struct tracks
per-protocol RX/TX/error/drop counters.

## HTTP

`curl <url>` is the working path end to end, verified fetching real pages off example.com and
Wikipedia over tap+NAT. What got it there: a recv loop that stopped giving up early, the
`FIN_WAIT` half-close path, and ephemeral port selection. `curl` is HTTP only, no TLS. The GUI
Browser tile is a placeholder, see the README's browser note.

## netfilter

`netfilter.cpp` is a 5-hook pipeline (`PRE_ROUTING`, `LOCAL_IN`, `FORWARD`, `LOCAL_OUT`,
`POST_ROUTING`) with a table/chain/rule model and `ACCEPT`, `DROP` and `REJECT` verdicts,
matching on IPv4 src/dst, port ranges, protocol and interface. It's wired into the
`tcpip.cpp` send and receive paths.

## Logging

The stack calls `RuntimeLog::LogNetwork` (`/kurono/var/log/network.log`) on connection
establishment and RST. See [../system/LOGGING.md](../system/LOGGING.md).

## When it breaks

| Symptom | Cause |
| --- | --- |
| no network at boot | no E1000 on the PCI bus |
| DHCP fails | no DHCP server on that segment |
| `ping` gets nothing back | ARP isn't resolving the gateway MAC |
| TCP stalls under double-NAT | run single-NAT (`-netdev user` under KVM). WSL's double-NAT drops guest TCP |

## Files

- `src/drivers/e1000.cpp` Ethernet frame source
- `src/net/tcpip.cpp`, `network.cpp` IPv4/ICMP/UDP/TCP plus Ethernet/ARP
- `src/net/ipv6.cpp` IPv6
- `src/net/netfilter.cpp` the filter pipeline
- `src/net/unix_socket.cpp` AF_UNIX plus `SCM_RIGHTS` fd passing
- `src/shell/linux_cmds.cpp` `ping`, `ifconfig`, `ip`, `ss`, `curl`, `wget`
