// container side of the KLD display ring. MUST stay in lockstep with
// src/kernel/kld_display.h (same layout, same magic and constants). (satoru)
//
// end (satoru)

#ifndef KLD_LX_DISPLAY_H
#define KLD_LX_DISPLAY_H

#define LX_DISP_MAGIC   0x4B4C4431u
#define LX_DISP_VERSION 1u

#define LX_DISP_MAX_MODES  32u
#define LX_DISP_DOWN_SLOTS 8u
#define LX_DISP_MAX_RECTS  16u

#define LX_DISP_CAP_W 1920u
#define LX_DISP_CAP_H 1080u
#define LX_DISP_SCANOUT_BYTES (LX_DISP_CAP_W * LX_DISP_CAP_H * 4u)
#define LX_DISP_SCANOUT_PAGES (LX_DISP_SCANOUT_BYTES / 4096u)
#define LX_DISP_RING_PAGES    1u

#define LX_DISP_DOWN_FRAME    1u
#define LX_DISP_DOWN_SET_MODE 2u
#define LX_DISP_DOWN_EXIT     3u

struct lx_disp_mode {
    unsigned int width, height, pitch, refresh_mhz, flags;
};

struct lx_disp_rect {
    int x, y;
    unsigned int w, h;
};

struct lx_disp_down {
    unsigned int seq, kind, generation, nr;
    unsigned int mode_index;
    unsigned int pad0, pad1, pad2;
    struct lx_disp_rect rects[LX_DISP_MAX_RECTS];
};

struct lx_disp_ring {
    unsigned int magic;
    unsigned int version;
    volatile unsigned int generation;
    volatile unsigned int current_mode;
    volatile unsigned int mode_count;
    volatile unsigned int modes_seq;
    volatile unsigned int doorbell;
    volatile unsigned int down_prod;
    volatile unsigned int down_cons;
    volatile unsigned int done_seq;
    volatile unsigned int done_count;
    volatile unsigned int hotplug_seq;
    volatile unsigned int hotplug_status;
    volatile unsigned int error_code;
    unsigned int scanout_bytes;
    unsigned int scanout_pages;
    unsigned int fuzz;       // kernel: run the hostile-value test writer (satoru)
    unsigned int stall;      // kernel: container stops acking (test) (satoru)
    unsigned int diepre;     // kernel: container exits before setup (test) (satoru)
    unsigned int attachloop; // kernel: hostile DETACH/ATTACH loop (test) (satoru)
    unsigned int modedelay;  // kernel: delay the set_mode ack (test) (satoru)
    unsigned int badcount;   // kernel: hostile wrong-count attach test (satoru)
    unsigned int setupfail;  // kernel: the next setup is forced to fail (test) (satoru)
    unsigned int badunmap;   // kernel: hostile munmap/madvise rig (test) (satoru)
    struct lx_disp_mode modes[LX_DISP_MAX_MODES];
    struct lx_disp_down down[LX_DISP_DOWN_SLOTS];
};

_Static_assert(sizeof(struct lx_disp_ring) <= LX_DISP_RING_PAGES * 4096u,
               "KLD display ring must fit its one page");

#endif
// end (satoru)
