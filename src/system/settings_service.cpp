// settings_service.cpp: the kernel side of the ring-3 Settings app. one unix socket,
// a validated key/value store over ui.conf, read-only system info for the pages, and
// queued live actions applied on the gui thread. privileged actions call AdminGate. (satoru)
#include "settings_service.h"
#include "settings_proto.h"
#include "ui_config.h"
#include "../net/unix_socket.h"
#include "../net/network.h"
#include "../net/dhcp.h"
#include "../drivers/e1000.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../security/admin.h"
#include "../security/ken.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/kmemx.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/display_mgr.h"
#include "../drivers/outputs.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/hda.h"
#include "../drivers/blockdev.h"
#include "../drivers/usb.h"
#include "../drivers/disklog.h"
#include "../drivers/keyboard.h"
#include "../hal/cpufreq.h"
#include "../ui/desktop.h"
#include "../ui/window_manager.h"
#include "../ui/style.h"
#include "../ui/frame_pacer.h"
#include "../ui/ui_elements.h"
#include "../system/user_mgmt.h"
#include "../system/system_update.h"
#include "../linux/kls.h"
#include "../linux/linux_kernel.h"
#include "../linux/linux_drivers.h"
#include "../packages/pkgmgr.h"
#include "../virt/vm_host.h"
#include "../virt/vmm.h"
#include "../virt/hypervisor.h"
#include "../apps/alpine_term.h"
#include "../proc/spinlock.h"

namespace {

// ---- small string helpers (satoru)
int s_len(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
bool s_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
bool s_prefix(const char* s, const char* p) {
    if (!s || !p) return false;
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}
void s_copy(char* d, const char* s, int mx) {
    int i = 0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
void s_append(char* d, const char* s, int mx) {
    int i = 0;
    while (i < mx - 1 && d[i]) i++;
    if (s) while (*s && i < mx - 1) d[i++] = *s++;
    d[i] = 0;
}
void s_num(char* d, int mx, long long v) {
    char t[24]; int n = 0;
    bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    if (!u) t[n++] = '0';
    while (u && n < 23) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    int i = 0;
    if (neg && i < mx - 1) d[i++] = '-';
    while (n > 0 && i < mx - 1) d[i++] = t[--n];
    d[i] = 0;
}

// ---- the two tables sent to ring 3 (satoru)
struct Table { uint32_t count; KssEntry entries[KSS_INFO_MAX]; };
Table g_cfg_all;
Table g_info_all;
KssTable g_wire;

void tbl_clear(Table* t) { t->count = 0; }
void tbl_add(Table* t, const char* k, const char* v) {
    if (t->count >= KSS_INFO_MAX) return;
    KssEntry* e = &t->entries[t->count++];
    s_copy(e->key, k, KSS_KEY_MAX);
    s_copy(e->val, v, KSS_VAL_MAX);
}
void tbl_int(Table* t, const char* k, long long v) {
    char b[KSS_VAL_MAX]; s_num(b, sizeof(b), v); tbl_add(t, k, b);
}
void tbl_key_int(Table* t, const char* prefix, int i, const char* suffix, long long v) {
    char k[KSS_KEY_MAX];
    s_copy(k, prefix, KSS_KEY_MAX);
    char nb[8]; s_num(nb, sizeof(nb), i);
    s_append(k, nb, KSS_KEY_MAX);
    s_append(k, suffix, KSS_KEY_MAX);
    tbl_int(t, k, v);
}
void tbl_key_str(Table* t, const char* prefix, int i, const char* suffix, const char* v) {
    char k[KSS_KEY_MAX];
    s_copy(k, prefix, KSS_KEY_MAX);
    char nb[8]; s_num(nb, sizeof(nb), i);
    s_append(k, nb, KSS_KEY_MAX);
    s_append(k, suffix, KSS_KEY_MAX);
    tbl_add(t, k, v ? v : "");
}

// ---- config load (satoru)
void build_cfg() {
    tbl_clear(&g_cfg_all);
    static char buf[8192];
    int n = KVFS::ReadFile(UIConfig::Path(), buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    int i = 0;
    while (i < n) {
        int e = i;
        while (e < n && buf[e] != '\n') e++;
        char line[136];
        int ln = 0;
        while (i < e && ln < (int)sizeof(line) - 1) line[ln++] = buf[i++];
        line[ln] = 0;
        i = e + 1;
        int p = 0;
        while (line[p] == ' ' || line[p] == '\t' || line[p] == '\r') p++;
        if (!line[p] || line[p] == '#') continue;
        char key[KSS_KEY_MAX];
        int ki = 0;
        while (line[p] && line[p] != '=' && line[p] != ' ' && line[p] != '\t' && ki < KSS_KEY_MAX - 1)
            key[ki++] = line[p++];
        key[ki] = 0;
        while (line[p] == ' ' || line[p] == '\t') p++;
        if (!ki || line[p] != '=') continue;
        p++;
        while (line[p] == ' ' || line[p] == '\t') p++;
        char val[KSS_VAL_MAX];
        int vi = 0;
        while (line[p] && line[p] != '#' && vi < KSS_VAL_MAX - 1) val[vi++] = line[p++];
        while (vi > 0 && (val[vi - 1] == ' ' || val[vi - 1] == '\t' || val[vi - 1] == '\r')) vi--;
        val[vi] = 0;
        if (vi) tbl_add(&g_cfg_all, key, val);
    }
}

// ---- validation (satoru)
enum ValType { V_INT, V_BOOL, V_COLOR, V_ENUM, V_TEXT };

struct KeyRule { const char* key; ValType type; int lo, hi; const char* allowed; };

const KeyRule kRules[] = {
    { "ui.appearance",            V_ENUM,  0, 0, "light,dark" },
    { "ui.style",                 V_ENUM,  0, 0, "modern,classic" },
    { "ui.icons",                 V_ENUM,  0, 0, "colorful,minimal" },
    { "theme.accent",             V_COLOR, 0, 0, nullptr },
    { "display.ui_scale",         V_INT, 100, 200, nullptr },
    { "display.brightness",       V_INT,   0, 100, nullptr },
    { "display.vsync",            V_BOOL,  0, 1, nullptr },
    { "display.refresh_mode",     V_INT,   0, 360, nullptr },
    { "display.refresh_hz",       V_INT,  24, 360, nullptr },
    { "display.adaptive_sync",    V_BOOL,  0, 1, nullptr },
    { "desktop.wallpaper_index",  V_INT,   0, 255, nullptr },
    { "desktop.bg",               V_COLOR, 0, 0, nullptr },
    { "desktop.icon_size",        V_INT,  32, 96, nullptr },
    { "compositor.window_animations",  V_BOOL, 0, 1, nullptr },
    { "compositor.animation_speed_ms", V_INT, 60, 400, nullptr },
    { "compositor.shadow_enabled",     V_BOOL, 0, 1, nullptr },
    { "compositor.shadow_during_drag", V_BOOL, 0, 1, nullptr },
    { "compositor.reduced_motion",     V_BOOL, 0, 1, nullptr },
    { "taskbar.position",         V_ENUM,  0, 0, "bottom,top" },
    { "taskbar.height",           V_INT,  32, 72, nullptr },
    { "taskbar.bg",               V_COLOR, 0, 0, nullptr },
    { "taskbar.start_btn_bg",     V_COLOR, 0, 0, nullptr },
    { "taskbar.start_btn_hover",  V_COLOR, 0, 0, nullptr },
    { "a11y.high_contrast",       V_BOOL,  0, 1, nullptr },
    { "a11y.sticky_keys",         V_BOOL,  0, 1, nullptr },
    { "a11y.slow_keys",           V_BOOL,  0, 1, nullptr },
    { "a11y.bounce_keys",         V_BOOL,  0, 1, nullptr },
    { "a11y.screen_reader",       V_BOOL,  0, 1, nullptr },
    { "a11y.color_filter",        V_INT,   0, 4, nullptr },
    { "audio.master_volume",      V_INT,   0, 100, nullptr },
    { "audio.muted",              V_BOOL,  0, 1, nullptr },
    { "power.plan",               V_INT,   0, 2, nullptr },
    { "net.wifi_enabled",         V_BOOL,  0, 1, nullptr },
    { "system.linux_enabled",     V_BOOL,  0, 1, nullptr },
    { "kmemx.pool_pct",           V_INT,  10, 40, nullptr },
    { "kmemx.threshold",          V_INT,   4, 16, nullptr },
    { "linux.enabled",            V_BOOL,  0, 1, nullptr },
    { "linux.autostart",          V_BOOL,  0, 1, nullptr },
    { "linux.distro",             V_ENUM,  0, 0, "alpine,debian" },
    { "linux.ram_mb",             V_INT, 256, 8192, nullptr },
};

const KeyRule* rule_for(const char* key) {
    for (unsigned i = 0; i < sizeof(kRules) / sizeof(kRules[0]); i++)
        if (s_eq(kRules[i].key, key)) return &kRules[i];
    if (s_prefix(key, "display.out") && key[11] >= '0' && key[11] <= '9') {
        static KeyRule out = { "", V_TEXT, 0, 0, nullptr };
        out.key = key;
        return &out;
    }
    if (s_prefix(key, "linux.dock") && key[10] >= '0' && key[10] <= '7') {
        static KeyRule dock = { "", V_TEXT, 0, 0, nullptr };
        dock.key = key;
        return &dock;
    }
    return nullptr;
}

int parse_int(const char* s, bool* ok) {
    int i = 0, sign = 1, v = 0;
    if (!s || !s[0]) { *ok = false; return 0; }
    if (s[0] == '-') { sign = -1; i = 1; }
    if (!s[i]) { *ok = false; return 0; }
    while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    *ok = s[i] == 0;
    return v * sign;
}

bool validate(const KeyRule* r, const char* val) {
    if (!r || !val) return false;
    if (s_len(val) >= KSS_VAL_MAX) return false;
    switch (r->type) {
    case V_INT: {
        bool ok = false;
        int v = parse_int(val, &ok);
        return ok && v >= r->lo && v <= r->hi;
    }
    case V_BOOL:
        return s_eq(val, "0") || s_eq(val, "1") || s_eq(val, "true") || s_eq(val, "false");
    case V_COLOR: {
        if (!(val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))) return false;
        int n = 0;
        for (int i = 2; val[i]; i++) {
            char c = val[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            n++;
        }
        return n >= 1 && n <= 8;
    }
    case V_ENUM: {
        const char* a = r->allowed;
        while (a && *a) {
            const char* e = a;
            while (*e && *e != ',') e++;
            int n = (int)(e - a);
            int k = 0;
            while (k < n && val[k] && val[k] == a[k]) k++;
            if (k == n && !val[k]) return true;
            a = *e ? e + 1 : e;
        }
        return false;
    }
    case V_TEXT:
    default:
        for (int i = 0; val[i]; i++) {
            unsigned char c = (unsigned char)val[i];
            if (c < 0x20 || c == 0x7f) return false;
        }
        return true;
    }
}

// ---- action queue, applied on the gui thread (satoru)
const int ACT_N = 12;
KssReq g_act[ACT_N];
volatile int g_act_head = 0, g_act_tail = 0;
Spinlock g_lock;

void queue_action(const KssReq& r) {
    SpinLockGuard g(g_lock);
    int next = (g_act_head + 1) % ACT_N;
    if (next == g_act_tail) return;   // full: drop, the service is best effort (satoru)
    g_act[g_act_head] = r;
    __atomic_store_n(&g_act_head, next, __ATOMIC_RELEASE);
}

bool action_wants_gate(const char* a) {
    return s_eq(a, "user.add") || s_eq(a, "kls.start") || s_eq(a, "kls.stop") ||
           s_eq(a, "pkg.sync") || s_prefix(a, "kmemx.") || s_prefix(a, "vm.") ||
           s_prefix(a, "display.") || s_eq(a, "open.alpine");
}

void apply_action(const KssReq& r) {
    const char* a = r.action;
    if (s_eq(a, "ui.style")) {
        UIStyle::Set(r.uval != 0);
    } else if (s_eq(a, "ui.icons")) {
        UIStyle::SetMinimalIcons(r.uval != 0);
    } else if (s_eq(a, "ui.scale")) {
        Graphics::RequestUIScale(r.ival);
    } else if (s_eq(a, "wm.reload")) {
        WindowManager::ReloadFromConfig();
    } else if (s_eq(a, "desktop.reload")) {
        Desktop::ReloadFromConfig();
    } else if (s_eq(a, "deskenv.reload")) {
        DesktopEnvironment::ReloadFromConfig();
    } else if (s_eq(a, "taskbar.reload")) {
        Taskbar::ReloadFromConfig();
    } else if (s_eq(a, "wallpaper.builtin")) {
        Desktop::ApplyBuiltinWallpaper(r.ival);
    } else if (s_eq(a, "wallpaper.color")) {
        Desktop::SetWallpaper(r.uval);
    } else if (s_eq(a, "a11y.contrast")) {
        Graphics::SetHighContrast(r.uval != 0);
        Widget::high_contrast_mode = r.uval != 0;
    } else if (s_eq(a, "a11y.filter")) {
        Graphics::SetColorFilter(r.ival);
    } else if (s_eq(a, "kbd.sticky")) {
        Keyboard::SetStickyKeys(r.uval != 0);
    } else if (s_eq(a, "kbd.slow")) {
        Keyboard::SetSlowKeys(r.ival > 0 ? 250u : 0u);
    } else if (s_eq(a, "kbd.bounce")) {
        Keyboard::SetBounceKeys(r.ival > 0 ? 200u : 0u);
    } else if (s_eq(a, "kbd.reader")) {
        Keyboard::SetScreenReader(r.uval != 0);
    } else if (s_eq(a, "audio.volume")) {
        AudioMixer::SetMasterVolume(r.ival);
        if (Audio::IsAvailable()) Audio::SetMasterVolume(r.ival);
    } else if (s_eq(a, "audio.mute")) {
        AudioMixer::SetMasterMute(r.uval != 0);
        if (Audio::IsAvailable()) Audio::SetMuted(r.uval != 0);
    } else if (s_eq(a, "audio.tone")) {
        AudioServer::PlayTone(660, 140, 70);
    } else if (s_eq(a, "cpufreq.plan")) {
        int n = CPUFreq::CPUCount();
        CPUFreq::Governor g = r.ival == 0 ? CPUFreq::GOV_PERFORMANCE
                            : (r.ival == 2 ? CPUFreq::GOV_POWERSAVE : CPUFreq::GOV_ONDEMAND);
        for (int i = 0; i < n; i++) CPUFreq::SetGovernor((uint32_t)i, g);
    } else if (s_eq(a, "kls.start")) {
        if (KLS::GetState() == KLS_STOPPED) KLS::Start();
    } else if (s_eq(a, "kls.stop")) {
        if (KLS::GetState() == KLS_RUNNING) KLS::Stop();
    } else if (s_eq(a, "pkg.sync")) {
        PackageManager::SyncRepository();
    } else if (s_eq(a, "kmemx.enable")) {
        KMemX::Enable();
    } else if (s_eq(a, "kmemx.disable")) {
        KMemX::Disable();
    } else if (s_eq(a, "kmemx.pool")) {
        KMemX::SetPoolPct(r.ival);
        KMemX::WriteConfFile();
    } else if (s_eq(a, "kmemx.threshold")) {
        KMemX::SetThreshold(r.ival);
        KMemX::WriteConfFile();
    } else if (s_eq(a, "vm.start")) {
        VMHost::Start(VMHost::SelectedDistro(), 0);
    } else if (s_eq(a, "vm.stop")) {
        VMHost::Stop();
    } else if (s_eq(a, "vm.restart")) {
        VMHost::Restart();
    } else if (s_eq(a, "vm.kill")) {
        VMHost::Kill();
    } else if (s_eq(a, "vm.agent")) {
        VMHost::AgentSend(r.arg);
    } else if (s_eq(a, "display.brightness")) {
        DisplayManager::SetBrightness((uint8_t)((r.ival * 255) / 100));
        Graphics::SetBrightness(r.ival);
    } else if (s_eq(a, "display.vsync")) {
        DisplayManager::SetVSync(r.uval ? VSYNC_ON : VSYNC_OFF);
        FramePacer::ReloadConfig();
    } else if (s_eq(a, "display.resolution")) {
        DisplayManager::SetResolution((uint32_t)r.ival, (uint32_t)r.uval, 32);
    } else if (s_eq(a, "display.config")) {
        bool on[OUTPUT_MAX] = { false, false, false, false };
        int ax[OUTPUT_MAX] = { 0, 0, 0, 0 }, ay[OUTPUT_MAX] = { 0, 0, 0, 0 };
        for (int i = 0; i < OUTPUT_MAX && i < 4; i++) {
            on[i] = (r.onmask & (1u << i)) != 0;
            ax[i] = r.a[i];
            ay[i] = r.b[i];
        }
        Graphics::RequestOutputConfig(r.ival, on, ax, ay);
    } else if (s_eq(a, "framepacer.mode")) {
        FramePacer::SetMode(r.uval);
    } else if (s_eq(a, "open.alpine")) {
        AlpineTerminal::Open();
    }
}

// ---- read-only info for the pages (satoru)
int count_nodes(KVFSNode* n, int depth) {
    if (!n || depth > 64) return 0;
    int t = 1;
    for (int i = 0; i < n->child_count; i++) t += count_nodes(n->children[i], depth + 1);
    return t;
}

void build_info() {
    tbl_clear(&g_info_all);
    char b[KSS_VAL_MAX];

    // processor (satoru)
    tbl_add(&g_info_all, "cpu.brand", CPUDetect::GetBrandString());
    tbl_add(&g_info_all, "cpu.vendor", CPUDetect::GetVendorName());
    tbl_int(&g_info_all, "cpu.cores", CPUDetect::GetCoreCount());
    tbl_int(&g_info_all, "cpu.threads", CPUDetect::GetThreadCount());
    tbl_int(&g_info_all, "cpu.base_mhz", CPUDetect::GetBaseMHz());
    tbl_int(&g_info_all, "cpu.sse2", CPUDetect::HasSSE2() ? 1 : 0);
    tbl_int(&g_info_all, "cpu.avx", CPUDetect::HasAVX() ? 1 : 0);
    tbl_int(&g_info_all, "cpu.avx2", CPUDetect::HasAVX2() ? 1 : 0);
    tbl_int(&g_info_all, "cpu.avx512", CPUDetect::HasAVX512() ? 1 : 0);
    tbl_int(&g_info_all, "cpu.aes", CPUDetect::HasAES() ? 1 : 0);
    tbl_int(&g_info_all, "cpu.rdrand", CPUDetect::HasRDRAND() ? 1 : 0);

    int ncpu = CPUFreq::CPUCount();
    tbl_int(&g_info_all, "cpu.count", ncpu);
    for (int i = 0; i < ncpu && i < 32; i++) {
        const CPUFreq::CPUInfo* ci = CPUFreq::GetCPU((uint32_t)i);
        if (!ci) continue;
        tbl_key_int(&g_info_all, "cpu.", i, ".present", ci->present ? 1 : 0);
        tbl_key_int(&g_info_all, "cpu.", i, ".base", ci->base_mhz);
        tbl_key_int(&g_info_all, "cpu.", i, ".turbo", ci->turbo_mhz);
        tbl_key_int(&g_info_all, "cpu.", i, ".cur", ci->cur_mhz);
        tbl_key_int(&g_info_all, "cpu.", i, ".gov", ci->governor);
        tbl_key_int(&g_info_all, "cpu.", i, ".pstates", ci->pstate_count);
        tbl_key_int(&g_info_all, "cpu.", i, ".load", ci->recent_load_pct);
    }

    tbl_int(&g_info_all, "mem.total", (long long)PMM::GetTotalMemory());
    tbl_int(&g_info_all, "mem.used", (long long)(PMM::GetUsedFrames() * (uint64_t)PAGE_SIZE));

    // screen (satoru)
    tbl_int(&g_info_all, "screen.w", Graphics::GetWidth());
    tbl_int(&g_info_all, "screen.h", Graphics::GetHeight());
    tbl_int(&g_info_all, "screen.scale", Graphics::GetUIScale());
    tbl_add(&g_info_all, "display.backend", DisplayManager::GetBackendName());
    tbl_int(&g_info_all, "display.w", (long long)DisplayManager::GetWidth());
    tbl_int(&g_info_all, "display.h", (long long)DisplayManager::GetHeight());
    tbl_int(&g_info_all, "display.brightness", DisplayManager::GetBrightness());
    tbl_int(&g_info_all, "display.vsync", DisplayManager::GetVSync() != VSYNC_OFF ? 1 : 0);

    // gpu inventory (satoru)
    const GpuProbeResult& g = GpuProbe::GetResult();
    tbl_int(&g_info_all, "gpu.count", g.count);
    tbl_int(&g_info_all, "gpu.primary", g.primary_idx);
    for (int i = 0; i < g.count && i < GPU_PROBE_MAX; i++) {
        tbl_key_int(&g_info_all, "gpu.", i, ".vendor", g.gpus[i].vendor_id);
        tbl_key_int(&g_info_all, "gpu.", i, ".device", g.gpus[i].device_id);
        tbl_key_int(&g_info_all, "gpu.", i, ".igpu", g.gpus[i].is_igpu ? 1 : 0);
        tbl_key_str(&g_info_all, "gpu.", i, ".desc", g.gpus[i].desc);
    }
    tbl_int(&g_info_all, "gpu.intel", IntelGPU::IsDetected() ? 1 : 0);
    tbl_add(&g_info_all, "gpu.intel.gen", IntelGPU::IsDetected() ? IntelGPU::GetGenName() : "");
    tbl_int(&g_info_all, "gpu.nvidia", NvidiaGPU::IsDetected() ? 1 : 0);
    tbl_add(&g_info_all, "gpu.nvidia.arch", NvidiaGPU::IsDetected() ? NvidiaGPU::GetArchName() : "");
    tbl_int(&g_info_all, "gpu.amd", AmdGPU::IsAvailable() ? 1 : 0);
    tbl_add(&g_info_all, "gpu.amd.arch", AmdGPU::IsAvailable() ? AmdGPU::GetArchName() : "");

    // audio (satoru)
    AudioServer::ServerStatus st = AudioServer::GetStatus();
    tbl_int(&g_info_all, "audio.ready", st.backend_ready ? 1 : 0);
    tbl_int(&g_info_all, "audio.available", Audio::IsAvailable() ? 1 : 0);
    tbl_add(&g_info_all, "audio.backend", st.backend_name ? st.backend_name : "none");
    tbl_int(&g_info_all, "audio.rate", st.backend_rate);
    tbl_int(&g_info_all, "audio.streams", st.active_streams);
    tbl_int(&g_info_all, "audio.volume", AudioMixer::GetMasterVolume());
    tbl_int(&g_info_all, "audio.muted", AudioMixer::GetMasterMute() ? 1 : 0);
    tbl_int(&g_info_all, "audio.hda", HDAudio::IsDetected() ? 1 : 0);
    tbl_int(&g_info_all, "audio.codecs", HDAudio::GetCodecCount());
    {
        char vend[96] = "";
        int cc = HDAudio::GetCodecCount();
        if (cc > 3) cc = 3;
        for (int i = 0; i < cc; i++) {
            uint32_t vid = (uint32_t)(HDAudio::GetCodecVendor(i) >> 16);
            const char* nm = "HD Audio codec";
            switch (vid) {
                case 0x10EC: nm = "Realtek"; break;
                case 0x8086: nm = "Intel"; break;
                case 0x1AF4: nm = "Red Hat"; break;
                case 0x1002: nm = "AMD"; break;
                case 0x10DE: nm = "NVIDIA"; break;
                case 0x14F1: nm = "Conexant"; break;
                case 0x11D4: nm = "Analog Devices"; break;
                case 0x1106: nm = "VIA"; break;
                case 0x1013: nm = "Cirrus Logic"; break;
                default: break;
            }
            if (i) s_append(vend, ", ", sizeof(vend));
            s_append(vend, nm, sizeof(vend));
        }
        tbl_add(&g_info_all, "audio.codec_vendor", vend);
    }

    // power source is ac in all current hardware paths (satoru)
    tbl_int(&g_info_all, "power.plan", UIConfig::Int("power.plan", 1));

    // storage (satoru)
    tbl_int(&g_info_all, "fs.used", (long long)KVFS::DiskUsage("/"));
    tbl_int(&g_info_all, "fs.nodes", count_nodes(KVFS::GetRoot(), 0));
    tbl_int(&g_info_all, "persist", PersistStore::Available() ? 1 : 0);
    tbl_int(&g_info_all, "heap.used", (long long)KernelHeap::GetUsed());
    tbl_int(&g_info_all, "heap.total", (long long)(KernelHeap::GetTotal() ? KernelHeap::GetTotal() : KernelHeap::GetUsed() + KernelHeap::GetFree()));
    int nd = BlockDev::Count();
    tbl_int(&g_info_all, "disk.count", nd);
    for (int i = 0; i < nd && i < 16; i++) {
        BlockDevice* d = BlockDev::Get(i);
        if (!d || !d->present) continue;
        tbl_key_str(&g_info_all, "disk.", i, ".name", d->name);
        tbl_key_str(&g_info_all, "disk.", i, ".driver", d->driver);
        tbl_key_str(&g_info_all, "disk.", i, ".model", d->model);
        tbl_key_int(&g_info_all, "disk.", i, ".bytes", (long long)(d->capacity_lba * (uint64_t)(d->sector_size ? d->sector_size : 512)));
        tbl_key_int(&g_info_all, "disk.", i, ".removable", d->removable ? 1 : 0);
    }

    // network (satoru)
    int ni = Network::GetInterfaceCount();
    if (ni > NET_MAX_INTERFACES) ni = NET_MAX_INTERFACES;
    tbl_int(&g_info_all, "net.count", ni);
    NetworkInterface* ifs = Network::GetInterfaces();
    for (int i = 0; i < ni && ifs; i++) {
        tbl_key_str(&g_info_all, "net.", i, ".name", ifs[i].name);
        tbl_key_int(&g_info_all, "net.", i, ".type", ifs[i].type);
        tbl_key_int(&g_info_all, "net.", i, ".state", ifs[i].state);
        s_num(b, sizeof(b), (ifs[i].mac.bytes[0] << 8) | ifs[i].mac.bytes[1]);
        tbl_key_str(&g_info_all, "net.", i, ".mac0", b);
        char mac[24]; int p = 0;
        static const char* hx = "0123456789ABCDEF";
        for (int k = 0; k < 6; k++) {
            if (k) mac[p++] = ':';
            mac[p++] = hx[(ifs[i].mac.bytes[k] >> 4) & 0xF];
            mac[p++] = hx[ifs[i].mac.bytes[k] & 0xF];
        }
        mac[p] = 0;
        tbl_key_str(&g_info_all, "net.", i, ".mac", mac);
        char ip[16];
        int q = 0;
        for (int k = 0; k < 4; k++) {
            if (k) ip[q++] = '.';
            s_num(ip + q, 4, ifs[i].ip.bytes[k]);
            while (ip[q]) q++;
        }
        ip[q] = 0;
        tbl_key_str(&g_info_all, "net.", i, ".ip", ip);
        q = 0;
        for (int k = 0; k < 4; k++) {
            if (k) ip[q++] = '.';
            s_num(ip + q, 4, ifs[i].gateway.bytes[k]);
            while (ip[q]) q++;
        }
        ip[q] = 0;
        tbl_key_str(&g_info_all, "net.", i, ".gw", ip);
        q = 0;
        for (int k = 0; k < 4; k++) {
            if (k) ip[q++] = '.';
            s_num(ip + q, 4, ifs[i].dns.bytes[k]);
            while (ip[q]) q++;
        }
        ip[q] = 0;
        tbl_key_str(&g_info_all, "net.", i, ".dns", ip);
        tbl_key_int(&g_info_all, "net.", i, ".rx_packets", ifs[i].rx_packets);
        tbl_key_int(&g_info_all, "net.", i, ".tx_packets", ifs[i].tx_packets);
        tbl_key_int(&g_info_all, "net.", i, ".rx_bytes", (long long)ifs[i].rx_bytes);
        tbl_key_int(&g_info_all, "net.", i, ".tx_bytes", (long long)ifs[i].tx_bytes);
    }
    uint32_t dns1 = 0, dns2 = 0;
    Network::GetDNSServers(&dns1, &dns2);
    tbl_int(&g_info_all, "net.dns1", dns1);
    tbl_int(&g_info_all, "net.dns2", dns2);
    // counters straight from the nic: interface tx bytes were never updated by
    // the kernel tcp path, so the status page reads these. (satoru)
    tbl_int(&g_info_all, "net.detected", E1000::IsDetected() ? 1 : 0);
    tbl_int(&g_info_all, "net.rx_bytes", E1000::GetRxBytes());
    tbl_int(&g_info_all, "net.tx_bytes", E1000::GetTxBytes());
    tbl_int(&g_info_all, "net.rx_packets", E1000::GetRxCount());
    tbl_int(&g_info_all, "net.tx_packets", E1000::GetTxCount());
    tbl_int(&g_info_all, "dhcp.bound", Dhcp::IsBound() ? 1 : 0);
    tbl_add(&g_info_all, "dhcp.state", Dhcp::StateString());
    tbl_int(&g_info_all, "dhcp.lease_left", Dhcp::LeaseRemainingSeconds());
    tbl_int(&g_info_all, "dhcp.server", Dhcp::GetServer());
    tbl_int(&g_info_all, "dhcp.dns1", Dhcp::GetPrimaryDNS());
    tbl_int(&g_info_all, "dhcp.dns2", Dhcp::GetSecondaryDNS());
    tbl_int(&g_info_all, "wifi.detected", WiFi::DetectedLink() == WiFi::LINK_WIFI ? 1 : 0);
    tbl_int(&g_info_all, "wifi.state", WiFi::GetState());
    tbl_int(&g_info_all, "wifi.signal", WiFi::GetSignalStrength());
    tbl_int(&g_info_all, "wifi.bars", WiFi::SignalBars());
    tbl_int(&g_info_all, "wifi.autoreconnect", WiFi::AutoReconnect() ? 1 : 0);
    tbl_add(&g_info_all, "wifi.chip", WiFi::WirelessChipName());
    int wn = WiFi::GetNetworkCount();
    if (wn > NET_MAX_WIFI_NETS) wn = NET_MAX_WIFI_NETS;
    tbl_int(&g_info_all, "wifi.count", wn);
    WiFiNetwork* wl = WiFi::GetNetworks();
    for (int i = 0; i < wn && wl; i++) {
        tbl_key_str(&g_info_all, "wifi.", i, ".ssid", wl[i].ssid);
        tbl_key_int(&g_info_all, "wifi.", i, ".sig", wl[i].signal_strength);
        tbl_key_int(&g_info_all, "wifi.", i, ".sec", wl[i].security);
        tbl_key_int(&g_info_all, "wifi.", i, ".conn", wl[i].connected ? 1 : 0);
    }

    // users and sessions (satoru)
    int uc = UserManager::GetUserCount();
    if (uc > UserManager::MAX_USERS) uc = UserManager::MAX_USERS;
    tbl_int(&g_info_all, "user.count", uc);
    tbl_int(&g_info_all, "user.cur", UserManager::GetCurrentUserIndex());
    for (int i = 0; i < uc; i++) {
        tbl_key_str(&g_info_all, "user.", i, ".name", UserManager::users[i].username);
        tbl_key_str(&g_info_all, "user.", i, ".display", UserManager::users[i].display_name);
        tbl_key_int(&g_info_all, "user.", i, ".admin", UserManager::users[i].is_admin ? 1 : 0);
    }
    int sid = KEN::GetCurrentSession();
    tbl_int(&g_info_all, "ken.level", sid >= 0 ? (int)KEN::GetLevel(sid) : -1);
    int act = 0;
    for (int i = 0; i < KEN_MAX_SESSIONS; i++) {
        KENSession* s = KEN::GetSession(i);
        if (s && s->active) act++;
    }
    tbl_int(&g_info_all, "ken.sessions", act);

    // linux subsystem and packages (satoru)
    tbl_int(&g_info_all, "kls.state", (int)KLS::GetState());
    tbl_add(&g_info_all, "kls.kernel", KLS::GetKernelVersion());
    tbl_int(&g_info_all, "kls.procs", KLS::LinuxProcessCount());
    KLSConfig* kc = KLS::GetConfig();
    tbl_add(&g_info_all, "hostname", (kc && kc->hostname[0]) ? kc->hostname : "kurono");
    tbl_add(&g_info_all, "linux.version", LinuxKernel::GetVersion());
    tbl_add(&g_info_all, "pkg.sync", PackageManager::GetLastSyncMessage() ? PackageManager::GetLastSyncMessage() : "Never");
    tbl_add(&g_info_all, "pkg.repo", PackageManager::GetRepositoryHost());
    tbl_int(&g_info_all, "pkg.installed", PackageManager::InstalledCount());
    tbl_int(&g_info_all, "pkg.available", PackageManager::AvailableCount());
    tbl_int(&g_info_all, "pkg.pending", PackageManager::GetPendingUpdateCount());
    tbl_int(&g_info_all, "update.pending", SystemUpdate::HasPendingUpdate() ? 1 : 0);

    // kmemx (satoru)
    tbl_int(&g_info_all, "kmemx.enabled", KMemX::IsEnabled() ? 1 : 0);
    tbl_int(&g_info_all, "kmemx.pool_pct", KMemX::PoolPct());
    tbl_int(&g_info_all, "kmemx.threshold", KMemX::Threshold());
    {
        const KMemX::Stats& ks = KMemX::GetStats();
        tbl_int(&g_info_all, "kmemx.pool_used", (long long)ks.pool_used);
        tbl_int(&g_info_all, "kmemx.pool_bytes", (long long)ks.pool_bytes);
        tbl_int(&g_info_all, "kmemx.saved", (long long)ks.bytes_saved);
    }
    tbl_int(&g_info_all, "kmemx.ratio", KMemX::RatioX100());

    // linux vm (satoru)
    tbl_int(&g_info_all, "vm.enabled", VMHost::Enabled() ? 1 : 0);
    tbl_int(&g_info_all, "vm.state", (int)VMHost::GetState());
    tbl_int(&g_info_all, "vm.distro", VMHost::SelectedDistro());
    tbl_int(&g_info_all, "vm.ram_mb", VMHost::ConfiguredRamMb());
    tbl_int(&g_info_all, "vm.generation", (long long)VMHost::Generation());
    tbl_int(&g_info_all, "vm.agent", VMHost::AgentReady() ? 1 : 0);
    tbl_int(&g_info_all, "vm.hv", (Hypervisor::IsAvailable() || VMM::IsSupported()) ? 1 : 0);
    tbl_int(&g_info_all, "vm.ram_cmdline", VMHost::RamFromCmdline() ? 1 : 0);
    {
        VMHost::Info vi; VMHost::GetInfo(&vi);
        tbl_int(&g_info_all, "vm.live_state", vi.state);
        tbl_int(&g_info_all, "vm.uptime", vi.uptime_ms);
        tbl_int(&g_info_all, "vm.live_ram", vi.ram_mb);
        tbl_int(&g_info_all, "vm.agent_live", vi.agent ? 1 : 0);
        tbl_int(&g_info_all, "vm.start_pending", vi.start_pending ? 1 : 0);
        tbl_int(&g_info_all, "vm.loop_stopped", vi.loop_stopped ? 1 : 0);
        tbl_add(&g_info_all, "vm.error", vi.error ? vi.error : "");
        const VMHost::DistroDesc* dd = VMHost::GetDistro(VMHost::SelectedDistro());
        if (dd) {
            tbl_add(&g_info_all, "vm.distro_name", dd->name);
            tbl_add(&g_info_all, "vm.distro_desc", dd->version);
            tbl_add(&g_info_all, "vm.image_name", dd->image_name);
            uint64_t img = 0;
            int why = DiskLog::GuestImage(dd->image83, nullptr, &img);
            tbl_int(&g_info_all, "vm.img_why", why);
            tbl_int(&g_info_all, "vm.img_bytes", (long long)img);
        }
        tbl_int(&g_info_all, "vm.distro_count", VMHost::DistroCount());
        for (int i = 0; i < VMHost::DistroCount() && i < 4; i++) {
            const VMHost::DistroDesc* d = VMHost::GetDistro(i);
            if (!d) continue;
            tbl_key_str(&g_info_all, "vmd.", i, ".name", d->name);
            tbl_key_str(&g_info_all, "vmd.", i, ".id", d->id);
            tbl_key_str(&g_info_all, "vmd.", i, ".version", d->version);
            tbl_key_str(&g_info_all, "vmd.", i, ".note", d->note ? d->note : "");
            tbl_key_int(&g_info_all, "vmd.", i, ".status", d->status);
        }
    }

    // displays and outputs (satoru)
    tbl_int(&g_info_all, "out.count", Outputs::Total());
    tbl_int(&g_info_all, "out.multi", Outputs::Multi() ? 1 : 0);
    tbl_int(&g_info_all, "out.active", Graphics::MultiActive() ? 1 : 0);
    for (int i = 0; i < Outputs::Total() && i < OUTPUT_MAX; i++) {
        const Output* o = Outputs::Get(i);
        if (!o) continue;
        tbl_key_str(&g_info_all, "out.", i, ".name", o->name);
        tbl_key_int(&g_info_all, "out.", i, ".present", o->present ? 1 : 0);
        tbl_key_int(&g_info_all, "out.", i, ".connected", o->connected ? 1 : 0);
        tbl_key_int(&g_info_all, "out.", i, ".x", o->x);
        tbl_key_int(&g_info_all, "out.", i, ".y", o->y);
        tbl_key_int(&g_info_all, "out.", i, ".w", o->w);
        tbl_key_int(&g_info_all, "out.", i, ".h", o->h);
        tbl_key_int(&g_info_all, "out.", i, ".pw", o->pw);
        tbl_key_int(&g_info_all, "out.", i, ".ph", o->ph);
        tbl_key_int(&g_info_all, "out.", i, ".ax", o->ax);
        tbl_key_int(&g_info_all, "out.", i, ".ay", o->ay);
    }
    {
        DisplayMode modes[DISPLAY_MODE_COUNT];
        int mc = 0;
        if (!DisplayManager::GetSupportedModes(modes, DISPLAY_MODE_COUNT, &mc)) mc = 0;
        tbl_int(&g_info_all, "mode.count", mc);
        for (int i = 0; i < mc && i < DISPLAY_MODE_COUNT; i++) {
            tbl_key_int(&g_info_all, "mode.", i, ".w", (long long)modes[i].width);
            tbl_key_int(&g_info_all, "mode.", i, ".h", (long long)modes[i].height);
            tbl_key_int(&g_info_all, "mode.", i, ".bpp", modes[i].bpp);
            tbl_key_int(&g_info_all, "mode.", i, ".refresh", modes[i].refresh_rate);
        }
    }
    tbl_int(&g_info_all, "fpc.mode", (int)FramePacer::Mode());
    tbl_int(&g_info_all, "fpc.current", (int)FramePacer::CurrentHz());
    tbl_int(&g_info_all, "fpc.cap", (int)FramePacer::CapHz());
    tbl_int(&g_info_all, "fpc.cmdline", (int)FramePacer::CmdlineHz());
    tbl_int(&g_info_all, "fpc.panel", (int)FramePacer::PanelHz());
    tbl_add(&g_info_all, "fpc.source", FramePacer::PanelSource());

    // usb and drivers (satoru)
    tbl_int(&g_info_all, "usb.detected", USB::IsDetected() ? 1 : 0);
    tbl_int(&g_info_all, "usb.ports", USB::GetPortCount());
    int dc = USB::GetDeviceCount();
    tbl_int(&g_info_all, "usb.count", dc);
    for (int i = 0; i < dc && i < 16; i++) {
        const USBDeviceInfo* d = USB::GetDevice(i);
        if (!d) continue;
        tbl_key_int(&g_info_all, "usb.", i, ".conn", d->connected ? 1 : 0);
        tbl_key_str(&g_info_all, "usb.", i, ".man", d->manufacturer);
        tbl_key_str(&g_info_all, "usb.", i, ".prod", d->product);
        tbl_key_int(&g_info_all, "usb.", i, ".port", d->port);
        tbl_key_int(&g_info_all, "usb.", i, ".speed", d->speed);
    }
    LinuxDriver* drv = LinuxDriverFramework::GetDrivers();
    int drvn = LinuxDriverFramework::GetDriverCount();
    tbl_int(&g_info_all, "drv.count", drvn);
    for (int i = 0; i < drvn && i < 48 && drv; i++) {
        tbl_key_str(&g_info_all, "drv.", i, ".name", drv[i].name);
        tbl_key_str(&g_info_all, "drv.", i, ".desc", drv[i].description);
        tbl_key_str(&g_info_all, "drv.", i, ".ver", drv[i].version);
        tbl_key_int(&g_info_all, "drv.", i, ".cat", drv[i].category);
        tbl_key_int(&g_info_all, "drv.", i, ".state", drv[i].state);
        tbl_key_int(&g_info_all, "drv.", i, ".bound", drv[i].bound ? 1 : 0);
    }
}

// ---- socket server (satoru)
uint8_t g_rx[UnixSocket::UNIX_MAX_SOCKETS][sizeof(KssReq)];
int g_rx_len[UnixSocket::UNIX_MAX_SOCKETS];
bool g_init = false;

void send_reply(int sd, bool ok, const char* why) {
    KssReply rp;
    rp.magic = KSS_REPLY_MAGIC;
    rp.ok = ok ? 1 : 0;
    s_copy(rp.why, why, sizeof(rp.why));
    UnixSocket::KernelInject(sd, &rp, (int)sizeof(rp));
}

void send_table(int sd, const Table* t, uint32_t start) {
    g_wire.magic = KSS_TABLE_MAGIC;
    uint32_t n = 0;
    for (uint32_t i = start; i < t->count && n < KSS_ENTRY_MAX; i++, n++)
        g_wire.entries[n] = t->entries[i];
    g_wire.count = n;
    UnixSocket::KernelInject(sd, &g_wire, (int)sizeof(KssTable));
}

bool wifi_action_sync(const KssReq& r) {
    if (s_eq(r.action, "wifi.enable")) return WiFi::Enable();
    if (s_eq(r.action, "wifi.disable")) return WiFi::Disable();
    if (s_eq(r.action, "wifi.scan")) return WiFi::Scan();
    if (s_eq(r.action, "wifi.disconnect")) return WiFi::Disconnect();
    if (s_eq(r.action, "wifi.connect")) return WiFi::Connect(r.key, r.val);
    return false;
}

void handle(int sd, const KssReq& r) {
    if (r.magic != KSS_MAGIC) {
        send_reply(sd, false, "bad magic");
        return;
    }
    if (r.kind == KSS_GET_CFG) {
        SpinLockGuard g(g_lock);
        build_cfg();
        send_reply(sd, true, "ok");
        send_table(sd, &g_cfg_all, 0);
        return;
    }
    if (r.kind == KSS_GET_INFO) {
        SpinLockGuard g(g_lock);
        static uint32_t built_ms = 0;
        static bool built = false;
        uint32_t now = Timer::GetRealMs();
        if (!built || now - built_ms > 500u) { build_info(); built_ms = now; built = true; }
        send_reply(sd, true, "ok");
        send_table(sd, &g_info_all, r.uval);
        return;
    }
    if (r.kind == KSS_SET) {
        const KeyRule* rule = rule_for(r.key);
        if (!rule) { send_reply(sd, false, "unknown key"); return; }
        char key[KSS_KEY_MAX], val[KSS_VAL_MAX];
        s_copy(key, r.key, sizeof(key));
        s_copy(val, r.val, sizeof(val));
        if (!validate(rule, val)) { send_reply(sd, false, "invalid value"); return; }
        UIConfig::Set(key, val, true);
        send_reply(sd, true, "ok");
        return;
    }
    if (r.kind == KSS_ACTION) {
        char a[KSS_KEY_MAX];
        s_copy(a, r.action, sizeof(a));
        bool gated = action_wants_gate(a);
        const char* detail = s_eq(a, "user.add") ? r.key : a;
        if (gated && !AdminGate::Authorize("system settings", detail)) {
            send_reply(sd, false, "denied");
            return;
        }
        if (s_eq(a, "user.add")) {
            bool ok = UserManager::AddUser(r.key, r.val);
            send_reply(sd, ok, ok ? "ok" : "could not create the account");
            return;
        }
        if (s_eq(a, "pkg.sync")) {
            queue_action(r);
            send_reply(sd, true, "queued");
            return;
        }
        if (s_prefix(a, "wifi.")) {
            bool ok = wifi_action_sync(r);
            send_reply(sd, ok, ok ? "ok" : "wifi refused");
            return;
        }
        if (s_eq(a, "audio.volume") || s_eq(a, "audio.mute")) {
            apply_action(r);
            send_reply(sd, true, "ok");
            return;
        }
        queue_action(r);
        send_reply(sd, true, "queued");
        return;
    }
    send_reply(sd, false, "bad kind");
}

void svc_on_connect(int server_sd, int new_sd, void* user) {
    (void)server_sd; (void)user;
    if (new_sd >= 0 && new_sd < UnixSocket::UNIX_MAX_SOCKETS) g_rx_len[new_sd] = 0;
}

void svc_on_data(int sd, const uint8_t* data, int len, void* user) {
    (void)user;
    if (sd < 0 || sd >= UnixSocket::UNIX_MAX_SOCKETS || !data || len <= 0) return;
    int have = g_rx_len[sd];
    if (have < 0 || have > (int)sizeof(KssReq)) have = 0;
    for (int i = 0; i < len && have < (int)sizeof(KssReq); i++) g_rx[sd][have++] = data[i];
    while (have >= (int)sizeof(KssReq)) {
        KssReq r;
        for (int i = 0; i < (int)sizeof(KssReq); i++) ((uint8_t*)&r)[i] = g_rx[sd][i];
        r.key[KSS_KEY_MAX - 1] = 0;
        r.val[KSS_VAL_MAX - 1] = 0;
        r.action[KSS_KEY_MAX - 1] = 0;
        r.arg[KSS_ARG_MAX - 1] = 0;
        handle(sd, r);
        int rem = have - (int)sizeof(KssReq);
        for (int i = 0; i < rem; i++) g_rx[sd][i] = g_rx[sd][i + (int)sizeof(KssReq)];
        have = rem;
    }
    g_rx_len[sd] = have;
}

}  // namespace

namespace SettingsService {

void Init() {
    if (g_init) return;
    g_init = true;
    KVFS::Mkdirs("/system/run/user/1000");
    KVFS::Mkdirs("/system/share/kurono");
    // the appearance page draws real wallpaper thumbnails from these files. (satoru)
    for (int i = 0; i < 2; i++) {
        int w = 0, h = 0;
        const uint8_t* px = Desktop::BuiltinWallpaperRGBA(i, &w, &h);
        if (!px || w <= 0 || h <= 0) continue;
        char path[64];
        s_copy(path, "/system/share/kurono/wallpaper", sizeof(path));
        char nb[4]; s_num(nb, sizeof(nb), i);
        s_append(path, nb, sizeof(path));
        s_append(path, ".rgba", sizeof(path));
        KVFS::WriteFileStatic(path, px, (uint32_t)(w * h * 4));
    }
    int sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (sd < 0) return;
    if (UnixSocket::Bind(sd, KSS_PATH) < 0) { UnixSocket::Close(sd); return; }
    UnixSocket::Listen(sd, 8);
    UnixSocket::RegisterServer(sd, svc_on_connect, svc_on_data, nullptr);
    SerialLogger::Log("[settings] service on " KSS_PATH "\r\n");
}

void Pump() {
    Init();
    for (;;) {
        KssReq r;
        {
            SpinLockGuard g(g_lock);
            int tail = g_act_tail;
            if (tail == g_act_head) break;
            r = g_act[tail];
            __atomic_store_n(&g_act_tail, (tail + 1) % ACT_N, __ATOMIC_RELEASE);
        }
        apply_action(r);
    }
}

}  // namespace SettingsService

// end (satoru)
