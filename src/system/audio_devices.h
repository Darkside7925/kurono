// audio_devices: the output device list Settings and the Control Center show,
// with a volume remembered per device (like every desktop does: headphones and
// speakers keep their own level). wraps AudioServer's backend registry; the
// levels live in ui.conf as audio.vol.<backend>. there is no capture driver
// yet, so InputCount() is 0. (satoru)
#pragma once
#include "../kernel/types.h"

namespace AudioDevices {

int  OutputCount();
// name: the backend id ("hda", "usb", ...); label: what a person reads. (satoru)
bool OutputInfo(int i, char* name, int ncap, char* label, int lcap, bool* active, bool* ready);
// switch the active output and apply its remembered volume. (satoru)
bool SelectOutput(const char* name);
// store the current master volume for the active output (call on commit). (satoru)
void RememberVolume(int v);
int  InputCount();

}  // namespace AudioDevices

// end (satoru)
