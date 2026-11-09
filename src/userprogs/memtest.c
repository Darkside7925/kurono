// memtest - fill most of free ram from ring 3 and verify every word.
//
// The physical memory manager used to stop at 16 GB. Now that it manages all
// the ram (up to the 512 GB identity map), this is the user-space proof: map
// anonymous chunks until a share of free memory is used, write a pattern that
// depends on the virtual address of every 8-byte word, then read it all back.
// A frame handed to two pages, a page table pointing at the wrong frame, or a
// kernel identity write landing in a user page all show up as a mismatch.
//
// The kernel side (kurono.memtest=1) samples how many frames above 16 GB are in
// use while this runs and prints the peak, so a PASS also says the test really
// lived above the old cap.
//
// usage: memtest [percent-of-free]   (default 75)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#define CHUNK   (256UL << 20)
#define MAXCH   4096

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logu(unsigned long v) {
    char t[24]; int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    char b[24]; int n = 0;
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}
static void logx(unsigned long v) {
    char b[18]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    (void)!write(2, b, 18);
}

static inline uint64_t pat(uint64_t va, uint64_t seed) {
    return (va * 0x9E3779B97F4A7C15ULL) ^ seed;
}

static uint64_t* g_ch[MAXCH];

static unsigned long fill(int nch, uint64_t seed) {
    for (int c = 0; c < nch; c++) {
        uint64_t* p = g_ch[c];
        for (unsigned long w = 0; w < CHUNK / 8; w++) p[w] = pat((uint64_t)(uintptr_t)&p[w], seed);
    }
    return 0;
}

static unsigned long verify(int nch, uint64_t seed) {
    unsigned long bad = 0;
    for (int c = 0; c < nch; c++) {
        uint64_t* p = g_ch[c];
        for (unsigned long w = 0; w < CHUNK / 8; w++) {
            uint64_t want = pat((uint64_t)(uintptr_t)&p[w], seed);
            if (p[w] != want) {
                if (bad < 8) {
                    logs("[memtest] MISMATCH va="); logx((unsigned long)(uintptr_t)&p[w]);
                    logs(" got="); logx(p[w]); logs(" want="); logx(want); logs("\n");
                }
                bad++;
            }
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    unsigned long pct = 75;
    if (argc > 1) { pct = strtoul(argv[1], 0, 10); if (pct < 1 || pct > 95) pct = 75; }
    struct sysinfo si;
    if (sysinfo(&si) != 0) { logs("[memtest] sysinfo failed\n"); return 2; }
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    unsigned long total = si.totalram * unit, freeb = si.freeram * unit;
    unsigned long target = freeb / 100 * pct;
    logs("[memtest] totalram_mb="); logu(total >> 20);
    logs(" freeram_mb="); logu(freeb >> 20);
    logs(" target_mb="); logu(target >> 20); logs("\n");

    int nch = 0;
    unsigned long have = 0;
    while (have + CHUNK <= target && nch < MAXCH) {
        void* p = mmap(0, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { logs("[memtest] mmap stopped at chunk "); logu((unsigned long)nch); logs("\n"); break; }
        g_ch[nch++] = (uint64_t*)p;
        have += CHUNK;
    }
    if (nch == 0) { logs("[memtest] no memory mapped\n"); return 2; }
    logs("[memtest] mapped_mb="); logu(have >> 20);
    logs(" chunks="); logu((unsigned long)nch);
    logs(" first="); logx((unsigned long)(uintptr_t)g_ch[0]); logs("\n");

    const uint64_t s1 = 0x5A5A0F0F12345678ULL, s2 = ~0x0123456789ABCDEFULL;
    fill(nch, s1);
    logs("[memtest] pass1 written, verifying\n");
    unsigned long bad = verify(nch, s1);
    logs("[memtest] pass1 mismatches="); logu(bad); logs("\n");
    // hold the peak so the kernel sampler sees it, then a second, inverted pass. (satoru)
    logs("[memtest] PEAK holding\n");
    usleep(1500000);
    fill(nch, s2);
    unsigned long bad2 = verify(nch, s2);
    logs("[memtest] pass2 mismatches="); logu(bad2); logs("\n");
    for (int c = 0; c < nch; c++) munmap(g_ch[c], CHUNK);
    logs(bad + bad2 == 0 ? "[memtest] OVERALL PASS mb=" : "[memtest] OVERALL FAIL mb=");
    logu(have >> 20); logs("\n");
    return bad + bad2 == 0 ? 0 : 1;
}
// end (satoru)
