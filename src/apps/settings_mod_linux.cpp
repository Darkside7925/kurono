//  kurono os - settings module: linux (satoru)
//  the linux virtual machine (virt/vm_host.cpp): which distribution, the
//  master switch, start at login or on demand, guest memory, live status with
//  start / stop / restart / force stop, the disk image and the space used
//  inside the guest, and the guest's own GUI apps with "show in dock".
//  everything persists in ui.conf ("linux.*"); vm_host reads the same keys at
//  boot and on every start. guest questions (df, .desktop files) go through the
//  guest agent on a small worker process, never on the gui thread. (satoru)
#include "system_settings.h"
#include "settings_linux.h"
#include "alpine_term.h"
#include "../virt/vm_host.h"
#include "../virt/vmm.h"
#include "../virt/hypervisor.h"
#include "../virt/guest_mem.h"
#include "../drivers/disklog.h"
#include "../drivers/timer.h"
#include "../drivers/graphics.h"
#include "../proc/scheduler.h"
#include "../system/ui_config.h"
#include "../kernel/pmm.h"
#include "../drivers/serial.h"

using namespace SettingsUI;

// ---- page state (satoru)
static bool s_on = true;
static int  s_auto = 0;            // 0 when needed, 1 at login (satoru)
static int  s_ram = 1024;
static int  s_ram_lo = 256, s_ram_hi = 2048;
static uint64_t s_img_bytes = 0;
static int  s_img_why = 1;         // DiskLog::GuestImage result (satoru)

// ---- guest answers, filled by the worker (satoru)
static const int MAX_APPS = 16;
struct GuestApp { char id[40]; char name[32]; char exec[48]; };
static GuestApp s_apps[MAX_APPS];
static volatile int s_app_count = 0;
static bool s_app_dock[MAX_APPS];
static volatile uint64_t s_df_total_kb = 0, s_df_used_kb = 0;
static volatile int  s_q_state = 0;          // 0 none, 1 asked, 2 running, 3 done, 4 failed (satoru)
static uint32_t s_q_gen = 0;                 // vm generation the answers belong to (satoru)
static uint32_t s_q_ms = 0;                  // when they were asked for (satoru)
static volatile int s_launch = -1;           // dock slot waiting for the agent (satoru)
static Process* s_worker = nullptr;

// ---- dock slots: linux.dock0..7 = "Name|exec" (empty = free) (satoru)
static const int DOCK_SLOTS = 8;
static char s_dock_name[DOCK_SLOTS][32];
static char s_dock_exec[DOCK_SLOTS][48];
static bool s_dock_loaded = false;

static bool s_eq(const char* a, const char* b){
    if(!a || !b) return false;
    while(*a && *a == *b){ a++; b++; }
    return *a == 0 && *b == 0;
}
static void slot_key(int i, char* k){ StrCpy(k, "linux.dock", 16); char d[2] = { (char)('0' + i), 0 }; StrApp(k, d, 16); }

static void dock_load(){
    for(int i = 0; i < DOCK_SLOTS; i++){
        char k[16]; slot_key(i, k);
        const char* v = UIConfig::Str(k, "");
        int a = 0, b = 0, p = 0;
        while(v[p] && v[p] != '|' && a < 31) s_dock_name[i][a++] = v[p++];
        s_dock_name[i][a] = 0;
        while(v[p] && v[p] != '|') p++;
        if(v[p] == '|') p++;
        while(v[p] && b < 47) s_dock_exec[i][b++] = v[p++];
        s_dock_exec[i][b] = 0;
        if(!s_dock_exec[i][0]) s_dock_name[i][0] = 0;
    }
    s_dock_loaded = true;
}
static int dock_find(const char* exec){
    for(int i = 0; i < DOCK_SLOTS; i++) if(s_dock_exec[i][0] && s_eq(s_dock_exec[i], exec)) return i;
    return -1;
}
static void dock_set(const GuestApp& a, bool on){
    if(!s_dock_loaded) dock_load();
    int i = dock_find(a.exec);
    char k[16];
    if(on){
        if(i >= 0) return;
        for(i = 0; i < DOCK_SLOTS && s_dock_exec[i][0]; i++) {}
        if(i >= DOCK_SLOTS) return;           // full (satoru)
        char v[48]; StrCpy(v, a.name, 48); StrApp(v, "|", 48); StrApp(v, a.exec, 48);
        slot_key(i, k); UIConfig::Set(k, v, true);
    } else {
        if(i < 0) return;
        slot_key(i, k); UIConfig::Set(k, "", true);
    }
    UIConfig::Save();
    dock_load();
    Graphics::MarkUIDirty();                  // the dock changes (satoru)
}

// ---- the worker: guest agent questions and dock launches (satoru)
static const char* kAppsCmd =
    "for f in /usr/share/applications/*.desktop; do [ -f \"$f\" ] || continue; "
    "grep -q '^NoDisplay=true' \"$f\" && continue; grep -q '^Terminal=true' \"$f\" && continue; "
    "n=$(grep -m1 '^Name=' \"$f\" | cut -d= -f2-); e=$(grep -m1 '^Exec=' \"$f\" | cut -d= -f2- | sed 's/ *%.*//'); "
    "[ -n \"$e\" ] && echo \"$(basename \"$f\" .desktop)|$n|$e\"; done";

static uint64_t parse_u64(const char*& p){
    while(*p == ' ' || *p == '\t') p++;
    uint64_t v = 0;
    while(*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
    return v;
}

static void parse_apps(const char* out){
    int n = 0;
    const char* p = out;
    while(*p && n < MAX_APPS){
        const char* e = p; while(*e && *e != '\n') e++;
        GuestApp& a = s_apps[n];
        int f = 0, k = 0; a.id[0] = a.name[0] = a.exec[0] = 0;
        for(const char* q = p; q < e; q++){
            if(*q == '|' && f < 2){ f++; k = 0; continue; }
            char* dst = f == 0 ? a.id : (f == 1 ? a.name : a.exec);
            int cap = f == 0 ? 40 : (f == 1 ? 32 : 48);
            if(k < cap - 1){ dst[k++] = *q; dst[k] = 0; }
        }
        if(f == 2 && a.exec[0]){ if(!a.name[0]) StrCpy(a.name, a.id, 32); n++; }
        p = *e ? e + 1 : e;
    }
    s_app_count = n;
}

static void worker_entry(){
    static char out[8192];
    for(;;){
        if(s_launch >= 0){
            int i = s_launch;
            bool up = VMHost::WaitAgent(120000);
            if(up && i < DOCK_SLOTS && s_dock_exec[i][0]){
                char line[64]; StrCpy(line, "run ", 64); StrApp(line, s_dock_exec[i], 64);
                VMHost::AgentSend(line);
                SerialLogger::Log("[linux] dock launch: "); SerialLogger::Log(line); SerialLogger::Log("\r\n");
            } else SerialLogger::Log("[linux] dock launch dropped (no guest agent)\r\n");
            s_launch = -1;
        }
        if(s_q_state == 1){
            if(!VMHost::AgentReady()){ s_q_state = 4; continue; }
            s_q_state = 2;
            uint32_t gen = VMHost::Generation();
            int rc = -1;
            int n = VMHost::Exec("df -kP / | tail -n 1", out, (int)sizeof(out), 15000, &rc);
            if(n > 0){
                // "dev 1k-blocks used available capacity mount" (satoru)
                const char* p = out;
                while(*p && *p != ' ' && *p != '\t') p++;
                uint64_t total = parse_u64(p), used = parse_u64(p);
                if(total){ s_df_total_kb = total; s_df_used_kb = used; }
            }
            n = VMHost::Exec(kAppsCmd, out, (int)sizeof(out), 20000, &rc);
            if(n >= 0) parse_apps(out);
            s_q_gen = gen;
            s_q_state = n >= 0 ? 3 : 4;
            Graphics::MarkUIDirty();
        }
        Scheduler::SleepMs(200);
    }
}

static void ensure_worker(){
    if(!s_worker && Scheduler::IsPreemptiveKernelSchedulerActive())
        s_worker = Scheduler::SpawnKernelProcess("linuxq", worker_entry, PRIO_NORMAL, 64, 128);
}

static void ask_guest(){
    if(s_q_state == 1 || s_q_state == 2) return;
    ensure_worker();
    if(s_worker){ s_q_state = 1; s_q_ms = Timer::GetRealMs(); }
}

// ---- helpers (satoru)
static void size_str(uint64_t bytes, char* b, int mx){
    uint64_t mb = bytes >> 20, gb = mb / 1024;
    if(gb >= 1){
        IntToStr((int)gb, b, mx); StrApp(b, ".", mx);
        char f[4]; IntToStr((int)((mb % 1024) * 10 / 1024), f, 4); StrApp(b, f, mx);
        StrApp(b, " GB", mx); return;
    }
    IntToStr((int)mb, b, mx); StrApp(b, " MB", mx);
}
static void ram_str(int mb, char* b, int mx){
    if(mb >= 1024 && mb % 1024 == 0){ IntToStr(mb / 1024, b, mx); StrApp(b, " GB", mx); return; }
    if(mb >= 1024){ size_str((uint64_t)mb << 20, b, mx); return; }
    IntToStr(mb, b, mx); StrApp(b, " MB", mx);
}
static void uptime_str(uint32_t ms, char* b, int mx){
    uint32_t s = ms / 1000, m = s / 60, h = m / 60;
    char t[12];
    b[0] = 0;
    if(h){ IntToStr((int)h, t, 12); StrApp(b, t, mx); StrApp(b, " h ", mx); IntToStr((int)(m % 60), t, 12); StrApp(b, t, mx); StrApp(b, " min", mx); }
    else if(m){ IntToStr((int)m, t, 12); StrApp(b, t, mx); StrApp(b, " min", mx); }
    else { IntToStr((int)s, t, 12); StrApp(b, t, mx); StrApp(b, " s", mx); }
}
static bool vtx(){ return Hypervisor::IsAvailable() || VMM::IsSupported(); }

static void linux_on_show(){
    s_on = VMHost::Enabled();
    s_auto = UIConfig::Bool("linux.autostart", false) ? 1 : 0;
    // guest ram: 256 MB up to half the installed ram, below the hypervisor's cap (satoru)
    uint64_t total_mb = PMM::GetTotalMemory() >> 20;
    int hi = (int)(total_mb / 2);
    if(hi > (int)GUEST_MAX_RAM_MB) hi = (int)GUEST_MAX_RAM_MB;
    hi = (hi / 128) * 128;
    if(hi < 512) hi = 512;
    s_ram_lo = 256; s_ram_hi = hi;
    s_ram = (int)VMHost::ConfiguredRamMb();
    if(s_ram < s_ram_lo) s_ram = s_ram_lo;
    if(s_ram > s_ram_hi) s_ram = s_ram_hi;
    const VMHost::DistroDesc* d = VMHost::GetDistro(VMHost::SelectedDistro());
    s_img_bytes = 0;
    s_img_why = DiskLog::GuestImage(d->image83, nullptr, &s_img_bytes);
    if(!s_dock_loaded) dock_load();
    if(VMHost::AgentReady()) ask_guest();
}

static void save_int(const char* k, int v){ UIConfig::SetInt(k, v, true); UIConfig::Save(); }

static void linux_build(){
    char buf[96], sub[128];
    VMHost::Info vi; VMHost::GetInfo(&vi);
    bool running = vi.state == VMHost::VMH_RUNNING;
    bool busy = vi.state == VMHost::VMH_STARTING || vi.state == VMHost::VMH_STOPPING || vi.start_pending;

    // the page shows live state: keep it redrawing while it is open (satoru)
    if(Drawing()) Graphics::RequestFrameAt(Timer::GetRealMs() + (busy ? 250 : 1000));
    // answers from the guest once its agent is up (and again after every boot) (satoru)
    // and every 30 s while the page is open, so apps installed meanwhile show up (satoru)
    if(running && vi.agent && s_q_state != 1 && s_q_state != 2 &&
       (s_q_gen != VMHost::Generation() || Timer::GetRealMs() - s_q_ms > 30000)) ask_guest();

    Group("Linux");
    if(Toggle("Use Linux", &s_on, "Runs a Linux system in a virtual machine next to Kurono")){
        save_int("linux.enabled", s_on ? 1 : 0);
        if(!s_on && vi.state != VMHost::VMH_OFF && vi.state != VMHost::VMH_FAILED) VMHost::Stop();
        Graphics::MarkUIDirty();
    }
    if(!vtx()) Info("Virtualization", "Not available", "This processor has no Intel VT-x, so Linux cannot run", T().warn);
    EndGroup();
    if(!s_on){
        Note("Linux is off. The virtual machine never starts, the Alpine tile is hidden from the dock and vm commands refuse to run.");
        return;
    }

    Group("Distribution");
    {
        int sel = VMHost::SelectedDistro();
        for(int i = 0; i < VMHost::DistroCount(); i++){
            const VMHost::DistroDesc* d = VMHost::GetDistro(i);
            PushId(i);
            // a distro only counts as installed when its disk image (and, for
            // debian, its kernel files) are on the KURONO partition; "In use"
            // is the running one, "Selected" the one the next start uses. (satoru)
            int inst = VMHost::DistroInstalled(i);
            bool active = vi.state != VMHost::VMH_OFF && vi.state != VMHost::VMH_FAILED;
            if(d->status != VMHost::DS_READY){
                StrCpy(sub, d->version, 128); StrApp(sub, ". ", 128); StrApp(sub, d->note, 128);
                Info(d->name, "Coming soon", sub);
            } else if(inst != 0){
                StrCpy(sub, inst == 4 ? "Its kernel files are missing from" : (inst == 2 ? "Its disk image is fragmented on" : "Its disk image is not on"), 128);
                StrApp(sub, " the KURONO partition (", 128); StrApp(sub, d->image_name, 128); StrApp(sub, ")", 128);
                Info(d->name, "Not installed", sub, T().warn);
            } else if(i == sel){
                if(active && vi.distro != i)
                    Info(d->name, "Selected", "Restart Linux to switch to it", T().warn);
                else
                    Info(d->name, active ? "In use" : "Selected", active ? d->version : "Used from the next start", T().success);
            } else if(Button(d->name, "Use", BTN_SECONDARY, active && vi.distro == i ? "Running now" : d->version)){
                UIConfig::Set("linux.distro", d->id, true);
                UIConfig::Save();
                linux_on_show();
            }
            PopId();
        }
    }
    EndGroup();

    Group("Status");
    {
        const char* st = "Stopped";
        uint32_t dot = T().text3;
        const char* why = nullptr;
        switch(vi.state){
            case VMHost::VMH_STARTING: st = "Starting"; dot = T().warn; break;
            case VMHost::VMH_RUNNING:  st = vi.agent ? "Running" : "Booting"; dot = vi.agent ? T().success : T().warn; break;
            case VMHost::VMH_STOPPING: st = "Shutting down"; dot = T().warn; break;
            case VMHost::VMH_FAILED:
                st = vi.loop_stopped ? "Stopped, crash loop" : "Failed";
                dot = T().danger;
                why = vi.loop_stopped ? "It rebooted 4 times in 2 minutes (kernel panic?). Start tries again."
                                      : (vi.error && vi.error[0] ? vi.error : nullptr);
                break;
            default:
                if(vi.start_pending){ st = "Starting soon"; dot = T().warn; }
                break;
        }
        if(!why && vi.state == VMHost::VMH_OFF && !vi.start_pending)
            why = s_auto ? "Starts by itself after the next login" : "Starts when you open the terminal or a Linux app";
        Info("State", st, why, dot);
        // one row set while the vm is up or on its way (a restart passes
        // through off), so the page does not jump while it changes state (satoru)
        bool active = (vi.state != VMHost::VMH_OFF && vi.state != VMHost::VMH_FAILED) || vi.start_pending;
        if(active){
            if(running) uptime_str(vi.uptime_ms, buf, 96); else StrCpy(buf, "Not yet", 96);
            Info("Uptime", buf);
            bool differs = vi.ram_mb && (int)vi.ram_mb != (int)VMHost::ConfiguredRamMb();
            if(vi.ram_mb) ram_str((int)vi.ram_mb, buf, 96); else StrCpy(buf, "Not yet", 96);
            Info("Memory in use", buf, differs ? "Restart Linux to use the new memory size" : "As set below");
            if(Button("Shut down", "Stop", BTN_SECONDARY, "Asks Linux to power off cleanly")) VMHost::Stop();
            if(Button("Restart", "Restart", BTN_SECONDARY, "Shuts down, then starts again")) VMHost::Restart();
            if(Button("Force stop", "Force stop", BTN_DESTRUCTIVE, "Turns it off at once; unsaved work inside Linux is lost")) VMHost::Kill();
        } else if(Button("Start Linux", "Start", BTN_PRIMARY, nullptr)){
            if(!VMHost::Start(VMHost::SelectedDistro(), 0)) Graphics::MarkUIDirty();
        }
        if(Button("Terminal", "Open", BTN_SECONDARY, "The Linux console in a Kurono window")) AlpineTerminal::Open();
    }
    EndGroup();

    Group("Startup");
    {
        static const char* const kStart[] = { "When needed", "At login" };
        if(Segmented("Start Linux", kStart, 2, &s_auto,
                     s_auto ? "Starts in the background once the desktop is up" : "The terminal or a Linux app starts it")){
            save_int("linux.autostart", s_auto);
        }
    }
    EndGroup();

    Group("Resources");
    {
        ram_str(s_ram, buf, 96);
        if(Slider("Memory", &s_ram, s_ram_lo, s_ram_hi, 128, buf, "Used from the next start") && Committed())
            save_int("linux.ram_mb", s_ram);
        if(VMHost::RamFromCmdline()){
            ram_str((int)VMHost::ConfiguredRamMb(), buf, 96);
            Info("Boot option kurono.vmram", buf, "Overrides the memory setting until the next restart", T().warn);
        }
        Info("Processors", "1", "The Kurono hypervisor gives Linux one virtual processor");
        size_str(PMM::GetTotalMemory(), buf, 96);
        Info("Installed memory", buf);
    }
    EndGroup();

    Group("Storage");
    {
        const VMHost::DistroDesc* d = VMHost::GetDistro(VMHost::SelectedDistro());
        StrCpy(sub, d->image_name, 128); StrApp(sub, " on the KURONO partition", 128);
        if(s_img_why == 0){ size_str(s_img_bytes, buf, 96); Info("Disk image", buf, sub); }
        else Info("Disk image", s_img_why == 2 ? "Fragmented" : "Not found", sub, T().warn);
        if(s_df_total_kb && s_q_gen == VMHost::Generation() && running){
            uint64_t t = s_df_total_kb * 1024, u = s_df_used_kb * 1024;
            char tb[24]; size_str(u, buf, 96); StrApp(buf, " of ", 96); size_str(t, tb, 24); StrApp(buf, tb, 96);
            Meter("Used inside Linux", buf, (int)(s_df_used_kb * 100 / s_df_total_kb));
        } else {
            Info("Used inside Linux", running ? (s_q_state == 4 ? "Unknown" : "Checking...") : "Start Linux to see");
        }
    }
    EndGroup();

    Group("Apps");
    {
        bool fresh = running && s_q_gen == VMHost::Generation() && s_q_state == 3;
        if(fresh && s_app_count > 0){
            for(int i = 0; i < s_app_count; i++){
                PushId(100 + i);
                s_app_dock[i] = dock_find(s_apps[i].exec) >= 0;
                if(Toggle(s_apps[i].name, &s_app_dock[i], "Show in dock")) dock_set(s_apps[i], s_app_dock[i]);
                PopId();
            }
        } else if(fresh){
            Info("Linux apps", "None installed", "Install one in the terminal, for example: apk add foot");
        } else {
            int pinned = 0;
            for(int i = 0; i < DOCK_SLOTS; i++) if(s_dock_exec[i][0]) pinned++;
            IntToStr(pinned, buf, 96); StrApp(buf, " in the dock", 96);
            Info("Linux apps", running ? "Checking..." : buf, running ? nullptr : "Start Linux to see all its apps");
        }
    }
    EndGroup();
}

extern const SettingsModule g_linux_module = {
    "linux", "Linux", "", linux_on_show, nullptr, nullptr, nullptr, linux_build
};

// ---- desktop hooks (settings_linux.h) (satoru)
bool LinuxSettings::ShowLinuxTile(){ return VMHost::Enabled(); }
int LinuxSettings::DockAppCount(){
    if(!s_dock_loaded) dock_load();
    if(!VMHost::Enabled()) return 0;
    int n = 0;
    for(int i = 0; i < DOCK_SLOTS; i++) if(s_dock_exec[i][0]) n++;
    return n;
}
static int nth_slot(int i){
    for(int s = 0; s < DOCK_SLOTS; s++) if(s_dock_exec[s][0] && i-- == 0) return s;
    return -1;
}
const char* LinuxSettings::DockAppName(int i){
    int s = nth_slot(i);
    return s >= 0 ? s_dock_name[s] : "";
}
void LinuxSettings::LaunchDockApp(int i){
    int s = nth_slot(i);
    if(s < 0 || !VMHost::Enabled()) return;
    ensure_worker();
    if(!s_worker) return;
    VMHost::State st = VMHost::GetState();
    if(st == VMHost::VMH_OFF || st == VMHost::VMH_FAILED) VMHost::Start(VMHost::SelectedDistro(), 0);
    s_launch = s;
}
void LinuxSettings::OpenPage(){ SystemSettings::OpenPage("linux"); }
// end (satoru)
