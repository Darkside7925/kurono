#include "tpm.h"
#include "serial.h"

namespace {
    TPM::DeviceInfo g_info;

    // ---- MMIO helpers ----
    inline uint8_t  mmio_r8 (uint64_t addr) {
        volatile uint8_t* p = (volatile uint8_t*)addr;
        return *p;
    }
    inline uint32_t mmio_r32(uint64_t addr) {
        volatile uint32_t* p = (volatile uint32_t*)addr;
        return *p;
    }
    inline void     mmio_w8 (uint64_t addr, uint8_t v) {
        volatile uint8_t* p = (volatile uint8_t*)addr;
        *p = v;
    }
    inline void     mmio_w32(uint64_t addr, uint32_t v) {
        volatile uint32_t* p = (volatile uint32_t*)addr;
        *p = v;
    }

    // TIS register offsets within locality 0.
    constexpr uint32_t TIS_ACCESS  = 0x0000;
    constexpr uint32_t TIS_STS     = 0x0018;
    constexpr uint32_t TIS_DATA    = 0x0024;
    constexpr uint32_t TIS_DID_VID = 0x0F00;
    constexpr uint32_t TIS_RID     = 0x0F04;

    // TIS_ACCESS bits.
    constexpr uint8_t  ACCESS_VALID            = 0x80;
    constexpr uint8_t  ACCESS_ACTIVE_LOCALITY  = 0x20;
    constexpr uint8_t  ACCESS_REQUEST_USE      = 0x02;
    constexpr uint8_t  ACCESS_RELINQUISH       = 0x20;

    // TIS_STS bits.
    constexpr uint32_t STS_VALID               = 0x80;
    constexpr uint32_t STS_COMMAND_READY       = 0x40;
    constexpr uint32_t STS_GO                  = 0x20;
    constexpr uint32_t STS_DATA_AVAIL          = 0x10;
    constexpr uint32_t STS_EXPECT              = 0x08;

    bool g_initialized = false;

    void delay_short() {
        for (volatile int i = 0; i < 1000; i++) { __asm__ __volatile__("nop"); }
    }

    bool tis_request_locality0() {
        // Request locality 0 by writing REQUEST_USE.
        mmio_w8(TPM::TPM_TIS_BASE + TIS_ACCESS, ACCESS_REQUEST_USE);
        for (int i = 0; i < 1000; i++) {
            uint8_t a = mmio_r8(TPM::TPM_TIS_BASE + TIS_ACCESS);
            if ((a & (ACCESS_VALID | ACCESS_ACTIVE_LOCALITY)) ==
                (ACCESS_VALID | ACCESS_ACTIVE_LOCALITY)) return true;
            delay_short();
        }
        return false;
    }

    bool tis_present() {
        // DID_VID == 0xFFFFFFFF or 0 means no chip.
        uint32_t didvid = mmio_r32(TPM::TPM_TIS_BASE + TIS_DID_VID);
        return didvid != 0xFFFFFFFFu && didvid != 0;
    }

    uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
    uint32_t bswap32(uint32_t v) {
        return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
               ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
    }
}

namespace TPM {

const DeviceInfo* Info() { return &g_info; }

bool Init() {
    g_info = {};
    g_info.present = false;

    // Quickly probe DID_VID; on bare-metal without a TPM the address is
    // unmapped and the read may return 0xFF or trigger an MCE.  We rely
    // on the page being mapped read-only by the early MMIO setup.
    if (!tis_present()) {
        SerialLogger::Log("TPM: no chip detected at 0xFED40000\r\n");
        return false;
    }

    if (!tis_request_locality0()) {
        SerialLogger::Log("TPM: failed to acquire locality 0\r\n");
        return false;
    }

    uint32_t didvid = mmio_r32(TPM_TIS_BASE + TIS_DID_VID);
    g_info.vendor_id = (uint16_t)(didvid & 0xFFFF);
    g_info.device_id = (uint16_t)(didvid >> 16);
    g_info.revision  = mmio_r8(TPM_TIS_BASE + TIS_RID);

    // Issue TPM2_GetCapability(TPM_CAP_TPM_PROPERTIES, MANUFACTURER) to
    // pick up the four-character manufacturer code.  Real implementation
    // assembles the command packet and calls SubmitCommand.
    g_info.manufacturer = 0;        // filled in by SubmitCommand path
    g_info.firmware_v1  = 0;
    g_info.firmware_v2  = 0;
    g_info.v2_capable   = true;     // assume 2.0  -  we don't fall back to 1.2
    g_info.present      = true;
    g_initialized       = true;

    SerialLogger::Log("TPM: device present (vendor ");
    SerialLogger::LogHex(g_info.vendor_id);
    SerialLogger::Log(", revision ");
    SerialLogger::LogHex(g_info.revision);
    SerialLogger::Log(")\r\n");
    return true;
}

Result SubmitCommand(const unsigned char* cmd_buf, uint32_t cmd_len,
                     unsigned char* resp_buf, uint32_t* resp_len)
{
    if (!g_initialized || !g_info.present) return TPM_RC_NOT_PRESENT;
    if (!cmd_buf || cmd_len < 10 || cmd_len > TPM_MAX_CMD_SIZE) return TPM_RC_BAD_REQUEST;
    if (!resp_buf || !resp_len) return TPM_RC_BAD_REQUEST;

    // Wait for STS_COMMAND_READY.
    mmio_w32(TPM_TIS_BASE + TIS_STS, STS_COMMAND_READY);
    int t = 100000;
    while (t-- > 0) {
        uint32_t s = mmio_r32(TPM_TIS_BASE + TIS_STS);
        if (s & STS_COMMAND_READY) break;
        delay_short();
    }
    if (t <= 0) return TPM_RC_TIMEOUT;

    // Stream cmd into the FIFO.  We respect EXPECT but ignore burst
    // count optimisation for clarity.
    for (uint32_t i = 0; i < cmd_len; i++) {
        // Block until EXPECT=1 (chip wants more bytes) or chip is done.
        int w = 100000;
        while (w-- > 0) {
            uint32_t s = mmio_r32(TPM_TIS_BASE + TIS_STS);
            if (s & STS_EXPECT) break;
            delay_short();
        }
        if (w <= 0) return TPM_RC_TIMEOUT;
        mmio_w8(TPM_TIS_BASE + TIS_DATA, cmd_buf[i]);
    }

    // Kick off the chip.
    mmio_w32(TPM_TIS_BASE + TIS_STS, STS_GO);

    // Wait for STS_DATA_AVAIL.
    int wait = 1000000;
    while (wait-- > 0) {
        uint32_t s = mmio_r32(TPM_TIS_BASE + TIS_STS);
        if (s & STS_DATA_AVAIL) break;
        delay_short();
    }
    if (wait <= 0) return TPM_RC_TIMEOUT;

    // Drain response.
    uint32_t got = 0;
    while (got < *resp_len) {
        uint32_t s = mmio_r32(TPM_TIS_BASE + TIS_STS);
        if (!(s & STS_DATA_AVAIL)) break;
        resp_buf[got++] = mmio_r8(TPM_TIS_BASE + TIS_DATA);
    }
    *resp_len = got;

    // Release locality.
    mmio_w8(TPM_TIS_BASE + TIS_ACCESS, ACCESS_RELINQUISH);

    if (got < 10) return TPM_RC_FAILURE;
    // Header layout: tag(2) | size(4) | rc(4), big-endian.
    uint32_t rc = ((uint32_t)resp_buf[6] << 24) | ((uint32_t)resp_buf[7] << 16) |
                  ((uint32_t)resp_buf[8] << 8)  |  (uint32_t)resp_buf[9];
    return (Result)rc;
}

Result PcrRead(uint32_t pcr_index, Algorithm alg,
               unsigned char* digest_out, uint32_t* digest_len)
{
    if (!g_info.present) return TPM_RC_NOT_PRESENT;
    if (pcr_index >= 24) return TPM_RC_BAD_REQUEST;
    if (!digest_out || !digest_len) return TPM_RC_BAD_REQUEST;

    // TPM2_PCR_Read command.  Tag: TPM_ST_NO_SESSIONS = 0x8001.
    // Body: count=1, hashAlg, sizeofSelect=3, pcrSelect[3] (bitmap).
    unsigned char cmd[64];
    int p = 0;
    cmd[p++] = 0x80; cmd[p++] = 0x01;          // tag
    cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 20;  // size = 20
    cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0x01; cmd[p++] = 0x7E;  // TPM2_CC_PCR_Read = 0x17E
    // PCR selection structure
    cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 1;   // count = 1
    cmd[p++] = (unsigned char)(alg >> 8); cmd[p++] = (unsigned char)alg;
    cmd[p++] = 3;                              // sizeofSelect
    cmd[p++] = (pcr_index < 8)  ? (unsigned char)(1 << (pcr_index))      : 0;
    cmd[p++] = (pcr_index < 16) ? (unsigned char)(1 << (pcr_index - 8))  : 0;
    cmd[p++] = (pcr_index < 24) ? (unsigned char)(1 << (pcr_index - 16)) : 0;

    unsigned char resp[256];
    uint32_t      resp_len = sizeof(resp);
    Result rc = SubmitCommand(cmd, (uint32_t)p, resp, &resp_len);
    if (rc != TPM_RC_SUCCESS) return rc;

    // Response: header(10) | pcrUpdateCounter(4) | pcrSelectionOut(...)
    //         | digestList: count(4) | { size(2) | digest[size] } * count
    if (resp_len < 32) return TPM_RC_FAILURE;
    // We assume one digest, locate via offsets.  First skip header(10) and
    // pcrUpdateCounter(4) → 14.  Then skip selection: count(4) +
    // (alg(2)+size(1)+select(3)) = 4 + 6 = 10  → 24.  Then digestList
    // count(4) → 28.  Then size(2) at 28..29, digest at 30..
    uint16_t dsize = ((uint16_t)resp[28] << 8) | resp[29];
    if (dsize == 0 || dsize > 64 || resp_len < (uint32_t)(30 + dsize)) return TPM_RC_FAILURE;
    if (*digest_len < dsize) return TPM_RC_BAD_REQUEST;
    for (int i = 0; i < dsize; i++) digest_out[i] = resp[30 + i];
    *digest_len = dsize;
    return TPM_RC_SUCCESS;
}

Result PcrExtend(uint32_t pcr_index, Algorithm alg,
                 const unsigned char* digest_in, uint32_t digest_len)
{
    if (!g_info.present) return TPM_RC_NOT_PRESENT;
    if (pcr_index >= 24 || !digest_in) return TPM_RC_BAD_REQUEST;
    if (digest_len < 20 || digest_len > 64) return TPM_RC_BAD_REQUEST;

    unsigned char cmd[256];
    int p = 0;
    cmd[p++] = 0x80; cmd[p++] = 0x02;          // tag = TPM_ST_SESSIONS
    // size placeholder
    int size_off = p; p += 4;
    cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0x01; cmd[p++] = 0x82;   // CC_PCR_Extend
    // handle: pcr_index (4 bytes BE)
    cmd[p++] = 0; cmd[p++] = 0;
    cmd[p++] = (unsigned char)(pcr_index >> 8); cmd[p++] = (unsigned char)pcr_index;
    // authorization area: size + TPM_RS_PW session
    cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 9;
    cmd[p++] = 0x40; cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0x09;   // TPM_RS_PW
    cmd[p++] = 0; cmd[p++] = 0;                 // nonce length 0
    cmd[p++] = 0;                               // session attributes
    cmd[p++] = 0; cmd[p++] = 0;                 // hmac length 0
    // digest list: count=1, alg, digest
    cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 0; cmd[p++] = 1;
    cmd[p++] = (unsigned char)(alg >> 8); cmd[p++] = (unsigned char)alg;
    for (uint32_t i = 0; i < digest_len; i++) cmd[p++] = digest_in[i];

    uint32_t total = (uint32_t)p;
    cmd[size_off + 0] = (unsigned char)(total >> 24);
    cmd[size_off + 1] = (unsigned char)(total >> 16);
    cmd[size_off + 2] = (unsigned char)(total >> 8);
    cmd[size_off + 3] = (unsigned char)(total);

    unsigned char resp[64];
    uint32_t      resp_len = sizeof(resp);
    return SubmitCommand(cmd, total, resp, &resp_len);
}

Result GetRandom(unsigned char* out, uint32_t bytes) {
    if (!g_info.present) return TPM_RC_NOT_PRESENT;
    if (!out || bytes == 0 || bytes > 256) return TPM_RC_BAD_REQUEST;

    unsigned char cmd[12];
    cmd[0] = 0x80; cmd[1] = 0x01;
    cmd[2] = 0; cmd[3] = 0; cmd[4] = 0; cmd[5] = 12;
    cmd[6] = 0; cmd[7] = 0; cmd[8] = 0x01; cmd[9] = 0x7B;          // CC_GetRandom
    cmd[10] = (unsigned char)(bytes >> 8); cmd[11] = (unsigned char)bytes;

    unsigned char resp[300];
    uint32_t      resp_len = sizeof(resp);
    Result rc = SubmitCommand(cmd, 12, resp, &resp_len);
    if (rc != TPM_RC_SUCCESS) return rc;
    if (resp_len < 12) return TPM_RC_FAILURE;

    uint16_t got = ((uint16_t)resp[10] << 8) | resp[11];
    if (got > bytes) got = (uint16_t)bytes;
    if (resp_len < (uint32_t)(12 + got)) return TPM_RC_FAILURE;
    for (int i = 0; i < got; i++) out[i] = resp[12 + i];
    return TPM_RC_SUCCESS;
}

}  // namespace TPM
