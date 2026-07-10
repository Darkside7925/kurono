//  intel e1000 (82540em) ethernet driver for kurono os
//  real pci network card driver - works with qemu -device e1000

#include "e1000.h"
#include "serial.h"
#include "../kernel/heap.h"

// static member definitions
bool E1000::detected = false;
uint32_t E1000::mmio_base = 0;
uint8_t E1000::mac[6] = {0};

E1000_RXDesc E1000::rx_descs[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
E1000_TXDesc E1000::tx_descs[E1000_NUM_TX_DESC] __attribute__((aligned(16)));
uint8_t E1000::rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUFFER_SIZE] __attribute__((aligned(16)));
uint8_t E1000::tx_buffers[E1000_NUM_TX_DESC][E1000_TX_BUFFER_SIZE] __attribute__((aligned(16)));

uint16_t E1000::rx_cur = 0;
uint16_t E1000::tx_cur = 0;

E1000_PacketHandler E1000::packet_handler = nullptr;
uint32_t E1000::tx_count = 0;
uint32_t E1000::rx_count = 0;
uint32_t E1000::tx_bytes = 0;
uint32_t E1000::rx_bytes = 0;
uint32_t E1000::rx_missed = 0;

static const uint16_t E1000_MAX_TX_FRAME = 1518;

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t E1000::PciRead(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void E1000::PciWrite(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, value);
}

bool E1000::ScanPCI() {
    // enumerate pci bus to find e1000 device
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = PciRead((uint8_t)bus, slot, 0, 0);
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;

            if (vendor == E1000_VENDOR_ID &&
                (device == E1000_DEVICE_ID || device == E1000_DEVICE_ID2 || device == E1000_DEVICE_ID3)) {

                SerialLogger::Log("[E1000] Found at PCI ");
                SerialLogger::LogDec(bus);
                SerialLogger::Log(":");
                SerialLogger::LogDec(slot);
                SerialLogger::Log(" device=0x");
                SerialLogger::LogHex(device);
                SerialLogger::Log("\r\n");

                // read bar0 (mmio base address)
                uint32_t bar0 = PciRead((uint8_t)bus, slot, 0, 0x10);
                if (bar0 & 1) {
                    // i/o space - not expected for e1000
                    SerialLogger::Log("[E1000] BAR0 is I/O space, skipping\r\n");
                    continue;
                }
                mmio_base = bar0 & 0xFFFFFFF0;

                SerialLogger::Log("[E1000] MMIO base: 0x");
                SerialLogger::LogHex(mmio_base);
                SerialLogger::Log("\r\n");

                // enable bus mastering + memory space
                uint32_t cmd = PciRead((uint8_t)bus, slot, 0, 0x04);
                cmd |= (1 << 1) | (1 << 2);  // memory space + bus master
                PciWrite((uint8_t)bus, slot, 0, 0x04, cmd);

                detected = true;
                return true;
            }
        }
    }
    return false;
}

void E1000::WriteReg(uint16_t offset, uint32_t value) {
    *((volatile uint32_t*)(uintptr_t)(mmio_base + offset)) = value;
}

uint32_t E1000::ReadReg(uint16_t offset) {
    return *((volatile uint32_t*)(uintptr_t)(mmio_base + offset));
}

void E1000::ReadMAC() {
    // method 1: try reading from eeprom
    bool eeprom_exists = false;

    // check if eeprom exists by writing to eerd and checking if done bit sets
    WriteReg(E1000_EERD, 0x01);  // read word 0
    for (int i = 0; i < 1000; i++) {
        uint32_t val = ReadReg(E1000_EERD);
        if (val & (1 << 4)) {  // done bit
            eeprom_exists = true;
            break;
        }
    }

    if (eeprom_exists) {
        // read mac from eeprom (3 words, starting at offset 0)
        for (int i = 0; i < 3; i++) {
            WriteReg(E1000_EERD, ((uint32_t)i << 8) | 0x01);
            uint32_t val;
            int timeout = 10000;
            do {
                val = ReadReg(E1000_EERD);
            } while (!(val & (1 << 4)) && --timeout > 0);

            uint16_t data = (uint16_t)(val >> 16);
            mac[i * 2]     = (uint8_t)(data & 0xFF);
            mac[i * 2 + 1] = (uint8_t)((data >> 8) & 0xFF);
        }
    } else {
        // method 2: read from ral/rah registers
        uint32_t ral = ReadReg(E1000_RAL);
        uint32_t rah = ReadReg(E1000_RAH);
        mac[0] = (uint8_t)(ral & 0xFF);
        mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        mac[4] = (uint8_t)(rah & 0xFF);
        mac[5] = (uint8_t)((rah >> 8) & 0xFF);
    }

    SerialLogger::Log("[E1000] MAC: ");
    for (int i = 0; i < 6; i++) {
        SerialLogger::LogHex(mac[i]);
        if (i < 5) SerialLogger::Log(":");
    }
    SerialLogger::Log("\r\n");
}

void E1000::InitRX() {
    // set up rx descriptor ring
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_descs[i].addr = (uint64_t)(uintptr_t)&rx_buffers[i][0];
        rx_descs[i].status = 0;
    }

    // program rdbal/rdbah
    uint64_t rx_addr = (uint64_t)(uintptr_t)&rx_descs[0];
    WriteReg(E1000_RDBAL, (uint32_t)(rx_addr & 0xFFFFFFFF));
    WriteReg(E1000_RDBAH, (uint32_t)(rx_addr >> 32));

    // set descriptor ring length (bytes)
    WriteReg(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(E1000_RXDesc));

    // set head and tail
    WriteReg(E1000_RDH, 0);
    WriteReg(E1000_RDT, E1000_NUM_RX_DESC - 1);

    rx_cur = 0;

    // enable receiver - also enable UPE/MPE so SLIRP unicast replies
    // are not silently filtered if the RAR programming raced.  Verbose
    // for diagnostics; we can tighten later.
    uint32_t rctl = E1000_RCTL_EN |
                    E1000_RCTL_BAM |         // accept broadcast
                    E1000_RCTL_UPE |         // unicast promiscuous
                    E1000_RCTL_MPE |         // multicast promiscuous
                    E1000_RCTL_BSIZE_2048 |  // 2k buffers
                    E1000_RCTL_SECRC;        // strip crc
    WriteReg(E1000_RCTL, rctl);
}

void E1000::InitTX() {
    // set up tx descriptor ring
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_descs[i].addr = (uint64_t)(uintptr_t)&tx_buffers[i][0];
        tx_descs[i].length = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = E1000_TXD_STAT_DD;  // mark as done
    }

    // program tdbal/tdbah
    uint64_t tx_addr = (uint64_t)(uintptr_t)&tx_descs[0];
    WriteReg(E1000_TDBAL, (uint32_t)(tx_addr & 0xFFFFFFFF));
    WriteReg(E1000_TDBAH, (uint32_t)(tx_addr >> 32));

    // set descriptor ring length
    WriteReg(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(E1000_TXDesc));

    // set head and tail
    WriteReg(E1000_TDH, 0);
    WriteReg(E1000_TDT, 0);

    tx_cur = 0;

    // set inter-packet gap (recommended values)
    WriteReg(E1000_TIPG, 0x0060200A);

    // enable transmitter
    uint32_t tctl = E1000_TCTL_EN |
                    E1000_TCTL_PSP |           // pad short packets
                    (15 << E1000_TCTL_CT_SHIFT) |   // collision threshold
                    (64 << E1000_TCTL_COLD_SHIFT);  // collision distance
    WriteReg(E1000_TCTL, tctl);
}

void E1000::EnableInterrupts() {
    // Driver is poll-only (Scheduler::Tick() drives E1000::Poll()).  We
    // intentionally MASK every interrupt cause so the NIC never asserts
    // its INTx line.  Previously we wrote IMS=0x1F6DC which armed the
    // legacy IRQ; once a real IDT vector gets hooked for that line the
    // unhandled storm would wedge the box.  IMC=all-ones disables, then
    // IMS=0 keeps it that way; ICR read clears any latched cause.
    WriteReg(E1000_IMC, 0xFFFFFFFFu);
    WriteReg(E1000_IMS, 0x00000000u);
    ReadReg(E1000_ICR);
}

void E1000::LinkUp() {
    uint32_t ctrl = ReadReg(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE | E1000_CTRL_FD;
    ctrl &= ~E1000_CTRL_LRST;
    ctrl &= ~E1000_CTRL_PHY_RST;
    WriteReg(E1000_CTRL, ctrl);
}

bool E1000::Init() {
    detected = false;
    mmio_base = 0;
    rx_cur = 0;
    tx_cur = 0;
    tx_count = rx_count = tx_bytes = rx_bytes = 0;
    packet_handler = nullptr;

    SerialLogger::Log("[E1000] Scanning PCI bus...\r\n");
    if (!ScanPCI()) {
        SerialLogger::Log("[E1000] No E1000 NIC found\r\n");
        return false;
    }

    // software reset
    WriteReg(E1000_CTRL, E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) {}  // wait for reset
    // re-read status after reset
    ReadReg(E1000_STATUS);

    // clear multicast table array
    for (int i = 0; i < 128; i++) {
        WriteReg(E1000_MTA + i * 4, 0);
    }

    // read mac address
    ReadMAC();

    // program mac into receive address register
    uint32_t ral = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                    ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    uint32_t rah = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31); // av bit
    WriteReg(E1000_RAL, ral);
    WriteReg(E1000_RAH, rah);

    // bring link up
    LinkUp();

    // initialize rx and tx descriptor rings
    InitRX();
    InitTX();

    // enable interrupts (for status, we poll)
    EnableInterrupts();

    // check link status
    uint32_t status = ReadReg(E1000_STATUS);
    bool link = (status & (1 << 1)) != 0;
    SerialLogger::Log("[E1000] Link: ");
    SerialLogger::Log(link ? "UP" : "DOWN");
    SerialLogger::Log(", Speed: ");
    uint32_t speed = (status >> 6) & 3;
    if (speed == 0) SerialLogger::Log("10 Mbps");
    else if (speed == 1) SerialLogger::Log("100 Mbps");
    else SerialLogger::Log("1000 Mbps");
    SerialLogger::Log(", Full Duplex: ");
    SerialLogger::Log((status & 1) ? "Yes" : "No");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("[E1000] Driver initialized successfully\r\n");
    return true;
}

bool E1000::IsDetected() { return detected; }

bool E1000::IsLinkUp() {
    if (!detected) return false;
    return (ReadReg(E1000_STATUS) & (1 << 1)) != 0;
}

void E1000::GetMAC(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

// reclaim tx descriptors the nic has finished with. a slot is reclaimable
// once its descriptor reports dd (descriptor done); we just clear the status
// so the producer (Send) can tell drained slots from in-flight ones. lazy:
// called from Send (entry) and from Poll, so the 32-entry ring drains in the
// background instead of stalling each Send. (satoru)
void E1000::ReclaimTx() {
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        // only slots we actually armed (cmd!=0) and that the nic marked done.
        if (tx_descs[i].cmd != 0 && (tx_descs[i].status & E1000_TXD_STAT_DD)) {
            tx_descs[i].cmd = 0;    // mark reclaimed / free (satoru)
        }
    }
}

bool E1000::Send(const uint8_t* data, uint16_t length) {
    if (!detected || !data || length == 0 || length > E1000_MAX_TX_FRAME) {
        SerialLogger::Log("[E1000:TX] reject: bad params or no NIC\r\n");
        return false;
    }

    // lazily reap descriptors the nic already finished so the ring keeps
    // flowing without ever blocking on a per-packet completion. (satoru)
    ReclaimTx();

    // the slot we're about to (re)use is free iff its descriptor is dd - the
    // nic has transmitted whatever was last queued there. all slots start dd
    // (InitTX), and once armed they only go dd again when the nic finishes, so
    // a non-dd slot here means the 32-entry ring has wrapped fully around and
    // is actually full. that's the ONLY case we block - briefly, bounded - 
    // rather than spinning on every frame. (satoru)
    volatile E1000_TXDesc* txd = &tx_descs[tx_cur];
    if (!(txd->status & E1000_TXD_STAT_DD)) {
        int timeout = 100000;
        while (!(txd->status & E1000_TXD_STAT_DD) && --timeout > 0) {
            __asm__ __volatile__("pause");
        }
        if (!(txd->status & E1000_TXD_STAT_DD)) {
            SerialLogger::Log("[E1000:TX] ring full, descriptor never drained (link down?)\r\n");
            return false;
        }
    }

    // Copy into a driver-owned DMA buffer. Most callers build packets in
    // stack-local buffers, so descriptor pointers must not alias ephemeral
    // memory if we want reliable DNS/TCP traffic.
    for (uint16_t i = 0; i < length; i++) {
        tx_buffers[tx_cur][i] = data[i];
    }

    // set up the descriptor. clear status (dd) so the nic can re-flag it on
    // completion; cmd!=0 also marks the slot in-flight for ReclaimTx. order the
    // descriptor writes before the doorbell so the nic sees a complete
    // descriptor when we bump the tail. (satoru)
    txd->length = length;
    txd->status = 0;
    txd->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;

    // count it as sent at enqueue time - we no longer wait for the wire. (satoru)
    tx_count++;
    tx_bytes += length;
    // first 6 packets: dump dst MAC + ethertype for diagnostics
    if (tx_count <= 6) {
        char b[80]; int n = 0;
        const char* p = "[E1000:TX] queued len=";
        while (*p) b[n++] = *p++;
        // length decimal
        uint16_t v = length; char t[8]; int ti = 0;
        if (v == 0) t[ti++] = '0';
        while (v) { t[ti++] = (char)('0' + (v % 10)); v /= 10; }
        while (ti) b[n++] = t[--ti];
        const char* p2 = " dst=";
        while (*p2) b[n++] = *p2++;
        const char* hex = "0123456789abcdef";
        for (int i = 0; i < 6; i++) {
            b[n++] = hex[(data[i] >> 4) & 0xF];
            b[n++] = hex[data[i] & 0xF];
            if (i < 5) b[n++] = ':';
        }
        const char* p3 = " et=";
        while (*p3) b[n++] = *p3++;
        uint16_t et = (uint16_t)((data[12] << 8) | data[13]);
        for (int i = 12; i >= 0; i -= 4) {
            b[n++] = hex[(et >> i) & 0xF];
        }
        b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
        SerialLogger::Log(b);
    }

    // advance the tail and ring the doorbell - fire-and-forget, no wait for
    // dd. the sfence makes the descriptor + buffer writes globally visible
    // before the nic reads them off the bump. (satoru)
    tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
    __asm__ __volatile__("sfence" ::: "memory");
    WriteReg(E1000_TDT, tx_cur);
    return true;
}

void E1000::Poll() {
    if (!detected) return;

    // accumulate the missed-packets count (clear-on-read in hw). a non-zero
    // value means frames arrived with no free rx descriptor and were dropped,
    // forcing the sender to time out + retransmit (the classic bulk-download
    // throughput killer when the ring is too small for the offered window).
    // (satoru)
    rx_missed += ReadReg(E1000_MPC);

    // drain finished tx descriptors here too so the ring empties even during
    // idle/rx-only periods, not just when the next Send happens. (satoru)
    ReclaimTx();

    while (rx_descs[rx_cur].status & E1000_RXD_STAT_DD) {
        uint16_t len = rx_descs[rx_cur].length;
        uint8_t* buf = rx_buffers[rx_cur];

        if (len > 0 && len <= E1000_RX_BUFFER_SIZE) {
            rx_count++;
            rx_bytes += len;

            // diagnostic: dump first 6 RX packets
            if (rx_count <= 6) {
                char b[100]; int n = 0;
                const char* p = "[E1000:RX] len=";
                while (*p) b[n++] = *p++;
                uint16_t v = len; char t[8]; int ti = 0;
                if (v == 0) t[ti++] = '0';
                while (v) { t[ti++] = (char)('0' + (v % 10)); v /= 10; }
                while (ti) b[n++] = t[--ti];
                const char* p2 = " src=";
                while (*p2) b[n++] = *p2++;
                const char* hex = "0123456789abcdef";
                for (int i = 6; i < 12; i++) {
                    b[n++] = hex[(buf[i] >> 4) & 0xF];
                    b[n++] = hex[buf[i] & 0xF];
                    if (i < 11) b[n++] = ':';
                }
                const char* p3 = " et=";
                while (*p3) b[n++] = *p3++;
                uint16_t et = (uint16_t)((buf[12] << 8) | buf[13]);
                for (int i = 12; i >= 0; i -= 4) {
                    b[n++] = hex[(et >> i) & 0xF];
                }
                b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
                SerialLogger::Log(b);
            }

            // deliver packet to handler
            if (packet_handler) {
                packet_handler(buf, len);
            }
        }

        // reset descriptor for reuse - clear status/length/errors so we
        // never re-deliver a stale frame on ring wrap, then issue a
        // compiler+memory barrier so the descriptor write is globally
        // visible before we hand the slot back to the NIC via RDT.
        rx_descs[rx_cur].status = 0;
        rx_descs[rx_cur].length = 0;
        rx_descs[rx_cur].errors = 0;
        __asm__ __volatile__("sfence" ::: "memory");
        WriteReg(E1000_RDT, rx_cur);

        rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
    }
}

void E1000::SetPacketHandler(E1000_PacketHandler handler) {
    packet_handler = handler;
}

uint32_t E1000::GetTxCount() { return tx_count; }
uint32_t E1000::GetRxCount() { return rx_count; }
uint32_t E1000::GetTxBytes() { return tx_bytes; }
uint32_t E1000::GetRxBytes() { return rx_bytes; }
uint32_t E1000::GetRxMissed() { return rx_missed; }
