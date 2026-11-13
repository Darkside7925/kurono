//  kurono os - virtio-console host device (hvc0 / hvc1 in the guest)
//
//  a plain (single port, no multiport) virtio console: queue 0 carries host
//  to guest bytes, queue 1 guest to host bytes. instance 0 is the guest's
//  login console (the "Alpine" terminal window talks to it), instance 1 is the
//  line channel of the guest agent (poweroff, ping, ...), instance 2 the
//  wayland bridge channel (see wlbridge.h). VIRTIO_CONSOLE_F_SIZE
//  is offered so a terminal resize reaches the guest tty as a config change.
//
//  the byte rings are single producer / single consumer: the vm thread is the
//  only one that touches the virtqueues, any other thread only fills the input
//  ring or drains the output ring, so no lock is needed. (satoru)
#pragma once
#include <stdint.h>

namespace GuestConsole {
    static const int MAX_CONSOLES = 4;   // hvc0 login, hvc1 agent, hvc2 wayland bridge, hvc3 guest audio (satoru)

    bool Init(int idx, uint8_t irq, const char* name);   // vm creation, vm thread
    void Poll(int idx);                                  // vm thread: deliver pending input

    // any thread
    int  Write(int idx, const void* data, int n);        // host -> guest, returns bytes queued
    // guest -> host bytes are handed to this sink on the vm thread (satoru)
    void SetSink(int idx, void (*sink)(int idx, const uint8_t* data, int n));
    // optional gate for the guest -> host queue: while it returns false the
    // pending transmit buffers are left in the queue, so the guest blocks (satoru)
    void SetFlow(int idx, bool (*ready)(int idx));
    int  InputSpace(int idx);
    bool DriverReady(int idx);                           // guest driver is up (DRIVER_OK)
    void SetSize(int idx, int cols, int rows);           // terminal size, sent as a config change
    uint64_t BytesOut(int idx);
    uint64_t BytesIn(int idx);
    void ResetRings(int idx);
    void ReleaseAll();                                   // vm teardown (satoru)
}

// end (satoru)
