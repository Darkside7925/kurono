// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Virtual IDE/ATA Disk Controller Emulation
//  Emulates a primary IDE controller with a single disk (master).
//  Supports PIO mode transfers, IDENTIFY DEVICE, READ/WRITE SECTORS.
//  Backed by a RAM buffer as virtual disk image.
//
//  Reference: ATA/ATAPI-7 specification, OSDev ATA PIO Mode
// ═══════════════════════════════════════════════════════════════════════════
#pragma once
#include <stdint.h>
#include <stddef.h>

// ─── ATA Ports (Primary Controller) ─────────────────────────────────────
// Command block: 0x1F0-0x1F7, Control block: 0x3F6-0x3F7
constexpr uint16_t ATA_PRI_DATA      = 0x1F0;
constexpr uint16_t ATA_PRI_ERROR     = 0x1F1; // Read: Error, Write: Features
constexpr uint16_t ATA_PRI_FEATURES  = 0x1F1;
constexpr uint16_t ATA_PRI_COUNT     = 0x1F2; // Sector count
constexpr uint16_t ATA_PRI_LBA_LO   = 0x1F3; // LBA bits [7:0] / Sector number
constexpr uint16_t ATA_PRI_LBA_MID  = 0x1F4; // LBA bits [15:8] / Cylinder low
constexpr uint16_t ATA_PRI_LBA_HI   = 0x1F5; // LBA bits [23:16] / Cylinder high
constexpr uint16_t ATA_PRI_DRVHEAD  = 0x1F6; // Drive/Head / LBA bits [27:24]
constexpr uint16_t ATA_PRI_STATUS   = 0x1F7; // Read: Status, Write: Command
constexpr uint16_t ATA_PRI_COMMAND  = 0x1F7;
constexpr uint16_t ATA_PRI_ALTSTAT  = 0x3F6; // Read: Alt Status, Write: Device Control
constexpr uint16_t ATA_PRI_DEVCTRL  = 0x3F6;

// ─── Secondary Controller (optional) ────────────────────────────────────
constexpr uint16_t ATA_SEC_DATA     = 0x170;
constexpr uint16_t ATA_SEC_STATUS   = 0x177;
constexpr uint16_t ATA_SEC_ALTSTAT  = 0x376;

// ─── ATA Status Register Bits ────────────────────────────────────────────
constexpr uint8_t ATA_SR_ERR   = 0x01; // Error
constexpr uint8_t ATA_SR_IDX   = 0x02; // Index
constexpr uint8_t ATA_SR_CORR  = 0x04; // Corrected data
constexpr uint8_t ATA_SR_DRQ   = 0x08; // Data Request
constexpr uint8_t ATA_SR_DSC   = 0x10; // Drive Seek Complete
constexpr uint8_t ATA_SR_DF    = 0x20; // Drive Write Fault
constexpr uint8_t ATA_SR_DRDY  = 0x40; // Drive Ready
constexpr uint8_t ATA_SR_BSY   = 0x80; // Busy

// ─── ATA Error Register Bits ─────────────────────────────────────────────
constexpr uint8_t ATA_ER_AMNF  = 0x01; // Address Mark Not Found
constexpr uint8_t ATA_ER_TK0NF = 0x02; // Track 0 Not Found
constexpr uint8_t ATA_ER_ABRT  = 0x04; // Aborted Command
constexpr uint8_t ATA_ER_MCR   = 0x08; // Media Change Request
constexpr uint8_t ATA_ER_IDNF  = 0x10; // ID Not Found
constexpr uint8_t ATA_ER_MC    = 0x20; // Media Changed
constexpr uint8_t ATA_ER_UNC   = 0x40; // Uncorrectable Data Error
constexpr uint8_t ATA_ER_BBK   = 0x80; // Bad Block Detected

// ─── ATA Commands ────────────────────────────────────────────────────────
constexpr uint8_t ATA_CMD_READ_SECTORS      = 0x20; // Read Sectors (PIO)
constexpr uint8_t ATA_CMD_READ_SECTORS_EXT  = 0x24; // Read Sectors Ext (48-bit)
constexpr uint8_t ATA_CMD_WRITE_SECTORS     = 0x30; // Write Sectors (PIO)
constexpr uint8_t ATA_CMD_WRITE_SECTORS_EXT = 0x34; // Write Sectors Ext
constexpr uint8_t ATA_CMD_READ_VERIFY       = 0x40; // Read Verify (no data)
constexpr uint8_t ATA_CMD_CACHE_FLUSH       = 0xE7; // Flush Cache
constexpr uint8_t ATA_CMD_IDENTIFY          = 0xEC; // Identify Device
constexpr uint8_t ATA_CMD_SET_FEATURES      = 0xEF; // Set Features
constexpr uint8_t ATA_CMD_NOP               = 0x00; // NOP
constexpr uint8_t ATA_CMD_DEVICE_RESET      = 0x08; // Device Reset
constexpr uint8_t ATA_CMD_RECALIBRATE       = 0x10; // Recalibrate
constexpr uint8_t ATA_CMD_READ_DMA          = 0xC8; // Read DMA
constexpr uint8_t ATA_CMD_WRITE_DMA         = 0xCA; // Write DMA
constexpr uint8_t ATA_CMD_STANDBY_IMM       = 0xE0; // Standby Immediate
constexpr uint8_t ATA_CMD_IDLE_IMM          = 0xE1; // Idle Immediate
constexpr uint8_t ATA_CMD_STANDBY           = 0xE2; // Standby
constexpr uint8_t ATA_CMD_IDLE              = 0xE3; // Idle
constexpr uint8_t ATA_CMD_CHECK_POWER       = 0xE5; // Check Power Mode
constexpr uint8_t ATA_CMD_SLEEP             = 0xE6; // Sleep
constexpr uint8_t ATA_CMD_INIT_DEV_PARAMS   = 0x91; // Initialize Device Params

// ─── Device Control Register Bits ────────────────────────────────────────
constexpr uint8_t ATA_DCR_NIEN  = 0x02; // Disable interrupt (nIEN)
constexpr uint8_t ATA_DCR_SRST  = 0x04; // Software Reset
constexpr uint8_t ATA_DCR_HOB   = 0x80; // High Order Byte (48-bit LBA)

// ─── Drive/Head Register Bits ────────────────────────────────────────────
constexpr uint8_t ATA_DH_DEV    = 0x10; // Device select (0=master, 1=slave)
constexpr uint8_t ATA_DH_LBA    = 0x40; // Use LBA addressing
constexpr uint8_t ATA_DH_FIXED  = 0xA0; // Fixed bits (must be set)

// ─── Virtual disk parameters ─────────────────────────────────────────────
constexpr uint32_t ATA_SECTOR_SIZE  = 512;
constexpr uint32_t VDISK_MAX_SIZE   = 16 * 1024 * 1024; // 16 MB max disk
constexpr uint32_t VDISK_MAX_SECTORS= VDISK_MAX_SIZE / ATA_SECTOR_SIZE;
constexpr int      ATA_IRQ_PRIMARY  = 14;
constexpr int      ATA_IRQ_SECONDARY= 15;

// ─── Transfer buffer ─────────────────────────────────────────────────────
constexpr int ATA_XFER_BUF_SIZE = ATA_SECTOR_SIZE; // 512 bytes

// ═══════════════════════════════════════════════════════════════════════════
//  IDENTIFY DEVICE data structure (512 bytes = 256 words)
// ═══════════════════════════════════════════════════════════════════════════
struct ATAIdentify {
    uint16_t words[256];

    void Build(uint32_t total_sectors, const char* serial, const char* firmware,
               const char* model);
};

// ═══════════════════════════════════════════════════════════════════════════
//  VirtualDisk — Full IDE/ATA controller with RAM-backed storage
// ═══════════════════════════════════════════════════════════════════════════
class VirtualDisk {
public:
    // ── Initialization ───────────────────────────────────────────────────
    void Init(uint32_t disk_size_bytes);
    void Reset();

    // ── Attach RAM-backed storage ────────────────────────────────────────
    // disk_data must point to 'disk_size' bytes of storage
    void AttachStorage(uint8_t* disk_data, uint32_t disk_size);

    // ── Load data into the virtual disk (e.g. kernel image) ──────────────
    bool LoadImage(const uint8_t* data, uint32_t size, uint32_t offset_bytes);

    // ── Port I/O (called by VM exit handler) ─────────────────────────────
    void     WritePort(uint16_t port, uint32_t value, uint8_t size);
    uint32_t ReadPort(uint16_t port, uint8_t size);

    // ── Interrupt management ─────────────────────────────────────────────
    bool     HasPendingIRQ() const;
    void     ClearIRQ();

    // ── Status ───────────────────────────────────────────────────────────
    uint32_t GetTotalSectors() const { return total_sectors; }
    uint32_t GetDiskSize()     const { return total_sectors * ATA_SECTOR_SIZE; }
    bool     IsAttached()      const { return storage != nullptr; }

    // ── Debug ────────────────────────────────────────────────────────────
    void DumpState();

private:
    // ── Storage backend ──────────────────────────────────────────────────
    uint8_t* storage;
    uint32_t total_sectors;
    uint32_t storage_size;

    // ── Registers ────────────────────────────────────────────────────────
    uint8_t  status;
    uint8_t  error;
    uint8_t  features;
    uint8_t  sector_count;
    uint8_t  lba_lo;
    uint8_t  lba_mid;
    uint8_t  lba_hi;
    uint8_t  drive_head;
    uint8_t  device_control;
    uint8_t  selected_drive;     // 0 = master, 1 = slave

    // ── Transfer state ───────────────────────────────────────────────────
    uint8_t  xfer_buf[ATA_XFER_BUF_SIZE];
    int      xfer_pos;          // Current position in transfer buffer
    int      xfer_len;          // Total bytes to transfer
    int      xfer_sectors_left; // Sectors remaining in multi-sector transfer
    bool     xfer_write;        // true = host→disk, false = disk→host
    uint32_t xfer_lba;          // Current LBA for multi-sector ops

    // ── Interrupt state ──────────────────────────────────────────────────
    bool     irq_pending;
    bool     irq_enabled;       // !nIEN

    // ── IDENTIFY data ────────────────────────────────────────────────────
    ATAIdentify identify_data;

    // ── Internal helpers ─────────────────────────────────────────────────
    void     ExecuteCommand(uint8_t cmd);
    void     DoReadSectors();
    void     DoWriteSectors();
    void     DoIdentify();
    void     DoReadVerify();
    void     DoSetFeatures();
    void     DoFlushCache();
    void     DoInitDevParams();

    uint32_t GetCurrentLBA() const;
    bool     ReadSectorToBuffer(uint32_t lba);
    bool     WriteSectorFromBuffer(uint32_t lba);
    void     PrepareNextReadSector();
    void     RaiseIRQ();
    void     AbortCommand(uint8_t err_bits);
};
