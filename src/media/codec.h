#pragma once
//  kurono os - unified codec registry
//  central dispatch for audio/video codecs: wav, mp3, aac, flac, ogg,
//  mp4 container, h.264 nal parser, avi container
#include "../kernel/types.h"

enum CodecType {
    CODEC_UNKNOWN = 0,
    // audio codecs
    CODEC_WAV,        // raw pcm in riff/wave container
    CODEC_MP3,        // mpeg-1 layer iii
    CODEC_AAC,        // advanced audio coding (aac-lc)
    CODEC_OGG,        // ogg vorbis
    CODEC_FLAC,       // free lossless audio codec
    CODEC_PCM,        // raw pcm (headerless)
    CODEC_OPUS,       // opus audio
    CODEC_WMA,        // windows media audio
    // video codecs
    CODEC_H264,       // h.264 / avc
    CODEC_H265,       // h.265 / hevc
    CODEC_VP9,        // vp9
    CODEC_AV1,        // av1
    // containers
    CODEC_MP4,        // iso bmff / mp4 container
    CODEC_AVI,        // avi container
    CODEC_MKV,        // matroska container
    CODEC_WEBM,       // webm container
};

enum CodecCaps : uint32_t {
    CAP_DECODE_AUDIO  = 0x01,
    CAP_DECODE_VIDEO  = 0x02,
    CAP_ENCODE_AUDIO  = 0x04,
    CAP_ENCODE_VIDEO  = 0x08,
    CAP_CONTAINER     = 0x10,
    CAP_HARDWARE_ACCEL= 0x20,
};

struct CodecInfo {
    CodecType   type;
    const char* name;
    const char* mime_type;
    const char* file_extensions;  // comma-separated: "mp3,mp2"
    uint32_t    caps;
    bool        available;        // whether decoder is loaded
};

struct AudioBuffer {
    int16_t* samples;      // interleaved pcm
    int      length;       // total bytes
    int      sample_rate;
    int      channels;
    int      bits;         // per sample (always 16 after decode)
    bool     valid;
};

struct MP4Box {
    uint32_t size;
    char     type[5];      // 4-char code + null
    uint64_t offset;       // file offset
    bool     is_container; // has child boxes
};

struct MP4TrackInfo {
    bool     has_audio;
    bool     has_video;
    int      audio_sample_rate;
    int      audio_channels;
    int      audio_codec;       // 0=unknown, 1=aac, 2=mp3
    int      video_width;
    int      video_height;
    int      video_codec;       // 0=unknown, 1=h.264, 2=h.265
    int      duration_ms;
    int      audio_data_offset; // offset to first audio sample
    int      audio_data_size;
    int      video_data_offset;
    int      video_data_size;
};

struct H264NALUnit {
    int      type;           // nal unit type (1-12)
    int      ref_idc;        // reference idc
    const uint8_t* data;
    int      length;
};

struct H264SPS {
    int profile_idc;
    int level_idc;
    int width;
    int height;
    int max_ref_frames;
    int chroma_format;     // 1=4:2:0, 2=4:2:2, 3=4:4:4
    bool valid;
};

struct AACFrameHeader {
    int  profile;         // 0=main, 1=lc, 2=ssr, 3=ltp
    int  sample_rate;
    int  channels;
    int  frame_length;    // including header
    bool crc_absent;
    bool valid;
};

struct AACDecoderState {
    int  profile;
    int  sample_rate;
    int  channels;
    float prev_samples[2][1024];  // previous frame overlap
    bool initialized;
    int  frames_decoded;
};

struct FLACStreamInfo {
    int min_block_size;
    int max_block_size;
    int sample_rate;
    int channels;
    int bits_per_sample;
    uint64_t total_samples;
    bool valid;
};

//  codec registry - central api
class CodecRegistry {
public:
    // initialize all codec decoders
    static void Init();

    // detect codec from file data (magic bytes / header)
    static CodecType Detect(const uint8_t* data, int length);

    // detect from file extension
    static CodecType DetectByExtension(const char* filename);

    // get codec info
    static const CodecInfo* GetCodecInfo(CodecType type);
    static int GetRegisteredCount();
    static const CodecInfo* GetCodecByIndex(int idx);

    // decode any supported audio format to pcm
    static AudioBuffer DecodeAudio(const uint8_t* data, int length);

    // decode wav
    static AudioBuffer DecodeWAV(const uint8_t* data, int length);

    // decode mp3
    static AudioBuffer DecodeMP3(const uint8_t* data, int length);

    // decode aac (adts-wrapped)
    static AudioBuffer DecodeAAC(const uint8_t* data, int length);

    // decode flac
    static AudioBuffer DecodeFLAC(const uint8_t* data, int length);

    // free decoded audio buffer
    static void FreeAudioBuffer(AudioBuffer& buf);

    static bool IsMP4(const uint8_t* data, int length);
    static bool ParseMP4(const uint8_t* data, int length, MP4TrackInfo* info);
    static bool ReadMP4Box(const uint8_t* data, int offset, int length, MP4Box* box);

    // extract audio track from mp4 container
    static AudioBuffer ExtractMP4Audio(const uint8_t* data, int length);

    static bool IsH264(const uint8_t* data, int length);
    static int  FindNALUnit(const uint8_t* data, int length, int start_offset, H264NALUnit* nal);
    static bool ParseSPS(const uint8_t* data, int length, H264SPS* sps);

    static bool IsAAC(const uint8_t* data, int length);
    static bool ParseAACHeader(const uint8_t* data, int length, AACFrameHeader* hdr);

    static bool IsFLAC(const uint8_t* data, int length);
    static bool ParseFLACStreamInfo(const uint8_t* data, int length, FLACStreamInfo* info);

private:
    static CodecInfo codecs[32];
    static int       codec_count;
    static AACDecoderState aac_state;

    // aac internal
    static void AACDecodeFrame(const uint8_t* data, int length, int ch_count,
                               float* left, float* right);
    static void AACApplyIMDCT(float* freq, float* time, int n);
    static void AACApplyWindow(float* samples, int n);
};
