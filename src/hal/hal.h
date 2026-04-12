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

//  hardware abstraction layer
//  idt (256 entries), pic 8259a remapping, exception handlers, io ports
class HAL {
public:
    static void Init();

    static void EnableInterrupts();
    static void DisableInterrupts();
    static void Halt();
    static void WaitForInterrupt();

    static void EnableIRQ(uint8_t irq);   // unmask a specific irq line (0-15)
    static void DisableIRQ(uint8_t irq);  // mask a specific irq line
    static void SendEOI(uint8_t irq);     // send end-of-interrupt

    typedef void (*IRQHandler)(InterruptFrame*);
    static void RegisterIRQHandler(uint8_t irq, IRQHandler handler);  // irq 0-15

    static void Reboot();

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
    static void InitIDT();
    static void InitPIC();
};
