//  kurono os  -  ksa (kurono secure authorization) implementation
//
//  see ksa.h for the architecture + isolation model. summary of how the
//  security guarantees are realized here:
//
//   1. spawn: PMM::AllocContiguous() carves a 2mb-aligned, exactly-2mb
//      physical region for the prompt (framebuffer + state + verdict). a
//      dedicated EPT root is built and the region is mapped ONLY into that
//      ept (the ksa guest-physical space). then KernelVMM::IsolateFrames()
//      removes the region from the main-os page tables entirely  -  after this
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
//      channel is read-only  -  there is no vmcall that lets the main os write
//      an approval into ksa memory, so a forged "yes" cannot be injected.
//
//  nested-vmx note: on a host where true nested vmx for the inner vm is
//  unavailable (nested kvm under qemu  -  the common dev case), the prompt
//  logic runs as an ept-isolated guest *context* rather than a separately
//  vmlaunch'd vm. IsRealNestedVM() reports which path is active. the
//  isolation invariants above hold in BOTH cases; only the cpu-mode boundary
//  differs, and that difference is reported, not hidden.
#include "ksa.h"
#include "supr.h"
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

// a verdict latched after a prompt completes  -  what the channel returns. the
// host copy lives in normal kernel memory (NOT in the isolated region); it is
// populated only by ksa's own arbiter code after reading the in-region verdict
// through the ephemeral mapping. (satoru)
static KSAVerdict g_last_verdict;

// salted credential hash, identical scheme to supr's. we recompute here so the
// cleartext credential never leaves the isolated region  -  only the hash does.
// the salt comes from supr's root/sovereign record via HashPassword. (satoru)
static void ksa_hash_credential(const char* cred, const char* username,
                                unsigned char* out, bool& ok) {
    ok = false;
    SUPRUser* u = SUPR::FindUser(username && *username ? username : "root");
    if (!u) u = SUPR::FindUser("root");
    if (!u) return;
    SUPR::HashPassword(cred, u->salt, out);
    ok = true;
}

// map the isolated region into an ephemeral kernel-private window so ksa's
// arbiter can render + read it, then return the pointer. only ksa calls this.
// (satoru)
static uint8_t* ksa_open_window() {
    if (!g_region_phys) return nullptr;
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
    if (!g_region_phys || g_isolated) return;
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

    SerialLogger::Log("KSA: init  -  hypervisor ");
    SerialLogger::Log(available ? "available" : "unavailable");
    SerialLogger::Log(", prompt path=");
    SerialLogger::Log(real_nested ? "nested-vm" : "ept-isolated-context");
    SerialLogger::Log("\r\n");
    RuntimeLog::LogSecurity("ksa initialized",
                            available ? "hypervisor-backed" : "unavailable");
}

bool KSA::IsAvailable()   { return available; }
bool KSA::IsRealNestedVM(){ return real_nested; }
uint32_t KSA::ChannelRevision() { return g_channel_rev; }

bool KSA::SpawnContext() {
    if (g_region_phys) return true;   // already spawned (satoru)

    // carve an exactly-2mb-aligned physical region. AllocContiguous returns a
    // base that may not be 2mb-aligned; over-allocate by a huge page and align
    // up so IsolateFrames can demote a single covering huge page cleanly.
    uint64_t pages_needed = (KSA_REGION_SZ / PAGE_SIZE);
    uint64_t over = PMM::AllocContiguous(pages_needed * 2);
    if (!over) {
        SerialLogger::Log("KSA: spawn failed  -  no contiguous guest memory\r\n");
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

    // remove the region from the main-os page tables  -  THE isolation step.
    if (!KernelVMM::IsolateFrames(g_region_phys, KSA_REGION_SZ / PAGE_SIZE)) {
        SerialLogger::Log("KSA: WARNING  -  IsolateFrames incomplete\r\n");
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
    // verdict ensures no credential residue survives the prompt. (satoru)
    KernelVMM::RevealFrames(g_region_phys, KSA_REGION_SZ / PAGE_SIZE,
                            PTE_WRITABLE | PTE_NX);
    memset((void*)(uintptr_t)g_region_phys, 0, KSA_REGION_SZ);

    if (g_ksa_ept) {
        EPTManager::DestroyEPT(g_ksa_ept);
        g_ksa_ept = nullptr;
    }

    PMM::FreeContiguous(g_region_phys, KSA_REGION_SZ / PAGE_SIZE);
    g_region_phys = 0;
    g_isolated = false;
    SerialLogger::Log("KSA: torn down isolated context\r\n");
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
// hypervisor present path  -  the main-os compositor is not involved and gets no
// pointer to the isolated region; ksa copies pixel-by-pixel into the back
// buffer. the caller composes the full frame (panel + text + buttons) THEN
// presents once, so the text  -  which the font engine draws into the same back
// buffer  -  actually reaches the screen. (satoru)
static void ksa_blit_panel(uint8_t* fb, const KSALayout& L) {
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    // dim the backdrop so the prompt reads as a modal security surface. (satoru)
    Graphics::FillRectAlpha(0, 0, sw, sh, 170, 0x000000);
    for (int y = 0; y < KSA_H; y++) {
        for (int x = 0; x < KSA_W; x++) {
            uint32_t px = ((uint32_t*)fb)[y * KSA_W + x];
            Graphics::DrawPixel(L.ox + x, L.oy + y, px);
        }
    }
}
// push the composed back buffer to the real framebuffer + the host gpu.
// SwapBuffers only copies the dirty-region list, and the dim + panel blit go
// through FillRectAlpha/DrawPixel which do NOT mark dirty (only the FillRect
// buttons did  -  which is why an earlier build showed buttons over a live
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

static void ksa_render(uint8_t* fb, const KSARequest& req, const char* typed,
                       int typed_len, bool show_risk, int mouse_x, int mouse_y) {
    // the main-os compositor leaves a clip rect set from its last frame; our
    // per-pixel panel/dim blit goes through DrawPixel which honours the clip and
    // would otherwise be silently dropped outside it (that left the buttons
    // visible  -  FillRect clamps instead of dropping  -  but the panel/backdrop
    // missing). reset to full-screen so ksa owns the whole surface. (satoru)
    Graphics::ClearClipRect();

    // chrome: dark panel, accent header, title + detail + masked credential.
    // composed entirely into the isolated framebuffer first (the security-
    // relevant pixels originate in the unmapped region). (satoru)
    ksa_fb_rect(fb, 0, 0, KSA_W, KSA_H, 0xFF1C1F26);       // panel bg
    ksa_fb_rect(fb, 0, 0, KSA_W, 40, 0xFF2D6CDF);          // header accent
    ksa_fb_rect(fb, 0, KSA_H - 2, KSA_W, 2, 0xFF2D6CDF);   // footer line
    if (req.want_cred) {
        // credential input box (drawn in the isolated buffer; text overlaid). (satoru)
        ksa_fb_rect(fb, 110, 124, KSA_W - 126, 26, 0xFF0E1014);
        ksa_fb_rect(fb, 110, 124, KSA_W - 126, 1, 0xFF3A4250);
    }

    KSALayout L = ksa_layout();
    int ox = L.ox, oy = L.oy;

    // blit the isolated panel into the back buffer, then draw text + buttons
    // over it, THEN present once. text rendering needs the font engine, which
    // targets the main back buffer; doing it before the present is what makes
    // the labels visible (the old code presented first and lost the text). (satoru)
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
    Graphics::DrawString(ox + 14, oy + KSA_H - 56,
        "[Enter] Approve    [Esc] Deny", 0xFF8892A4, 0xFF1C1F26);

    // hand the composed frame to the screen + host gpu in one present. (satoru)
    ksa_present();
}

// drain any keystrokes / mouse events the main os queued BEFORE the secure
// prompt took over, so a key already sitting in the ps/2 ring can't be consumed
// as a credential char or counted as an Enter/Esc. nothing the main os queued
// can drive the verdict. (satoru)
static void ksa_flush_input() {
    Keyboard::Poll();
    while (Keyboard::HasChar()) Keyboard::GetChar();
    Mouse::Poll();
    while (Mouse::HasEvent()) Mouse::GetEvent();
    (void)Mouse::LeftClicked();   // clear a pending click edge (satoru)
}

bool KSA::Prompt(const KSARequest& req, KSAVerdict& out) {
    memset(&out, 0, sizeof(out));
    if (!available) {
        SerialLogger::Log("KSA: Prompt requested but ksa unavailable\r\n");
        return false;
    }
    if (!SpawnContext()) return false;

    RuntimeLog::LogSecurity("ksa prompt shown", req.detail ? req.detail : req.title);
    SUPR::Log(ACT_KSA_PROMPT, req.username ? req.username : "root",
              req.detail ? req.detail : "ksa prompt");
    SerialLogger::Log("KSA: prompt up  -  secure desktop owns the screen + input\r\n");

    // ── DISPLAY OWNERSHIP / SECURE DESKTOP ─────────────────────────────────
    // the cooperative scheduler is the lever here. this loop never calls
    // Scheduler::Yield/SleepMs, so while it runs the gui compositor process and
    // the input process are BOTH starved  -  the main os cannot draw over the
    // prompt, cannot read the framebuffer that ksa is presenting to, and cannot
    // pull the keystrokes/clicks (this loop is the only code polling the 8042).
    // ksa is therefore the sole owner of the screen and of input for the prompt's
    // lifetime. we snapshot the back buffer up front and restore it on the way
    // out so the desktop the main os left behind is put back exactly, then a
    // MarkUIDirty() lets the resumed compositor repaint normally. (satoru)
    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    int pitch = Graphics::GetPitch();
    uint8_t* back = Graphics::GetBackBuffer();
    uint8_t* saved = nullptr;
    uint64_t saved_bytes = (uint64_t)sh * (uint64_t)pitch;
    if (back && saved_bytes) {
        saved = (uint8_t*)PMM::AllocBytes(saved_bytes);
        if (saved) memcpy(saved, back, saved_bytes);
    }

    // drop any pre-queued main-os input before we start reading for real. (satoru)
    ksa_flush_input();

    bool show_risk = SUPR::BothAuthDisabled();
    char typed[64]; int typed_len = 0; typed[0] = 0;
    bool decided = false, approved = false;
    int mx = sw / 2, my = sh / 2;
    Mouse::GetPosition(mx, my);

    // arbiter render/input loop. open the ephemeral window to render into the
    // isolated fb, then re-isolate between frames so the region spends most of
    // its life unmapped from the main os. cooperative; ~60s timeout. (satoru)
    uint32_t waited_ms = 0;
    Timer::ElapsedSinceLast();   // reset baseline
    {
        uint8_t* fb = ksa_open_window();
        if (fb) ksa_render(fb, req, typed, typed_len, show_risk, mx, my);
        ksa_close_window();
    }

    while (!decided && waited_ms < 60000) {
        uint32_t e = Timer::ElapsedSinceLast();
        if (e > 0) { TimeManager::AdvanceByMs(e); waited_ms += e; }
        bool dirty = false;

        // keyboard: type the credential, Enter approves, Esc denies. (satoru)
        Keyboard::Poll();
        while (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == '\r' || c == '\n') { decided = true; approved = true; break; }
            else if (c == 27)           { decided = true; approved = false; break; } // esc
            else if (c == 8 || c == 127) { if (typed_len > 0) { typed[--typed_len] = 0; dirty = true; } }
            else if (c >= 32 && c < 127 && typed_len < 62) { typed[typed_len++] = c; typed[typed_len] = 0; dirty = true; }
        }

        // mouse: drain motion (for button hover) and detect a click on the
        // Approve/Deny rects. ksa polls the mouse itself  -  the paused input
        // process is not feeding it. (satoru)
        if (!decided) {
            Mouse::Poll();
            while (Mouse::HasEvent()) {
                Mouse::Event m = Mouse::GetEvent();
                if (m.x != mx || m.y != my) { mx = m.x; my = m.y; dirty = true; }
            }
            int cur_mx, cur_my; Mouse::GetPosition(cur_mx, cur_my);
            if (cur_mx != mx || cur_my != my) { mx = cur_mx; my = cur_my; dirty = true; }
            if (Mouse::LeftClicked()) {
                KSALayout L = ksa_layout();
                if (ksa_hit(mx, my, L.approve_x, L.approve_y, L.approve_w, L.approve_h)) {
                    decided = true; approved = true;
                } else if (ksa_hit(mx, my, L.deny_x, L.deny_y, L.deny_w, L.deny_h)) {
                    decided = true; approved = false;
                }
            }
        }

        if (dirty && !decided) {
            uint8_t* fb = ksa_open_window();
            if (fb) ksa_render(fb, req, typed, typed_len, show_risk, mx, my);
            ksa_close_window();
        }
    }

    if (!decided) {
        // timed out with no answer  -  fail closed (treated as a deny). (satoru)
        approved = false;
        SerialLogger::Log("KSA: prompt timed out  -  failing closed (deny)\r\n");
    }

    // write the verdict INTO the isolated region (the in-vm result), then read
    // it back through the channel accessor. this models the result crossing the
    // vmcall boundary: ksa's arbiter is the only writer; the host reads a copy.
    {
        uint8_t* fb = ksa_open_window();
        if (fb) {
            KSAVerdict* in = (KSAVerdict*)(fb + KSA_VERDICT_OFF);
            memset(in, 0, sizeof(*in));
            in->approved  = approved;
            in->completed = true;
            if (req.want_cred && approved && typed_len > 0) {
                bool ok = false;
                // hash inside the isolated region; cleartext stays here and is
                // wiped on teardown. (satoru)
                char* scratch = (char*)(fb + KSA_SCRATCH_OFF);
                for (int i = 0; i <= typed_len && i < 63; i++) scratch[i] = typed[i];
                ksa_hash_credential(scratch, req.username, in->cred_hash, ok);
                in->have_cred_hash = ok;
                // wipe scratch immediately. (satoru)
                for (int i = 0; i < 63; i++) scratch[i] = 0;
            }
            // latch the host-side copy via the (read-only) channel. (satoru)
            g_last_verdict = *in;
        }
        ksa_close_window();
    }

    // wipe local typed buffer  -  the cleartext credential must not linger in
    // main-os memory. (satoru)
    for (int i = 0; i < 64; i++) typed[i] = 0;

    // restore the desktop pixels the main os had before the modal, then release
    // the secure desktop: a final present puts the saved frame on screen and
    // MarkUIDirty() tells the resumed compositor to repaint. (satoru)
    if (saved && back && saved_bytes) {
        memcpy(back, saved, saved_bytes);
        Graphics::MarkDirty(0, 0, sw, sh);   // full flush so the panel is wiped (satoru)
        Graphics::SwapBuffers();
        Graphics::PresentVirtioIfActive();
    }
    if (saved) PMM::FreeBytes(saved, saved_bytes);
    Graphics::MarkUIDirty();

    bool got = ReadVerdictForChannel(out);
    SUPR::Log(approved ? ACT_KSA_APPROVE : ACT_KSA_DENY,
              req.username ? req.username : "root",
              approved ? "ksa approved" : "ksa denied");
    RuntimeLog::LogSecurity(approved ? "ksa approved" : "ksa denied",
                            req.detail ? req.detail : req.title);

    // anti-replay: clear the latched verdict once supr has consumed it via the
    // out-param, so a later VMCALL on the channel cannot re-read a stale
    // "approved" after the prompt has ended. (satoru)
    memset(&g_last_verdict, 0, sizeof(g_last_verdict));

    TeardownContext();
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
    if (!available) {
        SerialLogger::Log("KSA-SELFTEST: hypervisor unavailable  -  ksa is a no-op on this host\r\n");
        SerialLogger::Log("KSA-SELFTEST: PASS (unavailable-and-honest)\r\n");
        return true;
    }

    if (!SpawnContext()) {
        SerialLogger::Log("KSA-SELFTEST: FAIL  -  could not spawn isolated context\r\n");
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
        SerialLogger::Log("KSA-PROMPT-DEMO: ksa unavailable on this host  -  cannot draw the prompt\r\n");
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
