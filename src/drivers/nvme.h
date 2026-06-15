#pragma once
//  kurono os  -  nvme (nvm express) block device driver
//  supports pcie nvme 1.4 ssds with admin + i/o queue pairs
#include "../kernel/types.h"

#define NVME_REG_CAP        0x0000  // controller capabilities
#define NVME_REG_VS         0x0008  // version
#define NVME_REG_INTMS      0x000C  // interrupt mask set
#define NVME_REG_INTMC      0x0010  // interrupt mask clear
#define NVME_REG_CC         0x0014  // controller configuration
#define NVME_REG_CSTS       0x001C  // controller status
#define NVME_REG_NSSR       0x0020  // nvm subsystem reset
#define NVME_REG_AQA        0x0024  // admin queue attributes
#define NVME_REG_ASQ        0x0028  // admin submission queue base
#define NVME_REG_ACQ        0x0030  // admin completion queue base
#define NVME_REG_SQ0TDBL    0x1000  // sq0 tail doorbell

#define NVME_CC_EN          (1 << 0)
#define NVME_CC_CSS_NVM     (0 << 4)
#define NVME_CC_MPS_SHIFT   7
#define NVME_CC_IOSQES      (6 << 16)  // 64-byte sq entries
#define NVME_CC_IOCQES      (4 << 20)  // 16-byte cq entries

#define NVME_CSTS_RDY       (1 << 0)
#define NVME_CSTS_CFS       (1 << 1)
#define NVME_CSTS_SHST_MASK (3 << 2)

#define NVME_ADM_DELETE_SQ   0x00
#define NVME_ADM_CREATE_SQ   0x01
#define NVME_ADM_DELETE_CQ   0x04
#define NVME_ADM_CREATE_CQ   0x05
#define NVME_ADM_IDENTIFY    0x06
#define NVME_ADM_SET_FEAT    0x09
#define NVME_ADM_GET_FEAT    0x0A

#define NVME_IO_FLUSH        0x00
#define NVME_IO_WRITE        0x01
#define NVME_IO_READ         0x02

#define NVME_ID_CNS_NS       0x00
#define NVME_ID_CNS_CTRL     0x01

#define NVME_MAX_QUEUES      4
#define NVME_QUEUE_DEPTH     64
//  layer 1: how many i/o commands a batched Read/Write posts before ringing the
//  doorbell + reaping. kept < QUEUE_DEPTH so the ring never wraps past unreaped
//  completions. 32 saturates the qemu nvme model's pipelining. (satoru)
#define NVME_IO_BATCH        32

struct NVMeSQE {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct NVMeCQE {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} __attribute__((packed));

struct NVMeQueuePair {
    NVMeSQE* sq;
    NVMeCQE* cq;
    volatile uint32_t* sq_doorbell;
    volatile uint32_t* cq_doorbell;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t depth;
    uint16_t cq_phase;
    uint16_t qid;
};

struct NVMeControllerInfo {
    char     serial[21];
    char     model[41];
    char     firmware[9];
    uint32_t max_transfer_size;
    uint32_t num_namespaces;
    uint64_t total_capacity_lba;
    uint32_t lba_size;
    bool     detected;
};

class NVMe {
public:
    // public entry: registers the "nvme" kdf driver (once) and brings it up
    // inside the kdf crash sandbox. (satoru)
    static bool Init();
    // the real init body, run by kdf via KDF::Start (RunGuarded). all dma buffers
    // are kdf-fenced; the bar0 window is kdf mmio. also the re-init entry kinit
    // fires after a guard-page crash. (satoru)
    static bool KdfInit();
    static bool IsDetected();
    static const NVMeControllerInfo& GetInfo();

    // block i/o
    static bool Read(uint64_t lba, uint32_t count, void* buffer);
    static bool Write(uint64_t lba, uint32_t count, const void* buffer);
    static bool Flush();

    static uint64_t GetCapacityLBA();
    static uint32_t GetLBASize();
    static uint32_t GetMaxTransferBlocks();
    // max bytes one Read/Write command can move (one prp-list page, ~2mb). (satoru)
    static uint32_t MaxTransferBytes();

    // admin operations
    static bool Identify();
    static bool CreateIOQueues();

    // statistics
    static uint64_t GetReadCount();
    static uint64_t GetWriteCount();
    static uint64_t GetBytesRead();
    static uint64_t GetBytesWritten();

    static void DumpInfo(char* out, int max_len);

private:
    static bool detected;
    static NVMeControllerInfo info;
    static volatile uint8_t* bar0;
    static NVMeQueuePair admin_queue;
    static NVMeQueuePair io_queues[NVME_MAX_QUEUES];
    static int io_queue_count;

    static uint64_t read_count, write_count;
    static uint64_t bytes_read, bytes_written;

    static uint32_t ReadReg(uint32_t offset);
    static void WriteReg(uint32_t offset, uint32_t val);
    static uint64_t ReadReg64(uint32_t offset);
    static void WriteReg64(uint32_t offset, uint64_t val);

    static bool WaitReady(bool expected, int timeout_ms);
    static bool SubmitAdminCmd(NVMeSQE* cmd, NVMeCQE* result);
    static bool SubmitIOCmd(int qid, NVMeSQE* cmd, NVMeCQE* result);
    static void RingDoorbell(NVMeQueuePair* qp);
    static bool PollCompletion(NVMeQueuePair* qp, NVMeCQE* result);

    // layer 1  -  queue-depth batching. a single Read/Write that spans many ≤2 MB
    // chunks posts up to NVME_IO_BATCH commands at once, rings the sq doorbell
    // ONCE, then reaps all their completions  -  letting the controller pipeline
    // the chunks instead of the old submit-one / poll-one (QD=1) round trips.
    // direction: NVME_IO_READ or NVME_IO_WRITE. (satoru)
    static bool SubmitIOBatch(int qid, uint8_t opcode, uint64_t lba,
                              uint32_t count, uint8_t* buffer);
    // reap exactly `n` completions off qp (any order), returning false on any
    // error/timeout. used by SubmitIOBatch after a batched doorbell ring. (satoru)
    static bool ReapCompletions(NVMeQueuePair* qp, int n);
};
