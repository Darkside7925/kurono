// error_screen: see error_screen.h. (satoru)
#include "error_screen.h"
#include "freeze.h"
#include "bootcfg.h"
#include "../kernel/kscreen.h"
#include "../kernel/panic.h"
#include "../drivers/keyboard.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"
#include "../drivers/disklog.h"
#include "../hal/hal.h"
#include "../proc/scheduler.h"
#include "../ui/notification.h"
#include "../shell/shell.h"

volatile bool g_kscreen_hold = false;

namespace ErrorScreen {
namespace {

// typed chars while the screen is up (keyboard.cpp diverts them here). (satoru)
char     g_ring[64];
volatile uint32_t g_rh = 0, g_rt = 0;

int pop_char() {
    uint32_t t = __atomic_load_n(&g_rt, __ATOMIC_ACQUIRE);
    if (t == __atomic_load_n(&g_rh, __ATOMIC_ACQUIRE)) return -1;
    char c = g_ring[t % 64];
    __atomic_store_n(&g_rt, t + 1, __ATOMIC_RELEASE);
    return (unsigned char)c;
}

const char* label(DevClass c) {
    switch (c) {
        case DC_DISPLAY:   return "display";
        case DC_STORAGE:   return "storage";
        case DC_NETWORK:   return "network";
        case DC_WIFI:      return "Wi-Fi";
        case DC_KEYBOARD:  return "keyboard";
        case DC_AUDIO:     return "sound";
        case DC_TOUCHPAD:  return "touchpad";
        case DC_BLUETOOTH: return "Bluetooth";
        case DC_CAMERA:    return "camera";
        case DC_USB:       return "USB device";
        case DC_SERVICE:   return "system service";
        default:           return "device";
    }
}

const char* area(const Failure& f) {
    if (f.kind == K_IOMMU) return "IOM";
    switch (f.dev_class) {
        case DC_DISPLAY:  return "DSP";
        case DC_STORAGE:  return "STO";
        case DC_NETWORK: case DC_WIFI: case DC_BLUETOOTH: return "NET";
        case DC_KEYBOARD: case DC_TOUCHPAD: return "INP";
        case DC_AUDIO:    return "AUD";
        default:          return "DRV";
    }
}

bool notify_tier(const Failure& f) {
    if (f.endangers || f.kind == K_IOMMU) return false;
    switch (f.dev_class) {
        case DC_WIFI: case DC_BLUETOOTH: case DC_AUDIO: case DC_TOUCHPAD:
        case DC_CAMERA: case DC_USB: case DC_NETWORK:
            return true;
        case DC_DISPLAY: case DC_KEYBOARD:
            return !f.only_device;   // a second monitor, an extra keyboard (satoru)
        default:
            return false;
    }
}

void sapp(char* d, int cap, const char* s) {
    int n = 0; while (d[n]) n++;
    while (s && *s && n < cap - 1) d[n++] = *s++;
    d[n] = 0;
}
void hexn(char* d, int cap, uint32_t v, int digits) {
    static const char* h = "0123456789abcdef";
    char t[9]; int i = 0;
    for (int k = digits - 1; k >= 0; k--) t[i++] = h[(v >> (k * 4)) & 0xF];
    t[i] = 0;
    sapp(d, cap, t);
}

// "Wi-Fi" at the start of a sentence, "display" inside one. (satoru)
void cap_first(char* d, int cap, const char* s) {
    d[0] = 0;
    sapp(d, cap, s);
    if (d[0] >= 'a' && d[0] <= 'z') d[0] = (char)(d[0] - 32);
}

bool get_fb(KScreen::Fb* fb) {
    return KernelPanic::GetFramebuffer(&fb->addr, &fb->pitch, &fb->width, &fb->height, &fb->bpp);
}

// wait ms while turning the spinner. (satoru)
void wait_spin(const KScreen::Fb& fb, KScreen::Screen& s, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
        s.progress++;
        KScreen::DrawProgress(fb, s);
        Scheduler::SleepMs(100);
    }
}

// ---- the terminal: the kernel shell on a plain console, everything else
// still paused. "exit" returns to the choices. (satoru)
const int TL = 120, TW = 132;
char g_lines[TL][TW];
const char* g_lp[TL];
int g_nlines = 0;

void tty_push(const char* s, int n) {
    if (g_nlines == TL) {
        for (int i = 1; i < TL; i++) for (int k = 0; k < TW; k++) g_lines[i - 1][k] = g_lines[i][k];
        g_nlines--;
    }
    int k = 0;
    for (; k < n && k < TW - 1; k++) g_lines[g_nlines][k] = (s[k] == '\t') ? ' ' : s[k];
    g_lines[g_nlines][k] = 0;
    g_nlines++;
}

void tty_out(const char* out) {
    int i = 0;
    while (out[i]) {
        int j = i;
        while (out[j] && out[j] != '\n') j++;
        tty_push(out + i, j - i);
        i = out[j] ? j + 1 : j;
    }
}

void terminal(const KScreen::Fb& fb, KScreen::Tone tone) {
    g_nlines = 0;
    tty_push("Kurono terminal. Your apps stay paused. Type exit to go back.", 62);
    char in[128]; int il = 0; in[0] = 0;
    static char out[8192];
    bool dirty = true;
    for (;;) {
        if (dirty) {
            for (int i = 0; i < g_nlines; i++) g_lp[i] = g_lines[i];
            KScreen::DrawConsole(fb, tone, g_lp, g_nlines, in);
            dirty = false;
        }
        int c = pop_char();
        if (c < 0) { Scheduler::SleepMs(20); continue; }
        dirty = true;
        if (c == '\b' || c == 127) { if (il) in[--il] = 0; continue; }
        if (c == '\n' || c == '\r') {
            char echo[140]; echo[0] = 0;
            sapp(echo, sizeof(echo), "> "); sapp(echo, sizeof(echo), in);
            tty_push(echo, 140);
            if (in[0] == 'e' && in[1] == 'x' && in[2] == 'i' && in[3] == 't' && !in[4]) return;
            if (il) {
                out[0] = 0;
                KuronoShell::Execute(in, out, sizeof(out));
                tty_out(out);
            }
            il = 0; in[0] = 0;
            continue;
        }
        if (c >= 32 && c < 127 && il < 126) { in[il++] = (char)c; in[il] = 0; }
    }
}

volatile bool g_active = false;

void resume_desktop() {
    g_kscreen_hold = false;
    g_active = false;
    Graphics::MarkDirty(0, 0, Graphics::GetWidth(), Graphics::GetHeight());
    Graphics::SwapBuffers();
    Graphics::PresentVirtioIfActive();
}

}  // namespace

bool Active() { return g_active; }

void BeginExclusive() { g_rh = g_rt = 0; g_active = true; g_kscreen_hold = true; }
void EndExclusive() { resume_desktop(); }
int  PopChar() { return pop_char(); }

void OnChar(char c) {
    uint32_t h = __atomic_load_n(&g_rh, __ATOMIC_RELAXED);
    if (h - __atomic_load_n(&g_rt, __ATOMIC_ACQUIRE) >= 64) return;
    g_ring[h % 64] = c;
    __atomic_store_n(&g_rh, h + 1, __ATOMIC_RELEASE);
}

Outcome Handle(const Failure& f) {
    char name[24]; cap_first(name, sizeof(name), label(f.dev_class));
    char msg[96];

    if (notify_tier(f)) {
        // notify tier: never takes the screen. two quiet retries. (satoru)
        msg[0] = 0; sapp(msg, sizeof(msg), name); sapp(msg, sizeof(msg), " stopped working. Kurono is restarting it.");
        NotificationManager::Post(name, msg, NotificationManager::ICON_WARNING, 4000);
        for (int a = 1; a <= 2; a++) {
            Scheduler::SleepMs(a == 1 ? 1000 : 4000);
            if (f.retry && f.retry(f.ctx, a)) {
                msg[0] = 0; sapp(msg, sizeof(msg), name); sapp(msg, sizeof(msg), " stopped working. Kurono restarted it.");
                NotificationManager::Post(name, msg, NotificationManager::ICON_SUCCESS, 4000);
                return O_RESTARTED;
            }
        }
        msg[0] = 0; sapp(msg, sizeof(msg), name);
        sapp(msg, sizeof(msg), " isn't working right now. Kurono tried to restart it twice.");
        NotificationManager::Post(name, msg, NotificationManager::ICON_WARNING, 8000);
        return O_NOTIFIED;
    }

    KScreen::Fb fb;
    if (!get_fb(&fb)) {
        SerialLogger::Log("[errscreen] no framebuffer, falling back to notifications\r\n");
        return O_NOTIFIED;
    }

    // blue: take the screen and the keyboard, pause everything. (satoru)
    g_rh = g_rt = 0;
    g_active = true;
    g_kscreen_hold = true;
    ProcessFreeze::FreezeAll();

    char code[16];
    KScreen::FormatStopCode(code, area(f), ((uint32_t)f.kind << 8) | (uint32_t)f.dev_class);
    static char tech[160];
    tech[0] = 0;
    sapp(tech, sizeof(tech), f.family ? f.family : "driver");
    if (f.bdf) {
        sapp(tech, sizeof(tech), "  0000:");
        hexn(tech, sizeof(tech), (f.bdf >> 8) & 0xFF, 2); sapp(tech, sizeof(tech), ":");
        hexn(tech, sizeof(tech), (f.bdf >> 3) & 0x1F, 2); sapp(tech, sizeof(tech), ".");
        hexn(tech, sizeof(tech), f.bdf & 7, 1);
    }
    if (f.vendor) {
        sapp(tech, sizeof(tech), "  ");
        hexn(tech, sizeof(tech), f.vendor, 4); sapp(tech, sizeof(tech), ":"); hexn(tech, sizeof(tech), f.device, 4);
    }
    if (f.detail) { sapp(tech, sizeof(tech), "  "); sapp(tech, sizeof(tech), f.detail); }

    static char head[96], body[200];
    const char* low = label(f.dev_class);
    head[0] = 0;
    sapp(head, sizeof(head), "Your "); sapp(head, sizeof(head), low);
    sapp(head, sizeof(head), f.kind == K_HANG ? " driver stopped responding." : " driver stopped working.");
    body[0] = 0;
    sapp(body, sizeof(body), "Kurono paused your apps so nothing is lost and is restarting the driver.");

    KScreen::Screen s;
    KScreen::InitScreen(s);
    s.tone = KScreen::TONE_BLUE;
    s.headline = head; s.body = body; s.code = code; s.tech = tech;
    s.progress = 0;
    static char ptext[64];
    s.progress_text = ptext;
    SerialLogger::Log("[errscreen] blue: "); SerialLogger::Log(code); SerialLogger::Log(" "); SerialLogger::Log(tech); SerialLogger::Log("\r\n");

    for (int a = 1; a <= 2; a++) {
        ptext[0] = 0;
        sapp(ptext, sizeof(ptext), a == 1 ? "Restarting the driver, attempt 1 of 2" : "Restarting the driver, attempt 2 of 2");
        KScreen::Draw(fb, s);
        wait_spin(fb, s, a == 1 ? 1000 : 4000);
        if (f.retry && f.retry(f.ctx, a)) {
            SerialLogger::Log("[errscreen] restarted on attempt "); SerialLogger::LogDec(a); SerialLogger::Log("\r\n");
            ProcessFreeze::ThawAll();
            resume_desktop();
            msg[0] = 0; sapp(msg, sizeof(msg), "The "); sapp(msg, sizeof(msg), low);
            sapp(msg, sizeof(msg), " driver stopped and was restarted.");
            NotificationManager::Post(name, msg, NotificationManager::ICON_SUCCESS, 5000);
            return O_RESTARTED;
        }
    }

    // both reloads failed: the choices (recovery_PLAN.md 1.4). (satoru)
    head[0] = 0;
    sapp(head, sizeof(head), "Your "); sapp(head, sizeof(head), low); sapp(head, sizeof(head), " driver couldn't be restarted.");
    body[0] = 0;
    if (f.only_device)
        sapp(body, sizeof(body), "Your apps are still paused. Kurono needs this device, so it can save what it can and restart into Recovery.");
    else
        sapp(body, sizeof(body), "Your apps are still paused and safe. You can keep working without it, or restart.");
    enum { C_CONT, C_BASIC, C_TERM, C_RECOV, C_RESTART, C_SAVE };
    const char* labels[6]; int ids[6]; int n = 0;
    if (f.only_device) { labels[n] = "Save open work and restart"; ids[n++] = C_SAVE; }
    else { labels[n] = "Continue without it"; ids[n++] = C_CONT; }
    if (f.basic_label && f.use_basic) { labels[n] = f.basic_label; ids[n++] = C_BASIC; }
    labels[n] = "Open terminal"; ids[n++] = C_TERM;
    if (!f.only_device) { labels[n] = "Restart into Recovery"; ids[n++] = C_RECOV; }
    labels[n] = "Restart"; ids[n++] = C_RESTART;
    s.progress = -1;
    s.choices = labels; s.nchoices = n; s.selected = 0;
    static char tech2[180];
    tech2[0] = 0; sapp(tech2, sizeof(tech2), tech); sapp(tech2, sizeof(tech2), "  (2 reloads failed)");
    s.tech = tech2;

    bool left_was = false, right_was = false;
    KScreen::Draw(fb, s);
    for (;;) {
        bool redraw = false;
        bool l = Keyboard::IsKeyDown(KEY_LEFT), r = Keyboard::IsKeyDown(KEY_RIGHT);
        if (l && !left_was) { s.selected = (s.selected + n - 1) % n; redraw = true; }
        if (r && !right_was) { s.selected = (s.selected + 1) % n; redraw = true; }
        left_was = l; right_was = r;
        int c = pop_char();
        int pick = -1;
        if (c == '\t') { s.selected = (s.selected + 1) % n; redraw = true; }
        else if (c >= '1' && c < '1' + n) { s.selected = c - '1'; redraw = true; pick = s.selected; }
        else if (c == '\n' || c == '\r' || c == ' ') pick = s.selected;
        if (redraw) KScreen::Draw(fb, s);
        if (pick < 0) { Scheduler::SleepMs(30); continue; }
        int id = ids[pick];
        SerialLogger::Log("[errscreen] choice: "); SerialLogger::Log(labels[pick]); SerialLogger::Log("\r\n");
        if (id == C_CONT) {
            ProcessFreeze::ThawAll();
            resume_desktop();
            msg[0] = 0; sapp(msg, sizeof(msg), name); sapp(msg, sizeof(msg), " is off until the next restart.");
            NotificationManager::Post(name, msg, NotificationManager::ICON_WARNING, 6000);
            return O_CONTINUE;
        }
        if (id == C_BASIC) {
            if (f.use_basic(f.ctx)) {
                ProcessFreeze::ThawAll();
                resume_desktop();
                return O_BASIC;
            }
            body[0] = 0; sapp(body, sizeof(body), "The basic driver did not start either.");
            KScreen::Draw(fb, s);
            continue;
        }
        if (id == C_TERM) {
            terminal(fb, KScreen::TONE_BLUE);
            KScreen::Draw(fb, s);
            continue;
        }
        if (id == C_RECOV || id == C_SAVE) {
            bool ok = BootCfg::RequestRecoveryOnce();
            SerialLogger::Log(ok ? "[errscreen] recovery requested for the next boot\r\n"
                                 : "[errscreen] KBOOT.CFG missing: hold R at power-on for Recovery\r\n");
        }
        // restart. the log goes to the KURONO partition first. (satoru)
        DiskLog::Flush();
        HAL::Reboot();
        return O_REBOOTING;
    }
}

// ---- fault injection for testing (shell `errscreen`) ---------------------
namespace {
int g_ok_attempt = 0;
bool test_retry(void*, int attempt) { return g_ok_attempt && attempt >= g_ok_attempt; }
bool test_basic(void*) { return true; }
bool streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
}

int TestCommand(int argc, const char** argv, char* out, int mx) {
    Failure f;
    f.family = "i915"; f.dev_class = DC_DISPLAY; f.kind = K_HANG; f.bdf = (0 << 8) | (2 << 3);
    f.vendor = 0x8086; f.device = 0x7d67; f.detail = "hang: no heartbeat for 5 s";
    f.only_device = true; f.endangers = false; f.basic_label = "Use basic display";
    f.retry = test_retry; f.use_basic = test_basic; f.ctx = nullptr;
    const char* cls = argc >= 2 ? argv[1] : "display";
    if (streq(cls, "wifi")) {
        f.family = "iwlwifi"; f.dev_class = DC_WIFI; f.kind = K_CRASH; f.bdf = (0x82 << 8);
        f.vendor = 0x8086; f.device = 0x272b; f.detail = "exit=SIGSEGV"; f.only_device = false; f.basic_label = nullptr;
    } else if (streq(cls, "disk")) {
        f.family = "nvme"; f.dev_class = DC_STORAGE; f.kind = K_HANG; f.bdf = (0x01 << 8);
        f.vendor = 0x144d; f.device = 0xa80a; f.detail = "io timeout"; f.basic_label = nullptr;
    } else if (streq(cls, "second")) {
        f.only_device = false;   // a second monitor: notify tier (satoru)
    } else if (streq(cls, "iommu")) {
        f.family = "iwlwifi"; f.dev_class = DC_WIFI; f.kind = K_IOMMU; f.detail = "iommu faults: 40 in 1 s";
        f.only_device = false; f.basic_label = "Use the wired network";
    }
    const char* mode = argc >= 3 ? argv[2] : "fail";
    g_ok_attempt = streq(mode, "ok") ? 1 : (streq(mode, "ok2") ? 2 : 0);
    Outcome o = Handle(f);
    const char* names[] = { "notified", "restarted", "continue", "basic", "rebooting" };
    int p = 0;
    const char* a = "errscreen: outcome ";
    while (*a && p < mx - 1) out[p++] = *a++;
    a = names[o];
    while (*a && p < mx - 2) out[p++] = *a++;
    out[p++] = '\n'; out[p] = 0;
    SerialLogger::Log(out);
    return p;
}

}  // namespace ErrorScreen

// end (satoru)
