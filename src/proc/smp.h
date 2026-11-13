#pragma once
#include "../kernel/types.h"

//  kurono smp - multi-core support. phase 1 is foundation only: enable the local
//  apic, enumerate the cpus (acpi madt, with a cpuid/sequential fallback), and
//  build the per-cpu data blocks. starting the application processors (sipi) and
//  per-cpu scheduling come in later phases. (satoru)

#define SMP_MAX_CPUS 32

struct Process;        // fwd - set per-cpu in the scheduler phase (satoru)
struct InterruptFrame; // fwd - ApIdleFrame fills one for the syscall exit path (satoru)

//  one of these per logical cpu, indexed by a dense cpu index (0 = bsp). the
//  apic-id -> index map lets a running cpu find its own block in O(1). (satoru)
struct PerCpu {
    // THESE TWO MUST STAY AT OFFSETS 0 AND 8 - the SYSCALL entry stub reads them
    // gs-relative (gs base = &this PerCpu, set via swapgs) so that two cores can
    // syscall at once without sharing one global kernel stack. (satoru)
    uint64_t user_rsp_save;      // offset 0: scratch for the user rsp at entry (satoru)
    uint64_t kernel_rsp;         // offset 8: this cpu's syscall kernel stack (satoru)
    uint32_t cpu_index;          // dense index, 0 = bsp (satoru)
    uint32_t apic_id;            // hardware local apic id (satoru)
    volatile uint32_t online;    // 0 until the cpu marks itself running (satoru)
    uint64_t kernel_stack_top;   // this cpu's idle/bring-up stack (satoru)
    void*    arch;               // per-cpu gdt+tss block (later) (satoru)
    Process* current;            // process currently on this cpu (satoru)
};

static_assert(__builtin_offsetof(PerCpu, user_rsp_save) == 0,
              "syscall stub reads the user-rsp scratch at gs:0 (satoru)");
static_assert(__builtin_offsetof(PerCpu, kernel_rsp) == 8,
              "syscall stub reads the kernel rsp at gs:8 (satoru)");

//  acpi root pointer handed over by the bootloader (multiboot2 tag 14/15, built
//  by the uefi loader from the efi configuration table). uefi has no legacy
//  0xe0000 rsdp, so without this the madt is invisible there. the rsdp is
//  copied into kernel-owned static memory; Rsdp() returns its address or 0. (satoru)
namespace BootAcpi {
    void     SetRsdp(const uint8_t* src, uint32_t avail);  // validates + copies (satoru)
    void     SetEfiBoot();                                 // mb2 tag 12 seen (satoru)
    // the efi system table pointer, so hal can call the runtime reset service. (satoru)
    void     SetSystemTable(uint64_t st);
    uint64_t SystemTable();
    uint64_t Rsdp();
    bool     IsEfiBoot();
}

class SMP {
public:
    // cap on cpus registered (kurono.maxcpus=N). 0 = default policy (8 on efi
    // boots, SMP_MAX_CPUS otherwise). call before Init. (satoru)
    static void SetMaxCpus(uint32_t n);
    // kurono.x2apictest=1: force the bsp into x2apic first so the disable path runs;
    // =2: also treat x2apic as hardware-locked so the msr-driven lapic path runs. (satoru)
    static void SetX2apicTest(int mode);
    // kurono.madttest=1: parse a fabricated madt (type 0 + type 9 + dup + hotplug +
    // id>=255) and log the verdict; touches no real cpu state. (satoru)
    static void SelfTestMadt();

    // bsp-side: enable the lapic, parse topology, register the bsp as cpu 0. safe
    // to call even on a single-core machine (it just registers the bsp). (satoru)
    static void Init();

    static uint32_t CpuCount();        // logical cpus discovered (satoru)
    static uint32_t OnlineCount();     // cpus that have marked themselves online (satoru)
    // kernel stack bytes handed to the application processors at bring-up (boot census). (satoru)
    static uint64_t KernelStackBytes();
    // containment park: a cli-hlt'ed cpu can never ack a tlb ipi again, but it
    // still counted as online - so EVERY later BroadcastTlbFlush burned its full
    // 20ms timeout (under kls_lock for mprotect/munmap) and the flush epoch
    // never advanced, degrading the frame quarantine for the rest of the
    // session. call this right before parking so want-counts shrink. (satoru)
    static void     MarkSelfOffline();

    static uint32_t ApicId();          // calling cpu's local apic id (reads the lapic) (satoru)
    static uint32_t CpuIndex();        // calling cpu's dense index (via the apic-id map) (satoru)
    static PerCpu*  Current();         // per-cpu block for the calling cpu (satoru)
    static PerCpu*  ByIndex(uint32_t i);

    // point this cpu's KERNEL_GS_BASE at its PerCpu block so the SYSCALL stub can
    // reach gs:0 / gs:8 after swapgs. called by the bsp + every ap. (satoru)
    static void SetupGsBase();

    // xapic register access - the lapic mmio window is identity-mapped. (satoru)
    static uint32_t LapicRead(uint32_t reg);
    static void     LapicWrite(uint32_t reg, uint32_t val);
    static void     LapicEnable();     // spurious-int vector reg: set enable + vector (satoru)

    // phase 2 - bring up the application processors. no-op until implemented. (satoru)
    static void StartAPs();

    // phase 3d - cooperative user-thread dispatch on the application processors.
    // when enabled, each ap claims fresh Ready user processes (affinity-permitting)
    // off the ready_queue and runs them in ring-3, in parallel with the bsp. default
    // OFF so a normal boot parks the APs exactly as before. (satoru)
    static void SetApUserSched(bool on);
    static bool ApUserSched();

    // smp thread dispatch - when enabled, each ap also RESUMES ready sibling
    // threads of multi-threaded processes (clone threads with a saved user
    // frame, affinity-permitting), so e.g. firefox's compositor/render/ipc
    // threads run truly in parallel with the chrome main on the bsp. default
    // OFF (kurono.apthreads=1 enables it before the aps start). (satoru)
    static void SetApThreadSched(bool on);
    static bool ApThreadSched();

    // fill f with a ring-0 reentry into this ap's dispatch loop: the syscall
    // exit path irets into it when the ap's current thread exits and nothing
    // else is claimable for this cpu. (satoru)
    static void ApIdleFrame(InterruptFrame* f);

    // switch this cpu to the KERNEL address space (captured at StartAPs) if it
    // is currently on some process's cr3. an idle/parked cpu must NEVER sit on
    // a dead process's root: the reaper can destroy that address space and
    // recycle its page-table frames while this cpu's walker still fetches
    // through them (garbage kernel translations = random corruption). no-op
    // when the kernel root was never captured or already active. (satoru)
    static void LoadKernelCr3();

    // tlb shootdown: ipi (vector 0x41) every OTHER online cpu to reload cr3,
    // then wait (bounded, with re-ipi retries) for their acks. call after
    // unmapping/reprotecting user pages of an address space whose threads may
    // run on other cores; no-op with a single online cpu. (satoru)
    static void BroadcastTlbFlush();
    static void HandleTlbIpi();   // receiver side: reload cr3 + ack (satoru)
    // count of shootdowns that gave up unacked (a peer held IF off >20ms) -
    // each one leaves a stale peer + the unmapped va is reused immediately,
    // the lost-write wedge suspect (task 21). (satoru)
    static uint32_t TlbFlushTimeouts();

    // flush epochs for the user-frame quarantine: a freed frame is only safe to
    // reuse once a shootdown that STARTED after the frame's pte was cleared has
    // been acked by EVERY other online cpu (a full cr3 reload everywhere wipes
    // any stale translation). start-seq bumps at each broadcast entry; full-seq
    // records the highest start-seq whose acks all arrived. (satoru)
    static uint64_t TlbFlushStartSeq();
    static uint64_t TlbFlushFullSeq();

    // sched=2 cpu scaling (no-ops in the old mode). an idle ap halts instead of
    // polling the scheduler lock every 50us; KickOneIdle wakes one halted ap
    // with the resched ipi (vector 0x42). (satoru)
    static void KickOneIdle();
    // retune the calling ap's lapic tick (kurono.svccpu service cpus run 1 khz). (satoru)
    static void SetLapicTickHz(uint32_t hz);
    // the bsp's 1 khz scheduler tick from its own lapic timer on vector 32 instead
    // of the pit through the 8259 (extint on lint0). mode 0 = auto (2+ cpus online
    // and an always-running lapic timer), 1 = force, -1 = keep the pit
    // (kurono.bsptick=pit). call once after StartAPs, interrupts off. (satoru)
    static void StartBspLapicTick(int mode);
    static bool BspLapicTickActive();
    static void TestSelfIpi(uint8_t vec);   // kurono.straytest: fixed ipi to this cpu (satoru)
    static void KickIdleFor(const Process* p);   // wake an idle ap that may run p (satoru)
    // sched=2: interrupt one cpu with the resched ipi (vector 0x42), so a pending
    // signal or kill is taken at its ring-3 return now. (satoru)
    static void KickCpu(uint32_t idx);
    // sched=2: CpuIndex() reads IA32_TSC_AUX (index+1, via rdtscp) instead of
    // the lapic id register, which is an mmio read (a vm exit under kvm) on
    // every call - and CpuIndex sits on every syscall and lock path. call on
    // the bsp after Init; aps program their own in ap_entry. (satoru)
    static void EnableFastCpuIndex();
    static bool DebugIdle(uint32_t idx);   // sched=2 halted-idle flag, diagnostics (satoru)

    // firmware IA32_TSC_ADJUST sanitize (see smp.cpp). the bsp calls this once,
    // before anything keeps a tsc base; every ap does its own in ap_entry.
    // fix=false (kurono.tscadjust=0) only records and logs the values.
    // inject_test (kurono.tscadjust.test=1) first writes the laptop's firmware
    // value on the bsp, for qemu. (satoru)
    static void TscAdjustBoot(bool fix, bool inject_test);
    static int64_t TscAdjustFirmware(uint32_t idx);   // what the firmware left on that cpu (satoru)
    static bool TscAdjustKept();                      // nonzero values were left in place (satoru)

    // cr0/cr4 policy, the same on the bsp and every ap. CR0.WP (bit 16) makes a
    // ring-0 store into a read-only user page fault, so a copy-out into a COW page
    // after fork goes through the cow fault path instead of writing the frame the
    // other process still shares. wp_on (kurono.wp=1) sets WP on every cpu; off,
    // each cpu keeps the WP bit it booted with (the grub path never set it, an efi
    // bsp inherits the firmware's). CR0.NE and the bsp's cr4 security bits
    // (cr4_set: smep/umip) are copied to the aps either way. the bsp calls this in
    // kernel_main, before smp; ap_entry calls ApplyCrPolicyAp. (satoru)
    static void CrPolicyBsp(bool wp_on, uint64_t cr4_set);
    static void ApplyCrPolicyAp();
    static bool WpOn();
    // a ring-0 write fault on a present user page that no handler resolved
    // (read-only, not cow, or no user task to own it). there is no uaccess fixup
    // table to return -EFAULT through, so with WP on this logs the site and runs
    // the store the way WP=0 does: WP is suspended on this cpu and re-armed by the
    // first interrupt that did not land on that store (WpRearm, isr_common_handler).
    // a stale read-only tlb entry for a pte that is writable now is just flushed
    // and retried. returns false when
    // WP is off (the fault is not ours). (satoru)
    static bool WpKernelWriteFallback(uint64_t cr2, uint64_t rip, uint64_t pte_flags);
    static void WpRearm(uint64_t interrupted_rip);
};
// end (satoru)
