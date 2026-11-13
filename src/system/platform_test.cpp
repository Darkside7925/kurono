// platform self-test, gated by kurono.platformtest=1: the crypto known
// answers, the password hash round trip (new and legacy forms) and the other
// platform pieces' offline checks. prints "[ptest] <name> PASS|FAIL" lines and
// a final "[ptest] done fails=N". never touches real accounts or disks. (satoru)
#include "platform_test.h"
#include "user_mgmt.h"
#include "../security/kcrypto.h"
#include "../net/wifi_store.h"
#include "../net/network.h"
#include "../fs/kvfs.h"
#include "key_layouts.h"
#include "ui_config.h"
#include "audio_devices.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"

namespace {
int g_fails = 0;
void verdict(const char* name, bool ok) {
    SerialLogger::Log("[ptest] ");
    SerialLogger::Log(name);
    SerialLogger::Log(ok ? " PASS\r\n" : " FAIL\r\n");
    if (!ok) g_fails++;
}
}  // namespace

namespace {
// KLD bridge (only when the container owns the radio, kurono.kld=iwlwifi): a
// sample wifi_scan file must come back as a sorted, de-duplicated list. the
// file is left in place so the picker can be looked at. (satoru)
void kld_bridge_test() {
    if (!WiFi::KldBridge()) return;
    const char* sample =
        "Cafe\t-80\t0\t6\t02:00:00:00:00:01\n"
        "HomeNet\t-48\t3\t36\t02:00:00:00:00:02\n"
        "Office\t-67\t4\t11\t02:00:00:00:00:03\n"
        "HomeNet\t-60\t3\t1\t02:00:00:00:00:04\n";
    KVFS::Mkdirs("/system/run/kld");
    int n = 0; while (sample[n]) n++;
    KVFS::WriteFile("/system/run/kld/wifi_scan", sample, (uint32_t)n);
    WiFi::Enable();
    WiFi::Scan();
    WiFiNetwork* w = WiFi::GetNetworks();
    bool ok = WiFi::GetNetworkCount() == 3 && w &&
              w[0].signal_strength == -48 && w[1].signal_strength == -67 && w[2].security == WIFI_OPEN;
    PlatformTest::Register("wifi-kld-scan-parse", ok);
}
// keyboard layouts: the de table and keymap, then back to the saved choice. (satoru)
void layout_test() {
    char keep[16];
    const char* cur = UIConfig::Str("kbd.layout", "us");
    int k = 0; while (cur[k] && k < 15) { keep[k] = cur[k]; k++; } keep[k] = 0;
    UIConfig::Set("kbd.layout", "de", false);
    bool ok = KeyLayouts::MapChar(KEY_Y, false, false, false) == 'z' &&
              KeyLayouts::MapChar(KEY_Z, true, false, false) == 'Y' &&
              KeyLayouts::MapChar(KEY_Q, false, false, true) == '@' &&
              KeyLayouts::MapChar(KEY_7, true, false, false) == '/' &&
              KeyLayouts::MapChar(KEY_SEMICOLON, false, false, false) == 0;
    uint32_t len = 0;
    const uint8_t* km = KeyLayouts::WaylandKeymap(&len);
    bool okk = km && len > 1000 && km[0] == 'x' && km[1] == 'k' && km[2] == 'b' && km[len - 1] == 0 &&
               ((uintptr_t)km & 0xFFF) == 0;
    UIConfig::Set("kbd.layout", "us", false);
    bool us_ok = KeyLayouts::MapChar(KEY_Y, false, false, false) == -1;
    UIConfig::Set("kbd.layout", keep, false);
    PlatformTest::Register("kbd-layout-de-table", ok);
    PlatformTest::Register("kbd-layout-de-keymap", okk);
    PlatformTest::Register("kbd-layout-us-passthrough", us_ok);
    SerialLogger::Log("[ptest] audio outputs: ");
    SerialLogger::LogDec(AudioDevices::OutputCount());
    SerialLogger::Log("\r\n");
}
}  // namespace

namespace PlatformTest {

// add one line per module self-test. (satoru)
TestFn kExtra[] = { WifiStore::SelfTest, kld_bridge_test, layout_test };
const int kExtraCount = (int)(sizeof(kExtra) / sizeof(kExtra[0]));

void Register(const char* name, bool ok) { verdict(name, ok); }

int Run() {
    g_fails = 0;
    verdict("kcrypto-vectors", KCrypto::SelfTest());

    char salt[24];
    UserManager::GenerateSalt(salt);
    char h[80];
    uint32_t t0 = Timer::GetRealMs();
    UserManager::HashPassword(salt, "correct horse", h);
    uint32_t t1 = Timer::GetRealMs();
    SerialLogger::Log("[ptest] pbkdf2 hash took ");
    SerialLogger::LogDec((int)(t1 - t0));
    SerialLogger::Log(" ms\r\n");
    verdict("pw-format", h[0] == 'p' && h[1] == '2' && h[2] == '$' && !UserManager::IsLegacyHash(h));
    verdict("pw-verify-good", UserManager::VerifyPassword(salt, "correct horse", h));
    verdict("pw-verify-bad", !UserManager::VerifyPassword(salt, "correct horsf", h));
    verdict("pw-verify-empty", !UserManager::VerifyPassword(salt, "", h));
    // legacy: hex sha-256(salt || password). (satoru)
    {
        KCrypto::Sha256 c; KCrypto::Sha256Init(&c);
        int sl = 0; while (salt[sl]) sl++;
        KCrypto::Sha256Update(&c, salt, (uint32_t)sl);
        KCrypto::Sha256Update(&c, "old pw", 6);
        uint8_t d[32]; KCrypto::Sha256Final(&c, d);
        char legacy[80]; KCrypto::ToHex(d, 32, legacy);
        verdict("pw-legacy-detect", UserManager::IsLegacyHash(legacy));
        verdict("pw-legacy-verify", UserManager::VerifyPassword(salt, "old pw", legacy));
        verdict("pw-legacy-bad", !UserManager::VerifyPassword(salt, "old pX", legacy));
    }
    for (int i = 0; i < kExtraCount; i++) if (kExtra[i]) kExtra[i]();
    SerialLogger::Log("[ptest] done fails=");
    SerialLogger::LogDec(g_fails);
    SerialLogger::Log("\r\n");
    return g_fails;
}

}  // namespace PlatformTest

// end (satoru)
