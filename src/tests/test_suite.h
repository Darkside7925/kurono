#pragma once
#include "../kernel/types.h"
#include "../drivers/serial.h"

class TestSuite {
public:
    static int passed;
    static int failed;
    
    static void Run();
    
    static void Assert(bool condition, const char* msg) {
        if (condition) {
            passed++;
            SerialLogger::Log("[PASS] ");
        } else {
            failed++;
            SerialLogger::Log("[FAIL] ");
        }
        SerialLogger::Log(msg);
        SerialLogger::Log("\r\n");
    }
    
    // individual module tests
    static void TestScheduler();
    static void TestVFS();
    static void TestMemory();
    static void TestInput(); // added input test
};
