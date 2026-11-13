#pragma once
#include "../kernel/types.h"

class SerialLogger {
public:
    static void Init();
    static void Log(const char* s);
    static void LogHex(uint32_t n);
    static void LogHex64(uint64_t n);   // full-width - LogHex silently truncated 64-bit addrs (satoru)
    static void LogDec(int n);

    // quiet mode: suppresses all Log/LogHex/LogDec output to COM1.
    // RuntimeLog mirroring is unaffected so the in-kernel `kurono log`
    // viewer still gets every line. honoured automatically when the
    // multiboot cmdline contains the token "quiet".
    static void SetQuiet(bool quiet);
    static bool IsQuiet();

    // panic-raw mode: Log/LogHex/LogDec write straight to the uart with
    // bounded tx waits - no per-cpu line buffer, no cross-core serial lock,
    // no RuntimeLog mirror. the fatal-fault dump sets this so it can never
    // block on a lock another (possibly wedged) core holds: an observed
    // lottery boot deadlocked the whole box when the fault dump waited on
    // log machinery while a sibling core spun for this core's tlb-shootdown
    // ack with interrupts off. (satoru)
    static void SetPanicRaw(bool on);

    // per-line boot-ms timestamps: every flushed serial line gets a
    // "[NNNNNN] " prefix once a monotonic clock source is registered
    // (Scheduler::NowMs at scheduler start). turns the whole boot log into
    // a startup profile for free. (satoru)
    static void SetTimestampSource(uint64_t (*now_ms)());

    // boot log capture: every flushed line (and panic-raw output) is also
    // appended to a 4 mb linear buffer, so bare metal without a uart can
    // still dump the full log to disk (drivers/disklog.cpp). (satoru)
    static const char* CaptureData();
    static uint32_t CaptureLen();
    static uint32_t CaptureCap();
    // enlarge the boot-log capture to 4 mb (kurono.serlog=big). (satoru)
    static void     SetBigCapture(bool big);
    // session stream (satoru): StreamTotal counts every byte ever captured;
    // EnableStreamRing(power of two) keeps the newest bytes in a heap ring so
    // the disk log can cover a whole session. StreamRead copies bytes by
    // stream offset and returns 0 once the ring has been lapped past them
    // (without a ring it reads the linear capture). (satoru)
    static bool     EnableStreamRing(uint32_t bytes);
    static uint64_t StreamTotal();
    static uint32_t StreamRingSize();
    static uint32_t StreamRead(uint64_t from, char* dst, uint32_t n);
    // per-line hook (cpu 0, outside the serial lock); null to clear. (satoru)
    static void SetLineHook(void (*hook)());
private:
    static void outb(uint16_t port, uint8_t val);
};
