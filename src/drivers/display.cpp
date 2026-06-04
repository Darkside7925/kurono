#include "display.h"
#include "serial.h"
#include "../hal/hal.h"
#include "../kernel/types.h"

// simple sprintf implementation for basic formatting
int sprintf(char* str, const char* format, ...) {
    // very basic sprintf - just handles %d, %x, %s
    const char* src = format;
    char* dst = str;
    uint32_t arg_idx = 0;
    
    // get variadic args (simplified, assumes 32-bit values)
    uint32_t* args = (uint32_t*)((char*)&format + sizeof(format));
    
    while (*src) {
        if (*src == '%') {
            src++;
            if (*src == 'd') {
                int val = (int)args[arg_idx++];
                if (val < 0) { *dst++ = '-'; val = -val; }
                char temp[16]; int i = 0;
                do { temp[i++] = '0' + (val % 10); val /= 10; } while (val);
                while (i > 0) *dst++ = temp[--i];
            } else if (*src == 'x') {
                uint32_t val = args[arg_idx++];
                char temp[16]; int i = 0;
                do { int d = val % 16; temp[i++] = (d < 10) ? '0' + d : 'a' + d - 10; val /= 16; } while (val);
                while (i > 0) *dst++ = temp[--i];
            } else if (*src == 's') {
                char* s = (char*)args[arg_idx++];
                while (s && *s) *dst++ = *s++;
            } else {
                *dst++ = *src;
            }
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = 0;
    return dst - str;
}

int abs(int x) { return x < 0 ? -x : x; }
int max(int a, int b) { return a > b ? a : b; }

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = 0;
    return dst;
}

bool DisplayController::vbe_available = false;
DisplayController::VbeInfo DisplayController::vbe_info = {};
DisplayController::DisplayMode DisplayController::modes[64] = {};
int DisplayController::mode_count = 0;
DisplayController::DisplayMode DisplayController::current_mode = {};
bool DisplayController::vsync_enabled = true;
uint32_t DisplayController::backbuffer_addr = 0;

bool DisplayController::Init() {
    SerialLogger::Log("Display: Initializing Enhanced Display Controller...\r\n");
    vbe_available = false;
    mode_count = 0;

    if (!GetVbeInfo()) {
        SerialLogger::Log("Display: VBE not available, fallback to basic mode\r\n");
        return false;
    }

    vbe_available = true;
    SerialLogger::Log("Display: VBE ");
    SerialLogger::LogHex(vbe_info.version);
    SerialLogger::Log(" detected, ");
    SerialLogger::LogDec(vbe_info.total_memory * 64);
    SerialLogger::Log("KB VRAM\r\n");

    if (!EnumerateModes()) {
        SerialLogger::Log("Display: Failed to enumerate modes\r\n");
        vbe_available = false;
        mode_count = 0;
        return false;
    }

    SerialLogger::Log("Display: Found ");
    SerialLogger::LogDec(mode_count);
    SerialLogger::Log(" display modes\r\n");

    return true;
}

bool DisplayController::EnumerateModes() {
    if (!vbe_available) return false;
    
    mode_count = 0;
    
    // get mode list pointer
    uint16_t* mode_list = (uint16_t*)vbe_info.modes_ptr;
    if (!mode_list) return false;
    
    for (int i = 0; mode_list[i] != 0xFFFF && mode_count < 64; i++) {
        uint16_t mode_id = mode_list[i];
        VbeModeInfo mode_info;
        
        if (GetModeInfo(mode_id, &mode_info)) {
            // filter for linear framebuffer modes with reasonable specs
            if ((mode_info.attributes & 0x90) == 0x90 && // linear framebuffer supported & available
                mode_info.phys_base_ptr != 0 &&
                mode_info.bpp >= 24 &&
                mode_info.x_res >= 640 && mode_info.y_res >= 480) {
                
                DisplayMode& mode = modes[mode_count];
                mode.width = mode_info.x_res;
                mode.height = mode_info.y_res;
                mode.bpp = mode_info.bpp;
                mode.mode_id = mode_id | 0x4000; // set linear framebuffer bit
                mode.framebuffer_addr = mode_info.phys_base_ptr;
                mode.pitch = mode_info.linear_bytes_per_scanline > 0 ? 
                           mode_info.linear_bytes_per_scanline : mode_info.pitch;
                mode.is_linear = true;
                mode.supports_double_buffer = (mode_info.image_pages > 0);
                mode.memory_size = mode.pitch * mode.height;
                
                // estimate refresh rate based on pixel clock and resolution
                // this is approximate since vbe doesn't always provide exact refresh rates
                if (mode_info.max_pixel_clock > 0) {
                    uint32_t total_pixels = (mode.width + 160) * (mode.height + 45); // add blanking estimate
                    mode.refresh_rate = mode_info.max_pixel_clock / (total_pixels / 1000000);
                    if (mode.refresh_rate > 240) mode.refresh_rate = 60; // fallback
                } else {
                    mode.refresh_rate = 60; // default
                }
                
                // build description
                char res_str[32];
                sprintf(res_str, "%dx%d@%dHz %dbit", mode.width, mode.height, mode.refresh_rate, mode.bpp);
                strncpy(mode.description, res_str, 63);
                mode.description[63] = 0;
                
                mode_count++;
            }
        }
    }
    
    // add some common high refresh rate modes manually if supported
    AddHighRefreshModes();
    
    return mode_count > 0;
}

void DisplayController::AddHighRefreshModes() {
    // try to add high refresh rate variants of common resolutions
    struct { uint16_t w, h; RefreshRate rates[4]; } common_modes[] = {
        { 1920, 1080, { REFRESH_144HZ, REFRESH_165HZ, REFRESH_180HZ, (RefreshRate)0 } },
        { 1440, 900,  { REFRESH_144HZ, REFRESH_180HZ, (RefreshRate)0, (RefreshRate)0 } },
        { 1024, 768,  { REFRESH_120HZ, REFRESH_144HZ, REFRESH_180HZ, (RefreshRate)0 } },
        { 0, 0, { (RefreshRate)0, (RefreshRate)0, (RefreshRate)0, (RefreshRate)0 } }
    };
    
    for (int i = 0; common_modes[i].w != 0 && mode_count < 60; i++) {
        for (int r = 0; r < 4 && common_modes[i].rates[r] != 0 && mode_count < 60; r++) {
            // check if we already have this resolution
            bool found = false;
            for (int m = 0; m < mode_count; m++) {
                if (modes[m].width == common_modes[i].w && modes[m].height == common_modes[i].h) {
                    found = true;
                    break;
                }
            }
            
            if (found) {
                // add high refresh rate variant
                DisplayMode& mode = modes[mode_count];
                mode.width = common_modes[i].w;
                mode.height = common_modes[i].h;
                mode.bpp = 32;
                mode.refresh_rate = common_modes[i].rates[r];
                mode.mode_id = 0x8000 | mode_count; // custom mode flag
                mode.framebuffer_addr = 0; // will be set when mode is activated
                mode.pitch = mode.width * 4;
                mode.is_linear = true;
                mode.supports_double_buffer = true;
                mode.memory_size = mode.pitch * mode.height;
                
                sprintf(mode.description, "%dx%d@%dHz 32bit (GTF)", 
                       mode.width, mode.height, mode.refresh_rate);
                
                mode_count++;
            }
        }
    }
}

const DisplayController::DisplayMode* DisplayController::FindBestMode(uint16_t width, uint16_t height, uint8_t bpp, RefreshRate target_refresh) {
    const DisplayMode* best = nullptr;
    int best_score = -1;
    
    for (int i = 0; i < mode_count; i++) {
        const DisplayMode& mode = modes[i];
        
        // calculate score based on how close the mode matches our requirements
        int score = 0;
        
        // exact resolution match gets highest priority
        if (mode.width == width && mode.height == height) score += 1000;
        else if (mode.width >= width && mode.height >= height) score += 500;
        else score += 250;
        
        // bpp match
        if (mode.bpp == bpp) score += 100;
        else if (mode.bpp > bpp) score += 50;
        
        // refresh rate match
        int refresh_diff = abs(mode.refresh_rate - target_refresh);
        score += max(0, 200 - refresh_diff);
        
        // prefer linear framebuffer
        if (mode.is_linear) score += 50;
        
        // prefer double buffer support
        if (mode.supports_double_buffer) score += 25;
        
        if (score > best_score) {
            best_score = score;
            best = &mode;
        }
    }
    
    return best;
}

bool DisplayController::SetMode(const DisplayMode* mode) {
    if (!mode) return false;
    
    SerialLogger::Log("Display: Setting mode ");
    SerialLogger::Log(mode->description);
    SerialLogger::Log("\r\n");
    
    bool success = false;
    
    if (mode->mode_id & 0x8000) {
        // custom mode - use gtf timing
        success = SetCustomMode(mode->width, mode->height, (RefreshRate)mode->refresh_rate);
    } else {
        // standard vbe mode
        success = SetVbeMode(mode->mode_id);
    }
    
    if (success) {
        current_mode = *mode;
        
        // set up backbuffer if supported
        if (mode->supports_double_buffer && mode->memory_size > 0) {
            backbuffer_addr = mode->framebuffer_addr + mode->memory_size;
            SerialLogger::Log("Display: Double buffering enabled\r\n");
        }
        
        SerialLogger::Log("Display: Mode set successfully\r\n");
        return true;
    }
    
    SerialLogger::Log("Display: Failed to set mode\r\n");
    return false;
}

bool DisplayController::SetCustomMode(uint16_t width, uint16_t height, RefreshRate refresh) {
    GTF_Timing timing = CalculateGTFTiming(width, height, refresh);
    return SetCustomTiming(timing);
}

DisplayController::GTF_Timing DisplayController::CalculateGTFTiming(uint16_t width, uint16_t height, uint16_t refresh_rate) {
    GTF_Timing timing = {0};
    
    // gtf (generalized timing formula) calculations
    // these are simplified - real gtf is more complex
    
    timing.h_active = width;
    timing.v_active = height;
    
    // estimate horizontal blanking (typically 20-25% of active)
    timing.h_blanking = width / 4;
    timing.h_sync_width = timing.h_blanking / 8;
    timing.h_sync_offset = timing.h_blanking / 2;
    
    // estimate vertical blanking
    timing.v_blanking = height / 20; // ~5%
    timing.v_sync_width = 3;
    timing.v_sync_offset = timing.v_blanking / 2;
    
    // calculate pixel clock
    uint32_t total_h = timing.h_active + timing.h_blanking;
    uint32_t total_v = timing.v_active + timing.v_blanking;
    timing.pixel_clock = total_h * total_v * refresh_rate;
    
    return timing;
}

bool DisplayController::SetCustomTiming(const GTF_Timing& timing) {
    // this would require direct hardware programming
    // for now, just log what we would do
    SerialLogger::Log("Display: Custom timing - Pixel Clock: ");
    SerialLogger::LogDec(timing.pixel_clock / 1000000);
    SerialLogger::Log("MHz\r\n");
    
    // in a real implementation, this would program the display controller directly
    // via registers like crtc, sequencer, etc.
    
    return false; // not implemented yet
}

void DisplayController::WaitVSync() {
    if (!vsync_enabled) return;

    // input status register 1 (0x3da/color). bit 3 = vertical retrace.
    // Modern EFI/non-VGA GPUs do not wire 0x3da; the port reads 0xff or
    // 0x00 in those cases. We do a quick probe and bail out  -  falling back
    // to the caller's frame-pacing yield path is far better than burning
    // a million cycles in a tight loop on real hardware.
    static bool vsync_dead = false;
    if (vsync_dead) return;

    uint8_t probe = HAL::InByte(0x3DA);
    if (probe == 0xFF || probe == 0x00) {
        vsync_dead = true;
        return;
    }

    volatile int timeout = 100000;
    while ((HAL::InByte(0x3DA) & 0x08) && --timeout > 0);
    if (timeout <= 0) { vsync_dead = true; return; }

    timeout = 100000;
    while (!(HAL::InByte(0x3DA) & 0x08) && --timeout > 0);
    if (timeout <= 0) { vsync_dead = true; return; }
}

uint32_t DisplayController::GetCurrentScanline() {
    // read current scanline from crtc registers
    uint8_t low = GetCRTC(0x0C);
    uint8_t high = GetCRTC(0x0D);
    return (high << 8) | low;
}

void DisplayController::SetCRTC(uint8_t reg, uint8_t value) {
    HAL::OutByte(0x3D4, reg);
    HAL::OutByte(0x3D5, value);
}

uint8_t DisplayController::GetCRTC(uint8_t reg) {
    HAL::OutByte(0x3D4, reg);
    return HAL::InByte(0x3D5);
}

bool DisplayController::GetVbeInfo() {
    // on real hardware, vbe bios calls are not available in long mode.
    // the display is already set up by grub via efi gop or vbe before
    // transferring control.  simulating vbe info here would cause
    // enumeratemodes() to dereference modes_ptr=0, reading garbage from
    // the ivt at physical address 0.  return false to indicate vbe is
    // not directly available  -  the kernel already has the framebuffer
    // from multiboot info.
    return false;
}

bool DisplayController::GetModeInfo(uint16_t mode_id, VbeModeInfo* info) {
    // simulate some common modes
    static const struct { uint16_t id, w, h; uint8_t bpp; } sim_modes[] = {
        { 0x101, 640, 480, 8 }, { 0x103, 800, 600, 8 }, { 0x105, 1024, 768, 8 },
        { 0x110, 640, 480, 15 }, { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 24 },
        { 0x113, 800, 600, 15 }, { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 24 },
        { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 24 },
        { 0x11A, 1280, 1024, 15 }, { 0x11B, 1280, 1024, 16 }, { 0x11C, 1280, 1024, 24 },
        { 0x140, 1400, 1050, 32 }, { 0x141, 1600, 1200, 32 }, { 0x142, 1920, 1080, 32 },
        { 0, 0, 0, 0 }
    };
    
    for (int i = 0; sim_modes[i].id != 0; i++) {
        if (sim_modes[i].id == mode_id) {
            memset(info, 0, sizeof(VbeModeInfo));
            info->attributes = 0x9B; // linear framebuffer supported
            info->x_res = sim_modes[i].w;
            info->y_res = sim_modes[i].h;
            info->bpp = sim_modes[i].bpp;
            info->pitch = sim_modes[i].w * (sim_modes[i].bpp / 8);
            info->phys_base_ptr = 0xE0000000; // typical framebuffer address
            info->linear_bytes_per_scanline = info->pitch;
            info->memory_model = (sim_modes[i].bpp >= 15) ? 6 : 4; // direct color or packed pixel
            info->max_pixel_clock = 400000000; // 400mhz max
            return true;
        }
    }
    
    return false;
}

bool DisplayController::SetVbeMode(uint16_t mode) {
    // simulate setting vbe mode
    SerialLogger::Log("Display: VBE mode ");
    SerialLogger::LogHex(mode);
    SerialLogger::Log(" set\r\n");
    return true;
}

const DisplayController::DisplayMode* DisplayController::GetModes(int& count) {
    count = mode_count;
    return modes;
}

const DisplayController::DisplayMode* DisplayController::GetCurrentMode() {
    return &current_mode;
}

bool DisplayController::IsDoubleBufferSupported() {
    return current_mode.supports_double_buffer;
}

uint32_t DisplayController::GetFramebufferAddress() {
    return current_mode.framebuffer_addr;
}

uint32_t DisplayController::GetBackbufferAddress() {
    return backbuffer_addr;
}

void DisplayController::EnableVSync(bool enable) {
    vsync_enabled = enable;
}

bool DisplayController::IsVSyncEnabled() {
    return vsync_enabled;
}

uint32_t DisplayController::GetRefreshRate() {
    return current_mode.refresh_rate;
}