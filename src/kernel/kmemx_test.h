#pragma once
#include "types.h"

//  kmemx self-test harness - runs headless at boot (gated by kurono.kmemxtest)
//  and logs PASS/FAIL per scenario to serial so a ci run can scrape it. (satoru)
//
//  stage 1 gate: a byte-exact lz4 roundtrip over 1000 synthetic 4 kb pages of
//  mixed content (zero pages, repetitive text, structured records, pseudo-random
//  incompressible data), each crc32-verified in and out. nothing builds on the
//  engine until this passes. later stages add pool, dedup, and pressure checks
//  via the same RunAll() entry. (satoru)

namespace KMemXTest {

//  run every registered self-test; log "KMEMX-TEST: <name> PASS|FAIL ..." lines
//  and a final "KMEMX-TEST: SUMMARY <pass>/<total>" to serial. returns the pass
//  count. safe to call once the heap + pmm are up. (satoru)
int RunAll();

}  // namespace KMemXTest

// end (satoru)
