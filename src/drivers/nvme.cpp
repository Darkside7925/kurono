//  kurono os  -  nvme block device driver implementation
//  pcie nvme 1.4 ssd support with admin + i/o queues
#include "nvme.h"
#include "../hal/hal.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"   // page-aligned dma memory for the admin/io queues (satoru)
#include "../kernel/vmm.h"   // identity-map the high 64-bit bar before touching it (satoru)
#include "../drivers/serial.h"
#include <string.h>

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

//  init  -  pci probe + controller enable

bool NVMe::Init() {
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
                // the bar may sit above the boot identity map -> identity-map its
                // register window (64kb covers controller regs + doorbells) as
                // uncached mmio before any dereference, or ReadReg #pfs. (satoru)
                for (uint64_t p = bar_addr & ~0xFFFULL;
                     p < (bar_addr & ~0xFFFULL) + 0x10000ULL; p += 0x1000ULL) {
                    KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
                }
                bar0 = (volatile uint8_t*)(uintptr_t)bar_addr;

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

                // diag: controller capabilities  -  MQES (max queue entries-1),
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
                // dma engine; KernelHeap is only 16-byte aligned, so the queues
                // straddle pages and the controller never reaches RDY. PMM hands
                // out page-aligned, identity-mapped (virt==phys) frames. (satoru)
                admin_queue.sq = (NVMeSQE*)PMM::AllocBytes(sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
                admin_queue.cq = (NVMeCQE*)PMM::AllocBytes(sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);
                if (admin_queue.sq) memset(admin_queue.sq, 0, sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
                if (admin_queue.cq) memset(admin_queue.cq, 0, sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);

                // program admin queue base addresses
                WriteReg(NVME_REG_AQA, ((NVME_QUEUE_DEPTH - 1) << 16) | (NVME_QUEUE_DEPTH - 1));
                WriteReg64(NVME_REG_ASQ, (uint64_t)(uintptr_t)admin_queue.sq);
                WriteReg64(NVME_REG_ACQ, (uint64_t)(uintptr_t)admin_queue.cq);

                // configure and enable controller
                uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_IOSQES | NVME_CC_IOCQES;
                WriteReg(NVME_REG_CC, cc);

                if (WaitReady(true, 5000)) {
                    SerialLogger::Log("[NVMe] Controller enabled\r\n");
                    Identify();
                    CreateIOQueues();
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
    // which the controller requires page-aligned. (satoru)
    uint8_t* id_buf = (uint8_t*)PMM::AllocBytes(4096);
    if (!id_buf) return false;
    memset(id_buf, 0, 4096);

    NVMeSQE cmd = {};
    cmd.opcode = NVME_ADM_IDENTIFY;
    cmd.nsid = 0;
    cmd.prp1 = (uint64_t)(uintptr_t)id_buf;
    cmd.cdw10 = NVME_ID_CNS_CTRL; // identify controller

    NVMeCQE result = {};
    if (!SubmitAdminCmd(&cmd, &result)) {
        PMM::FreeBytes(id_buf, 4096);
        return false;
    }

    // parse controller identity
    for (int i = 0; i < 20; i++) info.serial[i] = (char)id_buf[4 + i];
    info.serial[20] = 0;
    for (int i = 0; i < 40; i++) info.model[i] = (char)id_buf[24 + i];
    info.model[40] = 0;
    for (int i = 0; i < 8; i++) info.firmware[i] = (char)id_buf[64 + i];
    info.firmware[8] = 0;
    info.max_transfer_size = 1u << id_buf[77]; // mdts
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
    cmd.prp1 = (uint64_t)(uintptr_t)id_buf;
    cmd.cdw10 = NVME_ID_CNS_NS;

    if (SubmitAdminCmd(&cmd, &result)) {
        info.total_capacity_lba = *(uint64_t*)(id_buf + 0);
        uint8_t lba_format_idx = id_buf[26] & 0x0F;
        uint32_t lbaf = *(uint32_t*)(id_buf + 128 + lba_format_idx * 4);
        info.lba_size = 1u << ((lbaf >> 16) & 0xFF);

        SerialLogger::Log("[NVMe] Capacity: ");
        SerialLogger::LogHex((uint32_t)(info.total_capacity_lba >> 32));
        SerialLogger::LogHex((uint32_t)(info.total_capacity_lba & 0xFFFFFFFF));
        SerialLogger::Log(" LBAs @ ");
        SerialLogger::LogDec(info.lba_size);
        SerialLogger::Log(" bytes\r\n");
    }

    PMM::FreeBytes(id_buf, 4096);
    return true;
}

bool NVMe::CreateIOQueues() {
    // simplified: create one i/o queue pair (qid=1)
    io_queue_count = 1;
    NVMeQueuePair* qp = &io_queues[0];
    qp->depth = NVME_QUEUE_DEPTH;
    qp->sq_tail = 0;
    qp->cq_head = 0;
    qp->cq_phase = 1;
    qp->qid = 1;

    // page-aligned for the controller dma engine, same as the admin queues. (satoru)
    qp->sq = (NVMeSQE*)PMM::AllocBytes(sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
    qp->cq = (NVMeCQE*)PMM::AllocBytes(sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);
    if (qp->sq) memset(qp->sq, 0, sizeof(NVMeSQE) * NVME_QUEUE_DEPTH);
    if (qp->cq) memset(qp->cq, 0, sizeof(NVMeCQE) * NVME_QUEUE_DEPTH);

    // create cq first, then sq
    NVMeSQE cmd = {};
    NVMeCQE result = {};

    cmd.opcode = NVME_ADM_CREATE_CQ;
    cmd.prp1 = (uint64_t)(uintptr_t)qp->cq;
    cmd.cdw10 = ((NVME_QUEUE_DEPTH - 1) << 16) | 1; // qid=1
    cmd.cdw11 = 1; // physically contiguous
    SubmitAdminCmd(&cmd, &result);

    cmd = {};
    cmd.opcode = NVME_ADM_CREATE_SQ;
    cmd.prp1 = (uint64_t)(uintptr_t)qp->sq;
    cmd.cdw10 = ((NVME_QUEUE_DEPTH - 1) << 16) | 1; // qid=1
    cmd.cdw11 = (1 << 16) | 1; // cq=1, physically contiguous
    SubmitAdminCmd(&cmd, &result);

    SerialLogger::Log("[NVMe] I/O queue pair created (QID=1)\r\n");
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
    if (qid < 0 || qid >= io_queue_count) return false;
    NVMeQueuePair* qp = &io_queues[qid];
    if (!qp->sq || !qp->cq) return false;

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
        // already in memory  -  this one missing volatile is why NO nvme command
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
    // diag: completion never appeared. dump the raw cqe at cq_head  -  if it's all
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

bool NVMe::Read(uint64_t lba, uint32_t count, void* buffer) {
    if (!detected || !buffer || count == 0 || io_queue_count == 0) return false;

    NVMeSQE cmd = {};
    NVMeCQE result = {};
    cmd.opcode = NVME_IO_READ;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)(uintptr_t)buffer;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1; // 0-based

    bool ok = SubmitIOCmd(0, &cmd, &result);
    if (ok) {
        read_count++;
        bytes_read += (uint64_t)count * GetLBASize();
    }
    return ok;
}

bool NVMe::Write(uint64_t lba, uint32_t count, const void* buffer) {
    if (!detected || !buffer || count == 0 || io_queue_count == 0) return false;

    NVMeSQE cmd = {};
    NVMeCQE result = {};
    cmd.opcode = NVME_IO_WRITE;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)(uintptr_t)buffer;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;

    bool ok = SubmitIOCmd(0, &cmd, &result);
    if (ok) {
        write_count++;
        bytes_written += (uint64_t)count * GetLBASize();
    }
    return ok;
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
