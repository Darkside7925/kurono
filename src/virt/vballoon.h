//  kurono os - virtio-balloon host device (free page reporting only)
//
//  the guest's virtio_balloon driver reports 2 mb aligned runs of free pages through
//  a reporting queue; every lazy guest ram chunk wholly inside a run goes back to the
//  host and is faulted in again, zeroed, if the guest touches it later. there is no
//  inflate / deflate policy: the guest returns what it is not using on its own. (satoru)
#pragma once
#include <stdint.h>

namespace VBalloon {
    bool Init(uint8_t irq);
    uint64_t ReleasedBytes();      // total handed back since the vm started (satoru)
    uint32_t Reports();
}

// end (satoru)
