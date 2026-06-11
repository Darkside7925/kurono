#pragma once
#include "../kernel/types.h"

// Enhanced Display Controller Driver
// Handles VBE mode enumeration, display timing, and refresh rate configuration
// Supports up to 240Hz refresh rates with proper video timing

class DisplayController {
public:
    enum RefreshRate {
        REFRESH_60HZ = 60,
        REFRESH_75HZ = 75,
        REFRESH_120HZ = 120,
        REFRESH_144HZ = 144,
        REFRESH_165HZ = 165,
        REFRESH_180HZ = 180,
        REFRESH_240HZ = 240
    };

    struct DisplayMode {
        uint16_t width;
        uint16_t height;
        uint8_t bpp;
        uint16_t refresh_rate;
        uint16_t mode_id;
        uint32_t framebuffer_addr;
        uint16_t pitch;
        bool is_linear;
        bool supports_double_buffer;
        uint32_t memory_size;
        char description[64];
    };

    struct VbeInfo {
        char signature[4];      // "VESA"
        uint16_t version;       // VBE version
        uint32_t oem_string;
        uint32_t capabilities;
        uint32_t modes_ptr;
        uint16_t total_memory;  // in 64K blocks
        uint16_t oem_software_rev;
        uint32_t oem_vendor_name;
        uint32_t oem_product_name;
        uint32_t oem_product_rev;
        uint8_t reserved[222];
        uint8_t oem_data[256];
    } __attribute__((packed));

    struct VbeModeInfo {
        uint16_t attributes;
        uint8_t winA_attr;
        uint8_t winB_attr;
        uint16_t granularity;
        uint16_t winsize;
        uint16_t segmentA;
        uint16_t segmentB;
        uint32_t realFctPtr;
        uint16_t pitch;
        
        // >= VBE 1.2
        uint16_t x_res;
        uint16_t y_res;
        uint8_t x_char_size;
        uint8_t y_char_size;
        uint8_t planes;
        uint8_t bpp;
        uint8_t banks;
        uint8_t memory_model;
        uint8_t bank_size;
        uint8_t image_pages;
        uint8_t reserved1;
        
        // Direct color fields
        uint8_t red_mask_size;
        uint8_t red_position;
        uint8_t green_mask_size;
        uint8_t green_position;
        uint8_t blue_mask_size;
        uint8_t blue_position;
        uint8_t reserved_mask_size;
        uint8_t reserved_position;
        uint8_t directcolor_mode_info;
        
        // >= VBE 2.0
        uint32_t phys_base_ptr;
        uint32_t reserved2;
        uint16_t reserved3;
        
        // >= VBE 3.0
        uint16_t linear_bytes_per_scanline;
        uint8_t banked_image_pages;
        uint8_t linear_image_pages;
        uint8_t linear_red_mask_size;
        uint8_t linear_red_position;
        uint8_t linear_green_mask_size;
        uint8_t linear_green_position;
        uint8_t linear_blue_mask_size;
        uint8_t linear_blue_position;
        uint8_t linear_reserved_mask_size;
        uint8_t linear_reserved_position;
        uint32_t max_pixel_clock;
        uint8_t reserved4[189];
    } __attribute__((packed));

    struct GTF_Timing {
        uint32_t pixel_clock;
        uint16_t h_active;
        uint16_t h_blanking;
        uint16_t h_sync_offset;
        uint16_t h_sync_width;
        uint16_t v_active;
        uint16_t v_blanking;
        uint16_t v_sync_offset;
        uint16_t v_sync_width;
    };

    static bool Init();
    static bool EnumerateModes();
    static const DisplayMode* GetModes(int& count);
    static const DisplayMode* FindBestMode(uint16_t width, uint16_t height, uint8_t bpp, RefreshRate target_refresh);
    static bool SetMode(const DisplayMode* mode);
    static bool SetCustomMode(uint16_t width, uint16_t height, RefreshRate refresh);
    
    // Advanced timing functions
    static GTF_Timing CalculateGTFTiming(uint16_t width, uint16_t height, uint16_t refresh_rate);
    static bool SetCustomTiming(const GTF_Timing& timing);
    
    // Current mode info
    static const DisplayMode* GetCurrentMode();
    static bool IsDoubleBufferSupported();
    static uint32_t GetFramebufferAddress();
    static uint32_t GetBackbufferAddress();
    
    // VSync and frame control
    static void WaitVSync();
    static void EnableVSync(bool enable);
    static bool IsVSyncEnabled();
    static uint32_t GetCurrentScanline();
    static uint32_t GetRefreshRate();
    
    // Display features
    static bool SetGammaCorrection(float gamma);
    static bool SetBrightness(uint8_t brightness);
    static bool SetContrast(uint8_t contrast);
    
    // Multi-monitor support stubs
    static int GetDisplayCount();
    static bool SetPrimaryDisplay(int display_id);
    
private:
    static bool vbe_available;
    static VbeInfo vbe_info;
    static DisplayMode modes[64];
    static int mode_count;
    static DisplayMode current_mode;
    static bool vsync_enabled;
    static uint32_t backbuffer_addr;
    
    // VBE BIOS call wrappers
    static bool VbeCall(uint16_t function, uint16_t* ax, uint16_t* bx = nullptr, uint16_t* cx = nullptr, uint16_t* dx = nullptr, uint32_t* es = nullptr, uint32_t* di = nullptr);
    static bool GetVbeInfo();
    static bool GetModeInfo(uint16_t mode, VbeModeInfo* info);
    static bool SetVbeMode(uint16_t mode);
    
    // Real mode interrupt wrapper for VBE calls
    static bool RealModeInt10(uint16_t ax, uint16_t bx = 0, uint16_t cx = 0, uint16_t dx = 0, uint32_t es = 0, uint32_t di = 0);
    
    // Timing calculation helpers
    static uint32_t CalculatePixelClock(uint16_t width, uint16_t height, uint16_t refresh_rate);
    static void CalculateTimings(uint16_t width, uint16_t height, uint16_t refresh, GTF_Timing* timing);
    
    // Hardware register access for advanced features
    static void SetCRTC(uint8_t reg, uint8_t value);
    static uint8_t GetCRTC(uint8_t reg);
    static void SetSequencer(uint8_t reg, uint8_t value);
    static void SetAttributeController(uint8_t reg, uint8_t value);
    
    // Helper methods
    static void AddHighRefreshModes();
};