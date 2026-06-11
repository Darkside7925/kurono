#include "timer.h"
#include "../proc/scheduler.h"
#include "cpu_detect.h"   // TSC frequency for a VMware-robust ms clock (satoru)

uint16_t Timer::pit_div;
uint32_t Timer::pit_freq;
volatile uint32_t Timer::ticks = 0;
uint16_t Timer::last_counter = 0;
uint32_t Timer::accum_ticks = 0;
uint32_t Timer::total_real_ms = 0;
uint32_t Timer::last_elapsed_ms = 0;

void Timer::Init(uint32_t hz) {
    pit_freq = hz;
    pit_div = (uint16_t)(1193182u / hz);
    if (pit_div == 0) pit_div = 1;
    outb_io(0x43, 0x34);              // channel 0, lo/hi, rate generator
    outb_io(0x40, (uint8_t)(pit_div & 0xFF));
    outb_io(0x40, (uint8_t)((pit_div >> 8) & 0xFF));

    // initialize polling state
    ticks = 0;
    last_counter = ReadCounter();
    accum_ticks = 0;
    total_real_ms = 0;
    last_elapsed_ms = 0;
}

// call this frequently (every frame) to accumulate real pit ticks
void Timer::PollUpdate() {
    uint16_t cur = ReadCounter();
    // pit counts down from pit_div to 0
    uint16_t diff;
    if (last_counter >= cur) {
        diff = last_counter - cur;
    } else {
        // counter wrapped around (crossed reload value)
        diff = last_counter + pit_div - cur;
    }
    last_counter = cur;

    // accumulate sub-ms pit ticks
    accum_ticks += diff;

    // convert accumulated ticks to milliseconds
    // pit_div ticks = 1 ms (at programmed frequency)
    while (accum_ticks >= pit_div) {
        accum_ticks -= pit_div;
        total_real_ms++;
        ticks++;
    }
}

uint32_t Timer::GetRealMs() { return (uint32_t)GetRealMs64(); }

uint64_t Timer::GetRealMs64() {
    // once the preemptive scheduler is running, source the millisecond clock
    // from the PIT-IRQ-advanced g_sched_now_ms (via Scheduler::NowMs). the
    // polled PollUpdate() clock loses whole PIT reload periods between calls,
    // so sampling it at ~1ms cadence (e.g. a SleepMs(1) frame pacer) makes it
    // crawl far behind wall-clock  -  which froze the gui at "FPS 0". the IRQ
    // clock is monotonic and cadence-independent. before Start() (early boot)
    // we still use the polled clock since IRQs/scheduler aren't up yet. (satoru)
    // TSC-based monotonic ms clock. the PIT-IRQ-advanced g_sched_now_ms
    // (OnTimerTick(1) per IRQ) UNDERCOUNTS badly on VMware, which COALESCES
    // timer interrupts  -  fewer IRQs delivered means the clock crawls, so the
    // gui's 60fps pace + 250ms damage-gate fallback rarely fire and you get a
    // 5-10 minute black screen before the desktop crawls up. the polled PIT
    // counter instead loses whole reload periods between infrequent samples.
    // the TSC is immune to both: VMware exposes a constant-rate virtual TSC
    // independent of interrupt delivery, calibrated once at boot (CPUDetect,
    // via the precise PIT ch2 one-shot). read it directly. (satoru)
    static uint64_t tsc_per_ms = 0;
    static uint64_t tsc_base   = 0;
    static bool     tsc_ready  = false;
    if (!tsc_ready) {
        uint64_t f = CPUDetect::GetInfo().frequency.tsc_frequency;
        if (f >= 1000000ull) {            // sane (>= 1 MHz)
            tsc_per_ms = f / 1000ull;
            uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
            tsc_base = ((uint64_t)hi << 32) | lo;
            tsc_ready = true;
        }
    }
    if (tsc_ready) {
        uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t now = ((uint64_t)hi << 32) | lo;
        return (now - tsc_base) / tsc_per_ms;   // 64-bit: no 49.7-day wrap
    }
    // pre-CPUDetect early-boot fallback
    if (Scheduler::IsPreemptiveKernelSchedulerActive())
        return Scheduler::NowMs();
    PollUpdate();
    return total_real_ms;
}

uint32_t Timer::ElapsedSinceLast() {
    uint32_t now = GetRealMs();
    uint32_t elapsed = now - last_elapsed_ms;
    // clamp impossible deltas: the clock source switches once at boot
    // (polled -> TSC), which can produce a one-time backward/huge jump, and a
    // single call never legitimately spans >60s of real time. (satoru)
    if (elapsed > 60000u) elapsed = 0;
    last_elapsed_ms = now;
    return elapsed;
}

uint32_t Timer::GetTicks() {
    return GetRealMs();  // real ms (compatible with old code expecting ms)
}

void Timer::WaitMs(uint32_t ms) {
    uint32_t start = GetRealMs();
    while (GetRealMs() - start < ms) {
        __asm__ __volatile__("pause");
    }
}

uint16_t Timer::ReadCounter() {
    outb_io(0x43, 0x00);     // latch counter 0
    uint8_t lo = inb_io(0x40);
    uint8_t hi = inb_io(0x40);
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

void Timer::outb_io(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t Timer::inb_io(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
