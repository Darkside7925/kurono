//  kurono os - settings module: system (satoru)
//  software update (sync the package repository on demand), the linux
//  compatibility subsystem (start / stop live), packages, and KMemX memory
//  compression (enable, pool size, aggressiveness, live statistics). (satoru)
#include "system_settings.h"
#include "../system/ui_config.h"
#include "../system/system_update.h"
#include "../linux/kls.h"
#include "../packages/pkgmgr.h"
#include "../kernel/kmemx.h"

using namespace SettingsUI;

static bool s_linux = true;
static bool s_kmx = false;
static int  s_pool = 20;
static int  s_aggr = 6;        // 0..12, maps to generation threshold 16..4 (satoru)
static char s_sync_msg[64];

static const char* guest_state(){
    switch(KLS::GetState()){
        case KLS_STOPPED:      return "Stopped";
        case KLS_INITIALIZING: return "Starting";
        case KLS_RUNNING:      return "Running";
        case KLS_ERROR:        return "Error";
        case KLS_SUSPENDED:    return "Suspended";
        default:               return "Unknown";
    }
}

static void system_on_show(){
    s_linux = KLS::GetState() != KLS_STOPPED;
    s_kmx = KMemX::IsEnabled();
    s_pool = KMemX::PoolPct();
    if(s_pool < 10) s_pool = 10;
    if(s_pool > 40) s_pool = 40;
    int thr = KMemX::Threshold();
    if(thr < 4) thr = 4;
    if(thr > 16) thr = 16;
    s_aggr = 16 - thr;
    s_sync_msg[0] = 0;
}

static void system_build(){
    char buf[64];

    Group("Software update");
    {
        const char* msg = PackageManager::GetLastSyncMessage();
        Info("Last check", s_sync_msg[0] ? s_sync_msg : ((msg && msg[0]) ? msg : "Never"));
        int pend = PackageManager::GetPendingUpdateCount();
        IntToStr(pend, buf, 64); StrApp(buf, pend == 1 ? " update" : " updates", 64);
        Info("Available", pend ? buf : "Up to date");
        if(SystemUpdate::HasPendingUpdate()) Info("Restart required", "Yes", "An update is installed and waits for a restart", T().warn);
        if(Button("Check for updates", "Check now", BTN_PRIMARY, PackageManager::GetRepositoryHost())){
            bool ok = PackageManager::SyncRepository();
            StrCpy(s_sync_msg, ok ? "Just now" : "Failed (no connection?)", 64);
        }
    }
    EndGroup();

    Group("Linux compatibility");
    {
        if(Toggle("Linux subsystem", &s_linux, "Runs Linux programs such as Firefox")){
            UIConfig::SetInt("system.linux_enabled", s_linux ? 1 : 0, true);
            UIConfig::Save();
            if(s_linux){ if(KLS::GetState() == KLS_STOPPED) KLS::Start(); }
            else if(KLS::GetState() == KLS_RUNNING) KLS::Stop();
        }
        uint32_t st_col = KLS::GetState() == KLS_RUNNING ? T().success : (KLS::GetState() == KLS_ERROR ? T().danger : T().text3);
        Info("Status", guest_state(), nullptr, st_col);
        Info("Kernel version", KLS::GetKernelVersion());
        IntToStr(KLS::LinuxProcessCount(), buf, 64);
        Info("Running programs", buf);
    }
    EndGroup();

    Group("Packages");
    IntToStr(PackageManager::InstalledCount(), buf, 64);  Info("Installed", buf);
    IntToStr(PackageManager::AvailableCount(), buf, 64);  Info("Available to install", buf);
    Info("Repository", PackageManager::GetRepositoryHost());
    EndGroup();

    Group("Memory compression");
    {
        if(Toggle("Compress inactive memory", &s_kmx, "KMemX keeps idle pages compressed in RAM")){
            if(s_kmx){ if(!KMemX::Enable()) s_kmx = false; }
            else KMemX::Disable();
        }
        if(s_kmx){
            IntToStr(s_pool, buf, 64); StrApp(buf, "% of RAM", 64);
            if(Slider("Pool size", &s_pool, 10, 40, 1, buf) && Committed()){
                KMemX::SetPoolPct(s_pool);
                UIConfig::SetInt("kmemx.pool_pct", s_pool, true);
                UIConfig::Save();
                KMemX::WriteConfFile();
            }
            const char* ag = s_aggr < 4 ? "Gentle" : (s_aggr < 9 ? "Balanced" : "Aggressive");
            if(Slider("Aggressiveness", &s_aggr, 0, 12, 1, ag) && Committed()){
                KMemX::SetThreshold(16 - s_aggr);
                UIConfig::SetInt("kmemx.threshold", 16 - s_aggr, true);
                UIConfig::Save();
                KMemX::WriteConfFile();
            }
            const KMemX::Stats& st = KMemX::GetStats();
            int used = st.pool_bytes ? (int)((st.pool_used * 100) / st.pool_bytes) : 0;
            IntToStr((int)(st.pool_used / (1024 * 1024)), buf, 64); StrApp(buf, " MB of ", 64);
            char nb[16]; IntToStr((int)(st.pool_bytes / (1024 * 1024)), nb, 16); StrApp(buf, nb, 64); StrApp(buf, " MB", 64);
            Meter("Pool usage", buf, used);
            int r = KMemX::RatioX100();
            IntToStr(r / 100, buf, 64); StrApp(buf, ".", 64);
            IntToStr((r % 100) / 10, nb, 16); StrApp(buf, nb, 64); StrApp(buf, "x", 64);
            Info("Compression ratio", buf);
            IntToStr((int)(st.bytes_saved / (1024 * 1024)), buf, 64); StrApp(buf, " MB", 64);
            Info("Memory saved", buf);
        }
    }
    EndGroup();
}

extern const SettingsModule g_system_module = {
    "system", "System", "", system_on_show, nullptr, nullptr, nullptr, system_build
};
// end (satoru)
