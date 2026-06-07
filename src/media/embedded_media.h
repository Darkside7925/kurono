#pragma once
#include "../kernel/types.h"

//  embedded media assets  -  linked via objcopy from binary files
//
//  objcopy converts the file name (with dots/dashes replaced by underscores)
//  into linker symbols:
//    denji.mp4 → _binary_denji_mp4_{start,end,size}

extern "C" {
    extern const uint8_t _binary_denji_mp4_start[]  __attribute__((weak));
    extern const uint8_t _binary_denji_mp4_end[]    __attribute__((weak));
    extern const uint8_t _binary_denji_kvid_start[] __attribute__((weak));
    extern const uint8_t _binary_denji_kvid_end[]   __attribute__((weak));
    // user-imported video: ssstik.mp4 (original h264/aac) + ssstik.kvid
    // (host-transcoded playable form). (satoru)
    extern const uint8_t _binary_ssstik_mp4_start[]  __attribute__((weak));
    extern const uint8_t _binary_ssstik_mp4_end[]    __attribute__((weak));
    extern const uint8_t _binary_ssstik_kvid_start[] __attribute__((weak));
    extern const uint8_t _binary_ssstik_kvid_end[]   __attribute__((weak));
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

// the playable native version (transcoded host-side via
// tools/transcode_to_kvid.ps1).  this is what the video player
// actually decodes; the .mp4 above is preserved for the demuxer
// to inspect its real metadata.
inline bool HasDenjiKVID() {
    return _binary_denji_kvid_start != nullptr &&
           _binary_denji_kvid_end   != nullptr &&
           +_binary_denji_kvid_end > +_binary_denji_kvid_start;
}

inline const uint8_t* DenjiKVIDData() {
    return _binary_denji_kvid_start;
}

inline uint32_t DenjiKVIDSize() {
    return (uint32_t)((uintptr_t)_binary_denji_kvid_end -
                      (uintptr_t)_binary_denji_kvid_start);
}

// user-imported ssstik.mp4 (original)  -  preserved for inspection/transcode (satoru)
inline bool HasSsstikMP4() {
    return _binary_ssstik_mp4_start != nullptr &&
           _binary_ssstik_mp4_end   != nullptr &&
           +_binary_ssstik_mp4_end > +_binary_ssstik_mp4_start;
}
inline const uint8_t* SsstikMP4Data() { return _binary_ssstik_mp4_start; }
inline uint32_t SsstikMP4Size() {
    return (uint32_t)((uintptr_t)_binary_ssstik_mp4_end -
                      (uintptr_t)_binary_ssstik_mp4_start);
}

// user-imported ssstik.kvid (playable native form) (satoru)
inline bool HasSsstikKVID() {
    return _binary_ssstik_kvid_start != nullptr &&
           _binary_ssstik_kvid_end   != nullptr &&
           +_binary_ssstik_kvid_end > +_binary_ssstik_kvid_start;
}
inline const uint8_t* SsstikKVIDData() { return _binary_ssstik_kvid_start; }
inline uint32_t SsstikKVIDSize() {
    return (uint32_t)((uintptr_t)_binary_ssstik_kvid_end -
                      (uintptr_t)_binary_ssstik_kvid_start);
}

} // namespace embeddedmedia
