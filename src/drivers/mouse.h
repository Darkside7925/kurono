#pragma once
#include "../kernel/types.h"

class Mouse {
public:
    static void Init();
    static void Poll();
    static bool LeftClicked();
    static bool RightClicked();
    static bool IsLeftDown();
    static bool IsOperational();
    static void ForceRedraw();
    enum DeviceType { DevUnknown=0, DevMouse=1, DevTouchpad=2 };
    struct Event { uint8_t type; int x; int y; int dx; int dy; int dz; uint8_t button; uint8_t buttons; uint8_t fingers; uint8_t pressure; uint8_t width; uint8_t gesture; uint64_t time_us; };
    static bool HasEvent();
    static Event GetEvent();
    static void Show();
    static void Hide();
    static void SetSpeed(uint16_t mul);
    static void SetInvertScroll(bool inv);
    static void GetPosition(int& x, int& y);
    static void DrawAt(int x, int y);
    static void SetRawMode(bool enable);  // 1:1 pointer, no accel (satoru)
    static int mx, my;
    
    // touchpad/scroll configuration
    static void SetTapToClick(bool en);
    static void SetTwoFingerScroll(bool en);
    static void SetEdgeScroll(bool en);
    static void SetPalmRejection(uint8_t thr);
    static void SetAcceleration(uint16_t mul);
    static void SetDeadzone(uint8_t px);
    static void SetSensitivity(uint16_t mul);
    static void SetNaturalScroll(bool en);
    static void SetButtonMap(uint8_t left, uint8_t right, uint8_t middle, uint8_t x1, uint8_t x2);
    // advanced precision settings
    static void SetRawInput(bool enable); // disable acceleration for gaming
    static void SetHighPrecision(bool enable); // 1000hz polling if available
    static void SetDPIScaling(uint16_t dpi_x, uint16_t dpi_y); // dpi adjustment
    static void SetAccelerationCurve(uint8_t curve_type); // 0=linear, 1=quadratic, 2=cubic
    static void SetScrollPrecision(uint8_t precision); // higher precision scrolling
    
    // enhanced touchpad gestures
    static void SetThreeFingerScroll(bool en);
    static void SetFourFingerGestures(bool en);
    static void SetPinchZoom(bool en);
    static void SetRotateGesture(bool en);
    
    // polling and performance
    static void SetPollingRate(uint16_t hz); // 125, 250, 500, 1000hz
    static uint16_t GetPollingRate();
    static void SetSmoothingLevel(uint8_t level); // 0-10, 0=none, 10=max
    
    // advanced device detection
    static void DetectExtendedCapabilities();
    static bool HasCapability(uint32_t capability_flag);
    static void PrintDeviceInfo();
    
    // gaming features
    static void SetDPIProfile(uint8_t profile_num, uint16_t dpi);
    static void SwitchDPIProfile(uint8_t profile_num);
    static uint8_t GetCurrentDPIProfile();
    
    // performance metrics
    struct PerformanceStats {
        uint32_t packets_processed;
        uint32_t events_generated;
        uint32_t avg_polling_rate;
        uint32_t max_jitter_us;
        uint32_t precision_errors;
    };
    static const PerformanceStats& GetPerformanceStats();
    
    // missing method used in implementation
    static DeviceType GetDeviceType();
    static void SetAutoDraw(bool enable);

    // usb hid: decode a boot-protocol mouse report and inject motion/button
    // events through the same ring as the ps/2 path (satoru).
    static void ProcessUSBReport(const uint8_t* report, int len);
private:
    static void Out(uint16_t p, uint8_t v);
    static uint8_t In(uint16_t p);
    static void WriteCmd(uint8_t c);
    static void WriteMouse(uint8_t c);
    static bool ExpectAck(int timeout_us);
    static bool CommandArg(uint8_t cmd, uint8_t arg);
    static uint8_t ReadID();
    static bool WaitInputBufferClear(int timeout_us);
    static bool ReadAuxByte(uint8_t& value, int timeout_us);
    static bool SendMouseByteAwaitAck(uint8_t value, int timeout_us, int max_attempts);
    static bool ReadControllerConfig(uint8_t& cfg);
    static bool WriteControllerConfig(uint8_t cfg);
    static bool ResetDevice(uint8_t& device_id);
    static void FlushOutput();
    static uint32_t BgAt(int x, int y);
    static void ClearAt(int x, int y);
    static uint8_t MapButtons(uint8_t hw);
    static DeviceType DetectDeviceType();
    static void InitVirtualBoxIntegration();
    static bool PollVirtualBoxAbsolute();
    static void InitVMwareIntegration();
    static bool PollVMwareAbsolute();
    static void EmitHostAbsoluteSample(int new_x, int new_y, uint8_t hw_buttons, int wheel_delta);
    static void RingPush(const Event& e);
    
    static int lastx, lasty;
    static uint8_t pkt[8];
    static uint8_t pkt_i;
    static bool left_down;
    static bool left_clicked;
    static bool right_clicked;
    static uint8_t buttons;
    static uint8_t prev_buttons;
    static uint8_t packet_len;
    static bool has_scroll;
    static bool has_xbuttons;
    static bool cursor_visible;
    static uint16_t speed_mul;
    static bool invert_scroll;
    static bool tap_to_click;
    static bool two_finger_scroll;
    static bool edge_scroll;
    static uint8_t palm_threshold;
    static uint16_t accel_mul;
    static uint8_t deadzone_px;
    static uint16_t sensitivity_mul;
    static bool natural_scroll;
    static uint8_t map_left;
    static uint8_t map_right;
    static uint8_t map_middle;
    static uint8_t map_x1;
    static uint8_t map_x2;
    static DeviceType device_type;
    static bool abs_mode;
    static uint8_t abs_proto;
    static int last_absx;
    static int last_absy;
    static int abs_maxx;
    static int abs_maxy;
    static int scroll_rest;
    static bool auto_draw;
    static Event events[256];
    static uint16_t ev_head;
    static uint16_t ev_tail;
};
