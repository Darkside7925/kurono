//  kurono os - guest audio bridge
//
//  the guest's pulseaudio pipe sink writes raw pcm to /dev/hvc3. the guest
//  side (kwl-audio in scratchpad/agent_alp_guest) sets the tty to raw mode,
//  sends one 16 byte format header and then interleaved frames. this side
//  turns those bytes into a normal kurono mixer stream, so guest apps are
//  heard through the same mixer, master volume and eq as native programs.
//
//  the byte ring is single producer / single consumer: the vm thread is the
//  only one that appends (the virtio console sink) and the only one that
//  drains (Pump from the vm host loop), so no lock is needed. (satoru)
#pragma once
#include <stdint.h>

namespace VAudio {
    // hvc3 sink callback, called on the vm thread (satoru)
    void OnGuestBytes(int idx, const uint8_t* data, int n);

    // flow control for the hvc3 virtio queue: false while the ring is full, so
    // the guest's writes block and pulseaudio paces itself to real time
    // instead of dumping a whole track into the mixer at once. (satoru)
    bool CanAccept(int idx);

    // drain the ring into the audio mixer; called from the vm host loop (satoru)
    void Pump();

    // close the mixer stream and forget the guest format (vm teardown) (satoru)
    void Reset();

    // human readable one line for `vm status` (satoru)
    int  Status(char* out, int max);
}

// end (satoru)
