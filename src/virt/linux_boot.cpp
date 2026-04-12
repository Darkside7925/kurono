//  kurono os  -  linux boot protocol implementation
//  loads a linux bzimage/vmlinuz into guest vm memory following the
//  official x86 linux boot protocol.
//
//  boot protocol summary:
//    1. the first (1 + setup_sects) × 512 bytes = real-mode setup code
//    2. everything after that = protected-mode kernel (compressed)
//    3. setup code is loaded to 0x10000 (or wherever the loader wants)
//    4. protected-mode kernel loaded to 0x100000 (1 mb)
//    5. boot_params (4 kb) at 0x10000 is filled by the loader
//    6. command line placed at linux_cmdline_addr
//    7. entry is to 32-bit protected mode at code32_start
//
//  reference: documentation/x86/boot.rst in linux kernel source
#include "linux_boot.h"
#include "guest_mem.h"
#include "../kernel/types.h"
#include "../drivers/serial.h"

LinuxImageInfo LinuxBootLoader::image_info;
char           LinuxBootLoader::cmdline_buf[LINUX_CMDLINE_MAX];
bool           LinuxBootLoader::kernel_loaded  = false;
bool           LinuxBootLoader::initrd_loaded  = false;
uint32_t       LinuxBootLoader::initrd_guest_addr = 0;
uint32_t       LinuxBootLoader::initrd_size    = 0;

//  parseimage  -  validate and extract information from a bzimage

bool LinuxBootLoader::ParseImage(const uint8_t* image_data, uint32_t image_size,
                                  LinuxImageInfo& info) {
    memset(&info, 0, sizeof(info));

    // minimum size: at least 1 kb for boot sector + setup header
    if (!image_data || image_size < 1024) {
        SerialLogger::Log("LinuxBoot: Image too small\r\n");
        return false;
    }

    // check boot flag (0xaa55 at offset 510-511)
    uint16_t boot_flag = *(uint16_t*)(image_data + 0x01FE);
    if (boot_flag != LINUX_BOOT_FLAG) {
        SerialLogger::Log("LinuxBoot: Missing boot flag (0xAA55)\r\n");
        return false;
    }

    // read setup header from offset 0x01f1
    const LinuxSetupHeader* hdr =
        (const LinuxSetupHeader*)(image_data + LINUX_HEADER_OFFSET);

    // check for "hdrs" magic at offset 0x0202
    if (hdr->header != LINUX_HDRS_MAGIC) {
        SerialLogger::Log("LinuxBoot: Missing HdrS magic (old kernel?)\r\n");
        // could be a very old kernel without the setup header
        // we require at least protocol 2.00
        return false;
    }

    info.valid = true;
    info.protocol_version = hdr->version;

    // setup_sects: number of 512-byte setup sectors after boot sector
    // if 0, assume 4 (old convention)
    uint8_t setup_sects = hdr->setup_sects;
    if (setup_sects == 0) setup_sects = 4;

    // setup size = (1 + setup_sects) * 512
    // the "1" is the boot sector itself
    info.setup_size = (1 + (uint32_t)setup_sects) * LINUX_BOOT_SECTOR;

    // protected-mode kernel starts after setup code
    if (info.setup_size >= image_size) {
        SerialLogger::Log("LinuxBoot: setup_size >= image_size\r\n");
        return false;
    }
    info.kernel_size = image_size - info.setup_size;
    info.total_image_size = image_size;

    // code32_start: entry point (protocol ≥ 2.00)
    info.code32_start = hdr->code32_start;
    if (info.code32_start == 0) info.code32_start = LINUX_KERNEL_ADDR;

    // loadflags
    info.loaded_high = (hdr->loadflags & LOADFLAG_LOADED_HIGH) != 0;
    info.can_use_heap = (hdr->loadflags & LOADFLAG_CAN_USE_HEAP) != 0;

    // command line max size (protocol ≥ 2.06)
    if (info.protocol_version >= 0x0206) {
        info.cmdline_max = hdr->cmdline_size;
    } else {
        info.cmdline_max = 255;
    }
    if (info.cmdline_max > LINUX_CMDLINE_MAX - 1) {
        info.cmdline_max = LINUX_CMDLINE_MAX - 1;
    }

    // initrd max address (protocol ≥ 2.03)
    if (info.protocol_version >= 0x0203) {
        info.initrd_addr_max = hdr->initrd_addr_max;
    } else {
        info.initrd_addr_max = 0x37FFFFFF;
    }

    // relocatable kernel (protocol ≥ 2.05)
    if (info.protocol_version >= 0x0205) {
        info.relocatable = (hdr->relocatable_kernel != 0);
        info.kernel_alignment = hdr->kernel_alignment;
    } else {
        info.relocatable = false;
        info.kernel_alignment = 0x1000; // 4 kb default
    }

    // init size (protocol ≥ 2.10)
    if (info.protocol_version >= 0x020A) {
        info.init_size = hdr->init_size;
    } else {
        info.init_size = info.kernel_size;
    }

    SerialLogger::Log("LinuxBoot: Valid bzImage\r\n");
    SerialLogger::Log("  Protocol: ");
    SerialLogger::LogHex(info.protocol_version);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("  Setup: ");
    SerialLogger::LogDec(info.setup_size);
    SerialLogger::Log(" bytes, Kernel: ");
    SerialLogger::LogDec(info.kernel_size);
    SerialLogger::Log(" bytes\r\n");
    SerialLogger::Log("  Entry: 0x");
    SerialLogger::LogHex(info.code32_start);
    SerialLogger::Log("\r\n");

    return true;
}

//  loadkernel  -  load bzimage into guest physical memory

bool LinuxBootLoader::LoadKernel(const uint8_t* image_data, uint32_t image_size,
                                  const char* cmdline) {
    // parse the image first
    if (!ParseImage(image_data, image_size, image_info)) {
        return false;
    }

    // store command line
    memset(cmdline_buf, 0, LINUX_CMDLINE_MAX);
    if (cmdline) {
        int i = 0;
        while (cmdline[i] && i < (int)LINUX_CMDLINE_MAX - 1) {
            cmdline_buf[i] = cmdline[i];
            i++;
        }
        cmdline_buf[i] = '\0';
    }

    // boot_params is a 4096-byte struct; the setup header occupies
    // a portion of it starting at offset 0x1f1.
    // we zero the full 4096 bytes first, then copy the setup code.

    SerialLogger::Log("LinuxBoot: Loading setup code to 0x");
    SerialLogger::LogHex(LINUX_BOOT_PARAMS_ADDR);
    SerialLogger::Log("\r\n");

    // zero boot_params (4 kb)
    if (!GuestMemoryManager::ZeroGuestPhys(LINUX_BOOT_PARAMS_ADDR, 4096)) {
        SerialLogger::Log("LinuxBoot: Failed to zero boot_params\r\n");
        return false;
    }

    // copy the first sector + setup sectors (the real-mode code)
    // this includes the setup header at the correct offsets.
    uint32_t copy_size = image_info.setup_size;
    if (copy_size > 4096) copy_size = 4096; // boot_params is only 4 kb
    if (!GuestMemoryManager::WriteGuestPhys(LINUX_BOOT_PARAMS_ADDR,
                                             image_data, copy_size)) {
        SerialLogger::Log("LinuxBoot: Failed to copy setup code\r\n");
        return false;
    }

    const uint8_t* kernel_data = image_data + image_info.setup_size;
    uint32_t kernel_size = image_info.kernel_size;

    SerialLogger::Log("LinuxBoot: Loading kernel (");
    SerialLogger::LogDec(kernel_size);
    SerialLogger::Log(" bytes) to 0x");
    SerialLogger::LogHex(LINUX_KERNEL_ADDR);
    SerialLogger::Log("\r\n");

    if (!GuestMemoryManager::WriteGuestPhys(LINUX_KERNEL_ADDR,
                                             kernel_data, kernel_size)) {
        SerialLogger::Log("LinuxBoot: Failed to copy kernel\r\n");
        return false;
    }

    SerialLogger::Log("LinuxBoot: Command line: '");
    SerialLogger::Log(cmdline_buf);
    SerialLogger::Log("'\r\n");

    uint32_t cmdline_len = strlen(cmdline_buf) + 1;
    if (!GuestMemoryManager::WriteGuestPhys(LINUX_CMDLINE_ADDR,
                                             cmdline_buf, cmdline_len)) {
        SerialLogger::Log("LinuxBoot: Failed to write command line\r\n");
        return false;
    }

    // we need to modify several fields in the setup header that's now
    // in guest memory at linux_boot_params_addr + 0x1f1.
    uint8_t* bp_host = GuestMemoryManager::GuestPhysToHost(LINUX_BOOT_PARAMS_ADDR);
    if (!bp_host) {
        SerialLogger::Log("LinuxBoot: Cannot map boot_params\r\n");
        return false;
    }

    LinuxSetupHeader* hdr = (LinuxSetupHeader*)(bp_host + LINUX_HEADER_OFFSET);

    // set loader type
    hdr->type_of_loader = BOOTLOADER_ID_KURONO;
    hdr->ext_loader_ver = 0;
    hdr->ext_loader_type = 0;

    // set loadflags: loaded high, can use heap
    hdr->loadflags |= LOADFLAG_LOADED_HIGH | LOADFLAG_CAN_USE_HEAP;
    hdr->loadflags &= ~LOADFLAG_QUIET; // not quiet  -  we want console output

    // heap end pointer (relative to setup start, 0x10000)
    // use the area just below the command line
    hdr->heap_end_ptr = 0xFE00; // near end of setup segment

    // command line pointer
    hdr->cmd_line_ptr = LINUX_CMDLINE_ADDR;

    // protected-mode code start
    hdr->code32_start = LINUX_KERNEL_ADDR;

    // video mode (vga text 80x25)
    hdr->vid_mode = 0xFFFF; // normal mode (let kernel decide)

    kernel_loaded = true;
    SerialLogger::Log("LinuxBoot: Kernel loaded successfully\r\n");
    return true;
}

//  loadinitrd  -  load initial ramdisk into guest memory

bool LinuxBootLoader::LoadInitrd(const uint8_t* initrd_data,
                                  uint32_t initrd_sz) {
    if (!kernel_loaded) {
        SerialLogger::Log("LinuxBoot: Cannot load initrd before kernel\r\n");
        return false;
    }

    if (!initrd_data || initrd_sz == 0) {
        SerialLogger::Log("LinuxBoot: No initrd to load\r\n");
        return true; // not an error  -  initrd is optional
    }

    // place initrd at linux_initrd_addr
    uint32_t addr = LINUX_INITRD_ADDR;
    if (addr + initrd_sz > GuestMemoryManager::GetTotalRAM()) {
        // if it doesn't fit at 8 mb, try right after kernel
        addr = LINUX_KERNEL_ADDR + image_info.kernel_size;
        addr = (addr + 0xFFF) & ~0xFFF; // page-align
    }

    SerialLogger::Log("LinuxBoot: Loading initrd (");
    SerialLogger::LogDec(initrd_sz);
    SerialLogger::Log(" bytes) to 0x");
    SerialLogger::LogHex(addr);
    SerialLogger::Log("\r\n");

    if (!GuestMemoryManager::WriteGuestPhys(addr, initrd_data, initrd_sz)) {
        SerialLogger::Log("LinuxBoot: Failed to write initrd\r\n");
        return false;
    }

    // patch boot_params
    uint8_t* bp_host = GuestMemoryManager::GuestPhysToHost(LINUX_BOOT_PARAMS_ADDR);
    if (bp_host) {
        LinuxSetupHeader* hdr = (LinuxSetupHeader*)(bp_host + LINUX_HEADER_OFFSET);
        hdr->ramdisk_image = addr;
        hdr->ramdisk_size = initrd_sz;
    }

    initrd_guest_addr = addr;
    initrd_size = initrd_sz;
    initrd_loaded = true;

    return true;
}

//  setupbootparams  -  fill remaining boot parameter fields

bool LinuxBootLoader::SetupBootParams() {
    if (!kernel_loaded) return false;

    uint8_t* bp_host = GuestMemoryManager::GuestPhysToHost(LINUX_BOOT_PARAMS_ADDR);
    if (!bp_host) return false;

    // fill e820 memory map
    FillE820(bp_host);

    // fill screen info (minimal  -  text mode)
    FillScreenInfo(bp_host);

    SerialLogger::Log("LinuxBoot: Boot params configured\r\n");
    return true;
}

//  fillscreeninfo  -  set up minimal vga text mode info

void LinuxBootLoader::FillScreenInfo(uint8_t* boot_params) {
    // struct screen_info at offset 0x000 in boot_params
    // we set up basic 80x25 text mode
    uint8_t* si = boot_params + BP_SCREEN_INFO;

    si[0x00] = 80;  // orig_x (cursor column)
    si[0x01] = 0;   // orig_y (cursor row)
    si[0x02] = 0;   // ext_mem_k low
    si[0x03] = 0;   // ext_mem_k high (set below)
    si[0x04] = 80;  // orig_video_cols
    si[0x06] = 0;   // orig_video_mode
    si[0x07] = 25;  // orig_video_lines
    si[0x0A] = 2;   // orig_video_points (character height)
    si[0x0F] = 0x22;// orig_video_isvga (vga)

    // ext_mem_k: extended memory in kb (above 1 mb, up to 64 mb)
    uint32_t ext_mem_kb = GuestMemoryManager::GetHighRAMSize() / 1024;
    if (ext_mem_kb > 0xFFFF) ext_mem_kb = 0xFFFF;
    si[0x02] = (uint8_t)(ext_mem_kb & 0xFF);
    si[0x03] = (uint8_t)((ext_mem_kb >> 8) & 0xFF);

    // alt_mem_k at offset 0x1e0 in boot_params (not in screen_info)
    // this is used when ext_mem_k is 0xffff
    uint32_t alt_kb = GuestMemoryManager::GetHighRAMSize() / 1024;
    boot_params[0x1E0] = (uint8_t)(alt_kb & 0xFF);
    boot_params[0x1E1] = (uint8_t)((alt_kb >> 8) & 0xFF);
    boot_params[0x1E2] = (uint8_t)((alt_kb >> 16) & 0xFF);
    boot_params[0x1E3] = (uint8_t)((alt_kb >> 24) & 0xFF);
}

//  fille820  -  write e820 memory map into boot_params

void LinuxBootLoader::FillE820(uint8_t* boot_params) {
    int count = GuestMemoryManager::GetE820Count();
    const E820Entry* table = GuestMemoryManager::GetE820Table();

    if (count > 128) count = 128; // linux limit

    // number of e820 entries at offset 0x1e8
    boot_params[BP_E820_ENTRIES] = (uint8_t)count;

    // e820 entries start at offset 0x2d0, each 20 bytes
    uint8_t* dest = boot_params + BP_E820_TABLE;
    for (int i = 0; i < count; i++) {
        // each entry: base(8) + length(8) + type(4) = 20 bytes
        memcpy(dest, &table[i].base, 8);
        memcpy(dest + 8, &table[i].length, 8);
        memcpy(dest + 16, &table[i].type, 4);
        dest += BP_E820_ENTRY_SIZE;
    }

    SerialLogger::Log("LinuxBoot: Wrote ");
    SerialLogger::LogDec(count);
    SerialLogger::Log(" E820 entries to boot_params\r\n");
}

//  getentrypoint  -  return the address to jump to for kernel start

uint32_t LinuxBootLoader::GetEntryPoint() {
    if (!kernel_loaded) return 0;
    return image_info.code32_start;
}

//  debug

void LinuxBootLoader::DumpImageInfo() {
    if (!image_info.valid) {
        SerialLogger::Log("LinuxBoot: No valid image loaded\r\n");
        return;
    }

    SerialLogger::Log("=== Linux Image Info ===\r\n");
    SerialLogger::Log("  Protocol version: ");
    SerialLogger::LogHex(image_info.protocol_version);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Setup size:   ");
    SerialLogger::LogDec(image_info.setup_size);
    SerialLogger::Log(" bytes\r\n");

    SerialLogger::Log("  Kernel size:  ");
    SerialLogger::LogDec(image_info.kernel_size);
    SerialLogger::Log(" bytes\r\n");

    SerialLogger::Log("  Entry point:  0x");
    SerialLogger::LogHex(image_info.code32_start);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Loaded high:  ");
    SerialLogger::Log(image_info.loaded_high ? "yes" : "no");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Relocatable:  ");
    SerialLogger::Log(image_info.relocatable ? "yes" : "no");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Cmdline max:  ");
    SerialLogger::LogDec(image_info.cmdline_max);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Init size:    ");
    SerialLogger::LogDec(image_info.init_size);
    SerialLogger::Log(" bytes\r\n");

    if (initrd_loaded) {
        SerialLogger::Log("  Initrd at:    0x");
        SerialLogger::LogHex(initrd_guest_addr);
        SerialLogger::Log(" (");
        SerialLogger::LogDec(initrd_size);
        SerialLogger::Log(" bytes)\r\n");
    }

    SerialLogger::Log("  Command line: '");
    SerialLogger::Log(cmdline_buf);
    SerialLogger::Log("'\r\n");
}
