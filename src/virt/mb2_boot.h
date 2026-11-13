//  kurono os - multiboot2 guest loader (satoru)
//
//  boots a multiboot2 ELF64 kernel (Kurono's own kurono_base.elf) as a guest
//  of Kurono's hypervisor, so a freshly built kernel can be tested without
//  rebooting the machine that built it. the guest is entered the way GRUB
//  enters it: 32-bit protected mode, paging off, EAX = 0x36d76289, EBX = the
//  multiboot2 info. the info carries the command line, the memory map (the
//  hypervisor's e820 with a framebuffer hole) and a 32 bpp linear framebuffer
//  that lives in guest RAM, so the host can read the guest's screen without
//  any display device. (satoru)
#pragma once
#include "../kernel/types.h"

namespace MB2Boot {
    // framebuffer geometry the guest is given (satoru)
    constexpr uint32_t FB_W = 1024, FB_H = 768, FB_BPP = 32;
    constexpr uint32_t FB_PITCH = FB_W * 4;

    // copy the PT_LOAD segments of `elf` to their physical addresses in guest
    // RAM (bss zeroed), build the multiboot2 info right after the image and
    // report where to enter. guest RAM must already exist (CreateVM). false,
    // with a reason in Error(), when the image is not a usable multiboot2
    // ELF64 or does not fit. (satoru)
    bool Load(const uint8_t* elf, uint64_t size, const char* cmdline,
              uint32_t* entry, uint32_t* mbi);
    const char* Error();
    // guest-physical framebuffer address (valid after Load), 0 if none (satoru)
    uint64_t FramebufferGPA();
}

// end (satoru)
