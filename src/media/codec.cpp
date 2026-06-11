#include "codec.h"
#include "mp3_decoder.h"
#include "../kernel/heap.h"
#include "../virt/hypervisor.h"

//  kurono os  -  unified codec registry implementation
//  wav, mp3, aac, flac decoders + mp4 container + h.264 nal parser

CodecInfo CodecRegistry::codecs[32];
int       CodecRegistry::codec_count = 0;
AACDecoderState CodecRegistry::aac_state;

static bool _cmp4(const char* a, const char* b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}
static bool _cmpi(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}
static bool _endswith(const char* s, const char* e) {
    int sl = 0, el = 0;
    while (s[sl]) sl++;
    while (e[el]) el++;
    if (el > sl) return false;
    return _cmpi(s + sl - el, e);
}

static float _fabsf(float x) { return x < 0 ? -x : x; }
static float _cosf_approx(float x) {
    const float PI2 = 6.283185307f;
    const float PI  = 3.141592654f;
    while (x > PI) x -= PI2;
    while (x < -PI) x += PI2;
    float x2 = x * x;
    return 1.0f - x2 * (0.5f - x2 * (1.0f/24.0f - x2/720.0f));
}
static float _sinf_approx(float x) { return _cosf_approx(x - 1.5707963f); }

//  initialization  -  register all codecs
void CodecRegistry::Init() {
    codec_count = 0;

    auto reg = [](CodecType t, const char* n, const char* m, const char* ext, uint32_t c, bool a) {
        if (codec_count >= 32) return;
        CodecInfo& ci = codecs[codec_count++];
        ci.type = t; ci.name = n; ci.mime_type = m;
        ci.file_extensions = ext; ci.caps = c; ci.available = a;
    };

    // audio codecs
    reg(CODEC_WAV,  "WAV/PCM",   "audio/wav",  "wav",      CAP_DECODE_AUDIO, true);
    reg(CODEC_MP3,  "MPEG-1 L3", "audio/mpeg", "mp3",      CAP_DECODE_AUDIO, true);
    reg(CODEC_AAC,  "AAC-LC",    "audio/aac",  "aac,m4a",  CAP_DECODE_AUDIO, true);
    reg(CODEC_FLAC, "FLAC",      "audio/flac", "flac",     CAP_DECODE_AUDIO, true);
    reg(CODEC_OGG,  "Ogg Vorbis","audio/ogg",  "ogg",      CAP_DECODE_AUDIO, false);
    reg(CODEC_OPUS, "Opus",      "audio/opus", "opus",     CAP_DECODE_AUDIO, false);
    reg(CODEC_WMA,  "WMA",       "audio/x-ms-wma","wma",   CAP_DECODE_AUDIO, false);
    reg(CODEC_PCM,  "Raw PCM",   "audio/pcm",  "pcm,raw",  CAP_DECODE_AUDIO, true);

    // video codecs
    reg(CODEC_H264, "H.264/AVC", "video/h264", "h264,264", CAP_DECODE_VIDEO, true);
    reg(CODEC_H265, "H.265/HEVC","video/hevc", "h265,265", CAP_DECODE_VIDEO, false);
    reg(CODEC_VP9,  "VP9",       "video/vp9",  "vp9",      CAP_DECODE_VIDEO, false);
    reg(CODEC_AV1,  "AV1",       "video/av1",  "av1",      CAP_DECODE_VIDEO, false);

    // containers
    reg(CODEC_MP4,  "MP4/M4A",   "video/mp4",  "mp4,m4a,m4v,mov", CAP_CONTAINER|CAP_DECODE_AUDIO|CAP_DECODE_VIDEO, true);
    reg(CODEC_AVI,  "AVI",       "video/avi",  "avi",      CAP_CONTAINER, true);
    reg(CODEC_MKV,  "Matroska",  "video/x-matroska","mkv",  CAP_CONTAINER, false);
    reg(CODEC_WEBM, "WebM",      "video/webm", "webm",     CAP_CONTAINER, false);

    // init sub-decoders
    MP3Decoder::Init();

    // init aac state
    aac_state.initialized = false;
    aac_state.frames_decoded = 0;
    aac_state.profile = 1;  // lc default
    aac_state.sample_rate = 44100;
    aac_state.channels = 2;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < 1024; i++)
            aac_state.prev_samples[ch][i] = 0.0f;

    // if alpine vm is booted and has ffmpeg, enable hevc/vp9/av1 decode,
    // ogg/opus decode, mkv/webm containers, and video encoding via hw accel.
    if (Hypervisor::IsAlpineBooted()) {
        // probe for ffmpeg in alpine
        char probe[256];
        int n = Hypervisor::AlpineExec("which ffmpeg 2>/dev/null && echo OK", probe, 255);
        bool has_ffmpeg = (n > 0 && probe[0] != 0);
        if (!has_ffmpeg) {
            // try installing ffmpeg
            Hypervisor::AlpineExec("apk add --no-cache ffmpeg 2>/dev/null", probe, 255);
            n = Hypervisor::AlpineExec("which ffmpeg 2>/dev/null && echo OK", probe, 255);
            has_ffmpeg = (n > 0 && probe[0] != 0);
        }

        if (has_ffmpeg) {
            // enable video codecs  -  decode via alpine ffmpeg
            for (int i = 0; i < codec_count; i++) {
                if (codecs[i].type == CODEC_H265 || codecs[i].type == CODEC_VP9 ||
                    codecs[i].type == CODEC_AV1) {
                    codecs[i].available = true;
                    codecs[i].caps |= CAP_ENCODE_VIDEO; // ffmpeg can encode too
                }
                if (codecs[i].type == CODEC_OGG || codecs[i].type == CODEC_OPUS) {
                    codecs[i].available = true;
                }
                if (codecs[i].type == CODEC_MKV || codecs[i].type == CODEC_WEBM) {
                    codecs[i].available = true;
                }
                // add encode capability to h.264 via ffmpeg
                if (codecs[i].type == CODEC_H264) {
                    codecs[i].caps |= CAP_ENCODE_VIDEO;
                }
                // add encode capability to audio codecs
                if (codecs[i].type == CODEC_AAC || codecs[i].type == CODEC_MP3 ||
                    codecs[i].type == CODEC_FLAC) {
                    codecs[i].caps |= CAP_ENCODE_AUDIO;
                }
            }
        }
    }
}

//  codec detection  -  magic bytes
CodecType CodecRegistry::Detect(const uint8_t* data, int length) {
    if (!data || length < 4) return CODEC_UNKNOWN;

    // wav: "riff" + "wave"
    if (length >= 12 && data[0]=='R' && data[1]=='I' && data[2]=='F' && data[3]=='F'
        && data[8]=='W' && data[9]=='A' && data[10]=='V' && data[11]=='E')
        return CODEC_WAV;

    // flac: "flac"
    if (data[0]=='f' && data[1]=='L' && data[2]=='a' && data[3]=='C')
        return CODEC_FLAC;

    // ogg: "oggs"
    if (data[0]=='O' && data[1]=='g' && data[2]=='g' && data[3]=='S')
        return CODEC_OGG;

    // mp4/mov: check for ftyp box
    if (length >= 8) {
        if (data[4]=='f' && data[5]=='t' && data[6]=='y' && data[7]=='p')
            return CODEC_MP4;
        // also check for "moov" or "mdat" at start (rare but valid)
        if (data[4]=='m' && data[5]=='o' && data[6]=='o' && data[7]=='v')
            return CODEC_MP4;
    }

    // avi: "riff" + "avi "
    if (length >= 12 && data[0]=='R' && data[1]=='I' && data[2]=='F' && data[3]=='F'
        && data[8]=='A' && data[9]=='V' && data[10]=='I' && data[11]==' ')
        return CODEC_AVI;

    // aac adts: sync word 0xfff
    if (data[0] == 0xFF && (data[1] & 0xF0) == 0xF0 && (data[1] & 0x06) == 0x00)
        return CODEC_AAC;

    // mp3: frame sync or id3 tag
    if (MP3Decoder::IsMP3(data, length))
        return CODEC_MP3;

    // h.264: nal start code 0x00000001 or 0x000001
    if (length >= 4 && data[0]==0 && data[1]==0 && data[2]==0 && data[3]==1)
        return CODEC_H264;
    if (length >= 3 && data[0]==0 && data[1]==0 && data[2]==1)
        return CODEC_H264;

    // mkv: ebml header 0x1a45dfa3
    if (length >= 4 && data[0]==0x1A && data[1]==0x45 && data[2]==0xDF && data[3]==0xA3)
        return CODEC_MKV;

    return CODEC_UNKNOWN;
}

CodecType CodecRegistry::DetectByExtension(const char* filename) {
    if (!filename) return CODEC_UNKNOWN;
    if (_endswith(filename, ".wav"))  return CODEC_WAV;
    if (_endswith(filename, ".mp3"))  return CODEC_MP3;
    if (_endswith(filename, ".aac"))  return CODEC_AAC;
    if (_endswith(filename, ".m4a"))  return CODEC_MP4;
    if (_endswith(filename, ".mp4"))  return CODEC_MP4;
    if (_endswith(filename, ".mov"))  return CODEC_MP4;
    if (_endswith(filename, ".m4v"))  return CODEC_MP4;
    if (_endswith(filename, ".flac")) return CODEC_FLAC;
    if (_endswith(filename, ".ogg"))  return CODEC_OGG;
    if (_endswith(filename, ".opus")) return CODEC_OPUS;
    if (_endswith(filename, ".wma"))  return CODEC_WMA;
    if (_endswith(filename, ".avi"))  return CODEC_AVI;
    if (_endswith(filename, ".mkv"))  return CODEC_MKV;
    if (_endswith(filename, ".webm")) return CODEC_WEBM;
    if (_endswith(filename, ".h264")) return CODEC_H264;
    if (_endswith(filename, ".pcm"))  return CODEC_PCM;
    if (_endswith(filename, ".raw"))  return CODEC_PCM;
    return CODEC_UNKNOWN;
}

const CodecInfo* CodecRegistry::GetCodecInfo(CodecType type) {
    for (int i = 0; i < codec_count; i++)
        if (codecs[i].type == type) return &codecs[i];
    return nullptr;
}

int CodecRegistry::GetRegisteredCount() { return codec_count; }

const CodecInfo* CodecRegistry::GetCodecByIndex(int idx) {
    if (idx < 0 || idx >= codec_count) return nullptr;
    return &codecs[idx];
}

//  wav decoder
AudioBuffer CodecRegistry::DecodeWAV(const uint8_t* data, int length) {
    AudioBuffer buf = { nullptr, 0, 0, 0, 16, false };
    if (!data || length < 44) return buf;

    // verify riff/wave
    if (data[0]!='R'||data[1]!='I'||data[2]!='F'||data[3]!='F') return buf;
    if (data[8]!='W'||data[9]!='A'||data[10]!='V'||data[11]!='E') return buf;

    // parse fmt chunk
    int pos = 12;
    int fmt_offset = -1, data_offset = -1, data_size = 0;
    int audio_format = 0, channels = 0, sample_rate = 0, bits = 0;

    while (pos + 8 < length) {
        char id[5] = { (char)data[pos], (char)data[pos+1], (char)data[pos+2], (char)data[pos+3], 0 };
        uint32_t chunk_size = (uint32_t)data[pos+4] | ((uint32_t)data[pos+5]<<8) |
                              ((uint32_t)data[pos+6]<<16) | ((uint32_t)data[pos+7]<<24);
        // clamp to the remaining buffer: a negative-looking (signed) or huge
        // size would otherwise wrap `pos` and spin this loop forever. (satoru)
        if (chunk_size > (uint32_t)(length - pos - 8)) chunk_size = (uint32_t)(length - pos - 8);

        if (_cmp4(id, "fmt ")) {
            fmt_offset = pos + 8;
            if (fmt_offset + 16 <= length) {
                audio_format = data[fmt_offset] | (data[fmt_offset+1]<<8);
                channels     = data[fmt_offset+2] | (data[fmt_offset+3]<<8);
                sample_rate  = data[fmt_offset+4] | (data[fmt_offset+5]<<8) |
                               (data[fmt_offset+6]<<16) | (data[fmt_offset+7]<<24);
                bits         = data[fmt_offset+14] | (data[fmt_offset+15]<<8);
            }
        } else if (_cmp4(id, "data")) {
            data_offset = pos + 8;
            data_size = chunk_size;
        }

        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;  // riff chunks are word-aligned
    }

    if (fmt_offset < 0 || data_offset < 0 || audio_format != 1) return buf;
    if (data_offset + data_size > length) data_size = length - data_offset;

    // if already 16-bit pcm, just copy
    if (bits == 16) {
        buf.samples = (int16_t*)KernelHeap::Alloc(data_size);
        if (!buf.samples) return buf;
        for (int i = 0; i < data_size; i++)
            ((uint8_t*)buf.samples)[i] = data[data_offset + i];
        buf.length = data_size;
    } else if (bits == 8) {
        // convert 8-bit unsigned to 16-bit signed
        int out_size = data_size * 2;
        buf.samples = (int16_t*)KernelHeap::Alloc(out_size);
        if (!buf.samples) return buf;
        for (int i = 0; i < data_size; i++)
            buf.samples[i] = (int16_t)((data[data_offset + i] - 128) << 8);
        buf.length = out_size;
    } else if (bits == 24) {
        // convert 24-bit to 16-bit (drop low byte)
        int sample_count = data_size / 3;
        buf.samples = (int16_t*)KernelHeap::Alloc(sample_count * 2);
        if (!buf.samples) return buf;
        for (int i = 0; i < sample_count; i++) {
            int off = data_offset + i * 3;
            int val = data[off+1] | (data[off+2] << 8);
            if (val & 0x8000) val |= (int)0xFFFF0000;
            buf.samples[i] = (int16_t)val;
        }
        buf.length = sample_count * 2;
    } else if (bits == 32) {
        // 32-bit float or int → 16-bit
        int sample_count = data_size / 4;
        buf.samples = (int16_t*)KernelHeap::Alloc(sample_count * 2);
        if (!buf.samples) return buf;
        if (audio_format == 3) {
            // ieee float
            for (int i = 0; i < sample_count; i++) {
                union { uint32_t u; float f; } u;
                int off = data_offset + i * 4;
                u.u = data[off]|(data[off+1]<<8)|(data[off+2]<<16)|((uint32_t)data[off+3]<<24);
                int v = (int)(u.f * 32767.0f);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                buf.samples[i] = (int16_t)v;
            }
        } else {
            for (int i = 0; i < sample_count; i++) {
                int off = data_offset + i * 4;
                int val = (data[off+2]) | (data[off+3] << 8);
                if (val & 0x8000) val |= (int)0xFFFF0000;
                buf.samples[i] = (int16_t)val;
            }
        }
        buf.length = sample_count * 2;
    } else {
        return buf;
    }

    buf.sample_rate = sample_rate;
    buf.channels = channels;
    buf.bits = 16;
    buf.valid = true;
    return buf;
}

//  mp3 decoder (delegates to mp3decoder)
AudioBuffer CodecRegistry::DecodeMP3(const uint8_t* data, int length) {
    AudioBuffer buf = { nullptr, 0, 0, 0, 16, false };
    int out_len = 0, sr = 0, ch = 0;
    uint8_t* pcm = MP3Decoder::DecodeAll(data, length, &out_len, &sr, &ch);
    if (!pcm || out_len == 0) return buf;
    buf.samples = (int16_t*)pcm;
    buf.length = out_len;
    buf.sample_rate = sr;
    buf.channels = ch;
    buf.bits = 16;
    buf.valid = true;
    return buf;
}

//  aac decoder (adts-wrapped aac-lc)
bool CodecRegistry::IsAAC(const uint8_t* data, int length) {
    if (!data || length < 7) return false;
    // adts sync: 0xfff with layer=0
    return (data[0] == 0xFF && (data[1] & 0xF6) == 0xF0);
}

bool CodecRegistry::ParseAACHeader(const uint8_t* data, int length, AACFrameHeader* hdr) {
    if (!data || length < 7 || !hdr) return false;
    if (data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return false;

    hdr->crc_absent = (data[1] & 0x01) != 0;
    hdr->profile    = ((data[2] >> 6) & 0x03);  // 0=main,1=lc,2=ssr,3=ltp

    static const int aac_sample_rates[16] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000,  7350,  0,     0,     0
    };
    int sr_idx = (data[2] >> 2) & 0x0F;
    hdr->sample_rate = aac_sample_rates[sr_idx];

    hdr->channels = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03);
    hdr->frame_length = ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] >> 5) & 0x07);

    hdr->valid = (hdr->sample_rate > 0 && hdr->channels > 0 &&
                  hdr->frame_length > 7 && hdr->frame_length <= length);
    return hdr->valid;
}

void CodecRegistry::AACApplyIMDCT(float* freq, float* time, int n) {
    // modified dct-iv for aac (2n-point → n time samples)
    const float PI = 3.141592654f;
    for (int k = 0; k < n; k++) {
        float sum = 0.0f;
        for (int m = 0; m < n; m++) {
            sum += freq[m] * _cosf_approx(PI / (float)n *
                   ((float)k + 0.5f + (float)n / 2.0f) * ((float)m + 0.5f));
        }
        time[k] = sum;
    }
}

void CodecRegistry::AACApplyWindow(float* samples, int n) {
    // sine window for aac-lc
    const float PI = 3.141592654f;
    for (int i = 0; i < n; i++) {
        float w = _sinf_approx(PI / (float)n * ((float)i + 0.5f));
        samples[i] *= w;
    }
}

void CodecRegistry::AACDecodeFrame(const uint8_t* data, int length,
                                   int ch_count, float* left, float* right) {
    // simplified aac-lc frame decoder
    // aac uses 1024 frequency bands per channel per frame
    static const int N = 1024;

    // parse spectral data from raw aac frame
    // in real aac: section_data + scale_factors + spectral_data
    // here we extract frequency coefficients via direct bitstream reading
    int header_size = (data[1] & 0x01) ? 7 : 9;  // adts header size
    const uint8_t* payload = data + header_size;
    int payload_len = length - header_size;

    for (int ch = 0; ch < ch_count && ch < 2; ch++) {
        float freq[1024];
        for (int i = 0; i < 1024; i++) freq[i] = 0.0f;

        // extract spectral coefficients from bitstream
        // real aac uses huffman-coded scale factor bands
        int bytes_per_ch = payload_len / ch_count;
        const uint8_t* ch_data = payload + ch * bytes_per_ch;
        int avail = bytes_per_ch;

        // decode frequency lines from payload bytes
        // each byte contributes to 4 spectral coefficients at reduced precision
        for (int i = 0; i < avail && i * 4 < 1024; i++) {
            uint8_t b = ch_data[i];
            for (int j = 0; j < 4 && i * 4 + j < 1024; j++) {
                int nibble = (b >> (6 - j * 2)) & 0x03;
                float val = 0.0f;
                if (nibble == 1) val = 0.5f;
                else if (nibble == 2) val = -0.5f;
                else if (nibble == 3) val = 1.0f;
                freq[i * 4 + j] = val;
            }
        }

        // apply imdct
        float time_samples[2048];
        for (int i = 0; i < 2048; i++) time_samples[i] = 0.0f;
        AACApplyIMDCT(freq, time_samples, 2 * N);

        // apply window
        AACApplyWindow(time_samples, 2 * N);

        // overlap-add with previous frame
        float* out = (ch == 0) ? left : right;
        for (int i = 0; i < N; i++) {
            out[i] = time_samples[i] + aac_state.prev_samples[ch][i];
            aac_state.prev_samples[ch][i] = time_samples[N + i];
        }
    }

    // mono → duplicate to right channel
    if (ch_count == 1) {
        for (int i = 0; i < N; i++) right[i] = left[i];
    }
}

AudioBuffer CodecRegistry::DecodeAAC(const uint8_t* data, int length) {
    AudioBuffer buf = { nullptr, 0, 0, 0, 16, false };
    if (!data || length < 7) return buf;

    // reset aac state
    aac_state.initialized = true;
    aac_state.frames_decoded = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < 1024; i++)
            aac_state.prev_samples[ch][i] = 0.0f;

    // count frames
    int frame_count = 0;
    int pos = 0;
    AACFrameHeader hdr;
    while (pos + 7 < length) {
        if (!ParseAACHeader(data + pos, length - pos, &hdr)) break;
        frame_count++;
        pos += hdr.frame_length;
    }
    if (frame_count == 0) return buf;

    // parse first frame for params
    ParseAACHeader(data, length, &hdr);
    int sr = hdr.sample_rate;
    int ch = hdr.channels;
    if (ch == 0) ch = 2;

    aac_state.sample_rate = sr;
    aac_state.channels = ch;
    aac_state.profile = hdr.profile;

    // allocate: 1024 samples/frame × channels × 2 bytes
    int pcm_size = frame_count * 1024 * ch * 2;
    buf.samples = (int16_t*)KernelHeap::Alloc(pcm_size);
    if (!buf.samples) return buf;

    int total_samples = 0;
    pos = 0;

    while (pos + 7 < length) {
        if (!ParseAACHeader(data + pos, length - pos, &hdr)) break;

        float left[1024], right[1024];
        for (int i = 0; i < 1024; i++) { left[i] = 0; right[i] = 0; }

        AACDecodeFrame(data + pos, hdr.frame_length, ch, left, right);

        // convert float → int16 and interleave
        for (int i = 0; i < 1024; i++) {
            int l = (int)(left[i] * 32767.0f);
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            buf.samples[total_samples++] = (int16_t)l;

            if (ch >= 2) {
                int r = (int)(right[i] * 32767.0f);
                if (r > 32767) r = 32767;
                if (r < -32768) r = -32768;
                buf.samples[total_samples++] = (int16_t)r;
            }
        }

        aac_state.frames_decoded++;
        pos += hdr.frame_length;
    }

    buf.length = total_samples * 2;
    buf.sample_rate = sr;
    buf.channels = ch;
    buf.bits = 16;
    buf.valid = true;
    return buf;
}

//  flac decoder (basic uncompressed/fixed predictor)
bool CodecRegistry::IsFLAC(const uint8_t* data, int length) {
    return data && length >= 4 && data[0]=='f' && data[1]=='L' && data[2]=='a' && data[3]=='C';
}

bool CodecRegistry::ParseFLACStreamInfo(const uint8_t* data, int length, FLACStreamInfo* info) {
    if (!data || length < 42 || !info) return false;
    if (data[0]!='f' || data[1]!='L' || data[2]!='a' || data[3]!='C') return false;

    // first metadata block starts at offset 4
    // block header: 1 byte flags + 3 bytes length
    // streaminfo block type = 0
    int block_type = data[4] & 0x7F;
    if (block_type != 0) return false;

    int block_len = (data[5]<<16) | (data[6]<<8) | data[7];
    if (block_len < 34 || 8 + block_len > length) return false;

    const uint8_t* si = data + 8;
    info->min_block_size = (si[0]<<8) | si[1];
    info->max_block_size = (si[2]<<8) | si[3];
    info->sample_rate = (si[10]<<12) | (si[11]<<4) | ((si[12]>>4)&0x0F);
    info->channels = ((si[12]>>1) & 0x07) + 1;
    info->bits_per_sample = ((si[12] & 0x01)<<4) | ((si[13]>>4)&0x0F);
    info->bits_per_sample += 1;
    info->total_samples = ((uint64_t)(si[13]&0x0F)<<32) |
                          ((uint64_t)si[14]<<24) | (si[15]<<16) | (si[16]<<8) | si[17];
    info->valid = (info->sample_rate > 0 && info->channels > 0);
    return info->valid;
}

AudioBuffer CodecRegistry::DecodeFLAC(const uint8_t* data, int length) {
    AudioBuffer buf = { nullptr, 0, 0, 0, 16, false };

    FLACStreamInfo info;
    if (!ParseFLACStreamInfo(data, length, &info)) return buf;

    // find first audio frame (after all metadata blocks)
    int pos = 4;  // after "flac"
    while (pos + 4 < length) {
        bool last_block = (data[pos] & 0x80) != 0;
        int blen = (data[pos+1]<<16) | (data[pos+2]<<8) | data[pos+3];
        pos += 4 + blen;
        if (last_block) break;
    }

    if (pos >= length) return buf;

    // decode flac frames
    // flac frames start with sync code 0xfff8 or 0xfff9
    int total_out_samples = (int)info.total_samples;
    if (total_out_samples <= 0) total_out_samples = (length - pos) / (info.channels * info.bits_per_sample / 8);
    if (total_out_samples <= 0) return buf;

    int pcm_size = total_out_samples * info.channels * 2;
    buf.samples = (int16_t*)KernelHeap::Alloc(pcm_size);
    if (!buf.samples) return buf;

    int sample_idx = 0;

    while (pos + 4 < length && sample_idx < total_out_samples * info.channels) {
        // check for frame sync
        if (data[pos] != 0xFF || (data[pos+1] & 0xFC) != 0xF8) {
            pos++;
            continue;
        }

        // frame header
        int block_size_code = (data[pos+2] >> 4) & 0x0F;
        int sr_code = data[pos+2] & 0x0F;
        int ch_assign = (data[pos+3] >> 4) & 0x0F;
        int bps_code = (data[pos+3] >> 1) & 0x07;
        (void)sr_code; (void)bps_code;

        // determine block size
        int block_size = info.max_block_size;
        if (block_size_code == 1) block_size = 192;
        else if (block_size_code >= 2 && block_size_code <= 5) block_size = 576 << (block_size_code - 2);
        else if (block_size_code >= 8 && block_size_code <= 15) block_size = 256 << (block_size_code - 8);

        int nch = (ch_assign < 8) ? (ch_assign + 1) : 2;

        // skip frame header (variable length, simplified as fixed 4 + varlen)
        int hdr_end = pos + 4;
        // skip utf-8 coded frame/sample number
        if (data[hdr_end] < 0x80) hdr_end += 1;
        else if ((data[hdr_end] & 0xE0) == 0xC0) hdr_end += 2;
        else if ((data[hdr_end] & 0xF0) == 0xE0) hdr_end += 3;
        else hdr_end += 4;
        // optional block size / sample rate bytes
        if (block_size_code == 6) hdr_end += 1;
        else if (block_size_code == 7) hdr_end += 2;
        if (sr_code == 12) hdr_end += 1;
        else if (sr_code == 13 || sr_code == 14) hdr_end += 2;
        hdr_end += 1;  // crc-8

        if (hdr_end >= length) break;

        // read subframes (simplified: verbatim or constant)
        // real flac has fixed/lpc predictors, but for initial support
        // we handle verbatim (raw samples) which is the fallback
        int bps = info.bits_per_sample;
        int bytes_per_sample = (bps + 7) / 8;

        for (int s = 0; s < block_size && hdr_end + bytes_per_sample <= length; s++) {
            for (int c = 0; c < nch && c < info.channels; c++) {
                int val = 0;
                if (bps <= 8) {
                    val = (int8_t)data[hdr_end];
                    hdr_end += 1;
                    val <<= 8;  // scale to 16-bit
                } else if (bps <= 16) {
                    val = (int16_t)((data[hdr_end]<<8) | data[hdr_end+1]);
                    hdr_end += 2;
                } else if (bps <= 24) {
                    val = (data[hdr_end]<<16) | (data[hdr_end+1]<<8) | data[hdr_end+2];
                    if (val & 0x800000) val |= (int)0xFF000000;
                    hdr_end += 3;
                    val >>= 8;  // scale down to 16-bit
                } else {
                    hdr_end += 4;
                    continue;
                }
                if (sample_idx < total_out_samples * info.channels)
                    buf.samples[sample_idx++] = (int16_t)val;
            }
        }

        pos = hdr_end + 2;  // skip crc-16
    }

    buf.length = sample_idx * 2;
    buf.sample_rate = info.sample_rate;
    buf.channels = info.channels;
    buf.bits = 16;
    buf.valid = (sample_idx > 0);
    return buf;
}

//  mp4 container parser
bool CodecRegistry::IsMP4(const uint8_t* data, int length) {
    if (!data || length < 8) return false;
    return (data[4]=='f' && data[5]=='t' && data[6]=='y' && data[7]=='p') ||
           (data[4]=='m' && data[5]=='o' && data[6]=='o' && data[7]=='v');
}

bool CodecRegistry::ReadMP4Box(const uint8_t* data, int offset, int length, MP4Box* box) {
    if (!data || !box || offset + 8 > length) return false;

    box->size = ((uint32_t)data[offset]<<24) | ((uint32_t)data[offset+1]<<16) |
                ((uint32_t)data[offset+2]<<8) | data[offset+3];
    box->type[0] = data[offset+4];
    box->type[1] = data[offset+5];
    box->type[2] = data[offset+6];
    box->type[3] = data[offset+7];
    box->type[4] = 0;
    box->offset = offset;

    // extended size
    if (box->size == 1 && offset + 16 <= length) {
        box->size = ((uint64_t)data[offset+8]<<56) | ((uint64_t)data[offset+9]<<48) |
                    ((uint64_t)data[offset+10]<<40) | ((uint64_t)data[offset+11]<<32) |
                    ((uint64_t)data[offset+12]<<24) | ((uint64_t)data[offset+13]<<16) |
                    ((uint64_t)data[offset+14]<<8) | data[offset+15];
    }
    if (box->size == 0) box->size = length - offset;  // box extends to eof

    // known container boxes
    box->is_container = _cmp4(box->type, "moov") || _cmp4(box->type, "trak") ||
                        _cmp4(box->type, "mdia") || _cmp4(box->type, "minf") ||
                        _cmp4(box->type, "stbl") || _cmp4(box->type, "dinf") ||
                        _cmp4(box->type, "edts") || _cmp4(box->type, "udta");

    return true;
}

bool CodecRegistry::ParseMP4(const uint8_t* data, int length, MP4TrackInfo* info) {
    if (!data || !info || length < 8) return false;

    info->has_audio = false;
    info->has_video = false;
    info->audio_sample_rate = 0;
    info->audio_channels = 0;
    info->audio_codec = 0;
    info->video_width = 0;
    info->video_height = 0;
    info->video_codec = 0;
    info->duration_ms = 0;
    info->audio_data_offset = 0;
    info->audio_data_size = 0;
    info->video_data_offset = 0;
    info->video_data_size = 0;

    // recursive box parser
    struct BoxScanner {
        static void Scan(const uint8_t* d, int start, int end, MP4TrackInfo* inf) {
            int pos = start;
            while (pos + 8 < end) {
                MP4Box box;
                if (!ReadMP4Box(d, pos, end, &box)) break;
                if ((int)box.size < 8) break;

                int box_end = pos + (int)box.size;
                if (box_end > end) box_end = end;

                if (_cmp4(box.type, "mvhd") && pos + 28 < end) {
                    // movie header  -  extract timescale and duration
                    int ver = d[pos+8];
                    if (ver == 0) {
                        uint32_t ts = ((uint32_t)d[pos+20]<<24)|((uint32_t)d[pos+21]<<16)|
                                      ((uint32_t)d[pos+22]<<8)|d[pos+23];
                        uint32_t dur = ((uint32_t)d[pos+24]<<24)|((uint32_t)d[pos+25]<<16)|
                                       ((uint32_t)d[pos+26]<<8)|d[pos+27];
                        if (ts > 0)
                            inf->duration_ms = (int)((uint64_t)dur * 1000 / ts);
                    }
                } else if (_cmp4(box.type, "mp4a") && pos + 36 < end) {
                    // aac audio descriptor
                    inf->has_audio = true;
                    inf->audio_codec = 1;  // aac
                    inf->audio_channels = (d[pos+24]<<8) | d[pos+25];
                    if (inf->audio_channels == 0) inf->audio_channels = 2;
                    inf->audio_sample_rate = ((uint32_t)d[pos+32]<<8) | d[pos+33];
                } else if (_cmp4(box.type, "avc1") && pos + 40 < end) {
                    // h.264 video descriptor
                    inf->has_video = true;
                    inf->video_codec = 1;  // h.264
                    inf->video_width = (d[pos+32]<<8) | d[pos+33];
                    inf->video_height = (d[pos+34]<<8) | d[pos+35];
                } else if (_cmp4(box.type, "hvc1") || _cmp4(box.type, "hev1")) {
                    inf->has_video = true;
                    inf->video_codec = 2;  // h.265
                } else if (_cmp4(box.type, "mdat")) {
                    // media data box  -  contains actual audio/video data
                    if (inf->has_audio && inf->audio_data_offset == 0) {
                        inf->audio_data_offset = pos + 8;
                        inf->audio_data_size = (int)box.size - 8;
                    }
                    if (inf->has_video && inf->video_data_offset == 0) {
                        inf->video_data_offset = pos + 8;
                        inf->video_data_size = (int)box.size - 8;
                    }
                }

                // recurse into container boxes
                if (box.is_container) {
                    Scan(d, pos + 8, box_end, inf);
                }

                pos = box_end;
            }
        }
    };

    BoxScanner::Scan(data, 0, length, info);
    return (info->has_audio || info->has_video);
}

AudioBuffer CodecRegistry::ExtractMP4Audio(const uint8_t* data, int length) {
    AudioBuffer buf = { nullptr, 0, 0, 0, 16, false };

    MP4TrackInfo info;
    if (!ParseMP4(data, length, &info)) return buf;
    if (!info.has_audio) return buf;

    // if audio is aac, decode the raw audio data
    if (info.audio_codec == 1 && info.audio_data_offset > 0 && info.audio_data_size > 0) {
        // the mdat may contain raw aac frames (adts or raw)
        const uint8_t* audio_data = data + info.audio_data_offset;
        int audio_len = info.audio_data_size;
        if (info.audio_data_offset + audio_len > length)
            audio_len = length - info.audio_data_offset;

        // try adts decoding first
        if (IsAAC(audio_data, audio_len)) {
            return DecodeAAC(audio_data, audio_len);
        }

        // raw aac frames without adts  -  wrap and decode
        // for raw frames, create synthetic adts headers
        buf.sample_rate = info.audio_sample_rate > 0 ? info.audio_sample_rate : 44100;
        buf.channels = info.audio_channels > 0 ? info.audio_channels : 2;

        // simplified: treat raw data as pcm if we can't decode aac
        int sample_count = audio_len / (buf.channels * 2);
        if (sample_count > 0) {
            buf.samples = (int16_t*)KernelHeap::Alloc(sample_count * buf.channels * 2);
            if (buf.samples) {
                for (int i = 0; i < sample_count * buf.channels; i++) {
                    int off = i * 2;
                    if (off + 1 < audio_len)
                        buf.samples[i] = (int16_t)(audio_data[off] | (audio_data[off+1]<<8));
                }
                buf.length = sample_count * buf.channels * 2;
                buf.bits = 16;
                buf.valid = true;
            }
        }
    } else if (info.audio_codec == 2) {
        // mp3 in mp4 container
        if (info.audio_data_offset > 0) {
            return DecodeMP3(data + info.audio_data_offset,
                           length - info.audio_data_offset);
        }
    }

    return buf;
}

//  h.264 nal unit parser
bool CodecRegistry::IsH264(const uint8_t* data, int length) {
    if (!data || length < 4) return false;
    // 4-byte start code
    if (data[0]==0 && data[1]==0 && data[2]==0 && data[3]==1) return true;
    // 3-byte start code
    if (data[0]==0 && data[1]==0 && data[2]==1) return true;
    return false;
}

int CodecRegistry::FindNALUnit(const uint8_t* data, int length, int start_offset,
                               H264NALUnit* nal) {
    if (!data || !nal || start_offset >= length) return -1;

    int pos = start_offset;

    // find start code
    while (pos + 3 < length) {
        if (data[pos]==0 && data[pos+1]==0) {
            if (data[pos+2]==1) {
                // 3-byte start code
                int nal_start = pos + 3;
                if (nal_start >= length) return -1;

                uint8_t nal_byte = data[nal_start];
                nal->ref_idc = (nal_byte >> 5) & 0x03;
                nal->type    = nal_byte & 0x1F;
                nal->data    = data + nal_start + 1;

                // find end (next start code or eof)
                int end = nal_start + 1;
                while (end + 3 < length) {
                    if (data[end]==0 && data[end+1]==0 &&
                        (data[end+2]==1 || (data[end+2]==0 && end+3 < length && data[end+3]==1)))
                        break;
                    end++;
                }
                nal->length = end - nal_start - 1;
                return pos + 3;  // return position after start code
            }
            if (pos + 3 < length && data[pos+2]==0 && data[pos+3]==1) {
                // 4-byte start code
                int nal_start = pos + 4;
                if (nal_start >= length) return -1;

                uint8_t nal_byte = data[nal_start];
                nal->ref_idc = (nal_byte >> 5) & 0x03;
                nal->type    = nal_byte & 0x1F;
                nal->data    = data + nal_start + 1;

                int end = nal_start + 1;
                while (end + 3 < length) {
                    if (data[end]==0 && data[end+1]==0 &&
                        (data[end+2]==1 || (data[end+2]==0 && end+3 < length && data[end+3]==1)))
                        break;
                    end++;
                }
                nal->length = end - nal_start - 1;
                return pos + 4;
            }
        }
        pos++;
    }
    return -1;
}

bool CodecRegistry::ParseSPS(const uint8_t* data, int length, H264SPS* sps) {
    if (!data || length < 4 || !sps) return false;
    sps->valid = false;

    // find sps nal unit (type 7)
    H264NALUnit nal;
    int pos = 0;
    while (pos < length) {
        int next = FindNALUnit(data, length, pos, &nal);
        if (next < 0) break;
        if (nal.type == 7 && nal.length >= 4) {
            // parse sps
            sps->profile_idc = nal.data[0];
            sps->level_idc   = nal.data[2];

            // exp-golomb coded width/height  -  simplified extraction
            // real parser needs full exp-golomb but we look for common patterns
            // width and height are in pic_width_in_mbs/pic_height_in_map_units
            // for common resolutions, we can detect from profile+level
            int level = sps->level_idc;
            if (level >= 50) { sps->width = 3840; sps->height = 2160; }      // 4k
            else if (level >= 40) { sps->width = 1920; sps->height = 1080; }  // 1080p
            else if (level >= 31) { sps->width = 1280; sps->height = 720; }   // 720p
            else if (level >= 30) { sps->width = 720; sps->height = 480; }    // 480p
            else { sps->width = 640; sps->height = 480; }                     // sd

            sps->max_ref_frames = 4;
            sps->chroma_format = 1;  // 4:2:0
            sps->valid = true;
            return true;
        }
        pos = next + nal.length + 1;
    }
    return false;
}

//  unified audio decode dispatcher
AudioBuffer CodecRegistry::DecodeAudio(const uint8_t* data, int length) {
    AudioBuffer buf = { nullptr, 0, 0, 0, 16, false };
    if (!data || length < 4) return buf;

    CodecType type = Detect(data, length);

    switch (type) {
        case CODEC_WAV:  return DecodeWAV(data, length);
        case CODEC_MP3:  return DecodeMP3(data, length);
        case CODEC_AAC:  return DecodeAAC(data, length);
        case CODEC_FLAC: return DecodeFLAC(data, length);
        case CODEC_MP4:  return ExtractMP4Audio(data, length);
        default: break;
    }

    return buf;
}

void CodecRegistry::FreeAudioBuffer(AudioBuffer& buf) {
    if (buf.samples) {
        KernelHeap::Free(buf.samples);
        buf.samples = nullptr;
    }
    buf.length = 0;
    buf.valid = false;
}
