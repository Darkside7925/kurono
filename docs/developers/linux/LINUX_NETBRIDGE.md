# Linux network bridge

`src/linux/linux_netbridge.cpp` and `linux_netbridge.h`. Bridges traffic between the Kurono
network stack and the Linux guest's virtual NIC.

From inside the guest it looks like a real Ethernet card on the same network as the host.

```
guest virtio-net -> vdevices.cpp -> LinuxNetBridge -> Kurono TCP/IP -> E1000
```

Packets the guest sends get intercepted by the virtual NIC device handler, handed to
`LinuxNetBridge::SendToHost()`, and injected into the Kurono stack as if they came from
outside. Packets the E1000 receives get filtered and forwarded to the guest through
`LinuxNetBridge::SendToGuest()`.

## Addresses

The guest gets its IP from the bridge's internal DHCP server, and Kurono acts as the default
gateway. Basic NAT is in there, so the guest reaches the outside network through the host's
IP.

## Files

- `src/virt/vdevices.cpp` the virtual NIC that calls into the bridge
- `src/net/network.cpp` the host network stack
- `src/net/tcpip.cpp` host TCP/IP used for masquerading
