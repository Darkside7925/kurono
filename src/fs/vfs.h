#pragma once
#include "../kernel/types.h"

// virtual file system architecture
// provides a unified interface for file operations across different physical file systems.

enum FileType {
    FT_File,
    FT_Directory,
    FT_Device,
    FT_Pipe
};

struct FileNode {
    char name[64];
    uint32_t size;
    FileType type;
    uint32_t flags;
    uint32_t inode;
    
    // function pointers for operations (vtable equivalent)
    int (*read)(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    int (*write)(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    void (*open)(FileNode* node);
    void (*close)(FileNode* node);
    FileNode* (*finddir)(FileNode* node, const char* name);
};

class VFS {
public:
    static FileNode* root;
    
    static void Init();
    static FileNode* Open(const char* path);
    static int Read(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    static int Write(FileNode* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    static void Close(FileNode* node);
    
    // mounting
    static bool Mount(const char* path, FileNode* fs_root);
};
