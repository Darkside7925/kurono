#include "kmemx.h"
#include "kmemx_pool.h"
#include "kmemx_lz4.h"
#include "pmm.h"
#include "../system/ui_config.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/cpu_detect.h"
#include "../shell/shell.h"

//  KMemX toggle + config + disclaimer/status text (stage 11). (satoru)
//
//  the engine's persistent state lives in UIConfig under the kmemx.* keys
//  (read by Settings + this file) and is mirrored to a human-readable
//  /kurono/system/config/kmemx.conf the installer + boot path use. enable/disable
//  orchestrate the worker process + a full pool drain on disable. all the
//  user-facing disclaimer/status strings are assembled here so the shell, the
//  Settings panel, and the installer share one source of truth. (satoru)

namespace KMemX {

namespace {
// ── tiny no-libc string helpers (this file builds user-facing text) ─────────
static int sappend(char* out, int pos, int mx, const char* s) {
    while (*s && pos < mx - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}
static int sappend_u(char* out, int pos, int mx, uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 24) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0 && pos < mx - 1) out[pos++] = tmp[--n];
    out[pos] = 0;
    return pos;
}
}  // namespace

void WriteConfFile() {
    // a small, readable conf mirroring the live config. the boot path logs from
    // it; the installer writes it directly. (satoru)
    char buf[256]; int p = 0;
    p = sappend(buf, p, sizeof(buf), "# kurono memory compression engine (KMemX)\n");
    p = sappend(buf, p, sizeof(buf), "enabled=");
    p = sappend(buf, p, sizeof(buf), IsEnabled() ? "1\n" : "0\n");
    p = sappend(buf, p, sizeof(buf), "pool_pct=");
    p = sappend_u(buf, p, sizeof(buf), (uint64_t)PoolPct());
    p = sappend(buf, p, sizeof(buf), "\nthreshold=");
    p = sappend_u(buf, p, sizeof(buf), (uint64_t)Threshold());
    p = sappend(buf, p, sizeof(buf), "\n");
    KVFS::WriteString("/kurono/system/config/kmemx.conf", buf);
}

void ApplyConfig() {
    // pool size + aggressiveness first (so an enable starts with the right
    // params), then the enable bit. defaults: enabled, 20% pool, threshold 8. (satoru)
    int pct = UIConfig::Int("kmemx.pool_pct", 20);
    int thr = UIConfig::Int("kmemx.threshold", 8);
    SetPoolPct(pct);
    SetThreshold(thr);

    bool want = UIConfig::Bool("kmemx.enabled", true);
    SetEnabled(want);

    if (want) {
        uint64_t mb = KMemXPool::TotalBytes() / (1024 * 1024);
        SerialLogger::Log("[KMemX] starting compression engine... pool=");
        SerialLogger::LogDec((int)mb);
        SerialLogger::Log("MB\r\n");
        StartProcess();
    } else {
        SerialLogger::Log("[KMemX] disabled by user configuration\r\n");
    }
    WriteConfFile();
}

bool Enable() {
    UIConfig::SetInt("kmemx.enabled", 1, true);
    UIConfig::Save();
    SetEnabled(true);
    StartProcess();
    WriteConfFile();
    SerialLogger::Log("[KMemX] enabled by user\r\n");
    return true;
}

int Disable() {
    // stop scanning first so the worker does not re-compress while we drain. (satoru)
    SetEnabled(false);
    int restored = DecompressAll();   // pull every pooled page back into ram (satoru)
    UIConfig::SetInt("kmemx.enabled", 0, true);
    UIConfig::Save();
    WriteConfFile();
    SerialLogger::Log("[KMemX] disabled by user; pages restored=");
    SerialLogger::LogDec(restored);
    SerialLogger::Log("\r\n");
    return restored;
}

int EnableDisclaimer(char* out, int mx, uint64_t ram_mb) {
    int p = 0;
    p = sappend(out, p, mx, "Enable KMemX memory compression?\n\n");
    p = sappend(out, p, mx,
        "KMemX transparently compresses inactive memory so you can run more "
        "apps in the same RAM:\n");
    p = sappend(out, p, mx, "  - Firefox idle: ~500 MB -> ~50 MB physical\n");
    p = sappend(out, p, mx, "  - Linux guests (Alpine/Debian): up to ~70% smaller\n");
    p = sappend(out, p, mx, "  - The base OS stays tiny; freed RAM runs more apps\n\n");
    p = sappend(out, p, mx, "Trade-offs:\n");
    p = sappend(out, p, mx, "  - 1-5% continuous CPU for the compression engine\n");
    p = sappend(out, p, mx, "  - Occasional ~5 us delay the first time a cold page is touched\n");
    p = sappend(out, p, mx, "  - Not recommended below 512 MB RAM\n\n");
    if (ram_mb > 0) {
        if (ram_mb < 512) {
            p = sappend(out, p, mx,
                "Your system has ");
            p = sappend_u(out, p, mx, ram_mb);
            p = sappend(out, p, mx, " MB RAM: KMemX is STRONGLY RECOMMENDED.\n\n");
        } else if (ram_mb > 4096) {
            p = sappend(out, p, mx, "Your system has ");
            p = sappend_u(out, p, mx, ram_mb);
            p = sappend(out, p, mx, " MB RAM: KMemX is optional (recommended).\n\n");
        } else {
            p = sappend(out, p, mx, "Your system has ");
            p = sappend_u(out, p, mx, ram_mb);
            p = sappend(out, p, mx, " MB RAM: KMemX is recommended.\n\n");
        }
    }
    p = sappend(out, p, mx, "If enabled, it starts automatically on every boot.\n");
    p = sappend(out, p, mx, "[Enable KMemX]   [Cancel]\n");
    return p;
}

int DisableDisclaimer(char* out, int mx) {
    int p = 0;
    p = sappend(out, p, mx, "Disable KMemX memory compression?\n\n");
    p = sappend(out, p, mx, "This will:\n");
    p = sappend(out, p, mx, "  - Decompress ALL compressed pages back into RAM now\n");
    p = sappend(out, p, mx, "  - Stop the compression engine process\n");
    p = sappend(out, p, mx, "  - Raise physical RAM usage immediately\n");
    p = sappend(out, p, mx, "  - May briefly slow the system while it decompresses\n");
    p = sappend(out, p, mx, "  - Takes a few seconds for a large compressed pool\n\n");
    p = sappend(out, p, mx, "KMemX will stay off on the next boot until you re-enable it.\n");
    p = sappend(out, p, mx, "[Disable KMemX]   [Cancel]\n");
    return p;
}

int StatusText(char* out, int mx) {
    const Stats& st = GetStats();
    int p = 0;
    p = sappend(out, p, mx, "KMemX (Kurono Memory Compression Engine)\n");
    p = sappend(out, p, mx, "  status:        ");
    p = sappend(out, p, mx, IsInitialized() ? (IsEnabled() ? "ENABLED\n" : "disabled\n") : "not initialized\n");
    p = sappend(out, p, mx, "  worker:        ");
    p = sappend(out, p, mx, ProcessRunning() ? "running\n" : "stopped\n");
    p = sappend(out, p, mx, "  pool size:     ");
    p = sappend_u(out, p, mx, st.pool_bytes / (1024 * 1024));
    p = sappend(out, p, mx, " MB (");
    p = sappend_u(out, p, mx, (uint64_t)PoolPct());
    p = sappend(out, p, mx, "% of RAM)\n");
    p = sappend(out, p, mx, "  pool used:     ");
    p = sappend_u(out, p, mx, st.pool_used / 1024);
    p = sappend(out, p, mx, " KB\n");
    p = sappend(out, p, mx, "  live pages:    ");
    p = sappend_u(out, p, mx, (uint64_t)st.live_pages);
    p = sappend(out, p, mx, "\n  compression:   ");
    p = sappend_u(out, p, mx, (uint64_t)RatioX100() / 100);
    p = sappend(out, p, mx, ".");
    p = sappend_u(out, p, mx, (uint64_t)(RatioX100() % 100));
    p = sappend(out, p, mx, ":1\n");
    p = sappend(out, p, mx, "  RAM saved:     ");
    p = sappend_u(out, p, mx, st.bytes_saved / 1024);
    p = sappend(out, p, mx, " KB\n");
    p = sappend(out, p, mx, "  dedup saved:   ");
    p = sappend_u(out, p, mx, (uint64_t)st.dedup_saved);
    p = sappend(out, p, mx, " pages\n");
    p = sappend(out, p, mx, "  faults served: ");
    p = sappend_u(out, p, mx, st.faults_served);
    p = sappend(out, p, mx, "\n  threshold:     gen>");
    p = sappend_u(out, p, mx, (uint64_t)Threshold());
    p = sappend(out, p, mx, "\n  pressure:      ");
    p = sappend(out, p, mx, PressureName(CurrentPressure()));
    p = sappend(out, p, mx, "\n  cpu budget:    ");
    p = sappend_u(out, p, mx, (uint64_t)TokenBudget());
    p = sappend(out, p, mx, " pages/10ms tick\n");
    if (st.faults_served > 0) {
        p = sappend(out, p, mx, "  decomp latency: min ");
        p = sappend_u(out, p, mx, st.ns_decompress_min);
        p = sappend(out, p, mx, " ns / mean ");
        p = sappend_u(out, p, mx, st.ns_decompress_sum / st.faults_served);
        p = sappend(out, p, mx, " ns / max ");
        p = sappend_u(out, p, mx, st.ns_decompress_max);
        p = sappend(out, p, mx, " ns\n  over-10us:     ");
        p = sappend_u(out, p, mx, st.decomp_over_10us);
        p = sappend(out, p, mx, "\n");
    }
    return p;
}

// ── benchmark ─────────────────────────────────────────────────────────────────
namespace {
static inline uint64_t bench_rdtsc() {
    uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// fill a page for a given type, mirroring the self-test generators but simpler.
static void bench_make(uint8_t* p, int kind, uint32_t seed) {
    uint32_t s = seed ? seed : 1;
    auto nx = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    switch (kind) {
        case 0: for (int i = 0; i < 4096; i++) p[i] = 0; break;                       // zero (satoru)
        case 1: { static const char* a = "the quick brown fox abcdefgh \n";          // text (satoru)
                  int n = 0; while (a[n]) n++;
                  for (int i = 0; i < 4096; i++) p[i] = (uint8_t)a[nx() % (uint32_t)n]; break; }
        case 2: { uint8_t r[64]; for (int i = 0; i < 64; i++) r[i] = (uint8_t)(nx() & 0xFF);  // structured (satoru)
                  for (int i = 0; i < 4096; i++) p[i] = r[i & 63]; break; }
        default: for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(nx() & 0xFF); break;  // random (satoru)
    }
}
}  // namespace

int Benchmark(char* out, int mx) {
    const int PAGE = 4096;
    const int CAP = KMemXLZ4::CompressBound(PAGE);
    uint8_t* src = (uint8_t*)PMM::AllocBytes(PAGE);
    uint8_t* cmp = (uint8_t*)PMM::AllocBytes((size_t)CAP);
    uint8_t* dec = (uint8_t*)PMM::AllocBytes(PAGE);
    void*    scr = PMM::AllocBytes((size_t)KMemXLZ4::SCRATCH_BYTES);
    if (!src || !cmp || !dec || !scr) {
        int p = sappend(out, 0, mx, "kmemx benchmark: alloc failed\n");
        return p;
    }
    uint64_t hz = CPUDetect::GetInfo().frequency.tsc_frequency;
    if (hz == 0) hz = 1000000000ULL;   // assume ~1ghz if unknown (satoru)

    int p = 0;
    p = sappend(out, p, mx, "KMemX micro-benchmark (synthetic 4KB pages)\n");
    p = sappend(out, p, mx, "  type        ratio   comp MB/s  decomp MB/s\n");

    static const char* names[4] = { "zero    ", "text    ", "struct  ", "random  " };
    const int ITERS = 2000;
    for (int kind = 0; kind < 4; kind++) {
        bench_make(src, kind, 0x1234u + (uint32_t)kind * 777u);
        // compress timing. (satoru)
        uint64_t c0 = bench_rdtsc();
        int csz = 0;
        for (int it = 0; it < ITERS; it++)
            csz = KMemXLZ4::Compress(src, PAGE, cmp, CAP, scr);
        uint64_t c1 = bench_rdtsc();
        // decompress timing (only if it compressed). (satoru)
        uint64_t d0 = 0, d1 = 0; int ratio_x100 = 100;
        if (csz > 0) {
            ratio_x100 = (PAGE * 100) / csz;
            d0 = bench_rdtsc();
            for (int it = 0; it < ITERS; it++)
                KMemXLZ4::Decompress(cmp, csz, dec, PAGE);
            d1 = bench_rdtsc();
        }
        // MB/s = bytes / seconds = (ITERS*4096) / (cycles/hz). (satoru)
        uint64_t cbytes = (uint64_t)ITERS * PAGE;
        uint64_t ccyc = c1 - c0; if (ccyc == 0) ccyc = 1;
        uint64_t dcyc = d1 - d0; if (dcyc == 0) dcyc = 1;
        uint64_t comp_mbps  = (cbytes * hz) / (ccyc * 1024 * 1024);
        uint64_t decomp_mbps = (csz > 0) ? (cbytes * hz) / (dcyc * 1024 * 1024) : 0;

        p = sappend(out, p, mx, "  ");
        p = sappend(out, p, mx, names[kind]);
        p = sappend(out, p, mx, "    ");
        p = sappend_u(out, p, mx, (uint64_t)ratio_x100 / 100);
        p = sappend(out, p, mx, ".");
        p = sappend_u(out, p, mx, (uint64_t)ratio_x100 % 100);
        p = sappend(out, p, mx, ":1   ");
        p = sappend_u(out, p, mx, comp_mbps);
        p = sappend(out, p, mx, "      ");
        p = sappend_u(out, p, mx, decomp_mbps);
        p = sappend(out, p, mx, "\n");
    }

    PMM::FreeBytes(src, PAGE);
    PMM::FreeBytes(cmp, (size_t)CAP);
    PMM::FreeBytes(dec, PAGE);
    PMM::FreeBytes(scr, (size_t)KMemXLZ4::SCRATCH_BYTES);
    return p;
}

// ── shell command ─────────────────────────────────────────────────────────────
namespace {
// parse a "key=value" arg's integer, accepting an optional trailing '%'. (satoru)
static int parse_int_arg(const char* s) {
    int v = 0; bool any = false;
    while (*s && *s != '=') s++;
    if (*s == '=') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = true; }
    return any ? v : -1;
}
static bool arg_is(const char* a, const char* b) {
    int i = 0; while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}
}  // namespace

int CmdKmemx(void* /*sh*/, int argc, const char** argv, char* out, int mx) {
    if (!IsInitialized()) {
        return sappend(out, 0, mx, "kmemx: engine not initialized\n");
    }
    const char* sub = (argc >= 2) ? argv[1] : "status";

    if (arg_is(sub, "status")) {
        return StatusText(out, mx);
    }
    if (arg_is(sub, "stats")) {
        // stats [--firefox|--guests]: the base stats plus a scope note. the
        // engine does not bucket by process name yet, so the flags annotate the
        // same live figures honestly. (satoru)
        int p = StatusText(out, mx);
        if (argc >= 3 && arg_is(argv[2], "--firefox"))
            p = sappend(out, p, mx, "(scope: linux runtime incl. firefox  -  per-process bucketing not yet tracked)\n");
        else if (argc >= 3 && arg_is(argv[2], "--guests")) {
            p = sappend(out, p, mx, "(scope: hypervisor guests  -  registered guests: ");
            p = sappend_u(out, p, mx, 0);   // guest count is internal; 0 when none (satoru)
            p = sappend(out, p, mx, ")\n");
        }
        return p;
    }
    if (arg_is(sub, "pressure")) {
        int p = sappend(out, 0, mx, "kmemx pressure: ");
        p = sappend(out, p, mx, PressureName(UpdatePressure()));
        p = sappend(out, p, mx, "  (cpu budget ");
        p = sappend_u(out, p, mx, (uint64_t)TokenBudget());
        p = sappend(out, p, mx, " pages/tick, threshold gen>");
        p = sappend_u(out, p, mx, (uint64_t)Threshold());
        p = sappend(out, p, mx, ")\n");
        return p;
    }
    if (arg_is(sub, "compress")) {
        if (argc >= 3 && arg_is(argv[2], "--all")) {
            // compress every eligible user process. (satoru)
            int total = 0;
            for (uint32_t pid = 1; pid < 4096; pid++) total += CompressProcess(pid);
            int p = sappend(out, 0, mx, "kmemx: compressed ");
            p = sappend_u(out, p, mx, (uint64_t)total);
            p = sappend(out, p, mx, " pages across all processes\n");
            return p;
        }
        if (argc >= 3 && arg_is(argv[2], "--guests")) {
            int n = CompressGuests(100000);
            int p = sappend(out, 0, mx, "kmemx: compressed ");
            p = sappend_u(out, p, mx, (uint64_t)n);
            p = sappend(out, p, mx, " guest pages\n");
            return p;
        }
        if (argc >= 3) {
            int pid = parse_int_arg(argv[2][0] >= '0' && argv[2][0] <= '9' ? argv[2] : "");
            if (pid < 0) { int v = 0; const char* s = argv[2]; while (*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} pid = v; }
            int n = CompressProcess((uint32_t)pid);
            int p = sappend(out, 0, mx, "kmemx: compressed ");
            p = sappend_u(out, p, mx, (uint64_t)n);
            p = sappend(out, p, mx, " pages of pid ");
            p = sappend_u(out, p, mx, (uint64_t)pid);
            p = sappend(out, p, mx, "\n");
            return p;
        }
        return sappend(out, 0, mx, "usage: kmemx compress <pid> | --all | --guests\n");
    }
    if (arg_is(sub, "decompress")) {
        if (argc >= 3) {
            int v = 0; const char* s = argv[2]; while (*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;}
            int n = DecompressProcess((uint32_t)v);
            int p = sappend(out, 0, mx, "kmemx: decompressed ");
            p = sappend_u(out, p, mx, (uint64_t)n);
            p = sappend(out, p, mx, " pages of pid ");
            p = sappend_u(out, p, mx, (uint64_t)v);
            p = sappend(out, p, mx, "\n");
            return p;
        }
        return sappend(out, 0, mx, "usage: kmemx decompress <pid>\n");
    }
    if (arg_is(sub, "flush")) {
        int n = DecompressAll();
        int p = sappend(out, 0, mx, "kmemx: flushed (decompressed) ");
        p = sappend_u(out, p, mx, (uint64_t)n);
        p = sappend(out, p, mx, " pages back into RAM\n");
        return p;
    }
    if (arg_is(sub, "config")) {
        if (argc >= 3) {
            const char* a = argv[2];
            if (a[0]=='p'&&a[1]=='o'&&a[2]=='o'&&a[3]=='l') {
                int v = parse_int_arg(a);
                if (v >= 10 && v <= 40) { SetPoolPct(v); UIConfig::SetInt("kmemx.pool_pct", v, true); UIConfig::Save(); WriteConfFile();
                    int p = sappend(out, 0, mx, "kmemx: pool target set to "); p = sappend_u(out,p,mx,(uint64_t)v); return sappend(out,p,mx,"%\n"); }
                return sappend(out, 0, mx, "kmemx: pool must be 10..40 (percent)\n");
            }
            if (a[0]=='t'&&a[1]=='h') {
                int v = parse_int_arg(a);
                if (v >= 4 && v <= 16) { SetThreshold(v); UIConfig::SetInt("kmemx.threshold", v, true); UIConfig::Save(); WriteConfFile();
                    int p = sappend(out, 0, mx, "kmemx: generation threshold set to "); p = sappend_u(out,p,mx,(uint64_t)v); return sappend(out,p,mx,"\n"); }
                return sappend(out, 0, mx, "kmemx: threshold must be 4..16\n");
            }
            return sappend(out, 0, mx, "usage: kmemx config [pool=NN%] [threshold=N]\n");
        }
        // print current config. (satoru)
        int p = sappend(out, 0, mx, "kmemx config:\n  pool_pct=");
        p = sappend_u(out, p, mx, (uint64_t)PoolPct());
        p = sappend(out, p, mx, "\n  threshold=");
        p = sappend_u(out, p, mx, (uint64_t)Threshold());
        p = sappend(out, p, mx, "\n  enabled=");
        p = sappend(out, p, mx, IsEnabled() ? "1\n" : "0\n");
        return p;
    }
    if (arg_is(sub, "enable")) {
        // show the disclaimer, then enable (the shell is non-interactive here, so
        // we print the disclaimer + proceed; the GUI toggle is the interactive
        // path). (satoru)
        int p = EnableDisclaimer(out, mx, PMM::GetTotalMemory() / (1024 * 1024));
        Enable();
        p = sappend(out, p, mx, "\n-> KMemX enabled.\n");
        return p;
    }
    if (arg_is(sub, "disable")) {
        int p = DisableDisclaimer(out, mx);
        int n = Disable();
        p = sappend(out, p, mx, "\n-> KMemX disabled; decompressed ");
        p = sappend_u(out, p, mx, (uint64_t)n);
        p = sappend(out, p, mx, " pages.\n");
        return p;
    }
    if (arg_is(sub, "benchmark") || arg_is(sub, "bench")) {
        return Benchmark(out, mx);
    }

    return sappend(out, 0, mx,
        "usage: kmemx status|stats [--firefox|--guests]|pressure|\n"
        "       compress <pid>|--all|--guests | decompress <pid> | flush |\n"
        "       config [pool=NN%] [threshold=N] | enable | disable | benchmark\n");
}

void RegisterShellCommands(void* shell) {
    KuronoShell* sh = (KuronoShell*)shell;
    if (!sh) return;
    sh->RegisterCommand("kmemx", CmdKmemx, "kurono memory compression engine");
}

}  // namespace KMemX

// end (satoru)
