// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Hypervisor / VM Lifecycle Manager
//  Type 1 (bare-metal) hypervisor that orchestrates:
//    - Hardware virtualization (VT-x / AMD-V)
//    - Extended Page Tables (EPT / NPT)
//    - Virtual device emulation
//    - Linux guest booting
//    - VM-exit handling loop
//    - Interrupt injection
//    - I/O port and MSR bitmaps
//
//  This is the top-level component that ties all virtualization
//  subsystems together into a working hypervisor.
// ═══════════════════════════════════════════════════════════════════════════
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

// ─── VM State ────────────────────────────────────────────────────────────
enum VMState {
    VM_STATE_UNINITIALIZED = 0,
    VM_STATE_CREATED,           // VM created, not yet started
    VM_STATE_RUNNING,           // VM is actively executing
    VM_STATE_PAUSED,            // VM is paused
    VM_STATE_HALTED,            // Guest executed HLT / shutdown
    VM_STATE_CRASHED,           // Guest triple-faulted or fatal error
    VM_STATE_REBOOTING,         // Guest requested reboot
    VM_STATE_DESTROYED          // VM destroyed, resources freed
};

// ─── VM Configuration ────────────────────────────────────────────────────
struct VMConfig {
    uint32_t ram_mb;            // Guest RAM in MB (default: 16)
    uint32_t disk_size_mb;      // Virtual disk in MB (default: 8)
    bool     enable_serial;     // Enable virtual COM1
    bool     enable_disk;       // Enable virtual IDE disk
    bool     enable_apic;       // Enable virtual APIC
    uint32_t timer_tick_us;     // Virtual timer tick interval (microseconds)
    const char* cmdline;        // Kernel command line

    // Default constructor
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

// ─── VM Statistics ───────────────────────────────────────────────────────
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

// ─── I/O Bitmap — one bit per I/O port (65536 ports / 8 = 8 KB) ─────────
constexpr uint32_t IO_BITMAP_SIZE = 65536 / 8; // 8192 bytes = 8 KB
// Two 4 KB pages: bitmap A (ports 0x0000-0x7FFF), bitmap B (0x8000-0xFFFF)
constexpr uint32_t IO_BITMAP_A_SIZE = 4096;
constexpr uint32_t IO_BITMAP_B_SIZE = 4096;

// ─── MSR Bitmap — 4 KB total ─────────────────────────────────────────────
// Covers MSR ranges 0x00000000-0x00001FFF and 0xC0000000-0xC0001FFF
// 1 bit per MSR, read/write separate = 4 * 1024 bytes
constexpr uint32_t MSR_BITMAP_SIZE  = 4096;

// ═══════════════════════════════════════════════════════════════════════════
//  Hypervisor — the main VM lifecycle manager
// ═══════════════════════════════════════════════════════════════════════════
class Hypervisor {
public:
    // ── Lifecycle ────────────────────────────────────────────────────────
    static bool Init();
    static bool IsAvailable();

    // ── VM Management ────────────────────────────────────────────────────
    static bool CreateVM(const VMConfig& config);
    static void DestroyVM();
    static VMState GetState() { return vm_state; }

    // ── Guest kernel loading ─────────────────────────────────────────────
    static bool LoadLinuxKernel(const uint8_t* bzimage, uint32_t size,
                                 const char* cmdline);
    static bool LoadInitrd(const uint8_t* data, uint32_t size);

    // ── VM Execution ─────────────────────────────────────────────────────
    // RunVM: enters the VM run loop. Returns when the guest halts/crashes
    // or after max_exits exits (0 = unlimited).
    static VMState RunVM(uint32_t max_exits = 0);

    // RunOneCycle: execute one VM-entry → VM-exit → handle cycle.
    // Returns the resulting VM state.
    static VMState RunOneCycle();

    // PauseVM / ResumeVM: pause/resume execution
    static void PauseVM();
    static void ResumeVM();

    // ── Device Access ────────────────────────────────────────────────────
    static VirtualSerial& GetSerial()  { return serial; }
    static VirtualDisk&   GetDisk()    { return disk; }

    // ── Interrupt Injection ──────────────────────────────────────────────
    static bool InjectInterrupt(uint8_t vector);
    static bool InjectException(uint8_t vector, bool has_error, uint32_t error);
    static void CheckAndInjectPendingIRQs();

    // ── I/O Port Handling ────────────────────────────────────────────────
    static bool HandleGuestIO(uint16_t port, bool is_out, uint8_t size,
                               uint32_t& value);

    // ── MMIO Handling ────────────────────────────────────────────────────
    static bool HandleGuestMMIO(uint64_t phys_addr, bool is_write,
                                 uint8_t size, uint32_t& value);

    // ── Statistics ───────────────────────────────────────────────────────
    static const VMStats& GetStats() { return stats; }
    static void ResetStats();

    // ── Debug ────────────────────────────────────────────────────────────
    static void DumpState();
    static void DumpGuestRegs();

    // ── Serial console output reading ────────────────────────────────────
    static int  ReadSerialOutput(char* buf, int max);
    static bool HasSerialOutput();

private:
    // ── VM State ─────────────────────────────────────────────────────────
    static VMState   vm_state;
    static VMConfig  config;
    static vCPU*     vcpu;
    static bool      hw_available;

    // ── Extended Page Tables ─────────────────────────────────────────────
    static EPT_PML4* ept_root;
    static NPT_PML4* npt_root;

    // ── Virtual Devices ──────────────────────────────────────────────────
    static VirtualSerial serial;
    static VirtualDisk   disk;

    // ── Bitmaps ──────────────────────────────────────────────────────────
    static uint8_t*  io_bitmap_a;
    static uint8_t*  io_bitmap_b;
    static uint8_t*  msr_bitmap;

    // ── Statistics ───────────────────────────────────────────────────────
    static VMStats   stats;

    // ── Internal Setup ───────────────────────────────────────────────────
    static bool SetupIOBitmap();
    static bool SetupMSRBitmap();
    static bool SetupEPT();
    static bool SetupDevices();
    static bool SetupVMCSForLinux();
    static bool SetupVMCBForLinux();

    // ── VM-Exit Processing ───────────────────────────────────────────────
    static VMState ProcessVMExit();
    static void    TickDevices();

    // ── VMCS guest state for protected-mode Linux entry ──────────────────
    static void ConfigureGuestProtectedMode(uint32_t entry_point,
                                             uint32_t boot_params_addr);
};
