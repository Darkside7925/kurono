#pragma once
#include "../kernel/types.h"

class Timer {
public:
    static void Init(uint32_t hz);
    static void WaitMs(uint32_t ms);
    static uint32_t GetTicks();       // returns real ms since init (pit-polled)
    static uint16_t ReadCounter();

    // real-time tracking via pit counter polling (no irq needed)
    static void     PollUpdate();     // call frequently to track elapsed ticks
    static uint32_t GetRealMs();      // total real milliseconds since init
    static uint32_t ElapsedSinceLast(); // ms elapsed since last call to this fn
    static uint32_t GetHz()   { return pit_freq; }

private:
    static volatile uint32_t ticks;
    static inline void outb_io(uint16_t port, uint8_t val);
    static inline uint8_t inb_io(uint16_t port);
    static uint16_t pit_div;
    static uint32_t pit_freq;

    // pit polling state
    static uint16_t last_counter;
    static uint32_t accum_ticks;     // sub-ms accumulated pit ticks
    static uint32_t total_real_ms;   // total real ms since init
    static uint32_t last_elapsed_ms; // snapshot for elapsedsincelast
};
