#pragma once
//  kurono os  -  ac'97 audio codec driver
//  intel ich / sis / via ac97 audio controller (pci class 04:01)
//  supports pcm playback via bus master dma with buffer descriptor list
#include "../kernel/types.h"

#define AC97_INTEL_ICH      0x2415   // intel 82801aa (ich)
#define AC97_INTEL_ICH2     0x2425   // intel 82801ab (ich2)
#define AC97_INTEL_ICH3     0x2445   // intel 82801ba (ich3)
#define AC97_INTEL_ICH4     0x24C5   // intel 82801db (ich4)
#define AC97_INTEL_ICH5     0x24D5   // intel 82801eb (ich5)
#define AC97_INTEL_ICH6     0x266E   // intel 82801fb (ich6)
#define AC97_INTEL_ICH7     0x27DE   // intel 82801g  (ich7)
#define AC97_SIS_7012       0x7012   // sis 7012
#define AC97_VIA_686        0x3058   // via vt82c686
#define AC97_VIA_8233       0x3059   // via vt8233

#define AC97_RESET          0x00     // reset register
#define AC97_MASTER_VOL     0x02     // master volume
#define AC97_HEADPHONE_VOL  0x04     // headphone volume
#define AC97_MASTER_MONO    0x06     // mono volume
#define AC97_PC_BEEP        0x0A     // pc speaker volume
#define AC97_PHONE_VOL      0x0C     // phone input volume
#define AC97_MIC_VOL        0x0E     // microphone volume
#define AC97_LINE_IN_VOL    0x10     // line-in volume
#define AC97_CD_VOL         0x12     // cd volume
#define AC97_PCM_OUT_VOL    0x18     // pcm out / dac volume
#define AC97_RECORD_SELECT  0x1A     // record source select
#define AC97_RECORD_GAIN    0x1C     // record gain
#define AC97_GENERAL_PURPOSE 0x20    // general purpose
#define AC97_POWERDOWN      0x26     // powerdown ctrl/stat
#define AC97_EXT_AUDIO_ID   0x28     // extended audio id
#define AC97_EXT_AUDIO_CTRL 0x2A     // extended audio ctrl
#define AC97_PCM_FRONT_RATE 0x2C     // front dac sample rate
#define AC97_PCM_SURR_RATE  0x2E     // surround dac rate
#define AC97_PCM_LFE_RATE   0x30     // lfe dac rate
#define AC97_PCM_LR_ADC_RATE 0x32    // adc sample rate
#define AC97_VENDOR_ID1     0x7C     // vendor id (high)
#define AC97_VENDOR_ID2     0x7E     // vendor id (low)

#define AC97_BM_PCM_IN      0x00     // pcm in (adc)
#define AC97_BM_PCM_OUT     0x10     // pcm out (dac)
#define AC97_BM_MIC_IN      0x20     // microphone

// per-channel bus master registers (add channel base)
#define BM_BDBAR            0x00     // buffer descriptor base address
#define BM_CIV              0x04     // current index value (uint8)
#define BM_LVI              0x05     // last valid index (uint8)
#define BM_STATUS           0x06     // status (uint16)
#define BM_PICB             0x08     // position in current buffer (uint16)
#define BM_PIV              0x0A     // prefetched index value (uint8)
#define BM_CR               0x0B     // control register (uint8)

// bus master control bits
#define BM_CR_IOCE          0x10     // interrupt on completion enable
#define BM_CR_FEIE          0x08     // fifo error interrupt enable
#define BM_CR_LVBIE         0x04     // last valid buffer interrupt enable
#define BM_CR_RUN           0x01     // run / pause
#define BM_CR_RESET         0x02     // reset channel

// status bits
#define BM_STATUS_DCH       0x01     // dma controller halted
#define BM_STATUS_CELV      0x02     // current equals last valid
#define BM_STATUS_LVBCI     0x04     // last valid buffer completion
#define BM_STATUS_BCIS      0x08     // buffer completion (ioc)
#define BM_STATUS_FIFOE     0x10     // fifo error

#define AC97_MAX_BDL_ENTRIES 32      // max buffer descriptors in list

struct AC97BufferDescriptor {
    uint32_t buffer_addr;    // physical address of pcm data
    uint16_t length;         // number of samples (not bytes!)
    uint16_t flags;          // bit15=ioc (interrupt on completion), bit14=bup (buffer underrun policy)
} __attribute__((packed));

#define AC97_BD_IOC          (1 << 15)
#define AC97_BD_BUP          (1 << 14)

#define AC97_BUFFER_SIZE     32768    // 32kb per dma buffer
#define AC97_DEFAULT_RATE    48000    // ac97 native rate
#define AC97_IRQ             10       // typical ac97 irq

enum AC97State {
    AC97_STOPPED = 0,
    AC97_PLAYING,
    AC97_PAUSED,
};

struct AC97Info {
    bool     available;
    uint16_t controller_vendor;
    uint16_t controller_device;
    uint16_t codec_vendor_id1;
    uint16_t codec_vendor_id2;
    uint16_t mixer_base;        // bar0 i/o
    uint16_t bus_master_base;   // bar1 i/o
    int      sample_rate;
    int      channels;
    int      bits;
    bool     variable_rate;     // vra support
    AC97State state;
};

class AC97 {
public:
    // initialize  -  pci scan, codec reset, bdl setup
    static bool Init();
    static bool IsAvailable();

    // playback
    static bool Play(const uint8_t* pcm_data, int length, int sample_rate, int bits, int channels);
    static void Stop();
    static void Pause();
    static void Resume();

    // volume (0=max, 63=mute for ac97; we normalize to 0-100)
    static void SetMasterVolume(int volume);
    static int  GetMasterVolume();
    static void SetPCMVolume(int volume);
    static int  GetPCMVolume();
    static void SetMuted(bool muted);
    static bool IsMuted();

    // sample rate
    static bool SetSampleRate(int rate);
    static int  GetSampleRate();

    // status
    static AC97State GetState();
    static AC97Info  GetInfo();

    // irq handler
    static void HandleIRQ();

    // tick  -  refill buffers
    static void Tick();

    // streaming-mode helpers used by audio_backend_ac97.cpp.  The mixer
    // pushes one period at a time via WriteRingChunk(); EnsureStreaming
    // boots the DMA ring with silence on first call and never restarts it.
    static bool     EnsureStreaming(int sample_rate, int bits, int channels);
    static uint32_t WriteRingChunk(const void* data, uint32_t bytes);
    static uint32_t RingQueuedBytes();
    static uint32_t RingChunkBytes();

private:
    static AC97Info info;
    static bool     muted;
    static int      master_vol;

    // dma buffer and bdl  -  must be in low physical memory (<4gb)
    static uint8_t* dma_buffer;     // points to physical memory
    static AC97BufferDescriptor* bdl;  // buffer descriptor list

    // current playback
    static const uint8_t* pcm_source;
    static int   pcm_length;
    static int   pcm_offset;
    static bool  looping;

    // pci scan
    static bool ScanPCI();

    // mixer i/o
    static uint16_t MixerRead(uint8_t reg);
    static void     MixerWrite(uint8_t reg, uint16_t val);

    // bus master i/o
    static uint8_t  BMRead8(uint16_t offset);
    static uint16_t BMRead16(uint16_t offset);
    static uint32_t BMRead32(uint16_t offset);
    static void     BMWrite8(uint16_t offset, uint8_t val);
    static void     BMWrite16(uint16_t offset, uint16_t val);
    static void     BMWrite32(uint16_t offset, uint32_t val);

    // setup
    static void ResetCodec();
    static void SetupBDL();
    static void FillBuffer(int idx);
    static void StartDMA();
    static void StopDMA();
};
