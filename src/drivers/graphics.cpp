#include "graphics.h"
#include "display.h"
#include "serial.h"
#include "../apps/settings.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/panic.h"
#include "../ui/font.h"
#include "virtio_gpu.h"   // accelerated present path (satoru)

// when the virtio-gpu backend is active, fb_addr is a guest-ram resource
// backing that must be transferred + flushed to the host after each frame.
// DisplayManager sets this once it selects + inits the virtio backend. (satoru)
static bool g_present_via_virtio = false;

// page directory tables from kurono_boot.asm  -  needed to remap fb caching
extern "C" uint8_t pd_tables[];
// pdpt from kurono_boot.asm  -  needed to add new pd tables for >16gb fbs
extern "C" uint8_t pdpt[];

// must stay in sync with num_pds in kurono_boot.asm.
// the boot path now identity-maps 512 gb specifically to cover high efi gop
// framebuffers seen on real laptops (e.g. 0x4000000000 = 256 gb).
static constexpr uint64_t BOOT_IDENTITY_MAP_GB = 512;

// utility functions (static to avoid linking conflicts)
static int abs(int x) { return x < 0 ? -x : x; }
static int max(int a, int b) { return a > b ? a : b; }

// uses movntdq (128-bit non-temporal stores) which bypass all cpu caches.
// movntdq + sfence works correctly on wc, wb, and uc memory types.
// the 256-byte unrolled inner loop saturates a single core's wc combine
// buffers on modern uarchs (4 - 6 buffers × 64 b cache lines).
static void fb_copy_nt(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    while (size > 0 && ((uintptr_t)d & 15)) {
        *d++ = *s++; size--;
    }

    // 256-byte unrolled loop  -  issues 4 wc-combine-buffer worth of stores
    // per iteration to keep the memory pipeline full.
    while (size >= 256) {
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
            :: "r"(s), "r"(d)
            : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","memory"
        );
        d += 256; s += 256; size -= 256;
    }
    while (size >= 64) {
        __asm__ __volatile__(
            "movdqu   (%0), %%xmm0;  movdqu 16(%0), %%xmm1;"
            "movdqu 32(%0), %%xmm2;  movdqu 48(%0), %%xmm3;"
            "movntdq %%xmm0,   (%1); movntdq %%xmm1, 16(%1);"
            "movntdq %%xmm2, 32(%1); movntdq %%xmm3, 48(%1);"
            :: "r"(s), "r"(d)
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
        );
        d += 64; s += 64; size -= 64;
    }
    while (size >= 16) {
        __asm__ __volatile__(
            "movdqu (%0), %%xmm0; movntdq %%xmm0, (%1);"
            :: "r"(s), "r"(d)
            : "xmm0", "memory"
        );
        d += 16; s += 16; size -= 16;
    }

    // 8/4/1-byte tail
    while (size >= 8) { *(uint64_t*)d = *(const uint64_t*)s; d+=8; s+=8; size-=8; }
    while (size >= 4) { *(uint32_t*)d = *(const uint32_t*)s; d+=4; s+=4; size-=4; }
    while (size > 0) { *d++ = *s++; size--; }
}

// extra page directories for fb addresses above the boot identity map.
// each 4 kb page directory covers 1 gb (512 × 2 mb pages).
// in practice the boot map now covers 512 gb, so this is just a last-resort
// fallback for truly unusual mmio placements.
static uint64_t extra_pd[4][512] __attribute__((aligned(4096)));
static int extra_pd_count = 0;

// pat was programmed in boot asm: pat1 = wc (0x01).
// to select pat1 for a 2mb page we set pwt=1, pcd=0, pat(bit12)=0.
// this is critical for bare-metal: wb-cached mmio writes never reach the
// gpu memory controller on real hardware, causing a permanent black screen.
static bool remap_fb_writecombining(uintptr_t fb_phys, size_t fb_size) {
    if (!fb_phys || !fb_size) {
        SerialLogger::Log("Graphics: WC remap skipped  -  no FB address\r\n");
        return false;
    }

    SerialLogger::Log("Graphics: WC remap FB phys=0x");
    SerialLogger::LogHex((uint32_t)(fb_phys >> 32));
    SerialLogger::LogHex((uint32_t)(fb_phys & 0xFFFFFFFF));
    SerialLogger::Log(" size=0x");
    SerialLogger::LogHex((uint32_t)fb_size);
    SerialLogger::Log("\r\n");

    uint64_t start = fb_phys & ~((uint64_t)0x1FFFFF);            // align down to 2 mb
    uint64_t end   = (fb_phys + fb_size + 0x1FFFFF) & ~((uint64_t)0x1FFFFF); // align up
    int pages_remapped = 0;
    bool had_overflow = false;

    for (uint64_t addr = start; addr < end; addr += 0x200000) {
        uint64_t gb_idx = addr >> 30;            // which gb  (pdpt index)
        uint64_t pd_idx = (addr >> 21) & 0x1FF;  // which 2mb page in that pd

        uint64_t* pd = nullptr;

        if (gb_idx < BOOT_IDENTITY_MAP_GB) {
            // within the boot-time identity map built in kurono_boot.asm.
            pd = (uint64_t*)((uintptr_t)pd_tables + gb_idx * 4096);
        } else if (gb_idx < (BOOT_IDENTITY_MAP_GB + 4) && extra_pd_count < 4) {
            // legacy fallback path: dynamically add a page directory.
            // build a fresh 1 gb identity-mapped pd for this gb region.
            int epi = -1;
            // check if we already allocated a pd for this gb_idx
            // (re-use across 2 mb pages in the same gb)
            // store gb_idx in entry 511 reserved field (we only need 0-511 entries)
            for (int i = 0; i < extra_pd_count; i++) {
                uint64_t tag = extra_pd[i][511] >> 48;
                if (tag == gb_idx) { epi = i; break; }
            }
            if (epi < 0) {
                // allocate a new extra pd
                epi = extra_pd_count++;
                uint64_t base_phys = gb_idx << 30;
                for (int j = 0; j < 512; j++) {
                    extra_pd[epi][j] = (base_phys + (uint64_t)j * 0x200000) | 0x83;
                }
                // tag entry 511 with gb_idx for re-use detection
                extra_pd[epi][511] = ((extra_pd[epi][511] & 0x0000FFFFFFFFFFFF) | (gb_idx << 48));
                // wire this pd into the pdpt
                uint64_t* pdpt64 = (uint64_t*)(uintptr_t)pdpt;
                pdpt64[gb_idx] = ((uint64_t)(uintptr_t)&extra_pd[epi][0]) | 0x03;
                // full tlb flush for new mapping
                __asm__ __volatile__("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
                SerialLogger::Log("Graphics: Extended identity map to cover GB ");
                SerialLogger::LogDec((int)gb_idx);
                SerialLogger::Log("\r\n");
            }
            pd = extra_pd[epi];
        } else {
            had_overflow = true;
            continue;
        }

        // clear pwt(3), pcd(4), pat-for-2mb(12), then set pwt → pat entry 1 = wc
        pd[pd_idx] &= ~((1ULL << 3) | (1ULL << 4) | (1ULL << 12));
        pd[pd_idx] |=  (1ULL << 3);              // pwt = 1

        // invalidate tlb for this virtual address
        __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
        pages_remapped++;
    }

    // full memory fence  -  ensures wc attribute is observed before any fb writes
    __asm__ __volatile__("mfence" ::: "memory");

    if (had_overflow) {
        SerialLogger::Log("Graphics: WARNING  -  FB region extends beyond mappable range!\r\n");
    }

    SerialLogger::Log("Graphics: FB remapped ");
    SerialLogger::LogDec(pages_remapped);
    SerialLogger::Log(" pages to Write-Combining (PAT1=WC)\r\n");
    return pages_remapped > 0;
}

uint8_t* Graphics::fb_addr = 0;
uint8_t* Graphics::back_buffer = 0;
uint8_t* Graphics::active_buffer = 0;
size_t   Graphics::back_buffer_size = 0;
uint32_t Graphics::fb_width = 0;
uint32_t Graphics::fb_height = 0;
uint32_t Graphics::fb_pitch = 0;
uint8_t Graphics::fb_bpp = 0;

Graphics::RenderMode Graphics::render_mode = SINGLE_BUFFER;
Graphics::BlendMode Graphics::blend_mode = BLEND_ALPHA;

uint32_t Graphics::target_frame_time_us = 16667; // 60hz (1000000/60) (satoru)
uint64_t Graphics::last_frame_time = 0;
uint32_t Graphics::frame_count = 0;
uint64_t Graphics::fps_sample_time = 0;
Graphics::DrawStats Graphics::draw_stats = {0};

// accessibility post-process state
static int  g_color_filter   = 0;     // 0=off,1=protan,2=deutan,3=tritan,4=gray
static bool g_high_contrast  = false;
// software display brightness 10..100 (%). this backend has no panel/gamma
// dimming, so we dim in the swapbuffers post-process pass instead  -  the
// display settings slider now has a real effect. 100 = unmodified. (satoru)
static int  g_brightness     = 100;

bool Graphics::fb_wc_active = false;
uint32_t Graphics::monitor_hz = 0;

int Graphics::clip_x = 0;
int Graphics::clip_y = 0;
int Graphics::clip_w = 0;
int Graphics::clip_h = 0;
bool Graphics::clipping_enabled = false;
Graphics::ClipSave Graphics::clip_stack[16] = {};
int Graphics::clip_sp = 0;

Graphics::DirtyRegion Graphics::dirty_regions[16];
int Graphics::dirty_count = 0;

// Global UI dirty signal  -  see graphics.h. volatile: written from input /
// app / animation paths (potentially other kernel processes) and read by
// the GUI process each frame. A monotonically-incrementing counter lets the
// GUI loop detect "changed since I last looked" via a snapshot compare, so a
// MarkUIDirty() that lands between the loop's check and its clear is still
// observed on the next iteration rather than being clobbered. (satoru)
static volatile uint32_t g_ui_dirty = 1;          // start dirty: paint frame 0
static uint32_t          g_ui_dirty_seen = 0;

void Graphics::MarkUIDirty()        { g_ui_dirty++; }
uint32_t Graphics::UIDirtyCount()   { return g_ui_dirty; }
bool Graphics::ConsumeUIDirty() {
    uint32_t cur = g_ui_dirty;
    bool dirty = (cur != g_ui_dirty_seen);
    g_ui_dirty_seen = cur;
    return dirty;
}

void Graphics::Init(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    if (addr == 0 || width == 0 || height == 0 || pitch == 0 ||
        bpp < 15 || bpp > 32) {
        SerialLogger::Log("Graphics: Init rejected  -  invalid framebuffer args\r\n");
        return;
    }
    // any back_buffer left over from a prior config is now sized wrong; drop it.
    if (back_buffer) {
        PMM::FreeBytes(back_buffer, back_buffer_size);
        back_buffer = 0;
        back_buffer_size = 0;
        render_mode = SINGLE_BUFFER;
    }

    fb_addr = (uint8_t*)addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = bpp;
    active_buffer = fb_addr;

    SerialLogger::Log("Graphics: Basic Init ");
    SerialLogger::LogHex(width); SerialLogger::Log("x"); SerialLogger::LogHex(height);
    SerialLogger::Log(" @ "); SerialLogger::LogHex(addr); SerialLogger::Log("\r\n");

    draw_stats.target_fps = 60;
    draw_stats.vsync_enabled = false;

    for (int i = 0; i < 16; i++) {
        dirty_regions[i].active = false;
    }
    dirty_count = 0;

    fb_wc_active = remap_fb_writecombining(addr, (size_t)pitch * height);
    if (!fb_wc_active) {
        SerialLogger::Log("Graphics: WARNING  -  WC remap FAILED, using wbinvd fallback (slower)\r\n");
    }
    KernelPanic::UpdateFramebuffer((uint64_t)addr, pitch, width, height, bpp);
}

void Graphics::ReinitForResolution(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    RenderMode wanted = render_mode;
    // Init() now does the back_buffer free + null itself
    Init(addr, width, height, pitch, bpp);
    if (wanted == DOUBLE_BUFFER || wanted == TRIPLE_BUFFER) {
        SetRenderMode(wanted);
    }
}

void Graphics::InitAdvanced() {
    if (!DisplayController::Init()) {
        SerialLogger::Log("Graphics: Display controller init failed, using basic mode\r\n");
        return;
    }
    
    // find best mode for 180hz
    const DisplayController::DisplayMode* mode = DisplayController::FindBestMode(
        1920, 1080, 32, DisplayController::REFRESH_180HZ);
    
    if (!mode) {
        SerialLogger::Log("Graphics: No 180Hz mode found, trying alternatives\r\n");
        // try other high refresh rates
        mode = DisplayController::FindBestMode(1920, 1080, 32, DisplayController::REFRESH_144HZ);
        if (!mode) mode = DisplayController::FindBestMode(1024, 768, 32, DisplayController::REFRESH_180HZ);
        if (!mode) mode = DisplayController::FindBestMode(1024, 768, 32, DisplayController::REFRESH_120HZ);
    }
    
    if (mode && DisplayController::SetMode(mode)) {
        Init(mode->framebuffer_addr, mode->width, mode->height, mode->pitch, mode->bpp);
        uint32_t hz = mode->refresh_rate ? mode->refresh_rate : 60;
        // cap the compositor at 60fps. recompositing the entire desktop at
        // 120-180hz pins the gui process at ~100% cpu redrawing identical
        // idle frames and starves every other process (the "slow asf" the
        // user hit). 60fps is smooth and frees the cpu. (satoru)
        if (hz > 60) hz = 60;
        target_frame_time_us = 1000000 / hz;
        draw_stats.target_fps = hz;
        
        SerialLogger::Log("Graphics: Advanced mode set - ");
        SerialLogger::Log(mode->description);
        SerialLogger::Log("\r\n");
        
        // set up double buffering if supported
        if (DisplayController::IsDoubleBufferSupported()) {
            SetRenderMode(DOUBLE_BUFFER);
        }
    } else {
        SerialLogger::Log("Graphics: Failed to set advanced mode\r\n");
    }
}

void Graphics::SetRenderMode(RenderMode mode) {
    if (mode == DOUBLE_BUFFER || mode == TRIPLE_BUFFER) {
        if (fb_pitch == 0 || fb_height == 0 || !fb_addr) {
            render_mode = SINGLE_BUFFER;
            return;
        }
        size_t buffer_size = (size_t)fb_pitch * fb_height;
        if (back_buffer && back_buffer_size == buffer_size) {
            render_mode = mode;
            active_buffer = back_buffer;
            return;
        }
        if (back_buffer) {
            PMM::FreeBytes(back_buffer, back_buffer_size);
            back_buffer = 0;
            back_buffer_size = 0;
        }
        back_buffer = (uint8_t*)PMM::AllocBytes(buffer_size);
        if (back_buffer) {
            back_buffer_size = buffer_size;
            active_buffer = back_buffer;
            render_mode = mode;
            SerialLogger::Log("Graphics: Double buffering enabled (PMM)\r\n");
        } else {
            SerialLogger::Log("Graphics: Failed to allocate back buffer\r\n");
            render_mode = SINGLE_BUFFER;
            active_buffer = fb_addr;
        }
    } else {
        if (back_buffer) {
            PMM::FreeBytes(back_buffer, back_buffer_size);
            back_buffer = 0;
            back_buffer_size = 0;
        }
        render_mode = mode;
        active_buffer = fb_addr;
    }
}

Graphics::RenderMode Graphics::GetRenderMode() {
    return render_mode;
}

void Graphics::BeginFrame() {
    uint64_t current_time = TimeManager::NowUTC().us;
    if (frame_count > 0 && current_time >= last_frame_time) {
        uint64_t dt = current_time - last_frame_time;
        draw_stats.last_frame_time_us = (dt > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)dt;
    }
    last_frame_time = current_time;
    ClearDirtyRegions();
}

void Graphics::EndFrame() {
    frame_count++;
    draw_stats.frames_rendered++;

    if (render_mode == DOUBLE_BUFFER || render_mode == TRIPLE_BUFFER) {
        SwapBuffers();
    }

    if (draw_stats.vsync_enabled) {
        WaitForVSync();
    }

    if (frame_count % 30 == 0) {
        uint64_t current_time = TimeManager::NowUTC().us;
        if (fps_sample_time != 0 && current_time > fps_sample_time) {
            uint64_t elapsed = current_time - fps_sample_time;
            if (elapsed > 0) {
                draw_stats.current_fps = (uint32_t)(30000000ull / elapsed);
                draw_stats.avg_frame_time_us = (uint32_t)(elapsed / 30);
            }
        }
        fps_sample_time = current_time;
    }
}

void Graphics::SetTargetFPS(uint32_t fps) {
    if (fps == 0) fps = 60;
    target_frame_time_us = 1000000 / fps;
    draw_stats.target_fps = fps;
}

uint32_t Graphics::GetTargetFPS() {
    return draw_stats.target_fps;
}

// frame budget in whole ms derived from the active target fps. drives the gui
// loop's adaptive pacing so it follows the user's selected display.refresh_hz
// (set via SetTargetFPS from WindowManager::ReloadFromConfig / settings) rather
// than a hardcoded 60. clamped to >=1ms. (satoru)
uint32_t Graphics::GetTargetFrameTimeMs() {
    uint32_t ms = target_frame_time_us / 1000u;
    return ms ? ms : 1u;
}

void Graphics::SetVirtioPresent(bool on) { g_present_via_virtio = on; }

// push the just-swapped frame to the host gpu. called by the gui loop right
// after SwapBuffers when the virtio-gpu backend is active; a no-op otherwise so
// the bga/std-vga present path is unchanged. PresentFramebuffer pipelines the
// transfer + flush into the virtqueue (one notify per frame). (satoru)
void Graphics::PresentVirtioIfActive() {
    if (!g_present_via_virtio || !fb_addr) return;
    VirtIOGPU::PresentFramebuffer((void*)fb_addr, fb_width, fb_height);
}

uint32_t Graphics::GetMonitorHz() {
    return monitor_hz;
}

bool Graphics::IsFramebufferWC() {
    return fb_wc_active;
}

// measures the interval between 4 consecutive vsync pulses using pit and
// computes hz.  returns 60 if vga controller isn't present (pure efi/gop).
uint32_t Graphics::DetectRefreshRate() {
    // check if vga vsync is available (port 0x3da)
    // on efi-only hardware there's no vga: port reads 0xff (bus float) or
    // 0x00 (port not wired). bits 0 and 3 should toggle on real vga.
    uint8_t probe;
    __asm__ __volatile__("inb %1, %0" : "=a"(probe) : "Nd"((uint16_t)0x3DA));
    if (probe == 0xFF || probe == 0x00) {
        SerialLogger::Log("Graphics: No VGA VSync (EFI?)  -  defaulting to 60 Hz\r\n");
        monitor_hz = 60;
        return 60;
    }

    const int cycles = 4;
    volatile int timeout;

    // sync to vsync start
    timeout = 300000;
    while (timeout > 0) {
        __asm__ __volatile__("inb %1, %0" : "=a"(probe) : "Nd"((uint16_t)0x3DA));
        if (!(probe & 0x08)) break;
        timeout--;
    }
    if (timeout <= 0) return 60;
    timeout = 300000;
    while (timeout > 0) {
        __asm__ __volatile__("inb %1, %0" : "=a"(probe) : "Nd"((uint16_t)0x3DA));
        if (probe & 0x08) break;
        timeout--;
    }
    if (timeout <= 0) return 60;

    uint64_t start_ms = TimeManager::NowUTC().us / 1000;

    for (int i = 0; i < cycles; i++) {
        timeout = 300000;
        while (timeout > 0) {
            __asm__ __volatile__("inb %1, %0" : "=a"(probe) : "Nd"((uint16_t)0x3DA));
            if (!(probe & 0x08)) break;
            timeout--;
        }
        if (timeout <= 0) return 60;
        timeout = 300000;
        while (timeout > 0) {
            __asm__ __volatile__("inb %1, %0" : "=a"(probe) : "Nd"((uint16_t)0x3DA));
            if (probe & 0x08) break;
            timeout--;
        }
        if (timeout <= 0) return 60;
    }

    uint64_t end_ms = TimeManager::NowUTC().us / 1000;
    uint64_t elapsed_ms = end_ms - start_ms;
    if (elapsed_ms == 0 || elapsed_ms > 500) return 60;

    uint32_t hz = (uint32_t)((cycles * 1000 + elapsed_ms / 2) / elapsed_ms);
    if (hz < 24 || hz > 360) hz = 60;
    monitor_hz = hz;
    return hz;
}

bool Graphics::ShouldRender() {
    uint64_t current_time = TimeManager::NowUTC().us;
    if (current_time < last_frame_time) return true;  // clock skew  -  render
    return (current_time - last_frame_time) >= (uint64_t)target_frame_time_us;
}

void Graphics::WaitForVSync() {
    DisplayController::WaitVSync();
}

// applies the accessibility color filter to one pixel
static inline uint32_t apply_a11y_filter(uint32_t c) {
    uint32_t a = (c >> 24) & 0xFF;
    int r = (c >> 16) & 0xFF;
    int g = (c >> 8)  & 0xFF;
    int b = (c)       & 0xFF;
    int nr = r, ng = g, nb = b;
    switch (g_color_filter) {
        case 1:
            nr = (567*r + 433*g) / 1000;
            ng = (558*r + 442*g) / 1000;
            nb = (242*g + 758*b) / 1000;
            break;
        case 2:
            nr = (625*r + 375*g) / 1000;
            ng = (700*r + 300*g) / 1000;
            nb = (300*g + 700*b) / 1000;
            break;
        case 3:
            nr = (950*r +  50*g) / 1000;
            ng = (433*g + 567*b) / 1000;
            nb = (475*g + 525*b) / 1000;
            break;
        case 4: { int yy = (299*r + 587*g + 114*b) / 1000; nr = ng = nb = yy; break; }
        default: break;
    }
    if (g_high_contrast) {
        int yy = (299*nr + 587*ng + 114*nb) / 1000;
        nr = ng = nb = (yy < 96) ? 0 : 255;
    }
    if (g_brightness < 100) {   // software dim (satoru)
        nr = nr * g_brightness / 100;
        ng = ng * g_brightness / 100;
        nb = nb * g_brightness / 100;
    }
    if (nr < 0) nr = 0; else if (nr > 255) nr = 255;
    if (ng < 0) ng = 0; else if (ng > 255) ng = 255;
    if (nb < 0) nb = 0; else if (nb > 255) nb = 255;
    return (a << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
}

// internal: copy one row of dirty pixels, applying the a11y filter if active.
// uses 16-pixel SSE-friendly inner loop with NT stores.
static void blit_filtered_row(uint8_t* dst_row, const uint8_t* src_row, int x, int w) {
    uint32_t* d = (uint32_t*)dst_row + x;
    const uint32_t* s = (const uint32_t*)src_row + x;
    int i = 0;
    for (; i + 4 <= w; i += 4) {
        uint32_t a = apply_a11y_filter(s[i+0]);
        uint32_t b = apply_a11y_filter(s[i+1]);
        uint32_t c = apply_a11y_filter(s[i+2]);
        uint32_t e = apply_a11y_filter(s[i+3]);
        d[i+0] = a; d[i+1] = b; d[i+2] = c; d[i+3] = e;
    }
    for (; i < w; i++) d[i] = apply_a11y_filter(s[i]);
}

void Graphics::SwapBuffers() {
    if (render_mode == SINGLE_BUFFER) return;
    if (!back_buffer || !fb_addr) return;

    const uint32_t bytes_per_pixel = fb_bpp / 8;
    if (bytes_per_pixel == 0) return;
    const uint32_t bytes_per_line = fb_width * bytes_per_pixel;
    const bool pitch_match = (bytes_per_line == fb_pitch);
    const bool filter_active = ((g_color_filter > 0 || g_high_contrast || g_brightness < 100) && bytes_per_pixel == 4);

    // accessibility path: per-pixel transform. We still honour the dirty
    // region list so a static screen with a moving cursor isn't a full
    // re-tonemap each frame.
    if (filter_active) {
        bool any_dirty = false;
        for (int i = 0; i < dirty_count && i < 16; i++) {
            if (dirty_regions[i].active) { any_dirty = true; break; }
        }
        if (!any_dirty || dirty_count == 0) {
            for (uint32_t y = 0; y < fb_height; y++) {
                blit_filtered_row(fb_addr + (size_t)y * fb_pitch,
                                  back_buffer + (size_t)y * fb_pitch,
                                  0, (int)fb_width);
            }
        } else {
            for (int i = 0; i < dirty_count; i++) {
                if (!dirty_regions[i].active) continue;
                int rx = dirty_regions[i].x;
                int ry = dirty_regions[i].y;
                int rw = dirty_regions[i].w;
                int rh = dirty_regions[i].h;
                if (rx < 0) { rw += rx; rx = 0; }
                if (ry < 0) { rh += ry; ry = 0; }
                if (rx >= (int)fb_width || ry >= (int)fb_height) continue;
                if (rx + rw > (int)fb_width)  rw = (int)fb_width  - rx;
                if (ry + rh > (int)fb_height) rh = (int)fb_height - ry;
                if (rw <= 0 || rh <= 0) continue;
                for (int y = ry; y < ry + rh; y++) {
                    blit_filtered_row(fb_addr + (size_t)y * fb_pitch,
                                      back_buffer + (size_t)y * fb_pitch,
                                      rx, rw);
                }
            }
        }
        __asm__ __volatile__("sfence" ::: "memory");
        ClearDirtyRegions();
        return;
    }

    // partial swap when dirty area is small enough; otherwise full-frame.
    // 60 % threshold trades per-rect copy overhead vs cache locality of a
    // single contiguous blit.
    if (dirty_count > 0 && dirty_count <= 16) {
        uint64_t dirty_pixels = 0;
        int active_rects = 0;
        for (int i = 0; i < dirty_count; i++) {
            if (!dirty_regions[i].active) continue;
            int rw = dirty_regions[i].w, rh = dirty_regions[i].h;
            if (rw <= 0 || rh <= 0) continue;
            dirty_pixels += (uint64_t)rw * (uint64_t)rh;
            active_rects++;
        }
        uint64_t total_pixels = (uint64_t)fb_width * (uint64_t)fb_height;

        if (active_rects > 0 && dirty_pixels < (total_pixels * 3 / 5)) {
            for (int i = 0; i < dirty_count; i++) {
                if (!dirty_regions[i].active) continue;
                int rx = dirty_regions[i].x;
                int ry = dirty_regions[i].y;
                int rw = dirty_regions[i].w;
                int rh = dirty_regions[i].h;
                if (rx < 0) { rw += rx; rx = 0; }
                if (ry < 0) { rh += ry; ry = 0; }
                if (rx >= (int)fb_width || ry >= (int)fb_height) continue;
                if (rx + rw > (int)fb_width)  rw = (int)fb_width  - rx;
                if (ry + rh > (int)fb_height) rh = (int)fb_height - ry;
                if (rw <= 0 || rh <= 0) continue;

                size_t region_bytes = (size_t)rw * bytes_per_pixel;
                size_t col_off = (size_t)rx * bytes_per_pixel;
                for (int y = ry; y < ry + rh; y++) {
                    size_t row_off = (size_t)y * fb_pitch + col_off;
                    fb_copy_nt(fb_addr + row_off, back_buffer + row_off, region_bytes);
                }
            }
            __asm__ __volatile__("sfence" ::: "memory");
            ClearDirtyRegions();
            return;
        }
    }

    // full-frame copy
    if (pitch_match) {
        fb_copy_nt(fb_addr, back_buffer, (size_t)fb_pitch * fb_height);
    } else {
        uint8_t* src = back_buffer;
        uint8_t* dst = fb_addr;
        for (uint32_t y = 0; y < fb_height; y++) {
            fb_copy_nt(dst, src, bytes_per_line);
            src += fb_pitch;
            dst += fb_pitch;
        }
    }

    __asm__ __volatile__("sfence" ::: "memory");
    ClearDirtyRegions();
}

// Atomic present of an explicit damage list. Each rect is clipped to the
// framebuffer, then blitted with non-temporal stores. A NULL rect list or
// count<=0 forces a full-frame present.
void Graphics::Present(const Rect* rects, int count) {
    if (render_mode == SINGLE_BUFFER || !back_buffer || !fb_addr) return;
    const uint32_t bytes_per_pixel = fb_bpp / 8;
    if (bytes_per_pixel == 0) return;
    const bool filter_active = ((g_color_filter > 0 || g_high_contrast || g_brightness < 100) && bytes_per_pixel == 4);

    if (!rects || count <= 0) {
        // hand off to SwapBuffers full-frame path
        ClearDirtyRegions();
        SwapBuffers();
        return;
    }

    if (count > 32) count = 32;
    for (int i = 0; i < count; i++) {
        int rx = rects[i].x, ry = rects[i].y;
        int rw = rects[i].w, rh = rects[i].h;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx >= (int)fb_width || ry >= (int)fb_height) continue;
        if (rx + rw > (int)fb_width)  rw = (int)fb_width  - rx;
        if (ry + rh > (int)fb_height) rh = (int)fb_height - ry;
        if (rw <= 0 || rh <= 0) continue;

        size_t region_bytes = (size_t)rw * bytes_per_pixel;
        size_t col_off = (size_t)rx * bytes_per_pixel;
        if (filter_active) {
            for (int y = ry; y < ry + rh; y++) {
                blit_filtered_row(fb_addr + (size_t)y * fb_pitch,
                                  back_buffer + (size_t)y * fb_pitch,
                                  rx, rw);
            }
        } else {
            for (int y = ry; y < ry + rh; y++) {
                size_t row_off = (size_t)y * fb_pitch + col_off;
                fb_copy_nt(fb_addr + row_off, back_buffer + row_off, region_bytes);
            }
        }
    }
    __asm__ __volatile__("sfence" ::: "memory");
    ClearDirtyRegions();
}

uint8_t* Graphics::GetActiveBuffer() {
    return active_buffer;
}

uint8_t* Graphics::GetBackBuffer() {
    return back_buffer ? back_buffer : fb_addr;
}

void Graphics::Clear(uint32_t color) {
    FillRect(0, 0, fb_width, fb_height, color);
}

void Graphics::ClearBackBuffer(uint32_t color) {
    uint8_t* original = active_buffer;
    if (back_buffer) active_buffer = back_buffer;
    Clear(color);
    active_buffer = original;
}

void Graphics::DrawPixel(int x, int y, uint32_t color) {
    if (!IsPointInBounds(x, y)) return;
    // honour the active clip rect: every line/text/rounded-rect/alpha primitive
    // funnels through here, so a single check makes them all respect SetClipRect.
    // without it, scrolled UI (settings panels) drew its toggles/sliders/text
    // outside the window. (satoru)
    if (clipping_enabled &&
        ((unsigned)(x - clip_x) >= (unsigned)clip_w ||
         (unsigned)(y - clip_y) >= (unsigned)clip_h)) return;

    uint8_t alpha = (color >> 24) & 0xFF;
    if (alpha == 0) return;  // fully transparent  -  skip
    if (alpha >= 0xF0) {
        // near-opaque: treat as fully opaque (huge perf win)
        DrawPixelUnsafe(x, y, color | 0xFF000000u);
    } else if (blend_mode == BLEND_ALPHA) {
        DrawPixelBlended(x, y, color, alpha);
    } else {
        DrawPixelUnsafe(x, y, color);
    }
}

void Graphics::DrawPixelUnsafe(int x, int y, uint32_t color) {
    if (!active_buffer) return;
    // bounds check  -  "Unsafe" historically meant unchecked vs clip rect, but
    // OOB store to a wc-mapped fb can corrupt unrelated mmio (page-aligned
    // bars sit right after the fb on many gpus).
    if ((unsigned)x >= fb_width || (unsigned)y >= fb_height) return;

    size_t offset = (size_t)y * fb_pitch + (size_t)x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        *(volatile uint32_t*)(active_buffer + offset) = color;
    } else if (fb_bpp == 24) {
        volatile uint8_t* p = active_buffer + offset;
        p[0] = color & 0xFF;
        p[1] = (color >> 8) & 0xFF;
        p[2] = (color >> 16) & 0xFF;
    } else if (fb_bpp == 16) {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        *(volatile uint16_t*)(active_buffer + offset) = v;
    }
}

void Graphics::DrawPixelBlended(int x, int y, uint32_t color, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { DrawPixelUnsafe(x, y, color); return; }
    
    uint32_t dst = ReadPixel(x, y);
    uint32_t blended = BlendColors(color, dst, alpha);
    DrawPixelUnsafe(x, y, blended);
}

uint32_t Graphics::ReadPixel(int x, int y) {
    if (!IsPointInBounds(x, y)) return 0xFF000000;
    return GetPixelUnsafe(x, y);
}

uint32_t Graphics::GetPixelUnsafe(int x, int y) {
    if (!active_buffer) return 0xFF000000;
    if ((unsigned)x >= fb_width || (unsigned)y >= fb_height) return 0xFF000000;

    size_t offset = (size_t)y * fb_pitch + (size_t)x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        return *(volatile uint32_t*)(active_buffer + offset);
    } else if (fb_bpp == 24) {
        volatile uint8_t* p = active_buffer + offset;
        uint8_t b = p[0];
        uint8_t g = p[1];
        uint8_t r = p[2];
        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    } else if (fb_bpp == 16) {
        uint16_t v = *(volatile uint16_t*)(active_buffer + offset);
        uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
        uint8_t b = (uint8_t)((v & 0x1F) << 3);
        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    }
    return 0xFF000000;
}

void Graphics::FillRect(int x, int y, int w, int h, uint32_t color) {
    ClipRect(x, y, w, h);
    if (w <= 0 || h <= 0 || !active_buffer) return;

    uint8_t alpha = (color >> 24) & 0xFF;
    if (fb_bpp == 32 && (blend_mode == BLEND_NONE || alpha >= 0xF0)) {
        uint32_t opaque_color = color | 0xFF000000u;
        uint64_t duo = ((uint64_t)opaque_color << 32) | (uint64_t)opaque_color;
        for (int j = 0; j < h; j++) {
            uint32_t* row = (uint32_t*)(active_buffer + (size_t)(y + j) * fb_pitch + (size_t)x * 4);
            int i = 0;
            // align row to 8-byte boundary so the 64-bit writes are aligned
            if (((uintptr_t)row & 7) != 0 && w > 0) {
                row[0] = opaque_color;
                i = 1;
            }
            // 4 pixels per 16 b iteration, fully unrolled
            uint64_t* row64 = (uint64_t*)(row + i);
            int remaining = w - i;
            int quads = remaining >> 2;
            for (int p = 0; p < quads; p++) {
                row64[p*2 + 0] = duo;
                row64[p*2 + 1] = duo;
            }
            int written = i + quads * 4;
            // tail: 0..3 pixels
            if (written < w) {
                int tail = w - written;
                if (tail >= 2) { row64[quads*2] = duo; written += 2; }
                if (written < w) row[written] = opaque_color;
            }
        }
    } else if (alpha == 0) {
        return;
    } else {
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                DrawPixel(x + i, y + j, color);
            }
        }
    }

    MarkDirty(x, y, w, h);
}

void Graphics::DrawLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    if (thickness <= 1) {
        DrawLineBresenham(x0, y0, x1, y1, color);
    } else {
        DrawThickLine(x0, y0, x1, y1, color, thickness);
    }
    
    // mark dirty region
    int min_x = x0 < x1 ? x0 : x1;
    int max_x = x0 > x1 ? x0 : x1;
    int min_y = y0 < y1 ? y0 : y1;
    int max_y = y0 > y1 ? y0 : y1;
    MarkDirty(min_x - thickness, min_y - thickness, 
             (max_x - min_x) + 2*thickness, (max_y - min_y) + 2*thickness);
}

void Graphics::DrawLineBresenham(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    while (true) {
        DrawPixel(x0, y0, color);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void Graphics::DrawThickLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    // draw multiple parallel lines for thickness
    for (int i = -thickness/2; i <= thickness/2; i++) {
        for (int j = -thickness/2; j <= thickness/2; j++) {
            DrawLineBresenham(x0 + i, y0 + j, x1 + i, y1 + j, color);
        }
    }
}

void Graphics::MarkDirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    // clip to fb so coalescing math doesn't blow up the union rect
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)fb_width || y >= (int)fb_height) return;
    if (x + w > (int)fb_width)  w = (int)fb_width  - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0) return;

    if (dirty_count < 16) {
        DirtyRegion& region = dirty_regions[dirty_count++];
        region.x = x; region.y = y; region.w = w; region.h = h;
        region.active = true;
        return;
    }
    // list full  -  coalesce the new rect into the existing region with
    // smallest growth so we never silently drop damage.
    int best = 0;
    int64_t best_growth = -1;
    for (int i = 0; i < 16; i++) {
        DirtyRegion& r = dirty_regions[i];
        int nx0 = r.x < x ? r.x : x;
        int ny0 = r.y < y ? r.y : y;
        int nx1 = (r.x + r.w) > (x + w) ? (r.x + r.w) : (x + w);
        int ny1 = (r.y + r.h) > (y + h) ? (r.y + r.h) : (y + h);
        int64_t cur = (int64_t)r.w * r.h;
        int64_t nu  = (int64_t)(nx1 - nx0) * (ny1 - ny0);
        int64_t growth = nu - cur;
        if (best_growth < 0 || growth < best_growth) {
            best_growth = growth;
            best = i;
        }
    }
    DirtyRegion& r = dirty_regions[best];
    int nx0 = r.x < x ? r.x : x;
    int ny0 = r.y < y ? r.y : y;
    int nx1 = (r.x + r.w) > (x + w) ? (r.x + r.w) : (x + w);
    int ny1 = (r.y + r.h) > (y + h) ? (r.y + r.h) : (y + h);
    r.x = nx0; r.y = ny0; r.w = nx1 - nx0; r.h = ny1 - ny0;
    r.active = true;
}

void Graphics::ClearDirtyRegions() {
    dirty_count = 0;
    for (int i = 0; i < 16; i++) {
        dirty_regions[i].active = false;
    }
}

bool Graphics::IsPointInBounds(int x, int y) {
    if (clipping_enabled) {
        return x >= clip_x && x < clip_x + clip_w && y >= clip_y && y < clip_y + clip_h;
    }
    return x >= 0 && x < (int)fb_width && y >= 0 && y < (int)fb_height;
}

void Graphics::ClipRect(int& x, int& y, int& w, int& h) {
    int right = x + w;
    int bottom = y + h;
    
    int clip_right = clipping_enabled ? clip_x + clip_w : (int)fb_width;
    int clip_bottom = clipping_enabled ? clip_y + clip_h : (int)fb_height;
    int clip_left = clipping_enabled ? clip_x : 0;
    int clip_top = clipping_enabled ? clip_y : 0;
    
    if (x < clip_left) x = clip_left;
    if (y < clip_top) y = clip_top;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    
    w = right - x;
    h = bottom - y;
}

void Graphics::SetClipRect(int x, int y, int w, int h) {
    clip_x = x;
    clip_y = y;
    clip_w = w;
    clip_h = h;
    clipping_enabled = true;
}

void Graphics::ClearClipRect() {
    clipping_enabled = false;
}

void Graphics::PushClipRect(int x, int y, int w, int h) {
    // save the current clip so PopClipRect can restore it. on overflow we simply
    // stop saving (the deepest clip still applies) rather than corrupt memory. (satoru)
    if (clip_sp < 16) {
        clip_stack[clip_sp].x = clip_x;
        clip_stack[clip_sp].y = clip_y;
        clip_stack[clip_sp].w = clip_w;
        clip_stack[clip_sp].h = clip_h;
        clip_stack[clip_sp].enabled = clipping_enabled;
    }
    clip_sp++;

    // intersect the requested rect with the active clip so a child never draws
    // outside its parent. (satoru)
    int nx = x, ny = y, nw = w, nh = h;
    if (clipping_enabled) {
        int ax = clip_x > nx ? clip_x : nx;
        int ay = clip_y > ny ? clip_y : ny;
        int ar = (clip_x + clip_w) < (nx + nw) ? (clip_x + clip_w) : (nx + nw);
        int ab = (clip_y + clip_h) < (ny + nh) ? (clip_y + clip_h) : (ny + nh);
        nx = ax; ny = ay;
        nw = ar - ax; nh = ab - ay;
        if (nw < 0) nw = 0;
        if (nh < 0) nh = 0;
    }
    clip_x = nx; clip_y = ny; clip_w = nw; clip_h = nh;
    clipping_enabled = true;
}

void Graphics::PopClipRect() {
    if (clip_sp <= 0) { clipping_enabled = false; return; }
    clip_sp--;
    if (clip_sp < 16) {
        clip_x = clip_stack[clip_sp].x;
        clip_y = clip_stack[clip_sp].y;
        clip_w = clip_stack[clip_sp].w;
        clip_h = clip_stack[clip_sp].h;
        clipping_enabled = clip_stack[clip_sp].enabled;
    }
}

uint32_t Graphics::RGB(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint32_t Graphics::RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t Graphics::BlendColors(uint32_t src, uint32_t dst, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    uint32_t a = alpha;
    uint32_t inv = 255u - a;
    // round-half-up via +127 then /255; gcc lowers /255 to mul-shift
    uint32_t r = (((src >> 16) & 0xFFu) * a + ((dst >> 16) & 0xFFu) * inv + 127u) / 255u;
    uint32_t g = (((src >> 8)  & 0xFFu) * a + ((dst >> 8)  & 0xFFu) * inv + 127u) / 255u;
    uint32_t b = (( src        & 0xFFu) * a + ( dst        & 0xFFu) * inv + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// keep the rest of the existing methods for compatibility
void Graphics::BlendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    if (a == 255) { DrawPixel(x, y, RGB(r, g, b)); return; }
    // honour the active clip  -  without this, alpha-blended primitives (shadows,
    // translucent panels, rounded-corner AA) bled past their window. (satoru)
    if (clipping_enabled &&
        ((unsigned)(x - clip_x) >= (unsigned)clip_w ||
         (unsigned)(y - clip_y) >= (unsigned)clip_h)) return;
    uint32_t dst = ReadPixel(x, y);
    uint32_t src = RGBA(r, g, b, a);
    uint32_t blended = BlendColors(src, dst, a);
    DrawPixelUnsafe(x, y, blended);
}

void Graphics::FillRectRounded(int x, int y, int w, int h, int r, uint32_t color) {
    if (r <= 0 || r > h/2 || r > w/2) { FillRect(x, y, w, h, color); return; }
    FillRect(x + r, y, w - 2 * r, h, color);
    FillRect(x, y + r, r, h - 2 * r, color);
    FillRect(x + w - r, y + r, r, h - 2 * r, color);

    uint8_t alpha = (color >> 24) & 0xFF;
    int r2 = r * r;
    if (alpha >= 0xF0) {
        uint32_t op = color | 0xFF000000u;
        for (int dy = 0; dy < r; dy++) {
            int dy2 = dy * dy;
            for (int dx = 0; dx < r; dx++) {
                if (dx*dx + dy2 <= r2) {
                    // route opaque corners through DrawPixel (clip-aware) so rounded
                    // panels respect the active clip; DrawPixel fast-paths opaque
                    // writes so the cost is just the clip test. (satoru)
                    DrawPixel(x + r - 1 - dx, y + r - 1 - dy, op);
                    DrawPixel(x + w - r + dx, y + r - 1 - dy, op);
                    DrawPixel(x + r - 1 - dx, y + h - r + dy, op);
                    DrawPixel(x + w - r + dx, y + h - r + dy, op);
                }
            }
        }
    } else {
        for (int dy = 0; dy < r; dy++) {
            int dy2 = dy * dy;
            for (int dx = 0; dx < r; dx++) {
                if (dx*dx + dy2 <= r2) {
                    DrawPixel(x + r - 1 - dx, y + r - 1 - dy, color);
                    DrawPixel(x + w - r + dx, y + r - 1 - dy, color);
                    DrawPixel(x + r - 1 - dx, y + h - r + dy, color);
                    DrawPixel(x + w - r + dx, y + h - r + dy, color);
                }
            }
        }
    }
    MarkDirty(x, y, w, h);
}

void Graphics::FillRectAlpha(int x, int y, int w, int h, uint8_t a, uint32_t color) {
    if (a == 0 || w <= 0 || h <= 0) return;
    // fully opaque  -  hand off to the 64-bit fast fill path. (satoru)
    if (a == 255) { FillRect(x, y, w, h, color | 0xFF000000u); return; }

    // clamp once to framebuffer bounds instead of bounds-checking every
    // pixel, then blend inline. for a constant source colour the per-channel
    // src*alpha terms are loop invariants (cr/cg/cb), so the inner loop only
    // reads the destination, blends, and writes  -  no BlendPixel call, no
    // per-pixel address math. this is the hot path for window drop-shadows
    // that are redrawn every frame, so the win compounds. note: we clamp to
    // the framebuffer (not the clip rect) to exactly match the old
    // per-pixel DrawPixelUnsafe behaviour. (satoru)
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_width)  w = (int)fb_width  - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0 || !active_buffer) return;

    if (fb_bpp == 32) {
        const uint32_t av  = a;
        const uint32_t inv = 255u - av;
        const uint32_t cr  = ((color >> 16) & 0xFFu) * av + 127u;
        const uint32_t cg  = ((color >> 8)  & 0xFFu) * av + 127u;
        const uint32_t cb  = ( color        & 0xFFu) * av + 127u;
        for (int j = 0; j < h; j++) {
            volatile uint32_t* p = (volatile uint32_t*)
                (active_buffer + (size_t)(y + j) * fb_pitch + (size_t)x * 4);
            for (int i = 0; i < w; i++) {
                uint32_t dst = p[i];
                uint32_t rr = (cr + ((dst >> 16) & 0xFFu) * inv) / 255u;
                uint32_t gg = (cg + ((dst >> 8)  & 0xFFu) * inv) / 255u;
                uint32_t bb = (cb + ( dst        & 0xFFu) * inv) / 255u;
                p[i] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            }
        }
        return;
    }

    // fallback for 16/24 bpp framebuffers: clipped per-pixel blend.
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            BlendPixel(x + i, y + j, r, g, b, a);
        }
    }
}

void Graphics::FillGradientV(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    uint8_t tr = (top >> 16) & 0xFF;
    uint8_t tg = (top >> 8) & 0xFF;
    uint8_t tb = top & 0xFF;
    uint8_t br = (bottom >> 16) & 0xFF;
    uint8_t bg = (bottom >> 8) & 0xFF;
    uint8_t bb = bottom & 0xFF;
    int h_safe = h > 0 ? h : 1;
    for (int j = 0; j < h; j++) {
        uint32_t r = tr + (int)(br - tr) * j / h_safe;
        uint32_t g = tg + (int)(bg - tg) * j / h_safe;
        uint32_t b = tb + (int)(bb - tb) * j / h_safe;
        uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
        // use fast fillrect for each scanline (hits 64-bit fast path)
        FillRect(x, y + j, w, 1, c);
    }
}

void Graphics::ApplyShadow(int x, int y, int w, int h, int offset_x, int offset_y, uint8_t alpha) {
    // soft drop shadow: two stacked translucent black rects offset from the
    // source rect, the outer one fainter, mirroring the taskbar's look. built
    // on the existing alpha-fill fast path so it stays cheap. (satoru)
    if (w <= 0 || h <= 0 || alpha == 0) return;
    int sx = x + offset_x;
    int sy = y + offset_y;
    FillRectAlpha(sx + 2, sy + 2, w, h, alpha, 0xFF000000u);
    uint8_t faint = (uint8_t)(alpha / 2);
    if (faint) FillRectAlpha(sx + 4, sy + 4, w, h, faint, 0xFF000000u);
}

void Graphics::BlurRect(int x, int y, int w, int h, int radius) {
    // simplified box blur - just average neighboring pixels
    if (radius < 1) return;
    ClipRect(x, y, w, h);
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint32_t r = 0, g = 0, b = 0, count = 0;
            for (int ky = -radius; ky <= radius; ky++) {
                for (int kx = -radius; kx <= radius; kx++) {
                    int px = x + i + kx;
                    int py = y + j + ky;
                    if (IsPointInBounds(px, py)) {
                         uint32_t c = ReadPixel(px, py);
                         r += (c >> 16) & 0xFF;
                         g += (c >> 8) & 0xFF;
                         b += c & 0xFF;
                         count++;
                    }
                }
            }
            if (count) {
                DrawPixel(x + i, y + j, RGB(r/count, g/count, b/count));
            }
        }
    }
}

int Graphics::GetWidth() { return fb_width; }
int Graphics::GetHeight() { return fb_height; }
uint32_t Graphics::GetPitch() { return fb_pitch; }
uint8_t Graphics::GetBpp() { return fb_bpp; }
uint8_t* Graphics::GetBuffer() { return active_buffer; }
void Graphics::SetBuffer(uint8_t* addr) { active_buffer = addr; }
const Graphics::DrawStats& Graphics::GetDrawStats() { return draw_stats; }

void Graphics::SetColorFilter(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 4) mode = 4;
    g_color_filter = mode;
}
int  Graphics::GetColorFilter()        { return g_color_filter; }
void Graphics::SetHighContrast(bool on){ g_high_contrast = on; }
void Graphics::SetBrightness(int pct){
    if (pct < 10) pct = 10; else if (pct > 100) pct = 100;
    g_brightness = pct;
    MarkUIDirty();   // force a re-tonemap of the whole screen (satoru)
}
int  Graphics::GetBrightness(){ return g_brightness; }

//  drawstring  -  convenience text renderer for desktop/app layers
//  font size scales with resolution: 16px at 1024x768, 16px at 1080p,
//  20px at 1440p for readable text at every density.
void Graphics::DrawString(int x, int y, const char* s, uint32_t fg, uint32_t /*bg*/) {
    if (!s) return;
    // scale font: base 16px for <=1080p, 20px for 1440p+
    float pxh = 16.0f;
    if (fb_height >= 1440) pxh = 20.0f;
    if (SettingsApp::state.font_scale > 1) {
        pxh *= (float)SettingsApp::state.font_scale;
    }
    FontTTF::DrawString(x, y, pxh, s, fg);
}

//  drawcircle  -  midpoint circle algorithm
void Graphics::DrawCircle(int cx, int cy, int radius, uint32_t color, bool filled) {
    if (radius <= 0) { DrawPixel(cx, cy, color); return; }

    int x = 0, y = radius;
    int d = 1 - radius;

    auto hline = [&](int lx, int rx, int ly) {
        if (ly < 0 || ly >= (int)fb_height) return;
        if (rx < lx) return;
        // FillRect-based span  -  hits the 64-bit bulk path on 32 bpp
        FillRect(lx, ly, rx - lx + 1, 1, color);
    };

    while (x <= y) {
        if (filled) {
            hline(cx - x, cx + x, cy + y);
            hline(cx - x, cx + x, cy - y);
            hline(cx - y, cx + y, cy + x);
            hline(cx - y, cx + y, cy - x);
        } else {
            DrawPixel(cx + x, cy + y, color);
            DrawPixel(cx - x, cy + y, color);
            DrawPixel(cx + x, cy - y, color);
            DrawPixel(cx - x, cy - y, color);
            DrawPixel(cx + y, cy + x, color);
            DrawPixel(cx - y, cy + x, color);
            DrawPixel(cx + y, cy - x, color);
            DrawPixel(cx - y, cy - x, color);
        }
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}
