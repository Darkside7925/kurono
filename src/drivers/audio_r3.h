#pragma once
//  kurono os - ring-3 audio daemon broker (phase 3 of "separate everything").
//
//  With kurono.r3audio=1 the pulseaudio-compatible server and the per-client
//  mixing run in a ring-3 process (/usr/bin/audiod). The kernel keeps only the
//  hardware: the HDA/AC97/USB backend, its DMA ring and the system mixer (for
//  kernel clients and beeps). The daemon hands its mixed 48 kHz s16 stereo
//  output to the kernel as ONE stream of that mixer through SYS_KAUD_CALL, so
//  master volume / EQ / limiter / backend switching keep working unchanged.
//
//  Only the process the boot runner spawned (SetOwnerPid) may call it, and a
//  daemon that stops talking to the broker has its stream closed after a
//  timeout, so a crashed daemon cannot leave the hardware pumping forever.

#include "../kernel/types.h"

namespace R3Audio {

// "KAUD" - dispatched from the x64 gate next to SYS_KLD_CALL.
constexpr uint64_t SYS_KAUD_CALL = 0x4B415544ull;

enum Op : uint32_t {
    OP_OPEN     = 1,   // open (or reuse) the daemon's kernel stream. returns backend rate, or <0
    OP_WRITE    = 2,   // a0=user ptr to s16le stereo frames, a1=frames. returns frames accepted
    OP_QUEUED   = 3,   // frames sitting in the kernel stream ring (not yet pulled by the mixer)
    OP_CONSUMED = 4,   // total frames the mixer has pulled from the stream
    OP_LATENCY  = 5,   // frames queued in the backend's hardware ring
    OP_CLOSE    = 6,   // let what is queued play out, then free the stream
    OP_PING     = 7    // keep-alive; returns 1 when a backend is ready
};

enum Err : int64_t { E_PERM = -1, E_INVAL = -22, E_NODEV = -19, E_FAULT = -14, E_BUSY = -16 };

void    SetEnabled(bool on);
bool    Enabled();
void    SetOwner(uint64_t address_space);   // threads of the daemon share it, so key on it, not the pid
int64_t Call(uint64_t address_space, uint32_t op, uint64_t a0, uint64_t a1);
// from the audio kernel process: closes the stream when the daemon went quiet.
void    Tick();
// true while the daemon has called the broker within the last 3 s (it pings every second).
bool    DaemonAlive();

}  // namespace R3Audio
