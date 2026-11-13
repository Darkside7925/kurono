// KLD firmware store: on-demand firmware for a container, from the KURONO
// partition, one directory per driver family.
//
// The embedded set (KURONO_KLD_FW=1) is unpacked into KVFS at boot and is
// served first; a miss is materialized here from the KURONO FAT32 partition:
//   /firmware/<family>/MANIFEST.TXT   exact driver name -> 8.3 store file
//   /firmware/<family>/<store83>      the bytes
//   /firmware/REGULATO.DB             shared regulatory.db
//   /firmware/REGULATO.P7S            shared regulatory.db.p7s
// The exact long names live in the manifest because the kernel's existing
// FAT32 reader has no LFN support; using it is a hard rule (no new fs code).
//
// The caller's family comes from the broker's per-container record (G3) with
// the process exe_path as the fallback for test families. A container cannot
// name a path: everything after /system/lib/firmware/ must be one plain
// component, else the request is refused and logged. Bytes are copied into
// KVFS, so the existing shim open/read path is unchanged.
//
// (satoru)

#pragma once

#include "types.h"

namespace KldFw {

// if kurono_path is under /system/lib/firmware/ and KVFS does not have it,
// try to serve it from the KURONO partition store for this container.
// address_space and exe_path identify the caller's family. cheap on a hit or
// on any other path. false = the requested name is not a plain firmware file
// name (path escape): the caller must make the whole lookup fail, not fall
// through to a kvfs/ext4 path that ".." might reach. (satoru)
bool EnsureFile(const char* kurono_path, uint64_t address_space,
                const char* exe_path);

// diagnostics for the shell/status (optional). (satoru)
uint32_t Served();
uint32_t Misses();
uint32_t Refused();

}  // namespace KldFw

// end (satoru)
