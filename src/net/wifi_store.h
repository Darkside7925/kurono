// wifi_store: the saved wi-fi networks, encrypted at rest. (satoru)
//
// file: KVFS /etc/kurono/wifi.kws (persisted with /etc by PersistStore). the
// network name, security and flags are stored in clear but authenticated; the
// passphrase is aes-128-ctr encrypted with a fresh nonce per save and every
// entry carries an hmac-sha256 tag. the key is derived per machine:
// pbkdf2-hmac-sha256(sha-256("kurono-wifi-v1" | cpuid vendor, signature and
// brand | nvme serial + model), file salt, 20000) -> 16 byte aes key + 32 byte
// mac key. a key check value in the header tells a foreign machine (or a
// changed system disk) apart from a damaged file; such a file is kept, never
// overwritten, and the networks read as absent.
//
// what this protects: a copy of the persisted volume or the KURONO partition
// taken to another machine reveals no passphrase. what it does not: someone
// who can read the file AND knows this machine's cpu model and nvme serial can
// derive the key. sealing the key to the tpm (drivers/tpm.cpp) is the next step.
//
// passphrases are never logged, never written in clear to any persistent
// store, and wiped from every buffer after use. the only clear copy outside
// this module is the RAM-only handoff file for the KLD wi-fi container
// (HandoffToKld), which lives in KVFS /system/run/kld and is removed again by
// Tick() after kHandoffMs or as soon as the container reports a result. (satoru)
#pragma once
#include "../kernel/types.h"

namespace WifiStore {

static const int MAX_NETWORKS = 16;
static const int MAX_SSID = 32;
static const int MAX_PSK = 63;        // wpa passphrase (8..63) (satoru)
static const uint32_t kHandoffMs = 30000;

enum Flags : uint8_t { F_AUTOJOIN = 1, F_HIDDEN = 2 };

struct Info {
    char     ssid[MAX_SSID + 1];
    uint8_t  security;    // WiFiSecurity (satoru)
    uint8_t  flags;
    uint32_t last_used;   // a counter, larger = more recent (satoru)
};

// load the file (once, lazily; cheap after). (satoru)
void Init();
// true when the file exists but was made on another machine / was damaged. (satoru)
bool Foreign();

int  Count();
bool Get(int i, Info* out);
bool Has(const char* ssid);
// add or replace. psk may be "" for an open network. persists. (satoru)
bool Save(const char* ssid, const char* psk, uint8_t security);
bool Forget(const char* ssid);
bool SetAutoJoin(const char* ssid, bool on);
void Touch(const char* ssid);          // mark as just used (auto-join order) (satoru)
// kernel only: decrypt the passphrase into out (wiped by the caller). (satoru)
bool Passphrase(const char* ssid, char* out, int cap);
// the saved auto-join network to try first: one present in `visible` (may be
// null = any), most recently used. returns false when none. (satoru)
bool BestAutoJoin(const char* const* visible, int nvisible, char* ssid_out);

// ---- the KLD wi-fi container handoff (see the contract in wifi_store.cpp) ----
// write the RAM-only /system/run/kld/wifi.conf (SSID=, PSK= lines) for `ssid`
// (null = BestAutoJoin over any network). logs only the byte count. (satoru)
bool HandoffToKld(const char* ssid);
// remove the handoff file now (idempotent). (satoru)
void ClearHandoff();
// broker-side alternative to the file: one-shot copy of the pending
// credential into a caller buffer, for a broker op the container calls. the
// caller must have checked that `pid` is the registered wi-fi container.
// returns bytes written ("SSID=..\nPSK=..\n"), 0 when nothing is pending. the
// pending copy is wiped after one take. (satoru)
int  TakeForContainer(uint32_t pid, char* out, int cap);
// expire a stale handoff file. called from the network process loop. (satoru)
void Tick();

// offline self-test (kurono.platformtest=1): round trip, tamper, forget. it
// runs on a private in-memory table and never touches the real file. (satoru)
void SelfTest();

}  // namespace WifiStore

// end (satoru)
