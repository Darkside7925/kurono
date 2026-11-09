#pragma once
#include "../kernel/types.h"
#include "../system/kasset.h"   // demo videos load on demand (satoru)

//  embedded media assets - linked via objcopy from binary files
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

// true only when the demo videos are linked in; on-demand copies are not
// preloaded into /home at boot (they would cost ~33 mb of ram up front). (satoru)
inline bool VideosLinked() { return KAsset::Linked(_binary_denji_kvid_start, _binary_denji_kvid_end) ||
                                    KAsset::Linked(_binary_ssstik_kvid_start, _binary_ssstik_kvid_end); }
inline bool HasDenjiMP4() {
    return KAsset::Has("DENJI   MP4", _binary_denji_mp4_start, _binary_denji_mp4_end);   // embedded or on-demand from the KURONO partition (satoru)
}

inline const uint8_t* DenjiMP4Data() {
    return KAsset::Data("DENJI   MP4", _binary_denji_mp4_start, _binary_denji_mp4_end);   // embedded or on-demand from the KURONO partition (satoru)
}

inline uint32_t DenjiMP4Size() {
    return KAsset::Size("DENJI   MP4", _binary_denji_mp4_start, _binary_denji_mp4_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// the playable native version (transcoded host-side via
// tools/transcode_to_kvid.ps1).  this is what the video player
// actually decodes; the .mp4 above is preserved for the demuxer
// to inspect its real metadata.
inline bool HasDenjiKVID() {
    return KAsset::Has("DENJI   KVD", _binary_denji_kvid_start, _binary_denji_kvid_end);   // embedded or on-demand from the KURONO partition (satoru)
}

inline const uint8_t* DenjiKVIDData() {
    return KAsset::Data("DENJI   KVD", _binary_denji_kvid_start, _binary_denji_kvid_end);   // embedded or on-demand from the KURONO partition (satoru)
}

inline uint32_t DenjiKVIDSize() {
    return KAsset::Size("DENJI   KVD", _binary_denji_kvid_start, _binary_denji_kvid_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// user-imported ssstik.mp4 (original) - preserved for inspection/transcode (satoru)
inline bool HasSsstikMP4() {
    return KAsset::Has("SSSTIK  MP4", _binary_ssstik_mp4_start, _binary_ssstik_mp4_end);   // embedded or on-demand from the KURONO partition (satoru)
}
inline const uint8_t* SsstikMP4Data() { return KAsset::Data("SSSTIK  MP4", _binary_ssstik_mp4_start, _binary_ssstik_mp4_end); }   // on-demand asset (satoru)
inline uint32_t SsstikMP4Size() {
    return KAsset::Size("SSSTIK  MP4", _binary_ssstik_mp4_start, _binary_ssstik_mp4_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// user-imported ssstik.kvid (playable native form) (satoru)
inline bool HasSsstikKVID() {
    return KAsset::Has("SSSTIK  KVD", _binary_ssstik_kvid_start, _binary_ssstik_kvid_end);   // embedded or on-demand from the KURONO partition (satoru)
}
inline const uint8_t* SsstikKVIDData() { return KAsset::Data("SSSTIK  KVD", _binary_ssstik_kvid_start, _binary_ssstik_kvid_end); }   // on-demand asset (satoru)
inline uint32_t SsstikKVIDSize() {
    return KAsset::Size("SSSTIK  KVD", _binary_ssstik_kvid_start, _binary_ssstik_kvid_end);   // embedded or on-demand from the KURONO partition (satoru)
}

} // namespace embeddedmedia
