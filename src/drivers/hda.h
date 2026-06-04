#pragma once
//  kurono os  -  intel hd audio (hda) controller driver
//  implements intel high definition audio specification for pcm playback
#include "../kernel/types.h"

#define HDA_GCAP        0x00  // global capabilities (16-bit)
#define HDA_VMIN        0x02  // minor version
#define HDA_VMAJ        0x03  // major version
#define HDA_OUTPAY      0x04  // output payload capability
#define HDA_INPAY       0x06  // input payload capability
#define HDA_GCTL        0x08  // global control
#define HDA_WAKEEN      0x0C  // wake enable
#define HDA_STATESTS    0x0E  // state change status
#define HDA_GSTS        0x10  // global status
#define HDA_INTCTL      0x20  // interrupt control
#define HDA_INTSTS      0x24  // interrupt status
#define HDA_WALCLK      0x30  // wall clock counter
#define HDA_CORBLBASE   0x40  // corb lower base address
#define HDA_CORBUBASE   0x44  // corb upper base address
#define HDA_CORBWP      0x48  // corb write pointer
#define HDA_CORBRP      0x4A  // corb read pointer
#define HDA_CORBCTL     0x4C  // corb control
#define HDA_CORBSTS     0x4D  // corb status
#define HDA_CORBSIZE    0x4E  // corb size
#define HDA_RIRBLBASE   0x50  // rirb lower base address
#define HDA_RIRBUBASE   0x54  // rirb upper base address
#define HDA_RIRBWP      0x58  // rirb write pointer
#define HDA_RINTCNT     0x5A  // response interrupt count
#define HDA_RIRBCTL     0x5C  // rirb control
#define HDA_RIRBSTS     0x5D  // rirb status
#define HDA_RIRBSIZE    0x5E  // rirb size

#define HDA_SD_CTL      0x00  // stream descriptor control (24-bit)
#define HDA_SD_STS      0x03  // stream descriptor status
#define HDA_SD_LPIB     0x04  // link position in buffer
#define HDA_SD_CBL      0x08  // cyclic buffer length
#define HDA_SD_LVI      0x0C  // last valid index
#define HDA_SD_FIFOS    0x10  // fifo size
#define HDA_SD_FMT      0x12  // stream format
#define HDA_SD_BDPL     0x18  // bdl lower address
#define HDA_SD_BDPU     0x1C  // bdl upper address

#define HDA_GCTL_CRST    (1 << 0)   // controller reset
#define HDA_GCTL_FCNTRL  (1 << 1)   // flush control
#define HDA_GCTL_UNSOL   (1 << 8)   // accept unsolicited response

#define HDA_SD_CTL_RUN    (1 << 1)   // stream run
#define HDA_SD_CTL_IOCE   (1 << 2)   // interrupt on completion enable
#define HDA_SD_CTL_FEIE   (1 << 3)   // fifo error interrupt enable
#define HDA_SD_CTL_DEIE   (1 << 4)   // descriptor error interrupt enable
#define HDA_SD_CTL_SRST   (1 << 0)   // stream reset

#define HDA_CORBCTL_RUN   (1 << 1)
#define HDA_RIRBCTL_RUN   (1 << 1)
#define HDA_RIRBCTL_INT   (1 << 0)

#define HDA_VERB(cad, nid, verb, payload) \
    (((uint32_t)(cad) << 28) | ((uint32_t)(nid) << 20) | ((uint32_t)(verb) << 8) | (payload))

#define HDA_VERB_GET_PARAM     0xF00
#define HDA_VERB_SET_STREAM    0x706
#define HDA_VERB_SET_FORMAT    0x200  // set_stream_format 2xxxx
#define HDA_VERB_SET_PINCTL    0x707
#define HDA_VERB_SET_EAPDBTL   0x70C
#define HDA_VERB_SET_POWER     0x705
#define HDA_VERB_SET_CONVCTRL  0x706
#define HDA_VERB_SET_AMP_GAIN  0x300  // 3xxxx
#define HDA_VERB_GET_CONNLIST  0xF02
#define HDA_VERB_GET_CONNSEL   0xF01
#define HDA_VERB_SET_CONNSEL   0x701

#define HDA_PARAM_VENDOR_ID    0x00
#define HDA_PARAM_NODE_COUNT   0x04
#define HDA_PARAM_FN_GROUP     0x05
#define HDA_PARAM_AUDIO_WIDGET 0x09
#define HDA_PARAM_PIN_CAP      0x0C
#define HDA_PARAM_CONN_LEN     0x0E
#define HDA_PARAM_AMP_OUT_CAP  0x12
#define HDA_PARAM_AMP_IN_CAP   0x0D

#define HDA_WIDGET_AUDIO_OUT    0x0
#define HDA_WIDGET_AUDIO_IN     0x1
#define HDA_WIDGET_AUDIO_MIX    0x2
#define HDA_WIDGET_AUDIO_SEL    0x3
#define HDA_WIDGET_PIN          0x4
#define HDA_WIDGET_POWER        0x5
#define HDA_WIDGET_VOLUME       0x6
#define HDA_WIDGET_BEEP         0x7
#define HDA_WIDGET_VENDOR       0xF

struct HDA_BDL_Entry {
    uint64_t address;
    uint32_t length;
    uint32_t ioc;      // interrupt on completion (bit 0)
} __attribute__((packed));

struct HDACodecNode {
    uint8_t  nid;
    uint8_t  type;        // widget type
    bool     has_amp_out;
    bool     has_amp_in;
    uint8_t  conn_count;
    uint8_t  connections[16];
};

struct HDAStreamFormat {
    uint32_t sample_rate;   // 44100, 48000, 96000, etc.
    uint8_t  bits;          // 16, 24, 32
    uint8_t  channels;      // 1=mono, 2=stereo
};

#define HDA_MAX_CODECS   4
#define HDA_MAX_NODES    32
#define HDA_BDL_ENTRIES  32
#define HDA_BUFFER_SIZE  (HDA_BDL_ENTRIES * 4096)  // 128kb

class HDAudio {
public:
    static bool Init();
    static bool IsDetected();

    // codec info
    static int  GetCodecCount();
    static uint32_t GetCodecVendor(int codec);

    // playback
    static bool SetFormat(uint32_t sample_rate, uint8_t bits, uint8_t channels);
    static bool Play(const void* pcm_data, uint32_t size);
    static bool Stop();
    static bool IsPlaying();

    // volume (0-255)
    static void SetVolume(uint8_t vol);
    static uint8_t GetVolume();

    // stream position
    static uint32_t GetPosition();
    static uint32_t GetBufferSize();

    static void DumpInfo(char* out, int max_len);

    // streaming-mode API used by audio_backend_hda.cpp.  StartStream()
    // arms the DMA engine on the shared buffer with silence; WriteRing()
    // copies a period into the next chunk and the controller picks it up
    // on the next BDL turn.  No per-call DMA restart.
    static bool     StartStream();
    static uint32_t WriteRing(const void* data, uint32_t bytes);
    static uint32_t RingQueuedBytes();
    static uint32_t RingChunkBytes();

private:
    static bool detected;
    static volatile uint8_t* bar0;

    // corb/rirb
    static uint32_t* corb;
    static uint64_t* rirb;
    static int corb_size;
    static int rirb_size;
    static int rirb_rp;

    // codec state
    static int codec_count;
    static uint32_t codec_vendors[HDA_MAX_CODECS];
    static int output_nid;     // output converter nid
    static int pin_nid;        // output pin nid
    static int codec_addr;     // active codec address

    // stream state
    static HDA_BDL_Entry* bdl;
    static void* dma_buffer;
    static bool playing;
    static uint8_t volume;
    static HDAStreamFormat current_format;

    // stream base offset
    static uint32_t stream_base;

    // register access
    static uint8_t  Read8(uint32_t offset);
    static uint16_t Read16(uint32_t offset);
    static uint32_t Read32(uint32_t offset);
    static void Write8(uint32_t offset, uint8_t val);
    static void Write16(uint32_t offset, uint16_t val);
    static void Write32(uint32_t offset, uint32_t val);

    // corb/rirb
    static bool InitCorbRirb();
    static bool SendVerb(uint32_t verb, uint32_t* response);
    static bool WaitRIRB(uint32_t* response, int timeout);

    // codec discovery
    static bool ProbeCodecs();
    static bool FindOutputPath(int cad);

    // stream setup
    static uint16_t EncodeFormat(uint32_t sample_rate, uint8_t bits, uint8_t channels);
    static bool SetupOutputStream();
};
