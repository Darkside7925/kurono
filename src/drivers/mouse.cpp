#include "mouse.h"
#include "graphics.h"
#include "serial.h"
#include "../ui/gui.h"
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

static int smooth_x256 = 0;
static int smooth_y256 = 0;
static int target_x256 = 0;
static int target_y256 = 0;
static bool smooth_inited = false;
static const int SMOOTH_ALPHA = 140;   // 0-256, higher = snappier (140 ≈ 55%)

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

void Mouse::Init() {
    mx = Graphics::GetWidth() / 2;
    my = Graphics::GetHeight() / 2;
    lastx = mx; lasty = my;
    
    SerialLogger::Log("Mouse: Initializing Enhanced Driver...\r\n");
    
    // reset performance stats
    perf_stats = {0};

    // enable ps/2 auxiliary device
    WriteCmd(0xA8);
    
    // get controller configuration byte
    WriteCmd(0x20);
    int t_cb = 100000; uint8_t ccb = 0; 
    while (t_cb-- > 0) { 
        uint8_t st = In(0x64); 
        if (st & 0x01) { ccb = In(0x60); break; } 
    }
    
    // enable interrupt (bit 1) and disable clock (bit 5)
    ccb &= (uint8_t)~0x20;
    // disable interrupt (bit 1) since we use polling
    ccb &= (uint8_t)~0x02;
    // enable auxiliary device (bit 5 clear = enabled? no, bit 5 is mouse clock disable. 0=enable)
    ccb &= (uint8_t)~0x20; 
    
    // set controller configuration byte
    WriteCmd(0x60);
    while (In(0x64) & 0x02){}
    Out(0x60, ccb);
    
    // defaults
    WriteMouse(0xF6); // set defaults
    if (ExpectAck(100000)) {
        SerialLogger::Log("Mouse: Defaults Set (ACK)\r\n");
    } else {
        SerialLogger::Log("Mouse: Defaults Failed (No ACK) - Ignoring\r\n");
    }
    
    FlushOutput();

    // enable streaming
    WriteMouse(0xF4);
    if (ExpectAck(100000)) {
        SerialLogger::Log("Mouse: Streaming Enabled (ACK)\r\n");
    } else {
        SerialLogger::Log("Mouse: Streaming Failed (No ACK) - Ignoring\r\n");
    }
    
    // initialize enhanced settings
    pkt_i = 0;
    packet_len = 3;
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
    has_scroll = false;
    has_xbuttons = false;
    packet_len = 3;
    device_type = DevMouse;
    abs_mode = false;
    abs_proto = 0;
    device_capabilities = 0x01;
    abs_maxx = 0; abs_maxy = 0; scroll_rest = 0;
    
    DrawAt(mx, my);
    SerialLogger::Log("Mouse: Compatibility PS/2 mode enabled\r\n");
    SerialLogger::Log("Mouse: Enhanced Driver initialized\r\n");
}

Mouse::DeviceType Mouse::DetectDeviceType() {
    // try to detect if we have a gaming mouse, touchpad, or basic mouse
    // this is a simplified detection
    
    // check for intellimouse support
    WriteMouse(0xF3); ExpectAck(10000); // set sample rate
    WriteMouse(200); ExpectAck(10000);
    WriteMouse(0xF3); ExpectAck(10000);
    WriteMouse(100); ExpectAck(10000);
    WriteMouse(0xF3); ExpectAck(10000);
    WriteMouse(80); ExpectAck(10000);
    
    uint8_t device_id = ReadID();
    
    if (device_id == 0x03) {
        has_scroll = true;
        packet_len = 4;
        SerialLogger::Log("Mouse: IntelliMouse detected (scroll wheel)\r\n");
        return DevMouse;
    } else if (device_id == 0x04) {
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
    // limit loop to prevent hanging if controller is spamming status but no data
    int loop_limit = 100;
    while (loop_limit-- > 0) {
        uint8_t st = In(0x64);
        if (!((st & 0x01) && (st & 0x20))) break;
        uint8_t v = In(0x60);
        
        // synchronization: ensure first byte of packet has bit 3 set (for standard ps/2 and intellimouse)
        if (pkt_i == 0 && !abs_mode && (packet_len == 3 || packet_len == 4)) {
            if ((v & 0x08) == 0) {
                continue;
            }
        }

        pkt[pkt_i++] = v;
        if (pkt_i >= packet_len) {
            pkt_i = 0;
            uint8_t b = pkt[0];
            if (!abs_mode && packet_len <= 4) {
                if ((b & 0x08) == 0) { prev_buttons = buttons; continue; }
            }
            int8_t dx = (int8_t)pkt[1];
            int8_t dy = (int8_t)pkt[2];
            int8_t dz = 0;
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
                if (do_scroll) { int s = m_dy + scroll_rest; int steps = s / 32; scroll_rest = s - steps * 32; if (steps) { int dzv = steps; if (invert_scroll) dzv = -dzv; uint64_t ts = TimeManager::NowUTC().us; Event e; e.type = 3; e.x = mx; e.y = my; e.dx = 0; e.dy = 0; e.dz = dzv; e.button = 0; e.buttons = new_buttons; e.fingers = 0; e.pressure = (uint8_t)pr; e.width = 0; e.gesture = 0; e.time_us = ts; events[ev_head++] = e; ev_head &= 255; } prev_buttons = new_buttons; continue; }
            } else { m_dx = (int)dx * (int)speed_mul; m_dy = (int)dy * (int)speed_mul; }
            if (deadzone_px) { if (m_dx > -deadzone_px && m_dx < deadzone_px) m_dx = 0; if (m_dy > -deadzone_px && m_dy < deadzone_px) m_dy = 0; }
            if (sensitivity_mul > 1) {
                m_dx *= (int)sensitivity_mul;
                m_dy *= (int)sensitivity_mul;
            }
            if (accel_mul > 1) { int ax = (m_dx >= 0 ? m_dx : -m_dx); int ay = (m_dy >= 0 ? m_dy : -m_dy); int a = ax + ay; if (a > 2) { m_dx = m_dx * (int)accel_mul; m_dy = m_dy * (int)accel_mul; } }

            // update target position (raw) in fixed-point
            if (!smooth_inited) {
                smooth_x256 = mx * 256;
                smooth_y256 = my * 256;
                smooth_inited = true;
            }
            target_x256 = (mx + m_dx) * 256;
            target_y256 = (my - m_dy) * 256;

            // clamp target
            int w = Graphics::GetWidth(); int h = Graphics::GetHeight();
            if (target_x256 < 0) target_x256 = 0;
            if (target_y256 < 0) target_y256 = 0;
            if (target_x256 > (w - 1) * 256) target_x256 = (w - 1) * 256;
            if (target_y256 > (h - 1) * 256) target_y256 = (h - 1) * 256;

            // exponential smoothing: smooth = smooth + alpha * (target - smooth) / 256
            smooth_x256 += (SMOOTH_ALPHA * (target_x256 - smooth_x256)) / 256;
            smooth_y256 += (SMOOTH_ALPHA * (target_y256 - smooth_y256)) / 256;

            // snap to target if very close (avoid perpetual sub-pixel crawl)
            int diff_x = target_x256 - smooth_x256;
            int diff_y = target_y256 - smooth_y256;
            if (diff_x > -64 && diff_x < 64) smooth_x256 = target_x256;
            if (diff_y > -64 && diff_y < 64) smooth_y256 = target_y256;

            mx = smooth_x256 / 256;
            my = smooth_y256 / 256;
            if (cursor_visible && auto_draw) { ClearAt(lastx, lasty); DrawAt(mx, my); }
            uint64_t ts = TimeManager::NowUTC().us;
            if (m_dx || m_dy) {
                Event e; e.type = 0; e.x = mx; e.y = my; e.dx = m_dx; e.dy = m_dy; e.dz = 0; e.button = 0; e.buttons = new_buttons; e.fingers = (uint8_t)((abs_mode && ((abs_proto==1 && (pkt[2]&0x02)) || (abs_proto==2 && (pkt[2]&0x02)))) ? 1 : 0); e.pressure = (uint8_t)(abs_mode ? (pkt[5] & 0x7F) : 0); e.width = 0; e.gesture = (uint8_t)(abs_mode ? (pkt[2] & 0x01) : 0); e.time_us = ts; events[ev_head++] = e; ev_head &= 255; }
            if (!abs_mode && dz) { Event e; e.type = 3; e.x = mx; e.y = my; e.dx = 0; e.dy = 0; e.dz = dz; e.button = 0; e.buttons = new_buttons; e.fingers = 0; e.pressure = 0; e.width = 0; e.gesture = 0; e.time_us = ts; events[ev_head++] = e; ev_head &= 255; }
            uint8_t changed = (uint8_t)(new_buttons ^ prev_buttons);
            if (changed) {
                for (int i = 0; i < 5; i++) {
                    uint8_t mask = (i == 0 ? 0x01 : i == 1 ? 0x02 : i == 2 ? 0x04 : i == 3 ? 0x08 : 0x10);
                    if (changed & mask) {
                        Event e; e.type = ((new_buttons & mask) ? 1 : 2); e.x = mx; e.y = my; e.dx = 0; e.dy = 0; e.dz = 0; e.button = (uint8_t)i; e.buttons = new_buttons; e.fingers = 0; e.pressure = 0; e.width = 0; e.gesture = 0; e.time_us = ts; events[ev_head++] = e; ev_head &= 255;
                    }
                }
            }
            prev_buttons = new_buttons;
        }
    }
}

void Mouse::Out(uint16_t p, uint8_t v) { __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(p)); }
uint8_t Mouse::In(uint16_t p) { uint8_t r; __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
void Mouse::WriteCmd(uint8_t c) { while (In(0x64) & 0x02){} Out(0x64, c); }
void Mouse::WriteMouse(uint8_t c) { while (In(0x64) & 0x02){} Out(0x64, 0xD4); while (In(0x64) & 0x02){} Out(0x60, c); }
bool Mouse::ExpectAck(int timeout_us) { 
    int t = timeout_us; 
    while (t-- > 0) { 
        uint8_t st = In(0x64); 
        if ((st & 0x01) && (st & 0x20)) { 
            uint8_t a = In(0x60); 
            if (a == 0xFA) return true;
            SerialLogger::Log("Mouse: Expected ACK, got "); SerialLogger::LogHex(a); SerialLogger::Log("\r\n");
            // don't return false immediately, maybe ack is next? 
            // actually standard ps/2 is strict.
            return false;
        } 
    } 
    return false; 
}
bool Mouse::CommandArg(uint8_t cmd, uint8_t arg) { WriteMouse(cmd); if (!ExpectAck(100000)) return false; WriteMouse(arg); return ExpectAck(100000); }
uint8_t Mouse::ReadID() { WriteMouse(0xF2); if (!ExpectAck(100000)) return 0x00; int t = 100000; while (t-- > 0) { uint8_t st = In(0x64); if ((st & 0x01) && (st & 0x20)) { return In(0x60); } } return 0x00; }
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
void Mouse::ForceRedraw() { if(auto_draw) DrawAt(mx, my); }
bool Mouse::HasEvent() { return ev_tail != ev_head; }
Mouse::Event Mouse::GetEvent() { Event e = events[ev_tail]; ev_tail = (uint16_t)((ev_tail + 1) & 255); return e; }
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
