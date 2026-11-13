// QEMU fw_cfg reader (I/O port interface). Spec: selector 0x510 is a 16-bit
// little-endian index, data 0x511 is an 8-bit stream, directory key 0x19.
// Directory entries are 64 bytes: size (be32), select (be16), reserved (be16),
// name[56]. (satoru)

#include "kld_fwcfg.h"
#include "io.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"

namespace {

constexpr uint16_t kSelPort = 0x510;
constexpr uint16_t kDataPort = 0x511;
constexpr uint16_t kFileDir = 0x0019;
constexpr char kConfName[] = "opt/kurono/wifi.conf";

uint8_t read_u8()
{
    return inb(kDataPort);
}

uint32_t read_be32()
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = (v << 8) | read_u8();
    return v;
}

uint16_t read_be16()
{
    uint16_t v = 0;
    for (int i = 0; i < 2; i++)
        v = (uint16_t)((v << 8) | read_u8());
    return v;
}

bool present()
{
    outw(kSelPort, 0x0000);   // FW_CFG_SIGNATURE
    char sig[4];
    for (int i = 0; i < 4; i++)
        sig[i] = (char)read_u8();
    return sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U';
}

void wipe(void *p, unsigned long n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--)
        *b++ = 0;
}


} // namespace

namespace KldFwcfg {

bool LoadWifiConf()
{
    if (!present()) {
        SerialLogger::Log("[kld-fwcfg] no QEMU fw_cfg signature\r\n");
        return false;
    }
    outw(kSelPort, kFileDir);
    uint32_t count = read_be32();
    if (count == 0 || count > 256) {
        SerialLogger::Log("[kld-fwcfg] wifi.conf not found\r\n");
        return false;
    }
    uint16_t select = 0;
    uint32_t size = 0;
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sz = read_be32();
        uint16_t sel = read_be16();
        (void)read_be16();
        char name[57];
        int n = 0;
        for (; n < 56; n++) {
            char c = (char)read_u8();
            name[n] = c;
            if (!c)
                break;
        }
        for (int j = n + 1; j < 56; j++)
            (void)read_u8();
        name[56] = 0;
        if (n < 56 && name[0] && !found) {
            const char *want = kConfName;
            const char *p = name;
            const char *q = want;
            while (*p && *q && *p == *q) { p++; q++; }
            if (*p == 0 && *q == 0) {
                select = sel;
                size = sz;
                found = true;
            }
        }
    }
    if (!found || size == 0 || size > 4096) {
        SerialLogger::Log("[kld-fwcfg] wifi.conf not found\r\n");
        return false;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    outw(kSelPort, select);
    for (got = 0; got < size; got++)
        buf[got] = read_u8();

    KVFS::Mkdirs("/system/run/kld");
    int wr = KVFS::WriteFile("/system/run/kld/wifi.conf", buf, got);
    wipe(buf, sizeof(buf));
    if (wr < 0) {
        SerialLogger::Log("[kld-fwcfg] write /system/run/kld/wifi.conf failed\r\n");
        return false;
    }
    SerialLogger::Log("[kld-fwcfg] wifi.conf ");
    SerialLogger::LogDec((int)got);
    SerialLogger::Log(" bytes\r\n");
    return true;
}

void RemoveWifiConf()
{
    KVFS::Unlink("/system/run/kld/wifi.conf");
}

} // namespace KldFwcfg
