//  kurono os  -  virtual ide/ata disk controller implementation
//  full pio-mode ata controller with ram-backed disk image.
//
//  supports:
//    - identify device (0xec)
//    - read sectors (0x20)  -  pio, 28-bit lba
//    - write sectors (0x30)  -  pio, 28-bit lba
//    - read verify (0x40)
//    - cache flush (0xe7)
//    - set features (0xef)
//    - initialize device parameters (0x91)
//    - power management commands (standby/idle/sleep)
//    - software reset (srst)
//    - multi-sector transfers
//    - irq 14 generation
//
//  reference: ata/atapi-7, osdev ata pio mode
#include "vdisk.h"
#include "../drivers/serial.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"

//  identify device  -  build the 512-byte identification data

static void SetATAString(uint16_t* words, int start_word, int num_words,
                          const char* str) {
    // ata strings are byte-swapped within each word
    for (int i = 0; i < num_words; i++) {
        char c1 = *str ? *str++ : ' ';
        char c2 = *str ? *str++ : ' ';
        words[start_word + i] = ((uint16_t)(uint8_t)c1 << 8) | (uint8_t)c2;
    }
}

void ATAIdentify::Build(uint32_t total_sectors, const char* serial,
                         const char* firmware, const char* model) {
    memset(words, 0, sizeof(words));

    // word 0: general config
    //   bit 15: 0 = ata, bit 6: fixed drive
    words[0] = 0x0040; // fixed disk, non-removable

    // word 1: number of logical cylinders (obsolete)
    uint32_t cyls = total_sectors / (16 * 63); // chs: 16 heads, 63 sectors
    if (cyls > 16383) cyls = 16383;
    words[1] = (uint16_t)cyls;

    // word 3: number of logical heads
    words[3] = 16;

    // word 6: number of logical sectors per track
    words[6] = 63;

    // words 10-19: serial number (20 ascii characters)
    SetATAString(words, 10, 10, serial);

    // word 22: obsolete (vendor-specific), ecc bytes
    words[22] = 4;

    // words 23-26: firmware revision (8 ascii characters)
    SetATAString(words, 23, 4, firmware);

    // words 27-46: model number (40 ascii characters)
    SetATAString(words, 27, 20, model);

    // word 47: max sectors per interrupt (read/write multiple)
    words[47] = 0x8010; // 16 sectors max

    // word 49: capabilities
    //   bit 8: dma supported, bit 9: lba supported
    //   bit 11: iordy supported, bit 13: standby timer
    words[49] = (1 << 9) | (1 << 11) | (1 << 13); // lba + iordy + standby

    // word 50: additional capabilities
    words[50] = 0x4001; // standby timer value minimum

    // word 51: pio data transfer cycle timing mode
    words[51] = 0x0200; // pio mode 2

    // word 53: field validity
    //   bit 0: words 54-58 valid
    //   bit 1: words 64-70 valid
    //   bit 2: word 88 valid
    words[53] = 0x0007;

    // words 54-56: current chs (same as logical)
    words[54] = words[1]; // cylinders
    words[55] = 16;       // heads
    words[56] = 63;       // sectors

    // words 57-58: current capacity in sectors (chs)
    uint32_t chs_sectors = (uint32_t)words[54] * 16 * 63;
    words[57] = (uint16_t)(chs_sectors & 0xFFFF);
    words[58] = (uint16_t)(chs_sectors >> 16);

    // word 59: multiple sector setting
    words[59] = 0x0110; // multiple sector valid, current = 16

    // words 60-61: total addressable sectors (28-bit lba)
    words[60] = (uint16_t)(total_sectors & 0xFFFF);
    words[61] = (uint16_t)(total_sectors >> 16);

    // word 63: multiword dma modes
    words[63] = 0x0007; // mdma modes 0, 1, 2 supported

    // word 64: pio modes supported
    words[64] = 0x0003; // pio modes 3, 4 supported

    // word 65: minimum multiword dma transfer cycle time
    words[65] = 120; // 120ns

    // word 66: recommended multiword dma transfer cycle time
    words[66] = 120;

    // word 67: minimum pio transfer cycle time without iordy
    words[67] = 120;

    // word 68: minimum pio transfer cycle time with iordy
    words[68] = 120;

    // word 80: major version (ata-6)
    words[80] = (1 << 6); // ata-6

    // word 81: minor version
    words[81] = 0x0019; // ata/atapi-6 t13 1410d rev 2

    // word 82: command set supported 1
    //   bit 0: smart, bit 5: write cache, bit 10: flush cache
    //   bit 12: nop, bit 14: set features required
    words[82] = (1 << 5) | (1 << 10) | (1 << 14);

    // word 83: command set supported 2
    //   bit 10: 48-bit lba (we don't support, keep 0)
    //   bit 14: must be 1
    words[83] = (1 << 14);

    // word 84: command set/feature supported extension
    words[84] = (1 << 14); // valid flag

    // word 85: command set enabled 1
    words[85] = words[82];

    // word 86: command set enabled 2
    words[86] = words[83];

    // word 87: command set/feature default
    words[87] = (1 << 14);

    // word 88: ultra dma modes
    words[88] = 0x003F; // udma modes 0-5 supported

    // word 93: hardware reset result
    words[93] = 0x6001;

    // words 100-103: 48-bit total addressable sectors
    words[100] = (uint16_t)(total_sectors & 0xFFFF);
    words[101] = (uint16_t)(total_sectors >> 16);
    words[102] = 0;
    words[103] = 0;

    // word 255: integrity checksum (a5 + sum of all bytes must = 0)
    uint8_t sum = 0;
    uint8_t* bytes = (uint8_t*)words;
    bytes[510] = 0xA5; // signature
    for (int i = 0; i < 511; i++) sum += bytes[i];
    bytes[511] = (uint8_t)(0 - sum); // checksum
}

//  init / reset

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
    status        = ATA_SR_DRDY | ATA_SR_DSC; // ready, seek complete
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
    irq_enabled   = true; // nien = 0 → interrupts enabled

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

//  port i/o  -  write

void VirtualDisk::WritePort(uint16_t port, uint32_t value, uint8_t size) {
    // handle data port specially (16-bit transfers)
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

            // check if sector buffer is full
            if (xfer_pos >= ATA_SECTOR_SIZE) {
                WriteSectorFromBuffer(xfer_lba);
                xfer_sectors_left--;
                xfer_lba++;

                if (xfer_sectors_left > 0) {
                    // more sectors to write  -  reset buffer
                    xfer_pos = 0;
                    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
                } else {
                    // transfer complete
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
                // only master drive exists  -  abort if slave selected
                status = 0; // no drive, cleared status
                return;
            }
            ExecuteCommand(val);
            break;

        case ATA_PRI_DEVCTRL:
            // device control register
            if (val & ATA_DCR_SRST) {
                // software reset
                Reset();
                status = ATA_SR_DRDY | ATA_SR_DSC;
                error = 0x01; // diagnostic code: no error
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

//  port i/o  -  read

uint32_t VirtualDisk::ReadPort(uint16_t port, uint8_t size) {
    // data port  -  16-bit pio read
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

            // end of sector buffer?
            if (xfer_pos >= xfer_len) {
                if (xfer_sectors_left > 0) {
                    xfer_sectors_left--;
                    if (xfer_sectors_left > 0) {
                        xfer_lba++;
                        PrepareNextReadSector();
                    } else {
                        // transfer complete
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
            // reading status clears pending interrupt
            irq_pending = false;
            if (selected_drive != 0 && !storage) {
                // floating bus: no device
                return 0x00;
            }
            return status;

        case ATA_PRI_ALTSTAT:
            // alt status does not clear irq
            if (selected_drive != 0 && !storage) return 0x00;
            return status;

        default:
            return 0xFF;
    }
}

//  command execution

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
            // no-op in emulation
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
            // power management  -  just report success
            status = ATA_SR_DRDY | ATA_SR_DSC;
            error = 0;
            RaiseIRQ();
            break;

        case ATA_CMD_CHECK_POWER:
            // report active/idle
            sector_count = 0xFF; // active or idle
            status = ATA_SR_DRDY | ATA_SR_DSC;
            error = 0;
            RaiseIRQ();
            break;

        case ATA_CMD_READ_DMA:
        case ATA_CMD_WRITE_DMA:
            // dma not supported  -  abort
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

//  identify device  -  report disk identity to guest

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

//  read sectors (0x20)  -  pio 28-bit lba

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

    // read first sector into buffer
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

//  write sectors (0x30)  -  pio 28-bit lba

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

    // drq: ready for host to write data
    status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    // no irq until first sector is written
}

//  read verify (0x40)  -  verify sectors are readable, no data transfer

void VirtualDisk::DoReadVerify() {
    uint32_t lba = GetCurrentLBA();
    uint32_t count = sector_count;
    if (count == 0) count = 256;

    if (lba + count > total_sectors) {
        AbortCommand(ATA_ER_IDNF);
        return;
    }

    // all sectors "verified" instantly
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

//  other commands

void VirtualDisk::DoSetFeatures() {
    // accept all feature requests silently
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

void VirtualDisk::DoFlushCache() {
    // ram-backed  -  nothing to flush
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

void VirtualDisk::DoInitDevParams() {
    // legacy chs parameter initialization  -  accept and continue
    status = ATA_SR_DRDY | ATA_SR_DSC;
    error = 0;
    RaiseIRQ();
}

//  lba computation

uint32_t VirtualDisk::GetCurrentLBA() const {
    if (drive_head & ATA_DH_LBA) {
        // lba mode
        return (uint32_t)lba_lo |
               ((uint32_t)lba_mid << 8) |
               ((uint32_t)lba_hi << 16) |
               ((uint32_t)(drive_head & 0x0F) << 24);
    } else {
        // chs mode  -  convert to lba
        uint32_t cylinder = ((uint32_t)lba_hi << 8) | lba_mid;
        uint32_t head = drive_head & 0x0F;
        uint32_t sector = lba_lo; // 1-based in chs
        if (sector == 0) sector = 1;
        // lba = (cylinder * heads + head) * sectors + (sector - 1)
        // assume 16 heads, 63 sectors per track
        return (cylinder * 16 + head) * 63 + (sector - 1);
    }
}

//  sector i/o

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

//  irq management

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

//  debug

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
