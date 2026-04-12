//  kurono os  -  display resolution manager implementation
//  detects display backend, provides unified resolution switching
#include "display_mgr.h"
#include "bga.h"
#include "graphics.h"
#include "virtio_gpu.h"
#include "gpu_probe.h"
#include "nvidia_gpu.h"
#include "amd_gpu.h"
#include "intel_gpu.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"

bool DisplayManager::initialized = false;
int DisplayManager::current_mode = -1;
DisplayBackend DisplayManager::backend = DISPLAY_BACKEND_NONE;
VSyncMode DisplayManager::vsync_mode = VSYNC_OFF;
float DisplayManager::scaling_factor = 1.0f;
uint8_t DisplayManager::brightness = 255;
float DisplayManager::gamma = 1.0f;
FramebufferInfo DisplayManager::fb_info = {};
MonitorInfo DisplayManager::monitor = {};
void* DisplayManager::back_buffer = nullptr;
bool DisplayManager::double_buffered = false;

int DisplayManager::num_modes = DISPLAY_MODE_COUNT;

DisplayMode DisplayManager::modes[DISPLAY_MODE_COUNT] = {
    {640,  480,  32, 60, false, "640x480@60"},
    {800,  600,  32, 60, false, "800x600@60"},
    {1024, 768,  32, 60, false, "1024x768@60"},
    {1280, 720,  32, 60, false, "1280x720@60"},
    {1280, 1024, 32, 60, false, "1280x1024@60"},
    {1366, 768,  32, 60, false, "1366x768@60"},
    {1600, 900,  32, 60, false, "1600x900@60"},
    {1920, 1080, 32, 60, false, "1920x1080@60"},
    {2560, 1440, 32, 60, false, "2560x1440@60"},
    {3840, 2160, 32, 60, false, "3840x2160@60"}
};

bool DisplayManager::Init() {
    initialized = false;
    backend = DISPLAY_BACKEND_NONE;

    // try virtio gpu first (highest quality in vm)
    if (VirtIOGPU::IsDetected()) {
        backend = DISPLAY_BACKEND_VIRTIO_GPU;
        if (InitVirtIOGPU()) {
            initialized = true;
            return true;
        }
    }

    // fall back to bga (bochs/qemu standard)
    if (BGA::IsAvailable()) {
        backend = DISPLAY_BACKEND_BGA;
        if (InitBGA()) {
            initialized = true;
            return true;
        }
    }

    // use native graphics framebuffer  -  classify backend from gpuprobe
    if (Graphics::GetWidth() > 0 && Graphics::GetHeight() > 0) {
        // determine the actual backend from detected gpu topology
        const GpuProbeResult& gpr = GpuProbe::GetResult();
        if (gpr.primary_idx >= 0) {
            uint16_t vid = gpr.gpus[gpr.primary_idx].vendor_id;
            if (vid == GPU_VENDOR_INTEL)       backend = DISPLAY_BACKEND_INTEL;
            else if (vid == GPU_VENDOR_NVIDIA)  backend = DISPLAY_BACKEND_NVIDIA;
            else if (vid == GPU_VENDOR_AMD)     backend = DISPLAY_BACKEND_AMD;
            else                                backend = DISPLAY_BACKEND_BGA;
        } else {
            // no primary detected  -  check individual drivers
            if (IntelGPU::IsDetected())         backend = DISPLAY_BACKEND_INTEL;
            else if (NvidiaGPU::IsDetected())   backend = DISPLAY_BACKEND_NVIDIA;
            else if (AmdGPU::IsAvailable())     backend = DISPLAY_BACKEND_AMD;
            else                                backend = DISPLAY_BACKEND_BGA;
        }

        fb_info.address = (void*)Graphics::GetBuffer();
        fb_info.width = Graphics::GetWidth();
        fb_info.height = Graphics::GetHeight();
        fb_info.bpp = Graphics::GetBpp();
        fb_info.pitch = Graphics::GetPitch();
        fb_info.size = fb_info.pitch * fb_info.height;
        fb_info.double_buffered = false;

        // find matching mode
        for (int i = 0; i < num_modes; i++) {
            if (modes[i].width == fb_info.width && modes[i].height == fb_info.height) {
                current_mode = i;
                break;
            }
        }

        initialized = true;
        return true;
    }

    return false;
}

bool DisplayManager::InitBGA() {
    // bga should already be initialized by the kernel
    fb_info.width = BGA::width;
    fb_info.height = BGA::height;
    fb_info.bpp = BGA::bpp;
    fb_info.pitch = fb_info.width * (fb_info.bpp / 8);
    fb_info.address = (void*)BGA::GetFramebuffer();
    fb_info.size = fb_info.pitch * fb_info.height;
    fb_info.double_buffered = false;

    // find matching mode
    for (int i = 0; i < num_modes; i++) {
        if (modes[i].width == fb_info.width && modes[i].height == fb_info.height) {
            current_mode = i;
            break;
        }
    }

    // read edid if available
    ReadEDID(&monitor);

    return fb_info.address != nullptr;
}

bool DisplayManager::InitVirtIOGPU() {
    uint32_t w = 0, h = 0;
    if (!VirtIOGPU::GetDisplayInfo(0, &w, &h)) {
        w = 1920;
        h = 1080;
    }

    fb_info.width = w;
    fb_info.height = h;
    fb_info.bpp = 32;
    fb_info.pitch = w * 4;
    fb_info.size = fb_info.pitch * h;

    // allocate framebuffer
    int pages = (fb_info.size + 4095) / 4096;
    fb_info.address = KernelHeap::Alloc(pages * 4096);
    if (!fb_info.address) return false;

    // create virtio gpu resource
    uint32_t res = VirtIOGPU::CreateResource2D(w, h, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
    if (res == 0) return false;

    VirtIOGPU::AttachBacking(res, fb_info.address, fb_info.size);
    VirtIOGPU::SetScanout(0, res, 0, 0, w, h);

    fb_info.double_buffered = false;

    for (int i = 0; i < num_modes; i++) {
        if (modes[i].width == w && modes[i].height == h) {
            current_mode = i;
            break;
        }
    }

    return true;
}

bool DisplayManager::SetMode(int mode_index) {
    if (mode_index < 0 || mode_index >= num_modes) return false;
    return SetResolution(modes[mode_index].width, modes[mode_index].height, modes[mode_index].bpp);
}

bool DisplayManager::SetResolution(uint32_t width, uint32_t height, uint8_t bpp) {
    if (!initialized) return false;

    bool result = false;
    switch (backend) {
        case DISPLAY_BACKEND_BGA:
            result = SetModeBGA(width, height, bpp);
            break;
        case DISPLAY_BACKEND_VIRTIO_GPU:
            result = SetModeVirtIOGPU(width, height, bpp);
            break;
        default:
            return false;
    }

    if (result) {
        fb_info.width = width;
        fb_info.height = height;
        fb_info.bpp = bpp;
        fb_info.pitch = width * (bpp / 8);
        fb_info.size = fb_info.pitch * height;

        // update current mode index
        current_mode = -1;
        for (int i = 0; i < num_modes; i++) {
            if (modes[i].width == width && modes[i].height == height) {
                current_mode = i;
                break;
            }
        }

        // reallocate back buffer if double buffering is on
        if (double_buffered) {
            // free old back buffer (if we had a free function)
            int pages = (fb_info.size + 4095) / 4096;
            back_buffer = KernelHeap::Alloc(pages * 4096);
        }

        // update graphics subsystem
        Graphics::ReinitForResolution((uintptr_t)fb_info.address, width, height, fb_info.pitch, bpp);
    }

    return result;
}

bool DisplayManager::SetModeBGA(uint32_t w, uint32_t h, uint8_t bpp) {
    BGA::SetMode(w, h, bpp);
    fb_info.address = (void*)BGA::GetFramebuffer();
    return fb_info.address != nullptr;
}

bool DisplayManager::SetModeVirtIOGPU(uint32_t w, uint32_t h, uint8_t bpp) {
    (void)bpp;

    // destroy old resource, create new one
    // for simplicity, create a new resource
    int pages = (w * h * 4 + 4095) / 4096;
    void* new_fb = KernelHeap::Alloc(pages * 4096);
    if (!new_fb) return false;

    uint32_t res = VirtIOGPU::CreateResource2D(w, h, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
    if (res == 0) return false;

    VirtIOGPU::AttachBacking(res, new_fb, w * h * 4);
    VirtIOGPU::SetScanout(0, res, 0, 0, w, h);

    fb_info.address = new_fb;
    return true;
}

int DisplayManager::GetCurrentMode() { return current_mode; }
const DisplayMode* DisplayManager::GetMode(int index) {
    if (index < 0 || index >= num_modes) return nullptr;
    return &modes[index];
}
int DisplayManager::GetModeCount() { return num_modes; }

uint32_t DisplayManager::GetWidth() { return fb_info.width; }
uint32_t DisplayManager::GetHeight() { return fb_info.height; }
uint8_t DisplayManager::GetBpp() { return fb_info.bpp; }
uint32_t DisplayManager::GetPitch() { return fb_info.pitch; }
void* DisplayManager::GetFramebuffer() { return fb_info.address; }
const FramebufferInfo& DisplayManager::GetFBInfo() { return fb_info; }

bool DisplayManager::GetSupportedModes(DisplayMode* out, int max_count, int* count) {
    int n = num_modes;
    if (n > max_count) n = max_count;
    for (int i = 0; i < n; i++) out[i] = modes[i];
    if (count) *count = n;
    return true;
}

bool DisplayManager::EnableDoubleBuffering() {
    if (!initialized) return false;

    int pages = (fb_info.size + 4095) / 4096;
    back_buffer = KernelHeap::Alloc(pages * 4096);
    if (!back_buffer) return false;

    double_buffered = true;
    fb_info.double_buffered = true;
    return true;
}

void DisplayManager::SwapBuffers() {
    if (!double_buffered || !back_buffer || !fb_info.address) return;

    // use non-temporal stores to bypass cpu cache → directly reach gpu vram.
    // regular stores to write-combining memory can sit in wc buffers and
    // never become visible on real hardware (permanent black screen).
    uint8_t* dst = (uint8_t*)fb_info.address;
    const uint8_t* src = (const uint8_t*)back_buffer;
    uint32_t remaining = fb_info.size;

    // align destination to 16 bytes
    while (remaining > 0 && ((uintptr_t)dst & 15)) {
        *dst++ = *src++; remaining--;
    }

    // 128-bit non-temporal stores (sse2  -  guaranteed on x86_64)
    while (remaining >= 64) {
        __asm__ __volatile__(
            "movdqu   (%0), %%xmm0;  movdqu 16(%0), %%xmm1;"
            "movdqu 32(%0), %%xmm2;  movdqu 48(%0), %%xmm3;"
            "movntdq %%xmm0,   (%1); movntdq %%xmm1, 16(%1);"
            "movntdq %%xmm2, 32(%1); movntdq %%xmm3, 48(%1);"
            :: "r"(src), "r"(dst)
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
        );
        dst += 64; src += 64; remaining -= 64;
    }
    while (remaining >= 16) {
        __asm__ __volatile__(
            "movdqu (%0), %%xmm0; movntdq %%xmm0, (%1);"
            :: "r"(src), "r"(dst)
            : "xmm0", "memory"
        );
        dst += 16; src += 16; remaining -= 16;
    }
    while (remaining > 0) { *dst++ = *src++; remaining--; }

    // fence: ensure all nt stores are globally visible before returning
    __asm__ __volatile__("sfence" ::: "memory");

    // if using virtio gpu, also flush to host
    if (backend == DISPLAY_BACKEND_VIRTIO_GPU) {
        VirtIOGPU::PresentFramebuffer(fb_info.address, fb_info.width, fb_info.height);
    }
}

void* DisplayManager::GetBackBuffer() {
    return double_buffered ? back_buffer : fb_info.address;
}

void DisplayManager::SetVSync(VSyncMode mode) { vsync_mode = mode; }
VSyncMode DisplayManager::GetVSync() { return vsync_mode; }

void DisplayManager::WaitVSync() {
    if (vsync_mode == VSYNC_OFF) return;

    // for bga/qemu, we can approximate vsync by waiting for vga retrace
    // vga input status register 1 (port 0x3da)
    // bit 3: vertical retrace
    if (backend == DISPLAY_BACKEND_BGA) {
        // wait for retrace to end
        while (inb(0x3DA) & 0x08);
        // wait for retrace to start
        while (!(inb(0x3DA) & 0x08));
    }
}

uint32_t DisplayManager::GetRefreshRate() {
    if (current_mode >= 0 && current_mode < num_modes)
        return modes[current_mode].refresh_rate;
    return 60;
}

void DisplayManager::SetScaling(float factor) {
    if (factor >= 0.5f && factor <= 4.0f) scaling_factor = factor;
}
float DisplayManager::GetScaling() { return scaling_factor; }

bool DisplayManager::ReadEDID(MonitorInfo* info) {
    if (!info) return false;

    // edid is typically read via ddc/i2c (gmbus on intel, aux on displayport).
    // for now, populate from detected gpu info + current resolution.
    info->connected = true;
    info->native_width = fb_info.width;
    info->native_height = fb_info.height;
    info->max_width = 3840;
    info->max_height = 2160;
    info->max_refresh = 60;
    info->physical_width_cm = 60.0f;
    info->physical_height_cm = 34.0f;

    // name from backend
    const char* name = "Unknown";
    switch (backend) {
        case DISPLAY_BACKEND_BGA:        name = "QEMU Monitor"; break;
        case DISPLAY_BACKEND_VIRTIO_GPU: name = "VirtIO Disp"; break;
        case DISPLAY_BACKEND_INTEL:      name = "Intel Panel"; break;
        case DISPLAY_BACKEND_NVIDIA:     name = "NV Display"; break;
        case DISPLAY_BACKEND_AMD:        name = "AMD Display"; break;
        default:                         name = "Display"; break;
    }
    int i = 0;
    while (name[i] && i < 13) { info->name[i] = name[i]; i++; }
    info->name[i] = 0;

    monitor = *info;
    return true;
}

const MonitorInfo& DisplayManager::GetMonitorInfo() { return monitor; }

DisplayBackend DisplayManager::GetBackend() { return backend; }

const char* DisplayManager::GetBackendName() {
    switch (backend) {
        case DISPLAY_BACKEND_BGA:        return "BGA/VBE (QEMU stdvga)";
        case DISPLAY_BACKEND_VIRTIO_GPU: return "VirtIO GPU";
        case DISPLAY_BACKEND_NVIDIA:     return "NVIDIA";
        case DISPLAY_BACKEND_INTEL:      return "Intel HD Graphics";
        case DISPLAY_BACKEND_AMD:        return "AMD Radeon";
        default:                         return "None";
    }
}

void DisplayManager::SetBrightness(uint8_t level) { brightness = level; }
uint8_t DisplayManager::GetBrightness() { return brightness; }
void DisplayManager::SetGamma(float g) {
    if (g >= 0.5f && g <= 3.0f) gamma = g;
}
float DisplayManager::GetGamma() { return gamma; }

void DisplayManager::DumpInfo(char* out, int max_len) {
    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < max_len - 1) out[pos++] = *s++;
    };
    auto append_num = [&](uint32_t val) {
        char buf[12]; int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else { char rev[12]; int ri = 0; uint32_t tmp = val;
            while (tmp) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            while (ri--) buf[i++] = rev[ri]; }
        buf[i] = 0; append(buf);
    };

    if (!initialized) {
        append("Display: Not initialized\n");
        out[pos] = 0; return;
    }

    append("Display Manager\n");
    append("  Backend: "); append(GetBackendName()); append("\n");
    append("  Resolution: "); append_num(fb_info.width);
    append("x"); append_num(fb_info.height);
    append("@"); append_num(fb_info.bpp); append("bpp\n");
    append("  Pitch: "); append_num(fb_info.pitch); append(" bytes/line\n");
    append("  Framebuffer: "); append_num(fb_info.size / 1024); append(" KB\n");
    append("  Double Buffer: "); append(double_buffered ? "Yes" : "No"); append("\n");

    append("  VSync: ");
    switch (vsync_mode) {
        case VSYNC_OFF:      append("Off"); break;
        case VSYNC_ON:       append("On"); break;
        case VSYNC_ADAPTIVE: append("Adaptive"); break;
    }
    append("\n");

    append("  Refresh: "); append_num(GetRefreshRate()); append(" Hz\n");
    append("  Brightness: "); append_num(brightness); append("/255\n");

    append("  Monitor: "); append(monitor.name); append("\n");
    append("  Native: "); append_num(monitor.native_width);
    append("x"); append_num(monitor.native_height); append("\n");

    append("  Available Modes:\n");
    for (int i = 0; i < num_modes; i++) {
        append("    ");
        if (i == current_mode) append("[*] "); else append("[ ] ");
        append(modes[i].name); append("\n");
    }

    out[pos] = 0;
}
