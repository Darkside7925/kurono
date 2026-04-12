//  kurono os  -  hypervisor / vm lifecycle manager
//  type 1 (bare-metal) hypervisor that orchestrates:
//    - hardware virtualization (vt-x / amd-v)
//    - extended page tables (ept / npt)
//    - virtual device emulation
//    - linux guest booting
//    - vm-exit handling loop
//    - interrupt injection
//    - i/o port and msr bitmaps
//
//  this is the top-level component that ties all virtualization
//  subsystems together into a working hypervisor.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "ept.h"
#include "vmexit.h"
#include "vdevices.h"
#include "vserial.h"
#include "vdisk.h"
#include "guest_mem.h"
#include "linux_boot.h"
#include "alpine_data.h"
#include "debian_data.h"

enum VMState {
    VM_STATE_UNINITIALIZED = 0,
    VM_STATE_CREATED,           // vm created, not yet started
    VM_STATE_RUNNING,           // vm is actively executing
    VM_STATE_PAUSED,            // vm is paused
    VM_STATE_HALTED,            // guest executed hlt / shutdown
    VM_STATE_CRASHED,           // guest triple-faulted or fatal error
    VM_STATE_REBOOTING,         // guest requested reboot
    VM_STATE_DESTROYED          // vm destroyed, resources freed
};

enum LinuxGuestProfile {
    LINUX_GUEST_ALPINE = 0,
    LINUX_GUEST_DEBIAN = 1,
};

struct VMConfig {
    uint32_t ram_mb;            // guest ram in mb (default: 16)
    uint32_t disk_size_mb;      // virtual disk in mb (default: 8)
    bool     enable_serial;     // enable virtual com1
    bool     enable_disk;       // enable virtual ide disk
    bool     enable_apic;       // enable virtual apic
    uint32_t timer_tick_us;     // virtual timer tick interval (microseconds)
    const char* cmdline;        // kernel command line

    // default constructor
    void SetDefaults() {
        ram_mb         = 16;
        disk_size_mb   = 8;
        enable_serial  = true;
        enable_disk    = true;
        enable_apic    = true;
        timer_tick_us  = 1000; // 1 ms
        cmdline        = "console=ttyS0 earlyprintk=serial,ttyS0,115200 "
                         "root=/dev/sda rw init=/bin/sh nokaslr noapic "
                         "nosmp noacpi pci=off";
    }
};

struct VMStats {
    uint32_t total_exits;
    uint32_t io_exits;
    uint32_t mmio_exits;
    uint32_t irq_injections;
    uint32_t hlt_exits;
    uint32_t run_cycles;
    uint32_t tick_count;
    uint64_t guest_tsc;
    uint32_t serial_bytes_tx;
    uint32_t serial_bytes_rx;
    uint32_t disk_reads;
    uint32_t disk_writes;
};

constexpr uint32_t IO_BITMAP_SIZE = 65536 / 8; // 8192 bytes = 8 kb
// two 4 kb pages: bitmap a (ports 0x0000-0x7fff), bitmap b (0x8000-0xffff)
constexpr uint32_t IO_BITMAP_A_SIZE = 4096;
constexpr uint32_t IO_BITMAP_B_SIZE = 4096;

// covers msr ranges 0x00000000-0x00001fff and 0xc0000000-0xc0001fff
// 1 bit per msr, read/write separate = 4 * 1024 bytes
constexpr uint32_t MSR_BITMAP_SIZE  = 4096;

//  hypervisor  -  the main vm lifecycle manager
class Hypervisor {
public:
    static bool Init();
    static bool IsAvailable();

    static bool CreateVM(const VMConfig& config);
    static void DestroyVM();
    static VMState GetState() { return vm_state; }

    // creates a vm configured for alpine, loads embedded kernel + initramfs,
    // and enters the vm run loop.  returns true if the vm ran successfully.
    static bool BootAlpine();

    // boot alpine and run limited cycles to capture boot output,
    // then extract driver information from /proc and /sys via serial.
    // max_boot_exits: how many vm exits to run during initial boot
    static bool BootAlpineWithExtraction(uint32_t max_boot_exits = 50000);

    // creates a vm configured for debian minbase, loads the shared embedded
    // linux kernel plus embedded debian ext4 rootfs, and captures initial
    // boot output over the serial bridge.
    static bool BootDebianWithExtraction(uint32_t max_boot_exits = 75000);

    // after alpine is booted, send commands via serial to enumerate
    // its drivers, modules, and hardware  -  register them into kurono's
    // linuxdriverframework.
    static int  ExtractAlpineDrivers();

    // run the alpine vm for n more exits (incremental execution).
    // returns the vm state after running.
    static VMState RunAlpineCycles(uint32_t max_exits);
    static VMState RunDebianCycles(uint32_t max_exits);

    // send a shell command to the alpine guest via serial, run cycles
    // to let it execute, and capture the output.  returns bytes written
    // to out_buf.
    static int  AlpineExec(const char* cmd, char* out_buf, int out_max);
    static int  DebianExec(const char* cmd, char* out_buf, int out_max);

    // alpine state
    static bool IsAlpineBooted();
    static const char* GetAlpineBootLog();
    static int  GetAlpineBootLogLen();
    static bool IsDebianBooted();
    static const char* GetDebianBootLog();
    static int  GetDebianBootLogLen();

    // linux guest selection / integration state
    static void SetLinuxGuestEnabled(bool enabled);
    static bool IsLinuxGuestEnabled();
    static bool SetLinuxGuestProfile(LinuxGuestProfile profile);
    static LinuxGuestProfile GetLinuxGuestProfile();
    static const char* GetLinuxGuestProfileName();
    static bool CanSwitchLinuxGuestProfile();

    static bool LoadLinuxKernel(const uint8_t* bzimage, uint32_t size,
                                 const char* cmdline);
    static bool LoadInitrd(const uint8_t* data, uint32_t size);

    // runvm: enters the vm run loop. returns when the guest halts/crashes
    // or after max_exits exits (0 = unlimited).
    static VMState RunVM(uint32_t max_exits = 0);

    // runonecycle: execute one vm-entry → vm-exit → handle cycle.
    // returns the resulting vm state.
    static VMState RunOneCycle();

    // pausevm / resumevm: pause/resume execution
    static void PauseVM();
    static void ResumeVM();

    static VirtualSerial& GetSerial()  { return serial; }
    static VirtualDisk&   GetDisk()    { return disk; }

    static bool InjectInterrupt(uint8_t vector);
    static bool InjectException(uint8_t vector, bool has_error, uint32_t error);
    static void CheckAndInjectPendingIRQs();

    static bool HandleGuestIO(uint16_t port, bool is_out, uint8_t size,
                               uint32_t& value);

    static bool HandleGuestMMIO(uint64_t phys_addr, bool is_write,
                                 uint8_t size, uint32_t& value);

    static const VMStats& GetStats() { return stats; }
    static void ResetStats();

    static void DumpState();
    static void DumpGuestRegs();

    static int  ReadSerialOutput(char* buf, int max);
    static bool HasSerialOutput();

    static void SendSerialCommand(const char* cmd);
    static void SendSerialData(const uint8_t* data, int len);
    static int  DrainSerialOutput(char* buf, int max, int max_cycles);

private:
    static VMState   vm_state;
    static VMConfig  config;
    static vCPU*     vcpu;
    static bool      hw_available;

    static EPT_PML4* ept_root;
    static NPT_PML4* npt_root;

    static VirtualSerial serial;
    static VirtualDisk   disk;

    static uint8_t*  io_bitmap_a;
    static uint8_t*  io_bitmap_b;
    static uint8_t*  msr_bitmap;

    static VMStats   stats;

    static bool      alpine_booted;
    static char      alpine_boot_log[8192];
    static int       alpine_boot_log_len;
    static bool      debian_booted;
    static char      debian_boot_log[8192];
    static int       debian_boot_log_len;
    static bool      linux_guest_enabled;
    static LinuxGuestProfile linux_guest_profile;

    static bool SetupIOBitmap();
    static bool SetupMSRBitmap();
    static bool SetupEPT();
    static bool SetupDevices();
    static bool SetupVMCSForLinux();
    static bool SetupVMCBForLinux();

    static VMState ProcessVMExit();
    static void    TickDevices();

    static void ConfigureGuestProtectedMode(uint32_t entry_point,
                                             uint32_t boot_params_addr);
};
