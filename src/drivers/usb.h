#pragma once
//  kurono os - usb (xhci) host controller driver
//  supports usb 3.0/2.0/1.1 via xhci (extensible host controller interface)
#include "../kernel/types.h"

#define XHCI_CAP_CAPLENGTH   0x00
#define XHCI_CAP_HCIVERSION  0x02
#define XHCI_CAP_HCSPARAMS1  0x04
#define XHCI_CAP_HCSPARAMS2  0x08
#define XHCI_CAP_HCSPARAMS3  0x0C
#define XHCI_CAP_HCCPARAMS1  0x10
#define XHCI_CAP_DBOFF       0x14
#define XHCI_CAP_RTSOFF      0x18

#define XHCI_OP_USBCMD       0x00
#define XHCI_OP_USBSTS       0x04
#define XHCI_OP_DNCTRL       0x14
#define XHCI_OP_CRCR         0x18
#define XHCI_OP_DCBAAP       0x30
#define XHCI_OP_CONFIG       0x38
#define XHCI_OP_PORTSC(n)    (0x400 + (n) * 0x10)

#define XHCI_CMD_RUN          (1 << 0)
#define XHCI_CMD_HCRESET      (1 << 1)
#define XHCI_CMD_INTE         (1 << 2)

#define XHCI_STS_HCH          (1 << 0)
#define XHCI_STS_HSE           (1 << 2)
#define XHCI_STS_CNR           (1 << 11)

#define XHCI_PORTSC_CCS       (1 << 0)   // current connect status
#define XHCI_PORTSC_PED       (1 << 1)   // port enabled
#define XHCI_PORTSC_PR        (1 << 4)   // port reset
#define XHCI_PORTSC_PLS_MASK  (0xF << 5) // port link state
#define XHCI_PORTSC_PP        (1 << 9)   // port power
#define XHCI_PORTSC_SPEED(x)  (((x) >> 10) & 0xF)

#define USB_SPEED_FULL   1
#define USB_SPEED_LOW    2
#define USB_SPEED_HIGH   3
#define USB_SPEED_SUPER  4

#define TRB_NORMAL        1
#define TRB_SETUP_STAGE   2
#define TRB_DATA_STAGE    3
#define TRB_STATUS_STAGE  4
#define TRB_LINK          6
#define TRB_EVENT_DATA    7
#define TRB_NOOP          8
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_TRANSFER_EVT  32
#define TRB_CMD_COMPLETE  33
#define TRB_PORT_STATUS   34

// transfer request block control-field helpers (satoru)
#define TRB_TYPE(t)        ((uint32_t)(t) << 10)   // bits 15:10 = trb type
#define TRB_CYCLE          (1u << 0)               // cycle bit
#define TRB_ENT            (1u << 1)               // evaluate-next-trb / toggle-cycle on link
#define TRB_ISP            (1u << 2)               // interrupt-on-short-packet
#define TRB_CHAIN          (1u << 4)               // chain bit
#define TRB_IOC            (1u << 5)               // interrupt-on-completion
#define TRB_IDT            (1u << 6)               // immediate data (setup stage)
#define TRB_DIR_IN         (1u << 16)              // data/status stage direction = device->host
#define TRB_TRT_NO_DATA    (0u << 16)              // setup: no data stage
#define TRB_TRT_OUT_DATA   (2u << 16)              // setup: out data stage
#define TRB_TRT_IN_DATA    (3u << 16)              // setup: in data stage

// completion codes reported in event trb status[31:24] (satoru)
#define TRB_CC_SUCCESS     1
#define TRB_CC_SHORT_PKT   13

// erdp event-handler-busy bit (satoru)
#define XHCI_ERDP_EHB      (1u << 3)

// standard usb request / descriptor constants (satoru)
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_SET_INTERFACE   0x0B
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_HID            0x21
#define USB_DESC_REPORT         0x22
// hid class-specific requests on the interface (satoru)
#define HID_REQ_SET_PROTOCOL    0x0B
#define HID_PROTO_BOOT          0
#define HID_PROTO_REPORT        1

// kurono hid device categories used by the poll dispatcher (satoru)
#define USB_HID_NONE       0
#define USB_HID_KEYBOARD   1
#define USB_HID_MOUSE      2

#define USB_MAX_DEVICES    16
#define USB_MAX_PORTS      16

// transfer ring length (in trbs) for ep0 and interrupt endpoints (satoru)
#define USB_RING_TRBS      64
// largest hid boot report we re-arm a buffer for (satoru)
#define USB_HID_REPORT_MAX 64

struct xHCI_TRB {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

struct USBDeviceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct USBDeviceInfo {
    bool     connected;
    uint8_t  port;
    uint8_t  slot;
    uint8_t  speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    char     manufacturer[32];
    char     product[32];
};

// per-slot runtime state needed for control + interrupt transfers (satoru).
// allocated lazily during enumeration; one entry per devices[] index so the
// poll dispatcher and hot-plug teardown can recover everything by index.
struct USBDeviceRuntime {
    bool        in_use;
    uint8_t     slot;
    uint8_t     port;
    uint8_t     speed;
    uint8_t     hid_type;          // USB_HID_KEYBOARD / _MOUSE / _NONE (satoru)

    // device context (output) + input context (for address/config commands).
    // xhci writes the device context; we own the input context. each is a
    // 4 kb page so a 64-byte-context controller and the dcbaa entry are happy.
    uint8_t*    device_ctx;
    uint8_t*    input_ctx;

    // ep0 (control) transfer ring.
    xHCI_TRB*   ep0_ring;
    int         ep0_idx;
    bool        ep0_cycle;

    // interrupt-in transfer ring + its completion buffer.
    xHCI_TRB*   intr_ring;
    int         intr_idx;
    bool        intr_cycle;
    uint8_t*    intr_buf;
    int         intr_buf_len;      // bytes we re-arm per interrupt transfer (satoru)
    uint8_t     intr_ep_addr;      // endpoint address incl. 0x80 in-direction (satoru)
    uint8_t     intr_ep_id;        // xhci dci / doorbell target (satoru)
    uint8_t     intr_interval;     // bInterval from the endpoint descriptor (satoru)
    bool        intr_armed;        // a Normal trb is queued and awaiting completion (satoru)
};

class USB {
public:
    static bool Init();
    static bool IsDetected();
    static int  GetPortCount();
    static int  GetDeviceCount();

    // port operations
    static bool IsPortConnected(int port);
    static uint8_t GetPortSpeed(int port);
    static bool ResetPort(int port);
    static const char* SpeedName(uint8_t speed);

    // device operations
    static const USBDeviceInfo* GetDevice(int index);
    static bool EnumerateDevices();

    // control transfers
    static bool ControlTransfer(int device, uint8_t bmRequestType, uint8_t bRequest,
                                uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                void* data);

    // bulk transfers
    static bool BulkRead(int device, uint8_t endpoint, void* buffer, int length);
    static bool BulkWrite(int device, uint8_t endpoint, const void* buffer, int length);

    // hid interrupt polling - call periodically (e.g. from the input poll
    // loop, ~1 khz) to drain transfer-completion + port-change events and
    // dispatch reports to the keyboard / mouse drivers (satoru).
    static void PollHID();

    // native usb hid input toggle (settings -> devices). when off, enumerated
    // usb keyboards/mice/tablets still stay alive but their reports are not
    // forwarded to the input stack, so the ps/2 path takes over. (satoru)
    static void SetHIDInputEnabled(bool on);
    static bool IsHIDInputEnabled();

    static void DumpInfo(char* out, int max_len);

private:
    static bool detected;
    static volatile uint8_t* bar0;
    static uint32_t cap_length;
    static uint32_t max_ports;
    static uint32_t max_slots;

    static USBDeviceInfo devices[USB_MAX_DEVICES];
    static USBDeviceRuntime runtime[USB_MAX_DEVICES];
    static int device_count;

    // context geometry: 64 bytes unless HCCPARAMS1.CSZ says 64-bit contexts,
    // in which case every context is 64 bytes already; ctx_stride is the byte
    // distance between consecutive contexts in the input/device context block.
    static uint32_t ctx_stride;
    static volatile uint8_t* erst_erdp_seg; // current erdp segment base (satoru)

    // ring buffers
    static xHCI_TRB* cmd_ring;
    static xHCI_TRB* event_ring;
    static uint64_t* dcbaa;
    static int cmd_ring_idx;
    static bool cmd_cycle;
    static int event_ring_idx;
    static bool event_ccs;

    static uint32_t ReadCap(uint32_t offset);
    static uint32_t ReadOp(uint32_t offset);
    static void WriteOp(uint32_t offset, uint32_t val);
    static uint32_t ReadPort(int port);
    static void WritePort(int port, uint32_t val);
    static uint32_t ReadRuntime(uint32_t offset);
    static void WriteRuntime(uint32_t offset, uint32_t val);
    static void WriteRuntime64(uint32_t offset, uint64_t val);
    static void WriteDoorbell(int slot, uint32_t val);

    static bool Reset();
    static bool WaitReady(int timeout_ms);
    static bool SubmitCommand(xHCI_TRB* cmd, xHCI_TRB* result);
    static bool PollEventRing(xHCI_TRB* result, int timeout);
    static void UpdateERDP();   // advance dequeue ptr after consuming events (satoru)

    // enumeration / configuration helpers (satoru)
    static bool AddressDevice(int idx);
    static bool ConfigureDevice(int idx, const uint8_t* dev_desc);
    static bool EnumeratePort(int port);
    static void TeardownDevice(int idx);
    static int  FindDeviceBySlot(uint8_t slot);
    static int  FindDeviceByPort(uint8_t port);

    // ep0 control + interrupt transfer-ring primitives (satoru)
    static bool ControlTransferEP(int idx, uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                  void* data);
    static void ArmInterrupt(int idx);
    static void DispatchReport(int idx, const uint8_t* report, int len);
    static void ParseReportDescriptor(int idx, const uint8_t* rpt, int len);

    // input-context accessors: stride-correct pointers into input_ctx (satoru)
    static uint32_t* InputControlCtx(int idx);   // input control context (satoru)
    static uint32_t* SlotCtx(int idx);           // slot context within input_ctx
    static uint32_t* EndpointCtx(int idx, int dci); // endpoint context for dci
};
