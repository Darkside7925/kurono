//  kurono os - minimal virtual i8042 (see vi8042.h) (satoru)
#include "vi8042.h"
#include "vdevices.h"

namespace {
    // controller / device replies: vm thread only (satoru)
    struct Reply { uint8_t b; bool aux; };
    Reply    g_rep[16];
    int      g_rep_h = 0, g_rep_n = 0;
    // host-typed scancodes: single producer (any thread, serialized by a flag),
    // single consumer (the vm thread) (satoru)
    const uint32_t KQ = 1024;
    uint8_t  g_kq[KQ];
    volatile uint32_t g_kq_w = 0, g_kq_r = 0;
    volatile uint32_t g_inj_busy = 0;

    uint8_t  g_cfg = 0x45;           // kbd irq on, system flag, translation (satoru)
    int      g_pending = 0;          // 0x64 command waiting for its data byte (satoru)
    bool     g_kbd_off = false;

    void reply(uint8_t b, bool aux = false) {
        if (g_rep_n >= 16) return;
        g_rep[(g_rep_h + g_rep_n) % 16] = { b, aux };
        g_rep_n++;
    }
    bool kq_empty() { return __atomic_load_n(&g_kq_w, __ATOMIC_ACQUIRE) == g_kq_r; }

    // set-1 make codes for ascii; bit 7 of the table entry = needs shift (satoru)
    uint8_t ascii_sc(char c, bool* shift) {
        static const char lo[] = "1234567890-=";
        static const char hi[] = "!@#$%^&*()_+";
        static const uint8_t num[] = { 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D };
        static const char row1[] = "qwertyuiop[]";  static const uint8_t r1[] = { 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B };
        static const char row1s[] = "QWERTYUIOP{}";
        static const char row2[] = "asdfghjkl;'`"; static const uint8_t r2[] = { 0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29 };
        static const char row2s[] = "ASDFGHJKL:\"~";
        static const char row3[] = "\\zxcvbnm,./"; static const uint8_t r3[] = { 0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0x34,0x35 };
        static const char row3s[] = "|ZXCVBNM<>?";
        *shift = false;
        if (c == ' ') return 0x39;
        if (c == '\n') return 0x1C;
        if (c == '\t') return 0x0F;
        for (int i = 0; i < 12; i++) { if (c == lo[i]) return num[i]; if (c == hi[i]) { *shift = true; return num[i]; } }
        for (int i = 0; i < 12; i++) { if (c == row1[i]) return r1[i]; if (c == row1s[i]) { *shift = true; return r1[i]; } }
        for (int i = 0; i < 12; i++) { if (c == row2[i]) return r2[i]; if (c == row2s[i]) { *shift = true; return r2[i]; } }
        for (int i = 0; i < 11; i++) { if (c == row3[i]) return r3[i]; if (c == row3s[i]) { *shift = true; return r3[i]; } }
        return 0;
    }
    void kq_push(uint8_t b) {
        uint32_t w = g_kq_w;
        if (w - __atomic_load_n(&g_kq_r, __ATOMIC_ACQUIRE) >= KQ) return;   // full: drop (satoru)
        g_kq[w % KQ] = b;
        __atomic_store_n(&g_kq_w, w + 1, __ATOMIC_RELEASE);
    }
    struct InjGuard {
        InjGuard() { while (__atomic_exchange_n(&g_inj_busy, 1u, __ATOMIC_ACQUIRE)) __asm__ __volatile__("pause"); }
        ~InjGuard() { __atomic_store_n(&g_inj_busy, 0u, __ATOMIC_RELEASE); }
    };
}

void VI8042::Reset() {
    g_rep_h = g_rep_n = 0;
    g_cfg = 0x45; g_pending = 0; g_kbd_off = false;
    __atomic_store_n(&g_kq_r, __atomic_load_n(&g_kq_w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

bool VI8042::HandlePort(uint16_t port, bool is_out, uint32_t& value) {
    if (port != 0x60 && port != 0x64) return false;
    uint8_t v = (uint8_t)value;
    if (port == 0x64 && !is_out) {
        uint8_t st = 0x14;                                     // system flag + unlocked (satoru)
        if (g_rep_n) { st |= 0x01; if (g_rep[g_rep_h].aux) st |= 0x20; }
        else if (!g_kbd_off && !kq_empty()) st |= 0x01;
        value = st;
        return true;
    }
    if (port == 0x60 && !is_out) {
        if (g_rep_n) { value = g_rep[g_rep_h].b; g_rep_h = (g_rep_h + 1) % 16; g_rep_n--; return true; }
        if (!kq_empty()) {
            uint32_t r = g_kq_r;
            value = g_kq[r % KQ];
            __atomic_store_n(&g_kq_r, r + 1, __ATOMIC_RELEASE);
            return true;
        }
        value = 0;
        return true;
    }
    if (port == 0x64) {   // controller command (satoru)
        switch (v) {
            case 0x20: reply(g_cfg); break;
            case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: g_pending = v; break;
            case 0xAA: reply(0x55); break;                    // controller self test ok (satoru)
            case 0xAB: reply(0x00); break;                    // keyboard interface ok
            case 0xA9: reply(0x01); break;                    // aux interface: clock stuck low = no mouse (satoru)
            case 0xAD: g_kbd_off = true; break;
            case 0xAE: g_kbd_off = false; break;
            case 0xC0: reply(0x00); break;                    // input port
            case 0xD0: reply(0x02); break;                    // output port: a20 on
            default: break;                                   // 0xA7/0xA8 and the rest: accepted (satoru)
        }
        return true;
    }
    // data write (satoru)
    int pend = g_pending; g_pending = 0;
    if (pend == 0x60) { g_cfg = v; return true; }
    if (pend == 0xD2) { reply(v); return true; }              // echo as keyboard byte
    if (pend == 0xD3) { reply(v, true); return true; }        // echo as aux byte
    if (pend == 0xD4) { reply(0xFE, true); return true; }     // aux device: RESEND (absent) (satoru)
    if (pend == 0xD1) return true;
    // a keyboard command: every one is acknowledged (satoru)
    reply(0xFA);
    if (v == 0xFF) reply(0xAA);                               // reset -> bat ok
    else if (v == 0xF2) { reply(0xAB); reply(0x83); }         // identify: mf2 keyboard
    else if (v == 0xEE) { g_rep_n--; reply(0xEE); }           // echo
    return true;
}

void VI8042::Tick() {
    if (g_rep_n || g_kbd_off || kq_empty() || !(g_cfg & 0x01)) return;
    // edge on irq1 for every tick that finds keys waiting; the guest's handler
    // drains 0x60 while bit 0 of 0x64 is set (satoru)
    VirtualDevices::GetMasterPIC().SetIRQ(1, true);
    VirtualDevices::GetMasterPIC().SetIRQ(1, false);
}

void VI8042::InjectScancode(uint8_t sc) { InjGuard g; kq_push(sc); }

int VI8042::InjectText(const char* s) {
    InjGuard g;
    int n = 0;
    for (; s && s[n]; n++) {
        bool sh = false;
        uint8_t sc = ascii_sc(s[n], &sh);
        if (!sc) continue;
        if (sh) kq_push(0x2A);
        kq_push(sc); kq_push((uint8_t)(sc | 0x80));
        if (sh) kq_push(0xAA);
    }
    return n;
}

bool VI8042::InjectKeyName(const char* k) {
    struct K { const char* n; uint8_t sc; bool ext; };
    static const K keys[] = { { "enter", 0x1C, false }, { "esc", 0x01, false }, { "tab", 0x0F, false },
                              { "backspace", 0x0E, false }, { "up", 0x48, true }, { "down", 0x50, true },
                              { "left", 0x4B, true }, { "right", 0x4D, true }, { "space", 0x39, false } };
    for (const K& e : keys) {
        int i = 0;
        while (e.n[i] && k[i] == e.n[i]) i++;
        if (e.n[i] || k[i]) continue;
        InjGuard g;
        if (e.ext) kq_push(0xE0);
        kq_push(e.sc);
        if (e.ext) kq_push(0xE0);
        kq_push((uint8_t)(e.sc | 0x80));
        return true;
    }
    return false;
}

// end (satoru)
