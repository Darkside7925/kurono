// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Boot Protocol Implementation
//  Implements the x86 Linux boot protocol for loading a bzImage/vmlinuz
//  into a virtual machine.
//
//  Reference: Linux kernel Documentation/x86/boot.rst
//             (formerly Documentation/x86/boot.txt)
//  Protocol version: 2.15 (kernel ≥ 5.0)
//
//  Boot process:
//    1. Parse real-mode setup header from bzImage offset 0x01F1
//    2. Load real-mode code to 0x10000 (or below 0xA0000)
//    3. Load protected-mode kernel to 0x100000 (1 MB)
//    4. Fill in boot parameters (struct boot_params at 0x10000)
//    5. Set up E820, command line, initrd (if applicable)
//    6. Enter protected-mode kernel at 0x100000
// ═══════════════════════════════════════════════════════════════════════════
#pragma once
#include <stdint.h>
#include <stddef.h>

// ─── Linux Setup Header (at bzImage offset 0x01F1) ──────────────────────
// This matches the Linux kernel's struct setup_header (arch/x86/include/uapi/asm/bootparam.h)
struct __attribute__((packed)) LinuxSetupHeader {
    uint8_t  setup_sects;       // 0x01F1: Number of setup sectors
    uint16_t root_flags;        // 0x01F2: if nonzero, root is mounted readonly
    uint32_t syssize;           // 0x01F4: Protected-mode code size in 16-byte paras
    uint16_t ram_size;          // 0x01F8: obsolete
    uint16_t vid_mode;          // 0x01FA: video mode
    uint16_t root_dev;          // 0x01FC: default root device
    uint16_t boot_flag;         // 0x01FE: 0xAA55 magic
    uint16_t jump;              // 0x0200: jump instruction
    uint32_t header;            // 0x0202: "HdrS" magic (0x53726448)
    uint16_t version;           // 0x0206: boot protocol version
    uint32_t realmode_swtch;    // 0x0208: obsolete
    uint16_t start_sys_seg;     // 0x020C: obsolete
    uint16_t kernel_version;    // 0x020E: offset to kernel version string
    uint8_t  type_of_loader;    // 0x0210: boot loader ID
    uint8_t  loadflags;         // 0x0211: various flags
    uint16_t setup_move_size;   // 0x0212: size of setup move
    uint32_t code32_start;      // 0x0214: start address of protected-mode code
    uint32_t ramdisk_image;     // 0x0218: initrd load address
    uint32_t ramdisk_size;      // 0x021C: initrd size
    uint32_t bootsect_kludge;   // 0x0220: obsolete
    uint16_t heap_end_ptr;      // 0x0224: setup heap end pointer
    uint8_t  ext_loader_ver;    // 0x0226: extended loader version
    uint8_t  ext_loader_type;   // 0x0227: extended loader type
    uint32_t cmd_line_ptr;      // 0x0228: pointer to kernel command line
    uint32_t initrd_addr_max;   // 0x022C: maximum initrd address
    uint32_t kernel_alignment;  // 0x0230: alignment for kernel
    uint8_t  relocatable_kernel;// 0x0234: is kernel relocatable?
    uint8_t  min_alignment;     // 0x0235: min alignment (2^n)
    uint16_t xloadflags;        // 0x0236: extended load flags
    uint32_t cmdline_size;      // 0x0238: max command line size
    uint32_t hardware_subarch;  // 0x023C: hardware subarchitecture
    uint64_t hardware_subarch_data; // 0x0240
    uint32_t payload_offset;    // 0x0248: compressed payload offset
    uint32_t payload_length;    // 0x024C: compressed payload length
    uint64_t setup_data;        // 0x0250: linked list of setup_data
    uint64_t pref_address;      // 0x0258: preferred load address
    uint32_t init_size;         // 0x0260: init size for kernel
    uint32_t handover_offset;   // 0x0264: EFI handover offset
    uint32_t kernel_info_offset;// 0x0268: kernel info offset (v2.15+)
};

// ─── Linux Setup Header Constants ────────────────────────────────────────
constexpr uint32_t LINUX_HDRS_MAGIC   = 0x53726448; // "HdrS"
constexpr uint16_t LINUX_BOOT_FLAG    = 0xAA55;
constexpr uint32_t LINUX_HEADER_OFFSET= 0x01F1;     // Offset of setup header in bzImage
constexpr uint32_t LINUX_BOOT_SECTOR  = 512;         // First 512 bytes = boot sector

// ─── Loadflags bits ──────────────────────────────────────────────────────
constexpr uint8_t LOADFLAG_LOADED_HIGH  = 0x01; // Protected-mode code loaded at 0x100000
constexpr uint8_t LOADFLAG_KASLR        = 0x02; // KASLR flag
constexpr uint8_t LOADFLAG_QUIET        = 0x20; // Quiet boot
constexpr uint8_t LOADFLAG_KEEP_SEGMENTS= 0x40; // Don't reload segments
constexpr uint8_t LOADFLAG_CAN_USE_HEAP= 0x80;  // Heap is available

// ─── Boot loader IDs ─────────────────────────────────────────────────────
constexpr uint8_t BOOTLOADER_ID_KURONO = 0xFF;   // Our custom ID

// ─── Boot parameter structure locations (guest physical addresses) ────────
constexpr uint32_t LINUX_BOOT_PARAMS_ADDR = 0x00010000; // boot_params struct
constexpr uint32_t LINUX_CMDLINE_ADDR     = 0x00020000; // Command line string
constexpr uint32_t LINUX_SETUP_ADDR       = 0x00010000; // Real-mode setup code
constexpr uint32_t LINUX_KERNEL_ADDR      = 0x00100000; // Protected-mode kernel at 1 MB
constexpr uint32_t LINUX_INITRD_ADDR      = 0x00800000; // Initrd at 8 MB
constexpr uint32_t LINUX_CMDLINE_MAX      = 2048;

// ─── struct boot_params offsets (from Linux kernel) ──────────────────────
// The full boot_params struct is 4096 bytes at 0x10000.
// Key fields (official kernel offsets):
constexpr int BP_SCREEN_INFO      = 0x000; // 64 bytes
constexpr int BP_E820_ENTRIES     = 0x1E8; // uint8_t: number of E820 entries
constexpr int BP_SETUP_HEADER     = 0x1F1; // setup_header starts here
constexpr int BP_E820_TABLE       = 0x2D0; // E820 entries start here
constexpr int BP_E820_ENTRY_SIZE  = 20;    // Each E820 entry is 20 bytes

// ═══════════════════════════════════════════════════════════════════════════
//  Linux bzImage Information (parsed from setup header)
// ═══════════════════════════════════════════════════════════════════════════
struct LinuxImageInfo {
    bool     valid;
    uint16_t protocol_version;
    uint32_t setup_size;        // Real-mode setup code size
    uint32_t kernel_size;       // Protected-mode kernel size
    uint32_t total_image_size;  // Total bzImage file size
    uint32_t code32_start;      // Entry point (usually 0x100000)
    uint32_t cmdline_max;       // Max command line size
    uint32_t initrd_addr_max;   // Max initrd address
    bool     loaded_high;       // Kernel loaded at 1 MB
    bool     can_use_heap;      // Setup heap available
    bool     relocatable;       // Kernel is relocatable
    uint32_t kernel_alignment;
    uint32_t init_size;         // Total init size
};

// ═══════════════════════════════════════════════════════════════════════════
//  LinuxBootLoader — handles bzImage loading and boot parameter setup
// ═══════════════════════════════════════════════════════════════════════════
class LinuxBootLoader {
public:
    // ── Parse and validate a bzImage ─────────────────────────────────────
    // Returns true if the image is a valid Linux bzImage.
    static bool ParseImage(const uint8_t* image_data, uint32_t image_size,
                           LinuxImageInfo& info);

    // ── Load kernel into guest memory ────────────────────────────────────
    // Loads the protected-mode kernel to 0x100000 and sets up boot_params
    // at 0x10000. Command line goes to LINUX_CMDLINE_ADDR.
    static bool LoadKernel(const uint8_t* image_data, uint32_t image_size,
                           const char* cmdline);

    // ── Load initrd into guest memory ────────────────────────────────────
    static bool LoadInitrd(const uint8_t* initrd_data, uint32_t initrd_size);

    // ── Setup boot parameters ────────────────────────────────────────────
    // Must be called after LoadKernel. Fills in E820 map, video info,
    // and other boot parameters.
    static bool SetupBootParams();

    // ── Get the entry point for the protected-mode kernel ────────────────
    static uint32_t GetEntryPoint();

    // ── Get info about loaded kernel ─────────────────────────────────────
    static const LinuxImageInfo& GetImageInfo() { return image_info; }
    static const char* GetCommandLine() { return cmdline_buf; }

    // ── Debug ────────────────────────────────────────────────────────────
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
