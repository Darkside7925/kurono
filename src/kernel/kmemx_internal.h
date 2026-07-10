#pragma once
#include "types.h"

//  KMemX internal test hooks - NOT part of the public engine api. these reach
//  the locked store/retrieve/free primitives so the headless self-test can
//  round-trip pages through the REAL pool + metadata table without exposing
//  those primitives to the rest of the kernel. (satoru)

namespace KMemX {

// compress + store `src` (4kb) keyed (as,vaddr) in the pool. returns the slot
// index or -1 (table/pool full). (satoru)
int  TestStore(uint64_t as, uint64_t vaddr, const uint8_t* src);

// retrieve + crc-verify the page keyed (as,vaddr) into `dst` (4kb). false if
// missing OR crc mismatch. (satoru)
bool TestRetrieve(uint64_t as, uint64_t vaddr, uint8_t* dst);

// free the pool extent + metadata for (as,vaddr). (satoru)
void TestFree(uint64_t as, uint64_t vaddr);

// number of live compressed entries. (satoru)
uint32_t TestMetaLive();

}  // namespace KMemX

// end (satoru)
