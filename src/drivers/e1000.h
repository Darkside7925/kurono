#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Intel E1000 (82540EM) Ethernet Driver for Kurono OS
//  Real PCI network card driver for QEMU -device e1000
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// PCI IDs
#define E1000_VENDOR_ID    0x8086
#define E1000_DEVICE_ID    0x100E   // 82540EM (QEMU default)
#define E1000_DEVICE_ID2   0x100F   // 82545EM
#define E1000_DEVICE_ID3   0x10D3   // 82574L

// Register offsets
#define E1000_CTRL         0x0000   // Device Control
#define E1000_STATUS       0x0008   // Device Status
#define E1000_EECD         0x0010   // EEPROM Control
#define E1000_EERD         0x0014   // EEPROM Read
#define E1000_ICR          0x00C0   // Interrupt Cause Read
#define E1000_IMS          0x00D0   // Interrupt Mask Set
#define E1000_IMC          0x00D8   // Interrupt Mask Clear
#define E1000_RCTL         0x0100   // Receive Control
#define E1000_TCTL         0x0400   // Transmit Control
#define E1000_RDBAL        0x2800   // RX Descriptor Base Low
#define E1000_RDBAH        0x2804   // RX Descriptor Base High
#define E1000_RDLEN        0x2808   // RX Descriptor Length
#define E1000_RDH          0x2810   // RX Descriptor Head
#define E1000_RDT          0x2818   // RX Descriptor Tail
#define E1000_TDBAL        0x3800   // TX Descriptor Base Low
#define E1000_TDBAH        0x3804   // TX Descriptor Base High
#define E1000_TDLEN        0x3808   // TX Descriptor Length
#define E1000_TDH          0x3810   // TX Descriptor Head
#define E1000_TDT          0x3818   // TX Descriptor Tail
#define E1000_RAL          0x5400   // Receive Address Low
#define E1000_RAH          0x5404   // Receive Address High
#define E1000_MTA          0x5200   // Multicast Table Array (128 entries)
#define E1000_TIPG         0x0410   // TX Inter-Packet Gap

// CTRL register bits
#define E1000_CTRL_FD      (1 << 0)   // Full Duplex
#define E1000_CTRL_LRST    (1 << 3)   // Link Reset
#define E1000_CTRL_ASDE    (1 << 5)   // Auto-Speed Detection Enable
#define E1000_CTRL_SLU     (1 << 6)   // Set Link Up
#define E1000_CTRL_RST     (1 << 26)  // Device Reset
#define E1000_CTRL_VME     (1 << 30)  // VLAN Mode Enable
#define E1000_CTRL_PHY_RST (1 << 31)  // PHY Reset

// RCTL register bits
#define E1000_RCTL_EN      (1 << 1)   // Receiver Enable
#define E1000_RCTL_SBP     (1 << 2)   // Store Bad Packets
#define E1000_RCTL_UPE     (1 << 3)   // Unicast Promiscuous Enable
#define E1000_RCTL_MPE     (1 << 4)   // Multicast Promiscuous Enable
#define E1000_RCTL_LBM     (3 << 6)   // Loopback Mode
#define E1000_RCTL_BAM     (1 << 15)  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_256   (3 << 16)
#define E1000_RCTL_BSIZE_512   (2 << 16)
#define E1000_RCTL_BSIZE_1024  (1 << 16)
#define E1000_RCTL_BSIZE_2048  (0 << 16)
#define E1000_RCTL_BSIZE_4096  ((3 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_8192  ((2 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_16384 ((1 << 16) | (1 << 25))
#define E1000_RCTL_SECRC   (1 << 26)  // Strip Ethernet CRC

// TCTL register bits
#define E1000_TCTL_EN      (1 << 1)   // Transmit Enable
#define E1000_TCTL_PSP     (1 << 3)   // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

// TX Descriptor command bits
#define E1000_TXD_CMD_EOP  (1 << 0)   // End of Packet
#define E1000_TXD_CMD_IFCS (1 << 1)   // Insert FCS
#define E1000_TXD_CMD_RS   (1 << 3)   // Report Status
#define E1000_TXD_STAT_DD  (1 << 0)   // Descriptor Done

// RX Descriptor status bits
#define E1000_RXD_STAT_DD  (1 << 0)   // Descriptor Done
#define E1000_RXD_STAT_EOP (1 << 1)   // End of Packet

// Descriptor counts  (must be multiple of 8, aligned to 128 bytes)
#define E1000_NUM_RX_DESC  32
#define E1000_NUM_TX_DESC  32
#define E1000_RX_BUFFER_SIZE  2048

// RX Descriptor (legacy format)
struct E1000_RXDesc {
    uint64_t addr;       // Buffer address
    uint16_t length;     // Packet length
    uint16_t checksum;   // Packet checksum
    uint8_t  status;     // Status
    uint8_t  errors;     // Errors
    uint16_t special;    // Special
} __attribute__((packed));

// TX Descriptor (legacy format)
struct E1000_TXDesc {
    uint64_t addr;       // Buffer address
    uint16_t length;     // Data length
    uint8_t  cso;        // Checksum offset
    uint8_t  cmd;        // Command
    uint8_t  status;     // Status
    uint8_t  css;        // Checksum start
    uint16_t special;    // Special
} __attribute__((packed));

// Received packet callback
typedef void (*E1000_PacketHandler)(const uint8_t* data, uint16_t length);

class E1000 {
public:
    static bool Init();           // Scan PCI, map MMIO, init descriptors
    static bool IsDetected();     // Was an E1000 found on PCI?
    static bool IsLinkUp();       // Is the link active?
    
    // MAC address
    static void GetMAC(uint8_t mac[6]);
    
    // Send a raw Ethernet frame
    static bool Send(const uint8_t* data, uint16_t length);
    
    // Poll for received packets (call from main loop)
    static void Poll();
    
    // Set receive callback
    static void SetPacketHandler(E1000_PacketHandler handler);
    
    // Statistics
    static uint32_t GetTxCount();
    static uint32_t GetRxCount();
    static uint32_t GetTxBytes();
    static uint32_t GetRxBytes();

private:
    static bool detected;
    static uint32_t mmio_base;     // Memory-mapped I/O base address
    static uint8_t mac[6];
    
    // Descriptor rings
    static E1000_RXDesc rx_descs[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
    static E1000_TXDesc tx_descs[E1000_NUM_TX_DESC] __attribute__((aligned(16)));
    static uint8_t rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUFFER_SIZE] __attribute__((aligned(16)));
    
    static uint16_t rx_cur;        // Current RX descriptor index
    static uint16_t tx_cur;        // Current TX descriptor index
    
    static E1000_PacketHandler packet_handler;
    static uint32_t tx_count, rx_count, tx_bytes, rx_bytes;
    
    // MMIO register access
    static void WriteReg(uint16_t offset, uint32_t value);
    static uint32_t ReadReg(uint16_t offset);
    
    // PCI config space
    static uint32_t PciRead(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    static void PciWrite(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
    static bool ScanPCI();
    
    // Initialization steps
    static void ReadMAC();
    static void InitRX();
    static void InitTX();
    static void EnableInterrupts();     // Set up interrupt masks (for polling)
    static void LinkUp();
};
