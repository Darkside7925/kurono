//  kurono os - settings module: devices (satoru)
//  a read-only inventory of the detected hardware, grouped by class: processor,
//  graphics, audio, network, storage, usb and input. each device is one row:
//  its name, the driver underneath, and a status on the right. detection is
//  live (cpuid, gpu probe + vendor drivers, audio server, network stack,
//  block devices, xhci) with real linux driver versions where bound. (satoru)
#include "system_settings.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/audio_server.h"
#include "../drivers/hda.h"
#include "../drivers/blockdev.h"
#include "../drivers/usb.h"
#include "../net/network.h"
#include "../linux/linux_drivers.h"

using namespace SettingsUI;

static bool dstarts(const char* s, const char* p){
    if(!s || !p) return false;
    int i = 0; while(p[i]){ if(s[i] != p[i]) return false; i++; }
    return true;
}
static const LinuxDriver* find_by_prefix(const char* const* pfx, int n){
    LinuxDriver* d = LinuxDriverFramework::GetDrivers();
    int dn = LinuxDriverFramework::GetDriverCount();
    for(int i = 0; i < dn; i++){
        if(!(d[i].bound || d[i].state == LDRV_ACTIVE || d[i].state == LDRV_BOUND)) continue;
        for(int p = 0; p < n; p++) if(dstarts(d[i].name, pfx[p])) return &d[i];
    }
    return nullptr;
}
static const LinuxDriver* find_in_category(LinuxDriverCategory cat){
    LinuxDriver* d = LinuxDriverFramework::GetDrivers();
    int dn = LinuxDriverFramework::GetDriverCount();
    for(int i = 0; i < dn; i++)
        if(d[i].category == cat && (d[i].bound || d[i].state == LDRV_ACTIVE || d[i].state == LDRV_BOUND)) return &d[i];
    return nullptr;
}

static void devices_on_show(){}

static void dev(const char* name, const char* sub, const char* status, bool ok){
    Info(name && name[0] ? name : "Unknown device", status, sub, ok ? T().success : T().text3);
}

static void devices_build(){
    char a[96], b[96];
    int seq = 0;

    Group("Processor");
    {
        const char* brand = CPUDetect::GetBrandString();
        if(!(brand && brand[0])) brand = CPUDetect::GetVendorName();
        IntToStr(CPUDetect::GetCoreCount(), a, 96); StrApp(a, " cores, ", 96);
        IntToStr(CPUDetect::GetThreadCount(), b, 96); StrApp(a, b, 96); StrApp(a, " threads", 96);
        int mhz = CPUDetect::GetBaseMHz();
        if(mhz > 0){ StrApp(a, ", ", 96); IntToStr(mhz, b, 96); StrApp(a, b, 96); StrApp(a, " MHz", 96); }
        dev(brand, a, "Active", true);
        a[0] = 0;
        if(CPUDetect::HasSSE2())   StrApp(a, "SSE2 ", 96);
        if(CPUDetect::HasAVX())    StrApp(a, "AVX ", 96);
        if(CPUDetect::HasAVX2())   StrApp(a, "AVX2 ", 96);
        if(CPUDetect::HasAVX512()) StrApp(a, "AVX-512 ", 96);
        if(CPUDetect::HasAES())    StrApp(a, "AES ", 96);
        if(CPUDetect::HasRDRAND()) StrApp(a, "RDRAND ", 96);
        if(!a[0]) StrCpy(a, "Baseline x86-64", 96);
        Info("Instruction sets", nullptr, a);
    }
    EndGroup();

    Group("Graphics");
    {
        const GpuProbeResult& g = GpuProbe::GetResult();
        if(g.count <= 0) Info("Display adapter", "None found");
        for(int i = 0; i < g.count && i < GPU_PROBE_MAX; i++){
            const GpuInfo& gi = g.gpus[i];
            const char* drv = "Framebuffer driver";
            b[0] = 0;
            if(gi.vendor_id == GPU_VENDOR_NVIDIA && NvidiaGPU::IsDetected()){
                drv = "Kurono NVIDIA driver"; StrCpy(b, NvidiaGPU::GetArchName(), 96);
            } else if(gi.vendor_id == GPU_VENDOR_AMD && AmdGPU::IsAvailable()){
                drv = "Kurono AMD driver"; StrCpy(b, AmdGPU::GetArchName(), 96);
            } else if(gi.vendor_id == GPU_VENDOR_INTEL && IntelGPU::IsDetected()){
                drv = "Kurono Intel driver"; StrCpy(b, IntelGPU::GetGenName(), 96);
            }
            StrCpy(a, drv, 96);
            StrApp(a, ", ", 96);
            StrApp(a, b[0] ? b : (gi.is_igpu ? "integrated" : "discrete"), 96);
            PushId(seq++);
            dev(gi.desc[0] ? gi.desc : "Display controller", a, "Ready", true);
            PopId();
        }
    }
    EndGroup();

    Group("Audio");
    {
        AudioServer::ServerStatus st = AudioServer::GetStatus();
        const char* be = (st.backend_name && st.backend_name[0]) ? st.backend_name : "none";
        StrCpy(a, "Driver: ", 96); StrApp(a, be, 96);
        dev("Audio output", a, st.backend_ready ? "Running" : "Idle", st.backend_ready);
        if(HDAudio::IsDetected()){
            IntToStr(HDAudio::GetCodecCount(), b, 96);
            StrCpy(a, b, 96); StrApp(a, HDAudio::GetCodecCount() == 1 ? " codec" : " codecs", 96);
            dev("Intel HD Audio controller", a, "Bound", true);
        }
    }
    EndGroup();

    Group("Network");
    {
        static const char* pfx[] = { "e1000", "e100", "virtio_net", "virtio-net", "rtl", "r8169", "8139",
                                     "igb", "ixgbe", "tg3", "iwl", "ath", "rtw", "brcm", "mt76" };
        const LinuxDriver* nd = find_by_prefix(pfx, 15);
        if(!nd) nd = find_in_category(LDRV_CAT_NET);
        int ifc = Network::GetInterfaceCount();
        NetworkInterface* ifs = Network::GetInterfaces();
        bool any = false;
        for(int i = 0; ifs && i < ifc && i < NET_MAX_INTERFACES; i++){
            NetworkInterface& ni = ifs[i];
            if(ni.type == NIC_LOOPBACK) continue;
            // the stack keeps a wlan placeholder even without a radio; list it only with real hardware (satoru)
            if(ni.type == NIC_WIFI && WiFi::DetectedLink() != WiFi::LINK_WIFI) continue;
            any = true;
            StrCpy(a, ni.type == NIC_WIFI ? "Wireless adapter (" : "Ethernet adapter (", 96);
            StrApp(a, ni.name[0] ? ni.name : "net", 96); StrApp(a, ")", 96);
            if(ni.type == NIC_WIFI){
                const char* chip = WiFi::WirelessChipName();
                StrCpy(b, (chip && chip[0]) ? chip : "Wireless network adapter", 96);
            } else {
                StrCpy(b, nd ? (nd->description[0] ? nd->description : nd->name) : "Kurono e1000 driver", 96);
                if(nd && nd->version[0]){ StrApp(b, " ", 96); StrApp(b, nd->version, 96); }
            }
            PushId(100 + i);
            dev(a, b, ni.state == NIC_UP ? "Up" : "Down", ni.state == NIC_UP);
            PopId();
        }
        if(!any) Info("Network adapter", "None found");
    }
    EndGroup();

    Group("Storage");
    {
        int n = BlockDev::Count(), shown = 0;
        for(int i = 0; i < n; i++){
            BlockDevice* d = BlockDev::Get(i);
            if(!d || !d->present) continue;
            StrCpy(a, d->name, 96); StrApp(a, ", ", 96);
            StrApp(a, d->driver[0] == 'n' ? "NVMe" : (d->driver[0] == 'a' ? "SATA (AHCI)" : d->driver), 96);
            PushId(200 + i);
            dev(d->model[0] ? d->model : d->name, a, "Online", true);
            PopId();
            shown++;
        }
        if(!shown) Info("Storage controller", "None found");
    }
    EndGroup();

    Group("USB and input");
    {
        if(USB::IsDetected()){
            IntToStr(USB::GetPortCount(), b, 96);
            StrCpy(a, b, 96); StrApp(a, " ports", 96);
            dev("USB host controller (xHCI)", a, "Active", true);
            int dc = USB::GetDeviceCount();
            for(int i = 0; i < dc; i++){
                const USBDeviceInfo* d = USB::GetDevice(i);
                if(!d || !d->connected) continue;
                StrCpy(a, d->manufacturer[0] ? d->manufacturer : "", 96);
                if(a[0]) StrApp(a, " ", 96);
                StrApp(a, d->product[0] ? d->product : "USB device", 96);
                StrCpy(b, USB::SpeedName(d->speed), 96); StrApp(b, ", port ", 96);
                char pn[8]; IntToStr(d->port, pn, 8); StrApp(b, pn, 96);
                PushId(300 + i);
                dev(a, b, "Connected", true);
                PopId();
            }
        }
        const LinuxDriver* id = find_in_category(LDRV_CAT_INPUT);
        dev("Keyboard", id ? (id->description[0] ? id->description : id->name) : "PS/2 keyboard (i8042)", "Ready", true);
        dev("Pointer", "PS/2 mouse (i8042)", "Ready", true);
    }
    EndGroup();
}

extern const SettingsModule g_devices_module = {
    "devices", "Devices", "", devices_on_show, nullptr, nullptr, nullptr, devices_build
};
// end (satoru)
