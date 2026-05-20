//  kurono os  -  linux device bridge  -  implementation

#include "linux_devices.h"
#include "linux_kernel.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../kernel/heap.h"
#include "../ui/window_manager.h"

LinuxDevice   LinuxDeviceBridge::devices[LDEV_MAX_DEVICES];
int           LinuxDeviceBridge::device_count = 0;
LinuxFBInfo   LinuxDeviceBridge::fb_info;
LinuxInputEvent LinuxDeviceBridge::input_queue[64];
int           LinuxDeviceBridge::input_head = 0;
int           LinuxDeviceBridge::input_tail = 0;

static void ld_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool ld_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

struct LinuxDisplaySurface {
    int window_id;
    uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    bool has_content;
    bool popped_up;
};

static LinuxDisplaySurface g_linux_display = {};
static int g_linux_guest_x = 0;
static int g_linux_guest_y = 0;
static bool g_linux_guest_pos_valid = false;
static int g_linux_button_mask = 0;
static int g_linux_emitted_button_mask = 0;

static uint32_t ld_min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t ld_bytes_per_pixel(uint32_t bpp) {
    return bpp == 0 ? 4u : ((bpp + 7) / 8);
}

static int ld_clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int ld_button_index_to_mask(int button) {
    static const int masks[] = {0x01, 0x02, 0x04, 0x08, 0x10};
    if (button < 0 || button >= (int)(sizeof(masks) / sizeof(masks[0]))) return 0;
    return masks[button];
}

static bool ld_map_local_to_guest(Window* win, int local_x, int local_y,
                                  int* guest_x, int* guest_y) {
    if (!win || !guest_x || !guest_y || g_linux_display.width == 0 ||
        g_linux_display.height == 0) {
        return false;
    }

    int draw_w = win->content_w;
    int draw_h = win->content_h;
    if ((uint64_t)g_linux_display.width * (uint64_t)win->content_h >
        (uint64_t)g_linux_display.height * (uint64_t)win->content_w) {
        draw_h = (int)(((uint64_t)win->content_w * g_linux_display.height) /
                       g_linux_display.width);
    } else {
        draw_w = (int)(((uint64_t)win->content_h * g_linux_display.width) /
                       g_linux_display.height);
    }

    if (draw_w <= 0 || draw_h <= 0) return false;

    int draw_x = (win->content_w - draw_w) / 2;
    int draw_y = (win->content_h - draw_h) / 2;
    int rel_x = ld_clamp_int(local_x - draw_x, 0, draw_w - 1);
    int rel_y = ld_clamp_int(local_y - draw_y, 0, draw_h - 1);

    *guest_x = (int)(((uint64_t)rel_x * g_linux_display.width) / (uint32_t)draw_w);
    *guest_y = (int)(((uint64_t)rel_y * g_linux_display.height) / (uint32_t)draw_h);
    if (*guest_x >= (int)g_linux_display.width) *guest_x = (int)g_linux_display.width - 1;
    if (*guest_y >= (int)g_linux_display.height) *guest_y = (int)g_linux_display.height - 1;
    return true;
}

static void ld_write_dec(char*& dst, int& remaining, uint32_t value) {
    char tmp[16];
    int len = 0;
    if (value == 0) {
        if (remaining > 1) {
            *dst++ = '0';
            remaining--;
        }
        return;
    }
    while (value > 0 && len < 15) {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (len > 0 && remaining > 1) {
        *dst++ = tmp[--len];
        remaining--;
    }
}

static void ld_update_window_title() {
    if (g_linux_display.window_id <= 0) return;

    char title[64];
    char* dst = title;
    int remaining = (int)sizeof(title);
    const char* prefix = "Linux Guest ";
    while (*prefix && remaining > 1) {
        *dst++ = *prefix++;
        remaining--;
    }
    ld_write_dec(dst, remaining, g_linux_display.width);
    if (remaining > 1) {
        *dst++ = 'x';
        remaining--;
    }
    ld_write_dec(dst, remaining, g_linux_display.height);
    *dst = 0;

    WindowManager::SetTitle(g_linux_display.window_id, title);
}

static bool ld_resize_surface(uint32_t width, uint32_t height, uint32_t bpp) {
    if (width == 0 || height == 0) return false;

    uint32_t bytes_per_pixel = ld_bytes_per_pixel(bpp);
    if (bytes_per_pixel != 4) return false;

    uint32_t pitch = width * bytes_per_pixel;
    uint64_t size64 = (uint64_t)pitch * height;
    if (size64 == 0 || size64 > 64ULL * 1024ULL * 1024ULL) return false;

    uint32_t size = (uint32_t)size64;
    if (g_linux_display.pixels && g_linux_display.width == width &&
        g_linux_display.height == height && g_linux_display.pitch == pitch &&
        g_linux_display.bpp == bpp) {
        return true;
    }

    uint8_t* new_pixels = (uint8_t*)KernelHeap::Alloc(size);
    if (!new_pixels) return false;
    memset(new_pixels, 0, size);

    if (g_linux_display.pixels) {
        uint32_t rows = ld_min_u32(height, g_linux_display.height);
        uint32_t row_bytes = ld_min_u32(pitch, g_linux_display.pitch);
        for (uint32_t row = 0; row < rows; row++) {
            memcpy(new_pixels + row * pitch,
                   g_linux_display.pixels + row * g_linux_display.pitch,
                   row_bytes);
        }
        KernelHeap::Free(g_linux_display.pixels);
    }

    g_linux_display.pixels = new_pixels;
    g_linux_display.width = width;
    g_linux_display.height = height;
    g_linux_display.pitch = pitch;
    g_linux_display.bpp = bpp;
    g_linux_display.size = size;

    LinuxDeviceBridge::GetFramebufferInfo()->xres = width;
    LinuxDeviceBridge::GetFramebufferInfo()->yres = height;
    LinuxDeviceBridge::GetFramebufferInfo()->xres_virtual = width;
    LinuxDeviceBridge::GetFramebufferInfo()->yres_virtual = height;
    LinuxDeviceBridge::GetFramebufferInfo()->bits_per_pixel = bpp;
    LinuxDeviceBridge::GetFramebufferInfo()->line_length = pitch;
    LinuxDeviceBridge::GetFramebufferInfo()->smem_len = size;

    ld_update_window_title();
    return true;
}

static void ld_render_surface(Window* win, int cx, int cy, int cw, int ch) {
    (void)win;

    Graphics::FillRect(cx, cy, cw, ch, 0xFF0A0A12);

    if (!g_linux_display.pixels || !g_linux_display.has_content ||
        g_linux_display.width == 0 || g_linux_display.height == 0 ||
        g_linux_display.bpp != 32) {
        Graphics::DrawString(cx + 16, cy + 18, "Linux guest display idle", 0xFFE8E8F0, 0x00000000);
        Graphics::DrawString(cx + 16, cy + 42, "Waiting for framebuffer updates", 0xFF9090A4, 0x00000000);
        return;
    }

    int draw_w = cw;
    int draw_h = ch;
    if ((uint64_t)g_linux_display.width * (uint64_t)ch >
        (uint64_t)g_linux_display.height * (uint64_t)cw) {
        draw_h = (int)(((uint64_t)cw * g_linux_display.height) / g_linux_display.width);
    } else {
        draw_w = (int)(((uint64_t)ch * g_linux_display.width) / g_linux_display.height);
    }

    if (draw_w <= 0 || draw_h <= 0) return;

    int draw_x = cx + (cw - draw_w) / 2;
    int draw_y = cy + (ch - draw_h) / 2;
    Graphics::FillRect(draw_x - 1, draw_y - 1, draw_w + 2, draw_h + 2, 0xFF202034);

    uint8_t* dst = Graphics::GetBackBuffer();
    if (!dst) dst = Graphics::GetBuffer();

    if (dst && Graphics::GetBpp() == 32 && draw_w == (int)g_linux_display.width &&
        draw_h == (int)g_linux_display.height) {
        uint32_t dst_pitch = Graphics::GetPitch();
        for (uint32_t row = 0; row < g_linux_display.height; row++) {
            memcpy(dst + (uint32_t)(draw_y + (int)row) * dst_pitch + (uint32_t)draw_x * 4,
                   g_linux_display.pixels + row * g_linux_display.pitch,
                   g_linux_display.width * 4);
        }
        return;
    }

    for (int y = 0; y < draw_h; y++) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * g_linux_display.height) / (uint32_t)draw_h);
        const uint32_t* src_row = (const uint32_t*)(g_linux_display.pixels + src_y * g_linux_display.pitch);
        for (int x = 0; x < draw_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * g_linux_display.width) / (uint32_t)draw_w);
            Graphics::DrawPixel(draw_x + x, draw_y + y, src_row[src_x]);
        }
    }
}

static void ld_input_surface(Window* win, int event, int param1, int param2) {
    if (event == 2) {
        LinuxDeviceBridge::QueueKeyEvent(param1, true);
        LinuxDeviceBridge::QueueKeyEvent(param1, false);
    } else if (event == 5) {
        int guest_x = 0;
        int guest_y = 0;
        if (!ld_map_local_to_guest(win, param1, param2, &guest_x, &guest_y)) return;

        int dx = 0;
        int dy = 0;
        if (g_linux_guest_pos_valid) {
            dx = guest_x - g_linux_guest_x;
            dy = guest_y - g_linux_guest_y;
        }

        g_linux_guest_x = guest_x;
        g_linux_guest_y = guest_y;
        g_linux_guest_pos_valid = true;

        if (dx != 0 || dy != 0) {
            LinuxDeviceBridge::QueueMouseEvent(dx, dy, g_linux_button_mask);
        }
    } else if (event == 6) {
        int mask = ld_button_index_to_mask(param1);
        if (mask == 0) return;

        if (param2) g_linux_button_mask |= mask;
        else g_linux_button_mask &= ~mask;
        LinuxDeviceBridge::QueueMouseEvent(0, 0, g_linux_button_mask);
    }
}

static void ld_ensure_window() {
    Window* win = g_linux_display.window_id > 0
        ? WindowManager::GetWindow(g_linux_display.window_id)
        : nullptr;
    if (win) return;

    if (!g_linux_display.pixels) {
        if (!ld_resize_surface(LinuxDeviceBridge::GetFramebufferInfo()->xres,
                               LinuxDeviceBridge::GetFramebufferInfo()->yres,
                               LinuxDeviceBridge::GetFramebufferInfo()->bits_per_pixel)) {
            return;
        }
    }

    g_linux_display.popped_up = false;
    int window_w = (int)g_linux_display.width + WM_BORDER_W * 2;
    int window_h = (int)g_linux_display.height + WM_TITLEBAR_H + WM_BORDER_W;
    int id = WindowManager::CreateWindow("Linux Guest", -1, -1, window_w, window_h,
                                         ld_render_surface, ld_input_surface);
    if (id < 0) return;

    g_linux_display.window_id = id;
    win = WindowManager::GetWindow(id);
    if (win) {
        win->user_data = &g_linux_display;
        win->bg_color = 0xFF080810;
    }
    ld_update_window_title();
    SerialLogger::Log("[LinuxDevBridge] Linux guest display window created\r\n");
}

static void ld_present_surface() {
    ld_ensure_window();
    if (g_linux_display.window_id > 0) {
        WindowManager::SetVisible(g_linux_display.window_id, true);
        WindowManager::MarkDirty(g_linux_display.window_id);
        if (g_linux_display.has_content && !g_linux_display.popped_up) {
            WindowManager::BringToFront(g_linux_display.window_id);
            WindowManager::Focus(g_linux_display.window_id);
            g_linux_display.popped_up = true;
        }
    }
}

//  init

void LinuxDeviceBridge::Init() {
    memset(devices, 0, sizeof(devices));
    device_count = 0;
    input_head = input_tail = 0;
    memset(&g_linux_display, 0, sizeof(g_linux_display));
    g_linux_guest_x = 0;
    g_linux_guest_y = 0;
    g_linux_guest_pos_valid = false;
    g_linux_button_mask = 0;
    g_linux_emitted_button_mask = 0;

    // init framebuffer info
    fb_info.xres = 1024;
    fb_info.yres = 768;
    fb_info.xres_virtual = 1024;
    fb_info.yres_virtual = 768;
    fb_info.bits_per_pixel = 32;
    fb_info.line_length = 1024 * 4;
    fb_info.smem_start = 0xFD000000;  // bga framebuffer address
    fb_info.smem_len = 1024 * 768 * 4;

    RegisterDefaults();
    PopulateDevFS();

    SerialLogger::Log("[LinuxDevBridge] Device bridge initialized (");
    SerialLogger::LogDec(device_count);
    SerialLogger::Log(" devices)\r\n");
}

void LinuxDeviceBridge::RegisterDefaults() {
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

    // /dev/tty (5, 0)  -  current terminal
    Register("tty", "/dev/tty", LDEV_CHAR, LDEV_CLASS_TTY, 5, 0);

    // /dev/console (5, 1)
    Register("console", "/dev/console", LDEV_CHAR, LDEV_CLASS_CONSOLE, 5, 1);

    // /dev/tty0 (4, 0)
    Register("tty0", "/dev/tty0", LDEV_CHAR, LDEV_CLASS_CONSOLE, 4, 0);

    // /dev/ttys0 (4, 64)  -  serial port
    Register("ttyS0", "/dev/ttyS0", LDEV_CHAR, LDEV_CLASS_SERIAL, 4, 64);

    // /dev/fb0 (29, 0)  -  framebuffer
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

    // /dev/input/event0 (13, 64)  -  keyboard
    idx = Register("event0", "/dev/input/event0", LDEV_CHAR, LDEV_CLASS_INPUT_KBD, 13, 64);
    if (idx >= 0) {
        devices[idx].read = ReadInput;
    }

    // /dev/sda (8, 0)  -  whole disk
    Register("sda", "/dev/sda", LDEV_BLOCK, LDEV_CLASS_DISK, 8, 0);

    // /dev/sda1 (8, 1)  -  kurono partition
    Register("sda1", "/dev/sda1", LDEV_BLOCK, LDEV_CLASS_DISK_PART, 8, 1);

    // /dev/sda2 (8, 2)  -  linux partition (ext4)
    Register("sda2", "/dev/sda2", LDEV_BLOCK, LDEV_CLASS_DISK_PART, 8, 2);

    // /dev/loop0 (7, 0)
    Register("loop0", "/dev/loop0", LDEV_BLOCK, LDEV_CLASS_LOOP, 7, 0);
}

//  registration

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

//  device i/o

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

//  lookup

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

bool LinuxDeviceBridge::BlitFramebufferRect(const void* buf, uint32_t src_pitch,
                                            uint32_t width, uint32_t height,
                                            int dst_x, int dst_y) {
    if (!buf || width == 0 || height == 0) return false;

    uint32_t required_w = width;
    uint32_t required_h = height;
    if (dst_x > 0) required_w += (uint32_t)dst_x;
    if (dst_y > 0) required_h += (uint32_t)dst_y;

    uint32_t target_w = fb_info.xres_virtual;
    uint32_t target_h = fb_info.yres_virtual;
    if (required_w > target_w) target_w = required_w;
    if (required_h > target_h) target_h = required_h;

    if (!ld_resize_surface(target_w, target_h, fb_info.bits_per_pixel) ||
        !g_linux_display.pixels) {
        return false;
    }

    uint32_t bytes_per_pixel = ld_bytes_per_pixel(g_linux_display.bpp);
    for (uint32_t row = 0; row < height; row++) {
        int target_y = dst_y + (int)row;
        if (target_y < 0 || (uint32_t)target_y >= g_linux_display.height) continue;

        int target_x = dst_x;
        uint32_t copy_pixels = width;
        uint32_t src_offset_pixels = 0;

        if (target_x < 0) {
            src_offset_pixels = (uint32_t)(-target_x);
            if (src_offset_pixels >= copy_pixels) continue;
            copy_pixels -= src_offset_pixels;
            target_x = 0;
        }
        if ((uint32_t)target_x >= g_linux_display.width) continue;
        if ((uint32_t)target_x + copy_pixels > g_linux_display.width) {
            copy_pixels = g_linux_display.width - (uint32_t)target_x;
        }
        if (copy_pixels == 0) continue;

        uint8_t* dst_row = g_linux_display.pixels + (uint32_t)target_y * g_linux_display.pitch +
                           (uint32_t)target_x * bytes_per_pixel;
        const uint8_t* src_row = (const uint8_t*)buf + row * src_pitch +
                                 src_offset_pixels * bytes_per_pixel;
        memcpy(dst_row, src_row, copy_pixels * bytes_per_pixel);
    }

    g_linux_display.has_content = true;
    ld_present_surface();
    return true;
}

void LinuxDeviceBridge::PresentFramebuffer() {
    if (!g_linux_display.has_content) return;
    ld_present_surface();
}

//  built-in device handlers

int LinuxDeviceBridge::ReadNull(void*, uint32_t, uint32_t) {
    return 0;  // eof
}

int LinuxDeviceBridge::WriteNull(const void*, uint32_t, uint32_t len) {
    return (int)len;  // swallow all data
}

int LinuxDeviceBridge::ReadZero(void* buf, uint32_t, uint32_t len) {
    memset(buf, 0, len);
    return (int)len;
}

int LinuxDeviceBridge::ReadRandom(void* buf, uint32_t, uint32_t len) {
    return LinuxKernel::GetRandom(buf, len);
}

int LinuxDeviceBridge::ReadFB(void* buf, uint32_t offset, uint32_t len) {
    // read from framebuffer memory
    uint32_t fb_size = fb_info.smem_len;
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size) len = fb_size - offset;

    if (!ld_resize_surface(fb_info.xres_virtual, fb_info.yres_virtual, fb_info.bits_per_pixel) ||
        !g_linux_display.pixels) {
        memset(buf, 0, len);
        return (int)len;
    }

    memcpy(buf, g_linux_display.pixels + offset, len);
    return (int)len;
}

int LinuxDeviceBridge::WriteFB(const void* buf, uint32_t offset, uint32_t len) {
    uint32_t fb_size = fb_info.smem_len;
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size) len = fb_size - offset;

    if (!ld_resize_surface(fb_info.xres_virtual, fb_info.yres_virtual, fb_info.bits_per_pixel) ||
        !g_linux_display.pixels) {
        return -1;
    }

    memcpy(g_linux_display.pixels + offset, buf, len);
    g_linux_display.has_content = true;
    ld_present_surface();
    return (int)len;
}

int LinuxDeviceBridge::IoctlFB(uint32_t cmd, uint32_t arg) {
    // fbioget_vscreeninfo = 0x4600
    // fbioget_fscreeninfo = 0x4602
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

//  input events

void LinuxDeviceBridge::QueueMouseEvent(int dx, int dy, int buttons) {
    auto enqueue = [&](uint16_t type, uint16_t code, int32_t value) -> bool {
        int next = (input_head + 1) % 64;
        if (next == input_tail) return false;

        LinuxInputEvent* ev = &input_queue[input_head];
        ev->tv_sec = LinuxKernel::GetUptime();
        ev->tv_usec = 0;
        ev->type = type;
        ev->code = code;
        ev->value = value;
        input_head = next;
        return true;
    };

    bool queued = false;
    if (dx != 0) queued = enqueue(EV_REL, 0, dx) || queued;
    if (dy != 0) queued = enqueue(EV_REL, 1, dy) || queued;

    static const struct {
        int mask;
        uint16_t code;
    } button_map[] = {
        {0x01, 0x110},
        {0x02, 0x111},
        {0x04, 0x112},
        {0x08, 0x113},
        {0x10, 0x114},
    };

    int changed = buttons ^ g_linux_emitted_button_mask;
    for (int i = 0; i < (int)(sizeof(button_map) / sizeof(button_map[0])); i++) {
        if (!(changed & button_map[i].mask)) continue;
        queued = enqueue(EV_KEY, button_map[i].code,
            (buttons & button_map[i].mask) ? 1 : 0) || queued;
    }
    g_linux_emitted_button_mask = buttons;

    if (queued) {
        enqueue(EV_SYN, 0, 0);
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

    // syn event
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

//  populate /dev tree

void LinuxDeviceBridge::PopulateDevFS() {
    KVFS::Mkdirs("/dev");
    KVFS::Mkdirs("/dev/input");
    KVFS::Mkdirs("/dev/pts");
    KVFS::Mkdirs("/dev/shm");
    KVFS::Mkdirs("/dev/mqueue");

    // create device nodes as special files in kvfs
    for (int i = 0; i < device_count; i++) {
        if (!devices[i].active) continue;
        // create the file path in kvfs
        KVFS::CreateFile(devices[i].path);
    }

    // create symlinks
    KVFS::CreateFile("/dev/stdin");
    KVFS::CreateFile("/dev/stdout");
    KVFS::CreateFile("/dev/stderr");
    KVFS::CreateFile("/dev/fd");
}

//  status

void LinuxDeviceBridge::DumpDevices(char* out, int max_out) {
    int p = 0;
    auto put = [&](const char* s) { while (*s && p < max_out - 1) out[p++] = *s++; };
    auto putd = [&](int v) {
        char tmp[12]; int i = 0;
        if (v == 0) { out[p++] = '0'; return; }
        while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
        for (int j = i - 1; j >= 0 && p < max_out - 1; j--) out[p++] = tmp[j];
    };

    put("Linux Device Bridge  -  ");
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
