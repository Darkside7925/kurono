#include "vfs.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

FileNode* VFS::root = nullptr;
static uint8_t ramdisk_buffer[1024 * 16]; // 16kb log buffer
static uint32_t ramdisk_size = 0;

int RamDiskRead(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)node;
    if (offset >= ramdisk_size) return 0;
    if (offset + size > ramdisk_size) size = ramdisk_size - offset;
    
    for(uint32_t i=0; i<size; i++) {
        buffer[i] = ramdisk_buffer[offset + i];
    }
    return size;
}

int RamDiskWrite(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)node;
    if (offset + size > sizeof(ramdisk_buffer)) {
        // simple circular buffer or cap
        size = sizeof(ramdisk_buffer) - offset;
    }
    
    for(uint32_t i=0; i<size; i++) {
        ramdisk_buffer[offset + i] = buffer[i];
    }
    if (offset + size > ramdisk_size) ramdisk_size = offset + size;
    return size;
}

void VFS::Init() {
    SerialLogger::Log("VFS.1\r\n");
    // create root node
    root = (FileNode*)KernelHeap::Alloc(sizeof(FileNode));
    SerialLogger::Log("VFS.2\r\n");
    if (!root) {
        SerialLogger::Log("VFS: root is NULL!\r\n");
        return;
    }
    root->name[0] = '/'; root->name[1] = 0;
    root->type = FT_Directory;
    root->flags = 0;
    root->inode = 0;
    root->size = 0;
    root->read = nullptr;
    root->write = nullptr;
    root->open = nullptr;
    root->close = nullptr;
    SerialLogger::Log("VFS.3\r\n");
    root->finddir = [](FileNode* node, const char* name) -> FileNode* {
        (void)node;
        // mock lookup
        if (name[0] == 'i' && name[1] == 'n') { // "input.log"
             FileNode* f = (FileNode*)KernelHeap::Alloc(sizeof(FileNode));
             int i=0; while(name[i]) { f->name[i]=name[i]; i++; } f->name[i]=0;
             f->type = FT_File;
             f->size = ramdisk_size;
             f->read = RamDiskRead;
             f->write = RamDiskWrite;
             return f;
        }
        return nullptr;
    };
    SerialLogger::Log("VFS.4\r\n");
    
    // clear ramdisk
    for(size_t i=0; i<sizeof(ramdisk_buffer); i++) ramdisk_buffer[i] = 0;
    SerialLogger::Log("VFS: Initialized with RAMDisk support\r\n");
}

FileNode* VFS::Open(const char* path) {
    // simplified path traversal
    if (!path || !root) return nullptr;
    if (path[0] == '/' && path[1] == 0) return root;
    
    // hack for input.log
    if (path[0] == '/' && path[1] == 'i') {
        return root->finddir(root, path+1);
    }
    
    return nullptr; 
}

int VFS::Read(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return 0;
}

int VFS::Write(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}

void VFS::Close(FileNode* node) {
    if (node && node->close) {
        node->close(node);
    }
}

bool VFS::Mount(const char* path, FileNode* fs_root) {
    (void)path; (void)fs_root;
    // mount logic stub
    return true;
}
