#pragma once
//  intel e1000 (82540em) ethernet driver for kurono os
//  real pci network card driver for qemu -device e1000

#include "../kernel/types.h"

// pci ids
#define E1000_VENDOR_ID    0x8086
#define E1000_DEVICE_ID    0x100E   // 82540em (qemu default)
#define E1000_DEVICE_ID2   0x100F   // 82545em
#define E1000_DEVICE_ID3   0x10D3   // 82574l

// register offsets
#define E1000_CTRL         0x0000   // device control
#define E1000_STATUS       0x0008   // device status
#define E1000_EECD         0x0010   // eeprom control
#define E1000_EERD         0x0014   // eeprom read
#define E1000_ICR          0x00C0   // interrupt cause read
#define E1000_IMS          0x00D0   // interrupt mask set
#define E1000_IMC          0x00D8   // interrupt mask clear
#define E1000_RCTL         0x0100   // receive control
#define E1000_TCTL         0x0400   // transmit control
#define E1000_RDBAL        0x2800   // rx descriptor base low
#define E1000_RDBAH        0x2804   // rx descriptor base high
#define E1000_RDLEN        0x2808   // rx descriptor length
#define E1000_RDH          0x2810   // rx descriptor head
#define E1000_RDT          0x2818   // rx descriptor tail
#define E1000_TDBAL        0x3800   // tx descriptor base low
#define E1000_TDBAH        0x3804   // tx descriptor base high
#define E1000_TDLEN        0x3808   // tx descriptor length
#define E1000_TDH          0x3810   // tx descriptor head
#define E1000_TDT          0x3818   // tx descriptor tail
#define E1000_RAL          0x5400   // receive address low
#define E1000_RAH          0x5404   // receive address high
#define E1000_MTA          0x5200   // multicast table array (128 entries)
#define E1000_TIPG         0x0410   // tx inter-packet gap

// ctrl register bits
#define E1000_CTRL_FD      (1 << 0)   // full duplex
#define E1000_CTRL_LRST    (1 << 3)   // link reset
#define E1000_CTRL_ASDE    (1 << 5)   // auto-speed detection enable
#define E1000_CTRL_SLU     (1 << 6)   // set link up
#define E1000_CTRL_RST     (1 << 26)  // device reset
#define E1000_CTRL_VME     (1 << 30)  // vlan mode enable
#define E1000_CTRL_PHY_RST (1 << 31)  // phy reset

// rctl register bits
#define E1000_RCTL_EN      (1 << 1)   // receiver enable
#define E1000_RCTL_SBP     (1 << 2)   // store bad packets
#define E1000_RCTL_UPE     (1 << 3)   // unicast promiscuous enable
#define E1000_RCTL_MPE     (1 << 4)   // multicast promiscuous enable
#define E1000_RCTL_LBM     (3 << 6)   // loopback mode
#define E1000_RCTL_BAM     (1 << 15)  // broadcast accept mode
#define E1000_RCTL_BSIZE_256   (3 << 16)
#define E1000_RCTL_BSIZE_512   (2 << 16)
#define E1000_RCTL_BSIZE_1024  (1 << 16)
#define E1000_RCTL_BSIZE_2048  (0 << 16)
#define E1000_RCTL_BSIZE_4096  ((3 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_8192  ((2 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_16384 ((1 << 16) | (1 << 25))
#define E1000_RCTL_SECRC   (1 << 26)  // strip ethernet crc

// tctl register bits
#define E1000_TCTL_EN      (1 << 1)   // transmit enable
#define E1000_TCTL_PSP     (1 << 3)   // pad short packets
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

// tx descriptor command bits
#define E1000_TXD_CMD_EOP  (1 << 0)   // end of packet
#define E1000_TXD_CMD_IFCS (1 << 1)   // insert fcs
#define E1000_TXD_CMD_RS   (1 << 3)   // report status
#define E1000_TXD_STAT_DD  (1 << 0)   // descriptor done

// rx descriptor status bits
#define E1000_RXD_STAT_DD  (1 << 0)   // descriptor done
#define E1000_RXD_STAT_EOP (1 << 1)   // end of packet

// descriptor counts  (must be multiple of 8, aligned to 128 bytes)
#define E1000_NUM_RX_DESC  32
#define E1000_NUM_TX_DESC  32
#define E1000_RX_BUFFER_SIZE  2048

// rx descriptor (legacy format)
struct E1000_RXDesc {
    uint64_t addr;       // buffer address
    uint16_t length;     // packet length
    uint16_t checksum;   // packet checksum
    uint8_t  status;     // status
    uint8_t  errors;     // errors
    uint16_t special;    // special
} __attribute__((packed));

// tx descriptor (legacy format)
struct E1000_TXDesc {
    uint64_t addr;       // buffer address
    uint16_t length;     // data length
    uint8_t  cso;        // checksum offset
    uint8_t  cmd;        // command
    uint8_t  status;     // status
    uint8_t  css;        // checksum start
    uint16_t special;    // special
} __attribute__((packed));

// received packet callback
typedef void (*E1000_PacketHandler)(const uint8_t* data, uint16_t length);

class E1000 {
public:
    static bool Init();           // scan pci, map mmio, init descriptors
    static bool IsDetected();     // was an e1000 found on pci?
    static bool IsLinkUp();       // is the link active?
    
    // mac address
    static void GetMAC(uint8_t mac[6]);
    
    // send a raw ethernet frame
    static bool Send(const uint8_t* data, uint16_t length);
    
    // poll for received packets (call from main loop)
    static void Poll();
    
    // set receive callback
    static void SetPacketHandler(E1000_PacketHandler handler);
    
    // statistics
    static uint32_t GetTxCount();
    static uint32_t GetRxCount();
    static uint32_t GetTxBytes();
    static uint32_t GetRxBytes();

private:
    static bool detected;
    static uint32_t mmio_base;     // memory-mapped i/o base address
    static uint8_t mac[6];
    
    // descriptor rings
    static E1000_RXDesc rx_descs[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
    static E1000_TXDesc tx_descs[E1000_NUM_TX_DESC] __attribute__((aligned(16)));
    static uint8_t rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUFFER_SIZE] __attribute__((aligned(16)));
    
    static uint16_t rx_cur;        // current rx descriptor index
    static uint16_t tx_cur;        // current tx descriptor index
    
    static E1000_PacketHandler packet_handler;
    static uint32_t tx_count, rx_count, tx_bytes, rx_bytes;
    
    // mmio register access
    static void WriteReg(uint16_t offset, uint32_t value);
    static uint32_t ReadReg(uint16_t offset);
    
    // pci config space
    static uint32_t PciRead(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    static void PciWrite(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
    static bool ScanPCI();
    
    // initialization steps
    static void ReadMAC();
    static void InitRX();
    static void InitTX();
    static void EnableInterrupts();     // set up interrupt masks (for polling)
    static void LinkUp();
};
