#ifndef KURONO_HAL_CPUFREQ_H
#define KURONO_HAL_CPUFREQ_H

#include "../kernel/types.h"

// CPU frequency scaling + idle/runtime power management.
//
// Mirrors the Linux /sys/devices/system/cpu/cpu*/cpufreq/ interface.
// On Intel chips ≥ Sandy Bridge we drive P-states via the IA32_PERF_CTL
// MSR (0x199); on AMD via IA32_HWCR + P-state MSRs (0xC0010062..0064).
// On bare-metal hypervisors we fall back to ACPI _PSS objects.
//
// Governors:
//   PERFORMANCE - pin to highest non-turbo P-state.
//   POWERSAVE   - pin to lowest P-state.
//   ONDEMAND    - sample CPU utilisation each tick; ramp up at >80%,
//                 step down at <20%.
//   SCHEDUTIL   - directly tied to scheduler load; bump on enqueue.
//
// Idle states (C-states) are entered from Scheduler::Idle() - we MWAIT
// into C1/C2/C3 depending on predicted residency.

namespace CPUFreq {

    static const int CPUFREQ_MAX_CPUS    = 32;
    static const int CPUFREQ_MAX_PSTATES = 16;

    enum Governor : uint8_t {
        GOV_PERFORMANCE = 0,
        GOV_POWERSAVE   = 1,
        GOV_ONDEMAND    = 2,
        GOV_SCHEDUTIL   = 3,
        GOV_USERSPACE   = 4,
    };

    struct PState {
        uint16_t freq_mhz;        // target operating frequency
        uint16_t voltage_mv;      // approximate VID
        uint8_t  perf_ctl;        // value to write to MSR_PERF_CTL
    };

    struct CPUInfo {
        bool     present;
        uint8_t  vendor;          // 0=intel, 1=amd, 2=other
        uint16_t base_mhz;
        uint16_t turbo_mhz;
        uint16_t cur_mhz;
        uint8_t  cur_pstate;
        Governor governor;
        uint8_t  pstate_count;
        PState   pstates[CPUFREQ_MAX_PSTATES];

        // Utilisation tracking for ondemand/schedutil.
        uint64_t busy_ticks;
        uint64_t total_ticks;
        uint8_t  recent_load_pct;
    };

    struct CState {
        uint8_t  level;           // 1=C1, 2=C2, 3=C3, 6=C6
        uint16_t latency_us;
        uint16_t target_residency_us;
    };

    void Init();

    // Tick from the scheduler at HZ.  Updates load average, advances
    // the active governor.
    void Tick(uint32_t cpu_id, bool was_idle);

    // Manual control.
    bool SetGovernor(uint32_t cpu_id, Governor g);
    bool SetPState(uint32_t cpu_id, uint8_t pstate_idx);
    Governor GetGovernor(uint32_t cpu_id);

    // Idle entry - called by Scheduler::Idle().  Picks the deepest
    // C-state whose latency fits the predicted idle residency.
    void EnterIdle(uint64_t predicted_residency_us);

    // Suspend-to-RAM (S3) and Hibernate (S4) entry points.  Both return
    // true on resume.
    bool SuspendToRam();
    bool Hibernate();

    const CPUInfo* GetCPU(uint32_t cpu_id);
    int  CPUCount();
}

#endif
