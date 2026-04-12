#pragma once
//  kurono os  -  mp3 decoder (mpeg-1 layer iii)
//  real frame parser with huffman decoding, imdct, subband synthesis
//  outputs signed 16-bit pcm for audio::play()
#include "../kernel/types.h"

#define MP3_FRAME_SYNC       0xFFE0   // 11-bit sync word
#define MP3_MAX_CHANNELS     2
#define MP3_MAX_GRANULES     2
#define MP3_SUBBANDS         32
#define MP3_SBLIMIT          32
#define MP3_SAMPLES_PER_GR   576      // 18 samples × 32 subbands
#define MP3_MAX_FRAME_SIZE   1441     // max frame bytes at 320kbps/32khz
#define MP3_PCM_BUFFER_SIZE  (1152 * 2 * 2)  // 1152 samples × stereo × 16-bit

enum MpegVersion {
    MPEG_25  = 0,    // mpeg 2.5 (unofficial)
    MPEG_RES = 1,    // reserved
    MPEG_2   = 2,    // mpeg-2 (iso 13818-3)
    MPEG_1   = 3,    // mpeg-1 (iso 11172-3)
};

enum MpegLayer {
    LAYER_RES = 0,
    LAYER_3   = 1,
    LAYER_2   = 2,
    LAYER_1   = 3,
};

enum ChannelMode {
    MODE_STEREO       = 0,
    MODE_JOINT_STEREO = 1,
    MODE_DUAL_CHANNEL = 2,
    MODE_MONO         = 3,
};

struct MP3FrameHeader {
    MpegVersion version;
    MpegLayer   layer;
    bool        crc_protected;
    int         bitrate;         // kbps
    int         sample_rate;     // hz
    bool        padding;
    ChannelMode channel_mode;
    int         mode_extension;  // for joint stereo
    int         frame_size;      // bytes including header
    int         channels;        // 1 or 2
};

struct GranuleInfo {
    int   part2_3_length;        // huffman data size in bits
    int   big_values;            // pairs of huffman-decoded values
    int   global_gain;           // quantizer step size
    int   scalefac_compress;     // scale factor compression
    bool  window_switching;
    int   block_type;            // 0=normal, 1=start, 2=short, 3=end
    bool  mixed_block;
    int   table_select[3];       // huffman table for each region
    int   subblock_gain[3];      // gain for short blocks
    int   region0_count;
    int   region1_count;
    int   preflag;
    int   scalefac_scale;
    int   count1table_select;
};

struct SideInfo {
    int          main_data_begin;  // negative offset into bit reservoir
    int          scfsi[2][4];      // scale factor selection info
    GranuleInfo  gr[2][2];         // [granule][channel]
};

struct MP3GranuleData {
    float is[576];           // frequency-line samples after huffman+dequant
    float xr[576];           // after stereo processing
    float samples[576];      // after imdct
};

struct MP3DecoderState {
    // bit reservoir
    uint8_t reservoir[2048];
    int     reservoir_size;       // bytes in reservoir

    // synthesis filter state (per channel)
    float   synth_buf[2][1024];   // circular buffer for polyphase synthesis
    int     synth_offset[2];      // current offset in synthesis buffer

    // previous granule overlap-add buffer
    float   overlap[2][576];

    // current frame info
    MP3FrameHeader header;
    SideInfo       side_info;

    // scale factors
    int   scalefac_l[2][22];      // long block scale factors [ch][sfb]
    int   scalefac_s[2][13][3];   // short block scale factors [ch][sfb][window]

    bool  initialized;
    int   frames_decoded;
};

class MP3Decoder {
public:
    // initialize decoder state
    static void Init();

    // check if data starts with valid mp3 frame sync
    static bool IsMP3(const uint8_t* data, int length);

    // find the first valid mp3 frame, skipping id3 tags
    static int FindFirstFrame(const uint8_t* data, int length);

    // parse frame header at given offset. returns false if invalid.
    static bool ParseHeader(const uint8_t* data, int length, MP3FrameHeader* hdr);

    // decode entire mp3 stream to pcm.
    // returns allocated pcm buffer (caller must free) and sets out_length (bytes),
    // out_sample_rate, out_channels.
    static uint8_t* DecodeAll(const uint8_t* mp3_data, int mp3_length,
                              int* out_length, int* out_sample_rate, int* out_channels);

    // decode a single frame. returns number of pcm bytes written to pcm_out.
    // pcm_out must hold at least mp3_pcm_buffer_size bytes.
    static int DecodeFrame(const uint8_t* frame_data, int frame_length,
                           int16_t* pcm_out);

    // get info about the stream without decoding
    static bool GetStreamInfo(const uint8_t* data, int length,
                              int* out_sample_rate, int* out_channels,
                              int* out_bitrate, int* out_duration_ms);

private:
    static MP3DecoderState state;

    // bitstream reader
    static int  ReadBits(const uint8_t* data, int* bit_pos, int n_bits);
    static int  ReadBitsSigned(const uint8_t* data, int* bit_pos, int n_bits);

    // frame parsing
    static bool ParseSideInfo(const uint8_t* data, int offset);
    static void ReadScaleFactors(const uint8_t* data, int* bit_pos, int gr, int ch);

    // huffman decoding
    static void HuffmanDecode(const uint8_t* data, int* bit_pos, int gr, int ch,
                              float* is_out);

    // dequantization
    static void Dequantize(int gr, int ch, float* is_data);

    // stereo processing
    static void ProcessStereo(int gr, float* left, float* right);

    // reordering (short blocks)
    static void Reorder(int gr, int ch, float* xr);

    // anti-alias butterflies
    static void AntiAlias(int gr, int ch, float* xr);

    // imdct + overlap-add
    static void IMDCT(int gr, int ch, float* xr, float* output);

    // polyphase synthesis filterbank: 576 frequency samples → 576 pcm samples
    static void SynthesisFilter(int ch, float* samples, int16_t* pcm_out, int stride);

    // windowing
    static void ApplyWindow(int block_type, float* in, float* out, int n);

    // dct-iv for imdct (18-point and 6-point)
    static void MDCT18(float* in, float* out);
    static void MDCT6(float* in, float* out);

    // lookup tables
    static const int bitrate_table[16];
    static const int samplerate_table[4];
    static const float  cs[8];          // anti-alias coefficients
    static const float  ca[8];
    static const float  imdct_win[4][36];  // window functions per block type
    static float  synth_window[512]; // polyphase synthesis window
};
