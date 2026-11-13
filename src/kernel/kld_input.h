#pragma once
#include "types.h"

// touchpad pointer bridge: the container's tp_logic sends deltas/buttons/wheel,
// the kernel injects them through the same USB-HID mouse path as a real mouse.
namespace KldInput {
void Pointer(int dx, int dy, uint32_t buttons, int wheel);
void SetVerbose(bool on);
}
// end (satoru)
