#ifndef KURONO_KERNEL_H
#define KURONO_KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// VGA text mode constants
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

// VGA colors
#define VGA_COLOR_BLACK 0
#define VGA_COLOR_BLUE 1
#define VGA_COLOR_GREEN 2
#define VGA_COLOR_CYAN 3
#define VGA_COLOR_RED 4
#define VGA_COLOR_MAGENTA 5
#define VGA_COLOR_BROWN 6
#define VGA_COLOR_LIGHT_GREY 7
#define VGA_COLOR_DARK_GREY 8
#define VGA_COLOR_LIGHT_BLUE 9
#define VGA_COLOR_LIGHT_GREEN 10
#define VGA_COLOR_LIGHT_CYAN 11
#define VGA_COLOR_LIGHT_RED 12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_LIGHT_BROWN 14
#define VGA_COLOR_WHITE 15

// Kurono OS constants
#define KURONO_VERSION "1.0.0"
#define KURONO_NAME "Kurono OS"
#define KERNEL_STACK_SIZE 16384

// Memory management
typedef struct {
    uint32_t total_memory;
    uint32_t available_memory;
    uint32_t kernel_size;
    uint32_t kernel_end;
} MemoryInfo;

// Terminal
typedef struct {
    size_t row;
    size_t column;
    uint8_t color;
    uint16_t* buffer;
} Terminal;

// Kernel context
typedef struct {
    MemoryInfo memory;
    Terminal terminal;
    bool initialized;
    char* current_user;
    char* current_directory;
} KernelContext;

// Function declarations
void kernel_main(void);
void kernel_panic(const char* message);

// Memory functions
void* memset(void* dest, int value, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* kmalloc(size_t size);
void kfree(void* ptr);

// Terminal functions
void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_clear(void);
void terminal_scroll(void);

// String functions
size_t strlen(const char* str);
int strcmp(const char* str1, const char* str2);
char* strcpy(char* dest, const char* src);
char* strdup(const char* src);

// I/O functions
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

#endif