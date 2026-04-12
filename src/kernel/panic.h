#pragma once
//  kurono os  -  kernel panic / bugcheck subsystem
//  direct framebuffer rendering  -  zero dependencies on graphics/font/heap.
//  scales to any monitor resolution (640x480 → 4k).
#include "types.h"

struct multiboot_info_t;
struct InterruptFrame;

namespace StopCode {
    // cpu exception-mirrored (vector number)
    constexpr uint32_t DIVIDE_BY_ZERO         = 0x00000000;
    constexpr uint32_t DEBUG_EXCEPTION        = 0x00000001;
    constexpr uint32_t NMI                    = 0x00000002;
    constexpr uint32_t BREAKPOINT             = 0x00000003;
    constexpr uint32_t OVERFLOW               = 0x00000004;
    constexpr uint32_t BOUND_RANGE_EXCEEDED   = 0x00000005;
    constexpr uint32_t INVALID_OPCODE         = 0x00000006;
    constexpr uint32_t DEVICE_NOT_AVAILABLE   = 0x00000007;
    constexpr uint32_t DOUBLE_FAULT           = 0x00000008;
    constexpr uint32_t INVALID_TSS            = 0x0000000A;
    constexpr uint32_t SEGMENT_NOT_PRESENT    = 0x0000000B;
    constexpr uint32_t STACK_SEGMENT_FAULT    = 0x0000000C;
    constexpr uint32_t GENERAL_PROTECTION     = 0x0000000D;
    constexpr uint32_t PAGE_FAULT             = 0x0000000E;
    constexpr uint32_t X87_FP_EXCEPTION       = 0x00000010;
    constexpr uint32_t ALIGNMENT_CHECK        = 0x00000011;
    constexpr uint32_t MACHINE_CHECK          = 0x00000012;
    constexpr uint32_t SIMD_FP_EXCEPTION      = 0x00000013;
    constexpr uint32_t VIRT_EXCEPTION         = 0x00000014;
    constexpr uint32_t CONTROL_PROTECTION     = 0x00000015;
    constexpr uint32_t SECURITY_EXCEPTION     = 0x0000001E;
    // kernel subsystem
    constexpr uint32_t HEAP_CORRUPTION        = 0x00010001;
    constexpr uint32_t STACK_OVERFLOW_FAULT   = 0x00010002;
    constexpr uint32_t NULL_DEREFERENCE       = 0x00010003;
    constexpr uint32_t ASSERTION_FAILED       = 0x00010004;
    constexpr uint32_t OUT_OF_MEMORY          = 0x00010005;
    constexpr uint32_t DRIVER_FAULT           = 0x00010006;
    constexpr uint32_t IRQ_NOT_LESS_OR_EQUAL  = 0x00010007;
    constexpr uint32_t SCHEDULER_FAULT        = 0x00010008;
    constexpr uint32_t VFS_CORRUPTION         = 0x00010009;
    constexpr uint32_t VMM_FAULT              = 0x0001000A;
    // fatal / unrecoverable
    constexpr uint32_t KERNEL_FATAL           = 0xDEAD0001;
    constexpr uint32_t MANUAL_CRASH           = 0xDEAD0002;
    constexpr uint32_t TRIPLE_FAULT_IMMINENT  = 0xDEAD0003;
    constexpr uint32_t WATCHDOG_TIMEOUT       = 0xDEAD0004;
}

namespace KernelPanic {
    // call once, very early boot (before graphics::init).
    void Initialize(multiboot_info_t* mbi);

    // update framebuffer info after graphics changes mode/address.
    // call this from graphics::init / reinitforresolution / etc.
    void UpdateFramebuffer(uint64_t addr, uint32_t pitch,
                           uint32_t width, uint32_t height, uint8_t bpp);

    // programmatic bugcheck from kernel code.
    [[noreturn]] void KeBugCheckEx(uint32_t stop_code,
                                   uint64_t param1  = 0,
                                   uint64_t param2  = 0,
                                   uint64_t param3  = 0,
                                   uint64_t param4  = 0,
                                   const char* reason = nullptr,
                                   const char* file   = nullptr,
                                   uint32_t    line   = 0);

    // called from isr for cpu exceptions.
    [[noreturn]] void BugCheckFromInterrupt(InterruptFrame* frame,
                                            const char* exception_name);
}

// convenience macros
#define KBUGCHECK(code, reason) \
    KernelPanic::KeBugCheckEx((code), 0, 0, 0, 0, (reason), __FILE__, (uint32_t)__LINE__)

#define KBUGCHECK_EX(code, p1, p2, p3, p4, reason) \
    KernelPanic::KeBugCheckEx((code), (p1), (p2), (p3), (p4), (reason), __FILE__, (uint32_t)__LINE__)

#define KERNEL_PANIC(msg) KBUGCHECK(StopCode::KERNEL_FATAL, (msg))
#define KASSERT(cond, msg) do { if (!(cond)) KBUGCHECK(StopCode::ASSERTION_FAILED, (msg)); } while(0)
