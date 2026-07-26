#include "serial.h"
#include "../system/logging.h"
#include "../proc/smp.h"   // per-cpu line buffers for cross-core logging (satoru)

static bool g_serial_quiet = false;

void SerialLogger::SetQuiet(bool quiet) { g_serial_quiet = quiet; }
bool SerialLogger::IsQuiet() { return g_serial_quiet; }

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
    // if lsr reads 0xff, there's no uart - don't hang later
    uint8_t lsr = inb(0x3F8 + 5);
    if (lsr == 0xFF) {
        // no uart present - mark as unavailable
        // (future: set a flag to skip serial output)
    }
}

// smp: with multiple cores logging concurrently, raw per-char writes interleave
// into unreadable soup (the '[apr]' line came out as 'aLpirn]'). each cpu now
// accumulates into its own line buffer and flushes WHOLE LINES to the uart under
// a cross-core lock. the lock is only held for a completed line's transmit;
// same-cpu reentry (an exception logging mid-log) just flushes directly. (satoru)
static volatile uint32_t g_ser_lock_word  = 0;
static volatile int      g_ser_lock_owner = -1;
static char              g_ser_linebuf[SMP_MAX_CPUS][512];
static int               g_ser_linelen[SMP_MAX_CPUS] = {};

static inline void serial_outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

// monotonic boot-ms source for the line timestamps, wired by the kernel once
// the scheduler clock runs (lines print without a stamp until then). kept as
// a function pointer so serial.cpp needs no scheduler include. (satoru)
static uint64_t (*g_ser_now_ms)() = nullptr;
void SerialLogger::SetTimestampSource(uint64_t (*now_ms)()) { g_ser_now_ms = now_ms; }

static void serial_flush_line(uint32_t cpu) {
    int n = g_ser_linelen[cpu];
    if (n <= 0) return;
    int me = (int)cpu;
    bool nested = (g_ser_lock_owner == me);
    if (!nested) {
        for (;;) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&g_ser_lock_word, &expected, 1u, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            do { __asm__ __volatile__("pause" ::: "memory"); }
            while (__atomic_load_n(&g_ser_lock_word, __ATOMIC_RELAXED) != 0);
        }
        g_ser_lock_owner = me;
    }
    // per-line boot-ms timestamp prefix "[NNNNNN] " - turns every existing
    // probe line into a startup-profiling data point for ~10 extra chars. (satoru)
    if (g_ser_now_ms) {
        uint64_t ms = g_ser_now_ms();
        char digits[12]; int nd = 0;
        if (ms == 0) digits[nd++] = '0';
        while (ms > 0 && nd < 12) { digits[nd++] = (char)('0' + (ms % 10)); ms /= 10; }
        wait_tx_ready(); serial_outb(0x3F8, '[');
        while (nd > 0) { wait_tx_ready(); serial_outb(0x3F8, (uint8_t)digits[--nd]); }
        wait_tx_ready(); serial_outb(0x3F8, ']');
        wait_tx_ready(); serial_outb(0x3F8, ' ');
    }
    for (int i = 0; i < n; i++) {
        wait_tx_ready();
        serial_outb(0x3F8, (uint8_t)g_ser_linebuf[cpu][i]);
    }
    if (!nested) {
        g_ser_lock_owner = -1;
        __atomic_store_n(&g_ser_lock_word, 0u, __ATOMIC_RELEASE);
    }
    g_ser_linelen[cpu] = 0;
}

// panic-raw mode (see serial.h): direct uart writes, no locks/buffers/mirror. (satoru)
static volatile bool g_ser_panic_raw = false;
void SerialLogger::SetPanicRaw(bool on) { g_ser_panic_raw = on; }

void SerialLogger::Log(const char* s) {
    const char* start = s;
    if (g_ser_panic_raw) {
        // fatal-dump path: straight to the wire, BOUNDED tx wait, touch no
        // shared state at all - never block on another core's lock. (satoru)
        while (*s) {
            for (int t = 0; t < 100000; t++) {
                if (inb(0x3F8 + 5) & 0x20) break;
            }
            serial_outb(0x3F8, (uint8_t)*s++);
        }
        return;
    }
    if (!g_serial_quiet) {
        uint32_t cpu = SMP::CpuIndex();
        if (cpu >= SMP_MAX_CPUS) cpu = 0;
        while (*s) {
            char c = *s++;
            g_ser_linebuf[cpu][g_ser_linelen[cpu]++] = c;
            if (c == '\n' || g_ser_linelen[cpu] >= (int)sizeof(g_ser_linebuf[cpu])) {
                serial_flush_line(cpu);
            }
        }
    }
    // always mirror to runtime log so the in-kernel viewer is complete
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

void SerialLogger::LogHex64(uint64_t n) {
    // full 16-digit form: LogHex(uint32_t) silently truncated 64-bit user
    // addresses in diagnostics (a futex uaddr printed as 0x0B3AC628 when the
    // real address was 0x18000B3AC628).
    // byte-wise init ON PURPOSE: drivers/ compiles WITHOUT -mno-sse, and the
    // original string-literal init emitted a movaps that #GP'd on the
    // misaligned irq stack when the futex sweep logged a wedge (the p4/p5
    // "graph strings on the panic stack" kernel panics - it was THIS, not the
    // page walk). do not "clean this up" into an initializer. (satoru)
    const char* hex = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 17; i >= 2; i--) {
        buf[i] = (char)hex[n & 0xF];
        n >>= 4;
    }
    buf[18] = 0;
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
