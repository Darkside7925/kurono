#pragma once
//  kurono os  -  task manager application (professional)

#include "../kernel/types.h"

#define TM_MAX_PROCS 64
#define TM_MAX_WINDOWS_ROWS 32

enum TMTab {
    TM_PROCESSES = 0,
    TM_PERFORMANCE = 1,
    TM_DETAILS = 2,
    TM_SERVICES = 3,
    TM_WINDOWS = 4,
    TM_TAB_COUNT = 5,
};

enum TMSortCol {
    TM_SORT_NAME = 0,
    TM_SORT_PID  = 1,
    TM_SORT_CPU  = 2,
    TM_SORT_MEM  = 3,
};

enum TMProcessSource {
    TM_PROC_SCHED = 0,
    TM_PROC_IDLE  = 1,
};

struct TMProcess {
    int  pid;
    char name[32];
    int  cpu_pct;     // 0-100
    int  mem_kb;
    int  threads;
    char state[12];   // running, sleeping, stopped, zombie
    char user[16];
    int  priority;
    int  io_read_kb;
    int  io_write_kb;
    uint32_t flags;
    uint8_t source_kind;
    // Adaptive kernel-stack telemetry from the preemptive scheduler.
    uint32_t stack_kb;
    uint32_t stack_cap_kb;
    uint32_t cpu_ms_total;
    uint32_t stack_grow_count;
    uint8_t  prio_tier;       // PRIO_REALTIME..PRIO_LOW
    bool     is_kernel_proc;
};

struct TMWindowRow {
    int  id;
    char title[64];
    char state[12];
    char session[16];
    int  x;
    int  y;
    int  w;
    int  h;
};

struct TMService {
    char name[32];
    char status[12]; // running, stopped
    char type[16];   // kernel, system, user
    int  pid;
};

class TaskManagerApp {
public:
    static void Init();
    static int  Open();

    static void Render(void* win, int x, int y, int w, int h);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

    static void RefreshProcesses();
    static void Tick();   // call each frame to update perf

private:
    static TMTab current_tab;
    static TMProcess procs[TM_MAX_PROCS];
    static int proc_count;
    static int selected_proc;
    static TMWindowRow window_rows[TM_MAX_WINDOWS_ROWS];
    static int window_count;
    static int selected_window;
    static int scroll_offset;
    static TMSortCol sort_col;
    static bool sort_asc;

    static TMService services[32];
    static int service_count;

    // real perf counters
    static int cpu_usage;
    static int cpu_cores;
    static int mem_used_kb;
    static int mem_total_kb;
    static int mem_cached_kb;
    static int uptime_sec;
    static int net_rx_kb;
    static int net_tx_kb;
    static int disk_read_kb;
    static int disk_write_kb;

    static int cpu_history[60];
    static int mem_history[60];
    static int hist_idx;
    static int tick_counter;

    static void RenderTabs(int x, int y, int w);
    static void RenderProcessList(int x, int y, int w, int h);
    static void RenderPerformance(int x, int y, int w, int h);
    static void RenderDetails(int x, int y, int w, int h);
    static void RenderServices(int x, int y, int w, int h);
    static void RenderWindows(int x, int y, int w, int h);
    static void RenderStatusBar(int x, int y, int w);
    static void RenderActionMenu(int x, int y, int w, int h);
    static void SortProcesses();
    static void InitServices();
    static void RefreshWindows();
    static void KillProcess(int idx);
    static void RestartProcess(int idx);
    static void KillWindow(int idx);
    static void RestartWindow(int idx);

    // action popup state
    static bool action_menu_open;
    static int  action_menu_row;       // process index that triggered it
    static TMTab action_menu_tab;
    static int  action_menu_x;
    static int  action_menu_y;
};
