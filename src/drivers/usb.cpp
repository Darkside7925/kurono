//  kurono os - usb (xhci) host controller driver implementation
//  pci class 0c:03:30 (xhci usb 3.0)
#include "usb.h"
#include "../kernel/kimage.h"   // control buffers may live in the high-linked image (satoru)
#include "../kernel/pci.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"          // isoch rings are whole pmm pages (satoru)
#include "../kernel/io.h"
#include "../drivers/serial.h"
#include "../drivers/nvme.h"       // shared bare-metal write guard switch (satoru)
#include "../drivers/keyboard.h"   // ProcessUSBReport for keyboard hid (satoru)
#include "../drivers/mouse.h"      // ProcessUSBReport for mouse hid (satoru)
#include "../kernel/vmm.h"         // map a high 64-bit xhci bar before touching it (satoru)
#include "../proc/scheduler.h"     // NowMs for the bios handoff wait (satoru)
#include "blockdev.h"              // usb mass storage shows up as sda (satoru)
#include <stdarg.h>

// per-controller state (see XhciState in usb.h). S is the controller the
// functions below act on; the old static members are macros onto it, so every
// existing function body works unchanged on whichever controller S names. (satoru)
XhciState USB::g_xs[USB_MAX_CTL];
XhciState* USB::S = &USB::g_xs[0];
int USB::g_nctl = 0;

#define detected       (S->x_detected)
#define bar0           (S->x_bar0)
#define cap_length     (S->x_cap_length)
#define max_ports      (S->x_max_ports)
#define max_slots      (S->x_max_slots)
#define devices        (S->x_devices)
#define runtime        (S->x_runtime)
#define device_count   (S->x_device_count)
#define ctx_stride     (S->x_ctx_stride)
#define erst_erdp_seg  (S->x_erst_erdp_seg)
#define cmd_ring       (S->x_cmd_ring)
#define event_ring     (S->x_event_ring)
#define dcbaa          (S->x_dcbaa)
#define cmd_ring_idx   (S->x_cmd_ring_idx)
#define cmd_cycle      (S->x_cmd_cycle)
#define event_ring_idx (S->x_event_ring_idx)
#define event_ccs      (S->x_event_ccs)

// callbacks registered by the usb audio driver (satoru)
static USB::IsoEventFn    g_iso_event_fn = nullptr;
static USB::AudioAttachFn g_audio_attach_fn = nullptr;
static USB::AudioServiceFn g_audio_service_fn = nullptr;
void USB::SetAudioHooks(IsoEventFn ev, AudioAttachFn att, AudioServiceFn svc) { g_iso_event_fn = ev; g_audio_attach_fn = att; g_audio_service_fn = svc; }
// set only while the audio driver drives a control transfer or command: any
// transfer event for someone else's endpoint that arrives meanwhile is handled
// (hid report dispatched + re-armed) instead of dropped. boot enumeration leaves
// it off so the hid bring-up behaves exactly as before. (satoru)
static bool g_dispatch_foreign = false;

// route usb driver diagnostics to the serial console. supports %d/%x/%s; plain
// runs are emitted in one go. previously a no-op, which left the whole xhci
// enumeration path invisible during bring-up. (satoru)
static void usb_log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char run[128]; int ri = 0;
    auto flush = [&](){ if (ri) { run[ri] = 0; SerialLogger::Log(run); ri = 0; } };
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            if (ri >= (int)sizeof(run) - 1) flush();
            run[ri++] = *p;
            continue;
        }
        ++p;
        flush();
        if      (*p == 'd') SerialLogger::LogDec(va_arg(ap, int));
        else if (*p == 'x') SerialLogger::LogHex((uint32_t)va_arg(ap, unsigned int));
        else if (*p == 's') SerialLogger::Log(va_arg(ap, const char*));
        else if (*p == '%') { run[ri++] = '%'; }
        else if (!*p) break;
    }
    flush();
    va_end(ap);
}

// xHCI DMA structures (dcbaa, command/event rings, erst) require >=64-byte
// alignment and must not straddle a 64KB boundary; KernelHeap::Alloc only
// guarantees 16-byte alignment, so an unaligned ring made the controller raise
// HCE (host controller error) and reject every command. over-allocate and round
// up to a page so all alignment + no-cross constraints hold. (satoru)
static void* usb_alloc_aligned(size_t size, size_t align) {
    uintptr_t raw = (uintptr_t)KernelHeap::Alloc(size + align);
    if (!raw) return nullptr;
    return (void*)((raw + (align - 1)) & ~(uintptr_t)(align - 1));
}

// small helpers kept local so we do not pull in <cstring> on a freestanding
// target (satoru).
static void usb_memset(void* dst, int v, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}
// crude spin used while waiting on controller state changes (satoru).
static inline void usb_spin(int loops) {
    for (volatile int d = 0; d < loops; d++);
}

// millisecond delay that also works before the scheduler has processes: spin
// on the wall clock the timer irq keeps advancing, and cap the spin so a dead
// clock cannot hang enumeration. (satoru)
static void usb_delay_ms(uint32_t ms) {
    if (!ms) return;
    uint64_t t0 = Scheduler::NowMs();
    uint64_t guard = (uint64_t)ms * 2000000ull;
    while (Scheduler::NowMs() - t0 < ms && guard--) __asm__ __volatile__("pause");
}

static inline uint16_t usb_le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t usb_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t USB::ReadCap(uint32_t offset) {
    return *(volatile uint32_t*)(bar0 + offset);
}

uint32_t USB::ReadOp(uint32_t offset) {
    return *(volatile uint32_t*)(bar0 + cap_length + offset);
}

void USB::WriteOp(uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(bar0 + cap_length + offset) = val;
}

uint32_t USB::ReadPort(int port) {
    return ReadOp(XHCI_OP_PORTSC(port));
}

void USB::WritePort(int port, uint32_t val) {
    WriteOp(XHCI_OP_PORTSC(port), val);
}

uint32_t USB::ReadRuntime(uint32_t offset) {
    uint32_t rts_off = ReadCap(XHCI_CAP_RTSOFF) & ~0x1F;
    return *(volatile uint32_t*)(bar0 + rts_off + offset);
}

void USB::WriteRuntime(uint32_t offset, uint32_t val) {
    uint32_t rts_off = ReadCap(XHCI_CAP_RTSOFF) & ~0x1F;
    *(volatile uint32_t*)(bar0 + rts_off + offset) = val;
}

// 64-bit runtime write, low dword first then high - required for ERDP so the
// controller latches a coherent dequeue pointer (satoru).
void USB::WriteRuntime64(uint32_t offset, uint64_t val) {
    WriteRuntime(offset, (uint32_t)(val & 0xFFFFFFFF));
    WriteRuntime(offset + 4, (uint32_t)(val >> 32));
}

void USB::WriteDoorbell(int slot, uint32_t val) {
    uint32_t db_off = ReadCap(XHCI_CAP_DBOFF) & ~0x3;
    *(volatile uint32_t*)(bar0 + db_off + slot * 4) = val;
}

const char* USB::SpeedName(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_FULL:  return "Full (12 Mbps)";
        case USB_SPEED_LOW:   return "Low (1.5 Mbps)";
        case USB_SPEED_HIGH:  return "High (480 Mbps)";
        case USB_SPEED_SUPER: return "Super (5 Gbps)";
        default:              return "Unknown";
    }
}

bool USB::WaitReady(int timeout_ms) {
    for (int i = 0; i < timeout_ms * 100; i++) {
        if (!(ReadOp(XHCI_OP_USBSTS) & XHCI_STS_CNR))
            return true;
        for (volatile int d = 0; d < 1000; d++);
    }
    return false;
}

bool USB::Reset() {
    // halt the controller first
    uint32_t cmd = ReadOp(XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RUN;
    WriteOp(XHCI_OP_USBCMD, cmd);

    // wait for halt
    for (int i = 0; i < 100000; i++) {
        if (ReadOp(XHCI_OP_USBSTS) & XHCI_STS_HCH)
            break;
        for (volatile int d = 0; d < 1000; d++);
    }

    // issue reset
    WriteOp(XHCI_OP_USBCMD, XHCI_CMD_HCRESET);
    // intel hosts need ~1 ms after setting HCRST before any register is
    // polled (linux xhci_reset, XHCI_INTEL_HOST quirk: udelay(1000)). (satoru)
    {
        uint64_t t0 = Scheduler::NowMs();
        for (uint64_t i = 0; i < 50000000ull && Scheduler::NowMs() - t0 < 2; i++)
            __asm__ __volatile__("pause");
    }

    // wait for reset to complete
    for (int i = 0; i < 500000; i++) {
        if (!(ReadOp(XHCI_OP_USBCMD) & XHCI_CMD_HCRESET))
            break;
        for (volatile int d = 0; d < 1000; d++);
    }

    return WaitReady(500);
}

bool USB::SubmitCommand(xHCI_TRB* cmd_trb, xHCI_TRB* result) {
    if (!cmd_ring || !event_ring) return false;

    // write the command into the ring honouring the producer cycle state, then
    // publish the cycle bit last so the controller never sees a half-written
    // trb with the owning cycle already flipped (satoru).
    xHCI_TRB* slot = &cmd_ring[cmd_ring_idx];
    const uint64_t my_trb = KernelPhysOf(slot);   // matched against the completion (satoru)
    slot->parameter = cmd_trb->parameter;
    slot->status = cmd_trb->status;
    uint32_t ctrl = cmd_trb->control & ~TRB_CYCLE;
    slot->control = ctrl | (cmd_cycle ? TRB_CYCLE : 0);

    cmd_ring_idx++;
    if (cmd_ring_idx >= USB_RING_TRBS - 1) {
        // link trb back to the head; toggle-cycle so we flip producer parity on
        // every wrap (satoru).
        cmd_ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(cmd_ring);
        cmd_ring[USB_RING_TRBS - 1].status = 0;
        cmd_ring[USB_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (cmd_cycle ? TRB_CYCLE : 0);
        cmd_ring_idx = 0;
        cmd_cycle = !cmd_cycle;
    }

    // ring the command doorbell (slot 0, target 0)
    WriteDoorbell(0, 0);

    // a command completion event lands on the (shared) event ring; spin for it,
    // SKIPPING port-status-change events that a port reset interleaves onto the
    // same ring (grabbing one made ENABLE_SLOT read a bogus slot id 0). ports are
    // polled directly, so dropping those events is safe. (satoru)
    // as linux: wait up to XHCI_CMD_DEFAULT_TIMEOUT (5000 ms) of wall time,
    // and accept only the completion whose command-trb pointer is the trb we
    // just queued (handle_cmd_completion: "mismatched command completion").
    // the old ~0.1 s limit gave up on a slow address-device on real hardware;
    // its late completion was then taken as the answer to the NEXT command,
    // and every later command saw the wrong slot (code 11). (satoru)
    // the iteration cap is the backstop: if the clock is not ticking yet this
    // loop must still end (a wall-clock-only wait hung the boot). (satoru)
    uint64_t t0 = Scheduler::NowMs();
    for (int it = 0; it < 2000 && Scheduler::NowMs() - t0 < 5000; it++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 50)) continue;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type != TRB_CMD_COMPLETE) {           // port status / stray transfer (satoru)
            // never drop these: each hid endpoint has one trb queued, so a report eaten here
            // left that device silent for the whole boot (the laptop's mouse). (satoru)
            if (type == TRB_TRANSFER_EVT) ForeignEvent(ev);
            else if (type == TRB_PORT_STATUS) NotePortStatus(ev);
            continue;
        }
        if ((ev.parameter & ~0xFULL) != my_trb) {
            usb_log("USB: skipped a stale command completion\n");
            continue;
        }
        uint32_t cc = (ev.status >> 24) & 0xFF;
        if (cc != 1) usb_log("USB: command completed with code %d (1 = success)\n", (int)cc);
        if (result) *result = ev;
        return true;
    }
    // say WHY: no completion arrived. usbsts shows a halted or errored
    // controller, crcr bit 3 shows whether the ring is running. (satoru)
    usb_log("USB: command timeout (5 s), usbsts=%x crcr=%x\n",
            ReadOp(XHCI_OP_USBSTS), ReadOp(XHCI_OP_CRCR));
    return false;
}

// write the event-ring dequeue pointer back to the controller with the
// event-handler-busy bit set, so the controller knows we have consumed up to
// (but not including) the current event_ring_idx slot. without this the event
// ring fills and the controller stops posting new events (satoru).
void USB::UpdateERDP() {
    uint64_t deq = KernelPhysOf(&event_ring[event_ring_idx]);
    // interrupter 0 erdp lives at runtime offset 0x38 (satoru).
    WriteRuntime64(0x38, deq | XHCI_ERDP_EHB);
}

bool USB::PollEventRing(xHCI_TRB* result, int timeout) {
    for (int i = 0; i < timeout * 100; i++) {
        // touch the controller every so often. an event ring is written by the
        // host controller (or, under qemu, by the device model's event loop), so
        // a pure memory spin can wait forever for a completion that is only
        // delivered on the next mmio exit. usbsts is read only. (satoru)
        if ((i & 0x7F) == 0) (void)ReadOp(XHCI_OP_USBSTS);
        // the ring is written by the host controller, not this thread: a
        // compiler barrier keeps the cycle test inside the spin. (satoru)
        __asm__ __volatile__("" ::: "memory");
        xHCI_TRB* evt = &event_ring[event_ring_idx];
        bool cycle = (evt->control & TRB_CYCLE) != 0;

        if (cycle == event_ccs) {
            if (result) *result = *evt;

            event_ring_idx++;
            if (event_ring_idx >= USB_RING_TRBS) {
                event_ring_idx = 0;
                event_ccs = !event_ccs;
            }

            // acknowledge: advance erdp so the ring does not stall (satoru).
            UpdateERDP();
            return true;
        }
        usb_spin(1000);
    }
    return false;
}

// every xhci controller the pci scan saw, kept so the ones other than the
// primary can be brought up afterwards (satoru)
struct XhciCand { uint8_t bus, dev, func; uint32_t bar_lo, bar_hi, ports; bool driven; };
static XhciCand g_cands[USB_MAX_CTL * 2];
static int g_ncands = 0;

bool USB::Init() {
    S = &g_xs[0];
    g_nctl = 0;
    g_ncands = 0;
    detected = false;
    device_count = 0;

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        devices[i].connected = false;
        usb_memset(&runtime[i], 0, sizeof(runtime[i]));
    }

    // scan pci bus for usb xhci controller (class 0c:03:30)
    uint32_t found_bar0 = 0;
    uint32_t found_bar_hi = 0;
    uint8_t found_bus = 0, found_dev = 0, found_func = 0;
    bool found = false;
    // scan ALL xhci controllers and keep the one with the most root ports.
    // taking the first one found picked the cpu's 3-port type-c controller
    // (00:0d.0) on the owner's laptop, while the built-in keyboard and every
    // usb-a port hang off the 26-port pch controller on bus 0x80. (satoru)
    uint32_t best_ports = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8);

                outl(0xCF8, addr | 0x00);
                uint32_t vendor_device = inl(0xCFC);
                if ((vendor_device & 0xFFFF) == 0xFFFF) { if (func == 0) break; continue; }   // empty slot: skip 7 functions (satoru)

                outl(0xCF8, addr | 0x08);
                uint32_t class_reg = inl(0xCFC);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class = (class_reg >> 16) & 0xFF;
                uint8_t prog_if = (class_reg >> 8) & 0xFF;

                // xhci: class=0c, subclass=03, progif=30
                if (base_class == 0x0C && sub_class == 0x03 && prog_if == 0x30) {
                    outl(0xCF8, addr | 0x10);
                    uint32_t bar_lo = inl(0xCFC) & ~0xFu;
                    // bare-metal diagnostics: the full 64-bit bar. the driver
                    // below only uses the low 32 bits, so a controller mapped
                    // above 4 gb would silently look dead. (satoru)
                    outl(0xCF8, addr | 0x14);
                    uint32_t bar_hi = inl(0xCFC);
                    // only a 64-bit memory bar (type bits 10b) has a high half (satoru)
                    outl(0xCF8, addr | 0x10);
                    if (((inl(0xCFC) >> 1) & 3) != 2) bar_hi = 0;
                    // read its root port count (HCSPARAMS1 bits 31:24). the bar
                    // may be above 4 gb, so map the first page uncached first;
                    // memory decode must be on for the read to land. (satoru)
                    uint64_t phys = ((uint64_t)bar_hi << 32) | bar_lo;
                    uint32_t ports = 0;
                    if (phys) {
                        outl(0xCF8, addr | 0x04);
                        uint32_t c = inl(0xCFC);
                        outl(0xCF8, addr | 0x04);
                        outl(0xCFC, c | (1u << 1));
                        KernelVMM::MapPage(phys & ~0xFFFULL, phys & ~0xFFFULL, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
                        uint32_t hcs1 = *(volatile uint32_t*)(uintptr_t)(phys + 0x04);
                        if (hcs1 != 0xFFFFFFFFu) ports = (hcs1 >> 24) & 0xFF;
                    }
                    usb_log("USB: xHCI candidate %d:%d.%d bar hi=%x lo=%x ports=%d\n",
                            bus, dev, func, bar_hi, bar_lo, (int)ports);
                    if (g_ncands < USB_MAX_CTL * 2) {
                        XhciCand& cd = g_cands[g_ncands++];
                        cd.bus = (uint8_t)bus; cd.dev = (uint8_t)dev; cd.func = (uint8_t)func;
                        cd.bar_lo = bar_lo; cd.bar_hi = bar_hi; cd.ports = ports; cd.driven = false;
                    }
                    if (!found || ports > best_ports) {
                        best_ports = ports;
                        found_bar0 = bar_lo;
                        found_bar_hi = bar_hi;
                        found_bus = bus;
                        found_dev = dev;
                        found_func = func;
                        found = true;
                    }
                }
            }
        }
    }

    if (!found) {
        usb_log("USB: No xHCI controller found\n");
        return false;
    }
    // primary first, exactly as before; the other controllers follow it (satoru)
    bool primary_ok = BringUp(found_bus, found_dev, found_func, found_bar0, found_bar_hi, best_ports, false);
    for (int i = 0; i < g_ncands; i++) {
        if (g_cands[i].bus == found_bus && g_cands[i].dev == found_dev && g_cands[i].func == found_func)
            g_cands[i].driven = true;
    }
    ProbeSecondaries();
    S = &g_xs[0];
    return primary_ok;
}

// bring one xhci controller up into g_xs[g_nctl]: bios handoff, reset, rings,
// run, then enumerate its ports. this is the body Init always had for the one
// controller; `secondary` only changes the log prefix. (satoru)
bool USB::BringUp(uint8_t found_bus, uint8_t found_dev, uint8_t found_func,
                  uint32_t found_bar0, uint32_t found_bar_hi, uint32_t best_ports, bool secondary) {
    S = &g_xs[g_nctl];
    usb_memset(S, 0, sizeof(*S));
    S->pci_bus = found_bus; S->pci_dev = found_dev; S->pci_func = found_func;
    ctx_stride = 32;
    cmd_cycle = true;
    event_ccs = true;
    usb_log("USB: using xHCI %d:%d.%d (%d root ports)%s\n",
            (int)found_bus, (int)found_dev, (int)found_func, (int)best_ports,
            secondary ? " [extra controller]" : "");

    // full 64-bit bar. uefi firmware (ovmf, and real laptops) often places the
    // xhci registers above 4 gb; using only the low 32 bits read garbage, the
    // controller reported 0 ports, and every usb keyboard/mouse was dead.
    // the high range is not in the boot identity map, so map a 64 kb uncached
    // window first (caps + operational + runtime + doorbells). (satoru)
    {
        uint64_t phys = ((uint64_t)found_bar_hi << 32) | found_bar0;
        for (uint64_t pg = phys & ~0xFFFULL; pg < phys + 0x10000ULL; pg += 0x1000ULL)
            KernelVMM::MapPage(pg, pg, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
        bar0 = (volatile uint8_t*)(uintptr_t)phys;
    }

    // enable bus mastering and memory space
    uint32_t cmd_addr = (1 << 31) | (found_bus << 16) | (found_dev << 11) | (found_func << 8) | 0x04;
    outl(0xCF8, cmd_addr);
    uint32_t pci_cmd = inl(0xCFC);
    pci_cmd |= (1 << 1) | (1 << 2); // memory space + bus master
    outl(0xCF8, cmd_addr);
    outl(0xCFC, pci_cmd);

    // read capability registers
    cap_length = ReadCap(XHCI_CAP_CAPLENGTH) & 0xFF;
    uint32_t hcsparams1 = ReadCap(XHCI_CAP_HCSPARAMS1);
    max_ports = (hcsparams1 >> 24) & 0xFF;
    max_slots = hcsparams1 & 0xFF;

    // HCCPARAMS1.CSZ (bit 2): 0 => 32-byte contexts, 1 => 64-byte contexts.
    // every device/endpoint context and the input control context use this
    // stride; getting it wrong scribbles the wrong context fields (satoru).
    uint32_t hccparams1 = ReadCap(XHCI_CAP_HCCPARAMS1);
    ctx_stride = (hccparams1 & (1u << 2)) ? 64 : 32;

    // bare-metal diagnostics: who owns the controller. the usb legacy support
    // extended capability (id 1) has a bios-owned (bit 16) and an os-owned
    // (bit 24) semaphore; on real firmware the bios usually still owns it and
    // expects the os to ask for it. this driver never does, so log the state.
    // read-only. (satoru)
    {
        uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;
        usb_log("USB: ports=%d slots=%d caplen=%d xECP=%x\n", (int)max_ports, (int)max_slots, (int)cap_length, xecp);
        for (int guard = 0; xecp && guard < 32; guard++) {
            uint32_t v = ReadCap(xecp * 4);
            if ((v & 0xFF) == 1) {
                usb_log("USB: legacy-support cap=%x bios-owned=%d os-owned=%d\n",
                        v, (int)((v >> 16) & 1), (int)((v >> 24) & 1));
                // bios -> os handoff, as linux quirk_usb_handoff_xhci: claim os
                // ownership, give the bios up to 1 s to let go, then mask the
                // legacy smis so firmware stops driving the controller. (satoru)
                volatile uint32_t* leg = (volatile uint32_t*)(bar0 + xecp * 4);
                if (v & (1u << 16)) {
                    *leg = v | (1u << 24);
                    uint64_t t0 = Scheduler::NowMs();
                    for (uint64_t i = 0; i < 20000000ull; i++) {
                        if (!(*leg & (1u << 16))) break;
                        if (Scheduler::NowMs() - t0 >= 1000) break;
                    }
                    if (*leg & (1u << 16)) {
                        usb_log("USB: bios did not release the controller in 1 s, taking it\n");
                        *leg = (*leg & ~(1u << 16)) | (1u << 24);
                    }
                }
                volatile uint32_t* ctl = leg + 1;
                *ctl = (*ctl & ~((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))) | (0x7u << 29);
                usb_log("USB: after handoff cap=%x ctlsts=%x\n", *leg, *ctl);
                break;
            }
            uint32_t next = (v >> 8) & 0xFF;
            if (!next) break;
            xecp += next;
        }
    }

    if (max_ports > USB_MAX_PORTS) max_ports = USB_MAX_PORTS;

    // reset controller
    if (!Reset()) {
        usb_log("USB: Controller reset failed\n");
        return false;
    }

    // set max device slots, keeping the other CONFIG bits (linux xhci_init:
    // config_reg &= ~HCS_SLOTS_MASK; config_reg |= max_slots). (satoru)
    WriteOp(XHCI_OP_CONFIG, (ReadOp(XHCI_OP_CONFIG) & ~0xFFu) | (max_slots & 0xFF));

    // allocate device context base address array (dcbaa)
    dcbaa = (uint64_t*)usb_alloc_aligned(4096, 4096);
    if (!dcbaa) return false;
    for (int i = 0; i < 256; i++) dcbaa[i] = 0;

    // scratchpad buffers (xhci 4.20). real intel controllers declare in
    // HCSPARAMS2 how many 4 kb pages of private memory they need, linked from
    // dcbaa[0]; until they get them, commands fail - the first one, enable
    // slot, failed on the owner's laptop. qemu's xhci asks for 0, which is why
    // this never showed up there. same as linux xhci_alloc_scratchpad. (satoru)
    {
        uint32_t hcs2 = ReadCap(XHCI_CAP_HCSPARAMS2);
        uint32_t nsp = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
        uint32_t pagesize = ReadOp(0x08) & 0xFFFF;   // PAGESIZE, bit n = 2^(n+12) (satoru)
        usb_log("USB: HCSPARAMS2=%x scratchpads=%d pagesize-bits=%x\n", hcs2, (int)nsp, pagesize);
        if (nsp) {
            uint64_t* spa = (uint64_t*)usb_alloc_aligned(4096, 4096);
            if (!spa) return false;
            for (uint32_t i = 0; i < 512; i++) spa[i] = 0;
            for (uint32_t i = 0; i < nsp && i < 512; i++) {
                uint8_t* pg = (uint8_t*)usb_alloc_aligned(4096, 4096);
                if (!pg) return false;
                usb_memset(pg, 0, 4096);
                spa[i] = KernelPhysOf(pg);
            }
            dcbaa[0] = KernelPhysOf(spa);
        }
    }

    // full 64-bit address: the high dword was hardcoded 0, which breaks the
    // moment the heap hands out memory above 4 gb (32 gb laptop). (satoru)
    uint64_t dcbaa_phys = KernelPhysOf(dcbaa);
    WriteOp(XHCI_OP_DCBAAP, (uint32_t)dcbaa_phys);
    WriteOp(XHCI_OP_DCBAAP + 4, (uint32_t)(dcbaa_phys >> 32));

    // allocate command ring (64 trbs)
    cmd_ring = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
    if (!cmd_ring) return false;
    usb_memset(cmd_ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
    cmd_ring_idx = 0;
    cmd_cycle = true;

    // set crcr (command ring control register), rcs=1 matches our initial
    // producer cycle (satoru).
    uint64_t crcr_val = KernelPhysOf(cmd_ring) | 1; // rcs=1
    WriteOp(XHCI_OP_CRCR, (uint32_t)crcr_val);
    WriteOp(XHCI_OP_CRCR + 4, (uint32_t)(crcr_val >> 32));

    // allocate event ring (64 trbs)
    event_ring = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
    if (!event_ring) return false;
    usb_memset(event_ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
    event_ring_idx = 0;
    event_ccs = true;
    erst_erdp_seg = (volatile uint8_t*)event_ring;

    // set up event ring segment table
    // erst entry: base address (8 bytes) + ring segment size (4 bytes) + reserved (4 bytes)
    uint64_t* erst = (uint64_t*)usb_alloc_aligned(4096, 4096);
    if (!erst) return false;
    erst[0] = KernelPhysOf(event_ring);
    ((uint32_t*)erst)[2] = USB_RING_TRBS; // segment size
    ((uint32_t*)erst)[3] = 0;             // reserved

    // write erstsz and erstba to interrupter 0
    WriteRuntime(0x28, 1);  // erstsz = 1 segment
    WriteRuntime64(0x30, KernelPhysOf(erst));

    // set erdp (with no EHB bit yet; controller has consumed nothing)
    WriteRuntime64(0x38, KernelPhysOf(event_ring));

    // start the controller
    uint32_t usbcmd = ReadOp(XHCI_OP_USBCMD);
    usbcmd |= XHCI_CMD_RUN | XHCI_CMD_INTE;
    WriteOp(XHCI_OP_USBCMD, usbcmd);

    // wait for controller to start
    for (int i = 0; i < 100000; i++) {
        if (!(ReadOp(XHCI_OP_USBSTS) & XHCI_STS_HCH))
            break;
        for (volatile int d = 0; d < 1000; d++);
    }

    detected = true;
    g_nctl = (int)(S - g_xs) + 1;   // visible to the audio hooks during the enumeration below (satoru)
    usb_log("USB: xHCI initialized, %d ports, %d max slots\n", max_ports, max_slots);
    // where the dma structures landed and whether the controller is happy
    // (USBSTS: bit 0 halted, bit 2 host system error, bit 12 host controller
    // error). (satoru)
    usb_log("USB: dcbaa hi=%x lo=%x cmdring lo=%x usbsts=%x\n",
            (uint32_t)(dcbaa_phys >> 32), (uint32_t)dcbaa_phys,
            (uint32_t)(uintptr_t)cmd_ring, ReadOp(XHCI_OP_USBSTS));

    // enumerate connected devices
    EnumerateDevices();

    return true;
}

bool USB::IsDetected() { return detected; }
int USB::GetPortCount() { return (int)max_ports; }
int USB::GetDeviceCount() { return device_count; }

bool USB::IsPortConnected(int port) {
    if (!detected || port < 0 || port >= (int)max_ports) return false;
    return (ReadPort(port) & XHCI_PORTSC_CCS) != 0;
}

uint8_t USB::GetPortSpeed(int port) {
    if (!detected || port < 0 || port >= (int)max_ports) return 0;
    return XHCI_PORTSC_SPEED(ReadPort(port));
}

bool USB::ResetPort(int port) {
    if (!detected || port < 0 || port >= (int)max_ports) return false;

    uint32_t portsc = ReadPort(port);
    // preserve rw bits, set port reset
    portsc = (portsc & 0x0E00C3E0) | XHCI_PORTSC_PR;
    WritePort(port, portsc);

    // wait for reset to complete
    for (int i = 0; i < 100000; i++) {
        portsc = ReadPort(port);
        if (!(portsc & XHCI_PORTSC_PR))
            return true;
        for (volatile int d = 0; d < 1000; d++);
    }
    return false;
}

// ── context accessors ─────────────────────────────────────────────────
// the input context is laid out as: input-control-context (1 entry), then the
// slot context (dci 0), then endpoint contexts (dci 1.. for ep0, ep1-out,
// ep1-in, ...). every entry is ctx_stride bytes apart (satoru).
uint32_t* USB::InputControlCtx(int idx) {
    return (uint32_t*)(runtime[idx].input_ctx + 0 * ctx_stride);
}
uint32_t* USB::SlotCtx(int idx) {
    return (uint32_t*)(runtime[idx].input_ctx + 1 * ctx_stride);
}
uint32_t* USB::EndpointCtx(int idx, int dci) {
    // dci 1 is ep0; it sits one stride past the slot context (satoru).
    return (uint32_t*)(runtime[idx].input_ctx + (uint32_t)(1 + dci) * ctx_stride);
}

// map xhci port speed id -> default control-endpoint max packet size (satoru).
static uint16_t default_max_packet(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return 8;
        // start full speed at 64 like linux hub_port_init: a device whose real
        // ep0 size is 64 sends 64-byte packets, and an 8-byte guess made the
        // controller flag babble (code 3) on the very first descriptor read.
        // a smaller device just short-packets; the 8-byte read below learns
        // the real size and Evaluate Context fixes it up. (satoru)
        case USB_SPEED_FULL:  return 64;
        case USB_SPEED_HIGH:  return 64;
        case USB_SPEED_SUPER: return 512;
        default:              return 8;
    }
}

int USB::FindDeviceBySlot(uint8_t slot) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (runtime[i].in_use && runtime[i].slot == slot) return i;
    return -1;
}

// allocate device + input contexts and the ep0 transfer ring, then issue
// Address Device so the slot leaves the "default" state and ep0 can carry
// real control transfers (satoru).
bool USB::AddressDevice(int idx) {
    USBDeviceRuntime& rt = runtime[idx];

    rt.device_ctx = (uint8_t*)usb_alloc_aligned(4096, 4096);
    rt.input_ctx  = (uint8_t*)usb_alloc_aligned(4096, 4096);
    rt.ep0_ring   = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
    if (!rt.device_ctx || !rt.input_ctx || !rt.ep0_ring) return false;
    usb_memset(rt.device_ctx, 0, 4096);
    usb_memset(rt.input_ctx, 0, 4096);
    usb_memset(rt.ep0_ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
    rt.ep0_idx = 0;
    rt.ep0_cycle = true;

    // link trb at the end of the ep0 ring so it wraps (satoru).
    rt.ep0_ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(rt.ep0_ring);
    rt.ep0_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;

    // dcbaa[slot] points at the output device context (satoru).
    dcbaa[rt.slot] = KernelPhysOf(rt.device_ctx);

    // input control context: add slot context (A0) + ep0 context (A1).
    uint32_t* icc = InputControlCtx(idx);
    icc[0] = 0;                 // drop flags
    icc[1] = (1u << 0) | (1u << 1); // add slot + ep0 (satoru)

    // slot context: route string (hub path), speed, one context entry (ep0),
    // root hub port = the root port the chain starts at (1-based). a ls/fs
    // device behind a high-speed hub also carries the transaction translator
    // hub slot id + port and, for multi-tt hubs, the mtt bit. (satoru)
    uint32_t* slot_ctx = SlotCtx(idx);
    slot_ctx[0] = ((uint32_t)1 << 27) | ((uint32_t)rt.speed << 20)   // ctx entries=1, speed
                | (rt.route & 0xFFFFFu);
    if (rt.tt_mtt) slot_ctx[0] |= (1u << 25);
    slot_ctx[1] = ((uint32_t)(rt.port + 1) << 16);                  // root hub port number (1-based)
    if (rt.tt_hub_slot) {
        slot_ctx[2] = (uint32_t)rt.tt_hub_slot | ((uint32_t)rt.tt_port << 8);
    }

    // ep0 context (dci 1): control endpoint (type 4), max packet by speed,
    // CErr=3, TR dequeue = ep0 ring base with DCS=1 (satoru).
    uint16_t mps = default_max_packet(rt.speed);
    uint32_t* ep0 = EndpointCtx(idx, 1);
    ep0[0] = 0;
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16); // CErr=3, EPType=Control, MaxPacketSize
    uint64_t ep0_deq = KernelPhysOf(rt.ep0_ring) | 1; // DCS=1 (satoru)
    ep0[2] = (uint32_t)(ep0_deq & 0xFFFFFFFF);
    ep0[3] = (uint32_t)(ep0_deq >> 32);
    ep0[4] = 8;  // average trb length hint (satoru)

    // Address Device command - input context pointer in parameter, slot in
    // control[31:24] (satoru).
    xHCI_TRB cmd = {};
    cmd.parameter = KernelPhysOf(rt.input_ctx);
    cmd.control = TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)rt.slot << 24);
    xHCI_TRB result = {};
    if (!SubmitCommand(&cmd, &result)) return false;
    uint8_t cc = (result.status >> 24) & 0xFF;
    return cc == TRB_CC_SUCCESS;
}

// scratch for one configuration parse. enumeration is serialized (boot, or
// PollHID under the input lock) and a hub only enumerates its children after
// its own ConfigureDevice returned, so these cannot be re-entered. (satoru)
struct UsbHidCand {
    uint8_t  iface, subclass, proto;
    uint8_t  ep_addr, ep_interval;
    uint16_t ep_mps, report_len;
    uint8_t  kind;          // 1 keyboard, 2 mouse, 0 not a usable hid input (satoru)
    int      lay_n;
    UsbHidLayout lay[4];
};
static uint8_t    s_cfg[1024];
static uint8_t    s_rpt[512];
static UsbHidCand s_cands[8];

// xhci interval field from a usb bInterval, as linux xhci_get_endpoint_interval:
// full/low speed interrupt bInterval is in frames (ms), high/super in microframes
// already encoded as an exponent. (satoru)
static uint32_t usb_xhci_interval(uint8_t speed, uint8_t interval) {
    if (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) {
        uint32_t frames = interval ? interval : 1;
        uint32_t v = frames * 8, fls = 0;
        while (v) { fls++; v >>= 1; }
        uint32_t xint = fls - 1;
        if (xint < 3) xint = 3;
        if (xint > 10) xint = 10;
        return xint;
    }
    uint32_t xint = interval ? (uint32_t)interval - 1 : 0;
    if (xint > 15) xint = 15;
    return xint;
}

// allocate the transfer ring + completion buffer of one interrupt endpoint and
// fill its endpoint context. returns the dci. (satoru)
bool USB::ConfigureDevice(int idx, const uint8_t* dev_desc) {
    USBDeviceRuntime& rt = runtime[idx];
    const USBDeviceDescriptor* dd = (const USBDeviceDescriptor*)dev_desc;

    uint8_t* cfg = s_cfg;
    usb_memset(cfg, 0, sizeof(s_cfg));
    if (!ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)(USB_DESC_CONFIG << 8), 0, 9, cfg))
        return false;
    uint16_t total_len = usb_le16(cfg + 2);
    if (total_len > sizeof(s_cfg)) {
        usb_log("USB: slot %d config %d bytes, scanning the first %d\n",
                (int)rt.slot, (int)total_len, (int)sizeof(s_cfg));
        total_len = sizeof(s_cfg);
    }
    if (total_len < 9) return false;
    if (!ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)(USB_DESC_CONFIG << 8), 0, total_len, cfg))
        return false;

    uint8_t config_value = cfg[5];

    // walk the concatenated descriptors. collect every hid interface with an
    // interrupt-in endpoint as a candidate, plus the hub class endpoint, the
    // first bulk-only mass storage pair and the audio flag. (satoru)
    int     ncand = 0;
    int     cur_hid = -1;
    bool    in_msd_iface = false;
    bool    in_hub_iface = false;
    bool    found_hub_iface = false;
    uint8_t hub_ep_addr = 0, hub_ep_interval = 0;
    uint16_t hub_ep_mps = 0;
    uint8_t bin_addr = 0, bout_addr = 0;
    uint16_t bin_mps = 0, bout_mps = 0;
    uint8_t bin_burst = 0, bout_burst = 0;
    uint8_t msd_iface_num = 0;
    int carried_bulk = 0;   // bulk ep whose ss companion may be the next descriptor (satoru)

    int p = 0;
    while (p + 2 <= (int)total_len) {
        uint8_t len = cfg[p];
        uint8_t type = cfg[p + 1];
        int prev_bulk = carried_bulk;
        carried_bulk = 0;
        if (len == 0) break; // malformed; bail rather than spin (satoru)

        if (type == 0x04 && p + 9 <= (int)total_len) {            // interface
            uint8_t iclass = cfg[p + 5];
            uint8_t isub   = cfg[p + 6];
            uint8_t iproto = cfg[p + 7];
            uint8_t ialt   = cfg[p + 3];
            if (iclass == 0x01) rt.has_audio = true;               // audio class: the audio driver takes it from here (satoru)
            cur_hid = -1;
            if (iclass == 0x03 && ncand < (int)(sizeof(s_cands)/sizeof(s_cands[0]))) {
                UsbHidCand& c = s_cands[ncand];
                usb_memset(&c, 0, sizeof(c));
                c.iface = cfg[p + 2];
                c.subclass = isub;
                c.proto = iproto;
                cur_hid = ncand++;
            }
            in_hub_iface = (iclass == USB_CLASS_HUB);
            if (in_hub_iface) found_hub_iface = true;
            // only the default alternate setting is ever selected (we never
            // send SET_INTERFACE), so the uas alt 1 must never contribute
            // endpoints to the bot pair. (satoru)
            in_msd_iface = (ialt == 0 && iclass == USB_MSC_CLASS &&
                            iproto == USB_MSC_PROTO_BOT && !(bin_addr && bout_addr));
            if (in_msd_iface) msd_iface_num = cfg[p + 2];
        } else if (type == USB_DESC_HID && cur_hid >= 0 && p + 9 <= (int)total_len) {
            // hid descriptor: bNumDescriptors at +5, then [type,len16] pairs.
            // the first class descriptor is the report descriptor. (satoru)
            if (cfg[p + 6] == USB_DESC_REPORT)
                s_cands[cur_hid].report_len = usb_le16(cfg + p + 7);
        } else if (type == 0x05 && cur_hid >= 0 && p + 7 <= (int)total_len) {
            uint8_t addr = cfg[p + 2];
            uint8_t attr = cfg[p + 3];
            if ((attr & 0x03) == 0x03 && (addr & 0x80) && !s_cands[cur_hid].ep_addr) {
                s_cands[cur_hid].ep_addr = addr;
                s_cands[cur_hid].ep_mps = (uint16_t)(usb_le16(cfg + p + 4) & 0x07FF);
                s_cands[cur_hid].ep_interval = cfg[p + 6];
            }
        } else if (type == 0x05 && in_hub_iface && p + 7 <= (int)total_len) {
            uint8_t addr = cfg[p + 2];
            uint8_t attr = cfg[p + 3];
            if ((attr & 0x03) == 0x03 && (addr & 0x80) && !hub_ep_addr) {
                hub_ep_addr = addr;
                hub_ep_mps = (uint16_t)(usb_le16(cfg + p + 4) & 0x07FF);
                hub_ep_interval = cfg[p + 6];
            }
        } else if (type == 0x05 && in_msd_iface && p + 7 <= (int)total_len) {
            uint8_t addr = cfg[p + 2];
            uint8_t attr = cfg[p + 3];
            if ((attr & 0x03) == 0x02) {                           // bulk (satoru)
                uint16_t mps = (uint16_t)(usb_le16(cfg + p + 4) & 0x07FF);
                if ((addr & 0x80) && !bin_addr)   { bin_addr = addr;  bin_mps = mps;  carried_bulk = 1; }
                if (!(addr & 0x80) && !bout_addr) { bout_addr = addr; bout_mps = mps; carried_bulk = 2; }
            }
        } else if (type == USB_DESC_SS_EP_COMP && prev_bulk && p + 6 <= (int)total_len) {
            // superspeed endpoint companion: bMaxBurst is what the xhci endpoint
            // context's max burst size field must carry. the streaming bits are
            // left alone (maxpstreams stays 0, so no streams are opened). (satoru)
            uint8_t burst = cfg[p + 2];
            if (prev_bulk == 1) bin_burst = burst;
            else                bout_burst = burst;
        }
        p += len;
    }

    bool found_bulk = (bin_addr != 0 && bout_addr != 0);
    bool class_hub  = dd && dd->bDeviceClass == USB_CLASS_HUB;
    bool is_hub     = class_hub || (found_hub_iface && hub_ep_addr);
    if (!is_hub && !found_bulk && ncand == 0) return false;

    // read and parse each hid report descriptor, then classify the interface
    // as keyboard / mouse. boot subclasses are trusted even when the report
    // descriptor is unusual (the fixed boot layout still decodes). (satoru)
    int selected[USB_MAX_INTR_EPS];
    int nsel = 0;
    if (!is_hub) {
        for (int c = 0; c < ncand; c++) {
            UsbHidCand& cd = s_cands[c];
            if (!cd.ep_addr) continue;
            uint16_t rlen = cd.report_len ? cd.report_len : 256;
            if (rlen > sizeof(s_rpt)) rlen = sizeof(s_rpt);
            usb_memset(s_rpt, 0, sizeof(s_rpt));
            if (ControlTransferEP(idx, 0x81, USB_REQ_GET_DESCRIPTOR,
                                  (uint16_t)(USB_DESC_REPORT << 8), cd.iface, rlen, s_rpt))
                ParseHidLayout(s_rpt, rlen, cd.lay, &cd.lay_n);
            for (int i = 0; i < cd.lay_n; i++) {
                if (cd.lay[i].has_kbd)   cd.kind = 1;
                if (cd.lay[i].has_mouse && cd.kind == 0) cd.kind = 2;
            }
            // re-scan for the interface protocol byte (bInterfaceProtocol @ +7).
            if (cd.kind == 0) {
                if (cd.subclass == 0x01 && cd.proto == 1) cd.kind = 1;
                else if (cd.subclass == 0x01 && cd.proto == 2) cd.kind = 2;
            }
            usb_log("USB: slot %d hid iface %d proto %d ep %x mps %d -> %s (%d report layout%s)\n",
                    (int)rt.slot, (int)cd.iface, (int)cd.proto, (uint32_t)cd.ep_addr,
                    (int)cd.ep_mps, cd.kind == 1 ? "keyboard" : (cd.kind == 2 ? "mouse" : "other"),
                    cd.lay_n, cd.lay_n == 1 ? "" : "s");
        }
        // bind one keyboard interface (the one with the most key fields) and
        // every mouse interface. the keyboard driver keeps one previous-report
        // history per device, so two keyboard interfaces on the same device
        // would overwrite each other's state. (satoru)
        int bestk = -1, bestks = -1;
        for (int c = 0; c < ncand; c++) {
            if (s_cands[c].kind != 1 || !s_cands[c].ep_addr) continue;
            int score = 0;
            for (int i = 0; i < s_cands[c].lay_n; i++)
                score += s_cands[c].lay[i].key_count + (s_cands[c].lay[i].mod_count ? 8 : 0);
            if (score > bestks) { bestks = score; bestk = c; }
        }
        if (bestk >= 0) selected[nsel++] = bestk;
        for (int c = 0; c < ncand && nsel < USB_MAX_INTR_EPS; c++)
            if (s_cands[c].kind == 2 && s_cands[c].ep_addr)
                selected[nsel++] = c;
        bool any_kbd = false, any_mouse = false;
        for (int i = 0; i < nsel; i++) {
            if (s_cands[selected[i]].kind == 1) any_kbd = true;
            else any_mouse = true;
        }
        rt.hid_type = any_kbd ? USB_HID_KEYBOARD : (any_mouse ? USB_HID_MOUSE : USB_HID_NONE);
    }

    // SET_CONFIGURATION(config_value) - no data stage (satoru).
    if (!ControlTransferEP(idx, 0x00, USB_REQ_SET_CONFIG, config_value, 0, 0, nullptr))
        return false;
    rt.cfg_value = config_value;

    // hub class descriptor (usb 2.0 11.23 / usb 3.0 10.15). read after set
    // configuration, as linux hub_configure does. it tells us the port count,
    // power-on delay, tt think time and multi-tt capability. (satoru)
    if (is_hub) {
        if (!HubReadDescriptor(idx, dd)) {
            usb_log("USB: slot %d hub descriptor read FAILED, not walking it\n", (int)rt.slot);
            is_hub = false;
        }
    }

    // build ONE input control context adding every endpoint this device gets
    // (interrupt endpoints for hid/hub, bulk in+out for mass storage), then
    // issue a single Configure Endpoint. (satoru)
    uint32_t* icc = InputControlCtx(idx);
    icc[0] = 0;
    icc[1] = (1u << 0);
    uint8_t max_dci = 1;

    for (int s = 0; s < nsel && rt.n_intr < USB_MAX_INTR_EPS; s++) {
        UsbHidCand& cd = s_cands[selected[s]];
        UsbIntrEp& ep = rt.intr[rt.n_intr];
        usb_memset(&ep, 0, sizeof(ep));
        ep.used     = true;
        ep.role     = (cd.kind == 1) ? USB_HID_KEYBOARD : USB_HID_MOUSE;
        ep.iface    = cd.iface;
        ep.ep_addr  = cd.ep_addr;
        ep.interval = cd.ep_interval;
        ep.boot     = (cd.subclass == 0x01 &&
                       ((cd.kind == 1 && cd.proto == 1) || (cd.kind == 2 && cd.proto == 2)));
        // a report-protocol mouse is decoded from its parsed fields; a boot
        // mouse without usable fields falls back to the fixed boot report. (satoru)
        bool has_fields = false;
        for (int i = 0; i < cd.lay_n; i++)
            if (cd.lay[i].has_mouse || cd.lay[i].has_kbd) has_fields = true;
        if (cd.kind == 2 && has_fields) ep.boot = false;
        ep.ep_id = (uint8_t)(((cd.ep_addr & 0x0F) * 2) + 1);
        for (int i = 0; i < cd.lay_n && i < 4; i++) ep.hid[i] = cd.lay[i];
        ep.hid_n = cd.lay_n > 4 ? 4 : cd.lay_n;

        ep.ring = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
        ep.buf  = (uint8_t*)usb_alloc_aligned(4096, 4096);
        if (!ep.ring || !ep.buf) {
            if (ep.ring) KernelHeap::Free(ep.ring);
            if (ep.buf)  KernelHeap::Free(ep.buf);
            usb_memset(&ep, 0, sizeof(ep));
            return false;
        }
        usb_memset(ep.ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
        usb_memset(ep.buf, 0, 4096);
        ep.idx = 0;
        ep.cycle = true;
        ep.buf_len = cd.ep_mps ? (int)cd.ep_mps : 8;
        if (ep.buf_len > USB_HID_REPORT_MAX) ep.buf_len = USB_HID_REPORT_MAX;
        ep.ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(ep.ring);
        ep.ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;

        icc[1] |= (1u << ep.ep_id);
        if (ep.ep_id > max_dci) max_dci = ep.ep_id;
        rt.n_intr++;

        uint32_t xint = usb_xhci_interval(rt.speed, cd.ep_interval);
        uint32_t* epc = EndpointCtx(idx, ep.ep_id);
        usb_memset(epc, 0, ctx_stride);
        epc[0] = (xint << 16);
        epc[1] = (3u << 1) | (7u << 3) | ((uint32_t)cd.ep_mps << 16);    // CErr=3, EPType=IntrIn, MPS
        uint64_t deq = KernelPhysOf(ep.ring) | 1;
        epc[2] = (uint32_t)deq;
        epc[3] = (uint32_t)(deq >> 32);
        epc[4] = (uint32_t)cd.ep_mps | ((uint32_t)cd.ep_mps << 16);
        usb_log("USB: slot %d hid iface %d boot=%d ep %x mps %d bInterval %d -> xhci interval %d\n",
                (int)rt.slot, (int)cd.iface, ep.boot ? 1 : 0, (uint32_t)cd.ep_addr,
                (int)cd.ep_mps, (int)cd.ep_interval, (int)xint);
    }

    if (is_hub && hub_ep_addr && rt.n_intr < USB_MAX_INTR_EPS) {
        UsbIntrEp& ep = rt.intr[rt.n_intr];
        usb_memset(&ep, 0, sizeof(ep));
        ep.used = true;
        ep.role = USB_HID_HUB;
        ep.ep_addr = hub_ep_addr;
        ep.interval = hub_ep_interval;
        ep.ep_id = (uint8_t)(((hub_ep_addr & 0x0F) * 2) + 1);
        ep.ring = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
        ep.buf  = (uint8_t*)usb_alloc_aligned(4096, 4096);
        if (!ep.ring || !ep.buf) {
            if (ep.ring) KernelHeap::Free(ep.ring);
            if (ep.buf)  KernelHeap::Free(ep.buf);
            usb_memset(&ep, 0, sizeof(ep));
            return false;
        }
        usb_memset(ep.ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
        usb_memset(ep.buf, 0, 4096);
        ep.idx = 0;
        ep.cycle = true;
        // the bitmap carries one hub status bit plus one bit per port. (satoru)
        int need = 1 + (rt.hub_ports + 7) / 8;
        ep.buf_len = hub_ep_mps > (uint16_t)need ? (int)hub_ep_mps : need;
        if (ep.buf_len > USB_HID_REPORT_MAX) ep.buf_len = USB_HID_REPORT_MAX;
        ep.ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(ep.ring);
        ep.ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
        icc[1] |= (1u << ep.ep_id);
        if (ep.ep_id > max_dci) max_dci = ep.ep_id;
        rt.n_intr++;

        uint32_t xint = usb_xhci_interval(rt.speed, hub_ep_interval);
        uint32_t* epc = EndpointCtx(idx, ep.ep_id);
        usb_memset(epc, 0, ctx_stride);
        epc[0] = (xint << 16);
        epc[1] = (3u << 1) | (7u << 3) | ((uint32_t)hub_ep_mps << 16);    // CErr=3, EPType=IntrIn, MPS
        uint64_t deq = KernelPhysOf(ep.ring) | 1;
        epc[2] = (uint32_t)deq;
        epc[3] = (uint32_t)(deq >> 32);
        epc[4] = (uint32_t)hub_ep_mps | ((uint32_t)hub_ep_mps << 16);
        usb_log("USB: slot %d hub status endpoint %x mps %d bInterval %d -> xhci interval %d\n",
                (int)rt.slot, (uint32_t)hub_ep_addr, (int)hub_ep_mps,
                (int)hub_ep_interval, (int)xint);
    }

    if (found_bulk) {
        rt.bulk_in_ep   = bin_addr;
        rt.bulk_out_ep  = bout_addr;
        rt.bulk_in_mps  = bin_mps  ? bin_mps  : 512;
        rt.bulk_out_mps = bout_mps ? bout_mps : 512;
        rt.bulk_in_burst  = bin_burst;
        rt.bulk_out_burst = bout_burst;
        rt.msd_iface    = msd_iface_num;
        // dci: OUT ep N -> 2N, IN ep N -> 2N+1 (satoru)
        rt.bulk_in_dci  = (uint8_t)(((bin_addr & 0x0F) * 2) + 1);
        rt.bulk_out_dci = (uint8_t)((bout_addr & 0x0F) * 2);
        usb_log("USB: slot %d msd iface %d bulk in %x mps %d burst %d, out %x mps %d burst %d\n",
                (int)rt.slot, (int)rt.msd_iface, (uint32_t)rt.bulk_in_ep,
                (int)rt.bulk_in_mps, (int)rt.bulk_in_burst, (uint32_t)rt.bulk_out_ep,
                (int)rt.bulk_out_mps, (int)rt.bulk_out_burst);

        rt.bulk_in_ring  = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
        rt.bulk_out_ring = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
        // 32kb bounce aligned to 32kb: a single normal trb's data buffer must
        // not cross a 64kb boundary, and a 32kb-aligned 32kb block never does.
        // (satoru)
        rt.bulk_bounce   = (uint8_t*)usb_alloc_aligned(USB_BULK_BOUNCE, USB_BULK_BOUNCE);
        if (!rt.bulk_in_ring || !rt.bulk_out_ring || !rt.bulk_bounce) return false;
        usb_memset(rt.bulk_in_ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
        usb_memset(rt.bulk_out_ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
        rt.bulk_in_idx = 0;  rt.bulk_in_cycle = true;
        rt.bulk_out_idx = 0; rt.bulk_out_cycle = true;
        rt.bulk_in_ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(rt.bulk_in_ring);
        rt.bulk_in_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
        rt.bulk_out_ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(rt.bulk_out_ring);
        rt.bulk_out_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;

        icc[1] |= (1u << rt.bulk_in_dci) | (1u << rt.bulk_out_dci);
        if (rt.bulk_in_dci  > max_dci) max_dci = rt.bulk_in_dci;
        if (rt.bulk_out_dci > max_dci) max_dci = rt.bulk_out_dci;

        // bulk endpoint contexts: EPType 2 = bulk-out, 6 = bulk-in, CErr=3,
        // max burst from the ss companion (0 is legal and means one packet per
        // burst), TR dequeue = ring base with DCS=1. max esit payload and
        // average trb length are reserved for bulk, so dword 4 stays 0. (satoru)
        uint32_t* epo = EndpointCtx(idx, rt.bulk_out_dci);
        usb_memset(epo, 0, ctx_stride);
        epo[1] = (3u << 1) | (2u << 3) | ((uint32_t)rt.bulk_out_burst << 8) |
                 ((uint32_t)rt.bulk_out_mps << 16);
        uint64_t odeq = KernelPhysOf(rt.bulk_out_ring) | 1;
        epo[2] = (uint32_t)(odeq & 0xFFFFFFFF);
        epo[3] = (uint32_t)(odeq >> 32);
        epo[4] = 0;

        uint32_t* epi = EndpointCtx(idx, rt.bulk_in_dci);
        usb_memset(epi, 0, ctx_stride);
        epi[1] = (3u << 1) | (6u << 3) | ((uint32_t)rt.bulk_in_burst << 8) |
                 ((uint32_t)rt.bulk_in_mps << 16);
        uint64_t ideq = KernelPhysOf(rt.bulk_in_ring) | 1;
        epi[2] = (uint32_t)(ideq & 0xFFFFFFFF);
        epi[3] = (uint32_t)(ideq >> 32);
        epi[4] = 0;
    }

    uint32_t* slot_ctx = SlotCtx(idx);
    // context entries must be the highest valid dci (satoru).
    slot_ctx[0] = (slot_ctx[0] & ~((uint32_t)0x1F << 27)) | ((uint32_t)max_dci << 27);
    // a hub advertises hub=1, its port count and the tt think time. a full
    // speed hub must clear mtt; a multi-tt high speed hub sets it. (satoru)
    if (is_hub) {
        slot_ctx[0] |= (1u << 26);
        if (rt.hub_multi_tt) slot_ctx[0] |= (1u << 25);
        else if (rt.speed == USB_SPEED_FULL) slot_ctx[0] &= ~(1u << 25);
        slot_ctx[1] = (slot_ctx[1] & 0x00FFFFFFu) | ((uint32_t)rt.hub_ports << 24);
        if (rt.speed == USB_SPEED_HIGH)
            slot_ctx[2] = (slot_ctx[2] & ~(0x3u << 16)) | ((uint32_t)rt.hub_ttt << 16);
    }

    xHCI_TRB cmd = {};
    cmd.parameter = KernelPhysOf(rt.input_ctx);
    cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
    xHCI_TRB result = {};
    if (!SubmitCommand(&cmd, &result)) return false;
    if (((result.status >> 24) & 0xFF) != TRB_CC_SUCCESS) return false;

    // a boot keyboard / boot fallback mouse is switched to the fixed boot
    // layout. report protocol is the hid power-on default, so non-boot
    // interfaces need no class request (and some devices stall on it). (satoru)
    for (int e = 0; e < rt.n_intr; e++) {
        UsbIntrEp& ep = rt.intr[e];
        if ((ep.role != USB_HID_KEYBOARD && ep.role != USB_HID_MOUSE) || !ep.boot) continue;
        ControlTransferEP(idx, 0x21, HID_REQ_SET_PROTOCOL, HID_PROTO_BOOT,
                          ep.iface, 0, nullptr);
    }

    if (is_hub) {
        rt.is_hub = true;
        usb_log("USB: slot %d is a %s hub, %d ports, tt=%s%s, power-on %d ms\n",
                (int)rt.slot, rt.hub_ss ? "SuperSpeed" : (rt.speed == USB_SPEED_HIGH ? "high-speed" : "full-speed"),
                (int)rt.hub_ports,
                rt.speed == USB_SPEED_HIGH ? (rt.hub_multi_tt ? "multi" : "single") : "none",
                rt.hub_ttt ? " (ttt)" : "", (int)rt.hub_pwr_good * 2);
        // usb 3 hubs need the tier depth before they forward packets. (satoru)
        if (rt.hub_ss) {
            uint8_t depth = rt.depth ? (uint8_t)(rt.depth - 1) : 0;
            if (!ControlTransferEP(idx, 0x20, USB_HUB_REQ_SET_DEPTH, depth, 0, 0, nullptr))
                usb_log("USB: slot %d SET_HUB_DEPTH(%d) failed\n", (int)rt.slot, (int)depth);
        }
    }

    // bring the mass-storage side up: inquiry + capacity. a failure leaves
    // msd_ready false (device still enumerated for lsusb) - honest. (satoru)
    if (found_bulk) {
        rt.is_msd = true;
        rt.msd_lun = 0;   // get-max-lun skipped: a stall on ep0 has no recovery
                          // path here, and single-lun sticks are the case that
                          // matters (qemu usb-storage is lun 0). (satoru)
        rt.msd_tag = 1;
        if (MsdInit(idx)) {
            usb_log("USB: mass storage ready, blocks=%d bs=%d\n",
                    (int)rt.msd_blocks, (int)rt.msd_block_size);
        } else {
            usb_log("USB: mass storage init FAILED (bulk eps configured)\n");
        }
    }

    return (rt.n_intr > 0 || found_bulk);
}
// run one complete control transfer on a slot's ep0 ring: setup stage, optional
// data stage, status stage; ring the slot's ep0 doorbell (dci 1) and wait for
// the transfer-event completion (satoru).
// the data stage never points into the caller's buffer: callers pass stack
// variables (a 4 byte port status), and a transfer that timed out stays queued
// on ep0. the controller completed it later into a stack frame that was being
// reused - the laptop panic 1112b: GET_PORT_STATUS (0x0100, port power) landed
// in the high half of a saved array offset and ControlTransferEP faulted on a
// garbage runtime pointer. one 4 kb dma buffer per device, never freed; the
// result is copied out only on success, and a timeout stops ep0 and moves its
// dequeue pointer past the abandoned td. (satoru)
static uint8_t* g_ctl_bounce[USB_MAX_CTL][USB_MAX_DEVICES];
static const uint32_t CTL_BOUNCE_SZ = 4096;
// kurono: the halt-recovery path in the input poll waits far less than 5 s. (satoru)
static uint32_t g_ctl_timeout_ms = 5000;

bool USB::ControlTransferEP(int idx, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                            void* data) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.ep0_ring) return false;
    void* caller_data = data;
    if (wLength > 0 && data) {
        int ci = (int)(S - g_xs);
        if (ci >= 0 && ci < USB_MAX_CTL && idx >= 0 && idx < USB_MAX_DEVICES && wLength <= CTL_BOUNCE_SZ) {
            if (!g_ctl_bounce[ci][idx]) g_ctl_bounce[ci][idx] = (uint8_t*)PMM::AllocBytes(CTL_BOUNCE_SZ);
            if (g_ctl_bounce[ci][idx]) {
                uint8_t* bb = g_ctl_bounce[ci][idx];
                if (!(bmRequestType & 0x80)) memcpy(bb, caller_data, wLength);
                data = bb;
            }
        }
    }

    bool in = (bmRequestType & 0x80) != 0;
    bool has_data = (wLength > 0);

    auto push = [&](uint64_t param, uint32_t status, uint32_t control) {
        xHCI_TRB* t = &rt.ep0_ring[rt.ep0_idx];
        t->parameter = param;
        t->status = status;
        t->control = (control & ~TRB_CYCLE) | (rt.ep0_cycle ? TRB_CYCLE : 0);
        rt.ep0_idx++;
        if (rt.ep0_idx >= USB_RING_TRBS - 1) {
            rt.ep0_ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(rt.ep0_ring);
            rt.ep0_ring[USB_RING_TRBS - 1].control =
                TRB_TYPE(TRB_LINK) | TRB_ENT | (rt.ep0_cycle ? TRB_CYCLE : 0);
            rt.ep0_idx = 0;
            rt.ep0_cycle = !rt.ep0_cycle;
        }
    };

    // setup stage: 8 bytes of the request packed into parameter (immediate
    // data), TRT selects the data-stage direction (satoru).
    uint64_t setup = (uint64_t)bmRequestType
                   | ((uint64_t)bRequest << 8)
                   | ((uint64_t)wValue   << 16)
                   | ((uint64_t)wIndex   << 32)
                   | ((uint64_t)wLength  << 48);
    uint32_t trt = !has_data ? TRB_TRT_NO_DATA : (in ? TRB_TRT_IN_DATA : TRB_TRT_OUT_DATA);
    push(setup, 8 /* always 8 */, TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | trt);

    // data stage (optional): buffer pointer + length, direction bit, IOC so we
    // also get a completion if no status stage is reached (satoru).
    if (has_data) {
        uint32_t dctrl = TRB_TYPE(TRB_DATA_STAGE) | (in ? TRB_DIR_IN : 0);
        push(KernelPhysOf(data), (uint32_t)wLength, dctrl);
    }

    // status stage: opposite direction of the data stage (or IN for no-data),
    // IOC so we get the final transfer event (satoru).
    bool status_in = has_data ? !in : true;
    push(0, 0, TRB_TYPE(TRB_STATUS_STAGE) | (status_in ? TRB_DIR_IN : 0) | TRB_IOC);

    // ring the doorbell for this slot, target = ep0 dci (1) (satoru).
    WriteDoorbell(rt.slot, 1);

    // wait for the transfer event that targets this slot; tolerate a couple of
    // unrelated events sneaking in (port-change etc.) by re-polling (satoru).
    // as linux usb_control_msg: up to 5000 ms of wall time
    // (USB_CTRL_GET_TIMEOUT), and only a transfer event for THIS slot's ep0
    // (dci 1) counts - the old loop took any transfer event and gave up after
    // ~0.1 s, which real devices can exceed. (satoru)
    // iteration cap as the backstop when the clock is not ticking. (satoru)
    uint64_t t0 = Scheduler::NowMs();
    uint32_t limit_ms = g_ctl_timeout_ms;
    for (int it = 0; it < 2000 && Scheduler::NowMs() - t0 < limit_ms; it++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 50)) continue;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type != TRB_TRANSFER_EVT) {           // port status / command noise (satoru)
            if (type == TRB_PORT_STATUS) NotePortStatus(ev);
            continue;
        }
        uint8_t ev_slot = (ev.control >> 24) & 0xFF;
        uint8_t ev_ep   = (ev.control >> 16) & 0x1F;
        if (ev_slot != rt.slot || ev_ep != 1) {
            ForeignEvent(ev);   // a wait must not eat hid reports, or that device goes silent (satoru)
            continue;
        }
        uint8_t cc = (ev.status >> 24) & 0xFF;
        if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
            usb_log("USB: control transfer slot %d failed, code %d\n", (int)rt.slot, (int)cc);
            // a stall halts ep0 (xhci 4.10.2.1). recover here, or each later request on this
            // device waits out its full 5 s: that was the boot freeze on the laptop. (satoru)
            RecoverEp(idx, 1, rt.ep0_ring, rt.ep0_idx, rt.ep0_cycle);
            HubClearTt(idx, 0, 0);   // control split state on the parent tt (satoru)
            return false;
        }
        if (data != caller_data && (bmRequestType & 0x80) && caller_data)
            memcpy(caller_data, data, wLength);   // (satoru)
        return true;
    }
    usb_log("USB: control transfer slot %d timeout (%d ms)\n", (int)rt.slot, (int)limit_ms);
    // abandon the td: stop ep0, then point its dequeue at our enqueue slot, so a
    // late completion cannot land anywhere and the next request does not queue
    // behind it. (satoru)
    {
        xHCI_TRB c = {};
        c.control = TRB_TYPE(15) | ((uint32_t)rt.slot << 24) | (1u << 16);   // stop endpoint, dci 1
        xHCI_TRB r = {};
        SubmitCommand(&c, &r);
        xHCI_TRB d = {};
        d.parameter = KernelPhysOf(&rt.ep0_ring[rt.ep0_idx]) | (rt.ep0_cycle ? 1u : 0u);
        d.control = TRB_TYPE(16) | ((uint32_t)rt.slot << 24) | (1u << 16);
        xHCI_TRB r2 = {};
        SubmitCommand(&d, &r2);
    }
    return false;
}

// endpoints that errored while something else was waiting on the event ring:
// one bit per interrupt endpoint. the next poll recovers them. (satoru)
static uint8_t g_halt_mask[USB_MAX_CTL][USB_MAX_DEVICES];
static uint8_t g_intr_errlog[USB_MAX_CTL][USB_MAX_DEVICES][USB_MAX_INTR_EPS];
static uint8_t g_intr_resets[USB_MAX_CTL][USB_MAX_DEVICES][USB_MAX_INTR_EPS];

// queue one Normal trb on an interrupt endpoint ring pointing at its report
// buffer and ring the doorbell so the controller delivers the next report. (satoru)
void USB::ArmIntrEp(int idx, int epi) {
    USBDeviceRuntime& rt = runtime[idx];
    if (epi < 0 || epi >= rt.n_intr) return;
    UsbIntrEp& ep = rt.intr[epi];
    if (!ep.used || !ep.ring || !ep.buf) return;

    xHCI_TRB* t = &ep.ring[ep.idx];
    t->parameter = KernelPhysOf(ep.buf);
    t->status = (uint32_t)ep.buf_len;
    t->control = (TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP)
               | (ep.cycle ? TRB_CYCLE : 0);

    ep.idx++;
    if (ep.idx >= USB_RING_TRBS - 1) {
        ep.ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(ep.ring);
        ep.ring[USB_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (ep.cycle ? TRB_CYCLE : 0);
        ep.idx = 0;
        ep.cycle = !ep.cycle;
    }

    ep.armed = true;
    WriteDoorbell(rt.slot, ep.ep_id);
}

void USB::ArmAllInterrupts(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    for (int e = 0; e < rt.n_intr; e++) ArmIntrEp(idx, e);
}

int USB::FindIntrEp(int idx, uint8_t ep_id) {
    USBDeviceRuntime& rt = runtime[idx];
    for (int e = 0; e < rt.n_intr; e++)
        if (rt.intr[e].used && rt.intr[e].ep_id == ep_id) return e;
    return -1;
}

// finish one interrupt completion: dispatch the report (hid) or latch the port
// change bitmap (hub), then re-arm the endpoint. errors halt the endpoint, so
// they are recorded for the next poll to reset it. (satoru)
void USB::HandleIntrCompletion(int idx, int epi, uint8_t cc, uint32_t residual) {
    USBDeviceRuntime& rt = runtime[idx];
    if (epi < 0 || epi >= rt.n_intr) return;
    UsbIntrEp& ep = rt.intr[epi];
    if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) {
        S->intr_done[idx]++;
        g_intr_resets[(int)(S - g_xs)][idx][epi] = 0;
        int got = ep.buf_len - (int)residual;
        if (got < 0) got = 0;
        if (ep.role == USB_HID_HUB) DispatchHubReport(idx, epi, ep.buf, got);
        else if (got > 0 && ep.role != USB_HID_NONE) DispatchHidReport(idx, epi, ep.buf, got);
        ep.armed = false;
        ArmIntrEp(idx, epi);
        return;
    }
    S->intr_err[idx]++;
    ep.armed = false;
    int ctl = (int)(S - g_xs);
    if (g_intr_errlog[ctl][idx][epi] < 8) {
        g_intr_errlog[ctl][idx][epi]++;
        usb_log("USB: slot %d interrupt ep %d (port %d) error code %d\n",
                (int)rt.slot, (int)ep.ep_id, (int)rt.port, (int)cc);
    }
    g_halt_mask[ctl][idx] |= (uint8_t)(1u << epi);
}

// ---- bulk transport + mass-storage bot (satoru) ------------------------------

// synchronous-transfer claim on the SHARED event ring: while a bulk/bot wait
// is consuming events (shell / block-layer context), the ~1khz PollHID drain
// (input kernel-process) must stay out or it would swallow the completion
// event this wait is spinning for. cooperative kernel processes + a volatile
// flag suffice; worst case PollHID skips a couple of passes. (satoru)
static volatile bool g_usb_sync_claim = false;

// scsi cbw/csw wire structs (bulk-only transport 1.0) (satoru)
struct __attribute__((packed)) UsbCbw {
    uint32_t sig;        // "USBC" (satoru)
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;      // 0x80 = data-in (satoru)
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cb[16];
};
struct __attribute__((packed)) UsbCsw {
    uint32_t sig;        // "USBS" (satoru)
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;     // 0 pass, 1 fail, 2 phase error (satoru)
};

// run ONE bulk transfer (a single normal trb through the bounce buffer) on
// the device's bulk in or out ring, ring the doorbell, and wait for the
// transfer event. len is capped to the bounce size; the bot layer chunks.
// short packets are success on bulk-in (out_transferred reports the actual
// count). mirrors the wait discipline of ControlTransferEP. (satoru)
bool USB::BulkTransferEP(int idx, bool dir_in, void* data, int len,
                         int* out_transferred) {
    USBDeviceRuntime& rt = runtime[idx];
    if (out_transferred) *out_transferred = 0;
    if (!rt.bulk_bounce || len < 0 || len > USB_BULK_BOUNCE) return false;
    xHCI_TRB* ring = dir_in ? rt.bulk_in_ring : rt.bulk_out_ring;
    if (!ring) return false;

    if (!dir_in && data && len > 0) {
        const uint8_t* s = (const uint8_t*)data;
        for (int i = 0; i < len; i++) rt.bulk_bounce[i] = s[i];
    }

    int*  pidx   = dir_in ? &rt.bulk_in_idx   : &rt.bulk_out_idx;
    bool* pcycle = dir_in ? &rt.bulk_in_cycle : &rt.bulk_out_cycle;
    uint8_t dci  = dir_in ? rt.bulk_in_dci    : rt.bulk_out_dci;

    xHCI_TRB* t = &ring[*pidx];
    const uint64_t my_trb = KernelPhysOf(t);
    t->parameter = KernelPhysOf(rt.bulk_bounce);
    t->status = (uint32_t)len;                    // trb transfer length (satoru)
    t->control = (TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP)
               | (*pcycle ? TRB_CYCLE : 0);
    (*pidx)++;
    if (*pidx >= USB_RING_TRBS - 1) {
        ring[USB_RING_TRBS - 1].parameter = KernelPhysOf(ring);
        ring[USB_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (*pcycle ? TRB_CYCLE : 0);
        *pidx = 0;
        *pcycle = !*pcycle;
    }

    bool was_claimed = g_usb_sync_claim;   // nested claims (cbw/data/csw) (satoru)
    g_usb_sync_claim = true;
    WriteDoorbell(rt.slot, dci);

    bool ok = false;
    uint8_t last_cc = 0xFF;
    for (int tries = 0; tries < 16; tries++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 5000)) break;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type != TRB_TRANSFER_EVT) {           // port/cmd noise: keep waiting (satoru)
            if (type == TRB_PORT_STATUS) NotePortStatus(ev);
            continue;
        }
        uint8_t ev_slot = (ev.control >> 24) & 0xFF;
        uint8_t ev_ep   = (ev.control >> 16) & 0x1F;
        if (ev_slot != rt.slot) {
            // another device's report: dispatch + re-arm it. dropping it here
            // left that endpoint armed with nothing queued, so a mouse moved
            // during a slow disk init stayed silent for the whole boot. (satoru)
            ForeignEvent(ev);
            continue;
        }
        int iep = FindIntrEp(idx, ev_ep);
        if (iep >= 0) {
            // a hid/hub report completed while we owned the ring: dispatch +
            // re-arm so input is not lost during long disk i/o. (satoru)
            HandleIntrCompletion(idx, iep, (uint8_t)((ev.status >> 24) & 0xFF),
                                 ev.status & 0x00FFFFFF);
            continue;
        }
        if (ev_ep != dci) continue;
        if ((ev.parameter & ~0xFULL) != (my_trb & ~0xFULL)) {
            // a completion from a transfer that a recovery just cancelled;
            // taking it as ours would pair the wrong data with this trb. (satoru)
            continue;
        }
        uint8_t cc = (ev.status >> 24) & 0xFF;
        last_cc = cc;
        uint32_t residual = ev.status & 0x00FFFFFF;
        int got = len - (int)residual;
        if (got < 0) got = 0;
        if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) {
            if (out_transferred) *out_transferred = got;
            ok = true;
        }
        break;
    }
    g_usb_sync_claim = was_claimed;

    if (!ok) {
        usb_log("USB: slot %d bulk %s len %d failed (cc %d)\n", (int)rt.slot,
                dir_in ? "in" : "out", len, (int)last_cc);
        HubClearTt(idx, dir_in ? rt.bulk_in_ep : rt.bulk_out_ep, 2);
    }
    if (ok && dir_in && data) {
        uint8_t* d = (uint8_t*)data;
        int got = out_transferred ? *out_transferred : len;
        if (got > len) got = len;   // never write past the caller's buffer (satoru)
        for (int i = 0; i < got; i++) d[i] = rt.bulk_bounce[i];
    }
    return ok;
}

// bulk-only mass storage reset (class request 0xff) plus the clear-halt and
// endpoint reset that linux does after it, so one stalled command cannot
// wedge the pipe for the rest of the boot. (satoru)
bool USB::MsdRecover(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.is_msd || !rt.bulk_in_ring || !rt.bulk_out_ring) return false;

    // the class reset readies the device for the next cbw; its status stage
    // is nacked until the reset is complete, so no extra wait is needed. (satoru)
    bool reset_ok = ControlTransferEP(idx, 0x21, USB_MSC_REQ_RESET, 0,
                                      rt.msd_iface, 0, nullptr);
    usb_delay_ms(50);

    // clear the device side halt on both pipes (harmless when not halted),
    // then reset the controller endpoint state and its dequeue pointer. (satoru)
    ControlTransferEP(idx, 0x02, USB_REQ_CLEAR_FEATURE, 0, rt.bulk_in_ep, 0, nullptr);
    ControlTransferEP(idx, 0x02, USB_REQ_CLEAR_FEATURE, 0, rt.bulk_out_ep, 0, nullptr);
    RecoverEp(idx, rt.bulk_in_dci, rt.bulk_in_ring, rt.bulk_in_idx, rt.bulk_in_cycle);
    RecoverEp(idx, rt.bulk_out_dci, rt.bulk_out_ring, rt.bulk_out_idx, rt.bulk_out_cycle);

    usb_log("USB: slot %d msd transport reset%s\n", (int)rt.slot,
            reset_ok ? "" : " (class reset stalled)");
    return true;
}

// one full bot round: cbw out, optional data stage, csw in + validation.
// a transport or phase error resets the pipe and retries the command; linux
// does the same from its scsi error handler. (satoru)
bool USB::MsdCommand(int idx, const uint8_t* cb, int cb_len, bool dir_in,
                     void* data, uint32_t data_len, uint32_t* out_residue) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.is_msd || !cb || cb_len < 1 || cb_len > 16) return false;
    if (data_len > USB_BULK_BOUNCE) return false;
    if (out_residue) *out_residue = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        UsbCbw cbw;
        usb_memset(&cbw, 0, sizeof(cbw));
        cbw.sig      = USB_CBW_SIG;
        cbw.tag      = ++rt.msd_tag;
        cbw.data_len = data_len;
        cbw.flags    = dir_in ? 0x80 : 0x00;
        cbw.lun      = rt.msd_lun;
        cbw.cb_len   = (uint8_t)cb_len;
        for (int i = 0; i < cb_len; i++) cbw.cb[i] = cb[i];

        int got = 0;
        bool phase_ok = true;
        UsbCsw csw;
        usb_memset(&csw, 0, sizeof(csw));
        bool have_csw = false;

        if (!BulkTransferEP(idx, false, &cbw, (int)sizeof(cbw), &got) ||
            got != (int)sizeof(cbw)) {
            phase_ok = false;
        }

        if (phase_ok && data_len > 0) {
            // data stage is at most one bounce chunk; callers chunk above. a
            // short bulk-in is fine (csw reports the residue). (satoru)
            if (!BulkTransferEP(idx, dir_in, data, (int)data_len, &got)) {
                phase_ok = false;
            } else if (dir_in && data_len > (uint32_t)sizeof(UsbCsw) &&
                       got == (int)sizeof(UsbCsw) && data &&
                       ((const UsbCsw*)data)->sig == USB_CSW_SIG) {
                // some devices skip a data phase and put the csw where the
                // data belonged; linux checks for this exact case. (satoru)
                csw = *(const UsbCsw*)data;
                have_csw = true;
            }
        }

        if (phase_ok && !have_csw) {
            // a device that appends a zero length packet shows up as a zero
            // byte csw read; retry the read like linux does. (satoru)
            for (int t = 0; t < 3 && !have_csw; t++) {
                if (!BulkTransferEP(idx, true, &csw, (int)sizeof(csw), &got)) {
                    phase_ok = false;
                    break;
                }
                if (got == (int)sizeof(csw)) { have_csw = true; break; }
                usb_delay_ms(1);
            }
        }

        if (phase_ok && have_csw) {
            // some bridges report a csw signature that is not the spec one;
            // linux learns the first signature and checks consistency after. (satoru)
            if (!rt.msd_csw_sig_ok) {
                rt.msd_csw_sig = csw.sig;
                rt.msd_csw_sig_ok = true;
                if (csw.sig != USB_CSW_SIG)
                    usb_log("USB: slot %d msd learned csw signature %x\n",
                            (int)rt.slot, csw.sig);
            }
            if (csw.sig != rt.msd_csw_sig) {
                usb_log("USB: slot %d msd csw signature %x, expected %x\n",
                        (int)rt.slot, csw.sig, rt.msd_csw_sig);
                phase_ok = false;
            } else if (csw.tag != cbw.tag) {
                usb_log("USB: slot %d msd csw tag %d, expected %d\n",
                        (int)rt.slot, (int)csw.tag, (int)cbw.tag);
                phase_ok = false;
            } else {
                if (out_residue) *out_residue = csw.residue;
                if (csw.status == 0) return true;
                if (csw.status != 2) return false;   // command failed, pipe is fine (satoru)
            }
        }

        if (attempt < 2) MsdRecover(idx);
    }
    return false;
}

// request sense (0x03): key/asc/ascq from the fixed format response. (satoru)
bool USB::MsdRequestSense(int idx, uint8_t* key, uint8_t* asc, uint8_t* ascq) {
    uint8_t sense[18];
    usb_memset(sense, 0, sizeof(sense));
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    if (!MsdCommand(idx, cb, 6, true, sense, 18, nullptr)) return false;
    uint8_t code = sense[0] & 0x7F;
    if (code != 0x70 && code != 0x71) return false;
    if (key)  *key  = (uint8_t)(sense[2] & 0x0F);
    if (asc)  *asc  = sense[12];
    if (ascq) *ascq = sense[13];
    return true;
}

// read capacity(10); when it fails or reports the 10-byte "too big" sentinel,
// ask with read capacity(16) so a > 2 tib disk still gets the right size. (satoru)
bool USB::MsdReadCapacity(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    uint64_t last_lba = 0;
    uint32_t bs = 0;

    uint8_t cap[8];
    usb_memset(cap, 0, sizeof(cap));
    uint8_t cb10[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    bool ok10 = MsdCommand(idx, cb10, 10, true, cap, 8, nullptr);
    if (ok10) {
        last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                   ((uint32_t)cap[2] << 8)  | (uint32_t)cap[3];
        bs       = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                   ((uint32_t)cap[6] << 8)  | (uint32_t)cap[7];
    }

    if (!ok10 || last_lba == 0xFFFFFFFFull || bs == 0 || bs > USB_BULK_BOUNCE) {
        uint8_t cap16[32];
        usb_memset(cap16, 0, sizeof(cap16));
        // service action in(16), service action 0x10 = read capacity(16).
        uint8_t cb16[16] = { 0x9E, 0x10, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 32, 0, 0 };
        if (!MsdCommand(idx, cb16, 16, true, cap16, 32, nullptr)) {
            usb_log("USB: slot %d msd read capacity failed\n", (int)rt.slot);
            return false;
        }
        last_lba = ((uint64_t)cap16[0] << 56) | ((uint64_t)cap16[1] << 48) |
                   ((uint64_t)cap16[2] << 40) | ((uint64_t)cap16[3] << 32) |
                   ((uint64_t)cap16[4] << 24) | ((uint64_t)cap16[5] << 16) |
                   ((uint64_t)cap16[6] << 8)  | (uint64_t)cap16[7];
        bs       = ((uint32_t)cap16[8] << 24) | ((uint32_t)cap16[9] << 16) |
                   ((uint32_t)cap16[10] << 8) | (uint32_t)cap16[11];
        usb_log("USB: slot %d msd read capacity(10)%s, capacity(16) last=%d bs=%d\n",
                (int)rt.slot, ok10 ? " sentinel" : " failed",
                (int)last_lba, (int)bs);
    }

    if (bs == 0 || bs > USB_BULK_BOUNCE) return false;
    rt.msd_blocks     = last_lba + 1;
    rt.msd_block_size = bs;
    return true;
}

// byte-granular read through the bot block layer, one block bounce at a time. (satoru)
bool USB::MsdReadBytes(int idx, uint64_t off, uint32_t len, void* buf) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!MsdIsReady(idx) || !buf || len == 0 || rt.msd_block_size == 0) return false;
    uint32_t bs = rt.msd_block_size;
    uint64_t first = off / bs;
    uint64_t last  = (off + (uint64_t)len - 1) / bs;
    uint32_t blocks = (uint32_t)(last - first + 1);
    uint8_t* tmp = (uint8_t*)KernelHeap::Alloc((uint64_t)blocks * bs);
    if (!tmp) return false;
    bool ok = MsdRead(idx, first, blocks, tmp);
    if (ok) {
        uint32_t skew = (uint32_t)(off % bs);
        uint8_t* d = (uint8_t*)buf;
        for (uint32_t i = 0; i < len; i++) d[i] = tmp[skew + i];
    }
    KernelHeap::Free(tmp);
    return ok;
}

// read the gpt entry table into ascii names/ranges. returns the count, or -1
// when there is no readable gpt. (satoru)
int USB::MsdGptScan(int idx, UsbMsdPart* out, int max) {
    USBDeviceRuntime& rt = runtime[idx];
    uint32_t bs = rt.msd_block_size ? rt.msd_block_size : 512;
    uint8_t* hdr = (uint8_t*)KernelHeap::Alloc(bs);
    if (!hdr) return -1;
    bool ok = MsdRead(idx, 1, 1, hdr);
    if (!ok) { KernelHeap::Free(hdr); return -1; }
    const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    bool gpt = true;
    for (int i = 0; i < 8; i++) if (hdr[i] != (uint8_t)sig[i]) gpt = false;
    if (!gpt) { KernelHeap::Free(hdr); return -1; }

    uint64_t ent_lba = ((uint64_t)hdr[72]) | ((uint64_t)hdr[73] << 8) |
                       ((uint64_t)hdr[74] << 16) | ((uint64_t)hdr[75] << 24) |
                       ((uint64_t)hdr[76] << 32) | ((uint64_t)hdr[77] << 40) |
                       ((uint64_t)hdr[78] << 48) | ((uint64_t)hdr[79] << 56);
    uint32_t nent = (uint32_t)hdr[80] | ((uint32_t)hdr[81] << 8) |
                    ((uint32_t)hdr[82] << 16) | ((uint32_t)hdr[83] << 24);
    uint32_t esz  = (uint32_t)hdr[84] | ((uint32_t)hdr[85] << 8) |
                    ((uint32_t)hdr[86] << 16) | ((uint32_t)hdr[87] << 24);
    KernelHeap::Free(hdr);
    if (esz < 128 || esz > 1024 || nent == 0 || nent > 256) return -1;

    uint32_t bytes = nent * esz;
    uint32_t blocks = (bytes + bs - 1) / bs;
    uint8_t* tab = (uint8_t*)KernelHeap::Alloc((uint64_t)blocks * bs);
    if (!tab) return -1;
    if (!MsdRead(idx, ent_lba, blocks, tab)) {
        KernelHeap::Free(tab);
        return -1;
    }

    int n = 0;
    for (uint32_t i = 0; i < nent && n < max; i++) {
        const uint8_t* e = tab + (uint64_t)i * esz;
        bool used = false;
        for (int k = 0; k < 16; k++) if (e[k]) { used = true; break; }
        if (!used) continue;
        UsbMsdPart& p = out[n];
        usb_memset(&p, 0, sizeof(p));
        p.start_lba = ((uint64_t)e[32]) | ((uint64_t)e[33] << 8) |
                      ((uint64_t)e[34] << 16) | ((uint64_t)e[35] << 24) |
                      ((uint64_t)e[36] << 32) | ((uint64_t)e[37] << 40) |
                      ((uint64_t)e[38] << 48) | ((uint64_t)e[39] << 56);
        p.end_lba   = ((uint64_t)e[40]) | ((uint64_t)e[41] << 8) |
                      ((uint64_t)e[42] << 16) | ((uint64_t)e[43] << 24) |
                      ((uint64_t)e[44] << 32) | ((uint64_t)e[45] << 40) |
                      ((uint64_t)e[46] << 48) | ((uint64_t)e[47] << 56);
        int j = 0;
        for (int k = 0; k < 36; k++) {
            uint16_t ch = (uint16_t)(e[56 + k * 2] | (e[57 + k * 2] << 8));
            if (ch >= 0x20 && ch < 0x7F && j < (int)sizeof(p.name) - 1)
                p.name[j++] = (char)ch;
        }
        p.name[j] = 0;
        n++;
    }
    KernelHeap::Free(tab);
    return n;
}

// the usb write guard window, in the owning disk's own lba space. only one
// usb disk can be the kurono disk, exactly like the nvme window. (satoru)
static int      g_usb_win_gid = -1;
static uint64_t g_usb_win_lo  = 0;
static uint64_t g_usb_win_end = 0;

static inline uint16_t msd_rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t msd_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// find the single partition named KURONO (or a fat32 volume labelled KURONO)
// on this usb disk and allow writes only inside it. no match, or more than
// one, leaves the whole disk read-only. mirrors DiskLog::FindKuronoWindow. (satoru)
bool USB::MsdFindKuronoWindow(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    int gid = (int)(S - g_xs) * USB_MAX_DEVICES + idx;
    if (g_usb_win_gid == gid) { g_usb_win_gid = -1; g_usb_win_lo = g_usb_win_end = 0; }

    UsbMsdPart parts[32];
    int n = MsdGptScan(idx, parts, 32);
    if (n < 0) {
        usb_log("USB: slot %d msd no readable gpt, usb disk stays read-only under the guard\n",
                (int)rt.slot);
        return false;
    }

    int hits = 0;
    uint64_t lo = 0, end = 0;
    for (int i = 0; i < n; i++) {
        bool match = false;
        const char* want = "KURONO";
        if (parts[i].name[0]) {
            match = true;
            for (int k = 0; k < 6; k++) if (parts[i].name[k] != want[k]) { match = false; break; }
            if (match && parts[i].name[6] != 0) match = false;
        }
        if (!match) {
            // a fat32 volume labelled KURONO counts too, like the nvme path.
            // windows only stores the real label in the root dir, so check
            // both the boot sector and the first root cluster. (satoru)
            uint8_t bs[90];
            usb_memset(bs, 0, sizeof(bs));
            if (!MsdReadBytes(idx, parts[i].start_lba * (uint64_t)rt.msd_block_size,
                              90, bs)) continue;
            const char fs[8] = { 'F','A','T','3','2',' ',' ',' ' };
            static const char lab[11] = { 'K','U','R','O','N','O',' ',' ',' ',' ',' ' };
            bool is32 = true;
            for (int k = 0; k < 8; k++) if (bs[82 + k] != (uint8_t)fs[k]) is32 = false;
            if (is32) {
                bool bpb = true;
                for (int k = 0; k < 11; k++) if (bs[71 + k] != (uint8_t)lab[k]) { bpb = false; break; }
                if (bpb) match = true;
            }
            if (!match && is32) {
                uint32_t bps = msd_rd16(bs + 11), spc = bs[13];
                uint32_t rsvd = msd_rd16(bs + 14), nfats = bs[16];
                uint32_t fatsz = msd_rd32(bs + 36), root = msd_rd32(bs + 44);
                uint32_t clus = bps * spc;
                if (bps >= 512 && bps <= 4096 && spc && nfats && fatsz &&
                    root >= 2 && clus <= USB_BULK_BOUNCE) {
                    uint64_t off = parts[i].start_lba * (uint64_t)rt.msd_block_size +
                                   ((uint64_t)rsvd + (uint64_t)nfats * fatsz) * bps;
                    uint8_t* dir = (uint8_t*)KernelHeap::Alloc(clus);
                    if (dir && MsdReadBytes(idx, off + (uint64_t)(root - 2) * clus, clus, dir)) {
                        for (uint32_t o = 0; o + 32 <= clus; o += 32) {
                            const uint8_t* ent = dir + o;
                            if (ent[0] == 0) break;
                            if (ent[0] == 0xE5 || ent[11] == 0x0F || (ent[11] & 0x08) == 0) continue;
                            bool m = true;
                            for (int k = 0; k < 11; k++) if (ent[k] != (uint8_t)lab[k]) { m = false; break; }
                            if (m) { match = true; break; }
                        }
                    }
                    if (dir) KernelHeap::Free(dir);
                }
            }
        }
        if (match) {
            hits++;
            lo  = parts[i].start_lba;
            end = parts[i].end_lba + 1;
        }
    }

    if (hits != 1 || end <= lo || end > rt.msd_blocks) {
        usb_log("USB: slot %d msd found %d KURONO partition(s), usb disk stays read-only\n",
                (int)rt.slot, hits);
        return false;
    }
    g_usb_win_gid = gid;
    g_usb_win_lo  = lo;
    g_usb_win_end = end;
    usb_log("USB: slot %d [diskguard] usb write window = KURONO partition lba ", (int)rt.slot);
    SerialLogger::LogHex64(lo);
    SerialLogger::Log(" - ");
    SerialLogger::LogHex64(end);
    SerialLogger::Log("\r\n");
    return true;
}

void USB::MsdRegisterBlockDev(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.msd_ready || rt.bd_name[0]) return;

    char name[8] = { 's', 'd', 'a', 0, 0, 0, 0, 0 };
    int c = 0;
    for (; c < 26; c++) {
        name[2] = (char)('a' + c);
        if (!BlockDev::FindByName(name)) break;
    }
    if (c >= 26) {
        usb_log("USB: slot %d msd no free sd name, not registering a block device\n",
                (int)rt.slot);
        return;
    }

    int gid = (int)(S - g_xs) * USB_MAX_DEVICES + idx;
    USBDeviceInfo& dev = devices[idx];
    BlockDevice bd = {};
    for (int i = 0; i < (int)sizeof(bd.name) - 1 && name[i]; i++) bd.name[i] = name[i];
    const char* drv = "usb";
    for (int i = 0; i < (int)sizeof(bd.driver) - 1 && drv[i]; i++) bd.driver[i] = drv[i];
    int m = 0;
    for (int i = 0; dev.manufacturer[i] && m < (int)sizeof(bd.model) - 2; i++) bd.model[m++] = dev.manufacturer[i];
    if (m && dev.product[0] && m < (int)sizeof(bd.model) - 2) bd.model[m++] = ' ';
    for (int i = 0; dev.product[i] && m < (int)sizeof(bd.model) - 1; i++) bd.model[m++] = dev.product[i];
    bd.model[m] = 0;
    if (!bd.model[0]) {
        const char* um = "USB mass storage";
        for (m = 0; um[m] && m < (int)sizeof(bd.model) - 1; m++) bd.model[m] = um[m];
        bd.model[m] = 0;
    }
    bd.capacity_lba = rt.msd_blocks;
    bd.sector_size  = rt.msd_block_size;
    bd.removable    = true;
    bd.ctx          = (void*)(intptr_t)gid;
    bd.read = [](void* ctx, uint64_t lba, uint32_t count, void* buf) -> bool {
        int g = (int)(intptr_t)ctx;
        int ctl = g / USB_MAX_DEVICES, i = g % USB_MAX_DEVICES;
        if (ctl < 0 || ctl >= g_nctl) return false;
        XhciState* sv = S;
        S = &g_xs[ctl];
        bool ok = MsdRead(i, lba, count, buf);
        S = sv;
        return ok;
    };
    bd.write = [](void* ctx, uint64_t lba, uint32_t count, const void* buf) -> bool {
        int g = (int)(intptr_t)ctx;
        int ctl = g / USB_MAX_DEVICES, i = g % USB_MAX_DEVICES;
        if (ctl < 0 || ctl >= g_nctl) return false;
        XhciState* sv = S;
        S = &g_xs[ctl];
        bool ok = MsdWrite(i, lba, count, buf);
        S = sv;
        return ok;
    };
    bd.flush = [](void* ctx) -> bool {
        int g = (int)(intptr_t)ctx;
        int ctl = g / USB_MAX_DEVICES, i = g % USB_MAX_DEVICES;
        if (ctl < 0 || ctl >= g_nctl) return false;
        XhciState* sv = S;
        S = &g_xs[ctl];
        bool ok = false;
        if (MsdIsReady(i)) {
            uint8_t cb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // synchronize cache(10) (satoru)
            ok = MsdCommand(i, cb, 10, false, nullptr, 0, nullptr);
        }
        S = sv;
        return ok;
    };
    if (BlockDev::Register(bd) >= 0) {
        for (int i = 0; i < (int)sizeof(rt.bd_name) - 1 && name[i]; i++) rt.bd_name[i] = name[i];
        rt.bd_name[sizeof(rt.bd_name) - 1] = 0;
    }
}

void USB::MsdUnregisterBlockDev(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    int gid = (int)(S - g_xs) * USB_MAX_DEVICES + idx;
    if (rt.bd_name[0]) {
        BlockDev::Unregister(rt.bd_name);
        rt.bd_name[0] = 0;
    }
    if (g_usb_win_gid == gid) {
        g_usb_win_gid = -1;
        g_usb_win_lo = g_usb_win_end = 0;
    }
}

// scsi bring-up for a freshly configured bot device. the order follows linux
// usb-storage + sd: get max lun, bulk-only reset, inquiry, then ready polling
// with request-sense and start-unit, then read capacity. the jms583 and other
// bridges report unit attention / not-ready for a while after power-on, so a
// fixed handful of retries without a real delay loses the disk. (satoru)
bool USB::MsdInit(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    USBDeviceInfo&    dev = devices[idx];
    rt.msd_tag = 0;
    rt.msd_csw_sig_ok = false;

    // get max lun (class interface request, one byte). a stall only means the
    // device wants lun 0, which is also the bot default. (satoru)
    uint8_t lun = 0;
    if (ControlTransferEP(idx, 0xA1, USB_MSC_REQ_GETLUN, 0, rt.msd_iface, 1, &lun) &&
        lun < 16) {
        rt.msd_lun = lun;
        usb_log("USB: slot %d msd max lun %d\n", (int)rt.slot, (int)lun);
    } else {
        rt.msd_lun = 0;
        usb_log("USB: slot %d msd get-max-lun stalled, using lun 0\n", (int)rt.slot);
    }

    // put the bridge in a known state; a warm reboot from another os can
    // leave it on the uas alt or mid-command. (satoru)
    if (ControlTransferEP(idx, 0x21, USB_MSC_REQ_RESET, 0, rt.msd_iface, 0, nullptr))
        usb_log("USB: slot %d msd bulk-only reset ok\n", (int)rt.slot);
    else
        usb_log("USB: slot %d msd bulk-only reset stalled\n", (int)rt.slot);
    usb_delay_ms(50);

    // inquiry: 36 bytes of standard data, retried on a transient sense (satoru)
    uint8_t inq[36];
    bool inq_ok = false;
    for (int attempt = 0; attempt < 4 && !inq_ok; attempt++) {
        usb_memset(inq, 0, sizeof(inq));
        uint8_t cb_inq[6] = { 0x12, 0, 0, 0, 36, 0 };
        if (MsdCommand(idx, cb_inq, 6, true, inq, 36, nullptr)) { inq_ok = true; break; }
        uint8_t key = 0, asc = 0, ascq = 0;
        if (MsdRequestSense(idx, &key, &asc, &ascq))
            usb_log("USB: slot %d msd inquiry sense key=%d asc=%x ascq=%x\n",
                    (int)rt.slot, (int)key, (int)asc, (int)ascq);
        usb_delay_ms(attempt == 0 ? 100 : 500);
    }
    if (!inq_ok) {
        usb_log("USB: slot %d msd inquiry failed\n", (int)rt.slot);
        return false;
    }
    // vendor(8)+product(16) ascii at offsets 8 and 16 (satoru)
    int j = 0;
    for (int i = 8; i < 16 && inq[i] >= 0x20 && inq[i] < 0x7F; i++)
        dev.manufacturer[j++] = (char)inq[i];
    while (j > 0 && dev.manufacturer[j - 1] == ' ') j--;
    dev.manufacturer[j] = 0;
    j = 0;
    for (int i = 16; i < 32 && inq[i] >= 0x20 && inq[i] < 0x7F; i++)
        dev.product[j++] = (char)inq[i];
    while (j > 0 && dev.product[j - 1] == ' ') j--;
    dev.product[j] = 0;

    // test unit ready: unit attention (after reset/power-on) and not-ready
    // (the bridge still bringing the nvme namespace up) are transient, so
    // retry with a real delay and send start unit once, as sd.c does. (satoru)
    bool ready = false;
    bool start_sent = false;
    int attempts = 0;
    uint8_t last_key = 0xFF, last_asc = 0, last_ascq = 0;
    uint64_t t0 = Scheduler::NowMs();
    while (attempts < 40) {
        attempts++;
        uint8_t cb_tur[6] = { 0x00, 0, 0, 0, 0, 0 };
        if (MsdCommand(idx, cb_tur, 6, false, nullptr, 0, nullptr)) { ready = true; break; }

        uint8_t key = 0, asc = 0, ascq = 0;
        if (!MsdRequestSense(idx, &key, &asc, &ascq)) {
            if (Scheduler::NowMs() - t0 > 15000) break;
            usb_delay_ms(250);
            continue;
        }
        if (key != last_key || asc != last_asc || ascq != last_ascq) {
            usb_log("USB: slot %d msd not ready: sense key=%d asc=%x ascq=%x (attempt %d)\n",
                    (int)rt.slot, (int)key, (int)asc, (int)ascq, attempts);
            last_key = key; last_asc = asc; last_ascq = ascq;
        }
        // anything other than unit attention / not ready is fatal (satoru)
        if (key != 2 && key != 6) break;
        if (key == 2 && !start_sent) {
            uint8_t cb_ss[6] = { 0x1B, 0, 0, 0, 1, 0 };   // start stop unit, start=1 (satoru)
            MsdCommand(idx, cb_ss, 6, false, nullptr, 0, nullptr);
            start_sent = true;
            usb_log("USB: slot %d msd start unit sent\n", (int)rt.slot);
        }
        if (Scheduler::NowMs() - t0 > 15000) break;
        usb_delay_ms(attempts < 2 ? 250 : 1000);
    }
    if (!ready) {
        usb_log("USB: slot %d msd never became ready (%d attempts)\n", (int)rt.slot, attempts);
        return false;
    }

    if (!MsdReadCapacity(idx)) return false;
    rt.msd_ready = true;
    usb_log("USB: slot %d msd capacity blocks=%d bs=%d\n", (int)rt.slot,
            (int)rt.msd_blocks, (int)rt.msd_block_size);

    MsdFindKuronoWindow(idx);
    MsdRegisterBlockDev(idx);
    return true;
}

int USB::MsdFirstDevice() {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (runtime[i].in_use && runtime[i].is_msd && runtime[i].msd_ready)
            return i;
    }
    return -1;
}

bool USB::MsdIsReady(int device) {
    if (device < 0 || device >= USB_MAX_DEVICES) return false;
    return runtime[device].in_use && runtime[device].is_msd &&
           runtime[device].msd_ready;
}

uint64_t USB::MsdBlocks(int device) {
    return MsdIsReady(device) ? runtime[device].msd_blocks : 0;
}

uint32_t USB::MsdBlockSize(int device) {
    return MsdIsReady(device) ? runtime[device].msd_block_size : 0;
}

bool USB::MsdRead(int device, uint64_t lba, uint32_t count, void* buf) {
    if (!MsdIsReady(device) || !buf || count == 0) return false;
    USBDeviceRuntime& rt = runtime[device];
    if (lba + count > rt.msd_blocks) return false;
    if (lba + count > 0xFFFFFFFFull) return false;   // read(10) is 32-bit (satoru)
    uint32_t max_blocks = USB_BULK_BOUNCE / rt.msd_block_size;
    if (max_blocks == 0) return false;
    uint8_t* dst = (uint8_t*)buf;
    while (count > 0) {
        uint32_t n = count > max_blocks ? max_blocks : count;
        uint8_t cb[10] = { 0x28, 0,
                           (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8),  (uint8_t)lba,
                           0,
                           (uint8_t)(n >> 8), (uint8_t)n, 0 };
        if (!MsdCommand(device, cb, 10, true, dst, n * rt.msd_block_size, nullptr))
            return false;
        dst   += n * rt.msd_block_size;
        lba   += n;
        count -= n;
    }
    return true;
}

bool USB::MsdWrite(int device, uint64_t lba, uint32_t count, const void* buf) {
    if (!MsdIsReady(device) || !buf || count == 0) return false;
    // the bare-metal write guard allows exactly one usb partition: the one
    // named (or labelled) KURONO on the disk that carries it. on the owner's
    // laptop every other usb partition holds macOS and their data. (satoru)
    if (NVMe::WriteGuardOn()) {
        int gid = (int)(S - g_xs) * USB_MAX_DEVICES + device;
        bool inside = g_usb_win_end != 0 && g_usb_win_gid == gid &&
                      lba >= g_usb_win_lo && lba + count <= g_usb_win_end &&
                      lba + count > lba;
        if (!inside) {
            static int s_logged = 0;
            if (s_logged < 8) {
                s_logged++;
                SerialLogger::Log("[diskguard] REFUSED usb write lba=");
                SerialLogger::LogHex64(lba);
                SerialLogger::Log(" count=");
                SerialLogger::LogDec((int)count);
                SerialLogger::Log("\r\n");
            }
            return false;
        }
    }
    USBDeviceRuntime& rt = runtime[device];
    if (lba + count > rt.msd_blocks) return false;
    if (lba + count > 0xFFFFFFFFull) return false;   // write(10) is 32-bit (satoru)
    uint32_t max_blocks = USB_BULK_BOUNCE / rt.msd_block_size;
    if (max_blocks == 0) return false;
    const uint8_t* src = (const uint8_t*)buf;
    while (count > 0) {
        uint32_t n = count > max_blocks ? max_blocks : count;
        uint8_t cb[10] = { 0x2A, 0,
                           (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8),  (uint8_t)lba,
                           0,
                           (uint8_t)(n >> 8), (uint8_t)n, 0 };
        if (!MsdCommand(device, cb, 10, false, (void*)src,
                        n * rt.msd_block_size, nullptr))
            return false;
        src   += n * rt.msd_block_size;
        lba   += n;
        count -= n;
    }
    return true;
}

// mass-storage end to end self test (kurono.usbmsdtest=1): gpt scan, a read
// from both partitions, and the write guard inside/outside the KURONO window.
// the writes put the sector back exactly as it was read. (satoru)
bool USB::MsdSelfTest() {
    int fails = 0;
    auto check = [&](const char* what, bool ok) {
        usb_log("[usbmsdtest] %s: %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    };
    int dev = MsdFirstDevice();
    if (dev < 0) {
        usb_log("[usbmsdtest] no ready mass-storage device\n");
        return false;
    }
    USBDeviceRuntime& rt = runtime[dev];
    usb_log("[usbmsdtest] dev %d blocks=%d bs=%d vendor '%s' product '%s'\n",
            dev, (int)rt.msd_blocks, (int)rt.msd_block_size,
            devices[dev].manufacturer, devices[dev].product);

    UsbMsdPart parts[32];
    int n = MsdGptScan(dev, parts, 32);
    usb_log("[usbmsdtest] gpt partitions=%d\n", n);
    int kur = -1, other = -1;
    for (int i = 0; i < n && i < 32; i++) {
        usb_log("[usbmsdtest]   part %d lba %d-%d name '%s'\n",
                i, (int)parts[i].start_lba, (int)parts[i].end_lba, parts[i].name);
        const char* want = "KURONO";
        bool match = parts[i].name[0] != 0;
        for (int k = 0; k < 6 && match; k++) if (parts[i].name[k] != want[k]) match = false;
        if (match && parts[i].name[6] != 0) match = false;
        if (match && kur < 0) kur = i;
        else if (!match && other < 0) other = i;
    }
    check("gpt found KURONO and a second partition", n >= 2 && kur >= 0 && other >= 0);
    if (kur < 0 || other < 0) return false;

    uint32_t bs = rt.msd_block_size;
    uint8_t* kbuf = (uint8_t*)KernelHeap::Alloc(bs);
    uint8_t* obuf = (uint8_t*)KernelHeap::Alloc(bs);
    if (!kbuf || !obuf) {
        if (kbuf) KernelHeap::Free(kbuf);
        if (obuf) KernelHeap::Free(obuf);
        return false;
    }
    bool rk = MsdRead(dev, parts[kur].start_lba, 1, kbuf);
    check("read KURONO partition", rk);
    bool ok_other = MsdRead(dev, parts[other].start_lba, 1, obuf);
    check("read second partition", ok_other);
    if (rk && bs >= 512) {
        bool fat = kbuf[510] == 0x55 && kbuf[511] == 0xAA;
        usb_log("[usbmsdtest] KURONO boot sector bytes 0-3 %x %x %x %x fat32sig=%d\n",
                (uint32_t)kbuf[0], (uint32_t)kbuf[1], (uint32_t)kbuf[2],
                (uint32_t)kbuf[3], fat ? 1 : 0);
    }

    if (NVMe::WriteGuardOn()) {
        bool wk = MsdWrite(dev, parts[kur].start_lba, 1, kbuf);
        check("write inside KURONO window allowed", wk);
        bool wo = ok_other ? MsdWrite(dev, parts[other].start_lba, 1, obuf) : true;
        check("write to the other partition refused", !wo);
        bool wg = MsdWrite(dev, 0, 1, kbuf);
        check("write outside every partition refused", !wg);
    } else {
        usb_log("[usbmsdtest] write guard off, write checks skipped\n");
    }

    KernelHeap::Free(kbuf);
    KernelHeap::Free(obuf);
    usb_log("[usbmsdtest] %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails == 0;
}
// ---- end bulk transport + bot (satoru) ---------------------------------------

// extract up to 32 bits from a hid report at a bit offset (hid packs fields
// lsb first). bytes past the received report read as zero. (satoru)
static uint32_t hid_bits(const uint8_t* d, int dlen, uint32_t off, uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        uint32_t b = off + i;
        if ((int)(b >> 3) < dlen && ((d[b >> 3] >> (b & 7)) & 1)) v |= 1ull << i;
    }
    return (uint32_t)v;
}

static int32_t hid_svalue(const uint8_t* d, int dlen, const UsbHidField& f) {
    if (!f.valid || f.size == 0) return 0;
    uint32_t v = hid_bits(d, dlen, f.off, f.size);
    if (f.size < 32 && f.lmin < 0 && (v & (1u << (f.size - 1)))) v |= ~0u << f.size;
    return (int32_t)v;
}

// parse a hid report descriptor into the report layouts we can decode. this is
// a focused parser, not a full hid stack: it records x/y/wheel, buttons,
// keyboard modifiers and the key array, per report id. it is what makes
// report-protocol keyboards and high-dpi gaming mice work. (satoru)
bool USB::ParseHidLayout(const uint8_t* rpt, int len, UsbHidLayout* out, int* out_n) {
    if (out_n) *out_n = 0;
    if (!rpt || len <= 0 || !out || !out_n) return false;

    struct Glob {
        uint32_t page;
        int32_t  lmin, lmax;
        uint32_t rsize, rcount;
        uint8_t  rid;
    };
    struct Acc {
        bool        used;
        uint8_t     id;
        uint32_t    bits;
        UsbHidLayout lay;
    };
    Glob g;
    usb_memset(&g, 0, sizeof(g));
    Glob gstack[4];
    int sp = 0;
    Acc acc[4];
    usb_memset(acc, 0, sizeof(acc));
    uint32_t local[32];
    int nlocal = 0;
    bool have_umin = false;
    uint32_t umin = 0, umax = 0;

    auto acc_for = [&](uint8_t id) -> Acc* {
        for (int a = 0; a < 4; a++) if (acc[a].used && acc[a].id == id) return &acc[a];
        for (int a = 0; a < 4; a++) if (!acc[a].used) {
            acc[a].used = true; acc[a].id = id;
            acc[a].lay.used = true; acc[a].lay.id = id;
            return &acc[a];
        }
        return nullptr;
    };
    auto add_usage = [&](uint32_t u) {
        if (nlocal < (int)(sizeof(local)/sizeof(local[0]))) local[nlocal++] = u;
    };
    auto resolved_usage = [&](uint32_t i) -> uint32_t {
        if ((int)i < nlocal) return local[i];
        if (have_umin) {
            uint32_t base = umin & 0xFFFFu;
            uint32_t u = base + i;
            if (u <= (umax & 0xFFFFu)) return (umin & 0xFFFF0000u) | u;
        }
        return 0;
    };

    int i = 0;
    while (i < len) {
        uint8_t item = rpt[i++];
        if (item == 0xFE) {                 // long item: skip per its size byte (satoru)
            if (i >= len) break;
            uint8_t sz = rpt[i];
            i += 2 + sz;
            continue;
        }
        uint8_t tag  = (item >> 4) & 0x0F;
        uint8_t type = (item >> 2) & 0x03;
        uint8_t size = item & 0x03;
        uint8_t bytes = (size == 3) ? 4 : size;
        uint32_t data = 0;
        for (uint8_t b = 0; b < bytes && i < len; b++) data |= ((uint32_t)rpt[i++]) << (8 * b);

        if (type == 1) {                                   // global (satoru)
            if (tag == 0x0) g.page = data & 0xFFFFu;       // Global: Usage Page
            else if (tag == 0x1) g.lmin = (bytes == 4) ? (int32_t)data :
                                  (bytes == 2) ? (int16_t)(data & 0xFFFF) : (int8_t)(data & 0xFF);
            else if (tag == 0x2) g.lmax = (bytes == 4) ? (int32_t)data :
                                  (bytes == 2) ? (int16_t)(data & 0xFFFF) : (int8_t)(data & 0xFF);
            else if (tag == 0x7) g.rsize = data;
            else if (tag == 0x8) g.rid = (uint8_t)data;    // report id (satoru)
            else if (tag == 0x9) g.rcount = data;
            else if (tag == 0xA && sp < 4) gstack[sp++] = g;   // push (satoru)
            else if (tag == 0xB && sp > 0) g = gstack[--sp];   // pop (satoru)
        } else if (type == 2) {                            // local (satoru)
            if (tag == 0x0) {                              // Local: Usage
                uint32_t u = (bytes == 4) ? data : ((g.page << 16) | (data & 0xFFFFu));
                add_usage(u);
            } else if (tag == 0x1) {
                umin = (bytes == 4) ? data : ((g.page << 16) | (data & 0xFFFFu));
                have_umin = true;
            } else if (tag == 0x2) {
                umax = (bytes == 4) ? data : ((g.page << 16) | (data & 0xFFFFu));
            }
        } else if (type == 0) {                            // main (satoru)
            Acc* a = acc_for(g.rid);
            if (tag == 0xA) {                              // Main: Collection
                uint32_t u = nlocal ? local[0] : 0;
                if ((u >> 16) == 0x01 && (u & 0xFFFF) == 0x06) a->lay.kind = 1;
                else if ((u >> 16) == 0x01 && (u & 0xFFFF) == 0x02 && a->lay.kind == 0) a->lay.kind = 2;
                nlocal = 0; have_umin = false;
            } else if (tag == 0x8) {                       // input (satoru)
                uint32_t n = g.rcount > 64 ? 64 : g.rcount;
                if (!(data & 1u)) {
                    for (uint32_t k = 0; k < n; k++) {
                        uint32_t usage = resolved_usage(k);
                        if (!usage) continue;
                        uint32_t page = usage >> 16, use = usage & 0xFFFFu;
                        uint32_t off = a->bits + k * g.rsize;
                        if (off > 0xFFFFu || g.rsize == 0 || g.rsize > 32) continue;
                        UsbHidLayout& L = a->lay;
                        if (page == 0x01) {
                            bool rel = (data & 0x04u) != 0;
                            if (use == 0x30 && !L.x.valid) {
                                L.x = { true, rel, (uint16_t)off, (uint8_t)g.rsize, g.lmin, g.lmax };
                            } else if (use == 0x31 && !L.y.valid) {
                                L.y = { true, rel, (uint16_t)off, (uint8_t)g.rsize, g.lmin, g.lmax };
                            } else if (use == 0x38 && !L.wheel.valid) {
                                L.wheel = { true, rel, (uint16_t)off, (uint8_t)g.rsize, g.lmin, g.lmax };
                            }
                        } else if (page == 0x09) {
                            if (use >= 1 && L.btn_count == 0) {
                                L.btn_off = (uint16_t)off;
                                L.btn_count = (uint8_t)(n > 16 ? 16 : n);
                            }
                        } else if (page == 0x07) {
                            if (use >= 0xE0 && use <= 0xE7 && L.mod_count == 0 &&
                                off >= (use - 0xE0) * g.rsize) {
                                uint32_t base = off - (use - 0xE0) * g.rsize;
                                L.mod_off = (uint16_t)base;
                                L.mod_count = 8;
                            } else if (use == 0 && (data & 0x02u) == 0 && L.key_count == 0) {
                                L.key_off = (uint16_t)off;
                                L.key_count = (uint16_t)(n > 64 ? 64 : n);
                                L.key_size = (uint8_t)g.rsize;
                            }
                        }
                    }
                }
                a->bits += n * g.rsize;
                if (a->bits > 0xFFFFu) a->bits = 0xFFFFu;
                nlocal = 0; have_umin = false;
            } else {
                // output/feature items do not consume bits of an input report. (satoru)
                nlocal = 0; have_umin = false;
            }
        }
    }

    int n = 0, nkind = 0;
    UsbHidLayout kind_only[4];
    usb_memset(kind_only, 0, sizeof(kind_only));
    for (int a = 0; a < 4; a++) {
        if (!acc[a].used) continue;
        UsbHidLayout& L = acc[a].lay;
        L.has_mouse = L.x.valid || L.y.valid;
        L.has_kbd = L.key_count > 0 || L.mod_count > 0;
        if (L.has_kbd && L.kind == 0) L.kind = 1;
        if (L.has_mouse && L.kind == 0) L.kind = 2;
        if (L.has_mouse || L.has_kbd) {
            if (n < 4) out[n++] = L;
        } else if (L.kind != 0 && nkind < 4) {
            kind_only[nkind++] = L;
        }
    }
    // a descriptor whose collection starts before its report id leaves a
    // field-less layout at id 0. only fall back to kind-only layouts when no
    // report has usable fields, otherwise the id 0 entry would shadow the
    // real one. (satoru)
    if (n == 0)
        for (int k = 0; k < nkind && n < 4; k++) out[n++] = kind_only[k];
    *out_n = n;
    return n > 0;
}

// native usb hid input enable (settings -> devices). default on. (satoru)
static bool g_usb_hid_input = true;
void USB::SetHIDInputEnabled(bool on) { g_usb_hid_input = on; }
bool USB::IsHIDInputEnabled() { return g_usb_hid_input; }

// decode one completed interrupt report with the layout parsed for its
// interface. boot keyboards keep their fixed layout; everything else uses the
// report-protocol fields, falling back to the old raw boot decode. (satoru)
void USB::DispatchHidReport(int idx, int epi, const uint8_t* report, int len) {
    USBDeviceRuntime& rt = runtime[idx];
    if (epi < 0 || epi >= rt.n_intr || !report || len <= 0) return;
    UsbIntrEp& ep = rt.intr[epi];
    const int kid = (int)(S - g_xs) * USB_MAX_DEVICES + idx;
    {
        static uint8_t seen[USB_MAX_CTL * USB_MAX_DEVICES * USB_MAX_INTR_EPS];
        int si = kid * USB_MAX_INTR_EPS + epi;
        if (si >= 0 && si < (int)sizeof(seen) && seen[si] < 3) {
            seen[si]++;
            usb_log("USB: slot %d ep %d (%s) report len %d: %x %x\n", (int)rt.slot,
                    (int)ep.ep_id, ep.role == USB_HID_KEYBOARD ? "kbd" : "mouse", len,
                    (uint32_t)(report[0] | (len > 1 ? report[1] << 8 : 0) | (len > 2 ? report[2] << 16 : 0) | (len > 3 ? (uint32_t)report[3] << 24 : 0)),
                    (uint32_t)(len > 4 ? (report[4] | (len > 5 ? report[5] << 8 : 0) | (len > 6 ? report[6] << 16 : 0) | (len > 7 ? (uint32_t)report[7] << 24 : 0)) : 0));
        }
    }
    if (!g_usb_hid_input) return;   // usb input disabled -> ps/2 takes over (satoru)

    // boot protocol devices speak the fixed layout we always decoded. (satoru)
    if (ep.boot) {
        if (ep.role == USB_HID_KEYBOARD) {
            Keyboard::ProcessUSBReport((uint8_t)kid, report, (size_t)len);
        } else if (ep.role == USB_HID_MOUSE) {
            if (len >= 6) Mouse::ProcessUSBAbsReport(report, len);
            else          Mouse::ProcessUSBReport(report, len);
        }
        return;
    }

    // pick the layout for this report id (a report with ids prefixes the id
    // byte, which is not part of the field offsets). (satoru)
    const uint8_t* data = report;
    int dlen = len;
    UsbHidLayout* lay = nullptr;
    if (ep.hid_n > 0) {
        bool any_id = false;
        for (int h = 0; h < ep.hid_n; h++) if (ep.hid[h].id) any_id = true;
        if (any_id) {
            if (dlen < 1) return;
            uint8_t rid = data[0];
            data++; dlen--;
            for (int h = 0; h < ep.hid_n; h++) if (ep.hid[h].id == rid) lay = &ep.hid[h];
            if (!lay) return;             // an output/vendor report id (satoru)
        } else {
            for (int h = 0; h < ep.hid_n; h++) if (ep.hid[h].id == 0) { lay = &ep.hid[h]; break; }
        }
    }

    if (lay && lay->has_mouse) {
        uint32_t btns = lay->btn_count ? hid_bits(data, dlen, lay->btn_off, lay->btn_count) : 0;
        int dx = (int)hid_svalue(data, dlen, lay->x);
        int dy = (int)hid_svalue(data, dlen, lay->y);
        int wh = (int)hid_svalue(data, dlen, lay->wheel);
        bool absolute = lay->x.valid && lay->y.valid &&
                        !lay->x.relative && !lay->y.relative;
        if (absolute) {
            int ax = dx, ay = dy;
            if (lay->x.lmax > lay->x.lmin) ax = (int)((int64_t)(dx - lay->x.lmin) * 32767 / (lay->x.lmax - lay->x.lmin));
            if (lay->y.lmax > lay->y.lmin) ay = (int)((int64_t)(dy - lay->y.lmin) * 32767 / (lay->y.lmax - lay->y.lmin));
            if (ax < 0) ax = 0;
            if (ax > 32767) ax = 32767;
            if (ay < 0) ay = 0;
            if (ay > 32767) ay = 32767;
            uint8_t r[6] = { (uint8_t)(btns & 0x1F),
                             (uint8_t)(ax & 0xFF), (uint8_t)((ax >> 8) & 0xFF),
                             (uint8_t)(ay & 0xFF), (uint8_t)((ay >> 8) & 0xFF),
                             (uint8_t)(int8_t)wh };
            Mouse::ProcessUSBAbsReport(r, 6);
        } else {
            if (dx < -127) dx = -127;
            if (dx > 127) dx = 127;
            if (dy < -127) dy = -127;
            if (dy > 127) dy = 127;
            if (wh < -127) wh = -127;
            if (wh > 127) wh = 127;
            uint8_t r[4] = { (uint8_t)(btns & 0x1F),
                             (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)(int8_t)wh };
            Mouse::ProcessUSBReport(r, 4);
        }
        return;
    }

    if (lay && lay->has_kbd) {
        // synthesize the 8 byte boot report the keyboard decoder expects:
        // modifiers in byte 0, up to six key usages in bytes 2..7. (satoru)
        uint8_t r[8] = {0};
        r[0] = (uint8_t)hid_bits(data, dlen, lay->mod_off, lay->mod_count ? lay->mod_count : 8);
        int k = 2;
        for (uint32_t u = 0; u < lay->key_count && k < 8; u++) {
            uint32_t usage = hid_bits(data, dlen, lay->key_off + u * lay->key_size,
                                      lay->key_size ? lay->key_size : 8);
            if (usage != 0 && usage != 1) r[k++] = (uint8_t)usage;
        }
        Keyboard::ProcessUSBReport((uint8_t)kid, r, 8);
        return;
    }

    // no usable parsed layout: old raw behaviour (satoru)
    if (ep.role == USB_HID_KEYBOARD) {
        Keyboard::ProcessUSBReport((uint8_t)kid, report, (size_t)len);
    } else if (ep.role == USB_HID_MOUSE) {
        if (len >= 6) Mouse::ProcessUSBAbsReport(report, len);
        else          Mouse::ProcessUSBReport(report, len);
    }
}

// tear down a slot's resources on disconnect. children of a hub go first so
// their slots are released while the hub is still addressable. heap has no
// Free-by-region quirk here; KernelHeap::Free coalesces, so returning the
// pages is safe. (satoru)
void USB::TeardownDevice(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.in_use) return;

    // a hub takes its whole subtree with it (nested hubs, their children). (satoru)
    for (int j = 0; j < USB_MAX_DEVICES; j++) {
        if (j != idx && runtime[j].in_use && runtime[j].parent == (uint8_t)idx)
            TeardownDevice(j);
    }

    // the audio driver must stop streaming before the slot goes away (satoru)
    if (rt.has_audio && g_audio_attach_fn)
        g_audio_attach_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, false);

    // Disable Slot so the controller releases the device context (satoru).
    if (rt.slot) {
        xHCI_TRB cmd = {};
        cmd.control = TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)rt.slot << 24);
        xHCI_TRB result = {};
        SubmitCommand(&cmd, &result);
        dcbaa[rt.slot] = 0;
    }
    bool had_kbd = false;
    for (int e = 0; e < rt.n_intr; e++) {
        if (rt.intr[e].role == USB_HID_KEYBOARD) had_kbd = true;
        if (rt.intr[e].ring) KernelHeap::Free(rt.intr[e].ring);
        if (rt.intr[e].buf)  KernelHeap::Free(rt.intr[e].buf);
    }
    if (had_kbd)
        Keyboard::RemoveUSBDevice((uint8_t)((int)(S - g_xs) * USB_MAX_DEVICES + idx));
    if (rt.is_msd) MsdUnregisterBlockDev(idx);   // drop the sd node before the rings go (satoru)
    if (rt.device_ctx) KernelHeap::Free(rt.device_ctx);
    if (rt.input_ctx)  KernelHeap::Free(rt.input_ctx);
    if (rt.ep0_ring)   KernelHeap::Free(rt.ep0_ring);
    if (rt.bulk_in_ring)  KernelHeap::Free(rt.bulk_in_ring);   // msd teardown (satoru)
    if (rt.bulk_out_ring) KernelHeap::Free(rt.bulk_out_ring);
    if (rt.bulk_bounce)   KernelHeap::Free(rt.bulk_bounce);
    if (rt.iso_ring)      PMM::FreeBytes(rt.iso_ring, 4096);   // pmm page, not a heap block (satoru)
    if (rt.iso_fb_ring)   PMM::FreeBytes(rt.iso_fb_ring, 4096);

    int ctl = (int)(S - g_xs);
    g_halt_mask[ctl][idx] = 0;
    usb_memset(g_intr_errlog[ctl][idx], 0, sizeof(g_intr_errlog[ctl][idx]));
    usb_memset(&devices[idx], 0, sizeof(devices[idx]));
    usb_memset(&rt, 0, sizeof(rt));
}

// printable device path: root port, then each hub port on the chain. (satoru)
static int usb_rt_path(const USBDeviceRuntime* rtbase, int idx, char* out, int cap) {
    if (idx < 0 || idx >= USB_MAX_DEVICES || cap <= 0) return 0;
    const USBDeviceRuntime& rt = rtbase[idx];
    int p = 0;
    if (rt.parent != 0xFF && rt.depth > 1)
        p = usb_rt_path(rtbase, rt.parent, out, cap);
    char num[8]; int n = 0;
    uint32_t v = (uint32_t)rt.port;             // root port is 0 based in logs (satoru)
    if (rt.parent != 0xFF) v = rt.parent_port;
    char rev[8]; int r = 0;
    while (v) { rev[r++] = (char)('0' + v % 10); v /= 10; }
    while (r) num[n++] = rev[--r];
    if (p && p < cap - 1) out[p++] = '.';
    for (int i = 0; i < n && p < cap - 1; i++) out[p++] = num[i];
    out[p] = 0;
    return p;
}

// address one device at the given topology position (root port or hub port),
// read its descriptors, configure it, and arm every interrupt endpoint. the
// caller has already reset the port and learned the speed. (satoru)
bool USB::EnumerateDeviceAt(int root_port, uint32_t route, uint8_t depth,
                            uint8_t parent, uint8_t parent_port, uint8_t speed) {
    if (speed == 0) return false;
    if (depth > USB_MAX_HUB_DEPTH) {
        usb_log("USB: refusing device at depth %d (xhci route limit)\n", (int)depth);
        return false;
    }
    if (parent != 0xFF && parent_port > USB_HUB_MAX_ROUTE_PORTS) {
        usb_log("USB: hub port %d is beyond the 15-port route limit\n", (int)parent_port);
        return false;
    }

    // find a free runtime/devices slot (satoru).
    int idx = -1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!runtime[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        usb_log("USB: no free device slot for port %d\n", (int)root_port);
        return false;
    }

    // enable slot (satoru).
    xHCI_TRB enable_cmd = {};
    enable_cmd.control = TRB_TYPE(TRB_ENABLE_SLOT);
    xHCI_TRB result = {};
    if (!SubmitCommand(&enable_cmd, &result)) {
        usb_log("USB: port %d enable-slot cmd failed\n", (int)root_port);
        return false;
    }
    uint8_t slot = (result.control >> 24) & 0xFF;
    if (slot == 0) {
        usb_log("USB: port %d enable-slot returned slot 0\n", (int)root_port);
        return false;
    }

    USBDeviceRuntime& rt = runtime[idx];
    usb_memset(&rt, 0, sizeof(rt));
    rt.in_use = true;
    rt.slot = slot;
    rt.port = (uint8_t)root_port;
    rt.speed = speed;
    rt.hid_type = USB_HID_NONE;
    rt.depth = depth;
    rt.parent = parent;
    rt.parent_port = parent_port;
    rt.route = route & 0xFFFFFu;

    // ls/fs behind a high-speed hub needs the parent's transaction translator:
    // the hub slot id, the 1-based port, and mtt for multi-tt hubs. a fs hub
    // has no tt, and hs/ss devices do not use one. (satoru)
    if (parent != 0xFF && (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW)) {
        USBDeviceRuntime& ph = runtime[parent];
        if (ph.in_use && ph.speed == USB_SPEED_HIGH) {
            rt.tt_hub_slot = ph.slot;
            rt.tt_port = parent_port;
            rt.tt_mtt = ph.hub_multi_tt;
        }
    }

    USBDeviceInfo& dev = devices[idx];
    usb_memset(&dev, 0, sizeof(dev));
    dev.connected = true;
    dev.port = (uint8_t)root_port;
    dev.slot = slot;
    dev.speed = speed;

    if (idx >= device_count) device_count = idx + 1;

    if (!AddressDevice(idx)) {
        usb_log("USB: Address Device failed on port %d slot %d\n", (int)root_port, (int)slot);
        TeardownDevice(idx);
        return false;
    }

    // read the first 8 bytes to learn bMaxPacketSize0 for full/low speed, then
    // the full 18-byte device descriptor (satoru).
    USBDeviceDescriptor desc = {};
    if ((speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) &&
        ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(USB_DESC_DEVICE << 8), 0, 8, &desc)) {
        uint8_t real = ((const uint8_t*)&desc)[7];
        uint16_t cur = default_max_packet(speed);
        if ((real == 8 || real == 16 || real == 32 || real == 64) && real != cur) {
            uint32_t* icc = InputControlCtx(idx);
            icc[0] = 0;
            icc[1] = (1u << 1);
            uint32_t* e0 = EndpointCtx(idx, 1);
            e0[1] = (e0[1] & 0x0000FFFFu) | ((uint32_t)real << 16);
            xHCI_TRB ev = {};
            ev.parameter = KernelPhysOf(rt.input_ctx);
            ev.control = TRB_TYPE(13) | ((uint32_t)rt.slot << 24);
            xHCI_TRB er = {};
            bool ok = SubmitCommand(&ev, &er) && ((er.status >> 24) & 0xFF) == TRB_CC_SUCCESS;
            usb_log("USB: port %d ep0 max packet %d -> %d (evaluate context %s)\n",
                    (int)root_port, (int)cur, (int)real, ok ? "ok" : "FAILED");
        }
    }
    if (ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(USB_DESC_DEVICE << 8), 0, 18, &desc)) {
        dev.vendor_id = desc.idVendor;
        dev.product_id = desc.idProduct;
        dev.dev_class = desc.bDeviceClass;
        dev.dev_subclass = desc.bDeviceSubClass;
    }

    // set product name based on class (preserves the original label table)
    const char* class_name = "Unknown";
    switch (dev.dev_class) {
        case 0x00: class_name = "Composite"; break;
        case 0x01: class_name = "Audio"; break;
        case 0x02: class_name = "CDC/Comm"; break;
        case 0x03: class_name = "HID"; break;
        case 0x05: class_name = "Physical"; break;
        case 0x06: class_name = "Image"; break;
        case 0x07: class_name = "Printer"; break;
        case 0x08: class_name = "Mass Storage"; break;
        case 0x09: class_name = "Hub"; break;
        case 0x0A: class_name = "CDC-Data"; break;
        case 0x0E: class_name = "Video"; break;
        case 0x0F: class_name = "Health"; break;
        case 0xE0: class_name = "Wireless"; break;
        case 0xFF: class_name = "Vendor-Specific"; break;
    }
    int j = 0;
    while (class_name[j] && j < 31) { dev.product[j] = class_name[j]; j++; }
    dev.product[j] = 0;
    dev.manufacturer[0] = 0;

    // configure the interrupt endpoints (or bulk pair). if this device is not
    // a usable HID/hub we still keep it enumerated for DumpInfo. (satoru)
    if (ConfigureDevice(idx, (const uint8_t*)&desc)) {
        if (rt.hid_type == USB_HID_KEYBOARD)
            Keyboard::AddUSBDevice((uint8_t)((int)(S - g_xs) * USB_MAX_DEVICES + idx));
        ArmAllInterrupts(idx);
    }

    char path[32];
    usb_rt_path(runtime, idx, path, sizeof(path));
    if (parent == 0xFF) {
        usb_log("USB: enumerated port %d -> slot %d hid_type=%d (0=none 1=kbd 2=mouse)\n",
                (int)root_port, (int)idx, (int)rt.hid_type);
    } else {
        usb_log("USB: enumerated hub port %s -> slot %d hid_type=%d\n",
                path, (int)idx, (int)rt.hid_type);
    }
    if (rt.tt_hub_slot) {
        usb_log("USB: slot %d routed through tt hub slot %d port %d%s\n",
                (int)rt.slot, (int)rt.tt_hub_slot, (int)rt.tt_port,
                rt.tt_mtt ? " (mtt)" : "");
    }

    // a hub brings its own ports up now, after its endpoint is armed so a
    // connect change that happens during the scan is latched too. (satoru)
    if (rt.is_hub) HubStart(idx);

    // usb audio: hand a device with an audio-class interface to the audio driver (satoru)
    if (rt.has_audio && g_audio_attach_fn)
        g_audio_attach_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, true);

    return true;
}

// full per-port bring-up for a root hub port: reset, learn the speed, address. (satoru)
bool USB::EnumeratePort(int port) {
    if (!IsPortConnected(port)) return false;
    ResetPort(port);
    usb_spin(20000);
    if (!IsPortConnected(port)) return false;
    uint8_t speed = GetPortSpeed(port);
    if (speed == 0) return false;
    return EnumerateDeviceAt(port, 0, 1, 0xFF, 0, speed);
}

bool USB::EnumerateDevices() {
    if (!detected) return false;
    device_count = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        devices[i].connected = false;
        if (runtime[i].in_use) TeardownDevice(i);
    }

    for (int port = 0; port < (int)max_ports; port++) {
        EnumeratePort(port);
    }
    // process status changes latched while the tree was being brought up (satoru)
    ServiceHubs();
    ServiceRootPorts();
    return true;
}

// ---- usb hub class (usb 2.0 chapter 11) -------------------------------------- (satoru)

int USB::FindHubChild(int hubidx, uint8_t port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (runtime[i].in_use && runtime[i].parent == (uint8_t)hubidx &&
            runtime[i].parent_port == port)
            return i;
    return -1;
}

int USB::FindRootDeviceByPort(uint8_t port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (runtime[i].in_use && runtime[i].parent == 0xFF && runtime[i].port == port)
            return i;
    return -1;
}

bool USB::HubReadDescriptor(int idx, const USBDeviceDescriptor* dd) {
    USBDeviceRuntime& rt = runtime[idx];
    bool ss = dd && dd->bDeviceProtocol == USB_HUB_PROTO_SS &&
              rt.speed == USB_SPEED_SUPER;
    uint8_t buf[16];
    usb_memset(buf, 0, sizeof(buf));
    uint8_t type = ss ? USB_DESC_HUB_SS : USB_DESC_HUB;
    if (!ControlTransferEP(idx, 0xA0, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)(type << 8), 0, sizeof(buf), buf)) {
        // some hubs answer the class descriptor with the usb2 type even when
        // they advertise superspeed; try the other one before giving up. (satoru)
        usb_memset(buf, 0, sizeof(buf));
        type = ss ? USB_DESC_HUB : USB_DESC_HUB_SS;
        if (!ControlTransferEP(idx, 0xA0, USB_REQ_GET_DESCRIPTOR,
                               (uint16_t)(type << 8), 0, sizeof(buf), buf))
            return false;
    }
    // some hubs answer a superspeed descriptor request with the usb2 type
    // byte; the first fields are identical, so accept either. (satoru)
    if (buf[0] < 7 || (buf[1] != USB_DESC_HUB && buf[1] != USB_DESC_HUB_SS)) return false;
    uint8_t ports = buf[2];
    if (ports == 0) return false;
    rt.hub_ss = ss;
    rt.hub_ports = ports > 31 ? 31 : ports;
    rt.hub_chars = usb_le16(buf + 3);
    rt.hub_pwr_good = buf[5];
    rt.hub_multi_tt = (dd && dd->bDeviceProtocol == USB_HUB_PROTO_HS_MULTI);
    rt.hub_ttt = (uint8_t)((rt.hub_chars >> 5) & 0x3);
    rt.hub_pending = 0;
    rt.hub_hub_status = false;
    rt.hub_dirty = false;
    usb_log("USB: hub descriptor type %x: %d ports chars %x power-good %d ms\n",
            (uint32_t)buf[1], (int)ports, (uint32_t)rt.hub_chars, (int)rt.hub_pwr_good * 2);
    return true;
}

bool USB::HubGetPortStatus(int idx, uint8_t port, uint16_t* status, uint16_t* change) {
    uint8_t b[4];
    usb_memset(b, 0, sizeof(b));
    if (!ControlTransferEP(idx, 0xA3, 0x00, 0, port, 4, b)) return false;
    if (status) *status = usb_le16(b);
    if (change) *change = usb_le16(b + 2);
    return true;
}

bool USB::HubSetPortFeature(int idx, uint8_t port, uint16_t feature) {
    return ControlTransferEP(idx, 0x23, 0x03, feature, port, 0, nullptr);
}

bool USB::HubClearPortFeature(int idx, uint8_t port, uint16_t feature) {
    return ControlTransferEP(idx, 0x23, 0x01, feature, port, 0, nullptr);
}

// tell the transaction translator to drop its state for an endpoint after a
// failed split transfer (usb 2.0 11.24.2.8, linux usb_hub_clear_tt_buffer).
// without this a single error can wedge one TT buffer for good. (satoru)
void USB::HubClearTt(int idx, uint8_t ep_addr, uint8_t xfer_type) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.tt_hub_slot) return;
    int hub = FindDeviceBySlot(rt.tt_hub_slot);
    if (hub < 0) return;
    // the request names the usb device address. the controller picks it at
    // address-device and reports it in the output slot context (dword 3 bits
    // 7:0); it is not the slot id. (satoru)
    uint8_t addr = rt.device_ctx ? (uint8_t)(((const uint32_t*)rt.device_ctx)[3] & 0xFF) : 0;
    if (!addr) return;
    static bool in_clear = false;
    if (in_clear) return;
    in_clear = true;
    bool in = (ep_addr & 0x80) != 0;
    uint16_t devinfo = (uint16_t)(ep_addr & 0x0F);
    devinfo |= (uint16_t)((uint16_t)(addr & 0x7F) << 4);
    devinfo |= (uint16_t)((uint16_t)xfer_type << 11);
    if (in) devinfo |= 0x8000;
    uint16_t ttport = rt.tt_mtt ? rt.tt_port : 1;
    // called from the hid poll (the gui's Mouse::Poll among others): a dead hub
    // must not freeze the desktop for 5 s per request. (satoru)
    uint32_t saved_to = g_ctl_timeout_ms;
    g_ctl_timeout_ms = 250;
    ControlTransferEP(hub, 0x23, USB_HUB_REQ_CLEAR_TT, devinfo, ttport, 0, nullptr);
    g_ctl_timeout_ms = saved_to;
    in_clear = false;
}

// set PORT_RESET, wait for the hub to finish, clear the change bits and report
// the negotiated speed. usb 2 waits for reset to drop with the connection
// still up; usb 3 waits for link state U0. (satoru)
bool USB::HubResetPort(int idx, uint8_t port, uint8_t* out_speed) {
    USBDeviceRuntime& rt = runtime[idx];
    if (out_speed) *out_speed = 0;
    if (!HubSetPortFeature(idx, port, USB_HUB_FEAT_PORT_RESET)) return false;

    uint16_t st = 0, ch = 0;
    bool done = false;
    uint64_t t0 = Scheduler::NowMs();
    for (int it = 0; it < 100 && Scheduler::NowMs() - t0 < 1000; it++) {
        usb_delay_ms(10);
        if (!HubGetPortStatus(idx, port, &st, &ch)) return false;
        if (rt.hub_ss) {
            uint16_t ls = (uint16_t)(st & USB_HUB_PORT_LINK_STATE);
            if ((ls == USB_HUB_SS_LS_U0) && (st & USB_HUB_PORT_ENABLE)) { done = true; break; }
        } else {
            if (!(st & USB_HUB_PORT_RESET) && (st & USB_HUB_PORT_CONNECTION)) { done = true; break; }
        }
    }
    if (!done) {
        usb_log("USB: hub %d port %d reset timeout (status %x change %x)\n",
                (int)rt.slot, (int)port, (uint32_t)st, (uint32_t)ch);
        return false;
    }
    if (!(st & USB_HUB_PORT_ENABLE)) {
        usb_delay_ms(20);
        HubGetPortStatus(idx, port, &st, &ch);
    }
    if (!(st & USB_HUB_PORT_ENABLE)) {
        usb_log("USB: hub %d port %d is not enabled after reset (status %x)\n",
                (int)rt.slot, (int)port, (uint32_t)st);
        return false;
    }

    // consume the change bits this reset produced. only clear the ones the
    // hub actually reports, so a hub that does not implement a selector is
    // never sent an unsupported clear request. (satoru)
    if (ch & USB_HUB_PORT_C_RESET || !rt.hub_ss)
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_RESET);
    if (rt.hub_ss) {
        if (ch & USB_HUB_PORT_C_BH_RESET)
            HubClearPortFeature(idx, port, USB_HUB_FEAT_C_BH_PORT_RESET);
        if (ch & USB_HUB_PORT_C_LINK_STATE)
            HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_LINK_STATE);
        if (ch & USB_HUB_PORT_C_CONFIG_ERROR)
            HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_CONFIG_ERROR);
    }
    if (ch & USB_HUB_PORT_C_CONNECTION || !rt.hub_ss)
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_CONNECTION);
    if (ch & USB_HUB_PORT_C_ENABLE || !rt.hub_ss)
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_ENABLE);

    // TRSTRCY is 10 ms; linux adds 40 ms before addressing the device. (satoru)
    usb_delay_ms(50);

    uint8_t speed;
    if (rt.hub_ss) speed = USB_SPEED_SUPER;
    else if (st & USB_HUB_PORT_HIGH_SPEED) speed = USB_SPEED_HIGH;
    else if (st & USB_HUB_PORT_LOW_SPEED)  speed = USB_SPEED_LOW;
    else                                   speed = USB_SPEED_FULL;
    if (out_speed) *out_speed = speed;
    return true;
}

bool USB::HubPortBringup(int idx, uint8_t port, uint16_t status) {
    USBDeviceRuntime& rt = runtime[idx];
    (void)status;
    if (rt.depth >= USB_MAX_HUB_DEPTH) {
        usb_log("USB: hub %d port %d not walked (max depth %d)\n",
                (int)rt.slot, (int)port, (int)USB_MAX_HUB_DEPTH);
        return false;
    }
    uint8_t speed = 0;
    if (!HubResetPort(idx, port, &speed)) return false;
    uint32_t child_route = rt.route |
        ((uint32_t)port << (4 * (rt.depth ? (rt.depth - 1) : 0)));
    bool ok = EnumerateDeviceAt(rt.port, child_route, (uint8_t)(rt.depth + 1),
                                (uint8_t)idx, port, speed);
    if (!ok) {
        usb_log("USB: hub %d port %d reset ok but enumeration failed\n", (int)rt.slot, (int)port);
        HubClearPortFeature(idx, port, USB_HUB_FEAT_PORT_ENABLE);
    }
    return ok;
}

void USB::HubPortChanged(int idx, uint8_t port) {
    USBDeviceRuntime& rt = runtime[idx];
    uint16_t st = 0, ch = 0;
    if (!HubGetPortStatus(idx, port, &st, &ch)) return;

    bool connected = (st & USB_HUB_PORT_CONNECTION) != 0;
    int child = FindHubChild(idx, port);

    if (!connected) {
        if (child >= 0) {
            char path[32];
            usb_rt_path(runtime, child, path, sizeof(path));
            usb_log("USB: hub port %s disconnected\n", path);
            TeardownDevice(child);
        }
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_CONNECTION);
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_ENABLE);
        if (ch & USB_HUB_PORT_C_RESET || !rt.hub_ss)
            HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_RESET);
        if (rt.hub_ss && (ch & USB_HUB_PORT_C_LINK_STATE))
            HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_LINK_STATE);
        return;
    }

    if (child >= 0) {
        // already enumerated: consume the change bits. a real connect change
        // (fast replug) or a port that lost enable means the old device is
        // gone, so tear its subtree down and address the port again. (satoru)
        bool redo = (ch & USB_HUB_PORT_C_CONNECTION) ||
                    ((ch & USB_HUB_PORT_C_ENABLE) && !(st & USB_HUB_PORT_ENABLE));
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_CONNECTION);
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_ENABLE);
        if (ch & USB_HUB_PORT_C_RESET || !rt.hub_ss)
            HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_RESET);
        HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_OVER_CURRENT);
        if (rt.hub_ss) {
            if (ch & USB_HUB_PORT_C_LINK_STATE)
                HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_LINK_STATE);
            if (ch & USB_HUB_PORT_C_CONFIG_ERROR)
                HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_CONFIG_ERROR);
        }
        if (redo) {
            usb_log("USB: hub %d port %d re-connected, re-enumerating\n", (int)rt.slot, (int)port);
            TeardownDevice(child);
            usb_delay_ms(20);
            HubPortBringup(idx, port, st);
        }
        return;
    }

    // a new device appeared. debounce briefly, then reset and address it. (satoru)
    HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_CONNECTION);
    HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_ENABLE);
    if (ch & USB_HUB_PORT_C_OVER_CURRENT) HubClearPortFeature(idx, port, USB_HUB_FEAT_C_PORT_OVER_CURRENT);
    usb_delay_ms(20);
    if (!HubGetPortStatus(idx, port, &st, &ch) || !(st & USB_HUB_PORT_CONNECTION)) return;

    char path[32];
    int plen = usb_rt_path(runtime, idx, path, sizeof(path));
    if (plen < (int)sizeof(path) - 4) {
        path[plen++] = '.';
        if (port >= 10) path[plen++] = (char)('0' + port / 10);
        path[plen++] = (char)('0' + port % 10);
        path[plen] = 0;
    }
    usb_log("USB: hub port %s connected, resetting\n", path);
    HubPortBringup(idx, port, st);
}

void USB::DispatchHubReport(int idx, int epi, const uint8_t* report, int len) {
    USBDeviceRuntime& rt = runtime[idx];
    (void)epi;
    rt.hub_dirty = true;
    if (!report || len < 1) return;
    // bit 0 is the hub itself; bit n is port n. (satoru)
    if (report[0] & 0x01) rt.hub_hub_status = true;
    for (uint8_t p = 1; p <= rt.hub_ports && p < 32; p++) {
        int byte = p >> 3;
        if (byte < len && ((report[byte] >> (p & 7)) & 1))
            rt.hub_pending |= (uint32_t)1u << p;
    }
}

void USB::HubServiceOne(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.in_use || !rt.is_hub) return;
    rt.hub_dirty = false;

    if (rt.hub_hub_status) {
        rt.hub_hub_status = false;
        uint8_t st[4];
        usb_memset(st, 0, sizeof(st));
        if (ControlTransferEP(idx, 0xA0, 0x00, 0, 0, 4, st)) {
            uint16_t change = usb_le16(st + 2);
            if (change & 0x0001) ControlTransferEP(idx, 0x20, 0x01, USB_HUB_FEAT_C_HUB_LOCAL_POWER, 0, 0, nullptr);
            if (change & 0x0002) ControlTransferEP(idx, 0x20, 0x01, USB_HUB_FEAT_C_HUB_OVERCURRENT, 0, 0, nullptr);
            usb_log("USB: hub %d status change %x\n", (int)rt.slot, (uint32_t)change);
        }
    }

    uint32_t pending = rt.hub_pending;
    rt.hub_pending = 0;
    for (uint8_t p = 1; p <= rt.hub_ports && p < 32; p++)
        if (pending & ((uint32_t)1u << p)) HubPortChanged(idx, p);
}

// service hub status changes after the event ring drain. never runs inside a
// control-transfer wait: syncing ports there could eat the outer completion. (satoru)
void USB::ServiceHubs() {
    uint32_t now = Scheduler::NowMs();
    for (int pass = 0; pass < 3; pass++) {
        bool any = false;
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            USBDeviceRuntime& rt = runtime[i];
            if (!rt.in_use || !rt.is_hub) continue;
            if (rt.hub_dirty) {
                any = true;
                HubServiceOne(i);
            } else if (pass == 0 && (int32_t)(now - rt.hub_next_poll) >= 0) {
                // fallback scan: an interrupt can be lost (or a hub may not
                // implement the status endpoint well), and a missed connect
                // would leave the port dead forever. (satoru)
                rt.hub_next_poll = now + 1000;
                for (uint8_t p = 1; p <= rt.hub_ports && p < 32; p++) {
                    uint16_t st = 0, ch = 0;
                    if (!HubGetPortStatus(i, p, &st, &ch)) continue;
                    int child = FindHubChild(i, p);
                    bool conn = (st & USB_HUB_PORT_CONNECTION) != 0;
                    if (ch || (conn && child < 0) || (!conn && child >= 0)) {
                        HubPortChanged(i, p);
                        any = true;
                    }
                }
            }
        }
        if (!any) break;
    }
}

// power the hub ports on and enumerate what is already plugged in. ports are
// always powered on a superspeed hub; usb 2 hubs follow the logical power
// switching mode and wait bPwrOn2PwrGood. (satoru)
bool USB::HubStart(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.is_hub) return false;

    char hpath[32];
    usb_rt_path(runtime, idx, hpath, sizeof(hpath));
    usb_log("USB: HUB found on ctl %d port %s (vid %x pid %x): %d ports, %s%s, power-on %d ms\n",
            (int)(S - g_xs), hpath, (uint32_t)devices[idx].vendor_id,
            (uint32_t)devices[idx].product_id, (int)rt.hub_ports,
            rt.hub_ss ? "SuperSpeed" : "USB 2.0",
            rt.hub_ss ? "" : (rt.hub_multi_tt ? " multi-TT" : (rt.speed == USB_SPEED_HIGH ? " single-TT" : "")),
            (int)rt.hub_pwr_good * 2);

    if (!rt.hub_ss) {
        // every port gets its own PORT_POWER, ganged hubs included, as linux
        // hub_power_on does. ganged only means the supply is shared: a port
        // that never saw the request stays in the powered-off state and never
        // reports a connection. powering port 1 alone left ports 2-4 of the
        // laptop's hub (1a40:0101, ganged) dead. (satoru)
        for (uint8_t p = 1; p <= rt.hub_ports; p++)
            HubSetPortFeature(idx, p, USB_HUB_FEAT_PORT_POWER);
        uint32_t delay = (uint32_t)rt.hub_pwr_good * 2;
        if (delay < 100) delay = 100;            // power stable time for an external hub (satoru)
        usb_delay_ms(delay);
    } else {
        usb_delay_ms(100);
    }
    usb_log("USB: hub %d ports powered, scanning\n", (int)rt.slot);

    for (uint8_t p = 1; p <= rt.hub_ports && p < 32; p++) {
        if (p > USB_HUB_MAX_ROUTE_PORTS) break;  // route string limit (satoru)
        uint16_t st = 0, ch = 0;
        if (!HubGetPortStatus(idx, p, &st, &ch)) {
            usb_log("USB: hub %d port %d status read FAILED\n", (int)rt.slot, (int)p);
            continue;
        }
        if (ch) HubPortChanged(idx, p);
        else if (st & USB_HUB_PORT_CONNECTION) HubPortBringup(idx, p, st);
    }
    rt.hub_next_poll = Scheduler::NowMs() + 1000;
    return true;
}

// a port status change event seen while a control/bulk wait owned the event
// ring is latched here and serviced after the wait. dropping it used to lose a
// hot plug that happened during a hub child enumeration. (satoru)
void USB::NotePortStatus(const xHCI_TRB& ev) {
    uint8_t port_id = (uint8_t)((ev.parameter >> 24) & 0xFF);
    if (port_id >= 1 && port_id <= 32)
        S->root_pending |= (uint32_t)1u << (port_id - 1);
}

void USB::ServiceRootPorts() {
    uint32_t pending = S->root_pending;
    S->root_pending = 0;
    for (int port = 0; port < (int)max_ports && port < 32; port++) {
        if (!(pending & ((uint32_t)1u << port))) continue;
        uint32_t portsc = ReadPort(port);
        WritePort(port, (portsc & 0x0E00C3E0) | (portsc & 0x00FE0000));
        bool connected = (portsc & XHCI_PORTSC_CCS) != 0;
        int existing = FindRootDeviceByPort((uint8_t)port);
        if (connected && existing < 0) {
            EnumeratePort(port);
        } else if (!connected && existing >= 0) {
            TeardownDevice(existing);
        }
    }
}

// drain the event ring once per call: re-arm interrupt endpoints whose
// transfer completed (dispatching their reports) and (re)enumerate / tear down
// ports on Port Status Change events. designed to be called at ~1 khz from the
// input poll loop (satoru).
void USB::PollHID() {
    // controller 0 exactly as before, then any extra controller (satoru)
    XhciState* saved = S;
    for (int c = 0; c < g_nctl; c++) {
        S = &g_xs[c];
        PollOne(c);
    }
    S = saved ? saved : &g_xs[0];
    // usb audio housekeeping (recovery, hotplug backend switch) runs here, in the
    // input process, never under the audio pump lock (satoru)
    if (g_audio_service_fn && !g_usb_sync_claim) g_audio_service_fn();
}

void USB::PollOne(int ctl) {
    if (!detected) return;

    // a synchronous bulk/bot wait owns the shared event ring right now: stay
    // out so we do not swallow its completion event. it re-arms + dispatches
    // hid reports it drains itself, so input stays alive. (satoru)
    if (g_usb_sync_claim) return;

    // bounded drain so a misbehaving controller cannot wedge the input loop
    // (satoru).
    for (int guard = 0; guard < USB_RING_TRBS; guard++) {
        xHCI_TRB* evt = &event_ring[event_ring_idx];
        bool cycle = (evt->control & TRB_CYCLE) != 0;
        if (cycle != event_ccs) break;   // ring empty (satoru)

        xHCI_TRB ev = *evt;

        event_ring_idx++;
        if (event_ring_idx >= USB_RING_TRBS) {
            event_ring_idx = 0;
            event_ccs = !event_ccs;
        }

        uint8_t type = (ev.control >> 10) & 0x3F;

        if (type == TRB_TRANSFER_EVT) {
            uint8_t slot = (ev.control >> 24) & 0xFF;
            uint8_t ep_id = (ev.control >> 16) & 0x1F;
            int idx = FindDeviceBySlot(slot);
            if (idx < 0) continue;
            int iep = FindIntrEp(idx, ep_id);
            if (iep >= 0) {
                HandleIntrCompletion(idx, iep, (uint8_t)((ev.status >> 24) & 0xFF),
                                     ev.status & 0x00FFFFFF);
            } else if (runtime[idx].iso_dci && runtime[idx].iso_dci == ep_id) {
                // isoch audio transfer event: hand it to the audio driver (satoru)
                if (g_iso_event_fn)
                    g_iso_event_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, ev.parameter,
                                   (uint8_t)((ev.status >> 24) & 0xFF), ev.status & 0x00FFFFFF);
            } else if (runtime[idx].iso_fb_dci && runtime[idx].iso_fb_dci == ep_id) {
                // feedback endpoint (async sink): same callback, the driver
                // tells the two rings apart by the trb pointer. (satoru)
                if (g_iso_event_fn)
                    g_iso_event_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, ev.parameter,
                                   (uint8_t)((ev.status >> 24) & 0xFF), ev.status & 0x00FFFFFF);
            }
        } else if (type == TRB_PORT_STATUS) {
            // parameter[31:24] holds the 1-based port id (satoru).
            uint8_t port_id = (uint8_t)((ev.parameter >> 24) & 0xFF);
            int port = (int)port_id - 1;
            if (port >= 0 && port < (int)max_ports) {
                // acknowledge connect/enable change bits by writing them back
                // (RW1CS) while preserving the rw bits (satoru).
                uint32_t portsc = ReadPort(port);
                WritePort(port, (portsc & 0x0E00C3E0) | (portsc & 0x00FE0000));

                bool connected = (portsc & XHCI_PORTSC_CCS) != 0;
                int existing = FindRootDeviceByPort((uint8_t)port);
                if (connected && existing < 0) {
                    EnumeratePort(port);            // hot-plug attach (satoru)
                } else if (!connected && existing >= 0) {
                    TeardownDevice(existing);       // hot-unplug detach, hub subtree follows (satoru)
                }
            }
        }
        // command-completion events are consumed by SubmitCommand's own poll;
        // any that land here are stale and safely ignored (satoru).
    }

    // tell the controller how far we have consumed (satoru).
    UpdateERDP();

    // recover halted interrupt endpoints: Reset Endpoint (trb 14), then Set TR
    // Dequeue Pointer (trb 16) to our current enqueue slot with its cycle bit,
    // dropping the failed td, then queue a fresh transfer. a device that keeps
    // failing is given up on after 50 tries. (satoru)
    for (int idx = 0; idx < USB_MAX_DEVICES; idx++) {
        uint8_t mask = g_halt_mask[ctl][idx];
        if (!mask) continue;
        USBDeviceRuntime& rt = runtime[idx];
        if (!rt.in_use) { g_halt_mask[ctl][idx] = 0; continue; }
        for (int e = 0; e < rt.n_intr && e < USB_MAX_INTR_EPS; e++) {
            if (!(mask & (1u << e))) continue;
            g_halt_mask[ctl][idx] &= (uint8_t)~(1u << e);
            UsbIntrEp& ep = rt.intr[e];
            if (!ep.used || !ep.ring) continue;
            if (g_intr_resets[ctl][idx][e] >= 50) continue;
            g_intr_resets[ctl][idx][e]++;
            HubClearTt(idx, ep.ep_addr, 3);   // interrupt split state on the parent tt (satoru)
            xHCI_TRB c = {};
            c.control = TRB_TYPE(14) | ((uint32_t)rt.slot << 24) | ((uint32_t)ep.ep_id << 16);
            xHCI_TRB r = {};
            bool ok1 = SubmitCommand(&c, &r) && ((r.status >> 24) & 0xFF) == TRB_CC_SUCCESS;
            xHCI_TRB d = {};
            d.parameter = KernelPhysOf(&ep.ring[ep.idx]) | (ep.cycle ? 1u : 0u);
            d.control = TRB_TYPE(16) | ((uint32_t)rt.slot << 24) | ((uint32_t)ep.ep_id << 16);
            bool ok2 = SubmitCommand(&d, &r) && ((r.status >> 24) & 0xFF) == TRB_CC_SUCCESS;
            if (g_intr_resets[ctl][idx][e] <= 4)
                usb_log("USB: slot %d ep %d reset %s, dequeue %s\n",
                        (int)rt.slot, (int)ep.ep_id, ok1 ? "ok" : "FAILED", ok2 ? "ok" : "FAILED");
            ArmIntrEp(idx, e);
        }
    }

    // hub port changes and the periodic hub scan run outside the drain so a
    // control transfer cannot consume this loop's next event. children are
    // enumerated here, after every hid endpoint has been re-armed. (satoru)
    ServiceHubs();
    // root port changes that arrived during those (nested) control transfers (satoru)
    ServiceRootPorts();
}

const USBDeviceInfo* USB::GetDevice(int index) {
    if (index < 0 || index >= device_count) return nullptr;
    return &devices[index];
}

// public control transfer: map the external device index to its runtime slot
// and run the transfer on ep0 (satoru).
bool USB::ControlTransfer(int device, uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void* data) {
    if (device < 0 || device >= USB_MAX_DEVICES) return false;
    if (!runtime[device].in_use) return false;
    return ControlTransferEP(device, bmRequestType, bRequest, wValue, wIndex, wLength, data);
}

bool USB::BulkRead(int device, uint8_t endpoint, void* buffer, int length,
                   int* out_transferred) {
    if (device < 0 || device >= USB_MAX_DEVICES) return false;
    USBDeviceRuntime& rt = runtime[device];
    // real xhci bulk-in through the device's configured bulk endpoint (satoru)
    if (!rt.in_use || !rt.bulk_in_ring || endpoint != rt.bulk_in_ep) return false;
    return BulkTransferEP(device, true, buffer, length, out_transferred);
}

bool USB::BulkWrite(int device, uint8_t endpoint, const void* buffer, int length) {
    if (device < 0 || device >= USB_MAX_DEVICES) return false;
    USBDeviceRuntime& rt = runtime[device];
    // real xhci bulk-out through the device's configured bulk endpoint (satoru)
    if (!rt.in_use || !rt.bulk_out_ring || endpoint != rt.bulk_out_ep) return false;
    int got = 0;
    if (!BulkTransferEP(device, false, (void*)buffer, length, &got)) return false;
    return got == length;
}

void USB::DumpInfo(char* out, int max_len) {
    if (!detected) {
        int n = 0;
        const char* msg = "USB: No xHCI controller detected\n";
        while (msg[n] && n < max_len - 1) { out[n] = msg[n]; n++; }
        out[n] = 0;
        return;
    }

    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < max_len - 1) out[pos++] = *s++;
    };
    auto append_num = [&](uint32_t val) {
        char buf[12];
        int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else {
            uint32_t tmp = val;
            char rev[12];
            int ri = 0;
            while (tmp) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            while (ri--) buf[i++] = rev[ri];
        }
        buf[i] = 0;
        append(buf);
    };
    auto append_hex = [&](uint32_t val, int digits) {
        const char* hex = "0123456789abcdef";
        for (int i = digits - 1; i >= 0; i--)
            if (pos < max_len - 1) out[pos++] = hex[(val >> (i * 4)) & 0xF];
    };

    append("USB xHCI Controller\n");
    append("  Ports: "); append_num(max_ports); append("\n");
    append("  Max Slots: "); append_num(max_slots); append("\n");
    append("  Devices: "); append_num(device_count); append("\n");

    for (int i = 0; i < device_count; i++) {
        const USBDeviceInfo& d = devices[i];
        if (!d.connected) continue; // skip torn-down / gap slots (satoru)
        char dpath[32];
        usb_rt_path(runtime, i, dpath, sizeof(dpath));
        append("  Device "); append_num(i);
        append(": Port "); append(dpath);
        append(", Speed="); append(SpeedName(d.speed));
        append(", VID:PID="); append_hex(d.vendor_id, 4);
        append(":"); append_hex(d.product_id, 4);
        append(" ["); append(d.product); append("]\n");
    }

    for (int p = 0; p < (int)max_ports; p++) {
        append("  Port "); append_num(p); append(": ");
        if (IsPortConnected(p)) {
            append("Connected, Speed=");
            append(SpeedName(GetPortSpeed(p)));
        } else {
            append("Empty");
        }
        // raw portsc: power (bit 9), link state (bits 8:5), enabled (bit 1).
        // tells "nothing plugged in" from "port unpowered / link down". (satoru)
        append(" portsc=0x"); append_hex(ReadPort(p), 8);
        append("\n");
    }

    // extra controllers (e.g. the cpu's type-c xhci), same layout, own header (satoru)
    XhciState* saved = S;
    for (int c = 1; c < g_nctl; c++) {
        S = &g_xs[c];
        append("USB xHCI Controller #"); append_num((uint32_t)c);
        append(" at "); append_num(S->pci_bus); append(":"); append_num(S->pci_dev);
        append("."); append_num(S->pci_func); append("\n");
        append("  Ports: "); append_num(max_ports); append("\n");
        append("  Max Slots: "); append_num(max_slots); append("\n");
        append("  Devices: "); append_num(device_count); append("\n");
        for (int i = 0; i < device_count; i++) {
            const USBDeviceInfo& d = devices[i];
            if (!d.connected) continue;
            char dpath[32];
            usb_rt_path(runtime, i, dpath, sizeof(dpath));
            append("  Device "); append_num(i);
            append(": Port "); append(dpath);
            append(", Speed="); append(SpeedName(d.speed));
            append(", VID:PID="); append_hex(d.vendor_id, 4);
            append(":"); append_hex(d.product_id, 4);
            append(" ["); append(d.product); append("]\n");
        }
        for (int p = 0; p < (int)max_ports; p++) {
            append("  Port "); append_num(p); append(": ");
            if (IsPortConnected(p)) { append("Connected, Speed="); append(SpeedName(GetPortSpeed(p))); }
            else append("Empty");
            append(" portsc=0x"); append_hex(ReadPort(p), 8);
            append("\n");
        }
    }
    S = saved;

    out[pos] = 0;
}

// ---- extra controllers, kbdiag, and the isoch api for usb audio (satoru) ----

// put a pci function into D0 if it sits in a lower power state (pm capability
// pmcsr bits 1:0). the cpu's type-c xhci may be powered down by firmware; linux
// xhci-pci does this through the pci core before probing. what we cannot know
// without the laptop: whether tcss also needs an acpi _PS0 / pmc mux request. (satoru)
static void usb_pci_to_d0(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8);
    outl(0xCF8, addr | 0x04);
    uint32_t sc = inl(0xCFC);
    if (!((sc >> 16) & 0x10)) { usb_log("USB: %d:%d.%d has no pci capability list\n", (int)bus, (int)dev, (int)func); return; }
    outl(0xCF8, addr | 0x34);
    uint32_t ptr = inl(0xCFC) & 0xFC;
    bool found = false;
    for (int guard = 0; guard < 48 && ptr >= 0x40; guard++) {
        outl(0xCF8, addr | ptr);
        uint32_t v = inl(0xCFC);
        if ((v & 0xFF) == 0x01) {
            outl(0xCF8, addr | (ptr + 4));
            uint32_t pm = inl(0xCFC);
            usb_log("USB: %d:%d.%d pm cap at %x pmcsr=%x power state D%d\n",
                    (int)bus, (int)dev, (int)func, ptr, pm, (int)(pm & 3));
            if (pm & 3) {
                outl(0xCF8, addr | (ptr + 4));
                outl(0xCFC, pm & ~3u);
                uint64_t t0 = Scheduler::NowMs();
                for (uint64_t i = 0; i < 20000000ull && Scheduler::NowMs() - t0 < 12; i++) __asm__ __volatile__("pause");
                outl(0xCF8, addr | (ptr + 4));
                usb_log("USB: %d:%d.%d moved to D0, pmcsr now %x\n", (int)bus, (int)dev, (int)func, inl(0xCFC));
            }
            found = true;
            break;
        }
        ptr = (v >> 8) & 0xFC;
    }
    if (!found) usb_log("USB: %d:%d.%d no pm capability found\n", (int)bus, (int)dev, (int)func);
}

void USB::ProbeSecondaries() {
    for (int i = 0; i < g_ncands; i++) {
        XhciCand& c = g_cands[i];
        if (c.driven) continue;
        if (g_nctl >= USB_MAX_CTL) { usb_log("USB: xHCI %d:%d.%d not driven (controller table full)\n", (int)c.bus, (int)c.dev, (int)c.func); continue; }
        usb_pci_to_d0(c.bus, c.dev, c.func);
        // re-read the port count now the function is awake (the scan read it before) (satoru)
        uint64_t phys = ((uint64_t)c.bar_hi << 32) | c.bar_lo;
        uint32_t ports = 0;
        if (phys) {
            uint32_t hcs1 = *(volatile uint32_t*)(uintptr_t)(phys + 0x04);
            if (hcs1 != 0xFFFFFFFFu) ports = (hcs1 >> 24) & 0xFF;
        }
        if (ports == 0) {
            usb_log("USB: xHCI %d:%d.%d NOT driven: registers read back all ones or 0 ports (bar %x:%x) - powered down?\n",
                    (int)c.bus, (int)c.dev, (int)c.func, c.bar_hi, c.bar_lo);
            continue;
        }
        c.driven = true;
        bool ok = BringUp(c.bus, c.dev, c.func, c.bar_lo, c.bar_hi, ports, true);
        usb_log("USB: extra xHCI %d:%d.%d bring-up %s\n", (int)c.bus, (int)c.dev, (int)c.func, ok ? "ok" : "FAILED");
    }
    S = &g_xs[0];
}

int USB::ControllerCount() { return g_nctl; }

bool USB::GidValid(int gid) {
    if (gid < 0) return false;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return false;
    return g_xs[c].x_detected && g_xs[c].x_runtime[i].in_use;
}
const USBDeviceInfo* USB::GidInfo(int gid) {
    return GidValid(gid) ? &g_xs[gid / USB_MAX_DEVICES].x_devices[gid % USB_MAX_DEVICES] : nullptr;
}
const USBDeviceRuntime* USB::GidRuntime(int gid) {
    return GidValid(gid) ? &g_xs[gid / USB_MAX_DEVICES].x_runtime[gid % USB_MAX_DEVICES] : nullptr;
}

// reset endpoint then set tr dequeue to the producer position (xhci 4.6.8 / 4.6.10) (satoru)
bool USB::RecoverEp(int idx, uint8_t dci, xHCI_TRB* ring, int ring_idx, bool cycle) {
    USBDeviceRuntime& rt = runtime[idx];
    xHCI_TRB c = {};
    c.control = TRB_TYPE(14) | ((uint32_t)rt.slot << 24) | ((uint32_t)dci << 16);
    xHCI_TRB r = {};
    bool ok1 = SubmitCommand(&c, &r);
    uint8_t cc1 = (uint8_t)((r.status >> 24) & 0xFF);
    xHCI_TRB d = {};
    d.parameter = KernelPhysOf(&ring[ring_idx]) | (cycle ? 1u : 0u);
    d.control = TRB_TYPE(16) | ((uint32_t)rt.slot << 24) | ((uint32_t)dci << 16);
    xHCI_TRB r2 = {};
    bool ok2 = SubmitCommand(&d, &r2);
    uint8_t cc2 = (uint8_t)((r2.status >> 24) & 0xFF);
    usb_log("USB: slot %d dci %d recover: reset cc %d, set-deq cc %d\n", (int)rt.slot, (int)dci, (int)cc1, (int)cc2);
    return ok1 && ok2 && cc2 == TRB_CC_SUCCESS;
}

// an event for an endpoint nobody was waiting on, seen inside an audio-driven
// control transfer / command: finish what PollHID would have done (satoru)
void USB::ForeignEvent(const xHCI_TRB& ev) {
    uint8_t slot = (ev.control >> 24) & 0xFF;
    uint8_t ep_id = (ev.control >> 16) & 0x1F;
    int idx = FindDeviceBySlot(slot);
    if (idx < 0) return;
    USBDeviceRuntime& rt = runtime[idx];
    uint8_t cc = (ev.status >> 24) & 0xFF;
    int iep = FindIntrEp(idx, ep_id);
    if (iep >= 0) {
        HandleIntrCompletion(idx, iep, cc, ev.status & 0x00FFFFFF);
        return;
    }
    if (rt.iso_dci && rt.iso_dci == ep_id) {
        if (g_iso_event_fn)
            g_iso_event_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, ev.parameter, cc, ev.status & 0x00FFFFFF);
    } else if (rt.iso_fb_dci && rt.iso_fb_dci == ep_id) {
        if (g_iso_event_fn)
            g_iso_event_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, ev.parameter, cc, ev.status & 0x00FFFFFF);
    }
}

bool USB::GidControl(int gid, uint8_t bmRequestType, uint8_t bRequest,
                     uint16_t wValue, uint16_t wIndex, uint16_t wLength, void* data) {
    if (!GidValid(gid)) return false;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    bool ok = ControlTransferEP(idx, bmRequestType, bRequest, wValue, wIndex, wLength, data);
    if (!ok) {
        // a stalled request halts ep0; without this the next request would hang too (satoru)
        USBDeviceRuntime& rt = runtime[idx];
        RecoverEp(idx, 1, rt.ep0_ring, rt.ep0_idx, rt.ep0_cycle);
    }
    g_dispatch_foreign = svf; S = sv;
    return ok;
}

bool USB::GidGetConfig(int gid, int index, uint8_t* buf, int cap, int* out_len) {
    if (!GidValid(gid) || !buf || cap < 9) return false;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    bool ok = false;
    int total = 0;
    if (ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)((USB_DESC_CONFIG << 8) | (index & 0xFF)), 0, 9, buf)) {
        total = buf[2] | (buf[3] << 8);
        if (total > cap) total = cap;
        if (total >= 9) {
            ok = ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)((USB_DESC_CONFIG << 8) | (index & 0xFF)),
                                   0, (uint16_t)total, buf);
        }
    }
    g_dispatch_foreign = svf; S = sv;
    if (ok && out_len) *out_len = total;
    return ok;
}

bool USB::GidSetConfig(int gid, uint8_t config_value) {
    if (!GidValid(gid)) return false;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    USBDeviceRuntime& rt = runtime[idx];
    bool ok = true;
    if (rt.cfg_value != config_value) {
        if (rt.cfg_value != 0) {
            // leaving a configuration the hid code already set up: deconfigure the
            // slot's endpoints first (configure endpoint with DC), and forget the
            // interrupt/bulk endpoints that no longer exist (satoru)
            xHCI_TRB cmd = {};
            cmd.control = TRB_TYPE(TRB_CONFIG_EP) | (1u << 9) | ((uint32_t)rt.slot << 24);
            xHCI_TRB res = {};
            SubmitCommand(&cmd, &res);
            usb_log("USB: slot %d deconfigure cc %d (config %d -> %d)\n", (int)rt.slot,
                    (int)((res.status >> 24) & 0xFF), (int)rt.cfg_value, (int)config_value);
            bool had_kbd = false;
            for (int e = 0; e < rt.n_intr; e++) {
                if (rt.intr[e].role == USB_HID_KEYBOARD) had_kbd = true;
                if (rt.intr[e].ring) KernelHeap::Free(rt.intr[e].ring);
                if (rt.intr[e].buf)  KernelHeap::Free(rt.intr[e].buf);
            }
            if (had_kbd) Keyboard::RemoveUSBDevice((uint8_t)((int)(S - g_xs) * USB_MAX_DEVICES + idx));
            usb_memset(rt.intr, 0, sizeof(rt.intr));
            rt.n_intr = 0;
            rt.hid_type = USB_HID_NONE;
            g_halt_mask[(int)(S - g_xs)][idx] = 0;
            rt.iso_dci = 0;
        }
        ok = ControlTransferEP(idx, 0x00, USB_REQ_SET_CONFIG, config_value, 0, 0, nullptr);
        if (ok) rt.cfg_value = config_value;
        else RecoverEp(idx, 1, rt.ep0_ring, rt.ep0_idx, rt.ep0_cycle);
    }
    g_dispatch_foreign = svf; S = sv;
    return ok;
}

bool USB::IsoOpen(int gid, const UsbIsoParams& p, uint32_t* out_period_us) {
    if (!GidValid(gid)) return false;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    USBDeviceRuntime& rt = runtime[idx];
    bool ok = false;
    do {
        if (rt.iso_dci) break;                                   // already open (satoru)
        bool in = (p.ep_addr & 0x80) != 0;
        uint8_t dci = (uint8_t)(((p.ep_addr & 0x0F) * 2) + (in ? 1 : 0));
        if (dci < 2 || dci > 31) break;
        xHCI_TRB* ring = (xHCI_TRB*)PMM::AllocBytes(4096);
        if (!ring) break;
        usb_memset(ring, 0, 4096);
        ring[USB_ISO_RING_TRBS - 1].parameter = KernelPhysOf(ring);
        ring[USB_ISO_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
        // service interval as linux xhci_parse_exponent_interval (+3 on full speed:
        // bInterval counts 1 ms frames there, the field counts 125 us units) (satoru)
        uint32_t bi = p.b_interval; if (bi < 1) bi = 1; if (bi > 16) bi = 16;
        uint32_t xint = bi - 1;
        if (rt.speed == USB_SPEED_FULL) xint += 3;
        if (xint > 15) xint = 15;
        uint32_t period_us = 125u << xint;
        uint32_t esit = p.bytes_per_interval ? p.bytes_per_interval : (uint32_t)p.max_packet * (p.burst + 1u);
        uint32_t mult = (rt.speed == USB_SPEED_SUPER) ? (p.mult & 3u) : 0;
        uint32_t burst = (rt.speed == USB_SPEED_FULL) ? 0 : p.burst;
        uint32_t avg = p.avg_bytes ? p.avg_bytes : esit;

        // explicit feedback endpoint: second isoch endpoint, always IN. its
        // ring is kept across a close so a completion racing the teardown
        // cannot touch freed memory; the device teardown frees it. (satoru)
        bool want_fb = p.fb_ep_addr && (p.fb_ep_addr & 0x80) && p.fb_max_packet;
        uint8_t fb_dci = 0;
        xHCI_TRB* fb_ring = nullptr;
        uint32_t fb_xint = 0;
        if (want_fb) {
            fb_dci = (uint8_t)(((p.fb_ep_addr & 0x0F) * 2) + 1);
            if (fb_dci < 2 || fb_dci > 31 || fb_dci == dci) { want_fb = false; fb_dci = 0; }
        }
        if (want_fb) {
            fb_ring = rt.iso_fb_ring;
            if (!fb_ring) fb_ring = (xHCI_TRB*)PMM::AllocBytes(4096);
            if (!fb_ring) { want_fb = false; fb_dci = 0; }
            else {
                usb_memset(fb_ring, 0, 4096);
                fb_ring[USB_ISO_RING_TRBS - 1].parameter = KernelPhysOf(fb_ring);
                fb_ring[USB_ISO_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
                uint32_t fbi = p.fb_b_interval; if (fbi < 1) fbi = 1; if (fbi > 16) fbi = 16;
                fb_xint = fbi - 1;
                if (rt.speed == USB_SPEED_FULL) fb_xint += 3;
                if (fb_xint > 15) fb_xint = 15;
            }
        }

        uint32_t* icc = InputControlCtx(idx);
        uint32_t* slot_ctx = SlotCtx(idx);
        uint32_t ents = (slot_ctx[0] >> 27) & 0x1F;
        if (ents < dci) ents = dci;
        if (want_fb && ents < fb_dci) ents = fb_dci;
        slot_ctx[0] = (slot_ctx[0] & ~((uint32_t)0x1F << 27)) | (ents << 27);

        // fill the data endpoint context (satoru)
        icc[0] = 0;
        icc[1] = (1u << 0) | (1u << dci);
        uint32_t* epc = EndpointCtx(idx, dci);
        usb_memset(epc, 0, ctx_stride);
        epc[0] = (xint << 16) | (mult << 8);
        // CErr 0 for isoch, EPType 1 = isoch out, 5 = isoch in (satoru)
        epc[1] = (0u << 1) | ((in ? 5u : 1u) << 3) | (burst << 8) | ((uint32_t)p.max_packet << 16);
        uint64_t deq = KernelPhysOf(ring) | 1;
        epc[2] = (uint32_t)deq;
        epc[3] = (uint32_t)(deq >> 32);
        epc[4] = (avg & 0xFFFF) | ((esit & 0xFFFF) << 16);

        if (want_fb) {
            icc[1] |= (1u << fb_dci);
            uint32_t* fep = EndpointCtx(idx, fb_dci);
            usb_memset(fep, 0, ctx_stride);
            fep[0] = (fb_xint << 16);
            fep[1] = (0u << 1) | (5u << 3) | ((uint32_t)p.fb_max_packet << 16);
            uint64_t fdeq = KernelPhysOf(fb_ring) | 1;
            fep[2] = (uint32_t)fdeq;
            fep[3] = (uint32_t)(fdeq >> 32);
            fep[4] = (uint32_t)p.fb_max_packet | ((uint32_t)p.fb_max_packet << 16);
        }

        xHCI_TRB cmd = {};
        cmd.parameter = KernelPhysOf(rt.input_ctx);
        cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
        xHCI_TRB res = {};
        bool sent = SubmitCommand(&cmd, &res);
        uint8_t cc = (uint8_t)((res.status >> 24) & 0xFF);
        usb_log("USB: iso open slot %d ep %x dci %d mps %d burst %d mult %d esit %d interval-field %d (%d us): configure cc %d\n",
                (int)rt.slot, (uint32_t)p.ep_addr, (int)dci, (int)p.max_packet, (int)burst, (int)mult,
                (int)esit, (int)xint, (int)period_us, (int)cc);
        if (want_fb) {
            usb_log("USB: iso feedback slot %d ep %x dci %d mps %d per %d us (%s)\n",
                    (int)rt.slot, (uint32_t)p.fb_ep_addr, (int)fb_dci, (int)p.fb_max_packet,
                    (int)(125u << fb_xint), (sent && cc == TRB_CC_SUCCESS) ? "configured" : "refused");
        }
        if (sent && cc == TRB_CC_SUCCESS) {
            rt.iso_ring = ring; rt.iso_idx = 0; rt.iso_cycle = true;
            rt.iso_dci = dci; rt.iso_ep_addr = p.ep_addr;
            if (want_fb) {
                rt.iso_fb_ring = fb_ring; rt.iso_fb_idx = 0; rt.iso_fb_cycle = true;
                rt.iso_fb_dci = fb_dci; rt.iso_fb_ep_addr = p.fb_ep_addr;
            }
            if (out_period_us) *out_period_us = period_us;
            ok = true;
            break;
        }
        // feedback endpoint refused (bandwidth or a picky controller): drop
        // it and configure the data endpoint alone, open loop. (satoru)
        if (want_fb) {
            usb_log("USB: feedback endpoint refused, falling back to open loop\n");
            icc[1] = (1u << 0) | (1u << dci);
            usb_memset(EndpointCtx(idx, fb_dci), 0, ctx_stride);
            cmd.parameter = KernelPhysOf(rt.input_ctx);
            cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
            res = xHCI_TRB();
            sent = SubmitCommand(&cmd, &res);
            cc = (uint8_t)((res.status >> 24) & 0xFF);
            usb_log("USB: iso open data-only: configure cc %d\n", (int)cc);
            if (sent && cc == TRB_CC_SUCCESS) {
                rt.iso_ring = ring; rt.iso_idx = 0; rt.iso_cycle = true;
                rt.iso_dci = dci; rt.iso_ep_addr = p.ep_addr;
                if (out_period_us) *out_period_us = period_us;
                ok = true;
                break;
            }
        }
        PMM::FreeBytes(ring, 4096);
    } while (0);
    g_dispatch_foreign = svf; S = sv;
    return ok;
}

void USB::IsoClose(int gid) {
    if (!GidValid(gid)) return;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    USBDeviceRuntime& rt = runtime[idx];
    uint8_t dci = rt.iso_dci, fb_dci = rt.iso_fb_dci;
    if (dci || fb_dci) {
        xHCI_TRB* ring = rt.iso_ring;
        rt.iso_dci = 0; rt.iso_fb_dci = 0;                       // events for them are ignored from here (satoru)
        rt.iso_ring = nullptr;
        // the feedback page is kept: isoch is not running while no td is queued
        // (g_fb_open is false), and the next open reuses it, so a completion
        // racing a close can never touch a freed page. (satoru)
        xHCI_TRB r = {};
        if (dci) {
            xHCI_TRB st = {};
            st.control = TRB_TYPE(15) | ((uint32_t)rt.slot << 24) | ((uint32_t)dci << 16);
            SubmitCommand(&st, &r);
        }
        if (fb_dci) {
            xHCI_TRB st = {};
            st.control = TRB_TYPE(15) | ((uint32_t)rt.slot << 24) | ((uint32_t)fb_dci << 16);
            SubmitCommand(&st, &r);
        }
        uint32_t* icc = InputControlCtx(idx);
        icc[0] = (dci ? (1u << dci) : 0u) | (fb_dci ? (1u << fb_dci) : 0u);
        icc[1] = (1u << 0);
        xHCI_TRB cmd = {};
        cmd.parameter = KernelPhysOf(rt.input_ctx);
        cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
        SubmitCommand(&cmd, &r);
        usb_log("USB: iso close slot %d dci %d fb %d: drop cc %d\n", (int)rt.slot, (int)dci, (int)fb_dci,
                (int)((r.status >> 24) & 0xFF));
        if (ring) PMM::FreeBytes(ring, 4096);
    }
    g_dispatch_foreign = svf; S = sv;
}

bool USB::IsoQueue(int gid, const void* data, uint32_t len, bool ioc) {
    if (gid < 0) return false;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return false;
    USBDeviceRuntime& rt = g_xs[c].x_runtime[i];
    if (!rt.in_use || !rt.iso_ring || !rt.iso_dci) return false;
    xHCI_TRB* t = &rt.iso_ring[rt.iso_idx];
    t->parameter = KernelPhysOf(data);
    t->status = len & 0x1FFFF;                                   // td size 0, interrupter 0 (satoru)
    __asm__ __volatile__("" ::: "memory");
    t->control = TRB_TYPE(TRB_ISOCH) | TRB_ISO_SIA | (ioc ? TRB_IOC : 0) | (rt.iso_cycle ? TRB_CYCLE : 0);
    rt.iso_idx++;
    if (rt.iso_idx >= USB_ISO_RING_TRBS - 1) {
        rt.iso_ring[USB_ISO_RING_TRBS - 1].parameter = KernelPhysOf(rt.iso_ring);
        rt.iso_ring[USB_ISO_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (rt.iso_cycle ? TRB_CYCLE : 0);
        rt.iso_idx = 0;
        rt.iso_cycle = !rt.iso_cycle;
    }
    return true;
}

void USB::IsoKick(int gid) {
    if (gid < 0) return;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return;
    XhciState* st = &g_xs[c];
    USBDeviceRuntime& rt = st->x_runtime[i];
    if (!rt.in_use || !rt.iso_dci) return;
    __asm__ __volatile__("mfence" ::: "memory");
    uint32_t db_off = *(volatile uint32_t*)(st->x_bar0 + XHCI_CAP_DBOFF) & ~0x3u;
    *(volatile uint32_t*)(st->x_bar0 + db_off + rt.slot * 4) = rt.iso_dci;
}

uint32_t USB::IsoEpState(int gid) {
    if (gid < 0) return 7;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return 7;
    XhciState* st = &g_xs[c];
    USBDeviceRuntime& rt = st->x_runtime[i];
    if (!rt.in_use || !rt.iso_dci || !rt.device_ctx) return 7;
    // output device context: slot context first, then ep contexts by dci (satoru)
    volatile uint32_t* ep = (volatile uint32_t*)(rt.device_ctx + (uint32_t)rt.iso_dci * st->x_ctx_stride);
    return ep[0] & 7u;
}

bool USB::IsoRecover(int gid) {
    if (!GidValid(gid)) return false;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    USBDeviceRuntime& rt = runtime[idx];
    bool ok = false;
    if (rt.iso_dci) ok = RecoverEp(idx, rt.iso_dci, rt.iso_ring, rt.iso_idx, rt.iso_cycle);
    g_dispatch_foreign = svf; S = sv;
    return ok;
}

// feedback endpoint queue/kick/state/recover, same shape as the data ones. (satoru)
bool USB::IsoQueueFb(int gid, const void* data, uint32_t len, bool ioc) {
    if (gid < 0) return false;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return false;
    USBDeviceRuntime& rt = g_xs[c].x_runtime[i];
    if (!rt.in_use || !rt.iso_fb_ring || !rt.iso_fb_dci) return false;
    xHCI_TRB* t = &rt.iso_fb_ring[rt.iso_fb_idx];
    t->parameter = KernelPhysOf(data);
    t->status = len & 0x1FFFF;
    __asm__ __volatile__("" ::: "memory");
    t->control = TRB_TYPE(TRB_ISOCH) | TRB_ISO_SIA | (ioc ? TRB_IOC : 0) | (rt.iso_fb_cycle ? TRB_CYCLE : 0);
    rt.iso_fb_idx++;
    if (rt.iso_fb_idx >= USB_ISO_RING_TRBS - 1) {
        rt.iso_fb_ring[USB_ISO_RING_TRBS - 1].parameter = KernelPhysOf(rt.iso_fb_ring);
        rt.iso_fb_ring[USB_ISO_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (rt.iso_fb_cycle ? TRB_CYCLE : 0);
        rt.iso_fb_idx = 0;
        rt.iso_fb_cycle = !rt.iso_fb_cycle;
    }
    return true;
}

void USB::IsoKickFb(int gid) {
    if (gid < 0) return;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return;
    XhciState* st = &g_xs[c];
    USBDeviceRuntime& rt = st->x_runtime[i];
    if (!rt.in_use || !rt.iso_fb_dci) return;
    __asm__ __volatile__("mfence" ::: "memory");
    uint32_t db_off = *(volatile uint32_t*)(st->x_bar0 + XHCI_CAP_DBOFF) & ~0x3u;
    *(volatile uint32_t*)(st->x_bar0 + db_off + rt.slot * 4) = rt.iso_fb_dci;
}

uint32_t USB::IsoEpStateFb(int gid) {
    if (gid < 0) return 7;
    int c = gid / USB_MAX_DEVICES, i = gid % USB_MAX_DEVICES;
    if (c >= g_nctl) return 7;
    XhciState* st = &g_xs[c];
    USBDeviceRuntime& rt = st->x_runtime[i];
    if (!rt.in_use || !rt.iso_fb_dci || !rt.device_ctx) return 7;
    volatile uint32_t* ep = (volatile uint32_t*)(rt.device_ctx + (uint32_t)rt.iso_fb_dci * st->x_ctx_stride);
    return ep[0] & 7u;
}

bool USB::IsoRecoverFb(int gid) {
    if (!GidValid(gid)) return false;
    XhciState* sv = S; bool svf = g_dispatch_foreign;
    S = &g_xs[GidCtl(gid)]; g_dispatch_foreign = true;
    int idx = GidIdx(gid);
    USBDeviceRuntime& rt = runtime[idx];
    bool ok = false;
    if (rt.iso_fb_dci) ok = RecoverEp(idx, rt.iso_fb_dci, rt.iso_fb_ring, rt.iso_fb_idx, rt.iso_fb_cycle);
    g_dispatch_foreign = svf; S = sv;
    return ok;
}

// self test for the hid report parser (kurono.usbtest=1). the descriptors are
// the qemu keyboard/tablet ones plus a synthetic report-id gaming mouse with
// 16 bit signed relative axes. (satoru)
bool USB::SelfTest() {
    static const uint8_t kbd[] = {
        0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x75,0x01, 0x95,0x08, 0x05,0x07,
        0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x08, 0x81,0x01,
        0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0xFF, 0x05,0x07,
        0x19,0x00, 0x29,0xFF, 0x81,0x00, 0xC0
    };
    static const uint8_t tablet[] = {
        0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
        0x95,0x05, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x03, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x7F,
        0x75,0x10, 0x95,0x02, 0x81,0x02,
        0x05,0x01, 0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
        0xC0,0xC0
    };
    static const uint8_t gmouse[] = {
        0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x01, 0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
        0x95,0x05, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x03, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x01,0x80, 0x26,0xFF,0x7F,
        0x75,0x10, 0x95,0x02, 0x81,0x06,
        0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
        0xC0,0xC0,
        0x85,0x02, 0x09,0x03, 0xA1,0x00, 0x05,0x0B, 0x09,0x01,
        0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01, 0x81,0x02, 0xC0
    };

    int fails = 0;
    auto check = [&](const char* what, bool ok) {
        usb_log("[usbtest] %s: %s\n", what, ok ? "ok" : "FAIL");
        if (!ok) fails++;
    };

    UsbHidLayout lay[4];
    int n = 0;
    usb_memset(lay, 0, sizeof(lay));
    ParseHidLayout(kbd, (int)sizeof(kbd), lay, &n);
    check("keyboard parsed", n == 1);
    if (n >= 1) {
        check("keyboard modifiers", lay[0].has_kbd && lay[0].mod_off == 0 && lay[0].mod_count == 8);
        check("keyboard key array", lay[0].key_off == 16 && lay[0].key_count == 6 && lay[0].key_size == 8);
    }

    n = 0;
    usb_memset(lay, 0, sizeof(lay));
    ParseHidLayout(tablet, (int)sizeof(tablet), lay, &n);
    check("tablet parsed", n == 1);
    if (n >= 1) {
        check("tablet absolute x/y", lay[0].has_mouse && !lay[0].x.relative &&
              !lay[0].y.relative && lay[0].x.size == 16 && lay[0].y.off == 24);
        check("tablet buttons", lay[0].btn_off == 0 && lay[0].btn_count == 5);
    }

    n = 0;
    usb_memset(lay, 0, sizeof(lay));
    ParseHidLayout(gmouse, (int)sizeof(gmouse), lay, &n);
    usb_log("[usbtest] gmouse n=%d id=%d xoff=%d xsize=%d rel=%d yoff=%d woff=%d mouse=%d kbd=%d\n",
            n, n ? (int)lay[0].id : -1, n ? (int)lay[0].x.off : -1, n ? (int)lay[0].x.size : -1,
            n ? (lay[0].x.relative ? 1 : 0) : -1, n ? (int)lay[0].y.off : -1,
            n ? (int)lay[0].wheel.off : -1, n ? (lay[0].has_mouse ? 1 : 0) : -1,
            n ? (lay[0].has_kbd ? 1 : 0) : -1);
    check("gaming mouse parsed", n == 1 && lay[0].id == 1);
    if (n >= 1) {
        check("gaming mouse report id", lay[0].id == 1);
        check("gaming mouse 16 bit axes", lay[0].x.valid && lay[0].x.size == 16 &&
              lay[0].x.relative && lay[0].y.off == 24 && lay[0].wheel.off == 40);
        uint8_t rpt[7] = { 0x01, 0x05, 0x10, 0xF0, 0x40, 0x00, 0xFF };
        // layout offsets start after the report id byte. (satoru)
        const uint8_t* d = rpt + 1;
        check("gaming mouse buttons", hid_bits(d, 6, lay[0].btn_off, lay[0].btn_count) == 5);
        check("gaming mouse signed x", hid_svalue(d, 6, lay[0].x) == -4080);
        check("gaming mouse y", hid_svalue(d, 6, lay[0].y) == 64);
        check("gaming mouse wheel", hid_svalue(d, 6, lay[0].wheel) == -1);
    }

    usb_log("[usbtest] hid layout parser %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails == 0;
}

// it is bound as a keyboard, and how many interrupt transfers ever completed.
// zero completions on a keyboard that was typed on = the endpoint never ran. (satoru)
void USB::LogKbDiag() {
    int nkb = 0;
    for (int c = 0; c < g_nctl; c++) {
        XhciState* st = &g_xs[c];
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            USBDeviceRuntime& rt = st->x_runtime[i];
            if (!rt.in_use || rt.n_intr == 0) continue;
            if (rt.hid_type == USB_HID_KEYBOARD) nkb++;
            for (int e = 0; e < rt.n_intr; e++) {
                const UsbIntrEp& ep = rt.intr[e];
                usb_log("[kbdiag] usb ctl %d dev %d port %d slot %d ep %d vid:pid %x:%x role %s intr_done %d intr_err %d armed %d\n",
                        c, i, (int)rt.port, (int)rt.slot, (int)ep.ep_id,
                        (uint32_t)st->x_devices[i].vendor_id, (uint32_t)st->x_devices[i].product_id,
                        ep.role == USB_HID_KEYBOARD ? "keyboard" : (ep.role == USB_HID_MOUSE ? "mouse" : "hub"),
                        (int)st->intr_done[i], (int)st->intr_err[i], ep.armed ? 1 : 0);
                // what the controller thinks of this endpoint, read from the
                // output context: state 1 running, 2 halted, 3 stopped, 4 error,
                // and its dequeue slot next to our enqueue slot. an armed
                // endpoint that is silent with state 1 is waiting on the
                // device; any other state means it stopped without an event
                // reaching us. memory reads only. (satoru)
                if (rt.device_ctx && ep.ring) {
                    const uint32_t* oc = (const uint32_t*)(rt.device_ctx + (uint32_t)ep.ep_id * st->x_ctx_stride);
                    const uint32_t* sc = (const uint32_t*)rt.device_ctx;
                    uint64_t deq = ((uint64_t)oc[3] << 32) | (oc[2] & ~0xFu);
                    int deq_i = (int)((deq - KernelPhysOf(ep.ring)) / sizeof(xHCI_TRB));
                    usb_log("[kbdiag]   slot %d ep %d xhci ep state %d deq slot %d enq slot %d | slot state %d addr %d depth %d hub port %d tt slot %d\n",
                            (int)rt.slot, (int)ep.ep_id, (int)(oc[0] & 0x7), deq_i, (int)ep.idx,
                            (int)((sc[3] >> 27) & 0x1F), (int)(sc[3] & 0xFF),
                            (int)rt.depth, (int)rt.parent_port, (int)rt.tt_hub_slot);
                }
            }
        }
    }
    usb_log("[kbdiag] usb keyboards bound: %d (controllers %d)\n", nkb, g_nctl);
}

// [usbports] all controllers, all ports, raw portsc, even when nothing is
// attached, so the next laptop log shows where a usb-c device landed. (satoru)
void USB::LogAllPorts() {
    XhciState* sv = S;
    for (int c = 0; c < g_nctl; c++) {
        S = &g_xs[c];
        usb_log("[usbports] ctl %d pci %d:%d.%d ports %d slots %d\n", c, (int)S->pci_bus, (int)S->pci_dev,
                (int)S->pci_func, (int)max_ports, (int)max_slots);
        for (int p = 0; p < (int)max_ports; p++) {
            uint32_t sc = ReadPort(p);
            usb_log("[usbports] ctl %d port %d portsc=%x ccs=%d ped=%d pls=%d pp=%d speed=%s\n", c, p, sc,
                    (int)(sc & 1), (int)((sc >> 1) & 1), (int)((sc >> 5) & 0xF), (int)((sc >> 9) & 1),
                    (sc & 1) ? SpeedName(XHCI_PORTSC_SPEED(sc)) : "none");
        }
    }
    for (int i = 0; i < g_ncands; i++) {
        if (!g_cands[i].driven)
            usb_log("[usbports] candidate %d:%d.%d NOT driven (ports %d)\n", (int)g_cands[i].bus,
                    (int)g_cands[i].dev, (int)g_cands[i].func, (int)g_cands[i].ports);
    }
    S = sv;
}
