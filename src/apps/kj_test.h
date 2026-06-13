#pragma once
//  kurono os  -  kj self-test harness
//  runs a suite of kj (kurono javascript) scripts headless at boot (gated by the
//  cmdline token kurono.kjtest) and logs PASS/FAIL per test to serial, so a
//  headless qemu run can be polled for correctness. covers the language subset
//  plus the kss host bindings. (satoru)

class KJTest {
public:
    // run the full suite; logs "KJ-TEST: <name> PASS|FAIL" lines and a final
    // "KJ-TEST: SUMMARY <pass>/<total>" line to serial. returns pass count. (satoru)
    static int RunAll();
};
// end (satoru)
