#include "serial.h"
#include "../system/logging.h"
#include "../kernel/heap.h"
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

// boot log capture. every completed line (with its timestamp) is also appended
// here, linear and never wrapped, so a machine with no serial port can still
// get the whole boot log out: disklog.cpp copies it into KURONO.LOG on the
// kurono partition. appends happen under the serial lock or in panic-raw
// mode, so a plain length store is enough. (satoru)
// the ring is 256 kb by default (enough for the boot log and the panic tail);
// kurono.serlog=big moves it to 4 mb for a full session capture. (satoru)
static const uint32_t SER_CAP_SMALL = 256u * 1024u;
static const uint32_t SER_CAP_BIG   = 4u * 1024u * 1024u;
static char              g_ser_cap_small[SER_CAP_SMALL];
static char*             g_ser_cap = g_ser_cap_small;
static uint32_t          g_ser_cap_size = SER_CAP_SMALL;
static volatile uint32_t g_ser_cap_len = 0;

// session stream: every captured byte also gets a running stream offset, and
// once EnableStreamRing has run it lands in a power-of-two ring too, so the
// disk log can follow a whole session instead of stopping where the linear
// buffer above fills. same writers as the linear buffer (serial lock or
// panic-raw), so plain stores plus one release store of the total. (satoru)
static char*             g_ser_ring = nullptr;
static uint32_t          g_ser_ring_size = 0;
static volatile uint64_t g_ser_total = 0;

static void ser_cap_put(const char* p, int n) {
    uint32_t l = g_ser_cap_len;
    for (int i = 0; i < n && l < g_ser_cap_size; i++) g_ser_cap[l++] = p[i];
    __atomic_store_n(&g_ser_cap_len, l, __ATOMIC_RELEASE);
    uint64_t t = g_ser_total;
    char* r = g_ser_ring;
    if (r) {
        uint32_t m = g_ser_ring_size - 1;
        for (int i = 0; i < n; i++) r[(uint32_t)(t + (uint64_t)i) & m] = p[i];
    }
    __atomic_store_n(&g_ser_total, t + (uint64_t)(n > 0 ? n : 0), __ATOMIC_RELEASE);
}

// opt-in big capture (kurono.serlog=big). copies what was captured so far so
// the log stays continuous. failure just leaves the small ring in place. (satoru)
void SerialLogger::SetBigCapture(bool big) {
    if (!big || g_ser_cap != g_ser_cap_small) return;
    char* b = (char*)KernelHeap::Alloc(SER_CAP_BIG);
    if (!b) return;
    uint32_t n = __atomic_load_n(&g_ser_cap_len, __ATOMIC_ACQUIRE);
    if (n > SER_CAP_BIG) n = SER_CAP_BIG;
    for (uint32_t i = 0; i < n; i++) b[i] = g_ser_cap_small[i];
    __atomic_store_n(&g_ser_cap_len, n, __ATOMIC_RELEASE);
    g_ser_cap = b;
    g_ser_cap_size = SER_CAP_BIG;
}

// called after each completed line, outside the serial lock. disklog uses it
// during early boot to put every line on disk the moment it is printed, so a
// hard freeze still leaves the log current up to the line before it. (satoru)
static void (*volatile g_ser_line_hook)() = nullptr;
void SerialLogger::SetLineHook(void (*hook)()) { g_ser_line_hook = hook; }

const char* SerialLogger::CaptureData() { return g_ser_cap; }

// allocate the session ring and seed it from the linear capture, under the
// serial lock so no line lands half in each. bytes the linear buffer already
// dropped (it filled before this ran) stay zero in the ring. (satoru)
bool SerialLogger::EnableStreamRing(uint32_t bytes) {
    if (g_ser_ring) return true;
    if (bytes < 65536u || (bytes & (bytes - 1)) != 0) return false;
    char* r = (char*)KernelHeap::Alloc(bytes);
    if (!r) return false;
    for (uint32_t i = 0; i < bytes; i++) r[i] = 0;
    uint32_t me = SMP::CpuIndex();
    if (me >= SMP_MAX_CPUS) me = 0;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&g_ser_lock_word, &expected, 1u, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        do { __asm__ __volatile__("pause" ::: "memory"); }
        while (__atomic_load_n(&g_ser_lock_word, __ATOMIC_RELAXED) != 0);
    }
    g_ser_lock_owner = (int)me;   // a log line from an irq here takes the nested path (satoru)
    uint64_t t = g_ser_total;
    uint32_t l = g_ser_cap_len;
    // linear byte i is stream byte i; keep the newest ring-full of them (satoru)
    uint32_t from = (l > bytes) ? l - bytes : 0;
    for (uint32_t i = from; i < l && (uint64_t)i < t; i++) r[i & (bytes - 1)] = g_ser_cap[i];
    g_ser_ring_size = bytes;
    __atomic_store_n(&g_ser_ring, r, __ATOMIC_RELEASE);
    g_ser_lock_owner = -1;
    __atomic_store_n(&g_ser_lock_word, 0u, __ATOMIC_RELEASE);
    return true;
}
uint64_t SerialLogger::StreamTotal() { return __atomic_load_n(&g_ser_total, __ATOMIC_ACQUIRE); }
uint32_t SerialLogger::StreamRingSize() { return g_ser_ring_size; }

// copy stream bytes [from, from+n) out. returns how many are still valid:
// from the ring when there is one (0 when the writers already lapped it, so
// the caller skips ahead), else from the linear buffer. the lap check runs
// AFTER the copy, so a byte overwritten mid-copy is never reported. (satoru)
uint32_t SerialLogger::StreamRead(uint64_t from, char* dst, uint32_t n) {
    uint64_t t = __atomic_load_n(&g_ser_total, __ATOMIC_ACQUIRE);
    if (from >= t) return 0;
    if ((uint64_t)n > t - from) n = (uint32_t)(t - from);
    char* r = __atomic_load_n(&g_ser_ring, __ATOMIC_ACQUIRE);
    if (!r) {
        uint32_t l = __atomic_load_n(&g_ser_cap_len, __ATOMIC_ACQUIRE);
        if (from >= l) return 0;
        if ((uint64_t)n > l - from) n = (uint32_t)(l - from);
        for (uint32_t i = 0; i < n; i++) dst[i] = g_ser_cap[from + i];
        return n;
    }
    uint32_t sz = g_ser_ring_size, m = sz - 1;
    if (t - from > sz) return 0;
    for (uint32_t i = 0; i < n; i++) dst[i] = r[(uint32_t)(from + i) & m];
    uint64_t t2 = __atomic_load_n(&g_ser_total, __ATOMIC_ACQUIRE);
    if (t2 - from > sz) return 0;
    return n;
}
uint32_t SerialLogger::CaptureLen() { return __atomic_load_n(&g_ser_cap_len, __ATOMIC_ACQUIRE); }
uint32_t SerialLogger::CaptureCap() { return g_ser_cap_size; }

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
        char stamp[16]; int sp = 0;
        stamp[sp++] = '[';
        for (int k = nd; k > 0; k--) stamp[sp++] = digits[k - 1];
        stamp[sp++] = ']'; stamp[sp++] = ' ';
        ser_cap_put(stamp, sp);
        wait_tx_ready(); serial_outb(0x3F8, '[');
        while (nd > 0) { wait_tx_ready(); serial_outb(0x3F8, (uint8_t)digits[--nd]); }
        wait_tx_ready(); serial_outb(0x3F8, ']');
        wait_tx_ready(); serial_outb(0x3F8, ' ');
    }
    ser_cap_put(g_ser_linebuf[cpu], n);
    for (int i = 0; i < n; i++) {
        wait_tx_ready();
        serial_outb(0x3F8, (uint8_t)g_ser_linebuf[cpu][i]);
    }
    if (!nested) {
        g_ser_lock_owner = -1;
        __atomic_store_n(&g_ser_lock_word, 0u, __ATOMIC_RELEASE);
    }
    g_ser_linelen[cpu] = 0;
    void (*hook)() = g_ser_line_hook;
    if (hook && !nested && cpu == 0) hook();
}

// panic-raw mode (see serial.h): direct uart writes, no locks/buffers/mirror. (satoru)
static volatile bool g_ser_panic_raw = false;
void SerialLogger::SetPanicRaw(bool on) { g_ser_panic_raw = on; }

void SerialLogger::Log(const char* s) {
    const char* start = s;
    if (g_ser_panic_raw) {
        // fatal-dump path: straight to the wire, BOUNDED tx wait, touch no
        // shared state at all - never block on another core's lock. (satoru)
        // the capture append is lock-free on purpose: a racing core can at
        // worst garble a few bytes of a dump that would otherwise be lost. (satoru)
        { int n = 0; while (s[n]) n++; ser_cap_put(s, n); }
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
