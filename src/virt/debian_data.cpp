//  kurono os - debian rootfs loader (embedded or on-disk)
#include "debian_data.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

extern "C" {
    extern const uint8_t _binary_debian_minbase_ext4_start[] __attribute__((weak));
    extern const uint8_t _binary_debian_minbase_ext4_end[]   __attribute__((weak));
}

static const char* DEBIAN_DISK_PATH = "/var/lib/kurono/debian-rootfs.ext4";

// runtime cache: filled on first Data() call when no embedded blob exists
static uint8_t* s_disk_buf  = nullptr;
static uint32_t s_disk_size = 0;
static bool     s_load_attempted = false;

static uint32_t embedded_size() {
    if (_binary_debian_minbase_ext4_start == nullptr) return 0;
    return (uint32_t)((uintptr_t)_binary_debian_minbase_ext4_end -
                       (uintptr_t)_binary_debian_minbase_ext4_start);
}

static bool try_load_from_disk() {
    if (s_load_attempted) return s_disk_buf != nullptr;
    s_load_attempted = true;

    if (!KVFS::Exists(DEBIAN_DISK_PATH)) return false;

    // probe size by reading into a small buffer first to query length;
    // KVFS::ReadFile returns bytes read, but we need the file size up
    // front to allocate.  We resolve the node directly.
    KVFSNode* n = KVFS::Resolve(DEBIAN_DISK_PATH);
    if (!n) return false;
    uint32_t sz = n->size;
    if (sz == 0) return false;

    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(sz);
    if (!buf) {
        SerialLogger::Log("[DebianRootfs] heap alloc for disk buffer failed\r\n");
        return false;
    }
    int n_read = KVFS::ReadFile(DEBIAN_DISK_PATH, (char*)buf, sz);
    if (n_read <= 0) {
        KernelHeap::Free(buf);
        return false;
    }
    s_disk_buf  = buf;
    s_disk_size = (uint32_t)n_read;
    SerialLogger::Log("[DebianRootfs] loaded debian rootfs from disk\r\n");
    return true;
}

bool DebianRootfs::Available() {
    if (embedded_size() > 0) return true;
    if (s_disk_buf) return true;
    return KVFS::Exists(DEBIAN_DISK_PATH);
}

uint32_t DebianRootfs::Size() {
    uint32_t es = embedded_size();
    if (es > 0) return es;
    if (!s_disk_buf) try_load_from_disk();
    return s_disk_size;
}

const uint8_t* DebianRootfs::Data() {
    if (embedded_size() > 0) return _binary_debian_minbase_ext4_start;
    if (!s_disk_buf) try_load_from_disk();
    return s_disk_buf;
}

bool DebianRootfs::SaveDownloaded(const uint8_t* buf, uint32_t len) {
    if (!buf || len == 0) return false;
    KVFS::Mkdirs("/var/lib/kurono");
    if (KVFS::WriteFile(DEBIAN_DISK_PATH, (const char*)buf, len) < 0) {
        SerialLogger::Log("[DebianRootfs] failed to write disk copy\r\n");
        return false;
    }
    // refresh cache from the freshly-saved buffer
    EvictCache();
    s_load_attempted = false;
    return true;
}

void DebianRootfs::EvictCache() {
    if (s_disk_buf) {
        KernelHeap::Free(s_disk_buf);
        s_disk_buf = nullptr;
    }
    s_disk_size = 0;
    s_load_attempted = false;
}
