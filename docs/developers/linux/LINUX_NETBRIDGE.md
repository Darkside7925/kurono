# Linux Network Bridge

`src/linux/linux_netbridge.cpp` and `linux_netbridge.h` bridge network traffic between the Kurono host network stack and the Linux guest VM.

## 1. What it does

The network bridge creates a virtual Ethernet link between the Kurono TCP/IP stack and the Linux guest's virtual NIC. From the guest's perspective, it appears to have a real Ethernet card connected to the same network as the host.

## 2. Architecture

```
Guest virtio-net → vdevices.cpp → LinuxNetBridge → Kurono TCP/IP → E1000 NIC
```

Packets sent from the guest are intercepted by the virtual NIC device handler, passed to `LinuxNetBridge::SendToHost()`, and injected into the Kurono network stack as if they came from an external source. Packets received by the Kurono E1000 driver are filtered and forwarded to the guest via `LinuxNetBridge::SendToGuest()`.

## 3. Address assignment

The guest receives an IP address from the internal bridge DHCP server. The host (Kurono) acts as the default gateway. The bridge implements basic NAT so the guest can reach the external network through the host's IP.

## 4. Related files

- `src/virt/vdevices.cpp` - virtual NIC device that calls into the bridge
- `src/net/network.cpp` - host network stack
- `src/net/tcpip.cpp` - host TCP/IP used for masquerade
