#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Virtual Device Emulation
//  Emulated PIC (8259A), APIC, PIT (8254), HPET for VM guests
// ═══════════════════════════════════════════════════════════════════════════
#include "../kernel/types.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Virtual PIC — Intel 8259A Programmable Interrupt Controller
//  Two PICs: master (ports 0x20-0x21) and slave (ports 0xA0-0xA1)
// ═══════════════════════════════════════════════════════════════════════════

struct VirtualPIC {
    // ── ICW (Initialization Command Words) state ──
    uint8_t  icw1;         // ICW1: edge/level, single/cascade, ICW4 needed
    uint8_t  icw2;         // ICW2: interrupt vector base
    uint8_t  icw3;         // ICW3: cascade mask (master) / slave ID
    uint8_t  icw4;         // ICW4: 8086 mode, auto-EOI, buffered, SFNM
    int      icw_step;     // Current ICW being programmed (0=done, 1-4=ICW#)
    bool     init_mode;    // Currently in initialization sequence

    // ── OCW (Operation Command Words) / Runtime state ──
    uint8_t  irr;          // Interrupt Request Register
    uint8_t  isr;          // In-Service Register
    uint8_t  imr;          // Interrupt Mask Register
    uint8_t  vector_base;  // Base vector number (set by ICW2)
    uint8_t  priority;     // Lowest priority level for rotation
    bool     auto_eoi;     // Auto-EOI mode (set by ICW4 bit 1)
    bool     special_mask; // Special Mask Mode
    bool     poll_mode;    // Poll mode
    bool     rotate_on_eoi;// Rotate on auto-EOI

    // ── Cascade ──
    bool     is_master;
    uint8_t  cascade_mask; // Master: which IRQs have slaves; Slave: ID

    // ── ELCR (Edge/Level Control Register for PIIX/ICH) ──
    uint8_t  elcr;         // Per-IRQ edge(0)/level(1) trigger

    void Reset();
    void WritePort(uint16_t port, uint8_t value);
    uint8_t ReadPort(uint16_t port);
    int GetHighestPriorityIRQ();      // Returns IRQ# or -1
    void SetIRQ(int irq, bool level); // Assert/deassert IRQ line
    void AcknowledgeIRQ(int irq);     // EOI
};

// ═══════════════════════════════════════════════════════════════════════════
//  Virtual APIC — Advanced Programmable Interrupt Controller
//  MMIO at 0xFEE00000 (4KB region), per-CPU local APIC
// ═══════════════════════════════════════════════════════════════════════════

#define VAPIC_BASE_ADDR     0xFEE00000
#define VAPIC_REG_SIZE      0x400  // 1024 bytes of registers

// Register offsets (aligned to 16 bytes)
#define APIC_ID             0x020
#define APIC_VERSION        0x030
#define APIC_TPR            0x080  // Task Priority Register
#define APIC_APR            0x090  // Arbitration Priority Register
#define APIC_PPR            0x0A0  // Processor Priority Register
#define APIC_EOI            0x0B0  // End of Interrupt
#define APIC_RRD            0x0C0  // Remote Read
#define APIC_LDR            0x0D0  // Logical Destination Register
#define APIC_DFR            0x0E0  // Destination Format Register
#define APIC_SVR            0x0F0  // Spurious Interrupt Vector Register
#define APIC_ISR_BASE       0x100  // In-Service Register (8 × 32-bit)
#define APIC_TMR_BASE       0x180  // Trigger Mode Register
#define APIC_IRR_BASE       0x200  // Interrupt Request Register
#define APIC_ESR            0x280  // Error Status Register
#define APIC_ICR_LO         0x300  // Interrupt Command Register (low)
#define APIC_ICR_HI         0x310  // Interrupt Command Register (high)
#define APIC_LVT_TIMER      0x320
#define APIC_LVT_THERMAL    0x330
#define APIC_LVT_PERFCNT    0x340
#define APIC_LVT_LINT0      0x350
#define APIC_LVT_LINT1      0x360
#define APIC_LVT_ERROR      0x370
#define APIC_TIMER_INIT     0x380  // Initial Count Register
#define APIC_TIMER_CURRENT  0x390  // Current Count Register
#define APIC_TIMER_DIVIDE   0x3E0  // Divide Configuration Register

// Timer modes
#define APIC_TIMER_ONE_SHOT     0
#define APIC_TIMER_PERIODIC     1
#define APIC_TIMER_TSC_DEADLINE 2

struct VirtualAPIC {
    uint32_t regs[VAPIC_REG_SIZE / 4];  // All registers
    uint32_t apic_id;
    bool     enabled;       // Software-enabled (SVR bit 8)
    bool     bsp;           // Bootstrap Processor flag

    // Timer emulation
    uint32_t timer_initial;
    uint32_t timer_current;
    uint32_t timer_divide;
    uint8_t  timer_mode;    // 0=one-shot, 1=periodic, 2=TSC-deadline
    uint8_t  timer_vector;
    bool     timer_masked;
    uint32_t timer_ticks;   // Simulation tick counter

    // ISR/IRR bitmaps (256 vectors)
    uint32_t isr[8];        // In-Service Register
    uint32_t irr[8];        // Interrupt Request Register
    uint32_t tmr[8];        // Trigger Mode Register

    void Reset();
    void WriteReg(uint32_t offset, uint32_t value);
    uint32_t ReadReg(uint32_t offset);
    void Tick(uint32_t elapsed_us);     // Advance timer
    void SendIPI(uint32_t icr_lo, uint32_t icr_hi);  // Inter-Processor Interrupt
    void InjectInterrupt(uint8_t vector);
    int  GetPendingVector();            // Returns highest-priority pending, or -1
    void EOI();                         // End of interrupt
};

// ═══════════════════════════════════════════════════════════════════════════
//  Virtual PIT — Intel 8254 Programmable Interval Timer
//  Ports 0x40-0x43: Channel 0/1/2 data, plus command register
// ═══════════════════════════════════════════════════════════════════════════

#define PIT_FREQ_HZ         1193182  // Base oscillator frequency

struct PITChannel {
    uint16_t reload_value;  // Reload / initial count
    uint16_t counter;       // Current count
    uint16_t latch_value;   // Latched count (for read-back)
    uint8_t  mode;          // Operating mode (0-5)
    uint8_t  access;        // Access mode: 0=latch, 1=lo, 2=hi, 3=lo-hi
    uint8_t  bcd;           // BCD mode flag
    bool     gate;          // Gate input
    bool     output;        // Output state
    bool     latched;       // Count latched for read
    bool     null_count;    // Count hasn't been loaded yet
    bool     flip_flop;     // For lo-hi access mode — tracks which byte
    uint32_t tick_accum;    // Sub-tick accumulator for precise timing
};

struct VirtualPIT {
    PITChannel channels[3]; // Channel 0=system timer, 1=DRAM refresh, 2=speaker
    bool       speaker_gate;
    uint32_t   irq0_count;  // Number of IRQ0 ticks generated

    void Reset();
    void WritePort(uint16_t port, uint8_t value);
    uint8_t ReadPort(uint16_t port);
    void Tick(uint32_t elapsed_us);  // Advance timer counters
    bool Channel0Fired();            // Did channel 0 generate an interrupt?
    uint32_t GetFrequencyHz(int channel);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Virtual HPET — High Precision Event Timer
//  MMIO at configurable base (typically 0xFED00000), 1024 bytes
// ═══════════════════════════════════════════════════════════════════════════

#define HPET_BASE_ADDR       0xFED00000
#define HPET_REG_SIZE        0x400

// HPET Register offsets
#define HPET_CAP_ID          0x000  // Capabilities and ID
#define HPET_CONFIG          0x010  // General Configuration
#define HPET_INT_STATUS      0x020  // General Interrupt Status
#define HPET_COUNTER         0x0F0  // Main Counter Value
#define HPET_TIMER0_CONFIG   0x100  // Timer 0 Config and Capabilities
#define HPET_TIMER0_COMP     0x108  // Timer 0 Comparator Value
#define HPET_TIMER0_FSB      0x110  // Timer 0 FSB Interrupt Route
#define HPET_TIMER1_CONFIG   0x120  // Timer 1 Config
#define HPET_TIMER1_COMP     0x128  // Timer 1 Comparator
#define HPET_TIMER2_CONFIG   0x140  // Timer 2 Config
#define HPET_TIMER2_COMP     0x148  // Timer 2 Comparator

// Timer config bits
#define HPET_TN_INT_TYPE     (1 << 1)   // 0=edge, 1=level
#define HPET_TN_INT_ENABLE   (1 << 2)   // Interrupt enable
#define HPET_TN_PERIODIC     (1 << 3)   // Periodic mode
#define HPET_TN_PERIODIC_CAP (1 << 4)   // Periodic capable
#define HPET_TN_SIZE_CAP     (1 << 5)   // 64-bit size capable
#define HPET_TN_VAL_SET      (1 << 6)   // Set accumulator (periodic)
#define HPET_TN_32MODE       (1 << 8)   // Force 32-bit mode
#define HPET_TN_INT_ROUTE    (0x1F << 9) // Interrupt routing

struct HPETTimer {
    uint64_t config;        // Configuration and capabilities
    uint64_t comparator;    // Comparator value
    uint64_t fsb_route;     // FSB interrupt route
    bool     int_pending;   // Interrupt pending
};

struct VirtualHPET {
    // Capability ID register (read-only)
    uint32_t rev_id;
    uint32_t num_timers;    // 3 timers typically
    uint32_t counter_size;  // 64-bit capable
    uint32_t vendor_id;
    uint32_t period_fs;     // Counter period in femtoseconds (100ns = 100,000,000 fs)

    // General config
    bool     enabled;       // Overall enable (HPET_CONFIG bit 0)
    bool     legacy_route;  // Legacy replacement routing (bit 1)

    // Main counter
    uint64_t counter;       // Free-running main counter
    uint64_t counter_offset;// Offset applied when counter is read

    // Timers
    HPETTimer timers[3];

    // Interrupt status
    uint32_t int_status;

    void Reset();
    void WriteReg(uint32_t offset, uint32_t value);
    uint32_t ReadReg(uint32_t offset);
    void Tick(uint32_t elapsed_us);
    int  CheckFiredTimers();  // Returns bitmask of timers that fired
};

// ═══════════════════════════════════════════════════════════════════════════
//  Virtual Device Manager — coordinates all virtual devices for a VM
// ═══════════════════════════════════════════════════════════════════════════

class VirtualDevices {
public:
    static void Init();

    // Port I/O handling (called from VM exit handler)
    static bool HandlePortIO(uint16_t port, bool is_out, uint8_t size,
                              uint32_t& value);

    // MMIO handling (called from EPT violation handler)
    static bool HandleMMIO(uint64_t phys_addr, bool is_write, uint8_t size,
                            uint32_t& value);

    // Timer ticks — advance all virtual timers
    static void Tick(uint32_t elapsed_us);

    // IRQ delivery — check if virtual devices want to inject interrupts
    static int GetPendingIRQ(); // Returns IRQ# or -1

    // Access individual devices
    static VirtualPIC&  GetMasterPIC();
    static VirtualPIC&  GetSlavePIC();
    static VirtualAPIC& GetAPIC();
    static VirtualPIT&  GetPIT();
    static VirtualHPET& GetHPET();

    // Debug
    static void DumpState();

private:
    static VirtualPIC  master_pic;
    static VirtualPIC  slave_pic;
    static VirtualAPIC apic;
    static VirtualPIT  pit;
    static VirtualHPET hpet;
    static bool        initialized;
};
