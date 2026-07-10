#pragma once
//  kurono os - 802.11i security self-test (satoru)
//  runs the wpa2 crypto core against published 802.11i / rfc test vectors at boot
//  (gated by the kurono.wifitest cmdline token) and logs PASS/FAIL per vector to
//  serial. these prove the security core is byte-correct even with no radio: if
//  these pass, the pmk/ptk/mic/ccmp math the supplicant relies on is right. (satoru)

class Ieee80211Test {
public:
    // run the full vector suite; logs "WIFI-TEST: <name> PASS|FAIL" lines and a
    // final "WIFI-TEST: SUMMARY <pass>/<total>" line to serial. returns the pass
    // count. (satoru)
    static int RunAll();
};
// end (satoru)
