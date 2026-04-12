#include "graphics.h"
#include "display.h"
#include "serial.h"
#include "../apps/settings.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/panic.h"
#include "../ui/font.h"

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
// this is critical for bare-metal: regular memcpy at -o2 can become
// 'rep movsb' (fast-string ops) which bypasses wc buffers on real cpus,
// causing cached fb writes to never reach gpu vram → permanent black screen.
// movntdq + sfence works correctly on wc, wb, and uc memory types.
static void fb_copy_nt(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    // align destination to 16 bytes for movntdq
    while (size > 0 && ((uintptr_t)d & 15)) {
        *d++ = *s++; size--;
    }

    // 128-bit non-temporal stores (sse2  -  guaranteed on x86_64)
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

    // remaining bytes (regular stores  -  only a few bytes, harmless)
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

uint32_t Graphics::target_frame_time_us = 5555; // ~180hz (1000000/180)
uint32_t Graphics::last_frame_time = 0;
uint32_t Graphics::frame_count = 0;
uint32_t Graphics::fps_sample_time = 0;
Graphics::DrawStats Graphics::draw_stats = {0};

bool Graphics::fb_wc_active = false;
uint32_t Graphics::monitor_hz = 0;

int Graphics::clip_x = 0;
int Graphics::clip_y = 0;
int Graphics::clip_w = 0;
int Graphics::clip_h = 0;
bool Graphics::clipping_enabled = false;

Graphics::DirtyRegion Graphics::dirty_regions[16];
int Graphics::dirty_count = 0;

void Graphics::Init(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    fb_addr = (uint8_t*)addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = bpp;
    active_buffer = fb_addr;
    
    SerialLogger::Log("Graphics: Basic Init ");
    SerialLogger::LogHex(width); SerialLogger::Log("x"); SerialLogger::LogHex(height);
    SerialLogger::Log(" @ "); SerialLogger::LogHex(addr); SerialLogger::Log("\r\n");
    
    // initialize stats  -  default to 60 hz until detectrefreshrate() overrides
    draw_stats.target_fps = 60;
    draw_stats.vsync_enabled = false; // disabled  -  vga vsync polling hangs on non-vga gpus
    
    // clear dirty regions
    for (int i = 0; i < 16; i++) {
        dirty_regions[i].active = false;
    }
    dirty_count = 0;

    // remap framebuffer pages to write-combining (pat entry 1).
    // this is essential for bare-metal: wb-cached mmio writes are invisible
    // on real gpus and cause a permanent black screen.
    fb_wc_active = remap_fb_writecombining(addr, (size_t)pitch * height);
    if (!fb_wc_active) {
        SerialLogger::Log("Graphics: WARNING  -  WC remap FAILED, using wbinvd fallback (slower)\r\n");
    }
    // keep panic subsystem's framebuffer info in sync
    KernelPanic::UpdateFramebuffer((uint64_t)addr, pitch, width, height, bpp);}

void Graphics::ReinitForResolution(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    // free old back buffer if allocated (pmm-backed)
    if (back_buffer) {
        PMM::FreeBytes(back_buffer, back_buffer_size);
        back_buffer = 0;
        back_buffer_size = 0;
    }
    // re-initialize base state
    Init(addr, width, height, pitch, bpp);
    // re-allocate double buffer at new size if needed (via pmm for large bufs)
    if (render_mode == DOUBLE_BUFFER || render_mode == TRIPLE_BUFFER) {
        size_t buffer_size = (size_t)fb_pitch * fb_height;
        back_buffer = (uint8_t*)PMM::AllocBytes(buffer_size);
        if (back_buffer) {
            back_buffer_size = buffer_size;
            active_buffer = back_buffer;
        } else {
            render_mode = SINGLE_BUFFER;
            active_buffer = fb_addr;
        }
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
        target_frame_time_us = 1000000 / mode->refresh_rate;
        draw_stats.target_fps = mode->refresh_rate;
        
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
    render_mode = mode;
    
    if (mode == DOUBLE_BUFFER || mode == TRIPLE_BUFFER) {
        // allocate back buffer via pmm (large allocation)
        size_t buffer_size = (size_t)fb_pitch * fb_height;
        back_buffer = (uint8_t*)PMM::AllocBytes(buffer_size);
        if (back_buffer) {
            back_buffer_size = buffer_size;
            active_buffer = back_buffer;
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
        active_buffer = fb_addr;
    }
}

Graphics::RenderMode Graphics::GetRenderMode() {
    return render_mode;
}

void Graphics::BeginFrame() {
    uint32_t current_time = TimeManager::NowUTC().us;
    if (frame_count > 0) {
        draw_stats.last_frame_time_us = current_time - last_frame_time;
    }
    last_frame_time = current_time;
    
    // clear dirty regions from previous frame
    ClearDirtyRegions();
}

void Graphics::EndFrame() {
    frame_count++;
    draw_stats.frames_rendered++;
    
    if (render_mode == DOUBLE_BUFFER || render_mode == TRIPLE_BUFFER) {
        SwapBuffers();
    }
    
    // vsync for frame pacing
    if (draw_stats.vsync_enabled) {
        WaitForVSync();
    }
    
    // accurate fps calculation every 30 frames using stored sample timestamp
    if (frame_count % 30 == 0) {
        uint32_t current_time = TimeManager::NowUTC().us;
        if (fps_sample_time != 0) {
            uint32_t elapsed = current_time - fps_sample_time;
            if (elapsed > 0) {
                draw_stats.current_fps = 30000000 / elapsed;
                draw_stats.avg_frame_time_us = elapsed / 30;
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

    uint32_t start_ms = TimeManager::NowUTC().us / 1000;
    if (start_ms == 0) start_ms = 1;

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

    uint32_t end_ms = TimeManager::NowUTC().us / 1000;
    uint32_t elapsed_ms = end_ms - start_ms;
    if (elapsed_ms == 0 || elapsed_ms > 500) return 60;

    uint32_t hz = (cycles * 1000 + elapsed_ms / 2) / elapsed_ms;
    if (hz < 24 || hz > 360) hz = 60;  // sanity clamp
    monitor_hz = hz;  // store for settings app
    return hz;
}

bool Graphics::ShouldRender() {
    uint32_t current_time = TimeManager::NowUTC().us;
    return (current_time - last_frame_time) >= target_frame_time_us;
}

void Graphics::WaitForVSync() {
    DisplayController::WaitVSync();
}

void Graphics::SwapBuffers() {
    if (render_mode == SINGLE_BUFFER) return;
    
    if (back_buffer && fb_addr) {
        uint32_t bytes_per_pixel = fb_bpp / 8;
        uint32_t bytes_per_line = fb_width * bytes_per_pixel;
        bool pitch_match = (bytes_per_line == fb_pitch);

        // if few dirty regions exist and they cover less than ~60% of the screen,
        // copy only those regions (partial swap). otherwise full-frame copy.
        // all copies use non-temporal stores (fb_copy_nt) which bypass cpu caches
        // and write directly to gpu vram  -  essential for bare-metal.
        if (dirty_count > 0 && dirty_count <= 16) {
            // compute total dirty area
            uint32_t dirty_pixels = 0;
            for (int i = 0; i < dirty_count; i++) {
                if (!dirty_regions[i].active) continue;
                dirty_pixels += (uint32_t)dirty_regions[i].w * (uint32_t)dirty_regions[i].h;
            }
            uint32_t total_pixels = fb_width * fb_height;

            if (dirty_pixels < (total_pixels * 3 / 5)) {
                // partial swap  -  copy only dirty regions (non-temporal)
                for (int i = 0; i < dirty_count; i++) {
                    if (!dirty_regions[i].active) continue;
                    int rx = dirty_regions[i].x;
                    int ry = dirty_regions[i].y;
                    int rw = dirty_regions[i].w;
                    int rh = dirty_regions[i].h;
                    // clamp to screen bounds
                    if (rx < 0) { rw += rx; rx = 0; }
                    if (ry < 0) { rh += ry; ry = 0; }
                    if (rx + rw > (int)fb_width)  rw = (int)fb_width - rx;
                    if (ry + rh > (int)fb_height) rh = (int)fb_height - ry;
                    if (rw <= 0 || rh <= 0) continue;

                    uint32_t region_bytes = (uint32_t)rw * bytes_per_pixel;
                    for (int y = ry; y < ry + rh; y++) {
                        uint32_t offset = y * fb_pitch + rx * bytes_per_pixel;
                        fb_copy_nt(fb_addr + offset, back_buffer + offset, region_bytes);
                    }
                }
                // sfence synchronizes non-temporal stores → gpu vram
                __asm__ __volatile__("sfence" ::: "memory");
                ClearDirtyRegions();
                return;
            }
        }

        // full-frame copy (non-temporal)
        if (pitch_match) {
            fb_copy_nt(fb_addr, back_buffer, fb_pitch * fb_height);
        } else {
            uint8_t* src = back_buffer;
            uint8_t* dst = fb_addr;
            for (uint32_t y = 0; y < fb_height; y++) {
                fb_copy_nt(dst, src, bytes_per_line);
                src += fb_pitch;
                dst += fb_pitch;
            }
        }
    }

    // sfence synchronizes all non-temporal writes → gpu vram.
    // unlike regular memcpy, fb_copy_nt uses movntdq which bypasses cpu caches
    // entirely, so sfence alone is sufficient regardless of wc/wb/uc memory type.
    __asm__ __volatile__("sfence" ::: "memory");

    // clear dirty regions after swap so they don't go stale across frames.
    // (the main loop never calls beginframe, so we do it here.)
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
    
    uint32_t offset = y * fb_pitch + x * (fb_bpp / 8);
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
    
    uint32_t offset = y * fb_pitch + x * (fb_bpp / 8);
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
    if (w <= 0 || h <= 0) return;
    
    uint8_t alpha = (color >> 24) & 0xFF;
    if (fb_bpp == 32 && (blend_mode == BLEND_NONE || alpha >= 0xF0)) {
        // fast path for 32-bit opaque/near-opaque fills
        uint32_t opaque_color = color | 0xFF000000u;
        // pack two pixels into a 64-bit value for double-wide writes
        uint64_t duo = ((uint64_t)opaque_color << 32) | (uint64_t)opaque_color;
        for (int j = 0; j < h; j++) {
            uint32_t* row = (uint32_t*)(active_buffer + (y + j) * fb_pitch + x * 4);
            int i = 0;
            // 64-bit bulk fill: process 2 pixels per iteration
            uint64_t* row64 = (uint64_t*)row;
            int pairs = w / 2;
            for (; i < pairs; i++) {
                row64[i] = duo;
            }
            // handle odd trailing pixel
            if (w & 1) {
                row[w - 1] = opaque_color;
            }
        }
    } else if (alpha == 0) {
        return; // fully transparent  -  skip
    } else {
        // general case (blended)
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
    if (dirty_count >= 16) return; // max regions reached
    
    DirtyRegion& region = dirty_regions[dirty_count++];
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    region.active = true;
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

uint32_t Graphics::RGB(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint32_t Graphics::RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t Graphics::BlendColors(uint32_t src, uint32_t dst, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    
    uint8_t src_r = (src >> 16) & 0xFF;
    uint8_t src_g = (src >> 8) & 0xFF;
    uint8_t src_b = src & 0xFF;
    
    uint8_t dst_r = (dst >> 16) & 0xFF;
    uint8_t dst_g = (dst >> 8) & 0xFF;
    uint8_t dst_b = dst & 0xFF;
    
    uint8_t inv_alpha = 255 - alpha;
    uint8_t out_r = (src_r * alpha + dst_r * inv_alpha) / 255;
    uint8_t out_g = (src_g * alpha + dst_g * inv_alpha) / 255;
    uint8_t out_b = (src_b * alpha + dst_b * inv_alpha) / 255;
    
    return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
}

// keep the rest of the existing methods for compatibility
void Graphics::BlendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    if (a == 255) { DrawPixel(x, y, RGB(r, g, b)); return; }
    uint32_t dst = ReadPixel(x, y);
    uint32_t src = RGBA(r, g, b, a);
    uint32_t blended = BlendColors(src, dst, a);
    DrawPixelUnsafe(x, y, blended);
}

void Graphics::FillRectRounded(int x, int y, int w, int h, int r, uint32_t color) {
    if (r <= 0 || r > h/2 || r > w/2) { FillRect(x, y, w, h, color); return; }
    // force opaque for maximum performance in rounded rects
    uint32_t opaque = color | 0xFF000000u;
    FillRect(x + r, y, w - 2 * r, h, opaque);           // center column
    FillRect(x, y + r, r, h - 2 * r, opaque);            // left strip
    FillRect(x + w - r, y + r, r, h - 2 * r, opaque);    // right strip
    
    // draw rounded corners with fast pixel writes
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            if ((dx * dx + dy * dy) <= (r * r)) {
                DrawPixelUnsafe(x + r - 1 - dx, y + r - 1 - dy, opaque); // tl
                DrawPixelUnsafe(x + w - r + dx, y + r - 1 - dy, opaque); // tr
                DrawPixelUnsafe(x + r - 1 - dx, y + h - r + dy, opaque); // bl
                DrawPixelUnsafe(x + w - r + dx, y + h - r + dy, opaque); // br
            }
        }
    }
}

void Graphics::FillRectAlpha(int x, int y, int w, int h, uint8_t a, uint32_t color) {
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
        if (lx < 0) lx = 0;
        if (rx >= (int)fb_width) rx = (int)fb_width - 1;
        for (int i = lx; i <= rx; i++) DrawPixel(i, ly, color);
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
