#pragma once
#include "types.h"

// kurono.taskmem=1: per-task kernel memory accounting. every kernel heap block
// is charged to the task that allocated it (the cpu's current task, or the
// built-in app whose code is running, see AppScope) and the charge is dropped
// from that same owner when the block is freed, whoever frees it. owners live
// in a side table keyed by pid (struct Process is never touched); block owners
// live in a separate hash keyed by the payload address, so the heap's own block
// headers and free lists are unchanged. allocations in interrupt context,
// before the scheduler runs, or that do not fit in the tables are charged to
// "kernel" (untagged). everything runs under the heap's existing lock: no new
// lock, and when the flag is off the heap pays one predictable branch. (satoru)
namespace TaskMem {
    extern volatile bool g_on;          // read on the heap hot path (satoru)
    inline bool On() { return g_on; }

    void Request();                     // boot flag seen (kernel_main) (satoru)
    void RequestBench();                // kurono.taskmem.bench=1: alloc/free microbenchmark at Init (satoru)
    void Init();                        // after the heap is up; no-op unless requested (satoru)
    void StartPeriodicLog(uint32_t secs);   // kurono.taskmem.log=N: [taskmem] lines every N s (satoru)
    void LogTop(const char* why);       // one [taskmem] report on serial (satoru)

    // heap hooks, called by KernelHeap with its lock held (satoru)
    void OnHeapAlloc(void* payload, uint64_t bytes, uint32_t cpu);
    void OnHeapFree(void* payload, uint64_t bytes);

    // interrupt marks (hal.cpp, legacy irq + msi blocks): an allocation whose
    // stack pointer lies just below the marked frame is interrupt work (satoru)
    uint64_t IrqEnter(const void* frame, uint32_t* cpu_out);   // returns the previous mark for IrqLeave (satoru)
    void     IrqLeave(uint32_t cpu, uint64_t prev);

    // pmm blocks a task takes directly for itself (back buffers, caches).
    // charged to the current owner, released from the original one (satoru)
    void ChargeFrames(const void* block, uint64_t bytes);
    void UnchargeFrames(const void* block);

    // built-in apps live inside the desktop / window manager task. while one is
    // set, the current task's allocations are charged to the app instead. the
    // scope only applies to the task that set it (a switch mid-scope cannot
    // leak it to another task on the cpu). app 0 = none. (satoru)
    struct AppScope {
        explicit AppScope(uint32_t app);
        ~AppScope();
        uint32_t cpu, prev_app, prev_pid;
        bool     armed;
    };
    // app ids: fixed table, name per id (satoru)
    enum : uint32_t {
        APP_NONE = 0, APP_TERMINAL, APP_FILES, APP_EDITOR, APP_SETTINGS, APP_TASKMGR,
        APP_CALC, APP_MEDIA, APP_BROWSER, APP_ALPINE, APP_SYSSETTINGS, APP_OTHER,
        APP_FILECACHE,   // kvfs file contents: shared data, like linux's page cache, not the writer's (satoru)
        APP_COUNT
    };
    const char* AppName(uint32_t app);
    // the gui task pumps the desktop from other tasks' syscalls too (firefox
    // drives KuronoShell::PumpUI on the bsp). a UiScope there charges that
    // work to the gui task instead of the task whose syscall it rides. (satoru)
    void SetUiTask();                   // called by the gui task itself (satoru)
    struct UiScope {
        UiScope();
        ~UiScope();
        uint32_t cpu, prev_key, prev_pid;
        bool     armed;
    };
    // window -> app, so render / input callbacks of a window run in its app's
    // scope. CreateWindow records the scope that is active when it runs. (satoru)
    void     BindWindow(int window_id);
    void     UnbindWindow(int window_id);
    uint32_t WindowApp(int window_id);
    struct WindowScope {
        explicit WindowScope(int window_id);
        AppScope s;
    };

    // read side (task manager). copies the live owner records under the heap
    // lock. key: pid for a task, APP_KEY | app for a built-in app. (satoru)
    static constexpr uint32_t APP_KEY = 0x80000000u;
    struct Row {
        uint32_t key;
        uint32_t pid;          // task that allocated (apps: the host task) (satoru)
        char     name[24];
        uint64_t heap_bytes;
        uint64_t heap_count;
        uint64_t frame_bytes;
        uint64_t peak_bytes;
    };
    struct Totals {
        uint64_t heap_live;      // every heap byte charged since enable + the baseline (satoru)
        uint64_t kernel_bytes;   // untagged: boot, irq, no task, table overflow (satoru)
        uint64_t kernel_frames;  // ChargeFrames with no task owner (satoru)
        uint32_t records, tags, tag_cap, tag_max_dist;
        uint64_t overflow, stale, sweeps;
    };
    int Snapshot(Row* out, int max, Totals* t);
    // one task's charge, copied under the heap lock; false when it holds none. (satoru)
    bool GetForPid(uint32_t pid, Row* out);
    // table-wide totals (heap_live / kernel_bytes / kernel_frames). (satoru)
    void GetTotals(Totals* t);
}

// end (satoru)
