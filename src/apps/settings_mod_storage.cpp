//  kurono os  -  settings module: storage (satoru)
//  read-only, scrollable storage page: kernel-heap usage bar, kvfs volume +
//  mount-point list with type/usage bars, detected block device (nvme model +
//  capacity, else the virtual disk), and filesystem detail rows (block size,
//  inode/node counts). everything is detected live; no persisted state  -  but
//  written through the same SettingsUI helpers so it matches the shell. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/nvme.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
// snapshots taken in on_show so render/content_height agree on one reading and
// the page does not flicker as the heap churns under us. (satoru)
static size_t s_heap_used   = 0;     // bytes (satoru)
static size_t s_heap_total  = 0;     // bytes (satoru)
static uint32_t s_kvfs_used = 0;     // bytes used under "/" (satoru)
static int    s_node_count  = 0;     // live kvfs nodes reachable from root (satoru)
static bool   s_nvme        = false; // nvme controller detected this boot (satoru)

// ── helpers ─────────────────────────────────────────────────────────────────
// safe percentage (0..100) of used/total, guarding divide-by-zero. (satoru)
static int pct_of(uint64_t used, uint64_t total){
    if(total == 0) return 0;
    uint64_t p = (used * 100) / total;
    if(p > 100) p = 100;
    return (int)p;
}

// recursively count nodes in the kvfs tree (acts as a live "inode" count). the
// tree is bounded (KVFS_MAX_CHILDREN per dir) so plain recursion is safe. (satoru)
static int count_nodes(KVFSNode* n){
    if(!n) return 0;
    int total = 1;
    for(int i = 0; i < n->child_count; i++)
        total += count_nodes(n->children[i]);
    return total;
}

// human-ish size into b: prints KB up to 1024 KB, then MB, then GB. keeps the
// label libc-free using only the SettingsUI int/str utilities. (satoru)
static void size_str(uint64_t bytes, char* b, int mx){
    uint64_t kb = bytes / 1024;
    if(kb < 1024){
        SettingsUI::IntToStr((int)kb, b, mx); SettingsUI::StrApp(b, " KB", mx); return;
    }
    uint64_t mb = kb / 1024;
    if(mb < 1024){
        SettingsUI::IntToStr((int)mb, b, mx); SettingsUI::StrApp(b, " MB", mx); return;
    }
    uint64_t gb = mb / 1024;
    SettingsUI::IntToStr((int)gb, b, mx); SettingsUI::StrApp(b, " GB", mx);
}

// nvme total capacity in bytes (lba count * lba size). (satoru)
static uint64_t nvme_bytes(){
    const NVMeControllerInfo& in = NVMe::GetInfo();
    uint64_t lba  = in.total_capacity_lba ? in.total_capacity_lba : NVMe::GetCapacityLBA();
    uint64_t bsz  = in.lba_size ? in.lba_size : NVMe::GetLBASize();
    return lba * bsz;
}

// ── layout constants shared by render + input/content_height so geometry and
//    the scrollbar stay in lock-step. read-only page: no *Hit math needed, but
//    the running-y advances must match between render and content_height. (satoru)
static const int PAD       = 8;    // top pad before the first row (satoru)
static const int BAR_H     = 16;   // usage bar height in px (satoru)
static const int VAL_X_OFF = 150;  // value column for Row()-style detail rows (satoru)

// draw one labelled read-only usage bar (track + accent fill + caption). the
// SettingsUI Slider is a read-only fill per the interface, so we use it for the
// bar and overlay a caption above it. returns the y advance consumed. (satoru)
static int usage_bar(int x, int ly, int w, const char* label,
                     uint64_t used, uint64_t total){
    int p = pct_of(used, total);
    Graphics::DrawString(x, ly, label, SettingsUI::COL_TEXT, 0xFF000000);

    // caption "used / total (pct%)" right-aligned-ish after the label. (satoru)
    char cap[64]; size_str(used, cap, 64);
    SettingsUI::StrApp(cap, " / ", 64);
    char tb[24]; size_str(total, tb, 24); SettingsUI::StrApp(cap, tb, 64);
    SettingsUI::StrApp(cap, " (", 64);
    char pb[8]; SettingsUI::IntToStr(p, pb, 8); SettingsUI::StrApp(cap, pb, 64);
    SettingsUI::StrApp(cap, "%)", 64);
    Graphics::DrawString(x + 110, ly, cap, SettingsUI::COL_DIM, 0xFF000000);
    ly += 18;

    int bw = w - 24;
    if(bw < 80) bw = 80;
    SettingsUI::Slider(x, ly + (BAR_H / 2) - (SettingsUI::SLIDER_H / 2), bw, p);
    return 18 + BAR_H + 8;
}

// ── on_show: snapshot heap + kvfs + probe nvme ───────────────────────────────
static void storage_on_show(){
    s_heap_used  = KernelHeap::GetUsed();
    s_heap_total = KernelHeap::GetTotal();
    if(s_heap_total == 0) s_heap_total = s_heap_used + KernelHeap::GetFree();

    s_kvfs_used  = KVFS::DiskUsage("/");
    s_node_count = count_nodes(KVFS::GetRoot());

    s_nvme = NVMe::IsDetected();
}

// the static mount table, mirrored from the legacy settings.cpp Storage tab so
// the two pages stay consistent. {path, type, mode}. (satoru)
static const char* kMounts[][3] = {
    {"/",     "kvfs",   "rw"},
    {"/dev",  "devfs",  "rw"},
    {"/proc", "procfs", "ro"},
    {"/tmp",  "tmpfs",  "rw"},
    {"/etc",  "kvfs",   "rw"},
};
static const int kMountCount = (int)(sizeof(kMounts) / sizeof(kMounts[0]));

static void storage_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ly = y - scroll + PAD;
    char buf[64];

    // ── overview: kernel heap usage ──────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Overview");
    ly += 26;
    ly += usage_bar(x, ly, w, "Kernel Heap", s_heap_used, s_heap_total);

    // free figure as a plain detail row beneath the bar. (satoru)
    {
        uint64_t freeb = (s_heap_total > s_heap_used) ? (s_heap_total - s_heap_used) : 0;
        size_str(freeb, buf, 64);
        Graphics::DrawString(x, ly, "Available:", SettingsUI::COL_TEXT, 0xFF000000);
        Graphics::DrawString(x + VAL_X_OFF, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
        ly += 22;
    }

    // ── filesystems / mounts ─────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Filesystems / Mounts");
    ly += 26;

    // the kvfs root volume gets its own usage bar (used vs the per-file 64 kb
    // budget * node count is meaningless, so bar it against the heap pool the
    // tree lives in). (satoru)
    ly += usage_bar(x, ly, w, "KVFS Volume", s_kvfs_used, s_heap_total);

    // column header for the mount list. (satoru)
    Graphics::DrawString(x + 12,  ly, "Path", SettingsUI::COL_DIM, 0xFF000000);
    Graphics::DrawString(x + 130, ly, "Type", SettingsUI::COL_DIM, 0xFF000000);
    Graphics::DrawString(x + 240, ly, "Mode", SettingsUI::COL_DIM, 0xFF000000);
    ly += 18;
    for(int i = 0; i < kMountCount; i++){
        Graphics::DrawString(x + 12,  ly, kMounts[i][0], SettingsUI::COL_TEXT, 0xFF000000);
        Graphics::DrawString(x + 130, ly, kMounts[i][1], SettingsUI::COL_DIM,  0xFF000000);
        Graphics::DrawString(x + 240, ly, kMounts[i][2], SettingsUI::COL_DIM,  0xFF000000);
        ly += 18;
    }
    ly += 8;

    // ── block devices ────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Block Devices");
    ly += 26;
    if(s_nvme){
        const NVMeControllerInfo& in = NVMe::GetInfo();
        SettingsUI::Row(x, ly, "Controller:", "NVMe (PCIe)");
        ly += 22;
        SettingsUI::Row(x, ly, "Model:", in.model[0] ? in.model : "NVMe SSD");
        ly += 22;
        SettingsUI::Row(x, ly, "Serial:", in.serial[0] ? in.serial : " - ");
        ly += 22;
        SettingsUI::Row(x, ly, "Firmware:", in.firmware[0] ? in.firmware : " - ");
        ly += 22;

        size_str(nvme_bytes(), buf, 64);
        SettingsUI::Row(x, ly, "Capacity:", buf);
        ly += 22;

        // lba size + namespace count. (satoru)
        SettingsUI::IntToStr((int)(in.lba_size ? in.lba_size : NVMe::GetLBASize()), buf, 64);
        SettingsUI::StrApp(buf, " B/LBA", 64);
        SettingsUI::Row(x, ly, "Sector Size:", buf);
        ly += 22;

        SettingsUI::IntToStr((int)in.num_namespaces, buf, 64);
        SettingsUI::Row(x, ly, "Namespaces:", buf);
        ly += 22;

        // i/o counters since boot. (satoru)
        size_str(NVMe::GetBytesRead(), buf, 64);
        SettingsUI::Row(x, ly, "Read:", buf);
        ly += 22;
        size_str(NVMe::GetBytesWritten(), buf, 64);
        SettingsUI::Row(x, ly, "Written:", buf);
        ly += 22;
    } else {
        // no nvme  -  describe the in-memory virtual disk the kvfs lives on. (satoru)
        SettingsUI::Row(x, ly, "Device:", "Virtual Disk (RAM-backed)");
        ly += 22;
        SettingsUI::Row(x, ly, "Model:", "Kurono KVFS Volume");
        ly += 22;
        size_str(s_heap_total, buf, 64);
        SettingsUI::Row(x, ly, "Capacity:", buf);
        ly += 22;
        SettingsUI::Row(x, ly, "Interface:", "Memory (no NVMe/AHCI present)");
        ly += 22;
    }

    // ── details ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Details");
    ly += 26;
    {
        SettingsUI::IntToStr(KVFS_BLOCK_SIZE, buf, 64);
        SettingsUI::StrApp(buf, " bytes", 64);
        SettingsUI::Row(x, ly, "Block Size:", buf);
        ly += 22;

        // live node count == live inode count for kvfs. (satoru)
        SettingsUI::IntToStr(s_node_count, buf, 64);
        SettingsUI::Row(x, ly, "Inodes (live):", buf);
        ly += 22;

        SettingsUI::IntToStr(KVFS_MAX_CHILDREN, buf, 64);
        SettingsUI::Row(x, ly, "Max Entries/Dir:", buf);
        ly += 22;

        SettingsUI::IntToStr(KVFS_MAX_FILE_SIZE / (1024 * 1024), buf, 64);
        SettingsUI::StrApp(buf, " MB", 64);
        SettingsUI::Row(x, ly, "Max File Size:", buf);
        ly += 22;

        SettingsUI::Row(x, ly, "Max Path:", "256 chars");
        ly += 22;
        SettingsUI::Row(x, ly, "Persistence:", "Serialized image (snapshot)");
        ly += 22;
    }
}

// storage is read-only: no interactive controls, nothing to persist. (satoru)
static bool storage_input(int mx, int my, bool click, char key, int scroll){
    (void)mx; (void)my; (void)click; (void)key; (void)scroll;
    return false;
}

// total content height for the scrollbar. must sum the SAME row advances the
// render walks: each usage_bar() contributes (18 + BAR_H + 8) = 42 px, headers
// 26, detail rows 22. the block-devices section differs by branch, so we use
// the taller (nvme, 9 rows) so the bar never under-scrolls. (satoru)
static int storage_content_height(){
    int bar = 18 + BAR_H + 8;                  // one usage_bar advance (satoru)
    int h = PAD;
    h += 26 + bar + 22;                         // overview: header + heap bar + available (satoru)
    h += 26 + bar;                              // mounts: header + kvfs bar (satoru)
    h += 18 + (kMountCount * 18) + 8;           // mount column header + rows + gap (satoru)
    h += 26 + (9 * 22);                         // block devices: header + nvme branch (9 rows) (satoru)
    h += 26 + (6 * 22);                         // details: header + 6 rows (satoru)
    h += 16;                                     // tail (satoru)
    return h;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_storage_module;` resolves at link time. (satoru)
extern const SettingsModule g_storage_module = {
    "storage", "Storage", "\x05",
    storage_on_show, storage_render, storage_input, storage_content_height
};
// end (satoru)
