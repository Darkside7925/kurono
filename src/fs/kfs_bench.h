#pragma once
#include "../kernel/types.h"

//  kfs_bench  -  headless KFS / NVMe storage benchmark. gated behind the
//  kurono.kfsbench cmdline token (parsed in kurono_kernel.cpp). it measures the
//  storage stack against the attached nvme DATA disk (NOT the boot disk) and logs
//  every number to serial so a CI run can scrape it:
//    - sequential write MB/s  (large contiguous file through KFS)
//    - sequential read  MB/s
//    - random 4 KB write IOPS (raw nvme 4 KB writes  -  the lsm/page-cache layers
//      build on this primitive)
//    - snapshot save time (full) + incremental
//    - boot restore time
//    - nvme command count per op (extent efficiency: 1 extent / contiguous file)
//
//  it is destructive to the data disk (it formats a fresh KFS volume), so it only
//  runs when explicitly requested and the disk is a dedicated scratch image. it
//  powers the machine off when done so a headless qemu run terminates. (satoru)

class KfsBench {
public:
    //  run the full suite against the nvme data disk and log results. returns
    //  false if no nvme device is present. (satoru)
    static bool Run();
};
// end (satoru)
