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

void SerialLogger::Log(const char* s) {
    const char* start = s;
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
