#pragma once
#include "../kernel/types.h"

// Comprehensive Key Enumeration
enum Key {
    KEY_UNKNOWN = 0,
    
    // Alphanumeric
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    
    // Function Keys
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, 
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    
    // Modifiers
    KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LSUPER,
    KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RSUPER,
    
    // Control Keys
    KEY_ESC, KEY_ENTER, KEY_BACKSPACE, KEY_TAB, KEY_SPACE,
    KEY_CAPSLOCK, KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    
    // Punctuation
    KEY_MINUS, KEY_EQUAL, KEY_LBRACKET, KEY_RBRACKET, KEY_BACKSLASH,
    KEY_SEMICOLON, KEY_QUOTE, KEY_GRAVE, KEY_COMMA, KEY_PERIOD, KEY_SLASH,
    
    // Keypad
    KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, 
    KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
    KEY_KP_DECIMAL, KEY_KP_DIVIDE, KEY_KP_MULTIPLY, KEY_KP_SUBTRACT, KEY_KP_ADD, KEY_KP_ENTER,
    
    // Media/Power
    KEY_PRINTSCREEN, KEY_PAUSE, KEY_MENU, KEY_POWER, KEY_SLEEP, KEY_WAKE
};

struct KeyboardState {
    bool shift;
    bool ctrl;
    bool alt;
    bool super;
    bool caps_lock;
    bool num_lock;
    bool scroll_lock;
    uint8_t key_bitmap[32]; // 256 bits for key states
};

class Keyboard {
public:
    static void Init();
    static void Poll();
    static bool HasChar();
    static char GetChar();
    
    // Advanced API
    static bool IsKeyDown(Key key);
    static bool IsKeyPressed(Key key); // True only on the frame it was pressed
    static const KeyboardState& GetState();
    static char KeyToChar(Key key, bool shift, bool caps);
    
    // Key Event Callback
    typedef void (*KeyCallback)(Key key, char c, bool pressed);
    static void SetCallback(KeyCallback cb);
    
    // Enhanced Features
    static void SetLEDs(bool num_lock, bool caps_lock, bool scroll_lock);
    static void UpdateLEDs();
    static void SetRepeatRate(uint8_t rate, uint8_t delay); // rate: 0-31, delay: 0-3
    static void EnableTypematic(bool enable);
    
    // USB HID Support (stubs for future expansion)
    static void InitUSB();
    static bool AddUSBDevice(uint8_t device_id);
    static void RemoveUSBDevice(uint8_t device_id);
    static void ProcessUSBReport(uint8_t device_id, const uint8_t* report, size_t length);
    
    // Diagnostics
    static void SelfTest();
    static bool GetControllerStatus(uint8_t& status);
    static void FlushBuffers();

private:
    static uint8_t Status();
    static uint8_t In(uint16_t p);
    static void Out(uint16_t p, uint8_t v);
    static void Enqueue(char c);
    static void HandleScancode(uint8_t sc);
    
    static Key ScancodeToKey(uint8_t sc, bool e0);
    // static char KeyToChar(Key key, bool shift, bool caps); // Moved to public
    
    // State
    static KeyboardState state;
    static bool e0_prefix;
    static uint8_t head;
    static uint8_t tail;
    static char buf[256];
    static KeyCallback callback;
    
    // Key State Tracking
    static bool keys[256]; // Simple array for now, mapped by Key enum
    static bool prev_keys[256];
};
