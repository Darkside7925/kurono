#pragma once
#include "../kernel/types.h"
#include "../drivers/keyboard.h"

// input manager to handle multiple devices and layouts
// supports abstraction for usb, ps/2, bluetooth keyboards

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

// Stable subscriber id so a callback removed during dispatch cannot crash a
// peer.  Zero means "unassigned".
typedef uint32_t SubscriberId;

class InputManager {
public:
    static void Init();
    static void Poll();

    // device management
    static int RegisterDevice(const char* name, DeviceType type);
    static void UnregisterDevice(int id);
    static void SetDeviceStatus(int id, bool connected);
    static int GetDeviceCount();
    static KeyboardDevice* GetDevice(int index);

    // layout management
    static void SetLayout(KeyLayout layout);
    static KeyLayout GetLayout();
    static const char* GetLayoutName();

    // input processing (called by drivers)
    static void OnKeyDown(int device_id, Key key);
    static void OnKeyUp(int device_id, Key key);

    // focus / subscriber routing. Subscribers receive routed key events.
    // Returning SubscriberId 0 means registration failed.
    typedef void (*KeyEventCallback)(void* ctx, int device_id, Key key, bool pressed);
    static SubscriberId Subscribe(KeyEventCallback cb, void* ctx);
    static void Unsubscribe(SubscriberId id);
    static void SetFocus(SubscriberId id);
    static SubscriberId GetFocus();

    // visualizer hook
    static Key last_key;
    static bool last_key_pressed;
    static int last_device_id;

private:
    static KeyboardDevice devices[8];
    static int device_count;
    static KeyLayout current_layout;

    static char MapKey(Key key);
};
