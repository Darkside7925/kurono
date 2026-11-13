#pragma once
#include "../kernel/types.h"

// sntp - network time. once the network is up a small kernel process asks
// pool.ntp.org (then time.google.com, time.cloudflare.com) for the time over
// udp 123, steps the realtime base by the measured offset and logs it, then
// repeats every few hours. only the realtime base moves; the monotonic clock
// is never touched. kurono.ntp=0 turns it off. (satoru)

namespace Sntp {

// false = kurono.ntp=0 (set by the boot flag parser before SpawnAll). (satoru)
extern bool g_enabled;

// one query against the server list. returns true and fills the offset
// (server minus local, ms) and the round trip on success. (satoru)
bool SyncOnce(int64_t* offset_ms, int* rtt_ms);

// the kernel process body: waits for the network, syncs, sleeps. (satoru)
[[noreturn]] void ProcessEntry();

// last successful sync: offset in ms and the uptime ms it happened at (0 = never). (satoru)
int64_t LastOffsetMs();
uint64_t LastSyncUptimeMs();

}  // namespace Sntp

// end (satoru)
