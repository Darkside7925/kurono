#pragma once
//  kurono os - usb audio class (uac1 / uac2) playback driver
//
//  descriptor parser + isochronous OUT streaming on top of the xhci driver
//  (usb.cpp), exposed to the mixer as the "usbaudio" backend. one playback
//  device at a time. see usb_audio.cpp for the design notes. (satoru)
#include "../kernel/types.h"

namespace UsbAudio {
    // a usable playback device was found and parsed (not necessarily streaming)
    bool Present();
    // backend is open and feeding the device
    bool Streaming();
    // multi-line status text for `audio status`
    void Describe(char* out, int max_len);
    // stop and start again (audio restart)
    bool Restart();
    // descriptor-parser self test with fabricated uac1 / uac2 / uac3 configs
    // (kurono.uactest=1). prints [uactest] lines, returns true when all pass.
    bool SelfTest();
    // install the usb driver hooks; call before USB::Init (global constructors run later)
    void EarlyInit();
    // AudioServer::Init finished (hotplug may switch backends from now on)
    void NoteAudioUp();
    // the user picked a backend by hand: stop auto-switching on hotplug
    void NoteUserChoice();
}

// end (satoru)
