//  kurono os - settings module: power (satoru)
//  power mode mapped onto the cpufreq governors (applied live on every managed
//  cpu, persisted, restored at boot), live processor clocks, and the power
//  source. (satoru)
#include "system_settings.h"
#include "../system/ui_config.h"
#include "../hal/cpufreq.h"

using namespace SettingsUI;

// segment order left to right; plan values match power.plan (0 perf, 1 bal, 2 saver). (satoru)
static const char* kModes[] = { "Power Saver", "Balanced", "Performance" };
static const int   kModePlan[] = { 2, 1, 0 };
static const char* kModeDesc[] = {
    "Lowest clocks, quietest and coolest",
    "Clocks follow the load (recommended)",
    "Highest clocks all the time",
};
static int s_mode = 1;

static CPUFreq::Governor plan_gov(int plan){
    return plan == 0 ? CPUFreq::GOV_PERFORMANCE : (plan == 2 ? CPUFreq::GOV_POWERSAVE : CPUFreq::GOV_ONDEMAND);
}

static void power_on_show(){
    int n = CPUFreq::CPUCount();
    int plan = 1;
    if(n > 0){
        CPUFreq::Governor g = CPUFreq::GetGovernor(0);
        plan = g == CPUFreq::GOV_PERFORMANCE ? 0 : (g == CPUFreq::GOV_POWERSAVE ? 2 : 1);
    }
    plan = UIConfig::Int("power.plan", plan);
    s_mode = 1;
    for(int i = 0; i < 3; i++) if(kModePlan[i] == plan) s_mode = i;
}

static void power_build(){
    int n = CPUFreq::CPUCount();
    char buf[64], nb[16];

    Group("Power mode");
    if(n > 0){
        if(Segmented("Mode", kModes, 3, &s_mode, kModeDesc[s_mode])){
            int plan = kModePlan[s_mode];
            for(int i = 0; i < n; i++) CPUFreq::SetGovernor((uint32_t)i, plan_gov(plan));
            UIConfig::SetInt("power.plan", plan, true);
            UIConfig::Save();
        }
    } else {
        Info("Mode", "Not available", "This processor has no frequency scaling");
    }
    EndGroup();

    const CPUFreq::CPUInfo* ci = n > 0 ? CPUFreq::GetCPU(0) : nullptr;
    if(ci){
        Group("Processor");
        IntToStr((int)ci->cur_mhz, buf, 64); StrApp(buf, " MHz", 64);
        Info("Current speed", buf);
        IntToStr((int)ci->base_mhz, buf, 64); StrApp(buf, " MHz", 64);
        if(ci->turbo_mhz > ci->base_mhz){ StrApp(buf, ", up to ", 64); IntToStr((int)ci->turbo_mhz, nb, 16); StrApp(buf, nb, 64); StrApp(buf, " MHz", 64); }
        Info("Base speed", buf);
        IntToStr((int)ci->recent_load_pct, buf, 64); StrApp(buf, "%", 64);
        Info("Load", buf);
        IntToStr((int)ci->pstate_count, buf, 64); StrApp(buf, " states on ", 64);
        IntToStr(n, nb, 16); StrApp(buf, nb, 64); StrApp(buf, n == 1 ? " CPU" : " CPUs", 64);
        Info("Performance states", buf);
        EndGroup();
    }

    Group("Battery");
    Info("Power source", "AC power", "No battery was detected");
    EndGroup();
}

extern const SettingsModule g_power_module = {
    "power", "Power", "", power_on_show, nullptr, nullptr, nullptr, power_build
};
// end (satoru)
