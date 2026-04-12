#include "test_suite.h"
#include "../proc/scheduler.h"
#include "../fs/vfs.h"
#include "../kernel/memory_mgr.h"
#include "../system/input_manager.h"

int TestSuite::passed = 0;
int TestSuite::failed = 0;

void TestSuite::Run() {
    SerialLogger::Log("=== Starting System Test Suite ===\r\n");
    passed = 0; failed = 0;
    
    TestMemory();
    TestScheduler();
    TestVFS();
    TestInput();
    
    SerialLogger::Log("=== Test Suite Completed ===\r\n");
    // char buf[32]; snprintf... manual print needed
    if (failed == 0) {
        SerialLogger::Log("ALL TESTS PASSED\r\n");
    } else {
        SerialLogger::Log("SOME TESTS FAILED\r\n");
    }
}

void TestSuite::TestMemory() {
    SerialLogger::Log("Testing Memory Manager...\r\n");
    void* p1 = MemoryManager::AllocPage();
    Assert(p1 != nullptr, "AllocPage returned valid pointer");
    
    // simple write check
    if (p1) {
        *(volatile int*)p1 = 0x12345678;
        Assert(*(volatile int*)p1 == 0x12345678, "Memory write/read verification");
    }
}

void TestSuite::TestScheduler() {
    SerialLogger::Log("Testing Scheduler...\r\n");
    Assert(Scheduler::next_pid > 0, "PID counter initialized");
    
    Process* p = Scheduler::CreateProcess("TestProc", nullptr, 1);
    Assert(p != nullptr, "CreateProcess returned valid pointer");
    if (p) {
        Assert(p->pid > 0, "Process has valid PID");
        Assert(p->state == Process_Ready, "Process is in Ready state");
    }
    
    Assert(Scheduler::GetProcessCount() >= 1, "Scheduler has processes in queue");
}

void TestSuite::TestVFS() {
    SerialLogger::Log("Testing VFS...\r\n");
    Assert(VFS::root != nullptr, "VFS Root initialized");
    if (VFS::root) {
        Assert(VFS::root->type == FT_Directory, "Root is a directory");
    }
}

void TestSuite::TestInput() {
    SerialLogger::Log("Testing Input System...\r\n");
    
    // ensure inputmanager is ready (re-init is safe)
    InputManager::Init();
    
    // 1. register virtual device
    int dev2 = InputManager::RegisterDevice("Virtual Test Keyboard", DeviceType::Virtual);
    Assert(dev2 >= 0, "Registered Virtual Device");
    
    // 2. simulate simultaneous input
    // device 0 (ps2) presses 'a'
    InputManager::OnKeyDown(0, KEY_A);
    
    // device dev2 (virtual) presses 'b'
    InputManager::OnKeyDown(dev2, KEY_B);
    
    // 3. check log
    FileNode* f = VFS::Open("/input.log");
    Assert(f != nullptr, "Input Log file exists");
    if (f) {
        // read log content
        uint8_t buf[512];
        for(int i=0; i<512; i++) buf[i] = 0;
        VFS::Read(f, 0, 511, buf);
        
        SerialLogger::Log("Log Content: ");
        SerialLogger::Log((char*)buf);
        SerialLogger::Log("\r\n");
        
        // helper to search string in buffer
        auto contains = [](const char* haystack, const char* needle) -> bool {
            if (!haystack || !needle) return false;
            while(*haystack) {
                const char* h = haystack;
                const char* n = needle;
                while(*n && *h && *h == *n) { h++; n++; }
                if (!*n) return true;
                haystack++;
            }
            return false;
        };
        
        // "dev:0"
        bool found_dev0 = contains((char*)buf, "DEV:0");
        
        // "dev:x"
        char needle[16]; 
        needle[0]='D'; needle[1]='E'; needle[2]='V'; needle[3]=':'; 
        needle[4]='0'+dev2; needle[5]=0;
        
        bool found_dev_virt = contains((char*)buf, needle);
        
        Assert(found_dev0, "Log contains Device 0 input");
        Assert(found_dev_virt, "Log contains Virtual Device input");
    }
}
