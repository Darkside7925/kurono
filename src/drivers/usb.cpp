//  kurono os - usb (xhci) host controller driver implementation
//  pci class 0c:03:30 (xhci usb 3.0)
#include "usb.h"
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
    const uint64_t my_trb = (uint64_t)(uintptr_t)slot;   // matched against the completion (satoru)
    slot->parameter = cmd_trb->parameter;
    slot->status = cmd_trb->status;
    uint32_t ctrl = cmd_trb->control & ~TRB_CYCLE;
    slot->control = ctrl | (cmd_cycle ? TRB_CYCLE : 0);

    cmd_ring_idx++;
    if (cmd_ring_idx >= USB_RING_TRBS - 1) {
        // link trb back to the head; toggle-cycle so we flip producer parity on
        // every wrap (satoru).
        cmd_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)cmd_ring;
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
    uint64_t deq = (uint64_t)(uintptr_t)&event_ring[event_ring_idx];
    // interrupter 0 erdp lives at runtime offset 0x38 (satoru).
    WriteRuntime64(0x38, deq | XHCI_ERDP_EHB);
}

bool USB::PollEventRing(xHCI_TRB* result, int timeout) {
    for (int i = 0; i < timeout * 100; i++) {
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
                spa[i] = (uint64_t)(uintptr_t)pg;
            }
            dcbaa[0] = (uint64_t)(uintptr_t)spa;
        }
    }

    // full 64-bit address: the high dword was hardcoded 0, which breaks the
    // moment the heap hands out memory above 4 gb (32 gb laptop). (satoru)
    uint64_t dcbaa_phys = (uint64_t)(uintptr_t)dcbaa;
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
    uint64_t crcr_val = (uint64_t)(uintptr_t)cmd_ring | 1; // rcs=1
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
    erst[0] = (uint64_t)(uintptr_t)event_ring;
    ((uint32_t*)erst)[2] = USB_RING_TRBS; // segment size
    ((uint32_t*)erst)[3] = 0;             // reserved

    // write erstsz and erstba to interrupter 0
    WriteRuntime(0x28, 1);  // erstsz = 1 segment
    WriteRuntime64(0x30, (uint64_t)(uintptr_t)erst);

    // set erdp (with no EHB bit yet; controller has consumed nothing)
    WriteRuntime64(0x38, (uint64_t)(uintptr_t)event_ring);

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
int USB::FindDeviceByPort(uint8_t port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (runtime[i].in_use && runtime[i].port == port) return i;
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
    rt.ep0_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.ep0_ring;
    rt.ep0_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;

    // dcbaa[slot] points at the output device context (satoru).
    dcbaa[rt.slot] = (uint64_t)(uintptr_t)rt.device_ctx;

    // input control context: add slot context (A0) + ep0 context (A1).
    uint32_t* icc = InputControlCtx(idx);
    icc[0] = 0;                 // drop flags
    icc[1] = (1u << 0) | (1u << 1); // add slot + ep0 (satoru)

    // slot context: route string 0, speed, one context entry (ep0), root hub
    // port = our 1-based port number (satoru).
    uint32_t* slot_ctx = SlotCtx(idx);
    slot_ctx[0] = ((uint32_t)1 << 27) | ((uint32_t)rt.speed << 20); // ctx entries=1, speed
    slot_ctx[1] = ((uint32_t)(rt.port + 1) << 16);                  // root hub port number (1-based)

    // ep0 context (dci 1): control endpoint (type 4), max packet by speed,
    // CErr=3, TR dequeue = ep0 ring base with DCS=1 (satoru).
    uint16_t mps = default_max_packet(rt.speed);
    uint32_t* ep0 = EndpointCtx(idx, 1);
    ep0[0] = 0;
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16); // CErr=3, EPType=Control, MaxPacketSize
    uint64_t ep0_deq = (uint64_t)(uintptr_t)rt.ep0_ring | 1; // DCS=1 (satoru)
    ep0[2] = (uint32_t)(ep0_deq & 0xFFFFFFFF);
    ep0[3] = (uint32_t)(ep0_deq >> 32);
    ep0[4] = 8;  // average trb length hint (satoru)

    // Address Device command - input context pointer in parameter, slot in
    // control[31:24] (satoru).
    xHCI_TRB cmd = {};
    cmd.parameter = (uint64_t)(uintptr_t)rt.input_ctx;
    cmd.control = TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)rt.slot << 24);
    xHCI_TRB result = {};
    if (!SubmitCommand(&cmd, &result)) return false;
    uint8_t cc = (result.status >> 24) & 0xFF;
    return cc == TRB_CC_SUCCESS;
}

// parse the configuration descriptor, locate the first HID interface and its
// interrupt-IN endpoint, then SET_CONFIGURATION + Configure Endpoint so the
// interrupt endpoint becomes usable (satoru).
bool USB::ConfigureDevice(int idx, const uint8_t* dev_desc) {
    USBDeviceRuntime& rt = runtime[idx];
    (void)dev_desc;

    // pull the 9-byte config header to learn the total length, then the whole
    // configuration block (descriptors are concatenated) (satoru).
    uint8_t cfg[256];
    usb_memset(cfg, 0, sizeof(cfg));
    if (!ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)(USB_DESC_CONFIG << 8), 0, 9, cfg))
        return false;
    uint16_t total_len = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total_len > sizeof(cfg)) total_len = sizeof(cfg);
    if (total_len < 9) return false;
    if (!ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)(USB_DESC_CONFIG << 8), 0, total_len, cfg))
        return false;

    uint8_t config_value = cfg[5];

    // walk the concatenated descriptors (satoru).
    bool     in_hid_iface = false;
    uint8_t  hid_iface_num = 0;
    bool     hid_boot = false;
    uint8_t  ep_addr = 0;
    uint8_t  ep_interval = 0;
    uint16_t ep_mps = 0;
    bool     found_ep = false;
    uint16_t report_desc_len = 0;
    // mass-storage (bot) scan state: first class-08/protocol-50 interface and
    // its bulk in/out endpoint pair (satoru)
    bool     in_msd_iface = false;
    uint8_t  bin_addr = 0, bout_addr = 0;
    uint16_t bin_mps = 0, bout_mps = 0;

    int p = 0;
    while (p + 2 <= (int)total_len) {
        uint8_t len = cfg[p];
        uint8_t type = cfg[p + 1];
        if (len == 0) break; // malformed; bail rather than spin (satoru)

        if (type == 0x04 && p + 9 <= (int)total_len) {            // interface
            uint8_t iclass = cfg[p + 5];
            uint8_t isub   = cfg[p + 6];
            uint8_t iproto = cfg[p + 7];
            if (iclass == 0x01) rt.has_audio = true;               // audio class: the audio driver takes it from here (satoru)
            // lock onto the FIRST hid interface that has an interrupt-in
            // endpoint. later hid interfaces used to overwrite the interface
            // number + report length while the endpoint stayed the first one,
            // so a combo device (logitech g502: iface 0 mouse, iface 1 macro
            // keyboard) read the keyboard's report layout and decoded mouse
            // packets as key presses. (satoru)
            if (iclass == 0x03 && !found_ep) {                     // HID class (satoru)
                in_hid_iface = true;
                in_msd_iface = false;
                hid_iface_num = cfg[p + 2];
                hid_boot = (isub == 0x01);                         // boot subclass (satoru)
                rt.hid_type = USB_HID_NONE;
            } else if (iclass == USB_MSC_CLASS && iproto == USB_MSC_PROTO_BOT &&
                       !(bin_addr && bout_addr)) {
                // bulk-only mass storage (any scsi-ish subclass; 0x06 is the
                // usual transparent-scsi) (satoru)
                in_msd_iface = true;
                in_hid_iface = false;
            } else {
                in_hid_iface = false;
                in_msd_iface = false;
            }
        } else if (type == USB_DESC_HID && in_hid_iface && p + 9 <= (int)total_len) {
            // hid descriptor: bNumDescriptors at +5, then [type,len16] pairs.
            // the first class descriptor is the report descriptor (satoru).
            if (cfg[p + 6] == USB_DESC_REPORT)
                report_desc_len = (uint16_t)(cfg[p + 7] | (cfg[p + 8] << 8));
        } else if (type == 0x05 && in_hid_iface && !found_ep && p + 7 <= (int)total_len) {
            uint8_t addr = cfg[p + 2];
            uint8_t attr = cfg[p + 3];
            // interrupt (attr & 3 == 3) and IN (addr & 0x80) (satoru)
            if ((attr & 0x03) == 0x03 && (addr & 0x80)) {
                ep_addr = addr;
                ep_mps = (uint16_t)((cfg[p + 4] | (cfg[p + 5] << 8)) & 0x07FF);
                ep_interval = cfg[p + 6];
                found_ep = true;
            }
        } else if (type == 0x05 && in_msd_iface && p + 7 <= (int)total_len) {
            uint8_t addr = cfg[p + 2];
            uint8_t attr = cfg[p + 3];
            if ((attr & 0x03) == 0x02) {                           // bulk (satoru)
                uint16_t mps = (uint16_t)((cfg[p + 4] | (cfg[p + 5] << 8)) & 0x07FF);
                if ((addr & 0x80) && !bin_addr)   { bin_addr = addr;  bin_mps = mps; }
                if (!(addr & 0x80) && !bout_addr) { bout_addr = addr; bout_mps = mps; }
            }
        }
        p += len;
    }

    bool found_bulk = (bin_addr != 0 && bout_addr != 0);
    if (!found_ep && !found_bulk) return false;

    if (found_ep) {
        rt.intr_ep_addr = ep_addr;
        rt.intr_interval = ep_interval;
        // dci for an IN endpoint N: (N*2)+1; ep_addr low nibble is the number (satoru)
        rt.intr_ep_id = (uint8_t)(((ep_addr & 0x0F) * 2) + 1);

        // optionally read the report descriptor to confirm device type / report
        // length. boot protocol still drives the actual dispatch (satoru).
        if (report_desc_len) {
            uint8_t rpt[256];
            uint16_t rlen = report_desc_len > sizeof(rpt) ? (uint16_t)sizeof(rpt) : report_desc_len;
            usb_memset(rpt, 0, sizeof(rpt));
            // GET_DESCRIPTOR(report) is directed at the interface (satoru).
            if (ControlTransferEP(idx, 0x81, USB_REQ_GET_DESCRIPTOR,
                                  (uint16_t)(USB_DESC_REPORT << 8), hid_iface_num, rlen, rpt)) {
                ParseReportDescriptor(idx, rpt, rlen);
            }
        }

        // fall back to interface protocol if the report descriptor parse did not
        // settle a type: HID protocol 1 = keyboard, 2 = mouse (boot) (satoru).
        if (rt.hid_type == USB_HID_NONE) {
            // re-scan for the interface protocol byte (bInterfaceProtocol @ +7).
            p = 0;
            while (p + 2 <= (int)total_len) {
                uint8_t len = cfg[p];
                uint8_t type = cfg[p + 1];
                if (len == 0) break;
                if (type == 0x04 && cfg[p + 2] == hid_iface_num && p + 9 <= (int)total_len) {
                    uint8_t proto = cfg[p + 7];
                    if (proto == 1) rt.hid_type = USB_HID_KEYBOARD;
                    else if (proto == 2) rt.hid_type = USB_HID_MOUSE;
                    break;
                }
                p += len;
            }
        }
    }

    // SET_CONFIGURATION(config_value) - no data stage (satoru).
    if (!ControlTransferEP(idx, 0x00, USB_REQ_SET_CONFIG, config_value, 0, 0, nullptr))
        return false;
    rt.cfg_value = config_value;

    // build ONE input control context adding every endpoint this device gets
    // (interrupt for hid, bulk in+out for mass storage), then issue a single
    // Configure Endpoint. a pure-hid device produces exactly the same context
    // this function always built. (satoru)
    uint32_t* icc = InputControlCtx(idx);
    icc[0] = 0;
    icc[1] = (1u << 0);
    uint8_t max_dci = 1;

    if (found_ep) {
        // allocate the interrupt transfer ring + report buffer (satoru).
        rt.intr_ring = (xHCI_TRB*)usb_alloc_aligned(4096, 4096);
        rt.intr_buf  = (uint8_t*)usb_alloc_aligned(4096, 4096);
        if (!rt.intr_ring || !rt.intr_buf) return false;
        usb_memset(rt.intr_ring, 0, USB_RING_TRBS * sizeof(xHCI_TRB));
        usb_memset(rt.intr_buf, 0, 4096);
        rt.intr_idx = 0;
        rt.intr_cycle = true;
        rt.intr_buf_len = ep_mps ? (int)ep_mps : 8;
        if (rt.intr_buf_len > USB_HID_REPORT_MAX) rt.intr_buf_len = USB_HID_REPORT_MAX;
        rt.intr_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.intr_ring;
        rt.intr_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;

        icc[1] |= (1u << rt.intr_ep_id);
        if (rt.intr_ep_id > max_dci) max_dci = rt.intr_ep_id;

        // interrupt-IN endpoint context: EPType 7 (interrupt-in), CErr=3,
        // interval copied from the descriptor, max packet, TR dequeue = intr ring
        // with DCS=1, plus a sane max-burst/ESIT hint (satoru).
        uint32_t* epc = EndpointCtx(idx, rt.intr_ep_id);
        usb_memset(epc, 0, ctx_stride);
        // interval as linux xhci_get_endpoint_interval: the xhci field is an
        // exponent (period = 2^n * 125 us). full/low speed bInterval is in
        // FRAMES (ms): xhci_parse_frame_interval = fls(8 * bInterval) - 1,
        // clamped 3..10. high/super speed bInterval is already an exponent
        // + 1: xhci_parse_exponent_interval = bInterval - 1, clamped 0..15.
        // copying bInterval raw gave a full-speed mouse with bInterval 1 an
        // illegal interval of 1. (satoru)
        uint32_t xint;
        if (rt.speed == USB_SPEED_FULL || rt.speed == USB_SPEED_LOW) {
            uint32_t frames = ep_interval ? ep_interval : 1;
            uint32_t v = frames * 8, fls = 0;
            while (v) { fls++; v >>= 1; }
            xint = fls - 1;
            if (xint < 3) xint = 3;
            if (xint > 10) xint = 10;
        } else {
            xint = ep_interval ? (uint32_t)ep_interval - 1 : 0;
            if (xint > 15) xint = 15;
        }
        epc[0] = (xint << 16);                                       // Interval (satoru)
        epc[1] = (3u << 1) | (7u << 3) | ((uint32_t)ep_mps << 16);    // CErr=3, EPType=IntrIn, MPS
        uint64_t intr_deq = (uint64_t)(uintptr_t)rt.intr_ring | 1;    // DCS=1 (satoru)
        epc[2] = (uint32_t)(intr_deq & 0xFFFFFFFF);
        epc[3] = (uint32_t)(intr_deq >> 32);
        // avg trb length AND max esit payload (bits 31:16), as linux:
        // max_esit_payload = mps * (burst + 1). leaving the payload 0 lets an
        // intel controller budget zero bandwidth for the endpoint. (satoru)
        epc[4] = (uint32_t)ep_mps | ((uint32_t)ep_mps << 16);
        usb_log("USB: slot %d hid iface %d boot=%d ep %x mps %d bInterval %d -> xhci interval %d\n",
                (int)rt.slot, (int)hid_iface_num, hid_boot ? 1 : 0, (uint32_t)ep_addr,
                (int)ep_mps, (int)ep_interval, (int)xint);
    }

    if (found_bulk) {
        rt.bulk_in_ep   = bin_addr;
        rt.bulk_out_ep  = bout_addr;
        rt.bulk_in_mps  = bin_mps  ? bin_mps  : 512;
        rt.bulk_out_mps = bout_mps ? bout_mps : 512;
        // dci: OUT ep N -> 2N, IN ep N -> 2N+1 (satoru)
        rt.bulk_in_dci  = (uint8_t)(((bin_addr & 0x0F) * 2) + 1);
        rt.bulk_out_dci = (uint8_t)((bout_addr & 0x0F) * 2);

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
        rt.bulk_in_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.bulk_in_ring;
        rt.bulk_in_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
        rt.bulk_out_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.bulk_out_ring;
        rt.bulk_out_ring[USB_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;

        icc[1] |= (1u << rt.bulk_in_dci) | (1u << rt.bulk_out_dci);
        if (rt.bulk_in_dci  > max_dci) max_dci = rt.bulk_in_dci;
        if (rt.bulk_out_dci > max_dci) max_dci = rt.bulk_out_dci;

        // bulk endpoint contexts: EPType 2 = bulk-out, 6 = bulk-in, CErr=3,
        // TR dequeue = ring base with DCS=1 (satoru).
        uint32_t* epo = EndpointCtx(idx, rt.bulk_out_dci);
        usb_memset(epo, 0, ctx_stride);
        epo[1] = (3u << 1) | (2u << 3) | ((uint32_t)rt.bulk_out_mps << 16);
        uint64_t odeq = (uint64_t)(uintptr_t)rt.bulk_out_ring | 1;
        epo[2] = (uint32_t)(odeq & 0xFFFFFFFF);
        epo[3] = (uint32_t)(odeq >> 32);
        epo[4] = rt.bulk_out_mps;

        uint32_t* epi = EndpointCtx(idx, rt.bulk_in_dci);
        usb_memset(epi, 0, ctx_stride);
        epi[1] = (3u << 1) | (6u << 3) | ((uint32_t)rt.bulk_in_mps << 16);
        uint64_t ideq = (uint64_t)(uintptr_t)rt.bulk_in_ring | 1;
        epi[2] = (uint32_t)(ideq & 0xFFFFFFFF);
        epi[3] = (uint32_t)(ideq >> 32);
        epi[4] = rt.bulk_in_mps;
    }

    uint32_t* slot_ctx = SlotCtx(idx);
    // context entries must be the highest valid dci (satoru).
    slot_ctx[0] = (slot_ctx[0] & ~((uint32_t)0x1F << 27)) | ((uint32_t)max_dci << 27);

    xHCI_TRB cmd = {};
    cmd.parameter = (uint64_t)(uintptr_t)rt.input_ctx;
    cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
    xHCI_TRB result = {};
    if (!SubmitCommand(&cmd, &result)) return false;
    if (((result.status >> 24) & 0xFF) != TRB_CC_SUCCESS) return false;

    // for boot-capable HID, ask the device to speak the fixed boot report
    // layout so our byte0/byte1/byte2 decode is valid (satoru).
    if (found_ep && hid_boot) {
        // SET_PROTOCOL(boot) - class request to the interface, no data (satoru).
        ControlTransferEP(idx, 0x21, HID_REQ_SET_PROTOCOL, HID_PROTO_BOOT,
                          hid_iface_num, 0, nullptr);
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

    return true;
}

// run one complete control transfer on a slot's ep0 ring: setup stage, optional
// data stage, status stage; ring the slot's ep0 doorbell (dci 1) and wait for
// the transfer-event completion (satoru).
bool USB::ControlTransferEP(int idx, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                            void* data) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.ep0_ring) return false;

    bool in = (bmRequestType & 0x80) != 0;
    bool has_data = (wLength > 0);

    auto push = [&](uint64_t param, uint32_t status, uint32_t control) {
        xHCI_TRB* t = &rt.ep0_ring[rt.ep0_idx];
        t->parameter = param;
        t->status = status;
        t->control = (control & ~TRB_CYCLE) | (rt.ep0_cycle ? TRB_CYCLE : 0);
        rt.ep0_idx++;
        if (rt.ep0_idx >= USB_RING_TRBS - 1) {
            rt.ep0_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.ep0_ring;
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
        push((uint64_t)(uintptr_t)data, (uint32_t)wLength, dctrl);
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
    for (int it = 0; it < 2000 && Scheduler::NowMs() - t0 < 5000; it++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 50)) continue;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type != TRB_TRANSFER_EVT) continue;   // port status / command noise (satoru)
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
            return false;
        }
        return true;
    }
    usb_log("USB: control transfer slot %d timeout (5 s)\n", (int)rt.slot);
    return false;
}

// queue one Normal trb on the interrupt ring pointing at the report buffer and
// ring the endpoint doorbell so the controller delivers the next report (satoru).
void USB::ArmInterrupt(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.intr_ring || !rt.intr_buf) return;

    xHCI_TRB* t = &rt.intr_ring[rt.intr_idx];
    t->parameter = (uint64_t)(uintptr_t)rt.intr_buf;
    t->status = (uint32_t)rt.intr_buf_len; // TRB transfer length (satoru)
    t->control = (TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP)
               | (rt.intr_cycle ? TRB_CYCLE : 0);

    rt.intr_idx++;
    if (rt.intr_idx >= USB_RING_TRBS - 1) {
        rt.intr_ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.intr_ring;
        rt.intr_ring[USB_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (rt.intr_cycle ? TRB_CYCLE : 0);
        rt.intr_idx = 0;
        rt.intr_cycle = !rt.intr_cycle;
    }

    rt.intr_armed = true;
    WriteDoorbell(rt.slot, rt.intr_ep_id);
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
    t->parameter = (uint64_t)(uintptr_t)rt.bulk_bounce;
    t->status = (uint32_t)len;                    // trb transfer length (satoru)
    t->control = (TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP)
               | (*pcycle ? TRB_CYCLE : 0);
    (*pidx)++;
    if (*pidx >= USB_RING_TRBS - 1) {
        ring[USB_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)ring;
        ring[USB_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_ENT | (*pcycle ? TRB_CYCLE : 0);
        *pidx = 0;
        *pcycle = !*pcycle;
    }

    bool was_claimed = g_usb_sync_claim;   // nested claims (cbw/data/csw) (satoru)
    g_usb_sync_claim = true;
    WriteDoorbell(rt.slot, dci);

    bool ok = false;
    for (int tries = 0; tries < 16; tries++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 5000)) break;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type != TRB_TRANSFER_EVT) continue;   // port/cmd noise: keep waiting (satoru)
        uint8_t ev_slot = (ev.control >> 24) & 0xFF;
        uint8_t ev_ep   = (ev.control >> 16) & 0x1F;
        if (ev_slot != rt.slot) continue;
        if (ev_ep == rt.intr_ep_id && rt.intr_ring) {
            // a hid report completed while we owned the ring: dispatch + re-arm
            // so input is not lost during long disk i/o. (satoru)
            uint8_t hc = (ev.status >> 24) & 0xFF;
            uint32_t hres = ev.status & 0x00FFFFFF;
            int got = rt.intr_buf_len - (int)hres;
            if ((hc == TRB_CC_SUCCESS || hc == TRB_CC_SHORT_PKT) && got > 0)
                DispatchReport(idx, rt.intr_buf, got);
            ArmInterrupt(idx);
            continue;
        }
        if (ev_ep != dci) continue;
        uint8_t cc = (ev.status >> 24) & 0xFF;
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

    if (ok && dir_in && data) {
        uint8_t* d = (uint8_t*)data;
        int got = out_transferred ? *out_transferred : len;
        for (int i = 0; i < got; i++) d[i] = rt.bulk_bounce[i];
    }
    return ok;
}

// one full bot round: cbw out, optional data stage, csw in + validation.
// returns true only when the device reports command PASSED. (satoru)
bool USB::MsdCommand(int idx, const uint8_t* cb, int cb_len, bool dir_in,
                     void* data, uint32_t data_len, uint32_t* out_residue) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.is_msd || !cb || cb_len < 1 || cb_len > 16) return false;
    if (out_residue) *out_residue = 0;

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
    if (!BulkTransferEP(idx, false, &cbw, (int)sizeof(cbw), &got) ||
        got != (int)sizeof(cbw)) {
        return false;
    }

    if (data_len > 0) {
        // data stage is at most one bounce chunk; callers chunk above. a
        // short bulk-in is fine (csw reports the residue). (satoru)
        if (data_len > USB_BULK_BOUNCE) return false;
        if (!BulkTransferEP(idx, dir_in, data, (int)data_len, &got)) return false;
    }

    UsbCsw csw;
    usb_memset(&csw, 0, sizeof(csw));
    if (!BulkTransferEP(idx, true, &csw, (int)sizeof(csw), &got) ||
        got != (int)sizeof(csw)) {
        return false;
    }
    if (csw.sig != USB_CSW_SIG || csw.tag != cbw.tag) return false;
    if (out_residue) *out_residue = csw.residue;
    return csw.status == 0;
}

// scsi bring-up for a freshly configured bot device: inquiry (names the
// stick), test-unit-ready with request-sense retries (a stick straight out
// of reset reports unit-attention once), then read-capacity(10). (satoru)
bool USB::MsdInit(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    USBDeviceInfo&    dev = devices[idx];

    // inquiry: 36 bytes of standard data (satoru)
    uint8_t inq[36];
    usb_memset(inq, 0, sizeof(inq));
    uint8_t cb_inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (MsdCommand(idx, cb_inq, 6, true, inq, 36, nullptr)) {
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
    }

    // test unit ready, clearing the post-reset unit-attention via request
    // sense; a real stick needs one or two rounds. (satoru)
    bool ready = false;
    for (int attempt = 0; attempt < 4 && !ready; attempt++) {
        uint8_t cb_tur[6] = { 0x00, 0, 0, 0, 0, 0 };
        if (MsdCommand(idx, cb_tur, 6, false, nullptr, 0, nullptr)) {
            ready = true;
            break;
        }
        uint8_t sense[18];
        usb_memset(sense, 0, sizeof(sense));
        uint8_t cb_sense[6] = { 0x03, 0, 0, 0, 18, 0 };
        MsdCommand(idx, cb_sense, 6, true, sense, 18, nullptr);
        usb_spin(50000);
    }
    if (!ready) return false;

    // read capacity(10): big-endian [last lba][block size] (satoru)
    uint8_t cap[8];
    usb_memset(cap, 0, sizeof(cap));
    uint8_t cb_cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!MsdCommand(idx, cb_cap, 10, true, cap, 8, nullptr)) return false;
    uint32_t last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                        ((uint32_t)cap[2] << 8)  | (uint32_t)cap[3];
    uint32_t bs       = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                        ((uint32_t)cap[6] << 8)  | (uint32_t)cap[7];
    if (bs == 0 || bs > USB_BULK_BOUNCE) return false;
    rt.msd_blocks     = (uint64_t)last_lba + 1;
    rt.msd_block_size = bs;
    rt.msd_ready      = true;
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
    // the bare-metal write guard has no usb window: guarded = read-only. on the
    // owner's laptop the usb disk holds macOS and their data. (satoru)
    if (NVMe::WriteGuardOn()) {
        // once per boot: a retrying caller used to flood serial and the disk log (satoru)
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; SerialLogger::Log("[diskguard] REFUSED usb mass-storage write (further refusals not logged)\r\n"); }
        return false;
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
// ---- end bulk transport + bot (satoru) ---------------------------------------

// minimal hid report-descriptor walk: track the current Usage Page + Usage to
// confirm keyboard vs mouse and remember nothing more than the device type;
// the boot-protocol fixed layout is what DispatchReport actually decodes
// (satoru).
void USB::ParseReportDescriptor(int idx, const uint8_t* rpt, int len) {
    USBDeviceRuntime& rt = runtime[idx];
    uint16_t usage_page = 0;
    uint16_t usage = 0;

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
        uint8_t bytes = (size == 3) ? 4 : size;   // 0,1,2,4 byte data (satoru)
        uint32_t data = 0;
        for (uint8_t b = 0; b < bytes && i < len; b++) data |= ((uint32_t)rpt[i++]) << (8 * b);

        if (type == 1 && tag == 0x0) usage_page = (uint16_t)data;       // Global: Usage Page
        else if (type == 2 && tag == 0x0) usage = (uint16_t)data;       // Local: Usage
        else if (type == 0 && tag == 0xA) {                             // Main: Collection
            // Generic Desktop (0x01): Mouse=0x02, Keyboard=0x06; Keyboard page
            // 0x07 is also a strong keyboard signal (satoru).
            if (usage_page == 0x01 && usage == 0x02 && rt.hid_type == USB_HID_NONE)
                rt.hid_type = USB_HID_MOUSE;
            else if (usage_page == 0x01 && usage == 0x06 && rt.hid_type == USB_HID_NONE)
                rt.hid_type = USB_HID_KEYBOARD;
            usage = 0;
        }
        if (usage_page == 0x07 && rt.hid_type == USB_HID_NONE)
            rt.hid_type = USB_HID_KEYBOARD;
    }
}

// dispatch a completed boot-protocol report to the right driver (satoru).
// native usb hid input enable (settings -> devices). default on. (satoru)
static bool g_usb_hid_input = true;
void USB::SetHIDInputEnabled(bool on) { g_usb_hid_input = on; }
bool USB::IsHIDInputEnabled() { return g_usb_hid_input; }

void USB::DispatchReport(int idx, const uint8_t* report, int len) {
    const int kid = (int)(S - g_xs) * USB_MAX_DEVICES + idx;   // controller 0: same as idx (satoru)
    {
        static uint8_t seen[USB_MAX_CTL * USB_MAX_DEVICES];
        if (idx >= 0 && idx < USB_MAX_DEVICES && seen[kid] < 3 && report && len > 0) {
            seen[kid]++;
            const USBDeviceRuntime& r0 = runtime[idx];
            usb_log("USB: slot %d (hid %d) report len %d: %x %x\n", (int)r0.slot, (int)r0.hid_type, len,
                    (uint32_t)(report[0] | (len > 1 ? report[1] << 8 : 0) | (len > 2 ? report[2] << 16 : 0) | (len > 3 ? (uint32_t)report[3] << 24 : 0)),
                    (uint32_t)(len > 4 ? (report[4] | (len > 5 ? report[5] << 8 : 0) | (len > 6 ? report[6] << 16 : 0) | (len > 7 ? (uint32_t)report[7] << 24 : 0)) : 0));
        }
    }
    if (!g_usb_hid_input) return;   // usb input disabled -> ps/2 takes over (satoru)
    USBDeviceRuntime& rt = runtime[idx];
    if (rt.hid_type == USB_HID_KEYBOARD) {
        // boot keyboard report is 8 bytes; the keyboard decoder requires >= 8
        // and indexes by device id (we use the devices[] index) (satoru).
        Keyboard::ProcessUSBReport((uint8_t)kid, report, (size_t)len);
    } else if (rt.hid_type == USB_HID_MOUSE) {
        // a 6+ byte pointer report is qemu's usb-tablet (and HID digitizers):
        // [buttons, X16, Y16, wheel] absolute. boot mice are 3-4 bytes relative.
        // route absolute pointers to the absolute handler so the tablet places
        // the cursor at a position instead of being decoded as relative deltas.
        // (satoru)
        if (len >= 6) Mouse::ProcessUSBAbsReport(report, len);
        else          Mouse::ProcessUSBReport(report, len);
    }
}

// tear down a slot's resources on disconnect. heap has no Free-by-region quirk
// here; KernelHeap::Free coalesces, so returning the pages is safe (satoru).
void USB::TeardownDevice(int idx) {
    USBDeviceRuntime& rt = runtime[idx];
    if (!rt.in_use) return;

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
    if (rt.device_ctx) KernelHeap::Free(rt.device_ctx);
    if (rt.input_ctx)  KernelHeap::Free(rt.input_ctx);
    if (rt.ep0_ring)   KernelHeap::Free(rt.ep0_ring);
    if (rt.intr_ring)  KernelHeap::Free(rt.intr_ring);
    if (rt.intr_buf)   KernelHeap::Free(rt.intr_buf);
    if (rt.bulk_in_ring)  KernelHeap::Free(rt.bulk_in_ring);   // msd teardown (satoru)
    if (rt.bulk_out_ring) KernelHeap::Free(rt.bulk_out_ring);
    if (rt.bulk_bounce)   KernelHeap::Free(rt.bulk_bounce);
    if (rt.iso_ring)      PMM::FreeBytes(rt.iso_ring, 4096);   // pmm page, not a heap block (satoru)

    uint8_t port = rt.port;
    usb_memset(&rt, 0, sizeof(rt));

    // remove from the public devices[] table by compaction, keeping indices
    // stable for the runtime[] entries that remain (satoru).
    for (int i = 0; i < device_count; i++) {
        if (devices[i].port == port && devices[i].connected) {
            devices[i].connected = false;
        }
    }
}

// full per-port bring-up: reset, enable slot, address device, read descriptors,
// configure the interrupt endpoint, and arm the first interrupt transfer. used
// at boot by EnumerateDevices and on hot-plug by PollHID (satoru).
bool USB::EnumeratePort(int port) {
    if (!IsPortConnected(port)) return false;
    uint8_t speed = GetPortSpeed(port);

    // reset port and re-read the (now valid) speed (satoru).
    ResetPort(port);
    usb_spin(20000);
    if (!IsPortConnected(port)) return false;
    speed = GetPortSpeed(port);
    if (speed == 0) return false;

    // find a free runtime/devices slot (satoru).
    int idx = -1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!runtime[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return false;

    // enable slot (satoru).
    xHCI_TRB enable_cmd = {};
    enable_cmd.control = TRB_TYPE(TRB_ENABLE_SLOT);
    xHCI_TRB result = {};
    if (!SubmitCommand(&enable_cmd, &result)) { usb_log("USB: port %d enable-slot cmd failed\n", port); return false; }
    uint8_t slot = (result.control >> 24) & 0xFF;
    if (slot == 0) { usb_log("USB: port %d enable-slot returned slot 0\n", port); return false; }

    USBDeviceRuntime& rt = runtime[idx];
    usb_memset(&rt, 0, sizeof(rt));
    rt.in_use = true;
    rt.slot = slot;
    rt.port = (uint8_t)port;
    rt.speed = speed;
    rt.hid_type = USB_HID_NONE;

    USBDeviceInfo& dev = devices[idx];
    usb_memset(&dev, 0, sizeof(dev));
    dev.connected = true;
    dev.port = (uint8_t)port;
    dev.slot = slot;
    dev.speed = speed;

    if (idx >= device_count) device_count = idx + 1;

    if (!AddressDevice(idx)) {
        usb_log("USB: Address Device failed\n");
        TeardownDevice(idx);
        return false;
    }

    // read the first 8 bytes to learn bMaxPacketSize0 for full/low speed, then
    // the full 18-byte device descriptor (satoru).
    USBDeviceDescriptor desc = {};
    if ((speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) &&
        ControlTransferEP(idx, 0x80, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(USB_DESC_DEVICE << 8), 0, 8, &desc)) {
        // byte 7 = bMaxPacketSize0. if it differs from the context, tell the
        // controller with Evaluate Context (trb type 13, only A1 = ep0), as
        // linux does after its first 64-byte read. (satoru)
        uint8_t real = ((const uint8_t*)&desc)[7];
        uint16_t cur = default_max_packet(speed);
        if ((real == 8 || real == 16 || real == 32 || real == 64) && real != cur) {
            uint32_t* icc = InputControlCtx(idx);
            icc[0] = 0;
            icc[1] = (1u << 1);
            uint32_t* e0 = EndpointCtx(idx, 1);
            e0[1] = (e0[1] & 0x0000FFFFu) | ((uint32_t)real << 16);
            xHCI_TRB ev = {};
            ev.parameter = (uint64_t)(uintptr_t)rt.input_ctx;
            ev.control = TRB_TYPE(13) | ((uint32_t)rt.slot << 24);
            xHCI_TRB er = {};
            bool ok = SubmitCommand(&ev, &er) && ((er.status >> 24) & 0xFF) == TRB_CC_SUCCESS;
            usb_log("USB: port %d ep0 max packet %d -> %d (evaluate context %s)\n",
                    port, (int)cur, (int)real, ok ? "ok" : "FAILED");
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

    // configure the interrupt endpoint; if this device is not a usable HID we
    // still keep it enumerated for DumpInfo, just without interrupt polling
    // (satoru).
    if (ConfigureDevice(idx, (const uint8_t*)&desc)) {
        // register keyboards with the keyboard driver so its per-device report
        // history is cleared (satoru).
        if (rt.hid_type == USB_HID_KEYBOARD) Keyboard::AddUSBDevice((uint8_t)((int)(S - g_xs) * USB_MAX_DEVICES + idx));
        ArmInterrupt(idx);
    }
    usb_log("USB: enumerated port %d -> slot %d hid_type=%d (0=none 1=kbd 2=mouse)\n",
            port, idx, (int)rt.hid_type);
    if (dev.dev_class == 0x09)
        usb_log("USB: HUB found on ctl %d port %d (vid %x pid %x) - hubs are NOT walked, nothing behind it is enumerated\n",
                (int)(S - g_xs), port, (uint32_t)dev.vendor_id, (uint32_t)dev.product_id);
    // usb audio: hand a device with an audio-class interface to the audio driver (satoru)
    if (rt.has_audio && g_audio_attach_fn)
        g_audio_attach_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, true);

    return true;
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
    return true;
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

// interrupt endpoints that errored while something else was waiting on the event ring. the
// next poll recovers them the normal way. (satoru)
static bool g_foreign_halt[USB_MAX_CTL][USB_MAX_DEVICES];

void USB::PollOne(int ctl) {
    if (!detected) return;

    // a synchronous bulk/bot wait owns the shared event ring right now: stay
    // out so we do not swallow its completion event. it re-arms + dispatches
    // hid reports it drains itself, so input stays alive. (satoru)
    if (g_usb_sync_claim) return;

    // endpoints that halted during this drain; recovered after it, because
    // SubmitCommand reads the same event ring and must not run mid-drain.
    // (satoru)
    int halted[8]; int nhalted = 0;
    for (int f = 0; f < USB_MAX_DEVICES && nhalted < 8; f++) {
        if (g_foreign_halt[ctl][f]) { g_foreign_halt[ctl][f] = false; halted[nhalted++] = f; }
    }
    static uint8_t errlog[USB_MAX_DEVICES];

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
            if (idx >= 0 && runtime[idx].iso_dci && runtime[idx].iso_dci == ep_id &&
                runtime[idx].intr_ep_id != ep_id) {
                // isoch audio transfer event: hand it to the audio driver (satoru)
                if (g_iso_event_fn)
                    g_iso_event_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, ev.parameter,
                                   (uint8_t)((ev.status >> 24) & 0xFF), ev.status & 0x00FFFFFF);
            } else if (idx >= 0 && runtime[idx].intr_ep_id == ep_id) {
                USBDeviceRuntime& rt = runtime[idx];
                uint8_t cc = (ev.status >> 24) & 0xFF;
                if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) S->intr_done[idx]++;
                else S->intr_err[idx]++;
                // residual = bytes NOT transferred; report length = requested -
                // residual (satoru).
                uint32_t residual = ev.status & 0x00FFFFFF;
                int got = rt.intr_buf_len - (int)residual;
                if (got < 0) got = 0;
                if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) {
                    if (got > 0) DispatchReport(idx, rt.intr_buf, got);
                    rt.intr_armed = false;
                    ArmInterrupt(idx);   // re-arm for the next report (satoru)
                } else {
                    // an error HALTS the endpoint (xhci 4.10.2.1): re-arming a
                    // halted endpoint does nothing, so one bad first transfer
                    // used to leave a device (the g502) silent for the whole
                    // boot. log it and recover after the drain, as linux does
                    // (xhci-ring.c: reset endpoint + set tr dequeue). (satoru)
                    if (errlog[idx] < 8) {
                        errlog[idx]++;
                        usb_log("USB: slot %d interrupt transfer error code %d (port %d)\n",
                                (int)slot, (int)cc, (int)rt.port);
                    }
                    rt.intr_armed = false;
                    bool dup = false;
                    for (int h = 0; h < nhalted; h++) if (halted[h] == idx) dup = true;
                    if (!dup && nhalted < 8) halted[nhalted++] = idx;
                }
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
                int existing = FindDeviceByPort((uint8_t)port);
                if (connected && existing < 0) {
                    EnumeratePort(port);            // hot-plug attach (satoru)
                } else if (!connected && existing >= 0) {
                    Keyboard::RemoveUSBDevice((uint8_t)((int)(S - g_xs) * USB_MAX_DEVICES + existing));
                    TeardownDevice(existing);       // hot-unplug detach (satoru)
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
    // dropping the failed td, then queue a fresh transfer. (satoru)
    for (int h = 0; h < nhalted; h++) {
        int idx = halted[h];
        USBDeviceRuntime& rt = runtime[idx];
        if (!rt.in_use || !rt.intr_ring) continue;
        static uint8_t resets[USB_MAX_DEVICES];
        if (resets[idx] >= 50) continue;   // give up on a device that keeps failing (satoru)
        resets[idx]++;
        xHCI_TRB c = {};
        c.control = TRB_TYPE(14) | ((uint32_t)rt.slot << 24) | ((uint32_t)rt.intr_ep_id << 16);
        xHCI_TRB r = {};
        bool ok1 = SubmitCommand(&c, &r) && ((r.status >> 24) & 0xFF) == TRB_CC_SUCCESS;
        xHCI_TRB d = {};
        d.parameter = (uint64_t)(uintptr_t)&rt.intr_ring[rt.intr_idx] | (rt.intr_cycle ? 1u : 0u);
        d.control = TRB_TYPE(16) | ((uint32_t)rt.slot << 24) | ((uint32_t)rt.intr_ep_id << 16);
        bool ok2 = SubmitCommand(&d, &r) && ((r.status >> 24) & 0xFF) == TRB_CC_SUCCESS;
        if (resets[idx] <= 4)
            usb_log("USB: slot %d endpoint reset %s, dequeue %s\n",
                    (int)rt.slot, ok1 ? "ok" : "FAILED", ok2 ? "ok" : "FAILED");
        ArmInterrupt(idx);
    }
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
        append("  Device "); append_num(i);
        append(": Port "); append_num(d.port);
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
            append("  Device "); append_num(i);
            append(": Port "); append_num(d.port);
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
    d.parameter = (uint64_t)(uintptr_t)&ring[ring_idx] | (cycle ? 1u : 0u);
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
    if (rt.iso_dci && rt.iso_dci == ep_id && rt.intr_ep_id != ep_id) {
        if (g_iso_event_fn)
            g_iso_event_fn((int)(S - g_xs) * USB_MAX_DEVICES + idx, ev.parameter, cc, ev.status & 0x00FFFFFF);
        return;
    }
    if (rt.intr_ring && rt.intr_ep_id == ep_id) {
        if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) {
            int got = rt.intr_buf_len - (int)(ev.status & 0x00FFFFFF);
            S->intr_done[idx]++;
            if (got > 0) DispatchReport(idx, rt.intr_buf, got);
            rt.intr_armed = false;
            ArmInterrupt(idx);
        } else {
            S->intr_err[idx]++;
            rt.intr_armed = false;
            g_foreign_halt[S - g_xs][idx] = true;
        }
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
            rt.intr_ep_id = 0; rt.intr_armed = false; rt.hid_type = USB_HID_NONE;
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
        ring[USB_ISO_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)ring;
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

        uint32_t* icc = InputControlCtx(idx);
        icc[0] = 0;
        icc[1] = (1u << 0) | (1u << dci);
        uint32_t* epc = EndpointCtx(idx, dci);
        usb_memset(epc, 0, ctx_stride);
        epc[0] = (xint << 16) | (mult << 8);
        // CErr 0 for isoch, EPType 1 = isoch out, 5 = isoch in (satoru)
        epc[1] = (0u << 1) | ((in ? 5u : 1u) << 3) | (burst << 8) | ((uint32_t)p.max_packet << 16);
        uint64_t deq = (uint64_t)(uintptr_t)ring | 1;
        epc[2] = (uint32_t)deq;
        epc[3] = (uint32_t)(deq >> 32);
        epc[4] = (avg & 0xFFFF) | ((esit & 0xFFFF) << 16);
        uint32_t* slot_ctx = SlotCtx(idx);
        uint32_t ents = (slot_ctx[0] >> 27) & 0x1F;
        if (ents < dci) ents = dci;
        slot_ctx[0] = (slot_ctx[0] & ~((uint32_t)0x1F << 27)) | (ents << 27);

        xHCI_TRB cmd = {};
        cmd.parameter = (uint64_t)(uintptr_t)rt.input_ctx;
        cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
        xHCI_TRB res = {};
        bool sent = SubmitCommand(&cmd, &res);
        uint8_t cc = (uint8_t)((res.status >> 24) & 0xFF);
        usb_log("USB: iso open slot %d ep %x dci %d mps %d burst %d mult %d esit %d interval-field %d (%d us): configure cc %d\n",
                (int)rt.slot, (uint32_t)p.ep_addr, (int)dci, (int)p.max_packet, (int)burst, (int)mult,
                (int)esit, (int)xint, (int)period_us, (int)cc);
        if (!sent || cc != TRB_CC_SUCCESS) { PMM::FreeBytes(ring, 4096); break; }
        rt.iso_ring = ring; rt.iso_idx = 0; rt.iso_cycle = true;
        rt.iso_dci = dci; rt.iso_ep_addr = p.ep_addr;
        if (out_period_us) *out_period_us = period_us;
        ok = true;
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
    if (rt.iso_dci) {
        uint8_t dci = rt.iso_dci;
        rt.iso_dci = 0;                                          // events for it are ignored from here (satoru)
        xHCI_TRB st = {};
        st.control = TRB_TYPE(15) | ((uint32_t)rt.slot << 24) | ((uint32_t)dci << 16);
        xHCI_TRB r = {};
        SubmitCommand(&st, &r);
        uint32_t* icc = InputControlCtx(idx);
        icc[0] = (1u << dci);
        icc[1] = (1u << 0);
        xHCI_TRB cmd = {};
        cmd.parameter = (uint64_t)(uintptr_t)rt.input_ctx;
        cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
        SubmitCommand(&cmd, &r);
        usb_log("USB: iso close slot %d dci %d: drop cc %d\n", (int)rt.slot, (int)dci, (int)((r.status >> 24) & 0xFF));
        if (rt.iso_ring) PMM::FreeBytes(rt.iso_ring, 4096);
        rt.iso_ring = nullptr;
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
    t->parameter = (uint64_t)(uintptr_t)data;
    t->status = len & 0x1FFFF;                                   // td size 0, interrupter 0 (satoru)
    __asm__ __volatile__("" ::: "memory");
    t->control = TRB_TYPE(TRB_ISOCH) | TRB_ISO_SIA | (ioc ? TRB_IOC : 0) | (rt.iso_cycle ? TRB_CYCLE : 0);
    rt.iso_idx++;
    if (rt.iso_idx >= USB_ISO_RING_TRBS - 1) {
        rt.iso_ring[USB_ISO_RING_TRBS - 1].parameter = (uint64_t)(uintptr_t)rt.iso_ring;
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

// [kbdiag] usb half: every hid device that got an interrupt endpoint, whether
// it is bound as a keyboard, and how many interrupt transfers ever completed.
// zero completions on a keyboard that was typed on = the endpoint never ran. (satoru)
void USB::LogKbDiag() {
    int nkb = 0;
    for (int c = 0; c < g_nctl; c++) {
        XhciState* st = &g_xs[c];
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            USBDeviceRuntime& rt = st->x_runtime[i];
            if (!rt.in_use || !rt.intr_ep_id) continue;
            if (rt.hid_type == USB_HID_KEYBOARD) nkb++;
            usb_log("[kbdiag] usb ctl %d dev %d port %d slot %d vid:pid %x:%x hid=%s intr_done %d intr_err %d armed %d\n",
                    c, i, (int)rt.port, (int)rt.slot,
                    (uint32_t)st->x_devices[i].vendor_id, (uint32_t)st->x_devices[i].product_id,
                    rt.hid_type == USB_HID_KEYBOARD ? "keyboard" : (rt.hid_type == USB_HID_MOUSE ? "mouse" : "other"),
                    (int)st->intr_done[i], (int)st->intr_err[i], rt.intr_armed ? 1 : 0);
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
