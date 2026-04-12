#pragma once
//  kurono os  -  usb (xhci) host controller driver
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

#define USB_MAX_DEVICES    16
#define USB_MAX_PORTS      16

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

    static void DumpInfo(char* out, int max_len);

private:
    static bool detected;
    static volatile uint8_t* bar0;
    static uint32_t cap_length;
    static uint32_t max_ports;
    static uint32_t max_slots;

    static USBDeviceInfo devices[USB_MAX_DEVICES];
    static int device_count;

    // ring buffers
    static xHCI_TRB* cmd_ring;
    static xHCI_TRB* event_ring;
    static uint64_t* dcbaa;
    static int cmd_ring_idx;
    static int event_ring_idx;
    static bool event_ccs;

    static uint32_t ReadCap(uint32_t offset);
    static uint32_t ReadOp(uint32_t offset);
    static void WriteOp(uint32_t offset, uint32_t val);
    static uint32_t ReadPort(int port);
    static void WritePort(int port, uint32_t val);
    static uint32_t ReadRuntime(uint32_t offset);
    static void WriteRuntime(uint32_t offset, uint32_t val);
    static void WriteDoorbell(int slot, uint32_t val);

    static bool Reset();
    static bool WaitReady(int timeout_ms);
    static bool SubmitCommand(xHCI_TRB* cmd, xHCI_TRB* result);
    static bool PollEventRing(xHCI_TRB* result, int timeout);
};
