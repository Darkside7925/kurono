//  kurono os - virtual ide/ata disk controller emulation
//  emulates a primary ide controller with a single disk (master).
//  supports pio mode transfers, identify device, read/write sectors.
//  backed by a ram buffer as virtual disk image.
//
//  reference: ata/atapi-7 specification, osdev ata pio mode
#pragma once
#include <stdint.h>
#include <stddef.h>

// command block: 0x1f0-0x1f7, control block: 0x3f6-0x3f7
constexpr uint16_t ATA_PRI_DATA      = 0x1F0;
constexpr uint16_t ATA_PRI_ERROR     = 0x1F1; // read: error, write: features
constexpr uint16_t ATA_PRI_FEATURES  = 0x1F1;
constexpr uint16_t ATA_PRI_COUNT     = 0x1F2; // sector count
constexpr uint16_t ATA_PRI_LBA_LO   = 0x1F3; // lba bits [7:0] / sector number
constexpr uint16_t ATA_PRI_LBA_MID  = 0x1F4; // lba bits [15:8] / cylinder low
constexpr uint16_t ATA_PRI_LBA_HI   = 0x1F5; // lba bits [23:16] / cylinder high
constexpr uint16_t ATA_PRI_DRVHEAD  = 0x1F6; // drive/head / lba bits [27:24]
constexpr uint16_t ATA_PRI_STATUS   = 0x1F7; // read: status, write: command
constexpr uint16_t ATA_PRI_COMMAND  = 0x1F7;
constexpr uint16_t ATA_PRI_ALTSTAT  = 0x3F6; // read: alt status, write: device control
constexpr uint16_t ATA_PRI_DEVCTRL  = 0x3F6;

constexpr uint16_t ATA_SEC_DATA     = 0x170;
constexpr uint16_t ATA_SEC_STATUS   = 0x177;
constexpr uint16_t ATA_SEC_ALTSTAT  = 0x376;

constexpr uint8_t ATA_SR_ERR   = 0x01; // error
constexpr uint8_t ATA_SR_IDX   = 0x02; // index
constexpr uint8_t ATA_SR_CORR  = 0x04; // corrected data
constexpr uint8_t ATA_SR_DRQ   = 0x08; // data request
constexpr uint8_t ATA_SR_DSC   = 0x10; // drive seek complete
constexpr uint8_t ATA_SR_DF    = 0x20; // drive write fault
constexpr uint8_t ATA_SR_DRDY  = 0x40; // drive ready
constexpr uint8_t ATA_SR_BSY   = 0x80; // busy

constexpr uint8_t ATA_ER_AMNF  = 0x01; // address mark not found
constexpr uint8_t ATA_ER_TK0NF = 0x02; // track 0 not found
constexpr uint8_t ATA_ER_ABRT  = 0x04; // aborted command
constexpr uint8_t ATA_ER_MCR   = 0x08; // media change request
constexpr uint8_t ATA_ER_IDNF  = 0x10; // id not found
constexpr uint8_t ATA_ER_MC    = 0x20; // media changed
constexpr uint8_t ATA_ER_UNC   = 0x40; // uncorrectable data error
constexpr uint8_t ATA_ER_BBK   = 0x80; // bad block detected

constexpr uint8_t ATA_CMD_READ_SECTORS      = 0x20; // read sectors (pio)
constexpr uint8_t ATA_CMD_READ_SECTORS_EXT  = 0x24; // read sectors ext (48-bit)
constexpr uint8_t ATA_CMD_WRITE_SECTORS     = 0x30; // write sectors (pio)
constexpr uint8_t ATA_CMD_WRITE_SECTORS_EXT = 0x34; // write sectors ext
constexpr uint8_t ATA_CMD_READ_VERIFY       = 0x40; // read verify (no data)
constexpr uint8_t ATA_CMD_CACHE_FLUSH       = 0xE7; // flush cache
constexpr uint8_t ATA_CMD_IDENTIFY          = 0xEC; // identify device
constexpr uint8_t ATA_CMD_SET_FEATURES      = 0xEF; // set features
constexpr uint8_t ATA_CMD_NOP               = 0x00; // nop
constexpr uint8_t ATA_CMD_DEVICE_RESET      = 0x08; // device reset
constexpr uint8_t ATA_CMD_RECALIBRATE       = 0x10; // recalibrate
constexpr uint8_t ATA_CMD_READ_DMA          = 0xC8; // read dma
constexpr uint8_t ATA_CMD_WRITE_DMA         = 0xCA; // write dma
constexpr uint8_t ATA_CMD_STANDBY_IMM       = 0xE0; // standby immediate
constexpr uint8_t ATA_CMD_IDLE_IMM          = 0xE1; // idle immediate
constexpr uint8_t ATA_CMD_STANDBY           = 0xE2; // standby
constexpr uint8_t ATA_CMD_IDLE              = 0xE3; // idle
constexpr uint8_t ATA_CMD_CHECK_POWER       = 0xE5; // check power mode
constexpr uint8_t ATA_CMD_SLEEP             = 0xE6; // sleep
constexpr uint8_t ATA_CMD_INIT_DEV_PARAMS   = 0x91; // initialize device params

constexpr uint8_t ATA_DCR_NIEN  = 0x02; // disable interrupt (nien)
constexpr uint8_t ATA_DCR_SRST  = 0x04; // software reset
constexpr uint8_t ATA_DCR_HOB   = 0x80; // high order byte (48-bit lba)

constexpr uint8_t ATA_DH_DEV    = 0x10; // device select (0=master, 1=slave)
constexpr uint8_t ATA_DH_LBA    = 0x40; // use lba addressing
constexpr uint8_t ATA_DH_FIXED  = 0xA0; // fixed bits (must be set)

constexpr uint32_t ATA_SECTOR_SIZE  = 512;
constexpr uint32_t VDISK_MAX_SIZE   = 512 * 1024 * 1024; // 512 mb max disk
constexpr uint32_t VDISK_MAX_SECTORS= VDISK_MAX_SIZE / ATA_SECTOR_SIZE;
constexpr int      ATA_IRQ_PRIMARY  = 14;
constexpr int      ATA_IRQ_SECONDARY= 15;

constexpr int ATA_XFER_BUF_SIZE = ATA_SECTOR_SIZE; // 512 bytes

//  identify device data structure (512 bytes = 256 words)
struct ATAIdentify {
    uint16_t words[256];

    void Build(uint32_t total_sectors, const char* serial, const char* firmware,
               const char* model);
};

//  virtualdisk - full ide/ata controller with ram-backed storage
class VirtualDisk {
public:
    void Init(uint32_t disk_size_bytes);
    void Reset();

    // disk_data must point to 'disk_size' bytes of storage
    void AttachStorage(uint8_t* disk_data, uint32_t disk_size);

    bool LoadImage(const uint8_t* data, uint32_t size, uint32_t offset_bytes);

    void     WritePort(uint16_t port, uint32_t value, uint8_t size);
    uint32_t ReadPort(uint16_t port, uint8_t size);

    bool     HasPendingIRQ() const;
    void     ClearIRQ();

    uint32_t GetTotalSectors() const { return total_sectors; }
    uint32_t GetDiskSize()     const { return total_sectors * ATA_SECTOR_SIZE; }
    bool     IsAttached()      const { return storage != nullptr; }

    void DumpState();

private:
    uint8_t* storage;
    uint32_t total_sectors;
    uint32_t storage_size;

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

    uint8_t  xfer_buf[ATA_XFER_BUF_SIZE];
    int      xfer_pos;          // current position in transfer buffer
    int      xfer_len;          // total bytes to transfer
    int      xfer_sectors_left; // sectors remaining in multi-sector transfer
    bool     xfer_write;        // true = host→disk, false = disk→host
    uint32_t xfer_lba;          // current lba for multi-sector ops

    bool     irq_pending;
    bool     irq_enabled;       // !nien

    ATAIdentify identify_data;

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
