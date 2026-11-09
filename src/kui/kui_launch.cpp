// kui_launch.cpp: kernel side of ring-3 kui apps: the app registry, /system/apps install, the shared (satoru)
// assets ring-3 programs read (fonts, icon art, theme), the ring-3 / in-kernel choice, and `kapp`. (satoru)
#include "kui_internal.h"
#include "kui_kernel.h"
#include "kui_launch_proto.h"
#include "../kernel/heap.h"
#include "../kernel/taskmem.h"
#include "../kernel/userspace.h"
#include "../kernel/elf_loader.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"
#include "../proc/kernel_processes.h"
#include "../proc/spinlock.h"
#include "../net/unix_socket.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../ui/window_manager.h"
#include "../ui/wayland_server.h"
#include "../ui/desktop.h"
#include "../system/ui_config.h"
#include "../security/admin.h"
#include "../fs/kvfs.h"
#include "../apps/calculator.h"
#include "../apps/text_editor.h"
#include "../apps/file_manager.h"
#include "../apps/media_player.h"
#include "../apps/denji_app.h"
#include "../apps/terminal.h"
#include "../apps/system_settings.h"
#include "../system/settings_service.h"

// the in-kernel builds of the ported apps (kui_launch_<name>, KUI_APP in each source) (satoru)
int kui_launch_texteditor();
int kui_launch_files();
int kui_launch_taskmgr();
int kui_launch_mediaplayer();
int kui_launch_denji();

extern "C" {
    // ring-3 builds of the kui apps (Makefile: build/userprogs/kui_<name>.elf, objcopy) (satoru)
    extern const uint8_t _binary_kui_calculator_elf_start[] __attribute__((weak));
    extern const uint8_t _binary_kui_calculator_elf_end[]   __attribute__((weak));
    extern const uint8_t _binary_kui_texteditor_elf_start[] __attribute__((weak));
    extern const uint8_t _binary_kui_texteditor_elf_end[]   __attribute__((weak));
    extern const uint8_t _binary_kui_files_elf_start[]      __attribute__((weak));
    extern const uint8_t _binary_kui_files_elf_end[]        __attribute__((weak));
    extern const uint8_t _binary_kui_taskmgr_elf_start[]    __attribute__((weak));
    extern const uint8_t _binary_kui_taskmgr_elf_end[]      __attribute__((weak));
    extern const uint8_t _binary_kui_mediaplayer_elf_start[] __attribute__((weak));
    extern const uint8_t _binary_kui_mediaplayer_elf_end[]   __attribute__((weak));
    extern const uint8_t _binary_kui_denji_elf_start[]      __attribute__((weak));
    extern const uint8_t _binary_kui_denji_elf_end[]        __attribute__((weak));
    extern const uint8_t _binary_kui_terminal_elf_start[]   __attribute__((weak));
    extern const uint8_t _binary_kui_terminal_elf_end[]     __attribute__((weak));
    extern const uint8_t _binary_kui_settings_elf_start[]   __attribute__((weak));
    extern const uint8_t _binary_kui_settings_elf_end[]     __attribute__((weak));
    // the ui faces and icon art, embedded for the kernel already (satoru)
    extern const uint8_t _binary_inter_regular_otf_start[]  __attribute__((weak));
    extern const uint8_t _binary_inter_regular_otf_end[]    __attribute__((weak));
    extern const uint8_t _binary_inter_semibold_otf_start[] __attribute__((weak));
    extern const uint8_t _binary_inter_semibold_otf_end[]   __attribute__((weak));
    extern const uint8_t _binary_dejavu_mono_ttf_start[]    __attribute__((weak));
    extern const uint8_t _binary_dejavu_mono_ttf_end[]      __attribute__((weak));
    extern const uint8_t _binary_modern_icons_bin_start[]   __attribute__((weak));
    extern const uint8_t _binary_modern_icons_bin_end[]     __attribute__((weak));
}

namespace kui {
    void PumpWindows();
    void ThemeChangedAll();
    void KeyToFocused(int key, bool pressed, bool shift, int raw);
}

namespace {

// in-kernel launches that take the same optional path the ring-3 build gets on argv (satoru)
int k_editor_arg(const char* path){ return TextEditorApp::OpenFile(path); }
int k_files_arg(const char* path){ return FileManagerApp::OpenAt(path); }
int k_media_arg(const char* path){ MediaPlayerApp::Open(path); return 0; }
int k_terminal(){ return TerminalApp::OpenKernel(); }   // the in-kernel pty terminal (satoru)
int k_settings_launch(){ SystemSettings::OpenKernel(); return 0; }

struct KuiApp {
    const char* name;                 // /system/apps/<name>, `kapp <name>`, the launcher command (satoru)
    const char* title;                // the window title, to find a running instance (satoru)
    int (*launch_kernel)();           // the in-kernel build of the same source (satoru)
    int (*launch_kernel_arg)(const char* arg);   // the same with a path, nullptr when it takes none (satoru)
    const uint8_t* elf;               // embedded ring-3 build, may be absent (satoru)
    const uint8_t* elf_end;
    uint32_t taskmem_app;             // kurono.taskmem=1 bucket for the in-kernel build (satoru)
};

KuiApp g_apps[] = {
    { "calculator", "Calculator", kui_launch_calculator, nullptr,
      _binary_kui_calculator_elf_start, _binary_kui_calculator_elf_end, TaskMem::APP_CALC },
    { "texteditor", "Text Editor", kui_launch_texteditor, k_editor_arg,
      _binary_kui_texteditor_elf_start, _binary_kui_texteditor_elf_end, TaskMem::APP_EDITOR },
    { "files", "Files", kui_launch_files, k_files_arg,
      _binary_kui_files_elf_start, _binary_kui_files_elf_end, TaskMem::APP_FILES },
    { "taskmgr", "Task Manager", kui_launch_taskmgr, nullptr,
      _binary_kui_taskmgr_elf_start, _binary_kui_taskmgr_elf_end, TaskMem::APP_TASKMGR },
    { "mediaplayer", "Media Player", kui_launch_mediaplayer, k_media_arg,
      _binary_kui_mediaplayer_elf_start, _binary_kui_mediaplayer_elf_end, TaskMem::APP_MEDIA },
    { "denji", "Media", kui_launch_denji, nullptr,
      _binary_kui_denji_elf_start, _binary_kui_denji_elf_end, TaskMem::APP_MEDIA },
    { "terminal", "Terminal", k_terminal, nullptr,
      _binary_kui_terminal_elf_start, _binary_kui_terminal_elf_end, TaskMem::APP_TERMINAL },
    { "settings", "Settings", k_settings_launch, nullptr,
      _binary_kui_settings_elf_start, _binary_kui_settings_elf_end, TaskMem::APP_SYSSETTINGS },
};
const int APP_N = (int)(sizeof(g_apps) / sizeof(g_apps[0]));

char g_boot_list[96];                 // kurono.r3apps=... (satoru)
bool g_installed = false;
volatile int g_running[8];            // per app: live ring-3 instances (satoru)
volatile int g_armed[8];              // per app: launches queued through the desktop, not started yet (satoru)
volatile int g_fallback[8];           // per app: the ring-3 start failed, open the kernel one (satoru)
char g_fallback_arg[8][KUI_LAUNCH_ARG_MAX];   // the path that failed launch, for the kernel fallback (satoru)
uint32_t g_theme_fp = 0;
char g_test_open[24];                 // kurono.kui.open=<name>: the harness opens it once (satoru)
uint32_t g_test_at = 0;
uint32_t g_theme_check_ms = 0;

// the argument of a launch that is already queued for a launcher process. the command line carries (satoru)
// only a token, so a path with spaces or shell punctuation never reaches the shell parser. (satoru)
struct StagedArg { bool used; uint32_t token; char arg[KUI_LAUNCH_ARG_MAX]; };
StagedArg g_staged[16];
uint32_t g_staged_next = 1;
Spinlock g_staged_lock;

uint32_t stage_arg(const char* arg) {
    if (!arg || !arg[0] || !kui_launch_path_ok(arg)) return 0;
    SpinLockGuard g(g_staged_lock);
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!g_staged[i].used) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < 16; i++) if (g_staged[i].token < g_staged[slot].token) slot = i;
    }
    uint32_t token = g_staged_next++;
    if (!g_staged_next) g_staged_next = 1;
    g_staged[slot].used = true;
    g_staged[slot].token = token;
    int i = 0;
    while (arg[i] && i < KUI_LAUNCH_ARG_MAX - 1) { g_staged[slot].arg[i] = arg[i]; i++; }
    g_staged[slot].arg[i] = 0;
    return token;
}

void unstage_arg(uint32_t token) {
    SpinLockGuard g(g_staged_lock);
    for (int i = 0; i < 16; i++) if (g_staged[i].used && g_staged[i].token == token) g_staged[i].used = false;
}

bool take_staged_arg(uint32_t token, char* out, int max) {
    if (!out || max <= 0) return false;
    out[0] = 0;
    SpinLockGuard g(g_staged_lock);
    for (int i = 0; i < 16; i++) {
        if (!g_staged[i].used || g_staged[i].token != token) continue;
        int p = 0;
        while (g_staged[i].arg[p] && p < max - 1) { out[p] = g_staged[i].arg[p]; p++; }
        out[p] = 0;
        g_staged[i].used = false;
        return true;
    }
    return false;
}

void KuiLaunchServerInit();
void KuiLaunchPump();

int app_index(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < APP_N; i++) {
        const char* a = g_apps[i].name;
        int k = 0;
        while (a[k] && a[k] == name[k]) k++;
        if (!a[k] && !name[k]) return i;
    }
    return -1;
}

// "all", "1" or a comma/space separated list containing name (satoru)
bool list_has(const char* list, const char* name) {
    if (!list || !list[0]) return false;
    for (int i = 0; list[i]; ) {
        while (list[i] == ',' || list[i] == ' ') i++;
        int s = i;
        while (list[i] && list[i] != ',' && list[i] != ' ') i++;
        int n = i - s;
        if (n <= 0) continue;
        if ((n == 3 && list[s] == 'a' && list[s + 1] == 'l' && list[s + 2] == 'l') || (n == 1 && list[s] == '1')) return true;
        int k = 0;
        while (k < n && name[k] && list[s + k] == name[k]) k++;
        if (k == n && !name[k]) return true;
    }
    return false;
}

void puthex(char* b, int& p, uint32_t v) {
    static const char* hx = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) b[p++] = hx[(v >> (i * 4)) & 0xF];
}
void putstr(char* b, int& p, const char* s) { while (*s) b[p++] = *s++; }
void putdec(char* b, int& p, uint32_t v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 11);
    while (n > 0) b[p++] = t[--n];
}
void putline(char* b, int& p, const char* k, uint32_t v) { putstr(b, p, k); b[p++] = '='; puthex(b, p, v); b[p++] = '\n'; }

// the theme ring-3 kui programs read (they cannot see /etc/kurono/ui.conf: linux /etc is (satoru)
// /system/etc). one key=hex per line. (satoru)
uint32_t write_theme_file() {
    const kui::Theme& t = kui::CurrentTheme();
    char b[512];
    int p = 0;
    putline(b, p, "dark", t.pal.dark ? 1 : 0);
    putline(b, p, "modern", t.style == kui::Style::Modern ? 1 : 0);
    putline(b, p, "minimal_icons", t.minimal_icons ? 1 : 0);
    putline(b, p, "reduced_motion", t.reduced_motion ? 1 : 0);
    putline(b, p, "high_contrast", t.high_contrast ? 1 : 0);
    putline(b, p, "ui_scale", (uint32_t)t.ui_scale);
    putline(b, p, "panel", t.pal.panel);   putline(b, p, "panel2", t.pal.panel2);
    putline(b, p, "line", t.pal.line);     putline(b, p, "text", t.pal.text);
    putline(b, p, "text2", t.pal.text2);   putline(b, p, "hover", t.pal.hover);
    putline(b, p, "danger", t.pal.danger); putline(b, p, "accent", t.pal.accent);
    b[p] = 0;
    uint32_t fp = 2166136261u;
    for (int i = 0; i < p; i++) fp = (fp ^ (uint8_t)b[i]) * 16777619u;
    if (fp != g_theme_fp) {
        KVFS::Mkdirs("/system/run/kui");
        KVFS::WriteFile("/system/run/kui/theme", b, (uint32_t)p);
    }
    return fp;
}

void publish(const char* path, const uint8_t* a, const uint8_t* b) {
    if (!a || !b || b <= a || KVFS::Exists(path)) return;
    KVFS::WriteFileStatic(path, a, (uint32_t)(b - a));   // zero copy: points at the kernel image (satoru)
}

// raise the running ring-3 instance: its bridged wayland window carries the app's title (satoru)
bool raise_running(const KuiApp& a) {
    Window* ws = WindowManager::GetWindows();
    if (!ws) return false;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* w = &ws[i];
        if (w->state == WIN_CLOSED || !WaylandServer::IsWaylandWindow(w->id)) continue;
        int k = 0;
        while (a.title[k] && w->title[k] == a.title[k]) k++;
        if (a.title[k] || w->title[k]) continue;
        if (w->state == WIN_MINIMIZED) WindowManager::Unminimize(w->id);
        WindowManager::Focus(w->id);
        return true;
    }
    return false;
}

}  // namespace

namespace {

int k_slen(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
bool k_ieq(const char* a, const char* b) {
    while (*a || *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        if (!ca) return true;
        a++; b++;
    }
    return true;
}
const char* k_ext(const char* name) {
    int n = k_slen(name);
    const char* dot = nullptr;
    for (int i = n - 1; i >= 0; i--) {
        if (name[i] == '.') { dot = name + i + 1; break; }
        if (name[i] == '/') break;
    }
    return dot ? dot : "";
}
bool k_ext_in(const char* name, const char* const* exts, int n) {
    const char* e = k_ext(name);
    for (int i = 0; i < n; i++) if (k_ieq(e, exts[i])) return true;
    return false;
}

const char* const k_text_ext[] = { "txt","c","h","cpp","cc","hpp","py","kcl","md","sh","cfg","json","ini","log","conf","html","css","js","rs","toml","yml","yaml","xml" };
const char* const k_media_ext[] = { "mp4","wav","mp3","ogg","flac","avi","mkv","webm","m4a","opus" };
const char* const k_image_ext[] = { "png","jpg","jpeg","bmp","gif","ppm","tga","ico","webp" };
const char* const k_kvid_ext[] = { "kvid","kvd" };

// per-app instance bookkeeping. an instance is armed from the desktop and turns running when (satoru)
// `kapp` has loaded it; the counters are atomic because launchers run on the aps under sched=2. (satoru)
void launch_started(int ai) {
    if (ai < 0 || ai >= 8) return;
    if (__atomic_load_n(&g_armed[ai], __ATOMIC_ACQUIRE) > 0)
        __atomic_sub_fetch(&g_armed[ai], 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&g_running[ai], 1, __ATOMIC_ACQ_REL);
}
void launch_failed(int ai, const char* arg = nullptr) {
    if (ai < 0 || ai >= 8) return;
    if (__atomic_load_n(&g_armed[ai], __ATOMIC_ACQUIRE) > 0)
        __atomic_sub_fetch(&g_armed[ai], 1, __ATOMIC_ACQ_REL);
    else if (__atomic_load_n(&g_running[ai], __ATOMIC_ACQUIRE) > 0)
        __atomic_sub_fetch(&g_running[ai], 1, __ATOMIC_ACQ_REL);
    int i = 0;
    if (arg) while (arg[i] && i < KUI_LAUNCH_ARG_MAX - 1) { g_fallback_arg[ai][i] = arg[i]; i++; }
    g_fallback_arg[ai][i] = 0;
    g_fallback[ai] = 1;
}
void launch_exited(int ai) {
    if (ai < 0 || ai >= 8) return;
    if (__atomic_load_n(&g_running[ai], __ATOMIC_ACQUIRE) > 0)
        __atomic_sub_fetch(&g_running[ai], 1, __ATOMIC_ACQ_REL);
}

}  // namespace

namespace KuiKernel {

void SetRing3Apps(const char* list) {
    int i = 0;
    if (list) while (list[i] && i < (int)sizeof(g_boot_list) - 1) { g_boot_list[i] = list[i]; i++; }
    g_boot_list[i] = 0;
}

bool Ring3Wanted(const char* name) {
    if (list_has(g_boot_list, name)) return true;
    return list_has(UIConfig::Str("apps.ring3", ""), name);
}

void SetTestOpen(const char* name) {
    int i = 0;
    if (name) while (name[i] && i < (int)sizeof(g_test_open) - 1) { g_test_open[i] = name[i]; i++; }
    g_test_open[i] = 0;
}

void Open(const char* name) { OpenArg(name, nullptr); }

void OpenArg(const char* name, const char* arg) {
    int ai = app_index(name);
    if (ai < 0 || ai >= APP_N) return;
    if (arg && arg[0]) {
        if (LaunchRing3WithArg(name, arg)) return;
        TaskMem::AppScope _tms(g_apps[ai].taskmem_app);
        if (g_apps[ai].launch_kernel_arg) g_apps[ai].launch_kernel_arg(arg);
        else g_apps[ai].launch_kernel();
        return;
    }
    if (LaunchRing3(name)) return;
    TaskMem::AppScope _tms(g_apps[ai].taskmem_app);
    g_apps[ai].launch_kernel();
}

bool OpenFile(const char* path) {
    if (!kui_launch_path_ok(path) || !KVFS::Exists(path)) return false;
    if (k_ext_in(path, k_kvid_ext, (int)(sizeof(k_kvid_ext) / sizeof(k_kvid_ext[0])))) {
        if (!DenjiApp::OpenFile(path)) OpenArg("mediaplayer", path);   // no kvid: the audio view (satoru)
    } else if (k_ext_in(path, k_media_ext, (int)(sizeof(k_media_ext) / sizeof(k_media_ext[0]))) ||
               k_ext_in(path, k_image_ext, (int)(sizeof(k_image_ext) / sizeof(k_image_ext[0])))) {
        OpenArg("mediaplayer", path);
    } else if (k_ext_in(path, k_text_ext, (int)(sizeof(k_text_ext) / sizeof(k_text_ext[0])))) {
        OpenArg("texteditor", path);
    } else {
        OpenArg("texteditor", path);   // the file manager's fallback for unknown types (satoru)
    }
    return true;
}

bool RunScript(const char* path) {
    if (!kui_launch_script_ok(path) || !KVFS::Exists(path)) return false;
    char cmd[KUI_LAUNCH_ARG_MAX + 8];
    int p = 0;
    putstr(cmd, p, "kcl ");
    putstr(cmd, p, path);
    cmd[p] = 0;
    TerminalApp::OpenKernel();   // a .kcl script runs in the in-kernel terminal, ring 3 or not (satoru)
    TerminalApp::EnqueueCommand(cmd);
    return true;
}

void RefreshDesktop() { Desktop::RefreshFiles(); }

void InstallApps() {
    if (g_installed) return;
    g_installed = true;
    KVFS::Mkdirs("/system/apps");
    char path[64];
    for (int i = 0; i < APP_N; i++) {
        int p = 0;
        putstr(path, p, "/system/apps/"); putstr(path, p, g_apps[i].name); path[p] = 0;
        publish(path, g_apps[i].elf, g_apps[i].elf_end);
    }
    KVFS::Mkdirs("/system/share/fonts/kurono");
    publish("/system/share/fonts/kurono/Inter-Regular.otf", _binary_inter_regular_otf_start, _binary_inter_regular_otf_end);
    publish("/system/share/fonts/kurono/Inter-SemiBold.otf", _binary_inter_semibold_otf_start, _binary_inter_semibold_otf_end);
    publish("/system/share/fonts/kurono/DejaVuSansMono.ttf", _binary_dejavu_mono_ttf_start, _binary_dejavu_mono_ttf_end);
    KVFS::Mkdirs("/system/share/kurono");
    publish("/system/share/kurono/modern_icons.bin", _binary_modern_icons_bin_start, _binary_modern_icons_bin_end);
    g_theme_fp = write_theme_file();
    SerialLogger::Log("[kui] ring-3 apps published under /system/apps\r\n");
}

void Pump() {
    if (!g_installed) InstallApps();
    SettingsService::Pump();
    KuiLaunchServerInit();
    kui::PumpWindows();
    KuiLaunchPump();
    uint32_t now = Timer::GetRealMs();
    if (g_test_open[0]) {
        if (!g_test_at) g_test_at = now + 4000;
        else if ((int32_t)(now - g_test_at) >= 0) {
            SerialLogger::Log("[kui] test open "); SerialLogger::Log(g_test_open); SerialLogger::Log("\r\n");
            Open(g_test_open);
            g_test_open[0] = 0;
        }
    }
    if (now - g_theme_check_ms >= 250u) {
        g_theme_check_ms = now;
        uint32_t fp = write_theme_file();
        if (fp != g_theme_fp) { g_theme_fp = fp; kui::ThemeChangedAll(); }
    }
    for (int i = 0; i < APP_N && i < 8; i++) {
        if (!g_fallback[i]) continue;
        g_fallback[i] = 0;
        SerialLogger::Log("[kui] ring-3 start failed, opening the in-kernel ");
        SerialLogger::Log(g_apps[i].name); SerialLogger::Log("\r\n");
        char farg[KUI_LAUNCH_ARG_MAX];
        int fp = 0;
        while (g_fallback_arg[i][fp] && fp < KUI_LAUNCH_ARG_MAX - 1) { farg[fp] = g_fallback_arg[i][fp]; fp++; }
        farg[fp] = 0;
        g_fallback_arg[i][0] = 0;
        TaskMem::AppScope _tms(g_apps[i].taskmem_app);
        if (farg[0] && g_apps[i].launch_kernel_arg) g_apps[i].launch_kernel_arg(farg);
        else g_apps[i].launch_kernel();
    }
}

void KeyEvent(int key, bool pressed, bool shift, bool ctrl, bool alt) {
    (void)ctrl; (void)alt;
    kui::KeyToFocused(key, pressed, shift, key);
}

static bool launch_ring3_impl(const char* name, const char* arg) {
    int ai = app_index(name);
    if (ai < 0 || ai >= 8 || !Ring3Wanted(name)) return false;
    if (!g_installed) InstallApps();
    char path[64];
    int p = 0;
    putstr(path, p, "/system/apps/"); putstr(path, p, name); path[p] = 0;
    if (!KVFS::Exists(path)) return false;
    bool has_arg = arg && arg[0];
    if (has_arg && !kui_launch_path_ok(arg)) return false;
    if (!has_arg) {
        // no argument: a second launch raises the running instance instead of starting another (satoru)
        if (__atomic_load_n(&g_running[ai], __ATOMIC_ACQUIRE) > 0) { raise_running(g_apps[ai]); return true; }
        if (__atomic_load_n(&g_armed[ai], __ATOMIC_ACQUIRE) > 0) return true;
    }
    char cmd[96];
    p = 0;
    putstr(cmd, p, "kapp "); putstr(cmd, p, name);
    uint32_t token = 0;
    if (has_arg) {
        token = stage_arg(arg);
        if (!token) return false;
        cmd[p++] = ' '; cmd[p++] = '@';
        putdec(cmd, p, token);
    }
    cmd[p] = 0;
    if (!KernelProcesses::RunShellCommandDetached(cmd)) {   // launcher busy: in-kernel (satoru)
        if (token) unstage_arg(token);
        return false;
    }
    __atomic_add_fetch(&g_armed[ai], 1, __ATOMIC_ACQ_REL);
    return true;
}

bool LaunchRing3(const char* name) { return launch_ring3_impl(name, nullptr); }
bool LaunchRing3WithArg(const char* name, const char* arg) { return launch_ring3_impl(name, arg); }

}  // namespace KuiKernel

namespace kui {

namespace Launch {

bool App(const char* name, const char* arg) {
    if (!kui_launch_app_ok(name) || app_index(name) < 0) return false;
    if (arg && arg[0] && !kui_launch_path_ok(arg)) return false;
    KuiKernel::OpenArg(name, (arg && arg[0]) ? arg : nullptr);
    return true;
}

bool Open(const char* path) { return KuiKernel::OpenFile(path); }

bool RunScript(const char* path) { return KuiKernel::RunScript(path); }

bool RefreshDesktop() { KuiKernel::RefreshDesktop(); return true; }

}  // namespace Launch

}  // namespace kui

// ---- the ring-3 launch request server (satoru)
// one fixed request per connection, validated here and again at dispatch. only registered app (satoru)
// names and checked paths get through, so no ring-3 program can ask for a shell command. (satoru)

namespace {

const int LREQ_N = 16;
struct LReq { uint32_t kind; char name[KUI_LAUNCH_NAME_MAX]; char arg[KUI_LAUNCH_ARG_MAX]; };
LReq g_lq[LREQ_N];
volatile int g_lq_head = 0, g_lq_tail = 0;
Spinlock g_lq_lock;
uint8_t g_lrx[UnixSocket::UNIX_MAX_SOCKETS][sizeof(KuiLaunchMsg)];
int g_lrx_len[UnixSocket::UNIX_MAX_SOCKETS];
bool g_linit = false;

void launch_reply(int sd, bool ok, const char* why) {
    KuiLaunchReply rp;
    rp.magic = KUI_LAUNCH_REPLY;
    rp.ok = ok ? 1 : 0;
    int i = 0;
    while (why && why[i] && i < (int)sizeof(rp.why) - 1) { rp.why[i] = why[i]; i++; }
    rp.why[i] = 0;
    UnixSocket::KernelInject(sd, &rp, (int)sizeof(rp));
}

void launch_handle(int sd, const KuiLaunchMsg& m) {
    bool ok = false;
    const char* why = "bad request";
    char name[KUI_LAUNCH_NAME_MAX];
    char arg[KUI_LAUNCH_ARG_MAX];
    name[0] = arg[0] = 0;
    int nl = 0;
    while (nl < KUI_LAUNCH_NAME_MAX - 1 && m.name[nl]) { name[nl] = m.name[nl]; nl++; }
    name[nl] = 0;
    int al = 0;
    while (al < KUI_LAUNCH_ARG_MAX - 1 && m.arg[al]) { arg[al] = m.arg[al]; al++; }
    arg[al] = 0;

    if (m.magic != KUI_LAUNCH_MAGIC) {
        why = "bad magic";
    } else if (m.kind == KUI_LAUNCH_APP) {
        if (!kui_launch_app_ok(name) || app_index(name) < 0) why = "unknown app";
        else if (arg[0] && !kui_launch_path_ok(arg)) why = "bad path";
        else ok = true;
    } else if (m.kind == KUI_LAUNCH_OPEN) {
        if (!kui_launch_path_ok(arg)) why = "bad path";
        else ok = true;
    } else if (m.kind == KUI_LAUNCH_KCL) {
        if (!kui_launch_script_ok(arg)) why = "bad script path";
        // a script handed to the shell runs code, so the requesting process
        // needs administrator approval like any other privileged action. (satoru)
        else if (!AdminGate::Authorize("run script", arg)) why = "approval denied";
        else ok = true;
    } else if (m.kind == KUI_LAUNCH_REFRESH) {
        ok = true;
    } else {
        why = "bad kind";
    }

    if (ok) {
        SpinLockGuard g(g_lq_lock);
        int next = (g_lq_head + 1) % LREQ_N;
        if (next == g_lq_tail) {
            ok = false; why = "busy";
        } else {
            LReq& r = g_lq[g_lq_head];
            r.kind = m.kind;
            int i = 0;
            while (name[i] && i < KUI_LAUNCH_NAME_MAX - 1) { r.name[i] = name[i]; i++; }
            r.name[i] = 0;
            i = 0;
            while (arg[i] && i < KUI_LAUNCH_ARG_MAX - 1) { r.arg[i] = arg[i]; i++; }
            r.arg[i] = 0;
            g_lq_head = next;
        }
    }
    launch_reply(sd, ok, ok ? "queued" : why);
}

void launch_on_connect(int server_sd, int new_sd, void* user) {
    (void)server_sd; (void)user;
    if (new_sd >= 0 && new_sd < UnixSocket::UNIX_MAX_SOCKETS) g_lrx_len[new_sd] = 0;
}

void launch_on_data(int sd, const uint8_t* data, int len, void* user) {
    (void)user;
    if (sd < 0 || sd >= UnixSocket::UNIX_MAX_SOCKETS || !data || len <= 0) return;
    int have = g_lrx_len[sd];
    if (have < 0 || have > (int)sizeof(KuiLaunchMsg)) have = 0;
    for (int i = 0; i < len && have < (int)sizeof(KuiLaunchMsg); i++) g_lrx[sd][have++] = data[i];
    while (have >= (int)sizeof(KuiLaunchMsg)) {
        KuiLaunchMsg m;
        for (int i = 0; i < (int)sizeof(KuiLaunchMsg); i++) ((uint8_t*)&m)[i] = g_lrx[sd][i];
        launch_handle(sd, m);
        int rem = have - (int)sizeof(KuiLaunchMsg);
        for (int i = 0; i < rem; i++) g_lrx[sd][i] = g_lrx[sd][i + (int)sizeof(KuiLaunchMsg)];
        have = rem;
    }
    g_lrx_len[sd] = have;
}

void KuiLaunchPump() {
    for (;;) {
        LReq r;
        {
            SpinLockGuard g(g_lq_lock);
            if (g_lq_tail == g_lq_head) break;
            r = g_lq[g_lq_tail];
            g_lq_tail = (g_lq_tail + 1) % LREQ_N;
        }
        if (r.kind == KUI_LAUNCH_APP) {
            SerialLogger::Log("[kui] launch request "); SerialLogger::Log(r.name);
            if (r.arg[0]) { SerialLogger::Log(" "); SerialLogger::Log(r.arg); }
            SerialLogger::Log("\r\n");
            KuiKernel::OpenArg(r.name, r.arg[0] ? r.arg : nullptr);
        } else if (r.kind == KUI_LAUNCH_OPEN) {
            SerialLogger::Log("[kui] open request "); SerialLogger::Log(r.arg); SerialLogger::Log("\r\n");
            KuiKernel::OpenFile(r.arg);
        } else if (r.kind == KUI_LAUNCH_KCL) {
            SerialLogger::Log("[kui] run script "); SerialLogger::Log(r.arg); SerialLogger::Log("\r\n");
            KuiKernel::RunScript(r.arg);
        } else if (r.kind == KUI_LAUNCH_REFRESH) {
            SerialLogger::Log("[kui] desktop refresh request\r\n");
            KuiKernel::RefreshDesktop();
        }
    }
}

void KuiLaunchServerInit() {
    if (g_linit) return;
    g_linit = true;
    KVFS::Mkdirs("/system/run/user/1000");
    int sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (sd < 0) return;
    if (UnixSocket::Bind(sd, KUI_LAUNCH_PATH) < 0) { UnixSocket::Close(sd); return; }
    UnixSocket::Listen(sd, 8);
    UnixSocket::RegisterServer(sd, launch_on_connect, launch_on_data, nullptr);
    SerialLogger::Log("[kui] launch requests on " KUI_LAUNCH_PATH "\r\n");
}

}  // namespace

static int k_append(char* out, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) out[p++] = *s++;
    if (p < mx) out[p] = 0;
    return p;
}

int ShellBuiltins::cmd_kapp(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2 || !argv[1]) return k_append(out, 0, mx, "usage: kapp <name> [args]   (ring-3 kui apps in /system/apps)\n");
    const char* name = argv[1];
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') || i > 40)
            return k_append(out, 0, mx, "kapp: bad app name\n");
    }
    // the shell reuses its command buffer for later commands: keep our own copy, or the exit log
    // names whichever app ran last. (satoru)
    char appname[48];
    int an = 0;
    while (name[an] && an < (int)sizeof(appname) - 1) { appname[an] = name[an]; an++; }
    appname[an] = 0;
    name = appname;

    // an optional "@token" argument carries a staged path from the desktop launcher, so a file (satoru)
    // name with spaces or quotes is passed as argv[1] and never parsed by the shell. (satoru)
    uint32_t token = 0;
    bool has_token = false;
    if (argc >= 3 && argv[2] && argv[2][0] == '@') {
        const char* t = argv[2] + 1;
        if (*t) {
            has_token = true;
            while (*t) {
                if (*t < '0' || *t > '9') { has_token = false; break; }
                token = token * 10u + (uint32_t)(*t - '0');
                t++;
            }
        }
    }
    char launch_arg[KUI_LAUNCH_ARG_MAX];
    launch_arg[0] = 0;
    if (has_token) take_staged_arg(token, launch_arg, (int)sizeof(launch_arg));

    KuiKernel::InstallApps();
    int ai = app_index(name);
    char path[64];
    int p = 0;
    putstr(path, p, "/system/apps/"); putstr(path, p, name); path[p] = 0;
    if (!KVFS::Exists(path)) {
        if (ai >= 0 && ai < 8) launch_failed(ai, launch_arg);
        p = k_append(out, 0, mx, "kapp: ");
        p = k_append(out, p, mx, path);
        return k_append(out, p, mx, " is not in this build\n");
    }
    if (!Userspace::IsReady()) Userspace::Init();
    Process* proc = ElfLoader::LoadELF64FromVFS(path, name);
    if (!proc) {
        if (ai >= 0 && ai < 8) launch_failed(ai, launch_arg);
        return k_append(out, 0, mx, "kapp: failed to load the binary\n");
    }
    if (ai >= 0 && ai < 8) launch_started(ai);
    // a pending open-with path wins; otherwise extra words after the app name go to the program. (satoru)
    const char* av[6];
    int ac = 0;
    av[ac++] = name;
    if (launch_arg[0]) av[ac++] = launch_arg;
    else for (int i = 2; i < argc && ac < 5; i++) av[ac++] = argv[i] ? argv[i] : "";
    av[ac] = nullptr;
    const char* envp[] = { "PATH=/system/bin:/usr/bin", "HOME=/home/user",
                           "XDG_RUNTIME_DIR=/system/run/user/1000", "WAYLAND_DISPLAY=wayland-0",
                           "KUI_THEME=/system/run/kui/theme", nullptr };
    LinuxSyscall::ClearConsoleOutput();
    SerialLogger::Log("[kui] ring-3 "); SerialLogger::Log(name); SerialLogger::Log(" start\r\n");
    int rc = Userspace::RunProcessWithArgs(proc, av, envp, 1000, 1000);   // kui apps run as the user (satoru)
    Scheduler::DestroyProcess(proc);
    if (ai >= 0 && ai < 8) {
        launch_exited(ai);
        // -2: this cpu already runs a ring-3 session (the old scheduler mode while firefox is up) (satoru)
        if (rc == -2 || rc == -3) {
            int fi = 0;
            while (launch_arg[fi] && fi < KUI_LAUNCH_ARG_MAX - 1) { g_fallback_arg[ai][fi] = launch_arg[fi]; fi++; }
            g_fallback_arg[ai][fi] = 0;
            g_fallback[ai] = 1;
        }
    }
    p = LinuxSyscall::ReadConsoleOutput(out, mx - 1);
    if (p < 0) p = 0;
    out[p] = 0;
    SerialLogger::Log("[kui] ring-3 "); SerialLogger::Log(name); SerialLogger::Log(" exited ");
    SerialLogger::LogDec(rc); SerialLogger::Log("\r\n");
    p = k_append(out, p, mx, "[");
    p = k_append(out, p, mx, name);
    p = k_append(out, p, mx, rc == -2 ? " not started: a ring-3 session is active, opened in-kernel]\n" : " exited]\n");
    return p;
}

// end (satoru)
