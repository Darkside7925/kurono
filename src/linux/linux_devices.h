#pragma once
//  kurono os  -  linux device bridge
//  shares hardware devices between kurono and linux subsystem.
//  creates /dev nodes that map to kurono's native drivers.
//
//  linux programs see standard /dev entries:
//    /dev/null, /dev/zero, /dev/random, /dev/urandom
//    /dev/tty, /dev/console, /dev/ttys0, /dev/pts/*
//    /dev/sda, /dev/sda1, /dev/sda2
//    /dev/fb0 (framebuffer)
//    /dev/input/mice, /dev/input/event0

#include "../kernel/types.h"

#define LDEV_MAX_DEVICES    32

enum LinuxDevType {
    LDEV_CHAR = 0,        // character device
    LDEV_BLOCK = 1,       // block device
    LDEV_PIPE  = 2        // named pipe
};

enum LinuxDevClass {
    LDEV_CLASS_NULL = 0,      // /dev/null
    LDEV_CLASS_ZERO,          // /dev/zero
    LDEV_CLASS_RANDOM,        // /dev/random, /dev/urandom
    LDEV_CLASS_TTY,           // /dev/tty*, /dev/pts/*
    LDEV_CLASS_CONSOLE,       // /dev/console
    LDEV_CLASS_SERIAL,        // /dev/ttys*
    LDEV_CLASS_FRAMEBUFFER,   // /dev/fb0
    LDEV_CLASS_INPUT_MOUSE,   // /dev/input/mice
    LDEV_CLASS_INPUT_KBD,     // /dev/input/event0
    LDEV_CLASS_DISK,          // /dev/sda
    LDEV_CLASS_DISK_PART,     // /dev/sda1, /dev/sda2
    LDEV_CLASS_LOOP,          // /dev/loop*
    LDEV_CLASS_MEM            // /dev/mem, /dev/kmem
};

struct LinuxDevice {
    char            name[32];      // e.g., "null", "sda1"
    char            path[64];      // full path: "/dev/null"
    LinuxDevType    type;
    LinuxDevClass   dev_class;
    int             major;
    int             minor;
    bool            active;

    // i/o function pointers
    int (*read)(void* buf, uint32_t offset, uint32_t len);
    int (*write)(const void* buf, uint32_t offset, uint32_t len);
    int (*ioctl)(uint32_t cmd, uint32_t arg);
};

struct LinuxFBInfo {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t bits_per_pixel;
    uint32_t line_length;
    uint32_t smem_start;     // physical address of framebuffer
    uint32_t smem_len;       // length in bytes
};

struct LinuxInputEvent {
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed));

// input event types
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03

//  linuxdevicebridge  -  the device management class

class LinuxDeviceBridge {
public:
    static void Init();
    static void RegisterDefaults();

    // device registration
    static int  Register(const char* name, const char* path,
                          LinuxDevType type, LinuxDevClass cls,
                          int major, int minor);
    static void Unregister(const char* path);

    // device i/o
    static int  Read(const char* path, void* buf, uint32_t offset, uint32_t len);
    static int  Write(const char* path, const void* buf, uint32_t offset, uint32_t len);
    static int  Ioctl(const char* path, uint32_t cmd, uint32_t arg);

    // lookup
    static LinuxDevice* FindByPath(const char* path);
    static LinuxDevice* FindByMajorMinor(int major, int minor);
    static bool Exists(const char* path);

    // query
    static LinuxDevice* GetDevices();
    static int  GetDeviceCount();

    // framebuffer
    static LinuxFBInfo* GetFramebufferInfo();
    static bool BlitFramebufferRect(const void* buf, uint32_t src_pitch,
                                     uint32_t width, uint32_t height,
                                     int dst_x, int dst_y);
    static void PresentFramebuffer();

    // input events
    static void QueueMouseEvent(int dx, int dy, int buttons);
    static void QueueKeyEvent(int keycode, bool pressed);
    static int  ReadInputEvents(LinuxInputEvent* events, int max_events);
    static int  PendingInputEvents();

    // create /dev tree in kvfs
    static void PopulateDevFS();

    // status
    static void DumpDevices(char* out, int max_out);

private:
    static LinuxDevice devices[LDEV_MAX_DEVICES];
    static int device_count;
    static LinuxFBInfo fb_info;

    // input event queue
    static LinuxInputEvent input_queue[64];
    static int input_head, input_tail;

    // built-in device handlers
    static int ReadNull(void* buf, uint32_t offset, uint32_t len);
    static int WriteNull(const void* buf, uint32_t offset, uint32_t len);
    static int ReadZero(void* buf, uint32_t offset, uint32_t len);
    static int ReadRandom(void* buf, uint32_t offset, uint32_t len);
    static int ReadFB(void* buf, uint32_t offset, uint32_t len);
    static int WriteFB(const void* buf, uint32_t offset, uint32_t len);
    static int IoctlFB(uint32_t cmd, uint32_t arg);
    static int ReadInput(void* buf, uint32_t offset, uint32_t len);
    static int IoctlTTY(uint32_t cmd, uint32_t arg);
};
