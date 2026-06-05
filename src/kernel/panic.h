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

// persistent crash minidump written to a fixed physical page on panic, then
// recovered into kvfs on the next boot. laid out as a flat pod so it can be
// memcpy'd byte-for-byte through a volatile pointer at the reserved physical
// address  -  no heap, no constructors. (satoru)
namespace MiniDump {
    constexpr uint32_t MAGIC        = 0x4B44554Du;  // 'KDUM' (satoru)
    constexpr uint64_t PHYS_ADDR    = 0x1000000ull; // 16 mb identity-mapped slot (satoru)
    constexpr uint32_t MAX_BYTES    = 512u * 1024u;  // hard cap on the dump region (satoru)
    constexpr uint32_t MSG_LEN      = 256u;          // panic message field width (satoru)
    constexpr uint32_t STACK_FRAMES = 32u;           // rbp-chain frames captured (satoru)
    constexpr uint32_t SERIAL_BYTES = 4096u;         // recent serial-log tail captured (satoru)
}

struct KuronoMiniDump {
    uint32_t magic;            // MiniDump::MAGIC when a dump is present (satoru)
    uint32_t version;          // layout version for forward compat (satoru)
    uint32_t size;             // total meaningful bytes in this struct (satoru)
    uint32_t stop_code;        // bugcheck stop code (satoru)

    // wall-clock from the rtc at panic time (satoru)
    uint32_t unix_time;        // seconds since 1970 derived from rtc (satoru)
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  time_valid;       // 1 if the rtc read succeeded (satoru)

    // panic message, truncated/zero-padded to MSG_LEN (satoru)
    char     message[256];

    // all 16 general-purpose registers (satoru)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;

    // control registers (satoru)
    uint64_t cr0, cr2, cr3, cr4;

    // rbp-chain stack trace; frame_count valid entries (satoru)
    uint32_t frame_count;
    uint32_t _pad0;
    uint64_t stack_trace[32];

    // tail of the recent serial/runtime log; serial_len valid bytes (satoru)
    uint32_t serial_len;
    uint8_t  serial_truncated;  // 1 if older log lines were dropped (satoru)
    uint8_t  _pad1[3];
    char     serial_tail[4096];
};

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

    // boot-time crash recovery. checks the reserved physical dump slot for a
    // valid KuronoMiniDump; if found, copies it to a timestamped kvfs file
    // under /var/log, clears the magic so it is recovered exactly once, and
    // returns true so the caller can surface a "recovered from crash" notice.
    // returns false when no dump is present. safe to call early in boot once
    // KVFS::Init has run. (satoru)
    bool ScanCrashDumpAtBoot();
}

// convenience macros
#define KBUGCHECK(code, reason) \
    KernelPanic::KeBugCheckEx((code), 0, 0, 0, 0, (reason), __FILE__, (uint32_t)__LINE__)

#define KBUGCHECK_EX(code, p1, p2, p3, p4, reason) \
    KernelPanic::KeBugCheckEx((code), (p1), (p2), (p3), (p4), (reason), __FILE__, (uint32_t)__LINE__)

#define KERNEL_PANIC(msg) KBUGCHECK(StopCode::KERNEL_FATAL, (msg))
#define KASSERT(cond, msg) do { if (!(cond)) KBUGCHECK(StopCode::ASSERTION_FAILED, (msg)); } while(0) 