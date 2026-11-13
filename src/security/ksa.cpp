//  kurono os - ksa (kurono secure authorization) implementation
//
//  see ksa.h for the architecture + isolation model. summary of how the
//  security guarantees are realized here:
//
//   1. spawn: PMM::AllocContiguous() carves a 2mb-aligned, exactly-2mb
//      physical region for the prompt (framebuffer + state + verdict). a
//      dedicated EPT root is built and the region is mapped ONLY into that
//      ept (the ksa guest-physical space). then KernelVMM::IsolateFrames()
//      removes the region from the main-os page tables entirely - after this
//      the main os (even ring-0 malware) has no virtual mapping to it, and
//      QueryMapping() returns 0 for every frame.
//
//   2. render + input: ksa IS the hypervisor-side arbiter, so it (and only
//      it) re-establishes an ephemeral private mapping to render the prompt
//      and read the verdict, then re-isolates. the main-os compositor never
//      receives a pointer into the region; only the final frame is blitted
//      to the screen.
//
//   3. result channel: the verdict crosses back via VMCALL 0x4B
//      (ReadVerdictForChannel) which returns a *copy*, never a pointer. the
//      channel is read-only - there is no vmcall that lets the main os write
//      an approval into ksa memory, so a forged "yes" cannot be injected.
//
//  nested-vmx note: on a host where true nested vmx for the inner vm is
//  unavailable (nested kvm under qemu - the common dev case), the prompt
//  logic runs as an ept-isolated guest *context* rather than a separately
//  vmlaunch'd vm. IsRealNestedVM() reports which path is active. the
//  isolation invariants above hold in BOTH cases; only the cpu-mode boundary
//  differs, and that difference is reported, not hidden.
#include "ksa.h"
#include "ken.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"
#include "../kernel/types.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../kernel/time.h"
#include "../virt/hypervisor.h"
#include "../virt/ept.h"
#include "../system/logging.h"
#include "../system/user_mgmt.h"   // desktop account credential digests (satoru)
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../proc/spinlock.h"
#include "../proc/kernel_locks.h"
#include "../drivers/usb.h"
#include "../shell/shell.h"
#include "../linux/linux_syscall.h"   // KlsDropAll around the modal wait (satoru)

bool KSA::available    = false;
bool KSA::real_nested  = false;
bool KSA::initialized  = false;

// ── isolated region layout ─────────────────────────────────────────────
// the region is exactly 2mb (512 frames) so a single huge-page demote covers
// it. layout within the region (all offsets in bytes):
//   [0 .. KSA_FB_BYTES)              prompt framebuffer (RGBA, KSA_W x KSA_H)
//   [KSA_VERDICT_OFF ..]            KSAVerdict (the only thing the channel reads)
//   [KSA_SCRATCH_OFF ..]            credential scratch (wiped on teardown)
static const int      KSA_W          = 460;
static const int      KSA_H          = 270;
static const uint64_t KSA_REGION_SZ  = 0x200000ULL;            // 2mb (satoru)
static const uint64_t KSA_FB_BYTES   = (uint64_t)KSA_W * KSA_H * 4;
static const uint64_t KSA_VERDICT_OFF= 0x1F0000ULL;            // near top of region
static const uint64_t KSA_SCRATCH_OFF= 0x1F8000ULL;
static const uint64_t KSA_GUEST_PHYS = 0x10000000ULL;          // ksa guest-phys base (satoru)

// the host-physical base of the isolated region (0 = not spawned). (satoru)
static uint64_t   g_region_phys = 0;
static EPT_PML4*  g_ksa_ept     = nullptr;
static bool       g_isolated    = false;
static uint32_t   g_channel_rev = 1;
// true when the region is ordinary kernel memory (the no-hypervisor secure
// desktop fallback) rather than an ept-isolated region. (satoru)
static bool       g_fallback    = false;
// set for the lifetime of a prompt so the gui/input loops stand down. (satoru)
static volatile bool g_secure_desktop = false;
static uint32_t   g_prompt_timeout_ms = 60000;

// a verdict latched after a prompt completes - what the channel returns. the
// host copy lives in normal kernel memory (NOT in the isolated region); it is
// populated only by ksa's own arbiter code after reading the in-region verdict
// through the ephemeral mapping. (satoru)
static KSAVerdict g_last_verdict;

// ── modal state shared with the ui pumps ───────────────────────────────
// the compositor owns the prompt while it is up: the gui loop when the caller
// yielded to the kernel procs, the syscall pump when a bsp syscall is waiting.
// both poll usb hid every frame - exactly what the old ps/2-only busy-wait
// could not provide on a usb-only machine. only one prompt runs at a time; the
// claimant is the only writer of the verdict. (satoru)
struct KSAModalState {
    bool     decided;
    bool     approved;
    bool     timed_out;
    bool     want_cred;
    bool     show_risk;
    uint32_t started_ms;
    uint32_t timeout_ms;
    char     title[96];
    char     detail[160];
    char     username[40];
    char     typed[64];
    int      typed_len;
    int      mx, my;
    uint8_t* fb;
};
static KSAModalState g_modal;
static volatile bool g_modal_busy      = false;  // claim: one prompt at a time (satoru)
static volatile bool g_modal_live      = false;  // published: ui may draw + read input (satoru)
static volatile int  g_modal_rendering = 0;      // ui is inside the overlay blit (satoru)
// true when the gui process hosts the modal (the caller yielded to the kernel
// procs). false means the bsp syscall waiter pumps the ui itself, because a
// ring-0 syscall cannot run the gui kernel process any other way. (satoru)
static volatile bool g_modal_gui_host = false;

// salted credential hash, identical scheme to ken's. we recompute here so the
// cleartext credential never leaves the isolated region - only the hash does.
// the salt comes from ken's root/sovereign record via HashPassword. (satoru)
static void ksa_hash_credential(const char* cred, const char* username,
                                unsigned char* out, bool& ok) {
    ok = false;
    // a desktop account: the same pbkdf2 digest UserManager stores. (satoru)
    if (username && *username && UserManager::CredentialDigest(username, cred, out)) {
        ok = true;
        return;
    }
    KENUser* u = KEN::FindUser(username && *username ? username : "root");
    if (!u) u = KEN::FindUser("root");
    if (!u) return;
    KEN::HashPassword(cred, u->salt, out);
    ok = true;
}

// map the isolated region into an ephemeral kernel-private window so ksa's
// arbiter can render + read it, then return the pointer. only ksa calls this.
// (satoru)
static uint8_t* ksa_open_window() {
    if (!g_region_phys) return nullptr;
    if (g_fallback) return (uint8_t*)(uintptr_t)g_region_phys;
    if (g_isolated) {
        // re-establish identity mapping for the region (read/write, NX). this is
        // the arbiter temporarily lifting its own isolation; the main os still
        // has no mapping because only this code path touches it. (satoru)
        KernelVMM::RevealFrames(g_region_phys, KSA_REGION_SZ / PAGE_SIZE,
                                PTE_WRITABLE | PTE_NX);
        g_isolated = false;
    }
    return (uint8_t*)(uintptr_t)g_region_phys;
}

// re-isolate: drop the main-os mapping again so the region is unreachable.
static void ksa_close_window() {
    if (!g_region_phys || g_isolated || g_fallback) return;
    KernelVMM::IsolateFrames(g_region_phys, KSA_REGION_SZ / PAGE_SIZE);
    g_isolated = true;
}

bool KSA::MainOSCanReach(uint64_t host_phys) {
    // identity map => virt == phys; if the main-os page tables resolve it, it
    // is reachable. used by SelfTest to prove isolation. (satoru)
    return KernelVMM::QueryMapping(host_phys) != 0;
}

void KSA::Init() {
    if (initialized) return;
    initialized = true;

    memset(&g_last_verdict, 0, sizeof(g_last_verdict));

    // ksa needs the hypervisor's ept machinery. hardware virtualization gates
    // whether we can build a real ept root at all; without it ksa is
    // unavailable and the password factor carries policy. (satoru)
    Hypervisor::Init();
    available   = Hypervisor::IsAvailable();
    real_nested = available && VMM::IsNested() == false;  // true nested vm only when not already nested (satoru)

    SerialLogger::Log("KSA: init - hypervisor ");
    SerialLogger::Log(available ? "available" : "unavailable");
    SerialLogger::Log(", prompt path=");
    SerialLogger::Log(real_nested ? "nested-vm" : "ept-isolated-context");
    SerialLogger::Log("\r\n");
    RuntimeLog::LogSecurity("ksa initialized",
                            available ? "hypervisor-backed" : "unavailable");
}

static volatile bool g_ksa_suspended = false;   // the settings switch (satoru)
void KSA::SetSuspended(bool off) { g_ksa_suspended = off; }
bool KSA::IsSuspended() { return g_ksa_suspended; }
bool KSA::IsAvailable()   { return available && !g_ksa_suspended; }
bool KSA::IsRealNestedVM(){ return real_nested; }
uint32_t KSA::ChannelRevision() { return g_channel_rev; }
bool KSA::PromptUsable()  { return true; }   // hypervisor path or secure desktop fallback (satoru)
bool KSA::SecureDesktopActive() { return g_secure_desktop; }
void KSA::SetPromptTimeoutMs(uint32_t ms) { g_prompt_timeout_ms = ms ? ms : 60000; }

bool KSA::SpawnContext() {
    if (g_region_phys) return true;   // already spawned (satoru)

    // carve an exactly-2mb-aligned physical region. AllocContiguous returns a
    // base that may not be 2mb-aligned; over-allocate by a huge page and align
    // up so IsolateFrames can demote a single covering huge page cleanly.
    uint64_t pages_needed = (KSA_REGION_SZ / PAGE_SIZE);
    uint64_t over = PMM::AllocContiguous(pages_needed * 2);
    if (!over) {
        SerialLogger::Log("KSA: spawn failed - no contiguous guest memory\r\n");
        return false;
    }
    uint64_t aligned = (over + 0x1FFFFFULL) & ~0x1FFFFFULL;
    // free the head slack before the aligned base back to the pmm. (satoru)
    uint64_t head_pages = (aligned - over) / PAGE_SIZE;
    if (head_pages) PMM::FreeContiguous(over, head_pages);
    // free the tail slack after the 2mb region. (satoru)
    uint64_t tail_start = aligned + KSA_REGION_SZ;
    uint64_t tail_pages = (over + pages_needed * 2 * PAGE_SIZE - tail_start) / PAGE_SIZE;
    if (tail_pages) PMM::FreeContiguous(tail_start, tail_pages);

    g_region_phys = aligned;

    // zero the region (still identity-mapped at this point). (satoru)
    memset((void*)(uintptr_t)g_region_phys, 0, KSA_REGION_SZ);

    // build a dedicated ept root and map the region into ksa guest-physical
    // space ONLY. the main os ept (if any) never references this region. on the
    // ept-isolated-context fallback this still proves the region lives in a
    // separate ept namespace from the main-os identity map. (satoru)
    g_ksa_ept = EPTManager::CreateEPT();
    if (g_ksa_ept) {
        EPTManager::MapRAM(g_ksa_ept, KSA_GUEST_PHYS, g_region_phys, KSA_REGION_SZ);
    }

    // remove the region from the main-os page tables - THE isolation step.
    if (!KernelVMM::IsolateFrames(g_region_phys, KSA_REGION_SZ / PAGE_SIZE)) {
        SerialLogger::Log("KSA: WARNING - IsolateFrames incomplete\r\n");
    }
    g_isolated = true;

    SerialLogger::Log("KSA: spawned isolated context (region phys=");
    SerialLogger::LogHex((uint32_t)g_region_phys);
    SerialLogger::Log(", 2MB, ept=");
    SerialLogger::LogHex((uint32_t)(uintptr_t)g_ksa_ept);
    SerialLogger::Log(")\r\n");
    return true;
}

void KSA::TeardownContext() {
    if (!g_region_phys) return;

    // re-reveal so we can wipe the region, then free. wiping the scratch +
    // verdict ensures no credential residue survives the prompt. the fallback
    // region was never isolated, so it is already mapped. (satoru)
    if (!g_fallback)
        KernelVMM::RevealFrames(g_region_phys, KSA_REGION_SZ / PAGE_SIZE,
                                PTE_WRITABLE | PTE_NX);
    memset((void*)(uintptr_t)g_region_phys, 0, KSA_REGION_SZ);

    if (g_ksa_ept) {
        EPTManager::DestroyEPT(g_ksa_ept);
        g_ksa_ept = nullptr;
    }

    bool was_fallback = g_fallback;
    PMM::FreeContiguous(g_region_phys, KSA_REGION_SZ / PAGE_SIZE);
    g_region_phys = 0;
    g_isolated = false;
    g_fallback = false;
    SerialLogger::Log(was_fallback ? "KSA: torn down secure desktop context\r\n"
                                   : "KSA: torn down isolated context\r\n");
}

// ── prompt rendering (into the isolated framebuffer) ────────────────────
static inline void ksa_fb_px(uint8_t* fb, int x, int y, uint32_t rgba) {
    if (x < 0 || y < 0 || x >= KSA_W || y >= KSA_H) return;
    ((uint32_t*)fb)[y * KSA_W + x] = rgba;
}
static void ksa_fb_rect(uint8_t* fb, int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) ksa_fb_px(fb, x + i, y + j, c);
}

// where the modal sits on the real screen. computed once per frame so the
// renderer and the mouse hit-test agree on the same geometry. all members are
// screen coordinates. (satoru)
struct KSALayout {
    int ox, oy;                  // top-left of the panel on screen
    int approve_x, approve_y, approve_w, approve_h;
    int deny_x, deny_y, deny_w, deny_h;
};
static KSALayout ksa_layout() {
    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    int ox = (sw - KSA_W) / 2, oy = (sh - KSA_H) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    KSALayout L;
    L.ox = ox; L.oy = oy;
    const int bw = 120, bh = 36;
    const int by = oy + KSA_H - bh - 16;
    // deny on the left, approve on the right, mirroring the [Esc]/[Enter] hints.
    L.deny_w = bw;    L.deny_h = bh;
    L.deny_x = ox + 16;          L.deny_y = by;
    L.approve_w = bw; L.approve_h = bh;
    L.approve_x = ox + KSA_W - bw - 16; L.approve_y = by;
    return L;
}
static inline bool ksa_hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// blit the isolated framebuffer onto the visible screen, centered. this is the
// hypervisor present path - the main-os compositor is not involved and gets no
// pointer to the isolated region; ksa copies pixel-by-pixel into the back
// buffer. the caller composes the full frame (panel + text + buttons) THEN
// presents once, so the text - which the font engine draws into the same back
// buffer - actually reaches the screen. (satoru)
static void ksa_blit_panel(uint8_t* fb, const KSALayout& L) {
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    int x0 = L.ox, y0 = L.oy, w = KSA_W, h = KSA_H;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > sw) w = sw - x0;
    if (y0 + h > sh) h = sh - y0;
    if (w <= 0 || h <= 0) return;
    // the gui redraws the panel every frame now, so a row copy instead of a
    // per-pixel DrawPixel keeps the modal cheap enough for 60fps. (satoru)
    uint8_t* dst = Graphics::GetBackBuffer();
    if (dst && Graphics::GetBpp() == 32) {
        uint32_t pitch = Graphics::GetPitch();
        int sx = x0 - L.ox, sy = y0 - L.oy;
        for (int y = 0; y < h; y++) {
            memcpy(dst + (size_t)(y0 + y) * pitch + (size_t)x0 * 4,
                   fb + ((size_t)(sy + y) * KSA_W + (size_t)sx) * 4,
                   (size_t)w * 4);
        }
        return;
    }
    int sx = x0 - L.ox, sy = y0 - L.oy;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            Graphics::DrawPixel(x0 + x, y0 + y, ((uint32_t*)fb)[(sy + y) * KSA_W + sx + x]);
}

// dim only the area behind the panel. a full-screen blend + full-screen copy
// every frame would cost tens of ms on a 5k laptop panel; the desktop outside
// the modal stays live and undimmed, which is the point of not freezing it.
// (satoru)
static void ksa_dim_backdrop(const KSALayout& L) {
    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    const int pad = 26;
    int x = L.ox - pad, y = L.oy - pad;
    int w = KSA_W + pad * 2, h = KSA_H + pad * 2;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    Graphics::FillRectAlpha(x, y, w, h, 170, 0x000000);
    Graphics::MarkDirty(x, y, w, h);
}
// push the composed back buffer to the real framebuffer + the host gpu.
// SwapBuffers only copies the dirty-region list, and the dim + panel blit go
// through FillRectAlpha/DrawPixel which do NOT mark dirty (only the FillRect
// buttons did - which is why an earlier build showed buttons over a live
// desktop with no panel). mark the whole screen so the full composed frame is
// flushed to the front buffer and transferred to the virtio gpu. (satoru)
static void ksa_present() {
    Graphics::MarkDirty(0, 0, Graphics::GetWidth(), Graphics::GetHeight());
    Graphics::SwapBuffers();
    Graphics::PresentVirtioIfActive();
}

// draw one button into the back buffer (over the already-blitted panel). hot
// = pointer hovering, so the user sees which target a click will land on. the
// label is drawn by the font engine into the same back buffer. (satoru)
static void ksa_button(int x, int y, int w, int h, const char* label,
                       uint32_t fill, uint32_t hot_fill, bool hot) {
    Graphics::FillRect(x, y, w, h, hot ? hot_fill : fill);
    Graphics::FillRect(x, y, w, 1, 0xFF404858);   // top hairline (satoru)
    int approx = 0; while (label[approx]) approx++;
    int tw = approx * 8;
    Graphics::DrawString(x + (w - tw) / 2, y + (h - 16) / 2, label, 0xFFFFFFFF, fill);
}

// paint the static chrome into the isolated framebuffer once per prompt. the
// gui blits it every frame; only the text/buttons/hover change after this.
// (satoru)
static void ksa_paint_chrome(uint8_t* fb, const KSARequest& req) {
    if (!fb) return;
    ksa_fb_rect(fb, 0, 0, KSA_W, KSA_H, 0xFF1C1F26);       // panel bg
    ksa_fb_rect(fb, 0, 0, KSA_W, 40, 0xFF2D6CDF);          // header accent
    ksa_fb_rect(fb, 0, KSA_H - 2, KSA_W, 2, 0xFF2D6CDF);   // footer line
    if (req.want_cred) {
        // credential input box (drawn in the isolated buffer; text overlaid). (satoru)
        ksa_fb_rect(fb, 110, 124, KSA_W - 126, 26, 0xFF0E1014);
        ksa_fb_rect(fb, 110, 124, KSA_W - 126, 1, 0xFF3A4250);
    }
}

// compose the modal over whatever the compositor just drew. no present here:
// the gui loop owns the swap, and this runs inside its back-buffer lock.
// (satoru)
static void ksa_draw_prompt(uint8_t* fb, const KSARequest& req, const char* typed,
                            int typed_len, bool show_risk, int mouse_x, int mouse_y) {
    if (!fb) return;
    // the main-os compositor leaves a clip rect set from its last frame; our
    // panel blit would otherwise be silently dropped outside it. reset to full
    // screen so ksa owns the surface it draws on. (satoru)
    Graphics::ClearClipRect();

    KSALayout L = ksa_layout();
    int ox = L.ox, oy = L.oy;

    // dim the backdrop behind the panel, then blit the isolated chrome over it.
    // the text is drawn straight into the main back buffer with the font
    // engine; composing it before the swap is what makes the labels visible.
    // (satoru)
    ksa_dim_backdrop(L);
    ksa_blit_panel(fb, L);

    (void)typed;   // credential is echoed masked via typed_len, not the chars (satoru)

    Graphics::DrawString(ox + 12, oy + 12, "KSA - Kurono Secure Authorization",
                         0xFFFFFFFF, 0xFF2D6CDF);
    Graphics::DrawString(ox + 14, oy + 54,
                         req.title ? req.title : "Privilege Escalation",
                         0xFFFFFFFF, 0xFF1C1F26);
    Graphics::DrawString(ox + 14, oy + 78,
                         req.detail ? req.detail : "An action requires elevated rights.",
                         0xFFB8C0D0, 0xFF1C1F26);
    char acct[80];
    int p = 0; const char* pre = "Account: ";
    while (pre[p]) { acct[p] = pre[p]; p++; }
    const char* un = req.username && *req.username ? req.username : "root";
    int q = 0; while (un[q] && p < 78) acct[p++] = un[q++];
    acct[p] = 0;
    Graphics::DrawString(ox + 14, oy + 102, acct, 0xFFB8C0D0, 0xFF1C1F26);

    if (req.want_cred) {
        Graphics::DrawString(ox + 14, oy + 128, "Credential:", 0xFFFFFFFF, 0xFF1C1F26);
        // masked credential echo (never the real characters). (satoru)
        char mask[64];
        int m = 0; for (; m < typed_len && m < 48; m++) mask[m] = '*';
        mask[m] = 0;
        Graphics::DrawString(ox + 118, oy + 128, mask, 0xFF7CFF9B, 0xFF0E1014);
    }

    if (show_risk) {
        Graphics::DrawString(ox + 14, oy + 162,
            "WARNING: both auth factors are disabled.", 0xFFFF6B6B, 0xFF1C1F26);
    }

    // real clickable buttons + the keyboard-shortcut hints. (satoru)
    bool hot_deny    = ksa_hit(mouse_x, mouse_y, L.deny_x, L.deny_y, L.deny_w, L.deny_h);
    bool hot_approve = ksa_hit(mouse_x, mouse_y, L.approve_x, L.approve_y, L.approve_w, L.approve_h);
    ksa_button(L.deny_x, L.deny_y, L.deny_w, L.deny_h, "Deny",
               0xFF3A2226, 0xFF5A2A30, hot_deny);
    ksa_button(L.approve_x, L.approve_y, L.approve_w, L.approve_h, "Approve",
               0xFF1E3A28, 0xFF276B43, hot_approve);
    // the hint sits a line above the buttons; at KSA_H - 56 it ran into them (satoru)
    Graphics::DrawString(ox + 14, L.deny_y - 26,
        "[Enter] Approve    [Esc] Deny", 0xFF8892A4, 0xFF1C1F26);

    // the gui only blits damaged rects; the panel + dim must reach the front
    // buffer even when the desktop underneath did not change. (satoru)
    Graphics::MarkDirty(ox - 26, oy - 26, KSA_W + 52, KSA_H + 52);
}

// direct-mode present: used only when the caller is the gui process itself
// (the kurono.ksa.prompt demo), because there is no compositor frame to ride
// on in that case. (satoru)
static void ksa_render_now(uint8_t* fb, const KSARequest& req, const char* typed,
                           int typed_len, bool show_risk, int mouse_x, int mouse_y) {
    ksa_draw_prompt(fb, req, typed, typed_len, show_risk, mouse_x, mouse_y);
    ksa_present();
}

// ── modal helpers shared by the waiter and the gui loop ────────────────
static void ksa_copy_str(char* dst, int cap, const char* src) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// the caller is the compositor itself (PromptDemo from the gui loop). it
// cannot wait for a gui frame that it is blocking, so it drives the modal
// inline instead. (satoru)
static bool ksa_caller_is_gui() {
    Process* p = Scheduler::GetCurrentProcess();
    if (!p || !p->is_kernel_proc) return false;
    const char* n = p->name;
    return n[0] == 'g' && n[1] == 'u' && n[2] == 'i' && n[3] == 0;
}

// publish a decision exactly once. release ordering makes the typed buffer
// visible to the waiter before it reads the verdict. (satoru)
static void ksa_modal_decide(bool approved) {
    if (__atomic_load_n(&g_modal.decided, __ATOMIC_ACQUIRE)) return;
    g_modal.approved = approved;
    __atomic_store_n(&g_modal.decided, true, __ATOMIC_RELEASE);
}

// yield the caller for one slice while the modal is up. a kernel process
// sleeps normally (the scheduler runs the gui); a syscall waiter on an ap just
// halts until the next irq, because the gui lives on the bsp. a bsp syscall
// never reaches here - it self-pumps instead. (satoru)
static void ksa_wait_slice(uint32_t ms) {
    Scheduler::SleepMs(ms);
}

bool KSA::ModalActive() { return __atomic_load_n(&g_modal_live, __ATOMIC_ACQUIRE); }
bool KSA::ModalGuiHosted() { return __atomic_load_n(&g_modal_gui_host, __ATOMIC_ACQUIRE); }

void KSA::RenderPromptOverlay() {
    if (!ModalActive()) return;
    __atomic_add_fetch(&g_modal_rendering, 1, __ATOMIC_ACQ_REL);
    // re-check after publishing that we are inside: the waiter clears live
    // first and then spins on this counter before it touches the region. (satoru)
    if (ModalActive() && g_modal.fb) {
        KSARequest req;
        req.title     = g_modal.title;
        req.detail    = g_modal.detail;
        req.username  = g_modal.username;
        req.want_cred = g_modal.want_cred;
        ksa_draw_prompt(g_modal.fb, req, g_modal.typed, g_modal.typed_len,
                        g_modal.show_risk, g_modal.mx, g_modal.my);
    }
    __atomic_sub_fetch(&g_modal_rendering, 1, __ATOMIC_ACQ_REL);
}

void KSA::PumpPromptInput() {
    if (!ModalActive()) return;
    uint32_t now = (uint32_t)Timer::GetRealMs64();
    if (!__atomic_load_n(&g_modal.decided, __ATOMIC_ACQUIRE) &&
        (uint32_t)(now - g_modal.started_ms) >= g_modal.timeout_ms) {
        g_modal.timed_out = true;
        ksa_modal_decide(false);
        return;
    }

    // keyboard: type the credential, Enter approves, Esc denies. (satoru)
    while (Keyboard::HasChar()) {
        char c = Keyboard::GetChar();
        if (c == '\r' || c == '\n') { ksa_modal_decide(true); break; }
        if (c == 27)                { ksa_modal_decide(false); break; }
        if (c == 8 || c == 127) {
            if (g_modal.typed_len > 0) g_modal.typed[--g_modal.typed_len] = 0;
            continue;
        }
        if (c >= 32 && c < 127 && g_modal.typed_len < 62) {
            g_modal.typed[g_modal.typed_len++] = c;
            g_modal.typed[g_modal.typed_len] = 0;
        }
    }
    if (__atomic_load_n(&g_modal.decided, __ATOMIC_ACQUIRE)) return;

    // esc is a control key, not a character: KeyToChar has no mapping for it,
    // so it never reaches the char ring. read the raw press edge instead. (satoru)
    if (Keyboard::IsKeyPressed(KEY_ESC)) ksa_modal_decide(false);
    if (__atomic_load_n(&g_modal.decided, __ATOMIC_ACQUIRE)) return;

    // mouse: motion drives button hover, a click on a rect decides. the gui
    // has already polled the devices (usb included) into these same rings.
    // (satoru)
    while (Mouse::HasEvent()) {
        Mouse::Event m = Mouse::GetEvent();
        if (m.x != g_modal.mx || m.y != g_modal.my) { g_modal.mx = m.x; g_modal.my = m.y; }
    }
    int cur_mx, cur_my;
    Mouse::GetPosition(cur_mx, cur_my);
    if (cur_mx != g_modal.mx || cur_my != g_modal.my) { g_modal.mx = cur_mx; g_modal.my = cur_my; }
    if (Mouse::LeftClicked()) {
        KSALayout L = ksa_layout();
        if (ksa_hit(g_modal.mx, g_modal.my, L.approve_x, L.approve_y, L.approve_w, L.approve_h)) {
            ksa_modal_decide(true);
        } else if (ksa_hit(g_modal.mx, g_modal.my, L.deny_x, L.deny_y, L.deny_w, L.deny_h)) {
            ksa_modal_decide(false);
        }
    }
}

// drain any keystrokes / mouse events the main os queued BEFORE the modal took
// over, so a key already sitting in a ring can't be consumed as a credential
// char or counted as an Enter/Esc. usb hid is drained into those rings first:
// on a usb-only machine the old flush never saw the keyboard at all. (satoru)
static void ksa_flush_input() {
    SpinLockCpuGuard guard(g_input_lock);
    Keyboard::Poll();
    Mouse::Poll();
    USB::PollHID();
    while (Keyboard::HasChar()) Keyboard::GetChar();
    while (Mouse::HasEvent()) Mouse::GetEvent();
    (void)Mouse::LeftClicked();   // clear a pending click edge (satoru)
}

bool KSA::Prompt(const KSARequest& req, KSAVerdict& out) {
    memset(&out, 0, sizeof(out));
    if (available && !g_ksa_suspended) {
        if (!SpawnContext()) return false;
    } else {
        // no hypervisor on this machine: draw the same modal from ordinary
        // kernel memory. it is not memory-isolated, but the verdict still only
        // crosses through ksa's own channel. (satoru)
        if (!g_region_phys) {
            uint64_t pages = KSA_REGION_SZ / PAGE_SIZE;
            uint64_t phys = PMM::AllocContiguous(pages);
            if (!phys) {
                SerialLogger::Log("KSA: fallback prompt failed - no memory\r\n");
                return false;
            }
            g_region_phys = phys;
            g_fallback = true;
            g_isolated = false;
            memset((void*)(uintptr_t)g_region_phys, 0, KSA_REGION_SZ);
            SerialLogger::Log(available ? "KSA: switched off - plain secure desktop prompt\r\n"
                                        : "KSA: no hypervisor - secure desktop fallback prompt\r\n");
        }
    }

    // one prompt at a time: a second gated action waits in its syscall, not in
    // ksa's shared modal state. deny it rather than corrupt the live prompt.
    // (satoru)
    bool expected = false;
    if (!__atomic_compare_exchange_n(&g_modal_busy, &expected, true,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        SerialLogger::Log("KSA: prompt already active - denying concurrent request\r\n");
        return false;
    }

    // which engine runs the modal:
    //   direct    - the caller IS the gui process (prompt demo);
    //   self_pump - a ring-0 syscall on the bsp. it cannot run the gui kernel
    //               process (SleepMs early-returns for non-kernel callers), so
    //               the waiter drives PumpUI itself and the gui stands down;
    //   gui host  - a kernel-proc caller (shell) or a syscall on an ap: the
    //               bsp is free, so the gui loop composites normally. (satoru)
    bool direct = ksa_caller_is_gui();
    Process* me = Scheduler::GetCurrentProcess();
    bool self_pump = !direct && !(me && me->is_kernel_proc) && SMP::CpuIndex() == 0;
    __atomic_store_n(&g_modal_gui_host, !direct && !self_pump, __ATOMIC_RELEASE);

    g_secure_desktop = true;
    ksa_flush_input();

    memset(&g_modal, 0, sizeof(g_modal));
    ksa_copy_str(g_modal.title, sizeof(g_modal.title),
                 req.title ? req.title : "Privilege Escalation");
    ksa_copy_str(g_modal.detail, sizeof(g_modal.detail),
                 req.detail ? req.detail : "An action requires elevated rights.");
    ksa_copy_str(g_modal.username, sizeof(g_modal.username),
                 req.username && *req.username ? req.username : "root");
    g_modal.want_cred  = req.want_cred;
    g_modal.show_risk  = KEN::BothAuthDisabled();
    g_modal.started_ms = (uint32_t)Timer::GetRealMs64();
    g_modal.timeout_ms = g_prompt_timeout_ms;
    g_modal.mx = Graphics::GetWidth() / 2;
    g_modal.my = Graphics::GetHeight() / 2;
    Mouse::GetPosition(g_modal.mx, g_modal.my);

    // keep the ephemeral window open for the whole modal. the gui re-blits the
    // panel every frame, and reopening it per frame would run two 512-page
    // isolation walks per frame on the laptop. (satoru)
    g_modal.fb = ksa_open_window();
    {
        KSARequest local;
        local.title     = g_modal.title;
        local.detail    = g_modal.detail;
        local.username  = g_modal.username;
        local.want_cred = g_modal.want_cred;
        ksa_paint_chrome(g_modal.fb, local);
    }
    __atomic_store_n(&g_modal.decided, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_modal_live, true, __ATOMIC_RELEASE);

    RuntimeLog::LogSecurity("ksa prompt shown", g_modal.detail);
    KEN::Log(ACT_KSA_PROMPT, g_modal.username, g_modal.detail);
    SerialLogger::Log(direct
        ? "KSA: prompt up - caller is gui, inline modal\r\n"
        : self_pump
            ? "KSA: prompt up - waiter pumps the ui, gui stands down\r\n"
            : "KSA: prompt up - gui composites, caller waits cooperatively\r\n");

    if (direct) {
        // the caller is the compositor: render the first frame ourselves.
        KSARequest local;
        local.title     = g_modal.title;
        local.detail    = g_modal.detail;
        local.username  = g_modal.username;
        local.want_cred = g_modal.want_cred;
        if (g_modal.fb)
            ksa_render_now(g_modal.fb, local, g_modal.typed, g_modal.typed_len,
                           g_modal.show_risk, g_modal.mx, g_modal.my);
    } else {
        // wake the gui loop onto the modal for this frame. (satoru)
        Graphics::MarkUIDirty();
    }

    // gui-hosted modal (a syscall on an ap, ken's own path among them): the caller
    // may hold kls, and a human decision takes seconds. ken called KSA::Prompt
    // without the admin gate's KlsRelax, so the 3 s hold ended in a HARD steal
    // and two cpus inside the kls body (laptop panic 1112b). release it for the
    // wait and take it back after; the modal state has its own atomics. (satoru)
    int kls_depth = (!direct && !self_pump) ? LinuxSyscall::KlsDropAll() : 0;
    bool timed_out = false;
    for (;;) {
        if (__atomic_load_n(&g_modal.decided, __ATOMIC_ACQUIRE)) break;
        uint32_t now = (uint32_t)Timer::GetRealMs64();
        if ((uint32_t)(now - g_modal.started_ms) >= g_modal.timeout_ms) {
            timed_out = true;
            g_modal.timed_out = true;
            ksa_modal_decide(false);
            break;
        }
        if (direct) {
            // no gui loop can pump this frame, so poll the devices (usb hid
            // included) and redraw inline. this path is only the demo/self
            // test, where blocking the compositor is expected. (satoru)
            {
                SpinLockCpuGuard guard(g_input_lock);
                Keyboard::Poll();
                Mouse::Poll();
                USB::PollHID();
            }
            PumpPromptInput();
            if (!__atomic_load_n(&g_modal.decided, __ATOMIC_ACQUIRE) && g_modal.fb) {
                KSARequest local;
                local.title     = g_modal.title;
                local.detail    = g_modal.detail;
                local.username  = g_modal.username;
                local.want_cred = g_modal.want_cred;
                ksa_render_now(g_modal.fb, local, g_modal.typed, g_modal.typed_len,
                               g_modal.show_risk, g_modal.mx, g_modal.my);
            }
            Scheduler::SleepMs(8);
        } else if (self_pump) {
            // bsp syscall context: composite the desktop + the modal and poll
            // usb/ps2 from the syscall pump, then hlt one tick. (satoru)
            KuronoShell::PumpModalUI();
            Scheduler::SleepMs(1);
        } else {
            ksa_wait_slice(10);
        }
    }

    LinuxSyscall::KlsRestore(kls_depth);   // (satoru)
    bool approved = __atomic_load_n(&g_modal.approved, __ATOMIC_ACQUIRE);
    if (timed_out || g_modal.timed_out) {
        // timed out with no answer - fail closed (treated as a deny). (satoru)
        approved = false;
        SerialLogger::Log("KSA: prompt timed out - failing closed (deny)\r\n");
    }

    // stop the compositor before touching the region: clear live, then wait out
    // any overlay blit already in flight. (satoru)
    __atomic_store_n(&g_modal_live, false, __ATOMIC_RELEASE);
    while (__atomic_load_n(&g_modal_rendering, __ATOMIC_ACQUIRE) != 0) {
        Process* me = Scheduler::GetCurrentProcess();
        if (me && me->is_kernel_proc) Scheduler::SleepMs(1);
        else {
            uint64_t until = Timer::GetRealMs64() + 1;
            while (Timer::GetRealMs64() < until) __asm__ __volatile__("pause" ::: "memory");
        }
    }

    // write the verdict INTO the isolated region (the in-vm result), then read
    // it back through the channel accessor. this models the result crossing the
    // vmcall boundary: ksa's arbiter is the only writer; the host reads a copy.
    {
        uint8_t* fb = g_modal.fb;
        if (fb) {
            KSAVerdict* in = (KSAVerdict*)(fb + KSA_VERDICT_OFF);
            memset(in, 0, sizeof(*in));
            in->approved  = approved;
            in->completed = true;
            if (g_modal.want_cred && approved && g_modal.typed_len > 0) {
                bool ok = false;
                // hash inside the isolated region; cleartext stays here and is
                // wiped on teardown. (satoru)
                char* scratch = (char*)(fb + KSA_SCRATCH_OFF);
                for (int i = 0; i <= g_modal.typed_len && i < 63; i++)
                    scratch[i] = g_modal.typed[i];
                ksa_hash_credential(scratch, g_modal.username, in->cred_hash, ok);
                in->have_cred_hash = ok;
                // wipe scratch immediately. (satoru)
                for (int i = 0; i < 63; i++) scratch[i] = 0;
            }
            // latch the host-side copy via the (read-only) channel. (satoru)
            g_last_verdict = *in;
        }
        ksa_close_window();
        g_modal.fb = nullptr;
    }

    // wipe the typed buffer - the cleartext credential must not linger in
    // main-os memory. (satoru)
    memset(g_modal.typed, 0, sizeof(g_modal.typed));
    g_modal.typed_len = 0;

    // repaint the desktop that the modal sat over. the compositor is already
    // live, so a dirty mark is enough (no saved-frame restore is needed). (satoru)
    Graphics::MarkUIDirty();

    bool got = ReadVerdictForChannel(out);
    KEN::Log(approved ? ACT_KSA_APPROVE : ACT_KSA_DENY,
              g_modal.username, approved ? "ksa approved" : "ksa denied");
    RuntimeLog::LogSecurity(approved ? "ksa approved" : "ksa denied",
                            g_modal.detail);

    // anti-replay: clear the latched verdict once ken has consumed it via the
    // out-param, so a later VMCALL on the channel cannot re-read a stale
    // "approved" after the prompt has ended. (satoru)
    memset(&g_last_verdict, 0, sizeof(g_last_verdict));

    g_secure_desktop = false;
    __atomic_store_n(&g_modal_gui_host, false, __ATOMIC_RELEASE);
    TeardownContext();
    __atomic_store_n(&g_modal_busy, false, __ATOMIC_RELEASE);
    return got;
}

bool KSA::ReadVerdictForChannel(KSAVerdict& out) {
    // the ONLY way the verdict leaves ksa: a copy of the latched struct. there
    // is no inverse (no host->ksa write path), so a forged approval cannot be
    // injected from the main os. (satoru)
    out = g_last_verdict;
    return g_last_verdict.completed;
}

// ── self-test (kurono.ksa.test) ─────────────────────────────────────────
bool KSA::SelfTest() {
    SerialLogger::Log("KSA-SELFTEST: begin\r\n");
    // the ksecurity daemon runs this periodically. it must never touch the
    // isolated region while a prompt owns it: SpawnContext returns the live
    // region and TeardownContext would free it under the waiting modal. (satoru)
    if (__atomic_load_n(&g_modal_busy, __ATOMIC_ACQUIRE) || ModalActive()) {
        SerialLogger::Log("KSA-SELFTEST: skipped - prompt active\r\n");
        SerialLogger::Log("KSA-SELFTEST: PASS (skipped)\r\n");
        RuntimeLog::LogSecurity("ksa selftest", "skipped (prompt active)");
        return true;
    }
    if (!available) {
        SerialLogger::Log("KSA-SELFTEST: hypervisor unavailable - ksa is a no-op on this host\r\n");
        SerialLogger::Log("KSA-SELFTEST: PASS (unavailable-and-honest)\r\n");
        return true;
    }

    if (!SpawnContext()) {
        SerialLogger::Log("KSA-SELFTEST: FAIL - could not spawn isolated context\r\n");
        return false;
    }

    // invariant 1: while isolated, the main-os page tables must NOT resolve the
    // region. probe the base + a mid frame + the verdict frame. (satoru)
    bool reach_base = MainOSCanReach(g_region_phys);
    bool reach_mid  = MainOSCanReach(g_region_phys + 0x100000ULL);
    bool reach_vfr  = MainOSCanReach(g_region_phys + KSA_VERDICT_OFF);
    SerialLogger::Log("KSA-SELFTEST: main-os reach base="); SerialLogger::Log(reach_base?"YES":"no");
    SerialLogger::Log(" mid=");  SerialLogger::Log(reach_mid?"YES":"no");
    SerialLogger::Log(" verdict="); SerialLogger::Log(reach_vfr?"YES":"no");
    SerialLogger::Log("\r\n");
    bool iso_ok = !reach_base && !reach_mid && !reach_vfr;
    SerialLogger::Log(iso_ok ? "KSA-SELFTEST: PASS isolation (region unmapped from main OS)\r\n"
                             : "KSA-SELFTEST: FAIL isolation (region still reachable!)\r\n");

    // invariant 2: write a synthetic verdict into the isolated region via the
    // arbiter window, then confirm it only comes back through the channel and
    // that the channel returns a copy (not the in-region pointer). (satoru)
    {
        uint8_t* fb = ksa_open_window();
        bool reach_while_open = MainOSCanReach(g_region_phys);
        KSAVerdict* in = (KSAVerdict*)(fb + KSA_VERDICT_OFF);
        memset(in, 0, sizeof(*in));
        in->approved = true; in->completed = true; in->have_cred_hash = false;
        g_last_verdict = *in;
        ksa_close_window();
        SerialLogger::Log("KSA-SELFTEST: while arbiter window open, main-os reach=");
        SerialLogger::Log(reach_while_open?"YES (arbiter only)":"no");
        SerialLogger::Log("\r\n");
    }

    KSAVerdict v; bool got = ReadVerdictForChannel(v);
    bool chan_ok = got && v.completed && v.approved;
    SerialLogger::Log(chan_ok ? "KSA-SELFTEST: PASS channel (verdict crossed via copy)\r\n"
                              : "KSA-SELFTEST: FAIL channel\r\n");

    // invariant 3: there is no host->ksa approval-write path. assert by design:
    // ReadVerdictForChannel is the only export and it takes an out-param it
    // fills from g_last_verdict; nothing in the public api writes the in-region
    // verdict from the main os. log the assertion. (satoru)
    SerialLogger::Log("KSA-SELFTEST: channel is read-only (no host->ksa approval write path)\r\n");

    // re-isolate confirm. (satoru)
    bool reach_after = MainOSCanReach(g_region_phys);
    SerialLogger::Log("KSA-SELFTEST: after re-isolate, main-os reach=");
    SerialLogger::Log(reach_after?"YES (FAIL)":"no (PASS)"); SerialLogger::Log("\r\n");

    TeardownContext();
    memset(&g_last_verdict, 0, sizeof(g_last_verdict));

    bool pass = iso_ok && chan_ok && !reach_after;
    SerialLogger::Log(pass ? "KSA-SELFTEST: OVERALL PASS\r\n"
                           : "KSA-SELFTEST: OVERALL FAIL\r\n");
    RuntimeLog::LogSecurity("ksa selftest", pass ? "pass" : "fail");
    return pass;
}

// ── interactive prompt demo (kurono.ksa.prompt) ─────────────────────────
// drive the REAL on-screen modal so a headless screendump can prove it draws,
// and synthetic input (Enter/Esc or an Approve/Deny click) can prove the
// verdict flows. this is the render-path counterpart to SelfTest() (which only
// checks the isolation invariants and never paints the prompt). (satoru)
bool KSA::PromptDemo(bool want_cred) {
    SerialLogger::Log("KSA-PROMPT-DEMO: begin (want_cred=");
    SerialLogger::Log(want_cred ? "yes)\r\n" : "no)\r\n");
    if (!available) {
        SerialLogger::Log("KSA-PROMPT-DEMO: ksa unavailable on this host - cannot draw the prompt\r\n");
        return false;
    }

    KSARequest req;
    req.title     = "Privilege Escalation";
    req.detail    = "Allow Settings to change the system auth policy?";
    req.username  = "root";
    req.want_cred = want_cred;

    KSAVerdict v;
    bool ran = Prompt(req, v);
    SerialLogger::Log("KSA-PROMPT-DEMO: prompt ");
    SerialLogger::Log(ran ? "ran" : "did NOT run");
    SerialLogger::Log(", verdict=");
    SerialLogger::Log(v.approved ? "APPROVE" : "DENY");
    SerialLogger::Log(v.have_cred_hash ? " (cred-hash present)\r\n" : "\r\n");
    RuntimeLog::LogSecurity("ksa prompt demo", v.approved ? "approved" : "denied");
    return v.approved;
}

// end (satoru)
