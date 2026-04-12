#include "input_manager.h"
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/usb.h"
#include "../fs/vfs.h"

KeyboardDevice InputManager::devices[8];
int InputManager::device_count = 0;
KeyLayout InputManager::current_layout = KeyLayout::QWERTY_US;
Key InputManager::last_key = KEY_UNKNOWN;
bool InputManager::last_key_pressed = false;
int InputManager::last_device_id = -1;

static bool im_streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void im_append_dec(char* dst, int& pos, int max_len, int val) {
    if (val == 0) {
        if (pos < max_len - 1) dst[pos++] = '0';
        dst[pos] = 0;
        return;
    }
    char rev[12];
    int ri = 0;
    while (val > 0 && ri < 11) {
        rev[ri++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (ri > 0 && pos < max_len - 1) dst[pos++] = rev[--ri];
    dst[pos] = 0;
}

static int FindDeviceByName(const char* name) {
    for (int i = 0; i < InputManager::GetDeviceCount(); i++) {
        KeyboardDevice* dev = InputManager::GetDevice(i);
        if (dev && im_streq(dev->name, name)) return i;
    }
    return -1;
}

static void SyncUSBHIDDevices() {
    if (!USB::IsDetected()) return;

    int usb_count = USB::GetDeviceCount();
    for (int i = 0; i < usb_count; i++) {
        const USBDeviceInfo* usb_dev = USB::GetDevice(i);
        if (!usb_dev || !usb_dev->connected) continue;

        // hid class keyboard devices (boot keyboard protocol 0x03:0x01)
        bool is_hid = (usb_dev->dev_class == 0x03);
        if (!is_hid) continue;

        char name[64];
        int pos = 0;
        const char* prefix = "USB HID Keyboard P";
        while (prefix[pos] && pos < 63) {
            name[pos] = prefix[pos];
            pos++;
        }
        im_append_dec(name, pos, 64, usb_dev->port);

        int existing = FindDeviceByName(name);
        if (existing < 0) {
            int id = InputManager::RegisterDevice(name, DeviceType::USB);
            if (id >= 0) InputManager::SetDeviceStatus(id, true);
        } else {
            InputManager::SetDeviceStatus(existing, true);
        }
    }
}

void InputManager::Init() {
    device_count = 0;
    // register standard ps/2 keyboard
    RegisterDevice("Standard PS/2 Keyboard", DeviceType::PS2);
    
    // register real usb hid devices if xhci is available
    SyncUSBHIDDevices();
    
    SerialLogger::Log("InputManager: Initialized\r\n");
    
    // set callback in low-level driver to route to us
    Keyboard::SetCallback([](Key k, char c, bool p){
        (void)c;
        if (p) OnKeyDown(0, k);
        else OnKeyUp(0, k);
    });
}

void InputManager::Poll() {
    Keyboard::Poll();
    // poll mouse after keyboard so touchpad packet bursts do not delay key
    // delivery on shared laptop 8042/ec controllers.
    Mouse::Poll();
    
    // real usb hid device sync (hotplug-safe)
    SyncUSBHIDDevices();
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
    // deprecated logic: now keyboard::keytochar handles it centrally.
    // but we keep this for legacy or specialized usage if needed.
    // actually, calling keytochar will now apply the layout again if we are not careful?
    // wait. keyboard::keytochar applies the layout.
    // if mapkey calls keytochar, it applies it.
    // if mapkey also swaps keys before calling keytochar, we double swap!
    // example: q -> a. keytochar(a) -> q (if layout applies again).
    // infinite loop or wrong char.
    
    // fix: mapkey should just call keytochar with the raw key, 
    // relying on keytochar to do the mapping.
    
    const KeyboardState& state = Keyboard::GetState();
    return Keyboard::KeyToChar(key, state.shift, state.caps_lock);
}

void InputManager::OnKeyDown(int device_id, Key key) {
    last_key = key;
    last_key_pressed = true;
    last_device_id = device_id;
    
    char c = MapKey(key);
    
    // log to file
    FileNode* f = VFS::Open("/input.log");
    if (f) {
        char msg[64];
        // manual formatting since no sprintf
        // "key dev:x char:c\n"
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
