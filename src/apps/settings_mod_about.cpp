//  kurono os - settings module: about / system (reference module #2) (satoru)
//  read-only info rows: cpu, gpu, memory, display backend + resolution, and the
//  kurono version string. no persisted state - purely informational. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/display_mgr.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../kernel/heap.h"

static const char* KURONO_VERSION = "Kurono OS 1.0.0";

// ── gpu description: primary probe entry, with a per-vendor label. (satoru)
static const char* gpu_primary_desc(){
    const GpuProbeResult& r = GpuProbe::GetResult();
    if(r.count <= 0) return "No GPU detected";
    int idx = (r.primary_idx >= 0 && r.primary_idx < r.count) ? r.primary_idx : 0;
    if(r.gpus[idx].desc[0]) return r.gpus[idx].desc;
    return "Display controller";
}
static const char* gpu_vendor_label(){
    const GpuProbeResult& r = GpuProbe::GetResult();
    if(r.count <= 0) return "framebuffer backend";
    int idx = (r.primary_idx >= 0 && r.primary_idx < r.count) ? r.primary_idx : 0;
    switch(r.gpus[idx].vendor_id){
        case GPU_VENDOR_INTEL:  return "Intel";
        case GPU_VENDOR_NVIDIA: return "NVIDIA";
        case GPU_VENDOR_AMD:    return "AMD";
        case GPU_VENDOR_VMWARE: return "VMware SVGA";
        case GPU_VENDOR_QEMU:   return "QEMU / Bochs";
        case GPU_VENDOR_VIRTIO: return "VirtIO GPU";
        case GPU_VENDOR_REDHAT: return "Red Hat QXL";
        default:                return "Generic";
    }
}

// on_show has nothing to load - the probes/detect ran at boot. (satoru)
static void about_on_show(){}

static void about_render(int x, int y, int w, int h, int scroll){
    (void)w; (void)h;
    int ly = y - scroll + 8;
    char buf[64];

    // ── identity banner ──────────────────────────────────────────────────
    Graphics::FillRoundedRect(x, ly, 40, 40, 10, SettingsUI::Accent());
    Graphics::DrawString(x + 14, ly + 13, "K", SettingsUI::COL_WHITE, 0xFF000000);
    Graphics::DrawString(x + 52, ly + 6,  KURONO_VERSION, SettingsUI::COL_WHITE, 0xFF000000);
    Graphics::DrawString(x + 52, ly + 24, "Hybrid bare-metal x86_64", SettingsUI::COL_DIM, 0xFF000000);
    ly += 56;

    // ── processor ────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Processor");
    ly += 26;
    {
        const char* brand = CPUDetect::GetBrandString();
        SettingsUI::Row(x, ly, "CPU:", (brand && brand[0]) ? brand : CPUDetect::GetVendorName());
        ly += 22;

        SettingsUI::IntToStr(CPUDetect::GetCoreCount(), buf, 64);
        SettingsUI::StrApp(buf, " cores / ", 64);
        char tb[16]; SettingsUI::IntToStr(CPUDetect::GetThreadCount(), tb, 16);
        SettingsUI::StrApp(buf, tb, 64);
        SettingsUI::StrApp(buf, " threads", 64);
        SettingsUI::Row(x, ly, "Topology:", buf);
        ly += 22;

        int mhz = CPUDetect::GetBaseMHz();
        if(mhz > 0){ SettingsUI::IntToStr(mhz, buf, 64); SettingsUI::StrApp(buf, " MHz base", 64); }
        else SettingsUI::StrCpy(buf, "Unknown", 64);
        SettingsUI::Row(x, ly, "Clock:", buf);
        ly += 30;
    }

    // ── graphics ─────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Graphics");
    ly += 26;
    SettingsUI::Row(x, ly, "GPU:", gpu_primary_desc());
    ly += 22;
    SettingsUI::Row(x, ly, "Vendor:", gpu_vendor_label());
    ly += 22;
    SettingsUI::Row(x, ly, "Backend:", DisplayManager::GetBackendName());
    ly += 22;
    {
        SettingsUI::IntToStr((int)Graphics::GetWidth(), buf, 64);
        SettingsUI::StrApp(buf, " x ", 64);
        char hb[16]; SettingsUI::IntToStr((int)Graphics::GetHeight(), hb, 16);
        SettingsUI::StrApp(buf, hb, 64);
        SettingsUI::StrApp(buf, " x ", 64);
        char bb[8]; SettingsUI::IntToStr((int)Graphics::GetBpp(), bb, 8);
        SettingsUI::StrApp(buf, bb, 64);
        SettingsUI::StrApp(buf, "bpp", 64);
        SettingsUI::Row(x, ly, "Resolution:", buf);
        ly += 30;
    }

    // ── memory ───────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Memory");
    ly += 26;
    {
        // kernel heap usage in kb. (satoru)
        size_t used = KernelHeap::GetUsed();
        SettingsUI::IntToStr((int)(used / 1024), buf, 64);
        SettingsUI::StrApp(buf, " KB heap used", 64);
        SettingsUI::Row(x, ly, "Kernel Heap:", buf);
        ly += 30;
    }

    // ── system ───────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "System");
    ly += 26;
    SettingsUI::Row(x, ly, "Kernel:", "Kurono Microkernel");
    ly += 22;
    SettingsUI::Row(x, ly, "Filesystem:", "KVFS");
    ly += 22;
    SettingsUI::Row(x, ly, "Architecture:", "x86_64");
    ly += 22;
}

// about is read-only: no interactive controls. (satoru)
static bool about_input(int mx, int my, bool click, char key, int scroll){
    (void)mx; (void)my; (void)click; (void)key; (void)scroll;
    return false;
}

static int about_content_height(){
    // banner + 4 sections; matches the row advances in about_render. (satoru)
    return 8 + 56
         + 26 + 22 + 22 + 30           // processor (satoru)
         + 26 + 22 + 22 + 22 + 30      // graphics (satoru)
         + 26 + 30                     // memory (satoru)
         + 26 + 22 + 22 + 22           // system (satoru)
         + 16;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_about_module;` resolves at link time. (satoru)
extern const SettingsModule g_about_module = {
    "about", "About", "\x69",
    about_on_show, about_render, about_input, about_content_height
};
// end (satoru)
