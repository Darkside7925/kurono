// see dbgwatch.h (satoru)
#include "dbgwatch.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../drivers/timer.h"

void UbDump(const Process* p);   // hal.cpp [ubchk] (satoru)

namespace {
    volatile uint64_t g_va[2] = { 0, 0 };
    struct Ev { uint64_t ms, rip, as; uint32_t val, pid; uint8_t cpu, slot; char name[14]; };
    const uint32_t RING = 256;
    Ev g_ring[RING];
    volatile uint32_t g_head = 0;
    volatile uint64_t g_hits = 0;
    volatile uint32_t g_said = 0;

    void put_dr(int i, uint64_t v) {
        if (i == 0) __asm__ volatile("mov %0, %%dr0" :: "r"(v));
        else        __asm__ volatile("mov %0, %%dr1" :: "r"(v));
    }
}

bool DbgWatch::Armed() { return g_va[0] || g_va[1]; }

void DbgWatch::ApplyThisCpu() {
    if (!Armed()) return;
    uint64_t dr7 = 0;
    for (int i = 0; i < 2; i++) {
        if (!g_va[i]) continue;
        put_dr(i, g_va[i] & ~3ull);
        // L/G enable, RW = 01 (write), LEN = 11 (4 bytes) (satoru)
        dr7 |= (3ull << (i * 2)) | (1ull << (16 + i * 4)) | (3ull << (18 + i * 4));
    }
    __asm__ volatile("mov %0, %%dr7" :: "r"(dr7));
}

void DbgWatch::Arm(uint64_t va0, uint64_t va1) {
    g_va[0] = va0; g_va[1] = va1;
    ApplyThisCpu();
    SMP::BroadcastTlbFlush();   // the ipi receiver loads the debug registers too (satoru)
    SerialLogger::Log("[dbgw] armed write watch va0=");
    SerialLogger::LogHex64(va0);
    SerialLogger::Log(" va1=");
    SerialLogger::LogHex64(va1);
    SerialLogger::Log("\r\n");
}

bool DbgWatch::OnDebugTrap(InterruptFrame* f) {
    uint64_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    if (!(dr6 & 3)) return false;
    __asm__ volatile("mov %0, %%dr6" :: "r"(0ull));
    int slot = (dr6 & 1) ? 0 : 1;
    uint64_t va = g_va[slot] & ~3ull;
    uint32_t val = 0;
    if (va) val = *(volatile uint32_t*)(uintptr_t)va;   // trap is after the store; smap is off (satoru)
    Process* p = Scheduler::GetCurrentProcess();
    uint64_t as;
    __asm__ volatile("mov %%cr3, %0" : "=r"(as));
    uint32_t i = __atomic_fetch_add(&g_head, 1u, __ATOMIC_RELAXED) % RING;
    Ev& e = g_ring[i];
    e.ms = Timer::GetRealMs64(); e.rip = f->rip; e.as = as; e.val = val;
    e.pid = p ? p->pid : 0; e.cpu = (uint8_t)SMP::CpuIndex(); e.slot = (uint8_t)slot;
    int k = 0;
    if (p) for (; k < 13 && p->name[k]; k++) e.name[k] = p->name[k];
    e.name[k] = 0;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    // a KERNEL store into the watched word (corruptor hunt, kurono.envwatch):
    // name the copy - dst/src/count registers and the first stack words (a
    // leaf memcpy has its return address at [rsp]) - plus the task's recent
    // syscalls from the [ubchk] trace. first 6 only. (satoru)
    if (!(f->cs & 3)) {
        static volatile uint32_t s_kh = 0;
        if (__atomic_add_fetch(&s_kh, 1u, __ATOMIC_RELAXED) <= 6) {
            SerialLogger::Log("[dbgw] KERNEL store va="); SerialLogger::LogHex64(va);
            SerialLogger::Log(" rip="); SerialLogger::LogHex64(f->rip);
            SerialLogger::Log(" rdi="); SerialLogger::LogHex64(f->rdi);
            SerialLogger::Log(" rsi="); SerialLogger::LogHex64(f->rsi);
            SerialLogger::Log(" rdx="); SerialLogger::LogHex64(f->rdx);
            SerialLogger::Log(" rcx="); SerialLogger::LogHex64(f->rcx);
            SerialLogger::Log(" pid="); SerialLogger::LogDec((int)e.pid);
            SerialLogger::Log(" "); SerialLogger::Log(e.name);
            SerialLogger::Log(" cr3="); SerialLogger::LogHex64(as);
            SerialLogger::Log("\r\n[dbgw] kstack:");
            const uint64_t* ks = (const uint64_t*)(uintptr_t)f->rsp;
            for (int k = 0; k < 24; k++) {
                uint64_t w = ks[k];
                if (w >= 0xFFFF800000000000ull && w < 0xFFFF800004000000ull) {
                    SerialLogger::Log(" "); SerialLogger::LogHex64(w);
                }
            }
            SerialLogger::Log("\r\n");
            if (p) ::UbDump(p);
        }
    }
    // a suspicious value right away: bit 31 set, or a small non-zero integer (satoru)
    bool odd = (val & 0x80000000u) || (val != 0 && val < 0x400 && val != 1 && val != 2);
    if (odd && __atomic_add_fetch(&g_said, 1u, __ATOMIC_RELAXED) <= 64) {
        SerialLogger::Log("[dbgw] write va="); SerialLogger::LogHex64(va);
        SerialLogger::Log(" val="); SerialLogger::LogHex(val);
        SerialLogger::Log(" rip="); SerialLogger::LogHex64(f->rip);
        SerialLogger::Log((f->cs & 3) ? " user" : " KERNEL");
        SerialLogger::Log(" pid="); SerialLogger::LogDec((int)e.pid);
        SerialLogger::Log(" "); SerialLogger::Log(e.name);
        SerialLogger::Log(" cpu="); SerialLogger::LogDec((int)e.cpu);
        SerialLogger::Log("\r\n");
    }
    return true;
}

void DbgWatch::Dump(const char* why) {
    if (!Armed()) return;
    uint32_t h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    uint32_t n = h < 24 ? h : 24;   // the newest 24 (satoru)
    SerialLogger::Log("[dbgw] "); SerialLogger::Log(why ? why : "");
    SerialLogger::Log(" hits="); SerialLogger::LogDec((int)g_hits);
    SerialLogger::Log("\r\n");
    for (uint32_t j = h - n; j != h; j++) {
        const Ev& e = g_ring[j % RING];
        SerialLogger::Log("[dbgw]  t="); SerialLogger::LogDec((int)e.ms);
        SerialLogger::Log(" s"); SerialLogger::LogDec((int)e.slot);
        SerialLogger::Log(" val="); SerialLogger::LogHex(e.val);
        SerialLogger::Log(" rip="); SerialLogger::LogHex64(e.rip);
        SerialLogger::Log(" pid="); SerialLogger::LogDec((int)e.pid);
        SerialLogger::Log(" "); SerialLogger::Log(e.name);
        SerialLogger::Log(" cpu="); SerialLogger::LogDec((int)e.cpu);
        SerialLogger::Log("\r\n");
    }
}

// end (satoru)
