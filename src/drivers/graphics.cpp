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
#include "bga.h"          // live hardware geometry for the resize self-heal (satoru)
#include "outputs.h"      // multi-monitor output list (satoru)
#include "timer.h"        // Timer::GetRealMs for the scaled-present safety refresh (satoru)
#include "../kernel/taskmem.h"   // kurono.taskmem=1 frame charges (satoru)

// when the virtio-gpu backend is active, fb_addr is a guest-ram resource
// backing that must be transferred + flushed to the host after each frame.
// DisplayManager sets this once it selects + inits the virtio backend. (satoru)
static bool g_present_via_virtio = false;

// multi-monitor: when more than one gop framebuffer is lit, fb_addr/fb_width/
// fb_height/fb_pitch describe a ram-backed VIRTUAL desktop spanning every output
// and SwapBuffers scatters each damaged rect to the hardware framebuffers it
// overlaps. false (the default) leaves the single-framebuffer path untouched. (satoru)
static bool    g_multi = false;
static uint8_t* g_virt_front = nullptr;
static size_t   g_virt_bytes = 0;
static bool     g_multi_relayout = false;   // set by a runtime relayout, taken by the gui loop

// ui scale: fb_width/fb_height/fb_pitch are LOGICAL (what every draw call and the
// whole desktop see) when g_scale != 100, and the hardware framebuffer keeps its own
// physical geometry here. at 100 these mirror fb_* and nothing below changes. (satoru)
static int      g_scale = 100;
static uint32_t g_ph_w = 0, g_ph_h = 0, g_ph_pitch = 0;

// page directory tables from kurono_boot.asm - needed to remap fb caching
extern "C" uint8_t pd_tables[];
// pdpt from kurono_boot.asm - needed to add new pd tables for >16gb fbs
extern "C" uint8_t pdpt[];
// 1 gib-page pdpts for pml4[1..3] (512 gb..2 tb) from kurono_boot.asm - needed to
// set write-combining on a high efi gop framebuffer that lands in that range (satoru)
extern "C" uint8_t efi_hi_pdpts[];

// must stay in sync with kurono_boot.asm: the EFI path maps 0..512 GB with 2 MB
// pages (PML4[0]) PLUS 512 GB..2 TB with 1 GB pages (PML4[1..3], efi_hi_pdpts),
// so high EFI GOP framebuffers up to 2 TB are reachable (seen: ~1 TB at
// 0xFA10000000 on a real laptop; older note: 0x4000000000 = 256 gb). (satoru)
static constexpr uint64_t BOOT_IDENTITY_MAP_GB = 2048;
// the 2 MB-page PD range (= NUM_PDS in kurono_boot.asm). gb indices below this live
// in pd_tables (2 MB PDs); [PD_MAP_GB, BOOT_IDENTITY_MAP_GB) are 1 GiB pages in
// efi_hi_pdpts and must have WC set on the pdpte, not a (nonexistent) pd. (satoru)
static constexpr uint64_t PD_MAP_GB = 512;

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

    // 256-byte unrolled loop - issues 4 wc-combine-buffer worth of stores
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
        SerialLogger::Log("Graphics: WC remap skipped - no FB address\r\n");
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

        if (gb_idx < PD_MAP_GB) {
            // 0..512 gb: 2 mb-page pds built by the boot asm.
            pd = (uint64_t*)((uintptr_t)pd_tables + gb_idx * 4096);
        } else if (gb_idx < BOOT_IDENTITY_MAP_GB) {
            // 512 gb..2 tb: a 1 gib page in efi_hi_pdpts (pml4[1..3]). there is NO pd
            // here (the old code indexed pd_tables out of bounds → memory corruption →
            // black screen), so set wc on the 1 gib pdpte directly. one 1 gib page
            // covers this whole gb, so this is idempotent across the 2 mb steps. (satoru)
            uint64_t* he = &((uint64_t*)(uintptr_t)efi_hi_pdpts)[gb_idx - PD_MAP_GB];
            *he &= ~((1ULL << 3) | (1ULL << 4) | (1ULL << 12));
            *he |=  (1ULL << 3);                 // pwt = 1 → pat entry 1 = wc
            __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
            pages_remapped++;
            continue;
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

    // full memory fence - ensures wc attribute is observed before any fb writes
    __asm__ __volatile__("mfence" ::: "memory");

    if (had_overflow) {
        SerialLogger::Log("Graphics: WARNING - FB region extends beyond mappable range!\r\n");
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
// dimming, so we dim in the swapbuffers post-process pass instead - the
// display settings slider now has a real effect. 100 = unmodified. (satoru)
static int  g_brightness     = 100;
// per-output software dim + a global warm night-light tint. (satoru)
static int  g_out_brightness[OUTPUT_MAX] = { 100, 100, 100, 100 };
static int  g_night_light    = 0;
static inline bool any_tone();

bool Graphics::fb_wc_active = false;
uint32_t Graphics::monitor_hz = 0;
// true only when DetectRefreshRate timed real vga vsync pulses; its 60 on an
// efi/gop machine is a default, not a measurement. (satoru)
static bool g_refresh_measured = false;

int Graphics::clip_x = 0;
int Graphics::clip_y = 0;
int Graphics::clip_w = 0;
int Graphics::clip_h = 0;
bool Graphics::clipping_enabled = false;
Graphics::ClipSave Graphics::clip_stack[16] = {};
int Graphics::clip_sp = 0;

Graphics::DirtyRegion Graphics::dirty_regions[Graphics::DIRTY_REGION_CAP];
int Graphics::dirty_count = 0;
Graphics::DirtyRegion Graphics::dirty_prev[Graphics::DIRTY_REGION_CAP];
int Graphics::dirty_prev_count = 0;
bool Graphics::full_screen_dirty = false;
uint64_t Graphics::dirty_area_total = 0;

// Global UI dirty signal - see graphics.h. volatile: written from input /
// app / animation paths (potentially other kernel processes) and read by
// the GUI process each frame. A monotonically-incrementing counter lets the
// GUI loop detect "changed since I last looked" via a snapshot compare, so a
// MarkUIDirty() that lands between the loop's check and its clear is still
// observed on the next iteration rather than being clobbered. (satoru)
static volatile uint32_t g_ui_dirty = 1;          // start dirty: paint frame 0
static uint32_t          g_ui_dirty_seen = 0;

// return address of the most recent MarkUIDirty caller, for the gui loop's
// [gui] report (finds whoever keeps an idle desktop redrawing).
static void* g_ui_dirty_from = nullptr;
__attribute__((noinline)) void Graphics::MarkUIDirty() {
    g_ui_dirty_from = __builtin_return_address(0);
    g_ui_dirty++;
}
void* Graphics::LastUIDirtyCaller() { return g_ui_dirty_from; }

// earliest pending frame deadline (real ms), 0 = none.
static volatile uint32_t g_frame_at = 0;
void Graphics::RequestFrameAt(uint32_t ms) {
    if (ms == 0) ms = 1;
    uint32_t cur = g_frame_at;
    if (cur == 0 || (int32_t)(ms - cur) < 0) g_frame_at = ms;
}
bool Graphics::ConsumeFrameDue(uint32_t now_ms) {
    uint32_t at = g_frame_at;
    if (at == 0 || (int32_t)(now_ms - at) < 0) return false;
    g_frame_at = 0;
    return true;
}
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
        SerialLogger::Log("Graphics: Init rejected - invalid framebuffer args\r\n");
        return;
    }
    // a mode change replaces the scanned buffer: put the original surfaces
    // back and leave zero-copy before anything is freed. (satoru)
    ZcSuspendIfActive();
    // any back_buffer left over from a prior config is now sized wrong; drop it.
    bool had_bb = (back_buffer != 0);
    if (back_buffer) {
        TaskMem::UnchargeFrames(back_buffer); PMM::FreeBytes(back_buffer, back_buffer_size);
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
    g_ph_w = width; g_ph_h = height; g_ph_pitch = pitch;
    // a resolution change keeps the ui scale only while double buffering (the
    // caller restores it) and a 32 bpp mode; otherwise fall back to 100%. (satoru)
    if (g_scale != 100) {
        if (had_bb && bpp == 32 && !g_multi && !g_present_via_virtio) {
            fb_width  = (uint32_t)((uint64_t)width  * 100 / (uint32_t)g_scale);
            fb_height = (uint32_t)((uint64_t)height * 100 / (uint32_t)g_scale);
            fb_pitch  = fb_width * 4u;
        } else {
            g_scale = 100;
        }
    }

    SerialLogger::Log("Graphics: Basic Init ");
    SerialLogger::LogHex(width); SerialLogger::Log("x"); SerialLogger::LogHex(height);
    SerialLogger::Log(" @ "); SerialLogger::LogHex(addr); SerialLogger::Log("\r\n");

    draw_stats.target_fps = 60;
    draw_stats.vsync_enabled = false;

    for (int i = 0; i < DIRTY_REGION_CAP; i++) {
        dirty_regions[i].active = false;
    }
    dirty_count = 0;

    fb_wc_active = remap_fb_writecombining(addr, (size_t)pitch * height);
    if (!fb_wc_active) {
        SerialLogger::Log("Graphics: WARNING - WC remap FAILED, using wbinvd fallback (slower)\r\n");
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

// resize self-heal: when the host resizes the qemu window the bga reprograms
// its mode registers, but if the guest's res-sync path is starved (firefox
// pinning every core) the compositor keeps blitting at the OLD pitch into the
// NEW-stride scanout = the diagonal-tear garbage the user saw. read the LIVE
// bga geometry every render; if it drifted from our cached fb_*, reinit +
// realloc the backbuffer + full-dirty so the very next frame matches the
// hardware. runs on whatever process renders, so starvation can't leave a
// torn frame on screen. returns true if a resync happened (caller relayouts
// the desktop). std-vga/bga only; virtio-gpu owns its own present size. (satoru)
bool Graphics::SyncToHardwareGeometry() {
    if (g_multi) return false;   // fb_addr is the virtual desktop, not the bga aperture (satoru)
    if (!BGA::available || !BGA::framebuffer) return false;
    uint32_t hw_w = BGA::width, hw_h = BGA::height, hw_pitch = BGA::pitch;
    uint8_t  hw_bpp = (uint8_t)BGA::bpp;
    if (hw_w == 0 || hw_h == 0 || hw_pitch == 0) return false;
    if (hw_w == g_ph_w && hw_h == g_ph_h && hw_pitch == g_ph_pitch &&
        (uintptr_t)BGA::framebuffer == (uintptr_t)fb_addr) {
        return false;   // in sync (satoru)
    }
    SerialLogger::Log("Graphics: hardware geometry drift - resyncing compositor\r\n");
    ReinitForResolution((uintptr_t)BGA::framebuffer, hw_w, hw_h, hw_pitch, hw_bpp);
    full_screen_dirty = true;
    MarkUIDirty();
    return true;
}

bool Graphics::MultiActive() { return g_multi; }

bool Graphics::TakeMultiRelayout() {
    bool r = g_multi_relayout;
    g_multi_relayout = false;
    return r;
}

int Graphics::GetUIScale() { return g_scale; }

// ---- wallpaper damage-only state and present self-check (satoru) ----
// the desktop may repaint only the damaged parts of its cached wallpaper. the
// crc check below turns it back off (with a log line) if the scanned buffer
// and the compositor back buffer ever disagree. default off. (satoru)
static bool g_wp_damage = false;
static int  g_sc_kind = Graphics::SELFCHECK_NONE;
static int  g_sc_frames = 0;
static int  g_sc_frames_total = 0;
static bool g_sc_skip_logged = false;

void Graphics::SetWallpaperDamageOnly(bool on) {
    g_wp_damage = on;
    MarkFullScreenDirty();
    MarkUIDirty();
}
bool Graphics::WallpaperDamageOnly() { return g_wp_damage; }

void Graphics::ArmSelfCheck(int kind, int frames) {
    g_sc_kind = kind;
    g_sc_frames = frames > 0 ? frames : 0;
    g_sc_frames_total = g_sc_frames;
    g_sc_skip_logged = false;
}
int Graphics::SelfCheckFramesLeft() { return g_sc_frames; }

// ---- zero-copy present state (satoru) ----
// the buffer the display engine scans while zero-copy is active. it is always
// write-back ram owned by the kernel: the multi-output virtual desktop, or a
// private buffer allocated here for a single output. the wc-mapped gop
// framebuffer is never a render target: uncached reads during blending were
// what made the first stage 2 attempt unusable. (satoru)
static bool g_zerocopy = false;
static uint8_t* g_zc_target = nullptr;
static size_t   g_zc_target_bytes = 0;
static bool     g_zc_own_buf = false;
static uint8_t* g_zc_own_ptr = nullptr;
static size_t   g_zc_own_bytes = 0;
static int      g_zc_test_mode = 0;      // 0 off, 1 ram target test, 2 wc reproduction (satoru)
static uint32_t g_zc_frames = 0;
static uint32_t g_zc_filter_state = 0;
static Graphics::ZeroCopyHook g_zc_suspend = nullptr;
static Graphics::ZeroCopyHook g_zc_present = nullptr;

bool Graphics::ZeroCopyActive() { return g_zerocopy; }
bool Graphics::ZeroCopyTestMode() { return g_zc_test_mode != 0; }
size_t Graphics::GetBufferBytes() {
    if (g_zerocopy) return g_zc_target_bytes;
    return (size_t)fb_pitch * (size_t)fb_height;
}
void Graphics::SetZeroCopyHooks(ZeroCopyHook suspend, ZeroCopyHook present) {
    g_zc_suspend = suspend;
    g_zc_present = present;
}

static void zc_copy_str(char* dst, int max, const char* src) {
    if (!dst || max <= 0) return;
    int i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// start rendering into a write-back ram buffer. multi output already has one
// (the compositor virtual desktop); single output gets a private buffer so the
// gop framebuffer stays the untouched, wc-mapped scanout the cpu present path
// uses. every damage published later is clflushed; the display engine reads
// memory, not cpu caches. (satoru)
bool Graphics::ZeroCopyEnter(char* why, int whylen) {
    if (why && whylen > 0) why[0] = 0;
    if (g_zerocopy) return true;
    if (!fb_addr || !back_buffer) { zc_copy_str(why, whylen, "no compositor buffer"); return false; }
    if (fb_bpp != 32) { zc_copy_str(why, whylen, "framebuffer is not 32 bpp"); return false; }
    if (g_scale != 100) { zc_copy_str(why, whylen, "ui scale is not 100"); return false; }
    if (g_present_via_virtio) { zc_copy_str(why, whylen, "virtio present is active"); return false; }
    if (any_tone()) {
        zc_copy_str(why, whylen, "accessibility filter is active");
        return false;
    }
    size_t bytes = (size_t)fb_pitch * (size_t)fb_height;
    if (!bytes) { zc_copy_str(why, whylen, "empty framebuffer"); return false; }
    if (g_multi) {
        g_zc_target = fb_addr;
        g_zc_target_bytes = bytes;
        g_zc_own_buf = false;
    } else {
        uint8_t* b = (uint8_t*)PMM::AllocBytes(bytes);
        if (!b) { zc_copy_str(why, whylen, "no ram for the scanout buffer"); return false; }
        TaskMem::ChargeFrames(b, bytes);
        // seed it with the frame on screen so any pixel the first damage pass
        // does not cover is still the old image instead of uninitialized ram. (satoru)
        uint32_t row = fb_width * 4u;
        if (row > fb_pitch) row = fb_pitch;
        for (uint32_t y = 0; y < fb_height; y++)
            memcpy(b + (size_t)y * fb_pitch, back_buffer + (size_t)y * fb_pitch, row);
        g_zc_target = b;
        g_zc_target_bytes = bytes;
        g_zc_own_buf = true;
        g_zc_own_ptr = b;
        g_zc_own_bytes = bytes;
    }
    g_zerocopy = true;
    g_zc_filter_state = (uint32_t)g_color_filter | ((uint32_t)g_high_contrast << 8) |
                        ((uint32_t)g_brightness << 16) | ((uint32_t)g_night_light << 24);
    active_buffer = g_zc_target;
    g_zc_frames = 0;
    if (g_zc_test_mode == 0) ArmSelfCheck(SELFCHECK_ZEROCOPY, 24);
    MarkFullScreenDirty();
    MarkUIDirty();
    return true;
}

// leave zero-copy: restore any redirected scanout first (driver hook), keep
// the last scanned frame as the first back-buffered one, then give the caller
// its normal cpu present path back. (satoru)
void Graphics::ZeroCopyLeave() {
    if (!g_zerocopy) return;
    // the driver restores the plane surfaces before this buffer can be freed
    // or repointed; the hook must be idempotent because a relayout may already
    // have run it. (satoru)
    if (g_zc_suspend) g_zc_suspend();
    g_zerocopy = false;
    if (back_buffer && g_zc_target && back_buffer_size >= g_zc_target_bytes) {
        uint32_t row = fb_width * 4u;
        if (row > fb_pitch) row = fb_pitch;
        for (uint32_t y = 0; y < fb_height; y++)
            memcpy(back_buffer + (size_t)y * fb_pitch, g_zc_target + (size_t)y * fb_pitch, row);
    }
    if (g_zc_own_buf && g_zc_own_ptr) {
        TaskMem::UnchargeFrames(g_zc_own_ptr);
        PMM::FreeBytes(g_zc_own_ptr, g_zc_own_bytes);
    }
    g_zc_own_buf = false;
    g_zc_own_ptr = nullptr;
    g_zc_own_bytes = 0;
    g_zc_target = nullptr;
    g_zc_target_bytes = 0;
    g_zc_test_mode = 0;
    if (g_sc_kind == SELFCHECK_ZEROCOPY) { g_sc_kind = SELFCHECK_NONE; g_sc_frames = 0; }
    active_buffer = back_buffer ? back_buffer : fb_addr;
    MarkFullScreenDirty();
    MarkUIDirty();
}

void Graphics::ZcSuspendIfActive() {
    if (!g_zerocopy) return;
    ZeroCopyLeave();
    SerialLogger::Log("Graphics: zero-copy suspended (compositor buffer change)\r\n");
}

// qemu test hook. mode 1 runs the same machinery on a ram target and copies
// the damage to the framebuffer as a stand-in for the display engine; mode 2
// renders straight into the wc framebuffer to reproduce the old single-output
// slowdown for measurement only. (satoru)
bool Graphics::ZeroCopyTestStart(int mode, char* why, int whylen) {
    if (why && whylen > 0) why[0] = 0;
    if (g_zerocopy) { zc_copy_str(why, whylen, "zero-copy is already active"); return false; }
    if (mode == 2) {
        if (g_multi) { zc_copy_str(why, whylen, "the wc reproduction needs a single output"); return false; }
        if (!fb_addr || !back_buffer || fb_bpp != 32) {
            zc_copy_str(why, whylen, "no usable framebuffer for the wc test");
            return false;
        }
        g_zc_test_mode = 2;
        g_zerocopy = true;
        g_zc_target = fb_addr;
        g_zc_target_bytes = (size_t)fb_pitch * (size_t)fb_height;
        g_zc_own_buf = false;
        active_buffer = fb_addr;
        // the self-check compares against the back buffer and would disable
        // this path on the first frame, so it is skipped for the measurement. (satoru)
        ArmSelfCheck(SELFCHECK_NONE, 0);
        MarkFullScreenDirty();
        MarkUIDirty();
        SerialLogger::Log("[igpu-zc] test: rendering into the wc framebuffer (reproduces the old single-output path, self-check off)\r\n");
        return true;
    }
    if (!ZeroCopyEnter(why, whylen)) return false;
    g_zc_test_mode = 1;
    SerialLogger::Log("[igpu-zc] test: zero-copy on a write-back ram scanout, present copies damage to the framebuffer\r\n");
    return true;
}

// flush one damaged rect of the scanned buffer. the display engine reads
// memory, so every cache line the cpu dirtied has to be written back before it
// fetches. clwb keeps the line in the cache (no invalidate, no re-fetch on the
// next frame) and clflushopt pipelines; plain clflush is the fallback. (satoru)
static int zc_flush_kind() {
    static int k = -1;
    if (k >= 0) return k;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
    (void)a; (void)c; (void)d;
    if (b & (1u << 24)) k = 2;        // clwb
    else if (b & (1u << 23)) k = 1;   // clflushopt
    else k = 0;                       // clflush
    return k;
}
static inline void zc_flush_line(const uint8_t* p) {
    switch (zc_flush_kind()) {
    case 2: __asm__ __volatile__("clwb (%0)" : : "r"(p) : "memory"); break;
    case 1: __asm__ __volatile__("clflushopt (%0)" : : "r"(p) : "memory"); break;
    default: __asm__ __volatile__("clflush (%0)" : : "r"(p) : "memory"); break;
    }
}
static void zc_flush_rect(const uint8_t* base, uint32_t pitch, int x, int y, int w, int h) {
    if (!base || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        const uint8_t* p = base + (size_t)(y + row) * pitch + (size_t)x * 4u;
        const uint8_t* e = p + (size_t)w * 4u;
        for (; p < e; p += 64)
            zc_flush_line(p);
    }
}

// clip one rect list entry to the visible frame. false when it is empty. (satoru)
static bool zc_clip_rect(int& x, int& y, int& w, int& h, uint32_t fw, uint32_t fh) {
    if (w <= 0 || h <= 0) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)fw || y >= (int)fh) return false;
    if (x + w > (int)fw) w = (int)fw - x;
    if (y + h > (int)fh) h = (int)fh - y;
    return w > 0 && h > 0;
}

void Graphics::ZcFlushRects(const Rect* rects, int count) {
    if (!g_zc_target) return;
    if (!rects || count <= 0) {
        zc_flush_rect(g_zc_target, fb_pitch, 0, 0, (int)fb_width, (int)fb_height);
        return;
    }
    if (count > 32) count = 32;
    for (int i = 0; i < count; i++) {
        int x = rects[i].x, y = rects[i].y, w = rects[i].w, h = rects[i].h;
        if (!zc_clip_rect(x, y, w, h, fb_width, fb_height)) continue;
        zc_flush_rect(g_zc_target, fb_pitch, x, y, w, h);
    }
}

void Graphics::ZcMirrorRects(const Rect* rects, int count) {
    if (!back_buffer || !g_zc_target || back_buffer_size < g_zc_target_bytes) return;
    if (!rects || count <= 0) {
        uint32_t row = fb_width * 4u;
        if (row > fb_pitch) row = fb_pitch;
        for (uint32_t y = 0; y < fb_height; y++)
            memcpy(back_buffer + (size_t)y * fb_pitch, g_zc_target + (size_t)y * fb_pitch, row);
        return;
    }
    if (count > 32) count = 32;
    for (int i = 0; i < count; i++) {
        int x = rects[i].x, y = rects[i].y, w = rects[i].w, h = rects[i].h;
        if (!zc_clip_rect(x, y, w, h, fb_width, fb_height)) continue;
        size_t bytes = (size_t)w * 4u;
        for (int row = 0; row < h; row++) {
            size_t off = (size_t)(y + row) * fb_pitch + (size_t)x * 4u;
            memcpy(back_buffer + off, g_zc_target + off, bytes);
        }
    }
}

// qemu test present: the real display engine is not pointed at the ram target,
// so copy the damage to the framebuffer exactly like the old present did. (satoru)
void Graphics::ZcCopyRects(const Rect* rects, int count) {
    if (!g_zc_target || !fb_addr || g_zc_target == fb_addr) return;
    if (!rects || count <= 0) {
        fb_copy_nt(fb_addr, g_zc_target, (size_t)fb_pitch * fb_height);
        return;
    }
    if (count > 32) count = 32;
    for (int i = 0; i < count; i++) {
        int x = rects[i].x, y = rects[i].y, w = rects[i].w, h = rects[i].h;
        if (!zc_clip_rect(x, y, w, h, fb_width, fb_height)) continue;
        size_t bytes = (size_t)w * 4u;
        for (int row = 0; row < h; row++) {
            size_t off = (size_t)(y + row) * fb_pitch + (size_t)x * 4u;
            fb_copy_nt(fb_addr + off, g_zc_target + off, bytes);
        }
    }
}

// 64-bit fnv-1a over the visible part of one frame, eight bytes at a time. the
// checks compare whole frames for a handful of frames, so this has to stay
// cheap enough not to stall the desktop it is checking. (satoru)
static uint64_t frame_hash(const uint8_t* base, uint32_t pitch, uint32_t row_bytes, uint32_t h) {
    uint64_t v = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* p = base + (size_t)y * pitch;
        uint32_t x = 0;
        for (; x + 8 <= row_bytes; x += 8) {
            uint64_t w;
            memcpy(&w, p + x, 8);
            v = (v ^ w) * 0x100000001b3ull;
        }
        for (; x < row_bytes; x++) v = (v ^ p[x]) * 0x100000001b3ull;
    }
    return v;
}
static void log_hash(const char* prefix, uint64_t v) {
    SerialLogger::Log(prefix);
    SerialLogger::LogHex64(v);
}

// report the first byte that differs, so a mismatch log says where. (satoru)
static bool frame_first_diff(const uint8_t* a, const uint8_t* b, uint32_t pitch,
                             uint32_t row_bytes, uint32_t h, uint32_t* off, uint8_t* av, uint8_t* bv) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* pa = a + (size_t)y * pitch;
        const uint8_t* pb = b + (size_t)y * pitch;
        for (uint32_t x = 0; x < row_bytes; x++) {
            if (pa[x] != pb[x]) {
                if (off) *off = y * pitch + x;
                if (av) *av = pa[x];
                if (bv) *bv = pb[x];
                return true;
            }
        }
    }
    return false;
}

// compare one zero-copy frame against the reference the check mirrored. on a
// mismatch restore the scanout and the cpu path immediately. (satoru)
bool Graphics::ZcSelfCheckTick() {
    if (g_sc_kind != SELFCHECK_ZEROCOPY || g_sc_frames <= 0) return true;
    if (!g_zc_target || !back_buffer || back_buffer_size < g_zc_target_bytes) {
        g_sc_kind = SELFCHECK_NONE; g_sc_frames = 0;
        return true;
    }
    uint32_t row_bytes = fb_width * 4u;
    if (row_bytes > fb_pitch) row_bytes = fb_pitch;
    uint64_t ca = frame_hash(g_zc_target, fb_pitch, row_bytes, fb_height);
    uint64_t cb = frame_hash(back_buffer, fb_pitch, row_bytes, fb_height);
    g_zc_frames++;
    if (ca != cb) {
        uint32_t off = 0; uint8_t av = 0, bv = 0;
        frame_first_diff(g_zc_target, back_buffer, fb_pitch, row_bytes, fb_height, &off, &av, &bv);
        SerialLogger::Log("[igpu-zc] selfcheck MISMATCH frame ");
        SerialLogger::LogDec((int)g_zc_frames);
        SerialLogger::Log(" off 0x");
        SerialLogger::LogHex(off);
        SerialLogger::Log(" scanned 0x");
        SerialLogger::LogHex(av);
        SerialLogger::Log(" reference 0x");
        SerialLogger::LogHex(bv);
        log_hash(" (hash ", ca);
        log_hash("/", cb);
        SerialLogger::Log("), disabling zero-copy\r\n");
        ZeroCopyLeave();
        return false;
    }
    if (--g_sc_frames == 0) {
        SerialLogger::Log("[igpu-zc] selfcheck PASS, scanned buffer == back buffer for ");
        SerialLogger::LogDec((int)g_zc_frames);
        SerialLogger::Log(" frames (hash ");
        SerialLogger::LogHex64(ca);
        SerialLogger::Log(")\r\n");
    }
    return true;
}

// after a cpu present: the scanned buffer must hold exactly the compositor
// back buffer. a mismatch means the wallpaper damage-only pass missed a stale
// region (or the present copied the wrong span), so turn wallpaper damage back
// off and repaint one full frame. (satoru)
void Graphics::SelfCheckWpTick() {
    if (g_sc_kind != SELFCHECK_WPDAMAGE || g_sc_frames <= 0) return;
    if (!g_wp_damage) { g_sc_kind = SELFCHECK_NONE; g_sc_frames = 0; return; }
    if (g_scale != 100 || g_present_via_virtio ||
        any_tone()) {
        if (!g_sc_skip_logged) {
            SerialLogger::Log("[wpdamage] selfcheck skipped (scaled/filtered present), damage-only stays\r\n");
            g_sc_skip_logged = true;
        }
        g_sc_kind = SELFCHECK_NONE; g_sc_frames = 0;
        return;
    }
    if (!fb_addr || !back_buffer || back_buffer_size < (size_t)fb_pitch * fb_height) {
        g_sc_kind = SELFCHECK_NONE; g_sc_frames = 0;
        return;
    }
    uint32_t row_bytes = fb_width * 4u;
    if (row_bytes > fb_pitch) row_bytes = fb_pitch;
    uint64_t ca = frame_hash(fb_addr, fb_pitch, row_bytes, fb_height);
    uint64_t cb = frame_hash(back_buffer, fb_pitch, row_bytes, fb_height);
    if (ca != cb) {
        uint32_t off = 0; uint8_t av = 0, bv = 0;
        frame_first_diff(fb_addr, back_buffer, fb_pitch, row_bytes, fb_height, &off, &av, &bv);
        SerialLogger::Log("[wpdamage] selfcheck MISMATCH off 0x");
        SerialLogger::LogHex(off);
        SerialLogger::Log(" scanned 0x");
        SerialLogger::LogHex(av);
        SerialLogger::Log(" reference 0x");
        SerialLogger::LogHex(bv);
        log_hash(" (hash ", ca);
        log_hash("/", cb);
        SerialLogger::Log("), disabling wallpaper damage-only\r\n");
        g_wp_damage = false;
        g_sc_kind = SELFCHECK_NONE; g_sc_frames = 0;
        MarkFullScreenDirty();
        MarkUIDirty();
        return;
    }
    if (--g_sc_frames == 0) {
        SerialLogger::Log("[wpdamage] selfcheck PASS, scanned buffer == back buffer for ");
        SerialLogger::LogDec(g_sc_frames_total);
        SerialLogger::Log(" frames\r\n");
    }
}

static volatile int g_op_kind = 0, g_op_a = 0, g_op_b = 0;
void Graphics::RequestOutputOp(int op, int a, int b) {
    g_op_a = a; g_op_b = b; g_op_kind = op; MarkUIDirty();
}
static bool g_reloc_valid = false;
static bool g_cfg_on[OUTPUT_MAX];
static int  g_cfg_ax[OUTPUT_MAX], g_cfg_ay[OUTPUT_MAX];
void Graphics::RequestOutputConfig(int primary, const bool* on, const int* ax, const int* ay) {
    for (int i = 0; i < OUTPUT_MAX; i++) { g_cfg_on[i] = on[i]; g_cfg_ax[i] = ax[i]; g_cfg_ay[i] = ay[i]; }
    RequestOutputOp(7, primary, 0);
}
static int  g_reloc_x[OUTPUT_MAX], g_reloc_y[OUTPUT_MAX], g_reloc_sa = -1, g_reloc_sb = -1;
bool Graphics::TakeOutputReloc(int* old_x, int* old_y, int* swap_a, int* swap_b) {
    if (!g_reloc_valid) return false;
    g_reloc_valid = false;
    for (int i = 0; i < OUTPUT_MAX; i++) { old_x[i] = g_reloc_x[i]; old_y[i] = g_reloc_y[i]; }
    *swap_a = g_reloc_sa; *swap_b = g_reloc_sb;
    return true;
}
void Graphics::ApplyPendingOutputOp() {
    int op = g_op_kind;
    if (!op) return;
    g_op_kind = 0;
    int a = g_op_a, b = g_op_b;
    if (!g_multi) return;
    for (int i = 0; i < OUTPUT_MAX; i++) {
        const Output* o = Outputs::Get(i);
        g_reloc_x[i] = o ? o->x : 0;
        g_reloc_y[i] = o ? o->y : 0;
    }
    g_reloc_sa = -1; g_reloc_sb = -1;
    bool ok = false;
    switch (op) {
        case 1: ok = Outputs::SetConnected(a, true); break;
        case 2: ok = Outputs::SetConnected(a, false); break;
        case 3: ok = Outputs::MakePrimary(a); if (ok) { g_reloc_sa = 0; g_reloc_sb = a; } break;
        case 4: Outputs::Arrange(nullptr, nullptr); Outputs::Layout(false); ok = true; break;
        case 5: Outputs::Arrange(nullptr, nullptr); Outputs::Layout(true); ok = true; break;
        case 6: ok = Outputs::Swap(a, b); if (ok) { g_reloc_sa = a; g_reloc_sb = b; } break;
        case 7: {
            // turn on what must be on, move the primary into slot 0 (the arrays
            // follow the swap), turn off the rest, then place everything. (satoru)
            bool on[OUTPUT_MAX]; int ax[OUTPUT_MAX], ay[OUTPUT_MAX];
            for (int i = 0; i < OUTPUT_MAX; i++) { on[i] = g_cfg_on[i]; ax[i] = g_cfg_ax[i]; ay[i] = g_cfg_ay[i]; }
            int n = Outputs::Total();
            if (a < 0 || a >= n || !on[a]) break;
            for (int i = 1; i < n; i++) if (on[i] && !Outputs::IsLive(i)) Outputs::SetConnected(i, true);
            if (a != 0 && Outputs::MakePrimary(a)) {
                g_reloc_sa = 0; g_reloc_sb = a;
                bool tb = on[0]; on[0] = on[a]; on[a] = tb;
                int tx = ax[0]; ax[0] = ax[a]; ax[a] = tx;
                int ty = ay[0]; ay[0] = ay[a]; ay[a] = ty;
            }
            for (int i = 1; i < n; i++) if (!on[i] && Outputs::IsLive(i)) Outputs::SetConnected(i, false);
            Outputs::Arrange(ax, ay);
            ok = true;
            break;
        }
    }
    if (!ok) { SerialLogger::Log("[outputs] op rejected\r\n"); return; }
    // a switched-off output is no longer scanned into: blank it so it does not
    // freeze on the last frame (the hardware mode itself stays as set). (satoru)
    for (int i = 0; i < Outputs::Total(); i++) {
        const Output* o = Outputs::Get(i);
        if (o && o->present && !o->connected && o->fb)
            memset(o->fb, 0, (size_t)o->pitch * (size_t)o->ph);
    }
    if (RelayoutOutputs()) g_reloc_valid = true;
    SerialLogger::Log("[outputs] op applied, outputs="); SerialLogger::LogDec(Outputs::Connected());
    SerialLogger::Log("\r\n");
}

static volatile int g_pending_scale = 0;
void Graphics::RequestUIScale(int pct) { g_pending_scale = pct; MarkUIDirty(); }
void Graphics::ApplyPendingUIScale() {
    int p = g_pending_scale;
    if (!p) return;
    g_pending_scale = 0;
    SetUIScale(p);
}

bool Graphics::SetUIScale(int pct) {
    if (pct < 100) pct = 100;
    if (pct > 300) pct = 300;
    if (pct == g_scale) return true;
    ZcSuspendIfActive();   // the logical/physical geometry changes (satoru)
    if (pct != 100) {
        if (fb_bpp != 32 || g_present_via_virtio) {
            SerialLogger::Log("Graphics: ui scale needs a 32 bpp non-virtio framebuffer\r\n");
            return false;
        }
        if (render_mode != DOUBLE_BUFFER && render_mode != TRIPLE_BUFFER) {
            SerialLogger::Log("Graphics: ui scale needs double buffering\r\n");
            return false;
        }
    }
    RenderMode rm = render_mode;
    g_scale = pct;
    if (g_multi) {
        Outputs::SetScale(pct);
        Outputs::Layout(Outputs::Vertical());
        RelayoutOutputs();
    } else {
        if (back_buffer) {
            TaskMem::UnchargeFrames(back_buffer); PMM::FreeBytes(back_buffer, back_buffer_size);
            back_buffer = 0;
            back_buffer_size = 0;
        }
        if (pct == 100) {
            fb_width = g_ph_w; fb_height = g_ph_h; fb_pitch = g_ph_pitch;
        } else {
            fb_width  = (uint32_t)((uint64_t)g_ph_w * 100 / (uint32_t)pct);
            fb_height = (uint32_t)((uint64_t)g_ph_h * 100 / (uint32_t)pct);
            fb_pitch  = fb_width * 4u;
        }
        active_buffer = fb_addr;
        if (rm == DOUBLE_BUFFER || rm == TRIPLE_BUFFER) SetRenderMode(rm);
        MarkFullScreenDirty();
        MarkUIDirty();
    }
    g_multi_relayout = true;   // soft relayout: windows stay open, desktop re-lays out (satoru)
    SerialLogger::Log("Graphics: ui scale "); SerialLogger::LogDec(pct);
    SerialLogger::Log("% logical "); SerialLogger::LogDec((int)fb_width);
    SerialLogger::Log("x"); SerialLogger::LogDec((int)fb_height);
    SerialLogger::Log(" physical "); SerialLogger::LogDec((int)g_ph_w);
    SerialLogger::Log("x"); SerialLogger::LogDec((int)g_ph_h);
    SerialLogger::Log("\r\n");
    return true;
}

// (re)size the virtual desktop to the current Outputs layout. allocates a fresh
// ram front buffer and drops the back buffer so SetRenderMode re-creates it at
// the new pitch. caller holds g_fb_lock once the gui loop is running. (satoru)
bool Graphics::RelayoutOutputs() {
    // the virtual desktop buffer is about to be replaced: restore the plane
    // surfaces that scan it first. (satoru)
    ZcSuspendIfActive();
    int vw = 0, vh = 0;
    Outputs::Bounds(&vw, &vh);
    if (vw <= 0 || vh <= 0) return false;
    size_t bytes = (size_t)vw * 4u * (size_t)vh;
    uint8_t* nf = (uint8_t*)PMM::AllocBytes(bytes); TaskMem::ChargeFrames(nf, bytes);
    if (!nf) {
        SerialLogger::Log("Graphics: multi-output virtual desktop alloc FAILED\r\n");
        return false;
    }
    memset(nf, 0, bytes);
    RenderMode rm = render_mode;
    if (back_buffer) {
        TaskMem::UnchargeFrames(back_buffer); PMM::FreeBytes(back_buffer, back_buffer_size);
        back_buffer = 0;
        back_buffer_size = 0;
    }
    uint8_t* old_front = g_virt_front;
    size_t old_bytes = g_virt_bytes;
    g_virt_front = nf;
    g_virt_bytes = bytes;
    fb_addr = nf;
    fb_width = (uint32_t)vw;
    fb_height = (uint32_t)vh;
    fb_pitch = (uint32_t)vw * 4u;
    fb_bpp = 32;
    active_buffer = fb_addr;
    if (old_front) { TaskMem::UnchargeFrames(old_front); PMM::FreeBytes(old_front, old_bytes); }
    if (rm == DOUBLE_BUFFER || rm == TRIPLE_BUFFER) SetRenderMode(rm);
    MarkFullScreenDirty();
    MarkUIDirty();
    SerialLogger::Log("Graphics: virtual desktop ");
    SerialLogger::LogDec(vw); SerialLogger::Log("x"); SerialLogger::LogDec(vh);
    SerialLogger::Log("\r\n");
    if (g_multi) g_multi_relayout = true;
    return true;
}

// switch from the single boot framebuffer to the multi-output virtual desktop.
// requires 2+ live outputs and the non-virtio present path. call before the
// desktop is laid out (it changes GetWidth/GetHeight). (satoru)
bool Graphics::EnableMultiOutput() {
    if (g_multi) return true;
    if (!Outputs::Multi() || g_present_via_virtio) return false;
    for (int i = 1; i < Outputs::Total(); i++) {
        const Output* o = Outputs::Get(i);
        if (o && o->present && o->connected)
            remap_fb_writecombining((uintptr_t)o->phys, (size_t)o->pitch * (size_t)o->h);
    }
    Outputs::Layout(false);
    if (!RelayoutOutputs()) return false;
    g_multi = true;
    SerialLogger::Log("Graphics: multi-output ENABLED, outputs=");
    SerialLogger::LogDec(Outputs::Connected());
    SerialLogger::Log("\r\n");
    return true;
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
    // a render-mode change reallocates the back buffer: leave zero-copy first
    // so the scanned target and the cpu target never disagree. (satoru)
    ZcSuspendIfActive();
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
            TaskMem::UnchargeFrames(back_buffer); PMM::FreeBytes(back_buffer, back_buffer_size);
            back_buffer = 0;
            back_buffer_size = 0;
        }
        back_buffer = (uint8_t*)PMM::AllocBytes(buffer_size); TaskMem::ChargeFrames(back_buffer, buffer_size);
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
            TaskMem::UnchargeFrames(back_buffer); PMM::FreeBytes(back_buffer, back_buffer_size);
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

void Graphics::SetVirtioPresent(bool on) {
    g_present_via_virtio = on;
    if (on && g_scale != 100) SetUIScale(100);   // the host resource is read at physical size (satoru)
}

// damage bounds of the frame SwapBuffers just wrote, kept for the virtio
// present that follows it. SwapBuffers already computes a dirty-rect list and
// only copies those rows, then calls ClearDirtyRegions() - so by the time
// PresentVirtioIfActive runs the damage is gone and the host transfer had no
// choice but to re-upload the whole screen (8.3mb at 1080p) for even a cursor
// move. capture the union box before it is cleared and hand it to the transfer,
// which already takes x/y/w/h. a full-frame swap sets the box to the screen, so
// the worst case is exactly the old behaviour. (satoru)
static uint32_t g_pres_x = 0, g_pres_y = 0, g_pres_w = 0, g_pres_h = 0;
static bool g_force_full_present = false;
void Graphics::SetForceFullPresent(bool on) { g_force_full_present = on; }
void Graphics::SetPresentDamage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    g_pres_x = x; g_pres_y = y; g_pres_w = w; g_pres_h = h;
}

// push the just-swapped frame to the host gpu. called by the gui loop right
// after SwapBuffers when the virtio-gpu backend is active; a no-op otherwise so
// the bga/std-vga present path is unchanged. PresentFramebuffer pipelines the
// transfer + flush into the virtqueue (one notify per frame). (satoru)
void Graphics::PresentVirtioIfActive() {
    if (!g_present_via_virtio || !fb_addr) return;
    // upload only the damaged box. a zero/unset box means "whole screen" so any
    // path that swaps without recording damage still behaves as before. (satoru)
    uint32_t dx = g_pres_x, dy = g_pres_y, dw = g_pres_w, dh = g_pres_h;
    if (g_force_full_present) { dx = 0; dy = 0; dw = fb_width; dh = fb_height; }
    if (dw == 0 || dh == 0 || dx >= fb_width || dy >= fb_height) {
        dx = 0; dy = 0; dw = fb_width; dh = fb_height;
    }
    if (dx + dw > fb_width)  dw = fb_width  - dx;
    if (dy + dh > fb_height) dh = fb_height - dy;
    VirtIOGPU::PresentFramebufferRect((void*)fb_addr, fb_width, fb_height,
                                      dx, dy, dw, dh);
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
        SerialLogger::Log("Graphics: No VGA VSync (EFI?) - defaulting to 60 Hz\r\n");
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
    else g_refresh_measured = true;   // a real vga vsync measurement (satoru)
    monitor_hz = hz;
    return hz;
}

bool Graphics::RefreshWasMeasured() { return g_refresh_measured; }

bool Graphics::ShouldRender() {
    uint64_t current_time = TimeManager::NowUTC().us;
    if (current_time < last_frame_time) return true;  // clock skew - render
    return (current_time - last_frame_time) >= (uint64_t)target_frame_time_us;
}

void Graphics::WaitForVSync() {
    DisplayController::WaitVSync();
}

static inline int out_dim(int out) {
    if (out < 0 || out >= OUTPUT_MAX) out = 0;
    int d = g_out_brightness[out];
    return d < 10 ? 10 : (d > 100 ? 100 : d);
}

// scorer: the accessibility colour transform (no dimming). (satoru)
static inline uint32_t apply_color_filter(uint32_t c) {
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
    if (nr < 0) nr = 0; else if (nr > 255) nr = 255;
    if (ng < 0) ng = 0; else if (ng > 255) ng = 255;
    if (nb < 0) nb = 0; else if (nb > 255) nb = 255;
    return ((c >> 24) & 0xFF) << 24 | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
}

// software panel dim + warm night-light tint, applied on the way to an output. (satoru)
static inline uint32_t apply_tone(uint32_t c, int dim, int night) {
    if (dim >= 100 && night <= 0) return c;
    uint32_t a = (c >> 24) & 0xFF;
    int nr = (c >> 16) & 0xFF;
    int ng = (c >> 8)  & 0xFF;
    int nb = c & 0xFF;
    if (dim < 100) {
        nr = nr * dim / 100;
        ng = ng * dim / 100;
        nb = nb * dim / 100;
    }
    if (night > 0) {
        if (night > 100) night = 100;
        ng = ng * (100 - night / 5) / 100;
        nb = nb * (100 - (night * 2) / 5) / 100;
    }
    if (nr < 0) nr = 0; else if (nr > 255) nr = 255;
    if (ng < 0) ng = 0; else if (ng > 255) ng = 255;
    if (nb < 0) nb = 0; else if (nb > 255) nb = 255;
    return (a << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
}

// single output: accessibility filter plus the primary panel's own dim/night. (satoru)
static inline uint32_t apply_a11y_filter(uint32_t c) {
    return apply_tone(apply_color_filter(c), out_dim(0), g_night_light);
}

// true when the single-output per-pixel present path has any work to do. (satoru)
static inline bool tone_active() {
    return g_color_filter > 0 || g_high_contrast || g_night_light > 0 || out_dim(0) < 100;
}

// same test across every output (multi-monitor present paths). (satoru)
static inline bool any_tone() {
    if (g_color_filter > 0 || g_high_contrast || g_night_light > 0) return true;
    for (int i = 0; i < OUTPUT_MAX; i++) if (out_dim(i) < 100) return true;
    return false;
}

// internal: copy one row of dirty pixels, applying the a11y filter if active.
// uses 16-pixel SSE-friendly inner loop with NT stores.
static void blit_filtered_row(uint8_t* dst_row, const uint8_t* src_row, int x, int w) {
    uint32_t* d = (uint32_t*)dst_row + x;
    const uint32_t* s = (const uint32_t*)src_row + x;
    // in multi output the per-output dim/night pass happens in scatter_rect, so
    // this row copy carries only the accessibility colour transform. (satoru)
    int dim = g_multi ? 100 : out_dim(0);
    int night = g_multi ? 0 : g_night_light;
    int i = 0;
    for (; i + 4 <= w; i += 4) {
        uint32_t a = apply_tone(apply_color_filter(s[i+0]), dim, night);
        uint32_t b = apply_tone(apply_color_filter(s[i+1]), dim, night);
        uint32_t c = apply_tone(apply_color_filter(s[i+2]), dim, night);
        uint32_t e = apply_tone(apply_color_filter(s[i+3]), dim, night);
        d[i+0] = a; d[i+1] = b; d[i+2] = c; d[i+3] = e;
    }
    for (; i < w; i++) d[i] = apply_tone(apply_color_filter(s[i]), dim, night);
}

// multi-monitor: copy one rect of the virtual desktop (src, src_pitch bytes per
// row) to every live output it overlaps, clipped per output. an output the rect
// does not touch costs nothing - that is the per-output damage. rgb-ordered
// framebuffers take a slow channel-swap path. (satoru)
static void scatter_rect(const uint8_t* src, uint32_t src_pitch, int rx, int ry, int rw, int rh) {
    for (int i = 0; i < Outputs::Total(); i++) {
        const Output* o = Outputs::Get(i);
        if (!o || !o->present || !o->connected || !o->fb) continue;
        int x0 = rx > o->x ? rx : o->x;
        int y0 = ry > o->y ? ry : o->y;
        int x1 = (rx + rw) < (o->x + o->w) ? (rx + rw) : (o->x + o->w);
        int y1 = (ry + rh) < (o->y + o->h) ? (ry + rh) : (o->y + o->h);
        if (x1 <= x0 || y1 <= y0) continue;
        size_t row_bytes = (size_t)(x1 - x0) * 4;
        int dim = out_dim(i);
        bool tone = (dim < 100 || g_night_light > 0);
        for (int y = y0; y < y1; y++) {
            uint8_t* d = o->fb + (size_t)(y - o->y) * o->pitch + (size_t)(x0 - o->x) * 4;
            const uint8_t* s = src + (size_t)y * src_pitch + (size_t)x0 * 4;
            if (!tone && !o->swap_rb) {
                fb_copy_nt(d, s, row_bytes);
            } else {
                uint32_t* dd = (uint32_t*)d;
                const uint32_t* ss = (const uint32_t*)s;
                for (int k = 0; k < x1 - x0; k++) {
                    uint32_t p = ss[k];
                    if (tone) p = apply_tone(p, dim, g_night_light);
                    dd[k] = o->swap_rb ? ((p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF)) : p;
                }
            }
        }
    }
}

// ---- ui scale present: resample the logical back buffer to the hardware ----
// one hardware target: its logical rect (lx..) maps to pw x ph physical pixels.
struct ScTarget { uint8_t* fb; uint32_t pitch; int lx, ly, lw, lh, pw, ph; bool swap; };
#define SC_MAXW 8192
static uint16_t sc_xi[SC_MAXW];
static uint8_t  sc_xf[SC_MAXW];
static uint32_t sc_row[SC_MAXW];

static inline uint32_t sc_lerp(uint32_t a, uint32_t b, uint32_t f) {
    uint32_t rb = ((a & 0x00FF00FFu) * (256u - f) + (b & 0x00FF00FFu) * f) >> 8;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * (256u - f) + ((b >> 8) & 0x00FF00FFu) * f);
    return (rb & 0x00FF00FFu) | (ag & 0xFF00FF00u);
}

// whole-percent scales (100/200/300) replicate pixels (crisp); fractional ones
// (125/150) use a bilinear filter so edges stay even. integer math only. (satoru)
static void sc_rect_to(const ScTarget& t, const uint8_t* src, uint32_t sp,
                       int rx, int ry, int rw, int rh, bool filt, int dim, int night) {
    const int s = g_scale;
    int x0 = rx - 1 > t.lx ? rx - 1 : t.lx;
    int y0 = ry - 1 > t.ly ? ry - 1 : t.ly;
    int x1 = rx + rw + 1 < t.lx + t.lw ? rx + rw + 1 : t.lx + t.lw;
    int y1 = ry + rh + 1 < t.ly + t.lh ? ry + rh + 1 : t.ly + t.lh;
    if (x1 <= x0 || y1 <= y0) return;
    int dx0 = ((x0 - t.lx) * s) / 100;
    int dx1 = ((x1 - t.lx) * s + 99) / 100; if (dx1 > t.pw) dx1 = t.pw;
    int dy0 = ((y0 - t.ly) * s) / 100;
    int dy1 = ((y1 - t.ly) * s + 99) / 100; if (dy1 > t.ph) dy1 = t.ph;
    int n = dx1 - dx0;
    if (n <= 0 || dy1 <= dy0 || n > SC_MAXW) return;
    const bool nearest = (s % 100) == 0;
    for (int i = 0; i < n; i++) {
        int dx = dx0 + i;
        if (nearest) {
            int si = (dx * 100) / s; if (si >= t.lw) si = t.lw - 1;
            sc_xi[i] = (uint16_t)si; sc_xf[i] = 0;
        } else {
            int fp = (int)(((long long)(2 * dx + 1) * 12800) / s) - 128;
            if (fp < 0) fp = 0;
            int si = fp >> 8, f = fp & 255;
            if (si >= t.lw - 1) { si = t.lw - 1; f = 0; }
            sc_xi[i] = (uint16_t)si; sc_xf[i] = (uint8_t)f;
        }
    }
    int last_sy = -1;
    for (int dy = dy0; dy < dy1; dy++) {
        int sy, yf = 0;
        if (nearest) {
            sy = (dy * 100) / s; if (sy >= t.lh) sy = t.lh - 1;
        } else {
            int fp = (int)(((long long)(2 * dy + 1) * 12800) / s) - 128;
            if (fp < 0) fp = 0;
            sy = fp >> 8; yf = fp & 255;
            if (sy >= t.lh - 1) { sy = t.lh - 1; yf = 0; }
        }
        if (!(nearest && sy == last_sy)) {
            const uint32_t* r0 = (const uint32_t*)(src + (size_t)(t.ly + sy) * sp) + t.lx;
            const uint32_t* r1 = yf ? (const uint32_t*)(src + (size_t)(t.ly + sy + 1) * sp) + t.lx : r0;
            for (int i = 0; i < n; i++) {
                int xi = sc_xi[i]; uint32_t f = sc_xf[i];
                uint32_t a = r0[xi];
                if (f) a = sc_lerp(a, r0[xi + 1], f);
                if (yf) {
                    uint32_t c = r1[xi];
                    if (f) c = sc_lerp(c, r1[xi + 1], f);
                    a = sc_lerp(a, c, (uint32_t)yf);
                }
                if (filt) a = apply_color_filter(a);
                a = apply_tone(a, dim, night);
                if (t.swap) a = (a & 0xFF00FF00u) | ((a & 0xFF) << 16) | ((a >> 16) & 0xFF);
                sc_row[i] = a;
            }
            last_sy = sy;
        }
        fb_copy_nt(t.fb + (size_t)dy * t.pitch + (size_t)dx0 * 4, sc_row, (size_t)n * 4);
    }
}

// logical rect -> every hardware target it touches (one target on a single screen).
static void sc_present_rect(const uint8_t* src, uint32_t sp, int rx, int ry, int rw, int rh,
                            bool filt, int lw, int lh, uint8_t* hwfb) {
    if (g_multi) {
        for (int i = 0; i < Outputs::Total(); i++) {
            const Output* o = Outputs::Get(i);
            if (!o || !o->present || !o->connected || !o->fb) continue;
            ScTarget t = { o->fb, o->pitch, o->x, o->y, o->w, o->h, o->pw, o->ph, o->swap_rb };
            sc_rect_to(t, src, sp, rx, ry, rw, rh, filt, out_dim(i), g_night_light);
        }
    } else {
        ScTarget t = { hwfb, g_ph_pitch, 0, 0, lw, lh, (int)g_ph_w, (int)g_ph_h, false };
        sc_rect_to(t, src, sp, rx, ry, rw, rh, filt, out_dim(0), g_night_light);
    }
}

// ---- scaled present: only resample what actually changed ----
// a scaled full-frame resample costs ~12ms at 2560x1600/125% (bilinear, no gpu)
// and frames with no usable damage list (a bare MarkUIDirty, the 250ms fallback)
// used to pay it every time even when nothing moved. keep a shadow copy of the
// logical frame that was last resampled; compare row spans against it and
// resample only the rows that really differ. exact: a changed pixel always
// reaches the hardware, an unchanged one is skipped. (satoru)
typedef uint64_t __attribute__((may_alias, aligned(4))) sc_u64a;
static uint8_t* g_sc_shadow = nullptr;
static size_t   g_sc_shadow_bytes = 0;
static bool     g_sc_shadow_ok = false;      // shadow == what the hardware shows
static uint8_t* g_sc_key_bb = nullptr;
static uint8_t* g_sc_key_fb = nullptr;
static uint32_t g_sc_key_w = 0, g_sc_key_h = 0;
static int      g_sc_key_scale = 0;
static bool     g_sc_key_filt = false, g_sc_key_multi = false;
static uint32_t g_sc_last_full_ms = 0;

static void sc_present_diff(const uint8_t* src, uint32_t sp, int rx, int ry, int rw, int rh,
                            bool filt, int lw, int lh, uint8_t* hwfb) {
    if (rw <= 0 || rh <= 0) return;
    int run_y = -1, run_x0 = 0, run_x1 = 0;
    for (int y = ry; y <= ry + rh; y++) {
        int fx0 = -1, fx1 = -1;
        if (y < ry + rh) {
            const uint32_t* a = (const uint32_t*)(src + (size_t)y * sp) + rx;
            uint32_t* b = (uint32_t*)(g_sc_shadow + (size_t)y * sp) + rx;
            int i = 0;
            while (i + 2 <= rw && *(const sc_u64a*)(a + i) == *(const sc_u64a*)(b + i)) i += 2;
            while (i < rw && a[i] == b[i]) i++;
            if (i < rw) {
                int j = rw - 1;
                while (j - 1 > i && *(const sc_u64a*)(a + j - 1) == *(const sc_u64a*)(b + j - 1)) j -= 2;
                while (j > i && a[j] == b[j]) j--;
                for (int k = i; k <= j; k++) b[k] = a[k];
                fx0 = rx + i; fx1 = rx + j + 1;
            }
        }
        if (fx0 >= 0) {
            if (run_y < 0) { run_y = y; run_x0 = fx0; run_x1 = fx1; }
            else { if (fx0 < run_x0) run_x0 = fx0; if (fx1 > run_x1) run_x1 = fx1; }
        } else if (run_y >= 0) {
            sc_present_rect(src, sp, run_x0, run_y, run_x1 - run_x0, y - run_y, filt, lw, lh, hwfb);
            run_y = -1;
        }
    }
}

// present the frame. zero-copy publishes damage with clflush and skips the
// copy entirely; the normal path is the cpu copy below plus a crc self-check
// when wallpaper damage-only is under test. (satoru)
void Graphics::SwapBuffers() {
    if (g_zerocopy) {
        uint32_t fstate = (uint32_t)g_color_filter | ((uint32_t)g_high_contrast << 8) |
                          ((uint32_t)g_brightness << 16) | ((uint32_t)g_night_light << 24);
        if (fstate != g_zc_filter_state) {
            // the accessibility filter transforms pixels on the way to the
            // panel; once we rendered into the panel buffer there is no clean
            // source left to re-tonemap from, so drop zero-copy and let the
            // normal filtered present repaint the full frame. (satoru)
            SerialLogger::Log("Graphics: zero-copy off, accessibility filter changed\r\n");
            ZeroCopyLeave();
            SwapBuffersBody();
            SelfCheckWpTick();
            return;
        }
        Rect rr[DIRTY_REGION_CAP];
        int n = 0;
        for (int i = 0; i < dirty_count && n < DIRTY_REGION_CAP; i++) {
            if (!dirty_regions[i].active) continue;
            if (dirty_regions[i].w <= 0 || dirty_regions[i].h <= 0) continue;
            rr[n].x = dirty_regions[i].x;
            rr[n].y = dirty_regions[i].y;
            rr[n].w = dirty_regions[i].w;
            rr[n].h = dirty_regions[i].h;
            n++;
        }
        if (n > 0) {
            ZcFlushRects(rr, n);
            __asm__ __volatile__("mfence" ::: "memory");
        }
        // the crc self-check needs a cpu-visible reference of what the scanned
        // buffer should hold; mirror the same damage into the back buffer for
        // the checked frames only. (satoru)
        if (g_sc_kind == SELFCHECK_ZEROCOPY && g_sc_frames > 0) ZcMirrorRects(rr, n);
        if (g_zc_test_mode == 1) ZcCopyRects(rr, n);
        SetPresentDamage(0, 0, 0, 0);
        ClearDirtyRegions();
        if (g_zc_present) g_zc_present();
        if (g_sc_kind == SELFCHECK_ZEROCOPY && g_sc_frames > 0) ZcSelfCheckTick();
        return;
    }
    SwapBuffersBody();
    SelfCheckWpTick();
}

void Graphics::SwapBuffersBody() {
    if (render_mode == SINGLE_BUFFER) return;
    // the virtio present is ASYNC: it hands the frame to the host and returns
    // without waiting. this is the last safe moment to collect it - everything
    // below writes into fb_addr, which is the very buffer the host reads. the
    // compositor has just spent ~1.9ms building the frame, which is longer
    // than the ~1.13ms transfer, so this drain normally costs nothing. no-op
    // unless a present is actually outstanding. (satoru)
    if (g_present_via_virtio) VirtIOGPU::DrainPresent();
    if (!back_buffer || !fb_addr) return;

    // ui scale: resample the damaged rects of the logical back buffer to the
    // hardware. same damage rules as the 1:1 path (a rect list, or the whole frame
    // when it is empty / too big); the present box handed to virtio-less callers is
    // in physical pixels. (satoru)
    if (g_scale != 100) {
        const bool sfilt = any_tone();
        const int lw = (int)fb_width, lh = (int)fb_height;
        int bx0 = lw, by0 = lh, bx1 = 0, by1 = 0;
        // shadow bookkeeping: any geometry / filter change, or 4s since the last
        // unconditional resample, invalidates it and forces one full present. (satoru)
        {
            uint32_t nowms = Timer::GetRealMs();
            size_t need = (size_t)fb_pitch * (size_t)lh;
            bool keychg = (back_buffer != g_sc_key_bb || fb_addr != g_sc_key_fb || (uint32_t)lw != g_sc_key_w ||
                           (uint32_t)lh != g_sc_key_h || g_scale != g_sc_key_scale || sfilt != g_sc_key_filt ||
                           g_multi != g_sc_key_multi);
            if (keychg || nowms - g_sc_last_full_ms > 4000u) g_sc_shadow_ok = false;
            if (keychg) {
                g_sc_key_bb = back_buffer; g_sc_key_fb = fb_addr; g_sc_key_w = (uint32_t)lw; g_sc_key_h = (uint32_t)lh;
                g_sc_key_scale = g_scale; g_sc_key_filt = sfilt; g_sc_key_multi = g_multi;
            }
            if (g_sc_shadow && g_sc_shadow_bytes < need) {
                TaskMem::UnchargeFrames(g_sc_shadow); PMM::FreeBytes(g_sc_shadow, g_sc_shadow_bytes); g_sc_shadow = nullptr; g_sc_shadow_bytes = 0;
                g_sc_shadow_ok = false;
            }
            if (!g_sc_shadow) {
                g_sc_shadow = (uint8_t*)PMM::AllocBytes(need); TaskMem::ChargeFrames(g_sc_shadow, need);
                if (g_sc_shadow) g_sc_shadow_bytes = need;
                g_sc_shadow_ok = false;
            }
        }
        const bool use_shadow = g_sc_shadow && g_sc_shadow_ok;
        bool partial = false;
        if (dirty_count > 0 && dirty_count <= DIRTY_REGION_CAP) {
            uint64_t px = 0;
            for (int i = 0; i < dirty_count; i++)
                if (dirty_regions[i].active && dirty_regions[i].w > 0 && dirty_regions[i].h > 0)
                    px += (uint64_t)dirty_regions[i].w * (uint64_t)dirty_regions[i].h;
            partial = (px > 0) && (g_multi || px < ((uint64_t)lw * (uint64_t)lh * 3 / 5));
        }
        // without a valid shadow only a full present can make it valid (satoru)
        if (g_sc_shadow && !use_shadow) partial = false;
        if (partial) {
            for (int i = 0; i < dirty_count; i++) {
                if (!dirty_regions[i].active) continue;
                int rx = dirty_regions[i].x, ry = dirty_regions[i].y;
                int rw = dirty_regions[i].w, rh = dirty_regions[i].h;
                if (rx < 0) { rw += rx; rx = 0; }
                if (ry < 0) { rh += ry; ry = 0; }
                if (rx >= lw || ry >= lh) continue;
                if (rx + rw > lw) rw = lw - rx;
                if (ry + rh > lh) rh = lh - ry;
                if (rw <= 0 || rh <= 0) continue;
                if (rx < bx0) bx0 = rx;
                if (ry < by0) by0 = ry;
                if (rx + rw > bx1) bx1 = rx + rw;
                if (ry + rh > by1) by1 = ry + rh;
                if (use_shadow) sc_present_diff(back_buffer, fb_pitch, rx, ry, rw, rh, sfilt, lw, lh, fb_addr);
                else sc_present_rect(back_buffer, fb_pitch, rx, ry, rw, rh, sfilt, lw, lh, fb_addr);
            }
        } else {
            bx0 = 0; by0 = 0; bx1 = lw; by1 = lh;
            if (use_shadow) {
                sc_present_diff(back_buffer, fb_pitch, 0, 0, lw, lh, sfilt, lw, lh, fb_addr);
            } else {
                sc_present_rect(back_buffer, fb_pitch, 0, 0, lw, lh, sfilt, lw, lh, fb_addr);
                if (g_sc_shadow) {
                    for (int y = 0; y < lh; y++)
                        memcpy(g_sc_shadow + (size_t)y * fb_pitch, back_buffer + (size_t)y * fb_pitch, (size_t)lw * 4u);
                    g_sc_shadow_ok = true;
                    g_sc_last_full_ms = Timer::GetRealMs();
                }
            }
        }
        __asm__ __volatile__("sfence" ::: "memory");
        if (bx1 > bx0 && by1 > by0) {
            uint32_t px0 = (uint32_t)(bx0 * g_scale / 100), py0 = (uint32_t)(by0 * g_scale / 100);
            uint32_t px1 = (uint32_t)((bx1 * g_scale + 99) / 100), py1 = (uint32_t)((by1 * g_scale + 99) / 100);
            SetPresentDamage(px0, py0, px1 - px0, py1 - py0);
        } else {
            SetPresentDamage(0, 0, g_ph_w, g_ph_h);
        }
        ClearDirtyRegions();
        return;
    }

    const uint32_t bytes_per_pixel = fb_bpp / 8;
    if (bytes_per_pixel == 0) return;
    const uint32_t bytes_per_line = fb_width * bytes_per_pixel;
    const bool pitch_match = (bytes_per_line == fb_pitch);
    const bool filter_active = (any_tone() && bytes_per_pixel == 4);

    // accessibility path: per-pixel transform. We still honour the dirty
    // region list so a static screen with a moving cursor isn't a full
    // re-tonemap each frame.
    if (filter_active) {
        bool any_dirty = false;
        for (int i = 0; i < dirty_count && i < DIRTY_REGION_CAP; i++) {
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
        if (g_multi) scatter_rect(fb_addr, fb_pitch, 0, 0, (int)fb_width, (int)fb_height);
        __asm__ __volatile__("sfence" ::: "memory");
        // this path has its own dirty handling and returns early. it MUST still
        // publish a damage box or the virtio present would reuse the PREVIOUS
        // frame's box and upload the wrong region. take the whole screen: the
        // accessibility tone-map is the rare path, and correctness beats the
        // upload saving here. (satoru)
        SetPresentDamage(0, 0, fb_width, fb_height);
        ClearDirtyRegions();
        return;
    }

    // partial swap when dirty area is small enough; otherwise full-frame.
    // 60 % threshold trades per-rect copy overhead vs cache locality of a
    // single contiguous blit.
    if (dirty_count > 0 && dirty_count <= DIRTY_REGION_CAP) {
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

        // multi-monitor always takes the per-rect path: a full-frame copy would
        // push the idle screen too, which is exactly what per-output damage avoids. (satoru)
        if (active_rects > 0 && (g_multi || dirty_pixels < (total_pixels * 3 / 5))) {
            // union box of everything we actually copy, for the virtio present
            // that follows (see SetPresentDamage). accumulated from the CLIPPED
            // rects so it can never exceed the screen. (satoru)
            int bx0 = (int)fb_width, by0 = (int)fb_height, bx1 = 0, by1 = 0;
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
                if (rx < bx0) bx0 = rx;
                if (ry < by0) by0 = ry;
                if (rx + rw > bx1) bx1 = rx + rw;
                if (ry + rh > by1) by1 = ry + rh;

                size_t region_bytes = (size_t)rw * bytes_per_pixel;
                size_t col_off = (size_t)rx * bytes_per_pixel;
                for (int y = ry; y < ry + rh; y++) {
                    size_t row_off = (size_t)y * fb_pitch + col_off;
                    fb_copy_nt(fb_addr + row_off, back_buffer + row_off, region_bytes);
                }
                if (g_multi) scatter_rect(fb_addr, fb_pitch, rx, ry, rw, rh);
            }
            __asm__ __volatile__("sfence" ::: "memory");
            if (bx1 > bx0 && by1 > by0)
                SetPresentDamage((uint32_t)bx0, (uint32_t)by0,
                                 (uint32_t)(bx1 - bx0), (uint32_t)(by1 - by0));
            else
                SetPresentDamage(0, 0, fb_width, fb_height);
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
    if (g_multi) scatter_rect(fb_addr, fb_pitch, 0, 0, (int)fb_width, (int)fb_height);

    __asm__ __volatile__("sfence" ::: "memory");
    // full-frame swap: the host must re-read everything, same as before. (satoru)
    SetPresentDamage(0, 0, fb_width, fb_height);
    ClearDirtyRegions();
}

// Atomic present of an explicit damage list. Each rect is clipped to the
// framebuffer, then blitted with non-temporal stores. A NULL rect list or
// count<=0 forces a full-frame present.
void Graphics::Present(const Rect* rects, int count) {
    if (render_mode == SINGLE_BUFFER || !back_buffer || !fb_addr) return;
    if (g_zerocopy) {
        ZcFlushRects(rects, count);
        __asm__ __volatile__("mfence" ::: "memory");
        if (g_sc_kind == SELFCHECK_ZEROCOPY && g_sc_frames > 0) ZcMirrorRects(rects, count);
        if (g_zc_test_mode == 1) ZcCopyRects(rects, count);
        ClearDirtyRegions();
        if (g_zc_present) g_zc_present();
        if (g_sc_kind == SELFCHECK_ZEROCOPY && g_sc_frames > 0) ZcSelfCheckTick();
        return;
    }
    const uint32_t bytes_per_pixel = fb_bpp / 8;
    if (bytes_per_pixel == 0) return;
    const bool filter_active = (any_tone() && bytes_per_pixel == 4);

    if (!rects || count <= 0) {
        // hand off to SwapBuffers full-frame path
        ClearDirtyRegions();
        SwapBuffers();
        return;
    }

    if (count > 32) count = 32;
    if (g_scale != 100) {
        const bool sfilt = any_tone();
        for (int i = 0; i < count; i++) {
            int rx = rects[i].x, ry = rects[i].y, rw = rects[i].w, rh = rects[i].h;
            if (rx < 0) { rw += rx; rx = 0; }
            if (ry < 0) { rh += ry; ry = 0; }
            if (rx >= (int)fb_width || ry >= (int)fb_height) continue;
            if (rx + rw > (int)fb_width)  rw = (int)fb_width  - rx;
            if (ry + rh > (int)fb_height) rh = (int)fb_height - ry;
            if (rw <= 0 || rh <= 0) continue;
            sc_present_rect(back_buffer, fb_pitch, rx, ry, rw, rh, sfilt, (int)fb_width, (int)fb_height, fb_addr);
        }
        __asm__ __volatile__("sfence" ::: "memory");
        ClearDirtyRegions();
        return;
    }
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
        if (g_multi) scatter_rect(fb_addr, fb_pitch, rx, ry, rw, rh);
    }
    __asm__ __volatile__("sfence" ::: "memory");
    ClearDirtyRegions();
}

uint8_t* Graphics::GetActiveBuffer() {
    return active_buffer;
}

uint8_t* Graphics::GetBackBuffer() {
    // in zero-copy the scanned buffer is the drawing target, so callers that
    // write pixels directly (wallpaper, snapshots) must get it, not the unused
    // back buffer kept for rollback. (satoru)
    if (g_zerocopy) return active_buffer;
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
    if (alpha == 0) return;  // fully transparent - skip
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
    // bounds check - "Unsafe" historically meant unchecked vs clip rect, but
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

    // already collapsed to full-screen for this frame - nothing more to track. (satoru)
    if (full_screen_dirty) return;

    // fast path: there's still room, just record the rect. o(1) - the running
    // area sum is kept incrementally so small-rect frames stay cheap. (satoru)
    if (dirty_count < DIRTY_REGION_CAP) {
        DirtyRegion& region = dirty_regions[dirty_count++];
        region.x = x; region.y = y; region.w = w; region.h = h;
        region.active = true;

        // accumulate damaged area in 64-bit so it can never wrap; w/h are already
        // clipped to the fb above, so each term is non-negative. (satoru)
        dirty_area_total += (uint64_t)w * (uint64_t)h;

        // if accumulated damage already covers most of the screen, a per-rect
        // partial blit buys nothing over one contiguous copy - collapse to a
        // single full-screen damage now so the decision is deterministic, not an
        // accident of coalescing order. (satoru)
        uint64_t total = (uint64_t)fb_width * (uint64_t)fb_height;
        if (!g_multi && total > 0 && dirty_area_total >= (total * 3 / 4)) {
            MarkFullScreenDirty();
        }
        return;
    }

    // list is full. rather than fragile minimum-area coalescing (which can
    // silently grow one rect to near-fullscreen and trip the partial-swap
    // threshold anyway), deterministically collapse the whole frame to a single
    // full-screen damage rect. this is the cheap, predictable thing to do once
    // the screen is this fragmented. (satoru)
    MarkFullScreenDirty();
}

void Graphics::MarkFullScreenDirty() {
    // one rect covering the whole framebuffer; latches until ClearDirtyRegions. (satoru)
    for (int i = 0; i < DIRTY_REGION_CAP; i++) dirty_regions[i].active = false;
    dirty_regions[0].x = 0;
    dirty_regions[0].y = 0;
    dirty_regions[0].w = (int)fb_width;
    dirty_regions[0].h = (int)fb_height;
    dirty_regions[0].active = true;
    dirty_count = 1;
    full_screen_dirty = true;
}

int Graphics::GetDirtyRects(Rect* out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    // the previous frame's damage first: a damage-only wallpaper pass has to
    // repaint under what vanished since it was drawn (menu closed, toast
    // faded, animation frame). (satoru)
    for (int i = 0; i < dirty_prev_count && n < max; i++) {
        const DirtyRegion& d = dirty_prev[i];
        if (!d.active || d.w <= 0 || d.h <= 0) continue;
        out[n].x = d.x; out[n].y = d.y; out[n].w = d.w; out[n].h = d.h;
        n++;
    }
    for (int i = 0; i < dirty_count && n < max; i++) {
        if (!dirty_regions[i].active) continue;
        if (dirty_regions[i].w <= 0 || dirty_regions[i].h <= 0) continue;
        out[n].x = dirty_regions[i].x;
        out[n].y = dirty_regions[i].y;
        out[n].w = dirty_regions[i].w;
        out[n].h = dirty_regions[i].h;
        n++;
    }
    return n;
}

void Graphics::ClearDirtyRegions() {
    // remember this frame's damage for the next wallpaper pass. a full-screen
    // latch becomes one full rect. (satoru)
    dirty_prev_count = 0;
    for (int i = 0; i < dirty_count && dirty_prev_count < DIRTY_REGION_CAP; i++) {
        if (!dirty_regions[i].active) continue;
        if (dirty_regions[i].w <= 0 || dirty_regions[i].h <= 0) continue;
        dirty_prev[dirty_prev_count++] = dirty_regions[i];
    }
    dirty_count = 0;
    full_screen_dirty = false;
    dirty_area_total = 0;
    for (int i = 0; i < DIRTY_REGION_CAP; i++) {
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
    // a rect that lies fully outside the clip yields right<x / bottom<y here; clamp
    // so callers don't get a negative w/h that their unsigned compares read as a
    // huge extent and draw far past the clip. (satoru)
    if (w < 0) w = 0;
    if (h < 0) h = 0;
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
    // honour the active clip - without this, alpha-blended primitives (shadows,
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
    // fully opaque - hand off to the 64-bit fast fill path. (satoru)
    if (a == 255) { FillRect(x, y, w, h, color | 0xFF000000u); return; }

    // clamp once to framebuffer bounds instead of bounds-checking every
    // pixel, then blend inline. for a constant source colour the per-channel
    // src*alpha terms are loop invariants (cr/cg/cb), so the inner loop only
    // reads the destination, blends, and writes - no BlendPixel call, no
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
    for (int i = 0; i < OUTPUT_MAX; i++) g_out_brightness[i] = pct;
    g_sc_shadow_ok = false;
    MarkUIDirty();   // force a re-tonemap of the whole screen (satoru)
}
int  Graphics::GetBrightness(){ return g_out_brightness[0]; }
void Graphics::SetOutputBrightness(int output, int pct){
    if (output < 0 || output >= OUTPUT_MAX) return;
    if (pct < 10) pct = 10; else if (pct > 100) pct = 100;
    if (g_out_brightness[output] == pct) return;
    g_out_brightness[output] = pct;
    if (output == 0) g_brightness = pct;   // legacy single-output mirror (satoru)
    g_sc_shadow_ok = false;
    MarkUIDirty();
}
int  Graphics::GetOutputBrightness(int output){
    if (output < 0 || output >= OUTPUT_MAX) output = 0;
    return g_out_brightness[output];
}
void Graphics::SetNightLight(int pct){
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    if (g_night_light == pct) return;
    g_night_light = pct;
    g_sc_shadow_ok = false;
    MarkUIDirty();
}
int  Graphics::GetNightLight(){ return g_night_light; }

//  drawstring - convenience text renderer for desktop/app layers
//  font size scales with resolution: 16px at 1024x768, 16px at 1080p,
//  20px at 1440p for readable text at every density.
void Graphics::DrawString(int x, int y, const char* s, uint32_t fg, uint32_t /*bg*/) {
    if (!s) return;
    // scale font: base 16px for <=1080p, 20px for 1440p+
    float pxh = 16.0f;
    // multi-monitor: size text for the primary screen, not the virtual desktop
    // (a stacked layout made every string jump to 20 px). (satoru)
    uint32_t ref_h = fb_height;
    if (g_multi) { const Output* po = Outputs::Get(0); if (po && po->h > 0) ref_h = (uint32_t)po->h; }
    if (ref_h >= 1440) pxh = 20.0f;
    if (SettingsApp::state.font_scale > 1) {
        pxh *= (float)SettingsApp::state.font_scale;
    }
    // top-left line box like the pixel font: the modern style draws inter,
    // the classic style (FontTTF inactive) keeps the 8x16 pixel font. (satoru)
    FontTTF::DrawText(x, y, pxh, s, fg);
}

//  drawcircle - midpoint circle algorithm
void Graphics::DrawCircle(int cx, int cy, int radius, uint32_t color, bool filled) {
    if (radius <= 0) { DrawPixel(cx, cy, color); return; }

    int x = 0, y = radius;
    int d = 1 - radius;

    auto hline = [&](int lx, int rx, int ly) {
        if (ly < 0 || ly >= (int)fb_height) return;
        if (rx < lx) return;
        // FillRect-based span - hits the 64-bit bulk path on 32 bpp
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
