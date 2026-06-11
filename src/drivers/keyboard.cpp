#include "keyboard.h"
#include "serial.h"
#include "../system/input_manager.h" // Include InputManager for Layouts

KeyboardState Keyboard::state = {false, false, false, false, false, false, false, {0}};
bool Keyboard::e0_prefix = false;
uint8_t Keyboard::head = 0;
uint8_t Keyboard::tail = 0;
char Keyboard::buf[256];
Keyboard::KeyCallback Keyboard::callback = nullptr;
bool Keyboard::keys[256] = {0};
bool Keyboard::prev_keys[256] = {0};

void Keyboard::Init() {
    for(int i=0; i<256; i++) { keys[i] = false; prev_keys[i] = false; }
    state = {false, false, false, false, false, false, false, {0}};
    e0_prefix = false;
    head = tail = 0;
    
    SerialLogger::Log("Keyboard: Initializing Enhanced Driver...\r\n");
    
    // Self-test
    SelfTest();
    
    // Timeout counter to avoid hanging if no keyboard present
    int timeout = 100000;
    while ((Status() & 0x01) && timeout-- > 0) { In(0x60); } // Flush buffer
    
    timeout = 100000;
    while ((Status() & 0x02) && timeout-- > 0) {} // Wait for input buffer empty
    
    Out(0x64, 0x20); // Read Command Byte
    
    timeout = 100000;
    while (!(Status() & 0x01) && timeout-- > 0) {} // Wait for response
    
    if (timeout <= 0) {
        SerialLogger::Log("Keyboard: Controller not responding\r\n");
        return;
    }
    
    uint8_t ccb = In(0x60);
    ccb |= 0x40; // enable set-2 -> set-1 translation
    ccb |= 0x01; // enable keyboard interrupt (optional but good practice)
    
    timeout = 100000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    Out(0x64, 0x60); // Write Command Byte
    
    timeout = 100000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    Out(0x60, ccb);
    
    // Enable keyboard
    timeout = 100000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    Out(0x64, 0xAE);
    
    // Reset/Enable scanning
    timeout = 100000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    Out(0x60, 0xF4);
    
    // Set default repeat rate (500ms delay, 10.9 cps rate)
    SetRepeatRate(0x0B, 0x01);
    
    // Update LEDs to match initial state
    UpdateLEDs();
    
    SerialLogger::Log("Keyboard: Enhanced Driver initialized\r\n");
}

void Keyboard::SetLEDs(bool num_lock, bool caps_lock, bool scroll_lock) {
    state.num_lock = num_lock;
    state.caps_lock = caps_lock;
    state.scroll_lock = scroll_lock;
    UpdateLEDs();
}

void Keyboard::UpdateLEDs() {
    uint8_t led_byte = 0;
    if (state.scroll_lock) led_byte |= 0x01;
    if (state.num_lock) led_byte |= 0x02;
    if (state.caps_lock) led_byte |= 0x04;
    
    // Send Set LEDs command
    int timeout = 50000;
    while ((Status() & 0x02) && timeout-- > 0) {} // Wait for input buffer empty
    
    Out(0x60, 0xED); // Set LEDs command
    
    // Wait for ACK
    timeout = 50000;
    bool got_ack = false;
    while (timeout-- > 0) {
        if (Status() & 0x01) {
            if (In(0x60) == 0xFA) {
                got_ack = true;
                break;
            }
        }
    }
    
    if (got_ack) {
        // Send LED data
        timeout = 50000;
        while ((Status() & 0x02) && timeout-- > 0) {}
        Out(0x60, led_byte);
        
        // Wait for final ACK
        timeout = 50000;
        while (timeout-- > 0) {
            if (Status() & 0x01) {
                if (In(0x60) == 0xFA) break;
            }
        }
        
        SerialLogger::Log("Keyboard: LEDs updated (");
        SerialLogger::LogHex(led_byte);
        SerialLogger::Log(")\r\n");
    } else {
        SerialLogger::Log("Keyboard: LED update failed\r\n");
    }
}

void Keyboard::SetRepeatRate(uint8_t rate, uint8_t delay) {
    // Rate: 0-31 (30 cps to 2 cps), Delay: 0-3 (250ms to 1000ms)
    uint8_t data = (delay & 0x03) << 5;
    data |= rate & 0x1F;
    
    int timeout = 50000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    
    Out(0x60, 0xF3); // Set Typematic Rate/Delay
    
    // Wait for ACK
    timeout = 50000;
    bool got_ack = false;
    while (timeout-- > 0) {
        if (Status() & 0x01) {
            if (In(0x60) == 0xFA) {
                got_ack = true;
                break;
            }
        }
    }
    
    if (got_ack) {
        timeout = 50000;
        while ((Status() & 0x02) && timeout-- > 0) {}
        Out(0x60, data);
        
        // Wait for final ACK
        timeout = 50000;
        while (timeout-- > 0) {
            if (Status() & 0x01) {
                if (In(0x60) == 0xFA) break;
            }
        }
        
        SerialLogger::Log("Keyboard: Repeat rate set (");
        SerialLogger::LogHex(data);
        SerialLogger::Log(")\r\n");
    }
}

void Keyboard::EnableTypematic(bool enable) {
    int timeout = 50000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    
    if (enable) {
        Out(0x60, 0xF4); // Enable scanning
    } else {
        Out(0x60, 0xF5); // Disable scanning
    }
    
    // Wait for ACK
    timeout = 50000;
    while (timeout-- > 0) {
        if (Status() & 0x01) {
            if (In(0x60) == 0xFA) break;
        }
    }
}

void Keyboard::SelfTest() {
    SerialLogger::Log("Keyboard: Running self-test...\r\n");
    
    // Controller self-test
    int timeout = 50000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    Out(0x64, 0xAA);
    
    timeout = 100000;
    while (timeout-- > 0) {
        if (Status() & 0x01) {
            uint8_t result = In(0x60);
            if (result == 0x55) {
                SerialLogger::Log("Keyboard: Controller self-test PASSED\r\n");
            } else {
                SerialLogger::Log("Keyboard: Controller self-test FAILED (");
                SerialLogger::LogHex(result);
                SerialLogger::Log(")\r\n");
            }
            break;
        }
    }
    
    // Interface test
    timeout = 50000;
    while ((Status() & 0x02) && timeout-- > 0) {}
    Out(0x64, 0xAB);
    
    timeout = 100000;
    while (timeout-- > 0) {
        if (Status() & 0x01) {
            uint8_t result = In(0x60);
            if (result == 0x00) {
                SerialLogger::Log("Keyboard: Interface test PASSED\r\n");
            } else {
                SerialLogger::Log("Keyboard: Interface test FAILED (");
                SerialLogger::LogHex(result);
                SerialLogger::Log(")\r\n");
            }
            break;
        }
    }
}

bool Keyboard::GetControllerStatus(uint8_t& status) {
    status = Status();
    return true;
}

void Keyboard::FlushBuffers() {
    head = tail = 0;
    for (int i = 0; i < 1000 && (Status() & 0x01); i++) {
        In(0x60); // Read and discard
    }
}

// USB HID Support stubs
void Keyboard::InitUSB() {
    SerialLogger::Log("Keyboard: USB HID support initialized (stub)\r\n");
}

bool Keyboard::AddUSBDevice(uint8_t device_id) {
    SerialLogger::Log("Keyboard: USB device ");
    SerialLogger::LogHex(device_id);
    SerialLogger::Log(" added (stub)\r\n");
    return true;
}

void Keyboard::RemoveUSBDevice(uint8_t device_id) {
    SerialLogger::Log("Keyboard: USB device ");
    SerialLogger::LogHex(device_id);
    SerialLogger::Log(" removed (stub)\r\n");
}

void Keyboard::ProcessUSBReport(uint8_t device_id, const uint8_t* report, size_t length) {
    // USB HID keyboard report processing would go here
    // For now, just log that we received it
    SerialLogger::Log("Keyboard: USB report from device ");
    SerialLogger::LogHex(device_id);
    SerialLogger::Log(" length ");
    SerialLogger::LogDec(length);
    SerialLogger::Log("\r\n");
}

void Keyboard::Poll() {
    // Copy current to prev
    for(int i=0; i<256; i++) prev_keys[i] = keys[i];
    
    int loop_limit = 100;
    while (loop_limit-- > 0) {
        uint8_t st = Status();
        if (!(st & 0x01)) break;
        if (st & 0x20) break; // Mouse data
        uint8_t sc = In(0x60);
        HandleScancode(sc);
    }
}

bool Keyboard::HasChar() { return head != tail; }
char Keyboard::GetChar() { char c = buf[tail]; tail = (uint8_t)((tail + 1) & 0xFF); return c; }
bool Keyboard::IsKeyDown(Key key) { return keys[(int)key]; }
bool Keyboard::IsKeyPressed(Key key) { return keys[(int)key] && !prev_keys[(int)key]; }
const KeyboardState& Keyboard::GetState() { return state; }
void Keyboard::SetCallback(KeyCallback cb) { callback = cb; }

uint8_t Keyboard::Status() { return In(0x64); }
uint8_t Keyboard::In(uint16_t p) { uint8_t r; __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
void Keyboard::Out(uint16_t p, uint8_t v) { __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(p)); }
void Keyboard::Enqueue(char c) { buf[head] = c; head = (uint8_t)((head + 1) & 0xFF); }

void Keyboard::HandleScancode(uint8_t sc) {
    if (sc == 0xE0) {
        e0_prefix = true;
        return;
    }
    
    bool release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;
    
    Key key = ScancodeToKey(code, e0_prefix);
    e0_prefix = false;
    
    if (key == KEY_UNKNOWN) return;
    
    // Update State
    keys[(int)key] = !release;
    
    if (key == KEY_LSHIFT || key == KEY_RSHIFT) state.shift = !release;
    if (key == KEY_LCTRL || key == KEY_RCTRL) state.ctrl = !release;
    if (key == KEY_LALT || key == KEY_RALT) state.alt = !release;
    if (key == KEY_LSUPER || key == KEY_RSUPER) state.super = !release;
    
    if (!release) {
        if (key == KEY_CAPSLOCK) state.caps_lock = !state.caps_lock;
        if (key == KEY_NUMLOCK) state.num_lock = !state.num_lock;
        if (key == KEY_SCROLLLOCK) state.scroll_lock = !state.scroll_lock;
        
        char c = KeyToChar(key, state.shift, state.caps_lock);
        
        // Log for debug
        SerialLogger::Log("Key: "); SerialLogger::LogDec((int)key); 
        if (c) { SerialLogger::Log(" Char: '"); char tmp[2]={c,0}; SerialLogger::Log(tmp); SerialLogger::Log("'"); }
        SerialLogger::Log("\r\n");
        
        if (c) Enqueue(c);
        
        if (callback) callback(key, c, true);
    } else {
        if (callback) callback(key, 0, false);
    }
}

Key Keyboard::ScancodeToKey(uint8_t sc, bool e0) {
    if (!e0) {
        switch (sc) {
            case 0x01: return KEY_ESC;
            case 0x02: return KEY_1; case 0x03: return KEY_2; case 0x04: return KEY_3; case 0x05: return KEY_4;
            case 0x06: return KEY_5; case 0x07: return KEY_6; case 0x08: return KEY_7; case 0x09: return KEY_8;
            case 0x0A: return KEY_9; case 0x0B: return KEY_0; case 0x0C: return KEY_MINUS; case 0x0D: return KEY_EQUAL;
            case 0x0E: return KEY_BACKSPACE;
            case 0x0F: return KEY_TAB;
            case 0x10: return KEY_Q; case 0x11: return KEY_W; case 0x12: return KEY_E; case 0x13: return KEY_R;
            case 0x14: return KEY_T; case 0x15: return KEY_Y; case 0x16: return KEY_U; case 0x17: return KEY_I;
            case 0x18: return KEY_O; case 0x19: return KEY_P; case 0x1A: return KEY_LBRACKET; case 0x1B: return KEY_RBRACKET;
            case 0x1C: return KEY_ENTER;
            case 0x1D: return KEY_LCTRL;
            case 0x1E: return KEY_A; case 0x1F: return KEY_S; case 0x20: return KEY_D; case 0x21: return KEY_F;
            case 0x22: return KEY_G; case 0x23: return KEY_H; case 0x24: return KEY_J; case 0x25: return KEY_K;
            case 0x26: return KEY_L; case 0x27: return KEY_SEMICOLON; case 0x28: return KEY_QUOTE; case 0x29: return KEY_GRAVE;
            case 0x2A: return KEY_LSHIFT;
            case 0x2B: return KEY_BACKSLASH;
            case 0x2C: return KEY_Z; case 0x2D: return KEY_X; case 0x2E: return KEY_C; case 0x2F: return KEY_V;
            case 0x30: return KEY_B; case 0x31: return KEY_N; case 0x32: return KEY_M; case 0x33: return KEY_COMMA;
            case 0x34: return KEY_PERIOD; case 0x35: return KEY_SLASH;
            case 0x36: return KEY_RSHIFT;
            case 0x37: return KEY_KP_MULTIPLY;
            case 0x38: return KEY_LALT;
            case 0x39: return KEY_SPACE;
            case 0x3A: return KEY_CAPSLOCK;
            case 0x3B: return KEY_F1; case 0x3C: return KEY_F2; case 0x3D: return KEY_F3; case 0x3E: return KEY_F4;
            case 0x3F: return KEY_F5; case 0x40: return KEY_F6; case 0x41: return KEY_F7; case 0x42: return KEY_F8;
            case 0x43: return KEY_F9; case 0x44: return KEY_F10;
            case 0x45: return KEY_NUMLOCK;
            case 0x46: return KEY_SCROLLLOCK;
            case 0x47: return KEY_KP_7; case 0x48: return KEY_KP_8; case 0x49: return KEY_KP_9; case 0x4A: return KEY_KP_SUBTRACT;
            case 0x4B: return KEY_KP_4; case 0x4C: return KEY_KP_5; case 0x4D: return KEY_KP_6; case 0x4E: return KEY_KP_ADD;
            case 0x4F: return KEY_KP_1; case 0x50: return KEY_KP_2; case 0x51: return KEY_KP_3; case 0x52: return KEY_KP_0;
            case 0x53: return KEY_KP_DECIMAL;
            case 0x57: return KEY_F11; case 0x58: return KEY_F12;
            default: return KEY_UNKNOWN;
        }
    } else {
        switch (sc) {
            case 0x1C: return KEY_KP_ENTER;
            case 0x1D: return KEY_RCTRL;
            case 0x35: return KEY_KP_DIVIDE;
            case 0x37: return KEY_PRINTSCREEN;
            case 0x38: return KEY_RALT;
            case 0x47: return KEY_HOME;
            case 0x48: return KEY_UP;
            case 0x49: return KEY_PAGEUP;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x4F: return KEY_END;
            case 0x50: return KEY_DOWN;
            case 0x51: return KEY_PAGEDOWN;
            case 0x52: return KEY_INSERT;
            case 0x53: return KEY_DELETE;
            case 0x5B: return KEY_LSUPER;
            case 0x5C: return KEY_RSUPER;
            case 0x5D: return KEY_MENU;
            default: return KEY_UNKNOWN;
        }
    }
}

char Keyboard::KeyToChar(Key key, bool shift, bool caps) {
    // Apply InputManager Layout Mapping
    // Note: InputManager::MapKey calls Keyboard::KeyToChar recursively if we are not careful.
    // Actually, InputManager::MapKey swaps KEYS and then calls KeyToChar.
    // So we should NOT call InputManager::MapKey here to avoid infinite recursion?
    // No, InputManager::MapKey calls Keyboard::KeyToChar.
    // If Keyboard::KeyToChar calls InputManager::MapKey... recursion.
    // So we must duplicate the swapping logic OR expose a "RawKeyToChar".
    
    // Better: Keyboard::KeyToChar implements the layout logic directly by querying InputManager for current layout type.
    
    Key mapped_key = key;
    KeyLayout layout = InputManager::GetLayout();
    
    if (layout == KeyLayout::QWERTZ_DE) {
        if (key == KEY_Y) mapped_key = KEY_Z;
        else if (key == KEY_Z) mapped_key = KEY_Y;
        // Add more DE specific mappings (e.g. symbols)
    }
    else if (layout == KeyLayout::AZERTY_FR) {
        if (key == KEY_Q) mapped_key = KEY_A;
        else if (key == KEY_A) mapped_key = KEY_Q;
        else if (key == KEY_W) mapped_key = KEY_Z;
        else if (key == KEY_Z) mapped_key = KEY_W;
        else if (key == KEY_M) mapped_key = KEY_SEMICOLON;
        else if (key == KEY_SEMICOLON) mapped_key = KEY_M;
        // Add more FR specific mappings
    }
    
    // Proceed with mapped_key
    key = mapped_key;

    if (key >= KEY_A && key <= KEY_Z) {
        bool upper = caps ^ shift;
        return upper ? ('A' + (key - KEY_A)) : ('a' + (key - KEY_A));
    }
    
    if (key >= KEY_0 && key <= KEY_9) {
        if (!shift) return '0' + (key - KEY_0);
        const char* syms = ")!@#$%^&*(";
        return syms[key - KEY_0];
    }
    
    if (key >= KEY_KP_0 && key <= KEY_KP_9) {
        // Numlock logic could go here
        return '0' + (key - KEY_KP_0);
    }
    
    switch (key) {
        case KEY_SPACE: return ' ';
        case KEY_ENTER: return '\n';
        case KEY_BACKSPACE: return '\b';
        case KEY_TAB: return '\t';
        case KEY_MINUS: return shift ? '_' : '-';
        case KEY_EQUAL: return shift ? '+' : '=';
        case KEY_LBRACKET: return shift ? '{' : '[';
        case KEY_RBRACKET: return shift ? '}' : ']';
        case KEY_BACKSLASH: return shift ? '|' : '\\';
        case KEY_SEMICOLON: return shift ? ':' : ';';
        case KEY_QUOTE: return shift ? '"' : '\'';
        case KEY_GRAVE: return shift ? '~' : '`';
        case KEY_COMMA: return shift ? '<' : ',';
        case KEY_PERIOD: return shift ? '>' : '.';
        case KEY_SLASH: return shift ? '?' : '/';
        case KEY_KP_DIVIDE: return '/';
        case KEY_KP_MULTIPLY: return '*';
        case KEY_KP_SUBTRACT: return '-';
        case KEY_KP_ADD: return '+';
        case KEY_KP_DECIMAL: return '.';
        default: return 0;
    }
}
