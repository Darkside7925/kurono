#include "system.h"
#include "../drivers/serial.h"

void System::Initialize() {
    SerialLogger::Log("System: Initializing Core Components...\r\n");
    
    // simulate loading sequence
    LoadGUI();
    Wait(500000);
    
    LoadGraphics();
    Wait(500000);
    
    LoadVulkan();
    Wait(800000);
    
    LoadOpenGL();
    Wait(800000);
    
    SerialLogger::Log("System: All Components Initialized.\r\n");
}

void System::Wait(int count) {
    for (volatile int i = 0; i < count; i++);
}

void System::LoadGUI() {
    SerialLogger::Log("System: Loading GUI Subsystem... [OK]\r\n");
}
void System::LoadGraphics() {
    SerialLogger::Log("System: Loading Graphics Drivers... [OK]\r\n");
}
void System::LoadVulkan() {
    SerialLogger::Log("System: Loading Vulkan Support... [Stubbed]\r\n");
}
void System::LoadOpenGL() {
    SerialLogger::Log("System: Loading OpenGL Support... [Stubbed]\r\n");
}
