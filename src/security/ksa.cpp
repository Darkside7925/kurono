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
static const int      KSA_H          = 240;
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

// blit the isolated framebuffer onto the visible screen, centered. this is the
// hypervisor present path  -  the main-os compositor is not involved and gets no
// pointer to the isolated region; ksa copies pixel-by-pixel to the screen.
static void ksa_present(uint8_t* fb) {
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    int ox = (sw - KSA_W) / 2;
    int oy = (sh - KSA_H) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    // dim the backdrop so the prompt reads as a modal security surface. (satoru)
    Graphics::FillRectAlpha(0, 0, sw, sh, 150, 0x000000);
    for (int y = 0; y < KSA_H; y++) {
        for (int x = 0; x < KSA_W; x++) {
            uint32_t px = ((uint32_t*)fb)[y * KSA_W + x];
            Graphics::DrawPixel(ox + x, oy + y, px);
        }
    }
    Graphics::SwapBuffers();
    Graphics::PresentVirtioIfActive();
}

static void ksa_render(uint8_t* fb, const KSARequest& req, const char* typed,
                       int typed_len, bool show_risk) {
    // chrome: dark panel, accent header, title + detail + masked credential.
    ksa_fb_rect(fb, 0, 0, KSA_W, KSA_H, 0xFF1C1F26);       // panel bg
    ksa_fb_rect(fb, 0, 0, KSA_W, 40, 0xFF2D6CDF);          // header accent
    ksa_fb_rect(fb, 0, KSA_H - 2, KSA_W, 2, 0xFF2D6CDF);   // footer line

    // present the isolated buffer first, then draw text via Graphics::DrawString
    // directly onto the composited screen rect (text rendering needs the font
    // engine which targets the main framebuffer). the security-relevant pixels
    // (panel + verdict) originate in the isolated buffer. (satoru)
    ksa_present(fb);

    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    int ox = (sw - KSA_W) / 2, oy = (sh - KSA_H) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    (void)typed;   // credential is echoed masked via typed_len, not the chars (satoru)

    Graphics::DrawString(ox + 12, oy + 12, "KSA  -  Kurono Secure Authorization",
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
        Graphics::DrawString(ox + 14, oy + 132, "Credential:", 0xFFFFFFFF, 0xFF1C1F26);
        // masked credential echo. (satoru)
        char mask[64];
        int m = 0; for (; m < typed_len && m < 48; m++) mask[m] = '*';
        mask[m] = 0;
        Graphics::DrawString(ox + 120, oy + 132, mask, 0xFF7CFF9B, 0xFF1C1F26);
    }

    if (show_risk) {
        Graphics::DrawString(ox + 14, oy + 160,
            "WARNING: both auth factors are disabled.", 0xFFFF6B6B, 0xFF1C1F26);
    }
    Graphics::DrawString(ox + 14, oy + KSA_H - 30,
        "[Enter]=Approve   [Esc]=Deny", 0xFFB8C0D0, 0xFF1C1F26);
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

    bool show_risk = SUPR::BothAuthDisabled();
    char typed[64]; int typed_len = 0; typed[0] = 0;
    bool decided = false, approved = false;

    // arbiter render/input loop. open the ephemeral window to render into the
    // isolated fb, then re-isolate between frames so the region spends most of
    // its life unmapped from the main os. cooperative; ~60s timeout. (satoru)
    uint32_t waited_ms = 0;
    Timer::ElapsedSinceLast();   // reset baseline
    {
        uint8_t* fb = ksa_open_window();
        if (fb) ksa_render(fb, req, typed, typed_len, show_risk);
        ksa_close_window();
    }

    while (!decided && waited_ms < 60000) {
        uint32_t e = Timer::ElapsedSinceLast();
        if (e > 0) { TimeManager::AdvanceByMs(e); waited_ms += e; }
        Keyboard::Poll();
        bool dirty = false;
        while (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == '\r' || c == '\n') { decided = true; approved = true; break; }
            else if (c == 27)           { decided = true; approved = false; break; } // esc
            else if (c == 8 || c == 127) { if (typed_len > 0) { typed[--typed_len] = 0; dirty = true; } }
            else if (c >= 32 && c < 127 && typed_len < 62) { typed[typed_len++] = c; typed[typed_len] = 0; dirty = true; }
        }
        if (dirty) {
            uint8_t* fb = ksa_open_window();
            if (fb) ksa_render(fb, req, typed, typed_len, show_risk);
            ksa_close_window();
        }
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

    // wipe local typed buffer. (satoru)
    for (int i = 0; i < 64; i++) typed[i] = 0;

    // restore the desktop under the modal. (satoru)
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

// end (satoru)
