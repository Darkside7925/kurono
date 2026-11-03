#include "hwinfo.h"
#include "serial.h"
#include "usb.h"
#include "blockdev.h"
#include "display_mgr.h"
#include "../kernel/pci.h"
#include "../proc/smp.h"
#include "../proc/scheduler.h"
#include "../net/tcpip.h"
#include "../fs/persist.h"

// see hwinfo.h. everything here only reads hardware state; nothing is
// configured or written. (satoru)

namespace {
    volatile uint64_t g_kbd_ps2 = 0, g_kbd_usb = 0, g_mouse_ev = 0, g_mouse_usb = 0;
    volatile uint32_t g_sc_logged = 0;
    volatile uint64_t g_polls = 0;

    void log_hex(uint64_t v, int digits) {
        static const char* hx = "0123456789ABCDEF";
        char b[20]; int n = 0;
        for (int i = digits - 1; i >= 0; i--) b[n++] = hx[(v >> (i * 4)) & 0xF];
        b[n] = 0;
        SerialLogger::Log(b);
    }
    void log_u(uint64_t v) {
        char b[24]; int n = 0;
        if (v == 0) b[n++] = '0';
        char t[24]; int k = 0;
        while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) b[n++] = t[--k];
        b[n] = 0;
        SerialLogger::Log(b);
    }

    // a DumpInfo-style text block, one log line per '\n'. (satoru)
    void log_block(const char* tag, const char* text) {
        char line[200]; int n = 0;
        for (const char* p = text; ; p++) {
            if (*p == '\n' || *p == 0 || n == (int)sizeof(line) - 1) {
                line[n] = 0;
                if (n) { SerialLogger::Log(tag); SerialLogger::Log(line); SerialLogger::Log("\r\n"); }
                n = 0;
                if (*p == 0) break;
                if (*p != '\n') line[n++] = *p;
                continue;
            }
            if (*p != '\r') line[n++] = *p;
        }
    }

    const char* pci_class_name(uint8_t cls, uint8_t sub, uint8_t pif) {
        switch (cls) {
            case 0x01:
                if (sub == 0x08) return "nvme storage";
                if (sub == 0x06) return "sata/ahci";
                return "storage";
            case 0x02: return sub == 0x80 ? "network (other, e.g. wifi)" : "ethernet";
            case 0x03: return sub == 0x00 ? "vga display" : sub == 0x02 ? "3d display (gpu)" : "display";
            case 0x04: return sub == 0x03 ? "hd audio" : "multimedia";
            case 0x05: return "memory controller";
            case 0x06:
                if (sub == 0x00) return "host bridge";
                if (sub == 0x01) return "isa/lpc bridge";
                if (sub == 0x04) return "pci bridge / root port";
                return "bridge";
            case 0x07: return "serial/comm controller";
            case 0x08: return "system peripheral";
            case 0x09: return "input controller";
            case 0x0C:
                if (sub == 0x03) {
                    if (pif == 0x30) return "usb xhci";
                    if (pif == 0x20) return "usb ehci";
                    if (pif == 0x40) return "usb4/thunderbolt";
                    return "usb";
                }
                if (sub == 0x05) return "smbus";
                if (sub == 0x80) return "serial bus (i2c/spi, touchpad lives here)";
                if (sub == 0x0A) return "thunderbolt";
                return "serial bus";
            case 0x0D: return "wireless";
            case 0x10: return "encryption";
            case 0x11: return "signal processing (thermal/telemetry)";
            case 0x12: return "processing accelerator (npu)";
            case 0x13: return "non-essential instrumentation";
            default:   return "other";
        }
    }

    void dump_cpu() {
        uint32_t r[12];
        for (uint32_t i = 0; i < 3; i++) {
            uint32_t a, b, c, d;
            __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + i), "c"(0));
            r[i * 4 + 0] = a; r[i * 4 + 1] = b; r[i * 4 + 2] = c; r[i * 4 + 3] = d;
        }
        char brand[49];
        for (int i = 0; i < 48; i++) brand[i] = ((const char*)r)[i];
        brand[48] = 0;
        const char* bp = brand; while (*bp == ' ') bp++;
        SerialLogger::Log("[hw] cpu: "); SerialLogger::Log(bp);
        SerialLogger::Log(" | logical "); log_u(SMP::CpuCount());
        SerialLogger::Log(" online "); log_u(SMP::OnlineCount());
        SerialLogger::Log("\r\n");
    }

    void dump_pci() {
        int n = 0;
        for (int bus = 0; bus < 256; bus++) {
            for (int dev = 0; dev < 32; dev++) {
                uint8_t hdr0 = PCI::Read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
                int fns = (hdr0 & 0x80) ? 8 : 1;
                for (int fn = 0; fn < fns; fn++) {
                    uint16_t ven = PCI::GetVendor((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
                    if (ven == 0xFFFF || ven == 0x0000) { if (fn == 0) break; continue; }
                    uint16_t did = PCI::GetDevice((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
                    uint32_t cc  = PCI::Read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                    uint8_t cls = (uint8_t)(cc >> 24), sub = (uint8_t)(cc >> 16), pif = (uint8_t)(cc >> 8);
                    uint8_t hdr = PCI::Read8((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x0E) & 0x7F;
                    uint16_t cmd = PCI::Read16((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x04);
                    SerialLogger::Log("[hw] pci ");
                    log_hex((uint64_t)bus, 2); SerialLogger::Log(":"); log_hex((uint64_t)dev, 2);
                    SerialLogger::Log("."); log_hex((uint64_t)fn, 1);
                    SerialLogger::Log(" "); log_hex(ven, 4); SerialLogger::Log(":"); log_hex(did, 4);
                    SerialLogger::Log(" class "); log_hex(cls, 2); log_hex(sub, 2); log_hex(pif, 2);
                    SerialLogger::Log(" cmd="); log_hex(cmd, 4);
                    SerialLogger::Log(" "); SerialLogger::Log(pci_class_name(cls, sub, pif));
                    SerialLogger::Log("\r\n");
                    if (hdr == 0) {
                        for (int b = 0; b < 6; b++) {
                            uint32_t lo = PCI::Read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, (uint8_t)(0x10 + b * 4));
                            if (lo == 0) continue;
                            bool io = lo & 1;
                            bool is64 = !io && ((lo >> 1) & 3) == 2;
                            uint64_t addr = io ? (lo & ~3u) : (lo & ~0xFu);
                            if (is64 && b < 5) {
                                uint32_t hi = PCI::Read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, (uint8_t)(0x10 + (b + 1) * 4));
                                addr |= (uint64_t)hi << 32;
                            }
                            SerialLogger::Log("[hw]      bar"); log_u((uint64_t)b);
                            SerialLogger::Log(io ? " io  0x" : (is64 ? " m64 0x" : " m32 0x"));
                            log_hex(addr, is64 ? 16 : 8);
                            SerialLogger::Log("\r\n");
                            if (is64) b++;
                        }
                    }
                    n++;
                }
            }
        }
        SerialLogger::Log("[hw] pci functions: "); log_u((uint64_t)n); SerialLogger::Log("\r\n");
    }

    void dump_ps2() {
        uint8_t st;
        __asm__ __volatile__("inb $0x64, %0" : "=a"(st));
        SerialLogger::Log("[hw] i8042 (ps/2) status=0x"); log_hex(st, 2);
        SerialLogger::Log(st == 0xFF ? " -> no legacy ps/2 controller\r\n"
                                     : " -> ps/2 controller present\r\n");
    }
}

void HwInfo::NoteScancode(uint8_t sc) {
    __atomic_add_fetch(&g_kbd_ps2, 1, __ATOMIC_RELAXED);
    // the first few raw scancodes, so the log proves which keys arrived. (satoru)
    if (__atomic_add_fetch(&g_sc_logged, 1, __ATOMIC_RELAXED) <= 24) {
        SerialLogger::Log("[input] ps2 scancode 0x"); log_hex(sc, 2); SerialLogger::Log("\r\n");
    }
}
void HwInfo::NoteUsbKeyboard() {
    if (__atomic_add_fetch(&g_kbd_usb, 1, __ATOMIC_RELAXED) <= 8)
        SerialLogger::Log("[input] usb keyboard report\r\n");
}
void HwInfo::NotePoll() { __atomic_add_fetch(&g_polls, 1, __ATOMIC_RELAXED); }
void HwInfo::NoteMouseEvent() { __atomic_add_fetch(&g_mouse_ev, 1, __ATOMIC_RELAXED); }
void HwInfo::NoteUsbMouse() {
    if (__atomic_add_fetch(&g_mouse_usb, 1, __ATOMIC_RELAXED) <= 8)
        SerialLogger::Log("[input] usb mouse report\r\n");
}

void HwInfo::DumpAll() {
    SerialLogger::Log("[hw] ================ hardware inventory ================\r\n");
    dump_cpu();
    dump_pci();
    dump_ps2();

    static char buf[8192];
    buf[0] = 0; USB::DumpInfo(buf, sizeof(buf));            log_block("[hw] usb: ", buf);
    USB::LogAllPorts();                                      // [usbports] every controller + port (satoru)
    SerialLogger::Log(USB::IsHIDInputEnabled() ? "[hw] usb hid input forwarding: ON\r\n"
                                               : "[hw] usb hid input forwarding: OFF (usb kbd/mouse ignored)\r\n");
    buf[0] = 0; DisplayManager::DumpInfo(buf, sizeof(buf)); log_block("[hw] display: ", buf);
    for (int i = 0; i < BlockDev::Count(); i++) {
        buf[0] = 0; BlockDev::Describe(i, buf, 512);        log_block("[hw] disk: ", buf);
    }
    buf[0] = 0; PersistStore::Describe(buf, 256);           log_block("[hw] persist: ", buf);
    buf[0] = 0; TCPStack::DumpInfo(buf, sizeof(buf));       log_block("[hw] net: ", buf);
    SerialLogger::Log("[hw] ================ end of inventory ================\r\n");
}

extern volatile uint32_t g_kbdiag_init[4];   // keyboard.cpp (satoru)

void HwInfo::KbDiag(int sample) {
    uint8_t st;
    __asm__ __volatile__("inb $0x64, %0" : "=a"(st));
    SerialLogger::Log("[kbdiag] sample "); log_u((uint64_t)sample);
    SerialLogger::Log(" up "); log_u(Scheduler::NowMs() / 1000);
    SerialLogger::Log("s | i8042 status 0x"); log_hex(st, 2);
    SerialLogger::Log(st == 0xFF ? " (no controller)" : " (controller present)");
    SerialLogger::Log(" | init: command byte ");
    if (g_kbdiag_init[0]) { SerialLogger::Log("0x"); log_hex(g_kbdiag_init[1], 2); } else SerialLogger::Log("unreadable");
    SerialLogger::Log(" kbd enable-reply ");
    if (g_kbdiag_init[2]) { SerialLogger::Log("0x"); log_hex(g_kbdiag_init[3], 2); SerialLogger::Log(g_kbdiag_init[3] == 0xFA ? " (ack: ps/2 keyboard answered)" : " (not an ack)"); }
    else SerialLogger::Log("none (no ps/2 keyboard answered)");
    SerialLogger::Log(" | ps2 scancodes "); log_u(g_kbd_ps2);
    SerialLogger::Log(" usb kbd reports "); log_u(g_kbd_usb);
    SerialLogger::Log(" usb hid input ");
    SerialLogger::Log(USB::IsHIDInputEnabled() ? "ON" : "OFF");
    SerialLogger::Log("\r\n");
    USB::LogKbDiag();
}

extern volatile uint32_t g_kbdbg[8];
extern volatile uint32_t g_imdbg[4];
extern volatile uint32_t g_termdbg[4];

void HwInfo::Heartbeat() {
    // key path: scancodes -> chars -> terminal; any drop shows here (satoru)
    SerialLogger::Log("[kbpath] ps2 "); log_u(g_kbd_ps2);
    SerialLogger::Log(" usb "); log_u(g_kbd_usb);
    SerialLogger::Log(" enq "); log_u(g_kbdbg[3]);
    SerialLogger::Log(" enqdrop "); log_u(g_kbdbg[0]);
    SerialLogger::Log(" irq1 "); log_u(g_kbdbg[5]);
    SerialLogger::Log(" imq "); log_u(g_imdbg[1]);
    SerialLogger::Log(" imdrop "); log_u(g_imdbg[0]);
    SerialLogger::Log(" maxgap "); log_u(g_kbdbg[1]);
    SerialLogger::Log("ms maxdrain "); log_u(g_kbdbg[2]);
    SerialLogger::Log(" term in "); log_u(g_termdbg[0]);
    SerialLogger::Log(" typed "); log_u(g_termdbg[1]);
    SerialLogger::Log(" ahead "); log_u(g_termdbg[2]);
    SerialLogger::Log(" enterlost "); log_u(g_termdbg[3]);
    SerialLogger::Log("\r\n");
    g_kbdbg[1] = 0;
    SerialLogger::Log("[hb] up ");
    log_u(Scheduler::NowMs() / 1000);
    SerialLogger::Log("s | ps2 keys "); log_u(g_kbd_ps2);
    SerialLogger::Log(" usb kbd "); log_u(g_kbd_usb);
    SerialLogger::Log(" | mouse events "); log_u(g_mouse_ev);
    SerialLogger::Log(" usb mouse "); log_u(g_mouse_usb);
    // input polls: if this stops growing the desktop loop is stuck (frozen),
    // if it grows while the counters stay 0 the devices are not delivering.
    // i8042 status: bit 0 set = a byte is waiting that nobody has read. (satoru)
    SerialLogger::Log(" | input polls "); log_u(g_polls);
    uint8_t st;
    __asm__ __volatile__("inb $0x64, %0" : "=a"(st));
    SerialLogger::Log(" | i8042 status 0x"); log_hex(st, 2);
    SerialLogger::Log("\r\n");
}

// end (satoru)
