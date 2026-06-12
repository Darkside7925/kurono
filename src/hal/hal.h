#pragma once
#include "../kernel/types.h"

//  interruptframe  -  matches the layout pushed by isr_stubs.asm
//  isr_common pushes: cr2, r15-r8, rbp, rdi, rsi, rdx, rcx, rbx, rax,
//                     then vector + error_code were pushed by the stub,
//                     and rip, cs, rflags, rsp, ss were pushed by the cpu.
struct InterruptFrame {
    uint64_t cr2;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

constexpr uint16_t GDT_KERNEL_CODE_SELECTOR = 0x08;
constexpr uint16_t GDT_KERNEL_DATA_SELECTOR = 0x10;
constexpr uint16_t GDT_USER_DATA_SELECTOR   = 0x18;
constexpr uint16_t GDT_USER_CODE_SELECTOR   = 0x20;
constexpr uint16_t GDT_TSS_SELECTOR         = 0x28;

//  hardware abstraction layer
//  idt (256 entries), pic 8259a remapping, exception handlers, io ports
class HAL {
public:
    typedef void (*SystemCallHandler)(InterruptFrame*);
    static void Init();

    static void EnableInterrupts();
    static void DisableInterrupts();
    static void Halt();
    static void WaitForInterrupt();
    [[noreturn]] static void EnterUserMode(uint64_t rip, uint64_t rsp);

    static void EnableIRQ(uint8_t irq);   // unmask a specific irq line (0-15)
    static void DisableIRQ(uint8_t irq);  // mask a specific irq line
    static void SendEOI(uint8_t irq);     // send end-of-interrupt

    typedef void (*IRQHandler)(InterruptFrame*);
    static void RegisterIRQHandler(uint8_t irq, IRQHandler handler);  // irq 0-15
    static void RegisterSystemCallHandler(SystemCallHandler handler);
    static void SetKernelStack(uint64_t rsp0);

    // bring an application processor's cpu state up to par with the bsp: its own
    // gdt + tss (so faults / int 0x80 don't share one rsp0 stack), the shared
    // idt, and the per-core SYSCALL msrs. called from ap_entry. (satoru)
    static void SetupAPCpuState();

    // Configure MSRs for the x86_64 SYSCALL/SYSRET fast path.
    // Must be called once after GDT is loaded.  Enables EFER.SCE,
    // programs STAR/LSTAR/SFMASK, and points the LSTAR handler at
    // syscall_entry_x64 (defined in src/hal/syscall_entry.asm).
    static void InitSyscallMSRs();

    static void Reboot();

    // soft power-off: try emulator/acpi poweroff ports, then fall back to
    // halt. does not return on success. (satoru)
    [[noreturn]] static void PowerOff();

    // acpi s3 suspend-to-ram. attempts to locate pm1a_cnt_blk + the \_s3
    // slp_typ via a minimal rsdp/rsdt/xsdt/fadt walk and enter s3. returns
    // false (and stays running) when the acpi tables cannot be parsed
    // safely. (satoru)
    static bool Suspend();

    static void    OutByte(uint16_t port, uint8_t  value);
    static uint8_t InByte(uint16_t port);
    static void    OutWord(uint16_t port, uint16_t value);
    static uint16_t InWord(uint16_t port);
    static void    OutLong(uint16_t port, uint32_t value);
    static uint32_t InLong(uint16_t port);
    static void    IOWait();

    static void MemoryBarrier();

    static volatile uint64_t pit_ticks;
    static volatile bool     irq_fired[16];
    static IRQHandler irq_handlers[16];

private:
    static void InitGDT();
    static void InitIDT();
    static void InitPIC();
};
