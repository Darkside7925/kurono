//  kurono os - linux boot protocol implementation
//  implements the x86 linux boot protocol for loading a bzimage/vmlinuz
//  into a virtual machine.
//
//  reference: linux kernel documentation/x86/boot.rst
//             (formerly documentation/x86/boot.txt)
//  protocol version: 2.15 (kernel ≥ 5.0)
//
//  boot process:
//    1. parse real-mode setup header from bzimage offset 0x01f1
//    2. load real-mode code to 0x10000 (or below 0xa0000)
//    3. load protected-mode kernel to 0x100000 (1 mb)
//    4. fill in boot parameters (struct boot_params at 0x10000)
//    5. set up e820, command line, initrd (if applicable)
//    6. enter protected-mode kernel at 0x100000
#pragma once
#include <stdint.h>
#include <stddef.h>

// this matches the linux kernel's struct setup_header (arch/x86/include/uapi/asm/bootparam.h)
struct __attribute__((packed)) LinuxSetupHeader {
    uint8_t  setup_sects;       // 0x01f1: number of setup sectors
    uint16_t root_flags;        // 0x01f2: if nonzero, root is mounted readonly
    uint32_t syssize;           // 0x01f4: protected-mode code size in 16-byte paras
    uint16_t ram_size;          // 0x01f8: obsolete
    uint16_t vid_mode;          // 0x01fa: video mode
    uint16_t root_dev;          // 0x01fc: default root device
    uint16_t boot_flag;         // 0x01fe: 0xaa55 magic
    uint16_t jump;              // 0x0200: jump instruction
    uint32_t header;            // 0x0202: "hdrs" magic (0x53726448)
    uint16_t version;           // 0x0206: boot protocol version
    uint32_t realmode_swtch;    // 0x0208: obsolete
    uint16_t start_sys_seg;     // 0x020c: obsolete
    uint16_t kernel_version;    // 0x020e: offset to kernel version string
    uint8_t  type_of_loader;    // 0x0210: boot loader id
    uint8_t  loadflags;         // 0x0211: various flags
    uint16_t setup_move_size;   // 0x0212: size of setup move
    uint32_t code32_start;      // 0x0214: start address of protected-mode code
    uint32_t ramdisk_image;     // 0x0218: initrd load address
    uint32_t ramdisk_size;      // 0x021c: initrd size
    uint32_t bootsect_kludge;   // 0x0220: obsolete
    uint16_t heap_end_ptr;      // 0x0224: setup heap end pointer
    uint8_t  ext_loader_ver;    // 0x0226: extended loader version
    uint8_t  ext_loader_type;   // 0x0227: extended loader type
    uint32_t cmd_line_ptr;      // 0x0228: pointer to kernel command line
    uint32_t initrd_addr_max;   // 0x022c: maximum initrd address
    uint32_t kernel_alignment;  // 0x0230: alignment for kernel
    uint8_t  relocatable_kernel;// 0x0234: is kernel relocatable?
    uint8_t  min_alignment;     // 0x0235: min alignment (2^n)
    uint16_t xloadflags;        // 0x0236: extended load flags
    uint32_t cmdline_size;      // 0x0238: max command line size
    uint32_t hardware_subarch;  // 0x023c: hardware subarchitecture
    uint64_t hardware_subarch_data; // 0x0240
    uint32_t payload_offset;    // 0x0248: compressed payload offset
    uint32_t payload_length;    // 0x024c: compressed payload length
    uint64_t setup_data;        // 0x0250: linked list of setup_data
    uint64_t pref_address;      // 0x0258: preferred load address
    uint32_t init_size;         // 0x0260: init size for kernel
    uint32_t handover_offset;   // 0x0264: efi handover offset
    uint32_t kernel_info_offset;// 0x0268: kernel info offset (v2.15+)
};

constexpr uint32_t LINUX_HDRS_MAGIC   = 0x53726448; // "hdrs"
constexpr uint16_t LINUX_BOOT_FLAG    = 0xAA55;
constexpr uint32_t LINUX_HEADER_OFFSET= 0x01F1;     // offset of setup header in bzimage
constexpr uint32_t LINUX_BOOT_SECTOR  = 512;         // first 512 bytes = boot sector

constexpr uint8_t LOADFLAG_LOADED_HIGH  = 0x01; // protected-mode code loaded at 0x100000
constexpr uint8_t LOADFLAG_KASLR        = 0x02; // kaslr flag
constexpr uint8_t LOADFLAG_QUIET        = 0x20; // quiet boot
constexpr uint8_t LOADFLAG_KEEP_SEGMENTS= 0x40; // don't reload segments
constexpr uint8_t LOADFLAG_CAN_USE_HEAP= 0x80;  // heap is available

constexpr uint8_t BOOTLOADER_ID_KURONO = 0xFF;   // our custom id

constexpr uint32_t LINUX_BOOT_PARAMS_ADDR = 0x00010000; // boot_params struct
constexpr uint32_t LINUX_CMDLINE_ADDR     = 0x00020000; // command line string
constexpr uint32_t LINUX_SETUP_ADDR       = 0x00010000; // real-mode setup code
constexpr uint32_t LINUX_KERNEL_ADDR      = 0x00100000; // protected-mode kernel at 1 mb
constexpr uint32_t LINUX_INITRD_ADDR      = 0x00800000; // initrd at 8 mb
constexpr uint32_t LINUX_CMDLINE_MAX      = 2048;

// the full boot_params struct is 4096 bytes at 0x10000.
// key fields (official kernel offsets):
constexpr int BP_SCREEN_INFO      = 0x000; // 64 bytes
constexpr int BP_E820_ENTRIES     = 0x1E8; // uint8_t: number of e820 entries
constexpr int BP_SETUP_HEADER     = 0x1F1; // setup_header starts here
constexpr int BP_E820_TABLE       = 0x2D0; // e820 entries start here
constexpr int BP_E820_ENTRY_SIZE  = 20;    // each e820 entry is 20 bytes

//  linux bzimage information (parsed from setup header)
struct LinuxImageInfo {
    bool     valid;
    uint16_t protocol_version;
    uint32_t setup_size;        // real-mode setup code size
    uint32_t kernel_size;       // protected-mode kernel size
    uint32_t total_image_size;  // total bzimage file size
    uint32_t code32_start;      // entry point (usually 0x100000)
    uint32_t cmdline_max;       // max command line size
    uint32_t initrd_addr_max;   // max initrd address
    bool     loaded_high;       // kernel loaded at 1 mb
    bool     can_use_heap;      // setup heap available
    bool     relocatable;       // kernel is relocatable
    uint32_t kernel_alignment;
    uint32_t init_size;         // total init size
};

//  linuxbootloader - handles bzimage loading and boot parameter setup
class LinuxBootLoader {
public:
    // returns true if the image is a valid linux bzimage.
    static bool ParseImage(const uint8_t* image_data, uint32_t image_size,
                           LinuxImageInfo& info);

    // loads the protected-mode kernel to 0x100000 and sets up boot_params
    // at 0x10000. command line goes to linux_cmdline_addr.
    static bool LoadKernel(const uint8_t* image_data, uint32_t image_size,
                           const char* cmdline);

    static bool LoadInitrd(const uint8_t* initrd_data, uint32_t initrd_size);

    // must be called after loadkernel. fills in e820 map, video info,
    // and other boot parameters.
    static bool SetupBootParams();

    static uint32_t GetEntryPoint();

    static const LinuxImageInfo& GetImageInfo() { return image_info; }
    static const char* GetCommandLine() { return cmdline_buf; }

    static void DumpImageInfo();

private:
    static LinuxImageInfo image_info;
    static char cmdline_buf[LINUX_CMDLINE_MAX];
    static bool kernel_loaded;
    static bool initrd_loaded;
    static uint32_t initrd_guest_addr;
    static uint32_t initrd_size;

    static void FillScreenInfo(uint8_t* boot_params);
    static void FillE820(uint8_t* boot_params);
};
