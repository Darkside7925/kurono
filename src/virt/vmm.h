#pragma once
//  kurono os  -  virtual machine monitor (vmm)
//  intel vt-x / amd-v hardware virtualization support
#include "../kernel/types.h"

#define CPUID_VMX_BIT       (1 << 5)   // ecx bit 5  -  intel vt-x
#define CPUID_SVM_BIT       (1 << 2)   // ecx bit 2  -  amd-v (svm)
#define CPUID_HYPERVISOR_BIT (1 << 31) // ecx bit 31  -  running under hypervisor

#define MSR_IA32_FEATURE_CONTROL    0x0000003A
#define MSR_IA32_VMX_BASIC          0x00000480
#define MSR_IA32_VMX_PINBASED_CTLS  0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS 0x00000482
#define MSR_IA32_VMX_EXIT_CTLS      0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS     0x00000484
#define MSR_IA32_VMX_MISC           0x00000485
#define MSR_IA32_VMX_CR0_FIXED0     0x00000486
#define MSR_IA32_VMX_CR0_FIXED1     0x00000487
#define MSR_IA32_VMX_CR4_FIXED0     0x00000488
#define MSR_IA32_VMX_CR4_FIXED1     0x00000489
#define MSR_IA32_VMX_PROCBASED_CTLS2 0x0000048B
#define MSR_IA32_VMX_EPT_VPID_CAP   0x0000048C
#define MSR_IA32_VMX_TRUE_PINBASED  0x0000048D
#define MSR_IA32_VMX_TRUE_PROCBASED 0x0000048E
#define MSR_IA32_VMX_TRUE_EXIT      0x0000048F
#define MSR_IA32_VMX_TRUE_ENTRY     0x00000490
#define MSR_VM_HSAVE_PA             0xC0010117  // amd  -  host save area pa
#define MSR_VM_CR                   0xC0010114  // amd  -  vm control
#define MSR_SVM_KEY                 0xC0010118  // amd  -  svm lock key

#define FEATURE_CONTROL_LOCKED      (1 << 0)
#define FEATURE_CONTROL_VMXON       (1 << 2)

#define CR4_VMXE   (1 << 13)

// guest state
#define VMCS_GUEST_ES_SEL           0x0800
#define VMCS_GUEST_CS_SEL           0x0802
#define VMCS_GUEST_SS_SEL           0x0804
#define VMCS_GUEST_DS_SEL           0x0806
#define VMCS_GUEST_FS_SEL           0x0808
#define VMCS_GUEST_GS_SEL           0x080A
#define VMCS_GUEST_LDTR_SEL         0x080C
#define VMCS_GUEST_TR_SEL           0x080E
#define VMCS_GUEST_CR0              0x6800
#define VMCS_GUEST_CR3              0x6802
#define VMCS_GUEST_CR4              0x6804
#define VMCS_CR0_READ_SHADOW        0x6004
#define VMCS_CR4_READ_SHADOW        0x6006
#define VMCS_GUEST_RSP              0x681C
#define VMCS_GUEST_RIP              0x681E
#define VMCS_GUEST_RFLAGS           0x6820
#define VMCS_GUEST_CS_BASE          0x6808
#define VMCS_GUEST_CS_LIMIT         0x4802
#define VMCS_GUEST_CS_AR            0x4816
#define VMCS_GUEST_SS_BASE          0x680C
#define VMCS_GUEST_SS_LIMIT         0x4804
#define VMCS_GUEST_SS_AR            0x4818
#define VMCS_GUEST_DS_BASE          0x680E
#define VMCS_GUEST_DS_LIMIT         0x4806
#define VMCS_GUEST_DS_AR            0x481A
#define VMCS_GUEST_ES_BASE          0x6806
#define VMCS_GUEST_ES_LIMIT         0x4800
#define VMCS_GUEST_ES_AR            0x4814
#define VMCS_GUEST_FS_BASE          0x680A
#define VMCS_GUEST_FS_LIMIT         0x4808
#define VMCS_GUEST_FS_AR            0x481C
#define VMCS_GUEST_GS_BASE          0x6810
#define VMCS_GUEST_GS_LIMIT         0x480A
#define VMCS_GUEST_GS_AR            0x481E
#define VMCS_GUEST_LDTR_BASE        0x6812
#define VMCS_GUEST_LDTR_LIMIT       0x480C
#define VMCS_GUEST_LDTR_AR          0x4820
#define VMCS_GUEST_TR_BASE          0x6814
#define VMCS_GUEST_TR_LIMIT         0x480E
#define VMCS_GUEST_TR_AR            0x4822
#define VMCS_GUEST_GDTR_BASE        0x6816
#define VMCS_GUEST_GDTR_LIMIT       0x4810
#define VMCS_GUEST_IDTR_BASE        0x6818
#define VMCS_GUEST_IDTR_LIMIT       0x4812
#define VMCS_GUEST_ACTIVITY         0x4826
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824
#define VMCS_VMCS_LINK_PTR          0x2800
#define VMCS_ENTRY_INT_INFO         0x4016
#define VMCS_ENTRY_EXCEPTION_ERROR  0x4018
#define VMCS_ENTRY_INSTR_LENGTH     0x401A
#define VMCS_MSR_BITMAP_ADDR        0x2004

// host state
#define VMCS_HOST_CS_SEL            0x0C02
#define VMCS_HOST_SS_SEL            0x0C04
#define VMCS_HOST_DS_SEL            0x0C06
#define VMCS_HOST_ES_SEL            0x0C00
#define VMCS_HOST_FS_SEL            0x0C08
#define VMCS_HOST_GS_SEL            0x0C0A
#define VMCS_HOST_TR_SEL            0x0C0C
#define VMCS_HOST_IA32_SYSENTER_CS  0x4C00
#define VMCS_HOST_CR0               0x6C00
#define VMCS_HOST_CR3               0x6C02
#define VMCS_HOST_CR4               0x6C04
#define VMCS_HOST_FS_BASE           0x6C06
#define VMCS_HOST_GS_BASE           0x6C08
#define VMCS_HOST_TR_BASE           0x6C0A
#define VMCS_HOST_GDTR_BASE         0x6C0C
#define VMCS_HOST_IDTR_BASE         0x6C0E
#define VMCS_HOST_IA32_SYSENTER_ESP 0x6C10
#define VMCS_HOST_IA32_SYSENTER_EIP 0x6C12
#define VMCS_HOST_RSP               0x6C14
#define VMCS_HOST_RIP               0x6C16

// control fields
#define VMCS_PIN_BASED_CONTROLS     0x4000
#define VMCS_PROC_BASED_CONTROLS    0x4002
#define VMCS_PROC_BASED_CONTROLS2   0x401E
#define VMCS_EXIT_CONTROLS          0x400C
#define VMCS_ENTRY_CONTROLS         0x4012
#define VMCS_EXCEPTION_BITMAP       0x4004
#define VMCS_IO_BITMAP_A            0x2000
#define VMCS_IO_BITMAP_B            0x2002
#define VMCS_MSR_BITMAP             0x2004
#define VMCS_EPT_POINTER            0x201A
#define VMCS_VPID                   0x0000

// vm-exit information
#define VMCS_EXIT_REASON            0x4402
#define VMCS_VM_INSTR_ERROR         0x4400
#define VMCS_EXIT_QUALIFICATION     0x6400
#define VMCS_EXIT_INT_INFO          0x4404
#define VMCS_EXIT_INT_ERROR         0x4406
#define VMCS_EXIT_INSTR_LENGTH      0x440C
#define VMCS_EXIT_INSTR_INFO        0x440E
#define VMCS_GUEST_PHYS_ADDR        0x2400

#define PIN_EXTERNAL_INT_EXIT       (1 << 0)
#define PIN_NMI_EXIT                (1 << 3)
#define PIN_VIRTUAL_NMI             (1 << 5)
#define PIN_PREEMPT_TIMER           (1 << 6)

#define PROC_INT_WINDOW_EXIT        (1 << 2)
#define PROC_USE_TSC_OFFSET         (1 << 3)
#define PROC_HLT_EXIT               (1 << 7)
#define PROC_INVLPG_EXIT            (1 << 9)
#define PROC_MWAIT_EXIT             (1 << 10)
#define PROC_RDPMC_EXIT             (1 << 11)
#define PROC_RDTSC_EXIT             (1 << 12)
#define PROC_CR3_LOAD_EXIT          (1 << 15)
#define PROC_CR3_STORE_EXIT         (1 << 16)
#define PROC_CR8_LOAD_EXIT          (1 << 19)
#define PROC_CR8_STORE_EXIT         (1 << 20)
#define PROC_USE_TPR_SHADOW         (1 << 21)
#define PROC_NMI_WINDOW_EXIT        (1 << 22)
#define PROC_MOV_DR_EXIT            (1 << 23)
#define PROC_IO_EXIT                (1 << 24)
#define PROC_IO_BITMAPS             (1 << 25)
#define PROC_MSR_BITMAPS            (1 << 28)
#define PROC_MONITOR_EXIT           (1 << 29)
#define PROC_PAUSE_EXIT             (1 << 30)
#define PROC_SECONDARY_CONTROLS     (1u << 31)

#define PROC2_VIRT_APIC             (1 << 0)
#define PROC2_ENABLE_EPT            (1 << 1)
#define PROC2_DESC_TABLE_EXIT       (1 << 2)
#define PROC2_ENABLE_RDTSCP         (1 << 3)
#define PROC2_VIRT_X2APIC           (1 << 4)
#define PROC2_ENABLE_VPID           (1 << 5)
#define PROC2_WBINVD_EXIT           (1 << 6)
#define PROC2_UNRESTRICTED_GUEST    (1 << 7)
#define PROC2_APIC_REGISTER_VIRT    (1 << 8)

#define VM_EXIT_HOST_ADDR_SPACE_SIZE (1 << 9)    // host is 64-bit
#define VM_EXIT_ACK_INT_ON_EXIT      (1 << 15)

#define VM_ENTRY_IA32E_MODE          (1 << 9)    // guest enters ia-32e (64-bit)
#define PROC2_VIRT_INT_DELIVERY     (1 << 9)

#define EXIT_REASON_EXCEPTION_NMI   0
#define EXIT_REASON_EXTERNAL_INT    1
#define EXIT_REASON_TRIPLE_FAULT    2
#define EXIT_REASON_INIT            3
#define EXIT_REASON_SIPI            4
#define EXIT_REASON_IO_SMI          5
#define EXIT_REASON_OTHER_SMI       6
#define EXIT_REASON_INT_WINDOW      7
#define EXIT_REASON_NMI_WINDOW      8
#define EXIT_REASON_TASK_SWITCH     9
#define EXIT_REASON_CPUID           10
#define EXIT_REASON_GETSEC          11
#define EXIT_REASON_HLT             12
#define EXIT_REASON_INVD            13
#define EXIT_REASON_INVLPG          14
#define EXIT_REASON_RDPMC           15
#define EXIT_REASON_RDTSC           16
#define EXIT_REASON_RSM             17
#define EXIT_REASON_VMCALL          18
#define EXIT_REASON_VMCLEAR         19
#define EXIT_REASON_VMLAUNCH        20
#define EXIT_REASON_VMPTRLD         21
#define EXIT_REASON_VMPTRST         22
#define EXIT_REASON_VMREAD          23
#define EXIT_REASON_VMRESUME        24
#define EXIT_REASON_VMWRITE         25
#define EXIT_REASON_VMXOFF          26
#define EXIT_REASON_VMXON           27
#define EXIT_REASON_CR_ACCESS       28
#define EXIT_REASON_MOV_DR          29
#define EXIT_REASON_IO_INSTR        30
#define EXIT_REASON_RDMSR           31
#define EXIT_REASON_WRMSR           32
#define EXIT_REASON_INVALID_GUEST   33
#define EXIT_REASON_MSR_LOADING     34
#define EXIT_REASON_MWAIT           36
#define EXIT_REASON_MONITOR_TRAP    37
#define EXIT_REASON_MONITOR         39
#define EXIT_REASON_PAUSE           40
#define EXIT_REASON_MCE             41
#define EXIT_REASON_TPR_BELOW       43
#define EXIT_REASON_APIC_ACCESS     44
#define EXIT_REASON_VIRT_EOI        45
#define EXIT_REASON_GDTR_IDTR       46
#define EXIT_REASON_LDTR_TR         47
#define EXIT_REASON_EPT_VIOLATION   48
#define EXIT_REASON_EPT_MISCONFIG   49
#define EXIT_REASON_INVEPT          50
#define EXIT_REASON_RDTSCP          51
#define EXIT_REASON_PREEMPT_TIMER   52
#define EXIT_REASON_INVVPID         53
#define EXIT_REASON_WBINVD          54
#define EXIT_REASON_XSETBV          55

#define SVM_INTERCEPT_INTR          (1 << 0)
#define SVM_INTERCEPT_NMI           (1 << 1)
#define SVM_INTERCEPT_SMI           (1 << 2)
#define SVM_INTERCEPT_INIT          (1 << 3)
#define SVM_INTERCEPT_VINTR         (1 << 4)
#define SVM_INTERCEPT_CPUID         (1 << 18)
#define SVM_INTERCEPT_HLT           (1 << 24)
#define SVM_INTERCEPT_IOIO          (1 << 27)
#define SVM_INTERCEPT_MSR           (1 << 28)

#define SVM_INTERCEPT2_VMRUN        (1 << 0)
#define SVM_INTERCEPT2_VMMCALL      (1 << 1)
#define SVM_INTERCEPT2_VMLOAD       (1 << 2)
#define SVM_INTERCEPT2_VMSAVE       (1 << 3)
#define SVM_INTERCEPT2_STGI         (1 << 4)
#define SVM_INTERCEPT2_CLGI         (1 << 5)
#define SVM_INTERCEPT2_SKINIT       (1 << 6)
#define SVM_INTERCEPT2_RDTSCP       (1 << 7)
#define SVM_INTERCEPT2_MONITOR      (1 << 10)
#define SVM_INTERCEPT2_MWAIT        (1 << 11)
#define SVM_INTERCEPT2_XSETBV       (1 << 13)

#define SVM_EXIT_CR0_READ           0x0000
#define SVM_EXIT_CR0_WRITE          0x0010
#define SVM_EXIT_CR3_WRITE          0x0013
#define SVM_EXIT_CR4_WRITE          0x0014
#define SVM_EXIT_EXCP_BASE          0x0040  // +vector (e.g. 0x004e = #pf)
#define SVM_EXIT_INTR               0x0060
#define SVM_EXIT_NMI                0x0061
#define SVM_EXIT_VINTR              0x0064
#define SVM_EXIT_RDTSC              0x006E
#define SVM_EXIT_PUSHF              0x0070
#define SVM_EXIT_POPF               0x0071
#define SVM_EXIT_CPUID              0x0072
#define SVM_EXIT_INVLPG             0x0073
#define SVM_EXIT_HLT                0x0078
#define SVM_EXIT_INVD               0x0076
#define SVM_EXIT_IOIO               0x007B
#define SVM_EXIT_MSR                0x007C
#define SVM_EXIT_SHUTDOWN           0x007F
#define SVM_EXIT_VMMCALL            0x0081
#define SVM_EXIT_NPF                0x0400  // nested page fault
#define SVM_EXIT_AVIC_INCOMPLETE    0x0401
#define SVM_EXIT_AVIC_NOACCEL       0x0402

enum VirtType {
    VIRT_NONE = 0,
    VIRT_INTEL_VTX,
    VIRT_AMD_SVM
};

//  intel vmcs region (4kb-aligned)
struct alignas(4096) VMCSRegion {
    uint32_t revision_id;
    uint32_t abort_indicator;
    uint8_t  data[4096 - 8];  // implementation-specific
};

//  amd vmcb  -  virtual machine control block (4kb-aligned)
//  split into control area (offset 0x000) and state save area (offset 0x400)
struct alignas(4096) VMCB {
    uint32_t intercept_cr_read;     // 0x000  -  cr read intercepts
    uint32_t intercept_cr_write;    // 0x004  -  cr write intercepts
    uint32_t intercept_dr_read;     // 0x008  -  dr read intercepts
    uint32_t intercept_dr_write;    // 0x00c  -  dr write intercepts
    uint32_t intercept_exceptions;  // 0x010  -  exception intercepts (bitmap)
    uint32_t intercept_misc1;       // 0x014  -  misc intercept set 1
    uint32_t intercept_misc2;       // 0x018  -  misc intercept set 2
    uint8_t  reserved_0[0x040 - 0x01C];
    uint16_t pause_filter_threshold;// 0x040
    uint16_t pause_filter_count;    // 0x042
    uint64_t iopm_base_pa;          // 0x048  -  i/o permission map base
    uint64_t msrpm_base_pa;         // 0x050  -  msr permission map base
    uint64_t tsc_offset;            // 0x058
    uint32_t guest_asid;            // 0x060  -  address space identifier
    uint8_t  tlb_control;           // 0x064
    uint8_t  reserved_1[0x068 - 0x065];
    uint64_t v_tpr;                 // 0x068  -  virtual tpr + v_irq (packed)
    uint64_t exit_code;             // 0x070  -  exit code
    uint64_t exit_info1;            // 0x078  -  exit info 1
    uint64_t exit_info2;            // 0x080  -  exit info 2
    uint64_t exit_int_info;         // 0x088  -  exit interrupt info
    uint64_t np_enable;             // 0x090  -  nested paging enable
    uint64_t avic_apic_bar;         // 0x098  -  avic apic bar
    uint8_t  reserved_2[0x0B0 - 0x0A0];
    uint64_t event_inject;          // 0x0b0  -  event injection
    uint64_t n_cr3;                 // 0x0b8  -  nested page table cr3 (ncr3)
    uint64_t lbr_virt_enable;       // 0x0c0
    uint64_t vmcb_clean_bits;       // 0x0c8
    uint64_t next_rip;              // 0x0d0  -  next sequential rip on intercept
    uint8_t  reserved_3[0x400 - 0x0D8];

    // segment registers
    struct SegmentReg {
        uint16_t selector;
        uint16_t attrib;
        uint32_t limit;
        uint64_t base;
    };
    SegmentReg es;                  // 0x400
    SegmentReg cs;                  // 0x410
    SegmentReg ss;                  // 0x420
    SegmentReg ds;                  // 0x430
    SegmentReg fs;                  // 0x440
    SegmentReg gs;                  // 0x450
    SegmentReg gdtr;                // 0x460
    SegmentReg ldtr;                // 0x470
    SegmentReg idtr;                // 0x480
    SegmentReg tr;                  // 0x490
    uint8_t  reserved_4[0x4CB - 0x4A0];
    uint8_t  cpl;                   // 0x4cb
    uint32_t reserved_5;            // 0x4cc
    uint64_t efer;                  // 0x4d0
    uint8_t  reserved_6[0x548 - 0x4D8];
    uint64_t cr4;                   // 0x548
    uint64_t cr3;                   // 0x550
    uint64_t cr0;                   // 0x558
    uint64_t dr7;                   // 0x560
    uint64_t dr6;                   // 0x568
    uint64_t rflags;                // 0x570
    uint64_t rip;                   // 0x578
    uint8_t  reserved_7[0x5D8 - 0x580];
    uint64_t rsp;                   // 0x5d8
    uint8_t  reserved_8[0x5F0 - 0x5E0];
    uint64_t rax;                   // 0x5f0
    uint64_t star;                  // 0x5f8
    uint64_t lstar;                 // 0x600
    uint64_t cstar;                 // 0x608
    uint64_t sfmask;                // 0x610
    uint64_t kernel_gs_base;        // 0x618
    uint64_t sysenter_cs;           // 0x620
    uint64_t sysenter_esp;          // 0x628
    uint64_t sysenter_eip;          // 0x630
    uint64_t cr2;                   // 0x638
    uint8_t  reserved_9[0x668 - 0x640];
    uint64_t g_pat;                 // 0x668  -  guest pat
    uint64_t dbg_ctl;               // 0x670
    uint64_t br_from;               // 0x678
    uint64_t br_to;                 // 0x680
    uint64_t last_excp_from;        // 0x688
    uint64_t last_excp_to;          // 0x690
    uint8_t  reserved_end[4096 - 0x698];
};

//  vcpu  -  per-virtual-cpu state
struct vCPU {
    int          vcpu_id;
    VirtType     type;       // vtx or svm
    bool         launched;   // true after first vmlaunch/vmrun
    VMCSRegion*  vmcs;       // intel vmcs (nullptr on amd)
    VMCB*        vmcb;       // amd vmcb  (nullptr on intel)
    uint32_t     exit_reason;
    uint64_t     exit_qualification;
    // general-purpose registers (not auto-saved by hw)
    uint64_t     regs[16];  // rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15
};

//  vmm  -  virtual machine monitor
class VMM {
public:
    static void   Init();
    static bool   IsSupported();
    static VirtType GetType();
    static const char* GetVendor();

    static bool   DetectVTx();         // intel vt-x via cpuid
    static bool   DetectSVM();         // amd-v  via cpuid
    static bool   IsVTxEnabled();      // check msr_ia32_feature_control
    static bool   IsSVMEnabled();      // check msr_vm_cr
    static bool   DetectWHPXNestedVirt(); // detect whpx nested virt support

    static bool   VMXOn();             // enter vmx root operation
    static void   VMXOff();            // leave vmx root
    static bool   VMClear(VMCSRegion* region);
    static bool   VMPtrLoad(VMCSRegion* region);
    static bool   VMLaunch();
    static bool   VMResume();
    static void VMWrite(uint32_t field, uint64_t value);
    static uint64_t VMRead(uint32_t field);

    static bool   SVMEnable();
    static void   SVMDisable();
    static void   VMRun(VMCB* vmcb);

    static vCPU*  CreateVCPU();
    static void   DestroyVCPU(vCPU* cpu);
    static bool   SetupVCPU(vCPU* cpu);
    static int    RunVCPU(vCPU* cpu); // returns exit reason

    static void   SetupVMCSGuestState(vCPU* cpu);
    static void   SetupVMCSHostState(vCPU* cpu);
    static void   SetupVMCSControls(vCPU* cpu);
    static void   SetupVMCBControls(vCPU* cpu);
    static void   SetupVMCBGuestState(vCPU* cpu);

    static int    GetActiveVCPUCount();
    static void   DumpVCPUState(vCPU* cpu);
    static uint32_t GetVMXRevisionId();
    static bool   IsNested();          // running under a hypervisor?
    static uint32_t GetLastVMInstructionError();
    static uint64_t GetLastVMEntryGuestRip();
    static uint64_t GetLastVMEntryGuestCr0();
    static uint64_t GetLastVMEntryGuestCr4();

    static bool   IsWHPX();            // running under hyper-v / whpx?
    static bool   IsWHPXNestedOk();    // whpx actually supports nested virt?
    static const char* GetHypervisorVendor(); // e.g. "microsoft hv"

    static uint64_t ReadMSR(uint32_t msr);
    static void     WriteMSR(uint32_t msr, uint64_t value);
    static bool     SafeReadMSR(uint32_t msr, uint64_t& out);

    static uint64_t ReadCR0();
    static uint64_t ReadCR3();
    static uint64_t ReadCR4();

private:
    static VirtType  virt_type;
    static bool      initialized;
    static bool      vmx_on;         // intel: vmx root mode active
    static bool      svm_enabled;    // amd: svm enabled
    static bool      nested;         // running under a hypervisor
    static bool      whpx_detected;  // running under hyper-v / whpx
    static bool      whpx_nested_ok;  // hyper-v actually supports nested virt
    static uint32_t  max_hv_leaf;     // max hypervisor cpuid leaf
    static int       vcpu_count;
    static uint32_t  vmx_revision_id;
    static uint32_t  last_vm_instr_error;
    static uint64_t  last_vm_entry_guest_rip;
    static uint64_t  last_vm_entry_guest_cr0;
    static uint64_t  last_vm_entry_guest_cr4;
    static char      vendor_string[16];
    static char      hv_vendor_string[16]; // hypervisor vendor (leaf 0x40000000)

    static void     DetectHypervisor();    // identify host hypervisor

    static void     WriteCR4(uint64_t val);
};
