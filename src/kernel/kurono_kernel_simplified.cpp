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
    // simple kernel for enhanced 180hz drivers
    
    // if this isn't a multiboot kernel, stop
    if (magic != 0x2BADB002) {
        // no logging available, just halt
        while(1) __asm__ volatile("hlt");
    }
    
    // initialize enhanced display controller
    if (!DisplayController::Init()) {
        // failed to initialize display, halt
        while(1) __asm__ volatile("hlt");
    }
    
    DisplayController::EnumerateModes();
    
    // find a high refresh rate mode (prefer 180hz)
    const DisplayController::DisplayMode* mode = DisplayController::FindBestMode(1920, 1080, 32, DisplayController::REFRESH_180HZ);
    if (!mode) {
        // fall back to any high resolution mode
        mode = DisplayController::FindBestMode(1024, 768, 32, DisplayController::REFRESH_60HZ);
    }
    
    if (mode) {
        DisplayController::SetMode(mode);
    }
    
    // initialize enhanced graphics with 180hz target
    Graphics::InitAdvanced(); // use display controller
    Graphics::SetTargetFPS(180);
    Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);
    
    // initialize enhanced input drivers
    Keyboard keyboard;
    keyboard.Init();
    keyboard.SetRepeatRate(30, 2); // fast repeat for gaming
    
    Mouse mouse; 
    mouse.Init();
    mouse.SetPollingRate(1000);  // 1000hz polling
    
    // 180hz main loop
    uint32_t frameCount = 0;
    while(1) {
        // check if we should render this frame (180hz pacing)
        if (Graphics::ShouldRender()) {
            Graphics::BeginFrame();
            
            // clear screen 
            Graphics::Clear(0xFF001122);
            
            frameCount++;
            
            // simple graphics display
            Graphics::FillRect(100, 100, 200, 50, 0xFF00FF00);
            
            Graphics::EndFrame();
            Graphics::WaitForVSync();
        }
        
        // small delay to prevent 100% cpu usage
        __asm__ volatile("pause");
    }
}