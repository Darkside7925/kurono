//  kurono os - wayland bridge: guest wayland clients as native kurono windows
//
//  the guest runs kwl-proxy (scratchpad/agent_alp_guest/kwl), which is the
//  guest's WAYLAND_DISPLAY. it relays each guest client's wire protocol over
//  the third virtio console (hvc2) to here, where every guest client becomes a
//  kernel-side client of kurono's own wayland server (a unix socket connected
//  to wayland-0). the server makes the windows: kurono title bars, motion,
//  dock, focus and input, exactly as for a linux program running on kurono.
//
//  shm pixels never travel through the channel: the proxy pins each pool's
//  pages (mlock) and sends their guest page frame numbers with every commit;
//  the bridge gathers the committed buffer straight out of guest ram into a
//  host pool buffer that the server reads. server -> client fds (the keymap)
//  are sent to the guest as byte blobs that the proxy turns into memfds.
//
//  frame format, both directions: { u8 type, u8 nfd, u16 cid, u32 len } + len
//  payload bytes, little endian. everything here runs on the vm thread. (satoru)
#pragma once
#include <stdint.h>

namespace WLBridge {
    void Reset();                                        // vm start / stop: drop every client
    void OnGuestBytes(int idx, const uint8_t* data, int n);  // hvc2 sink (vm thread)
    void Poll();                                         // vm thread: server -> guest traffic
    int  Status(char* out, int max);
    int  ClientCount();
}

// end (satoru)
