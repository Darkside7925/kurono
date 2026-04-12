#include "timer.h"

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

uint32_t Timer::GetRealMs() {
    PollUpdate();
    return total_real_ms;
}

uint32_t Timer::ElapsedSinceLast() {
    PollUpdate();
    uint32_t elapsed = total_real_ms - last_elapsed_ms;
    last_elapsed_ms = total_real_ms;
    return elapsed;
}

uint32_t Timer::GetTicks() {
    PollUpdate();
    return total_real_ms;  // return real ms (compatible with old code expecting ms)
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
