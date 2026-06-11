#pragma once
#include "../kernel/types.h"

class Timer {
public:
    static void Init(uint32_t hz);
    static void WaitMs(uint32_t ms);
    static uint32_t GetTicks();       // Returns real ms since Init (PIT-polled)
    static uint16_t ReadCounter();

    // Real-time tracking via PIT counter polling (no IRQ needed)
    static void     PollUpdate();     // Call frequently to track elapsed ticks
    static uint32_t GetRealMs();      // Total real milliseconds since Init
    static uint32_t ElapsedSinceLast(); // Ms elapsed since last call to this fn
    static uint32_t GetHz()   { return pit_freq; }

private:
    static volatile uint32_t ticks;
    static inline void outb_io(uint16_t port, uint8_t val);
    static inline uint8_t inb_io(uint16_t port);
    static uint16_t pit_div;
    static uint32_t pit_freq;

    // PIT polling state
    static uint16_t last_counter;
    static uint32_t accum_ticks;     // sub-ms accumulated PIT ticks
    static uint32_t total_real_ms;   // total real ms since Init
    static uint32_t last_elapsed_ms; // snapshot for ElapsedSinceLast
};
