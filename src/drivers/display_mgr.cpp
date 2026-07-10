//  kurono os - display resolution manager implementation
//  detects display backend, provides unified resolution switching
#include "display_mgr.h"
#include "bga.h"
#include "graphics.h"
#include "virtio_gpu.h"
#include "gpu_probe.h"
#include "nvidia_gpu.h"
#include "amd_gpu.h"
#include "intel_gpu.h"
#include "serial.h"
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
MonitorInfo DisplayManager::monitors[DISPLAY_MAX_MONITORS] = {};
int DisplayManager::monitor_count = 0;
FramebufferInfo DisplayManager::fb_secondary = {};
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
            DetectDisplays();   // build multi-monitor list (satoru)
            return true;
        }
    }

    // fall back to bga (bochs/qemu standard)
    if (BGA::IsAvailable()) {
        backend = DISPLAY_BACKEND_BGA;
        if (InitBGA()) {
            initialized = true;
            DetectDisplays();   // build multi-monitor list (satoru)
            return true;
        }
    }

    // use native graphics framebuffer - classify backend from gpuprobe
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
            // no primary detected - check individual drivers
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
        DetectDisplays();   // build multi-monitor list (satoru)
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
    // default to 1080p. qemu's virtio-gpu advertises a 1280x800 "preferred"
    // mode through GetDisplayInfo, but the guest drives the real scanout size
    // and the host window resizes to match whatever we set -- so we create a
    // 1920x1080 resource. only adopt the host-reported geometry when it is
    // strictly larger (e.g. a 4k panel on real hardware) so we never downgrade
    // a bigger display below 1080p. (satoru)
    uint32_t w = 1920, h = 1080;
    uint32_t hw = 0, hh = 0;
    if (VirtIOGPU::GetDisplayInfo(0, &hw, &hh) && hw >= 1920 && hh >= 1080) {
        w = hw;
        h = hh;
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

    // point Graphics at this virtio backing buffer so all UI rendering lands in
    // the resource that gets transferred to the host each present, and enable
    // the per-frame host transfer in the gui loop. (satoru)
    Graphics::ReinitForResolution((uintptr_t)fb_info.address, w, h, fb_info.pitch, 32);
    Graphics::SetVirtioPresent(true);

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
    if (width == 0 || height == 0 || bpp < 15 || bpp > 32) return false;

    // snapshot for rollback on failure
    FramebufferInfo prev = fb_info;
    int prev_mode = current_mode;

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

    if (!result) {
        // restore snapshot - most backends keep state on a failed call
        fb_info = prev;
        current_mode = prev_mode;
        return false;
    }

    fb_info.width = width;
    fb_info.height = height;
    fb_info.bpp = bpp;
    fb_info.pitch = width * (bpp / 8);
    fb_info.size = fb_info.pitch * height;

    current_mode = -1;
    for (int i = 0; i < num_modes; i++) {
        if (modes[i].width == width && modes[i].height == height) {
            current_mode = i;
            break;
        }
    }

    // free old back buffer first to prevent leaks across mode changes.
    // KernelHeap exposes Free in this codebase; if it doesn't, we still
    // null the pointer so we don't keep a dangling reference.
    if (double_buffered && back_buffer) {
        KernelHeap::Free(back_buffer);
        back_buffer = nullptr;
    }
    if (double_buffered) {
        int pages = (fb_info.size + 4095) / 4096;
        back_buffer = KernelHeap::Alloc(pages * 4096);
        if (!back_buffer) {
            double_buffered = false;
            fb_info.double_buffered = false;
        }
    }

    Graphics::ReinitForResolution((uintptr_t)fb_info.address, width, height, fb_info.pitch, bpp);
    return true;
}

bool DisplayManager::SetModeBGA(uint32_t w, uint32_t h, uint8_t bpp) {
    BGA::SetMode(w, h, bpp);
    fb_info.address = (void*)BGA::GetFramebuffer();
    return fb_info.address != nullptr;
}

bool DisplayManager::SetModeVirtIOGPU(uint32_t w, uint32_t h, uint8_t bpp) {
    (void)bpp;

    int pages = (w * h * 4 + 4095) / 4096;
    void* new_fb = KernelHeap::Alloc(pages * 4096);
    if (!new_fb) return false;

    uint32_t res = VirtIOGPU::CreateResource2D(w, h, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
    if (res == 0) {
        KernelHeap::Free(new_fb);
        return false;
    }

    VirtIOGPU::AttachBacking(res, new_fb, w * h * 4);
    VirtIOGPU::SetScanout(0, res, 0, 0, w, h);

    // free the previous framebuffer allocation now that scanout is on the new one
    void* prev = fb_info.address;
    fb_info.address = new_fb;
    if (prev && prev != new_fb) KernelHeap::Free(prev);
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
    if (double_buffered && back_buffer) return true;

    if (back_buffer) { KernelHeap::Free(back_buffer); back_buffer = nullptr; }

    int pages = (fb_info.size + 4095) / 4096;
    void* buf = KernelHeap::Alloc(pages * 4096);
    if (!buf) return false;

    back_buffer = buf;
    double_buffered = true;
    fb_info.double_buffered = true;
    return true;
}

void DisplayManager::SwapBuffers() {
    if (!double_buffered || !back_buffer || !fb_info.address) return;
    if (fb_info.size == 0) return;

    uint8_t* dst = (uint8_t*)fb_info.address;
    const uint8_t* src = (const uint8_t*)back_buffer;
    size_t remaining = fb_info.size;

    while (remaining > 0 && ((uintptr_t)dst & 15)) {
        *dst++ = *src++; remaining--;
    }

    // 256-byte unrolled NT path keeps the WC combine buffers saturated.
    while (remaining >= 256) {
        __asm__ __volatile__(
            "prefetchnta 256(%0)\n\t"
            "prefetchnta 320(%0)\n\t"
            "movdqu    (%0), %%xmm0\n\t  movdqu  16(%0), %%xmm1\n\t"
            "movdqu  32(%0), %%xmm2\n\t  movdqu  48(%0), %%xmm3\n\t"
            "movdqu  64(%0), %%xmm4\n\t  movdqu  80(%0), %%xmm5\n\t"
            "movdqu  96(%0), %%xmm6\n\t  movdqu 112(%0), %%xmm7\n\t"
            "movntdq %%xmm0,    (%1)\n\t movntdq %%xmm1,  16(%1)\n\t"
            "movntdq %%xmm2,  32(%1)\n\t movntdq %%xmm3,  48(%1)\n\t"
            "movntdq %%xmm4,  64(%1)\n\t movntdq %%xmm5,  80(%1)\n\t"
            "movntdq %%xmm6,  96(%1)\n\t movntdq %%xmm7, 112(%1)\n\t"
            "movdqu 128(%0), %%xmm0\n\t  movdqu 144(%0), %%xmm1\n\t"
            "movdqu 160(%0), %%xmm2\n\t  movdqu 176(%0), %%xmm3\n\t"
            "movdqu 192(%0), %%xmm4\n\t  movdqu 208(%0), %%xmm5\n\t"
            "movdqu 224(%0), %%xmm6\n\t  movdqu 240(%0), %%xmm7\n\t"
            "movntdq %%xmm0, 128(%1)\n\t movntdq %%xmm1, 144(%1)\n\t"
            "movntdq %%xmm2, 160(%1)\n\t movntdq %%xmm3, 176(%1)\n\t"
            "movntdq %%xmm4, 192(%1)\n\t movntdq %%xmm5, 208(%1)\n\t"
            "movntdq %%xmm6, 224(%1)\n\t movntdq %%xmm7, 240(%1)\n\t"
            :: "r"(src), "r"(dst)
            : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","memory"
        );
        dst += 256; src += 256; remaining -= 256;
    }
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
    while (remaining >= 8) { *(uint64_t*)dst = *(const uint64_t*)src; dst+=8; src+=8; remaining-=8; }
    while (remaining > 0) { *dst++ = *src++; remaining--; }

    __asm__ __volatile__("sfence" ::: "memory");

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

    // VGA retrace polling - bit 3 of port 0x3da. Unbounded reads can hang
    // forever on EFI/non-VGA hardware (port reads 0xff or 0x00); time them
    // out so the present path never wedges the kernel.
    static bool vsync_dead = false;
    if (vsync_dead) return;
    if (backend != DISPLAY_BACKEND_BGA) return;

    uint8_t probe = inb(0x3DA);
    if (probe == 0xFF || probe == 0x00) { vsync_dead = true; return; }

    volatile int timeout = 200000;
    while ((inb(0x3DA) & 0x08) && --timeout > 0) {}
    if (timeout <= 0) { vsync_dead = true; return; }
    timeout = 200000;
    while (!(inb(0x3DA) & 0x08) && --timeout > 0) {}
    if (timeout <= 0) { vsync_dead = true; }
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

//  multi-monitor detection (satoru)
//  reuses the early pci scan performed by GpuProbe (which already enumerates
//  every class 0x03 display controller and its bars) instead of re-walking
//  the bus, then turns each usable controller into a MonitorInfo laid out
//  left-to-right across a virtual desktop. monitors[0] is always the primary
//  and mirrors GetMonitorInfo(); the existing single-monitor api is unchanged.
int DisplayManager::DetectDisplays() {
    monitor_count = 0;
    fb_secondary = {};
    for (int i = 0; i < DISPLAY_MAX_MONITORS; i++) monitors[i] = {};

    // primary monitor - make sure `monitor` is populated (the virtio/native
    // init paths don't call ReadEDID like InitBGA does). (satoru)
    if (!monitor.connected) {
        ReadEDID(&monitor);
    }
    monitors[0] = monitor;
    monitors[0].origin_x = 0;
    monitors[0].origin_y = 0;
    if (monitors[0].native_width == 0)  monitors[0].native_width  = fb_info.width;
    if (monitors[0].native_height == 0) monitors[0].native_height = fb_info.height;
    monitor_count = 1;

    // pull the system gpu inventory from the early probe. (satoru)
    const GpuProbeResult& gpr = GpuProbe::GetResult();

    // tag the primary monitor with the gpu that drives the panel.
    if (gpr.primary_idx >= 0 && gpr.primary_idx < gpr.count) {
        const GpuInfo& pg = gpr.gpus[gpr.primary_idx];
        monitors[0].pci_bus      = pg.bus;
        monitors[0].pci_device   = pg.device;
        monitors[0].pci_function = pg.function;
        monitors[0].vendor_id    = pg.vendor_id;
    }

    // running x-origin for left-to-right virtual-desktop layout. (satoru)
    int32_t next_origin_x = (int32_t)monitors[0].native_width;

    // additional usable display controllers become extra outputs. "usable"
    // means a distinct controller (not the primary) that exposes a linear
    // framebuffer aperture via one of its bars. (satoru)
    for (int i = 0; i < gpr.count && monitor_count < DISPLAY_MAX_MONITORS; i++) {
        if (i == gpr.primary_idx) continue;
        const GpuInfo& g = gpr.gpus[i];
        if (!g.present) continue;
        // bar2 is the vram/framebuffer aperture on every vendor we classify
        // (intel gmadr, nvidia bar1, amd vram); bar0 is the mmio aperture.
        // require at least one nonzero aperture so the output is real. (satoru)
        if (g.bar2 == 0 && g.bar0 == 0) continue;

        MonitorInfo& m = monitors[monitor_count];
        m.connected     = true;
        m.native_width  = fb_info.width;   // unknown without per-vendor edid/modeset
        m.native_height = fb_info.height;
        m.max_width     = 3840;
        m.max_height    = 2160;
        m.max_refresh   = 60;
        m.physical_width_cm  = 60.0f;
        m.physical_height_cm = 34.0f;
        m.origin_x      = next_origin_x;
        m.origin_y      = 0;
        m.pci_bus       = g.bus;
        m.pci_device    = g.device;
        m.pci_function  = g.function;
        m.vendor_id     = g.vendor_id;

        // short label from the probe description (MonitorInfo.name is 14b).
        int n = 0;
        while (g.desc[n] && n < 13) { m.name[n] = g.desc[n]; n++; }
        m.name[n] = 0;

        next_origin_x += (int32_t)m.native_width;
        monitor_count++;
    }

    // if we found a second output, expose a ram-backed virtual framebuffer
    // for it at the primary resolution. scanning this out to the secondary
    // controller's physical display needs per-vendor modeset code, so the
    // compositor can render into it but presentation is a follow-up. (satoru)
    if (monitor_count >= 2 && fb_info.width > 0 && fb_info.height > 0) {
        fb_secondary.width  = fb_info.width;
        fb_secondary.height = fb_info.height;
        fb_secondary.bpp    = fb_info.bpp ? fb_info.bpp : 32;
        fb_secondary.pitch  = fb_secondary.width * (fb_secondary.bpp / 8);
        fb_secondary.size   = fb_secondary.pitch * fb_secondary.height;
        fb_secondary.double_buffered = false;
        int pages = (fb_secondary.size + 4095) / 4096;
        fb_secondary.address = KernelHeap::Alloc(pages * 4096);
        // TODO (satoru): drive fb_secondary to the secondary controller's
        // scanout - needs per-vendor modeset (intel ggtt/pipe-b, virtio
        // second scanout, etc.); until then it is a compositor-only surface.
        if (!fb_secondary.address) {
            // allocation failed - keep the output listed but with no fb.
            fb_secondary.size = 0;
        }
    }

    SerialLogger::Log("DisplayManager: detected ");
    SerialLogger::LogDec(monitor_count);
    SerialLogger::Log(" monitor(s)\r\n");
    return monitor_count;
}

int DisplayManager::GetMonitorCount() { return monitor_count; }

const MonitorInfo* DisplayManager::GetMonitor(int index) {
    if (index < 0 || index >= monitor_count) return nullptr;
    return &monitors[index];
}

const FramebufferInfo* DisplayManager::GetFramebuffer(int index) {
    if (index == 0) return &fb_info;
    if (index == 1 && fb_secondary.address != nullptr) return &fb_secondary;
    return nullptr;
}

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
