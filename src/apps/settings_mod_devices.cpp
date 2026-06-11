//  kurono os  -  settings module: device manager (satoru)
//  a read-only, device-manager-style inventory of all detected hardware,
//  grouped by class. each device renders as a two-line block: a name line with
//  a colour-coded status word on the right, and a dim "driver  vX.Y.Z" line
//  underneath. detection is live (cpuid, the gpu probe + bound vendor drivers,
//  the audio server, the network stack, nvme, xhci usb and the ps/2 input
//  drivers); the linux driver framework supplies real MODULE_VERSION strings
//  where a device is bound to a native driver. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/audio_server.h"
#include "../drivers/hda.h"
#include "../drivers/nvme.h"
#include "../drivers/usb.h"
#include "../net/network.h"
#include "../linux/linux_drivers.h"

// kernel/driver build version shown when a device exposes no real version of
// its own  -  kept consistent across the whole page. (satoru)
static const char* KDRV_VERSION = "kurono 1.0.0";

// ── tiny libc-free string helpers (the kernel has no libc) (satoru) ──────────
static int dlen(const char* s){ int n=0; if(s) while(s[n]) n++; return n; }
static void dcpy(char* d, const char* s, int mx){
    int i=0; if(s) while(s[i] && i<mx-1){ d[i]=s[i]; i++; } d[i]=0;
}
static void dapp(char* d, const char* s, int mx){
    int n=dlen(d), i=0; if(s) while(s[i] && n<mx-1){ d[n++]=s[i++]; } d[n]=0;
}
static void dappint(char* d, int v, int mx){ char t[16]; SettingsUI::IntToStr(v,t,16); dapp(d,t,mx); }
static bool dstarts(const char* s, const char* p){
    if(!s||!p) return false;
    int i=0;
    while(p[i]){ if(s[i]!=p[i]) return false; i++; }
    return true;
}

// ── bound linux driver lookups (same descriptor table the old settings.cpp
//    walked via find_wifi_driver)  -  gives a real name/description/version for a
//    device that is bound to a native driver. (satoru) ────────────────────────
static const LinuxDriver* find_driver_by_prefixes(const char* const* prefixes, int count){
    LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
    int dn = LinuxDriverFramework::GetDriverCount();
    for(int i=0;i<dn;i++){
        if(!(drivers[i].bound || drivers[i].state == LDRV_ACTIVE || drivers[i].state == LDRV_BOUND))
            continue;
        for(int p=0;p<count;p++)
            if(dstarts(drivers[i].name, prefixes[p])) return &drivers[i];
    }
    return nullptr;
}
// first bound driver in a category, regardless of name. (satoru)
static const LinuxDriver* find_driver_in_category(LinuxDriverCategory cat){
    LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
    int dn = LinuxDriverFramework::GetDriverCount();
    for(int i=0;i<dn;i++){
        if(drivers[i].category != cat) continue;
        if(drivers[i].bound || drivers[i].state == LDRV_ACTIVE || drivers[i].state == LDRV_BOUND)
            return &drivers[i];
    }
    return nullptr;
}
static const LinuxDriver* find_net_driver(){
    static const char* pfx[] = {
        "e1000", "e100", "virtio_net", "virtio-net", "rtl", "r8169", "8139",
        "igb", "ixgbe", "tg3", "iwl", "ath", "rtw", "brcm", "mt76"
    };
    return find_driver_by_prefixes(pfx, (int)(sizeof(pfx)/sizeof(pfx[0])));
}

// the linux version string if present, else the shared kernel build tag. (satoru)
static const char* drv_version(const LinuxDriver* d){
    if(d && d->version[0]) return d->version;
    return KDRV_VERSION;
}

// ── cached counts so content_height + render agree without re-walking pci every
//    frame; refreshed in on_show. (satoru) ─────────────────────────────────────
static int   s_gpu_count   = 0;   // display adapters reported by the probe (satoru)
static int   s_net_count   = 0;   // network interfaces (satoru)
static int   s_usb_count   = 0;   // connected usb devices (satoru)
static bool  s_nvme        = false;
static bool  s_hda         = false;

// ── layout metrics: name line + driver line per device, with section gaps so
//    the measure pass and the draw pass advance identically. (satoru) ──────────
static const int PAD_TOP      = 8;    // top padding inside the pane (satoru)
static const int HDR_ADV      = 26;   // a SectionHeader row (satoru)
static const int DEV_NAME_ADV = 18;   // device name line (satoru)
static const int DEV_DRV_ADV  = 20;   // dim driver/version line + a little gap (satoru)
static const int SECTION_GAP  = 12;   // gap after a class before the next header (satoru)
static const int EMPTY_ADV    = 22;   // "none detected" placeholder line (satoru)
static const int STATUS_X_OFF = 0;    // status word is right-aligned; see draw (satoru)

// status colours pulled from the shared palette. (satoru)
static unsigned int col_status(bool ok){ return ok ? SettingsUI::COL_ON : SettingsUI::COL_DIM; }

// ── a single device block. when draw==false we only advance y so the height
//    calc and the painter never drift. returns the y AFTER the block. (satoru)
static int dev_block(int x, int y, int w, bool draw,
                     const char* name, const char* driver, const char* version,
                     const char* status, bool status_ok){
    if(draw){
        char nm[80]; dcpy(nm, (name && name[0]) ? name : "Unknown device", 80);
        Graphics::DrawString(x + 4, y, nm, SettingsUI::COL_TEXT, 0xFF000000);
        // right-aligned status word (each glyph is ~8px in the 8x16 font). (satoru)
        if(status && status[0]){
            int sw = dlen(status) * 8;
            int sx = x + w - sw - 4 + STATUS_X_OFF;
            if(sx < x + 4) sx = x + 4;
            Graphics::DrawString(sx, y, status, col_status(status_ok), 0xFF000000);
        }
    }
    y += DEV_NAME_ADV;
    if(draw){
        // "driver  ·  vVERSION" on the dim sub-line. (satoru)
        char sub[112];
        dcpy(sub, (driver && driver[0]) ? driver : "generic kernel driver", 112);
        dapp(sub, "   v", 112);
        dapp(sub, (version && version[0]) ? version : KDRV_VERSION, 112);
        Graphics::DrawString(x + 16, y, sub, SettingsUI::COL_DIM, 0xFF000000);
    }
    y += DEV_DRV_ADV;
    return y;
}

// "(none detected)" filler for an empty class. (satoru)
static int dev_empty(int x, int y, bool draw, const char* msg){
    if(draw) Graphics::DrawString(x + 16, y, msg, SettingsUI::COL_DIM, 0xFF000000);
    return y + EMPTY_ADV;
}

// ── on_show: (re)detect everything so the inventory + the scrollbar are fresh.
//    the heavy pci scans already ran at boot; here we just sample their cached
//    results and (cheaply) re-enumerate usb. (satoru) ──────────────────────────
static void devices_on_show(){
    const GpuProbeResult& gpr = GpuProbe::GetResult();
    s_gpu_count = gpr.count > 0 ? gpr.count : 0;

    s_net_count = Network::GetInterfaceCount();
    if(s_net_count < 0) s_net_count = 0;

    s_nvme = NVMe::IsDetected();
    s_hda  = HDAudio::IsDetected();

    s_usb_count = 0;
    if(USB::IsDetected()){
        int dc = USB::GetDeviceCount();
        for(int i=0;i<dc;i++){
            const USBDeviceInfo* d = USB::GetDevice(i);
            if(d && d->connected) s_usb_count++;
        }
    }
}

// ── the one layout routine shared by render (draw=true) and content_height
//    (draw=false). it lays every section top-to-bottom from `top` and returns
//    the absolute y reached, so the caller derives either the painted frame or
//    the total pixel height. (satoru) ──────────────────────────────────────────
static int devices_layout(int x, int top, int w, bool draw){
    int ly = top;
    char buf[96];

    // ── processor ────────────────────────────────────────────────────────────
    if(draw) SettingsUI::SectionHeader(x, ly, "Processor");
    ly += HDR_ADV;
    {
        const char* brand = CPUDetect::GetBrandString();
        if(!(brand && brand[0])) brand = CPUDetect::GetVendorName();

        // detail line: vendor + cores/threads + base clock. (satoru)
        char det[96]; dcpy(det, CPUDetect::GetVendorName(), 96);
        dapp(det, "  ", 96);
        dappint(det, CPUDetect::GetCoreCount(), 96);
        dapp(det, "C/", 96);
        dappint(det, CPUDetect::GetThreadCount(), 96);
        dapp(det, "T", 96);
        int mhz = CPUDetect::GetBaseMHz();
        if(mhz > 0){ dapp(det, "  ", 96); dappint(det, mhz, 96); dapp(det, " MHz", 96); }
        ly = dev_block(x, ly, w, draw, brand, det, KDRV_VERSION, "Active", true);

        // an instruction-set feature summary as a second informational device. (satoru)
        char feat[96]; feat[0]=0;
        if(CPUDetect::HasSSE2())  dapp(feat, "SSE2 ", 96);
        if(CPUDetect::HasAVX())   dapp(feat, "AVX ", 96);
        if(CPUDetect::HasAVX2())  dapp(feat, "AVX2 ", 96);
        if(CPUDetect::HasAVX512())dapp(feat, "AVX-512 ", 96);
        if(CPUDetect::HasAES())   dapp(feat, "AES ", 96);
        if(CPUDetect::HasRDRAND())dapp(feat, "RDRAND ", 96);
        if(!feat[0]) dcpy(feat, "baseline x86-64", 96);
        ly = dev_block(x, ly, w, draw, "Instruction Set Extensions", feat,
                       CPUDetect::HasLongMode() ? "x86-64 long mode" : "32-bit",
                       "Enabled", true);
    }
    ly += SECTION_GAP;

    // ── display adapters ───────────────────────────────────────────────────────
    if(draw) SettingsUI::SectionHeader(x, ly, "Display Adapters");
    ly += HDR_ADV;
    {
        const GpuProbeResult& gpr = GpuProbe::GetResult();
        if(gpr.count <= 0){
            ly = dev_empty(x, ly, draw, "No display controller detected");
        } else {
            for(int i=0;i<gpr.count && i<GPU_PROBE_MAX;i++){
                const GpuInfo& g = gpr.gpus[i];
                const char* name = g.desc[0] ? g.desc : "Display controller";

                // driver name (prefer the bound vendor driver) + an arch/vram tag
                // folded together so the dim sub-line reads "driver  arch  N MB". (satoru)
                const char* drv = "framebuffer (vesa/bga)";
                char extra[88]; extra[0]=0;

                if(g.vendor_id == GPU_VENDOR_NVIDIA && NvidiaGPU::IsDetected()){
                    const NvidiaGPUInfo& nv = NvidiaGPU::GetInfo();
                    drv = "kurono nvidia";
                    dcpy(extra, NvidiaGPU::GetArchName(), 88);
                    if(nv.vram_mb){ dapp(extra, "  ", 88); dappint(extra, (int)nv.vram_mb, 88); dapp(extra, " MB", 88); }
                } else if(g.vendor_id == GPU_VENDOR_AMD && AmdGPU::IsAvailable()){
                    const AmdGPUInfo& am = AmdGPU::GetInfo();
                    drv = "kurono amdgpu";
                    dcpy(extra, AmdGPU::GetArchName(), 88);
                    uint64_t vmb = am.vram_size / (1024u*1024u);
                    if(vmb){ dapp(extra, "  ", 88); dappint(extra, (int)vmb, 88); dapp(extra, " MB", 88); }
                } else if(g.vendor_id == GPU_VENDOR_INTEL && IntelGPU::IsDetected()){
                    const IntelGPUInfo& ig = IntelGPU::GetInfo();
                    drv = "kurono i915";
                    dcpy(extra, IntelGPU::GetGenName(), 88);
                    if(ig.stolen_mem_mb){ dapp(extra, "  ", 88); dappint(extra, (int)ig.stolen_mem_mb, 88); dapp(extra, " MB stolen", 88); }
                }
                if(!extra[0]) dcpy(extra, g.is_igpu ? "integrated" : "discrete", 88);

                // build "driver  ·  arch/vram" then hand it to dev_block as the
                // driver string; version stays the shared kernel tag. (satoru)
                dcpy(buf, drv, 96);
                dapp(buf, "  ", 96);
                dapp(buf, extra, 96);
                ly = dev_block(x, ly, w, draw, name, buf, KDRV_VERSION, "Ready", true);
            }
        }
    }
    ly += SECTION_GAP;

    // ── audio devices ──────────────────────────────────────────────────────────
    if(draw) SettingsUI::SectionHeader(x, ly, "Audio Devices");
    ly += HDR_ADV;
    {
        AudioServer::ServerStatus st = AudioServer::GetStatus();
        const char* be = st.backend_name ? st.backend_name : AudioServer::ActiveBackendName();
        if(!(be && be[0])) be = "none";

        char nm[80]; dcpy(nm, "Audio Output (", 80); dapp(nm, be, 80); dapp(nm, ")", 80);
        char det[80]; dcpy(det, "mixer ", 80);
        dappint(det, st.backend_rate ? (int)st.backend_rate : 48000, 80);
        dapp(det, " Hz", 80);
        ly = dev_block(x, ly, w, draw, nm, det, KDRV_VERSION,
                       st.backend_ready ? "Running" : "Idle", st.backend_ready);

        if(HDAudio::IsDetected()){
            char hd[80]; dcpy(hd, "Intel HD Audio Codec", 80);
            char cid[80]; dcpy(cid, "codec vendor 0x", 80);
            uint32_t cv = HDAudio::GetCodecVendor(0);
            // hex of the 32-bit codec vendor/device id. (satoru)
            static const char* HEX = "0123456789ABCDEF";
            char hx[9]; for(int k=0;k<8;k++) hx[7-k] = HEX[(cv >> (k*4)) & 0xF];
            hx[8]=0; dapp(cid, hx, 80);
            dapp(cid, "  codecs:", 80); dappint(cid, HDAudio::GetCodecCount(), 80);
            ly = dev_block(x, ly, w, draw, hd, cid, KDRV_VERSION, "Bound", true);
        }
    }
    ly += SECTION_GAP;

    // ── network adapters ───────────────────────────────────────────────────────
    if(draw) SettingsUI::SectionHeader(x, ly, "Network Adapters");
    ly += HDR_ADV;
    {
        const LinuxDriver* ndrv = find_net_driver();
        if(!ndrv) ndrv = find_driver_in_category(LDRV_CAT_NET);
        int ifc = Network::GetInterfaceCount();
        NetworkInterface* ifs = Network::GetInterfaces();
        bool any = false;
        for(int i=0; ifs && i<ifc && i<NET_MAX_INTERFACES; i++){
            NetworkInterface& ni = ifs[i];
            if(ni.type == NIC_LOOPBACK) continue;   // listed separately below (satoru)
            any = true;

            // name line: interface name + a friendly type. (satoru)
            char nm[64]; dcpy(nm, ni.name[0] ? ni.name : "net", 64);
            dapp(nm, "  (", 64);
            switch(ni.type){
                case NIC_ETHERNET: dapp(nm, "Ethernet", 64); break;
                case NIC_WIFI:     dapp(nm, "Wireless", 64); break;
                case NIC_TUN:      dapp(nm, "TUN", 64); break;
                case NIC_TAP:      dapp(nm, "TAP", 64); break;
                case NIC_BRIDGE:   dapp(nm, "Bridge", 64); break;
                case NIC_VLAN:     dapp(nm, "VLAN", 64); break;
                default:           dapp(nm, "Adapter", 64); break;
            }
            dapp(nm, ")", 64);

            // sub-line: driver description + MAC. (satoru)
            char det[112];
            const char* dname = ndrv ? (ndrv->description[0] ? ndrv->description : ndrv->name)
                                     : "kurono e1000 (Intel 82540EM)";
            dcpy(det, dname, 112);
            dapp(det, "  ", 112);
            static const char* HEX = "0123456789ABCDEF";
            char mac[20]; int mp=0;
            for(int b=0;b<6;b++){
                mac[mp++]=HEX[(ni.mac.bytes[b]>>4)&0xF];
                mac[mp++]=HEX[ni.mac.bytes[b]&0xF];
                if(b<5) mac[mp++]=':';
            }
            mac[mp]=0; dapp(det, mac, 112);

            bool up = (ni.state == NIC_UP);
            ly = dev_block(x, ly, w, draw, nm, det, drv_version(ndrv),
                           up ? "Up" : "Down", up);
        }
        if(!any) ly = dev_empty(x, ly, draw, "No network adapter detected");
    }
    ly += SECTION_GAP;

    // ── storage controllers ────────────────────────────────────────────────────
    if(draw) SettingsUI::SectionHeader(x, ly, "Storage Controllers");
    ly += HDR_ADV;
    {
        bool any = false;
        if(NVMe::IsDetected()){
            any = true;
            const NVMeControllerInfo& nv = NVMe::GetInfo();
            char nm[64]; dcpy(nm, "NVMe SSD", 64);
            if(nv.model[0]){ dapp(nm, "  ", 64); dapp(nm, nv.model, 64); }

            // capacity in GB from lba count * lba size. (satoru)
            char det[96]; det[0]=0;
            uint64_t bytes = nv.total_capacity_lba *
                             (uint64_t)(nv.lba_size ? nv.lba_size : 512);
            uint64_t gb = bytes / (1024ull*1024ull*1024ull);
            if(gb){ dappint(det, (int)gb, 96); dapp(det, " GB", 96); }
            else  { dcpy(det, "capacity unknown", 96); }
            if(nv.firmware[0]){ dapp(det, "  fw ", 96); dapp(det, nv.firmware, 96); }
            // firmware revision doubles as the device's real version string. (satoru)
            ly = dev_block(x, ly, w, draw, nm, det,
                           nv.firmware[0] ? nv.firmware : KDRV_VERSION, "Online", true);
        }
        const LinuxDriver* bdrv = find_driver_in_category(LDRV_CAT_BLOCK);
        if(bdrv){
            any = true;
            ly = dev_block(x, ly, w, draw,
                           bdrv->description[0] ? bdrv->description : bdrv->name,
                           bdrv->name, drv_version(bdrv), "Bound", true);
        }
        if(!any) ly = dev_empty(x, ly, draw, "No NVMe / block controller detected");
    }
    ly += SECTION_GAP;

    // ── usb & input devices ─────────────────────────────────────────────────────
    if(draw) SettingsUI::SectionHeader(x, ly, "USB & Input Devices");
    ly += HDR_ADV;
    {
        // host controller. (satoru)
        if(USB::IsDetected()){
            char det[64]; dcpy(det, "xHCI host  ports:", 64);
            dappint(det, USB::GetPortCount(), 64);
            ly = dev_block(x, ly, w, draw, "USB Host Controller (xHCI)", det,
                           KDRV_VERSION, "Active", true);

            int dc = USB::GetDeviceCount();
            for(int i=0;i<dc;i++){
                const USBDeviceInfo* d = USB::GetDevice(i);
                if(!d || !d->connected) continue;
                char nm[80]; nm[0]=0;
                if(d->product[0]){ dcpy(nm, d->product, 80); }
                else dcpy(nm, "USB Device", 80);
                if(d->manufacturer[0]){ char tmp[80]; dcpy(tmp, d->manufacturer, 80);
                    dapp(tmp, " ", 80); dapp(tmp, nm, 80); dcpy(nm, tmp, 80); }
                char det[80]; dcpy(det, "speed ", 80);
                dapp(det, USB::SpeedName(d->speed), 80);
                dapp(det, "  port ", 80); dappint(det, d->port, 80);
                ly = dev_block(x, ly, w, draw, nm, det, KDRV_VERSION, "Connected", true);
            }
        } else {
            ly = dev_empty(x, ly, draw, "No USB host controller detected");
        }

        // ps/2 keyboard + mouse are brought up unconditionally at boot; surface
        // their bound linux input-driver descriptor where present. (satoru)
        const LinuxDriver* idrv = find_driver_in_category(LDRV_CAT_INPUT);
        ly = dev_block(x, ly, w, draw, "PS/2 Keyboard",
                       idrv ? (idrv->description[0] ? idrv->description : idrv->name)
                            : "i8042 keyboard",
                       drv_version(idrv), "Ready", true);
        ly = dev_block(x, ly, w, draw, "PS/2 Mouse",
                       "i8042 / ImPS-2 pointer",
                       drv_version(idrv), "Ready", true);
    }
    ly += SECTION_GAP;

    return ly;
}

// ── render: paint the inventory from the running y = top - scroll + pad. (satoru)
static void devices_render(int x, int y, int w, int h, int scroll){
    (void)h;
    devices_layout(x, y - scroll + PAD_TOP, w, /*draw=*/true);
}

// device manager is read-only  -  there are no interactive controls. (satoru)
static bool devices_input(int mx, int my, bool click, char key, int scroll){
    (void)mx; (void)my; (void)click; (void)key; (void)scroll;
    return false;
}

// total content height: run the SAME layout with draw=false from a zero origin
// (plus the top pad) so the scrollbar matches the painted list exactly, then
// add a small tail. (satoru)
static int devices_content_height(){
    int end = devices_layout(0, PAD_TOP, 600, /*draw=*/false);
    return end + 16;
}

// `extern` forces EXTERNAL linkage so the shell's
// `extern const SettingsModule g_devices_module;` resolves at link time;
// a namespace-scope const would otherwise have internal linkage. (satoru)
extern const SettingsModule g_devices_module = {
    "devices", "Device Manager", "\x12",
    devices_on_show, devices_render, devices_input, devices_content_height
};
// end (satoru)
