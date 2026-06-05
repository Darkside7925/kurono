#include "mouse.h"
#include "graphics.h"
#include "serial.h"
#include "../ui/gui.h"
#include "../kernel/pci.h"
#include "../kernel/time.h"

int Mouse::mx, Mouse::my, Mouse::lastx, Mouse::lasty;
bool Mouse::auto_draw = true;
uint8_t Mouse::pkt[8];
uint8_t Mouse::pkt_i;
bool Mouse::left_down = false;
bool Mouse::left_clicked = false;
bool Mouse::right_clicked = false;
uint8_t Mouse::buttons = 0;
uint8_t Mouse::prev_buttons = 0;
uint8_t Mouse::packet_len = 3;
bool Mouse::has_scroll = false;
bool Mouse::has_xbuttons = false;
bool Mouse::cursor_visible = true;
uint16_t Mouse::speed_mul = 1;
bool Mouse::invert_scroll = false;
bool Mouse::tap_to_click = false;
bool Mouse::two_finger_scroll = true;
bool Mouse::edge_scroll = false;
uint8_t Mouse::palm_threshold = 5;
uint16_t Mouse::accel_mul = 1;
uint8_t Mouse::deadzone_px = 0;
uint16_t Mouse::sensitivity_mul = 1;
bool Mouse::natural_scroll = false;
uint8_t Mouse::map_left = 0x01;
uint8_t Mouse::map_right = 0x02;
uint8_t Mouse::map_middle = 0x04;
uint8_t Mouse::map_x1 = 0x10;
uint8_t Mouse::map_x2 = 0x20;
Mouse::DeviceType Mouse::device_type = DevUnknown;
bool Mouse::abs_mode = false;
uint8_t Mouse::abs_proto = 0;
int Mouse::last_absx = 0;
int Mouse::last_absy = 0;
int Mouse::abs_maxx = 0;
int Mouse::abs_maxy = 0;
int Mouse::scroll_rest = 0;
Mouse::Event Mouse::events[256];
uint16_t Mouse::ev_head = 0;
uint16_t Mouse::ev_tail = 0;

// Sub-pixel accumulation for slow movements. 8.8 fixed point. Slow physical
// motion that produces sub-1 pixel deltas after acceleration still moves the
// cursor over multiple frames instead of dropping to zero.
static int32_t subpixel_x_q8 = 0;
static int32_t subpixel_y_q8 = 0;
static bool smooth_inited = false;

// Pointer acceleration curve. Piecewise linear with a small dead-zone so a
// resting hand does not jitter the pointer. After the dead-zone, gain ramps
// from 1.0x up to ACCEL_MAX_GAIN at ACCEL_FAST_THRESHOLD device units / poll.
static constexpr int32_t ACCEL_DEADZONE = 1;        // device units
static constexpr int32_t ACCEL_FAST_THRESHOLD = 24; // device units / packet
static constexpr int32_t ACCEL_MAX_GAIN_Q8 = 640;   // 2.5x in Q8
static constexpr int32_t ACCEL_BASE_GAIN_Q8 = 256;  // 1.0x

static inline int32_t apply_pointer_accel_q8(int32_t v) {
    if (v == 0) return 0;
    int32_t mag = v < 0 ? -v : v;
    if (mag <= ACCEL_DEADZONE) return 0;
    int32_t adj = mag - ACCEL_DEADZONE;
    int32_t gain;
    if (adj >= ACCEL_FAST_THRESHOLD) {
        gain = ACCEL_MAX_GAIN_Q8;
    } else {
        gain = ACCEL_BASE_GAIN_Q8 +
               ((ACCEL_MAX_GAIN_Q8 - ACCEL_BASE_GAIN_Q8) * adj) /
               ACCEL_FAST_THRESHOLD;
    }
    int32_t scaled = adj * gain;       // Q8 result, fits comfortably in int32
    return (v < 0) ? -scaled : scaled; // Q8
}

// Pending coalesced motion delivered to the event ring at end of Poll().
static int32_t pending_motion_dx = 0;
static int32_t pending_motion_dy = 0;
static int32_t pending_wheel_dz = 0;
static bool pending_motion_dirty = false;

// enhanced variables
static bool raw_input_enabled = false;
static bool high_precision_enabled = false;
static uint16_t dpi_scale_x = 800;
static uint16_t dpi_scale_y = 800;
static uint8_t acceleration_curve = 1; // quadratic
static uint8_t scroll_precision = 3;
static uint16_t polling_rate_hz = 125;
static uint8_t smoothing_level = 2;
static bool three_finger_scroll = false;
static bool four_finger_gestures = false;
static bool pinch_zoom_enabled = false;
static bool rotate_gesture_enabled = false;
static uint32_t device_capabilities = 0;
static uint16_t dpi_profiles[5] = {400, 800, 1600, 3200, 6400};
static uint8_t current_dpi_profile = 1;
static Mouse::PerformanceStats perf_stats = {0};
static bool ps2_poll_enabled = false;

namespace {
constexpr uint8_t PS2_PACKET_START_BIT = 0x08;
constexpr uint8_t PS2_RET_BAT = 0xAA;
constexpr uint8_t PS2_RET_ACK = 0xFA;
constexpr uint8_t PS2_RET_NAK = 0xFE;
constexpr uint8_t PS2_RET_ERR = 0xFC;
constexpr uint8_t PS2_ID_STANDARD = 0x00;
constexpr uint8_t PS2_ID_INTELLIMOUSE = 0x03;
constexpr uint8_t PS2_ID_EXPLORER = 0x04;
constexpr uint8_t PS2_CMD_RESET = 0xFF;
constexpr uint8_t PS2_CMD_SET_DEFAULTS = 0xF6;
constexpr uint8_t PS2_CMD_DISABLE_STREAM = 0xF5;
constexpr uint8_t PS2_CMD_ENABLE_STREAM = 0xF4;
constexpr uint8_t PS2_CMD_SET_SAMPLE_RATE = 0xF3;
constexpr uint8_t PS2_CMD_GET_ID = 0xF2;
constexpr int DEFAULT_MOUSE_POLL_LIMIT = 100;
constexpr int VBOX_MOUSE_POLL_LIMIT = 256;
constexpr int VBOX_PACKET_COALESCE_SPINS = 64;
constexpr uint32_t CPUID_HYPERVISOR_BIT = (1u << 31);

// ===== VirtualBox VMMDev =====
constexpr uint16_t VBOX_PCI_VENDOR_ID = 0x80EE;
constexpr uint16_t VBOX_VMMDEV_DEVICE_ID = 0xCAFE;
constexpr uint32_t VBOX_VMMDEV_REQUEST_VERSION = 0x10001;
constexpr uint32_t VBOX_VMMDEV_PORT_OFF_REQUEST = 0;
constexpr uint32_t VBOX_VMMDEV_REQ_REPORT_GUEST_INFO  = 50;
constexpr uint32_t VBOX_VMMDEV_REQ_REPORT_GUEST_INFO2 = 58;
constexpr uint32_t VBOX_VMMDEV_REQ_REPORT_GUEST_CAPS  = 55;
constexpr uint32_t VBOX_VMMDEV_REQ_GET_MOUSE_STATUS_EX = 223;
constexpr uint32_t VBOX_VMMDEV_REQ_SET_MOUSE_STATUS = 2;
constexpr uint32_t VBOX_VMMDEV_GUEST_INTERFACE_VERSION = 0x00010004;
constexpr uint32_t VBOX_VMMDEV_GUEST_OS_TYPE_UNKNOWN = 0;
constexpr uint32_t VBOX_VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE = (1u << 0);
constexpr uint32_t VBOX_VMMDEV_MOUSE_NEW_PROTOCOL = (1u << 4);
constexpr uint32_t VBOX_VMMDEV_MOUSE_GUEST_NEEDS_HOST_CURSOR = (1u << 2);
constexpr uint32_t VBOX_VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE = (1u << 1);
constexpr uint32_t VBOX_VMMDEV_MOUSE_HOST_HAS_ABS_DEV = (1u << 6);
constexpr uint32_t VBOX_VMMDEV_MOUSE_BUTTON_LEFT = (1u << 0);
constexpr uint32_t VBOX_VMMDEV_MOUSE_BUTTON_RIGHT = (1u << 1);
constexpr uint32_t VBOX_VMMDEV_MOUSE_BUTTON_MIDDLE = (1u << 2);
constexpr uint32_t VBOX_VMMDEV_MOUSE_BUTTON_X1 = (1u << 3);
constexpr uint32_t VBOX_VMMDEV_MOUSE_BUTTON_X2 = (1u << 4);
constexpr int32_t VBOX_VMMDEV_MOUSE_RANGE_MAX = 0xFFFF;

// ===== VMware backdoor (vmmouse) =====
constexpr uint32_t VMWARE_BDOOR_MAGIC = 0x564D5868u; // 'VMXh'
constexpr uint16_t VMWARE_BDOOR_PORT  = 0x5658;
constexpr uint16_t VMWARE_CMD_GETVERSION         = 10;
constexpr uint16_t VMWARE_CMD_ABSPOINTER_DATA    = 39;
constexpr uint16_t VMWARE_CMD_ABSPOINTER_STATUS  = 40;
constexpr uint16_t VMWARE_CMD_ABSPOINTER_COMMAND = 41;
constexpr uint16_t VMWARE_CMD_ABSPOINTER_RESTRICT = 86;
constexpr uint32_t VMWARE_VMMOUSE_CMD_ENABLE = 0x45414552u;
constexpr uint32_t VMWARE_VMMOUSE_CMD_DISABLE = 0x000000F5u;
constexpr uint32_t VMWARE_VMMOUSE_CMD_REQUEST_RELATIVE = 0x4C455252u;
constexpr uint32_t VMWARE_VMMOUSE_CMD_REQUEST_ABSOLUTE = 0x53424152u;
constexpr uint32_t VMWARE_ABSPOINTER_STATUS_ERROR = 0xFFFF0000u;
constexpr uint32_t VMWARE_VERSION_ID = 0x3442554Au;
constexpr uint32_t VMWARE_RELATIVE_PACKET = 0x00010000u;
constexpr uint32_t VMWARE_RESTRICT_CPL0 = 0x01u;
constexpr uint32_t VMWARE_BTN_LEFT   = 0x20;
constexpr uint32_t VMWARE_BTN_RIGHT  = 0x10;
constexpr uint32_t VMWARE_BTN_MIDDLE = 0x08;
constexpr int32_t  VMWARE_RANGE_MAX = 0xFFFF;

enum HypervisorKind : uint8_t { HYP_NONE = 0, HYP_VBOX = 1, HYP_VMWARE = 2 };

bool virtualbox_compat_mode = false;          // alias used by PS/2 packet coalescing path
HypervisorKind detected_hypervisor = HYP_NONE;

// VBox state
bool vbox_vmmdev_available = false;
bool vbox_absolute_mode = false;
uint16_t vbox_vmmdev_port = 0;
uint32_t vbox_mouse_features = 0;
bool vbox_guest_info_reported = false;
// Last raw absolute coordinates the host reported (in 0..0xFFFF VMMDev space).
// We use 0xFFFFFFFF as "no prior sample" sentinel so we can ignore the host's
// initial 0,0 read-back before the user has actually moved their pointer into
// the VM window  -  without this the cursor warps to top-left whenever absolute
// mode flips active.
uint32_t vbox_last_raw_x = 0xFFFFFFFFu;
uint32_t vbox_last_raw_y = 0xFFFFFFFFu;
uint32_t vbox_last_raw_buttons = 0xFFFFFFFFu;
bool vbox_waiting_for_live_absolute_sample = true;

// VMware state
bool vmware_available = false;
bool vmware_absolute_mode = false;
uint32_t vmware_version = 0;
uint32_t vmware_last_raw_x = 0xFFFFFFFFu;
uint32_t vmware_last_raw_y = 0xFFFFFFFFu;
uint32_t vmware_last_raw_buttons = 0xFFFFFFFFu;

struct VBoxVMMDevRequestHeader {
    uint32_t size;
    uint32_t version;
    uint32_t request_type;
    int32_t rc;
    uint32_t reserved1;
    uint32_t requestor;
} __attribute__((packed));

struct VBoxVMMDevMouseStatusRequest {
    VBoxVMMDevRequestHeader header;
    uint32_t mouse_features;
    int32_t pointer_x;
    int32_t pointer_y;
} __attribute__((packed));

struct VBoxVMMDevMouseStatusExRequest {
    VBoxVMMDevRequestHeader header;
    uint32_t mouse_features;
    int32_t pointer_x;
    int32_t pointer_y;
    int32_t dz;
    int32_t dw;
    uint32_t buttons;
} __attribute__((packed));

struct VBoxVMMDevReportGuestInfoRequest {
    VBoxVMMDevRequestHeader header;
    uint32_t interface_version;
    uint32_t os_type;
} __attribute__((packed));

struct VBoxVMMDevReportGuestInfo2Request {
    VBoxVMMDevRequestHeader header;
    uint32_t additions_major;
    uint32_t additions_minor;
    uint32_t additions_build;
    uint32_t additions_revision;
    uint32_t additions_features;
    char     name[128];
} __attribute__((packed));

struct VBoxVMMDevReportGuestCapsRequest {
    VBoxVMMDevRequestHeader header;
    uint32_t caps;
} __attribute__((packed));

VBoxVMMDevMouseStatusRequest vbox_set_mouse_status_request = {};
VBoxVMMDevMouseStatusExRequest vbox_get_mouse_status_request = {};
VBoxVMMDevReportGuestInfoRequest vbox_report_guest_info_request = {};
VBoxVMMDevReportGuestInfo2Request vbox_report_guest_info2_request = {};
VBoxVMMDevReportGuestCapsRequest vbox_report_guest_caps_request = {};

struct VMwareBdoorRegs {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

static inline void vmware_bdoor(VMwareBdoorRegs& r) {
    asm volatile("inl %%dx, %%eax"
                 : "+a"(r.eax), "+b"(r.ebx), "+c"(r.ecx), "+d"(r.edx));
}

static uint32_t vmware_send(uint16_t cmd, uint32_t arg, uint32_t* out_ebx = nullptr,
                            uint32_t* out_ecx = nullptr, uint32_t* out_edx = nullptr) {
    VMwareBdoorRegs r;
    r.eax = VMWARE_BDOOR_MAGIC;
    r.ebx = arg;
    r.ecx = cmd;
    r.edx = VMWARE_BDOOR_PORT;
    vmware_bdoor(r);
    if (out_ebx) *out_ebx = r.ebx;
    if (out_ecx) *out_ecx = r.ecx;
    if (out_edx) *out_edx = r.edx;
    return r.eax;
}

static void cpuid_query(uint32_t leaf, uint32_t subleaf,
                        uint32_t& eax, uint32_t& ebx,
                        uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(leaf), "c"(subleaf));
}

static bool starts_with(const char* value, const char* prefix) {
    while (*prefix) {
        if (*value++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static bool is_valid_relative_packet_start(uint8_t value) {
    return (value & PS2_PACKET_START_BIT) != 0;
}

static uint8_t recover_relative_packet_sync(uint8_t* packet, uint8_t packet_bytes) {
    for (uint8_t start = 1; start < packet_bytes; start++) {
        if (!is_valid_relative_packet_start(packet[start])) {
            continue;
        }

        uint8_t carry = (uint8_t)(packet_bytes - start);
        for (uint8_t index = 0; index < carry; index++) {
            packet[index] = packet[start + index];
        }
        return carry;
    }

    return 0;
}

static HypervisorKind detect_hypervisor_kind() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid_query(1, 0, eax, ebx, ecx, edx);
    if ((ecx & CPUID_HYPERVISOR_BIT) == 0) {
        return HYP_NONE;
    }

    cpuid_query(0x40000000, 0, eax, ebx, ecx, edx);
    char vendor[13];
    ((uint32_t*)&vendor[0])[0] = ebx;
    ((uint32_t*)&vendor[4])[0] = ecx;
    ((uint32_t*)&vendor[8])[0] = edx;
    vendor[12] = 0;

    if (starts_with(vendor, "VBoxVBoxVBox")) return HYP_VBOX;
    if (starts_with(vendor, "VMwareVMware")) return HYP_VMWARE;
    if (starts_with(vendor, "KVMKVMKVM")) return HYP_VMWARE;
    // Hyper-V / Xen fall through to none for our purposes.
    return HYP_NONE;
}

[[maybe_unused]] static bool is_virtualbox_hypervisor() {
    return detect_hypervisor_kind() == HYP_VBOX;
}

static void vbox_init_request(VBoxVMMDevRequestHeader* header, uint32_t request_type, uint32_t size) {
    header->size = size;
    header->version = VBOX_VMMDEV_REQUEST_VERSION;
    header->request_type = request_type;
    header->rc = -1;
    header->reserved1 = 0;
    header->requestor = 0;
}

static bool vbox_submit_request(VBoxVMMDevRequestHeader* header) {
    if (!vbox_vmmdev_port) {
        return false;
    }

    uintptr_t phys_addr = (uintptr_t)header;
    if ((phys_addr >> 32) != 0) {
        SerialLogger::Log("Mouse: VBox request buffer above 4GiB, skipping VMMDev\r\n");
        return false;
    }

    asm volatile("" ::: "memory");
    outl((uint16_t)(vbox_vmmdev_port + VBOX_VMMDEV_PORT_OFF_REQUEST), (uint32_t)phys_addr);
    asm volatile("" ::: "memory");
    return header->rc >= 0;
}

static uint16_t vbox_find_request_port() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                if (PCI::GetVendor((uint8_t)bus, dev, func) != VBOX_PCI_VENDOR_ID) {
                    continue;
                }
                if (PCI::GetDevice((uint8_t)bus, dev, func) != VBOX_VMMDEV_DEVICE_ID) {
                    continue;
                }

                for (int bar = 0; bar < 6; bar++) {
                    uint32_t bar_value = PCI::GetBAR((uint8_t)bus, dev, func, bar);
                    if ((bar_value & 0x1u) == 0) {
                        continue;
                    }
                    return (uint16_t)(bar_value & ~0x3u);
                }
            }
        }
    }

    return 0;
}

static int vbox_scale_pointer_axis(int value, int max_pixels) {
    if (value < 0) {
        value = 0;
    } else if (value > VBOX_VMMDEV_MOUSE_RANGE_MAX) {
        value = VBOX_VMMDEV_MOUSE_RANGE_MAX;
    }

    if (max_pixels <= 1) {
        return 0;
    }

    return (int)(((uint64_t)(uint32_t)value * (uint64_t)(max_pixels - 1)) / (uint64_t)VBOX_VMMDEV_MOUSE_RANGE_MAX);
}
}

void Mouse::Init() {
    mx = Graphics::GetWidth() / 2;
    my = Graphics::GetHeight() / 2;
    lastx = mx; lasty = my;
    
    SerialLogger::Log("Mouse: Initializing Enhanced Driver...\r\n");
    
    // reset performance stats
    perf_stats = {0};
    detected_hypervisor = detect_hypervisor_kind();
    virtualbox_compat_mode = (detected_hypervisor == HYP_VBOX);
    ps2_poll_enabled = false;
    has_scroll = false;
    has_xbuttons = false;
    packet_len = 3;

    FlushOutput();

    // Linux's i8042 path flushes stale controller bytes before probing AUX,
    // toggles the port, and verifies the controller config. Do the same here
    // so BAT/ID bytes from power-on do not get mistaken for failed ACKs.
    WriteCmd(0xA8);
    uint8_t ccb = 0;
    bool have_ccb = ReadControllerConfig(ccb);
    if (have_ccb) {
        ccb &= (uint8_t)~0x20; // AUX clock enabled
        ccb &= (uint8_t)~0x02; // keep IRQ delivery off, we poll
        if (!WriteControllerConfig(ccb)) {
            SerialLogger::Log("Mouse: Failed to write controller config\r\n");
        }
    } else {
        SerialLogger::Log("Mouse: Failed to read controller config\r\n");
    }
    FlushOutput();

    uint8_t reset_id = 0;
    bool reset_ok = ResetDevice(reset_id);
    if (reset_ok) {
        SerialLogger::Log("Mouse: Reset/BAT OK id=");
        SerialLogger::LogHex(reset_id);
        SerialLogger::Log("\r\n");
    } else {
        SerialLogger::Log("Mouse: Reset/BAT did not complete cleanly\r\n");
    }

    // defaults
    bool defaults_ok = SendMouseByteAwaitAck(PS2_CMD_SET_DEFAULTS, 100000, 2);
    if (defaults_ok) {
        SerialLogger::Log("Mouse: Defaults Set (ACK)\r\n");
    } else {
        SerialLogger::Log("Mouse: Defaults Failed (No ACK) - Ignoring\r\n");
    }
    
    FlushOutput();

    uint8_t detected_id = ReadID();
    if (detected_id == PS2_ID_STANDARD) {
        detected_id = reset_id;
    }
    if (detected_id == PS2_ID_INTELLIMOUSE) {
        has_scroll = true;
        packet_len = 4;
        SerialLogger::Log("Mouse: IntelliMouse detected (scroll wheel)\r\n");
    } else if (detected_id == PS2_ID_EXPLORER) {
        has_scroll = true;
        has_xbuttons = true;
        packet_len = 4;
        SerialLogger::Log("Mouse: IntelliMouse Explorer detected (5-button)\r\n");
    } else {
        has_scroll = false;
        has_xbuttons = false;
        packet_len = 3;
        SerialLogger::Log("Mouse: Standard PS/2 mouse\r\n");
    }

    // enable streaming
    bool streaming_ok = SendMouseByteAwaitAck(PS2_CMD_ENABLE_STREAM, 100000, 3);
    if (streaming_ok) {
        SerialLogger::Log("Mouse: Streaming Enabled (ACK)\r\n");
    } else {
        SerialLogger::Log("Mouse: Streaming Failed (No ACK) - Ignoring\r\n");
    }
    
    // initialize enhanced settings
    pkt_i = 0;
    speed_mul = 1;
    invert_scroll = false;
    cursor_visible = true;
    left_down = false;
    left_clicked = false;
    right_clicked = false;
    buttons = 0;
    prev_buttons = 0;
    tap_to_click = false;
    two_finger_scroll = true;
    edge_scroll = false;
    palm_threshold = 5;
    accel_mul = 1;
    deadzone_px = 0;
    sensitivity_mul = 1;
    natural_scroll = false;
    map_left = 0x01; map_right = 0x02; map_middle = 0x04; map_x1 = 0x10; map_x2 = 0x20;
    if (!has_scroll && !has_xbuttons) packet_len = 3;
    device_type = DevMouse;
    abs_mode = false;
    abs_proto = 0;
    device_capabilities = 0x01;
    abs_maxx = 0; abs_maxy = 0; scroll_rest = 0;

    InitVirtualBoxIntegration();
    InitVMwareIntegration();

    ps2_poll_enabled = streaming_ok;
    bool absolute_fallback_ok = vbox_vmmdev_available || vmware_available;
    if (!ps2_poll_enabled && !absolute_fallback_ok) {
        cursor_visible = false;
        auto_draw = false;
        device_type = DevUnknown;
        FlushOutput();
        SerialLogger::Log("Mouse: Disabled - no acknowledged PS/2 stream or host integration\r\n");
        return;
    }

    if (!ps2_poll_enabled) {
        SerialLogger::Log("Mouse: PS/2 stream unavailable, waiting for host absolute input\r\n");
    }

    smooth_inited = false;
    subpixel_x_q8 = 0;
    subpixel_y_q8 = 0;
    pending_motion_dx = 0;
    pending_motion_dy = 0;
    pending_wheel_dz = 0;
    pending_motion_dirty = false;
    ev_head = 0;
    ev_tail = 0;

    DrawAt(mx, my);
    if (detected_hypervisor == HYP_VBOX) {
        SerialLogger::Log("Mouse: VirtualBox compatibility mode enabled\r\n");
    } else if (detected_hypervisor == HYP_VMWARE) {
        SerialLogger::Log("Mouse: VMware compatibility mode enabled\r\n");
    }
    if (ps2_poll_enabled) {
        SerialLogger::Log("Mouse: Compatibility PS/2 mode enabled\r\n");
    }
    SerialLogger::Log("Mouse: Enhanced Driver initialized\r\n");
}

void Mouse::InitVirtualBoxIntegration() {
    vbox_vmmdev_available = false;
    vbox_absolute_mode = false;
    vbox_vmmdev_port = 0;
    vbox_mouse_features = 0;
    vbox_guest_info_reported = false;
    vbox_waiting_for_live_absolute_sample = true;

    if (detected_hypervisor != HYP_VBOX) {
        return;
    }

    vbox_vmmdev_port = vbox_find_request_port();
    if (!vbox_vmmdev_port) {
        SerialLogger::Log("Mouse: VBox VMMDev request port not found\r\n");
        return;
    }

    SerialLogger::Log("Mouse: VBox VMMDev request port 0x");
    SerialLogger::LogHex(vbox_vmmdev_port);
    SerialLogger::Log("\r\n");

    // Step 1: ReportGuestInfo - REQUIRED to set fu32AdditionsOk so the host honors
    // any subsequent SetMouseStatus call. Without this VBox returns VERR_VERSION_MISMATCH
    // for nearly every other VMMDev request.
    vbox_init_request(&vbox_report_guest_info_request.header,
                      VBOX_VMMDEV_REQ_REPORT_GUEST_INFO,
                      (uint32_t)sizeof(vbox_report_guest_info_request));
    vbox_report_guest_info_request.interface_version = VBOX_VMMDEV_GUEST_INTERFACE_VERSION;
    vbox_report_guest_info_request.os_type = VBOX_VMMDEV_GUEST_OS_TYPE_UNKNOWN;
    if (!vbox_submit_request(&vbox_report_guest_info_request.header)) {
        SerialLogger::Log("Mouse: VBox ReportGuestInfo failed rc=");
        SerialLogger::LogHex((uint32_t)vbox_report_guest_info_request.header.rc);
        SerialLogger::Log("\r\n");
        vbox_vmmdev_port = 0;
        return;
    }
    vbox_guest_info_reported = true;
    SerialLogger::Log("Mouse: VBox ReportGuestInfo OK (Additions handshake complete)\r\n");

    // Step 2 (best-effort): ReportGuestInfo2 with KuronoOS identity.
    vbox_init_request(&vbox_report_guest_info2_request.header,
                      VBOX_VMMDEV_REQ_REPORT_GUEST_INFO2,
                      (uint32_t)sizeof(vbox_report_guest_info2_request));
    vbox_report_guest_info2_request.additions_major = 7;
    vbox_report_guest_info2_request.additions_minor = 0;
    vbox_report_guest_info2_request.additions_build = 0;
    vbox_report_guest_info2_request.additions_revision = 0;
    vbox_report_guest_info2_request.additions_features = 0;
    const char* nm = "KuronoOS Guest Mouse Integration";
    for (int i = 0; i < 128; i++) vbox_report_guest_info2_request.name[i] = 0;
    for (int i = 0; nm[i] && i < 127; i++) vbox_report_guest_info2_request.name[i] = nm[i];
    (void)vbox_submit_request(&vbox_report_guest_info2_request.header);

    // Step 3 (best-effort): clear guest capabilities.
    vbox_init_request(&vbox_report_guest_caps_request.header,
                      VBOX_VMMDEV_REQ_REPORT_GUEST_CAPS,
                      (uint32_t)sizeof(vbox_report_guest_caps_request));
    vbox_report_guest_caps_request.caps = 0;
    (void)vbox_submit_request(&vbox_report_guest_caps_request.header);

    // Step 4: SetMouseStatus - advertise a modern absolute-input guest driver.
    // Linux's vboxguest input path reports CAN_ABSOLUTE | NEW_PROTOCOL here.
    // We draw our own cursor, so we should not ask the host to keep ownership
    // of pointer rendering via GUEST_NEEDS_HOST_CURSOR.
    vbox_init_request(&vbox_set_mouse_status_request.header,
                      VBOX_VMMDEV_REQ_SET_MOUSE_STATUS,
                      (uint32_t)sizeof(vbox_set_mouse_status_request));
    vbox_set_mouse_status_request.mouse_features =
        VBOX_VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE | VBOX_VMMDEV_MOUSE_NEW_PROTOCOL;
    vbox_set_mouse_status_request.pointer_x = 0;
    vbox_set_mouse_status_request.pointer_y = 0;

    if (!vbox_submit_request(&vbox_set_mouse_status_request.header)) {
        SerialLogger::Log("Mouse: VBox SetMouseStatus failed rc=");
        SerialLogger::LogHex((uint32_t)vbox_set_mouse_status_request.header.rc);
        SerialLogger::Log("\r\n");
        vbox_vmmdev_port = 0;
        return;
    }

    vbox_vmmdev_available = true;
    vbox_mouse_features = vbox_set_mouse_status_request.mouse_features;
    SerialLogger::Log("Mouse: VBox absolute pointer integration ENABLED (mouse_features=");
    SerialLogger::LogHex(vbox_mouse_features);
    SerialLogger::Log(")\r\n");
}

void Mouse::InitVMwareIntegration() {
    vmware_available = false;
    vmware_absolute_mode = false;
    vmware_version = 0;
    vmware_last_raw_x = 0xFFFFFFFFu;
    vmware_last_raw_y = 0xFFFFFFFFu;
    vmware_last_raw_buttons = 0xFFFFFFFFu;

    if (detected_hypervisor != HYP_VMWARE) {
        return;
    }

    // Linux vmmouse first probes GETVERSION, then does ENABLE -> STATUS ->
    // DATA(version id) -> RESTRICT -> REQUEST_ABSOLUTE.
    uint32_t response = ~VMWARE_BDOOR_MAGIC;
    uint32_t hypervisor_type = 0;
    uint32_t version = vmware_send(VMWARE_CMD_GETVERSION, 0, &response, &hypervisor_type, nullptr);
    if (response != VMWARE_BDOOR_MAGIC || version == 0xFFFFFFFFu) {
        SerialLogger::Log("Mouse: VMware backdoor probe failed\r\n");
        return;
    }
    vmware_version = version;
    SerialLogger::Log("Mouse: VMware backdoor present, version=");
    SerialLogger::LogHex(vmware_version);
    SerialLogger::Log("\r\n");

    vmware_send(VMWARE_CMD_ABSPOINTER_COMMAND, VMWARE_VMMOUSE_CMD_ENABLE);
    uint32_t status = vmware_send(VMWARE_CMD_ABSPOINTER_STATUS, 0);
    if ((status & VMWARE_ABSPOINTER_STATUS_ERROR) == VMWARE_ABSPOINTER_STATUS_ERROR ||
        (status & 0x0000FFFFu) == 0) {
        SerialLogger::Log("Mouse: VMware vmmouse enable failed\r\n");
        vmware_send(VMWARE_CMD_ABSPOINTER_COMMAND, VMWARE_VMMOUSE_CMD_DISABLE);
        return;
    }

    uint32_t vmmouse_version = vmware_send(VMWARE_CMD_ABSPOINTER_DATA, 1);
    if (vmmouse_version != VMWARE_VERSION_ID) {
        SerialLogger::Log("Mouse: VMware vmmouse version mismatch value=");
        SerialLogger::LogHex(vmmouse_version);
        SerialLogger::Log("\r\n");
        vmware_send(VMWARE_CMD_ABSPOINTER_COMMAND, VMWARE_VMMOUSE_CMD_DISABLE);
        return;
    }

    vmware_send(VMWARE_CMD_ABSPOINTER_RESTRICT, VMWARE_RESTRICT_CPL0);
    vmware_send(VMWARE_CMD_ABSPOINTER_COMMAND, VMWARE_VMMOUSE_CMD_REQUEST_ABSOLUTE);

    vmware_available = true;
    SerialLogger::Log("Mouse: VMware vmmouse absolute integration ENABLED\r\n");
}

// SPSC ring push. Drops on overflow rather than corrupting the reader.
void Mouse::RingPush(const Mouse::Event& e) {
    uint16_t h = __atomic_load_n(&ev_head, __ATOMIC_RELAXED);
    uint16_t t = __atomic_load_n(&ev_tail, __ATOMIC_ACQUIRE);
    uint16_t next = (uint16_t)((h + 1) & 255);
    if (next == (t & 255)) return;
    events[h & 255] = e;
    __atomic_store_n(&ev_head, next, __ATOMIC_RELEASE);
}

void Mouse::EmitHostAbsoluteSample(int new_x, int new_y, uint8_t hw_buttons, int wheel_delta) {
    perf_stats.packets_processed++;

    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (new_x < 0) new_x = 0; else if (new_x >= w) new_x = w - 1;
    if (new_y < 0) new_y = 0; else if (new_y >= h) new_y = h - 1;

    int old_x = mx;
    int old_y = my;
    int rel_dx = new_x - old_x;
    int rel_dy = old_y - new_y;

    uint8_t new_buttons = MapButtons(hw_buttons);
    bool left = (new_buttons & 0x01) != 0;
    bool right = (new_buttons & 0x02) != 0;
    if (left && !(prev_buttons & 0x01)) {
        left_clicked = true;
        SerialLogger::Log("Mouse: Left Click\r\n");
    }
    if (right && !(prev_buttons & 0x02)) {
        right_clicked = true;
        SerialLogger::Log("Mouse: Right Click\r\n");
    }
    left_down = left;
    buttons = new_buttons;

    if (rel_dx || rel_dy) {
        lastx = old_x;
        lasty = old_y;
        mx = new_x;
        my = new_y;
        if (cursor_visible && auto_draw) {
            ClearAt(lastx, lasty);
            DrawAt(mx, my);
        }
        // Coalesce  -  multiple absolute samples per Poll() tick collapse into a
        // single motion event holding cumulative deltas + the final position.
        pending_motion_dx += rel_dx;
        pending_motion_dy += rel_dy;
        pending_motion_dirty = true;
    }

    if (wheel_delta) {
        if (invert_scroll) wheel_delta = -wheel_delta;
        pending_wheel_dz += wheel_delta;
    }

    uint8_t changed = (uint8_t)(new_buttons ^ prev_buttons);
    if (changed) {
        // Flush motion before button edges so consumers see "moved to (x,y)
        // then clicked" in the right order.
        if (pending_motion_dirty) {
            uint64_t ts = TimeManager::NowUTC().us;
            Event me{};
            me.type = 0; me.x = mx; me.y = my;
            me.dx = pending_motion_dx; me.dy = pending_motion_dy;
            me.buttons = new_buttons; me.time_us = ts;
            RingPush(me);
            pending_motion_dx = pending_motion_dy = 0;
            pending_motion_dirty = false;
        }
        uint64_t ts = TimeManager::NowUTC().us;
        for (int i = 0; i < 5; i++) {
            uint8_t mask = (i == 0 ? 0x01 : i == 1 ? 0x02 : i == 2 ? 0x04 : i == 3 ? 0x08 : 0x10);
            if (!(changed & mask)) continue;
            Event e{};
            e.type = (uint8_t)((new_buttons & mask) ? 1 : 2);
            e.x = mx; e.y = my; e.button = (uint8_t)i;
            e.buttons = new_buttons; e.time_us = ts;
            RingPush(e);
        }
    }
    prev_buttons = new_buttons;
}

// USB HID boot-protocol mouse report decode (satoru).
//   byte0: buttons  -  bit0 left, bit1 right, bit2 middle
//   byte1: dx  (signed int8, +x = right)
//   byte2: dy  (signed int8, +y = down per HID convention)
//   byte3: wheel (signed int8, optional; +z = up)
// We translate into the same relative motion/button/wheel events the PS/2 path
// emits, keeping the codebase convention of "dy positive = up" in events and
// moving the cursor with down-positive screen coordinates (satoru).
void Mouse::ProcessUSBReport(const uint8_t* report, int len) {
    if (!report || len < 3) return;

    perf_stats.packets_processed++;

    uint8_t hw_buttons = 0;
    if (report[0] & 0x01) hw_buttons |= 0x01; // left
    if (report[0] & 0x02) hw_buttons |= 0x02; // right
    if (report[0] & 0x04) hw_buttons |= 0x04; // middle
    if (report[0] & 0x08) hw_buttons |= 0x10; // button 4 -> x1 lane (satoru)
    if (report[0] & 0x10) hw_buttons |= 0x20; // button 5 -> x2 lane (satoru)

    int dx = (int)(int8_t)report[1];
    int dy_hid = (int)(int8_t)report[2];        // +down (satoru)
    int wheel = (len >= 4) ? (int)(int8_t)report[3] : 0; // +up (satoru)

    // apply the coarse user multipliers used by the PS/2 path so feel matches
    // (acceleration curve is intentionally left to the PS/2 stream; HID mice
    // are already reported at device dpi) (satoru).
    int m_dx = dx * (int)speed_mul;
    int m_dy_down = dy_hid * (int)speed_mul;    // down-positive screen delta (satoru)
    if (sensitivity_mul > 1) { m_dx *= (int)sensitivity_mul; m_dy_down *= (int)sensitivity_mul; }
    if (deadzone_px) {
        int dzp = (int)deadzone_px;
        if (m_dx > -dzp && m_dx < dzp) m_dx = 0;
        if (m_dy_down > -dzp && m_dy_down < dzp) m_dy_down = 0;
    }

    uint8_t new_buttons = MapButtons(hw_buttons);
    bool left = (new_buttons & 0x01) != 0;
    bool right = (new_buttons & 0x02) != 0;
    if (left && !left_down) { left_clicked = true; SerialLogger::Log("Mouse: Left Click\r\n"); }
    static bool usb_right_down = false;
    if (right && !usb_right_down) { right_clicked = true; SerialLogger::Log("Mouse: Right Click\r\n"); }
    usb_right_down = right;
    left_down = left;
    buttons = new_buttons;

    lastx = mx; lasty = my;
    int w = Graphics::GetWidth(); int h = Graphics::GetHeight();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int new_mx = mx + m_dx;
    int new_my = my + m_dy_down;              // down-positive (satoru)
    if (new_mx < 0) new_mx = 0; else if (new_mx > w - 1) new_mx = w - 1;
    if (new_my < 0) new_my = 0; else if (new_my > h - 1) new_my = h - 1;
    int delivered_dx = new_mx - mx;
    int delivered_dy_up = my - new_my;         // event convention: + = up (satoru)
    mx = new_mx; my = new_my;
    if (cursor_visible && auto_draw && (delivered_dx || delivered_dy_up)) {
        ClearAt(lastx, lasty);
        DrawAt(mx, my);
    }

    uint64_t ts = TimeManager::NowUTC().us;
    if (delivered_dx || delivered_dy_up) {
        Event e{};
        e.type = 0; e.x = mx; e.y = my;
        e.dx = delivered_dx; e.dy = delivered_dy_up;
        e.buttons = new_buttons; e.time_us = ts;
        RingPush(e);
        perf_stats.events_generated++;
    }

    uint8_t changed = (uint8_t)(new_buttons ^ prev_buttons);
    if (changed) {
        for (int i = 0; i < 5; i++) {
            uint8_t mask = (i == 0 ? 0x01 : i == 1 ? 0x02 : i == 2 ? 0x04 : i == 3 ? 0x08 : 0x10);
            if (!(changed & mask)) continue;
            Event e{};
            e.type = (uint8_t)((new_buttons & mask) ? 1 : 2);
            e.x = mx; e.y = my; e.button = (uint8_t)i;
            e.buttons = new_buttons; e.time_us = ts;
            RingPush(e);
        }
    }
    prev_buttons = new_buttons;

    if (wheel) {
        int dz = invert_scroll ? -wheel : wheel;
        Event e{};
        e.type = 3; e.x = mx; e.y = my; e.dz = dz;
        e.buttons = new_buttons; e.time_us = ts;
        RingPush(e);
    }
}

bool Mouse::PollVirtualBoxAbsolute() {
    if (!vbox_vmmdev_available) {
        return false;
    }

    vbox_init_request(&vbox_get_mouse_status_request.header,
                      VBOX_VMMDEV_REQ_GET_MOUSE_STATUS_EX,
                      (uint32_t)sizeof(vbox_get_mouse_status_request));
    vbox_get_mouse_status_request.mouse_features = 0;
    vbox_get_mouse_status_request.pointer_x = 0;
    vbox_get_mouse_status_request.pointer_y = 0;
    vbox_get_mouse_status_request.dz = 0;
    vbox_get_mouse_status_request.dw = 0;
    vbox_get_mouse_status_request.buttons = 0;

    if (!vbox_submit_request(&vbox_get_mouse_status_request.header)) {
        if (vbox_absolute_mode) {
            SerialLogger::Log("Mouse: VBox mouse polling failed, falling back to PS/2\r\n");
        }
        vbox_absolute_mode = false;
        smooth_inited = false;
        return false;
    }

    vbox_mouse_features = vbox_get_mouse_status_request.mouse_features;
    bool host_absolute = (vbox_mouse_features & (VBOX_VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE | VBOX_VMMDEV_MOUSE_HOST_HAS_ABS_DEV)) != 0;
    if (host_absolute != vbox_absolute_mode) {
        vbox_absolute_mode = host_absolute;
        smooth_inited = false;
        // On any active<->inactive transition reset the raw tracker so the next
        // "real" sample isn't compared against a stale value (which would let a
        // 0,0 readback through as a valid movement).
        vbox_last_raw_x = 0xFFFFFFFFu;
        vbox_last_raw_y = 0xFFFFFFFFu;
        vbox_last_raw_buttons = 0xFFFFFFFFu;
        vbox_waiting_for_live_absolute_sample = host_absolute;
        SerialLogger::Log("Mouse: VBox absolute integration ");
        SerialLogger::Log(host_absolute ? "active" : "inactive");
        SerialLogger::Log("\r\n");
    }
    if (!host_absolute) {
        return false;
    }

    uint32_t raw_x = (uint32_t)vbox_get_mouse_status_request.pointer_x;
    uint32_t raw_y = (uint32_t)vbox_get_mouse_status_request.pointer_y;
    uint32_t raw_buttons = vbox_get_mouse_status_request.buttons;
    int wheel = vbox_get_mouse_status_request.dz;

    // Only emit when the host actually reports a change. This kills the
    // "snap to (0,0) on capture / first activation" warp: VBox returns the
    // last-known host pointer (often 0,0) until the user moves into the VM
    // window, and we'd rather sit still than warp.
    bool first_sample = (vbox_last_raw_x == 0xFFFFFFFFu);
    bool changed = (raw_x != vbox_last_raw_x) ||
                   (raw_y != vbox_last_raw_y) ||
                   (raw_buttons != vbox_last_raw_buttons) ||
                   (wheel != 0);
    vbox_last_raw_x = raw_x;
    vbox_last_raw_y = raw_y;
    vbox_last_raw_buttons = raw_buttons;
    if (first_sample || !changed) {
        // Swallow the very first reading and any "no-op" polls. Returning true
        // still claims the input path so PS/2 doesn't double-fire while
        // absolute integration is active.
        return true;
    }

    // VBox commonly reports 0,0 until the host pointer has actually crossed
    // into the VM window. Keep swallowing those placeholder coordinates until
    // we observe the first non-zero absolute sample after activation. After
    // that, real top-left movement remains valid.
    if (vbox_waiting_for_live_absolute_sample) {
        if (raw_x == 0 && raw_y == 0) {
            return true;
        }
        vbox_waiting_for_live_absolute_sample = false;
    }

    int new_x = vbox_scale_pointer_axis((int32_t)raw_x, Graphics::GetWidth());
    int new_y = vbox_scale_pointer_axis((int32_t)raw_y, Graphics::GetHeight());

    uint8_t hw_buttons = 0;
    if (raw_buttons & VBOX_VMMDEV_MOUSE_BUTTON_LEFT)   hw_buttons |= 0x01;
    if (raw_buttons & VBOX_VMMDEV_MOUSE_BUTTON_RIGHT)  hw_buttons |= 0x02;
    if (raw_buttons & VBOX_VMMDEV_MOUSE_BUTTON_MIDDLE) hw_buttons |= 0x04;
    if (raw_buttons & VBOX_VMMDEV_MOUSE_BUTTON_X1)     hw_buttons |= 0x10;
    if (raw_buttons & VBOX_VMMDEV_MOUSE_BUTTON_X2)     hw_buttons |= 0x20;

    EmitHostAbsoluteSample(new_x, new_y, hw_buttons, wheel);
    return true;
}

bool Mouse::PollVMwareAbsolute() {
    if (!vmware_available) return false;

    bool delivered_any = false;
    // Linux drains the backdoor queue in 4-dword packets: status, x, y, z.
    for (int guard = 0; guard < 255; guard++) {
        uint32_t status = vmware_send(VMWARE_CMD_ABSPOINTER_STATUS, 0);
        if ((status & VMWARE_ABSPOINTER_STATUS_ERROR) == VMWARE_ABSPOINTER_STATUS_ERROR) {
            SerialLogger::Log("Mouse: VMware ABSPOINTER status error, disabling integration\r\n");
            vmware_send(VMWARE_CMD_ABSPOINTER_COMMAND, VMWARE_VMMOUSE_CMD_DISABLE);
            vmware_available = false;
            vmware_absolute_mode = false;
            vmware_last_raw_x = 0xFFFFFFFFu;
            vmware_last_raw_y = 0xFFFFFFFFu;
            vmware_last_raw_buttons = 0xFFFFFFFFu;
            return delivered_any;
        }
        uint32_t words = status & 0xFFFFu;
        if (words < 4) break;
        if ((words & 0x3u) != 0) {
            SerialLogger::Log("Mouse: VMware ABSPOINTER invalid queue length\r\n");
            vmware_send(VMWARE_CMD_ABSPOINTER_COMMAND, VMWARE_VMMOUSE_CMD_DISABLE);
            vmware_available = false;
            vmware_absolute_mode = false;
            return delivered_any;
        }

        uint32_t pkt_x = 0, pkt_y = 0, pkt_z = 0;
        uint32_t pkt_status = vmware_send(VMWARE_CMD_ABSPOINTER_DATA, 4, &pkt_x, &pkt_y, &pkt_z);

        uint8_t hw_buttons = 0;
        if (pkt_status & VMWARE_BTN_LEFT)   hw_buttons |= 0x01;
        if (pkt_status & VMWARE_BTN_RIGHT)  hw_buttons |= 0x02;
        if (pkt_status & VMWARE_BTN_MIDDLE) hw_buttons |= 0x04;

        // Linux reports wheel as REL_WHEEL, -(s8)((u8)z).
        int wheel = -(int)(int8_t)(pkt_z & 0xFF);

        if (pkt_status & VMWARE_RELATIVE_PACKET) {
            int new_x = mx + (int32_t)pkt_x;
            int new_y = my - (int32_t)pkt_y;
            EmitHostAbsoluteSample(new_x, new_y, hw_buttons, wheel);
            delivered_any = true;
            continue;
        }

        if (!vmware_absolute_mode) {
            vmware_absolute_mode = true;
            smooth_inited = false;
            vmware_last_raw_x = 0xFFFFFFFFu;
            vmware_last_raw_y = 0xFFFFFFFFu;
            vmware_last_raw_buttons = 0xFFFFFFFFu;
            SerialLogger::Log("Mouse: VMware vmmouse absolute integration active\r\n");
        }

        // Same anti-warp gate as the VBox path: drop the first reading and any
        // packet whose raw coordinates+buttons match the previous one.
        bool first_sample = (vmware_last_raw_x == 0xFFFFFFFFu);
        bool changed = (pkt_x != vmware_last_raw_x) ||
                       (pkt_y != vmware_last_raw_y) ||
                       (pkt_status != vmware_last_raw_buttons) ||
                       (wheel != 0);
        vmware_last_raw_x = pkt_x;
        vmware_last_raw_y = pkt_y;
        vmware_last_raw_buttons = pkt_status;
        delivered_any = true;
        if (first_sample || !changed) {
            continue;
        }

        int sw = Graphics::GetWidth();
        int sh = Graphics::GetHeight();
        if (sw <= 1) sw = 2;
        if (sh <= 1) sh = 2;
        int new_x = (int)(((uint64_t)pkt_x * (uint64_t)(sw - 1)) / (uint64_t)VMWARE_RANGE_MAX);
        int new_y = (int)(((uint64_t)pkt_y * (uint64_t)(sh - 1)) / (uint64_t)VMWARE_RANGE_MAX);

        EmitHostAbsoluteSample(new_x, new_y, hw_buttons, wheel);
    }
    // Once vmmouse is enabled it should own the mouse stream; falling back to
    // raw PS/2 when the queue is momentarily empty causes duplicate/jittery
    // motion in VMware because both paths race to update the cursor.
    return vmware_available || delivered_any;
}

Mouse::DeviceType Mouse::DetectDeviceType() {
    // Mirror Linux's psmouse extension probe sequence.
    if (!CommandArg(PS2_CMD_SET_SAMPLE_RATE, 200)) return DevMouse;
    if (!CommandArg(PS2_CMD_SET_SAMPLE_RATE, 100)) return DevMouse;
    if (!CommandArg(PS2_CMD_SET_SAMPLE_RATE, 80)) return DevMouse;

    uint8_t device_id = ReadID();

    if (device_id == PS2_ID_INTELLIMOUSE) {
        has_scroll = true;
        packet_len = 4;
        SerialLogger::Log("Mouse: IntelliMouse detected (scroll wheel)\r\n");
        return DevMouse;
    }

    if (!CommandArg(PS2_CMD_SET_SAMPLE_RATE, 200)) return DevMouse;
    if (!CommandArg(PS2_CMD_SET_SAMPLE_RATE, 200)) return DevMouse;
    if (!CommandArg(PS2_CMD_SET_SAMPLE_RATE, 80)) return DevMouse;

    device_id = ReadID();
    if (device_id == PS2_ID_EXPLORER) {
        has_scroll = true;
        has_xbuttons = true;
        packet_len = 4;
        SerialLogger::Log("Mouse: IntelliMouse Explorer detected (5-button)\r\n");
        return DevMouse;
    } else {
        SerialLogger::Log("Mouse: Standard PS/2 mouse\r\n");
        return DevMouse;
    }
}

void Mouse::DetectExtendedCapabilities() {
    device_capabilities = 0;
    
    // check for various extended capabilities
    // this is hardware-specific and would need real device detection
    
    // simulate some common capabilities
    device_capabilities |= 0x01; // basic movement
    device_capabilities |= 0x02; // scroll wheel
    device_capabilities |= 0x04; // extra buttons
    
    // check for high dpi support (simulated)
    if (device_type == DevMouse) {
        device_capabilities |= 0x08; // high dpi
        device_capabilities |= 0x10; // adjustable dpi
    }
}

bool Mouse::HasCapability(uint32_t capability_flag) {
    return (device_capabilities & capability_flag) != 0;
}

void Mouse::PrintDeviceInfo() {
    SerialLogger::Log("Mouse Device Info:\r\n");
    SerialLogger::Log("  Type: ");
    switch (device_type) {
        case DevMouse: SerialLogger::Log("Mouse\r\n"); break;
        case DevTouchpad: SerialLogger::Log("Touchpad\r\n"); break;
        default: SerialLogger::Log("Unknown\r\n"); break;
    }
    
    SerialLogger::Log("  Capabilities: ");
    SerialLogger::LogHex(device_capabilities);
    SerialLogger::Log("\r\n");
    
    SerialLogger::Log("  DPI Profile: ");
    SerialLogger::LogDec(dpi_profiles[current_dpi_profile]);
    SerialLogger::Log(" (profile ");
    SerialLogger::LogDec(current_dpi_profile);
    SerialLogger::Log(")\r\n");
    
    SerialLogger::Log("  Polling Rate: ");
    SerialLogger::LogDec(polling_rate_hz);
    SerialLogger::Log("Hz\r\n");
}

void Mouse::SetRawInput(bool enable) {
    raw_input_enabled = enable;
    SerialLogger::Log("Mouse: Raw input ");
    SerialLogger::Log(enable ? "enabled" : "disabled");
    SerialLogger::Log("\r\n");
}

void Mouse::SetHighPrecision(bool enable) {
    high_precision_enabled = enable;
    
    if (enable) {
        // try to set 1000hz sample rate for high precision
        WriteMouse(0xF3); ExpectAck(50000);
        WriteMouse(200); ExpectAck(50000);
        SerialLogger::Log("Mouse: High precision enabled\r\n");
    } else {
        // set standard sample rate
        WriteMouse(0xF3); ExpectAck(50000);
        WriteMouse(100); ExpectAck(50000);
        SerialLogger::Log("Mouse: High precision disabled\r\n");
    }
}

void Mouse::SetDPIScaling(uint16_t dpi_x, uint16_t dpi_y) {
    dpi_scale_x = dpi_x;
    dpi_scale_y = dpi_y;
    
    SerialLogger::Log("Mouse: DPI scaling set to ");
    SerialLogger::LogDec(dpi_x);
    SerialLogger::Log("x");
    SerialLogger::LogDec(dpi_y);
    SerialLogger::Log("\r\n");
}

void Mouse::SetPollingRate(uint16_t hz) {
    polling_rate_hz = hz;
    
    // convert to sample rate command
    uint8_t rate_val = 100; // default
    if (hz >= 1000) rate_val = 200;
    else if (hz >= 500) rate_val = 150;
    else if (hz >= 250) rate_val = 120;
    else rate_val = 100;
    
    WriteMouse(0xF3); // set sample rate
    ExpectAck(50000);
    WriteMouse(rate_val);
    ExpectAck(50000);
    
    SerialLogger::Log("Mouse: Polling rate set to ");
    SerialLogger::LogDec(hz);
    SerialLogger::Log("Hz\r\n");
}

uint16_t Mouse::GetPollingRate() {
    return polling_rate_hz;
}

void Mouse::SetDPIProfile(uint8_t profile_num, uint16_t dpi) {
    if (profile_num < 5) {
        dpi_profiles[profile_num] = dpi;
        SerialLogger::Log("Mouse: DPI profile ");
        SerialLogger::LogDec(profile_num);
        SerialLogger::Log(" set to ");
        SerialLogger::LogDec(dpi);
        SerialLogger::Log("\r\n");
    }
}

void Mouse::SwitchDPIProfile(uint8_t profile_num) {
    if (profile_num < 5) {
        current_dpi_profile = profile_num;
        SetDPIScaling(dpi_profiles[profile_num], dpi_profiles[profile_num]);
        SerialLogger::Log("Mouse: Switched to DPI profile ");
        SerialLogger::LogDec(profile_num);
        SerialLogger::Log(" (");
        SerialLogger::LogDec(dpi_profiles[profile_num]);
        SerialLogger::Log(" DPI)\r\n");
    }
}

uint8_t Mouse::GetCurrentDPIProfile() {
    return current_dpi_profile;
}

void Mouse::SetAccelerationCurve(uint8_t curve_type) {
    acceleration_curve = curve_type;
    SerialLogger::Log("Mouse: Acceleration curve set to ");
    SerialLogger::LogDec(curve_type);
    SerialLogger::Log("\r\n");
}

const Mouse::PerformanceStats& Mouse::GetPerformanceStats() {
    return perf_stats;
}

void Mouse::Poll() {
    if (PollVirtualBoxAbsolute()) {
        FlushOutput();
        return;
    }
    if (PollVMwareAbsolute()) {
        FlushOutput();
        return;
    }
    if (!ps2_poll_enabled) {
        // If PS/2 init failed we still need to drain auxiliary bytes so they
        // do not keep the shared 8042 output buffer permanently occupied and
        // starve keyboard polling in the GUI loop.
        FlushOutput();
        return;
    }

    // limit loop to prevent hanging if controller is spamming status but no data
    int loop_limit = virtualbox_compat_mode ? VBOX_MOUSE_POLL_LIMIT : DEFAULT_MOUSE_POLL_LIMIT;
    while (loop_limit-- > 0) {
        uint8_t st = In(0x64);
        if (!((st & 0x01) && (st & 0x20))) {
            // VirtualBox can split packet bytes across tightly-spaced IRQs.
            if (!(virtualbox_compat_mode && pkt_i != 0)) break;

            int spins = VBOX_PACKET_COALESCE_SPINS;
            while (spins-- > 0) {
                asm volatile("pause");
                st = In(0x64);
                if ((st & 0x01) && (st & 0x20)) {
                    break;
                }
            }

            if (!((st & 0x01) && (st & 0x20))) break;
        }
        uint8_t v = In(0x60);
        
        // synchronization: ensure first byte of packet has bit 3 set (for standard ps/2 and intellimouse)
        if (pkt_i == 0 && !abs_mode && (packet_len == 3 || packet_len == 4)) {
            if (!is_valid_relative_packet_start(v)) {
                perf_stats.precision_errors++;
                continue;
            }
        }

        pkt[pkt_i++] = v;
        if (pkt_i >= packet_len) {
            uint8_t packet_bytes = pkt_i;
            pkt_i = 0;
            uint8_t b = pkt[0];
            if (!abs_mode && packet_len <= 4) {
                if (!is_valid_relative_packet_start(b)) {
                    perf_stats.precision_errors++;
                    pkt_i = recover_relative_packet_sync(pkt, packet_bytes);
                    prev_buttons = buttons;
                    continue;
                }
            }
            perf_stats.packets_processed++;
            // PS/2 packet format: pkt[0] holds the per-axis SIGN bits and OVERFLOW
            // bits; pkt[1]/pkt[2] are the low 8 bits of a 9-bit signed delta.
            //   bit 0: left button
            //   bit 1: right button
            //   bit 2: middle button
            //   bit 3: always 1 (sync marker)
            //   bit 4: X sign  (1 = negative)
            //   bit 5: Y sign  (1 = negative)
            //   bit 6: X overflow
            //   bit 7: Y overflow
            // Reconstruct the signed delta as (raw_byte | sign_extension):
            //   delta = pkt[N] - ((pkt[0] << k) & 0x100)
            // Without this, fast motion (|delta| > 127) wraps into the opposite
            // direction and warps the cursor into a corner  -  exactly the bug
            // we hit when VBox synthesises a big delta on capture.
            int dx_full = 0;
            int dy_full = 0;
            int8_t dz = 0;
            if (!abs_mode && (packet_len == 3 || packet_len == 4)) {
                if (b & 0x40) {
                    // X overflow  -  host saw a delta too big to encode. Drop the
                    // axis rather than guess; otherwise we'd jam the cursor at
                    // an edge.
                    dx_full = 0;
                } else {
                    dx_full = (int)pkt[1] - (int)((b << 4) & 0x100);
                }
                if (b & 0x80) {
                    dy_full = 0;
                } else {
                    dy_full = (int)pkt[2] - (int)((b << 3) & 0x100);
                }
            } else {
                // Absolute / 6-byte protocols re-derive m_dx/m_dy below from
                // ax/ay diffs, so the legacy int8_t cast is fine as a stub.
                dx_full = (int)(int8_t)pkt[1];
                dy_full = (int)(int8_t)pkt[2];
            }
            int8_t dx = (int8_t)(dx_full < -128 ? -128 : (dx_full > 127 ? 127 : dx_full));
            int8_t dy = (int8_t)(dy_full < -128 ? -128 : (dy_full > 127 ? 127 : dy_full));
            (void)dx; (void)dy;
            uint8_t xbtn = 0;
            if (packet_len == 4) {
                if (has_xbuttons) { xbtn = (uint8_t)((pkt[3] & 0x10 ? 0x10 : 0) | (pkt[3] & 0x20 ? 0x20 : 0)); }
                int8_t z4 = (int8_t)(pkt[3] & 0x0F);
                if (pkt[3] & 0x08) z4 |= 0xF0;
                dz = z4;
                if (invert_scroll) dz = (int8_t)(-dz);
            } else if (packet_len == 6) {
                if (!abs_mode) {
                    uint8_t z = pkt[4];
                    if (z == 0xFF) dz = -1; else if (z == 0x01) dz = 1; else dz = 0;
                    if (invert_scroll) dz = (int8_t)(-dz);
                }
            }
            uint8_t hw_buttons;
            if (abs_mode && packet_len == 6) {
                if (abs_proto == 1) { hw_buttons = 0; if (pkt[2] & 0x08) hw_buttons |= 0x01; if (pkt[2] & 0x04) hw_buttons |= 0x02; } else { hw_buttons = 0; if (pkt[3] & 0x01) hw_buttons |= 0x01; if (pkt[3] & 0x02) hw_buttons |= 0x02; if (pkt[3] & 0x04) hw_buttons |= 0x04; }
            } else {
                hw_buttons = (uint8_t)((b & 0x07) | xbtn);
            }
            uint8_t new_buttons = MapButtons(hw_buttons);
            bool left = (new_buttons & 0x01) != 0;
            bool right = (new_buttons & 0x02) != 0;
            if (left && !left_down) {
                left_clicked = true;
                SerialLogger::Log("Mouse: Left Click\r\n");
            }
            static bool right_down = false;
            if (right && !right_down) {
                right_clicked = true;
                SerialLogger::Log("Mouse: Right Click\r\n");
            }
            right_down = right;
            left_down = left;
            buttons = new_buttons;
            lastx = mx; lasty = my;
            int m_dx;
            int m_dy;
            if (abs_mode && packet_len == 6) {
                int ax = 0, ay = 0, az = 0;
                if (abs_proto == 1) { ax = (((int)(pkt[0] & 0x07)) << 7) | (int)(pkt[1] & 0x7F); ay = (((int)(pkt[3] & 0x07)) << 7) | (int)(pkt[4] & 0x7F); az = (int)(pkt[5] & 0x7F); }
                else { ax = (((int)((pkt[2] & 0xF0) >> 4)) << 7) | (int)(pkt[1] & 0x7F); ay = (((int)((pkt[3] & 0x70) >> 4)) << 7) | (int)(pkt[4] & 0x7F); az = (int)(pkt[5] & 0x7F); }
                m_dx = (ax - last_absx); m_dy = (ay - last_absy); last_absx = ax; last_absy = ay;
                dx = (int8_t)m_dx; dy = (int8_t)m_dy; dz = (int8_t)az;
                if (ax > abs_maxx) abs_maxx = ax;
                if (ay > abs_maxy) abs_maxy = ay;
                int pr = az;
                if (palm_threshold && pr >= (int)palm_threshold) { prev_buttons = new_buttons; continue; }
                bool near_edge = edge_scroll && abs_maxx && (ax > abs_maxx - (abs_maxx / 12));
                bool do_scroll = near_edge || (two_finger_scroll && ((pkt[2] & 0x01) != 0));
                if (do_scroll) {
                    int s = m_dy + scroll_rest;
                    int steps = s / 32;
                    scroll_rest = s - steps * 32;
                    if (steps) {
                        int dzv = steps;
                        if (invert_scroll) dzv = -dzv;
                        pending_wheel_dz += dzv;
                    }
                    prev_buttons = new_buttons;
                    continue;
                }
            } else {
                // speed_mul is a small linear scaler; keep separate from
                // pointer-acceleration which is applied below.
                m_dx = dx_full * (int)speed_mul;
                m_dy = dy_full * (int)speed_mul;
            }
            if (deadzone_px) {
                int dz_px = (int)deadzone_px;
                if (m_dx > -dz_px && m_dx < dz_px) m_dx = 0;
                if (m_dy > -dz_px && m_dy < dz_px) m_dy = 0;
            }
            if (sensitivity_mul > 1) {
                m_dx *= (int)sensitivity_mul;
                m_dy *= (int)sensitivity_mul;
            }

            // Sub-pixel pointer acceleration in Q8. The legacy accel_mul stays
            // available as a coarse user multiplier on top of the curve.
            int32_t ax_q8 = apply_pointer_accel_q8(m_dx);
            int32_t ay_q8 = apply_pointer_accel_q8(m_dy);
            if (accel_mul > 1) {
                ax_q8 *= (int32_t)accel_mul;
                ay_q8 *= (int32_t)accel_mul;
            }

            // Accumulate sub-pixel residue so slow motion still moves the
            // cursor over consecutive packets instead of being rounded away.
            subpixel_x_q8 += ax_q8;
            subpixel_y_q8 += ay_q8;
            int32_t step_x = subpixel_x_q8 / 256;
            int32_t step_y = subpixel_y_q8 / 256;
            subpixel_x_q8 -= step_x * 256;
            subpixel_y_q8 -= step_y * 256;

            int w = Graphics::GetWidth(); int h = Graphics::GetHeight();
            if (w < 1) w = 1; if (h < 1) h = 1;

            int64_t target_x = (int64_t)mx + (int64_t)step_x;
            int64_t target_y = (int64_t)my - (int64_t)step_y; // PS/2: +y = up
            if (target_x < 0) target_x = 0;
            if (target_y < 0) target_y = 0;
            if (target_x > w - 1) target_x = w - 1;
            if (target_y > h - 1) target_y = h - 1;

            if (!smooth_inited) smooth_inited = true;

            int new_mx = (int)target_x;
            int new_my = (int)target_y;
            int delivered_dx = new_mx - mx;
            int delivered_dy = my - new_my; // positive = up, mirrors PS/2 convention
            mx = new_mx;
            my = new_my;
            if (cursor_visible && auto_draw && (delivered_dx || delivered_dy)) {
                ClearAt(lastx, lasty);
                DrawAt(mx, my);
            }
            uint64_t ts = TimeManager::NowUTC().us;
            if (delivered_dx || delivered_dy) {
                pending_motion_dx += delivered_dx;
                pending_motion_dy += delivered_dy;
                pending_motion_dirty = true;
                perf_stats.events_generated++;
            }
            if (!abs_mode && dz) {
                pending_wheel_dz += dz;
            }
            uint8_t changed = (uint8_t)(new_buttons ^ prev_buttons);
            if (changed) {
                if (pending_motion_dirty) {
                    Event me{};
                    me.type = 0; me.x = mx; me.y = my;
                    me.dx = pending_motion_dx; me.dy = pending_motion_dy;
                    me.buttons = new_buttons; me.time_us = ts;
                    RingPush(me);
                    pending_motion_dx = pending_motion_dy = 0;
                    pending_motion_dirty = false;
                }
                for (int i = 0; i < 5; i++) {
                    uint8_t mask = (i == 0 ? 0x01 : i == 1 ? 0x02 : i == 2 ? 0x04 : i == 3 ? 0x08 : 0x10);
                    if (changed & mask) {
                        Event e{};
                        e.type = (uint8_t)((new_buttons & mask) ? 1 : 2);
                        e.x = mx; e.y = my;
                        e.button = (uint8_t)i;
                        e.buttons = new_buttons; e.time_us = ts;
                        RingPush(e);
                    }
                }
            }
            prev_buttons = new_buttons;
        }
    }

    // Flush the per-tick coalesced motion and wheel deltas.
    if (pending_motion_dirty || pending_wheel_dz) {
        uint64_t ts = TimeManager::NowUTC().us;
        if (pending_motion_dirty) {
            Event e{};
            e.type = 0; e.x = mx; e.y = my;
            e.dx = pending_motion_dx; e.dy = pending_motion_dy;
            e.buttons = buttons; e.time_us = ts;
            RingPush(e);
            pending_motion_dx = pending_motion_dy = 0;
            pending_motion_dirty = false;
        }
        if (pending_wheel_dz) {
            Event e{};
            e.type = 3; e.x = mx; e.y = my; e.dz = pending_wheel_dz;
            e.buttons = buttons; e.time_us = ts;
            RingPush(e);
            pending_wheel_dz = 0;
        }
    }
}

void Mouse::Out(uint16_t p, uint8_t v) { __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(p)); }
uint8_t Mouse::In(uint16_t p) { uint8_t r; __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
bool Mouse::WaitInputBufferClear(int timeout_us) {
    int t = timeout_us;
    while (t-- > 0) {
        if (!(In(0x64) & 0x02)) return true;
        __asm__ __volatile__("pause");
    }
    return false;
}
bool Mouse::ReadAuxByte(uint8_t& value, int timeout_us) {
    int t = timeout_us;
    while (t-- > 0) {
        uint8_t st = In(0x64);
        if (!(st & 0x01)) {
            __asm__ __volatile__("pause");
            continue;
        }
        uint8_t b = In(0x60);
        if (st & 0x20) {
            value = b;
            return true;
        }
    }
    return false;
}
void Mouse::WriteCmd(uint8_t c) {
    if (WaitInputBufferClear(200000)) Out(0x64, c);
}
void Mouse::WriteMouse(uint8_t c) {
    if (!WaitInputBufferClear(200000)) return;
    Out(0x64, 0xD4);
    if (!WaitInputBufferClear(200000)) return;
    Out(0x60, c);
}
bool Mouse::SendMouseByteAwaitAck(uint8_t value, int timeout_us, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        WriteMouse(value);
        int t = timeout_us;
        while (t-- > 0) {
            uint8_t reply = 0;
            if (!ReadAuxByte(reply, 1)) {
                __asm__ __volatile__("pause");
                continue;
            }
            if (reply == PS2_RET_ACK) return true;
            if (reply == PS2_RET_NAK) break;
            if (reply == PS2_RET_BAT || reply == PS2_ID_STANDARD ||
                reply == PS2_ID_INTELLIMOUSE || reply == PS2_ID_EXPLORER) {
                // Linux's libps2 path treats BAT/ID bytes as distinct from ACK
                // handling. Skip these stale probe/reset bytes instead of
                // classifying the whole command as failed immediately.
                continue;
            }
            if (reply == PS2_RET_ERR) return false;
            SerialLogger::Log("Mouse: Expected ACK, got ");
            SerialLogger::LogHex(reply);
            SerialLogger::Log("\r\n");
        }
    }
    return false;
}
bool Mouse::ExpectAck(int timeout_us) {
    int t = timeout_us;
    while (t-- > 0) {
        uint8_t reply = 0;
        if (!ReadAuxByte(reply, 1)) {
            __asm__ __volatile__("pause");
            continue;
        }
        if (reply == PS2_RET_ACK) return true;
        if (reply == PS2_RET_BAT || reply == PS2_ID_STANDARD ||
            reply == PS2_ID_INTELLIMOUSE || reply == PS2_ID_EXPLORER) {
            continue;
        }
        if (reply == PS2_RET_NAK || reply == PS2_RET_ERR) return false;
        SerialLogger::Log("Mouse: Expected ACK, got ");
        SerialLogger::LogHex(reply);
        SerialLogger::Log("\r\n");
    }
    return false;
}
bool Mouse::ReadControllerConfig(uint8_t& cfg) {
    if (!WaitInputBufferClear(200000)) return false;
    Out(0x64, 0x20);
    int t = 200000;
    while (t-- > 0) {
        uint8_t st = In(0x64);
        if (!(st & 0x01)) {
            __asm__ __volatile__("pause");
            continue;
        }
        uint8_t v = In(0x60);
        if (st & 0x20) continue;
        cfg = v;
        return true;
    }
    return false;
}
bool Mouse::WriteControllerConfig(uint8_t cfg) {
    if (!WaitInputBufferClear(200000)) return false;
    Out(0x64, 0x60);
    if (!WaitInputBufferClear(200000)) return false;
    Out(0x60, cfg);
    return true;
}
bool Mouse::ResetDevice(uint8_t& device_id) {
    device_id = 0;
    if (!SendMouseByteAwaitAck(PS2_CMD_RESET, 200000, 2)) return false;
    uint8_t reply = 0;
    bool saw_bat = false;
    if (ReadAuxByte(reply, 400000)) {
        if (reply == PS2_RET_BAT) {
            saw_bat = true;
            uint8_t maybe_id = 0;
            if (ReadAuxByte(maybe_id, 150000)) device_id = maybe_id;
        } else if (reply == PS2_ID_STANDARD || reply == PS2_ID_INTELLIMOUSE || reply == PS2_ID_EXPLORER) {
            saw_bat = true;
            device_id = reply;
        }
    }
    return saw_bat;
}
bool Mouse::CommandArg(uint8_t cmd, uint8_t arg) {
    if (!SendMouseByteAwaitAck(cmd, 100000, 2)) return false;
    return SendMouseByteAwaitAck(arg, 100000, 2);
}
uint8_t Mouse::ReadID() {
    for (int attempt = 0; attempt < 2; attempt++) {
        WriteMouse(PS2_CMD_GET_ID);
        int t = 100000;
        bool saw_ack = false;
        while (t-- > 0) {
            uint8_t reply = 0;
            if (!ReadAuxByte(reply, 1)) {
                __asm__ __volatile__("pause");
                continue;
            }
            if (reply == PS2_RET_ACK) {
                saw_ack = true;
                continue;
            }
            if (reply == PS2_RET_NAK) break;
            if (reply == PS2_RET_BAT) continue;
            if (reply == PS2_ID_STANDARD || reply == PS2_ID_INTELLIMOUSE || reply == PS2_ID_EXPLORER) {
                return reply;
            }
            if (saw_ack) return reply;
        }
    }
    return PS2_ID_STANDARD;
}
void Mouse::FlushOutput() { for (int i = 0; i < 2048; i++) { uint8_t st = In(0x64); if (!((st & 0x01) && (st & 0x20))) break; (void)In(0x60); } }

uint32_t Mouse::BgAt(int x, int y) { (void)x; (void)y; return 0xFF000000; }

void Mouse::ClearAt(int x, int y) { GUI::DrawRegion(x, y, 12, 16); }

void Mouse::DrawAt(int x, int y) {
    static const uint8_t cursor_bitmap[16][12] = {
        {1,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,1,1,1,1,0,0},
        {1,2,2,1,2,2,1,0,0,0,0,0},
        {1,2,1,0,1,2,2,1,0,0,0,0},
        {1,1,0,0,1,2,2,1,0,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,0,1,1,0,0,0,0}
    };
    
    for(int cy=0; cy<16; cy++) {
        for(int cx=0; cx<12; cx++) {
            uint8_t p = cursor_bitmap[cy][cx];
            if (p == 1) Graphics::DrawPixel(x+cx, y+cy, 0xFF000000);
            else if (p == 2) Graphics::DrawPixel(x+cx, y+cy, 0xFFFFFFFF);
        }
    }
}

bool Mouse::LeftClicked() { bool r = left_clicked; left_clicked = false; return r; }
bool Mouse::RightClicked() { bool r = right_clicked; right_clicked = false; return r; }
bool Mouse::IsLeftDown() { return left_down; }
bool Mouse::IsOperational() { return ps2_poll_enabled || vbox_vmmdev_available || vmware_available; }
void Mouse::ForceRedraw() { if(auto_draw) DrawAt(mx, my); }
bool Mouse::HasEvent() {
    uint16_t h = __atomic_load_n(&ev_head, __ATOMIC_ACQUIRE);
    uint16_t t = __atomic_load_n(&ev_tail, __ATOMIC_RELAXED);
    return h != t;
}
Mouse::Event Mouse::GetEvent() {
    uint16_t h = __atomic_load_n(&ev_head, __ATOMIC_ACQUIRE);
    uint16_t t = __atomic_load_n(&ev_tail, __ATOMIC_RELAXED);
    if (h == t) { Event empty{}; return empty; }
    Event e = events[t & 255];
    __atomic_store_n(&ev_tail, (uint16_t)((t + 1) & 255), __ATOMIC_RELEASE);
    return e;
}
void Mouse::Show() { if (!cursor_visible) { cursor_visible = true; if(auto_draw) DrawAt(mx, my); } }
void Mouse::Hide() { if (cursor_visible) { cursor_visible = false; if(auto_draw) ClearAt(mx, my); } }
void Mouse::SetSpeed(uint16_t mul) { speed_mul = mul ? mul : 1; }
void Mouse::SetInvertScroll(bool inv) { invert_scroll = inv; }
void Mouse::GetPosition(int& x, int& y) { x = mx; y = my; }
void Mouse::SetAutoDraw(bool enable) { auto_draw = enable; }
void Mouse::SetTapToClick(bool en) { tap_to_click = en; }
void Mouse::SetTwoFingerScroll(bool en) { two_finger_scroll = en; }
void Mouse::SetEdgeScroll(bool en) { edge_scroll = en; }
void Mouse::SetPalmRejection(uint8_t thr) { palm_threshold = thr; }
void Mouse::SetAcceleration(uint16_t mul) { accel_mul = mul ? mul : 1; }
void Mouse::SetDeadzone(uint8_t px) { deadzone_px = px; }
void Mouse::SetSensitivity(uint16_t mul) { sensitivity_mul = mul ? mul : 1; }
void Mouse::SetNaturalScroll(bool en) { natural_scroll = en; invert_scroll = en; }
void Mouse::SetButtonMap(uint8_t left, uint8_t right, uint8_t middle, uint8_t x1, uint8_t x2) { map_left = left; map_right = right; map_middle = middle; map_x1 = x1; map_x2 = x2; }
Mouse::DeviceType Mouse::GetDeviceType() { return device_type; }
uint8_t Mouse::MapButtons(uint8_t hw) {
    uint8_t r = 0;
    if (hw & map_left) r |= 0x01;
    if (hw & map_right) r |= 0x02;
    if (hw & map_middle) r |= 0x04;
    if (hw & map_x1) r |= 0x08;
    if (hw & map_x2) r |= 0x10;
    return r;
}
