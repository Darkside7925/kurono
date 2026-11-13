// KLD display bridge implementation. see kld_display.h for the contract.
// (satoru)
//
// end (satoru)

#include "kld_display.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"
#include "../drivers/graphics.h"
#include "../drivers/outputs.h"
#include "../drivers/display_mgr.h"
#include "kld_broker.h"
#include "../kernel/vmm.h"
#include "pmm.h"   // PhysToVirt for the direct map (satoru)
#include "../ui/window_manager.h"
#include "../apps/system_settings.h"
#include "../linux/linux_syscall.h"   // KlsDropAll around the setup-race rig sleep (satoru)
#include "../ui/gfx_bench.h"

extern int LinuxSyscallFutexWakePhys(uint64_t phys_key, int max);

namespace KldDisplay {

namespace {

Spinlock g_lock;
Mode     g_mode = MODE_OFF;

bool     g_attached;
uint64_t g_as;
uint64_t g_ring_phys;
uint64_t g_scanout_phys;
uint32_t g_scanout_pages;
DispRing* g_ring;
uint8_t*  g_scanout;

bool     g_modes_ok;
uint32_t g_gen;
uint32_t g_w, g_h, g_pitch;
uint32_t g_current_mode;

// kernel-private copy of the container's mode table: every value is copied
// ONCE under the lock and validated; nothing after MODES reads the ring for
// geometry. (satoru)
DispMode g_modes[KLD_DISP_MAX_MODES];
uint32_t g_mode_n;

uint32_t g_disp_id;          // broker display instance (D2.8) (satoru)
uint32_t g_success_attach;   // only successful attaches count toward 32/boot (satoru)
bool     g_attach_lockout_logged;

bool     g_fuzz;
bool     g_stall;
bool     g_diepre;
bool     g_setupdelay;
bool     g_setuprdelay;
bool     g_killpre;
bool     g_attachloop;
bool     g_attach_unlimited;
bool     g_modedelay;
bool     g_setupfail;
bool     g_badcount;
int      g_detachdelay_ms;   // rig: widen the exit->detach window (satoru)
int      g_freedelay_ms;     // rig: hold the final free between batches (satoru)
bool     g_killonsetup;      // rig: exit the container as setup starts (satoru)
bool     g_killonsetup_sent;
bool     g_badunmap;      // rig: hostile munmap/madvise loop (satoru)
bool     g_patterncycle;  // rig: 20 translator mode changes in pattern mode (satoru)
bool     g_killpre_sent;
uint32_t g_attach_refusals;

uint32_t g_sent_seq;
uint32_t g_done_seq;
uint32_t g_done_count;
uint32_t g_drops;
bool     g_frame_in_flight;
bool     g_logged_no_route;
bool send_frame_locked(bool full, const DispRect* rects, uint32_t nr, uint32_t* seq_out);

// d2.2 desktop state. (satoru)
bool     g_desktop_configured;
bool     g_setup_failed;
bool     g_restore_pending;
DisplayBackend g_prev_backend = DISPLAY_BACKEND_NONE;
bool     g_windowtest;
bool     g_bench;
bool     g_killtest;
bool     g_killloop;
int      g_windowtest_frames = -1;
int      g_killtest_frames = -1;
int      g_exitdelay_ms;              // rig: exit this long after the desktop is up (satoru)
bool     g_exitdelay_sent;
uint64_t g_desktop_up_ms;
int      g_pending_mode = -1;
uint32_t g_mode_inflight_seq, g_mode_inflight_idx;
uint32_t g_exit_inflight_seq;
bool     g_geometry_changed;
bool     g_kill_now;
uint32_t g_setmode_w, g_setmode_h;
bool     g_setmode_valid, g_setmode_sent;
int      g_setmode_frames = -1;
uint64_t g_stat_bytes, g_stat_frames, g_stat_drops;
uint64_t g_stat_last_ms;
uint32_t g_stat_last_frames;
int      g_kld_out = -1;
int      g_setup_mode = MODE_OFF;
uintptr_t g_fw_addr;
uint32_t g_fw_w, g_fw_h, g_fw_pitch;
uint8_t  g_fw_bpp;
bool     g_added_fw;
bool     g_had_multi;
bool     g_setup_in_progress;
uint32_t g_setup_instance;
uint32_t g_instance;
uint64_t g_last_attach_ms;
bool     g_backend_taken;
bool     g_setup_did_primary;
bool     g_setup_did_output;
bool     g_setup_did_front;
bool     g_had_custom;
bool     g_had_vertical;
bool     g_pattern_busy;
int      g_setup_ax[OUTPUT_MAX], g_setup_ay[OUTPUT_MAX], g_setup_n;
static uint64_t g_abort_log_ms;
uint64_t g_frame_started_ms;
uint64_t g_mode_started_ms;
int      g_consec_drops;
bool     g_watchdog_fatal;
#define KLD_PENDING_MAX 16
DispRect g_pending[KLD_PENDING_MAX];
uint32_t g_pending_count;
bool     g_pending_full;

bool mode_fits(uint32_t w, uint32_t h, uint32_t pitch)
{
    if (!w || !h || w > KLD_DISP_CAP_W || h > KLD_DISP_CAP_H)
        return false;
    if (pitch < w * 4u || pitch > KLD_DISP_CAP_W * 4u)
        return false;
    if ((uint64_t)pitch * h > KLD_DISP_SCANOUT_BYTES)
        return false;
    return true;
}

bool rect_fits(int32_t x, int32_t y, uint32_t w, uint32_t h,
               uint32_t mw, uint32_t mh)
{
    if (x < 0 || y < 0 || !w || !h)
        return false;
    if ((uint64_t)x + w > mw || (uint64_t)y + h > mh)
        return false;
    return true;
}

bool gen_matches(uint32_t gen, uint32_t cur)
{
    return gen == cur;
}

bool done_seq_valid(uint32_t seq, uint32_t sent, uint32_t done)
{
    return sent && seq <= sent && seq > done;
}

void pending_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (g_pending_full)
        return;
    if (g_pending_count >= KLD_PENDING_MAX) {
        // coalesce the whole accumulated damage into one box. (satoru)
        int x0 = g_pending[0].x, y0 = g_pending[0].y;
        int x1 = x0 + (int)g_pending[0].w, y1 = y0 + (int)g_pending[0].h;
        for (uint32_t i = 1; i < g_pending_count; i++) {
            if (g_pending[i].x < x0) x0 = g_pending[i].x;
            if (g_pending[i].y < y0) y0 = g_pending[i].y;
            if (g_pending[i].x + (int)g_pending[i].w > x1) x1 = g_pending[i].x + (int)g_pending[i].w;
            if (g_pending[i].y + (int)g_pending[i].h > y1) y1 = g_pending[i].y + (int)g_pending[i].h;
        }
        g_pending[0].x = x0; g_pending[0].y = y0;
        g_pending[0].w = (uint32_t)(x1 - x0); g_pending[0].h = (uint32_t)(y1 - y0);
        g_pending_count = 1;
    }
    g_pending[g_pending_count].x = x;
    g_pending[g_pending_count].y = y;
    g_pending[g_pending_count].w = (uint32_t)w;
    g_pending[g_pending_count].h = (uint32_t)h;
    g_pending_count++;
}

void pending_add_full()
{
    g_pending_full = true;
    g_pending_count = 0;
}

void wake_container();

bool send_control_locked(uint32_t kind, uint32_t mode_index, uint32_t* seq_out)
{
    if (!g_attached || !g_modes_ok || g_frame_in_flight)
        return false;
    uint32_t seq = ++g_sent_seq;
    DispDown* d = &g_ring->down[(seq - 1) % KLD_DISP_DOWN_SLOTS];
    d->seq = seq;
    d->kind = kind;
    d->generation = g_gen;
    d->nr = 0;
    d->mode_index = mode_index;
    __atomic_store_n(&g_ring->down_prod, seq, __ATOMIC_RELEASE);
    g_frame_in_flight = true;
    // a control message also occupies the one in-flight slot; it needs its own
    // start timestamp or the frame watchdog fires immediately after attach
    // resets the field. (satoru)
    g_frame_started_ms = Scheduler::NowMs();
    if (kind == DOWN_SET_MODE) {
        g_mode_inflight_seq = seq;
        g_mode_inflight_idx = mode_index;
        g_mode_started_ms = Scheduler::NowMs();
    } else if (kind == DOWN_EXIT) {
        g_exit_inflight_seq = seq;
    }
    wake_container();
    if (seq_out) *seq_out = seq;
    return true;
}

void flush_pending_locked()
{
    if (!g_pending_count && !g_pending_full && g_pending_mode < 0 && !g_kill_now)
        return;
    if (!g_attached || !g_modes_ok || g_frame_in_flight)
        return;   // keep the damage until the container acks the last frame
    if (g_kill_now) {
        g_kill_now = false;
        send_control_locked(DOWN_EXIT, 0, nullptr);
        return;
    }
    if (g_pending_mode >= 0) {
        send_control_locked(DOWN_SET_MODE, (uint32_t)g_pending_mode, nullptr);
        g_pending_mode = -1;
        return;
    }
    uint32_t seq = 0;
    if (send_frame_locked(g_pending_full, g_pending, g_pending_count, &seq)) {
        g_pending_count = 0;
        g_pending_full = false;
    }
}

void kld_out_aux(int lx, int ly, int lw, int lh)
{
    SpinLockGuard g(g_lock);
    pending_add(lx, ly, lw, lh);
}

void kld_gfx_hook(const Graphics::Rect* rects, int count, bool full)
{
    {
        SpinLockGuard g(g_lock);
        if (g_setup_mode == MODE_PRIMARY) {
            uint32_t px, py, pw, ph;
            if (full || count <= 0) {
                pending_add_full();
            } else if (Graphics::GetPresentDamage(&px, &py, &pw, &ph)) {
                // the published box is already clipped and in physical pixels
                // (ui scale), unlike the logical dirty rects. (satoru)
                pending_add((int)px, (int)py, (int)pw, (int)ph);
            } else {
                for (int i = 0; i < count; i++)
                    pending_add(rects[i].x, rects[i].y, rects[i].w, rects[i].h);
            }
        }
        // mirror/extend damage came through kld_out_aux during scatter_rect. (satoru)
        flush_pending_locked();
        uint64_t now = Scheduler::NowMs();
        if (g_stat_last_ms == 0)
            g_stat_last_ms = now;
        if (now - g_stat_last_ms >= 5000 && g_stat_frames != g_stat_last_frames) {
            uint64_t dt = now - g_stat_last_ms;
            uint64_t mib = g_stat_bytes / (1024ull * 1024ull);
            uint64_t mibps = dt ? (g_stat_bytes * 1000ull) / (dt * 1024ull * 1024ull) : 0;
            SerialLogger::Log("[klddisp] stats frames=");
            SerialLogger::LogDec((int)g_stat_frames);
            SerialLogger::Log(" mib=");
            SerialLogger::LogDec((int)mib);
            SerialLogger::Log(" mibps=");
            SerialLogger::LogDec((int)mibps);
            SerialLogger::Log(" drops=");
            SerialLogger::LogDec((int)g_stat_drops);
            if (g_attach_refusals) {
                SerialLogger::Log(" attref=");
                SerialLogger::LogDec((int)g_attach_refusals);
            }
            SerialLogger::Log("\n");
            g_stat_last_ms = now;
            g_stat_last_frames = (uint32_t)g_stat_frames;
        }
    }
}

void wake_container()
{
    if (!g_ring)
        return;
    __atomic_add_fetch(&g_ring->doorbell, 1u, __ATOMIC_RELEASE);
    LinuxSyscallFutexWakePhys(g_ring_phys + __builtin_offsetof(DispRing, doorbell), 1);
}

bool send_frame_locked(bool full, const DispRect* rects, uint32_t nr, uint32_t* seq_out)
{
    if (!g_attached || !g_modes_ok || !g_ring)
        return false;
    if (g_frame_in_flight) {
        g_drops++;
        g_stat_drops++;
        return false;
    }
    if (!full) {
        if (!nr || nr > KLD_DISP_MAX_RECTS)
            return false;
        for (uint32_t i = 0; i < nr; i++)
            if (!rect_fits(rects[i].x, rects[i].y, rects[i].w, rects[i].h,
                           g_w, g_h))
                return false;
    }
    uint64_t bytes = full ? (uint64_t)g_w * g_h * 4u : 0;
    if (!full)
        for (uint32_t i = 0; i < nr; i++)
            bytes += (uint64_t)rects[i].w * rects[i].h * 4u;
    g_stat_bytes += bytes;
    g_stat_frames++;
    uint32_t seq = ++g_sent_seq;
    DispDown* d = &g_ring->down[(seq - 1) % KLD_DISP_DOWN_SLOTS];
    d->seq = seq;
    d->kind = DOWN_FRAME;
    d->generation = g_gen;
    d->nr = full ? 0 : nr;
    d->mode_index = 0;
    if (!full)
        for (uint32_t i = 0; i < nr; i++)
            d->rects[i] = rects[i];
    __atomic_store_n(&g_ring->down_prod, seq, __ATOMIC_RELEASE);
    g_frame_in_flight = true;
    g_frame_started_ms = Scheduler::NowMs();
    if (g_done_count < 5) {
        SerialLogger::Log("[klddisp] frame ");
        SerialLogger::LogDec((int)seq);
        SerialLogger::Log(full ? " full" : " rects=");
        if (!full) {
            SerialLogger::LogDec((int)nr);
            SerialLogger::Log(" first=");
            SerialLogger::LogDec(rects[0].x);
            SerialLogger::Log(",");
            SerialLogger::LogDec(rects[0].y);
            SerialLogger::Log(" ");
            SerialLogger::LogDec((int)rects[0].w);
            SerialLogger::Log("x");
            SerialLogger::LogDec((int)rects[0].h);
        }
        SerialLogger::Log("\n");
    }
    wake_container();
    if (seq_out)
        *seq_out = seq;
    return true;
}

bool wait_done(uint32_t seq, uint32_t timeout_ms)
{
    uint64_t deadline = Scheduler::NowMs() + timeout_ms;
    for (;;) {
        if (__atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE) >= seq)
            return true;
        if (Scheduler::NowMs() >= deadline)
            return false;
        Scheduler::SleepMs(1);
    }
}

uint32_t xrgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (r << 16) | (g << 8) | b;
}

void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    SpinLockGuard g(g_lock);
    if (!g_scanout || !g_pitch)
        return;
    if (x >= g_w || y >= g_h)
        return;
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    for (uint32_t yy = y; yy < y + h; yy++) {
        uint32_t* row = (uint32_t*)(g_scanout + (size_t)yy * g_pitch) + x;
        for (uint32_t xx = 0; xx < w; xx++)
            row[xx] = color;
    }
}

uint32_t frame_color(unsigned int f)
{
    // exact through xr24 -> rgb16 too. (satoru)
    return (((f * 8u) & 0xf8u) << 16) | (((f * 4u) & 0xfcu) << 8) |
           ((f * 8u) & 0xf8u);
}

void draw_static(unsigned int frame)
{
    static const uint32_t bars[8] = {
        0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
        0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
    };
    uint32_t bw = g_w / 8;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t x = i * bw;
        fill(x, 0, (i == 7) ? g_w - x : bw, g_h, bars[i]);
    }
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            fill(64 + x, 64 + y, 1, 1, ((x ^ y) & 1) ? 0x00ffffff : 0x00000000);
    fill(96, 64, 32, 32, frame_color(frame));
}

bool cmdline_has(const char* cmdline, const char* tok)
{
    unsigned want = 0;
    while (tok[want]) want++;
    const char* p = cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* t = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        unsigned n = (unsigned)(p - t);
        if (n == want) {
            unsigned i = 0;
            while (i < want && t[i] == tok[i]) i++;
            if (i == want)
                return true;
        }
    }
    return false;
}

}  // namespace

void BootInit(const char* boot_cmdline)
{
    g_windowtest = boot_cmdline &&
                   cmdline_has(boot_cmdline, "kurono.kld.display.windowtest=1");
    g_bench = boot_cmdline &&
              cmdline_has(boot_cmdline, "kurono.kld.display.bench=1");
    if (!boot_cmdline)
        return;
    const char* p = boot_cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* t = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        unsigned n = (unsigned)(p - t);
        const char* k = "kurono.kld.display=";
        unsigned kl = 19;
        if (n <= kl)
            continue;
        bool pre = true;
        for (unsigned i = 0; i < kl; i++)
            if (t[i] != k[i]) { pre = false; break; }
        if (!pre)
            continue;
        const char* v = t + kl;
        unsigned vl = n - kl;
        if (vl == 3 && v[0] == 'o' && v[1] == 'f' && v[2] == 'f') g_mode = MODE_OFF;
        else if (vl == 7 && v[0] == 'p' && v[1] == 'a' && v[2] == 't' &&
                 v[3] == 't' && v[4] == 'e' && v[5] == 'r' && v[6] == 'n')
            g_mode = MODE_PATTERN;
        else if (vl == 6 && v[0] == 'm' && v[1] == 'i' && v[2] == 'r' &&
                 v[3] == 'r' && v[4] == 'o' && v[5] == 'r')
            g_mode = MODE_MIRROR;
        else if (vl == 6 && v[0] == 'e' && v[1] == 'x' && v[2] == 't' &&
                 v[3] == 'e' && v[4] == 'n' && v[5] == 'd')
            g_mode = MODE_EXTEND;
        else if (vl == 7 && v[0] == 'p' && v[1] == 'r' && v[2] == 'i' &&
                 v[3] == 'm' && v[4] == 'a' && v[5] == 'r' && v[6] == 'y')
            g_mode = MODE_PRIMARY;
        else if (vl == 1 && v[0] == '1')
            g_mode = MODE_MIRROR;   // "1" is the generic enable; mirror first
        else
            g_mode = MODE_OFF;
    }
    // kurono.kld.display.setmode=WxH and .killtest=1 (rig hooks). (satoru)
    {
        const char* k = "kurono.kld.display.setmode=";
        unsigned kl = 27;
        const char* p2 = boot_cmdline;
        while (p2 && *p2) {
            while (*p2 == ' ' || *p2 == '\t') p2++;
            const char* t2 = p2;
            while (*p2 && *p2 != ' ' && *p2 != '\t') p2++;
            unsigned n2 = (unsigned)(p2 - t2);
            if (n2 <= kl)
                continue;
            unsigned i = 0;
            while (i < kl && t2[i] == k[i]) i++;
            if (i != kl)
                continue;
            uint32_t w = 0, h = 0;
            const char* v = t2 + kl;
            while (*v >= '0' && *v <= '9') { w = w * 10 + (uint32_t)(*v - '0'); v++; }
            if (*v == 'x' || *v == 'X') v++;
            while (*v >= '0' && *v <= '9') { h = h * 10 + (uint32_t)(*v - '0'); v++; }
            if (w && h) { g_setmode_w = w; g_setmode_h = h; g_setmode_valid = true; }
        }
    }
    g_killtest = cmdline_has(boot_cmdline, "kurono.kld.display.killtest=1");
    g_killloop = cmdline_has(boot_cmdline, "kurono.kld.display.killloop=1");
    g_fuzz = cmdline_has(boot_cmdline, "kurono.kld.display.fuzz=1");
    g_stall = cmdline_has(boot_cmdline, "kurono.kld.display.stall=1");
    g_diepre = cmdline_has(boot_cmdline, "kurono.kld.display.diepre=1");
    g_setupdelay = cmdline_has(boot_cmdline, "kurono.kld.display.setupdelay=1");
    g_setuprdelay = cmdline_has(boot_cmdline, "kurono.kld.display.setuprdelay=1");
    g_killpre = cmdline_has(boot_cmdline, "kurono.kld.display.killpre=1");
    g_attachloop = cmdline_has(boot_cmdline, "kurono.kld.display.attachloop=1");
    g_attach_unlimited = cmdline_has(boot_cmdline, "kurono.kld.display.attachunlimited=1");
    g_modedelay = cmdline_has(boot_cmdline, "kurono.kld.display.modedelay=1");
    g_setupfail = cmdline_has(boot_cmdline, "kurono.kld.display.setupfail=1");
    g_badcount = cmdline_has(boot_cmdline, "kurono.kld.display.badcount=1");
    g_badunmap = cmdline_has(boot_cmdline, "kurono.kld.display.badunmap=1");
    g_patterncycle = cmdline_has(boot_cmdline, "kurono.kld.display.patterncycle=1");
    g_killonsetup = cmdline_has(boot_cmdline, "kurono.kld.display.killonsetup=1");
    // kurono.kld.display.freedelay=<ms>: rig, hold disp_run_free between two
    // batches so the address-space teardown can run concurrently. (satoru)
    {
        const char* k = "kurono.kld.display.freedelay=";
        unsigned kl = 29;
        const char* p5 = boot_cmdline;
        while (p5 && *p5) {
            while (*p5 == ' ' || *p5 == '\t') p5++;
            const char* t5 = p5;
            while (*p5 && *p5 != ' ' && *p5 != '\t') p5++;
            unsigned n5 = (unsigned)(p5 - t5);
            if (n5 <= kl)
                continue;
            unsigned i = 0;
            while (i < kl && t5[i] == k[i]) i++;
            if (i != kl)
                continue;
            int v = 0;
            for (const char* q = t5 + kl; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (*q - '0');
            if (v >= 5 && v <= 2000) g_freedelay_ms = v;
        }
    }
    // kurono.kld.display.detachdelay=<ms>: rig, default 200 for "=1". (satoru)
    {
        const char* k = "kurono.kld.display.detachdelay=";
        unsigned kl = 31;
        const char* p4 = boot_cmdline;
        while (p4 && *p4) {
            while (*p4 == ' ' || *p4 == '\t') p4++;
            const char* t4 = p4;
            while (*p4 && *p4 != ' ' && *p4 != '\t') p4++;
            unsigned n4 = (unsigned)(p4 - t4);
            if (n4 <= kl)
                continue;
            unsigned i = 0;
            while (i < kl && t4[i] == k[i]) i++;
            if (i != kl)
                continue;
            int v = 0;
            for (const char* q = t4 + kl; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (*q - '0');
            if (v == 1) v = 200;
            if (v >= 50 && v <= 5000) g_detachdelay_ms = v;
        }
    }
    // kurono.kld.display.exitdelay=<ms>: rig only. one exit, this long after
    // the desktop first came up, so the host can screendump before and after
    // the re-attach. (satoru)
    {
        const char* k = "kurono.kld.display.exitdelay=";
        unsigned kl = 29;
        const char* p3 = boot_cmdline;
        while (p3 && *p3) {
            while (*p3 == ' ' || *p3 == '\t') p3++;
            const char* t3 = p3;
            while (*p3 && *p3 != ' ' && *p3 != '\t') p3++;
            unsigned n3 = (unsigned)(p3 - t3);
            if (n3 <= kl)
                continue;
            unsigned i = 0;
            while (i < kl && t3[i] == k[i]) i++;
            if (i != kl)
                continue;
            int v = 0;
            for (const char* q = t3 + kl; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (*q - '0');
            if (v >= 500 && v <= 120000) g_exitdelay_ms = v;
        }
    }
}

bool Wanted() { return g_mode != MODE_OFF; }

void ResetAttachBudget()
{
    SpinLockGuard g(g_lock);
    g_success_attach = 0;
    g_last_attach_ms = 0;
    g_attach_lockout_logged = false;
}

bool RestartUnlimitedForTest() { return g_killloop; }
bool DisplayBenchRequested() { return g_bench; }
bool BadCountTest() { return g_badcount; }
int ExitDetachDelayMsForTest() { return g_detachdelay_ms; }
int FreeDelayMsForTest() { return g_freedelay_ms; }
Mode GetMode() { return g_mode; }
const char* ModeName()
{
    switch (g_mode) {
    case MODE_PATTERN: return "pattern";
    case MODE_MIRROR:  return "mirror";
    case MODE_EXTEND:  return "extend";
    case MODE_PRIMARY: return "primary";
    default:           return "off";
    }
}

int64_t AttachPrecheck()
{
    uint64_t now = Scheduler::NowMs();
    SpinLockGuard g(g_lock);
    if (g_mode == MODE_OFF)
        return -19;
    if (g_attached || g_desktop_configured || g_restore_pending ||
        g_setup_in_progress)
        return -16;
    if (g_success_attach >= 32 && !g_attach_unlimited) {
        if (!g_attach_lockout_logged) {
            g_attach_lockout_logged = true;
            SerialLogger::Log("[klddisp] attach locked out: 32 successful attaches this boot\n");
        }
        g_attach_refusals++;
        return -11;
    }
    if (g_last_attach_ms && now - g_last_attach_ms < 1000) {
        g_attach_refusals++;
        return -11;
    }
    return 0;
}

int64_t Attach(uint32_t id, uint64_t as, uint64_t ring_phys,
               uint64_t scanout_phys, uint32_t scanout_pages)
{
    uint64_t now = Scheduler::NowMs();
    SpinLockGuard g(g_lock);
    if (g_mode == MODE_OFF)
        return -19;
    if (g_attached || g_desktop_configured || g_restore_pending ||
        g_setup_in_progress)
        return -16;
    if (scanout_pages != KLD_DISP_SCANOUT_PAGES)
        return -22;
    if (g_success_attach >= 32 && !g_attach_unlimited) {
        if (!g_attach_lockout_logged) {
            g_attach_lockout_logged = true;
            SerialLogger::Log("[klddisp] attach locked out: 32 successful attaches this boot\n");
        }
        return -11;
    }
    if (g_last_attach_ms && now - g_last_attach_ms < 1000)
        return -11;
    g_success_attach++;
    g_last_attach_ms = now;
    g_disp_id = id;
    g_instance++;
    g_as = as;
    g_ring_phys = ring_phys;
    g_scanout_phys = scanout_phys;
    g_scanout_pages = scanout_pages;
    // pmm frames are reached through the direct map when it is on. (satoru)
    g_ring = (DispRing*)PMM::PhysToVirt(ring_phys);
    g_scanout = (uint8_t*)PMM::PhysToVirt(scanout_phys);
    g_attached = true;
    g_modes_ok = false;
    g_gen = 0;
    g_sent_seq = g_done_seq = g_done_count = g_drops = 0;
    g_stat_bytes = g_stat_frames = g_stat_drops = 0;
    g_stat_last_ms = 0;
    g_stat_last_frames = 0;
    g_frame_in_flight = false;
    g_frame_started_ms = 0;
    g_mode_started_ms = 0;
    g_consec_drops = 0;
    g_watchdog_fatal = false;
    g_setup_failed = false;
    g_restore_pending = false;
    g_pending_mode = -1;
    g_pending_count = 0;
    g_pending_full = false;
    g_kill_now = false;
    g_mode_inflight_seq = g_mode_inflight_idx = 0;
    g_exit_inflight_seq = 0;
    g_geometry_changed = false;
    g_setmode_sent = false;
    g_setmode_frames = -1;
    g_killtest_frames = -1;
    g_setup_did_primary = g_setup_did_output = g_setup_did_front = false;
    g_killpre_sent = false;
    g_had_custom = g_had_vertical = false;
    g_ring->scanout_bytes = KLD_DISP_SCANOUT_BYTES;
    g_ring->scanout_pages = KLD_DISP_SCANOUT_PAGES;
    g_ring->fuzz = g_fuzz ? 1u : 0u;
    g_ring->stall = g_stall ? 1u : 0u;
    g_ring->diepre = g_diepre ? 1u : 0u;
    g_ring->attachloop = g_attachloop ? 1u : 0u;
    // one shot: the hostile loop is a rig, and the restarted container must
    // come up as a normal display so the test can see the desktop recover.
    // (satoru)
    g_attachloop = false;
    g_ring->modedelay = g_modedelay ? 1u : 0u;
    g_ring->badcount = g_badcount ? 1u : 0u;
    g_ring->setupfail = g_setupfail ? 1u : 0u;
    g_ring->badunmap = g_badunmap ? 1u : 0u;
    g_badunmap = false;   // one shot: the restarted container serves normally (satoru)
    SerialLogger::Log("[klddisp] attached, scanout ");
    SerialLogger::LogDec((int)(KLD_DISP_SCANOUT_BYTES / (1024 * 1024)));
    SerialLogger::Log(" MiB\n");
    return 0;
}

static void detach_common(uint32_t id, bool set_restore)
{
    bool logged = false;
    {
        SpinLockGuard g(g_lock);
        if (g_disp_id != id)
            return;
        if (g_attached) {
            g_attached = false;
            g_modes_ok = false;
            g_ring = nullptr;
            g_scanout = nullptr;
            g_pending_count = 0;
            g_pending_full = false;
            g_kill_now = false;
            g_frame_in_flight = false;
            g_mode_inflight_seq = 0;
            g_setup_failed = false;
            logged = true;
        }
        if (set_restore)
            g_restore_pending = true;
    }
    if (logged)
        SerialLogger::Log("[klddisp] frames deferred (the gui owns the cleanup)\n");
}

void Detach(uint32_t id) { detach_common(id, true); }
void DetachForRelease(uint32_t id) { detach_common(id, false); }

int64_t ReportModes(uint64_t as)
{
    DispMode local[KLD_DISP_MAX_MODES];
    uint32_t n, cur;
    uint32_t lw, lh, lp, lgen;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as || !g_ring)
            return -19;
        DispRing* r = g_ring;
        if (r->magic != KLD_DISP_MAGIC || r->version != KLD_DISP_VERSION)
            return -22;
        // copy every container-writable field ONCE, then validate the copy.
        n = r->mode_count;
        if (!n || n > KLD_DISP_MAX_MODES)
            return -22;
        cur = r->current_mode;
        if (cur >= n)
            return -22;
        for (uint32_t i = 0; i < n; i++) {
            local[i].width = r->modes[i].width;
            local[i].height = r->modes[i].height;
            local[i].pitch = r->modes[i].pitch;
            local[i].refresh_mhz = r->modes[i].refresh_mhz;
            local[i].flags = r->modes[i].flags;
            if (!mode_fits(local[i].width, local[i].height, local[i].pitch))
                return -22;
        }
        for (uint32_t i = 0; i < n; i++)
            g_modes[i] = local[i];
        g_mode_n = n;
        g_current_mode = cur;
        g_w = local[cur].width;
        g_h = local[cur].height;
        g_pitch = local[cur].pitch;
        g_gen++;
        lgen = g_gen;
        r->generation = lgen;
        r->current_mode = cur;
        g_modes_ok = true;
        lw = g_w; lh = g_h; lp = g_pitch;
    }
    SerialLogger::Log("[klddisp] modes ");
    SerialLogger::LogDec((int)n);
    SerialLogger::Log(" current ");
    SerialLogger::LogDec((int)lw);
    SerialLogger::Log("x");
    SerialLogger::LogDec((int)lh);
    SerialLogger::Log(" pitch ");
    SerialLogger::LogDec((int)lp);
    SerialLogger::Log(" gen ");
    SerialLogger::LogDec((int)lgen);
    SerialLogger::Log("\n");
    return 0;
}

int64_t Done(uint64_t as, uint32_t seq)
{
    SpinLockGuard g(g_lock);
    if (!g_attached || g_as != as)
        return -19;
    if (!done_seq_valid(seq, g_sent_seq, g_done_seq))
        return -22;
    if (g_done_count < 5) {
        SerialLogger::Log("[klddisp] done ");
        SerialLogger::LogDec((int)seq);
        SerialLogger::Log("\n");
    }
    if (g_mode_inflight_seq && seq == g_mode_inflight_seq) {
        uint32_t idx = g_mode_inflight_idx;
        if (idx < g_mode_n) {
            g_current_mode = idx;
            g_w = g_modes[idx].width;
            g_h = g_modes[idx].height;
            g_pitch = g_modes[idx].pitch;
            g_gen++;
            g_ring->generation = g_gen;
            g_ring->current_mode = idx;
            g_geometry_changed = true;
            pending_add_full();
            SerialLogger::Log("[klddisp] mode change acked ");
            SerialLogger::LogDec((int)g_w);
            SerialLogger::Log("x");
            SerialLogger::LogDec((int)g_h);
            SerialLogger::Log("\n");
        }
        g_mode_inflight_seq = 0;
    }
    g_done_seq = seq;
    g_done_count++;
    if (seq == g_sent_seq)
        g_frame_in_flight = false;
    g_consec_drops = 0;
    flush_pending_locked();
    return 0;
}

int64_t Hotplug(uint64_t as, uint32_t status)
{
    static int logged;
    bool ok;
    {
        SpinLockGuard g(g_lock);
        ok = g_attached && g_as == as;
    }
    if (!ok)
        return -19;
    if (logged++ < 5) {
        SerialLogger::Log("[klddisp] hotplug status ");
        SerialLogger::LogDec((int)status);
        SerialLogger::Log("\n");
    }
    return 0;
}

int64_t Error(uint64_t as, uint32_t code)
{
    static int logged;
    bool ok;
    {
        SpinLockGuard g(g_lock);
        ok = g_attached && g_as == as;
    }
    if (!ok)
        return -19;
    if (logged++ < 5) {
        SerialLogger::Log("[klddisp] container error ");
        SerialLogger::LogDec((int)code);
        SerialLogger::Log("\n");
    }
    return 0;
}

bool RequestMode(uint32_t w, uint32_t h)
{
    SpinLockGuard g(g_lock);
    if (!g_attached || !g_modes_ok || !g_ring)
        return false;
    for (uint32_t i = 0; i < g_mode_n; i++) {
        if (g_modes[i].width == w && g_modes[i].height == h) {
            if (i == g_current_mode)
                return true;
            g_pending_mode = (int)i;
            flush_pending_locked();
            return true;
        }
    }
    return false;
}

bool SetModeRequested(uint32_t* w, uint32_t* h)
{
    if (!g_setmode_valid)
        return false;
    if (w) *w = g_setmode_w;
    if (h) *h = g_setmode_h;
    return true;
}

void RequestExitForTest()
{
    SpinLockGuard g(g_lock);
    g_kill_now = true;
    flush_pending_locked();
}

bool Ready()
{
    return g_attached && g_modes_ok;
}
uint32_t ModeWidth()  { return g_w; }
uint32_t ModeHeight() { return g_h; }
uint32_t ModePitch()  { return g_pitch; }

bool PresentRects(const Rect* rects, int count, bool full, uint32_t* seq_out)
{
    DispRect rr[KLD_DISP_MAX_RECTS];
    int n = count;
    if (n < 0) n = 0;
    if (n > (int)KLD_DISP_MAX_RECTS) n = KLD_DISP_MAX_RECTS;
    for (int i = 0; i < n; i++) {
        rr[i].x = rects[i].x;
        rr[i].y = rects[i].y;
        rr[i].w = (uint32_t)rects[i].w;
        rr[i].h = (uint32_t)rects[i].h;
    }
    SpinLockGuard g(g_lock);
    return send_frame_locked(full || n == 0, rr, (uint32_t)n, seq_out);
}

static int find_kld_output()
{
    for (int i = 0; i < Outputs::Total(); i++) {
        const Output* o = Outputs::Get(i);
        if (o && o->kld)
            return i;
    }
    return -1;
}

static void remove_added_outputs()
{
    int idx = find_kld_output();
    if (idx >= 0)
        Outputs::Remove(idx);
    if (g_added_fw) {
        for (int i = 0; i < Outputs::Total(); i++) {
            const Output* o = Outputs::Get(i);
            if (o && o->bridge_fw) {
                Outputs::Remove(i);
                break;
            }
        }
        g_added_fw = false;
    }
}

// The gui proc is the only owner of the compositor references. Restore only
// what setup actually did; a detach before the modes message or an aborted
// setup must not reinitialise the compositor with stale values. The broker
// instance owns the frames; this function releases its ref exactly once and
// keeps the mapping ref until the container exits (or the explicit detach
// path unmaps it). (satoru)
static void reinit_keep_scale(uintptr_t addr, uint32_t w, uint32_t h,
                              uint32_t pitch, uint8_t bpp)
{
    int sc = Graphics::GetUIScale();
    Graphics::ReinitForResolution(addr, w, h, pitch, bpp);
    if (Graphics::GetUIScale() != sc) {
        // Init dropped the scale because the old back buffer was gone. the
        // scale needs double buffering, so restore that first; every caller
        // wants it anyway. (satoru)
        Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
        Graphics::SetUIScale(sc);
    }
}

static void restore_desktop(bool aborted)
{
    int was = g_setup_mode;
    uint32_t id = 0;
    if (g_setup_did_output || g_setup_did_primary) {
        Graphics::SetAuxPresentHook(nullptr);
        Graphics::SetFramebufferIsRam(false);
        if (g_backend_taken) {
            DisplayManager::SetBackend(g_prev_backend);
            g_backend_taken = false;
        }
        if (g_setup_did_output) {
            bool unchanged = true;
            for (int i = 0; i < Outputs::Total(); i++) {
                const Output* o = Outputs::Get(i);
                if (!o || o->kld) continue;
                bool found = false;
                for (int j = 0; j < g_setup_n; j++) {
                    if (o->ax == g_setup_ax[j] && o->ay == g_setup_ay[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) { unchanged = false; break; }
            }
            remove_added_outputs();
            if (unchanged) {
                // nothing the user owns changed while we were attached: put
                // back exactly the layout mode setup found. (satoru)
                if (!g_had_custom)
                    Outputs::Arrange(nullptr, nullptr);
                Outputs::Layout(g_had_vertical);
            } else {
                // the user rearranged another output meanwhile; keep the mode
                // and placement the user chose and only drop our output. (satoru)
                Outputs::Layout(Outputs::Vertical());
            }
            if (Outputs::Multi()) {
                if (Graphics::MultiActive())
                    Graphics::RelayoutOutputs();
                else
                    Graphics::EnableMultiOutput();
            } else {
                if (Graphics::MultiActive())
                    Graphics::LeaveMultiOutput();
                if (g_fw_addr) {
                    reinit_keep_scale(g_fw_addr, g_fw_w, g_fw_h,
                                      g_fw_pitch, g_fw_bpp);
                    Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
                }
            }
        } else {
            if (Outputs::Multi()) {
                if (Graphics::MultiActive())
                    Graphics::RelayoutOutputs();
                else
                    Graphics::EnableMultiOutput();
            } else if (g_fw_addr) {
                reinit_keep_scale(g_fw_addr, g_fw_w, g_fw_h,
                                  g_fw_pitch, g_fw_bpp);
                Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
            }
        }
    }
    {
        SpinLockGuard g(g_lock);
        g_desktop_configured = false;
        g_kld_out = -1;
        g_setup_mode = MODE_OFF;
        g_setup_in_progress = false;
        g_setup_did_output = g_setup_did_primary = false;
        id = g_disp_id;
    }
    if (aborted) {
        uint64_t now = Scheduler::NowMs();
        if (now - g_abort_log_ms > 1000) {
            g_abort_log_ms = now;
            SerialLogger::Log("[klddisp] setup aborted: the instance died before publishing\n");
        }
    }
    else if (was == MODE_PRIMARY)
        SerialLogger::Log("[klddisp] container gone, desktop back on the firmware framebuffer\n");
    else
        SerialLogger::Log("[klddisp] container gone, output removed, previous outputs restored\n");
    if (id) {
        if (KldBroker::ReleaseDisplayInstance(id)) {
            SpinLockGuard g(g_lock);
            if (g_disp_id == id) g_disp_id = 0;
        } else {
            KldBroker::MarkDisplayDying(id);
            SpinLockGuard g(g_lock);
            g_restore_pending = true;   // retry the release
        }
    }
}

static bool setup_desktop(uint32_t inst, int mode, uint32_t w, uint32_t h,
                          uint32_t pitch, uint64_t scan_phys)
{
    bool alive;
    uint32_t id;
    {
        SpinLockGuard g(g_lock);
        alive = g_attached && g_instance == inst && !g_restore_pending;
        id = g_disp_id;
    }
    if (!alive) {
        restore_desktop(true);
        return false;
    }
    if (g_setupfail) {
        // one shot: the first setup after the boot fails, so the container
        // can exercise the abort path and a later restart really re-attaches.
        // (satoru)
        g_setupfail = false;
        g_setup_failed = true;
        restore_desktop(true);
        return false;
    }
    if (g_setuprdelay)
        Scheduler::SleepMs(400);   // widens the detach-racing-setup window (test) (satoru)
    if (g_killonsetup && !g_killonsetup_sent) {
        // rig: the container's serve thread has had the delay window to start;
        // ask it to exit now, then finish setup so the exit path's DYING lands
        // between our first alive check and the publication. the in-flight
        // frame slot is stolen for the control message, test boots only. (satoru)
        g_killonsetup_sent = true;
        {
            SpinLockGuard g(g_lock);
            g_frame_in_flight = false;
            g_kill_now = true;
            flush_pending_locked();
        }
        // stay inside setup long enough for the container to process the exit.
        // the gui loop holds kls across this function, so drop it for the rig
        // sleep or the container's exit syscall could never run here. (satoru)
        int kls_depth = LinuxSyscall::KlsDropAll();
        Scheduler::SleepMs(300);
        LinuxSyscall::KlsRestore(kls_depth);
    }

    g_had_multi = Graphics::MultiActive();
    g_had_custom = Outputs::Custom();
    g_had_vertical = Outputs::Vertical();
    g_fw_addr = (uintptr_t)Graphics::GetFrontBuffer();
    g_fw_w = (uint32_t)Graphics::GetPhysicalWidth();
    g_fw_h = (uint32_t)Graphics::GetPhysicalHeight();
    g_fw_pitch = Graphics::GetPhysicalPitch();
    g_fw_bpp = Graphics::GetBpp();
    g_added_fw = false;

    if (mode == MODE_PRIMARY) {
        g_setup_did_primary = true;
        g_setup_did_front = true;
        Graphics::LeaveMultiOutput();
        g_prev_backend = DisplayManager::SetBackend(DISPLAY_BACKEND_KLD);
        g_backend_taken = true;
        Graphics::SetFramebufferIsRam(true);
        reinit_keep_scale((uintptr_t)PMM::PhysToVirt(scan_phys), w, h, pitch, 32);
        Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
        g_kld_out = -1;
    } else {
        g_setup_did_output = true;
        if (Outputs::Total() == 0) {
            if (Outputs::Add(g_fw_addr, (int)g_fw_w, (int)g_fw_h,
                             g_fw_pitch, g_fw_bpp, false, "Display 1") < 0) {
                g_setup_failed = true;
                restore_desktop(true);
                return false;
            }
            g_added_fw = true;
            Output& fo = *(Output*)Outputs::Get(Outputs::Total() - 1);
            fo.bridge_fw = true;
        }
        int idx = Outputs::Add(scan_phys, (int)w, (int)h, pitch, 32, false,
                               "KLD Display");
        if (idx < 0) {
            g_setup_failed = true;
            restore_desktop(true);
            return false;
        }
        Output& ko = *(Output*)Outputs::Get(idx);
        ko.kld = true;
        Outputs::SetRam(idx, true);
        Outputs::SetAuxPresent(idx, kld_out_aux);
        int ax = 0, ay = 0;
        if (mode == MODE_EXTEND) {
            for (int i = 0; i < Outputs::Total(); i++) {
                const Output* o = Outputs::Get(i);
                if (!o || i == idx || !o->connected)
                    continue;
                if (o->ax + o->pw > ax)
                    ax = o->ax + o->pw;
            }
        }
        Outputs::PlaceCustom(idx, ax, ay);
        g_setup_n = Outputs::Total();
        for (int i = 0; i < g_setup_n; i++) {
            const Output* o = Outputs::Get(i);
            g_setup_ax[i] = o ? o->ax : 0;
            g_setup_ay[i] = o ? o->ay : 0;
        }
        bool ok = g_had_multi ? Graphics::RelayoutOutputs()
                              : Graphics::EnableMultiOutput();
        if (!ok) {
            g_setup_failed = true;
            restore_desktop(true);
            return false;
        }
        g_kld_out = idx;
    }

    // final instance check: a detach that arrived while the outputs/front
    // buffer were being built must abort before anything is published. (satoru)
    {
        SpinLockGuard g(g_lock);
        alive = g_attached && g_instance == inst && !g_restore_pending;
    }
    if (!alive || !KldBroker::MarkDisplayPublished(id)) {
        restore_desktop(true);
        return false;
    }
    Graphics::SetAuxPresentHook(kld_gfx_hook);
    {
        SpinLockGuard g(g_lock);
        g_desktop_configured = true;
        g_setup_mode = mode;
        g_setup_in_progress = false;
        g_desktop_up_ms = Scheduler::NowMs();
    }
    if (mode == MODE_PRIMARY) {
        uint64_t pte = KernelVMM::QueryPageFlags((uint64_t)scan_phys);
        SerialLogger::Log("[klddisp] desktop PRIMARY on the container output ");
        SerialLogger::LogDec((int)w);
        SerialLogger::Log("x");
        SerialLogger::LogDec((int)h);
        SerialLogger::Log(" pte 0x");
        SerialLogger::LogHex((unsigned long)pte);
        SerialLogger::Log(" (pwt/pcd 0)\n");
    } else {
        SerialLogger::Log(mode == MODE_MIRROR
                          ? "[klddisp] desktop MIRROR on the container output\n"
                          : "[klddisp] desktop EXTEND on the container output\n");
    }
    if (g_windowtest)
        g_windowtest_frames = 3;
    if (g_bench) {
        g_bench = false;
        GfxBench::Start();
    }
    return true;
}

void ApplyPendingDesktop()
{
    if (g_mode == MODE_OFF)
        return;   // the bridge was never enabled: no locks, no clock (satoru)

    // liveness: a mode change in flight is governed by the 2000 ms mode
    // timeout only; a real modeset can take longer than 500 ms. frames use the
    // 500 ms timeout when no control message occupies the in-flight slot.
    // (satoru)
    {
        bool fault = false, dropped = false;
        uint64_t now = Scheduler::NowMs();
        {
            SpinLockGuard g(g_lock);
            if (g_attached && g_modes_ok) {
                if (g_mode_inflight_seq) {
                    if (now - g_mode_started_ms > 2000)
                        fault = true;
                } else if (g_frame_in_flight &&
                           now - g_frame_started_ms > 500) {
                    g_frame_in_flight = false;
                    g_drops++;
                    g_stat_drops++;
                    g_consec_drops++;
                    pending_add_full();
                    dropped = true;
                }
                if (g_consec_drops >= 5)
                    fault = true;
                if (fault) {
                    g_stall = false;   // one-shot test flag (satoru)
                    // detach fully: a later setup or request must never see the
                    // bridge still naming frames the restore is about to free.
                    // (satoru)
                    g_attached = false;
                    g_modes_ok = false;
                    g_frame_in_flight = false;
                    g_mode_inflight_seq = 0;
                    g_pending_count = 0;
                    g_pending_full = false;
                    g_kill_now = false;
                    g_ring = nullptr;
                    g_scanout = nullptr;
                    g_as = 0;
                    g_ring_phys = 0;
                    g_scanout_phys = 0;
                    g_scanout_pages = 0;
                    g_restore_pending = true;
                }
            }
        }
        if (dropped)
            SerialLogger::Log("[klddisp] frame not acked in 500 ms, dropped\n");
        if (fault) {
            SerialLogger::Log("[klddisp] container unresponsive for display, detaching the output\n");
            uint32_t id;
            {
                SpinLockGuard g(g_lock);
                id = g_disp_id;
            }
            if (id)
                KldBroker::MarkDisplayDying(id);   // the state machine owns the later exit (satoru)
        }
    }

    bool do_restore = false, pattern_busy = false;
    {
        SpinLockGuard g(g_lock);
        do_restore = g_restore_pending;
        pattern_busy = g_pattern_busy;
        if (do_restore && g_mode != MODE_PATTERN) {
            g_restore_pending = false;
            g_setup_in_progress = true;
        }
    }
    if (g_mode == MODE_PATTERN) {
        if (!do_restore || pattern_busy)
            return;
        uint32_t id = 0;
        {
            SpinLockGuard g(g_lock);
            g_restore_pending = false;
            g_desktop_configured = false;
            g_kld_out = -1;
            g_setup_mode = MODE_OFF;
            id = g_disp_id;
        }
        KldDisplay::DetachForRelease(id);
        if (KldBroker::ReleaseDisplayInstance(id)) {
            SpinLockGuard g(g_lock);
            if (g_disp_id == id) g_disp_id = 0;
            SerialLogger::Log("[klddisp] pattern instance released\n");
        } else {
            SpinLockGuard g(g_lock);
            g_restore_pending = true;
        }
        return;
    }
    if (do_restore) {
        restore_desktop(false);
        return;
    }

    if (g_geometry_changed && g_desktop_configured) {
        uint32_t w, h, pitch;
        uint64_t scan_phys;
        {
            SpinLockGuard g(g_lock);
            g_geometry_changed = false;
            w = g_w; h = g_h; pitch = g_pitch;
            scan_phys = g_scanout_phys;
        }
        if (g_setup_mode == MODE_PRIMARY) {
            reinit_keep_scale((uintptr_t)PMM::PhysToVirt(scan_phys), w, h, pitch, 32);
            Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
        } else {
            int idx = find_kld_output();
            if (idx >= 0) {
                Outputs::SetGeometry(idx, (int)w, (int)h, pitch);
                Graphics::RelayoutOutputs();
            }
        }
        SerialLogger::Log("[klddisp] desktop geometry now ");
        SerialLogger::LogDec((int)w);
        SerialLogger::Log("x");
        SerialLogger::LogDec((int)h);
        SerialLogger::Log("\n");
    }

    if (g_killpre && g_attached && g_modes_ok && !g_desktop_configured &&
        !g_setup_in_progress) {
        if (!g_killpre_sent) {
            g_killpre_sent = true;
            SerialLogger::Log("[klddisp] killpre: exiting after modes, before setup\n");
            RequestExitForTest();
        }
        // let the serve thread process the exit before the setup pass.
        // (satoru)
        Scheduler::SleepMs(1500);
    }
    if (g_desktop_configured) {
        if (g_exitdelay_ms > 0 && !g_exitdelay_sent &&
            Scheduler::NowMs() - g_desktop_up_ms >= (uint64_t)g_exitdelay_ms) {
            g_exitdelay_sent = true;
            SerialLogger::Log("[klddisp] exitdelay: asking the container to exit\n");
            RequestExitForTest();
        }
        if (g_setmode_valid && !g_setmode_sent) {
            if (g_setmode_frames < 0)
                g_setmode_frames = 3;
            else if (g_setmode_frames-- == 0) {
                g_setmode_sent = true;
                RequestMode(g_setmode_w, g_setmode_h);
                SerialLogger::Log("[klddisp] setmode requested\n");
            }
        }
        if (g_killtest || g_killloop) {
            if (g_killtest_frames < 0)
                g_killtest_frames = g_killloop ? 120 : 30;
            else if (g_killtest_frames-- == 0) {
                if (!g_killloop)
                    g_killtest = false;
                g_killtest_frames = -1;
                SerialLogger::Log("[klddisp] killtest: asking the container to exit\n");
                RequestExitForTest();
            }
        }
        if (g_windowtest && g_windowtest_frames >= 0) {
            if (g_windowtest_frames-- == 0) {
                SystemSettings::OpenPage(nullptr);
                SerialLogger::Log("[klddisp] windowtest opened settings on the container output\n");
                g_windowtest_frames = -1;
            }
        }
    }

    bool do_setup;
    int mode;
    uint32_t w, h, pitch;
    uint64_t scan_phys;
    uint32_t inst = 0;
    {
        SpinLockGuard g(g_lock);
        do_setup = !g_desktop_configured && !g_setup_in_progress &&
                   !g_setup_failed && g_attached && g_modes_ok;
        if (do_setup) {
            g_setup_in_progress = true;
            inst = g_instance;
        }
        mode = g_mode;
        w = g_w; h = g_h; pitch = g_pitch;
        scan_phys = g_scanout_phys;
    }
    if (!do_setup)
        return;
    setup_desktop(inst, mode, w, h, pitch, scan_phys);
}

void SelfCheck()
{
    int pass = 0, total = 0;
    struct Case { bool got; bool want; const char* name; };
    Case cases[] = {
        { mode_fits(1024, 768, 4096), true, "valid mode" },
        { mode_fits(1920, 1081, 7680), false, "oversized height" },
        { mode_fits(1024, 768, 4092), false, "pitch too small" },
        { mode_fits(0, 768, 4096), false, "zero width" },
        { mode_fits(KLD_DISP_CAP_W + 1, 100, (KLD_DISP_CAP_W + 1) * 4), false, "cap width" },
        { mode_fits(1920, 1080, 7680), true, "cap edge" },
        { rect_fits(0, 0, 100, 100, 1024, 768), true, "valid rect" },
        { rect_fits(1000, 700, 100, 100, 1024, 768), false, "rect past edge" },
        { rect_fits(-1, 0, 10, 10, 1024, 768), false, "negative rect" },
        { rect_fits(0, 0, 0, 10, 1024, 768), false, "zero rect" },
        { rect_fits(1023, 767, 1, 1, 1024, 768), true, "last pixel" },
        { gen_matches(3, 3), true, "generation match" },
        { gen_matches(2, 3), false, "stale generation" },
        { done_seq_valid(4, 4, 3), true, "done seq" },
        { done_seq_valid(5, 4, 3), false, "bogus flip seq" },
        { done_seq_valid(3, 4, 3), false, "duplicate done seq" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        total++;
        if (cases[i].got == cases[i].want) {
            pass++;
        } else {
            SerialLogger::Log("[klddisp] selfcheck FAIL ");
            SerialLogger::Log(cases[i].name);
            SerialLogger::Log("\n");
        }
    }
    SerialLogger::Log("[klddisp] selfcheck ");
    SerialLogger::LogDec(pass);
    SerialLogger::Log("/");
    SerialLogger::LogDec(total);
    SerialLogger::Log(pass == total ? " PASS\n" : " FAIL\n");
}

// after a mode change ack the bridge itself queues one full frame of the
// scanout as it is at that moment (Done -> pending_add_full), and that frame
// can still be in flight while the pattern is redrawn: the container then
// copies a half-drawn scanout (the d3 device dump showed old pixels in the
// top rows of the later bars) and the pattern's own full frame is refused as
// "in flight" and never sent. so: wait until nothing is in flight or pending,
// draw, then send with a bounded retry and wait for the flip. (satoru)
static bool pattern_full_frame(unsigned int frame, uint32_t* seq_out)
{
    for (int i = 0; i < 3000; i++) {
        bool idle;
        {
            SpinLockGuard g(g_lock);
            idle = !g_frame_in_flight && !g_pending_full && !g_pending_count;
        }
        if (idle) break;
        Scheduler::SleepMs(1);
    }
    draw_static(frame);
    uint32_t s = 0;
    bool sent = false;
    for (int i = 0; i < 3000 && !sent; i++) {
        {
            SpinLockGuard g(g_lock);
            if (!g_frame_in_flight)
                sent = send_frame_locked(true, nullptr, 0, &s);
        }
        if (!sent) Scheduler::SleepMs(1);
    }
    if (seq_out) *seq_out = s;
    return sent && wait_done(s, 3000);
}

// ctr: the counter block colour (frame_color of the frame drawn). (satoru)
static int pattern_exact(uint32_t* fb, uint32_t pitch, uint32_t w, uint32_t h,
                         uint32_t ctr)
{
    static const uint32_t bars[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000
    };
    uint32_t bw = w / 8;
    int match = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)fb + (size_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t want;
            if (x >= 64 && x < 80 && y >= 64 && y < 80)
                want = (((x - 64) ^ (y - 64)) & 1) ? 0xFFFFFF : 0;
            else if (x >= 96 && x < 128 && y >= 64 && y < 96)
                want = ctr & 0xFFFFFF;
            else
                want = bars[x / bw > 7 ? 7 : x / bw];
            if ((row[x] & 0xFFFFFF) == want)
                match++;
        }
    }
    return match;
}

void PatternEntry()
{
    SelfCheck();
    {
        SpinLockGuard g(g_lock);
        g_pattern_busy = true;
    }
    // wait for attach + a mode. the container can start after this process
    // (the boot pattern used to give up on the first not-yet-attached pass and
    // then ran its frames against nothing). only a container that was attached
    // once and then died ends the wait early, and there is a total bound. (satoru)
    {
        bool ever = false;
        int waits = 0;
        while (!Ready()) {
            bool alive;
            {
                SpinLockGuard g(g_lock);
                alive = g_attached;
            }
            if (alive) ever = true;
            if (ever && !alive) break;
            if (++waits > 3000) break;   // 30 s
            Scheduler::SleepMs(10);
        }
    }

    draw_static(0);
    uint32_t seq = 0;
    bool ok;
    {
        SpinLockGuard g(g_lock);
        ok = send_frame_locked(true, nullptr, 0, &seq);
    }
    if (!ok)
        SerialLogger::Log("[klddisp] FAIL first frame not sent\n");
    bool done = wait_done(seq, 3000);
    SerialLogger::Log("[klddisp] pattern ready ");
    SerialLogger::LogDec((int)g_w);
    SerialLogger::Log("x");
    SerialLogger::LogDec((int)g_h);
    SerialLogger::Log(done ? "\n" : " (no flip-done)\n");

    // hold the pattern for the host screendump. (satoru)
    Scheduler::SleepMs(20000);

    uint32_t rw = 0, rh = 0;
    if (g_patterncycle) {
        // 20 translator-driven mode changes among the three d3 sizes. after each
        // ack the pattern is redrawn and every scanout pixel is checked, and the
        // 12 s hold gives the host time to stop the vm, read the device
        // registers and vram, and take its screendumps (guest time keeps
        // running while the vm is stopped).
        // the used frame count brackets the loop for the leak check. (satoru)
        static const uint32_t cyc[3][2] = {
            { 1024, 768 }, { 1920, 1080 }, { 1280, 800 },
        };
        SerialLogger::Log("[klddisp] pattern cycle start pmm_used_frames=");
        SerialLogger::LogDec((int)PMM::GetUsedFrames());
        SerialLogger::Log("\n");
        for (int k = 0; k < 20; k++) {
            uint32_t w = cyc[k % 3][0], h = cyc[k % 3][1];
            uint32_t gen0;
            {
                SpinLockGuard g(g_lock);
                gen0 = g_gen;
            }
            if (!RequestMode(w, h)) {
                SerialLogger::Log("[klddisp] pattern cycle mode not offered ");
                SerialLogger::LogDec((int)w);
                SerialLogger::Log("x");
                SerialLogger::LogDec((int)h);
                SerialLogger::Log("\n");
                continue;
            }
            for (int i = 0; i < 5000; i++) {
                bool ch;
                {
                    SpinLockGuard g(g_lock);
                    ch = g_gen != gen0;
                }
                if (ch) break;
                Scheduler::SleepMs(1);
            }
            bool ch;
            {
                SpinLockGuard g(g_lock);
                ch = g_gen != gen0;
            }
            if (!ch) {
                SerialLogger::Log("[klddisp] pattern cycle mode timeout\n");
                continue;
            }
            // the counter block carries the change number, so the host can
            // tell this change's frame from an older one of the same size.
            // (satoru)
            uint32_t s2 = 0;
            bool flipped = pattern_full_frame((unsigned int)(k + 1), &s2);
            if (!flipped)
                SerialLogger::Log("[klddisp] pattern cycle frame not flipped\n");
            int exact = 0, total = 0;
            {
                // the facts under the lock, the scan outside it: the pattern
                // process owns this instance until it ends. (satoru)
                uint8_t* sc;
                uint32_t sp, sw, sh;
                {
                    SpinLockGuard g(g_lock);
                    sc = g_scanout; sp = g_pitch; sw = g_w; sh = g_h;
                }
                if (sc && sp && sw && sh) {
                    exact = pattern_exact((uint32_t*)sc, sp, sw, sh,
                                          frame_color((unsigned int)(k + 1)));
                    total = (int)(sw * sh);
                }
            }
            SerialLogger::Log("[klddisp] pattern mode ");
            SerialLogger::LogDec((int)g_w);
            SerialLogger::Log("x");
            SerialLogger::LogDec((int)g_h);
            SerialLogger::Log(" exact ");
            SerialLogger::LogDec(exact);
            SerialLogger::Log("/");
            SerialLogger::LogDec(total);
            SerialLogger::Log(" change ");
            SerialLogger::LogDec(k + 1);
            SerialLogger::Log("\n");
            Scheduler::SleepMs(12000);
        }
        SerialLogger::Log("[klddisp] pattern cycle end pmm_used_frames=");
        SerialLogger::LogDec((int)PMM::GetUsedFrames());
        SerialLogger::Log("\n");
    } else if (SetModeRequested(&rw, &rh)) {
        uint32_t gen0 = g_gen;
        if (RequestMode(rw, rh)) {
            for (int i = 0; i < 5000 && g_gen == gen0; i++)
                Scheduler::SleepMs(1);
            if (g_gen != gen0) {
                uint32_t s2 = 0;
                pattern_full_frame(0, &s2);   // same race as the cycle (satoru)
                int exact = 0, total = 0;
                if (g_scanout && g_pitch && g_w && g_h) {
                    exact = pattern_exact((uint32_t*)g_scanout, g_pitch, g_w, g_h, 0);
                    total = (int)(g_w * g_h);
                }
                SerialLogger::Log("[klddisp] pattern mode ");
                SerialLogger::LogDec((int)g_w);
                SerialLogger::Log("x");
                SerialLogger::LogDec((int)g_h);
                SerialLogger::Log(" exact ");
                SerialLogger::LogDec(exact);
                SerialLogger::Log("/");
                SerialLogger::LogDec(total);
                SerialLogger::Log("\n");
                Scheduler::SleepMs(10000);
            }
        }
    }

    uint64_t t0 = Scheduler::NowMs();
    uint32_t done_frames = 0;
    int bx = 0, by = (int)g_h - 96;
    for (unsigned int f = 1; f <= 120; f++) {
        int nx = (int)((uint64_t)(g_w - 32) * f / 120u);
        int x1 = bx < nx ? bx : nx;
        int x2 = (bx > nx ? bx : nx) + 32;
        fill((uint32_t)bx, (uint32_t)by, 32, 32, 0);
        fill((uint32_t)nx, (uint32_t)by, 32, 32, frame_color(f));
        DispRect rr[2] = {
            { x1, by, (uint32_t)(x2 - x1), 32 },
            { nx, by, 32, 32 },
        };
        uint32_t s = 0;
        {
            SpinLockGuard g(g_lock);
            ok = send_frame_locked(false, rr, 2, &s);
        }
        if (!ok) {
            g_drops++;
            continue;
        }
        if (wait_done(s, 1000)) {
            done_frames++;
        } else {
            g_drops++;
            SerialLogger::Log("[klddisp] frame ");
            SerialLogger::LogDec((int)s);
            SerialLogger::Log(" dropped (timeout)\n");
            SpinLockGuard g(g_lock);
            g_frame_in_flight = false;
        }
        bx = nx;
    }
    uint64_t ms = Scheduler::NowMs() - t0;
    SerialLogger::Log("[klddisp] frames=120 done=");
    SerialLogger::LogDec((int)done_frames);
    SerialLogger::Log(" drops=");
    SerialLogger::LogDec((int)g_drops);
    SerialLogger::Log(" ms=");
    SerialLogger::LogDec((int)ms);
    SerialLogger::Log("\n");
    {
        SpinLockGuard g(g_lock);
        g_pattern_busy = false;
        g_restore_pending = true;   // the gui releases the instance (D2.8) (satoru)
    }
    // a kernel process entry must never return: the seeded frame has no
    // caller, so a return pops the alignment pad as rip and runs off the top
    // of this stack into the next guard page (the d2/d3 pattern-mode "kernel
    // stack overflow in timekeeper" panic right after this line). (satoru)
    for (;;) Scheduler::SleepMs(60000);
}

}  // namespace KldDisplay

// end (satoru)
