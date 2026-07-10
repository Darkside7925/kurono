#pragma once
//  kurono os - kcl self-test harness
//  runs a suite of kcl scripts headless at boot (gated by a cmdline token)
//  and logs PASS/FAIL per test to serial. covers every major language
//  feature plus the stdlib. (satoru)

class KCLTest {
public:
    // run the full suite; logs "KCL-TEST: <name> PASS|FAIL" lines and a final
    // "KCL-TEST: SUMMARY <pass>/<total>" line to serial. returns pass count. (satoru)
    static int RunAll();
};
// end (satoru)
