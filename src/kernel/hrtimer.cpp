//  kurono os  -  high-resolution timer subsystem
//
//  see hrtimer.h for design notes. this implementation is intentionally
//  linear: 64 entries scanned on every Tick(). that costs ~64 cycles per
//  tick, far cheaper than the alternative red-black tree maintenance
//  for our scale, and lock-free because Tick() is the sole writer to
//  `active`/`fires`/`deadline_ms` for armed entries.
//
#include "hrtimer.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"

HRTimer::Entry HRTimer::table[HRTIMER_MAX];
uint32_t       HRTimer::next_id     = 1;
uint64_t       HRTimer::total_fires = 0;
bool           HRTimer::initialized = false;

static void hrt_scpy(char* d, const char* s, int mx) {
    int i = 0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int hrt_itoa(uint64_t v, char* out) {
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    char tmp[24]; int t = 0;
    while (v) { tmp[t++] = '0' + (int)(v % 10); v /= 10; }
    int n = 0;
    while (t > 0) out[n++] = tmp[--t];
    out[n] = 0;
    return n;
}

static int hrt_append(char* dst, int p, int mx, const char* s) {
    while (*s && p < mx - 1) dst[p++] = *s++;
    dst[p] = 0;
    return p;
}

static int hrt_append_u(char* dst, int p, int mx, uint64_t v) {
    char tmp[24];
    int n = hrt_itoa(v, tmp);
    if (p + n >= mx - 1) n = mx - p - 1;
    for (int i = 0; i < n; i++) dst[p++] = tmp[i];
    dst[p] = 0;
    return p;
}

void HRTimer::Init() {
    if (initialized) return;
    for (int i = 0; i < HRTIMER_MAX; i++) {
        table[i].active   = false;
        table[i].periodic = false;
        table[i].id       = 0;
        table[i].fires    = 0;
        table[i].name[0]  = 0;
    }
    next_id     = 1;
    total_fires = 0;
    initialized = true;
    SerialLogger::Log("[HRTimer] initialized (");
    SerialLogger::LogDec(HRTIMER_MAX);
    SerialLogger::Log(" slots)\r\n");
}

static int hrt_find_free() {
    for (int i = 0; i < HRTimer::HRTIMER_MAX; i++) {
        if (!HRTimer::table[i].active) return i;
    }
    return -1;
}

uint32_t HRTimer::Add(const char* name, uint32_t ms_from_now, Callback cb, void* ctx) {
    if (!initialized) Init();
    int idx = hrt_find_free();
    if (idx < 0) return 0;
    Entry& e = table[idx];
    e.id          = next_id++;
    e.active      = true;
    e.periodic    = false;
    e.deadline_ms = Timer::GetRealMs() + ms_from_now;
    e.interval_ms = 0;
    e.cb          = cb;
    e.ctx         = ctx;
    e.fires       = 0;
    hrt_scpy(e.name, name ? name : "unnamed", sizeof(e.name));
    return e.id;
}

uint32_t HRTimer::AddPeriodic(const char* name, uint32_t interval_ms, Callback cb, void* ctx) {
    if (!initialized) Init();
    if (interval_ms == 0) return 0;
    int idx = hrt_find_free();
    if (idx < 0) return 0;
    Entry& e = table[idx];
    e.id          = next_id++;
    e.active      = true;
    e.periodic    = true;
    e.deadline_ms = Timer::GetRealMs() + interval_ms;
    e.interval_ms = interval_ms;
    e.cb          = cb;
    e.ctx         = ctx;
    e.fires       = 0;
    hrt_scpy(e.name, name ? name : "periodic", sizeof(e.name));
    return e.id;
}

bool HRTimer::Cancel(uint32_t id) {
    for (int i = 0; i < HRTIMER_MAX; i++) {
        if (table[i].active && table[i].id == id) {
            table[i].active = false;
            return true;
        }
    }
    return false;
}

void HRTimer::Tick() {
    if (!initialized) return;
    uint32_t now = Timer::GetRealMs();
    for (int i = 0; i < HRTIMER_MAX; i++) {
        Entry& e = table[i];
        if (!e.active) continue;
        // unsigned compare: if (deadline - now) underflows, deadline has passed.
        if ((int32_t)(now - e.deadline_ms) < 0) continue;

        // fire
        Callback cb = e.cb;
        void*    cx = e.ctx;
        uint32_t id = e.id;
        e.fires++;
        total_fires++;
        if (e.periodic) {
            e.deadline_ms = now + e.interval_ms;
        } else {
            e.active = false;
        }
        if (cb) cb(id, cx);
    }
}

uint64_t HRTimer::TotalFires() { return total_fires; }

uint32_t HRTimer::ActiveCount() {
    uint32_t n = 0;
    for (int i = 0; i < HRTIMER_MAX; i++) if (table[i].active) n++;
    return n;
}

int HRTimer::DumpProcInfo(char* buf, int max_len) {
    if (!buf || max_len <= 0) return 0;
    int p = 0;
    p = hrt_append(buf, p, max_len,
        "Timer List Version: kurono v1\n"
        "HRTIMER_MAX_CLOCK_BASES: 1\n"
        "ACTIVE: ");
    p = hrt_append_u(buf, p, max_len, ActiveCount());
    p = hrt_append(buf, p, max_len, " / ");
    p = hrt_append_u(buf, p, max_len, (uint64_t)HRTIMER_MAX);
    p = hrt_append(buf, p, max_len, "\nNOW (ms): ");
    p = hrt_append_u(buf, p, max_len, Timer::GetRealMs());
    p = hrt_append(buf, p, max_len, "\nTOTAL FIRES: ");
    p = hrt_append_u(buf, p, max_len, total_fires);
    p = hrt_append(buf, p, max_len, "\n\n");

    int slot = 0;
    for (int i = 0; i < HRTIMER_MAX; i++) {
        Entry& e = table[i];
        if (!e.active) continue;
        if (p > max_len - 96) break;
        p = hrt_append  (buf, p, max_len, "  #");
        p = hrt_append_u(buf, p, max_len, (uint64_t)slot++);
        p = hrt_append  (buf, p, max_len, " id=");
        p = hrt_append_u(buf, p, max_len, e.id);
        p = hrt_append  (buf, p, max_len, " name=\"");
        p = hrt_append  (buf, p, max_len, e.name);
        p = hrt_append  (buf, p, max_len, e.periodic ? "\" type=periodic interval_ms=" : "\" type=oneshot interval_ms=");
        p = hrt_append_u(buf, p, max_len, e.interval_ms);
        p = hrt_append  (buf, p, max_len, " deadline_ms=");
        p = hrt_append_u(buf, p, max_len, e.deadline_ms);
        p = hrt_append  (buf, p, max_len, " fires=");
        p = hrt_append_u(buf, p, max_len, e.fires);
        p = hrt_append  (buf, p, max_len, "\n");
    }
    if (slot == 0) p = hrt_append(buf, p, max_len, "  (no active timers)\n");
    return p;
}
