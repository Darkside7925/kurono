// src owns the wire format of the KLD drm userspace interface (stage D5); the
// container's copy is kld/lx_emul/kimpl/lx_dri.h and must stay byte identical from
// the first #ifndef on (kld/tests/check_dri_proto.sh diffs them). sizes are asserted
// on both sides. plain integer types only. (satoru)
//
// end (satoru)

#ifndef KLD_LX_DRI_H
#define KLD_LX_DRI_H

#define LXDRI_MAGIC      0x4B445231u   /* "KDR1" */
#define LXDRI_VERSION    1u
#define LXDRI_SLOTS      32u
#define LXDRI_BOUNCE     16384u
#define LXDRI_SLOT_PAGES 5u
#define LXDRI_FILES      256u
#define LXDRI_CLOSEQ     256u
#define LXDRI_MAX_SEG    64u
#define LXDRI_RING_PAGES (1u + LXDRI_SLOTS * LXDRI_SLOT_PAGES + 1u)

/* request kinds */
#define LXDRI_K_OPEN   1u
#define LXDRI_K_IOCTL  2u
#define LXDRI_K_MMAP   3u
#define LXDRI_K_READ   4u

/* shared slot state word */
#define LXDRI_S_FREE    0u
#define LXDRI_S_QUEUED  1u   /* kernel wrote it, no worker has it */
#define LXDRI_S_SERVING 2u   /* a worker claimed it (compare and swap) */
#define LXDRI_S_DONE    3u   /* reply op sent */

/* uaccess direction */
#define LXDRI_UA_FROM_USER 1u
#define LXDRI_UA_TO_USER   2u

struct lxdri_seg {
    unsigned int obj;
    unsigned int first;
    unsigned int npages;
    unsigned int flags;
};

struct lxdri_slot {
    volatile unsigned int state;
    unsigned int kind;
    unsigned int file_id;
    unsigned int cmd;
    unsigned long long arg;
    unsigned int pid, tgid, euid, egid;
    unsigned long long req_id;
    unsigned long long mm_offset;
    unsigned long long mm_len;
    unsigned int mm_prot, mm_flags;
    unsigned int open_flags, open_minor;
    /* caller memory access requested by the container for this request */
    unsigned long long ua_ptr;
    unsigned int ua_len, ua_dir;
    volatile unsigned int ua_ack;   /* kernel bumps after the caller's copy; container futex waits */
    unsigned int ua_pad;
    long long ua_res;               /* bytes NOT copied */
    /* reply, written by the container before the REPLY op */
    long long result;
    unsigned int nseg, reply_flags;
    struct lxdri_seg seg[LXDRI_MAX_SEG];
};

struct lxdri_file {
    volatile unsigned int ev_bytes;
    unsigned int pad[3];
};

struct lxdri_hdr {
    unsigned int magic, version, nslots, nfiles;
    volatile unsigned int doorbell;      /* futex word the workers wait on */
    volatile unsigned int closeq_prod;   /* kernel */
    volatile unsigned int closeq_cons;   /* container */
    unsigned int test;                   /* kernel: rig test id, 0 = none */
    unsigned int minors;                 /* container told the kernel at attach (bit0 card0, bit1 renderD128) */
    unsigned int pad[3];
    unsigned int closeq[LXDRI_CLOSEQ];
};

#define LXDRI_SLOT_OFF(i)   ((1u + (i) * LXDRI_SLOT_PAGES) * 4096u)
#define LXDRI_BOUNCE_OFF(i) (LXDRI_SLOT_OFF(i) + 4096u)
#define LXDRI_FILES_OFF     ((1u + LXDRI_SLOTS * LXDRI_SLOT_PAGES) * 4096u)

/* broker ops, 0xA0..0xBF are the drm userspace interface's */
#define LX_KLD_OP_DRI_ATTACH  0xA0u   /* a0=ring va, a1=pages, a2=minor mask */
#define LX_KLD_OP_DRI_DETACH  0xA1u
#define LX_KLD_OP_DRI_UACCESS 0xA2u   /* a0=req id, a1=user ptr, a2=len, a3=dir */
#define LX_KLD_OP_DRI_REPLY   0xA3u   /* a0=req id, a1=result */
#define LX_KLD_OP_DRI_OBJ_ALLOC 0xA4u /* a0=va, a1=pages */
#define LX_KLD_OP_DRI_OBJ_FREE  0xA5u /* a0=object id */
#define LX_KLD_OP_DRI_STATUS    0xA6u /* rig: logs the interface counters, returns free pmm frames */

/* rig only (kurono.kld.dri.test): the kernel starts and kills ordinary unprivileged test processes
 * (uid 1000, this same image with --dri-child), because a multithreaded container cannot fork */
#define LX_KLD_OP_DRI_CHILD_SPAWN 0xA7u  /* a0=mode, a1=arg; returns a handle 1..15 */
#define LX_KLD_OP_DRI_CHILD_KILL  0xA8u  /* a0=handle */
#define LX_KLD_OP_DRI_CHILD_STATE 0xA9u  /* a0=handle; returns state | pid<<8 | (rc&0xff)<<40; state 1 running 2 exited */
#define LX_KLD_OP_DRI_CHILD_FREE  0xAAu  /* a0=handle */
#define LX_KLD_OP_DRI_UA_CHECK    0xACu  /* a0=req id; returns 1 when the caller of that request is gone (a bounded wait in the container asks) */

/* ---------------------------------------------------------------------- */
#ifdef __cplusplus
#define LXDRI_ASSERT(c, m) static_assert(c, m)
#else
#define LXDRI_ASSERT(c, m) _Static_assert(c, m)
#endif
LXDRI_ASSERT(sizeof(struct lxdri_slot) == 1152, "lxdri_slot layout");
LXDRI_ASSERT(sizeof(struct lxdri_hdr) == 1072, "lxdri_hdr layout");
LXDRI_ASSERT(sizeof(struct lxdri_file) == 16, "lxdri_file layout");
LXDRI_ASSERT(sizeof(struct lxdri_seg) == 16, "lxdri_seg layout");
LXDRI_ASSERT(sizeof(struct lxdri_slot) <= 4096, "slot header fits its page");

#endif

/* end (satoru) */
