# Network Stack

`src/net/network.cpp` / `network.h` and `src/net/tcpip.cpp` / `tcpip.h` implement the Kurono network stack.

## 1. Architecture

The network stack is a two-file design.

**`network.cpp`**  -  handles Ethernet frame reception from the E1000 driver, ARP resolution, and the interface management layer. It is the boundary between hardware and protocol.

**`tcpip.cpp`**  -  implements IPv4, ICMP, UDP, and TCP on top of the frames network.cpp provides.

## 2. Supported protocols

| Protocol | Status |
| --- | --- |
| ARP | Send/receive, table maintained |
| IPv4 | Send/receive, basic fragmentation |
| ICMP | Echo request/reply (ping) |
| UDP | Send/receive |
| TCP | Connect, send, receive, close (basic state machine) |
| DNS | UDP-based stub resolver |
| DHCP | DISCOVER/OFFER/REQUEST/ACK; gets IP at boot |

## 3. DHCP at boot

On startup, if an E1000 NIC is detected, the network stack sends a DHCP DISCOVER. The assigned IP, gateway, and DNS server are stored and used for all subsequent connections.

## 4. Socket API

The TCP layer provides a minimal socket-like interface:

```cpp
int TCPSocket::Connect(ip, port)
int TCPSocket::Send(buf, len)
int TCPSocket::Recv(buf, maxlen)
void TCPSocket::Close()
```

The browser app uses this interface to fetch HTTP pages.

## 5. Common problems

| Problem | Likely cause |
| --- | --- |
| No network at boot | E1000 not detected on PCI bus |
| DHCP fails | No DHCP server on the network segment |
| `ping` no response | ARP not resolving gateway MAC |
| HTTP fetch hangs | TCP state stuck; server RST not handled |

## 6. Related files

- `src/drivers/e1000.cpp`  -  Ethernet frame source
- `src/apps/browser.cpp`  -  TCP client consumer
- `src/shell/linux_cmds.cpp`  -  `ping`, `ifconfig`, `curl` commands
