// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Virtual IDE/ATA Disk Controller Implementation
//  Full PIO-mode ATA controller with RAM-backed disk image.
//
//  Supports:
//    - IDENTIFY DEVICE (0xEC)
//    - READ SECTORS (0x20) — PIO, 28-bit LBA
//    - WRITE SECTORS (0x30) — PIO, 28-bit LBA
//    - READ VERIFY (0x40)
//    - CACHE FLUSH (0xE7)
//    - SET FEATURES (0xEF)
//    - INITIALIZE DEVICE PARAMETERS (0x91)
//    - Power management commands (STANDBY/IDLE/SLEEP)
//    - Software Reset (SRST)
//    - Multi-sector transfers
//    - IRQ 14 generation
//
//  Reference: ATA/ATAPI-7, OSDev ATA PIO Mode
// ═══════════════════════════════════════════════════════════════════════════
#include "vdisk.h"
#include "../drivers/serial.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"

// ═══════════════════════════════════════════════════════════════════════════
//  IDENTIFY DEVICE — build the 512-byte identification data
// ═══════════════════════════════════════════════════════════════════════════

static void SetATAString(uint16_t* words, int start_word, int num_words,
                          const char* str) {
    // ATA strings are byte-swapped within each word
    for (int i = 0; i < num_words; i++) {
        char c1 = *str ? *str++ : ' ';
        char c2 = *str ? *str++ : ' ';
        words[start_word + i] = ((uint16_t)(uint8_t)c1 << 8) | (uint8_t)c2;
    }
}

void ATAIdentify::Build(uint32_t total_sectors, const char* serial,
                         const char* firmware, const char* model) {
    memset(words, 0, sizeof(words));

    // Word 0: General config
    //   bit 15: 0 = ATA, bit 6: fixed drive
    words[0] = 0x0040; // Fixed disk, non-removable

    // Word 1: Number of logical cylinders (obsolete)
    uint32_t cyls = total_sectors / (16 * 63); // CHS: 16 heads, 63 sectors
    if (cyls > 16383) cyls = 16383;
    words[1] = (uint16_t)cyls;

    // Word 3: Number of logical heads
    words[3] = 16;

    // Word 6: Number of logical sectors per track
    words[6] = 63;

    // Words 10-19: Serial number (20 ASCII characters)
    SetATAString(words, 10, 10, serial);

    // Word 22: Obsolete (vendor-specific), ECC bytes
    words[22] = 4;

    // Words 23-26: Firmware revision (8 ASCII characters)
    SetATAString(words, 23, 4, firmware);

    // Words 27-46: Model number (40 ASCII characters)
    SetATAString(words, 27, 20, model);

    // Word 47: Max sectors per interrupt (READ/WRITE MULTIPLE)
    words[47] = 0x8010; // 16 sectors max

    // Word 49: Capabilities
    //   bit 8: DMA supported, bit 9: LBA supported
    //   bit 11: IORDY supported, bit 13: standby timer
    words[49] = (1 << 9) | (1 << 11) | (1 << 13); // LBA + IORDY + standby

    // Word 50: Additional capabilities
    words[50] = 0x4001; // Standby timer value minimum

    // Word 51: PIO data transfer cycle timing mode
    words[51] = 0x0200; // PIO mode 2

    // Word 53: Field validity
    //   bit 0: words 54-58 valid
    //   bit 1: words 64-70 valid
    //   bit 2: word 88 valid
    words[53] = 0x0007;

    // Words 54-56: Current CHS (same as logical)
    words[54] = words[1]; // cylinders
    words[55] = 16;       // heads
    words[56] = 63;       // sectors

    // Words 57-58: Current capacity in sectors (CHS)
    uint32_t chs_sectors = (uint32_t)words[54] * 16 * 63;
    words[57] = (uint16_t)(chs_sectors & 0xFFFF);
    words[58] = (uint16_t)(chs_sectors >> 16);

    // Word 59: Multiple sector setting
    words[59] = 0x0110; // Multiple sector valid, current = 16

    // Words 60-61: Total addressable sectors (28-bit LBA)
    words[60] = (uint16_t)(total_sectors & 0xFFFF);
    words[61] = (uint16_t)(total_sectors >> 16);

    // Word 63: Multiword DMA modes
    words[63] = 0x0007; // MDMA modes 0, 1, 2 supported

    // Word 64: PIO modes supported
    words[64] = 0x0003; // PIO modes 3, 4 supported

    // Word 65: Minimum multiword DMA transfer cycle time
    words[65] = 120; // 120ns

    // Word 66: Recommended multiword DMA transfer cycle time
    words[66] = 120;

    // Word 67: Minimum PIO transfer cycle time without IORDY
    words[67] = 120;

    // Word 68: Minimum PIO transfer cycle time with IORDY
    words[68] = 120;

    // Word 80: Major version (ATA-6)
    words[80] = (1 << 6); // ATA-6

    // Word 81: Minor version
    words[81] = 0x0019; // ATA/ATAPI-6 T13 1410D rev 2

    // Word 82: Command set supported 1
    //   bit 0: SMART, bit 5: write cache, bit 10: FLUSH CACHE
    //   bit 12: NOP, bit 14: SET FEATURES required
    words[82] = (1 << 5) | (1 << 10) | (1 << 14);

    // Word 83: Command set supported 2
    //   bit 10: 48-bit LBA (we don't support, keep 0)
    //   bit 14: must be 1
    words[83] = (1 << 14);

    // Word 84: Command set/feature supported extension
    words[84] = (1 << 14); // Valid flag

    // Word 85: Command set enabled 1
    words[85] = words[82];

    // Word 86: Command set enabled 2
    words[86] = words[83];

    // Word 87: Command set/feature default
    words[87] = (1 << 14);

    // Word 88: Ultra DMA modes
    words[88] = 0x003F; // UDMA modes 0-5 supported

    // Word 93: Hardware reset result
    words[93] = 0x6001;

    // Words 100-103: 48-bit total addressable sectors
    words[100] = (uint16_t)(total_sectors & 0xFFFF);
    words[101] = (uint16_t)(total_sectors >> 16);
    words[102] = 0;
    words[103] = 0;

    // Word 255: Integrity checksum (A5 + sum of all bytes must = 0)
    uint8_t sum = 0;
    uint8_t* bytes = (uint8_t*)words;
    bytes[510] = 0xA5; // Signature
    for (int i = 0; i < 511; i++) sum += bytes[i];
    bytes[511] = (uint8_t)(0 - sum); // Checksum
}

// ═══════════════════════════════════════════════════════════════════════════
//  Init / Reset
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::Init(uint32_t disk_size_bytes) {
    storage = nullptr;
    storage_size = 0;
    total_sectors = 0;

    if (disk_size_bytes > 0) {
        storage_size = disk_size_bytes;
        if (storage_size > VDISK_MAX_SIZE) storage_size = VDISK_MAX_SIZE;
        total_sectors = storage_size / ATA_SECTOR_SIZE;

        storage = (uint8_t*)KernelHeap::Alloc(storage_size);
        if (storage) {
            memset(storage, 0, storage_size);
        } else {
            SerialLogger::Log("VDisk: FAILED to allocate ");
            SerialLogger::LogDec(storage_size);
            SerialLogger::Log(" bytes\r\n");
            total_sectors = 0;
            storage_size = 0;
        }
    }

    Reset();

    if (total_sectors > 0) {
        identify_data.Build(total_sectors, "KRNVSN0001  ",
                            "1.00    ", "Kurono Virtual Disk             ");
    }

    SerialLogger::Log("VDisk: ");
    SerialLogger::LogDec(total_sectors);
    SerialLogger::Log(" sectors (");
    SerialLogger::LogDec(storage_size / 1024);
    SerialLogger::Log(" KB)\r\n");
}

void VirtualDisk::Reset() {
    status        = ATA_SR_DRDY | ATA_SR_DSC; // Ready, seek complete
    error         = 0;
    features      = 0;
    sector_count  = 1;
    lba_lo        = 0;
    lba_mid       = 0;
    lba_hi        = 0;
    drive_head    = ATA_DH_FIXED;
    device_control= 0;
    selected_drive= 0;

    xfer_pos      = 0;
    xfer_len      = 0;
    xfer_sectors_left = 0;
    xfer_write    = false;
    xfer_lba      = 0;

    irq_pending   = false;
    irq_enabled   = true; // nIEN = 0 → interrupts enabled

    memset(xfer_buf, 0, ATA_XFER_BUF_SIZE);
}

void VirtualDisk::AttachStorage(uint8_t* disk_data, uint32_t disk_size) {
    storage = disk_data;
    storage_size = disk_size;
    total_sectors = disk_size / ATA_SECTOR_SIZE;

    identify_data.Build(total_sectors, "KRNVSN0001  ",
                        "1.00    ", "Kurono Virtual Disk             ");

    SerialLogger::Log("VDisk: Attached external storage, ");
    SerialLogger::LogDec(total_sectors);
    SerialLogger::Log(" sectors\r\n");
}

bool VirtualDisk::LoadImage(const uint8_t* data, uint32_t size,
                             uint32_t offset_bytes) {
    if (!storage) return false;
    if (offset_bytes + size > storage_size) return false;
    memcpy(storage + offset_bytes, data, size);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Port I/O — Write
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::WritePort(uint16_t port, uint32_t value, uint8_t size) {
    // Handle data port specially (16-bit transfers)
    if (port == ATA_PRI_DATA) {
        if (xfer_write && xfer_pos < xfer_len) {
            if (size >= 2) {
                // 16-bit word write
                if (xfer_pos + 1 < ATA_XFER_BUF_SIZE) {
                    xfer_buf[xfer_pos]     = (uint8_t)(value & 0xFF);
                    xfer_buf[xfer_pos + 1] = (uint8_t)((value >> 8) & 0xFF);
                    xfer_pos += 2;
                }
            } else {
                // 8-bit byte write
                xfer_buf[xfer_pos++] = (uint8_t)(value & 0xFF);
            }

            // Check if sector buffer is full
            if (xfer_pos >= ATA_SECTOR_SIZE) {
                WriteSectorFromBuffer(xfer_lba);
                xfer_sectors_left--;
                xfer_lba++;

                if (xfer_sectors_left > 0) {
                    // More sectors to write — reset buffer
                    xfer_pos = 0;
                    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
                } else {
                    // Transfer complete
                    xfer_pos = 0;
                    xfer_len = 0;
                    xfer_write = false;
                    status = ATA_SR_DRDY | ATA_SR_DSC;
                    RaiseIRQ();
                }
            }
        }
        return;
    }

    uint8_t val = (uint8_t)(value & 0xFF);

    switch (port) {
        case ATA_PRI_FEATURES:
            features = val;
            break;

        case ATA_PRI_COUNT:
            sector_count = val;
            break;

        case ATA_PRI_LBA_LO:
            lba_lo = val;
            break;

        case ATA_PRI_LBA_MID:
            lba_mid = val;
            break;

        case ATA_PRI_LBA_HI:
            lba_hi = val;
            break;

        case ATA_PRI_DRVHEAD:
            drive_head = val;
            selected_drive = (val & ATA_DH_DEV) ? 1 : 0;
            break;

        case ATA_PRI_COMMAND:
            if (selected_drive != 0) {
                // Only master drive exists — abort if slave selected
                status = 0; // No drive, cleared status
                return;
            }
            ExecuteCommand(val);
            break;

        case ATA_PRI_DEVCTRL:
            // Device Control Register
            if (val & ATA_DCR_SRST) {
                // Software Reset
                Reset();
                status = ATA_SR_DRDY | ATA_SR_DSC;
                error = 0x01; // Diagnostic code: no error
                sector_count = 0x01;
                lba_lo = 0x01;
                lba_mid = 0x00;
                lba_hi = 0x00;
                drive_head = 0x00;
            }
            irq_enabled = !(val & ATA_DCR_NIEN);
            device_control = val;
            break;

        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Port I/O — Read
// ═══════════════════════════════════════════════════════════════════════════

uint32_t VirtualDisk::ReadPort(uint16_t port, uint8_t size) {
    // Data port — 16-bit PIO read
    if (port == ATA_PRI_DATA) {
        uint32_t val = 0;
        if (!xfer_write && xfer_pos < xfer_len) {
            if (size >= 2) {
                val = xfer_buf[xfer_pos] |
                      ((uint32_t)xfer_buf[xfer_pos + 1] << 8);
                xfer_pos += 2;
            } else {
                val = xfer_buf[xfer_pos++];
            }

            // End of sector buffer?
            if (xfer_pos >= xfer_len) {
                if (xfer_sectors_left > 0) {
                    xfer_sectors_left--;
                    if (xfer_sectors_left > 0) {
                        xfer_lba++;
                        PrepareNextReadSector();
                    } else {
                        // Transfer complete
                        status = ATA_SR_DRDY | ATA_SR_DSC;
                        xfer_len = 0;
                    }
                } else {
                    status = ATA_SR_DRDY | ATA_SR_DSC;
                    xfer_len = 0;
                }
            }
        }
        return val;
    }

    switch (port) {
        case ATA_PRI_ERROR:
            return error;

        case ATA_PRI_COUNT:
            return sector_count;

        case ATA_PRI_LBA_LO:
            return lba_lo;

        case ATA_PRI_LBA_MID:
            return lba_mid;

        case ATA_PRI_LBA_HI:
            return lba_hi;

        case ATA_PRI_DRVHEAD:
            return drive_head;

        case ATA_PRI_STATUS:
            // Reading status clears pending interrupt
            irq_pending = false;
            if (selected_drive != 0 && !storage) {
                // Floating bus: no device
                return 0x00;
            }
            return status;

        case ATA_PRI_ALTSTAT:
            // Alt Status does NOT clear IRQ
            if (selected_drive != 0 && !storage) return 0x00;
            return status;

        default:
            return 0xFF;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command Execution
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::ExecuteCommand(uint8_t cmd) {
    if (!storage) {
        AbortCommand(ATA_ER_ABRT);
        return;
    }

    switch (cmd) {
        case ATA_CMD_IDENTIFY:
            DoIdentify();
            break;

        case ATA_CMD_READ_SECTORS:
            DoReadSectors();
            break;

        case ATA_CMD_WRITE_SECTORS:
            DoWriteSectors();
            break;

        case ATA_CMD_READ_VERIFY:
            DoReadVerify();
            break;

        case ATA_CMD_SET_FEATURES:
            DoSetFeatures();
            break;

        case ATA_CMD_CACHE_FLUSH:
            DoFlushCache();
            break;

        case ATA_CMD_INIT_DEV_PARAMS:
            DoInitDevParams();
            break;

        case ATA_CMD_RECALIBRATE:
            // No-op in emulation
            status = ATA_SR_DRDY | ATA_SR_DSC;
            error = 0;
            RaiseIRQ();
            break;

        case ATA_CMD_NOP:
            AbortCommand(ATA_ER_ABRT);
            break;

        case ATA_CMD_DEVICE_RESET:
            Reset();
            break;

        case ATA_CMD_STANDBY_IMM:
        case ATA_CMD_IDLE_IMM:
        case ATA_CMD_STANDBY:
        case ATA_CMD_IDLE:
        case ATA_CMD_SLEEP:
            // Power management — just report success
            status = ATA_SR_DRDY | ATA_SR_DSC;
            error = 0;
            RaiseIRQ();
            break;

        case ATA_CMD_CHECK_POWER:
            // Report active/idle
            sector_count = 0xFF; // Active or idle
            status = ATA_SR_DRDY | ATA_SR_DSC;
            error = 0;
            RaiseIRQ();
            break;

        case ATA_CMD_READ_DMA:
        case ATA_CMD_WRITE_DMA:
            // DMA not supported — abort
            AbortCommand(ATA_ER_ABRT);
            break;

        default:
            SerialLogger::Log("VDisk: Unknown command 0x");
            SerialLogger::LogHex(cmd);
            SerialLogger::Log("\r\n");
            AbortCommand(ATA_ER_ABRT);
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  IDENTIFY DEVICE — report disk identity to guest
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::DoIdentify() {
    memcpy(xfer_buf, identify_data.words, ATA_SECTOR_SIZE);
    xfer_pos = 0;
    xfer_len = ATA_SECTOR_SIZE;
    xfer_write = false;
    xfer_sectors_left = 0;
    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    error = 0;
    RaiseIRQ();
}

// ═══════════════════════════════════════════════════════════════════════════
//  READ SECTORS (0x20) — PIO 28-bit LBA
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::DoReadSectors() {
    uint32_t lba = GetCurrentLBA();
    uint32_t count = sector_count;
    if (count == 0) count = 256; // 0 means 256 sectors

    if (lba + count > total_sectors) {
        AbortCommand(ATA_ER_IDNF);
        return;
    }

    xfer_lba = lba;
    xfer_sectors_left = count;
    xfer_write = false;
    error = 0;

    // Read first sector into buffer
    PrepareNextReadSector();
}

void VirtualDisk::PrepareNextReadSector() {
    if (!ReadSectorToBuffer(xfer_lba)) {
        AbortCommand(ATA_ER_UNC);
        return;
    }
    xfer_pos = 0;
    xfer_len = ATA_SECTOR_SIZE;
    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    RaiseIRQ();
}

// ═══════════════════════════════════════════════════════════════════════════
//  WRITE SECTORS (0x30) — PIO 28-bit LBA
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::DoWriteSectors() {
    uint32_t lba = GetCurrentLBA();
    uint32_t count = sector_count;
    if (count == 0) count = 256;

    if (lba + count > total_sectors) {
        AbortCommand(ATA_ER_IDNF);
        return;
    }

    xfer_lba = lba;
    xfer_sectors_left = count;
    xfer_pos = 0;
    xfer_len = ATA_SECTOR_SIZE;
    xfer_write = true;
    error = 0;

    // DRQ: ready for host to write data
    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    // No IRQ until first sector is written
}

// ═══════════════════════════════════════════════════════════════════════════
//  READ VERIFY (0x40) — verify sectors are readable, no data transfer
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::DoReadVerify() {
    uint32_t lba = GetCurrentLBA();
    uint32_t count = sector_count;
    if (count == 0) count = 256;

    if (lba + count > total_sectors) {
        AbortCommand(ATA_ER_IDNF);
        return;
    }

    // All sectors "verified" instantly
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Other commands
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::DoSetFeatures() {
    // Accept all feature requests silently
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

void VirtualDisk::DoFlushCache() {
    // RAM-backed — nothing to flush
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

void VirtualDisk::DoInitDevParams() {
    // Legacy CHS parameter initialization — accept and continue
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

// ═══════════════════════════════════════════════════════════════════════════
//  LBA computation
// ═══════════════════════════════════════════════════════════════════════════

uint32_t VirtualDisk::GetCurrentLBA() const {
    if (drive_head & ATA_DH_LBA) {
        // LBA mode
        return (uint32_t)lba_lo |
               ((uint32_t)lba_mid << 8) |
               ((uint32_t)lba_hi << 16) |
               ((uint32_t)(drive_head & 0x0F) << 24);
    } else {
        // CHS mode — convert to LBA
        uint32_t cylinder = ((uint32_t)lba_hi << 8) | lba_mid;
        uint32_t head = drive_head & 0x0F;
        uint32_t sector = lba_lo; // 1-based in CHS
        if (sector == 0) sector = 1;
        // LBA = (cylinder * heads + head) * sectors + (sector - 1)
        // Assume 16 heads, 63 sectors per track
        return (cylinder * 16 + head) * 63 + (sector - 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sector I/O
// ═══════════════════════════════════════════════════════════════════════════

bool VirtualDisk::ReadSectorToBuffer(uint32_t lba) {
    if (!storage || lba >= total_sectors) return false;
    uint32_t offset = lba * ATA_SECTOR_SIZE;
    memcpy(xfer_buf, storage + offset, ATA_SECTOR_SIZE);
    return true;
}

bool VirtualDisk::WriteSectorFromBuffer(uint32_t lba) {
    if (!storage || lba >= total_sectors) return false;
    uint32_t offset = lba * ATA_SECTOR_SIZE;
    memcpy(storage + offset, xfer_buf, ATA_SECTOR_SIZE);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  IRQ Management
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::RaiseIRQ() {
    if (irq_enabled) {
        irq_pending = true;
    }
}

bool VirtualDisk::HasPendingIRQ() const {
    return irq_pending && irq_enabled;
}

void VirtualDisk::ClearIRQ() {
    irq_pending = false;
}

void VirtualDisk::AbortCommand(uint8_t err_bits) {
    error = err_bits;
    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_ERR;
    xfer_pos = 0;
    xfer_len = 0;
    xfer_write = false;
    xfer_sectors_left = 0;
    RaiseIRQ();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debug
// ═══════════════════════════════════════════════════════════════════════════

void VirtualDisk::DumpState() {
    SerialLogger::Log("=== VDisk State ===\r\n");

    SerialLogger::Log("  Storage: ");
    if (storage) {
        SerialLogger::LogDec(total_sectors);
        SerialLogger::Log(" sectors (");
        SerialLogger::LogDec(storage_size / 1024);
        SerialLogger::Log(" KB)\r\n");
    } else {
        SerialLogger::Log("not attached\r\n");
    }

    SerialLogger::Log("  Status=");   SerialLogger::LogHex(status);
    SerialLogger::Log(" Error=");     SerialLogger::LogHex(error);
    SerialLogger::Log(" DrvHead=");   SerialLogger::LogHex(drive_head);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  LBA=");
    SerialLogger::LogHex(GetCurrentLBA());
    SerialLogger::Log(" SecCount=");
    SerialLogger::LogDec(sector_count);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Xfer: pos=");
    SerialLogger::LogDec(xfer_pos);
    SerialLogger::Log(" len=");
    SerialLogger::LogDec(xfer_len);
    SerialLogger::Log(" sectors_left=");
    SerialLogger::LogDec(xfer_sectors_left);
    SerialLogger::Log(" write=");
    SerialLogger::Log(xfer_write ? "yes" : "no");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  IRQ: pending=");
    SerialLogger::Log(irq_pending ? "yes" : "no");
    SerialLogger::Log(" enabled=");
    SerialLogger::Log(irq_enabled ? "yes" : "no");
    SerialLogger::Log("\r\n");
}
