#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Sound Blaster 16 Audio Driver
//  Supports PCM playback via ISA DMA + SB16 DSP
// ═══════════════════════════════════════════════════════════════════════════
#include "../kernel/types.h"

// ── SB16 I/O Ports (base = 0x220) ──
#define SB16_BASE          0x220
#define SB16_MIXER_ADDR    (SB16_BASE + 0x04)
#define SB16_MIXER_DATA    (SB16_BASE + 0x05)
#define SB16_DSP_RESET     (SB16_BASE + 0x06)
#define SB16_DSP_READ      (SB16_BASE + 0x0A)
#define SB16_DSP_WRITE     (SB16_BASE + 0x0C)
#define SB16_DSP_STATUS    (SB16_BASE + 0x0E)
#define SB16_DSP_INT_ACK   (SB16_BASE + 0x0F)   // 16-bit IRQ ack

// ── DSP Commands ──
#define DSP_CMD_SET_TIME    0x40   // Set time constant
#define DSP_CMD_SET_RATE    0x41   // Set output sample rate
#define DSP_CMD_SPEAKER_ON  0xD1   // Turn speaker on
#define DSP_CMD_SPEAKER_OFF 0xD3   // Turn speaker off
#define DSP_CMD_PLAY_8BIT   0xC0   // Start 8-bit DMA, single-cycle
#define DSP_CMD_PLAY_16BIT  0xB0   // Start 16-bit DMA, single-cycle
#define DSP_CMD_PLAY_8_AUTO 0xC6   // Start 8-bit DMA, auto-init
#define DSP_CMD_PLAY_16_AUTO 0xB6  // Start 16-bit DMA, auto-init
#define DSP_CMD_STOP_8      0xD0   // Pause 8-bit DMA
#define DSP_CMD_RESUME_8    0xD4   // Resume 8-bit DMA
#define DSP_CMD_STOP_16     0xD5   // Pause 16-bit DMA
#define DSP_CMD_RESUME_16   0xD6   // Resume 16-bit DMA
#define DSP_CMD_GET_VERSION 0xE1   // Get DSP version

// ── Mixer Registers ──
#define MIXER_MASTER_VOL    0x22   // Master volume (L:R nibbles)
#define MIXER_VOICE_VOL     0x04   // Voice/DAC volume
#define MIXER_IRQ_SEL       0x80   // IRQ select
#define MIXER_DMA_SEL       0x81   // DMA select  
#define MIXER_INT_STATUS    0x82   // Interrupt status

// ── DMA Channels ──
#define DMA_CHANNEL_1       1      // 8-bit DMA
#define DMA_CHANNEL_5       5      // 16-bit DMA

// ── Audio Buffer ──
#define AUDIO_BUFFER_SIZE   32768  // 32KB DMA buffer
#define AUDIO_SAMPLE_RATE   22050  // Default sample rate
#define AUDIO_IRQ           5

// ── Playback State ──
enum AudioState {
    AUDIO_STOPPED = 0,
    AUDIO_PLAYING,
    AUDIO_PAUSED
};

struct AudioInfo {
    int      sample_rate;
    int      channels;       // 1=mono, 2=stereo
    int      bits;           // 8 or 16
    int      buffer_fill;    // bytes in buffer
    bool     looping;
    AudioState state;
};

class Audio {
public:
    // Initialization
    static bool Init();
    static bool IsAvailable();

    // Playback control
    static bool Play(const uint8_t* pcm_data, int length, int sample_rate, int bits, int channels);
    static void Stop();
    static void Pause();
    static void Resume();

    // Volume (0-100)
    static void SetMasterVolume(int volume);
    static int  GetMasterVolume();
    static void SetMuted(bool muted);
    static bool IsMuted();

    // Status
    static AudioState GetState();
    static AudioInfo  GetInfo();
    static int        GetDSPVersion();

    // Tone generation (for system sounds)
    static void Beep(int frequency, int duration_ms);
    static void PlayTone(int frequency, int duration_ms, int volume);
    static void PlayLoopTone(int frequency, int volume);  // Continuous loop
    static void GenerateBuffer(int frequency, int volume); // Fill DMA with tone

    // IRQ handler (called from interrupt)
    static void HandleIRQ();

    // Tick — maintains playback buffer refilling
    static void Tick();

private:
    static bool available;
    static bool muted;
    static int  master_volume;    // 0-100
    static int  dsp_version;
    static AudioState state;

    // DMA buffer — must be in low physical memory (<16MB) for ISA DMA.
    // Pointer to a fixed address (0x10000) in conventional memory below the
    // kernel load address (1MB).  The BSS lives above the 2 GB heap, far
    // beyond the 16 MB ISA DMA addressing limit, so a static array won't work.
    static uint8_t* dma_buffer;

    // Current playback
    static const uint8_t* pcm_source;
    static int   pcm_length;
    static int   pcm_offset;
    static int   current_rate;
    static int   current_bits;
    static int   current_channels;
    static bool  looping;

    // Internal
    static bool  ResetDSP();
    static void  WriteDSP(uint8_t cmd);
    static uint8_t ReadDSP();
    static void  SetMixerVolume(int vol);
    static void  ProgramDMA8(uint32_t addr, uint16_t length);
    static void  ProgramDMA16(uint32_t addr, uint16_t length);
    static void  StartPlayback();
    static void  FillDMABuffer();
};
