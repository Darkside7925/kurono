// kcrash: driver crash records and the recovery policy files on the KURONO
// partition (KCRASH.LOG, KDRIVERS.CFG, KBOOT.CFG). the ring in memory always
// works; the disk part needs kurono.kcrash=1 and a KURONO partition, and on a
// failure path only overwrites preallocated files in place. (satoru)
//
// on-disk formats, all little endian, version 1, fnv-1a 32 checksums: (satoru)
//
// KCRASH.LOG, 1 MiB: 4 KiB header { u32 magic 'KCRH', u32 version, u32 next
//   slot, u32 count, u32 fnv(bytes 0..15) }, then 16 slots of 4 KiB at
//   4096 + i * 4096, each holding one 128 byte Record (fnv in its last u32).
//   the rest of the file is reserved (later: the last log lines). (satoru)
// KDRIVERS.CFG, 4 KiB: { u32 magic 'KDRV', u16 version, u16 count, u32
//   fnv(bytes 16..4095), u32 reserved } then 64 entries of 32 bytes { char
//   family[16], u8 policy (0 enabled, 1 disabled, 2 nostop), u8 pad[3], u32
//   fails, u32 last_fail_boot, u32 reserved }. (satoru)
// KBOOT.CFG, 512 B: { u32 magic 'KBOT', u16 version, u16 pad, u32 boot_count,
//   u32 flags (bit0 recovery once, bit1 previous kernel once), char
//   last_good[32], ... , u32 fnv(bytes 0..507) at 508 }. (satoru)
#pragma once
#include <stdint.h>

namespace KCrash {

enum Kind : uint16_t { KIND_KLD = 1 };
enum Result : uint16_t { RES_RESTARTING = 1, RES_GAVE_UP = 2 };

struct __attribute__((packed)) Record {
    uint32_t magic;        // 'KCR1' (satoru)
    uint16_t version;
    uint16_t kind;
    uint32_t boot_id;      // KBOOT.CFG boot counter, 0 without the disk (satoru)
    uint32_t seq;
    uint64_t uptime_ms;
    uint64_t rtc;          // yyyymmddhhmmss, 0 if the rtc is unreadable (satoru)
    uint32_t stop_code;    // reserved (satoru)
    char     family[16];
    uint32_t bdf;
    uint16_t vendor;
    uint16_t device;
    int32_t  pid;
    int32_t  rc;
    int32_t  signal;       // 9 = killed by the supervisor (hang), 0 = unknown (satoru)
    uint32_t pad0;
    uint64_t fault_addr;   // 0 = not known (satoru)
    uint16_t attempt;
    uint16_t result;
    uint32_t pad1;
    char     action[32];
    uint32_t checksum;     // fnv-1a over the first 124 bytes (satoru)
};
static_assert(sizeof(Record) == 128, "kcrash record is 128 bytes");

// once, from a kernel process (it reads and maybe creates the files). (satoru)
void BootInit(bool disk);
// a fresh 512-byte KBOOT.CFG (boot counter 0, no flags): the installer writes
// it so a new install has the boot watch from its first start. (satoru)
void BootFileImage(uint8_t* out512);
bool FamilyDisabled(const char* family);
// fill the header fields, push to the ring, print "[kcrash] ...", and try to
// write it to KCRASH.LOG (kept in memory and retried when the disk is busy). (satoru)
void Report(Record& r);
// the supervisor gave a family up: bump its KDRIVERS.CFG fail counter. (satoru)
void NoteGiveUp(const char* family);
// write records / KDRIVERS.CFG that a busy disk deferred. cheap when nothing
// is pending; the supervisor calls it from its poll loop. (satoru)
void FlushPending();
bool Pending();

// recovery (R7) reads: the newest record found on disk at BootInit (or the
// newest of this boot), and the KDRIVERS.CFG table. policy: 0 enabled,
// 1 disabled, 2 nostop. SetDriverPolicy creates the entry when missing and
// writes the file through the normal pending flush. (satoru)
bool LastRecord(Record* out);
int  DriverCount();
bool DriverAt(int i, char* family16, uint8_t* policy, uint32_t* fails);
bool SetDriverPolicy(const char* family, uint8_t policy);

}  // namespace KCrash

// end (satoru)
