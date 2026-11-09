#include "ktools.h"
#include "kinit.h"
#include "../shell/shell.h"
#include "../kernel/kdf.h"
#include "../kernel/pmm.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/e1000.h"
#include "../drivers/igc.h"
#include "../drivers/hda.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/usb.h"
#include "../drivers/usb_audio.h"
#include "../drivers/nvme.h"
#include "../drivers/ahci.h"
#include "../drivers/bga.h"
#include "../drivers/virtio_gpu.h"
#include "../drivers/nvidia_gpu.h"
#include "../net/tcpip.h"
#include "../net/dhcp.h"
#include "../proc/smp.h"

// kdrv: list / inspect / reload / unload / start the drivers kurono has.
// ksys: read-only diagnostics that reuse the existing commands and counters.
//
// what can really be stopped and started (checked against the driver code, not
// guessed): the nic (e1000 re-inits from the saved mmio window, igc via kdf),
// audio (hda and usb audio have Restart()), and usb hid input (a real enable
// flag). everything else is either protected on purpose (storage holding
// KURONO.LOG / KURONO.KFS, the display path, timer and interrupt core, the usb
// host controller, ps/2 input) or has no clean stop in this build. there is no
// loader for new binaries: "start" only brings up a built-in driver that is
// present but inactive. nothing here writes to a disk, nic nvm or firmware. (satoru)

namespace KTools {

namespace {

KuronoShell*  g_sh = nullptr;
volatile int  g_agent = 0;    // >0 while a /dev/kshell command is running (satoru)
bool          g_nic_stopped = false;
bool          g_hid_stopped = false;

// ---- tiny output helpers (no libc in the kernel) --------------------------- (satoru)
int ap(char* out, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) out[p++] = *s++;
    out[p] = 0;
    return p;
}
int apu(char* out, int p, int mx, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n && p < mx - 1) out[p++] = t[--n];
    out[p] = 0;
    return p;
}
int pad(char* out, int p, int mx, const char* s, int w) {
    int start = p;
    p = ap(out, p, mx, s);
    do { if (p < mx - 1) out[p++] = ' '; } while (p - start < w && p < mx - 1);
    out[p] = 0;
    return p;
}
bool eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
int slen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

// count occurrences of needle in the captured boot log (satoru)
int count_in_log(const char* needle) {
    const char* d = SerialLogger::CaptureData();
    uint32_t len = SerialLogger::CaptureLen();
    int nl = slen(needle), c = 0;
    if (nl == 0) return 0;
    for (uint32_t i = 0; i + (uint32_t)nl <= len; i++) {
        if (d[i] != needle[0]) continue;
        int k = 1;
        while (k < nl && d[i + k] == needle[k]) k++;
        if (k == nl) { c++; i += (uint32_t)nl - 1; }
    }
    return c;
}

// run an existing shell command's handler straight into a buffer (reuse, not
// reimplementation). returns bytes written. (satoru)
int run_cmd(const char* name, const char* a1, char* out, int mx) {
    ShellCommand* c = KuronoShell::FindCommand(name);
    if (!c || !c->handler || mx < 2) return 0;
    const char* av[3] = { name, a1, nullptr };
    out[0] = 0;
    c->handler(g_sh, a1 ? 2 : 1, av, out, mx);
    out[mx - 1] = 0;
    return slen(out);
}

// ---- driver table ---------------------------------------------------------- (satoru)
struct Drv {
    const char* name;
    const char* cat;
    const char* desc;
    const char* (*state)();                  // running / stopped / absent / active
    int         (*detail)(char* out, int p, int mx);
    bool        (*do_reload)();              // null = no clean reload
    bool        (*do_unload)();              // null = no clean unload
    bool        (*do_start)();               // null = no start
    bool        agent_unload_ok;             // false: console only (satoru)
    const char* protect;                     // non-null: refused for every change (satoru)
};

// ---- nic ---- (satoru)
const char* nic_state() {
    if (g_nic_stopped) return "stopped";
    return E1000::IsDetected() ? "running" : "absent";
}
int nic_detail(char* out, int p, int mx) {
    p = ap(out, p, mx, Igc::IsDetected() ? "igc/igb" : "e1000");
    if (!E1000::IsDetected()) return p;
    p = ap(out, p, mx, E1000::IsLinkUp() ? " link up" : " link down");
    uint8_t m[6]; E1000::GetMAC(m);
    const char* h = "0123456789abcdef";
    p = ap(out, p, mx, " mac ");
    for (int i = 0; i < 6; i++) {
        char c[3] = { h[m[i] >> 4], h[m[i] & 15], 0 };
        p = ap(out, p, mx, c);
        if (i < 5) p = ap(out, p, mx, ":");
    }
    return p;
}
bool nic_reload() { bool ok = E1000::Restart(); if (ok) g_nic_stopped = false; return ok; }
bool nic_unload() {
    if (Igc::IsDetected()) return false;     // igc/igb has no stop, only restart (satoru)
    if (!E1000::Stop()) return false;
    g_nic_stopped = true;
    return true;
}
bool nic_start() { return nic_reload(); }

// ---- audio ---- (satoru)
bool usb_audio_active() {
    return AudioServer::ActiveBackend() != nullptr &&
           AudioServer::ActiveBackend() == AudioServer::FindBackend("usbaudio");
}
const char* audio_state() {
    if (usb_audio_active() || HDAudio::IsDetected() || Audio::IsAvailable()) return "running";
    return "absent";
}
int audio_detail(char* out, int p, int mx) {
    p = ap(out, p, mx, "backend ");
    return ap(out, p, mx, AudioServer::ActiveBackendName());
}
bool audio_reload() {
    if (usb_audio_active()) return UsbAudio::Restart();
    if (HDAudio::IsDetected()) return HDAudio::Restart();
    return false;                            // sb16 has no restart (satoru)
}

// ---- usb hid input ---- (satoru)
const char* hid_state() {
    if (!USB::IsDetected()) return "absent";
    return USB::IsHIDInputEnabled() ? "running" : "stopped";
}
int hid_detail(char* out, int p, int mx) {
    p = apu(out, p, mx, (uint64_t)USB::GetDeviceCount());
    return ap(out, p, mx, " usb device(s) attached");
}
bool hid_reload() {
    USB::SetHIDInputEnabled(false);
    for (volatile int i = 0; i < 2000000; i++) {}
    USB::SetHIDInputEnabled(true);
    g_hid_stopped = false;
    return true;
}
bool hid_unload() { USB::SetHIDInputEnabled(false); g_hid_stopped = true; return true; }
bool hid_start()  { USB::SetHIDInputEnabled(true);  g_hid_stopped = false; return true; }

// ---- read-only / protected rows ---- (satoru)
const char* st_active() { return "active"; }
const char* tcpip_state() { return TCPStack::IsUp() ? "running" : "stopped"; }
int tcpip_detail(char* out, int p, int mx) {
    if (!TCPStack::IsUp()) return p;
    char ip[16]; TCPStack::FormatIP(TCPStack::GetIP(), ip);
    p = ap(out, p, mx, "ip "); p = ap(out, p, mx, ip);
    p = ap(out, p, mx, Dhcp::IsBound() ? " (dhcp bound)" : " (static or no lease)");
    return p;
}
const char* storage_state() {
    return (NVMe::IsDetected() || Ahci::IsDetected()) ? "running" : "absent";
}
int storage_detail(char* out, int p, int mx) {
    if (NVMe::IsDetected()) { p = ap(out, p, mx, "nvme "); p = ap(out, p, mx, NVMe::GetInfo().model); }
    if (Ahci::IsDetected()) { p = ap(out, p, mx, p ? ", sata/ahci" : "sata/ahci"); }
    return p;
}
const char* display_state() { return "running"; }
int display_detail(char* out, int p, int mx) {
    bool any = false;
    if (BGA::IsAvailable())       { p = ap(out, p, mx, "bga"); any = true; }
    if (VirtIOGPU::IsDetected())  { p = ap(out, p, mx, any ? ", virtio-gpu" : "virtio-gpu"); any = true; }
    if (NvidiaGPU::IsDetected())  { p = ap(out, p, mx, any ? ", nvidia" : "nvidia"); any = true; }
    if (!any) p = ap(out, p, mx, "boot framebuffer (efi gop / vbe)");
    return p;
}
const char* usbhost_state() { return USB::IsDetected() ? "running" : "absent"; }
int usbhost_detail(char* out, int p, int mx) {
    p = apu(out, p, mx, (uint64_t)USB::ControllerCount());
    return ap(out, p, mx, " xhci controller(s)");
}

const Drv kDrivers[] = {
    { "nic",       "net",     "ethernet nic (e1000, or igc/igb on real hardware)",
      nic_state, nic_detail, nic_reload, nic_unload, nic_start, false, nullptr },
    { "audio",     "audio",   "audio output (intel hda, usb audio, sb16)",
      audio_state, audio_detail, audio_reload, nullptr, audio_reload, true, nullptr },
    { "usb-hid",   "input",   "usb keyboard and mouse input",
      hid_state, hid_detail, hid_reload, hid_unload, hid_start, false, nullptr },
    { "tcpip",     "net",     "tcp/ip stack, arp, dhcp (a stack, not a device driver)",
      tcpip_state, tcpip_detail, nullptr, nullptr, nullptr, true, nullptr },
    { "usb-host",  "usb",     "xhci usb host controller(s)",
      usbhost_state, usbhost_detail, nullptr, nullptr, nullptr, true,
      "the usb host carries keyboards, mice and possibly the boot disk; a reset can lose them" },
    { "storage",   "storage", "nvme / sata boot and log disk (KURONO.LOG, KURONO.KFS)",
      storage_state, storage_detail, nullptr, nullptr, nullptr, true,
      "holds KURONO.LOG and KURONO.KFS and the running filesystem; stopping it loses data" },
    { "display",   "display", "display path (bga, virtio-gpu, nvidia, boot framebuffer)",
      display_state, display_detail, nullptr, nullptr, nullptr, true,
      "the desktop is drawing through it right now" },
    { "timer",     "core",    "pit / lapic timer",
      st_active, nullptr, nullptr, nullptr, nullptr, true,
      "the scheduler and every sleep depend on it" },
    { "interrupts","core",    "idt, local apic, io-apic",
      st_active, nullptr, nullptr, nullptr, nullptr, true,
      "interrupt core; there is no safe stop" },
    { "ps2-input", "input",   "ps/2 keyboard and mouse",
      st_active, nullptr, nullptr, nullptr, nullptr, true,
      "no clean stop/start in this build; it may be the only keyboard" },
};
const int kDriverCount = (int)(sizeof(kDrivers) / sizeof(kDrivers[0]));

const Drv* find_drv(const char* name) {
    for (int i = 0; i < kDriverCount; i++) if (eq(kDrivers[i].name, name)) return &kDrivers[i];
    return nullptr;
}

const char* row_ops(const Drv& d) {
    if (d.protect) return "protected";
    if (d.do_reload && d.do_unload) return "reload unload start";
    if (d.do_reload) return "reload start";
    return "read-only";
}

int kdrv_list(char* out, int mx, bool all) {
    int p = 0;
    p = ap(out, p, mx, "NAME        CATEGORY  STATE    OPS                  BOUND DEVICE\n");
    for (int i = 0; i < kDriverCount; i++) {
        const Drv& d = kDrivers[i];
        p = pad(out, p, mx, d.name, 12);
        p = pad(out, p, mx, d.cat, 10);
        p = pad(out, p, mx, d.state(), 9);
        p = pad(out, p, mx, row_ops(d), 21);
        if (d.detail) p = d.detail(out, p, mx);
        p = ap(out, p, mx, "\n");
    }
    // kinit-managed services are listed read-only (satoru)
    int sc = KInit::GetServiceCount();
    KInit::KService* sv = KInit::GetServices();
    if (all && sv && sc > 0) {
        p = ap(out, p, mx, "\nkinit services (change them from the console with `kinit`; not touched here):\n");
        for (int i = 0; i < sc; i++) {
            if (!sv[i].name[0]) continue;
            p = ap(out, p, mx, "  ");
            p = pad(out, p, mx, sv[i].name, 14);
            p = pad(out, p, mx, KInit::StateName(sv[i].state), 10);
            p = ap(out, p, mx, sv[i].description);
            p = ap(out, p, mx, "\n");
        }
    }
    if (!all) return ap(out, p, mx, "\n(`kdrv list all` adds kinit services and the kdf crash sandbox)\n");
    p = ap(out, p, mx, "\nkdf sandbox (drivers running under crash isolation):\n");
    char kb[1200];
    KDF::Status(kb, (int)sizeof(kb));
    p = ap(out, p, mx, kb);
    return p;
}

int kdrv_info(const char* name, char* out, int mx) {
    const Drv* d = find_drv(name);
    int p = 0;
    if (!d) {
        // a kinit service name? (satoru)
        int sc = KInit::GetServiceCount();
        KInit::KService* sv = KInit::GetServices();
        for (int i = 0; sv && i < sc; i++) {
            if (!eq(sv[i].name, name)) continue;
            p = ap(out, p, mx, "name     : "); p = ap(out, p, mx, sv[i].name);
            p = ap(out, p, mx, "\ncategory : kinit service\nstate    : ");
            p = ap(out, p, mx, KInit::StateName(sv[i].state));
            p = ap(out, p, mx, "\ndesc     : "); p = ap(out, p, mx, sv[i].description);
            p = ap(out, p, mx, "\nops      : read-only here; use `kinit` at the console\n");
            return p;
        }
        p = ap(out, p, mx, "kdrv: unknown driver '"); p = ap(out, p, mx, name);
        return ap(out, p, mx, "'. run `kdrv list`\n");
    }
    p = ap(out, p, mx, "name     : "); p = ap(out, p, mx, d->name);
    p = ap(out, p, mx, "\ncategory : "); p = ap(out, p, mx, d->cat);
    p = ap(out, p, mx, "\nstate    : "); p = ap(out, p, mx, d->state());
    p = ap(out, p, mx, "\ndesc     : "); p = ap(out, p, mx, d->desc);
    p = ap(out, p, mx, "\ndevice   : ");
    if (d->detail) p = d->detail(out, p, mx);
    p = ap(out, p, mx, "\nops      : "); p = ap(out, p, mx, row_ops(*d));
    if (d->protect) { p = ap(out, p, mx, "\nprotected: "); p = ap(out, p, mx, d->protect); }
    if (d->do_unload && !d->agent_unload_ok)
        p = ap(out, p, mx, "\nnote     : unload is console-only; from an agent use reload (unloading would cut your own link)");
    // the kdf sandbox view, when this driver is registered there under the same
    // or the obvious name (satoru)
    const char* kn = eq(d->name, "nic") ? "igc" : eq(d->name, "storage") ? "nvme" : d->name;
    int kid = KDF::FindDriver(kn);
    if (kid >= 0) {
        KDF::KDFCrashInfo ci;
        p = ap(out, p, mx, "\nkdf      : "); p = ap(out, p, mx, kn);
        p = ap(out, p, mx, " "); p = ap(out, p, mx, KDF::StateName(KDF::GetState(kid)));
        if (KDF::GetCrashInfo(kid, &ci) && ci.valid) {
            p = ap(out, p, mx, ", crashes "); p = apu(out, p, mx, ci.crash_count);
        }
    }
    return ap(out, p, mx, "\n");
}

int kdrv_change(const char* verb, const char* name, char* out, int mx) {
    int p = 0;
    const Drv* d = find_drv(name);
    if (!d) {
        p = ap(out, p, mx, "kdrv: unknown driver '"); p = ap(out, p, mx, name);
        return ap(out, p, mx, "'. run `kdrv list`. kinit services are not changed from here\n");
    }
    bool is_reload = eq(verb, "reload"), is_unload = eq(verb, "unload"), is_start = eq(verb, "start");
    if (d->protect) {
        p = ap(out, p, mx, "kdrv: REFUSED. '"); p = ap(out, p, mx, d->name);
        p = ap(out, p, mx, "' is protected: "); p = ap(out, p, mx, d->protect);
        p = ap(out, p, mx, ".\n  no kdrv action touches it. see `kdrv info "); p = ap(out, p, mx, d->name);
        return ap(out, p, mx, "`\n");
    }
    bool(*fn)() = is_reload ? d->do_reload : is_unload ? d->do_unload : d->do_start;
    if (!fn) {
        p = ap(out, p, mx, "kdrv: '"); p = ap(out, p, mx, d->name);
        p = ap(out, p, mx, "' does not support "); p = ap(out, p, mx, verb);
        p = ap(out, p, mx, " (no clean stop/start in the driver code). ops: ");
        p = ap(out, p, mx, row_ops(*d));
        return ap(out, p, mx, "\n");
    }
    if (is_unload && g_agent > 0 && !d->agent_unload_ok) {
        p = ap(out, p, mx, "kdrv: REFUSED for the agent. unloading '"); p = ap(out, p, mx, d->name);
        p = ap(out, p, mx, "' would cut the link you are running over (network or input). use `kdrv reload ");
        p = ap(out, p, mx, d->name);
        return ap(out, p, mx, "`, or ask the operator to unload it at the console\n");
    }
    SerialLogger::Log("[ktools] kdrv "); SerialLogger::Log(verb); SerialLogger::Log(" ");
    SerialLogger::Log(d->name); SerialLogger::Log(g_agent > 0 ? " (agent)\r\n" : " (console)\r\n");
    if (is_start && eq(d->state(), "running")) {
        p = ap(out, p, mx, "kdrv: "); p = ap(out, p, mx, d->name);
        return ap(out, p, mx, " is already running; use reload to restart it\n");
    }
    bool ok = fn();
    p = ap(out, p, mx, "kdrv: "); p = ap(out, p, mx, d->name); p = ap(out, p, mx, " ");
    p = ap(out, p, mx, verb); p = ap(out, p, mx, ok ? ": ok\n" : ": FAILED (see `ksys log 30`)\n");
    p = ap(out, p, mx, "  state now: "); p = ap(out, p, mx, d->state());
    if (d->detail) { p = ap(out, p, mx, "  ("); p = d->detail(out, p, mx); p = ap(out, p, mx, ")"); }
    return ap(out, p, mx, "\n");
}

int cmd_kdrv(KuronoShell*, int argc, const char** argv, char* out, int mx) {
    const char* sub = argc >= 2 ? argv[1] : "list";
    if (eq(sub, "list") || eq(sub, "ls")) return kdrv_list(out, mx, argc >= 3 && eq(argv[2], "all"));
    if (argc >= 3 && eq(sub, "info")) return kdrv_info(argv[2], out, mx);
    if (argc >= 3 && (eq(sub, "reload") || eq(sub, "unload") || eq(sub, "start")))
        return kdrv_change(sub, argv[2], out, mx);
    return ap(out, 0, mx,
        "usage: kdrv list [all] | info <name> | reload <name> | unload <name> | start <name>\n"
        "  reload restarts a driver, unload stops it, start brings up one that is stopped.\n"
        "  protected drivers (storage, display, timer, interrupts, usb-host, ps2-input) are refused.\n");
}

// ---- ksys ------------------------------------------------------------------ (satoru)
int sys_log(int lines, char* out, int mx) {
    int p = 0;
    uint32_t len = SerialLogger::CaptureLen();
    const char* d = SerialLogger::CaptureData();
    if (len + 2 >= SerialLogger::CaptureCap()) {
        // the linear capture is full and holds only the early boot; the mirrored
        // serial log has the recent lines, so reuse dmesg (satoru)
        p = ap(out, p, mx, "(boot capture is full; showing the mirrored serial log)\n");
        return p + run_cmd("dmesg", nullptr, out + p, mx - p);
    }
    uint32_t start = len;
    int nl = 0;
    while (start > 0) {
        if (d[start - 1] == '\n' && ++nl > lines) break;
        start--;
    }
    uint32_t room = (uint32_t)(mx - 120);
    if (len - start > room) {
        start = len - room;
        while (start < len && d[start] != '\n') start++;   // drop the cut first line (satoru)
        if (start < len) start++;
    }
    p = ap(out, p, mx, "== kernel log, last "); p = apu(out, p, mx, (uint64_t)lines);
    p = ap(out, p, mx, " lines of "); p = apu(out, p, mx, len); p = ap(out, p, mx, " bytes ==\n");
    for (uint32_t i = start; i < len && p < mx - 1; i++)
        if (d[i] != '\r') out[p++] = d[i];
    out[p] = 0;
    return p;
}

int sys_mem(char* out, int mx) {
    int p = 0;
    uint64_t tot = PMM::GetTotalMemory() >> 20, fr = PMM::GetFreeMemory() >> 20;
    p = ap(out, p, mx, "physical memory (pmm):\n  usable : "); p = apu(out, p, mx, tot);
    p = ap(out, p, mx, " MB\n  free   : "); p = apu(out, p, mx, fr);
    p = ap(out, p, mx, " MB\n  used   : "); p = apu(out, p, mx, tot > fr ? tot - fr : 0);
    p = ap(out, p, mx, " MB\n  ram top: "); p = apu(out, p, mx, PMM::GetPhysTop() >> 20);
    p = ap(out, p, mx, " MB\nkernel heap used: "); p = apu(out, p, mx, (uint64_t)KernelHeap::GetUsed() >> 10);
    return ap(out, p, mx, " KB\n");
}

int sys_net(char* out, int mx) {
    int p = 0;
    p = ap(out, p, mx, "== nic ==\n  ");
    p = nic_detail(out, p, mx);
    p = ap(out, p, mx, "\n  tx "); p = apu(out, p, mx, E1000::GetTxCount());
    p = ap(out, p, mx, " pkts, rx "); p = apu(out, p, mx, E1000::GetRxCount());
    p = ap(out, p, mx, " pkts, rx-missed "); p = apu(out, p, mx, E1000::GetRxMissed());
    p = ap(out, p, mx, "\n== tcp/ip ==\n  ");
    p = tcpip_detail(out, p, mx);
    char b[16];
    p = ap(out, p, mx, "\n  gateway "); TCPStack::FormatIP(TCPStack::GetGateway(), b); p = ap(out, p, mx, b);
    p = ap(out, p, mx, ", mask "); TCPStack::FormatIP(TCPStack::GetSubnetMask(), b); p = ap(out, p, mx, b);
    const NetStats& ns = TCPStack::GetStats();
    p = ap(out, p, mx, "\n  packets rx "); p = apu(out, p, mx, ns.packets_rx);
    p = ap(out, p, mx, " tx "); p = apu(out, p, mx, ns.packets_tx);
    p = ap(out, p, mx, ", errors rx "); p = apu(out, p, mx, ns.errors_rx);
    p = ap(out, p, mx, " tx "); p = apu(out, p, mx, ns.errors_tx);
    p = ap(out, p, mx, "\n== interfaces ==\n");
    return p + run_cmd("ifconfig", nullptr, out + p, mx - p);
}

int sys_health(char* out, int mx) {
    int p = 0;
    uint64_t s = Timer::GetRealMs64() / 1000;
    p = ap(out, p, mx, "== kurono health ==\nuptime   : ");
    p = apu(out, p, mx, s / 3600); p = ap(out, p, mx, "h ");
    p = apu(out, p, mx, (s / 60) % 60); p = ap(out, p, mx, "m ");
    p = apu(out, p, mx, s % 60); p = ap(out, p, mx, "s\ncpus     : ");
    p = apu(out, p, mx, SMP::OnlineCount()); p = ap(out, p, mx, " online of ");
    p = apu(out, p, mx, SMP::CpuCount());
    p = ap(out, p, mx, "\nmemory   : "); p = apu(out, p, mx, PMM::GetFreeMemory() >> 20);
    p = ap(out, p, mx, " MB free of "); p = apu(out, p, mx, PMM::GetTotalMemory() >> 20);
    p = ap(out, p, mx, " MB\nnic      : "); p = ap(out, p, mx, nic_state());
    p = ap(out, p, mx, ", "); p = nic_detail(out, p, mx);
    p = ap(out, p, mx, "\ndhcp/ip  : ");
    if (TCPStack::IsUp()) { p = tcpip_detail(out, p, mx); } else p = ap(out, p, mx, "tcp/ip down");
    p = ap(out, p, mx, "\naudio    : "); p = ap(out, p, mx, audio_state());
    p = ap(out, p, mx, ", "); p = audio_detail(out, p, mx);
    p = ap(out, p, mx, "\nusb      : "); p = ap(out, p, mx, usbhost_state());
    p = ap(out, p, mx, ", "); p = hid_detail(out, p, mx);
    p = ap(out, p, mx, ", hid input "); p = ap(out, p, mx, hid_state());
    p = ap(out, p, mx, "\nstorage  : "); p = storage_detail(out, p, mx);
    int usrflt = count_in_log("[usrflt] ring-3 fault");
    int stk    = count_in_log("[STKCORRUPT]");
    int rawexc = count_in_log("[RAWEXC]");
    int kdfcr  = count_in_log("kdf crash");
    int panics = count_in_log("PANIC");
    p = ap(out, p, mx, "\nfaults   : usrflt "); p = apu(out, p, mx, (uint64_t)usrflt);
    p = ap(out, p, mx, ", STKCORRUPT "); p = apu(out, p, mx, (uint64_t)stk);
    p = ap(out, p, mx, ", RAWEXC "); p = apu(out, p, mx, (uint64_t)rawexc);
    p = ap(out, p, mx, ", kdf crash "); p = apu(out, p, mx, (uint64_t)kdfcr);
    p = ap(out, p, mx, ", PANIC "); p = apu(out, p, mx, (uint64_t)panics);
    p = ap(out, p, mx, "  (counted in the boot log)\nverdict  : ");
    bool bad = false;
    if (!E1000::IsDetected())  { p = ap(out, p, mx, "no nic; "); bad = true; }
    else if (!E1000::IsLinkUp()) { p = ap(out, p, mx, "nic link down (try `kdrv reload nic`); "); bad = true; }
    else if (!TCPStack::IsUp()) { p = ap(out, p, mx, "tcp/ip down; "); bad = true; }
    if (usrflt || stk) { p = ap(out, p, mx, "ring-3 faults seen (see `ksys log 60`); "); bad = true; }
    if (!bad) p = ap(out, p, mx, "ok");
    return ap(out, p, mx, "\n");
}

int cmd_ksys(KuronoShell*, int argc, const char** argv, char* out, int mx) {
    const char* sub = argc >= 2 ? argv[1] : "health";
    if (eq(sub, "health")) return sys_health(out, mx);
    if (eq(sub, "log")) {
        int n = 40;
        if (argc >= 3) {
            n = 0;
            for (const char* c = argv[2]; *c >= '0' && *c <= '9'; c++) n = n * 10 + (*c - '0');
            if (n < 1) n = 40;
            if (n > 400) n = 400;
        }
        return sys_log(n, out, mx);
    }
    if (eq(sub, "hw")) {
        int p = run_cmd("hwinfo", nullptr, out, mx);
        p = ap(out, p, mx, "\n== pci ==\n");
        p += run_cmd("lspci", nullptr, out + p, mx - p);
        p = ap(out, p, mx, "\n== usb ==\n");
        p += run_cmd("lsusb", nullptr, out + p, mx - p);
        return p;
    }
    if (eq(sub, "procs")) return run_cmd("ps", nullptr, out, mx);
    if (eq(sub, "mem")) return sys_mem(out, mx);
    if (eq(sub, "net")) return sys_net(out, mx);
    return ap(out, 0, mx, "usage: ksys health | log [N] | hw | procs | mem | net\n");
}

}  // namespace

void SetAgentCaller(bool on) {
    if (on) __atomic_add_fetch(&g_agent, 1, __ATOMIC_SEQ_CST);
    else if (g_agent > 0) __atomic_sub_fetch(&g_agent, 1, __ATOMIC_SEQ_CST);
}

void RegisterShellCommands(void* shell) {
    g_sh = (KuronoShell*)shell;
    KuronoShell::RegisterCommand("kdrv", "kdrv list|info|reload|unload|start <driver>", ENV_AUTO, "system", cmd_kdrv);
    KuronoShell::RegisterCommand("ksys", "ksys health|log|hw|procs|mem|net", ENV_AUTO, "system", cmd_ksys);
}

}  // namespace KTools

// end (satoru)
