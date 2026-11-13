//  kurono os - background vm host
//
//  runs a linux guest (alpine by default, debian with the same machinery) from
//  its own kernel process, so guest time advances whether or not anything in
//  kurono is looking at it. the process owns the vmcs: every vmx operation
//  happens on it (kernel processes stay on the bsp), it runs the guest in
//  short real-time slices and yields to the desktop between them, and sleeps
//  while the guest idles in hlt. other threads only post requests and use the
//  console / agent byte rings. a guest that crashes, panics or powers off is
//  torn down here and never takes kurono with it. (satoru)
#pragma once
#include <stdint.h>

namespace VMHost {
    enum State { VMH_OFF = 0, VMH_STARTING, VMH_RUNNING, VMH_STOPPING, VMH_FAILED };
    // DISTRO_KURONO is not in the distro table (settings never lists it): a
    // multiboot2 Kurono kernel as the guest, started only by StartKurono. (satoru)
    enum Distro { DISTRO_ALPINE = 0, DISTRO_DEBIAN = 1, DISTRO_KURONO = 2 };

    // one entry per linux the vm host knows. the settings page lists this
    // table and prepare() reads the file names from it, so a new distro is one
    // entry here (plus its boot path while it is not DS_READY). index = Distro. (satoru)
    enum DistroStatus { DS_READY = 0, DS_COMING_SOON = 1 };
    struct DistroDesc {
        const char* id;             // settings key value ("alpine") (satoru)
        const char* name;           // "Alpine Linux" (satoru)
        const char* version;        // shown under the name (satoru)
        const char* kernel83;       // 8.3 names in the KURONO partition root (satoru)
        const char* initrd83;
        const char* image83;
        const char* image_name;     // the same image as a user sees it ("ALPINE.IMG") (satoru)
        uint32_t    default_ram_mb;
        int         status;         // DistroStatus (satoru)
        const char* note;           // why it is not ready, or a one line description (satoru)
    };
    int DistroCount();
    const DistroDesc* GetDistro(int i);
    int DistroIndex(const char* id);               // -1 if unknown (satoru)

    // persisted choices (ui.conf, written by settings > linux) (satoru)
    bool Enabled();                                // linux.enabled, default on (satoru)
    int  SelectedDistro();                         // linux.distro, a DS_READY entry (satoru)
    // can distro i start: 0 yes, 1 its disk image is missing, 2 the image is fragmented,
    // 4 its kernel / initramfs files are missing. cached for 10 s (it reads the disk). (satoru)
    int  DistroInstalled(int i);
    uint32_t ConfiguredRamMb();                    // what the next start uses for ram_mb 0 (satoru)
    bool RamFromCmdline();                         // kurono.vmram=N overrides the setting (satoru)
    // linux.autostart: start the vm a few seconds after the desktop is up (satoru)
    void AutostartAtBoot();
    void SetForceAutostart(bool on);   // kurono.vmboot=1 test rig (satoru)
    // kurono.vmtest="cmd ;; cmd": a kernel process runs each command in the guest once
    // the agent is up and logs [vmtest] lines. "@sleep N" waits N seconds. test rig (satoru)
    void SetTestCmds(const char* cmds);

    // live status for the settings page (satoru)
    struct Info {
        int      state;             // State (satoru)
        int      distro;
        uint32_t uptime_ms;
        uint32_t ram_mb;            // ram of the running (or last) vm (satoru)
        uint32_t boots, reboots;
        bool     agent;
        bool     loop_stopped;      // the reboot loop guard stopped it (satoru)
        bool     start_pending;     // autostart or restart queued (satoru)
        const char* error;
    };
    void GetInfo(Info* out);
    void Restart();                                // clean stop, then start again (satoru)

    void Init();                                   // spawn the host process (idempotent)
    // request a boot; ram_mb 0 = default (kurono.vmram=N or 1024). false if
    // already up. (satoru)
    // refused (false, LastError says why) while linux is off in settings. (satoru)
    bool Start(int distro = DISTRO_ALPINE, uint32_t ram_mb = 0);
    // boot a Kurono kernel (multiboot2 ELF64) as the guest, for testing a new
    // build without rebooting this machine. src: a vfs path ("/home/user/k.elf")
    // or an 8.3 name in the KURONO partition root ("KGUEST  ELF"). the guest's
    // com1 is its whole log: every line lands in this kernel's log as "[gs] "
    // (so in KURONO.LOG too) and in the console scrollback. no agent, so Stop
    // forces it off. (satoru)
    bool StartKurono(const char* src, uint32_t ram_mb, const char* cmdline);
    // write the guest framebuffer (Kurono guest) as a png into the vfs. (satoru)
    int  SnapKurono(const char* png_path, char* msg, int msgmax);
    void Stop();                                   // clean: guest agent poweroff, forced after a timeout
    void Kill();                                   // immediate teardown
    State GetState();
    const char* StateName();
    const char* DistroName();
    const char* LastError();
    uint32_t UptimeMs();
    uint64_t SliceCount();
    uint32_t Generation();                         // bumps on every boot

    // the guest login console (hvc0). output is kept in a scrollback ring that
    // any number of readers follow with their own cursor. (satoru)
    void ConsoleWrite(const char* s, int n);
    int  ConsoleRead(uint64_t* cursor, char* buf, int max);
    uint64_t ConsoleHead();
    void ConsoleResize(int cols, int rows);

    // the guest agent line channel (hvc1) (satoru)
    bool AgentReady();
    bool AgentSend(const char* line);              // one command, newline added
    int  AgentLastLine(char* buf, int max);
    // run a shell command in the guest (login environment) and capture its
    // output (up to max-1 bytes, 60 kb at most). blocks the caller up to
    // timeout_ms; returns the output length or -1 (no agent / timed out). (satoru)
    int  Exec(const char* cmd, char* out, int max, uint32_t timeout_ms, int* rc = nullptr);
    // wait for a starting vm's agent; false if it failed or timed out (satoru)
    bool WaitAgent(uint32_t timeout_ms);

    int  Status(char* out, int max);               // human readable summary
    void SetLogMirror(bool on);                    // copy guest console lines to the serial log
    void SetDefaultRam(uint32_t mb);
    void SetFixedMode(bool on);                    // kurono.vm.fixed=1: the old fixed ram / one cpu vm (satoru)
    bool Fixed();
    void SetDiskGb(int gb);                        // kurono.vmdisk=N virtual guest disk size in gb (satoru)
    int  DiskGb();                                 // configured size, 0 = classic image only
    void SetSmp(int n);
    void SetPar(bool on);                          // kurono.vm.par=1: each vcpu on its own host cpu (satoru)
    bool Par();
    // no kurono.vm.smp given: dynamic mode defaults to min(4, half the program cpus) parallel vcpus on a
    // 12+ cpu machine; kurono.vm.smp=1 / kurono.vm.fixed=1 keep one (call before PlanServiceCpus) (satoru)
    void AutoSmp(int online_cpus);
    int  ParCpus();                                // vcpus that need service cpus (0/1 = none)                            // kurono.vm.smp=N vcpus for the alpine guest (0/1 = one) (satoru)
    void SetDebug(bool on);                        // kurono.vmdebug=1: guest kernel log on com1, heartbeat
}

// end (satoru)
