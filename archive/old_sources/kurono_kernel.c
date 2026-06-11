#include <stdint.h>

// Remove logo.h include as we are loading from module
// #include "logo.h"

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms0;
    uint32_t syms1;
    uint32_t syms2;
    uint32_t syms3;
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
} __attribute__((packed)) multiboot_info_t;

typedef struct {
    uint16_t attributes;
    uint8_t winA;
    uint8_t winB;
    uint16_t granularity;
    uint16_t winsize;
    uint16_t segmentA;
    uint16_t segmentB;
    uint32_t realFctPtr;
    uint16_t pitch;
    uint16_t Xres;
    uint16_t Yres;
    uint8_t Xchar;
    uint8_t Ychar;
    uint8_t planes;
    uint8_t bpp;
    uint8_t banks;
    uint8_t memory_model;
    uint8_t bank_size;
    uint8_t image_pages;
    uint8_t reserved1;
    uint8_t red_mask;
    uint8_t red_position;
    uint8_t green_mask;
    uint8_t green_position;
    uint8_t blue_mask;
    uint8_t blue_position;
    uint8_t rsv_mask;
    uint8_t rsv_position;
    uint8_t directcolor_attributes;
    uint32_t physbase;
} __attribute__((packed)) vbe_mode_info_t;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void serial_write_char(char c) {
    outb(0x3F8, c);
}

static void serial_write_str(const char* s) {
    while (*s) {
        serial_write_char(*s++);
    }
}

static void serial_write_hex(uint32_t n) {
    const char* hex = "0123456789ABCDEF";
    serial_write_str("0x");
    for (int i = 28; i >= 0; i -= 4) {
        serial_write_char(hex[(n >> i) & 0xF]);
    }
}

static inline void fb_put_pixel(uint32_t addr, uint16_t pitch, uint8_t bpp, int x, int y, uint32_t color) {
    // Bounds checking is handled by caller or we can add it here if we pass width/height
    // But since we don't pass width/height here easily without changing signature everywhere...
    // We will rely on the draw functions to clamp, OR we add bounds check if we assume 1024x768? 
    // Better to pass width/height to draw functions.
    // However, for a quick fix, let's just accept we need to be careful.
    // Actually, let's update the signature to include width/height or make them global?
    // No, keep it simple. We'll fix the call sites.
    
    volatile uint8_t* fb = (uint8_t*)addr;
    uint32_t offset = y * pitch + x * (bpp / 8);
    if (bpp == 32) {
        *(volatile uint32_t*)(fb + offset) = color;
    } else if (bpp == 24) {
        volatile uint8_t* p = fb + offset;
        p[0] = color & 0xFF;
        p[1] = (color >> 8) & 0xFF;
        p[2] = (color >> 16) & 0xFF;
    }
}

static inline void fb_put_pixel_safe(uint32_t addr, uint16_t pitch, uint8_t bpp, uint32_t width, uint32_t height, int x, int y, uint32_t color) {
    if (x < 0 || x >= (int)width || y < 0 || y >= (int)height) return;
    fb_put_pixel(addr, pitch, bpp, x, y, color);
}

static inline void fb_fill(uint32_t addr, uint16_t width, uint16_t height, uint16_t pitch, uint8_t bpp, uint32_t color) {
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            fb_put_pixel(addr, pitch, bpp, x, y, color);
        }
    }
}

static inline uint32_t fb_get_pixel(uint32_t addr, uint16_t pitch, uint8_t bpp, int x, int y) {
    volatile uint8_t* fb = (uint8_t*)addr;
    uint32_t offset = y * pitch + x * (bpp / 8);
    if (bpp == 32) {
        return *(volatile uint32_t*)(fb + offset);
    } else {
        volatile uint8_t* p = fb + offset;
        return 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
    }
}

static inline void fb_put_pixel_blend(uint32_t addr, uint16_t pitch, uint8_t bpp, int x, int y, uint32_t overlay) {
    uint32_t bg = fb_get_pixel(addr, pitch, bpp, x, y);
    uint8_t oa = (overlay >> 24) & 0xFF;
    uint8_t or = (overlay >> 16) & 0xFF;
    uint8_t og = (overlay >> 8) & 0xFF;
    uint8_t ob = overlay & 0xFF;
    uint8_t br = (bg >> 16) & 0xFF;
    uint8_t bgc = (bg >> 8) & 0xFF;
    uint8_t bb = bg & 0xFF;
    uint32_t r = (or * oa + br * (255 - oa)) / 255;
    uint32_t g = (og * oa + bgc * (255 - oa)) / 255;
    uint32_t b = (ob * oa + bb * (255 - oa)) / 255;
    fb_put_pixel(addr, pitch, bpp, x, y, 0xFF000000u | (r << 16) | (g << 8) | b);
}

static inline void fb_fill_gradient_v(uint32_t addr, uint16_t width, uint16_t height, uint16_t pitch, uint8_t bpp, uint32_t top, uint32_t bottom) {
    uint8_t tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    uint8_t br = (bottom >> 16) & 0xFF, bgc = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (uint16_t y = 0; y < height; y++) {
        uint32_t r = tr + (br - tr) * y / height;
        uint32_t g = tg + (bgc - tg) * y / height;
        uint32_t b = tb + (bb - tb) * y / height;
        uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
        for (uint16_t x = 0; x < width; x++) fb_put_pixel(addr, pitch, bpp, x, y, c);
    }
}

static void fb_fill_rect_alpha(uint32_t addr, uint16_t pitch, uint8_t bpp, uint32_t width, uint32_t height, int x0, int y0, int w, int h, uint8_t alpha, uint32_t color) {
    uint32_t c = (alpha << 24) | (color & 0x00FFFFFFu);
    for (int y = 0; y < h; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= (int)height) continue;
        for (int x = 0; x < w; x++) {
            int xx = x0 + x;
            if (xx < 0 || xx >= (int)width) continue;
            fb_put_pixel_blend(addr, pitch, bpp, xx, yy, c);
        }
    }
}

static void fb_fill_rounded_rect_alpha(uint32_t addr, uint16_t pitch, uint8_t bpp, uint32_t width, uint32_t height, int x0, int y0, int w, int h, int r, uint8_t alpha, uint32_t color) {
    fb_fill_rect_alpha(addr, pitch, bpp, width, height, x0 + r, y0, w - 2 * r, h, alpha, color);
    fb_fill_rect_alpha(addr, pitch, bpp, width, height, x0, y0 + r, r, h - 2 * r, alpha, color);
    fb_fill_rect_alpha(addr, pitch, bpp, width, height, x0 + w - r, y0 + r, r, h - 2 * r, alpha, color);
    for (int yy = 0; yy < r; yy++) {
        for (int xx = 0; xx < r; xx++) {
            int dx = r - xx, dy = r - yy; if (dx * dx + dy * dy <= r * r) {
                fb_put_pixel_blend(addr, pitch, bpp, x0 + xx, y0 + yy, (alpha << 24) | (color & 0x00FFFFFFu));
                fb_put_pixel_blend(addr, pitch, bpp, x0 + w - 1 - xx, y0 + yy, (alpha << 24) | (color & 0x00FFFFFFu));
                fb_put_pixel_blend(addr, pitch, bpp, x0 + xx, y0 + h - 1 - yy, (alpha << 24) | (color & 0x00FFFFFFu));
                fb_put_pixel_blend(addr, pitch, bpp, x0 + w - 1 - xx, y0 + h - 1 - yy, (alpha << 24) | (color & 0x00FFFFFFu));
            }
        }
    }
}

static const uint8_t font5x7[][7] = {
    {0x1E,0x11,0x13,0x15,0x19,0x11,0x1E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x1E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x11,0x01,0x0E,0x01,0x11,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x1E},
    {0x0E,0x10,0x1E,0x11,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}
};

static const uint8_t colon5x7[7] = {0x00,0x04,0x00,0x00,0x00,0x04,0x00};

static void draw_glyph(uint32_t addr, uint16_t pitch, uint8_t bpp, uint32_t width, uint32_t height, int x0, int y0, const uint8_t glyph[7], int scale, uint32_t color) {
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 5; x++) {
            if (glyph[y] & (1 << (4 - x))) {
                for (int yy = 0; yy < scale; yy++) {
                    for (int xx = 0; xx < scale; xx++) {
                        fb_put_pixel_safe(addr, pitch, bpp, width, height, x0 + x * scale + xx, y0 + y * scale + yy, color);
                    }
                }
            }
        }
    }
}

static void fb_draw_circle(uint32_t addr, uint16_t pitch, uint8_t bpp, int cx, int cy, int r, uint32_t color) {
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;
    
    while (y >= x) {
        fb_put_pixel(addr, pitch, bpp, cx + x, cy + y, color);
        fb_put_pixel(addr, pitch, bpp, cx - x, cy + y, color);
        fb_put_pixel(addr, pitch, bpp, cx + x, cy - y, color);
        fb_put_pixel(addr, pitch, bpp, cx - x, cy - y, color);
        fb_put_pixel(addr, pitch, bpp, cx + y, cy + x, color);
        fb_put_pixel(addr, pitch, bpp, cx - y, cy + x, color);
        fb_put_pixel(addr, pitch, bpp, cx + y, cy - x, color);
        fb_put_pixel(addr, pitch, bpp, cx - y, cy - x, color);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

static inline uint8_t clamp8(int v) {
    if (v < 0) return 0; if (v > 255) return 255; return (uint8_t)v;
}

static inline uint32_t force_opaque_if_visible(uint32_t color) {
    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color) & 0xFF;
    if (a == 0 && (r > 5 || g > 5 || b > 5)) {
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    return color;
}

static void fb_draw_arc_segment(uint32_t addr, uint16_t pitch, uint8_t bpp, uint32_t width, uint32_t height, int cx, int cy, int r, int arc_len, int segment, uint32_t color) {
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;
    int steps = 0;
    while (y >= x && steps <= arc_len) {
        switch (segment) {
            case 0:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx + x, cy + y, color);
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - x, cy + y, color);
                break;
            case 1:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx + y, cy + x, color);
                break;
            case 2:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx + y, cy - x, color);
                break;
            case 3:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx + x, cy - y, color);
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - x, cy - y, color);
                break;
            case 4:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - y, cy - x, color);
                break;
            case 5:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - y, cy + x, color);
                break;
            case 6:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - y, cy + x, color);
                break;
            case 7:
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx + x, cy + y, color);
                fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - x, cy + y, color);
                break;
        }
        if (segment == 0 || segment == 7 || segment == 3) {
            fb_put_pixel_safe(addr, pitch, bpp, width, height, cx + x, cy + y - 1, color);
            fb_put_pixel_safe(addr, pitch, bpp, width, height, cx - x, cy + y - 1, color);
        }
        steps++;
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

static void draw_logo_from_module(uint32_t fb_addr, uint16_t fb_pitch, uint8_t fb_bpp, uint32_t fb_width, uint32_t fb_height, uint32_t mod_addr) {
    // Parse our simple raw format
    // Header: Magic(4) Width(4) Height(4)
    uint32_t* header = (uint32_t*)mod_addr;
    if (header[0] != 0x4F474F4C) { // "LOGO"
        serial_write_str("Invalid logo magic!\r\n");
        return;
    }
    
    int logo_w = (int)header[1];
    int logo_h = (int)header[2];
    
    serial_write_str("Loading Logo: "); 
    serial_write_hex(logo_w); serial_write_str("x"); serial_write_hex(logo_h); serial_write_str("\r\n");
    
    uint8_t* data = (uint8_t*)(mod_addr + 12); // Skip header
    
    int start_x = (fb_width - logo_w) / 2;
    int start_y = (fb_height - logo_h) / 2;
    
    // Debug box around logo area
    for(int x = 0; x < logo_w; x++) fb_put_pixel_safe(fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, start_x + x, start_y - 1, 0xFFFFFFFF);
    for(int x = 0; x < logo_w; x++) fb_put_pixel_safe(fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, start_x + x, start_y + logo_h, 0xFFFFFFFF);
    for(int y = 0; y < logo_h; y++) fb_put_pixel_safe(fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, start_x - 1, start_y + y, 0xFFFFFFFF);
    for(int y = 0; y < logo_h; y++) fb_put_pixel_safe(fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, start_x + logo_w, start_y + y, 0xFFFFFFFF);

    for (int y = 0; y < logo_h; y++) {
        int screen_y = start_y + y;
        if (screen_y < 0 || screen_y >= (int)fb_height) continue;
        
        for (int x = 0; x < logo_w; x++) {
            int screen_x = start_x + x;
            if (screen_x < 0 || screen_x >= (int)fb_width) continue;
            
            // Read BGRA from module
            int offset = (y * logo_w + x) * 4;
            uint8_t b = data[offset + 0];
            uint8_t g = data[offset + 1];
            uint8_t r = data[offset + 2];
            uint8_t a = data[offset + 3];
            
            if (a > 0) {
                uint32_t color = (0xFF << 24) | (r << 16) | (g << 8) | b;
                fb_put_pixel(fb_addr, fb_pitch, fb_bpp, screen_x, screen_y, color);
            }
        }
    }
}

void kernel_main(uint32_t magic, uint32_t mb_addr) {
    serial_init();
    serial_write_str("Kurono kernel start\r\n");
    serial_write_str("Magic: "); serial_write_hex(magic); serial_write_str("\r\n");
    serial_write_str("MB Addr: "); serial_write_hex(mb_addr); serial_write_str("\r\n");
    
    if (magic == 0x2BADB002) {
        multiboot_info_t* mbi = (multiboot_info_t*)(uint32_t)mb_addr;
        serial_write_str("Flags: "); serial_write_hex(mbi->flags); serial_write_str("\r\n");

        uint64_t fb_addr = 0;
        uint32_t fb_pitch = 0;
        uint32_t fb_width = 0;
        uint32_t fb_height = 0;
        uint8_t fb_bpp = 0;

        if (mbi->flags & (1u << 12)) {
             serial_write_str("Using Multiboot Framebuffer\r\n");
             fb_addr = mbi->framebuffer_addr;
             fb_pitch = mbi->framebuffer_pitch;
             fb_width = mbi->framebuffer_width;
             fb_height = mbi->framebuffer_height;
             fb_bpp = mbi->framebuffer_bpp;
        } else if (mbi->flags & (1u << 11)) {
            serial_write_str("Using VBE Info\r\n");
            vbe_mode_info_t* vmi = (vbe_mode_info_t*)(uint32_t)mbi->vbe_mode_info;
            fb_addr = vmi->physbase;
            fb_pitch = vmi->pitch;
            fb_width = vmi->Xres;
            fb_height = vmi->Yres;
            fb_bpp = vmi->bpp;
        }

        serial_write_str("FB Addr: "); serial_write_hex((uint32_t)fb_addr); serial_write_str("\r\n");
        serial_write_str("FB BPP: "); serial_write_hex(fb_bpp); serial_write_str("\r\n");
        serial_write_str("FB Width: "); serial_write_hex(fb_width); serial_write_str("\r\n");

        if (fb_addr && (fb_bpp == 32 || fb_bpp == 24)) {
            fb_fill_gradient_v((uint32_t)fb_addr, fb_width, fb_height, fb_pitch, fb_bpp, 0xFF0E1F2F, 0xFF2C4A63);
            
            if (mbi->flags & (1u << 3)) { // Check if modules exist
                multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;
                if (mbi->mods_count > 0) {
                    serial_write_str("Found Module at: "); serial_write_hex(mods[0].mod_start); serial_write_str("\r\n");
                    draw_logo_from_module((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, mods[0].mod_start);
                } else {
                    serial_write_str("No modules found!\r\n");
                }
            } else {
                serial_write_str("Module flag not set!\r\n");
            }
            
            int box_w = (int)(fb_width * 0.6);
            int box_h = (int)(fb_height * 0.3);
            int box_x = (int)((fb_width - box_w) / 2);
            int box_y = (int)((fb_height - box_h) / 2);
            fb_fill_rounded_rect_alpha((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, box_x, box_y, box_w, box_h, 20, 160, 0xFFFFFF);
            int scale = (int)(fb_width / 40);
            if (scale < 8) scale = 8;
            int time_y = box_y + (box_h - 7 * scale) / 2;
            int cur_x = box_x + (box_w - (5*scale*4 + scale + 5*scale)) / 2;
            draw_glyph((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, cur_x, time_y, font5x7[1], scale, 0xFFFFFFFF); cur_x += 5*scale;
            draw_glyph((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, cur_x, time_y, font5x7[0], scale, 0xFFFFFFFF); cur_x += 5*scale;
            draw_glyph((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, cur_x, time_y, colon5x7, scale, 0xFFFFFFFF); cur_x += scale;
            draw_glyph((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, cur_x, time_y, font5x7[4], scale, 0xFFFFFFFF); cur_x += 5*scale;
            draw_glyph((uint32_t)fb_addr, fb_pitch, fb_bpp, fb_width, fb_height, cur_x, time_y, font5x7[2], scale, 0xFFFFFFFF);
            for (;;) { __asm__ __volatile__("hlt"); }
        } else {
             serial_write_str("Invalid FB: BPP must be 24 or 32, Addr must be non-zero.\r\n");
        }
    }
    
    // Fallback
    serial_write_str("Halting.\r\n");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
