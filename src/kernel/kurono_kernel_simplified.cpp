/**
 * Enhanced Kurono OS Kernel - 180Hz Graphics Edition
 * Simplified kernel focusing on high refresh rate graphics and enhanced drivers
 */

#include "types.h"
#include "multiboot.h"
#include "system.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/display.h"

extern "C" void kernel_main(uint32_t magic, uint32_t mb_addr) {
    // Simple kernel for enhanced 180Hz drivers
    
    // If this isn't a multiboot kernel, stop
    if (magic != 0x2BADB002) {
        // No logging available, just halt
        while(1) __asm__ volatile("hlt");
    }
    
    // Initialize enhanced display controller
    if (!DisplayController::Init()) {
        // Failed to initialize display, halt
        while(1) __asm__ volatile("hlt");
    }
    
    DisplayController::EnumerateModes();
    
    // Find a high refresh rate mode (prefer 180Hz)
    const DisplayController::DisplayMode* mode = DisplayController::FindBestMode(1920, 1080, 32, DisplayController::REFRESH_180HZ);
    if (!mode) {
        // Fall back to any high resolution mode
        mode = DisplayController::FindBestMode(1024, 768, 32, DisplayController::REFRESH_60HZ);
    }
    
    if (mode) {
        DisplayController::SetMode(mode);
    }
    
    // Initialize enhanced graphics with 180Hz target
    Graphics::InitAdvanced(); // Use display controller
    Graphics::SetTargetFPS(180);
    Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
    
    // Initialize enhanced input drivers
    Keyboard keyboard;
    keyboard.Init();
    keyboard.SetRepeatRate(30, 2); // Fast repeat for gaming
    
    Mouse mouse; 
    mouse.Init();
    mouse.SetPollingRate(1000);  // 1000Hz polling
    
    // 180Hz main loop
    uint32_t frameCount = 0;
    while(1) {
        // Check if we should render this frame (180Hz pacing)
        if (Graphics::ShouldRender()) {
            Graphics::BeginFrame();
            
            // Clear screen 
            Graphics::Clear(0xFF001122);
            
            frameCount++;
            
            // Simple graphics display
            Graphics::FillRect(100, 100, 200, 50, 0xFF00FF00);
            
            Graphics::EndFrame();
            Graphics::WaitForVSync();
        }
        
        // Small delay to prevent 100% CPU usage
        __asm__ volatile("pause");
    }
}