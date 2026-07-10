//  kurono os - usb (xhci) host controller driver implementation
//  pci class 0c:03:30 (xhci usb 3.0)
#include "usb.h"
#include "../kernel/pci.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"   // ProcessUSBReport for keyboard hid (satoru)
#include "../drivers/mouse.h"      // ProcessUSBReport for mouse hid (satoru)
#include <stdarg.h>

bool USB::detected = false;
volatile uint8_t* USB::bar0 = nullptr;
uint32_t USB::cap_length = 0;
uint32_t USB::max_ports = 0;
uint32_t USB::max_slots = 0;

USBDeviceInfo USB::devices[USB_MAX_DEVICES];
USBDeviceRuntime USB::runtime[USB_MAX_DEVICES];
int USB::device_count = 0;

uint32_t USB::ctx_stride = 32;        // re-derived from HCCPARAMS1.CSZ in Init (satoru)
volatile uint8_t* USB::erst_erdp_seg = nullptr;

xHCI_TRB* USB::cmd_ring = nullptr;
xHCI_TRB* USB::event_ring = nullptr;
uint64_t* USB::dcbaa = nullptr;
int USB::cmd_ring_idx = 0;
bool USB::cmd_cycle = true;
int USB::event_ring_idx = 0;
bool USB::event_ccs = true;

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
    for (int tries = 0; tries < 16; tries++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 5000)) return false;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type == TRB_CMD_COMPLETE) { if (result) *result = ev; return true; }
        // not a command completion (port status / stray transfer) - keep waiting.
    }
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

bool USB::Init() {
    detected = false;
    device_count = 0;

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        devices[i].connected = false;
        usb_memset(&runtime[i], 0, sizeof(runtime[i]));
    }

    // scan pci bus for usb xhci controller (class 0c:03:30)
    uint32_t found_bar0 = 0;
    uint8_t found_bus = 0, found_dev = 0, found_func = 0;
    bool found = false;

    for (int bus = 0; bus < 256 && !found; bus++) {
        for (int dev = 0; dev < 32 && !found; dev++) {
            for (int func = 0; func < 8 && !found; func++) {
                uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8);

                outl(0xCF8, addr | 0x00);
                uint32_t vendor_device = inl(0xCFC);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                outl(0xCF8, addr | 0x08);
                uint32_t class_reg = inl(0xCFC);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class = (class_reg >> 16) & 0xFF;
                uint8_t prog_if = (class_reg >> 8) & 0xFF;

                // xhci: class=0c, subclass=03, progif=30
                if (base_class == 0x0C && sub_class == 0x03 && prog_if == 0x30) {
                    outl(0xCF8, addr | 0x10);
                    found_bar0 = inl(0xCFC) & ~0xF;
                    found_bus = bus;
                    found_dev = dev;
                    found_func = func;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        usb_log("USB: No xHCI controller found\n");
        return false;
    }

    bar0 = (volatile uint8_t*)(uintptr_t)found_bar0;

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

    if (max_ports > USB_MAX_PORTS) max_ports = USB_MAX_PORTS;

    // reset controller
    if (!Reset()) {
        usb_log("USB: Controller reset failed\n");
        return false;
    }

    // set max device slots
    WriteOp(XHCI_OP_CONFIG, max_slots);

    // allocate device context base address array (dcbaa)
    dcbaa = (uint64_t*)usb_alloc_aligned(4096, 4096);
    if (!dcbaa) return false;
    for (int i = 0; i < 256; i++) dcbaa[i] = 0;
    WriteOp(XHCI_OP_DCBAAP, (uint32_t)(uintptr_t)dcbaa);
    WriteOp(XHCI_OP_DCBAAP + 4, 0);

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
    usb_log("USB: xHCI initialized, %d ports, %d max slots\n", max_ports, max_slots);

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
        case USB_SPEED_FULL:  return 8;   // could be 8/16/32/64; refined after 8-byte read (satoru)
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

    int p = 0;
    while (p + 2 <= (int)total_len) {
        uint8_t len = cfg[p];
        uint8_t type = cfg[p + 1];
        if (len == 0) break; // malformed; bail rather than spin (satoru)

        if (type == 0x04 && p + 9 <= (int)total_len) {            // interface
            uint8_t iclass = cfg[p + 5];
            uint8_t isub   = cfg[p + 6];
            if (iclass == 0x03) {                                  // HID class (satoru)
                in_hid_iface = true;
                hid_iface_num = cfg[p + 2];
                hid_boot = (isub == 0x01);                         // boot subclass (satoru)
                rt.hid_type = USB_HID_NONE;
            } else {
                in_hid_iface = false;
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
        }
        p += len;
    }

    if (!found_ep) return false;

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

    // SET_CONFIGURATION(config_value) - no data stage (satoru).
    if (!ControlTransferEP(idx, 0x00, USB_REQ_SET_CONFIG, config_value, 0, 0, nullptr))
        return false;

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

    // Configure Endpoint: input control context adds the interrupt ep (A bit at
    // its dci) and keeps the slot context (A0); slot context entries grows to
    // cover the new dci (satoru).
    uint32_t* icc = InputControlCtx(idx);
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << rt.intr_ep_id);
    uint32_t* slot_ctx = SlotCtx(idx);
    // context entries must be the highest valid dci (satoru).
    slot_ctx[0] = (slot_ctx[0] & ~((uint32_t)0x1F << 27)) | ((uint32_t)rt.intr_ep_id << 27);

    // interrupt-IN endpoint context: EPType 7 (interrupt-in), CErr=3,
    // interval copied from the descriptor, max packet, TR dequeue = intr ring
    // with DCS=1, plus a sane max-burst/ESIT hint (satoru).
    uint32_t* epc = EndpointCtx(idx, rt.intr_ep_id);
    usb_memset(epc, 0, ctx_stride);
    epc[0] = ((uint32_t)ep_interval << 16);                       // Interval (satoru)
    epc[1] = (3u << 1) | (7u << 3) | ((uint32_t)ep_mps << 16);    // CErr=3, EPType=IntrIn, MPS
    uint64_t intr_deq = (uint64_t)(uintptr_t)rt.intr_ring | 1;    // DCS=1 (satoru)
    epc[2] = (uint32_t)(intr_deq & 0xFFFFFFFF);
    epc[3] = (uint32_t)(intr_deq >> 32);
    epc[4] = ep_mps;                                             // avg trb len / max esit hint (satoru)

    xHCI_TRB cmd = {};
    cmd.parameter = (uint64_t)(uintptr_t)rt.input_ctx;
    cmd.control = TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)rt.slot << 24);
    xHCI_TRB result = {};
    if (!SubmitCommand(&cmd, &result)) return false;
    if (((result.status >> 24) & 0xFF) != TRB_CC_SUCCESS) return false;

    // for boot-capable HID, ask the device to speak the fixed boot report
    // layout so our byte0/byte1/byte2 decode is valid (satoru).
    if (hid_boot) {
        // SET_PROTOCOL(boot) - class request to the interface, no data (satoru).
        ControlTransferEP(idx, 0x21, HID_REQ_SET_PROTOCOL, HID_PROTO_BOOT,
                          hid_iface_num, 0, nullptr);
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
    for (int tries = 0; tries < 8; tries++) {
        xHCI_TRB ev = {};
        if (!PollEventRing(&ev, 5000)) return false;
        uint8_t type = (ev.control >> 10) & 0x3F;
        if (type == TRB_TRANSFER_EVT) {
            uint8_t cc = (ev.status >> 24) & 0xFF;
            return cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT;
        }
        // not ours (port status / command completion) - keep waiting (satoru).
    }
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
    if (!g_usb_hid_input) return;   // usb input disabled -> ps/2 takes over (satoru)
    USBDeviceRuntime& rt = runtime[idx];
    if (rt.hid_type == USB_HID_KEYBOARD) {
        // boot keyboard report is 8 bytes; the keyboard decoder requires >= 8
        // and indexes by device id (we use the devices[] index) (satoru).
        Keyboard::ProcessUSBReport((uint8_t)idx, report, (size_t)len);
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
        if (rt.hid_type == USB_HID_KEYBOARD) Keyboard::AddUSBDevice((uint8_t)idx);
        ArmInterrupt(idx);
    }
    usb_log("USB: enumerated port %d -> slot %d hid_type=%d (0=none 1=kbd 2=mouse)\n",
            port, idx, (int)rt.hid_type);

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
    if (!detected) return;

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
            if (idx >= 0 && runtime[idx].intr_ep_id == ep_id) {
                USBDeviceRuntime& rt = runtime[idx];
                uint8_t cc = (ev.status >> 24) & 0xFF;
                // residual = bytes NOT transferred; report length = requested -
                // residual (satoru).
                uint32_t residual = ev.status & 0x00FFFFFF;
                int got = rt.intr_buf_len - (int)residual;
                if (got < 0) got = 0;
                if ((cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) && got > 0)
                    DispatchReport(idx, rt.intr_buf, got);
                rt.intr_armed = false;
                ArmInterrupt(idx);   // re-arm for the next report (satoru)
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
                    Keyboard::RemoveUSBDevice((uint8_t)existing);
                    TeardownDevice(existing);       // hot-unplug detach (satoru)
                }
            }
        }
        // command-completion events are consumed by SubmitCommand's own poll;
        // any that land here are stale and safely ignored (satoru).
    }

    // tell the controller how far we have consumed (satoru).
    UpdateERDP();
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

bool USB::BulkRead(int device, uint8_t endpoint, void* buffer, int length) {
    if (device < 0 || device >= device_count) return false;
    (void)endpoint; (void)buffer; (void)length;
    return false; // stub
}

bool USB::BulkWrite(int device, uint8_t endpoint, const void* buffer, int length) {
    if (device < 0 || device >= device_count) return false;
    (void)endpoint; (void)buffer; (void)length;
    return false; // stub
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
        append("\n");
    }

    out[pos] = 0;
}
