#pragma once
#include "../kernel/types.h"
#include "../hal/hal.h"
#include "../ui/font.h"
#include "../drivers/graphics.h"

// System Monitor and Panic Handler
// Ensures system stability and provides diagnostics.

class System {
public:
    static void Panic(const char* message, const char* file = nullptr, int line = 0) {
        HAL::DisableInterrupts();
        
        // Red Screen of Death
        int w = Graphics::GetWidth();
        int h = Graphics::GetHeight();
        Graphics::FillRect(0, 0, w, h, 0xFF880000);
        
        if (FontTTF::ok) {
            FontTTF::DrawStringCenter(w/2, h/2 - 40, 32.0f, "KERNEL PANIC", 0xFFFFFFFF);
            FontTTF::DrawStringCenter(w/2, h/2, 16.0f, message, 0xFFFFFFFF);
            
            if (file) {
                // char buf[128]; 
                // snprintf(buf, ...); // No snprintf yet
                FontTTF::DrawStringCenter(w/2, h/2 + 30, 14.0f, file, 0xFFAAAAAA);
            }
        }
        
        HAL::Halt();
    }
    
    static void Log(const char* msg) {
        // Serial logging
        // Serial::Write(msg);
    }
};

#define KERNEL_PANIC(msg) System::Panic(msg, __FILE__, __LINE__)
#define ASSERT(cond, msg) if (!(cond)) KERNEL_PANIC(msg)
