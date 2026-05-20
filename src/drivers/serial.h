#pragma once
#include "../kernel/types.h"

class SerialLogger {
public:
    static void Init();
    static void Log(const char* s);
    static void LogHex(uint32_t n);
    static void LogDec(int n);

    // quiet mode: suppresses all Log/LogHex/LogDec output to COM1.
    // RuntimeLog mirroring is unaffected so the in-kernel `kurono log`
    // viewer still gets every line. honoured automatically when the
    // multiboot cmdline contains the token "quiet".
    static void SetQuiet(bool quiet);
    static bool IsQuiet();
private:
    static void outb(uint16_t port, uint8_t val);
};
