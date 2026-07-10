#pragma once
//  kurono os - virtual device emulation
//  emulated pic (8259a), apic, pit (8254), hpet for vm guests
#include "../kernel/types.h"

//  virtual pic - intel 8259a programmable interrupt controller
//  two pics: master (ports 0x20-0x21) and slave (ports 0xa0-0xa1)

struct VirtualPIC {
    uint8_t  icw1;         // icw1: edge/level, single/cascade, icw4 needed
    uint8_t  icw2;         // icw2: interrupt vector base
    uint8_t  icw3;         // icw3: cascade mask (master) / slave id
    uint8_t  icw4;         // icw4: 8086 mode, auto-eoi, buffered, sfnm
    int      icw_step;     // current icw being programmed (0=done, 1-4=icw#)
    bool     init_mode;    // currently in initialization sequence

    uint8_t  irr;          // interrupt request register
    uint8_t  isr;          // in-service register
    uint8_t  imr;          // interrupt mask register
    uint8_t  vector_base;  // base vector number (set by icw2)
    uint8_t  priority;     // lowest priority level for rotation
    bool     auto_eoi;     // auto-eoi mode (set by icw4 bit 1)
    bool     special_mask; // special mask mode
    bool     poll_mode;    // poll mode
    bool     rotate_on_eoi;// rotate on auto-eoi

    bool     is_master;
    uint8_t  cascade_mask; // master: which irqs have slaves; slave: id

    uint8_t  elcr;         // per-irq edge(0)/level(1) trigger

    void Reset();
    void WritePort(uint16_t port, uint8_t value);
    uint8_t ReadPort(uint16_t port);
    int GetHighestPriorityIRQ();      // returns irq# or -1
    void SetIRQ(int irq, bool level); // assert/deassert irq line
    void AcknowledgeIRQ(int irq);     // eoi
};

//  virtual apic - advanced programmable interrupt controller
//  mmio at 0xfee00000 (4kb region), per-cpu local apic

#define VAPIC_BASE_ADDR     0xFEE00000
#define VAPIC_REG_SIZE      0x400  // 1024 bytes of registers

// register offsets (aligned to 16 bytes)
#define APIC_ID             0x020
#define APIC_VERSION        0x030
#define APIC_TPR            0x080  // task priority register
#define APIC_APR            0x090  // arbitration priority register
#define APIC_PPR            0x0A0  // processor priority register
#define APIC_EOI            0x0B0  // end of interrupt
#define APIC_RRD            0x0C0  // remote read
#define APIC_LDR            0x0D0  // logical destination register
#define APIC_DFR            0x0E0  // destination format register
#define APIC_SVR            0x0F0  // spurious interrupt vector register
#define APIC_ISR_BASE       0x100  // in-service register (8 × 32-bit)
#define APIC_TMR_BASE       0x180  // trigger mode register
#define APIC_IRR_BASE       0x200  // interrupt request register
#define APIC_ESR            0x280  // error status register
#define APIC_ICR_LO         0x300  // interrupt command register (low)
#define APIC_ICR_HI         0x310  // interrupt command register (high)
#define APIC_LVT_TIMER      0x320
#define APIC_LVT_THERMAL    0x330
#define APIC_LVT_PERFCNT    0x340
#define APIC_LVT_LINT0      0x350
#define APIC_LVT_LINT1      0x360
#define APIC_LVT_ERROR      0x370
#define APIC_TIMER_INIT     0x380  // initial count register
#define APIC_TIMER_CURRENT  0x390  // current count register
#define APIC_TIMER_DIVIDE   0x3E0  // divide configuration register

// timer modes
#define APIC_TIMER_ONE_SHOT     0
#define APIC_TIMER_PERIODIC     1
#define APIC_TIMER_TSC_DEADLINE 2

struct VirtualAPIC {
    uint32_t regs[VAPIC_REG_SIZE / 4];  // all registers
    uint32_t apic_id;
    bool     enabled;       // software-enabled (svr bit 8)
    bool     bsp;           // bootstrap processor flag

    // timer emulation
    uint32_t timer_initial;
    uint32_t timer_current;
    uint32_t timer_divide;
    uint8_t  timer_mode;    // 0=one-shot, 1=periodic, 2=tsc-deadline
    uint8_t  timer_vector;
    bool     timer_masked;
    uint32_t timer_ticks;   // simulation tick counter

    // isr/irr bitmaps (256 vectors)
    uint32_t isr[8];        // in-service register
    uint32_t irr[8];        // interrupt request register
    uint32_t tmr[8];        // trigger mode register

    void Reset();
    void WriteReg(uint32_t offset, uint32_t value);
    uint32_t ReadReg(uint32_t offset);
    void Tick(uint32_t elapsed_us);     // advance timer
    void SendIPI(uint32_t icr_lo, uint32_t icr_hi);  // inter-processor interrupt
    void InjectInterrupt(uint8_t vector);
    int  GetPendingVector();            // returns highest-priority pending, or -1
    void EOI();                         // end of interrupt
};

//  virtual pit - intel 8254 programmable interval timer
//  ports 0x40-0x43: channel 0/1/2 data, plus command register

#define PIT_FREQ_HZ         1193182  // base oscillator frequency

struct PITChannel {
    uint16_t reload_value;  // reload / initial count
    uint16_t counter;       // current count
    uint16_t latch_value;   // latched count (for read-back)
    uint8_t  mode;          // operating mode (0-5)
    uint8_t  access;        // access mode: 0=latch, 1=lo, 2=hi, 3=lo-hi
    uint8_t  bcd;           // bcd mode flag
    bool     gate;          // gate input
    bool     output;        // output state
    bool     latched;       // count latched for read
    bool     null_count;    // count hasn't been loaded yet
    bool     flip_flop;     // for lo-hi access mode - tracks which byte
    uint32_t tick_accum;    // sub-tick accumulator for precise timing
};

struct VirtualPIT {
    PITChannel channels[3]; // channel 0=system timer, 1=dram refresh, 2=speaker
    bool       speaker_gate;
    uint32_t   irq0_count;  // number of irq0 ticks generated

    void Reset();
    void WritePort(uint16_t port, uint8_t value);
    uint8_t ReadPort(uint16_t port);
    void Tick(uint32_t elapsed_us);  // advance timer counters
    bool Channel0Fired();            // did channel 0 generate an interrupt?
    uint32_t GetFrequencyHz(int channel);
};

//  virtual hpet - high precision event timer
//  mmio at configurable base (typically 0xfed00000), 1024 bytes

#define HPET_BASE_ADDR       0xFED00000
#define HPET_REG_SIZE        0x400

// hpet register offsets
#define HPET_CAP_ID          0x000  // capabilities and id
#define HPET_CONFIG          0x010  // general configuration
#define HPET_INT_STATUS      0x020  // general interrupt status
#define HPET_COUNTER         0x0F0  // main counter value
#define HPET_TIMER0_CONFIG   0x100  // timer 0 config and capabilities
#define HPET_TIMER0_COMP     0x108  // timer 0 comparator value
#define HPET_TIMER0_FSB      0x110  // timer 0 fsb interrupt route
#define HPET_TIMER1_CONFIG   0x120  // timer 1 config
#define HPET_TIMER1_COMP     0x128  // timer 1 comparator
#define HPET_TIMER2_CONFIG   0x140  // timer 2 config
#define HPET_TIMER2_COMP     0x148  // timer 2 comparator

// timer config bits
#define HPET_TN_INT_TYPE     (1 << 1)   // 0=edge, 1=level
#define HPET_TN_INT_ENABLE   (1 << 2)   // interrupt enable
#define HPET_TN_PERIODIC     (1 << 3)   // periodic mode
#define HPET_TN_PERIODIC_CAP (1 << 4)   // periodic capable
#define HPET_TN_SIZE_CAP     (1 << 5)   // 64-bit size capable
#define HPET_TN_VAL_SET      (1 << 6)   // set accumulator (periodic)
#define HPET_TN_32MODE       (1 << 8)   // force 32-bit mode
#define HPET_TN_INT_ROUTE    (0x1F << 9) // interrupt routing

struct HPETTimer {
    uint64_t config;        // configuration and capabilities
    uint64_t comparator;    // comparator value
    uint64_t fsb_route;     // fsb interrupt route
    bool     int_pending;   // interrupt pending
};

struct VirtualHPET {
    // capability id register (read-only)
    uint32_t rev_id;
    uint32_t num_timers;    // 3 timers typically
    uint32_t counter_size;  // 64-bit capable
    uint32_t vendor_id;
    uint32_t period_fs;     // counter period in femtoseconds (100ns = 100,000,000 fs)

    // general config
    bool     enabled;       // overall enable (hpet_config bit 0)
    bool     legacy_route;  // legacy replacement routing (bit 1)

    // main counter
    uint64_t counter;       // free-running main counter
    uint64_t counter_offset;// offset applied when counter is read

    // timers
    HPETTimer timers[3];

    // interrupt status
    uint32_t int_status;

    void Reset();
    void WriteReg(uint32_t offset, uint32_t value);
    uint32_t ReadReg(uint32_t offset);
    void Tick(uint32_t elapsed_us);
    int  CheckFiredTimers();  // returns bitmask of timers that fired
};

//  virtual device manager - coordinates all virtual devices for a vm

class VirtualDevices {
public:
    static void Init();

    // port i/o handling (called from vm exit handler)
    static bool HandlePortIO(uint16_t port, bool is_out, uint8_t size,
                              uint32_t& value);

    // mmio handling (called from ept violation handler)
    static bool HandleMMIO(uint64_t phys_addr, bool is_write, uint8_t size,
                            uint32_t& value);

    // timer ticks - advance all virtual timers
    static void Tick(uint32_t elapsed_us);

    // irq delivery - check if virtual devices want to inject interrupts
    static int GetPendingIRQ(); // returns irq# or -1

    // access individual devices
    static VirtualPIC&  GetMasterPIC();
    static VirtualPIC&  GetSlavePIC();
    static VirtualAPIC& GetAPIC();
    static VirtualPIT&  GetPIT();
    static VirtualHPET& GetHPET();

    // debug
    static void DumpState();

private:
    static VirtualPIC  master_pic;
    static VirtualPIC  slave_pic;
    static VirtualAPIC apic;
    static VirtualPIT  pit;
    static VirtualHPET hpet;
    static bool        initialized;
};
