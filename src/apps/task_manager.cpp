//  kurono os  -  task manager (professional redesign)
#include "task_manager.h"
#include "browser.h"
#include "media_player.h"
#include "../ui/desktop.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
#include "../kernel/userspace.h"
#include "../linux/linux_drivers.h"
#include "../linux/linux_syscall.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../proc/scheduler.h"
#include "../fs/kvfs.h"
#include "../net/network.h"
#include "../system/logging.h"

static const unsigned int TM_BG       = 0xFF0C0C14;
static const unsigned int TM_PANEL    = 0xFF12121E;
static const unsigned int TM_TAB_BG   = 0xFF0C0C14;
static const unsigned int TM_TAB_SEL  = 0xFF1A1A30;
static const unsigned int TM_TAB_TXT  = 0xFF808090;
static const unsigned int TM_TAB_ACT  = 0xFFE0E0F0;
static const unsigned int TM_HEAD_BG  = 0xFF161626;
static const unsigned int TM_ROW_ALT  = 0xFF0F0F1C;
static const unsigned int TM_SEL_BG   = 0xFF1E3A5F;
static const unsigned int TM_TEXT     = 0xFFCCCCD8;
static const unsigned int TM_DIM      = 0xFF606072;
static const unsigned int TM_BLUE     = 0xFF3B82F6;
static const unsigned int TM_GREEN    = 0xFF22C55E;
static const unsigned int TM_RED      = 0xFFEF4444;
static const unsigned int TM_YELLOW   = 0xFFEAB308;
static const unsigned int TM_ORANGE   = 0xFFF97316;
static const unsigned int TM_PURPLE   = 0xFF8B5CF6;
static const unsigned int TM_CYAN     = 0xFF06B6D4;
static const unsigned int TM_GRAPH_BG = 0xFF0A0A16;
static const unsigned int TM_GRAPH_GD = 0xFF151525;
static const unsigned int TM_BORDER   = 0xFF252540;
static const unsigned int TM_WHITE    = 0xFFFFFFFF;
static const unsigned int TM_STATUS   = 0xFF0A0A12;
static constexpr const char TM_IDLE_NAME[] = "CPU Idle";

static const int ROW_H = 20;
static const int TAB_H = 32;

static int slen(const char* s){int n=0;if(s)while(s[n])n++;return n;}
static void scpy(char* d,const char* s,int mx){
    int i=0;if(s)while(s[i]&&i<mx-1){d[i]=s[i];i++;}d[i]=0;}
static void sapp(char* d,const char* s,int mx){
    int n=slen(d),i=0;if(s)while(s[i]&&n<mx-1){d[n++]=s[i++];}d[n]=0;}
static void int_to_str(int v,char*b,int mx){
    if(mx<2){b[0]=0;return;}if(v<0){b[0]='-';int_to_str(-v,b+1,mx-1);return;}
    char t[16];int n=0;do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}
static int scmp(const char*a,const char*b){
    while(*a&&*b){if(*a!=*b)return *a-*b;a++;b++;}return *a-*b;
}
static bool tm_starts_with(const char* s,const char* prefix){
    if(!s||!prefix) return false;
    int i=0; while(prefix[i]){ if(s[i]!=prefix[i]) return false; i++; }
    return true;
}
static const LinuxDriver* tm_find_bound_driver_by_prefixes(const char* const* prefixes,int count){
    LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
    int driver_count = LinuxDriverFramework::GetDriverCount();
    for(int i=0;i<driver_count;i++){
        if(!(drivers[i].bound || drivers[i].state == LDRV_ACTIVE)) continue;
        for(int p=0;p<count;p++){
            if(tm_starts_with(drivers[i].name, prefixes[p])) return &drivers[i];
        }
    }
    return nullptr;
}
static const LinuxDriver* tm_find_wifi_driver(){
    static const char* prefixes[] = {
        "wifi_", "iwl", "ath", "rtw", "rtl", "brcm", "mt76", "cfg80211", "mac80211"
    };
    return tm_find_bound_driver_by_prefixes(prefixes, (int)(sizeof(prefixes)/sizeof(prefixes[0])));
}
static const LinuxDriver* tm_find_bt_driver(){
    static const char* prefixes[] = {
        "bluetooth_", "bluetooth", "bt", "hci"
    };
    return tm_find_bound_driver_by_prefixes(prefixes, (int)(sizeof(prefixes)/sizeof(prefixes[0])));
}
static void tm_draw_device_pane(int x,int y,int w,int h,const char* title,unsigned int accent,
                                const char* line1,const char* line2){
    Graphics::FillRoundedRect(x, y, w, h, 6, TM_GRAPH_BG);
    Graphics::DrawString(x+8, y+6, title, accent, 0xFF000000);
    Graphics::DrawString(x+8, y+20, line1, TM_TEXT, 0xFF000000);
    Graphics::DrawString(x+8, y+34, line2, TM_DIM, 0xFF000000);
}

static int tm_max(int a,int b){ return a > b ? a : b; }
static int tm_clamp(int v,int lo,int hi){ return v < lo ? lo : (v > hi ? hi : v); }

struct TMCpuSample {
    uint32_t pid;
    uint64_t cpu_ticks_total;
};

static TMCpuSample g_tm_cpu_samples[TM_MAX_PROCS];
static int g_tm_cpu_sample_count = 0;
static bool g_tm_cpu_samples_ready = false;

static const char* tm_gpu_role_name(GpuRole role){
    switch(role){
        case GPU_ROLE_PRIMARY: return "P";
        case GPU_ROLE_SECONDARY: return "S";
        case GPU_ROLE_VIRTUAL: return "V";
        default: return "?";
    }
}

static void tm_append_gpu_short(char* out,int mx,const GpuInfo& gpu){
    sapp(out, tm_gpu_role_name(gpu.role), mx);
    sapp(out, ":", mx);
    sapp(out, gpu.desc, mx);
}

static void tm_format_gpu_pane(const GpuProbeResult& gpr,char* line1,int mx1,char* line2,int mx2){
    line1[0]=0; line2[0]=0;
    if(gpr.count <= 0){
        scpy(line1, "No GPU detected", mx1);
        scpy(line2, "Topology: none", mx2);
        return;
    }

    if(gpr.count == 1){
        scpy(line1, gpr.gpus[0].desc, mx1);
        scpy(line2, "Topology: Single GPU", mx2);
        return;
    }

    char nbuf[8]={0};
    int_to_str(gpr.count, nbuf, 8);
    sapp(line1, nbuf, mx1);
    sapp(line1, " GPUs | ", mx1);
    if(gpr.primary_idx >= 0 && gpr.primary_idx < gpr.count) tm_append_gpu_short(line1, mx1, gpr.gpus[gpr.primary_idx]);
    else tm_append_gpu_short(line1, mx1, gpr.gpus[0]);

    scpy(line2, "Others: ", mx2);
    bool first = true;
    for(int i=0;i<gpr.count;i++){
        if(i == gpr.primary_idx) continue;
        if(!first) sapp(line2, " | ", mx2);
        tm_append_gpu_short(line2, mx2, gpr.gpus[i]);
        first = false;
    }
    if(first){
        scpy(line2, "Topology: Multi-GPU", mx2);
    }
}

static int tm_find_cpu_sample(uint32_t pid){
    for(int i=0;i<g_tm_cpu_sample_count;i++){
        if(g_tm_cpu_samples[i].pid == pid) return i;
    }
    return -1;
}

static int tm_cpu_pct_from_ticks(uint64_t delta_ticks,uint32_t elapsed_ms){
    if(elapsed_ms == 0) return 0;
    uint64_t pct = (delta_ticks * 100ULL + (uint64_t)(elapsed_ms / 2)) / (uint64_t)elapsed_ms;
    if(pct > 100ULL) pct = 100ULL;
    return (int)pct;
}

static void tm_store_cpu_samples(const SchedulerProcessSnapshot* snapshots,int count){
    if(!snapshots || count <= 0){
        g_tm_cpu_sample_count = 0;
        return;
    }

    if(count > TM_MAX_PROCS) count = TM_MAX_PROCS;
    for(int i=0;i<count;i++){
        g_tm_cpu_samples[i].pid = snapshots[i].pid;
        g_tm_cpu_samples[i].cpu_ticks_total = snapshots[i].cpu_ticks_total;
    }
    g_tm_cpu_sample_count = count;
}

static void tm_format_process_state(ProcessState state,char* out,int max_len){
    switch(state){
        case Process_Running:   scpy(out, "RUNNING", max_len); break;
        case Process_Ready:     scpy(out, "READY", max_len); break;
        case Process_Blocked:   scpy(out, "BLOCKED", max_len); break;
        case Process_Sleeping:  scpy(out, "SLEEPING", max_len); break;
        case Process_Terminated:scpy(out, "EXITED", max_len); break;
        default:                scpy(out, "UNKNOWN", max_len); break;
    }
}

static void tm_query_memory_usage_kb(int& total_kb,int& used_kb,int& available_kb){
    uint64_t total_bytes = PMM::GetTotalMemory();
    uint64_t free_bytes = PMM::GetFreeMemory();

    if (total_bytes == 0) {
        total_bytes = (uint64_t)KernelHeap::GetTotal();
        free_bytes = (uint64_t)KernelHeap::GetFree();
    }
    if (free_bytes > total_bytes) free_bytes = 0;

    total_kb = (int)(total_bytes / 1024ULL);
    available_kb = (int)(free_bytes / 1024ULL);
    used_kb = total_kb - available_kb;
    if (used_kb < 0) used_kb = 0;
    if (total_kb < used_kb) total_kb = used_kb;
}

static void tm_format_window_state(WindowState state,char* out,int max_len){
    switch(state){
        case WIN_NORMAL:     scpy(out, "OPEN", max_len); break;
        case WIN_MINIMIZED:  scpy(out, "MIN", max_len); break;
        case WIN_MAXIMIZED:  scpy(out, "MAX", max_len); break;
        case WIN_FULLSCREEN: scpy(out, "FULL", max_len); break;
        default:             scpy(out, "CLOSED", max_len); break;
    }
}

static void tm_close_window_row(int window_id,const char* title){
    if(window_id <= 0) return;

    if(title && tm_starts_with(title, "Browser")){
        BrowserApp::Close();
        return;
    }
    if(title && scmp(title, "Media Player") == 0){
        MediaPlayerApp::Close();
        return;
    }
    WindowManager::CloseWindow(window_id);
}

static int tm_find_linux_process_index_by_task_pid(int native_pid){
    for(int i=0;i<LINUX_MAX_PROCS;i++){
        LinuxProcess* proc = LinuxSyscall::GetProcess(i);
        if(proc && proc->task && (int)proc->task->pid == native_pid) return i;
    }
    return -1;
}

static bool tm_terminate_scheduler_process(const TMProcess* proc){
    if(!proc || proc->pid <= 0 || proc->source_kind != TM_PROC_SCHED) return false;

    Process* task = Scheduler::FindProcessByPid((uint32_t)proc->pid);
    if(!task) return false;

    Process* current = Scheduler::GetCurrentProcess();
    if(Userspace::IsActive() && current && current->pid == task->pid) return false;

    int linux_idx = tm_find_linux_process_index_by_task_pid(proc->pid);
    Scheduler::MarkProcessExited(task, -9);
    if(linux_idx >= 0){
        LinuxProcess* linux_proc = LinuxSyscall::GetProcess(linux_idx);
        if(linux_proc){
            linux_proc->exit_code = -9;
            linux_proc->exited = true;
        }
        LinuxSyscall::DestroyProcess(linux_idx);
    }
    Scheduler::DestroyProcess(task);
    return true;
}

static bool tm_launch_process_by_name(const char* name){
    if(scmp(name, "Terminal") == 0){ DesktopEnvironment::LaunchTerminal(); return true; }
    if(scmp(name, "Files") == 0){ DesktopEnvironment::LaunchFileBrowser(); return true; }
    if(scmp(name, "Calculator") == 0){ DesktopEnvironment::LaunchCalculator(); return true; }
    if(scmp(name, "Text Editor") == 0){ DesktopEnvironment::LaunchTextEditor(); return true; }
    if(scmp(name, "Settings") == 0){ DesktopEnvironment::LaunchSettings(); return true; }
    if(scmp(name, "Task Manager") == 0){ DesktopEnvironment::LaunchTaskManager(); return true; }
    if(tm_starts_with(name, "Browser")){ DesktopEnvironment::LaunchBrowser(); return true; }
    if(scmp(name, "Media Player") == 0){ DesktopEnvironment::LaunchMediaPlayer(); return true; }
    return false;
}

TMTab       TaskManagerApp::current_tab    = TM_PROCESSES;
TMProcess   TaskManagerApp::procs[TM_MAX_PROCS];
int         TaskManagerApp::proc_count     = 0;
int         TaskManagerApp::selected_proc  = -1;
TMWindowRow TaskManagerApp::window_rows[TM_MAX_WINDOWS_ROWS];
int         TaskManagerApp::window_count   = 0;
int         TaskManagerApp::selected_window = -1;
int         TaskManagerApp::scroll_offset  = 0;
TMSortCol   TaskManagerApp::sort_col       = TM_SORT_CPU;
bool        TaskManagerApp::sort_asc       = false;

TMService   TaskManagerApp::services[32];
int         TaskManagerApp::service_count  = 0;

int         TaskManagerApp::cpu_usage      = 0;
int         TaskManagerApp::cpu_cores      = 1;
int         TaskManagerApp::mem_used_kb    = 0;
int         TaskManagerApp::mem_total_kb   = 0;
int         TaskManagerApp::mem_cached_kb  = 0;
int         TaskManagerApp::uptime_sec     = 0;
int         TaskManagerApp::net_rx_kb      = 0;
int         TaskManagerApp::net_tx_kb      = 0;
int         TaskManagerApp::disk_read_kb   = 0;
int         TaskManagerApp::disk_write_kb  = 0;
int         TaskManagerApp::cpu_history[60];
int         TaskManagerApp::mem_history[60];
int         TaskManagerApp::hist_idx       = 0;
int         TaskManagerApp::tick_counter   = 0;
bool        TaskManagerApp::action_menu_open = false;
int         TaskManagerApp::action_menu_row  = -1;
TMTab       TaskManagerApp::action_menu_tab  = TM_PROCESSES;
int         TaskManagerApp::action_menu_x    = 0;
int         TaskManagerApp::action_menu_y    = 0;

//  init / open
void TaskManagerApp::Init(){
    current_tab=TM_PROCESSES;
    proc_count=0; window_count=0; selected_proc=-1; selected_window=-1; scroll_offset=0;
    sort_col=TM_SORT_CPU; sort_asc=false;
    cpu_cores=CPUDetect::GetCoreCount();
    if(cpu_cores < 1) cpu_cores = 1;
    tm_query_memory_usage_kb(mem_total_kb, mem_used_kb, mem_cached_kb);
    if(mem_total_kb < 1024) mem_total_kb = 262144;
    for(int i=0;i<60;i++){cpu_history[i]=0;mem_history[i]=0;}
    hist_idx=0; tick_counter=0;
    InitServices();
    RefreshProcesses();
}

int TaskManagerApp::Open(){
    Init();
    RuntimeLog::LogAppEvent("tasks", "open");
    int wid = WindowManager::CreateWindow("Task Manager", -1, -1, 560, 420,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TaskManagerApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TaskManagerApp::Input(w,p1,p2,true,0);
            else if(ev==2) TaskManagerApp::Input(w,0,0,false,(char)p1);
            else if(ev==4) TaskManagerApp::Input(w,p1 - w->content_x,p2 - w->content_y,false,(char)4);
            else if(ev==6 && p1 == 1 && p2 == 1){
                int mx = 0, my = 0;
                Mouse::GetPosition(mx, my);
                TaskManagerApp::Input(w, mx - w->content_x, my - w->content_y, false, (char)4);
            }
        }
    );
    return wid;
}

void TaskManagerApp::InitServices(){
    service_count = 0;
    static const struct { const char* name; const char* status; const char* type; int pid; } svc[] = {
        {"kthread",      "Running", "Kernel",  2},
        {"scheduler",    "Running", "Kernel",  3},
        {"irq/timer",    "Running", "Kernel",  4},
        {"irq/keyboard", "Running", "Kernel",  5},
        {"irq/mouse",    "Running", "Kernel",  6},
        {"bga_display",  "Running", "Kernel",  7},
        {"kvfs",         "Running", "System",  10},
        {"network",      "Running", "System",  11},
        {"supr",         "Running", "System",  12},
        {"pkgmgr",       "Running", "System",  13},
        {"window_mgr",   "Running", "System",  14},
        {"shell",        "Running", "User",    20},
        {"desktop",      "Running", "User",    21},
        {"lockscreen",   "Stopped", "User",    0},
    };
    int n = 14;
    for(int i=0;i<n&&service_count<32;i++){
        scpy(services[service_count].name, svc[i].name, 32);
        scpy(services[service_count].status, svc[i].status, 12);
        scpy(services[service_count].type, svc[i].type, 16);
        services[service_count].pid = svc[i].pid;
        service_count++;
    }
}

void TaskManagerApp::Tick(){
    // Time-based, not frame-based: refresh process list at ~1 Hz so we
    // don't burn CPU at high frame rates and stay smooth at low ones.
    static uint32_t last_refresh_ms = 0;
    uint32_t now = Time::GetTicks();
    if(last_refresh_ms != 0 && (now - last_refresh_ms) < 1000) return;
    last_refresh_ms = now;
    tick_counter++;
    RefreshProcesses();
}

void TaskManagerApp::RefreshProcesses(){
    proc_count=0;
    uint32_t now_ms = Time::GetTicks();
    static uint32_t last_sample_ms = 0;
    uint32_t elapsed_ms = (last_sample_ms == 0 || now_ms <= last_sample_ms) ? 500 : (now_ms - last_sample_ms);
    last_sample_ms = now_ms;

    NetworkInterface* eth = Network::GetInterface("eth0");
    int current_net_rx_kb = eth ? (int)(eth->rx_bytes / 1024) : 0;
    int current_net_tx_kb = eth ? (int)(eth->tx_bytes / 1024) : 0;
    net_rx_kb = current_net_rx_kb;
    net_tx_kb = current_net_tx_kb;

    SchedulerProcessSnapshot snapshots[TM_MAX_PROCS];
    int snapshot_count = Scheduler::GetProcessSnapshot(snapshots, TM_MAX_PROCS - 1);
    int total_busy_cpu = 0;

    for(int i=0;i<snapshot_count && proc_count<TM_MAX_PROCS-1;i++){
        const SchedulerProcessSnapshot& snap = snapshots[i];
        int sample_idx = tm_find_cpu_sample(snap.pid);
        uint64_t prev_ticks = snap.cpu_ticks_total;
        if(g_tm_cpu_samples_ready && sample_idx >= 0){
            prev_ticks = g_tm_cpu_samples[sample_idx].cpu_ticks_total;
        }

        uint64_t delta_ticks = 0;
        if(snap.cpu_ticks_total >= prev_ticks) delta_ticks = snap.cpu_ticks_total - prev_ticks;
        int cpu_pct = g_tm_cpu_samples_ready ? tm_cpu_pct_from_ticks(delta_ticks, elapsed_ms) : 0;

        TMProcess* p = &procs[proc_count++];
        p->pid = (int)snap.pid;
        scpy(p->name, snap.name[0] ? snap.name : "process", 32);
        p->cpu_pct = cpu_pct;
        p->mem_kb = tm_max(4, (int)snap.memory_kb);
        p->threads = 1;
        tm_format_process_state(snap.state, p->state, 12);
        scpy(p->user, (snap.flags & PROCESS_FLAG_USER) ? "user" : "SYSTEM", 16);
        p->priority = (int)snap.priority;
        p->io_read_kb = 0;
        p->io_write_kb = 0;
        p->flags = snap.flags;
        p->source_kind = TM_PROC_SCHED;
        p->stack_kb = snap.stack_kb;
        p->stack_cap_kb = snap.stack_cap_kb;
        p->cpu_ms_total = snap.cpu_ms_total;
        p->stack_grow_count = snap.stack_grow_count;
        p->prio_tier = snap.prio_tier;
        p->is_kernel_proc = snap.is_kernel_proc;
        total_busy_cpu += cpu_pct;
    }

    tm_store_cpu_samples(snapshots, snapshot_count);
    g_tm_cpu_samples_ready = true;

    // Blend open windows as process rows (CPU 0  -  these are UI sessions,
    // not scheduler tasks, so no fake percentages).
    RefreshWindows();
    for(int i=0;i<window_count && proc_count<TM_MAX_PROCS-1;i++){
        TMWindowRow* row = &window_rows[i];
        bool already_listed = false;
        for(int j=0;j<proc_count;j++){
            if(procs[j].pid == row->id && procs[j].source_kind == TM_PROC_SCHED){
                already_listed = true;
                break;
            }
        }
        if(already_listed) continue;

        TMProcess* p = &procs[proc_count++];
        p->pid = row->id;
        scpy(p->name, row->title, 32);
        p->cpu_pct = 0;
        p->mem_kb = 128 + i * 32;
        p->threads = 1;
        scpy(p->state, "RUNNING", 12);
        scpy(p->user, "user", 16);
        p->priority = 20;
        p->io_read_kb = 0;
        p->io_write_kb = 0;
        p->flags = PROCESS_FLAG_NONE;
        p->source_kind = TM_PROC_IDLE;
        p->stack_kb = 0;
        p->stack_cap_kb = 0;
        p->cpu_ms_total = 0;
        p->stack_grow_count = 0;
        p->prio_tier = 0;
        p->is_kernel_proc = false;
    }

    if(proc_count < TM_MAX_PROCS){
        TMProcess* idle = &procs[proc_count++];
        idle->pid = 0;
        scpy(idle->name, TM_IDLE_NAME, 32);
        idle->cpu_pct = tm_clamp(100 - total_busy_cpu, 0, 100);
        idle->mem_kb = 4;
        idle->threads = 1;
        scpy(idle->state, "RUNNING", 12);
        scpy(idle->user, "SYSTEM", 16);
        idle->priority = 0;
        idle->io_read_kb = 0;
        idle->io_write_kb = 0;
        idle->flags = PROCESS_FLAG_NONE;
        idle->source_kind = TM_PROC_IDLE;
        idle->stack_kb = 0;
        idle->stack_cap_kb = 0;
        idle->cpu_ms_total = 0;
        idle->stack_grow_count = 0;
        idle->prio_tier = 0;
        idle->is_kernel_proc = false;
    }

    SortProcesses();

    // compute totals
    int total_mem=0;
    for(int i=0;i<proc_count;i++){
        total_mem+=procs[i].mem_kb;
    }
    cpu_usage = tm_clamp(total_busy_cpu, 0, 100);

    tm_query_memory_usage_kb(mem_total_kb, mem_used_kb, mem_cached_kb);
    if(mem_used_kb<1) mem_used_kb = total_mem;
    if(mem_total_kb < mem_used_kb) mem_total_kb = mem_used_kb + 1024;

    uptime_sec = Time::GetTicks() / 1000;
    disk_read_kb = (int)(KVFS::DiskUsage("/") / 1024);
    disk_write_kb = (int)(KernelHeap::GetFree() / 1024);

    // record history
    cpu_history[hist_idx % 60] = tm_clamp(cpu_usage, 0, 100);
    mem_history[hist_idx % 60] = tm_clamp((mem_used_kb * 100) / (mem_total_kb > 0 ? mem_total_kb : 1), 0, 100);
    hist_idx++;

    if(selected_proc >= proc_count) selected_proc = proc_count - 1;
    if(selected_proc < -1) selected_proc = -1;
}

void TaskManagerApp::RefreshWindows(){
    window_count = 0;
    Window* windows = WindowManager::GetWindows();
    if(!windows) return;

    for(int i=0;i<WM_MAX_WINDOWS && window_count<TM_MAX_WINDOWS_ROWS;i++){
        Window* win = &windows[i];
        if(win->state == WIN_CLOSED) continue;

        TMWindowRow* row = &window_rows[window_count++];
        row->id = win->id;
        scpy(row->title, win->title, 64);
        tm_format_window_state(win->state, row->state, 12);
        if(win->focused) scpy(row->session, "Focused", 16);
        else if(win->visible) scpy(row->session, "Background", 16);
        else scpy(row->session, "Hidden", 16);
        row->x = win->x;
        row->y = win->y;
        row->w = win->w;
        row->h = win->h;
    }

    if(selected_window >= window_count) selected_window = window_count - 1;
    if(selected_window < -1) selected_window = -1;
}

void TaskManagerApp::SortProcesses(){
    // simple bubble sort
    for(int i=0;i<proc_count-1;i++){
        for(int j=0;j<proc_count-1-i;j++){
            bool swap=false;
            switch(sort_col){
                case TM_SORT_NAME: swap = (scmp(procs[j].name,procs[j+1].name) > 0) != sort_asc; break;
                case TM_SORT_PID:  swap = (procs[j].pid > procs[j+1].pid) != sort_asc; break;
                case TM_SORT_CPU:  swap = (procs[j].cpu_pct < procs[j+1].cpu_pct) != sort_asc; break;
                case TM_SORT_MEM:  swap = (procs[j].mem_kb < procs[j+1].mem_kb) != sort_asc; break;
            }
            if(swap){
                TMProcess tmp = procs[j];
                procs[j] = procs[j+1];
                procs[j+1] = tmp;
            }
        }
    }
}

//  rendering
void TaskManagerApp::RenderTabs(int x,int y,int w){
    Graphics::FillRect(x,y,w,TAB_H,TM_TAB_BG);
    Graphics::DrawLine(x,y+TAB_H-1,x+w,y+TAB_H-1,TM_BORDER);

    static const char* tabs[] = {"Processes", "Performance", "Details", "Services", "Windows"};
    int tx=x+6;
    for(int i=0;i<TM_TAB_COUNT;i++){
        int tw=slen(tabs[i])*8+20;
        bool active = (i==(int)current_tab);
        if(active){
            Graphics::FillRoundedRect(tx, y+4, tw, TAB_H-6, 6, TM_TAB_SEL);
            Graphics::FillRect(tx+4, y+TAB_H-3, tw-8, 2, TM_BLUE);
        }
        Graphics::DrawString(tx+10, y+9, tabs[i], active ? TM_TAB_ACT : TM_TAB_TXT, 0xFF000000);
        tx+=tw+4;
    }
}

static void DrawGraph(int gx, int gy, int gw, int gh, int* data, int idx, int max_val, unsigned int line_col, unsigned int fill_col){
    Graphics::FillRect(gx,gy,gw,gh,TM_GRAPH_BG);
    Graphics::DrawRect(gx,gy,gw,gh,TM_BORDER);
    // grid lines
    for(int i=1;i<4;i++){
        int gy2=gy+i*gh/4;
        for(int p=gx+2;p<gx+gw-2;p+=6) Graphics::FillRect(p,gy2,2,1,TM_GRAPH_GD);
    }
    // plot
    for(int i=1;i<60;i++){
        int i0=(idx-60+i-1+600)%60;
        int i1=(idx-60+i+600)%60;
        int v0=data[i0]; if(v0>max_val)v0=max_val;
        int v1=data[i1]; if(v1>max_val)v1=max_val;
        int y0=gy+gh-2-(v0*(gh-4)/max_val);
        int y1=gy+gh-2-(v1*(gh-4)/max_val);
        int x0=gx+2+(i-1)*(gw-4)/60;
        int x1=gx+2+i*(gw-4)/60;
        // fill under line
        int fy=y1<y0?y1:y0;
        Graphics::FillRectAlpha(x1, fy, (gw-4)/60+1, gy+gh-2-fy, 40, fill_col);
        Graphics::DrawLine(x0,y0,x1,y1,line_col);
    }
}

void TaskManagerApp::RenderProcessList(int x,int y,int w,int h){
    // column header
    Graphics::FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    int cols[] = {8, 48, 200, 258, 320, 360, 432}; // pid, name, cpu%, mem, thr, stack, status
    const char* headers[] = {"PID", "Name", "CPU", "Memory", "Thr", "Stack KB", "Status"};
    TMSortCol scols[] = {TM_SORT_PID, TM_SORT_NAME, TM_SORT_CPU, TM_SORT_MEM, TM_SORT_PID, TM_SORT_MEM, TM_SORT_NAME};
    for(int i=0;i<7;i++){
        unsigned int hc = (i < 4 && scols[i] == sort_col) ? TM_BLUE : TM_DIM;
        Graphics::DrawString(x+cols[i], y+3, headers[i], hc, 0xFF000000);
    }
    y+=ROW_H;

    int vis=(h-ROW_H)/ROW_H;
    for(int i=0;i<vis && (i+scroll_offset)<proc_count;i++){
        int idx=i+scroll_offset;
        TMProcess* p=&procs[idx];
        int ry=y+i*ROW_H;

        if(idx==selected_proc) Graphics::FillRect(x,ry,w,ROW_H,TM_SEL_BG);
        else if(i%2) Graphics::FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        // pid
        char num[8]; int_to_str(p->pid,num,8);
        Graphics::DrawString(x+8, ry+2, num, TM_DIM, 0xFF000000);
        // name with colored indicator
        unsigned int nc = (p->state[0]=='R') ? TM_TEXT : TM_DIM;
        Graphics::DrawString(x+48, ry+2, p->name, nc, 0xFF000000);
        // cpu bar + number
        int cpu_bar_w = (p->cpu_pct * 40) / 100;
        unsigned int bar_c = p->cpu_pct > 50 ? TM_RED : (p->cpu_pct > 20 ? TM_ORANGE : TM_GREEN);
        if (cpu_bar_w > 0) Graphics::FillRect(x+200, ry+6, cpu_bar_w, 8, bar_c);
        int_to_str(p->cpu_pct, num, 8); sapp(num, "%", 8);
        Graphics::DrawString(x+244, ry+2, num, bar_c, 0xFF000000);
        // memory
        char mem[16]={0};
        if (p->mem_kb >= 1024) {
            int_to_str(p->mem_kb/1024, mem, 16); sapp(mem, " MB", 16);
        } else {
            int_to_str(p->mem_kb, mem, 16); sapp(mem, " KB", 16);
        }
        Graphics::DrawString(x+258, ry+2, mem, TM_DIM, 0xFF000000);
        // threads
        int_to_str(p->threads, num, 8);
        Graphics::DrawString(x+328, ry+2, num, TM_DIM, 0xFF000000);
        // stack KB / cap KB (only meaningful for kernel processes)
        if (p->is_kernel_proc && p->stack_cap_kb > 0) {
            char stk[24] = {0};
            int_to_str((int)p->stack_kb, stk, 24);
            sapp(stk, "/", 24);
            char capb[12] = {0};
            int_to_str((int)p->stack_cap_kb, capb, 12);
            sapp(stk, capb, 24);
            unsigned int sc = TM_DIM;
            if (p->stack_cap_kb > 0 &&
                (uint32_t)((uint64_t)p->stack_kb * 100ULL / (uint64_t)p->stack_cap_kb) > 75)
                sc = TM_ORANGE;
            Graphics::DrawString(x+360, ry+2, stk, sc, 0xFF000000);
        } else {
            Graphics::DrawString(x+360, ry+2, "-", TM_DIM, 0xFF000000);
        }
        // state with color
        unsigned int st_clr = TM_DIM;
        if(p->state[0]=='R') st_clr = TM_GREEN;
        else if(p->state[0]=='S') st_clr = TM_YELLOW;
        else if(p->state[0]=='Z') st_clr = TM_RED;
        Graphics::FillCircle(x+436, ry+10, 3, st_clr);
        Graphics::DrawString(x+444, ry+2, p->state, st_clr, 0xFF000000);
    }
}

void TaskManagerApp::RenderPerformance(int x,int y,int w,int h){
    (void)h;
    int ly=y+8;
    int half_w = (w-36)/2;
    CpuInfo cpu_info = CPUDetect::GetInfo();
    const char* cpu_brand = cpu_info.brand_string[0] ? cpu_info.brand_string : CPUDetect::GetVendorName();
    int cpu_threads = cpu_info.topology.logical_cores > 0 ? cpu_info.topology.logical_cores : cpu_cores;
    int cpu_base_mhz = cpu_info.frequency.base_mhz > 0 ? cpu_info.frequency.base_mhz : CPUDetect::GetBaseMHz();
    int cpu_turbo_mhz = cpu_info.frequency.max_mhz;
    const GpuProbeResult& gpr = GpuProbe::GetResult();
    const LinuxDriver* wifi_drv = tm_find_wifi_driver();
    const LinuxDriver* bt_drv = tm_find_bt_driver();

    Graphics::FillRoundedRect(x+8, ly, half_w, 130, 6, TM_PANEL);
    Graphics::DrawString(x+16, ly+6, "CPU", TM_BLUE, 0xFF000000);
    char pct[8]; int_to_str(cpu_usage,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+half_w-20, ly+6, pct, TM_WHITE, 0xFF000000);
    DrawGraph(x+16, ly+24, half_w-16, 64, cpu_history, hist_idx, 100, TM_BLUE, TM_BLUE);
    // cpu info
    char cpu_brand_line[48] = {0};
    char cpu_core_line[48] = "Cores: ";
    char cpu_freq_line[48] = "Base: ";
    char cpu_arch_line[48] = "Arch: x86-64";
    char nbuf[12] = {0};
    scpy(cpu_brand_line, cpu_brand, 48);
    int_to_str(cpu_cores, nbuf, 12); sapp(cpu_core_line, nbuf, 48);
    sapp(cpu_core_line, "  Threads: ", 48);
    int_to_str(cpu_threads, nbuf, 12); sapp(cpu_core_line, nbuf, 48);
    if (cpu_base_mhz > 0) {
        int_to_str(cpu_base_mhz, nbuf, 12); sapp(cpu_freq_line, nbuf, 48); sapp(cpu_freq_line, " MHz", 48);
        if (cpu_turbo_mhz > 0) {
            sapp(cpu_freq_line, "  Turbo: ", 48);
            int_to_str(cpu_turbo_mhz, nbuf, 12); sapp(cpu_freq_line, nbuf, 48); sapp(cpu_freq_line, " MHz", 48);
        }
    } else {
        sapp(cpu_freq_line, "unknown", 48);
    }
    Graphics::DrawString(x+16, ly+94, cpu_brand_line, TM_TEXT, 0xFF000000);
    Graphics::DrawString(x+16, ly+110, cpu_core_line, TM_DIM, 0xFF000000);
    Graphics::DrawString(x+half_w/2, ly+94, cpu_arch_line, TM_DIM, 0xFF000000);
    Graphics::DrawString(x+half_w/2, ly+110, cpu_freq_line, TM_DIM, 0xFF000000);

    Graphics::FillRoundedRect(x+half_w+20, ly, half_w, 130, 6, TM_PANEL);
    Graphics::DrawString(x+half_w+28, ly+6, "Memory", TM_PURPLE, 0xFF000000);
    int mem_pct = tm_clamp((mem_used_kb*100)/(mem_total_kb>0?mem_total_kb:1), 0, 100);
    int_to_str(mem_pct,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+w-40, ly+6, pct, TM_WHITE, 0xFF000000);
    DrawGraph(x+half_w+28, ly+24, half_w-16, 64, mem_history, hist_idx, 100, TM_PURPLE, TM_PURPLE);
    // memory details
    char mb[24]={0}; int_to_str(mem_used_kb/1024,mb,24); sapp(mb," / ",24);
    char t2[8]; int_to_str(mem_total_kb/1024,t2,8); sapp(mb,t2,24); sapp(mb," MB",24);
    Graphics::DrawString(x+half_w+28, ly+94, mb, TM_DIM, 0xFF000000);
    char cached[24]="Available: "; int_to_str(mem_cached_kb/1024,t2,8); sapp(cached,t2,24); sapp(cached," MB",24);
    Graphics::DrawString(x+half_w+28, ly+110, cached, TM_DIM, 0xFF000000);
    ly += 140;

    Graphics::FillRoundedRect(x+8, ly, half_w, 70, 6, TM_PANEL);
    Graphics::DrawString(x+16, ly+6, "Disk", TM_ORANGE, 0xFF000000);
    char dio[32]="Read: "; int_to_str(disk_read_kb,t2,8); sapp(dio,t2,32); sapp(dio," KB",32);
    Graphics::DrawString(x+16, ly+26, dio, TM_DIM, 0xFF000000);
    scpy(dio,"Write: ",32); int_to_str(disk_write_kb,t2,8); sapp(dio,t2,32); sapp(dio," KB",32);
    Graphics::DrawString(x+16, ly+44, dio, TM_DIM, 0xFF000000);
    // kvfs bar
    int disk_total_kb = (int)(KernelHeap::GetTotal() / 1024);
    int disk_pct = (disk_read_kb * 100) / (disk_total_kb > 0 ? disk_total_kb : 1);
    disk_pct = tm_clamp(disk_pct, 0, 100);
    int dbw = half_w - 32;
    Graphics::FillRoundedRect(x+half_w-dbw-8, ly+30, dbw, 10, 4, TM_GRAPH_BG);
    Graphics::FillRoundedRect(x+half_w-dbw-8, ly+30, dbw*disk_pct/100, 10, 4, TM_ORANGE);

    Graphics::FillRoundedRect(x+half_w+20, ly, half_w, 70, 6, TM_PANEL);
    Graphics::DrawString(x+half_w+28, ly+6, "Network", TM_CYAN, 0xFF000000);
    char nio[32]="RX: "; int_to_str(net_rx_kb,t2,8); sapp(nio,t2,32); sapp(nio," KB",32);
    Graphics::DrawString(x+half_w+28, ly+26, nio, TM_DIM, 0xFF000000);
    scpy(nio,"TX: ",32); int_to_str(net_tx_kb,t2,8); sapp(nio,t2,32); sapp(nio," KB",32);
    Graphics::DrawString(x+half_w+28, ly+44, nio, TM_DIM, 0xFF000000);
    Graphics::DrawString(x+w-80, ly+26, "eth0", TM_GREEN, 0xFF000000);
    ly += 80;

    Graphics::FillRoundedRect(x+8, ly, w-16, 120, 6, TM_PANEL);
    // uptime
    int hrs=uptime_sec/3600, mins=(uptime_sec%3600)/60, secs=uptime_sec%60;
    char up[48]="Uptime: ";
    int_to_str(hrs,t2,8); sapp(up,t2,48); sapp(up,"h ",48);
    int_to_str(mins,t2,8); sapp(up,t2,48); sapp(up,"m ",48);
    int_to_str(secs,t2,8); sapp(up,t2,48); sapp(up,"s",48);
    Graphics::DrawString(x+16, ly+6, up, TM_TEXT, 0xFF000000);
    // process/thread count
    char pi[32]="Processes: "; int_to_str(proc_count,t2,8); sapp(pi,t2,32);
    Graphics::DrawString(x+16, ly+22, pi, TM_DIM, 0xFF000000);
    int total_threads=0; for(int i=0;i<proc_count;i++) total_threads+=procs[i].threads;
    scpy(pi,"Threads: ",32); int_to_str(total_threads,t2,8); sapp(pi,t2,32);
    Graphics::DrawString(x+160, ly+22, pi, TM_DIM, 0xFF000000);
    // heap
    scpy(pi,"Heap: ",32); int_to_str((int)(KernelHeap::GetUsed()/1024),t2,8); sapp(pi,t2,32); sapp(pi," KB used",32);
    Graphics::DrawString(x+280, ly+22, pi, TM_DIM, 0xFF000000);
    char cpu_pane_1[64]={0}, cpu_pane_2[64]="Cores: ";
    char gpu_pane_1[64]={0}, gpu_pane_2[64]={0};
    char wifi_pane_1[64]="State: ", wifi_pane_2[64]="Driver: ";
    char bt_pane_1[64]="Status: ", bt_pane_2[64]="Driver: ";
    scpy(cpu_pane_1, cpu_brand, 64);
    int_to_str(cpu_cores, t2, 8); sapp(cpu_pane_2, t2, 64); sapp(cpu_pane_2, "  Thr: ", 64);
    int_to_str(cpu_threads, t2, 8); sapp(cpu_pane_2, t2, 64);
    tm_format_gpu_pane(gpr, gpu_pane_1, 64, gpu_pane_2, 64);
    sapp(wifi_pane_1, WiFi::StateString(), 64);
    WiFiNetwork* connected_wifi = WiFi::GetConnectedNetwork();
    if (connected_wifi && WiFi::GetState() == WIFI_CONNECTED) {
        sapp(wifi_pane_1, " ", 64);
        sapp(wifi_pane_1, connected_wifi->ssid, 64);
    }
    sapp(wifi_pane_2, wifi_drv ? wifi_drv->name : "none", 64);
    sapp(bt_pane_1, bt_drv ? "Bound" : "Unavailable", 64);
    sapp(bt_pane_2, bt_drv ? bt_drv->name : "none", 64);
    int pane_w = (w - 40) / 2;
    int pane_y = ly + 42;
    tm_draw_device_pane(x+16, pane_y, pane_w, 46, "CPU", TM_BLUE, cpu_pane_1, cpu_pane_2);
    tm_draw_device_pane(x+20+pane_w, pane_y, pane_w, 46, "GPU", TM_PURPLE, gpu_pane_1, gpu_pane_2);
    tm_draw_device_pane(x+16, pane_y+54, pane_w, 46, "WiFi", TM_CYAN, wifi_pane_1, wifi_pane_2);
    tm_draw_device_pane(x+20+pane_w, pane_y+54, pane_w, 46, "Bluetooth", TM_YELLOW, bt_pane_1, bt_pane_2);
}

void TaskManagerApp::RenderDetails(int x,int y,int w,int h){
    // detailed view  -  all columns
    Graphics::FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    Graphics::DrawString(x+4,  y+3,"PID",  TM_DIM,0xFF000000);
    Graphics::DrawString(x+36, y+3,"User", TM_DIM,0xFF000000);
    Graphics::DrawString(x+100,y+3,"Name", TM_DIM,0xFF000000);
    Graphics::DrawString(x+220,y+3,"CPU", TM_DIM,0xFF000000);
    Graphics::DrawString(x+260,y+3,"Mem",  TM_DIM,0xFF000000);
    Graphics::DrawString(x+320,y+3,"Thr",  TM_DIM,0xFF000000);
    Graphics::DrawString(x+350,y+3,"Pri",  TM_DIM,0xFF000000);
    Graphics::DrawString(x+390,y+3,"I/O R",TM_DIM,0xFF000000);
    Graphics::DrawString(x+440,y+3,"I/O W",TM_DIM,0xFF000000);
    y+=ROW_H;

    int vis=(h-ROW_H)/ROW_H;
    for(int i=0;i<vis && (i+scroll_offset)<proc_count;i++){
        int idx=i+scroll_offset;
        TMProcess* p=&procs[idx];
        int ry=y+i*ROW_H;
        if(idx==selected_proc) Graphics::FillRect(x,ry,w,ROW_H,TM_SEL_BG);
        else if(i%2) Graphics::FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        char num[8];
        int_to_str(p->pid,num,8);
        Graphics::DrawString(x+4,  ry+2,num,TM_DIM,0xFF000000);
        Graphics::DrawString(x+36, ry+2,p->user,TM_DIM,0xFF000000);
        Graphics::DrawString(x+100,ry+2,p->name,TM_TEXT,0xFF000000);
        int_to_str(p->cpu_pct,num,8); sapp(num,"%",8);
        Graphics::DrawString(x+220,ry+2,num,p->cpu_pct>20?TM_ORANGE:TM_DIM,0xFF000000);
        char mem[16]={0}; int_to_str(p->mem_kb,mem,16); sapp(mem,"K",16);
        Graphics::DrawString(x+260,ry+2,mem,TM_DIM,0xFF000000);
        int_to_str(p->threads,num,8);
        Graphics::DrawString(x+328,ry+2,num,TM_DIM,0xFF000000);
        int_to_str(p->priority,num,8);
        Graphics::DrawString(x+354,ry+2,num,TM_DIM,0xFF000000);
        int_to_str(p->io_read_kb,num,8);
        Graphics::DrawString(x+390,ry+2,num,TM_DIM,0xFF000000);
        int_to_str(p->io_write_kb,num,8);
        Graphics::DrawString(x+440,ry+2,num,TM_DIM,0xFF000000);
    }
}

void TaskManagerApp::RenderWindows(int x,int y,int w,int h){
    Graphics::FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    Graphics::DrawString(x+8,  y+3, "ID",      TM_DIM,0xFF000000);
    Graphics::DrawString(x+44, y+3, "Title",   TM_DIM,0xFF000000);
    Graphics::DrawString(x+230,y+3, "State",   TM_DIM,0xFF000000);
    Graphics::DrawString(x+292,y+3, "Session", TM_DIM,0xFF000000);
    Graphics::DrawString(x+390,y+3, "Pos",     TM_DIM,0xFF000000);
    Graphics::DrawString(x+470,y+3, "Size",    TM_DIM,0xFF000000);
    y += ROW_H;

    int vis = (h - ROW_H) / ROW_H;
    for(int i=0;i<vis && (i+scroll_offset)<window_count;i++){
        int idx = i + scroll_offset;
        TMWindowRow* row = &window_rows[idx];
        int ry = y + i * ROW_H;
        if(idx == selected_window) Graphics::FillRect(x,ry,w,ROW_H,TM_SEL_BG);
        else if(i%2) Graphics::FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        char num[16] = {0};
        char pos[24] = {0};
        char size[24] = {0};
        int_to_str(row->id, num, 16);
        Graphics::DrawString(x+8, ry+2, num, TM_DIM, 0xFF000000);
        Graphics::DrawString(x+44, ry+2, row->title, TM_TEXT, 0xFF000000);

        unsigned int state_color = TM_DIM;
        if(row->state[0] == 'O' || row->state[0] == 'F') state_color = TM_GREEN;
        else if(row->state[0] == 'M') state_color = TM_YELLOW;
        Graphics::DrawString(x+230, ry+2, row->state, state_color, 0xFF000000);

        unsigned int session_color = scmp(row->session, "Focused") == 0 ? TM_BLUE : TM_DIM;
        Graphics::DrawString(x+292, ry+2, row->session, session_color, 0xFF000000);

        int_to_str(row->x, pos, 24); sapp(pos, ",", 24); int_to_str(row->y, num, 16); sapp(pos, num, 24);
        Graphics::DrawString(x+390, ry+2, pos, TM_DIM, 0xFF000000);

        size[0] = 0;
        int_to_str(row->w, size, 24); sapp(size, "x", 24); int_to_str(row->h, num, 16); sapp(size, num, 24);
        Graphics::DrawString(x+470, ry+2, size, TM_DIM, 0xFF000000);
    }
}

void TaskManagerApp::RenderServices(int x,int y,int w,int h){
    (void)h;
    Graphics::FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    Graphics::DrawString(x+8,  y+3,"Service",   TM_DIM,0xFF000000);
    Graphics::DrawString(x+180,y+3,"Status",    TM_DIM,0xFF000000);
    Graphics::DrawString(x+280,y+3,"Type",      TM_DIM,0xFF000000);
    Graphics::DrawString(x+380,y+3,"PID",       TM_DIM,0xFF000000);
    y+=ROW_H;

    for(int i=0;i<service_count;i++){
        int ry=y+i*ROW_H;
        if(i%2) Graphics::FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        Graphics::DrawString(x+8,  ry+2, services[i].name, TM_TEXT, 0xFF000000);
        bool running = (services[i].status[0]=='R');
        Graphics::FillCircle(x+184, ry+10, 3, running ? TM_GREEN : TM_RED);
        Graphics::DrawString(x+192, ry+2, services[i].status, running ? TM_GREEN : TM_RED, 0xFF000000);
        Graphics::DrawString(x+280, ry+2, services[i].type, TM_DIM, 0xFF000000);
        if(services[i].pid > 0){
            char num[8]; int_to_str(services[i].pid, num, 8);
            Graphics::DrawString(x+380, ry+2, num, TM_DIM, 0xFF000000);
        } else {
            Graphics::DrawString(x+380, ry+2, "-", TM_DIM, 0xFF000000);
        }
    }
}

void TaskManagerApp::RenderStatusBar(int x,int y,int w){
    Graphics::FillRect(x,y,w,20,TM_STATUS);
    Graphics::DrawLine(x,y,x+w,y,TM_BORDER);

    char info[128]={0};
    char n[8];
    sapp(info,"Processes: ",80); int_to_str(proc_count,n,8); sapp(info,n,80);
    sapp(info,"  |  Windows: ",80); int_to_str(window_count,n,8); sapp(info,n,80);
    sapp(info,"  |  CPU: ",80); int_to_str(cpu_usage,n,8); sapp(info,n,80); sapp(info,"%",80);
    sapp(info,"  |  Mem: ",80); int_to_str(mem_used_kb/1024,n,8); sapp(info,n,80); sapp(info,"/",80);
    int_to_str(mem_total_kb/1024,n,8); sapp(info,n,80); sapp(info," MB",80);
    sapp(info,"  |  Up: ",80); int_to_str(uptime_sec/60,n,8); sapp(info,n,80); sapp(info,"m",80);
    Graphics::DrawString(x+8,y+3,info,TM_DIM,0xFF000000);
}

void TaskManagerApp::Render(void* win_ptr,int cx,int cy,int cw,int ch){
    (void)win_ptr;
    Graphics::FillRect(cx,cy,cw,ch,TM_BG);

    RenderTabs(cx,cy,cw);
    int content_y = cy + TAB_H + 1;
    int content_h = ch - TAB_H - 1 - 20;

    switch(current_tab){
        case TM_PROCESSES:   RenderProcessList(cx,content_y,cw,content_h); break;
        case TM_PERFORMANCE: RenderPerformance(cx,content_y,cw,content_h); break;
        case TM_DETAILS:     RenderDetails(cx,content_y,cw,content_h); break;
        case TM_SERVICES:    RenderServices(cx,content_y,cw,content_h); break;
        case TM_WINDOWS:     RenderWindows(cx,content_y,cw,content_h); break;
        default: break;
    }

    RenderStatusBar(cx, cy+ch-20, cw);

    // action popup (kill / restart) drawn on top
    RenderActionMenu(cx, cy, cw, ch);

    // auto-refresh perf data
    Tick();
}

bool TaskManagerApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    Window* w = (Window*)win_ptr;
    (void)w;
    int abs_mx = w ? (w->content_x + mx) : mx;
    int abs_my = w ? (w->content_y + my) : my;

    if(key=='r' || key=='R'){
        RefreshProcesses();
        return true;
    }

    if(key == 4){
        if(current_tab==TM_PROCESSES || current_tab==TM_DETAILS){
            int list_y = TAB_H + 1 + ROW_H;
            if(my >= list_y){
                int row = (my - list_y) / ROW_H + scroll_offset;
                if(row>=0 && row<proc_count){
                    selected_proc = row;
                    action_menu_open = true;
                    action_menu_row  = row;
                    action_menu_tab  = current_tab;
                    action_menu_x    = abs_mx;
                    action_menu_y    = abs_my;
                    return true;
                }
            }
        } else if(current_tab==TM_WINDOWS){
            int list_y = TAB_H + 1 + ROW_H;
            if(my >= list_y){
                int row = (my - list_y) / ROW_H + scroll_offset;
                if(row>=0 && row<window_count){
                    selected_window = row;
                    action_menu_open = true;
                    action_menu_row  = row;
                    action_menu_tab  = current_tab;
                    action_menu_x    = abs_mx;
                    action_menu_y    = abs_my;
                    return true;
                }
            }
        }
        action_menu_open = false;
        return false;
    }

    if(!clicked) return false;

    // action menu takes priority when open
    if(action_menu_open){
        int am_w = 120, am_h = 52;
        if(abs_mx >= action_menu_x && abs_mx < action_menu_x + am_w &&
           abs_my >= action_menu_y && abs_my < action_menu_y + am_h){
            int row = (abs_my - action_menu_y) / 26;
            if(action_menu_tab == TM_WINDOWS){
                if(row == 0) KillWindow(action_menu_row);
                else if(row == 1) RestartWindow(action_menu_row);
            } else {
                if(row == 0) KillProcess(action_menu_row);
                else if(row == 1) RestartProcess(action_menu_row);
            }
        }
        action_menu_open = false;
        return true;
    }

    // tab selection
    if(my >= 0 && my < TAB_H){
        int tx=6;
        static const char* tabs[] = {"Processes", "Performance", "Details", "Services", "Windows"};
        for(int i=0;i<TM_TAB_COUNT;i++){
            int tw=slen(tabs[i])*8+20;
            if(mx>=tx && mx<tx+tw){
                current_tab=(TMTab)i;
                return true;
            }
            tx+=tw+4;
        }
    }

    // column header click for sorting (processes tab)
    if(current_tab==TM_PROCESSES && my >= TAB_H+1 && my < TAB_H+1+ROW_H){
        if(mx>=48 && mx<200){ sort_col=TM_SORT_NAME; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=200 && mx<258){ sort_col=TM_SORT_CPU; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=258 && mx<320){ sort_col=TM_SORT_MEM; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=8 && mx<48){ sort_col=TM_SORT_PID; sort_asc=!sort_asc; SortProcesses(); return true; }
    }

    // process row click  -  if already selected, open action menu
    if(current_tab==TM_PROCESSES || current_tab==TM_DETAILS){
        int list_y = TAB_H + 1 + ROW_H;
        if(my >= list_y){
            int row = (my - list_y) / ROW_H + scroll_offset;
            if(row>=0 && row<proc_count){
                if(row == selected_proc){
                    // second click on same row  -  open action menu
                    action_menu_open = true;
                    action_menu_row  = row;
                    action_menu_tab  = current_tab;
                    action_menu_x    = abs_mx;
                    action_menu_y    = abs_my;
                } else {
                    selected_proc = row;
                }
                return true;
            }
        }
    } else if(current_tab==TM_WINDOWS){
        int list_y = TAB_H + 1 + ROW_H;
        if(my >= list_y){
            int row = (my - list_y) / ROW_H + scroll_offset;
            if(row>=0 && row<window_count){
                if(row == selected_window){
                    action_menu_open = true;
                    action_menu_row  = row;
                    action_menu_tab  = current_tab;
                    action_menu_x    = abs_mx;
                    action_menu_y    = abs_my;
                } else {
                    selected_window = row;
                }
                return true;
            }
        }
    }

    return false;
}

void TaskManagerApp::RenderActionMenu(int cx, int cy, int cw, int ch){
    (void)cx; (void)cy; (void)cw; (void)ch;
    bool windows_menu = (action_menu_tab == TM_WINDOWS);
    if(!action_menu_open || action_menu_row < 0) return;
    if(windows_menu && action_menu_row >= window_count) return;
    if(!windows_menu && action_menu_row >= proc_count) return;

    // Allow restart for window-blended rows (pid>0, not a scheduler task)
    // and for any row in the dedicated Windows tab.
    bool allow_restart = windows_menu;
    if(!windows_menu && action_menu_row < proc_count){
        const TMProcess* proc = &procs[action_menu_row];
        if(proc->pid > 0 && proc->source_kind != TM_PROC_SCHED) allow_restart = true;
    }

    int am_x = action_menu_x;
    int am_y = action_menu_y;
    int am_w = 120, am_h = 52;

    // clamp to content area
    if(am_x + am_w > cx + cw) am_x = cx + cw - am_w;
    if(am_y + am_h > cy + ch) am_y = cy + ch - am_h;

    // shadow + background
    Graphics::FillRoundedRect(am_x+3, am_y+3, am_w, am_h, 6, 0xFF060610);
    Graphics::FillRoundedRect(am_x, am_y, am_w, am_h, 6, 0xFF161626);
    Graphics::DrawRect(am_x, am_y, am_w, am_h, TM_BORDER);

    // kill row
    Graphics::DrawString(am_x+10, am_y+6,  windows_menu ? "Kill" : "Terminate", TM_RED, 0xFF000000);
    // divider
    Graphics::FillRect(am_x+8, am_y+25, am_w-16, 1, 0xFF252540);
    // restart row
    Graphics::DrawString(am_x+10, am_y+30, allow_restart ? "Restart" : "Restart N/A", allow_restart ? TM_GREEN : TM_DIM, 0xFF000000);
}

void TaskManagerApp::KillProcess(int idx){
    if(idx < 0 || idx >= proc_count) return;
    TMProcess proc = procs[idx];
    action_menu_open = false;

    // Scheduler-backed row: terminate by real PID.
    if(proc.source_kind == TM_PROC_SCHED && proc.pid > 0){
        if(!tm_terminate_scheduler_process(&proc)){
            RuntimeLog::LogAppEvent("tasks", "kill-denied", proc.name);
            return;
        }
        RuntimeLog::LogAppEvent("tasks", "kill-pid", proc.name);
        RefreshProcesses();
        if(selected_proc >= proc_count) selected_proc = proc_count - 1;
        return;
    }

    // Window-blended row: close the window by its id.
    if(proc.pid > 0){
        RuntimeLog::LogAppEvent("tasks", "kill-window", proc.name);
        tm_close_window_row(proc.pid, proc.name);
        RefreshProcesses();
        if(selected_proc >= proc_count) selected_proc = proc_count - 1;
        return;
    }

    RuntimeLog::LogAppEvent("tasks", "kill-denied", proc.name);
}

void TaskManagerApp::RestartProcess(int idx){
    if(idx < 0 || idx >= proc_count) return;
    TMProcess proc = procs[idx];
    action_menu_open = false;

    // Window-blended row: close and relaunch.
    if(proc.pid > 0 && proc.source_kind != TM_PROC_SCHED){
        RuntimeLog::LogAppEvent("tasks", "restart-window", proc.name);
        tm_close_window_row(proc.pid, proc.name);
        if(!tm_launch_process_by_name(proc.name)){
            RuntimeLog::LogAppEvent("tasks", "restart-failed", proc.name);
        }
        RefreshProcesses();
        return;
    }

    RuntimeLog::LogAppEvent("tasks", "restart-denied", proc.name);
}

void TaskManagerApp::KillWindow(int idx){
    if(idx < 0 || idx >= window_count) return;
    TMWindowRow row = window_rows[idx];
    action_menu_open = false;

    RuntimeLog::LogAppEvent("tasks", "kill-window", row.title);
    tm_close_window_row(row.id, row.title);
    RefreshProcesses();
}

void TaskManagerApp::RestartWindow(int idx){
    if(idx < 0 || idx >= window_count) return;
    TMWindowRow row = window_rows[idx];
    action_menu_open = false;

    RuntimeLog::LogAppEvent("tasks", "restart-window", row.title);
    tm_close_window_row(row.id, row.title);
    if(!tm_launch_process_by_name(row.title)){
        RuntimeLog::LogAppEvent("tasks", "restart-failed", row.title);
        RefreshProcesses();
        return;
    }

    RefreshProcesses();
}
