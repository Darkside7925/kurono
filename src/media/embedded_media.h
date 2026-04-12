#pragma once
#include "../kernel/types.h"

//  embedded media assets  -  linked via objcopy from binary files
//
//  objcopy converts the file name (with dots/dashes replaced by underscores)
//  into linker symbols:
//    denji.mp4 → _binary_denji_mp4_{start,end,size}

extern "C" {
    extern const uint8_t _binary_denji_mp4_start[] __attribute__((weak));
    extern const uint8_t _binary_denji_mp4_end[]   __attribute__((weak));
}

namespace EmbeddedMedia {

inline bool HasDenjiMP4() {
    return _binary_denji_mp4_start != nullptr &&
           _binary_denji_mp4_end   != nullptr &&
           +_binary_denji_mp4_end > +_binary_denji_mp4_start;
}

inline const uint8_t* DenjiMP4Data() {
    return _binary_denji_mp4_start;
}

inline uint32_t DenjiMP4Size() {
    return (uint32_t)((uintptr_t)_binary_denji_mp4_end -
                      (uintptr_t)_binary_denji_mp4_start);
}

} // namespace embeddedmedia
