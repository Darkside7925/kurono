// ═══════════════════════════════════════════════════════════════════════════
//  Intel E1000 (82540EM) Ethernet Driver for Kurono OS
//  Real PCI network card driver — works with QEMU -device e1000
// ═══════════════════════════════════════════════════════════════════════════

#include "e1000.h"
#include "serial.h"
#include "../kernel/heap.h"

// Static member definitions
bool E1000::detected = false;
uint32_t E1000::mmio_base = 0;
uint8_t E1000::mac[6] = {0};

E1000_RXDesc E1000::rx_descs[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
E1000_TXDesc E1000::tx_descs[E1000_NUM_TX_DESC] __attribute__((aligned(16)));
uint8_t E1000::rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUFFER_SIZE] __attribute__((aligned(16)));

uint16_t E1000::rx_cur = 0;
uint16_t E1000::tx_cur = 0;

E1000_PacketHandler E1000::packet_handler = nullptr;
uint32_t E1000::tx_count = 0;
uint32_t E1000::rx_count = 0;
uint32_t E1000::tx_bytes = 0;
uint32_t E1000::rx_bytes = 0;

// ─── PCI Configuration Space Access ──────────────────────────────────────

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
    // Enumerate PCI bus to find E1000 device
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

                // Read BAR0 (MMIO base address)
                uint32_t bar0 = PciRead((uint8_t)bus, slot, 0, 0x10);
                if (bar0 & 1) {
                    // I/O space — not expected for e1000
                    SerialLogger::Log("[E1000] BAR0 is I/O space, skipping\r\n");
                    continue;
                }
                mmio_base = bar0 & 0xFFFFFFF0;

                SerialLogger::Log("[E1000] MMIO base: 0x");
                SerialLogger::LogHex(mmio_base);
                SerialLogger::Log("\r\n");

                // Enable bus mastering + memory space
                uint32_t cmd = PciRead((uint8_t)bus, slot, 0, 0x04);
                cmd |= (1 << 1) | (1 << 2);  // Memory Space + Bus Master
                PciWrite((uint8_t)bus, slot, 0, 0x04, cmd);

                detected = true;
                return true;
            }
        }
    }
    return false;
}

// ─── MMIO Register Access ────────────────────────────────────────────────

void E1000::WriteReg(uint16_t offset, uint32_t value) {
    *((volatile uint32_t*)(uintptr_t)(mmio_base + offset)) = value;
}

uint32_t E1000::ReadReg(uint16_t offset) {
    return *((volatile uint32_t*)(uintptr_t)(mmio_base + offset));
}

// ─── MAC Address Reading ─────────────────────────────────────────────────

void E1000::ReadMAC() {
    // Method 1: Try reading from EEPROM
    bool eeprom_exists = false;

    // Check if EEPROM exists by writing to EERD and checking if done bit sets
    WriteReg(E1000_EERD, 0x01);  // Read word 0
    for (int i = 0; i < 1000; i++) {
        uint32_t val = ReadReg(E1000_EERD);
        if (val & (1 << 4)) {  // Done bit
            eeprom_exists = true;
            break;
        }
    }

    if (eeprom_exists) {
        // Read MAC from EEPROM (3 words, starting at offset 0)
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
        // Method 2: Read from RAL/RAH registers
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

// ─── RX Initialization ──────────────────────────────────────────────────

void E1000::InitRX() {
    // Set up RX descriptor ring
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_descs[i].addr = (uint64_t)(uintptr_t)&rx_buffers[i][0];
        rx_descs[i].status = 0;
    }

    // Program RDBAL/RDBAH
    uint64_t rx_addr = (uint64_t)(uintptr_t)&rx_descs[0];
    WriteReg(E1000_RDBAL, (uint32_t)(rx_addr & 0xFFFFFFFF));
    WriteReg(E1000_RDBAH, (uint32_t)(rx_addr >> 32));

    // Set descriptor ring length (bytes)
    WriteReg(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(E1000_RXDesc));

    // Set head and tail
    WriteReg(E1000_RDH, 0);
    WriteReg(E1000_RDT, E1000_NUM_RX_DESC - 1);

    rx_cur = 0;

    // Enable receiver
    uint32_t rctl = E1000_RCTL_EN |
                    E1000_RCTL_BAM |         // Accept broadcast
                    E1000_RCTL_BSIZE_2048 |  // 2K buffers
                    E1000_RCTL_SECRC;        // Strip CRC
    WriteReg(E1000_RCTL, rctl);
}

// ─── TX Initialization ──────────────────────────────────────────────────

void E1000::InitTX() {
    // Set up TX descriptor ring
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = E1000_TXD_STAT_DD;  // Mark as done
    }

    // Program TDBAL/TDBAH
    uint64_t tx_addr = (uint64_t)(uintptr_t)&tx_descs[0];
    WriteReg(E1000_TDBAL, (uint32_t)(tx_addr & 0xFFFFFFFF));
    WriteReg(E1000_TDBAH, (uint32_t)(tx_addr >> 32));

    // Set descriptor ring length
    WriteReg(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(E1000_TXDesc));

    // Set head and tail
    WriteReg(E1000_TDH, 0);
    WriteReg(E1000_TDT, 0);

    tx_cur = 0;

    // Set inter-packet gap (recommended values)
    WriteReg(E1000_TIPG, 0x0060200A);

    // Enable transmitter
    uint32_t tctl = E1000_TCTL_EN |
                    E1000_TCTL_PSP |           // Pad short packets
                    (15 << E1000_TCTL_CT_SHIFT) |   // Collision threshold
                    (64 << E1000_TCTL_COLD_SHIFT);  // Collision distance
    WriteReg(E1000_TCTL, tctl);
}

void E1000::EnableInterrupts() {
    // Clear pending interrupts
    ReadReg(E1000_ICR);
    // We're polling-based, but set masks for status awareness
    WriteReg(E1000_IMS, 0x1F6DC);  // All useful interrupt causes
}

void E1000::LinkUp() {
    uint32_t ctrl = ReadReg(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE | E1000_CTRL_FD;
    ctrl &= ~E1000_CTRL_LRST;
    ctrl &= ~E1000_CTRL_PHY_RST;
    WriteReg(E1000_CTRL, ctrl);
}

// ─── Public Interface ────────────────────────────────────────────────────

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

    // Software reset
    WriteReg(E1000_CTRL, E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) {}  // Wait for reset
    // Re-read status after reset
    ReadReg(E1000_STATUS);

    // Clear multicast table array
    for (int i = 0; i < 128; i++) {
        WriteReg(E1000_MTA + i * 4, 0);
    }

    // Read MAC address
    ReadMAC();

    // Program MAC into receive address register
    uint32_t ral = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                    ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    uint32_t rah = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31); // AV bit
    WriteReg(E1000_RAL, ral);
    WriteReg(E1000_RAH, rah);

    // Bring link up
    LinkUp();

    // Initialize RX and TX descriptor rings
    InitRX();
    InitTX();

    // Enable interrupts (for status, we poll)
    EnableInterrupts();

    // Check link status
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

bool E1000::Send(const uint8_t* data, uint16_t length) {
    if (!detected || !data || length == 0 || length > 1500) return false;

    // Wait for current TX descriptor to be done
    volatile E1000_TXDesc* txd = &tx_descs[tx_cur];
    int timeout = 10000;
    while (!(txd->status & E1000_TXD_STAT_DD) && --timeout > 0) {
        __asm__ __volatile__("pause");
    }
    if (timeout == 0) return false;

    // Set up the descriptor
    txd->addr = (uint64_t)(uintptr_t)data;
    txd->length = length;
    txd->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    txd->status = 0;

    // Advance tail
    uint16_t old_cur = tx_cur;
    tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
    WriteReg(E1000_TDT, tx_cur);

    // Wait for transmit to complete
    timeout = 100000;
    while (!(tx_descs[old_cur].status & E1000_TXD_STAT_DD) && --timeout > 0) {
        __asm__ __volatile__("pause");
    }

    if (tx_descs[old_cur].status & E1000_TXD_STAT_DD) {
        tx_count++;
        tx_bytes += length;
        return true;
    }
    return false;
}

void E1000::Poll() {
    if (!detected) return;

    while (rx_descs[rx_cur].status & E1000_RXD_STAT_DD) {
        uint16_t len = rx_descs[rx_cur].length;
        uint8_t* buf = rx_buffers[rx_cur];

        if (len > 0 && len <= E1000_RX_BUFFER_SIZE) {
            rx_count++;
            rx_bytes += len;

            // Deliver packet to handler
            if (packet_handler) {
                packet_handler(buf, len);
            }
        }

        // Reset descriptor for reuse
        rx_descs[rx_cur].status = 0;
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
