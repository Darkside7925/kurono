//  kurono os - per vcpu local apic (xapic mmio mode) for smp linux guests
//
//  one instance per vcpu, apic id = vcpu index. registers, irr / isr, tpr, eoi, a
//  one-shot / periodic timer on a 100 mhz base clock (no tsc-deadline), fixed ipis
//  in physical and flat logical mode, and init / sipi delivered through a callback
//  to the hypervisor, which owns the vcpus. the legacy pic still feeds the bsp
//  (virtual wire), there is no io-apic. (satoru)
#pragma once
#include <stdint.h>

constexpr int VLAPIC_MAX = 8;

namespace VLapic {
    // init / sipi delivery: mode 5 = init, 6 = sipi (vector = start page) (satoru)
    typedef void (*IpiFn)(int target, int mode, uint8_t vector);
    void Reset(int ncpu, IpiFn ipi);
    int  Count();
    uint32_t Read(int cpu, uint32_t off);
    void Write(int cpu, uint32_t off, uint32_t value);
    void Poll(uint64_t now_ns);                 // fire due timers of every vcpu
    int  Pending(int cpu);                      // highest vector that may be delivered, or -1
    void Accept(int cpu, int vector);           // irr -> isr
    void Raise(int cpu, uint8_t vector);        // set irr (ipi, timer)
    void ResetOne(int cpu);                     // init: back to power-on state
    bool Enabled(int cpu);
    void SetKick(void (*fn)(int cpu));          // called when a vcpu gets a new interrupt (satoru)
    void DumpCounts();                          // debug (satoru)
}

// end (satoru)
