// KLD display bridge: the kernel half of the DRM translator (D2).
//
// A DRM container owns a display adapter and drives it with an unmodified
// upstream driver. This file turns that container's display into an output
// backend of Kurono's graphics layer:
//
//   modes up        the container writes its mode table into the shared ring,
//                   then issues KLD_OP_DISPLAY_MODES; every value is validated
//                   here against the scanout cap before it is used.
//   frame down      the kernel composes into a broker-owned scanout buffer
//                   (mapped READ-ONLY into the container) and posts a damage
//                   frame into the ring. the container copies the damage into
//                   its DRM dumb framebuffer and flushes it.
//   flip-done up    the container calls KLD_OP_DISPLAY_DONE(seq); a frame not
//                   acknowledged within the timeout is dropped and counted,
//                   and the rest of the desktop is never blocked on it.
//
// The ring layout below must stay in lockstep with
// kld/lx_emul/kimpl/lx_display.h. (satoru)
//
// end (satoru)

#pragma once
#include "types.h"

namespace KldDisplay {

constexpr uint32_t KLD_DISP_MAGIC   = 0x4B4C4431u;   // "KLD1" (satoru)
constexpr uint32_t KLD_DISP_VERSION = 1u;

constexpr uint32_t KLD_DISP_MAX_MODES  = 32;
constexpr uint32_t KLD_DISP_DOWN_SLOTS = 8;
constexpr uint32_t KLD_DISP_MAX_RECTS  = 16;

// the scanout is xrgb8888. 1080p fits one broker DMA_ALLOC (2025 pages); a
// larger cap needs a second allocation and a phys list, so 4k is later work.
// (satoru)
constexpr uint32_t KLD_DISP_CAP_W = 1920;
constexpr uint32_t KLD_DISP_CAP_H = 1080;
constexpr uint32_t KLD_DISP_SCANOUT_BYTES = KLD_DISP_CAP_W * KLD_DISP_CAP_H * 4u;
constexpr uint32_t KLD_DISP_SCANOUT_PAGES = KLD_DISP_SCANOUT_BYTES / 4096u;
constexpr uint32_t KLD_DISP_RING_PAGES    = 1u;

enum DownKind : uint32_t {
    DOWN_FRAME    = 1,
    DOWN_SET_MODE = 2,
    DOWN_EXIT     = 3,   // test hook: the container exits, the bridge restores (satoru)
};

struct DispMode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;        // bytes per line of the scanout
    uint32_t refresh_mhz;
    uint32_t flags;
};

struct DispRect {
    int32_t  x, y;
    uint32_t w, h;
};

struct DispDown {
    uint32_t seq;
    uint32_t kind;
    uint32_t generation;
    uint32_t nr;
    uint32_t mode_index;
    uint32_t pad0, pad1, pad2;
    DispRect rects[KLD_DISP_MAX_RECTS];
};

struct DispRing {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t generation;      // kernel writes
    volatile uint32_t current_mode;    // kernel writes
    volatile uint32_t mode_count;      // container writes
    volatile uint32_t modes_seq;       // container increments
    volatile uint32_t doorbell;        // kernel bumps + futex-wakes
    volatile uint32_t down_prod;       // kernel writes
    volatile uint32_t down_cons;       // container writes
    volatile uint32_t done_seq;        // container writes
    volatile uint32_t done_count;      // container accumulates
    volatile uint32_t hotplug_seq;     // container writes
    volatile uint32_t hotplug_status;
    volatile uint32_t error_code;
    uint32_t scanout_bytes;            // kernel writes
    uint32_t scanout_pages;
    uint32_t fuzz;                     // kernel writes: hostile-value test (satoru)
    uint32_t stall;                    // kernel writes: container stops acking (test) (satoru)
    uint32_t diepre;                   // kernel writes: container exits before setup (test) (satoru)
    uint32_t attachloop;               // kernel writes: hostile DETACH/ATTACH loop (test) (satoru)
    uint32_t modedelay;                // kernel writes: delay set_mode ack 1.5 s (test) (satoru)
    uint32_t badcount;                 // kernel writes: hostile wrong-count attach test (satoru)
    uint32_t setupfail;                // kernel writes: the next setup is forced to fail (test) (satoru)
    uint32_t badunmap;                 // kernel writes: hostile munmap/madvise rig (test) (satoru)
    DispMode modes[KLD_DISP_MAX_MODES];
    DispDown down[KLD_DISP_DOWN_SLOTS];
};

static_assert(sizeof(DispRing) <= KLD_DISP_RING_PAGES * 4096u,
              "KLD display ring must fit its one page");

// boot flag: off by default. pattern is the D2.1 kernel pattern; mirror,
// extend and primary are the D2.2 compositor modes. (satoru)
enum Mode : uint32_t {
    MODE_OFF = 0,
    MODE_PATTERN,
    MODE_MIRROR,
    MODE_EXTEND,
    MODE_PRIMARY,
};

void BootInit(const char* boot_cmdline);
bool Wanted();
Mode GetMode();
const char* ModeName();

// kurono.kld.display.bench=1: start gfxbench once the desktop is on the
// container output (the boot bench would run before the container attaches).
// (satoru)
bool DisplayBenchRequested();
// kurono.kld.display.badcount=1: the broker logs the pmm frame count for each
// refused wrong-count attach, so the log proves the refusal allocates nothing.
// (satoru)
bool BadCountTest();
// kurono.kld.display.detachdelay=<ms>: rig, sleep this long between the exit
// state change and the bridge detach so the gui runs in that window. (satoru)
int ExitDetachDelayMsForTest();
// kurono.kld.display.freedelay=<ms>: rig, hold the final free between two
// batches so the vmm teardown hook can run concurrently. (satoru)
int FreeDelayMsForTest();

// broker-facing: the broker allocated + mapped the broker-owned frames. the
// kernel side reads both through the identity map. (satoru)
int64_t AttachPrecheck();
int64_t Attach(uint32_t id, uint64_t as, uint64_t ring_phys,
               uint64_t scanout_phys, uint32_t scanout_pages);
// two-phase teardown: the broker removes the mapping (explicit) and calls this;
// the frames are only freed after the gui has presented two frames with the
// output removed. (satoru)
void Detach(uint32_t id);
void DetachForRelease(uint32_t id);
int64_t ReportModes(uint64_t as);
int64_t Done(uint64_t as, uint32_t seq);
int64_t Hotplug(uint64_t as, uint32_t status);
int64_t Error(uint64_t as, uint32_t code);

// kernel self check of every value a container can send up. (satoru)
void SelfCheck();

// the kernel pattern process (MODE_PATTERN): draws the D1.3 pattern and 120
// moving-block frames into the scanout and counts flip-done. (satoru)
void PatternEntry();

// d2.2: the GUI loop calls this at a safe point every frame. it applies a
// pending mirror/extend/primary setup once the container has reported its
// modes, and restores the firmware framebuffer if the container died. (satoru)
void ApplyPendingDesktop();

// d2.4: ask the container for a mode in its reported list. the change is sent
// through the ring and applied only after DISPLAY_DONE. false when the mode is
// not in the container's table or the bridge is not ready. (satoru)
bool RequestMode(uint32_t w, uint32_t h);
// kurono.kld.display.setmode=WxH (test/rig hook). (satoru)
bool SetModeRequested(uint32_t* w, uint32_t* h);
// test hook: make the container exit once (kill/restore path). (satoru)
void RequestExitForTest();
// kld_auto's supervisor loop starts: a fresh attach budget for this boot. (satoru)
void ResetAttachBudget();
// the killloop stress flag disables the supervisor's give-up (rig only). (satoru)
bool RestartUnlimitedForTest();

// d2.2 compositor backend (added when the desktop is wired). (satoru)
bool Ready();
uint32_t ModeWidth();
uint32_t ModeHeight();
uint32_t ModePitch();
// copy the compositor's damage rects into the scanout and post one frame.
// returns false when not attached/ready or a frame is already in flight. the
// caller keeps its own frame; this never blocks. (satoru)
typedef struct { int x, y, w, h; } Rect;
bool PresentRects(const Rect* rects, int count, bool full, uint32_t* seq_out);

}  // namespace KldDisplay

// end (satoru)
