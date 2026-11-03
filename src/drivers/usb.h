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

// mass-storage class bulk-only transport (bot) constants (satoru)
#define USB_MSC_CLASS      0x08
#define USB_MSC_PROTO_BOT  0x50
#define USB_MSC_REQ_RESET  0xFF   // bulk-only mass storage reset (satoru)
#define USB_MSC_REQ_GETLUN 0xFE   // get max lun (satoru)
#define USB_CBW_SIG        0x43425355u   // "USBC" (satoru)
#define USB_CSW_SIG        0x53425355u   // "USBS" (satoru)
// bounce buffer for bulk data stages: 32kb aligned to 32kb so a single normal
// trb never crosses a 64kb boundary (xhci data-buffer rule) (satoru)
#define USB_BULK_BOUNCE    32768

#define USB_MAX_DEVICES    16
#define USB_MAX_PORTS      32   // pch xhci on the owner laptop has 26 (satoru)

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

    // bulk in/out endpoint state for mass-storage (bot) devices. rings are
    // allocated only when the config descriptor exposes a bulk pair. (satoru)
    xHCI_TRB*   bulk_in_ring;
    int         bulk_in_idx;
    bool        bulk_in_cycle;
    xHCI_TRB*   bulk_out_ring;
    int         bulk_out_idx;
    bool        bulk_out_cycle;
    uint8_t     bulk_in_ep;        // endpoint address (0x8N) (satoru)
    uint8_t     bulk_out_ep;       // endpoint address (0x0N) (satoru)
    uint8_t     bulk_in_dci;       // xhci dci: in ep N -> 2N+1 (satoru)
    uint8_t     bulk_out_dci;      // xhci dci: out ep N -> 2N (satoru)
    uint16_t    bulk_in_mps;
    uint16_t    bulk_out_mps;
    uint8_t*    bulk_bounce;       // 32kb/32kb-aligned dma bounce buffer (satoru)

    // bot / scsi state (valid when is_msd) (satoru)
    bool        is_msd;
    bool        msd_ready;         // inquiry + read-capacity succeeded (satoru)
    uint8_t     msd_lun;
    uint32_t    msd_tag;           // cbw tag counter (satoru)
    uint64_t    msd_blocks;        // total lba count from read capacity (satoru)
    uint32_t    msd_block_size;    // bytes per lba (satoru)

    // usb audio: set while parsing the first 256 bytes of the config when an
    // audio-class interface is seen (the audio driver then does its own full
    // descriptor read). isoch endpoint state is owned by IsoOpen/IsoClose. (satoru)
    bool        has_audio;
    xHCI_TRB*   iso_ring;          // USB_ISO_RING_TRBS trbs, last one is the link (satoru)
    int         iso_idx;           // next trb to fill (satoru)
    bool        iso_cycle;
    uint8_t     iso_dci;           // 0 = no isoch endpoint configured (satoru)
    uint8_t     iso_ep_addr;
    uint8_t     cfg_value;         // configuration last selected with SET_CONFIGURATION, 0 = none (satoru)
};

// isoch transfer ring: one page = 256 trbs (255 usable + link) (satoru)
#define USB_ISO_RING_TRBS  256
#define TRB_ISOCH          5
#define TRB_ISO_SIA        (1u << 31)   // start isoch asap (satoru)
#define USB_MAX_CTL        4            // xhci controllers driven at once (satoru)

// everything one xhci controller owns. the usb driver used to hold this as
// static members (one controller); it now lives in one of these per controller
// and USB::S points at the one being operated on. controller 0 is the one the
// driver always drove (most root ports); extra controllers are brought up after
// it and polled in turn. (satoru)
struct XhciState {
    bool x_detected;
    volatile uint8_t* x_bar0;
    uint32_t x_cap_length;
    uint32_t x_max_ports;
    uint32_t x_max_slots;
    USBDeviceInfo x_devices[USB_MAX_DEVICES];
    USBDeviceRuntime x_runtime[USB_MAX_DEVICES];
    int x_device_count;
    uint32_t x_ctx_stride;
    volatile uint8_t* x_erst_erdp_seg;
    xHCI_TRB* x_cmd_ring;
    xHCI_TRB* x_event_ring;
    uint64_t* x_dcbaa;
    int x_cmd_ring_idx;
    bool x_cmd_cycle;
    int x_event_ring_idx;
    bool x_event_ccs;
    // pci identity, for logs (satoru)
    uint8_t  pci_bus, pci_dev, pci_func;
    uint16_t pci_vid, pci_did;
    uint32_t intr_done[USB_MAX_DEVICES];   // interrupt-in transfers that completed (kbdiag) (satoru)
    uint32_t intr_err[USB_MAX_DEVICES];
};

// what the usb audio class driver needs to know about one isoch endpoint (satoru)
struct UsbIsoParams {
    uint8_t  ep_addr;          // 0x0N = out (satoru)
    uint16_t max_packet;       // wMaxPacketSize bits 10:0
    uint8_t  burst;            // hs: additional transactions per uframe; ss: bMaxBurst
    uint8_t  mult;             // ss companion mult
    uint8_t  b_interval;       // raw bInterval
    uint32_t bytes_per_interval; // max esit payload
    uint32_t avg_bytes;        // typical td length
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

    // bulk transfers. endpoint is the usb endpoint ADDRESS (0x8N = in, 0x0N =
    // out) and must match one of the device's configured bulk endpoints.
    // synchronous: builds a normal trb, rings the doorbell and waits for the
    // transfer event. out_transferred (optional) reports the actual byte
    // count (short packets are legal on bulk-in). (satoru)
    static bool BulkRead(int device, uint8_t endpoint, void* buffer, int length,
                         int* out_transferred = nullptr);
    static bool BulkWrite(int device, uint8_t endpoint, const void* buffer, int length);

    // mass-storage (bulk-only transport, scsi) - populated automatically when
    // a class-08 protocol-50 interface is configured during enumeration. a
    // usb stick shows up here as a readable block device. (satoru)
    static int      MsdFirstDevice();            // first ready msd index or -1 (satoru)
    static bool     MsdIsReady(int device);
    static uint64_t MsdBlocks(int device);       // capacity in lbas (satoru)
    static uint32_t MsdBlockSize(int device);    // bytes per lba (satoru)
    static bool     MsdRead(int device, uint64_t lba, uint32_t count, void* buf);
    static bool     MsdWrite(int device, uint64_t lba, uint32_t count, const void* buf);

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

    // ---- more than one controller + the usb audio class driver's view (satoru) ----
    // a gid names a device on any controller: controller * USB_MAX_DEVICES + index.
    static int  ControllerCount();
    static bool GidValid(int gid);
    static const USBDeviceInfo*    GidInfo(int gid);
    static const USBDeviceRuntime* GidRuntime(int gid);
    // control transfer on a gid; a stalled control pipe is recovered (reset
    // endpoint + set tr dequeue) so one unsupported request cannot kill ep0.
    static bool GidControl(int gid, uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t wValue, uint16_t wIndex, uint16_t wLength, void* data);
    // fetch configuration descriptor number `index` (whole block, up to cap bytes)
    static bool GidGetConfig(int gid, int index, uint8_t* buf, int cap, int* out_len);
    // deconfigure the slot's endpoints (hid interrupt etc) then SET_CONFIGURATION
    static bool GidSetConfig(int gid, uint8_t config_value);
    // isoch endpoint: allocate ring + endpoint context and Configure Endpoint.
    // out_period_us is the service interval. IsoClose drops it again.
    static bool IsoOpen(int gid, const UsbIsoParams& p, uint32_t* out_period_us);
    static void IsoClose(int gid);
    // queue ONE isoch td (single trb, sia set) of len bytes at data (identity
    // mapped, must not cross a 64k boundary). false when the ring is full.
    static bool IsoQueue(int gid, const void* data, uint32_t len, bool ioc);
    static void IsoKick(int gid);                       // ring the endpoint doorbell
    static uint32_t IsoEpState(int gid);                // endpoint context state 0..4
    static bool IsoRecover(int gid);                    // reset + set tr dequeue after an error
    // callbacks into the audio driver: isoch transfer events (from PollHID) and
    // attach/detach of a device that has an audio interface.
    typedef void (*IsoEventFn)(int gid, uint64_t trb_ptr, uint8_t cc, uint32_t residual);
    typedef void (*AudioAttachFn)(int gid, bool attach);
    typedef void (*AudioServiceFn)();
    static void SetAudioHooks(IsoEventFn ev, AudioAttachFn att, AudioServiceFn svc);

    // one-shot [kbdiag] line group: usb keyboards bound + interrupt completions (satoru)
    static void LogKbDiag();
    // [usbports] every controller, every port, raw portsc, even when empty (satoru)
    static void LogAllPorts();

private:
    static XhciState* S;                       // controller being operated on (satoru)
    static XhciState  g_xs[USB_MAX_CTL];
    static int        g_nctl;
    static bool BringUp(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar_lo, uint32_t bar_hi,
                        uint32_t ports, bool secondary);
    static void PollOne(int ctl);
    static void ProbeSecondaries();
    static int  GidCtl(int gid) { return gid / USB_MAX_DEVICES; }
    static int  GidIdx(int gid) { return gid % USB_MAX_DEVICES; }
    static bool RecoverEp(int idx, uint8_t dci, xHCI_TRB* ring, int ring_idx, bool cycle);
    static void ForeignEvent(const xHCI_TRB& ev);   // a transfer event nobody waited for (satoru)

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

    // bulk / mass-storage internals (satoru)
    static bool BulkTransferEP(int idx, bool dir_in, void* data, int len,
                               int* out_transferred);
    static bool MsdCommand(int idx, const uint8_t* cb, int cb_len, bool dir_in,
                           void* data, uint32_t data_len, uint32_t* out_residue);
    static bool MsdInit(int idx);

    // input-context accessors: stride-correct pointers into input_ctx (satoru)
    static uint32_t* InputControlCtx(int idx);   // input control context (satoru)
    static uint32_t* SlotCtx(int idx);           // slot context within input_ctx
    static uint32_t* EndpointCtx(int idx, int dci); // endpoint context for dci
};
