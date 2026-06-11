#pragma once
#include "../kernel/types.h"

// Hardware Abstraction Layer
// Isolates the kernel from specific hardware implementation details.
// Handles IDT, PIC, and core CPU operations.

class HAL {
public:
    enum InterruptState {
        Disabled = 0,
        Enabled = 1
    };

    static void Init();          // Sets up IDT, remaps PIC, installs ISRs
    static void InitIDT();       // Called from Init — builds interrupt descriptor table
    
    // Core CPU control
    static void EnableInterrupts();
    static void DisableInterrupts();
    static void Halt();
    static void WaitForInterrupt();
    static void Reboot();
    
    // IO Ports (x86 specific, but wrapped)
    static void OutByte(uint16_t port, uint8_t value);
    static uint8_t InByte(uint16_t port);
    static void OutWord(uint16_t port, uint16_t value);
    static uint16_t InWord(uint16_t port);
    static void OutLong(uint16_t port, uint32_t value);
    static uint32_t InLong(uint16_t port);
    
    // Memory barriers
    static void MemoryBarrier();
    
    // Interrupt management
    static volatile uint64_t pit_ticks;     // PIT IRQ0 tick counter
    static volatile bool     irq_fired[16]; // flags set by IRQ handlers
};
