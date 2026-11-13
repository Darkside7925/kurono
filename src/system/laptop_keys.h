// laptop_keys: the system keys (mute, volume, brightness, wi-fi, play/pause)
// handled before any app sees them. volume steps by 6 % and remembers the
// level per output; brightness goes to the panel backlight when one is
// driven (system/backlight); the wi-fi key turns the radio on and off.
// (satoru)
#pragma once
#include "../drivers/keyboard.h"

namespace LaptopKeys {
// true when the key is a system key (consumed, press and release). (satoru)
bool Handle(Key k, bool pressed);
}

// end (satoru)
