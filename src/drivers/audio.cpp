// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Sound Blaster 16 Audio Driver Implementation
//  Polling-based (no IRQ handler) — compatible with QEMU -device sb16
// ═══════════════════════════════════════════════════════════════════════════
#include "audio.h"
#include "../hal/hal.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include <string.h>

// ── Static member initialization ──
bool        Audio::available       = false;
bool        Audio::muted           = false;
int         Audio::master_volume   = 80;
int         Audio::dsp_version     = 0;
AudioState  Audio::state           = AUDIO_STOPPED;

// DMA buffer lives at a fixed low physical address so ISA DMA can reach it.
// 0x10000 is in conventional memory (64 KB), well below the 1 MB kernel load
// address and within the first 64 KB page boundary required by 8-bit DMA.
uint8_t* Audio::dma_buffer = reinterpret_cast<uint8_t*>(0x10000);

const uint8_t* Audio::pcm_source       = nullptr;
int         Audio::pcm_length      = 0;
int         Audio::pcm_offset      = 0;
int         Audio::current_rate    = AUDIO_SAMPLE_RATE;
int         Audio::current_bits    = 8;
int         Audio::current_channels = 1;
bool        Audio::looping         = false;

// ── I/O helper — small delay between port accesses ──
static inline void io_wait() {
    // Reading from port 0x80 causes a ~1μs delay on x86
    HAL::InByte(0x80);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DSP Communication
// ═══════════════════════════════════════════════════════════════════════════

bool Audio::ResetDSP() {
    // 1) Write 1 to reset port
    HAL::OutByte(SB16_DSP_RESET, 1);
    
    // 2) Wait at least 3μs
    for (int i = 0; i < 10; i++) io_wait();
    
    // 3) Write 0 to reset port
    HAL::OutByte(SB16_DSP_RESET, 0);
    
    // 4) Wait for ready byte (0xAA) on read port, with timeout
    //    Poll bit 7 of status port first to check data available
    for (int timeout = 0; timeout < 1000; timeout++) {
        uint8_t status = HAL::InByte(SB16_DSP_STATUS);     // port 0x22E
        if (status & 0x80) {
            uint8_t val = HAL::InByte(SB16_DSP_READ);       // port 0x22A
            if (val == 0xAA) {
                return true;  // DSP reset successful
            }
        }
        io_wait();
    }
    return false;  // Reset timed out
}

void Audio::WriteDSP(uint8_t cmd) {
    // Wait until DSP is ready to accept a command
    // Bit 7 of write status port must be 0 (not busy)
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
    // Wait until data is available from DSP
    for (int timeout = 0; timeout < 10000; timeout++) {
        if (HAL::InByte(SB16_DSP_STATUS) & 0x80) {
            return HAL::InByte(SB16_DSP_READ);
        }
        io_wait();
    }
    SerialLogger::Log("[AUDIO] ReadDSP timeout!\n");
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DMA Programming
// ═══════════════════════════════════════════════════════════════════════════

void Audio::ProgramDMA8(uint32_t phys_addr, uint16_t length) {
    // Program ISA DMA channel 1 for 8-bit single-cycle transfer
    // DMA channel 1 ports:
    //   Mask    = 0x0A
    //   Mode    = 0x0B
    //   FlipFlop= 0x0C
    //   Address = 0x02 (low/high byte of offset within 64K page)
    //   Count   = 0x03 (low/high byte of transfer count - 1)
    //   Page    = 0x83 (bits 16-23 of physical address)
    
    uint8_t page   = (uint8_t)((phys_addr >> 16) & 0xFF);
    uint16_t offset = (uint16_t)(phys_addr & 0xFFFF);
    uint16_t count  = (uint16_t)(length - 1);
    
    // 1) Mask (disable) channel 1
    HAL::OutByte(0x0A, 0x04 | DMA_CHANNEL_1);  // bit 2 = mask, lower bits = channel
    io_wait();
    
    // 2) Clear flip-flop
    HAL::OutByte(0x0C, 0x00);    // Any write resets the flip-flop
    io_wait();
    
    // 3) Set mode: single mode, read (from memory to device), channel 1
    //    Bits: [7:6]=00 demand, [5:4]=01 single, [3:2]=10 read (mem→device), [1:0]=01 channel
    //    Actually for playback: transfer from memory TO device = "read" mode
    //    Mode byte = 0x48 | channel = 0x48 | 0x01 = 0x49
    //    0x48 = single mode (01) + read (10) in bits [5:2] → 01 10 = 0x48
    HAL::OutByte(0x0B, 0x48 | DMA_CHANNEL_1);  // Single-cycle, read (mem→IO), channel 1
    io_wait();
    
    // 4) Set address (offset within 64K page)
    HAL::OutByte(0x02, (uint8_t)(offset & 0xFF));        // Low byte
    io_wait();
    HAL::OutByte(0x02, (uint8_t)((offset >> 8) & 0xFF)); // High byte
    io_wait();
    
    // 5) Set count (transfer length - 1)
    HAL::OutByte(0x03, (uint8_t)(count & 0xFF));          // Low byte
    io_wait();
    HAL::OutByte(0x03, (uint8_t)((count >> 8) & 0xFF));   // High byte
    io_wait();
    
    // 6) Set page register for channel 1
    HAL::OutByte(0x83, page);
    io_wait();
    
    // 7) Unmask (enable) channel 1
    HAL::OutByte(0x0A, DMA_CHANNEL_1);  // bit 2 = 0 means unmask
    io_wait();
}

void Audio::ProgramDMA16(uint32_t phys_addr, uint16_t length) {
    // Program ISA DMA channel 5 for 16-bit transfer
    // 16-bit DMA uses ports offset from 0xC0:
    //   Mask    = 0xD4
    //   Mode    = 0xD6
    //   FlipFlop= 0xD8
    //   Address = 0xC4 (for channel 5, word-addressed)
    //   Count   = 0xC6 (for channel 5, word count - 1)
    //   Page    = 0x8B
    
    // For 16-bit DMA, address is in 16-bit words, not bytes
    uint8_t page      = (uint8_t)((phys_addr >> 16) & 0xFE);  // Must be 128K-aligned page
    uint16_t offset   = (uint16_t)((phys_addr >> 1) & 0xFFFF); // Word address within page
    uint16_t count    = (uint16_t)((length / 2) - 1);           // Word count - 1
    
    // 1) Mask channel 5 (channel 5 = channel 1 in the second DMA controller)
    HAL::OutByte(0xD4, 0x04 | (DMA_CHANNEL_5 & 0x03));  // Channel 5 → index 1 in second controller
    io_wait();
    
    // 2) Clear flip-flop
    HAL::OutByte(0xD8, 0x00);
    io_wait();
    
    // 3) Set mode: single mode, read, channel 5 (index 1)
    HAL::OutByte(0xD6, 0x48 | (DMA_CHANNEL_5 & 0x03));
    io_wait();
    
    // 4) Set address (word offset)
    HAL::OutByte(0xC4, (uint8_t)(offset & 0xFF));
    io_wait();
    HAL::OutByte(0xC4, (uint8_t)((offset >> 8) & 0xFF));
    io_wait();
    
    // 5) Set count (word count - 1)
    HAL::OutByte(0xC6, (uint8_t)(count & 0xFF));
    io_wait();
    HAL::OutByte(0xC6, (uint8_t)((count >> 8) & 0xFF));
    io_wait();
    
    // 6) Set page register for channel 5
    HAL::OutByte(0x8B, page);
    io_wait();
    
    // 7) Unmask channel 5
    HAL::OutByte(0xD4, (DMA_CHANNEL_5 & 0x03));
    io_wait();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Mixer Control
// ═══════════════════════════════════════════════════════════════════════════

void Audio::SetMixerVolume(int vol) {
    // vol: 0-15 for each channel
    uint8_t v = (uint8_t)(vol & 0x0F);
    
    // SBPro-compatible registers (0x22 master, 0x04 voice)
    // Format: high nibble = left, low nibble = right
    uint8_t sbpro_val = (uint8_t)((v << 4) | v);
    
    HAL::OutByte(SB16_MIXER_ADDR, MIXER_MASTER_VOL);
    io_wait();
    HAL::OutByte(SB16_MIXER_DATA, sbpro_val);
    io_wait();
    
    HAL::OutByte(SB16_MIXER_ADDR, MIXER_VOICE_VOL);
    io_wait();
    HAL::OutByte(SB16_MIXER_DATA, sbpro_val);
    io_wait();
    
    // SB16-specific registers (0x30/0x31 master L/R, 0x32/0x33 voice L/R)
    // Format: bits [7:3] = volume (0-248 in steps of 8)
    uint8_t sb16_val = (uint8_t)((vol * 248) / 15);
    sb16_val &= 0xF8;  // Align to step of 8
    
    HAL::OutByte(SB16_MIXER_ADDR, 0x30); io_wait(); // Master Left
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
    HAL::OutByte(SB16_MIXER_ADDR, 0x31); io_wait(); // Master Right
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
    HAL::OutByte(SB16_MIXER_ADDR, 0x32); io_wait(); // Voice Left
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
    HAL::OutByte(SB16_MIXER_ADDR, 0x33); io_wait(); // Voice Right
    HAL::OutByte(SB16_MIXER_DATA, sb16_val); io_wait();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Initialization
// ═══════════════════════════════════════════════════════════════════════════

bool Audio::Init() {
    SerialLogger::Log("[AUDIO] Initializing Sound Blaster 16...\n");
    
    // Zero the DMA buffer
    memset(dma_buffer, 0x80, AUDIO_BUFFER_SIZE);  // 0x80 = silence for unsigned 8-bit PCM
    
    // Try to reset the DSP
    if (!ResetDSP()) {
        SerialLogger::Log("[AUDIO] DSP reset failed — no SB16 detected\n");
        available = false;
        return false;
    }
    
    // Get DSP version
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
    
    // Turn on speaker
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    // Set initial volume
    SetMixerVolume((master_volume * 15) / 100);
    
    available = true;
    state = AUDIO_STOPPED;
    
    SerialLogger::Log("[AUDIO] Sound Blaster 16 initialized successfully\n");
    return true;
}

bool Audio::IsAvailable() {
    return available;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Playback
// ═══════════════════════════════════════════════════════════════════════════

void Audio::FillDMABuffer() {
    if (!pcm_source || pcm_offset >= pcm_length) {
        // No more data — fill with silence
        if (current_bits == 8) {
            memset(dma_buffer, 0x80, AUDIO_BUFFER_SIZE);   // Unsigned 8-bit silence
        } else {
            memset(dma_buffer, 0x00, AUDIO_BUFFER_SIZE);   // Signed 16-bit silence
        }
        return;
    }
    
    int remaining = pcm_length - pcm_offset;
    int to_copy = (remaining < AUDIO_BUFFER_SIZE) ? remaining : AUDIO_BUFFER_SIZE;
    
    memcpy(dma_buffer, pcm_source + pcm_offset, to_copy);
    pcm_offset += to_copy;
    
    // If we didn't fill the whole buffer, pad with silence
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
    
    // Always transfer the full DMA buffer — FillDMABuffer() already padded with silence
    int transfer_len = AUDIO_BUFFER_SIZE;
    
    uint32_t buf_phys = (uint32_t)(uintptr_t)dma_buffer;
    
    if (current_bits == 8) {
        // ── 8-bit playback via DMA channel 1 ──
        ProgramDMA8(buf_phys, (uint16_t)transfer_len);
        
        // Set sample rate (DSP 4.x+ command 0x41)
        WriteDSP(DSP_CMD_SET_RATE);
        WriteDSP((uint8_t)((current_rate >> 8) & 0xFF));   // High byte
        WriteDSP((uint8_t)(current_rate & 0xFF));            // Low byte
        
        // Start 8-bit single-cycle DMA playback
        // Command 0xC0, then mode byte, then length-1 (low, high)
        WriteDSP(DSP_CMD_PLAY_8BIT);
        
        // Mode byte: bit 5 = stereo, bit 4 = signed
        uint8_t mode = 0x00;  // Mono, unsigned
        if (current_channels == 2) mode |= 0x20;
        WriteDSP(mode);
        
        // Transfer count - 1 (samples)
        uint16_t sample_count = (uint16_t)(transfer_len - 1);
        WriteDSP((uint8_t)(sample_count & 0xFF));
        WriteDSP((uint8_t)((sample_count >> 8) & 0xFF));
        
    } else {
        // ── 16-bit playback via DMA channel 5 ──
        ProgramDMA16(buf_phys, (uint16_t)transfer_len);
        
        // Set sample rate
        WriteDSP(DSP_CMD_SET_RATE);
        WriteDSP((uint8_t)((current_rate >> 8) & 0xFF));
        WriteDSP((uint8_t)(current_rate & 0xFF));
        
        // Start 16-bit single-cycle DMA playback
        // Command 0xB0, then mode byte, then sample count - 1
        WriteDSP(DSP_CMD_PLAY_16BIT);
        
        // Mode byte: bit 5 = stereo, bit 4 = signed (16-bit is usually signed)
        uint8_t mode = 0x10;  // Signed
        if (current_channels == 2) mode |= 0x20;
        WriteDSP(mode);
        
        // Transfer count in samples (not bytes). For 16-bit: length/2 - 1
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
    
    // Stop any current playback
    Stop();
    
    SerialLogger::Log("[AUDIO] Starting playback: ");
    SerialLogger::LogHex(sample_rate);
    SerialLogger::Log(" Hz, ");
    SerialLogger::LogHex(bits);
    SerialLogger::Log("-bit, ");
    SerialLogger::Log(channels == 2 ? "stereo" : "mono");
    SerialLogger::Log("\n");
    
    // Store playback parameters
    pcm_source      = pcm_data;
    pcm_length      = length;
    pcm_offset      = 0;
    current_rate     = sample_rate;
    current_bits     = bits;
    current_channels = channels;
    looping          = false;
    
    // Turn speaker on
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    // Apply current volume
    if (muted) {
        SetMixerVolume(0);
    } else {
        SetMixerVolume((master_volume * 15) / 100);
    }
    
    // Fill the DMA buffer with first chunk
    FillDMABuffer();
    
    // Program DMA and start DSP
    StartPlayback();
    
    state = AUDIO_PLAYING;
    return true;
}

void Audio::Stop() {
    if (!available) return;
    
    if (state == AUDIO_PLAYING || state == AUDIO_PAUSED) {
        // Send stop command
        if (current_bits == 8) {
            WriteDSP(DSP_CMD_STOP_8);
        } else {
            WriteDSP(DSP_CMD_STOP_16);
        }
        
        // Mask DMA channels to stop transfer
        HAL::OutByte(0x0A, 0x04 | DMA_CHANNEL_1);    // Mask channel 1
        HAL::OutByte(0xD4, 0x04 | (DMA_CHANNEL_5 & 0x03));  // Mask channel 5
    }
    
    state = AUDIO_STOPPED;
    pcm_source = nullptr;
    pcm_offset = 0;
    pcm_length = 0;
}

void Audio::Pause() {
    if (!available || state != AUDIO_PLAYING) return;
    
    if (current_bits == 8) {
        WriteDSP(DSP_CMD_STOP_8);    // Pause 8-bit DMA
    } else {
        WriteDSP(DSP_CMD_STOP_16);   // Pause 16-bit DMA
    }
    
    state = AUDIO_PAUSED;
}

void Audio::Resume() {
    if (!available || state != AUDIO_PAUSED) return;
    
    if (current_bits == 8) {
        WriteDSP(DSP_CMD_RESUME_8);   // Resume 8-bit DMA
    } else {
        WriteDSP(DSP_CMD_RESUME_16);  // Resume 16-bit DMA
    }
    
    state = AUDIO_PLAYING;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Volume Control
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Status
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Tone Generation
// ═══════════════════════════════════════════════════════════════════════════

// Simple sine approximation using integer math (no floating point in kernel)
// Returns value in range [-127, 127]
static int fast_sin(int angle_deg) {
    // Normalize to 0-359
    angle_deg = angle_deg % 360;
    if (angle_deg < 0) angle_deg += 360;
    
    // Simple piecewise linear approximation of sine
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
    PlayTone(frequency, duration_ms, 80);
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
    
    // Use auto-init DMA for gapless looping
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
    
    // Generate tone at 22050 Hz, 8-bit, mono
    int sample_rate = AUDIO_SAMPLE_RATE;
    int num_samples = (sample_rate * duration_ms) / 1000;
    if (num_samples > AUDIO_BUFFER_SIZE) num_samples = AUDIO_BUFFER_SIZE;
    
    // Generate waveform directly into DMA buffer
    // 8-bit unsigned PCM: silence = 128, range [0, 255]
    int vol_scale = (volume * 127) / 100;
    
    // Use fixed-point angle accumulation to avoid float
    // Phase accumulator: increment = (frequency * 360 * 256) / sample_rate
    // Using x256 fixed point to avoid overflow (frequency * 92160 fits in int32 for freq < 23000)
    int angle_inc_x256 = (int)(((long long)frequency * 360 * 256) / sample_rate);
    int angle_x256 = 0;
    
    for (int i = 0; i < num_samples; i++) {
        int sin_val = fast_sin(angle_x256 / 256);  // [-127, 127]
        int sample = 128 + ((sin_val * vol_scale) / 127);
        
        // Clamp
        if (sample < 0) sample = 0;
        if (sample > 255) sample = 255;
        
        dma_buffer[i] = (uint8_t)sample;
        angle_x256 += angle_inc_x256;
        if (angle_x256 >= 360 * 256) angle_x256 -= 360 * 256;
    }
    
    // Fill rest with silence
    if (num_samples < AUDIO_BUFFER_SIZE) {
        memset(dma_buffer + num_samples, 0x80, AUDIO_BUFFER_SIZE - num_samples);
    }
    
    // Play the generated tone
    current_rate = sample_rate;
    current_bits = 8;
    current_channels = 1;
    pcm_source = nullptr;  // Using DMA buffer directly (already filled)
    pcm_length = num_samples;
    pcm_offset = num_samples;  // Nothing more to refill
    
    // Apply volume
    if (muted) {
        SetMixerVolume(0);
    } else {
        SetMixerVolume((master_volume * 15) / 100);
    }
    
    WriteDSP(DSP_CMD_SPEAKER_ON);
    
    // Program DMA
    uint32_t buf_phys = (uint32_t)(uintptr_t)dma_buffer;
    ProgramDMA8(buf_phys, (uint16_t)num_samples);
    
    // Set sample rate
    WriteDSP(DSP_CMD_SET_RATE);
    WriteDSP((uint8_t)((sample_rate >> 8) & 0xFF));
    WriteDSP((uint8_t)(sample_rate & 0xFF));
    
    // Start playback
    WriteDSP(DSP_CMD_PLAY_8BIT);
    WriteDSP(0x00);  // Mono, unsigned
    uint16_t count_minus1 = (uint16_t)(num_samples - 1);
    WriteDSP((uint8_t)(count_minus1 & 0xFF));
    WriteDSP((uint8_t)((count_minus1 >> 8) & 0xFF));
    
    state = AUDIO_PLAYING;
}

// ═══════════════════════════════════════════════════════════════════════════
//  IRQ Handler (called if we have interrupt support)
// ═══════════════════════════════════════════════════════════════════════════

void Audio::HandleIRQ() {
    if (!available) return;
    
    // Acknowledge the interrupt
    if (current_bits == 8) {
        HAL::InByte(SB16_DSP_STATUS);      // 8-bit IRQ ack: read port 0x22E
    } else {
        HAL::InByte(SB16_DSP_INT_ACK);     // 16-bit IRQ ack: read port 0x22F
    }
    
    // If more data to play, refill and restart
    if (pcm_source && pcm_offset < pcm_length) {
        FillDMABuffer();
        StartPlayback();
    } else if (looping && pcm_source) {
        pcm_offset = 0;
        FillDMABuffer();
        StartPlayback();
    } else {
        state = AUDIO_STOPPED;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tick — Polling-based buffer management
//  Call this periodically from the main loop
// ═══════════════════════════════════════════════════════════════════════════

void Audio::Tick() {
    if (!available || state != AUDIO_PLAYING) return;
    
    // In polling mode without IRQs, we need to check if the DMA transfer completed.
    // We do this by reading the DMA channel 1 current count register.
    // When count reaches 0, the transfer is done.
    
    if (current_bits == 8) {
        // Clear flip-flop for DMA controller 1
        HAL::OutByte(0x0C, 0x00);
        io_wait();
        
        // Read current count for channel 1 (port 0x03)
        uint8_t lo = HAL::InByte(0x03);
        io_wait();
        uint8_t hi = HAL::InByte(0x03);
        uint16_t remaining_count = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        
        // If count is 0 or very low, transfer is complete
        if (remaining_count <= 1) {
            // Check if more data to play
            if (pcm_source && pcm_offset < pcm_length) {
                // Acknowledge any pending interrupt
                HAL::InByte(SB16_DSP_STATUS);
                
                FillDMABuffer();
                StartPlayback();
            } else if (looping && pcm_source) {
                pcm_offset = 0;
                HAL::InByte(SB16_DSP_STATUS);
                FillDMABuffer();
                StartPlayback();
            } else {
                // Playback finished
                HAL::InByte(SB16_DSP_STATUS);
                state = AUDIO_STOPPED;
                SerialLogger::Log("[AUDIO] Playback complete\n");
            }
        }
    } else {
        // 16-bit DMA channel 5 — clear flip-flop for controller 2
        HAL::OutByte(0xD8, 0x00);
        io_wait();
        
        // Read current count for channel 5 (port 0xC6)
        uint8_t lo = HAL::InByte(0xC6);
        io_wait();
        uint8_t hi = HAL::InByte(0xC6);
        uint16_t remaining_count = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        
        if (remaining_count <= 1) {
            if (pcm_source && pcm_offset < pcm_length) {
                HAL::InByte(SB16_DSP_INT_ACK);
                FillDMABuffer();
                StartPlayback();
            } else if (looping && pcm_source) {
                pcm_offset = 0;
                HAL::InByte(SB16_DSP_INT_ACK);
                FillDMABuffer();
                StartPlayback();
            } else {
                HAL::InByte(SB16_DSP_INT_ACK);
                state = AUDIO_STOPPED;
                SerialLogger::Log("[AUDIO] Playback complete\n");
            }
        }
    }
}
