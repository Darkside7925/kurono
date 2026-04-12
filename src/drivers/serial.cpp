#include "serial.h"
#include "../system/logging.h"

// helper: read byte from i/o port
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// wait for transmit holding register empty (thre) bit in lsr
static inline void wait_tx_ready() {
    // lsr = com1_base + 5 = 0x3fd, bit 5 = thre
    while ((inb(0x3F8 + 5) & 0x20) == 0) {
        // spin
    }
}

void SerialLogger::Init() {
    outb(0x3F8 + 1, 0x00);   // disable all interrupts
    outb(0x3F8 + 3, 0x80);   // enable dlab (set baud rate divisor)
    outb(0x3F8 + 0, 0x01);   // set divisor to 1 (115200 baud) lo byte
    outb(0x3F8 + 1, 0x00);   //   hi byte
    outb(0x3F8 + 3, 0x03);   // 8 bits, no parity, one stop bit
    outb(0x3F8 + 2, 0xC7);   // enable fifo, clear, 14-byte threshold
    outb(0x3F8 + 4, 0x0B);   // irqs enabled, rts/dsr set

    // self-test: verify uart exists by reading back lsr
    // if lsr reads 0xff, there's no uart  -  don't hang later
    uint8_t lsr = inb(0x3F8 + 5);
    if (lsr == 0xFF) {
        // no uart present  -  mark as unavailable
        // (future: set a flag to skip serial output)
    }
}

void SerialLogger::Log(const char* s) {
    const char* start = s;
    while (*s) {
        wait_tx_ready();
        outb(0x3F8, *s++);
    }
    RuntimeLog::MirrorSerial(start);
}

void SerialLogger::LogHex(uint32_t n) {
    const char* hex = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[n & 0xF];
        n >>= 4;
    }
    Log(buf);
}

void SerialLogger::LogDec(int n) {
    char buf[12];
    int i = 0;
    bool neg = false;
    if (n == 0) {
        Log("0");
        return;
    }
    if (n < 0) {
        neg = true;
        n = -n;
    }
    while (n > 0 && i < 11) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    if (neg && i < 11) buf[i++] = '-';
    char out[12];
    int o = 0;
    while (i > 0 && o < 11) out[o++] = buf[--i];
    out[o] = 0;
    Log(out);
}

void SerialLogger::outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
