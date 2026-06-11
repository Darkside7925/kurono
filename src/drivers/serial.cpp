#include "serial.h"

void SerialLogger::Init() {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

void SerialLogger::Log(const char* s) {
    while (*s) {
        outb(0x3F8, *s++);
    }
}

void SerialLogger::LogHex(uint32_t n) {
    const char* hex = "0123456789ABCDEF";
    Log("0x");
    for (int i = 28; i >= 0; i -= 4) {
        outb(0x3F8, hex[(n >> i) & 0xF]);
    }
}

void SerialLogger::LogDec(int n) {
    if (n == 0) {
        Log("0");
        return;
    }
    if (n < 0) {
        Log("-");
        n = -n;
    }
    char buf[12];
    int i = 0;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    while (i > 0) {
        char c[2] = {buf[--i], 0};
        Log(c);
    }
}

void SerialLogger::outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
