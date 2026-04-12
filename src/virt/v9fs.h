//  kurono os  -  virtual 9p filesystem (host ↔ guest shared fs)
//
//  implements a subset of 9p2000.l over the vmcall interface so that a
//  guest vm can transparently access files on the host kvfs.
//
//  the guest issues vmcall 0x20 with a sub-function code in ecx and
//  arguments in other registers.  the host processes the request and
//  returns results via the guest's register file.
//
//  fid table: up to 64 open file handles, each associated with a
//  path on kvfs.  fid 0 is always the root ("/").
#pragma once
#include <stdint.h>
#include <stddef.h>

// these mirror 9p2000.l opcodes but are dispatched via vmcall, not virtio.
enum V9FS_Op {
    V9FS_VERSION  = 0,   // negotiate protocol version
    V9FS_ATTACH   = 1,   // attach to root, returns root fid
    V9FS_WALK     = 2,   // walk from a fid to a child path element
    V9FS_OPEN     = 3,   // open a fid for i/o
    V9FS_READ     = 4,   // read data from an open fid
    V9FS_WRITE    = 5,   // write data to an open fid
    V9FS_CLUNK    = 6,   // close / release a fid
    V9FS_STAT     = 7,   // get attributes of a fid
    V9FS_READDIR  = 8,   // read directory entries from a fid
    V9FS_CREATE   = 9,   // create file/directory under a fid
    V9FS_REMOVE   = 10,  // remove the file/directory at a fid
};

enum V9FS_Err {
    V9FS_OK        = 0,
    V9FS_ENOENT    = 2,   // no such file
    V9FS_EIO       = 5,   // i/o error
    V9FS_EBADF     = 9,   // bad fid
    V9FS_ENOMEM    = 12,  // out of memory / fids
    V9FS_EEXIST    = 17,  // file exists
    V9FS_ENOTDIR   = 20,  // not a directory
    V9FS_EISDIR    = 21,  // is a directory (can't read as file)
    V9FS_EINVAL    = 22,  // invalid argument
    V9FS_ENOSPC    = 28,  // no space / table full
    V9FS_ENOSYS    = 38,  // not implemented
};

constexpr uint32_t V9FS_TYPE_FILE  = 0;
constexpr uint32_t V9FS_TYPE_DIR   = 1;
constexpr uint32_t V9FS_TYPE_LINK  = 2;

constexpr uint32_t V9FS_OREAD   = 0;
constexpr uint32_t V9FS_OWRITE  = 1;
constexpr uint32_t V9FS_ORDWR   = 2;
constexpr uint32_t V9FS_OTRUNC  = 0x10;
constexpr uint32_t V9FS_OAPPEND = 0x20;

constexpr int V9FS_MAX_FIDS     = 64;
constexpr int V9FS_MAX_PATH     = 256;
constexpr int V9FS_MAX_IOSIZE   = 4096;  // max single i/o transfer

struct V9FS_Fid {
    bool     active;
    bool     is_open;
    bool     is_dir;
    uint32_t mode;           // open mode
    uint32_t offset;         // current file offset
    uint32_t dir_offset;     // directory enumeration position
    char     path[V9FS_MAX_PATH];
};

struct V9FS_Stat {
    uint32_t type;           // v9fs_type_*
    uint32_t size;           // file size in bytes
    uint32_t mode;           // permission bits
    uint32_t mtime;          // last modification time (epoch seconds)
    char     name[64];       // basename
};

//  v9fs  -  virtual 9p filesystem handler
class V9FS {
public:
    static void Init();
    static bool IsInitialized() { return initialized; }

    // cpu->regs[1] = ecx = v9fs_op sub-function
    // other registers carry operation-specific args.
    // returns v9fs_err in cpu->regs[0] (eax).
    static void HandleVMCall(uint64_t* regs);

private:
    static bool initialized;
    static V9FS_Fid fids[V9FS_MAX_FIDS];

    static int  AllocFid();
    static void FreeFid(int fid);
    static bool ValidFid(int fid);

    static int  DoVersion(uint64_t* regs);
    static int  DoAttach(uint64_t* regs);
    static int  DoWalk(uint64_t* regs);
    static int  DoOpen(uint64_t* regs);
    static int  DoRead(uint64_t* regs);
    static int  DoWrite(uint64_t* regs);
    static int  DoClunk(uint64_t* regs);
    static int  DoStat(uint64_t* regs);
    static int  DoReadDir(uint64_t* regs);
    static int  DoCreate(uint64_t* regs);
    static int  DoRemove(uint64_t* regs);
};
