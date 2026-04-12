#pragma once
//  kurono os  -  task manager application (professional)

#define TM_MAX_PROCS 64

enum TMTab {
    TM_PROCESSES = 0,
    TM_PERFORMANCE = 1,
    TM_DETAILS = 2,
    TM_SERVICES = 3,
    TM_TAB_COUNT = 4,
};

enum TMSortCol {
    TM_SORT_NAME = 0,
    TM_SORT_PID  = 1,
    TM_SORT_CPU  = 2,
    TM_SORT_MEM  = 3,
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
    static void RenderStatusBar(int x, int y, int w);
    static void RenderActionMenu(int x, int y, int w, int h);
    static void SortProcesses();
    static void InitServices();
    static void KillProcess(int idx);
    static void RestartProcess(int idx);

    // action popup state
    static bool action_menu_open;
    static int  action_menu_row;       // process index that triggered it
    static int  action_menu_x;
    static int  action_menu_y;
};
