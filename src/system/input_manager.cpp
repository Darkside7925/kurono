#include "input_manager.h"
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../fs/vfs.h"

KeyboardDevice InputManager::devices[8];
int InputManager::device_count = 0;
KeyLayout InputManager::current_layout = KeyLayout::QWERTY_US;
Key InputManager::last_key = KEY_UNKNOWN;
bool InputManager::last_key_pressed = false;
int InputManager::last_device_id = -1;

void InputManager::Init() {
    device_count = 0;
    // Register Standard PS/2 Keyboard
    RegisterDevice("Standard PS/2 Keyboard", DeviceType::PS2);
    
    // Mock USB Detection
    RegisterDevice("Generic USB Keyboard", DeviceType::USB);
    SetDeviceStatus(1, false); // Start disconnected
    
    SerialLogger::Log("InputManager: Initialized\r\n");
    
    // Set callback in low-level driver to route to us
    Keyboard::SetCallback([](Key k, char c, bool p){
        (void)c;
        if (p) OnKeyDown(0, k);
        else OnKeyUp(0, k);
    });
}

void InputManager::Poll() {
    // Poll hardware drivers
    Keyboard::Poll();
    Mouse::Poll(); // Restored Mouse Polling
    
    // Simulate hotplug events for demo
    static int sim_timer = 0;
    sim_timer++;
    if (sim_timer == 300) { // ~5 seconds
        SetDeviceStatus(1, true);
        SerialLogger::Log("InputManager: New Device Detected: Generic USB Keyboard\r\n");
        
        // Log to file
        FileNode* f = VFS::Open("/input.log");
        if (f) {
            const char* msg = "EVENT: DEVICE_CONNECTED ID:1 NAME:Generic USB Keyboard\n";
            int len = 0; while(msg[len]) len++;
            VFS::Write(f, f->size, len, (uint8_t*)msg);
        }
    }
}

int InputManager::RegisterDevice(const char* name, DeviceType type) {
    if (device_count >= 8) return -1;
    int id = device_count++;
    KeyboardDevice* dev = &devices[id];
    
    int i=0; while(name[i] && i<63) { dev->name[i]=name[i]; i++; } dev->name[i]=0;
    dev->type = type;
    dev->connected = true;
    dev->id = id;
    
    SerialLogger::Log("InputManager: Registered "); SerialLogger::Log(name); SerialLogger::Log("\r\n");
    return id;
}

void InputManager::SetDeviceStatus(int id, bool connected) {
    if (id >= 0 && id < device_count) {
        devices[id].connected = connected;
    }
}

int InputManager::GetDeviceCount() { return device_count; }
KeyboardDevice* InputManager::GetDevice(int index) { 
    if (index >= 0 && index < device_count) return &devices[index];
    return nullptr;
}

void InputManager::SetLayout(KeyLayout layout) {
    current_layout = layout;
    SerialLogger::Log("InputManager: Layout changed\r\n");
}

KeyLayout InputManager::GetLayout() { return current_layout; }

const char* InputManager::GetLayoutName() {
    switch(current_layout) {
        case KeyLayout::QWERTY_US: return "US (QWERTY)";
        case KeyLayout::QWERTZ_DE: return "DE (QWERTZ)";
        case KeyLayout::AZERTY_FR: return "FR (AZERTY)";
        default: return "Unknown";
    }
}

char InputManager::MapKey(Key key) {
    // Deprecated logic: Now Keyboard::KeyToChar handles it centrally.
    // But we keep this for legacy or specialized usage if needed.
    // Actually, calling KeyToChar will now apply the layout AGAIN if we are not careful?
    // Wait. Keyboard::KeyToChar applies the layout.
    // If MapKey calls KeyToChar, it applies it.
    // If MapKey ALSO swaps keys before calling KeyToChar, we double swap!
    // Example: Q -> A. KeyToChar(A) -> Q (if layout applies again).
    // infinite loop or wrong char.
    
    // Fix: MapKey should just call KeyToChar with the raw key, 
    // relying on KeyToChar to do the mapping.
    
    const KeyboardState& state = Keyboard::GetState();
    return Keyboard::KeyToChar(key, state.shift, state.caps_lock);
}

void InputManager::OnKeyDown(int device_id, Key key) {
    last_key = key;
    last_key_pressed = true;
    last_device_id = device_id;
    
    char c = MapKey(key);
    
    // Log to file
    FileNode* f = VFS::Open("/input.log");
    if (f) {
        char msg[64];
        // Manual formatting since no sprintf
        // "KEY DEV:x CHAR:c\n"
        char* p = msg;
        const char* prefix = "KEY DEV:";
        while(*prefix) *p++ = *prefix++;
        *p++ = '0' + device_id;
        const char* mid = " CHAR:";
        while(*mid) *p++ = *mid++;
        if (c >= 32 && c <= 126) *p++ = c; else *p++ = '?';
        *p++ = '\n';
        *p = 0;
        
        VFS::Write(f, f->size, (uint32_t)(p - msg), (uint8_t*)msg);
    }
}

void InputManager::OnKeyUp(int device_id, Key key) {
    last_key = key;
    last_key_pressed = false;
    last_device_id = device_id;
}
