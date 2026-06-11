// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Device Bridge — Implementation
// ═══════════════════════════════════════════════════════════════════════════

#include "linux_devices.h"
#include "linux_kernel.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"

// ─── Static storage ──────────────────────────────────────────────────────

LinuxDevice   LinuxDeviceBridge::devices[LDEV_MAX_DEVICES];
int           LinuxDeviceBridge::device_count = 0;
LinuxFBInfo   LinuxDeviceBridge::fb_info;
LinuxInputEvent LinuxDeviceBridge::input_queue[64];
int           LinuxDeviceBridge::input_head = 0;
int           LinuxDeviceBridge::input_tail = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────

static void ld_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool ld_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════════════════════════

void LinuxDeviceBridge::Init() {
    memset(devices, 0, sizeof(devices));
    device_count = 0;
    input_head = input_tail = 0;

    // Init framebuffer info
    fb_info.xres = 1024;
    fb_info.yres = 768;
    fb_info.xres_virtual = 1024;
    fb_info.yres_virtual = 768;
    fb_info.bits_per_pixel = 32;
    fb_info.line_length = 1024 * 4;
    fb_info.smem_start = 0xFD000000;  // BGA framebuffer address
    fb_info.smem_len = 1024 * 768 * 4;

    RegisterDefaults();
    PopulateDevFS();

    SerialLogger::Log("[LinuxDevBridge] Device bridge initialized (");
    SerialLogger::LogDec(device_count);
    SerialLogger::Log(" devices)\r\n");
}

void LinuxDeviceBridge::RegisterDefaults() {
    // ── Character devices ──
    int idx;

    // /dev/null (1, 3)
    idx = Register("null", "/dev/null", LDEV_CHAR, LDEV_CLASS_NULL, 1, 3);
    if (idx >= 0) {
        devices[idx].read = ReadNull;
        devices[idx].write = WriteNull;
    }

    // /dev/zero (1, 5)
    idx = Register("zero", "/dev/zero", LDEV_CHAR, LDEV_CLASS_ZERO, 1, 5);
    if (idx >= 0) {
        devices[idx].read = ReadZero;
        devices[idx].write = WriteNull;
    }

    // /dev/random (1, 8)
    idx = Register("random", "/dev/random", LDEV_CHAR, LDEV_CLASS_RANDOM, 1, 8);
    if (idx >= 0) {
        devices[idx].read = ReadRandom;
        devices[idx].write = WriteNull;
    }

    // /dev/urandom (1, 9)
    idx = Register("urandom", "/dev/urandom", LDEV_CHAR, LDEV_CLASS_RANDOM, 1, 9);
    if (idx >= 0) {
        devices[idx].read = ReadRandom;
        devices[idx].write = WriteNull;
    }

    // /dev/tty (5, 0) — current terminal
    Register("tty", "/dev/tty", LDEV_CHAR, LDEV_CLASS_TTY, 5, 0);

    // /dev/console (5, 1)
    Register("console", "/dev/console", LDEV_CHAR, LDEV_CLASS_CONSOLE, 5, 1);

    // /dev/tty0 (4, 0)
    Register("tty0", "/dev/tty0", LDEV_CHAR, LDEV_CLASS_CONSOLE, 4, 0);

    // /dev/ttyS0 (4, 64) — serial port
    Register("ttyS0", "/dev/ttyS0", LDEV_CHAR, LDEV_CLASS_SERIAL, 4, 64);

    // /dev/fb0 (29, 0) — framebuffer
    idx = Register("fb0", "/dev/fb0", LDEV_CHAR, LDEV_CLASS_FRAMEBUFFER, 29, 0);
    if (idx >= 0) {
        devices[idx].read = ReadFB;
        devices[idx].write = WriteFB;
        devices[idx].ioctl = IoctlFB;
    }

    // /dev/input/mice (13, 63)
    idx = Register("mice", "/dev/input/mice", LDEV_CHAR, LDEV_CLASS_INPUT_MOUSE, 13, 63);
    if (idx >= 0) {
        devices[idx].read = ReadInput;
    }

    // /dev/input/event0 (13, 64) — keyboard
    idx = Register("event0", "/dev/input/event0", LDEV_CHAR, LDEV_CLASS_INPUT_KBD, 13, 64);
    if (idx >= 0) {
        devices[idx].read = ReadInput;
    }

    // ── Block devices ──

    // /dev/sda (8, 0) — whole disk
    Register("sda", "/dev/sda", LDEV_BLOCK, LDEV_CLASS_DISK, 8, 0);

    // /dev/sda1 (8, 1) — Kurono partition
    Register("sda1", "/dev/sda1", LDEV_BLOCK, LDEV_CLASS_DISK_PART, 8, 1);

    // /dev/sda2 (8, 2) — Linux partition (ext4)
    Register("sda2", "/dev/sda2", LDEV_BLOCK, LDEV_CLASS_DISK_PART, 8, 2);

    // /dev/loop0 (7, 0)
    Register("loop0", "/dev/loop0", LDEV_BLOCK, LDEV_CLASS_LOOP, 7, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

int LinuxDeviceBridge::Register(const char* name, const char* path,
                                 LinuxDevType type, LinuxDevClass cls,
                                 int major, int minor) {
    if (device_count >= LDEV_MAX_DEVICES) return -1;
    int idx = device_count++;
    LinuxDevice* d = &devices[idx];
    ld_scpy(d->name, name, sizeof(d->name));
    ld_scpy(d->path, path, sizeof(d->path));
    d->type = type;
    d->dev_class = cls;
    d->major = major;
    d->minor = minor;
    d->active = true;
    d->read = nullptr;
    d->write = nullptr;
    d->ioctl = nullptr;
    return idx;
}

void LinuxDeviceBridge::Unregister(const char* path) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].active && ld_seq(devices[i].path, path)) {
            devices[i].active = false;
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Device I/O
// ═══════════════════════════════════════════════════════════════════════════

int LinuxDeviceBridge::Read(const char* path, void* buf, uint32_t offset, uint32_t len) {
    LinuxDevice* d = FindByPath(path);
    if (!d || !d->active) return -1;
    if (d->read) return d->read(buf, offset, len);
    return -1;
}

int LinuxDeviceBridge::Write(const char* path, const void* buf, uint32_t offset, uint32_t len) {
    LinuxDevice* d = FindByPath(path);
    if (!d || !d->active) return -1;
    if (d->write) return d->write(buf, offset, len);
    return -1;
}

int LinuxDeviceBridge::Ioctl(const char* path, uint32_t cmd, uint32_t arg) {
    LinuxDevice* d = FindByPath(path);
    if (!d || !d->active) return -1;
    if (d->ioctl) return d->ioctl(cmd, arg);
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Lookup
// ═══════════════════════════════════════════════════════════════════════════

LinuxDevice* LinuxDeviceBridge::FindByPath(const char* path) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].active && ld_seq(devices[i].path, path))
            return &devices[i];
    }
    return nullptr;
}

LinuxDevice* LinuxDeviceBridge::FindByMajorMinor(int major, int minor) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].active && devices[i].major == major && devices[i].minor == minor)
            return &devices[i];
    }
    return nullptr;
}

bool LinuxDeviceBridge::Exists(const char* path) {
    return FindByPath(path) != nullptr;
}

LinuxDevice* LinuxDeviceBridge::GetDevices() { return devices; }
int LinuxDeviceBridge::GetDeviceCount() { return device_count; }
LinuxFBInfo* LinuxDeviceBridge::GetFramebufferInfo() { return &fb_info; }

// ═══════════════════════════════════════════════════════════════════════════
//  Built-in device handlers
// ═══════════════════════════════════════════════════════════════════════════

int LinuxDeviceBridge::ReadNull(void*, uint32_t, uint32_t) {
    return 0;  // EOF
}

int LinuxDeviceBridge::WriteNull(const void*, uint32_t, uint32_t len) {
    return (int)len;  // Swallow all data
}

int LinuxDeviceBridge::ReadZero(void* buf, uint32_t, uint32_t len) {
    memset(buf, 0, len);
    return (int)len;
}

int LinuxDeviceBridge::ReadRandom(void* buf, uint32_t, uint32_t len) {
    return LinuxKernel::GetRandom(buf, len);
}

int LinuxDeviceBridge::ReadFB(void* buf, uint32_t offset, uint32_t len) {
    // Read from framebuffer memory
    uint32_t fb_size = fb_info.smem_len;
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size) len = fb_size - offset;

    uint8_t* fb = (uint8_t*)(uintptr_t)fb_info.smem_start;
    memcpy(buf, fb + offset, len);
    return (int)len;
}

int LinuxDeviceBridge::WriteFB(const void* buf, uint32_t offset, uint32_t len) {
    uint32_t fb_size = fb_info.smem_len;
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size) len = fb_size - offset;

    uint8_t* fb = (uint8_t*)(uintptr_t)fb_info.smem_start;
    memcpy(fb + offset, buf, len);
    return (int)len;
}

int LinuxDeviceBridge::IoctlFB(uint32_t cmd, uint32_t arg) {
    // FBIOGET_VSCREENINFO = 0x4600
    // FBIOGET_FSCREENINFO = 0x4602
    (void)cmd;
    (void)arg;
    return 0;
}

int LinuxDeviceBridge::ReadInput(void* buf, uint32_t, uint32_t len) {
    int event_size = sizeof(LinuxInputEvent);
    int events_wanted = (int)(len / event_size);
    int events_read = 0;

    LinuxInputEvent* out = (LinuxInputEvent*)buf;
    while (events_read < events_wanted && input_head != input_tail) {
        memcpy(&out[events_read], &input_queue[input_tail], event_size);
        input_tail = (input_tail + 1) % 64;
        events_read++;
    }
    return events_read * event_size;
}

int LinuxDeviceBridge::IoctlTTY(uint32_t cmd, uint32_t arg) {
    (void)cmd;
    (void)arg;
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Input events
// ═══════════════════════════════════════════════════════════════════════════

void LinuxDeviceBridge::QueueMouseEvent(int dx, int dy, int buttons) {
    int next = (input_head + 1) % 64;
    if (next == input_tail) return;  // Full

    LinuxInputEvent* ev = &input_queue[input_head];
    ev->tv_sec = LinuxKernel::GetUptime();
    ev->tv_usec = 0;
    ev->type = EV_REL;
    ev->code = 0;  // REL_X
    ev->value = dx;
    input_head = next;

    next = (input_head + 1) % 64;
    if (next != input_tail) {
        ev = &input_queue[input_head];
        ev->tv_sec = LinuxKernel::GetUptime();
        ev->tv_usec = 0;
        ev->type = EV_REL;
        ev->code = 1;  // REL_Y
        ev->value = dy;
        input_head = next;
    }

    // Button state
    next = (input_head + 1) % 64;
    if (next != input_tail) {
        ev = &input_queue[input_head];
        ev->tv_sec = LinuxKernel::GetUptime();
        ev->tv_usec = 0;
        ev->type = EV_KEY;
        ev->code = 0x110;  // BTN_LEFT
        ev->value = buttons & 1;
        input_head = next;
    }
}

void LinuxDeviceBridge::QueueKeyEvent(int keycode, bool pressed) {
    int next = (input_head + 1) % 64;
    if (next == input_tail) return;

    LinuxInputEvent* ev = &input_queue[input_head];
    ev->tv_sec = LinuxKernel::GetUptime();
    ev->tv_usec = 0;
    ev->type = EV_KEY;
    ev->code = (uint16_t)keycode;
    ev->value = pressed ? 1 : 0;
    input_head = next;

    // SYN event
    next = (input_head + 1) % 64;
    if (next != input_tail) {
        ev = &input_queue[input_head];
        ev->type = EV_SYN;
        ev->code = 0;
        ev->value = 0;
        input_head = next;
    }
}

int LinuxDeviceBridge::ReadInputEvents(LinuxInputEvent* events, int max_events) {
    int count = 0;
    while (count < max_events && input_head != input_tail) {
        memcpy(&events[count], &input_queue[input_tail], sizeof(LinuxInputEvent));
        input_tail = (input_tail + 1) % 64;
        count++;
    }
    return count;
}

int LinuxDeviceBridge::PendingInputEvents() {
    if (input_head >= input_tail)
        return input_head - input_tail;
    return 64 - input_tail + input_head;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Populate /dev tree
// ═══════════════════════════════════════════════════════════════════════════

void LinuxDeviceBridge::PopulateDevFS() {
    KVFS::Mkdirs("/dev");
    KVFS::Mkdirs("/dev/input");
    KVFS::Mkdirs("/dev/pts");
    KVFS::Mkdirs("/dev/shm");
    KVFS::Mkdirs("/dev/mqueue");

    // Create device nodes as special files in KVFS
    for (int i = 0; i < device_count; i++) {
        if (!devices[i].active) continue;
        // Create the file path in KVFS
        KVFS::CreateFile(devices[i].path);
    }

    // Create symlinks
    KVFS::CreateFile("/dev/stdin");
    KVFS::CreateFile("/dev/stdout");
    KVFS::CreateFile("/dev/stderr");
    KVFS::CreateFile("/dev/fd");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Status
// ═══════════════════════════════════════════════════════════════════════════

void LinuxDeviceBridge::DumpDevices(char* out, int max_out) {
    int p = 0;
    auto put = [&](const char* s) { while (*s && p < max_out - 1) out[p++] = *s++; };
    auto putd = [&](int v) {
        char tmp[12]; int i = 0;
        if (v == 0) { out[p++] = '0'; return; }
        while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
        for (int j = i - 1; j >= 0 && p < max_out - 1; j--) out[p++] = tmp[j];
    };

    put("Linux Device Bridge — ");
    putd(device_count);
    put(" devices\n\n");

    for (int i = 0; i < device_count; i++) {
        if (!devices[i].active) continue;
        put("  ");
        put(devices[i].path);
        put("\t(");
        putd(devices[i].major);
        put(", ");
        putd(devices[i].minor);
        put(") ");
        put(devices[i].type == LDEV_CHAR ? "char" : "block");
        put("\n");
    }
    out[p] = 0;
}
