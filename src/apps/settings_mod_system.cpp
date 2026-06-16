//  kurono os  -  settings module: system & updates (satoru)
//  a rich "System & Updates" page: an about banner with the live kurono
//  version / kernel build / cpu brand / total memory / hostname, a software
//  updates section (auto-update toggle + repo server + last-sync + pending
//  counts + a check-for-updates action), a linux subsystem section (enable
//  toggle + boot-profile selector + live guest state), and a packages section
//  (installed / available / update counts + a few real package names). all
//  toggles persist through UIConfig and apply live where an api exists. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/cpu_detect.h"
#include "../system/ui_config.h"
#include "../system/system_update.h"
#include "../linux/dual_boot.h"
#include "../linux/linux_kernel.h"
#include "../linux/kls.h"
#include "../packages/pkgmgr.h"
#include "../kernel/pmm.h"
#include "../kernel/kmemx.h"   // memory-compression status + toggle + sliders (satoru)

static const char* KURONO_VERSION = "Kurono OS 1.0.0";

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
static bool s_auto_update    = true;   // persisted to system.auto_update (satoru)
static bool s_linux_enabled  = true;   // persisted to system.linux_enabled (satoru)
static int  s_boot_profile   = 0;      // persisted to system.boot_profile, == BootMode (satoru)

// ── helpers ─────────────────────────────────────────────────────────────────
// human-readable total physical memory: prefer MB, fall back to KB. (satoru)
static void mem_total_str(char* b, int mx){
    uint64_t bytes = PMM::GetTotalMemory();
    uint64_t mb = bytes / (1024ULL * 1024ULL);
    if(mb >= 1){
        SettingsUI::IntToStr((int)mb, b, mx);
        SettingsUI::StrApp(b, " MB", mx);
    } else {
        SettingsUI::IntToStr((int)(bytes / 1024ULL), b, mx);
        SettingsUI::StrApp(b, " KB", mx);
    }
}

// live guest run-state label from the kls state machine. (satoru)
static const char* guest_state_label(){
    switch(KLS::GetState()){
        case KLS_STOPPED:      return "Stopped";
        case KLS_INITIALIZING: return "Initializing";
        case KLS_RUNNING:      return "Running";
        case KLS_ERROR:        return "Error";
        case KLS_SUSPENDED:    return "Suspended";
        default:               return "Unknown";
    }
}

// hostname comes from the kls config blob (mirrors /etc/hostname). (satoru)
static const char* host_name(){
    KLSConfig* c = KLS::GetConfig();
    if(c && c->hostname[0]) return c->hostname;
    return "kurono";
}

// apply the linux enable intent live: start/stop the subsystem. (satoru)
static void apply_linux_enabled(){
    UIConfig::SetInt("system.linux_enabled", s_linux_enabled ? 1 : 0, true);
    if(s_linux_enabled){
        if(KLS::GetState() == KLS_STOPPED) KLS::Start();
    } else {
        if(KLS::GetState() == KLS_RUNNING) KLS::Stop();
    }
    UIConfig::Save();
}

// apply the boot-profile intent live through the dual-boot manager. (satoru)
static void apply_boot_profile(){
    if(s_boot_profile < 0) s_boot_profile = 0;
    if(s_boot_profile > BOOT_STANDALONE_KURONO) s_boot_profile = BOOT_STANDALONE_KURONO;
    UIConfig::SetInt("system.boot_profile", s_boot_profile, true);
    DualBootManager::SetBootMode((BootMode)s_boot_profile);
    UIConfig::Save();
}

static void persist_auto_update(){
    UIConfig::SetInt("system.auto_update", s_auto_update ? 1 : 0, true);
    UIConfig::Save();
}

// ── on_show: (re)load config + (re)detect subsystem state ───────────────────
static void system_on_show(){
    s_auto_update   = UIConfig::Int("system.auto_update",   s_auto_update   ? 1 : 0) != 0;
    s_linux_enabled = UIConfig::Int("system.linux_enabled", s_linux_enabled ? 1 : 0) != 0;
    // seed the boot profile from config, else from the live dual-boot mode. (satoru)
    int bp = UIConfig::Int("system.boot_profile", -1);
    if(bp < 0) bp = (int)DualBootManager::GetBootMode();
    if(bp < 0) bp = 0;
    if(bp > BOOT_STANDALONE_KURONO) bp = BOOT_STANDALONE_KURONO;
    s_boot_profile = bp;
}

// ── layout constants shared by render + input so hit-testing matches the
//    drawing exactly (the input() signature gives no pane width, so we fix the
//    control geometry here rather than derive it from w). (satoru)
static const int CTRL_X_OFF = 180;   // controls column, relative to pane left (satoru)
static const int CTRL_W     = 300;   // dropdown width in px (satoru)
static const int BTN_W      = 170;   // action-button width in px (satoru)
static const int BTN_H      = 26;    // action-button height in px (satoru)

static int ctrl_w_for(int pane_w){
    int avail = pane_w - CTRL_X_OFF;
    int cw = CTRL_W;
    if(cw > avail) cw = avail;
    if(cw < 80) cw = 80;
    return cw;
}

// name of the boot profile currently selected (for the pill). (satoru)
static const char* boot_profile_name(){
    return DualBootManager::BootModeName((BootMode)s_boot_profile);
}

static void system_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int ctrl_w = ctrl_w_for(w);
    int ly = y - scroll + 8;
    char buf[80];

    // ── identity banner ──────────────────────────────────────────────────
    Graphics::FillRoundedRect(x, ly, 40, 40, 10, SettingsUI::Accent());
    Graphics::DrawString(x + 14, ly + 13, "K", SettingsUI::COL_WHITE, 0xFF000000);
    Graphics::DrawString(x + 52, ly + 6,  KURONO_VERSION, SettingsUI::COL_WHITE, 0xFF000000);
    Graphics::DrawString(x + 52, ly + 24, "System & Updates", SettingsUI::COL_DIM, 0xFF000000);
    ly += 56;

    // ── about this system ────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "About This System");
    ly += 26;
    SettingsUI::Row(x, ly, "Version:", KURONO_VERSION);
    ly += 22;
    {
        // kernel build = kurono microkernel + linux abi level. (satoru)
        SettingsUI::StrCpy(buf, "Microkernel + Linux ", 80);
        SettingsUI::StrApp(buf, LinuxKernel::GetVersion(), 80);
        SettingsUI::Row(x, ly, "Kernel:", buf);
    }
    ly += 22;
    {
        const char* brand = CPUDetect::GetBrandString();
        SettingsUI::Row(x, ly, "Processor:",
                        (brand && brand[0]) ? brand : CPUDetect::GetVendorName());
    }
    ly += 22;
    mem_total_str(buf, 80);
    SettingsUI::Row(x, ly, "Memory:", buf);
    ly += 22;
    SettingsUI::Row(x, ly, "Hostname:", host_name());
    ly += 30;

    // ── software updates ─────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Software Updates");
    ly += 26;
    Graphics::DrawString(x, ly + 2, "Automatic Updates:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_auto_update);
    // honest: no background scheduler consumes system.auto_update yet; it is a
    // saved preference, and "Check for Updates" below syncs on demand. (satoru)
    Graphics::DrawString(ctrl_x + SettingsUI::TOGGLE_W + 10, ly + 2,
                         s_auto_update ? "Preference (manual sync below)" : "Off",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 32;
    SettingsUI::Row(x, ly, "Update Server:", PackageManager::GetRepositoryHost());
    ly += 22;
    {
        const char* msg = PackageManager::GetLastSyncMessage();
        SettingsUI::Row(x, ly, "Last Sync:", (msg && msg[0]) ? msg : "Never synced");
    }
    ly += 22;
    {
        SettingsUI::StrCpy(buf, PackageManager::LastSyncSucceeded() ? "OK" : "Stale", 80);
        SettingsUI::Row(x, ly, "Sync Status:", buf);
    }
    ly += 22;
    {
        int pend = PackageManager::GetPendingUpdateCount();
        SettingsUI::IntToStr(pend, buf, 80);
        SettingsUI::StrApp(buf, pend == 1 ? " update available" : " updates available", 80);
        SettingsUI::Row(x, ly, "Pending:", buf);
    }
    ly += 22;
    SettingsUI::Row(x, ly, "Reboot Pending:",
                    SystemUpdate::HasPendingUpdate() ? "Yes  -  restart to apply" : "No");
    ly += 22;
    {
        // check-for-updates action button (a plain pill we hit-test by rect). (satoru)
        unsigned int accent = SettingsUI::Accent();
        Graphics::FillRoundedRect(ctrl_x, ly, BTN_W, BTN_H, 6, accent);
        Graphics::DrawString(ctrl_x + 18, ly + 6, "Check for Updates",
                             SettingsUI::COL_WHITE, 0xFF000000);
    }
    ly += 38;

    // ── linux subsystem ──────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Linux Subsystem");
    ly += 26;
    Graphics::DrawString(x, ly + 2, "Enable Subsystem:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_linux_enabled);
    ly += 32;
    Graphics::DrawString(x, ly + 4, "Boot Profile:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(ctrl_x, ly, ctrl_w, boot_profile_name());
    ly += 32;
    SettingsUI::Row(x, ly, "Guest State:", guest_state_label());
    ly += 22;
    SettingsUI::Row(x, ly, "Guest Kernel:", KLS::GetKernelVersion());
    ly += 22;
    {
        SettingsUI::IntToStr(KLS::LinuxProcessCount(), buf, 80);
        SettingsUI::StrApp(buf, " running", 80);
        SettingsUI::Row(x, ly, "Linux Processes:", buf);
    }
    ly += 30;

    // ── packages ─────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Packages");
    ly += 26;
    SettingsUI::IntToStr(PackageManager::InstalledCount(), buf, 80);
    SettingsUI::Row(x, ly, "Installed:", buf);
    ly += 22;
    SettingsUI::IntToStr(PackageManager::AvailableCount(), buf, 80);
    SettingsUI::Row(x, ly, "Available:", buf);
    ly += 22;
    {
        SettingsUI::IntToStr(PackageManager::GetPackageCount(), buf, 80);
        SettingsUI::StrApp(buf, " total in repository", 80);
        SettingsUI::Row(x, ly, "Repository:", buf);
    }
    ly += 22;

    // a few real package names with their version + state. (satoru)
    {
        int total = PackageManager::GetPackageCount();
        int shown = 0;
        for(int i = 0; i < total && shown < 4; i++){
            Package* p = PackageManager::GetPackage(i);
            if(!p || !p->name[0]) continue;
            SettingsUI::StrCpy(buf, p->version[0] ? p->version : " - ", 80);
            SettingsUI::StrApp(buf, p->state == PKG_INSTALLED ? " (installed)" : " (available)", 80);
            SettingsUI::Row(x, ly, p->name, buf);
            ly += 22;
            shown++;
        }
    }
    ly += 8;

    // ── memory compression (KMemX) ───────────────────────────────────────
    // physical ram, pool size, ratio, pages compressed; a toggle; a pool-size
    // slider (10-40% RAM) and an aggressiveness slider (generation threshold
    // 4-16, shown as a 0-100 fill). a usage indicator approximates physical-vs-
    // logical (the spec's real-time graph reduced to a one-line gauge here). (satoru)
    SettingsUI::SectionHeader(x, ly, "Memory Compression (KMemX)");
    ly += 26;
    {
        const KMemX::Stats& st = KMemX::GetStats();
        // physical ram. (satoru)
        mem_total_str(buf, 80);
        SettingsUI::Row(x, ly, "Physical RAM:", buf);
        ly += 22;
        // pool size. (satoru)
        SettingsUI::IntToStr((int)(st.pool_bytes / (1024 * 1024)), buf, 80);
        SettingsUI::StrApp(buf, " MB", 80);
        SettingsUI::Row(x, ly, "Pool Size:", buf);
        ly += 22;
        // compression ratio (x.yy:1). (satoru)
        {
            int r = KMemX::RatioX100();
            SettingsUI::IntToStr(r / 100, buf, 80);
            SettingsUI::StrApp(buf, ".", 80);
            char frac[8]; SettingsUI::IntToStr(r % 100, frac, 8);
            SettingsUI::StrApp(buf, frac, 80);
            SettingsUI::StrApp(buf, ":1", 80);
            SettingsUI::Row(x, ly, "Compression:", buf);
        }
        ly += 22;
        // pages compressed (live). (satoru)
        SettingsUI::IntToStr((int)st.live_pages, buf, 80);
        SettingsUI::StrApp(buf, " pages", 80);
        SettingsUI::Row(x, ly, "Compressed:", buf);
        ly += 22;
        // RAM saved. (satoru)
        SettingsUI::IntToStr((int)(st.bytes_saved / (1024 * 1024)), buf, 80);
        SettingsUI::StrApp(buf, " MB saved", 80);
        SettingsUI::Row(x, ly, "RAM Saved:", buf);
        ly += 26;

        // enable toggle. (satoru)
        Graphics::DrawString(x, ly + 2, "Enable KMemX:", SettingsUI::COL_TEXT, 0xFF000000);
        SettingsUI::Toggle(ctrl_x, ly, KMemX::IsEnabled());
        Graphics::DrawString(ctrl_x + SettingsUI::TOGGLE_W + 10, ly + 2,
                             KMemX::IsEnabled() ? "On (compressing inactive memory)" : "Off (full RAM mode)",
                             SettingsUI::COL_DIM, 0xFF000000);
        ly += 32;

        // pool-size slider: map 10..40% -> 0..100 fill. (satoru)
        Graphics::DrawString(x, ly - 2, "Pool Size (10-40%):", SettingsUI::COL_TEXT, 0xFF000000);
        {
            int pct = KMemX::PoolPct();
            int fill = ((pct - 10) * 100) / 30;
            SettingsUI::Slider(ctrl_x, ly + 2, ctrl_w, fill);
            char pb[8]; SettingsUI::IntToStr(pct, pb, 8);
            Graphics::DrawString(ctrl_x + ctrl_w + 8, ly - 2, pb, SettingsUI::COL_DIM, 0xFF000000);
        }
        ly += 26;

        // aggressiveness slider: generation threshold 4..16 -> 0..100 fill. a
        // LOWER threshold == more aggressive, so we invert the fill. (satoru)
        Graphics::DrawString(x, ly - 2, "Aggressiveness:", SettingsUI::COL_TEXT, 0xFF000000);
        {
            int thr = KMemX::Threshold();
            int fill = 100 - (((thr - 4) * 100) / 12);
            SettingsUI::Slider(ctrl_x, ly + 2, ctrl_w, fill);
        }
        ly += 26;

        // physical-vs-logical usage gauge: pool_used / pool_bytes. (satoru)
        Graphics::DrawString(x, ly - 2, "Pool Usage:", SettingsUI::COL_TEXT, 0xFF000000);
        {
            int used_pct = st.pool_bytes ? (int)((st.pool_used * 100) / st.pool_bytes) : 0;
            SettingsUI::Slider(ctrl_x, ly + 2, ctrl_w, used_pct);
        }
        ly += 26;
    }
}

// ── input: pane-local mx,my. we walk the SAME running-y layout as render
//    (already offset by -scroll), so the hit rects line up exactly. (satoru)
static bool system_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ly = -scroll + 8;

    ly += 56;          // identity banner (satoru)
    ly += 26;          // "About This System" header (satoru)
    ly += 22;          // version row (satoru)
    ly += 22;          // kernel row (satoru)
    ly += 22;          // processor row (satoru)
    ly += 22;          // memory row (satoru)
    ly += 22;          // hostname row (satoru)
    ly += 30;          // tail after about (satoru)
    ly += 26;          // "Software Updates" header (satoru)

    // automatic-updates toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_auto_update = !s_auto_update;
        persist_auto_update();
        return true;
    }
    ly += 32;          // auto-update row (satoru)
    ly += 22;          // update-server row (satoru)
    ly += 22;          // last-sync row (satoru)
    ly += 22;          // sync-status row (satoru)
    ly += 22;          // pending row (satoru)
    ly += 22;          // reboot-pending row (satoru)

    // check-for-updates button (manual rect hit, matching the drawn pill). (satoru)
    if(mx >= ctrl_x && mx <= ctrl_x + BTN_W && my >= ly && my <= ly + BTN_H){
        PackageManager::SyncRepository();   // refresh repo + last-sync state (satoru)
        return true;
    }
    ly += 38;          // button row (satoru)
    ly += 26;          // "Linux Subsystem" header (satoru)

    // enable-subsystem toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_linux_enabled = !s_linux_enabled;
        apply_linux_enabled();
        return true;
    }
    ly += 32;          // enable row (satoru)

    // boot-profile dropdown  -  cycles through the four boot modes. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, CTRL_W, mx, my);
        if(hit >= 0){
            s_boot_profile += (hit == 0) ? -1 : 1;
            if(s_boot_profile < 0) s_boot_profile = BOOT_STANDALONE_KURONO;
            if(s_boot_profile > BOOT_STANDALONE_KURONO) s_boot_profile = 0;
            apply_boot_profile();
            return true;
        }
    }
    ly += 32;          // boot-profile dropdown row (satoru)
    ly += 22;          // guest-state row (satoru)
    ly += 22;          // guest-kernel row (satoru)
    ly += 22;          // linux-processes row (satoru)
    ly += 30;          // tail after linux subsystem (satoru)

    // ── packages section (read-only; walk its advances to reach memory) ──
    ly += 26;          // "Packages" header (satoru)
    ly += 22;          // installed row (satoru)
    ly += 22;          // available row (satoru)
    ly += 22;          // repository row (satoru)
    {
        // mirror render's "up to 4 real package rows" advance exactly. (satoru)
        int total = PackageManager::GetPackageCount();
        int shown = 0;
        for(int i = 0; i < total && shown < 4; i++){
            Package* p = PackageManager::GetPackage(i);
            if(!p || !p->name[0]) continue;
            ly += 22;
            shown++;
        }
    }
    ly += 8;

    // ── memory compression (KMemX) controls ──────────────────────────────
    // the input signature has no pane width; mirror the existing dropdown hit
    // convention and use the fixed CTRL_W for the slider tracks (render clamps to
    // the same value on any normally-wide pane). (satoru)
    int ctrl_w = CTRL_W;
    ly += 26;          // "Memory Compression (KMemX)" header (satoru)
    ly += 22;          // physical ram row (satoru)
    ly += 22;          // pool size row (satoru)
    ly += 22;          // compression ratio row (satoru)
    ly += 22;          // compressed pages row (satoru)
    ly += 26;          // ram saved row (satoru)

    // enable toggle: flips KMemX live (Enable decompresses-all on its own). (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        if(KMemX::IsEnabled()) KMemX::Disable(); else KMemX::Enable();
        return true;
    }
    ly += 32;          // enable toggle row (satoru)

    // pool-size slider: 0..100 fill -> 10..40%. (satoru)
    {
        int hit = SettingsUI::SliderHit(ctrl_x, ly + 2, ctrl_w, mx, my);
        if(hit >= 0){
            int pct = 10 + (hit * 30) / 100;
            if(pct < 10) pct = 10; if(pct > 40) pct = 40;
            KMemX::SetPoolPct(pct);
            UIConfig::SetInt("kmemx.pool_pct", pct, true);
            UIConfig::Save();
            KMemX::WriteConfFile();
            return true;
        }
    }
    ly += 26;          // pool slider row (satoru)

    // aggressiveness slider: 0..100 fill (high == aggressive) -> threshold 16..4. (satoru)
    {
        int hit = SettingsUI::SliderHit(ctrl_x, ly + 2, ctrl_w, mx, my);
        if(hit >= 0){
            int thr = 16 - (hit * 12) / 100;
            if(thr < 4) thr = 4; if(thr > 16) thr = 16;
            KMemX::SetThreshold(thr);
            UIConfig::SetInt("kmemx.threshold", thr, true);
            UIConfig::Save();
            KMemX::WriteConfFile();
            return true;
        }
    }
    ly += 26;          // aggressiveness slider row (satoru)
    ly += 26;          // pool-usage gauge row (read-only) (satoru)
    return false;
}

// total content height for the scrollbar (sum of the row advances + tail). (satoru)
static int system_content_height(){
    int h = 8 + 56;                       // pad + banner (satoru)
    h += 26 + 22 + 22 + 22 + 22 + 22 + 30; // about this system (satoru)
    h += 26 + 32 + 22 + 22 + 22 + 22 + 22 + 38; // software updates (satoru)
    h += 26 + 32 + 32 + 22 + 22 + 30;     // linux subsystem (satoru)
    h += 26 + 22 + 22 + 22 + (4 * 22);    // packages + up to 4 package rows (satoru)
    h += 8;                               // gap before memory section (satoru)
    h += 26 + 22 + 22 + 22 + 22 + 26;     // memory: header + 5 info rows (satoru)
    h += 32 + 26 + 26 + 26;               // memory: toggle + 3 sliders/gauge (satoru)
    h += 16;                              // tail (satoru)
    return h;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_system_module;` resolves at link time. (satoru)
extern const SettingsModule g_system_module = {
    "system", "System & Updates", "\x13",
    system_on_show, system_render, system_input, system_content_height
};
// end (satoru)
