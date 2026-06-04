//  kurono os  -  sound blaster 16 audio driver implementation
//  polling-based (no irq handler)  -  compatible with qemu -device sb16
#include "audio.h"
#include "../hal/hal.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include <string.h>

bool        Audio::available       = false;
bool        Audio::muted           = false;
int         Audio::master_volume   = 80;
int         Audio::dsp_version     = 0;
AudioState  Audio::state           = AUDIO_STOPPED;

// dma buffer lives at a fixed low physical address so isa dma can reach it.
// 0x68000 keeps the 32kb buffer fully inside one 64kb dma page
// (0x68000-0x6FFFF), below 1mb, and out of the way of the new
// AudioDMA-managed regions for AC97 (0x70000+) and the new SB16
// backend (0x60000).  See drivers/audio_dma.h for the full map.
uint8_t* Audio::dma_buffer = reinterpret_cast<uint8_t*>(0x68000);

const uint8_t* Audio::pcm_source       = nullptr;
int         Audio::pcm_length      = 0;
int         Audio::pcm_offset      = 0;
int         Audio::current_rate    = AUDIO_SAMPLE_RATE;
int         Audio::current_bits    = 8;
int         Audio::current_channels = 1;
bool        Audio::looping         = false;

static inline void io_wait() {
    // reading from port 0x80 causes a ~1μs delay on x86
    HAL::InByte(0x80);
}

//  dsp communication

bool Audio::ResetDSP() {
    // 1) write 1 to reset port
    HAL::OutByte(SB16_DSP_RESET, 1);
    
    // 2) wait at least 3μs
    for (int i = 0; i < 10; i++) io_wait();
    
    // 3) write 0 to reset port
    HAL::OutByte(SB16_DSP_RESET, 0);
    
    // 4) wait for ready byte (0xaa) on read port, with timeout
    //    poll bit 7 of status port first to check data available
    for (int timeout = 0; timeout < 1000; timeout++) {
        uint8_t status = HAL::InByte(SB16_DSP_STATUS);     // port 0x22e
        if (status & 0x80) {
            uint8_t val = HAL::InByte(SB16_DSP_READ);       // port 0x22a
            if (val == 0xAA) {
                return true;  // dsp reset successful
            }
        }
        io_wait();
    }
    return false;  // reset timed out
}

void Audio::WriteDSP(uint8_t cmd) {
    // wait until dsp is ready to accept a command
    // bit 7 of write status port must be 0 (not busy)
    for (int timeout = 0; timeout < 10000; timeout++) {
        if (!(HAL::InByte(SB16_DSP_WRITE) & 0x80)) {
            HAL::OutByte(SB16_DSP_WRITE, cmd);
            return;
        }
        io_wait();
    }
    SerialLogger::Log("[AUDIO] WriteDSP timeout!\n");
}

uint8_t Audio::ReadDSP() {
    // wait until data is available from dsp
    for (int timeout = 0; timeout < 10000; timeout++) {
        if (HAL::InByte(SB16_DSP_STATUS) & 0x80) {
            return HAL::InByte(SB16_DSP_READ);
        }
        io_wait();
    }
    SerialLogger::Log("[AUDIO] ReadDSP timeout!\n");
    return 0;
}

//  dma programming

void Audio::ProgramDMA8(uint32_t phys_addr, uint16_t length) {
    // program isa dma channel 1 for 8-bit single-cycle transfer
    // dma channel 1 ports:
    //   mask    = 0x0a
    //   mode    = 0x0b
    //   flipflop= 0x0c
    //   address = 0x02 (low/high byte of offset within 64k page)
    //   count   = 0x03 (low/high byte of transfer count - 1)
    //   page    = 0x83 (bits 16-23 of physical address)
    
    uint8_t page   = (uint8_t)((phys_addr >> 16) & 0xFF);
    uint16_t offset = (uint16_t)(phys_addr & 0xFFFF);
    uint16_t count  = (uint16_t)(length - 1);
    
    // 1) mask (disable) channel 1
    HAL::OutByte(0x0A, 0x04 | DMA_CHANNEL_1);  // bit 2 = mask, lower bits = channel
    io_wait();
    
    // 2) clear flip-flop
    HAL::OutByte(0x0C, 0x00);    // any write resets the flip-flop
    io_wait();
    
    // 3) set mode: single mode, read (from memory to device), channel 1
    //    bits: [7:6]=00 demand, [5:4]=01 single, [3:2]=10 read (mem→device), [1:0]=01 channel
    //    actually for playback: transfer from memory to device = "read" mode
    //    mode byte = 0x48 | channel = 0x48 | 0x01 = 0x49
    //    0x48 = single mode (01) + read (10) in bits [5:2] → 01 10 = 0x48
    HAL::OutByte(0x0B, 0x48 | DMA_CHANNEL_1);  // single-cycle, read (mem→io), channel 1
    io_wait();
    
    // 4) set address (offset within 64k page)
    HAL::OutByte(0x02, (uint8_t)(offset & 0xFF));        // low byte
    io_wait();
    HAL::OutByte(0x02, (uint8_t)((offset >> 8) & 0xFF)); // high byte
    io_wait();
    
    // 5) set count (transfer length - 1)
    HAL::OutByte(0x03, (uint8_t)(count & 0xFF));          // low byte
    io_wait();
    HAL::OutByte(0x03, (uint8_t)((count >> 8) & 0xFF));   // high byte
    io_wait();
    
    // 6) set page register for channel 1
    HAL::OutByte(0x83, page);
    io_wait();
    
    // 7) unmask (enable) channel 1
    HAL::OutByte(0x0A, DMA_CHANNEL_1);  // bit 2 = 0 means unmask
    io_wait();
}

void Audio::ProgramDMA16(uint32_t phys_addr, uint16_t length) {
    // program isa dma channel 5 for 16-bit transfer
    // 16-bit dma uses ports offset from 0xc0:
    //   mask    = 0xd4
    //   mode    = 0xd6
    //   flipflop= 0xd8
    //   address = 0xc4 (for channel 5, word-addressed)
    //   count   = 0xc6 (for channel 5, word count - 1)
    //   page    = 0x8b
    
    // for 16-bit dma, address is in 16-bit words, not bytes
    uint8_t page      = (uint8_t)((phys_addr >> 16) & 0xFE);  // must be 128k-aligned page
    uint16_t offset   = (uint16_t)((phys_addr >> 1) & 0xFFFF); // word address within page
    uint16_t count    = (uint16_t)((length / 2) - 1);           // word count - 1
    
    // 1) mask channel 5 (channel 5 = channel 1 in the second dma controller)
    HAL::OutByte(0xD4, 0x04 | (DMA_CHANNEL_5 & 0x03));  // channel 5 → index 1 in second controller
    io_wait();
    
    // 2) clear flip-flop
    HAL::OutByte(0xD8, 0x00);
    io_wait();
    
    // 3) set mode: single mode, read, channel 5 (index 1)
    HAL::OutByte(0xD6, 0x48 | (DMA_CHANNEL_5 & 0x03));
    io_wait();
    
    // 4) set address (word offset)
    HAL::OutByte(0xC4, (uint8_t)(offset & 0xFF));
    io_wait();
    HAL::OutByte(0xC4, (uint8_t)((offset >> 8) & 0xFF));
    io_wait();
    
    // 5) set count (word count - 1)
    HAL::OutByte(0xC6, (uint8_t)(count & 0xFF));
    io_wait();
    HAL::OutByte(0xC6, (uint8_t)((count >> 8) & 0xFF));
    io_wait();
    
    // 6) set page register for channel 5
    HAL::OutByte(0x8B, page);
    io_wait();
    
    // 7) unmask channel 5
    HAL::OutByte(0xD4, (DMA_CHANNEL_5 & 0x03));
    io_wait();
}

//  mixer control

void Audio::SetMixerVolume(int vol) {
    // vol: 0-15 for each channel
    uint8_t v = (uint8_t)(vol & 0x0F);
    
    // sbpro-compatible registers (0x22 master, 0x04 voice)
    // format: high nibble = left, low nibble = right
    uint8_t sbpro_val = (uint8_t)((v << 4) | v);
    
    HAL::OutByte(SB16_MIXER_ADDR, MIXER_MASTER_VOL);
    io_wait();
    HAL::OutByte(SB16_MIXER_DATA, sbpro_val);
    io_wait();
    
    HAL::OutByte(SB16_MIXER_ADDR, MIXER_VOICE_VOL);
    io_wait();
    HAL::OutByte(SB16_MIXER_DATA, sbpro_val);
    io_wait();
    
    // sb16-specific registers (0x30/0x31 master l/r, 0x32/0x33 voice l/r)
    // format: bits [7:3] = volume (0-248 in steps of 8)
    uint8_t sb16_val = (uint8_t)((vol * 248) / 15);
    sb16_val &= 0xF8;  // align to step of 8
    
    HAL::OutByte(SB16_MIXER_ADDR, 0x30); io_wait(); // master left
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
    HAL::OutByte(SB16_MIXER_ADDR, 0x31); io_wait(); // master right
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
    HAL::OutByte(SB16_MIXER_ADDR, 0x32); io_wait(); // voice left
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
    HAL::OutByte(SB16_MIXER_ADDR, 0x33); io_wait(); // voice right
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
}

//  initialization

bool Audio::Init() {
    SerialLogger::Log("[AUDIO] Initializing Sound Blaster 16...\n");
    
    // zero the dma buffer
    memset(dma_buffer, 0x80, AUDIO_BUFFER_SIZE);  // 0x80 = silence for unsigned 8-bit pcm
    
    // try to reset the dsp
    if (!ResetDSP()) {
        SerialLogger::Log("[AUDIO] DSP reset failed  -  no SB16 detected\n");
        available = false;
        return false;
    }
    
    // get dsp version
    WriteDSP(DSP_CMD_GET_VERSION);
    uint8_t ver_hi = ReadDSP();
    uint8_t ver_lo = ReadDSP();
    dsp_version = (ver_hi << 8) | ver_lo;
    
    SerialLogger::Log("[AUDIO] DSP version: ");
    SerialLogger::LogHex(ver_hi);
    SerialLogger::Log(".");
    SerialLogger::LogHex(ver_lo);
    SerialLogger::Log("\n");
    
    if (ver_hi < 4) {
        SerialLogger::Log("[AUDIO] Warning: DSP version < 4.x, some features may not work\n");
    }
    
    // turn on speaker
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    // set initial volume
    SetMixerVolume((master_volume * 15) / 100);
    
    available = true;
    state = AUDIO_STOPPED;
    
    SerialLogger::Log("[AUDIO] Sound Blaster 16 initialized successfully\n");
    return true;
}

bool Audio::IsAvailable() {
    return available;
}

//  playback

void Audio::FillDMABuffer() {
    if (!pcm_source || pcm_offset >= pcm_length) {
        // no more data  -  fill with silence
        if (current_bits == 8) {
            memset(dma_buffer, 0x80, AUDIO_BUFFER_SIZE);   // unsigned 8-bit silence
        } else {
            memset(dma_buffer, 0x00, AUDIO_BUFFER_SIZE);   // signed 16-bit silence
        }
        return;
    }
    
    int remaining = pcm_length - pcm_offset;
    int to_copy = (remaining < AUDIO_BUFFER_SIZE) ? remaining : AUDIO_BUFFER_SIZE;
    
    memcpy(dma_buffer, pcm_source + pcm_offset, to_copy);
    pcm_offset += to_copy;
    
    // if we didn't fill the whole buffer, pad with silence
    if (to_copy < AUDIO_BUFFER_SIZE) {
        if (current_bits == 8) {
            memset(dma_buffer + to_copy, 0x80, AUDIO_BUFFER_SIZE - to_copy);
        } else {
            memset(dma_buffer + to_copy, 0x00, AUDIO_BUFFER_SIZE - to_copy);
        }
    }
}

void Audio::StartPlayback() {
    if (!available) return;
    
    // always transfer the full dma buffer  -  filldmabuffer() already padded with silence
    int transfer_len = AUDIO_BUFFER_SIZE;
    
    uint32_t buf_phys = (uint32_t)(uintptr_t)dma_buffer;
    
    if (current_bits == 8) {
        ProgramDMA8(buf_phys, (uint16_t)transfer_len);
        
        // set sample rate (dsp 4.x+ command 0x41)
        WriteDSP(DSP_CMD_SET_RATE);
        WriteDSP((uint8_t)((current_rate >> 8) & 0xFF));   // high byte
        WriteDSP((uint8_t)(current_rate & 0xFF));            // low byte
        
        // start 8-bit single-cycle dma playback
        // command 0xc0, then mode byte, then length-1 (low, high)
        WriteDSP(DSP_CMD_PLAY_8BIT);
        
        // mode byte: bit 5 = stereo, bit 4 = signed
        uint8_t mode = 0x00;  // mono, unsigned
        if (current_channels == 2) mode |= 0x20;
        WriteDSP(mode);
        
        // transfer count - 1 (samples)
        uint16_t sample_count = (uint16_t)(transfer_len - 1);
        WriteDSP((uint8_t)(sample_count & 0xFF));
        WriteDSP((uint8_t)((sample_count >> 8) & 0xFF));
        
    } else {
        ProgramDMA16(buf_phys, (uint16_t)transfer_len);
        
        // set sample rate
        WriteDSP(DSP_CMD_SET_RATE);
        WriteDSP((uint8_t)((current_rate >> 8) & 0xFF));
        WriteDSP((uint8_t)(current_rate & 0xFF));
        
        // start 16-bit single-cycle dma playback
        // command 0xb0, then mode byte, then sample count - 1
        WriteDSP(DSP_CMD_PLAY_16BIT);
        
        // mode byte: bit 5 = stereo, bit 4 = signed (16-bit is usually signed)
        uint8_t mode = 0x10;  // signed
        if (current_channels == 2) mode |= 0x20;
        WriteDSP(mode);
        
        // transfer count in samples (not bytes). for 16-bit: length/2 - 1
        uint16_t sample_count = (uint16_t)((transfer_len / 2) - 1);
        WriteDSP((uint8_t)(sample_count & 0xFF));
        WriteDSP((uint8_t)((sample_count >> 8) & 0xFF));
    }
}

bool Audio::Play(const uint8_t* pcm_data, int length, int sample_rate, int bits, int channels) {
    if (!available) return false;
    if (!pcm_data || length <= 0) return false;
    if (bits != 8 && bits != 16) return false;
    if (channels != 1 && channels != 2) return false;
    if (sample_rate < 4000 || sample_rate > 44100) return false;
    
    // stop any current playback
    Stop();
    
    SerialLogger::Log("[AUDIO] Starting playback: ");
    SerialLogger::LogHex(sample_rate);
    SerialLogger::Log(" Hz, ");
    SerialLogger::LogHex(bits);
    SerialLogger::Log("-bit, ");
    SerialLogger::Log(channels == 2 ? "stereo" : "mono");
    SerialLogger::Log("\n");
    
    // store playback parameters
    pcm_source      = pcm_data;
    pcm_length      = length;
    pcm_offset      = 0;
    current_rate     = sample_rate;
    current_bits     = bits;
    current_channels = channels;
    looping          = false;
    
    // turn speaker on
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    // apply current volume
    if (muted) {
        SetMixerVolume(0);
    } else {
        SetMixerVolume((master_volume * 15) / 100);
    }
    
    // fill the dma buffer with first chunk
    FillDMABuffer();
    
    // program dma and start dsp
    StartPlayback();
    
    state = AUDIO_PLAYING;
    return true;
}

void Audio::Stop() {
    if (!available) return;
    
    if (state == AUDIO_PLAYING || state == AUDIO_PAUSED) {
        // send stop command
        if (current_bits == 8) {
            WriteDSP(DSP_CMD_STOP_8);
        } else {
            WriteDSP(DSP_CMD_STOP_16);
        }
        
        // mask dma channels to stop transfer
        HAL::OutByte(0x0A, 0x04 | DMA_CHANNEL_1);    // mask channel 1
        HAL::OutByte(0xD4, 0x04 | (DMA_CHANNEL_5 & 0x03));  // mask channel 5
    }
    
    state = AUDIO_STOPPED;
    pcm_source = nullptr;
    pcm_offset = 0;
    pcm_length = 0;
}

void Audio::Pause() {
    if (!available || state != AUDIO_PLAYING) return;
    
    if (current_bits == 8) {
        WriteDSP(DSP_CMD_STOP_8);    // pause 8-bit dma
    } else {
        WriteDSP(DSP_CMD_STOP_16);   // pause 16-bit dma
    }
    
    state = AUDIO_PAUSED;
}

void Audio::Resume() {
    if (!available || state != AUDIO_PAUSED) return;
    
    if (current_bits == 8) {
        WriteDSP(DSP_CMD_RESUME_8);   // resume 8-bit dma
    } else {
        WriteDSP(DSP_CMD_RESUME_16);  // resume 16-bit dma
    }
    
    state = AUDIO_PLAYING;
}

//  volume control

void Audio::SetMasterVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    master_volume = volume;
    
    if (available && !muted) {
        SetMixerVolume((volume * 15) / 100);
    }
}

int Audio::GetMasterVolume() {
    return master_volume;
}

void Audio::SetMuted(bool m) {
    muted = m;
    if (available) {
        if (muted) {
            SetMixerVolume(0);
        } else {
            SetMixerVolume((master_volume * 15) / 100);
        }
    }
}

bool Audio::IsMuted() {
    return muted;
}

//  status

AudioState Audio::GetState() {
    return state;
}

AudioInfo Audio::GetInfo() {
    AudioInfo info;
    info.sample_rate = current_rate;
    info.channels    = current_channels;
    info.bits        = current_bits;
    info.buffer_fill = (pcm_length > 0) ? ((pcm_offset * 100) / pcm_length) : 0;
    info.looping     = looping;
    info.state       = state;
    return info;
}

int Audio::GetDSPVersion() {
    return dsp_version;
}

//  tone generation

// simple sine approximation using integer math (no floating point in kernel)
// returns value in range [-127, 127]
static int fast_sin(int angle_deg) {
    // normalize to 0-359
    angle_deg = angle_deg % 360;
    if (angle_deg < 0) angle_deg += 360;
    
    // simple piecewise linear approximation of sine
    // 0-90:   rise from 0 to 127
    // 90-180: fall from 127 to 0
    // 180-270: fall from 0 to -127
    // 270-360: rise from -127 to 0
    if (angle_deg <= 90) {
        return (angle_deg * 127) / 90;
    } else if (angle_deg <= 180) {
        return ((180 - angle_deg) * 127) / 90;
    } else if (angle_deg <= 270) {
        return -((angle_deg - 180) * 127) / 90;
    } else {
        return -((360 - angle_deg) * 127) / 90;
    }
}

void Audio::Beep(int frequency, int duration_ms) {
    // Route through the unified mixer so beeps don't fight with active
    // music streams.  Falls back to the legacy SB16-direct PlayTone if
    // the AudioServer hasn't been initialised yet (e.g. very early boot).
    extern void __audio_server_play_tone_proxy(int, int, int);
    __audio_server_play_tone_proxy(frequency, duration_ms, 80);
}

void Audio::GenerateBuffer(int frequency, int volume) {
    if (frequency < 20 || frequency > 20000) return;
    int vol_scale = (volume * 127) / 100;
    int angle_inc_x256 = (int)(((long long)frequency * 360 * 256) / AUDIO_SAMPLE_RATE);
    static int angle_x256 = 0; // persistent phase for gapless loops
    for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        int sin_val = 0;
        int a = (angle_x256 / 256) % 360;
        if (a < 0) a += 360;
        if (a <= 90) sin_val = (a * 127) / 90;
        else if (a <= 180) sin_val = ((180-a) * 127) / 90;
        else if (a <= 270) sin_val = -((a-180) * 127) / 90;
        else sin_val = -((360-a) * 127) / 90;
        int sample = 128 + ((sin_val * vol_scale) / 127);
        if (sample < 0) sample = 0;
        if (sample > 255) sample = 255;
        dma_buffer[i] = (uint8_t)sample;
        angle_x256 += angle_inc_x256;
        if (angle_x256 >= 360 * 256) angle_x256 -= 360 * 256;
    }
}

void Audio::PlayLoopTone(int frequency, int volume) {
    if (!available) return;
    if (frequency < 20 || frequency > 20000) return;
    
    Stop();
    GenerateBuffer(frequency, volume);
    
    current_rate = AUDIO_SAMPLE_RATE;
    current_bits = 8;
    current_channels = 1;
    pcm_source = dma_buffer; // point to self for looping
    pcm_length = AUDIO_BUFFER_SIZE;
    pcm_offset = 0;
    looping = true;
    
    if (muted) SetMixerVolume(0);
    else SetMixerVolume((master_volume * 15) / 100);
    
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    uint32_t buf_phys = (uint32_t)(uintptr_t)dma_buffer;
    ProgramDMA8(buf_phys, (uint16_t)AUDIO_BUFFER_SIZE);
    WriteDSP(DSP_CMD_SET_RATE);
    WriteDSP((uint8_t)((AUDIO_SAMPLE_RATE >> 8) & 0xFF));
    WriteDSP((uint8_t)(AUDIO_SAMPLE_RATE & 0xFF));
    
    // use auto-init dma for gapless looping
    WriteDSP(DSP_CMD_PLAY_8_AUTO);
    WriteDSP(0x00); // mono, unsigned
    uint16_t cnt = (uint16_t)(AUDIO_BUFFER_SIZE - 1);
    WriteDSP((uint8_t)(cnt & 0xFF));
    WriteDSP((uint8_t)((cnt >> 8) & 0xFF));
    
    state = AUDIO_PLAYING;
    SerialLogger::Log("[AUDIO] Loop tone started\n");
}

void Audio::PlayTone(int frequency, int duration_ms, int volume) {
    if (!available) return;
    if (frequency < 20 || frequency > 20000) return;
    if (duration_ms <= 0 || duration_ms > 5000) return;
    
    // generate tone at 22050 hz, 8-bit, mono
    int sample_rate = AUDIO_SAMPLE_RATE;
    int num_samples = (sample_rate * duration_ms) / 1000;
    if (num_samples > AUDIO_BUFFER_SIZE) num_samples = AUDIO_BUFFER_SIZE;
    
    // generate waveform directly into dma buffer
    // 8-bit unsigned pcm: silence = 128, range [0, 255]
    int vol_scale = (volume * 127) / 100;
    
    // use fixed-point angle accumulation to avoid float
    // phase accumulator: increment = (frequency * 360 * 256) / sample_rate
    // using x256 fixed point to avoid overflow (frequency * 92160 fits in int32 for freq < 23000)
    int angle_inc_x256 = (int)(((long long)frequency * 360 * 256) / sample_rate);
    int angle_x256 = 0;
    
    for (int i = 0; i < num_samples; i++) {
        int sin_val = fast_sin(angle_x256 / 256);  // [-127, 127]
        int sample = 128 + ((sin_val * vol_scale) / 127);
        
        // clamp
        if (sample < 0) sample = 0;
        if (sample > 255) sample = 255;
        
        dma_buffer[i] = (uint8_t)sample;
        angle_x256 += angle_inc_x256;
        if (angle_x256 >= 360 * 256) angle_x256 -= 360 * 256;
    }
    
    // fill rest with silence
    if (num_samples < AUDIO_BUFFER_SIZE) {
        memset(dma_buffer + num_samples, 0x80, AUDIO_BUFFER_SIZE - num_samples);
    }
    
    // play the generated tone
    current_rate = sample_rate;
    current_bits = 8;
    current_channels = 1;
    pcm_source = nullptr;  // using dma buffer directly (already filled)
    pcm_length = num_samples;
    pcm_offset = num_samples;  // nothing more to refill
    
    // apply volume
    if (muted) {
        SetMixerVolume(0);
    } else {
        SetMixerVolume((master_volume * 15) / 100);
    }
    
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    // program dma
    uint32_t buf_phys = (uint32_t)(uintptr_t)dma_buffer;
    ProgramDMA8(buf_phys, (uint16_t)num_samples);
    
    // set sample rate
    WriteDSP(DSP_CMD_SET_RATE);
    WriteDSP((uint8_t)((sample_rate >> 8) & 0xFF));
    WriteDSP((uint8_t)(sample_rate & 0xFF));
    
    // start playback
    WriteDSP(DSP_CMD_PLAY_8BIT);
    WriteDSP(0x00);  // mono, unsigned
    uint16_t count_minus1 = (uint16_t)(num_samples - 1);
    WriteDSP((uint8_t)(count_minus1 & 0xFF));
    WriteDSP((uint8_t)((count_minus1 >> 8) & 0xFF));
    
    state = AUDIO_PLAYING;
}

//  irq handler (called if we have interrupt support)

void Audio::HandleIRQ() {
    if (!available) return;

    // 1) Acknowledge the IRQ first so the controller is ready to fire
    //    again the moment the next buffer completes.  Order matters: a
    //    late ack causes the next half to be lost on auto-init paths.
    if (current_bits == 8) {
        HAL::InByte(SB16_DSP_STATUS);      // 8-bit irq ack: read port 0x22e
    } else {
        HAL::InByte(SB16_DSP_INT_ACK);     // 16-bit irq ack: read port 0x22f
    }

    // 2) If there's more data, refill + relaunch the DMA so the DAC's
    //    FIFO is fed before we do any bookkeeping.
    if (pcm_source && pcm_offset < pcm_length) {
        FillDMABuffer();
        StartPlayback();
    } else if (looping && pcm_source) {
        pcm_offset = 0;
        FillDMABuffer();
        StartPlayback();
    } else {
        // Stream finished  -  mark stopped and release the source pointer
        // so a subsequent Play() / Stop() doesn't see stale state.
        state = AUDIO_STOPPED;
        pcm_source = nullptr;
        pcm_offset = 0;
        pcm_length = 0;
    }
}

//  tick  -  polling-based buffer management
//  call this periodically from the main loop

void Audio::Tick() {
    if (!available || state != AUDIO_PLAYING) return;

    // Read DMA remaining-count from the relevant controller.  Bytes (8-bit
    // DMA) or words (16-bit DMA)  -  semantics here are just "did the DMA
    // reach the end of the buffer?"  Ack the IRQ regardless to avoid
    // missing the next half on auto-init engines.
    uint16_t remaining_count;
    if (current_bits == 8) {
        HAL::OutByte(0x0C, 0x00);            // flip-flop reset (DMAC 1)
        uint8_t lo = HAL::InByte(0x03);
        uint8_t hi = HAL::InByte(0x03);
        remaining_count = (uint16_t)lo | ((uint16_t)hi << 8);
    } else {
        HAL::OutByte(0xD8, 0x00);            // flip-flop reset (DMAC 2)
        uint8_t lo = HAL::InByte(0xC6);
        uint8_t hi = HAL::InByte(0xC6);
        remaining_count = (uint16_t)lo | ((uint16_t)hi << 8);
    }

    if (remaining_count > 1) return;        // still draining

    // Ack first so the next buffer's completion interrupt won't be missed.
    if (current_bits == 8) HAL::InByte(SB16_DSP_STATUS);
    else                   HAL::InByte(SB16_DSP_INT_ACK);

    if (pcm_source && pcm_offset < pcm_length) {
        FillDMABuffer();
        StartPlayback();
    } else if (looping && pcm_source) {
        pcm_offset = 0;
        FillDMABuffer();
        StartPlayback();
    } else {
        state = AUDIO_STOPPED;
        pcm_source = nullptr;
        pcm_offset = 0;
        pcm_length = 0;
        SerialLogger::Log("[AUDIO] Playback complete\n");
    }
}
