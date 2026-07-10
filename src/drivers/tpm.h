#ifndef KURONO_DRIVERS_TPM_H
#define KURONO_DRIVERS_TPM_H

#include "../kernel/types.h"

// TPM 2.0 driver - talks to the TPM via the TIS (TPM Interface
// Specification) MMIO interface at 0xFED40000.
//
// TIS register map (per locality, 0x1000 bytes apart):
//   0x000  ACCESS         R/W
//   0x008  INT_ENABLE     R/W
//   0x010  INT_VECTOR     R
//   0x014  INT_STATUS     R/W1C
//   0x018  INTF_CAPS      R
//   0x024  STS            R/W
//   0x024-0x028 BURST_COUNT (within STS)
//   0x024  DATA_FIFO      R/W   (offset 0x80 in 32-bit form)
//   0xF00  DID_VID        R     (vendor / device id)
//   0xF04  RID            R
//
// We probe at boot, latch the vendor id, and expose:
//   - PCR read/extend (the bread-and-butter for measured boot)
//   - GetRandom (well-tested entropy source)
//   - Capability query (manufacturer, firmware version)
//
// All other commands are accepted opaquely via SubmitCommand so that
// userspace tpm2-tools can drive the chip directly through /dev/tpm0.

namespace TPM {

    static const uint64_t TPM_TIS_BASE     = 0xFED40000ULL;
    static const uint16_t TPM_MAX_CMD_SIZE = 4096;

    enum Result : uint32_t {
        TPM_RC_SUCCESS         = 0x000,
        TPM_RC_FAILURE         = 0x101,
        TPM_RC_NOT_PRESENT     = 0x901,
        TPM_RC_TIMEOUT         = 0x902,
        TPM_RC_BAD_REQUEST     = 0x903,
    };

    enum Algorithm : uint16_t {
        TPM_ALG_SHA1   = 0x0004,
        TPM_ALG_SHA256 = 0x000B,
        TPM_ALG_SHA384 = 0x000C,
        TPM_ALG_SHA512 = 0x000D,
    };

    struct DeviceInfo {
        bool     present;
        uint16_t vendor_id;
        uint16_t device_id;
        uint8_t  revision;
        uint32_t manufacturer;     // ASCII
        uint32_t firmware_v1;
        uint32_t firmware_v2;
        bool     v2_capable;       // false ⇒ TPM 1.2
    };

    // Probe MMIO interface, request locality 0, fill DeviceInfo.  Safe
    // to call even when no TPM is present - sets present=false.
    bool   Init();
    const  DeviceInfo* Info();

    // Raw command interface.  cmd_buf must contain a valid TPM2_*
    // header per TCG spec; resp_buf receives the response.  Returns the
    // response code (TPM_RC_SUCCESS on success).
    Result SubmitCommand(const unsigned char* cmd_buf, uint32_t cmd_len,
                         unsigned char* resp_buf, uint32_t* resp_len);

    // PCR ops (high-level helpers).
    Result PcrRead(uint32_t pcr_index, Algorithm alg,
                   unsigned char* digest_out, uint32_t* digest_len);
    Result PcrExtend(uint32_t pcr_index, Algorithm alg,
                     const unsigned char* digest_in, uint32_t digest_len);

    // RNG: pull `bytes` of entropy from the TPM into `out`.
    Result GetRandom(unsigned char* out, uint32_t bytes);
}

#endif
