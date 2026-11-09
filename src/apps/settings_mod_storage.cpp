//  kurono os - settings module: storage (satoru)
//  memory use (physical ram + kernel heap) as animated meters, every disk in
//  the block device registry (nvme, sata, ...), and the kurono file system with
//  its persistence state. read-only; values are sampled when the page opens. (satoru)
#include "system_settings.h"
#include "../drivers/blockdev.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#if defined(KUI_BACKEND_WAYLAND)
#include "../kui/ring3/kui_r3_settings.h"
#endif

using namespace SettingsUI;

static uint64_t s_ram_used = 0, s_ram_total = 0;
static uint64_t s_heap_used = 0, s_heap_total = 0;
static uint32_t s_fs_used = 0;
static int      s_nodes = 0;
static bool     s_persist = false;

static int count_nodes(KVFSNode* n, int depth){
    if(!n || depth > 64) return 0;
    int t = 1;
    for(int i = 0; i < n->child_count; i++) t += count_nodes(n->children[i], depth + 1);
    return t;
}

static void size_str(uint64_t bytes, char* b, int mx){
    uint64_t kb = bytes / 1024, mb = kb / 1024, gb = mb / 1024;
    if(gb >= 10){ IntToStr((int)gb, b, mx); StrApp(b, " GB", mx); return; }
    if(gb >= 1){
        IntToStr((int)gb, b, mx); StrApp(b, ".", mx);
        char f[4]; IntToStr((int)((mb % 1024) * 10 / 1024), f, 4); StrApp(b, f, mx);
        StrApp(b, " GB", mx); return;
    }
    if(mb >= 1){ IntToStr((int)mb, b, mx); StrApp(b, " MB", mx); return; }
    IntToStr((int)kb, b, mx); StrApp(b, " KB", mx);
}
static int pct(uint64_t u, uint64_t t){ return t ? (int)((u * 100) / t) : 0; }

static void storage_on_show(){
    s_ram_total = PMM::GetTotalMemory();
    s_ram_used  = PMM::GetUsedFrames() * PAGE_SIZE;
    s_heap_used = KernelHeap::GetUsed();
    s_heap_total = KernelHeap::GetTotal();
    if(!s_heap_total) s_heap_total = s_heap_used + KernelHeap::GetFree();
    s_fs_used = KVFS::DiskUsage("/");
#if defined(KUI_BACKEND_WAYLAND)
    s_nodes = SettingsBridge::NodeCount();
#else
    s_nodes = count_nodes(KVFS::GetRoot(), 0);
#endif
    s_persist = PersistStore::Available();
}

static void used_of(uint64_t u, uint64_t t, char* b, int mx){
    char x[24]; size_str(u, b, mx); StrApp(b, " of ", mx); size_str(t, x, 24); StrApp(b, x, mx);
}

static void storage_build(){
    char buf[96];

    Group("Memory");
    used_of(s_ram_used, s_ram_total, buf, 96);
    Meter("Physical memory", buf, pct(s_ram_used, s_ram_total));
    used_of(s_heap_used, s_heap_total, buf, 96);
    Meter("Kernel heap", buf, pct(s_heap_used, s_heap_total));
    EndGroup();

    Group("Disks");
    int n = BlockDev::Count();
    int shown = 0;
    for(int i = 0; i < n; i++){
        BlockDevice* d = BlockDev::Get(i);
        if(!d || !d->present) continue;
        PushId(i);
        size_str(d->capacity_lba * (uint64_t)(d->sector_size ? d->sector_size : 512), buf, 96);
        char sub[64];
        StrCpy(sub, d->name, 64); StrApp(sub, ", ", 64);
        if(d->driver[0] == 'n') StrApp(sub, "NVMe", 64);
        else if(d->driver[0] == 'a') StrApp(sub, "SATA", 64);
        else StrApp(sub, d->driver, 64);
        if(d->removable) StrApp(sub, ", removable", 64);
        Info(d->model[0] ? d->model : d->name, buf, sub);
        PopId();
        shown++;
    }
    if(!shown) Info("Disks", "None found", "Kurono is running from memory");
    EndGroup();

    Group("Kurono file system");
    size_str(s_fs_used, buf, 96);
    Info("Space used", buf);
    IntToStr(s_nodes, buf, 96);
    Info("Files and folders", buf);
    Info("Saved to disk", s_persist ? "Yes" : "No",
         s_persist ? "Files and settings are kept across restarts" : "Changes last until the next restart");
    EndGroup();
}

extern const SettingsModule g_storage_module = {
    "storage", "Storage", "", storage_on_show, nullptr, nullptr, nullptr, storage_build
};
// end (satoru)
