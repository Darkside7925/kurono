// recovery: the Kurono Recovery boot mode (recovery R6/R7/R8). see recovery.h. (satoru)
#include "recovery.h"
#include "bootcfg.h"
#include "../kernel/kscreen.h"
#include "../kernel/kcrash.h"
#include "../kernel/panic.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../drivers/disklog.h"
#include "../drivers/blockdev.h"
#include "../drivers/timer.h"
#include "../drivers/usb.h"
#include "../kernel/time.h"
#include "../proc/scheduler.h"
#include "../hal/hal.h"
#include "../shell/shell.h"

namespace Recovery {
namespace {

KScreen::Fb g_fb;
int g_reason = 0;

void sapp(char* d, int cap, const char* s) {
    int n = 0; while (d[n]) n++;
    while (s && *s && n < cap - 1) d[n++] = *s++;
    d[n] = 0;
}
void sdec(char* d, int cap, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v && n < 23) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[24]; int k = 0;
    while (n) r[k++] = t[--n];
    r[k] = 0;
    sapp(d, cap, r);
}
void shex(char* d, int cap, uint32_t v, int digits) {
    static const char* h = "0123456789abcdef";
    char t[9]; int i = 0;
    for (int k = digits - 1; k >= 0; k--) t[i++] = h[(v >> (k * 4)) & 0xF];
    t[i] = 0;
    sapp(d, cap, t);
}

// ---- input: the cli loop's way (no desktop runs in this mode) --------------
enum Key { K_NONE, K_UP, K_DOWN, K_ENTER, K_BACK, K_CHAR };
int g_char = 0;

void pump() {
    uint32_t el = Timer::ElapsedSinceLast();
    if (el) TimeManager::AdvanceByMs(el);
    Scheduler::Tick();
    Keyboard::Poll();
    USB::PollHID();   // usb keyboards: no input process runs in this mode (satoru)
}

// key edges straight from the driver callback (no desktop drains the input
// manager's ring in this mode, and a quick press can fall between two polls
// of the key state). (satoru)
volatile uint32_t g_kh = 0, g_kt = 0;
uint8_t g_kring[32];
void key_cb(::Key k, char c, bool pressed) {
    (void)c;
    if (!pressed) return;
    uint8_t v = k == KEY_UP ? 1 : k == KEY_DOWN ? 2 : k == KEY_ESC ? 3 : 0;
    if (!v) return;
    uint32_t h = g_kh;
    if (h - g_kt >= 32) return;
    g_kring[h % 32] = v;
    __atomic_store_n(&g_kh, h + 1, __ATOMIC_RELEASE);
}

Key next_key() {
    for (;;) {
        pump();
        if (g_kt != __atomic_load_n(&g_kh, __ATOMIC_ACQUIRE)) {
            uint8_t v = g_kring[g_kt % 32];
            g_kt++;
            if (v == 1) return K_UP;
            if (v == 2) return K_DOWN;
            if (v == 3) return K_BACK;
        }
        if (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == '\n' || c == '\r' || c == ' ') return K_ENTER;
            if (c == 27 || c == 8) return K_BACK;
            if (c == 'k' || c == 'w') return K_UP;
            if (c == 'j' || c == 's') return K_DOWN;
            g_char = (unsigned char)c;
            return K_CHAR;
        }
        for (volatile int i = 0; i < 20000; i++) __asm__ __volatile__("pause");
    }
}

// a menu page. returns the chosen row, or -1 for back. (satoru)
int menu(const char* title, const char* sub, const KScreen::MenuItem* items, int n, int start = 0) {
    int sel = start < n ? start : 0;
    for (;;) {
        KScreen::DrawMenu(g_fb, title, sub, items, n, sel, "Up and Down to move, Enter to choose, Esc to go back");
        Key k = next_key();
        if (k == K_UP) sel = (sel + n - 1) % n;
        else if (k == K_DOWN) sel = (sel + 1) % n;
        else if (k == K_ENTER) return sel;
        else if (k == K_BACK) return -1;
        else if (k == K_CHAR && g_char >= '1' && g_char < '1' + n) return g_char - '1';
    }
}

void message(const char* title, const char* text) {
    KScreen::MenuItem ok[1] = { { "OK", nullptr } };
    menu(title, text, ok, 1);
}

[[noreturn]] void reboot() {
    DiskLog::Flush();
    SerialLogger::Log("[recovery] restarting\r\n");
    HAL::Reboot();
    for (;;) __asm__ __volatile__("hlt");
}

// ---- terminal: the kernel shell (plus the embedded rescue toolbox through
// its `recovery` command) on a plain console. "exit" returns. (satoru)
const int TL = 150, TW = 132;
char g_lines[TL][TW];
const char* g_lp[TL];
int g_nl = 0;
void push(const char* s, int n) {
    if (g_nl == TL) {
        for (int i = 1; i < TL; i++) for (int k = 0; k < TW; k++) g_lines[i - 1][k] = g_lines[i][k];
        g_nl--;
    }
    int k = 0;
    for (; k < n && k < TW - 1; k++) g_lines[g_nl][k] = s[k] == '\t' ? ' ' : s[k];
    g_lines[g_nl][k] = 0;
    g_nl++;
}
void terminal() {
    g_nl = 0;
    const char* intro = "Kurono Recovery terminal. Rescue tools: type recovery. Type exit to go back.";
    push(intro, 80);
    char in[160]; int il = 0; in[0] = 0;
    static char out[16384];
    for (;;) {
        for (int i = 0; i < g_nl; i++) g_lp[i] = g_lines[i];
        KScreen::DrawConsole(g_fb, KScreen::TONE_BLUE, g_lp, g_nl, in);
        bool got = false;
        while (!got) {
            pump();
            while (Keyboard::HasChar()) {
                char c = Keyboard::GetChar();
                got = true;
                if (c == 8 || c == 127) { if (il) in[--il] = 0; continue; }
                if (c == '\n' || c == '\r') {
                    char echo[170]; echo[0] = 0;
                    sapp(echo, sizeof(echo), "> "); sapp(echo, sizeof(echo), in);
                    push(echo, 170);
                    if (in[0] == 'e' && in[1] == 'x' && in[2] == 'i' && in[3] == 't' && !in[4]) return;
                    if (il) {
                        out[0] = 0;
                        KuronoShell::Execute(in, out, sizeof(out));
                        int i = 0;
                        while (out[i]) {
                            int j = i;
                            while (out[j] && out[j] != '\n') j++;
                            push(out + i, j - i);
                            i = out[j] ? j + 1 : j;
                        }
                    }
                    il = 0; in[0] = 0;
                    continue;
                }
                if ((unsigned char)c >= 32 && (unsigned char)c < 127 && il < 158) { in[il++] = c; in[il] = 0; }
            }
            if (!got) for (volatile int i = 0; i < 20000; i++) __asm__ __volatile__("pause");
        }
    }
}

// ---- what went wrong last ------------------------------------------------
bool g_have_rec = false;
KCrash::Record g_rec;
char g_issue[160];
char g_issue_code[16];

void describe_issue() {
    g_issue[0] = 0; g_issue_code[0] = 0;
    g_have_rec = KCrash::LastRecord(&g_rec);
    uint32_t panic_stop = KernelPanic::LastRecoveredStopCode();
    if (panic_stop != 0xFFFFFFFFu) {
        KScreen::PanicStopCode(panic_stop, g_issue_code);
        sapp(g_issue, sizeof(g_issue), "The kernel stopped with ");
        sapp(g_issue, sizeof(g_issue), g_issue_code);
        sapp(g_issue, sizeof(g_issue), " and restarted.");
        return;
    }
    if (g_have_rec) {
        char fam[17];
        for (int i = 0; i < 16; i++) fam[i] = g_rec.family[i];
        fam[16] = 0;
        sapp(g_issue, sizeof(g_issue), "The ");
        sapp(g_issue, sizeof(g_issue), fam);
        sapp(g_issue, sizeof(g_issue), g_rec.signal == 9 ? " driver stopped responding" : " driver crashed");
        sapp(g_issue, sizeof(g_issue), g_rec.result == KCrash::RES_GAVE_UP ? " and could not be restarted." : ".");
        KScreen::FormatStopCode(g_issue_code, "DRV", g_rec.signal == 9 ? 0x0200 : 0x0100);
        return;
    }
    if (g_reason == 2) sapp(g_issue, sizeof(g_issue), "Kurono did not reach the desktop twice in a row.");
}

// ---- pages -----------------------------------------------------------------
void page_fix() {
    describe_issue();
    char fam[17]; fam[0] = 0;
    if (g_have_rec) { for (int i = 0; i < 16; i++) fam[i] = g_rec.family[i]; fam[16] = 0; }
    static char turn_off[48];
    turn_off[0] = 0;
    sapp(turn_off, sizeof(turn_off), "Turn off "); sapp(turn_off, sizeof(turn_off), fam);
    KScreen::MenuItem items[5];
    int ids[5]; int n = 0;
    items[n] = { "Start with drivers disabled", "Linux drivers stay off for one start" }; ids[n++] = 0;
    if (fam[0]) { items[n] = { turn_off, "Kurono starts without this driver until you turn it on" }; ids[n++] = 1; }
    items[n] = { "Start the previous version", "Runs the last Kurono that reached the desktop, once" }; ids[n++] = 2;
    items[n] = { "Open terminal", "For the rescue tools" }; ids[n++] = 3;
    const char* sub = g_issue[0] ? g_issue : "No recent problem was recorded.";
    int c = menu("Fix last issue", sub, items, n);
    if (c < 0) return;
    switch (ids[c]) {
        case 0: BootCfg::RequestSafeStartOnce(); reboot();
        case 1:
            if (KCrash::SetDriverPolicy(fam, 1)) message("Driver turned off", "It stays off until you turn it on in Drivers.");
            else message("Could not save", "KDRIVERS.CFG is missing on the KURONO partition.");
            return;
        case 2:
            if (BootCfg::RequestPreviousOnce()) reboot();
            message("Not available", "There is no previous version recorded yet.");
            return;
        case 3: terminal(); return;
    }
}

void page_drivers() {
    int start = 0;
    for (;;) {
        int n = KCrash::DriverCount();
        if (n <= 0) { message("Drivers", "No driver has failed yet, so there is nothing to change."); return; }
        if (n > 32) n = 32;
        static char names[32][17], subs[32][48];
        KScreen::MenuItem items[32];
        for (int i = 0; i < n; i++) {
            uint8_t pol = 0; uint32_t fails = 0;
            KCrash::DriverAt(i, names[i], &pol, &fails);
            subs[i][0] = 0;
            sapp(subs[i], 48, pol == 1 ? "Off" : "On");
            sapp(subs[i], 48, ", ");
            sdec(subs[i], 48, fails);
            sapp(subs[i], 48, fails == 1 ? " failure" : " failures");
            items[i] = { names[i], subs[i] };
        }
        int c = menu("Drivers", "Enter turns a driver on or off for the next starts.", items, n, start);
        if (c < 0) return;
        uint8_t pol = 0; uint32_t fails = 0;
        KCrash::DriverAt(c, names[c], &pol, &fails);
        KCrash::SetDriverPolicy(names[c], pol == 1 ? 0 : 1);
        start = c;
    }
}

void page_restore() {
    BootCfg::State s;
    if (!BootCfg::Read(&s)) {
        message("Restore", "Restore needs KBOOT.CFG on the KURONO partition (start once with kurono.kcrash=1, or reinstall).");
        return;
    }
    static char cur[64], prev[64];
    cur[0] = 0; sapp(cur, sizeof(cur), "Now: "); sapp(cur, sizeof(cur), s.current[0] ? s.current : "built in");
    prev[0] = 0; sapp(prev, sizeof(prev), "Previous: "); sapp(prev, sizeof(prev), s.previous[0] ? s.previous : "none yet");
    KScreen::MenuItem items[2] = {
        { "Start the previous version once", prev },
        { "Make the previous version the default", cur },
    };
    int c = menu("Restore", "Kurono keeps the version that last reached the desktop.", items, 2);
    if (c == 0) {
        if (s.previous[0] && BootCfg::RequestPreviousOnce()) reboot();
        message("Not available", "There is no previous version recorded yet.");
    } else if (c == 1) {
        if (!s.previous[0]) { message("Not available", "There is no previous version recorded yet."); return; }
        BootCfg::Update([](BootCfg::State& st, void*) {
            char t[32];
            for (int i = 0; i < 32; i++) t[i] = st.current[i];
            for (int i = 0; i < 32; i++) st.current[i] = st.previous[i];
            for (int i = 0; i < 32; i++) st.previous[i] = t[i];
            // "EMBEDDED" is how the loader names the kernel inside itself. (satoru)
            if (st.current[0] == 'E' && st.current[1] == 'M') st.current[0] = 0;
        }, nullptr);
        message("Done", "The previous version starts by default now.");
    }
}

void page_disks() {
    int n = BlockDev::Count();
    if (n > 16) n = 16;
    static char lab[16][80], sub[16][64];
    KScreen::MenuItem items[17];
    int k = 0;
    for (int i = 0; i < n; i++) {
        BlockDevice* d = BlockDev::Get(i);
        if (!d || !d->present) continue;
        lab[k][0] = 0; sapp(lab[k], 80, d->name); sapp(lab[k], 80, "  "); sapp(lab[k], 80, d->model);
        sub[k][0] = 0;
        uint64_t gb = d->capacity_lba * d->sector_size / 1000000000ull;
        sdec(sub[k], 64, gb); sapp(sub[k], 64, " GB, "); sapp(sub[k], 64, d->driver);
        sapp(sub[k], 64, d->removable ? ", removable" : "");
        items[k] = { lab[k], sub[k] };
        k++;
    }
    items[k++] = { "Open terminal", "ntfsfix, e2fsck, fsck.fat, testdisk, photorec: type recovery" };
    int c = menu("Files & disks", "Disks are read only here. Repairs run only from the terminal.", items, k);
    if (c == k - 1) terminal();
}

// ---- export diagnostics: a text report on the KURONO partition, no secrets ----
void page_export() {
    static char rep[24576];
    rep[0] = 0;
    const int cap = (int)sizeof(rep);
    sapp(rep, cap, "Kurono diagnostics\n\n");
    BootCfg::State s;
    if (BootCfg::Read(&s)) {
        sapp(rep, cap, "boot_count="); sdec(rep, cap, s.boot_count);
        sapp(rep, cap, " fail_streak="); sdec(rep, cap, s.fail_streak);
        sapp(rep, cap, " flags=0x"); shex(rep, cap, s.flags, 8);
        sapp(rep, cap, "\ncurrent="); sapp(rep, cap, s.current);
        sapp(rep, cap, " previous="); sapp(rep, cap, s.previous);
        sapp(rep, cap, " last_good="); sapp(rep, cap, s.last_good);
        sapp(rep, cap, " trial="); sapp(rep, cap, s.trial);
        sapp(rep, cap, "\n");
    } else sapp(rep, cap, "KBOOT.CFG: none\n");
    describe_issue();
    sapp(rep, cap, "last issue: "); sapp(rep, cap, g_issue[0] ? g_issue : "none"); sapp(rep, cap, "\n");
    int dn = KCrash::DriverCount();
    for (int i = 0; i < dn && i < 64; i++) {
        char fam[17]; uint8_t pol = 0; uint32_t fails = 0;
        if (!KCrash::DriverAt(i, fam, &pol, &fails)) continue;
        sapp(rep, cap, "driver "); sapp(rep, cap, fam); sapp(rep, cap, pol == 1 ? " off" : " on");
        sapp(rep, cap, " fails="); sdec(rep, cap, fails); sapp(rep, cap, "\n");
    }
    for (int i = 0; i < BlockDev::Count() && i < 16; i++) {
        BlockDevice* d = BlockDev::Get(i);
        if (!d || !d->present) continue;
        sapp(rep, cap, "disk "); sapp(rep, cap, d->name); sapp(rep, cap, " "); sapp(rep, cap, d->model);
        sapp(rep, cap, " lba="); sdec(rep, cap, d->capacity_lba); sapp(rep, cap, "\n");
    }
    // pci ids (bus 0..63 is enough for a laptop). (satoru)
    for (uint32_t bus = 0; bus < 64; bus++)
        for (uint32_t dev = 0; dev < 32; dev++)
            for (uint32_t fn = 0; fn < 8; fn++) {
                uint32_t a = (1u << 31) | (bus << 16) | (dev << 11) | (fn << 8);
                uint32_t id, cls;
                __asm__ __volatile__("outl %0, %1" : : "a"(a), "Nd"((uint16_t)0xCF8));
                __asm__ __volatile__("inl %1, %0" : "=a"(id) : "Nd"((uint16_t)0xCFC));
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                __asm__ __volatile__("outl %0, %1" : : "a"(a | 8), "Nd"((uint16_t)0xCF8));
                __asm__ __volatile__("inl %1, %0" : "=a"(cls) : "Nd"((uint16_t)0xCFC));
                sapp(rep, cap, "pci "); shex(rep, cap, bus, 2); sapp(rep, cap, ":"); shex(rep, cap, dev, 2);
                sapp(rep, cap, "."); shex(rep, cap, fn, 1); sapp(rep, cap, " ");
                shex(rep, cap, id & 0xFFFF, 4); sapp(rep, cap, ":"); shex(rep, cap, id >> 16, 4);
                sapp(rep, cap, " class "); shex(rep, cap, cls >> 8, 6); sapp(rep, cap, "\n");
            }
    // the last log lines, without anything that could carry a secret. (satoru)
    sapp(rep, cap, "\nlog tail:\n");
    const char* log = SerialLogger::CaptureData();
    uint32_t len = SerialLogger::CaptureLen();
    uint32_t from = len > 8192 ? len - 8192 : 0;
    while (from < len && log[from] != '\n') from++;
    int n = 0; while (rep[n]) n++;
    uint32_t i = from;
    while (i < len && n < cap - 2) {
        uint32_t j = i;
        while (j < len && log[j] != '\n') j++;
        bool secret = false;
        for (uint32_t k = i; k + 3 < j; k++) {
            char a = log[k], b = log[k + 1], c = log[k + 2];
            if ((a == 'P' && b == 'S' && c == 'K') || (a == 'p' && b == 'a' && c == 's' && log[k + 3] == 's') ||
                (a == 'e' && b == 'n' && c == 'v' && log[k + 3] == '=')) { secret = true; break; }
        }
        if (!secret) for (uint32_t k = i; k <= j && k < len && n < cap - 2; k++) rep[n++] = log[k];
        i = j + 1;
    }
    rep[n] = 0;
    // KDIAG001.TXT .. KDIAG999.TXT: the first free name (new files only). (satoru)
    char name83[12];
    int wr = -1;
    for (int num = 1; num < 1000 && wr != 0; num++) {
        const char* b = "KDIAG";
        for (int k = 0; k < 5; k++) name83[k] = b[k];
        name83[5] = (char)('0' + num / 100); name83[6] = (char)('0' + (num / 10) % 10); name83[7] = (char)('0' + num % 10);
        name83[8] = 'T'; name83[9] = 'X'; name83[10] = 'T'; name83[11] = 0;
        uint64_t sz = 0;
        if (DiskLog::StatRootFile(name83, &sz) == 0) continue;   // taken (satoru)
        wr = DiskLog::WriteRootFile(name83, rep, (uint32_t)n);
        if (wr != 0 && wr != 7) break;
    }
    static char msg[96];
    msg[0] = 0;
    if (wr == 0) {
        sapp(msg, sizeof(msg), "Saved as ");
        char nice[13]; int p = 0;
        for (int k = 0; k < 8; k++) nice[p++] = name83[k];
        nice[p++] = '.'; nice[p++] = 'T'; nice[p++] = 'X'; nice[p++] = 'T'; nice[p] = 0;
        sapp(msg, sizeof(msg), nice);
        sapp(msg, sizeof(msg), " on the KURONO partition.");
        SerialLogger::Log("[recovery] diagnostics exported\r\n");
    } else {
        sapp(msg, sizeof(msg), "Could not write to the KURONO partition (");
        sapp(msg, sizeof(msg), DiskLog::RootFileMsg(wr));
        sapp(msg, sizeof(msg), ").");
    }
    message("Export diagnostics", msg);
}

}  // namespace

[[noreturn]] void Run(int reason) {
    g_reason = reason;
    if (!KernelPanic::GetFramebuffer(&g_fb.addr, &g_fb.pitch, &g_fb.width, &g_fb.height, &g_fb.bpp)) {
        SerialLogger::Log("[recovery] no framebuffer: staying in the cli\r\n");
        for (;;) __asm__ __volatile__("hlt");
    }
    SerialLogger::Log("[recovery] Kurono Recovery, reason=");
    SerialLogger::LogDec(reason);
    SerialLogger::Log("\r\n");
    // the crash store and the driver policy file (KURONO partition only). (satoru)
    KCrash::BootInit(true);
    Keyboard::FlushBuffers();
    Keyboard::SetCallback(key_cb);
    const char* why = reason == 1 ? "You asked to start in Recovery."
                    : reason == 2 ? "Kurono couldn't start twice in a row. Fix last issue shows why."
                    : reason == 3 ? "R was held at power-on."
                    : "Kurono Recovery";
    for (;;) {
        describe_issue();
        static char fix_sub[96];
        fix_sub[0] = 0;
        sapp(fix_sub, sizeof(fix_sub), g_issue[0] ? g_issue : "No recent problem recorded");
        KScreen::MenuItem home[8] = {
            { "Fix last issue", fix_sub },
            { "Drivers", "Turn drivers on or off" },
            { "Restore", "Previous versions of Kurono" },
            { "Files & disks", "Your disks and the rescue tools" },
            { "Terminal", "Kernel shell" },
            { "Export diagnostics", "Save a report to the KURONO partition" },
            { "Start Kurono", "Restart normally" },
            { "Start with drivers disabled", "Linux drivers stay off for one start" },
        };
        int c = menu("Kurono Recovery", why, home, 8);
        switch (c) {
            case 0: page_fix(); break;
            case 1: page_drivers(); break;
            case 2: page_restore(); break;
            case 3: page_disks(); break;
            case 4: terminal(); break;
            case 5: page_export(); break;
            case 6: reboot();
            case 7: BootCfg::RequestSafeStartOnce(); reboot();
            default: break;
        }
    }
}

}  // namespace Recovery

// end (satoru)
