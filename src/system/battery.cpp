// battery: see battery.h. (satoru)
#include "battery.h"
#include "../fs/kvfs.h"
#include "../drivers/timer.h"

namespace Battery {
namespace {
bool     g_present = false;
int      g_pct = -1, g_min = -1;
State    g_state = S_UNKNOWN;
uint32_t g_at = 0;

void sapp(char* d, int cap, const char* s) { int n = 0; while (d[n]) n++; while (s && *s && n < cap - 1) d[n++] = *s++; d[n] = 0; }
void sdec(char* d, int cap, int v) {
    char t[12]; int n = 0;
    if (v <= 0) t[n++] = '0';
    while (v > 0 && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[12]; int k = 0; while (n) r[k++] = t[--n]; r[k] = 0;
    sapp(d, cap, r);
}
bool pre(const char* s, const char* p) { while (*p) if (*s++ != *p++) return false; return true; }
}  // namespace

void Tick() {
    uint32_t now = Timer::GetRealMs();
    if (g_at && now - g_at < 5000u) return;
    g_at = now ? now : 1;
    char b[96];
    int n = KVFS::ReadFile("/system/run/kld/battery", b, sizeof(b) - 1);
    if (n <= 0) { g_present = false; return; }
    b[n] = 0;
    int i = 0, pct = 0, nd = 0;
    while (b[i] >= '0' && b[i] <= '9' && nd < 3) { pct = pct * 10 + (b[i] - '0'); i++; nd++; }
    if (!nd || b[i] != '\t') { g_present = false; return; }
    i++;
    const char* st = b + i;
    g_state = pre(st, "charging") ? S_CHARGING : pre(st, "discharging") ? S_DISCHARGING
            : pre(st, "full") ? S_FULL : S_UNKNOWN;
    while (b[i] && b[i] != '\t' && b[i] != '\n') i++;
    int mins = -1;
    if (b[i] == '\t') {
        i++;
        if (b[i] >= '0' && b[i] <= '9') { mins = 0; while (b[i] >= '0' && b[i] <= '9') { mins = mins * 10 + (b[i] - '0'); i++; } }
    }
    g_pct = pct > 100 ? 100 : pct;
    g_min = mins;
    g_present = true;
}

bool Present() { return g_present; }
int Percent() { return g_pct; }
State GetState() { return g_state; }
int Minutes() { return g_min; }

void Describe(char* out, int cap) {
    out[0] = 0;
    if (!g_present) { sapp(out, cap, "No battery"); return; }
    if (g_state == S_CHARGING) sapp(out, cap, "Charging");
    else if (g_state == S_FULL) sapp(out, cap, "Fully charged");
    else { sdec(out, cap, g_pct); sapp(out, cap, "%"); }
    if (g_min > 0 && g_state != S_FULL) {
        sapp(out, cap, g_state == S_CHARGING ? ", " : ", ");
        if (g_min >= 60) { sdec(out, cap, g_min / 60); sapp(out, cap, " h "); }
        sdec(out, cap, g_min % 60); sapp(out, cap, " min");
        sapp(out, cap, g_state == S_CHARGING ? " until full" : " left");
    }
}

}  // namespace Battery

// end (satoru)
