/* ═══════════════════════════════════════════════════════════════════════════
 *  Kurono OS  -  Standalone EFI Loader
 *
 *  Bypasses GRUB's multiboot2 relocator entirely.  The kernel ELF is
 *  embedded directly in this binary (via objcopy), eliminating filesystem
 *  dependencies.  Memory is allocated with EfiLoaderCode (executable  - 
 *  avoids NX page faults on real firmware).  Gets the framebuffer via
 *  GOP, builds a minimal Multiboot2-compatible info structure, calls
 *  ExitBootServices, and jumps directly to the kernel's 64-bit entry.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <efi.h>
#include <efilib.h>

#ifndef KURONO_EFI_CMDLINE
#define KURONO_EFI_CMDLINE "efi"
#endif

/* ── ELF64 structures (minimal, self-contained) ─────────────────────── */

typedef struct {
    unsigned char e_ident[16];
    UINT16 e_type, e_machine;
    UINT32 e_version;
    UINT64 e_entry, e_phoff, e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize, e_phentsize, e_phnum;
    UINT16 e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type, p_flags;
    UINT64 p_offset, p_vaddr, p_paddr;
    UINT64 p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_LOAD  1
#define ELF_MAGIC 0x464C457FU  /* "\x7FELF" as little-endian u32 */

/* ── Multiboot2-compatible structures for the kernel ────────────────── */

#define MB2_MAGIC_BOOT  0x36d76289U

#pragma pack(1)
struct mb2_info {
    UINT32 total_size;
    UINT32 reserved;
};
struct mb2_tag_hdr {
    UINT32 type;
    UINT32 size;
};
struct mb2_tag_fb {
    UINT32 type;          /* 8 */
    UINT32 size;
    UINT64 fb_addr;
    UINT32 pitch;
    UINT32 width;
    UINT32 height;
    UINT8  bpp;
    UINT8  fb_type;       /* 1 = RGB direct */
    UINT8  reserved;
    UINT8  pad;
    UINT8  red_pos, red_mask;
    UINT8  green_pos, green_mask;
    UINT8  blue_pos, blue_mask;
};
struct mb2_tag_cmdline {
    UINT32 type;          /* 1 */
    UINT32 size;
    char   string[8];     /* "efi\0" + padding */
};
struct mb2_tag_mmap_entry {
    UINT64 addr;
    UINT64 len;
    UINT32 type;          /* 1=RAM,2=reserved,3=ACPI,4=NVS,5=bad */
    UINT32 reserved;
};
struct mb2_tag_mmap {
    UINT32 type;          /* 6 */
    UINT32 size;
    UINT32 entry_size;
    UINT32 entry_version;
    /* entries follow */
};
#pragma pack()

/* Statically-allocated boot info buffer (must survive ExitBootServices) */
static UINT8 boot_info_buf[16384] __attribute__((aligned(8)));

/* ── Embedded kernel binary (linked via objcopy) ─────────────────────── */
extern UINT8 _binary_kurono_elf_start[];
extern UINT8 _binary_kurono_elf_end[];

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void serial_out(UINT16 port, UINT8 val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static UINT8 serial_in(UINT16 port) {
    UINT8 val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void serial_char(char c) {
    while (!(serial_in(0x3FD) & 0x20)) {}
    serial_out(0x3F8, (UINT8)c);
}

static void serial_str(const char *s) {
    while (*s) serial_char(*s++);
}

static void serial_hex(UINT64 val) {
    for (int i = 60; i >= 0; i -= 4) {
        int nib = (int)((val >> i) & 0xF);
        serial_char((char)(nib < 10 ? '0' + nib : 'A' + nib - 10));
    }
}

static UINTN ascii_len(const char *s) {
    UINTN n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* ── EFI Entry Point ─────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS  status;
    UINT8      *elf_buf;
    UINTN       elf_size;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINT64      entry_point;
    int         i;
    const char *mb2_cmdline = KURONO_EFI_CMDLINE;

    /* ── 0. Initialise gnu-efi library ──────────────────────────────── */
    InitializeLib(ImageHandle, SystemTable);

    Print(L"\r\n=== Kurono OS EFI Loader ===\r\n");
    Print(L"Cmdline: %a\r\n", mb2_cmdline);

    /* ── 1. Use embedded kernel data ────────────────────────────────── */
    elf_buf  = _binary_kurono_elf_start;
    elf_size = (UINTN)(_binary_kurono_elf_end - _binary_kurono_elf_start);
    Print(L"Kernel: %u bytes (embedded)\r\n", elf_size);

    /* ── 2. Validate ELF header ────────────────────────────────────── */
    ehdr = (Elf64_Ehdr *)elf_buf;
    if (*(UINT32 *)ehdr->e_ident != ELF_MAGIC) {
        Print(L"ERR: Not ELF (magic 0x%08x)\r\n",
              *(UINT32 *)ehdr->e_ident);
        goto hang;
    }
    entry_point = ehdr->e_entry;  /* default  -  will be overridden below */
    Print(L"ELF entry: 0x%lx  PHs: %d\r\n", entry_point, ehdr->e_phnum);

    /* ── 3. Load ELF LOAD segments ─────────────────────────────────── */
    /*    KEY: allocate as EfiLoaderCode so pages are EXECUTABLE.       */
    /*    This avoids the NX page fault that kills the GRUB relocator   */
    /*    on real firmware.                                              */
    phdr = (Elf64_Phdr *)(elf_buf + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
        if (phdr->p_type != PT_LOAD) continue;

        UINT64 paddr = phdr->p_paddr;
        UINT64 memsz = phdr->p_memsz;
        UINT64 filesz = phdr->p_filesz;
        UINTN pages = (UINTN)((memsz + 4095) / 4096);

        /* Allocate at exact physical address as EfiLoaderCode (EXECUTABLE) */
        EFI_PHYSICAL_ADDRESS alloc_addr = paddr;
        status = uefi_call_wrapper(BS->AllocatePages, 4,
                                   AllocateAddress,
                                   EfiLoaderCode,
                                   pages, &alloc_addr);
        if (EFI_ERROR(status)) {
            Print(L"WARN: AllocatePages at 0x%lx: %r\r\n", paddr, status);
            /* On some firmware the low 1MB is reserved.  For the kernel
               at 0x100000 this should generally be free.  If not, we
               can't relocate a non-PIC kernel, so just try anyway. */
        }

        /* Copy segment data */
        CopyMem((void *)(UINTN)paddr, elf_buf + phdr->p_offset, (UINTN)filesz);
        /* Zero BSS */
        if (memsz > filesz)
            SetMem((void *)(UINTN)(paddr + filesz), (UINTN)(memsz - filesz), 0);

        Print(L"  LOAD 0x%lx  file=%u mem=%u\r\n", paddr, filesz, memsz);
    }

    /* ── 4. Find _start_efi64 from the loaded MB2 header ─────────── */
    /*    Scan for MB2 magic 0xE85250D6 in the loaded kernel, then    */
    /*    parse tags to find type 9 (EFI amd64 entry address).        */
    {
        UINT32 *scan = (UINT32 *)(UINTN)0x100000;
        UINT32 *scan_end = scan + (0x8000 / 4); /* first 32 KiB */
        for (; scan < scan_end; scan++) {
            if (*scan == 0xE85250D6U) { /* MB2 header magic */
                UINT32 hdr_len = scan[2];
                UINT8 *tp = (UINT8 *)(scan + 4); /* first tag */
                UINT8 *te = (UINT8 *)scan + hdr_len;
                while (tp < te) {
                    UINT16 ttype = *(UINT16 *)tp;
                    UINT32 tsize = *(UINT32 *)(tp + 4);
                    if (ttype == 0) break;          /* end tag */
                    if (ttype == 9 && tsize >= 12) { /* EFI amd64 entry */
                        UINT32 efi64 = *(UINT32 *)(tp + 8);
                        entry_point = (UINT64)efi64;
                        Print(L"EFI64 entry: 0x%lx (MB2 tag 9)\r\n",
                              entry_point);
                    }
                    tp += (tsize + 7) & ~(UINTN)7;  /* align to 8 */
                }
                break;
            }
        }
    }

    /* ── 5. Get framebuffer via GOP ────────────────────────────────── */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    UINT64 fb_addr = 0;
    UINT32 fb_width = 0, fb_height = 0, fb_pitch = 0;
    UINT8  fb_bpp = 32;

    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &gop_guid, NULL, (void **)&gop);
    if (!EFI_ERROR(status) && gop && gop->Mode) {
        fb_addr   = gop->Mode->FrameBufferBase;
        fb_width  = gop->Mode->Info->HorizontalResolution;
        fb_height = gop->Mode->Info->VerticalResolution;
        fb_pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
        Print(L"GOP: %dx%d @ 0x%lx\r\n", fb_width, fb_height, fb_addr);

        /* Draw proof-of-life green bar (4 rows) */
        UINT32 *fb = (UINT32 *)(UINTN)fb_addr;
        for (i = 0; i < (int)(fb_width * 4); i++)
            fb[i] = 0xFF00FF00;  /* green */
    } else {
        Print(L"WARN: No GOP framebuffer\r\n");
    }

    /* ── 6. Build Multiboot2-compatible info structure ─────────────── */
    {
        UINT8 *p = boot_info_buf;

        /* Header (filled last) */
        struct mb2_info *mbi = (struct mb2_info *)p;
        mbi->reserved = 0;
        p += 8;

        /* Tag 1: command line */
        struct mb2_tag_cmdline *t_cmd = (struct mb2_tag_cmdline *)p;
        UINTN cmd_len = ascii_len(mb2_cmdline);
        t_cmd->type = 1;
        t_cmd->size = (UINT32)(8 + cmd_len + 1);
        CopyMem(t_cmd->string, mb2_cmdline, cmd_len);
        t_cmd->string[cmd_len] = 0;
        p += (t_cmd->size + 7) & ~(UINTN)7;

        /* Tag 8: framebuffer */
        if (fb_addr) {
            struct mb2_tag_fb *t_fb = (struct mb2_tag_fb *)p;
            t_fb->type    = 8;
            t_fb->size    = 32;
            t_fb->fb_addr = fb_addr;
            t_fb->pitch   = fb_pitch;
            t_fb->width   = fb_width;
            t_fb->height  = fb_height;
            t_fb->bpp     = fb_bpp;
            t_fb->fb_type = 1;    /* RGB direct */
            t_fb->reserved = 0;
            t_fb->pad     = 0;
            t_fb->red_pos = 16; t_fb->red_mask = 8;
            t_fb->green_pos = 8; t_fb->green_mask = 8;
            t_fb->blue_pos = 0; t_fb->blue_mask = 8;
            p += (t_fb->size + 7) & ~(UINTN)7;
        }

        /* Tag 6: memory map  -  convert EFI memory map to MB2 format */
        {
            UINTN map_size2 = 0, desc_size2;
            UINT32 desc_ver;
            UINTN map_key2;
            EFI_MEMORY_DESCRIPTOR *mmap2 = NULL;

            /* Get memory map size */
            uefi_call_wrapper(BS->GetMemoryMap, 5,
                              &map_size2, mmap2, &map_key2,
                              &desc_size2, &desc_ver);
            map_size2 += 4 * desc_size2;
            uefi_call_wrapper(BS->AllocatePool, 3,
                              EfiLoaderData, map_size2,
                              (void **)&mmap2);
            uefi_call_wrapper(BS->GetMemoryMap, 5,
                              &map_size2, mmap2, &map_key2,
                              &desc_size2, &desc_ver);

            struct mb2_tag_mmap *t_mmap = (struct mb2_tag_mmap *)p;
            t_mmap->type = 6;
            t_mmap->entry_size = 24;
            t_mmap->entry_version = 0;
            UINT8 *ep = p + 16;  /* after tag header */

            UINTN n_entries = map_size2 / desc_size2;
            for (UINTN j = 0; j < n_entries; j++) {
                EFI_MEMORY_DESCRIPTOR *d =
                    (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap2 + j * desc_size2);

                UINT32 mb_type;
                switch (d->Type) {
                case EfiConventionalMemory:
                case EfiBootServicesCode:
                case EfiBootServicesData:
                case EfiLoaderCode:
                case EfiLoaderData:
                    mb_type = 1; break;  /* Available */
                case EfiACPIReclaimMemory:
                    mb_type = 3; break;
                case EfiACPIMemoryNVS:
                    mb_type = 4; break;
                default:
                    mb_type = 2; break;  /* Reserved */
                }

                struct mb2_tag_mmap_entry *me =
                    (struct mb2_tag_mmap_entry *)ep;
                me->addr = d->PhysicalStart;
                me->len  = d->NumberOfPages * 4096ULL;
                me->type = mb_type;
                me->reserved = 0;
                ep += 24;
            }
            t_mmap->size = (UINT32)(ep - p);
            p = ep;
            p = (UINT8 *)(((UINTN)p + 7) & ~(UINTN)7);
        }

        /* End tag */
        struct mb2_tag_hdr *t_end = (struct mb2_tag_hdr *)p;
        t_end->type = 0;
        t_end->size = 8;
        p += 8;

        mbi->total_size = (UINT32)(p - boot_info_buf);
    }

    /* ── 7. ExitBootServices ──────────────────────────────────────── */
    {
        UINTN  map_size3 = 0, desc_size3;
        UINT32 desc_ver3;
        UINTN  map_key3;
        EFI_MEMORY_DESCRIPTOR *mmap3 = NULL;

        uefi_call_wrapper(BS->GetMemoryMap, 5,
                          &map_size3, mmap3, &map_key3,
                          &desc_size3, &desc_ver3);
        map_size3 += 4 * desc_size3;
        uefi_call_wrapper(BS->AllocatePool, 3,
                          EfiLoaderData, map_size3,
                          (void **)&mmap3);
        uefi_call_wrapper(BS->GetMemoryMap, 5,
                          &map_size3, mmap3, &map_key3,
                          &desc_size3, &desc_ver3);

        status = uefi_call_wrapper(BS->ExitBootServices, 2,
                                   ImageHandle, map_key3);
        if (EFI_ERROR(status)) {
            /* Map key changed  -  retry once */
            uefi_call_wrapper(BS->GetMemoryMap, 5,
                              &map_size3, mmap3, &map_key3,
                              &desc_size3, &desc_ver3);
            status = uefi_call_wrapper(BS->ExitBootServices, 2,
                                       ImageHandle, map_key3);
        }
    }

    /* ═══════════════════════════════════════════════════════════════
     * NO MORE EFI BOOT SERVICES FROM HERE.  We're in 64-bit long
     * mode with EFI's identity-mapped page tables still active.
     * The kernel code was allocated as EfiLoaderCode → executable.
     * ═══════════════════════════════════════════════════════════════ */

    /* ── 8. Init COM1 serial (115200 8N1) ──────────────────────────── */
    serial_out(0x3F9, 0x00); /* IER: disable ints */
    serial_out(0x3FB, 0x80); /* LCR: DLAB on */
    serial_out(0x3F8, 0x01); /* DLL: 115200 baud */
    serial_out(0x3F9, 0x00); /* DLH: 0 */
    serial_out(0x3FB, 0x03); /* LCR: 8N1, DLAB off */
    serial_out(0x3FA, 0xC7); /* FCR: FIFO enable */
    serial_out(0x3FC, 0x03); /* MCR: DTR+RTS */

    serial_str("EFI_LOADER: entry=0x");
    serial_hex(entry_point);
    serial_str("\r\n");
    serial_str("EFI_LOADER: cmdline=");
    serial_str(mb2_cmdline);
    serial_str("\r\n");

    /* ── 9. PC Speaker beep  -  audible proof-of-life ───────────────── */
    serial_out(0x43, 0xB6);
    serial_out(0x42, 0xA9);  /* 1000 Hz low byte */
    serial_out(0x42, 0x04);  /* 1000 Hz high byte */
    {
        UINT8 tmp = serial_in(0x61);
        serial_out(0x61, tmp | 0x03);
    }
    for (volatile UINTN d = 0; d < 50000000ULL; d++) {}
    {
        UINT8 tmp = serial_in(0x61);
        serial_out(0x61, tmp & 0xFC);
    }

    serial_str("EFI_LOADER: beep done, jumping\r\n");

    /* ── 10. Jump to kernel's 64-bit entry ─────────────────────────── */
    /*    _start_efi64 expects: EAX = MB2 magic, EBX = info pointer.   */
    {
        UINT32 magic32 = MB2_MAGIC_BOOT;
        UINT32 info32  = (UINT32)(UINTN)boot_info_buf;
        void  *target  = (void *)(UINTN)entry_point;

        __asm__ volatile(
            "cli\n\t"
            "mov %[magic], %%eax\n\t"
            "mov %[info],  %%ebx\n\t"
            "jmp *%[entry]\n\t"
            :
            : [magic] "r"(magic32),
              [info]  "r"(info32),
              [entry] "r"(target)
            : "eax", "ebx"
        );
    }

    /* Never reached */
    while (1) { __asm__ volatile("hlt"); }

hang:
    Print(L"\r\nPress any key to reboot...\r\n");
    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
    {
        EFI_INPUT_KEY key;
        UINTN idx;
        uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &idx);
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
    }
    return EFI_LOAD_ERROR;
}
