//  kurono os - background vm host (see vm_host.h)
#include "vm_host.h"
#include "pci_passthrough.h"   // (satoru)
#include "hypervisor.h"
#include "vdevices.h"
#include "vlapic.h"
#include "vcon.h"
#include "vblk.h"
#include "vballoon.h"
#include "vnet.h"
#include "wlbridge.h"
#include "vaudio.h"
#include "alpine_data.h"
#include "../proc/scheduler.h"
#include "../linux/linux_syscall.h"   // kls around the host loop (satoru)
#include "../proc/spinlock.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/disklog.h"
#include "../drivers/graphics.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "guest_mem.h"
#include "../system/ui_config.h"
#include "mb2_boot.h"            // kurono guests (satoru)
#define KGUEST_LOG "/home/user/vmshare/kguest.log"   // the kurono guest's com1, live (satoru)
#include "../fs/kvfs.h"
#include "../system/png_encode.h"

extern void (*g_hv_foreign_destroy)();

namespace {
    volatile int g_state = VMHost::VMH_OFF;
    volatile int g_req_start = 0, g_req_stop = 0, g_req_kill = 0;
    volatile int g_req_distro = VMHost::DISTRO_ALPINE;
    volatile uint32_t g_req_ram = 0;
    int      g_distro = VMHost::DISTRO_ALPINE;
    uint32_t g_ram_default = 1024;
    Process* g_proc = nullptr;
    uint64_t g_start_ms = 0, g_stop_ms = 0;
    uint64_t g_slices = 0, g_idle_slices = 0, g_run_us = 0;
    uint32_t g_gen = 0;
    uint32_t g_reboots = 0;
    uint64_t g_reboot_ms[4] = { 0, 0, 0, 0 };    // the last guest reboots, for loop detection
    bool     g_mirror = false;
    bool     g_debug = false;                      // kurono.vmdebug=1: kernel console on com1 too
    uint64_t g_hb_ms = 0;
    uint32_t g_mirror_budget = 0;                 // bytes mirrored per boot without vmlog=1
    const char* g_err = "";
    char     g_errbuf[96];
    // settings > linux hooks (satoru)
    uint32_t g_cur_ram = 0;                       // ram of the running (or last) vm (satoru)
    bool     g_ram_cmdline = false;               // kurono.vmram=N was given (satoru)
    volatile bool g_loop_stopped = false;         // the reboot loop guard fired (satoru)
    volatile int  g_restart = 0;                  // start again once the stop completes (satoru)
    volatile uint64_t g_start_at = 0;             // delayed start (autostart) (satoru)
    char     g_kg_src[128];                       // StartKurono request (satoru)
    char     g_kg_cmd[256];
    bool     g_fixed = false;                     // kurono.vm.fixed=1: eager guest ram, one vcpu (satoru)
    uint64_t g_mem_log_ms = 0;
    int      g_disk_cmdline = -1;                 // kurono.vmdisk=N, -1 = not given (satoru)
    bool     g_par = false;                       // kurono.vm.par=1 (satoru)
    bool     g_smp_explicit = false;              // kurono.vm.smp / par given on the command line (satoru)
    int      g_smp = 1;                           // vcpus asked for (satoru)

    const VMHost::DistroDesc g_distros[] = {
        { "alpine", "Alpine Linux", "Alpine 3.23, Linux 6.18",
          "ALPKERN BIN", "ALPINIT BIN", "ALPINE  IMG", "ALPINE.IMG", 1024, VMHost::DS_READY,
          "A small, fast Linux with apk packages" },
        { "debian", "Debian", "Debian 13, Linux 6.12",
          "DEBKERN BIN", "DEBINIT BIN", "DEBIAN  IMG", "DEBIAN.IMG", 768, VMHost::DS_READY,
          "Debian 13 trixie with its own kernel, apt and Wayland apps" },
    };
    const int g_distro_count = (int)(sizeof(g_distros) / sizeof(g_distros[0]));

    // hvc0 scrollback: one writer (the vm thread), readers follow by cursor (satoru)
    const uint32_t SCROLL = 256 * 1024;
    char*    g_out = nullptr;                     // allocated when a vm first starts (satoru)
    volatile uint64_t g_out_head = 0;             // total bytes ever written

    // agent (hvc1) line assembly (satoru)
    char     g_agent_line[1024]; int g_agent_ll = 0;
    char     g_agent_last[160];

    // VMHost::Exec: one command at a time through the agent (satoru)
    volatile int g_exec_busy = 0;
    volatile uint32_t g_exec_id = 0;
    volatile bool g_exec_done = false;
    volatile int g_exec_rc = -1;
    // written only by the vm thread; the caller copies it out (a timed-out
    // caller's own buffer may already be gone) (satoru)
    char     g_exec_out[61440]; const int g_exec_max = (int)sizeof(g_exec_out); volatile int g_exec_len = 0;

    uint32_t parse_u(const char* s, int* adv) {
        uint32_t v = 0; int i = 0;
        while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (uint32_t)(s[i++] - '0');
        if (adv) *adv = i;
        return v;
    }

    // "o<id> text" output lines and "done <id> <rc>" from the agent (satoru)
    bool exec_line(const char* l, int n) {
        if (n >= 2 && l[0] == 'o' && l[1] >= '0' && l[1] <= '9') {
            int a = 0;
            uint32_t id = parse_u(l + 1, &a);
            if (id != g_exec_id || !id) return true;
            const char* t = l + 1 + a;
            if (*t == ' ') t++;
            while (*t && g_exec_len < g_exec_max - 2) g_exec_out[g_exec_len++] = *t++;
            if (g_exec_len < g_exec_max - 1) g_exec_out[g_exec_len++] = '\n';
            g_exec_out[g_exec_len] = 0;
            return true;
        }
        if (n >= 6 && l[0] == 'd' && l[1] == 'o' && l[2] == 'n' && l[3] == 'e' && l[4] == ' ') {
            int a = 0;
            uint32_t id = parse_u(l + 5, &a);
            if (id == g_exec_id) {
                const char* r = l + 5 + a;
                if (*r == ' ') r++;
                bool neg = (*r == '-'); if (neg) r++;
                g_exec_rc = (int)parse_u(r, nullptr) * (neg ? -1 : 1);
                g_exec_done = true;
            }
            return true;
        }
        return false;
    }
    volatile bool g_agent_ready = false;

    // console log mirror, "[g] " prefixed lines (satoru)
    char     g_mline[200]; int g_mll = 0;

    void mirror(const uint8_t* b, int n, const char* tag) {
        for (int i = 0; i < n; i++) {
            char c = (char)b[i];
            if (c == '\r') continue;
            if (c == '\n' || g_mll >= (int)sizeof(g_mline) - 2) {
                g_mline[g_mll] = 0;
                SerialLogger::Log(tag); SerialLogger::Log(g_mline); SerialLogger::Log("\r\n");
                g_mll = 0;
                if (c != '\n') g_mline[g_mll++] = c;
            } else if ((unsigned char)c >= 32 || c == '\t') {
                g_mline[g_mll++] = c;
            }
        }
    }

    void console_sink(int idx, const uint8_t* data, int n) {
        if (idx == 0) {
            if (!g_out) return;
            uint64_t h = g_out_head;
            for (int i = 0; i < n; i++) g_out[(h + i) % SCROLL] = (char)data[i];
            __atomic_store_n(&g_out_head, h + n, __ATOMIC_RELEASE);
            Graphics::MarkUIDirty();      // an open Alpine window repaints (satoru)
            if (g_mirror) mirror(data, n, "[g] ");
            else if (g_mirror_budget) {
                int k = (uint32_t)n < g_mirror_budget ? n : (int)g_mirror_budget;
                g_mirror_budget -= k;
                mirror(data, k, "[g] ");
            }
            return;
        }
        for (int i = 0; i < n; i++) {
            char c = (char)data[i];
            if (c == '\r') continue;
            if (c == '\n' || g_agent_ll >= (int)sizeof(g_agent_line) - 1) {
                g_agent_line[g_agent_ll] = 0;
                if (exec_line(g_agent_line, g_agent_ll)) { g_agent_ll = 0; continue; }
                for (int k = 0; k <= g_agent_ll && k < (int)sizeof(g_agent_last) - 1; k++) g_agent_last[k] = g_agent_line[k];
                g_agent_last[sizeof(g_agent_last) - 1] = 0;
                if (g_agent_ll >= 5 && g_agent_line[0] == 'h' && g_agent_line[1] == 'e' &&
                    g_agent_line[2] == 'l' && g_agent_line[3] == 'l' && g_agent_line[4] == 'o') {
                    if (!g_agent_ready) {
                        SerialLogger::Log("[vmhost] guest agent up: ");
                        SerialLogger::Log(g_agent_line);
                        SerialLogger::Log("\r\n");
                    }
                    if (!g_agent_ready) {
                        uint32_t cp = 0, ct = 0;
                        VBlk::ChunkStats(&cp, &ct);
                        if (ct) {
                            // the disk is bigger than the filesystem on it: grow ext4 online once the
                            // network is up (resize2fs comes with e2fsprogs-extra on Alpine, e2fsprogs on Debian) (satoru)
                            const char* rz = "run sh -c 'command -v resize2fs >/dev/null || { i=0; while [ $i -lt 40 ]; do { apk add --no-cache e2fsprogs-extra || { apt-get update && apt-get install -y --no-install-recommends e2fsprogs; }; } >/dev/null 2>&1 && break; i=$((i+1)); sleep 3; done; }; resize2fs /dev/vda >/var/log/kurono-resize.log 2>&1'\n";
                            int n = 0; while (rz[n]) n++;
                            GuestConsole::Write(1, rz, n);
                        }
                    }
                    if (!g_agent_ready && GuestMemoryManager::IsLazy()) {
                        // dynamic mode: the guest returns idle memory through free page reporting (satoru)
                        const char* mp = "run modprobe virtio_balloon";
                        char b[48]; int n = 0;
                        while (mp[n]) { b[n] = mp[n]; n++; }
                        b[n++] = '\n';
                        GuestConsole::Write(1, b, n);
                    }
                    g_agent_ready = true;
                }
                g_agent_ll = 0;
            } else g_agent_line[g_agent_ll++] = c;
        }
    }

    // the early com1 console (earlyprintk / kurono.vmdebug) (satoru)
    void pump_com1() {
        char b[256];
        int n;
        while ((n = Hypervisor::ReadSerialOutput(b, sizeof(b))) > 0) {
            mirror((const uint8_t*)b, n, "[gs] ");
            if (g_distro == VMHost::DISTRO_KURONO && g_out) {   // a kurono guest has no hvc0: com1 is its console (satoru)
                uint64_t h = g_out_head;
                for (int i = 0; i < n; i++) g_out[(h + i) % SCROLL] = b[i];
                __atomic_store_n(&g_out_head, h + n, __ATOMIC_RELEASE);
                // and a host file an agent can tail live: the whole guest log of
                // this boot, restarted when it passes 32 MB (satoru)
                if (KVFS::GetFileSize(KGUEST_LOG) > (32 << 20)) KVFS::WriteFile(KGUEST_LOG, "[log restarted at 32 MB]\n", 25);
                KVFS::AppendFile(KGUEST_LOG, b, (uint32_t)n);
            }
        }
    }

    uint32_t cpu_id() { uint32_t a, b, c, d; asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0)); return b >> 24; }
    uint32_t g_vm_cpu = 0xFFFFFFFF;

    void set_err(const char* a, const char* b = "") {
        int p = 0;
        while (*a && p < (int)sizeof(g_errbuf) - 1) g_errbuf[p++] = *a++;
        while (*b && p < (int)sizeof(g_errbuf) - 1) g_errbuf[p++] = *b++;
        g_errbuf[p] = 0;
        g_err = g_errbuf;
    }

    bool prepare(int distro, uint32_t ram_mb) {
        Hypervisor::SetDiskVirtualGb((uint32_t)VMHost::DiskGb());
        // the debian cloud kernel has no mp table support: one vcpu. a kurono
        // guest (vm kurono / kloop) stays on one vcpu too, the only shape its
        // passthrough and kloop runs were verified with (satoru)
        int ncpu = (g_fixed || distro == VMHost::DISTRO_DEBIAN || distro == VMHost::DISTRO_KURONO) ? 1 : g_smp;
        Hypervisor::SetGuestCpus(ncpu);
        // dynamic mode: host frames on first touch; a guest with a passed-through
        // device gets all its ram up front (the device dma needs it mapped) (satoru)
        GuestMemoryManager::SetLazy(!g_fixed && !(distro == VMHost::DISTRO_KURONO && PCIPassthrough::ReservedCount()));
        if (!g_out) {
            g_out = (char*)KernelHeap::Alloc(SCROLL);
            if (!g_out) { set_err("no memory for the guest console"); return false; }
            for (uint32_t i = 0; i < SCROLL; i++) g_out[i] = 0;
            g_out_head = 0;
        }
        g_agent_ready = false;
        g_agent_ll = 0; g_agent_last[0] = 0;
        g_mll = 0;
        // 256 KB of the guest console per boot reaches the kernel log (and the
        // whole-session KURONO.LOG). 16 KB ran out during the first apk add, so a
        // later failure in the guest (OpenCode on the 11-12 laptop run) left no
        // trace. kurono.vmlog=1 still mirrors everything. (satoru)
        g_mirror_budget = 262144;
        GuestConsole::SetSink(0, console_sink);
        GuestConsole::SetSink(1, console_sink);
        GuestConsole::SetSink(2, WLBridge::OnGuestBytes);
        GuestConsole::SetSink(3, VAudio::OnGuestBytes);
        GuestConsole::SetFlow(3, VAudio::CanAccept);
        WLBridge::Reset();
        VAudio::Reset();
        GuestConsole::ReleaseAll();
        if (distro == VMHost::DISTRO_KURONO) {   // (satoru)
            KVFS::Mkdirs("/home/user/vmshare");
            KVFS::WriteFile(KGUEST_LOG, "", 0);   // a fresh log per guest boot (satoru)
            const uint8_t* img = nullptr; uint64_t isz = 0; uint8_t* owned = nullptr;
            if (g_kg_src[0] == '/') {
                KVFSNode* nd = KVFS::Resolve(g_kg_src);
                if (nd && nd->content && nd->size > 64) { img = nd->content; isz = nd->size; }
            } else {
                uint64_t fsz = 0;
                if (DiskLog::StatRootFile(g_kg_src, &fsz) == 0 && fsz > 64 && fsz < (512ull << 20)) {
                    owned = (uint8_t*)KernelHeap::Alloc(fsz);
                    if (owned && DiskLog::ReadRootFile(g_kg_src, owned, fsz, nullptr) == 0) { img = owned; isz = fsz; }
                }
            }
            if (!img) {
                if (owned) KernelHeap::Free(owned);
                set_err("kurono guest: the kernel image was not found (vfs path or KURONO root 8.3 name)");
                return false;
            }
            bool ok = Hypervisor::PrepareMultiboot2Guest(img, isz, g_kg_cmd, ram_mb);
            if (owned) KernelHeap::Free(owned);
            if (!ok) { set_err("the hypervisor could not boot the kurono image (see the serial log)"); return false; }
            g_cur_ram = ram_mb;
            return true;
        }
        if (distro == VMHost::DISTRO_DEBIAN) {
            const VMHost::DistroDesc* dd = VMHost::GetDistro(distro);
            const uint8_t* k = KAsset::Data(dd->kernel83, nullptr, nullptr);
            uint32_t ks = KAsset::Size(dd->kernel83, nullptr, nullptr);
            const uint8_t* r = KAsset::Data(dd->initrd83, nullptr, nullptr);
            uint32_t rs = KAsset::Size(dd->initrd83, nullptr, nullptr);
            if (!k || !ks || !r || !rs) {
                set_err("DEBKERN.BIN / DEBINIT.BIN not on the KURONO partition");
                return false;
            }
            int why = DiskLog::GuestImage(dd->image83, nullptr, nullptr);
            if (why != 0) {
                set_err(dd->image_name, why == 2 ? " is fragmented (recreate it on an empty partition)"
                                                 : " not found in the KURONO partition root");
                return false;
            }
            if (!Hypervisor::DebianPrepare(ram_mb, g_debug)) {
                set_err("the hypervisor could not create the vm (see the serial log)");
                return false;
            }
            g_cur_ram = ram_mb;
            return true;
        }
        const VMHost::DistroDesc* dd = VMHost::GetDistro(distro);
        Hypervisor::SetLinuxGuestProfile(LINUX_GUEST_ALPINE);
        const char* img83 = dd->image83;          // 11 chars, space padded (satoru)
        LinuxGuestSpec sp = {};
        sp.name = "alpine";
        sp.kernel = alpine_kernel_data();
        sp.kernel_size = alpine_kernel_size();
        sp.initrd = alpine_initramfs_data();
        sp.initrd_size = alpine_initramfs_size();
        if (!sp.kernel || !sp.initrd) { set_err("ALPKERN.BIN / ALPINIT.BIN not on the KURONO partition"); return false; }
        int why = DiskLog::GuestImage(img83, nullptr, nullptr);
        if (why != 0) {
            set_err(dd->image_name, why == 2 ? " is fragmented (recreate it on an empty partition)"
                                             : " not found in the KURONO partition root");
            return false;
        }
        DiskLog::ArmGuestImage(img83, &sp.disk_lba, &sp.disk_bytes);
        sp.disk_id = "kurono-alpine";
        sp.cmdline = ncpu > 1
            ? (g_debug
                ? "console=ttyS0 console=hvc0 kroot=/dev/vda loglevel=7 panic=5 nokaslr noapic acpi=off i8042.noaux i8042.nokbd"
                : "console=hvc0 kroot=/dev/vda loglevel=4 panic=5 nokaslr noapic acpi=off i8042.noaux i8042.nokbd")
            : (g_debug
                ? "console=ttyS0 console=hvc0 kroot=/dev/vda loglevel=7 panic=5 "
                  "nokaslr noapic nolapic nosmp acpi=off i8042.noaux i8042.nokbd"
                : "console=hvc0 kroot=/dev/vda loglevel=4 panic=5 nokaslr noapic nolapic nosmp acpi=off "
                  "i8042.noaux i8042.nokbd");
        sp.ram_mb = ram_mb;
        g_cur_ram = ram_mb;
        sp.net = true;
        sp.gpu = false;
        if (!Hypervisor::PrepareLinuxGuest(sp)) { set_err("the hypervisor could not create the vm (see the serial log)"); return false; }
        return true;
    }

    void teardown() {
        pump_com1();
        WLBridge::Reset();            // the guest's windows close with it (satoru)
        VAudio::Reset();              // and its mixer stream is freed (satoru)
        GuestConsole::ReleaseAll();   // four 256 kb console rings (satoru)
        Hypervisor::DestroyVM();
        g_agent_ready = false;
    }

    void stop_now(const char* why) {
        SerialLogger::Log("[vmhost] guest stopped: ");
        SerialLogger::Log(why);
        SerialLogger::Log("\r\n");
        teardown();
    }

    // sched=2: the vm host runs beside the linux programs (and, with
    // kurono.svccpu, on a cpu of its own). its device models (virtio-blk,
    // v9fs over kvfs, the nat into the net stack, the wayland bridge, the audio
    // bridge) share state with the syscall bodies, so a running slice and the
    // bridge pumps after it hold kls; the guest itself runs with it released
    // (Hypervisor::RunOneCycle). vm creation and teardown stay outside it: they
    // can take seconds, and a 3 s hold makes another cpu steal kls. old mode:
    // no-op. (satoru)
    struct HostKls {
        bool held = false;
        void enter() { if (!held && Scheduler::SchedV2()) { LinuxSyscall::LayerLock(); held = true; } }
        void leave() { if (held) { LinuxSyscall::LayerUnlock(); held = false; } }
        ~HostKls() { leave(); }
    };

    void host_entry() {
        Hypervisor::SetOwner(Scheduler::GetCurrentProcess());
        g_hv_foreign_destroy = VMHost::Kill;
        // vmx state is per cpu: remember where the vm was created and never run
        // it anywhere else (kernel processes stay on the bsp; if that ever
        // changes, the vm is torn down instead of resuming a foreign vmcs) (satoru)
        // (kurono.svccpu homes this proc on the vm cpu for good, so the guard
        // below never fires there.) (satoru)
        for (;;) {
            HostKls ks;
            if (g_start_at && Timer::GetRealMs64() >= g_start_at) {
                g_start_at = 0;
                if (VMHost::Enabled() && (g_state == VMHost::VMH_OFF || g_state == VMHost::VMH_FAILED)) g_req_start = 1;
            }
            if (g_restart && g_state == VMHost::VMH_OFF && !g_req_stop) {
                g_restart = 0;
                // the distro chosen in settings since the last start (satoru)
                if (VMHost::Enabled()) { g_req_ram = 0; g_req_distro = VMHost::SelectedDistro(); g_req_start = 1; }
            }
            if (g_req_kill) {
                g_req_kill = 0; g_req_stop = 0; g_restart = 0;
                if (g_state != VMHost::VMH_OFF) { stop_now("killed"); g_state = VMHost::VMH_OFF; }
            }
            if (g_req_start && (g_state == VMHost::VMH_OFF || g_state == VMHost::VMH_FAILED)) {
                g_req_start = 0;
                g_state = VMHost::VMH_STARTING;
                g_distro = g_req_distro;
                uint32_t ram = g_req_ram ? g_req_ram : VMHost::ConfiguredRamMb();
                SerialLogger::Log("[vmhost] starting ");
                SerialLogger::Log(VMHost::DistroName());
                SerialLogger::Log(" guest, ");
                SerialLogger::LogDec((int)ram);
                SerialLogger::Log(" MB\r\n");
                g_err = "";
                g_loop_stopped = false;
                g_vm_cpu = cpu_id();
                if (prepare(g_distro, ram)) {
                    g_start_ms = Timer::GetRealMs64();
                    g_gen++;
                    g_state = VMHost::VMH_RUNNING;
                    SerialLogger::Log("[vmhost] guest running\r\n");
                } else {
                    teardown();
                    g_state = VMHost::VMH_FAILED;
                    if (!g_err[0]) set_err("start failed");
                    SerialLogger::Log("[vmhost] guest failed to start: ");
                    SerialLogger::Log(g_err);
                    SerialLogger::Log("\r\n");
                }
            }
            if (g_req_stop) {
                g_req_stop = 0;
                if (g_state == VMHost::VMH_RUNNING) {
                    g_state = VMHost::VMH_STOPPING;
                    g_stop_ms = Timer::GetRealMs64();
                    if (g_agent_ready) VMHost::AgentSend("poweroff");
                    else g_stop_ms = 0;          // no agent: nothing will shut it down cleanly
                } else if (g_state == VMHost::VMH_FAILED) {
                    g_state = VMHost::VMH_OFF;
                }
            }
            if (g_state == VMHost::VMH_STOPPING && (g_stop_ms == 0 || Timer::GetRealMs64() - g_stop_ms > 30000)) {
                stop_now(g_stop_ms ? "poweroff timed out, forced" : "forced (no guest agent)");
                g_state = VMHost::VMH_OFF;
            }
            if ((g_state == VMHost::VMH_RUNNING || g_state == VMHost::VMH_STOPPING) && cpu_id() != g_vm_cpu) {
                SerialLogger::Log("[vmhost] host thread moved to another cpu, vm dropped\r\n");
                set_err("the vm host thread migrated cpus");
                g_state = VMHost::VMH_FAILED;
                g_agent_ready = false;
                continue;
            }
            if (g_state == VMHost::VMH_RUNNING || g_state == VMHost::VMH_STOPPING) {
                bool idle = false;
                uint64_t t = Timer::GetRealMs64();
                ks.enter();
                VMState st = Hypervisor::RunSlice(3000, &idle);
                Hypervisor::ParLock();      // parallel vcpus: the pumps touch device state the ap threads' exits use (satoru)
                pump_com1();
                WLBridge::Poll();
                VAudio::Pump();
                Hypervisor::ParUnlock();
                ks.leave();
                g_slices++;
                if (idle) g_idle_slices++;
                g_run_us += (Timer::GetRealMs64() - t) * 1000;
                if (st != VM_STATE_RUNNING && st != VM_STATE_PAUSED) {
                    if (st == VM_STATE_REBOOTING && g_state == VMHost::VMH_RUNNING) {
                        // a guest that panics reboots (panic=5); one that keeps
                        // doing it is stopped instead of looping forever (satoru)
                        uint64_t now = Timer::GetRealMs64();
                        for (int k = 3; k > 0; k--) g_reboot_ms[k] = g_reboot_ms[k - 1];
                        g_reboot_ms[0] = now;
                        g_reboots++;
                        if (g_reboot_ms[3] && now - g_reboot_ms[3] < 120000) {
                            stop_now("rebooting in a loop");
                            set_err("the guest rebooted 4 times in 2 minutes (kernel panic?), stopped");
                            g_state = VMHost::VMH_FAILED;
                            g_loop_stopped = true;
                            g_restart = 0;
                            for (auto& t : g_reboot_ms) t = 0;
                            continue;
                        }
                        stop_now("reboot");
                        g_state = VMHost::VMH_OFF;
                        g_req_start = 1;
                        continue;
                    }
                    bool clean = (st == VM_STATE_HALTED);
                    stop_now(clean ? "powered off" : "crashed");
                    if (!clean) set_err("the guest crashed (triple fault or fatal exit, see the serial log)");
                    g_state = clean ? VMHost::VMH_OFF : VMHost::VMH_FAILED;
                    continue;
                }
                if (g_debug && Timer::GetRealMs64() - g_hb_ms > 5000) {
                    g_hb_ms = Timer::GetRealMs64();
                    const VMStats& vs = Hypervisor::GetStats();
                    SerialLogger::Log("[vmhost] hb exits="); SerialLogger::LogDec((int)vs.total_exits);
                    SerialLogger::Log(" io="); SerialLogger::LogDec((int)vs.io_exits);
                    SerialLogger::Log(" mmio="); SerialLogger::LogDec((int)vs.mmio_exits);
                    SerialLogger::Log(" irq="); SerialLogger::LogDec((int)vs.irq_injections);
                    SerialLogger::Log(" slices="); SerialLogger::LogDec((int)g_slices);
                    SerialLogger::Log(" idle="); SerialLogger::LogDec((int)g_idle_slices);
                    SerialLogger::Log(" con="); SerialLogger::LogDec((int)GuestConsole::BytesOut(0));
                    SerialLogger::Log(" rip=0x"); SerialLogger::LogHex64(Hypervisor::GuestRIP());
                    {   // interrupt plumbing: the pic and the lapic as the guest left them (satoru)
                        VirtualPIC& mp = VirtualDevices::GetMasterPIC();
                        SerialLogger::Log(" pic imr="); SerialLogger::LogHex(mp.imr);
                        SerialLogger::Log(" irr="); SerialLogger::LogHex(mp.irr);
                        SerialLogger::Log(" isr="); SerialLogger::LogHex(mp.isr);
                        SerialLogger::Log(" base="); SerialLogger::LogHex(mp.vector_base);
                        SerialLogger::Log(" lapic en="); SerialLogger::LogDec(VLapic::Enabled(0) ? 1 : 0);
                        SerialLogger::Log(" lvt_t="); SerialLogger::LogHex(VLapic::Read(0, 0x320));
                        SerialLogger::Log(" t_init="); SerialLogger::LogHex(VLapic::Read(0, 0x380));
                        SerialLogger::Log(" svr="); SerialLogger::LogHex(VLapic::Read(0, 0xF0));
                        PITChannel& c0 = VirtualDevices::GetPIT().channels[0];
                        SerialLogger::Log(" pit0 rl="); SerialLogger::LogDec(c0.reload_value);
                        SerialLogger::Log(" mode="); SerialLogger::LogDec(c0.mode);
                        SerialLogger::Log(" null="); SerialLogger::LogDec(c0.null_count ? 1 : 0);
                        SerialLogger::Log(" gate="); SerialLogger::LogDec(c0.gate ? 1 : 0);
                        SerialLogger::Log(" hpetleg="); SerialLogger::LogDec(VirtualDevices::GetHPET().legacy_route ? 1 : 0);
                        SerialLogger::Log(" ticks="); SerialLogger::LogDec((int)Hypervisor::GetStats().tick_count);
                        uint64_t rf, in, ij, pc;
                        Hypervisor::GuestIrqState(&rf, &in, &ij, &pc);
                        SerialLogger::Log(" rflags="); SerialLogger::LogHex((uint32_t)rf);
                        SerialLogger::Log(" intr="); SerialLogger::LogHex((uint32_t)in);
                        SerialLogger::Log(" inj="); SerialLogger::LogHex((uint32_t)ij);
                        SerialLogger::Log(" proc="); SerialLogger::LogHex((uint32_t)pc);
                        SerialLogger::Log(" halted="); SerialLogger::LogDec(Hypervisor::IsGuestHalted() ? 1 : 0);
                    }
                    SerialLogger::Log("\r\n");
                    Hypervisor::DumpSmp();
                }
                if (Timer::GetRealMs64() - g_mem_log_ms > 30000) {
                    g_mem_log_ms = Timer::GetRealMs64();
                    SerialLogger::Log("[vmmem] guest "); SerialLogger::LogDec((int)g_cur_ram);
                    SerialLogger::Log(" MB, resident "); SerialLogger::LogDec((int)(GuestMemoryManager::ResidentBytes() >> 20));
                    SerialLogger::Log(" MB (lazy="); SerialLogger::LogDec(GuestMemoryManager::IsLazy() ? 1 : 0);
                    SerialLogger::Log(" reported "); SerialLogger::LogDec((int)(VBalloon::ReleasedBytes() >> 20)); SerialLogger::Log(" MB in "); SerialLogger::LogDec((int)VBalloon::Reports());
                    SerialLogger::Log("), host free "); SerialLogger::LogDec((int)(PMM::GetFreeMemory() >> 20));
                    SerialLogger::Log(" MB\r\n");
                }
                ks.leave();
                if (idle) Scheduler::SleepMs(1); else Scheduler::YieldNow();
            } else {
                ks.leave();
                Scheduler::SleepMs(20);
            }
        }
    }
}

static void test_entry();
static char g_testcmds[900];
void VMHost::Init() {
    if (g_proc) return;
    g_proc = Scheduler::SpawnKernelProcess("vmhost", host_entry, PRIO_NORMAL, 64, 256);
    if (g_testcmds[0]) Scheduler::SpawnKernelProcess("vmtest", test_entry, PRIO_NORMAL, 64, 256);   // test rig (satoru)
}

bool VMHost::Start(int distro, uint32_t ram_mb) {
    // settings > linux turned off: nothing starts the vm (satoru)
    if (!Enabled()) {
        set_err("Linux is turned off (Settings > Linux)");
        return false;
    }
    if (distro < 0 || distro >= g_distro_count) distro = DISTRO_ALPINE;
    // the host is a kernel process; the kurono.cli=1 boot shell runs before the
    // scheduler, so nothing would ever run it there (satoru)
    if (!Scheduler::IsPreemptiveKernelSchedulerActive()) {
        set_err("the background vm needs the kernel scheduler (not running in cli boot mode)");
        g_state = VMH_FAILED;
        return false;
    }
    Init();
    if (g_state != VMH_OFF && g_state != VMH_FAILED) return false;
    if (ram_mb && ram_mb < 128) ram_mb = 128;
    if (ram_mb > GUEST_MAX_RAM_MB) ram_mb = GUEST_MAX_RAM_MB;
    g_req_distro = distro;
    g_req_ram = ram_mb;
    g_req_start = 1;
    return true;
}

bool VMHost::StartKurono(const char* src, uint32_t ram_mb, const char* cmdline) {   // (satoru)
    if (!src || !*src) { set_err("kurono guest: no image given"); return false; }
    if (!Scheduler::IsPreemptiveKernelSchedulerActive()) { set_err("the background vm needs the kernel scheduler"); return false; }
    Init();
    if (g_state != VMH_OFF && g_state != VMH_FAILED) { set_err("a guest is already running (vm stop first)"); return false; }
    int n = 0;
    while (src[n] && n < (int)sizeof(g_kg_src) - 1) { g_kg_src[n] = src[n]; n++; }
    g_kg_src[n] = 0;
    n = 0;
    while (cmdline && cmdline[n] && n < (int)sizeof(g_kg_cmd) - 1) { g_kg_cmd[n] = cmdline[n]; n++; }
    g_kg_cmd[n] = 0;
    if (!ram_mb) ram_mb = 1024;
    if (ram_mb < 256) ram_mb = 256;
    if (ram_mb > GUEST_MAX_RAM_MB) ram_mb = GUEST_MAX_RAM_MB;
    g_req_distro = DISTRO_KURONO;
    g_req_ram = ram_mb;
    g_req_start = 1;
    return true;
}

int VMHost::SnapKurono(const char* png_path, char* msg, int msgmax) {   // (satoru)
    auto say = [&](const char* t) { int i = 0; while (t[i] && i < msgmax - 1) { msg[i] = t[i]; i++; } if (msgmax) msg[i] = 0; };
    uint64_t fb = MB2Boot::FramebufferGPA();
    if (g_distro != DISTRO_KURONO || g_state != VMH_RUNNING || !fb) { say("no kurono guest running"); return -1; }
    const int W = (int)MB2Boot::FB_W, H = (int)MB2Boot::FB_H;
    uint32_t fbsz = MB2Boot::FB_PITCH * MB2Boot::FB_H;
    uint8_t* pix = (uint8_t*)KernelHeap::Alloc(fbsz);
    uint8_t* raw = (uint8_t*)KernelHeap::Alloc(PngEncode::RawSize(W, H));
    uint32_t cap = PngEncode::MaxSize(W, H);
    uint8_t* out = (uint8_t*)KernelHeap::Alloc(cap);
    int rc = -1;
    if (pix && raw && out) {
        bool ok = true;
        for (uint32_t o = 0; ok && o < fbsz; o += 65536) {
            uint32_t k = fbsz - o > 65536 ? 65536 : fbsz - o;
            ok = GuestMemoryManager::ReadGuestPhys(fb + o, pix + o, k);
        }
        uint32_t len = ok ? PngEncode::Encode(pix, W, H, MB2Boot::FB_PITCH, 32, raw, out, cap) : 0;
        if (len && png_path[0] != '/') {
            // no leading '/': a new file in the KURONO partition root, readable
            // from the other os after a reboot (NAME.PNG, 8.3) (satoru)
            char n83[12]; for (int k = 0; k < 11; k++) n83[k] = ' ';
            n83[11] = 0;
            int k = 0, j = 0;
            while (png_path[j] && png_path[j] != '.' && k < 8) { char c = png_path[j++]; n83[k++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
            while (png_path[j] && png_path[j] != '.') j++;
            if (png_path[j] == '.') j++;
            k = 8;
            while (png_path[j] && k < 11) { char c = png_path[j++]; n83[k++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
            int wr = DiskLog::WriteRootFile(n83, out, len);
            if (wr == 0) { say("ok"); rc = (int)len; } else say(DiskLog::RootFileMsg(wr));
        } else if (len && KVFS::WriteFile(png_path, out, len) == KVFS_OK) { say("ok"); rc = (int)len; }
        else say(ok ? "png encode / vfs write failed" : "guest framebuffer read failed");
    } else say("out of memory");
    if (pix) KernelHeap::Free(pix);
    if (raw) KernelHeap::Free(raw);
    if (out) KernelHeap::Free(out);
    return rc;
}

void VMHost::Stop() { g_restart = 0; g_start_at = 0; g_req_stop = 1; }
void VMHost::Kill() { g_restart = 0; g_start_at = 0; g_req_kill = 1; }
VMHost::State VMHost::GetState() { return (State)g_state; }
const char* VMHost::LastError() { return g_err; }
uint32_t VMHost::Generation() { return g_gen; }
void VMHost::SetLogMirror(bool on) { g_mirror = on; }
void VMHost::SetFixedMode(bool on) { g_fixed = on; }
void VMHost::SetDiskGb(int gb) { g_disk_cmdline = gb < 0 ? 0 : (gb > 64 ? 64 : gb); }
int VMHost::DiskGb() {
    if (g_fixed) return 0;
    int gb = g_disk_cmdline >= 0 ? g_disk_cmdline : UIConfig::Int("linux.disk_gb", 16);
    return gb < 0 ? 0 : (gb > 64 ? 64 : gb);
}
void VMHost::SetPar(bool on) { g_par = on; g_smp_explicit = true; }
void VMHost::AutoSmp(int n) {
    if (g_smp_explicit || g_fixed || n < 12) return;
    int prog = n - 1 - 3;                      // minus the desktop, io and vm service cpus
    int v = prog / 2; if (v > 4) v = 4;
    if (v < 2) return;
    g_smp = v; g_par = true;
}
bool VMHost::Par() { return g_par && !g_fixed && g_smp > 1; }
int VMHost::ParCpus() { return Par() ? g_smp : 0; }
void VMHost::SetSmp(int n) { g_smp = n < 1 ? 1 : (n > 8 ? 8 : n); g_smp_explicit = true; }
bool VMHost::Fixed() { return g_fixed; }
void VMHost::SetDebug(bool on) { g_debug = on; if (on) g_mirror = true; }
void VMHost::SetDefaultRam(uint32_t mb) { if (mb >= 128 && mb <= GUEST_MAX_RAM_MB) { g_ram_default = mb; g_ram_cmdline = true; } }

const char* VMHost::StateName() {
    switch (g_state) {
        case VMH_OFF: return "off";
        case VMH_STARTING: return "starting";
        case VMH_RUNNING: return "running";
        case VMH_STOPPING: return "stopping";
        default: return "failed";
    }
}

const char* VMHost::DistroName() { return g_distro == DISTRO_DEBIAN ? "debian" : g_distro == DISTRO_KURONO ? "kurono" : "alpine"; }

void VMHost::ConsoleWrite(const char* s, int n) {
    // the input ring holds 16 kb; a paste larger than that waits for the guest (satoru)
    int done = 0, spins = 0;
    while (done < n && spins < 2000) {
        int k = GuestConsole::Write(0, s + done, n - done);
        done += k;
        if (done < n) {
            if (g_state != VMH_RUNNING) break;
            Scheduler::SleepMs(1);
            spins++;
        }
    }
}

int VMHost::ConsoleRead(uint64_t* cursor, char* buf, int max) {
    if (!g_out) return 0;
    uint64_t head = __atomic_load_n(&g_out_head, __ATOMIC_ACQUIRE);
    uint64_t c = *cursor;
    if (c > head) c = head;
    if (head - c > SCROLL - 4096) c = head - (SCROLL - 4096);   // fell behind: skip ahead
    int n = 0;
    while (c < head && n < max) buf[n++] = g_out[(c++) % SCROLL];
    *cursor = c;
    return n;
}

uint64_t VMHost::ConsoleHead() { return __atomic_load_n(&g_out_head, __ATOMIC_ACQUIRE); }
void VMHost::ConsoleResize(int cols, int rows) { GuestConsole::SetSize(0, cols, rows); }

bool VMHost::AgentReady() { return g_agent_ready && g_state == VMH_RUNNING; }

bool VMHost::AgentSend(const char* line) {
    char b[600]; int n = 0;
    while (line[n] && n < (int)sizeof(b) - 2) { b[n] = line[n]; n++; }
    b[n++] = '\n';
    return GuestConsole::Write(1, b, n) == n;
}

int VMHost::Exec(const char* cmd, char* out, int max, uint32_t timeout_ms, int* rc) {
    if (out && max > 0) out[0] = 0;
    if (rc) *rc = -1;
    if (!cmd || !AgentReady() || !out || max < 2) return -1;
    // one at a time; a second caller waits its turn (satoru)
    uint64_t t0 = Timer::GetRealMs64();
    while (__atomic_exchange_n(&g_exec_busy, 1, __ATOMIC_ACQUIRE)) {
        if (Timer::GetRealMs64() - t0 > timeout_ms) return -1;
        Scheduler::SleepMs(10);
    }
    static uint32_t s_id = 0;
    g_exec_len = 0; g_exec_out[0] = 0; g_exec_rc = -1; g_exec_done = false;
    g_exec_id = ++s_id;
    char line[560]; int n = 0;
    const char* pre = "exec ";
    while (*pre) line[n++] = *pre++;
    char idb[12]; int k = 0; uint32_t v = g_exec_id;
    do { idb[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) line[n++] = idb[--k];
    line[n++] = ' ';
    for (int i = 0; cmd[i] && n < (int)sizeof(line) - 1; i++) line[n++] = (cmd[i] == '\n') ? ' ' : cmd[i];
    line[n] = 0;
    AgentSend(line);
    while (!g_exec_done && Timer::GetRealMs64() - t0 < timeout_ms && GetState() == VMH_RUNNING)
        Scheduler::SleepMs(10);
    bool done = g_exec_done;
    g_exec_id = 0;                  // late lines for this id are dropped from here on
    int len = g_exec_len;
    if (len > max - 1) len = max - 1;
    for (int i = 0; i < len; i++) out[i] = g_exec_out[i];
    out[len] = 0;
    if (rc) *rc = g_exec_rc;
    __atomic_store_n(&g_exec_busy, 0, __ATOMIC_RELEASE);
    return done ? len : -1;
}

bool VMHost::WaitAgent(uint32_t timeout_ms) {
    uint64_t t0 = Timer::GetRealMs64();
    while (!AgentReady()) {
        State st = GetState();
        if (st == VMH_FAILED || st == VMH_OFF) return false;
        if (Timer::GetRealMs64() - t0 > timeout_ms) return false;
        Scheduler::SleepMs(20);
    }
    return true;
}

int VMHost::AgentLastLine(char* buf, int max) {
    int n = 0;
    while (g_agent_last[n] && n < max - 1) { buf[n] = g_agent_last[n]; n++; }
    buf[n] = 0;
    return n;
}

uint32_t VMHost::UptimeMs() { return g_state == VMH_RUNNING ? (uint32_t)(Timer::GetRealMs64() - g_start_ms) : 0; }
uint64_t VMHost::SliceCount() { return g_slices; }

static int app(char* o, int p, int mx, const char* s) { while (*s && p < mx - 1) o[p++] = *s++; o[p] = 0; return p; }
static int appu(char* o, int p, int mx, uint64_t v) { char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) { if (p < mx - 1) o[p++] = t[--k]; else --k; } o[p] = 0; return p; }

int VMHost::Status(char* out, int mx) {
    int p = 0;
    p = app(out, p, mx, DistroName()); p = app(out, p, mx, " vm: "); p = app(out, p, mx, StateName());
    if (g_err[0] && (g_state == VMH_FAILED || g_state == VMH_OFF)) { p = app(out, p, mx, " ("); p = app(out, p, mx, g_err); p = app(out, p, mx, ")"); }
    p = app(out, p, mx, "\n  uptime s: "); p = appu(out, p, mx, UptimeMs() / 1000);
    p = app(out, p, mx, ", boots: "); p = appu(out, p, mx, g_gen);
    p = app(out, p, mx, ", guest reboots: "); p = appu(out, p, mx, g_reboots);
    p = app(out, p, mx, "\n  agent: "); p = app(out, p, mx, AgentReady() ? "ready" : "not connected");
    p = app(out, p, mx, "\n  guest ram mb: "); p = appu(out, p, mx, g_cur_ram);
    p = app(out, p, mx, ", resident mb: "); p = appu(out, p, mx, (GuestMemoryManager::ResidentBytes() >> 20));
    p = app(out, p, mx, "\n  slices: "); p = appu(out, p, mx, g_slices);
    p = app(out, p, mx, " (idle "); p = appu(out, p, mx, g_idle_slices);
    p = app(out, p, mx, "), host ms in guest: "); p = appu(out, p, mx, g_run_us / 1000);
    const VMStats& st = Hypervisor::GetStats();
    p = app(out, p, mx, "\n  exits: "); p = appu(out, p, mx, st.total_exits);
    p = app(out, p, mx, " io: "); p = appu(out, p, mx, st.io_exits);
    p = app(out, p, mx, " mmio: "); p = appu(out, p, mx, st.mmio_exits);
    p = app(out, p, mx, " irq inj: "); p = appu(out, p, mx, st.irq_injections);
    uint32_t r = 0, w = 0, f = 0, e = 0;
    VBlk::Stats(&r, &w, &f, &e);
    p = app(out, p, mx, "\n  disk: reads "); p = appu(out, p, mx, r);
    p = app(out, p, mx, " ("); p = appu(out, p, mx, VBlk::BytesRead() >> 20);
    p = app(out, p, mx, " MB) writes "); p = appu(out, p, mx, w);
    p = app(out, p, mx, " ("); p = appu(out, p, mx, VBlk::BytesWritten() >> 20);
    p = app(out, p, mx, " MB) flushes "); p = appu(out, p, mx, f);
    p = app(out, p, mx, " errors "); p = appu(out, p, mx, e);
    p = app(out, p, mx, "\n  console: out "); p = appu(out, p, mx, GuestConsole::BytesOut(0));
    p = app(out, p, mx, " in "); p = appu(out, p, mx, GuestConsole::BytesIn(0));
    p = app(out, p, mx, "\n");
    p += VNet::Status(out + p, mx - p);
    p += WLBridge::Status(out + p, mx - p);
    p += VAudio::Status(out + p, mx - p);
    return p;
}

// ---- settings > linux (satoru)
int VMHost::DistroCount() { return g_distro_count; }
const VMHost::DistroDesc* VMHost::GetDistro(int i) {
    if (i < 0 || i >= g_distro_count) i = 0;
    return &g_distros[i];
}
int VMHost::DistroIndex(const char* id) {
    for (int i = 0; id && i < g_distro_count; i++) {
        const char* a = g_distros[i].id; const char* b = id;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}
bool VMHost::Enabled() { return UIConfig::Bool("linux.enabled", true); }
int VMHost::SelectedDistro() {
    int i = DistroIndex(UIConfig::Str("linux.distro", "alpine"));
    return (i >= 0 && g_distros[i].status == DS_READY) ? i : (int)DISTRO_ALPINE;
}
int VMHost::DistroInstalled(int i) {
    if (i < 0 || i >= g_distro_count) return 1;
    static int s_why[4] = { -1, -1, -1, -1 };
    static uint64_t s_at[4] = { 0, 0, 0, 0 };
    uint64_t now = Timer::GetRealMs64();
    if (i < 4 && s_why[i] >= 0 && now - s_at[i] < 10000) return s_why[i];
    const DistroDesc* d = &g_distros[i];
    int why = DiskLog::GuestImage(d->image83, nullptr, nullptr);
    if (why == 0 && i == DISTRO_DEBIAN &&
        (!KAsset::Size(d->kernel83, nullptr, nullptr) || !KAsset::Size(d->initrd83, nullptr, nullptr)))
        why = 4;
    if (why != 0 && why != 2 && why != 4) why = 1;
    if (i < 4) { s_why[i] = why; s_at[i] = now; }
    return why;
}

uint32_t VMHost::ConfiguredRamMb() {
    if (g_ram_cmdline) return g_ram_default;
    int mb = UIConfig::Int("linux.ram_mb", 0);
    if (mb >= 128 && mb <= (int)GUEST_MAX_RAM_MB) return (uint32_t)mb;
    // dynamic mode: the guest sees half of the host's ram (up to the guest cap); host
    // frames are only taken as it touches them (satoru)
    if (!g_fixed) {
        uint64_t half = PMM::GetTotalMemory() / 2 / (1024 * 1024);
        if (half > GUEST_MAX_RAM_MB) half = GUEST_MAX_RAM_MB;
        if (half >= g_ram_default) return (uint32_t)half;
    }
    return g_ram_default;
}
bool VMHost::RamFromCmdline() { return g_ram_cmdline; }

// kurono.vmboot=1 (test rig): start the vm at boot as if linux.autostart were
// set, so a vm + firefox boot needs no saved setting. (satoru)
static bool g_force_autostart = false;
void VMHost::SetForceAutostart(bool on) { g_force_autostart = on; }

// kurono.vmtest (satoru)
static void test_entry() {
    for (;;) {
        if (!g_testcmds[0] || !VMHost::AgentReady()) { Scheduler::SleepMs(200); continue; }
        char cmds[sizeof(g_testcmds)];
        for (unsigned i = 0; i < sizeof(cmds); i++) cmds[i] = g_testcmds[i];
        g_testcmds[0] = 0;
        char* p = cmds;
        while (*p) {
            while (*p == ' ') p++;
            char* e = p;
            while (*e && !(e[0] == ';' && e[1] == ';')) e++;
            bool last = !*e;
            *e = 0;
            char* t = e;
            while (t > p && t[-1] == ' ') *--t = 0;
            if (*p == '@' && p[1] == 's') {
                int s = (int)parse_u(p + 7, nullptr);
                SerialLogger::Log("[vmtest] sleep "); SerialLogger::LogDec(s); SerialLogger::Log("\r\n");
                for (int i = 0; i < s * 10; i++) Scheduler::SleepMs(100);
            } else if (*p) {
                static char res[8192]; int rc = -1;
                SerialLogger::Log("[vmtest] $ "); SerialLogger::Log(p); SerialLogger::Log("\r\n");
                int n = VMHost::Exec(p, res, (int)sizeof(res), 300000, &rc);
                if (n < 0) SerialLogger::Log("[vmtest] (timeout)\r\n");
                else {
                    for (int i = 0, ls = 0; i <= n; i++) {
                        if (i == n || res[i] == '\n') {
                            if (i > ls) {
                                char sv = res[i]; res[i] = 0;
                                SerialLogger::Log("[vmtest] > "); SerialLogger::Log(res + ls); SerialLogger::Log("\r\n");
                                if (i < n) res[i] = sv;
                            }
                            ls = i + 1;
                        }
                    }
                    SerialLogger::Log("[vmtest] rc="); SerialLogger::LogDec(rc); SerialLogger::Log("\r\n");
                }
            }
            if (last) break;
            p = e + 2;
        }
    }
}
void VMHost::SetTestCmds(const char* c) {
    int i = 0;
    while (c && c[i] && i < (int)sizeof(g_testcmds) - 1) { g_testcmds[i] = c[i]; i++; }
    g_testcmds[i] = 0;
}

void VMHost::AutostartAtBoot() {
    if (!Enabled() || (!g_force_autostart && !UIConfig::Bool("linux.autostart", false))) return;
    // called while the desktop is set up, before the scheduler starts: the
    // host process is spawned now and runs once it does (satoru)
    Init();
    // a few seconds later, so the desktop's first frames and autorun apps
    // are not competing with the 1 s vm creation on the bsp (satoru)
    SerialLogger::Log("[vmhost] autostart queued\r\n");
    g_req_distro = SelectedDistro();
    g_req_ram = 0;
    g_start_at = Timer::GetRealMs64() + 4000;
}

void VMHost::Restart() {
    if (!Enabled()) return;
    if (g_state == VMH_RUNNING || g_state == VMH_STARTING) { g_restart = 1; g_req_stop = 1; return; }
    if (g_state == VMH_STOPPING) { g_restart = 1; return; }
    Start(SelectedDistro(), 0);   // "use debian" applies on the next start (satoru)
}

void VMHost::GetInfo(Info* o) {
    o->state = g_state;
    o->distro = g_distro;
    o->uptime_ms = UptimeMs();
    o->ram_mb = g_cur_ram;
    o->boots = g_gen;
    o->reboots = g_reboots;
    o->agent = AgentReady();
    o->loop_stopped = g_loop_stopped && g_state == VMH_FAILED;
    o->start_pending = g_start_at != 0 || g_restart != 0 || g_req_start != 0;
    o->error = g_err;
}

// end (satoru)
