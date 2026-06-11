#pragma once
#include "../kernel/types.h"
#include "../drivers/keyboard.h"

// Input Manager to handle multiple devices and layouts
// Supports abstraction for USB, PS/2, Bluetooth keyboards

enum class DeviceType {
    PS2,
    USB,
    Bluetooth,
    Virtual
};

enum class KeyLayout {
    QWERTY_US,
    QWERTZ_DE,
    AZERTY_FR
};

struct KeyboardDevice {
    char name[64];
    DeviceType type;
    bool connected;
    int id;
};

class InputManager {
public:
    static void Init();
    static void Poll();
    
    // Device Management
    static int RegisterDevice(const char* name, DeviceType type);
    static void UnregisterDevice(int id);
    static void SetDeviceStatus(int id, bool connected);
    static int GetDeviceCount();
    static KeyboardDevice* GetDevice(int index);
    
    // Layout Management
    static void SetLayout(KeyLayout layout);
    static KeyLayout GetLayout();
    static const char* GetLayoutName();
    
    // Input Processing (Called by drivers)
    static void OnKeyDown(int device_id, Key key);
    static void OnKeyUp(int device_id, Key key);
    
    // Visualizer Hook
    static Key last_key;
    static bool last_key_pressed;
    static int last_device_id;
    
private:
    static KeyboardDevice devices[8];
    static int device_count;
    static KeyLayout current_layout;
    
    static char MapKey(Key key);
};
