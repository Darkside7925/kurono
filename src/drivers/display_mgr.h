#pragma once
//  kurono os  -  display resolution manager
//  dynamic resolution switching, vsync control, multi-monitor support
//  wraps bga, virtio gpu, and native gpu drivers
#include "../kernel/types.h"

struct DisplayMode {
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  refresh_rate;    // hz (60, 75, 120, 144)
    bool     interlaced;
    const char* name;         // "1920x1080@60"
};

#define DISPLAY_MODE_640x480     0
#define DISPLAY_MODE_800x600     1
#define DISPLAY_MODE_1024x768    2
#define DISPLAY_MODE_1280x720    3
#define DISPLAY_MODE_1280x1024   4
#define DISPLAY_MODE_1366x768    5
#define DISPLAY_MODE_1600x900    6
#define DISPLAY_MODE_1920x1080   7
#define DISPLAY_MODE_2560x1440   8
#define DISPLAY_MODE_3840x2160   9
#define DISPLAY_MODE_COUNT       10

enum DisplayBackend {
    DISPLAY_BACKEND_NONE,
    DISPLAY_BACKEND_BGA,        // bochs vbe / qemu stdvga
    DISPLAY_BACKEND_VIRTIO_GPU, // virtio gpu
    DISPLAY_BACKEND_NVIDIA,     // nvidia discrete gpu
    DISPLAY_BACKEND_INTEL,      // intel integrated
    DISPLAY_BACKEND_AMD         // amd discrete gpu
};

enum VSyncMode {
    VSYNC_OFF,
    VSYNC_ON,
    VSYNC_ADAPTIVE
};

struct FramebufferInfo {
    void*    address;        // linear framebuffer base
    uint32_t width;
    uint32_t height;
    uint32_t pitch;          // bytes per scanline
    uint8_t  bpp;            // bits per pixel
    uint32_t size;           // total framebuffer size in bytes
    bool     double_buffered;
};

struct MonitorInfo {
    bool     connected;
    char     name[14];       // monitor name from edid
    uint32_t native_width;   // native/preferred resolution
    uint32_t native_height;
    uint32_t max_width;
    uint32_t max_height;
    uint8_t  max_refresh;
    float    physical_width_cm;
    float    physical_height_cm;
};

class DisplayManager {
public:
    static bool Init();

    // resolution
    static bool SetMode(int mode_index);
    static bool SetResolution(uint32_t width, uint32_t height, uint8_t bpp);
    static int  GetCurrentMode();
    static const DisplayMode* GetMode(int index);
    static int  GetModeCount();
    static bool GetSupportedModes(DisplayMode* modes, int max_count, int* count);

    // current display info
    static uint32_t GetWidth();
    static uint32_t GetHeight();
    static uint8_t  GetBpp();
    static uint32_t GetPitch();
    static void*    GetFramebuffer();
    static const FramebufferInfo& GetFBInfo();

    // double buffering
    static bool EnableDoubleBuffering();
    static void SwapBuffers();
    static void* GetBackBuffer();

    // vsync
    static void SetVSync(VSyncMode mode);
    static VSyncMode GetVSync();
    static void WaitVSync();
    static uint32_t GetRefreshRate();

    // scaling
    static void SetScaling(float factor);  // 1.0 = 100%, 1.5 = 150%, 2.0 = 200%
    static float GetScaling();

    // monitor info
    static bool ReadEDID(MonitorInfo* info);
    static const MonitorInfo& GetMonitorInfo();

    // backend info
    static DisplayBackend GetBackend();
    static const char* GetBackendName();

    // gamma/brightness
    static void SetBrightness(uint8_t level);  // 0-255
    static uint8_t GetBrightness();
    static void SetGamma(float gamma);          // 0.5 - 3.0
    static float GetGamma();

    static void DumpInfo(char* out, int max_len);

private:
    static bool initialized;
    static int current_mode;
    static DisplayBackend backend;
    static VSyncMode vsync_mode;
    static float scaling_factor;
    static uint8_t brightness;
    static float gamma;
    static FramebufferInfo fb_info;
    static MonitorInfo monitor;

    // double buffering
    static void* back_buffer;
    static bool double_buffered;

    // available modes
    static DisplayMode modes[DISPLAY_MODE_COUNT];
    static int num_modes;

    // backend-specific init
    static bool InitBGA();
    static bool InitVirtIOGPU();
    static bool SetModeBGA(uint32_t w, uint32_t h, uint8_t bpp);
    static bool SetModeVirtIOGPU(uint32_t w, uint32_t h, uint8_t bpp);
};
