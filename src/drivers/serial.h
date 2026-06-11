#pragma once
#include "../kernel/types.h"

class SerialLogger {
public:
    static void Init();
    static void Log(const char* s);
    static void LogHex(uint32_t n);
    static void LogDec(int n);
private:
    static void outb(uint16_t port, uint8_t val);
};
