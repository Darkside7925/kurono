//  kurono os  -  usb (xhci) host controller driver implementation
//  pci class 0c:03:30 (xhci usb 3.0)
#include "usb.h"
#include "../kernel/pci.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"
#include "../drivers/serial.h"
#include <stdarg.h>

bool USB::detected = false;
volatile uint8_t* USB::bar0 = nullptr;
uint32_t USB::cap_length = 0;
uint32_t USB::max_ports = 0;
uint32_t USB::max_slots = 0;

USBDeviceInfo USB::devices[USB_MAX_DEVICES];
int USB::device_count = 0;

xHCI_TRB* USB::cmd_ring = nullptr;
xHCI_TRB* USB::event_ring = nullptr;
uint64_t* USB::dcbaa = nullptr;
int USB::cmd_ring_idx = 0;
int USB::event_ring_idx = 0;
bool USB::event_ccs = true;

static void usb_log(const char* fmt, ...) {
    (void)fmt;
    // serial debug output could be added here
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

    // copy trb to command ring
    cmd_ring[cmd_ring_idx] = *cmd_trb;
    cmd_ring[cmd_ring_idx].control |= 1; // cycle bit

    cmd_ring_idx++;
    if (cmd_ring_idx >= 63) {
        // link trb to wrap around
        cmd_ring[63].parameter = (uint64_t)(uintptr_t)cmd_ring;
        cmd_ring[63].status = 0;
        cmd_ring[63].control = (TRB_LINK << 10) | (1 << 1) | 1; // toggle cycle
        cmd_ring_idx = 0;
    }

    // ring the command doorbell (slot 0, target 0)
    WriteDoorbell(0, 0);

    // poll for completion
    return PollEventRing(result, 5000);
}

bool USB::PollEventRing(xHCI_TRB* result, int timeout) {
    for (int i = 0; i < timeout * 100; i++) {
        xHCI_TRB* evt = &event_ring[event_ring_idx];
        bool cycle = evt->control & 1;

        if (cycle == event_ccs) {
            if (result) *result = *evt;

            event_ring_idx++;
            if (event_ring_idx >= 64) {
                event_ring_idx = 0;
                event_ccs = !event_ccs;
            }

            // acknowledge by updating erdp
            // writeruntime(0x38, (uint64_t)(uintptr_t)evt | (1 << 3));

            return true;
        }
        for (volatile int d = 0; d < 1000; d++);
    }
    return false;
}

bool USB::Init() {
    detected = false;
    device_count = 0;

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        devices[i].connected = false;
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

    if (max_ports > USB_MAX_PORTS) max_ports = USB_MAX_PORTS;

    // reset controller
    if (!Reset()) {
        usb_log("USB: Controller reset failed\n");
        return false;
    }

    // set max device slots
    WriteOp(XHCI_OP_CONFIG, max_slots);

    // allocate device context base address array (dcbaa)
    dcbaa = (uint64_t*)KernelHeap::Alloc(4096);
    if (!dcbaa) return false;
    for (int i = 0; i < 256; i++) dcbaa[i] = 0;
    WriteOp(XHCI_OP_DCBAAP, (uint32_t)(uintptr_t)dcbaa);
    WriteOp(XHCI_OP_DCBAAP + 4, 0);

    // allocate command ring (64 trbs)
    cmd_ring = (xHCI_TRB*)KernelHeap::Alloc(4096);
    if (!cmd_ring) return false;
    for (int i = 0; i < 64; i++) {
        cmd_ring[i].parameter = 0;
        cmd_ring[i].status = 0;
        cmd_ring[i].control = 0;
    }
    cmd_ring_idx = 0;

    // set crcr (command ring control register)
    uint64_t crcr_val = (uint64_t)(uintptr_t)cmd_ring | 1; // rcs=1
    WriteOp(XHCI_OP_CRCR, (uint32_t)crcr_val);
    WriteOp(XHCI_OP_CRCR + 4, (uint32_t)(crcr_val >> 32));

    // allocate event ring (64 trbs)
    event_ring = (xHCI_TRB*)KernelHeap::Alloc(4096);
    if (!event_ring) return false;
    for (int i = 0; i < 64; i++) {
        event_ring[i].parameter = 0;
        event_ring[i].status = 0;
        event_ring[i].control = 0;
    }
    event_ring_idx = 0;
    event_ccs = true;

    // set up event ring segment table
    // erst entry: base address (8 bytes) + ring segment size (4 bytes) + reserved (4 bytes)
    uint64_t* erst = (uint64_t*)KernelHeap::Alloc(4096);
    if (!erst) return false;
    erst[0] = (uint64_t)(uintptr_t)event_ring;
    ((uint32_t*)erst)[2] = 64; // segment size
    ((uint32_t*)erst)[3] = 0;  // reserved

    // write erstsz and erstba to interrupter 0
    WriteRuntime(0x28, 1);  // erstsz = 1 segment
    WriteRuntime(0x30, (uint32_t)(uintptr_t)erst);
    WriteRuntime(0x34, 0);

    // set erdp
    WriteRuntime(0x38, (uint32_t)(uintptr_t)event_ring);
    WriteRuntime(0x3C, 0);

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

bool USB::EnumerateDevices() {
    if (!detected) return false;
    device_count = 0;

    for (int port = 0; port < (int)max_ports && device_count < USB_MAX_DEVICES; port++) {
        if (!IsPortConnected(port)) continue;

        uint8_t speed = GetPortSpeed(port);
        if (speed == 0) continue;

        // reset port
        ResetPort(port);

        // enable slot
        xHCI_TRB enable_cmd = {};
        enable_cmd.control = (TRB_ENABLE_SLOT << 10);
        xHCI_TRB result = {};

        if (!SubmitCommand(&enable_cmd, &result)) continue;

        uint8_t slot = (result.control >> 24) & 0xFF;
        if (slot == 0) continue;

        USBDeviceInfo& dev = devices[device_count];
        dev.connected = true;
        dev.port = port;
        dev.slot = slot;
        dev.speed = speed;
        dev.vendor_id = 0;
        dev.product_id = 0;
        dev.dev_class = 0;
        dev.dev_subclass = 0;

        // try to read device descriptor via get_descriptor
        USBDeviceDescriptor desc = {};
        if (ControlTransfer(device_count, 0x80, 0x06, 0x0100, 0, 18, &desc)) {
            dev.vendor_id = desc.idVendor;
            dev.product_id = desc.idProduct;
            dev.dev_class = desc.bDeviceClass;
            dev.dev_subclass = desc.bDeviceSubClass;
        }

        // set product name based on class
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

        // copy class name as product
        int j = 0;
        while (class_name[j] && j < 31) {
            dev.product[j] = class_name[j];
            j++;
        }
        dev.product[j] = 0;
        dev.manufacturer[0] = 0;

        device_count++;
    }

    return true;
}

const USBDeviceInfo* USB::GetDevice(int index) {
    if (index < 0 || index >= device_count) return nullptr;
    return &devices[index];
}

bool USB::ControlTransfer(int device, uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void* data) {
    if (device < 0 || device >= device_count) return false;
    (void)bmRequestType; (void)bRequest; (void)wValue;
    (void)wIndex; (void)wLength; (void)data;

    // full control transfer implementation would set up:
    // 1. setup stage trb with bmrequesttype, brequest, wvalue, windex, wlength
    // 2. data stage trb(s) for wlength bytes
    // 3. status stage trb
    // then ring the endpoint doorbell for endpoint 0

    return false; // stub  -  requires full endpoint context setup
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
