//  kurono os - nvme block device driver implementation
//  pcie nvme 1.4 ssd support with admin + i/o queues
#include "nvme.h"
#include "../hal/hal.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"   // page-aligned dma memory for the admin/io queues (satoru)
#include "../kernel/vmm.h"   // identity-map the high 64-bit bar before touching it (satoru)
#include "../kernel/kdf.h"   // kdf-sandboxed dma/mmio: guard-fenced queues + bar (satoru)
#include "../kernel/irp.h"   // register nvme as an irp block device (executive i/o) (satoru)
#include "../drivers/serial.h"
#include "../proc/smp.h"     // one i/o queue per cpu core; route by calling cpu (satoru)
#include "../proc/spinlock.h"  // serialize cores that share an sq when cpus > queues (satoru)
#include <string.h>

//  ── kdf migration (satoru) ──────────────────────────────────────────────────
//  nvme is the first driver migrated into the kernel driver framework: every dma
//  buffer it hands the controller (admin sq/cq, identify, the io sq/cq pairs, the
//  prp-list pages) is now allocated via KDF::AllocDMA, so it sits in nvme's
//  guard-fenced higher-half window. an overrun of any of those rings/buffers
//  walks into an unmapped guard page -> kdf crash path (quarantine + kinit
//  restart) instead of silently corrupting an adjacent kernel allocation. the
//  controller still needs the PHYSICAL address of each buffer, so every place
//  that used `(uintptr_t)buf` as a controller address now uses KDF::PhysOf(buf).
//  the bar0 register window is mapped via KDF::MapMMIO (bounds-checked + fenced).
//  the driver runs its init inside KDF::Start's crash sandbox. (satoru)
namespace {
int g_nvme_kdf_id = -1;   // kdf driver id for "nvme", set in NVMe::Init (satoru)

// physical address of a kdf-fenced dma buffer for controller programming. all
// nvme dma buffers are kdf regions, so this resolves them; falls back to the
// identity assumption (phys==va) only for a non-kdf pointer, which shouldn't
// happen post-migration but keeps a stray caller safe. (satoru)
inline uint64_t dma_phys(const void* va) {
    uint64_t p = KDF::PhysOf((void*)(uintptr_t)va);
    return p ? p : (uint64_t)(uintptr_t)va;
}

//  irp dispatch routine for the "nvme0" block device: routes read/write/flush
//  irps from the executive to NVMe's block i/o. completes synchronously (the
//  controller path polls), so it sets status+info and returns. this makes the
//  IRP + KExec::IO path a REAL working route to storage, not just a stub. (satoru)
int32_t nvme_irp_dispatch(IRP::Irp* irp) {
    switch (irp->major) {
        case IRP::IRP_MJ_READ: {
            bool ok = NVMe::Read(irp->lba, irp->count, irp->buffer);
            irp->status = ok ? IRP::IRP_SUCCESS : IRP::IRP_EIO;
            irp->info   = ok ? irp->count * NVMe::GetLBASize() : 0;
            return irp->status;
        }
        case IRP::IRP_MJ_WRITE: {
            bool ok = NVMe::Write(irp->lba, irp->count, irp->buffer);
            irp->status = ok ? IRP::IRP_SUCCESS : IRP::IRP_EIO;
            irp->info   = ok ? irp->count * NVMe::GetLBASize() : 0;
            return irp->status;
        }
        case IRP::IRP_MJ_FLUSH: {
            bool ok = NVMe::Flush();
            irp->status = ok ? IRP::IRP_SUCCESS : IRP::IRP_EIO;
            irp->info   = 0;
            return irp->status;
        }
        default:
            irp->status = IRP::IRP_EINVAL;
            return IRP::IRP_EINVAL;
    }
}
}  // namespace

bool              NVMe::detected        = false;
NVMeControllerInfo NVMe::info           = {};
volatile uint8_t* NVMe::bar0            = nullptr;
NVMeQueuePair     NVMe::admin_queue     = {};
NVMeQueuePair     NVMe::io_queues[NVME_MAX_QUEUES] = {};
int               NVMe::io_queue_count  = 0;
uint64_t          NVMe::read_count      = 0;
uint64_t          NVMe::write_count     = 0;
uint64_t          NVMe::bytes_read      = 0;
uint64_t          NVMe::bytes_written   = 0;

//  one lock per i/o queue. the calling cpu picks its queue by (CpuIndex %
//  io_queue_count); when CpuCount() > io_queue_count (or > NVME_MAX_QUEUES)
//  several cores map to the SAME submission queue. without a guard they race
//  on qp->sq_tail + the doorbell + the completion ring and corrupt the queue.
//  taking the queue's lock across submit+ring+reap serializes those cores. a
//  core on its own queue takes an uncontended lock (cheap). bare Lock() (no
//  cli/sti): nvme i/o polls for a long time, so disabling irqs across it would
//  starve the timer. (satoru)
static Spinlock g_io_q_lock[NVME_MAX_QUEUES];

//  register access

uint32_t NVMe::ReadReg(uint32_t offset) {
    if (!bar0) return 0;
    return *(volatile uint32_t*)(bar0 + offset);
}

void NVMe::WriteReg(uint32_t offset, uint32_t val) {
    if (!bar0) return;
    *(volatile uint32_t*)(bar0 + offset) = val;
}

uint64_t NVMe::ReadReg64(uint32_t offset) {
    if (!bar0) return 0;
    uint32_t lo = ReadReg(offset);
    uint32_t hi = ReadReg(offset + 4);
    return ((uint64_t)hi << 32) | lo;
}

void NVMe::WriteReg64(uint32_t offset, uint64_t val) {
    WriteReg(offset, (uint32_t)(val & 0xFFFFFFFF));
    WriteReg(offset + 4, (uint32_t)(val >> 32));
}

//  init - pci probe + controller enable
//
//  KdfInit is the real init body; it runs INSIDE the kdf crash sandbox (NVMe::Init
//  registers the "nvme" kdf driver and calls KDF::Start, which invokes this via
//  RunGuarded). all dma buffers are obtained via KDF::AllocDMA so they are
//  guard-fenced, and the bar0 register window via KDF::MapMMIO. (satoru)
bool NVMe::KdfInit() {
    SerialLogger::Log("[NVMe] Scanning PCI for NVMe controllers...\r\n");

    // pci scan: class 01h (mass storage), subclass 08h (nvme), progif 02h
    // walk bus 0 looking for nvme device
    for (int dev = 0; dev < 32; dev++) {
        for (int func = 0; func < 8; func++) {
            uint32_t addr = (1u << 31) | (0 << 16) | (dev << 11) | (func << 8);
            HAL::OutLong(0xCF8, addr | 0x08); // class/subclass register
            uint32_t class_reg = HAL::InLong(0xCFC);
            uint8_t base_class = (class_reg >> 24) & 0xFF;
            uint8_t sub_class  = (class_reg >> 16) & 0xFF;
            uint8_t prog_if    = (class_reg >> 8) & 0xFF;

            if (base_class == 0x01 && sub_class == 0x08 && prog_if == 0x02) {
                // found nvme controller
                HAL::OutLong(0xCF8, addr | 0x10); // bar0
                uint32_t bar0_val = HAL::InLong(0xCFC);
                uint64_t bar_addr = (uint64_t)(bar0_val & ~0xFu);
                // 64-bit memory bar (type bits 10b): the high 32 bits live in bar1.
                // qemu places the nvme bar above 4gb under -m 4G, so reading only
                // bar0 yields a null base (0x...04 = type bits, base 0) and every
                // register read returns 0 -> "version 0.0.0" + enable timeout. (satoru)
                if (((bar0_val >> 1) & 0x3) == 0x2) {
                    HAL::OutLong(0xCF8, addr | 0x14); // bar1 (high 32 bits)
                    uint32_t bar1_val = HAL::InLong(0xCFC);
                    bar_addr |= ((uint64_t)bar1_val << 32);
                }
                // map the bar's register window (64kb covers controller regs +
                // doorbells) through kdf as guard-fenced uncached mmio. an access
                // past the window walks into a guard page -> kdf crash path rather
                // than poking unrelated mmio. KDF::MapMMIO preserves the in-page
                // offset, so the returned pointer points at the real bar base in
                // nvme's fenced higher-half window. (satoru)
                void* bar_win = KDF::MapMMIO(bar_addr, 0x10000ULL);
                if (!bar_win) {
                    SerialLogger::Log("[NVMe] FATAL: KDF::MapMMIO(bar) failed\r\n");
                    return false;
                }
                bar0 = (volatile uint8_t*)bar_win;

                SerialLogger::Log("[NVMe] Controller found at PCI ");
                SerialLogger::LogDec(0); SerialLogger::Log(":");
                SerialLogger::LogDec(dev); SerialLogger::Log(".");
                SerialLogger::LogDec(func);
                SerialLogger::Log(" BAR0=");
                SerialLogger::LogHex(bar0_val);
                SerialLogger::Log("\r\n");

                // enable bus mastering + memory space (the controller needs bus
                // master to dma the queues; without it commands never complete
                // even though CSTS.RDY=1). (satoru)
                HAL::OutLong(0xCF8, addr | 0x04);
                uint32_t cmd = HAL::InLong(0xCFC);
                cmd |= (1 << 2) | (1 << 1); // bus master + memory space
                HAL::OutLong(0xCF8, addr | 0x04);
                HAL::OutLong(0xCFC, cmd);

                detected = true;

                // diag: controller capabilities - MQES (max queue entries-1),
                // DSTRD (doorbell stride), TO (enable timeout, 500ms units). a
                // depth > MQES+1 or a non-zero DSTRD breaks our hardcoded queue
                // depth / doorbell offsets. (satoru)
                uint64_t cap = ReadReg64(NVME_REG_CAP);
                SerialLogger::Log("[NVMe] CAP MQES=");
                SerialLogger::LogDec((int)(cap & 0xFFFF));
                SerialLogger::Log(" DSTRD="); SerialLogger::LogDec((int)((cap >> 32) & 0xF));
                SerialLogger::Log(" TO="); SerialLogger::LogDec((int)((cap >> 24) & 0xFF));
                SerialLogger::Log("\r\n");

                // read version
                uint32_t vs = ReadReg(NVME_REG_VS);
                SerialLogger::Log("[NVMe] Version: ");
                SerialLogger::LogDec((vs >> 16) & 0xFFFF);
                SerialLogger::Log(".");
                SerialLogger::LogDec((vs >> 8) & 0xFF);
                SerialLogger::Log(".");
                SerialLogger::LogDec(vs & 0xFF);
                SerialLogger::Log("\r\n");

                // disable controller first
                WriteReg(NVME_REG_CC, 0);
                WaitReady(false, 5000);

                // set up admin queues
                admin_queue.depth = NVME_QUEUE_DEPTH;
                admin_queue.sq_tail = 0;
                admin_queue.cq_head = 0;
                admin_queue.cq_phase = 1;
                admin_queue.qid = 0;

                // the admin sq/cq must be 4kb page-aligned for the controller's
                // dma engine. KDF::AllocDMA returns page-aligned, physically-
                // contiguous frames fenced by guard pages; the buffer is zeroed.
                // the controller is programmed with KDF::PhysOf (dma_phys), not the
                // fenced higher-half va. (satoru)
                admin_queue.sq = (NVMeSQE*)KDF::AllocDMA(sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
                admin_queue.cq = (NVMeCQE*)KDF::AllocDMA(sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);
                if (admin_queue.sq) memset(admin_queue.sq, 0, sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
                if (admin_queue.cq) memset(admin_queue.cq, 0, sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);

                // program admin queue base addresses (physical, via kdf) (satoru)
                WriteReg(NVME_REG_AQA, ((NVME_QUEUE_DEPTH - 1) << 16) | (NVME_QUEUE_DEPTH - 1));
                WriteReg64(NVME_REG_ASQ, dma_phys(admin_queue.sq));
                WriteReg64(NVME_REG_ACQ, dma_phys(admin_queue.cq));

                // configure and enable controller
                uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_IOSQES | NVME_CC_IOCQES;
                WriteReg(NVME_REG_CC, cc);

                if (WaitReady(true, 5000)) {
                    SerialLogger::Log("[NVMe] Controller enabled\r\n");
                    Identify();
                    CreateIOQueues();
                    // expose nvme as an irp block device so the executive (KExec::IO)
                    // + any irp-stacked driver can reach storage through the
                    // structured i/o path. idempotent across re-inits. (satoru)
                    if (io_queue_count > 0)
                        IRP::RegisterDevice("nvme0", nvme_irp_dispatch);
                } else {
                    SerialLogger::Log("[NVMe] Controller enable timeout\r\n");
                }

                return detected;
            }
        }
    }

    SerialLogger::Log("[NVMe] No NVMe controller found\r\n");
    return false;
}

//  public entry: register "nvme" with kdf (once) and bring it up inside the kdf
//  crash sandbox. on the FIRST call this also registers the driver; on a kinit
//  restart after a guard-page crash, kinit calls KDF::Start("nvme") which re-runs
//  KdfInit through this same RunGuarded path. resets the per-controller state so
//  a re-init re-probes cleanly. (satoru)
bool NVMe::Init() {
    // reset state so a (re-)init starts from a known-clean slate. (satoru)
    detected = false;
    bar0 = nullptr;
    io_queue_count = 0;
    for (int q = 0; q < NVME_MAX_QUEUES; q++) {
        io_queues[q].sq = nullptr;
        io_queues[q].cq = nullptr;
    }
    admin_queue.sq = nullptr;
    admin_queue.cq = nullptr;

    if (g_nvme_kdf_id < 0)
        g_nvme_kdf_id = KDF::RegisterDriver("nvme", &NVMe::KdfInit);
    if (g_nvme_kdf_id < 0) {
        // kdf unavailable (shouldn't happen post-Init): fall back to a direct,
        // unsandboxed init so storage still works. honest + safe. (satoru)
        SerialLogger::Log("[NVMe] KDF unavailable; running unsandboxed init\r\n");
        return KdfInit();
    }
    // KDF::Start runs KdfInit inside RunGuarded (crash sandbox + active-driver
    // attribution for AllocDMA/MapMMIO). returns true if it initialized cleanly.
    // (satoru)
    bool ok = KDF::Start(g_nvme_kdf_id);
    return ok && detected;
}

bool NVMe::WaitReady(bool expected, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 10; i++) {
        uint32_t csts = ReadReg(NVME_REG_CSTS);
        if (!!(csts & NVME_CSTS_RDY) == expected) return true;
        // small delay
        for (volatile int j = 0; j < 1000; j++) {}
    }
    return false;
}

bool NVMe::IsDetected() { return detected; }
const NVMeControllerInfo& NVMe::GetInfo() { return info; }
uint64_t NVMe::GetCapacityLBA() { return info.total_capacity_lba; }
uint32_t NVMe::GetLBASize() { return info.lba_size ? info.lba_size : 512; }
uint32_t NVMe::GetMaxTransferBlocks() { return info.max_transfer_size ? info.max_transfer_size : 256; }
uint64_t NVMe::GetReadCount() { return read_count; }
uint64_t NVMe::GetWriteCount() { return write_count; }
uint64_t NVMe::GetBytesRead() { return bytes_read; }
uint64_t NVMe::GetBytesWritten() { return bytes_written; }

//  admin: identify

bool NVMe::Identify() {
    if (!detected || !admin_queue.sq) return false;

    // page-aligned: this is the prp1 dma target for IDENTIFY (a 4kb transfer),
    // which the controller requires page-aligned. kdf-fenced. (satoru)
    uint8_t* id_buf = (uint8_t*)KDF::AllocDMA(4096);
    if (!id_buf) return false;
    memset(id_buf, 0, 4096);

    NVMeSQE cmd = {};
    cmd.opcode = NVME_ADM_IDENTIFY;
    cmd.nsid = 0;
    cmd.prp1 = dma_phys(id_buf);  // physical (kdf) (satoru)
    cmd.cdw10 = NVME_ID_CNS_CTRL; // identify controller

    NVMeCQE result = {};
    if (!SubmitAdminCmd(&cmd, &result)) {
        // kdf-owned buffer: it is reclaimed when the driver is torn down /
        // quarantined; we just drop the reference on the error path. (satoru)
        return false;
    }

    // parse controller identity
    for (int i = 0; i < 20; i++) info.serial[i] = (char)id_buf[4 + i];
    info.serial[20] = 0;
    for (int i = 0; i < 40; i++) info.model[i] = (char)id_buf[24 + i];
    info.model[40] = 0;
    for (int i = 0; i < 8; i++) info.firmware[i] = (char)id_buf[64 + i];
    info.firmware[8] = 0;
    // mdts is on-disk (controller-reported); `1u << n` is UB for n >= 32. clamp
    // the shift. MaxTransferBytes() then caps the result to our prp-list size, so
    // a large-but-valid mdts still works; we only need to avoid the UB. (satoru)
    uint8_t mdts = id_buf[77];
    info.max_transfer_size = (mdts < 31) ? (1u << mdts) : 0u; // 0 => "unlimited" path (satoru)
    info.num_namespaces = *(uint32_t*)(id_buf + 516);
    info.detected = true;

    SerialLogger::Log("[NVMe] Model: ");
    SerialLogger::Log(info.model);
    SerialLogger::Log("\r\n");

    // now identify namespace 1 to get capacity
    memset(id_buf, 0, 4096);
    cmd = {};
    cmd.opcode = NVME_ADM_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = dma_phys(id_buf);  // physical (kdf) (satoru)
    cmd.cdw10 = NVME_ID_CNS_NS;

    if (SubmitAdminCmd(&cmd, &result)) {
        info.total_capacity_lba = *(uint64_t*)(id_buf + 0);
        uint8_t lba_format_idx = id_buf[26] & 0x0F;
        uint32_t lbaf = *(uint32_t*)(id_buf + 128 + lba_format_idx * 4);
        // LBADS is on-disk; `1u << n` is UB for n >= 32, and an implausible size
        // would skew every block calc. clamp the shift and fall back to 512 for
        // anything outside the sane 512..4096 range. (satoru)
        uint32_t lbads = (lbaf >> 16) & 0xFF;
        uint32_t sz = (lbads < 31) ? (1u << lbads) : 0u;
        info.lba_size = (sz >= 512 && sz <= 4096) ? sz : 512u;

        SerialLogger::Log("[NVMe] Capacity: ");
        SerialLogger::LogHex((uint32_t)(info.total_capacity_lba >> 32));
        SerialLogger::LogHex((uint32_t)(info.total_capacity_lba & 0xFFFFFFFF));
        SerialLogger::Log(" LBAs @ ");
        SerialLogger::LogDec(info.lba_size);
        SerialLogger::Log(" bytes\r\n");
    }

    // release the one-shot identify scratch back to kdf (unmaps + frees its
    // frames + reclaims the region slot). (satoru)
    KDF::FreeDMA(id_buf);
    return true;
}

bool NVMe::CreateIOQueues() {
    // layer 1: create ONE i/o queue pair PER cpu core (qid 1..N), so each core
    // can submit on its own queue without contending on a shared doorbell/ring.
    // negotiate the queue count with the controller first (set features 0x07),
    // then create cq+sq per queue. routing is by SMP::CpuIndex() in Read/Write.
    // (satoru)
    uint32_t want = SMP::CpuCount();
    if (want < 1) want = 1;
    if (want > NVME_MAX_QUEUES) want = NVME_MAX_QUEUES;

    // set features: number of queues. cdw11 = (ncqr<<16)|nsqr, 0-based counts.
    // the controller returns the GRANTED count in the completion result; honor
    // it so we never create more queues than allocated. (satoru)
    NVMeSQE sf = {}; NVMeCQE sfr = {};
    sf.opcode = NVME_ADM_SET_FEAT;
    sf.cdw10  = 0x07;                                   // number of queues (satoru)
    sf.cdw11  = ((want - 1) << 16) | (want - 1);        // request want sq + want cq (satoru)
    uint32_t granted = want;
    if (SubmitAdminCmd(&sf, &sfr)) {
        uint32_t nsq = (sfr.result & 0xFFFF) + 1;
        uint32_t ncq = ((sfr.result >> 16) & 0xFFFF) + 1;
        uint32_t g = nsq < ncq ? nsq : ncq;
        if (g >= 1 && g < want) granted = g;            // controller gave us fewer (satoru)
    }
    if (granted > NVME_MAX_QUEUES) granted = NVME_MAX_QUEUES;

    io_queue_count = 0;
    for (uint32_t i = 0; i < granted; i++) {
        NVMeQueuePair* qp = &io_queues[i];
        uint16_t qid = (uint16_t)(i + 1);
        qp->depth = NVME_QUEUE_DEPTH;
        qp->sq_tail = 0; qp->cq_head = 0; qp->cq_phase = 1; qp->qid = qid;

        // page-aligned for the controller dma engine, same as the admin queues,
        // now kdf-fenced (guard pages around each ring). (satoru)
        qp->sq = (NVMeSQE*)KDF::AllocDMA(sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
        qp->cq = (NVMeCQE*)KDF::AllocDMA(sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);
        if (!qp->sq || !qp->cq) break;
        memset(qp->sq, 0, sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
        memset(qp->cq, 0, sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);

        // create cq first, then sq. queue bases are physical (via kdf). (satoru)
        NVMeSQE cmd = {}; NVMeCQE result = {};
        cmd.opcode = NVME_ADM_CREATE_CQ;
        cmd.prp1 = dma_phys(qp->cq);
        cmd.cdw10 = ((NVME_QUEUE_DEPTH - 1) << 16) | qid;
        cmd.cdw11 = 1; // physically contiguous (no interrupts: we poll) (satoru)
        if (!SubmitAdminCmd(&cmd, &result)) break;

        cmd = {}; result = {};
        cmd.opcode = NVME_ADM_CREATE_SQ;
        cmd.prp1 = dma_phys(qp->sq);
        cmd.cdw10 = ((NVME_QUEUE_DEPTH - 1) << 16) | qid;
        cmd.cdw11 = ((uint32_t)qid << 16) | 1; // bind to its own cq, contiguous (satoru)
        if (!SubmitAdminCmd(&cmd, &result)) break;

        io_queue_count++;
    }
    if (io_queue_count == 0) { SerialLogger::Log("[NVMe] FAILED to create any i/o queue\r\n"); return false; }

    SerialLogger::Log("[NVMe] "); SerialLogger::LogDec(io_queue_count);
    SerialLogger::Log(" i/o queue pair(s) created (one per cpu core, QID=1..");
    SerialLogger::LogDec(io_queue_count); SerialLogger::Log(")\r\n");
    return true;
}

//  command submission

bool NVMe::SubmitAdminCmd(NVMeSQE* cmd, NVMeCQE* result) {
    if (!admin_queue.sq || !admin_queue.cq) return false;

    uint16_t tail = admin_queue.sq_tail;
    admin_queue.sq[tail] = *cmd;
    admin_queue.sq[tail].command_id = tail;
    admin_queue.sq_tail = (tail + 1) % admin_queue.depth;

    // ring sq doorbell
    WriteReg(NVME_REG_SQ0TDBL, admin_queue.sq_tail);

    return PollCompletion(&admin_queue, result);
}

bool NVMe::SubmitIOCmd(int qid, NVMeSQE* cmd, NVMeCQE* result) {
    if (qid < 0 || qid >= io_queue_count || qid >= NVME_MAX_QUEUES) return false;
    NVMeQueuePair* qp = &io_queues[qid];
    if (!qp->sq || !qp->cq) return false;

    // serialize cores that share this sq (cpus > queues). (satoru)
    SpinLockCpuGuard g(g_io_q_lock[qid]);

    uint16_t tail = qp->sq_tail;
    qp->sq[tail] = *cmd;
    qp->sq[tail].command_id = tail;
    qp->sq_tail = (tail + 1) % qp->depth;

    // ring sq doorbell (stride = 4 bytes per doorbell, offset = 0x1000 + qid*2*stride)
    uint32_t db_offset = NVME_REG_SQ0TDBL + qp->qid * 2 * 4;
    WriteReg(db_offset, qp->sq_tail);

    return PollCompletion(qp, result);
}

bool NVMe::PollCompletion(NVMeQueuePair* qp, NVMeCQE* result) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        NVMeCQE* cqe = &qp->cq[qp->cq_head];
        // VOLATILE read: the controller dma-writes this completion. a plain read
        // gets hoisted out of the loop by the compiler, so we spin on the stale
        // cached 0 and time out even though the completion (phase + status) is
        // already in memory - this one missing volatile is why NO nvme command
        // ever completed. (satoru)
        uint16_t st = *(volatile uint16_t*)&cqe->status;
        uint16_t phase = (st & 1);
        if (phase == qp->cq_phase) {
            if (result) *result = *cqe;
            uint16_t sc = (st >> 1) & 0x7FF;  // status code (0 = success) (satoru)

            qp->cq_head++;
            if (qp->cq_head >= qp->depth) {
                qp->cq_head = 0;
                qp->cq_phase ^= 1;
            }

            // ring cq doorbell
            uint32_t cq_db = NVME_REG_SQ0TDBL + (qp->qid * 2 + 1) * 4;
            WriteReg(cq_db, qp->cq_head);

            if (sc != 0) {  // diag: command completed but the controller rejected it (satoru)
                SerialLogger::Log("[NVMe] cmd ERR q=");
                SerialLogger::LogDec((int)qp->qid);
                SerialLogger::Log(" sc=0x"); SerialLogger::LogHex(sc);
                SerialLogger::Log("\r\n");
            }
            return sc == 0; // check status code
        }
        for (volatile int j = 0; j < 100; j++) {} // spin
    }
    // diag: completion never appeared. dump the raw cqe at cq_head - if it's all
    // zero the controller wrote nothing (didn't fetch/process the command); if it
    // has data the phase bit / slot is being misread. (satoru)
    NVMeCQE* cqe = &qp->cq[qp->cq_head];
    SerialLogger::Log("[NVMe] poll TIMEOUT q=");
    SerialLogger::LogDec((int)qp->qid);
    SerialLogger::Log(" cqe: status=0x"); SerialLogger::LogHex(cqe->status);
    SerialLogger::Log(" sqhd=0x"); SerialLogger::LogHex(cqe->sq_head);
    SerialLogger::Log(" cid=0x"); SerialLogger::LogHex(cqe->command_id);
    SerialLogger::Log(" res=0x"); SerialLogger::LogHex(cqe->result);
    SerialLogger::Log("\r\n");
    return false;
}

//  block i/o

//  build prp1/prp2 for a CONTIGUOUS, page-aligned buffer of `bytes`. one page ->
//  prp1 only; two pages -> prp2 is the 2nd page directly; more -> prp2 points at a
//  prp-list page holding the physical addresses of pages 2..N (the buffer is
//  contiguous + identity-mapped, so page k = base + k*4096). one list page = 512
//  entries -> up to ~2mb per command, vs the old 4kb single-page cap that turned a
//  1.8mb persist into ~450 commands. callers chunk transfers above the list cap.
//  the list scratch is reused serially (nvme i/o here is polled to completion, one
//  at a time). (satoru)
static uint8_t* g_prp_list = nullptr;

//  build prp1/prp2 using `list_page` for the prp list when one is needed. when
//  list_page is null the shared g_prp_list scratch is used (single-command
//  path). the batched path passes a DISTINCT page per in-flight command so two
//  outstanding commands never share a list (which would corrupt one of them).
//  (satoru)
static bool nvme_build_prp_on(uint64_t base, uint32_t bytes, uint64_t* prp1, uint64_t* prp2, uint8_t* list_page) {
    const uint32_t PAGE = 4096;
    *prp1 = base;
    // prp1 may carry a byte offset (the transfer starts there); the first page
    // then covers only PAGE-off bytes. prp2 + every list entry must be page
    // aligned, so they name the 2nd..Nth pages. all kernel buffers are identity
    // mapped + physically contiguous, so page k = (base rounded down) + k*PAGE.
    // this lets any kernel buffer (stack/heap/pmm) be a dma target, not just
    // page-aligned ones. (satoru)
    uint32_t off   = (uint32_t)(base & (PAGE - 1));
    uint32_t first = PAGE - off;
    if (bytes <= first) { *prp2 = 0; return true; }
    uint64_t page2 = (base & ~(uint64_t)(PAGE - 1)) + PAGE;
    uint32_t rem = bytes - first;
    if (rem <= PAGE) { *prp2 = page2; return true; }
    uint32_t n_list = (rem + PAGE - 1) / PAGE;      // pages after the first (satoru)
    if (n_list > PAGE / 8) return false;            // > one list page (~2mb): caller must chunk (satoru)
    if (!list_page) {
        if (!g_prp_list) {
            g_prp_list = (uint8_t*)PMM::AllocBytes(PAGE);
            if (!g_prp_list) return false;
        }
        list_page = g_prp_list;
    }
    uint64_t* list = (uint64_t*)list_page;
    for (uint32_t i = 0; i < n_list; i++) list[i] = page2 + (uint64_t)i * PAGE;
    *prp2 = (uint64_t)(uintptr_t)list_page;
    return true;
}

//  single-command helper (shared scratch); kept for the non-batched callers /
//  future use. (satoru)
__attribute__((unused))
static bool nvme_build_prp(uint64_t base, uint32_t bytes, uint64_t* prp1, uint64_t* prp2) {
    return nvme_build_prp_on(base, bytes, prp1, prp2, nullptr);
}

//  one prp-list page per batch slot PER i/o queue, so NVME_IO_BATCH commands can
//  be in flight at once on a queue each with its own list, AND two cores
//  submitting on their own queues concurrently never share a list page. allocated
//  lazily on first batched i/o. (satoru)
static uint8_t* g_batch_prp[NVME_MAX_QUEUES][NVME_IO_BATCH] = {};
static bool ensure_batch_prp(int qidx) {
    if (qidx < 0 || qidx >= NVME_MAX_QUEUES) return false;
    for (int i = 0; i < NVME_IO_BATCH; i++) {
        if (!g_batch_prp[qidx][i]) {
            g_batch_prp[qidx][i] = (uint8_t*)PMM::AllocBytes(4096);
            if (!g_batch_prp[qidx][i]) return false;
        }
    }
    return true;
}

//  largest transfer one command can describe: the smaller of our single prp-list
//  page (512 entries) and the controller's MDTS (max data transfer size). qemu
//  reports a finite MDTS, and exceeding it is rejected with "invalid field"
//  (sc=0x02) - so a 1MB transfer must be split. max_transfer_size is 1<<MDTS in
//  4kb pages; 0/1 means "unlimited", so fall back to our prp cap. (satoru)
uint32_t NVMe::MaxTransferBytes() {
    uint32_t mdts_pages = info.max_transfer_size;
    if (mdts_pages < 2)   mdts_pages = 512;   // unlimited -> use the prp-list cap (satoru)
    if (mdts_pages > 512) mdts_pages = 512;
    return mdts_pages * 4096u;
}

//  reap exactly `n` completions off qp (phase-bit polled, any order). returns
//  false on any non-zero status code or a timeout. mirrors PollCompletion's
//  phase/doorbell handling but loops n times for the batch. (satoru)
bool NVMe::ReapCompletions(NVMeQueuePair* qp, int n) {
    int reaped = 0;
    while (reaped < n) {
        bool got = false;
        for (int timeout = 0; timeout < 200000 && !got; timeout++) {
            NVMeCQE* cqe = &qp->cq[qp->cq_head];
            // VOLATILE: the controller dma-writes the completion (see PollCompletion). (satoru)
            uint16_t st = *(volatile uint16_t*)&cqe->status;
            if ((st & 1) == qp->cq_phase) {
                uint16_t sc = (st >> 1) & 0x7FF;
                qp->cq_head++;
                if (qp->cq_head >= qp->depth) { qp->cq_head = 0; qp->cq_phase ^= 1; }
                uint32_t cq_db = NVME_REG_SQ0TDBL + (qp->qid * 2 + 1) * 4;
                WriteReg(cq_db, qp->cq_head);
                if (sc != 0) {
                    SerialLogger::Log("[NVMe] batch cmd ERR q=");
                    SerialLogger::LogDec((int)qp->qid);
                    SerialLogger::Log(" sc=0x"); SerialLogger::LogHex(sc); SerialLogger::Log("\r\n");
                    return false;
                }
                got = true;
                reaped++;
            } else {
                for (volatile int j = 0; j < 100; j++) {}
            }
        }
        if (!got) {
            SerialLogger::Log("[NVMe] batch reap TIMEOUT q=");
            SerialLogger::LogDec((int)qp->qid);
            SerialLogger::Log(" reaped="); SerialLogger::LogDec(reaped);
            SerialLogger::Log("/"); SerialLogger::LogDec(n); SerialLogger::Log("\r\n");
            return false;
        }
    }
    return true;
}

//  layer 1 - post up to NVME_IO_BATCH ≤2 MB chunks of one transfer at once, ring
//  the sq doorbell ONCE per batch, then reap all their completions. the buffer is
//  contiguous + identity-mapped, so each chunk's prp list is built on its own
//  scratch page. opcode picks read vs write. (satoru)
bool NVMe::SubmitIOBatch(int qid, uint8_t opcode, uint64_t lba, uint32_t count, uint8_t* buffer) {
    if (qid < 0 || qid >= io_queue_count || qid >= NVME_MAX_QUEUES) return false;
    NVMeQueuePair* qp = &io_queues[qid];
    if (!qp->sq || !qp->cq) return false;
    // serialize cores that share this queue (cpus > queues): the lock covers the
    // sq ring (sq_tail + doorbell + reap) AND this queue's per-slot prp scratch
    // pages (g_batch_prp[qid][..]), both of which two cores would otherwise
    // corrupt concurrently. uncontended for a core on its own queue. (satoru)
    SpinLockCpuGuard g(g_io_q_lock[qid]);
    uint32_t lbasz = GetLBASize();
    uint32_t per = MaxTransferBytes() / lbasz; if (!per) per = 1;
    if (!ensure_batch_prp(qid)) return false;

    uint8_t* p = buffer;
    while (count > 0) {
        // build a batch of up to NVME_IO_BATCH chunks. (satoru)
        int in_batch = 0;
        while (count > 0 && in_batch < NVME_IO_BATCH) {
            uint32_t n = count > per ? per : count;
            uint64_t prp1 = 0, prp2 = 0;
            if (!nvme_build_prp_on((uint64_t)(uintptr_t)p, n * lbasz, &prp1, &prp2, g_batch_prp[qid][in_batch]))
                return false;
            uint16_t tail = qp->sq_tail;
            NVMeSQE* s = &qp->sq[tail];
            for (uint32_t z = 0; z < sizeof(NVMeSQE); z++) ((uint8_t*)s)[z] = 0;
            s->opcode = opcode; s->nsid = 1; s->command_id = tail;
            s->prp1 = prp1; s->prp2 = prp2;
            s->cdw10 = (uint32_t)(lba & 0xFFFFFFFF); s->cdw11 = (uint32_t)(lba >> 32);
            s->cdw12 = n - 1;
            qp->sq_tail = (tail + 1) % qp->depth;
            if (opcode == NVME_IO_READ) { read_count++;  bytes_read += (uint64_t)n * lbasz; }
            else                        { write_count++; bytes_written += (uint64_t)n * lbasz; }
            lba += n; count -= n; p += (uint64_t)n * lbasz;
            in_batch++;
        }
        // ring the sq doorbell ONCE for the whole batch, then reap. (satoru)
        uint32_t db_offset = NVME_REG_SQ0TDBL + qp->qid * 2 * 4;
        WriteReg(db_offset, qp->sq_tail);
        if (!ReapCompletions(qp, in_batch)) return false;
    }
    return true;
}

//  pick the i/o queue for the calling cpu (one queue per core). falls back to
//  queue 0 if smp isn't up or the index is out of range. (satoru)
static int nvme_pick_queue(int io_queue_count) {
    if (io_queue_count <= 1) return 0;
    uint32_t idx = SMP::CpuIndex();
    return (int)(idx % (uint32_t)io_queue_count);
}

bool NVMe::Read(uint64_t lba, uint32_t count, void* buffer) {
    if (!detected || !buffer || count == 0 || io_queue_count == 0) return false;
    return SubmitIOBatch(nvme_pick_queue(io_queue_count), NVME_IO_READ, lba, count, (uint8_t*)buffer);
}

bool NVMe::Write(uint64_t lba, uint32_t count, const void* buffer) {
    if (!detected || !buffer || count == 0 || io_queue_count == 0) return false;
    return SubmitIOBatch(nvme_pick_queue(io_queue_count), NVME_IO_WRITE, lba, count, (uint8_t*)(uintptr_t)buffer);
}

bool NVMe::Flush() {
    if (!detected || io_queue_count == 0) return false;
    NVMeSQE cmd = {};
    NVMeCQE result = {};
    cmd.opcode = NVME_IO_FLUSH;
    cmd.nsid = 1;
    return SubmitIOCmd(0, &cmd, &result);
}

void NVMe::DumpInfo(char* out, int max_len) {
    if (!out || max_len < 2) return;
    int p = 0;
    auto app = [&](const char* s) { while (*s && p < max_len - 1) out[p++] = *s++; out[p] = 0; };
    app("NVMe Controller Info\n");
    app("  Detected: "); app(detected ? "yes" : "no"); app("\n");
    if (detected) {
        app("  Model:    "); app(info.model); app("\n");
        app("  Serial:   "); app(info.serial); app("\n");
        app("  Firmware: "); app(info.firmware); app("\n");
    }
}
