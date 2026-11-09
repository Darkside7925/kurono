//  kurono os - settings module: about (satoru)
//  a hero with the kurono mark and version, the machine (processor, graphics,
//  memory, display), the software, and links to the devices and storage pages. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/display_mgr.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../kernel/pmm.h"
#include "../linux/kls.h"
#include "../linux/linux_kernel.h"

using namespace SettingsUI;

static const char* KURONO_NAME    = "Kurono OS";
static const char* KURONO_VERSION = "Version 1.0.0";

static const char* gpu_desc(){
    const GpuProbeResult& r = GpuProbe::GetResult();
    if(r.count <= 0) return "Framebuffer";
    int idx = (r.primary_idx >= 0 && r.primary_idx < r.count) ? r.primary_idx : 0;
    return r.gpus[idx].desc[0] ? r.gpus[idx].desc : "Display controller";
}

static void about_on_show(){}

static void about_build(){
    char buf[96], nb[16];

    Group(nullptr);
    Rect r = Custom(176, false);
    if(r.w){
        // the mark: an accent squircle with a soft sheen and the K (satoru)
        int s = 76, x = r.x + (r.w - s) / 2, y = r.y + 22;
        uint32_t a = Accent();
        FillRound(x, y + 3, s, s, 20, 0xFF000000u, 40);
        FillRound(x, y, s, s, 20, a);
        StrokeRound(x, y, s, s, 20, 0xFFFFFFFFu, 46);
        int kw = TextWidth(44.0f, "K", true);
        Text(x + (s - kw) / 2, y + 12, 44.0f, "K", 0xFFFFFFFFu, true);
        int tw = TextWidth(22.0f, KURONO_NAME, true);
        Text(r.x + (r.w - tw) / 2, y + s + 16, 22.0f, KURONO_NAME, T().text, true);
        int vw = TextWidth(13.5f, KURONO_VERSION);
        Text(r.x + (r.w - vw) / 2, y + s + 46, 13.5f, KURONO_VERSION, T().text2);
    }
    EndGroup();

    Group("This computer");
    {
        const char* brand = CPUDetect::GetBrandString();
        Info("Processor", (brand && brand[0]) ? brand : CPUDetect::GetVendorName());
        IntToStr(CPUDetect::GetCoreCount(), buf, 96); StrApp(buf, " cores, ", 96);
        IntToStr(CPUDetect::GetThreadCount(), nb, 16); StrApp(buf, nb, 96); StrApp(buf, " threads", 96);
        Info("Cores", buf);
        Info("Graphics", gpu_desc());
        uint64_t mb = PMM::GetTotalMemory() / (1024ull * 1024ull);
        if(mb >= 1024){
            IntToStr((int)(mb / 1024), buf, 96);
            if((mb % 1024) * 10 / 1024){ StrApp(buf, ".", 96); IntToStr((int)((mb % 1024) * 10 / 1024), nb, 16); StrApp(buf, nb, 96); }
            StrApp(buf, " GB", 96);
        } else { IntToStr((int)mb, buf, 96); StrApp(buf, " MB", 96); }
        Info("Memory", buf);
        IntToStr(Graphics::GetWidth(), buf, 96); StrApp(buf, " x ", 96);
        IntToStr(Graphics::GetHeight(), nb, 16); StrApp(buf, nb, 96);
        StrApp(buf, ", ", 96); StrApp(buf, DisplayManager::GetBackendName(), 96);
        Info("Display", buf);
    }
    EndGroup();

    Group("Software");
    {
        KLSConfig* c = KLS::GetConfig();
        Info("Computer name", (c && c->hostname[0]) ? c->hostname : "kurono");
        StrCpy(buf, "Kurono, Linux ABI ", 96); StrApp(buf, LinuxKernel::GetVersion(), 96);
        Info("Kernel", buf);
        Info("Architecture", "x86-64");
    }
    EndGroup();

    Group(nullptr);
    if(Nav("Devices", nullptr, "Hardware and drivers")) GoTo("devices");
    if(Nav("Storage", nullptr, "Memory, disks and files")) GoTo("storage");
    EndGroup();
}

extern const SettingsModule g_about_module = {
    "about", "About", "", about_on_show, nullptr, nullptr, nullptr, about_build
};
// end (satoru)
