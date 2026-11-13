#include "laptop_keys.h"   // system keys (satoru)
#include "input_manager.h"
#include "../ui/control_center.h"   // shell keyboard owner (satoru)
#include "../kui/kui_kernel.h"   // kui key edges (satoru)
#include "../ui/window_manager.h"
#include "../ui/wayland_server.h"
#include "../ui/clipboard_ui.h"
#include "../apps/terminal.h"
#include "../apps/system_settings.h"   // SystemSettings::KeyEvent (satoru)
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/usb.h"
#include "../fs/vfs.h"
#include "../system/screenshot.h"   // print screen key (satoru)
#include "../apps/alpine_term.h"   // the alpine vm terminal takes raw keys (satoru)

KeyboardDevice InputManager::devices[8];
int InputManager::device_count = 0;
KeyLayout InputManager::current_layout = KeyLayout::QWERTY_US;
Key InputManager::last_key = KEY_UNKNOWN;
bool InputManager::last_key_pressed = false;
int InputManager::last_device_id = -1;

// event ring drop count [0] and key-downs queued [1], shown in [hb] (satoru)
volatile uint32_t g_imdbg[4] = {0};

namespace {
constexpr int IM_MAX_SUBSCRIBERS = 16;
struct Subscriber {
    SubscriberId id;
    InputManager::KeyEventCallback cb;
    void* ctx;
};
Subscriber g_subs[IM_MAX_SUBSCRIBERS] = {};
SubscriberId g_next_sub_id = 1;
SubscriberId g_focus_sub = 0;

// Tiny ring of (device, key, pressed) events written from the keyboard
// callback and drained at Poll(). The callback may be invoked from an IRQ
// or from the polling tick - either way the consumer side must never block.
struct ImEvent { int device_id; Key key; bool pressed; };
constexpr int IM_EVENT_RING = 128;
ImEvent g_events[IM_EVENT_RING];
volatile uint32_t g_ev_head = 0;
volatile uint32_t g_ev_tail = 0;

bool push_event(int device_id, Key key, bool pressed) {
    uint32_t h = __atomic_load_n(&g_ev_head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&g_ev_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (h + 1) % IM_EVENT_RING;
    if (next == (t % IM_EVENT_RING)) { __atomic_add_fetch(&g_imdbg[0], 1, __ATOMIC_RELAXED); return false; }
    if (pressed) __atomic_add_fetch(&g_imdbg[1], 1, __ATOMIC_RELAXED);
    g_events[h % IM_EVENT_RING] = {device_id, key, pressed};
    __atomic_store_n(&g_ev_head, next, __ATOMIC_RELEASE);
    return true;
}

bool pop_event(ImEvent& out) {
    uint32_t h = __atomic_load_n(&g_ev_head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&g_ev_tail, __ATOMIC_RELAXED);
    if (h == t) return false;
    out = g_events[t % IM_EVENT_RING];
    __atomic_store_n(&g_ev_tail, (t + 1) % IM_EVENT_RING, __ATOMIC_RELEASE);
    return true;
}

void dispatch_to_focus(int device_id, Key key, bool pressed) {
    SubscriberId target = __atomic_load_n(&g_focus_sub, __ATOMIC_ACQUIRE);
    if (target == 0) return;
    for (int i = 0; i < IM_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].id == target && g_subs[i].cb) {
            g_subs[i].cb(g_subs[i].ctx, device_id, key, pressed);
            return;
        }
    }
    // Focused subscriber vanished mid-stream - drop the event silently rather
    // than crash. The next Subscribe() / SetFocus() reattaches routing.
}
} // namespace

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
    for (int i = 0; i < IM_MAX_SUBSCRIBERS; i++) g_subs[i] = {};
    g_next_sub_id = 1;
    g_focus_sub = 0;
    g_ev_head = g_ev_tail = 0;

    RegisterDevice("Standard PS/2 Keyboard", DeviceType::PS2);
    SyncUSBHIDDevices();

    SerialLogger::Log("InputManager: Initialized\r\n");

    // Low-level driver callback: enqueue, do not block. The previous
    // implementation issued a synchronous VFS write inside the keyboard IRQ
    // path, which stalled input under any FS load.
    Keyboard::SetCallback([](Key k, char c, bool p){
        (void)c;
        push_event(0, k, p);
    });
}

void InputManager::Poll() {
    // NOTE: Keyboard::Poll()/Mouse::Poll() are NOT called here. InputProcessEntry
    // already polls both devices directly each tick; calling them again here
    // doubled the 8042 status-port reads + VMware backdoor probes (every one a
    // VM-exit) for zero benefit. This now only drains the event ring. (satoru)

    // keys forwarded to a wayland window; released if focus leaves it. (satoru)
    TerminalApp::PtyPoll();   // hang up a pty session whose window was closed (satoru)
    static bool s_wl_down[KEY_WAKE + 1] = {};
    static int  s_wl_win = -1;
    Window* fw = WindowManager::GetFocusedWindow();
    int wl_win = (fw && fw->state != WIN_CLOSED && WaylandServer::IsWaylandWindow(fw->id)) ? fw->id : -1;
    if (wl_win != s_wl_win) {
        for (int k = 1; k <= (int)KEY_WAKE; k++)
            if (s_wl_down[k]) { WaylandServer::ForwardKey(k, false, 0); s_wl_down[k] = false; }
        s_wl_win = wl_win;
    }

    // Drain pending key events onto the routing fabric.
    ImEvent ev;
    while (pop_event(ev)) {
        last_key = ev.key;
        last_key_pressed = ev.pressed;
        last_device_id = ev.device_id;
        // mute / volume / brightness / wi-fi keys belong to the system. (satoru)
        if (LaptopKeys::Handle(ev.key, ev.pressed)) continue;
        if (ev.pressed) OnKeyDown(ev.device_id, ev.key);
        else            OnKeyUp(ev.device_id, ev.key);
        // modifiers from the event stream itself, not the driver's latest state,
        // so queued events keep their own shift/ctrl. (satoru)
        static bool s_held[KEY_WAKE + 1] = {};
        int k = (int)ev.key;
        bool was_held = (k > 0 && k <= (int)KEY_WAKE) ? s_held[k] : false;   // auto-repeat check (satoru)
        if (k > 0 && k <= (int)KEY_WAKE) s_held[k] = ev.pressed;
        // an open control center owns every key edge (its own focus/wifi field). (satoru)
        if (ControlCenter::HandleKeyEvent(k, s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT],
                                          s_held[KEY_LCTRL] || s_held[KEY_RCTRL],
                                          s_held[KEY_LALT] || s_held[KEY_RALT], ev.pressed))
            continue;
        // super+v: clipboard history popup. the key press never reaches the app. (satoru)
        bool clip_consumed = false;
        if (ev.pressed && !was_held && k == (int)KEY_V &&
            (s_held[KEY_LSUPER] || s_held[KEY_RSUPER])) {
            ClipboardUI::Toggle();
            clip_consumed = true;
        }
        // while the popup is open it owns the keyboard (esc, arrows, enter). (satoru)
        if (ClipboardUI::IsOpen() &&
            ClipboardUI::KeyEvent(k, s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT],
                                  s_held[KEY_LCTRL] || s_held[KEY_RCTRL],
                                  s_held[KEY_LALT] || s_held[KEY_RALT]))
            clip_consumed = true;
        if (clip_consumed) continue;
        // super+d: show desktop. consumed so the focused app never sees it. (satoru)
        if (ev.pressed && !was_held && k == (int)KEY_D &&
            (s_held[KEY_LSUPER] || s_held[KEY_RSUPER])) {
            WindowManager::ToggleShowDesktop();
            continue;
        }
        // print screen belongs to the desktop: the whole screen, shift+print the
        // focused window. the capture itself runs on the desktop frame. (satoru)
        if (k == (int)KEY_PRINTSCREEN) {
            if (ev.pressed && !was_held)
                Screenshot::Request(s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT]);
            continue;
        }
        // arrows step an open switcher too, like tab and shift+tab. (satoru)
        if (ev.pressed && WindowManager::IsTabSwitcherOpen() &&
            (k == (int)KEY_LEFT || k == (int)KEY_RIGHT)) {
            WindowManager::AltTabStep(k == (int)KEY_LEFT);
            continue;
        }
        // alt+tab drives the live switcher. tab steps are consumed; releasing
        // alt commits the selection. esc cancels. (satoru)
        if (k == (int)KEY_TAB && (s_held[KEY_LALT] || s_held[KEY_RALT])) {
            if (ev.pressed && !was_held)
                WindowManager::AltTabStep(s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT]);
            continue;
        }
        if (ev.pressed && !was_held && k == (int)KEY_ESC &&
            WindowManager::IsTabSwitcherOpen()) {
            WindowManager::AltTabCancel();
            continue;
        }
        if (!ev.pressed && was_held && (k == (int)KEY_LALT || k == (int)KEY_RALT) &&
            WindowManager::IsTabSwitcherOpen()) {
            WindowManager::AltTabFinish();
        }
        // a terminal running a pty program gets raw key events (arrows, ctrl, alt) (satoru)
        if (ev.pressed)
            TerminalApp::PtyKeyEvent(k, s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT],
                                     s_held[KEY_LCTRL] || s_held[KEY_RCTRL],
                                     s_held[KEY_LALT] || s_held[KEY_RALT]);
        if (ev.pressed)
            AlpineTerminal::KeyEvent(k, s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT],
                                     s_held[KEY_LCTRL] || s_held[KEY_RCTRL],
                                     s_held[KEY_LALT] || s_held[KEY_RALT]);   // (satoru)
        // a focused kui window takes the keys the char queue never carries, and key ups (satoru)
        KuiKernel::KeyEvent(k, ev.pressed, s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT],
                            s_held[KEY_LCTRL] || s_held[KEY_RCTRL],
                            s_held[KEY_LALT] || s_held[KEY_RALT]);
        // settings takes arrows / esc / tab / paging when it has focus (satoru)
        if (ev.pressed)
            SystemSettings::KeyEvent(k, s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT],
                                     s_held[KEY_LCTRL] || s_held[KEY_RCTRL],
                                     s_held[KEY_LALT] || s_held[KEY_RALT]);
        // raw key edges -> the focused wayland client (gtk reads evdev keys). (satoru)
        // alt+tab belongs to the desktop, not to the wayland app under it (satoru)
        bool alt_tab_key = (k == (int)KEY_TAB) && (s_held[KEY_LALT] || s_held[KEY_RALT]);
        if (wl_win >= 0 && !alt_tab_key) {
            uint32_t mods = 0;
            if (s_held[KEY_LSHIFT] || s_held[KEY_RSHIFT]) mods |= WaylandServer::WL_MOD_SHIFT;
            if (s_held[KEY_LCTRL]  || s_held[KEY_RCTRL])  mods |= WaylandServer::WL_MOD_CTRL;
            if (s_held[KEY_LALT]   || s_held[KEY_RALT])   mods |= WaylandServer::WL_MOD_ALT;
            if (s_held[KEY_LSUPER] || s_held[KEY_RSUPER]) mods |= WaylandServer::WL_MOD_META;
            WaylandServer::ForwardKey(k, ev.pressed, mods);
            if (k > 0 && k <= (int)KEY_WAKE) s_wl_down[k] = ev.pressed;
        }
    }

    SyncUSBHIDDevices();
}

SubscriberId InputManager::Subscribe(KeyEventCallback cb, void* ctx) {
    if (!cb) return 0;
    for (int i = 0; i < IM_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].id == 0) {
            SubscriberId id = g_next_sub_id++;
            if (id == 0) id = g_next_sub_id++; // skip the sentinel
            g_subs[i].id = id;
            g_subs[i].cb = cb;
            g_subs[i].ctx = ctx;
            return id;
        }
    }
    return 0;
}

void InputManager::Unsubscribe(SubscriberId id) {
    if (id == 0) return;
    for (int i = 0; i < IM_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].id == id) {
            g_subs[i] = {};
            // Clearing focus on unsubscribe prevents the next routed key
            // from being delivered to a stale slot.
            SubscriberId cur = __atomic_load_n(&g_focus_sub, __ATOMIC_RELAXED);
            if (cur == id) __atomic_store_n(&g_focus_sub, (SubscriberId)0, __ATOMIC_RELEASE);
            return;
        }
    }
}

void InputManager::SetFocus(SubscriberId id) {
    if (id != 0) {
        bool exists = false;
        for (int i = 0; i < IM_MAX_SUBSCRIBERS; i++) {
            if (g_subs[i].id == id) { exists = true; break; }
        }
        if (!exists) return;
    }
    __atomic_store_n(&g_focus_sub, id, __ATOMIC_RELEASE);
}

SubscriberId InputManager::GetFocus() {
    return __atomic_load_n(&g_focus_sub, __ATOMIC_ACQUIRE);
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
    // /input.log append. this is process context (Poll or the boot self-test),
    // never the irq path, and the ramdisk write is bounded to 16k, so it can
    // not stall the interrupt-time capture. the boot self-test reads this
    // back to prove both device ids reach the routing layer. (satoru)
    {
        FileNode* f = VFS::Open("/input.log");
        if (f) {
            char msg[48];
            int n = 0;
            const char* p = "KEY DEV:";
            while (*p && n < 40) msg[n++] = *p++;
            int v = device_id < 0 ? 0 : device_id;
            char rev[8];
            int rn = 0;
            if (v == 0) rev[rn++] = '0';
            while (v > 0 && rn < 7) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
            while (rn > 0 && n < 44) msg[n++] = rev[--rn];
            const char* m = " CHAR:";
            while (*m && n < 44) msg[n++] = *m++;
            const KeyboardState& st = Keyboard::GetState();
            char c = Keyboard::KeyToChar(key, st.shift, st.caps_lock);
            msg[n++] = (c >= 32 && c <= 126) ? c : '?';
            msg[n++] = '\n';
            VFS::Write(f, f->size, (uint32_t)n, (uint8_t*)msg);
        }
    }
    dispatch_to_focus(device_id, key, true);
}

void InputManager::OnKeyUp(int device_id, Key key) {
    last_key = key;
    last_key_pressed = false;
    last_device_id = device_id;
    dispatch_to_focus(device_id, key, false);
}
