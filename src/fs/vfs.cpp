#include "vfs.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

FileNode* VFS::root = nullptr;
static uint8_t ramdisk_buffer[1024 * 16];
static uint32_t ramdisk_size = 0;
static FileNode input_log_node;
static bool input_log_init = false;

int RamDiskRead(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)node;
    if (!buffer || size == 0) return 0;
    if (offset >= ramdisk_size) return 0;
    uint32_t avail = ramdisk_size - offset;
    if (size > avail) size = avail;
    for (uint32_t i = 0; i < size; i++) buffer[i] = ramdisk_buffer[offset + i];
    return (int)size;
}

int RamDiskWrite(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)node;
    if (!buffer || size == 0) return 0;
    if (offset >= sizeof(ramdisk_buffer)) return 0;
    uint32_t cap = (uint32_t)sizeof(ramdisk_buffer) - offset;
    if (size > cap) size = cap;
    for (uint32_t i = 0; i < size; i++) ramdisk_buffer[offset + i] = buffer[i];
    if (offset + size > ramdisk_size) ramdisk_size = offset + size;
    if (node) node->size = ramdisk_size;
    return (int)size;
}

static FileNode* GetInputLogNode() {
    if (!input_log_init) {
        for (size_t i = 0; i < sizeof(input_log_node); i++) ((uint8_t*)&input_log_node)[i] = 0;
        const char* nm = "input.log";
        int j = 0;
        while (nm[j] && j < (int)sizeof(input_log_node.name) - 1) { input_log_node.name[j] = nm[j]; j++; }
        input_log_node.name[j] = 0;
        input_log_node.type = FT_File;
        input_log_node.read = RamDiskRead;
        input_log_node.write = RamDiskWrite;
        input_log_node.open = nullptr;
        input_log_node.close = nullptr;
        input_log_node.finddir = nullptr;
        input_log_init = true;
    }
    input_log_node.size = ramdisk_size;
    return &input_log_node;
}

static bool name_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

void VFS::Init() {
    SerialLogger::Log("VFS.1\r\n");
    root = (FileNode*)KernelHeap::Alloc(sizeof(FileNode));
    SerialLogger::Log("VFS.2\r\n");
    if (!root) {
        SerialLogger::Log("VFS: root is NULL!\r\n");
        return;
    }
    for (size_t i = 0; i < sizeof(*root); i++) ((uint8_t*)root)[i] = 0;
    root->name[0] = '/'; root->name[1] = 0;
    root->type = FT_Directory;
    SerialLogger::Log("VFS.3\r\n");
    root->finddir = [](FileNode* node, const char* name) -> FileNode* {
        (void)node;
        if (!name || !name[0]) return nullptr;
        if (name_eq(name, "input.log")) return GetInputLogNode();
        return nullptr;
    };
    SerialLogger::Log("VFS.4\r\n");

    for (size_t i = 0; i < sizeof(ramdisk_buffer); i++) ramdisk_buffer[i] = 0;
    SerialLogger::Log("VFS: Initialized with RAMDisk support\r\n");
}

FileNode* VFS::Open(const char* path) {
    if (!path || !root) return nullptr;
    if (path[0] == 0) return nullptr;
    if (path[0] == '/' && path[1] == 0) return root;
    if (path[0] != '/') return nullptr;

    // iterative path walk to avoid recursion / stack blowouts
    FileNode* cur = root;
    int i = 1;
    char comp[64];
    while (path[i]) {
        int ci = 0;
        while (path[i] && path[i] != '/') {
            if (ci < (int)sizeof(comp) - 1) comp[ci++] = path[i];
            i++;
        }
        comp[ci] = 0;
        if (path[i] == '/') i++;
        if (ci == 0) continue;
        if (!cur || !cur->finddir) return nullptr;
        cur = cur->finddir(cur, comp);
        if (!cur) return nullptr;
    }
    return cur;
}

int VFS::Read(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->read || !buffer) return 0;
    return node->read(node, offset, size, buffer);
}

int VFS::Write(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->write || !buffer) return 0;
    return node->write(node, offset, size, buffer);
}

void VFS::Close(FileNode* node) {
    if (node && node->close) node->close(node);
}

bool VFS::Mount(const char* path, FileNode* fs_root) {
    (void)path; (void)fs_root;
    return true;
}
