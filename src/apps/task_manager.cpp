//  kurono os - task manager (professional redesign)
#include "task_manager.h"
#include "../kui/kui.h"           // the toolkit: one source for the kernel and the ring 3 build (satoru)

// the kernel build reads the scheduler, the page-table accounting, kurono.taskmem and the window
// manager directly, exactly as before. the ring 3 build compiles the same file and reads /proc and
// uses kill(2) instead, see the two host sections below. (satoru)
#if !defined(KUI_BACKEND_WAYLAND)
#include "terminal.h"
#include "../drivers/serial.h"
#include "../drivers/audio_server.h"
#include "browser.h"
#include "media_player.h"
#include "../ui/desktop.h"
#include "../ui/window_manager.h"
#include "../ui/wayland_server.h"
#include "../drivers/bga.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../hal/cpufreq.h"
#include "../kernel/userspace.h"
#include "../linux/linux_drivers.h"
#include "../linux/linux_syscall.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/memacct.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 per-task kernel heap (satoru)
#include "../proc/scheduler.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../net/network.h"
#include "../system/logging.h"
#include "../system/ui_config.h"
#include "../ui/kss.h"
#include "../drivers/timer.h"
#include "../drivers/nvme.h"
#include "../proc/smp.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#endif

// ── toolkit shims ─────────────────────────────────────────────────────
// the canvas of the frame being drawn, valid only inside OnFrame. the draw helpers read it so the
// render code keeps its old shape (x, y, w, h, colour). (satoru)
static kui::Canvas* g_cv = nullptr;
static kui::Window* g_tm_win = nullptr;   // the window being drawn, for animation frames (satoru)

#define TM_BODY_PX 16   // Graphics::DrawString drew inter at the body size, 16 px at 100 percent (satoru)

// task_manager.o is built without sse (a kernel process runs Refresh on a stack that is not
// 16-byte aligned), so text goes through the toolkit's whole-pixel TextI. ui_scale carries the
// 1440p and font scale factor. (satoru)
static int tm_ds(int x,int y,const char* s,uint32_t c){
    return g_cv->TextI(x,y,kui::Face::Sans,TM_BODY_PX * kui::CurrentTheme().ui_scale / 100,s,c);
}

static const unsigned int TM_BG       = 0xFF1B1B1D;
static const unsigned int TM_PANEL    = 0xFF26262A;
static const unsigned int TM_TAB_BG   = 0xFF1B1B1D;
static const unsigned int TM_TAB_SEL  = 0xFF34343A;
static const unsigned int TM_TAB_TXT  = 0xFF9A9AA2;
static const unsigned int TM_TAB_ACT  = 0xFFF0F0F2;
static const unsigned int TM_HEAD_BG  = 0xFF202023;
static const unsigned int TM_ROW_ALT  = 0xFF1F1F22;
static const unsigned int TM_SEL_BG   = 0xFF2C3E5C;
static const unsigned int TM_TEXT     = 0xFFE6E6EA;
static const unsigned int TM_DIM      = 0xFF8E8E96;
static const unsigned int TM_BLUE     = 0xFF3B82F6;
static const unsigned int TM_GREEN    = 0xFF22C55E;
static const unsigned int TM_RED      = 0xFFEF4444;
static const unsigned int TM_YELLOW   = 0xFFEAB308;
static const unsigned int TM_ORANGE   = 0xFFF97316;
static const unsigned int TM_PURPLE   = 0xFF8B5CF6;
static const unsigned int TM_CYAN     = 0xFF06B6D4;
static const unsigned int TM_GRAPH_BG = 0xFF17171A;
static const unsigned int TM_GRAPH_GD = 0xFF2A2A2F;
static const unsigned int TM_BORDER   = 0xFF34343A;
static const unsigned int TM_WHITE    = 0xFFFFFFFF;
static const unsigned int TM_STATUS   = 0xFF202023;

static const int ROW_H = 22;
static const int TAB_H = 36;

// Processes-tab column geometry (content-relative x offsets), sized so every
// column + its widest value fits inside the ~558 px content width with clean
// gaps. The old layout collided cpu% into memory ("0%16 KB") and the kernel
// stack value into the status dot ("1024/32768RUNNING"); these offsets give
// each field breathing room. The header, the cells, and the sort hit-test all
// read from here so they can never drift apart. (satoru)
static const int PL_PID    = 8;    // pid (up to ~6 digits)
static const int PL_NAME   = 56;   // process name (~15 chars before cpu)
static const int PL_CPUBAR = 188;  // cpu utilisation bar (36 px wide)
static const int PL_CPUNUM = 228;  // cpu percentage text ("100%")
static const int PL_MEM    = 270;  // memory ("xxxx MB")
static const int PL_THR    = 340;  // thread count
static const int PL_STACK  = 372;  // kernel stack "used/cap" ("1024/32768")
static const int PL_STDOT  = 458;  // status indicator dot
static const int PL_STATE  = 468;  // status text ("SLEEPING")

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
// the shell launcher tasks exist only to host one ring-3 app; the app itself
// is the row the user cares about, so the launcher plumbing stays hidden. (satoru)
static bool tm_is_hidden_task(const char* n){
    if(tm_starts_with(n, "r3launch")) return true;
    if(tm_starts_with(n, "launcher") && n[8] >= '0' && n[8] <= '9') return true;
    return false;
}
static int tm_max(int a,int b){ return a > b ? a : b; }
static int tm_clamp(int v,int lo,int hi){ return v < lo ? lo : (v > hi ? hi : v); }

// AppIcons::Id values, duplicated so the ring 3 build needs no kernel header. (satoru)
enum { TM_ICON_TERMINAL = 0, TM_ICON_FILES = 1, TM_ICON_CALCULATOR = 2, TM_ICON_EDITOR = 3,
       TM_ICON_SETTINGS = 4, TM_ICON_BROWSER = 5, TM_ICON_TASKS = 6, TM_ICON_MEDIA = 7,
       TM_ICON_HOME = 8, TM_ICON_FIREFOX = 9, TM_ICON_GENERIC = 10 };

// friendly process names for the rows: a ring-3 app is named after its binary
// (denji, taskmgr), which is not what the user opened. the table also gives
// each row its app tile and the few kernel tasks that belong with the apps
// (a guest vm). unknown names keep their own name and a generic tile. (satoru)
struct TmNameMap { const char* proc; const char* label; int icon; int group; };
static const TmNameMap kTmNames[] = {
    { "calculator",  "Calculator",       TM_ICON_CALCULATOR, 0 },
    { "texteditor",  "Text Editor",      TM_ICON_EDITOR,     0 },
    { "files",       "Files",            TM_ICON_FILES,      0 },
    { "taskmgr",     "Task Manager",     TM_ICON_TASKS,      0 },
    { "mediaplayer", "Media Player",     TM_ICON_MEDIA,      0 },
    { "denji",       "Media Player",     TM_ICON_MEDIA,      0 },
    { "terminal",    "Terminal",         TM_ICON_TERMINAL,   0 },
    { "settings",    "Settings",         TM_ICON_SETTINGS,   0 },
    { "installer",   "Installer",        TM_ICON_GENERIC,    0 },
    { "firefox",     "Firefox",          TM_ICON_FIREFOX,    0 },
    { "claude",      "Claude Code",      TM_ICON_GENERIC,    0 },
    { "vmhost",      "Virtual Machine",  TM_ICON_GENERIC,    0 },
    { "bash",        "Shell",            TM_ICON_TERMINAL,   0 },
    { "sh",          "Shell",            TM_ICON_TERMINAL,   0 },
    { "busybox",     "Shell",            TM_ICON_TERMINAL,   0 },
    { "timekeeper",  "Timekeeper",       TM_ICON_GENERIC,    1 },
    { "scheduler",   "Scheduler",        TM_ICON_GENERIC,    1 },
    { "network",     "Network",          TM_ICON_BROWSER,    1 },
    { "input",       "Input",            TM_ICON_GENERIC,    1 },
    { "audio",       "Audio",            TM_ICON_MEDIA,      1 },
    { "gui",         "GUI / compositor", TM_ICON_SETTINGS,   1 },
    { "shell",       "Shell service",    TM_ICON_TERMINAL,   1 },
    { "logging",     "Logging",          TM_ICON_GENERIC,    1 },
    { "watchdog",    "Watchdog",         TM_ICON_GENERIC,    1 },
    { "disklog",     "Disk log",         TM_ICON_GENERIC,    1 },
    { "installer-job","Installer job",   TM_ICON_GENERIC,    1 },
};
// icon for an in-kernel window title, -1 when the title is not a built-in app. (satoru)
static int tm_icon_for_title(const char* t){
    if(!t || !t[0]) return -1;
    if(tm_starts_with(t, "Calculator")) return TM_ICON_CALCULATOR;
    if(tm_starts_with(t, "Text Editor")) return TM_ICON_EDITOR;
    if(tm_starts_with(t, "Files")) return TM_ICON_FILES;
    if(tm_starts_with(t, "Media")) return TM_ICON_MEDIA;
    if(tm_starts_with(t, "Settings")) return TM_ICON_SETTINGS;
    if(tm_starts_with(t, "Task Manager")) return TM_ICON_TASKS;
    if(tm_starts_with(t, "Terminal")) return TM_ICON_TERMINAL;
    if(tm_starts_with(t, "Kurono Installer") || tm_starts_with(t, "Install")) return TM_ICON_GENERIC;
    return -1;
}

// apply the map to one row name. returns the group when the name is known. (satoru)
static bool tm_name_map(const char* raw, char* out, int mx, int* icon, int* group){
    *icon = TM_ICON_GENERIC;
    *group = -1;
    if(!raw || !raw[0]) return false;
    for(int i=0;i<(int)(sizeof(kTmNames)/sizeof(kTmNames[0]));i++){
        const char* a = kTmNames[i].proc;
        int k = 0;
        while(a[k] && raw[k] == a[k]) k++;
        if(a[k] || raw[k]) continue;
        scpy(out, kTmNames[i].label, mx);
        *icon = kTmNames[i].icon;
        *group = kTmNames[i].group;
        return true;
    }
    scpy(out, raw, mx);
    return false;
}


struct TMThread { uint32_t leader; uint32_t pid; int cpu_pct; char state[12]; };
static TMThread g_threads[192];
static int      g_thread_count = 0;
static uint32_t g_expanded[8];
static int      g_expanded_n = 0;
// 32: the laptop has 20 cpus, the old 16 hid cpus 16-19 (satoru)
static int      g_core_pct[32];
static int      g_core_n = 1;
static int      g_core_hist[32][60];   // per-cpu usage history for the performance graphs (satoru)
static int      g_core_hist_idx = 0;
static int      g_net_rx_hist[60], g_net_tx_hist[60], g_dsk_rd_hist[60], g_dsk_wr_hist[60];   // kb/s (satoru)
static int      g_io_idx = 0;
static int      g_net_rx_rate = 0, g_net_tx_rate = 0, g_dsk_rd_rate = 0, g_dsk_wr_rate = 0;
struct TMRow { int16_t kind; int16_t idx; int16_t proc; };   // kind 0 = process, 1 = thread (idx into g_threads); proc = owning procs index (satoru)
static TMRow    g_rows[TM_MAX_PROCS + 192];
static int      g_row_count = 0;

static int tm_kb_i(uint64_t kb){ return kb > 0x7FFFFFFFull ? 0x7FFFFFFF : (int)kb; }


static int g_tm_user_kb = 0;      // unique user ram (pss sum) over the address spaces walked in the last few seconds (satoru)
static int g_tm_other_kb = 0;     // used minus the named parts, never negative (satoru)
static int g_tm_kmx_kb = 0;       // kmemx pool, for the memory card (satoru)
static uint64_t g_tm_last_render_ms = 0;   // page-table walks only run while the window is being drawn (satoru)

// transient action feedback shown in the status bar ("real replies"): the
// last kill/restart outcome plus when it happened, so the user sees what the
// action actually did instead of a silent list refresh. (satoru)
static char     g_tm_action_msg[96] = {0};
static uint32_t g_tm_action_ms      = 0;

static void tm_set_action_msg(const char* verb,const char* name,const char* extra){
    g_tm_action_msg[0] = 0;
    sapp(g_tm_action_msg, verb, 96);
    sapp(g_tm_action_msg, name, 96);
    if(extra) sapp(g_tm_action_msg, extra, 96);
    g_tm_action_ms = kui::NowMs();
}


#if !defined(KUI_BACKEND_WAYLAND)
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
    g_cv->FillRoundedRect(x, y, w, h, 6, TM_GRAPH_BG);
    tm_ds(x+8, y+6, title, accent);
    tm_ds(x+8, y+20, line1, TM_TEXT);
    tm_ds(x+8, y+34, line2, TM_DIM);
}

struct TMCpuSample {
    uint32_t pid;
    uint64_t cpu_ticks_total;   // busy ms of this pid at the last full window (satoru)
    int      pct;               // percentage computed at that window (satoru)
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


extern volatile uint64_t g_cpu_busy_ticks[32];   // per-cpu busy ms, charged from the timer irqs in hal.cpp (satoru)
extern uint64_t hal_kernel_cpu_ms();             // busy kernel time no task owns, hal.cpp (satoru)

static void tm_store_cpu_samples(const SchedulerProcessSnapshot* snapshots,const int* pcts,int count){
    if(!snapshots || count <= 0){
        g_tm_cpu_sample_count = 0;
        return;
    }

    if(count > TM_MAX_PROCS) count = TM_MAX_PROCS;
    for(int i=0;i<count;i++){
        g_tm_cpu_samples[i].pid = snapshots[i].pid;
        g_tm_cpu_samples[i].cpu_ticks_total = snapshots[i].cpu_ticks_total;
        g_tm_cpu_samples[i].pct = pcts[i];
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

// honest system-ram accounting straight from the physical memory manager:
// total = all usable frames, free = unallocated frames, used = total - free.
// this is REAL physical ram, not a sum of per-process figures. the used figure
// is split into the parts the kernel can name: the kernel heap arena, the kmemx
// pool, user memory (the proportional share of every walked address space, so a
// page shared by several processes is counted once) and "other" (kernel image,
// page tables, framebuffers, dma buffers, shared memory not yet attributed).
// everything comes from MemAcct (kernel/memacct.cpp), read-only. (satoru)
extern uint64_t pmm_installed_ram_bytes();   // firmware-map ram incl. what kurono.memcap or the 512 gb identity bound leaves out, pmm.cpp (satoru)
static MemAcct::SysMem g_tm_sys;
static uint64_t tm_now64(){ return Timer::GetRealMs64(); }   // tsc clock, the base the busy samplers use (satoru)

// kurono.taskmem=1: owner records copied once per refresh. a kernel task's row
// is its committed kernel stack + the heap blocks and pmm blocks it allocated;
// a user process adds the kernel heap it caused (fd tables, sockets, thread
// records) to its resident set; each built-in app with memory gets its own row
// (its heap is not in the host task's row), and "Kernel (shared)" is whatever
// used ram no row claims, so the rows add up to the used figure. (satoru)
static TaskMem::Row    g_tmrows[1024];
static int             g_tmrow_n = 0;
static TaskMem::Totals g_tmtot = {};
static int tm_task_kb(uint32_t pid){
    uint64_t b = 0;
    for(int i=0;i<g_tmrow_n;i++){
        const TaskMem::Row& r = g_tmrows[i];
        if(!(r.key & TaskMem::APP_KEY) && r.key == pid) b += r.heap_bytes + r.frame_bytes;
    }
    return tm_kb_i(b >> 10);
}

static void tm_query_memory_usage_kb(int& total_kb,int& used_kb,int& free_kb,int& heap_kb){
    MemAcct::SysMem m;
    MemAcct::QuerySystem(&m);

    // pmm not initialised (e.g. very early / fallback): fall back to the heap
    // arena so we still show a plausible total instead of zero. (satoru)
    if (m.total_kb == 0) {
        m.total_kb = (uint64_t)KernelHeap::GetTotal() / 1024;
        m.free_kb  = (uint64_t)KernelHeap::GetFree() / 1024;
        m.used_kb  = m.total_kb - m.free_kb;
    }
    g_tm_sys = m;
    g_tm_kmx_kb = tm_kb_i(m.kmemx_pool_kb);
    total_kb = tm_kb_i(m.total_kb);
    free_kb  = tm_kb_i(m.free_kb);
    used_kb  = total_kb - free_kb;
    if (used_kb < 0) used_kb = 0;
    heap_kb  = tm_kb_i(m.heap_arena_kb);
    g_tm_user_kb = tm_kb_i(MemAcct::SumUserPssKb(5000));
    int64_t other = (int64_t)used_kb - heap_kb - g_tm_user_kb - tm_kb_i(m.kmemx_pool_kb);
    g_tm_other_kb = other > 0 ? (int)other : 0;
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

// a linux/user process is backed by a scheduler task but carries richer info
// in its LinuxProcess record: a real command name and a heap (brk) region the
// scheduler snapshot's region scan may not fully cover. find the live, non-exited
// LinuxProcess whose task matches this pid so we can enrich the row. (satoru)
static const LinuxProcess* tm_find_live_linux_proc(int native_pid){
    int idx = tm_find_linux_process_index_by_task_pid(native_pid);
    if(idx < 0) return nullptr;
    const LinuxProcess* lp = LinuxSyscall::GetProcess(idx);
    if(!lp || lp->exited || !lp->active) return nullptr;
    return lp;
}

static const int TM_KILL_GROUP_MAX = 64;

// true when the address-space group of `pid` contains `who`. (satoru)
static bool tm_group_has(uint32_t pid, const Process* who){
    if(!who) return false;
    uint32_t g[TM_KILL_GROUP_MAX];
    int n = Scheduler::CollectAddressSpacePids(pid, g, TM_KILL_GROUP_MAX);
    for(int i=0;i<n;i++) if(g[i] == who->pid) return true;
    return false;
}

// post SIGKILL to every live linux record of one thread group, so whichever
// member reaches a syscall boundary first takes the fatal path. the group's
// mixer streams are dropped now: a killed process may sit unreaped (its pulse
// socket still open) for a long time, and it must not hold audio slots. (satoru)
static void tm_post_kill_group(uint32_t pid){
    uint32_t g[TM_KILL_GROUP_MAX];
    int n = Scheduler::CollectAddressSpacePids(pid, g, TM_KILL_GROUP_MAX);
    for(int i=0;i<n;i++){
        AudioServer::ReleaseStreamsOf(g[i]);
        int li = tm_find_linux_process_index_by_task_pid((int)g[i]);
        LinuxProcess* lp = li >= 0 ? LinuxSyscall::GetProcess(li) : nullptr;
        if(lp && lp->active && !lp->exited) LinuxSyscall::PostSignalIdx(li, 9);
    }
}

// end one thread group without quiescing it from here: SIGKILL when it holds
// the task we run inside (it exits at its own boundary), else the async group
// kill kill(2) uses (mark dead now, the scheduler's reaper frees it once every
// member is off cpu). (satoru)
static void tm_kill_group_async(Process* leader, const Process* cur){
    if(!leader) return;
    int li = tm_find_linux_process_index_by_task_pid((int)leader->pid);
    LinuxProcess* lp = li >= 0 ? LinuxSyscall::GetProcess(li) : nullptr;
    SerialLogger::Log("[tmkill] group pid=");
    SerialLogger::LogDec((int)leader->pid);
    SerialLogger::Log(" linux=");
    SerialLogger::LogDec(lp ? (int)lp->pid : -1);
    if(tm_group_has(leader->pid, cur)){
        SerialLogger::Log(" holds the current task: sigkill\r\n");
        tm_post_kill_group(leader->pid);
        return;
    }
    SerialLogger::Log(" async kill\r\n");
    tm_post_kill_group(leader->pid);
    Scheduler::KillProcessGroupAsync(leader->pid, nullptr);
    if(lp && lp->active && !lp->exited){ lp->exit_code = -9; lp->exited = true; }
}

// the async kill applies when the target's group holds the current task or
// the bsp's userspace session root (the shell's launcher sits on that one). it
// also takes the rest of the job: when the target leads its process group,
// every other process in that group (forked children, a stuck bash tool) goes
// too, or they keep the session alive and hold the shell. returns how many
// groups were ended, 0 when the plain synchronous kill should run. (satoru)
static int tm_kill_job_async(Process* task){
    Process* cur  = Scheduler::GetCurrentProcess();
    Process* sess = Userspace::IsActive() ? Userspace::ActiveProcess() : nullptr;
    int li = tm_find_linux_process_index_by_task_pid((int)task->pid);
    LinuxProcess* lp = li >= 0 ? LinuxSyscall::GetProcess(li) : nullptr;
    uint32_t pgid = (lp && lp->pgid && lp->pgid == lp->pid) ? lp->pgid : 0;
    // which groups take part: the target's, plus the job's other leaders. (satoru)
    Process* leaders[TM_KILL_GROUP_MAX];
    int nl = 0;
    leaders[nl++] = task;
    if(pgid){
        for(int i=0;i<LINUX_MAX_PROCS && nl<TM_KILL_GROUP_MAX;i++){
            LinuxProcess* q = LinuxSyscall::GetProcess(i);
            if(!q || !q->active || q->exited || !q->task || q == lp) continue;
            if(q->pgid != pgid || q->task->is_thread()) continue;
            if(tm_group_has(task->pid, q->task)) continue;
            leaders[nl++] = q->task;
        }
    }
    bool special = false;
    for(int i=0;i<nl && !special;i++){
        if(tm_group_has(leaders[i]->pid, cur) || tm_group_has(leaders[i]->pid, sess)) special = true;
    }
    if(!special){
        // the caller's synchronous kill takes the target; the rest of the job
        // still goes, asynchronously. (satoru)
        for(int i=1;i<nl;i++) tm_kill_group_async(leaders[i], cur);
        return 0;
    }
    for(int i=0;i<nl;i++) tm_kill_group_async(leaders[i], cur);
    return nl;
}

static bool tm_terminate_scheduler_process(const TMProcess* proc){
    if(!proc || proc->pid <= 0 || proc->source_kind != TM_PROC_SCHED) return false;

    Process* task = Scheduler::FindProcessByPid((uint32_t)proc->pid);
    if(!task){ tm_set_action_msg("Not running anymore: ", proc->name, nullptr); return false; }

    // kernel tasks ARE the os (gui, input, kvfs, net...) - ending one bricks
    // the session, so the ui refuses instead of letting the kill crash. (satoru)
    if(!task->is_user()){
        tm_set_action_msg("Protected: ", proc->name, " is a kernel process");
        return false;
    }

    // the gui pumps from inside a user thread's syscall on the bsp, and a linux
    // program started from a terminal runs as the shell process's userspace
    // session. if the target's thread group holds the task we are running
    // inside, or the session root the shell is parked under, it cannot be
    // quiesced and reaped from here: the old path refused its own group
    // ("Failed to end") whenever the gui happened to pump from a sibling thread,
    // so a stuck program stayed listed and the shell (and every new terminal)
    // stayed blocked behind it. end it the way kill(pid, SIGKILL) does instead:
    // the pending SIGKILL makes the thread we are inside leave its syscall at
    // once (DispatchInner), the fatal-signal path marks the whole group dead,
    // and the last exit unwinds the shell's session. the rest of the job
    // (children in the same process group) goes the same way. (satoru)
    {
        int njob = tm_kill_job_async(task);
        if(njob > 0){
            char extra[40] = " (SIGKILL sent";
            if(njob > 1){
                char nb[12]; int_to_str(njob, nb, 12);
                sapp(extra, ", ", 40); sapp(extra, nb, 40); sapp(extra, " processes", 40);
            }
            sapp(extra, ")", 40);
            tm_set_action_msg("Ending ", proc->name, extra);
            return true;
        }
    }

    // user process: end the WHOLE address-space group (leader + threads).
    // killing one thread and freeing the shared address space under the
    // siblings still running on other cores crashed the entire os. collect
    // the linux-side records first (the mapping needs live task pointers),
    // quiesce + reap via the scheduler, then release the linux records and
    // drop the dead client's windows so the kill visibly closes the app. (satoru)
    uint32_t member_pids[TM_KILL_GROUP_MAX];
    int      linux_idx[TM_KILL_GROUP_MAX];
    uint32_t linux_pid_at[TM_KILL_GROUP_MAX];
    int      idx_count = 0;
    // pids only, copied under the scheduler lock - the raw Process* variant
    // could hand back pointers a concurrent DestroyProcess heap-frees before
    // we read them. (satoru)
    int n = Scheduler::CollectAddressSpacePids(task->pid, member_pids, TM_KILL_GROUP_MAX);
    if(n <= 0){ tm_set_action_msg("No task group for ", proc->name, nullptr); return false; }
    for(int i=0;i<n;i++){
        int li = tm_find_linux_process_index_by_task_pid((int)member_pids[i]);
        if(li >= 0 && idx_count < TM_KILL_GROUP_MAX){
            LinuxProcess* lp0 = LinuxSyscall::GetProcess(li);
            linux_pid_at[idx_count] = lp0 ? lp0->pid : 0;   // revalidation stamp (satoru)
            linux_idx[idx_count++] = li;
        }
    }

    int kr = Scheduler::KillProcessGroup(task->pid);
    if(kr == 0){
        tm_set_action_msg("Failed to end ", proc->name, nullptr);
        return false;
    }
    if(kr == 2){
        // quiesce timed out: the group is marked dead and queued for the
        // deferred reaper, but a member may still be finishing a syscall on
        // another core - destroying its fd tables NOW would be a teardown uaf.
        // mark the linux records exited and leave the destroy to a later kill
        // attempt / the records simply staying exited. (satoru)
        for(int i=0;i<idx_count;i++){
            LinuxProcess* linux_proc = LinuxSyscall::GetProcess(linux_idx[i]);
            if(linux_proc && linux_proc->pid == linux_pid_at[i]){
                linux_proc->exit_code = -9;
                linux_proc->exited = true;
            }
        }
        tm_set_action_msg("Ending ", proc->name, " (deferred)");
        return true;
    }

    for(int i=0;i<idx_count;i++){
        LinuxProcess* linux_proc = LinuxSyscall::GetProcess(linux_idx[i]);
        // revalidate: the slot can be recycled by an unrelated exec while the
        // kill drained (up to 1.6s) - destroying a recycled slot killed the
        // wrong process's records. (satoru)
        if(!linux_proc || linux_proc->pid != linux_pid_at[i]) continue;
        linux_proc->exit_code = -9;
        linux_proc->exited = true;
        LinuxSyscall::DestroyProcess(linux_idx[i]);
    }
    for(int i=0;i<n;i++) WaylandServer::DropClientsOfPid(member_pids[i]);
    for(int i=0;i<n;i++) AudioServer::ReleaseStreamsOf(member_pids[i]);   // (satoru)

    if(n > 1){
        char extra[32] = " (";
        char nb[12]; int_to_str(n, nb, 12);
        sapp(extra, nb, 32); sapp(extra, " threads)", 32);
        tm_set_action_msg("Ended: ", proc->name, extra);
    } else {
        tm_set_action_msg("Ended: ", proc->name, nullptr);
    }
    return true;
}

// kurono.tmkill=1: the kill rehearsal driven from the gui loop. (satoru)
bool g_tmkill_test = false;
void TaskManagerApp::DebugKillTick(){
    if(!g_tmkill_test) return;
    static int      st = 0;
    static uint32_t t0 = 0, last = 0;
    static int      kpid = 0;
    uint32_t now = Timer::GetRealMs();
    if(st == 0){
        SchedulerProcessSnapshot snaps[TM_MAX_PROCS];
        int n = Scheduler::GetProcessSnapshot(snaps, TM_MAX_PROCS - 1);
        int found = 0;
        for(int i=0;i<n;i++){
            if(!(snaps[i].flags & PROCESS_FLAG_USER) || (snaps[i].flags & PROCESS_FLAG_THREAD)) continue;
            if(scmp(snaps[i].name, "claude") != 0) continue;
            if(!found || (int)snaps[i].pid < found) found = (int)snaps[i].pid;
        }
        if(!found){ t0 = 0; return; }
        if(!t0){ t0 = now; return; }
        if(now - t0 < 8000) return;
        kpid = found;
        TMProcess row = {};
        row.pid = kpid;
        scpy(row.name, "claude", 32);
        row.source_kind = TM_PROC_SCHED;
        Process* cur = Scheduler::GetCurrentProcess();
        SerialLogger::Log("[tmkill] end task pid=");
        SerialLogger::LogDec(kpid);
        SerialLogger::Log(" from current=");
        SerialLogger::Log(cur ? cur->name : "?");
        SerialLogger::Log("\r\n");
        bool ok = tm_terminate_scheduler_process(&row);
        SerialLogger::Log("[tmkill] result ");
        SerialLogger::Log(ok ? "ok: " : "fail: ");
        SerialLogger::Log(g_tm_action_msg);
        SerialLogger::Log("\r\n");
        st = 1; t0 = now; last = now;
        return;
    }
    if(st == 1){
        if(now - last < 1000) return;
        last = now;
        Process* t = Scheduler::FindProcessByPid((uint32_t)kpid);
        int alive_linux = 0;
        char who[96]; who[0] = 0;
        for(int i=0;i<LINUX_MAX_PROCS;i++){
            const LinuxProcess* lp = LinuxSyscall::GetProcess(i);
            if(!lp || !lp->active || lp->exited) continue;
            alive_linux++;
            char nb[12]; int_to_str((int)lp->pid, nb, 12);
            sapp(who, " ", 96); sapp(who, nb, 96);
            sapp(who, lp->task ? (lp->task->is_thread() ? "t" : "p") : "-", 96);
        }
        SerialLogger::Log("[tmkill] +");
        SerialLogger::LogDec((int)((now - t0) / 1000));
        SerialLogger::Log("s listed=");
        SerialLogger::LogDec(t && t->state != Process_Terminated ? 1 : (t ? 2 : 0));
        SerialLogger::Log(" live_linux=");
        SerialLogger::LogDec(alive_linux);
        SerialLogger::Log(" busy=");
        SerialLogger::LogDec(TerminalApp::IsBusy() ? 1 : 0);
        SerialLogger::Log(" live:");
        SerialLogger::Log(who);
        SerialLogger::Log("\r\n");
        // a fresh terminal must be able to start a linux program again. (satoru)
        if(now - t0 >= 5000 && now - t0 < 6000){
            SerialLogger::Log("[tmkill] new terminal: claude\r\n");
            DesktopEnvironment::LaunchTerminal();
            TerminalApp::EnqueueCommand("claude");
        }
        if(now - t0 > 15000) st = 2;
    }
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


static const char* tm_cpu_brand(){
    static CpuInfo ci;
    ci = CPUDetect::GetInfo();
    return ci.brand_string[0] ? ci.brand_string : CPUDetect::GetVendorName();
}
static int tm_installed_mb(){ return (int)(pmm_installed_ram_bytes() >> 20); }

#else  // ring 3 host: /proc and kill(2), see agent_port_B.md for what the kernel does not expose yet (satoru)

static uint64_t tm_now64(){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

// reads a small /proc file whole, returns the length (0 when missing). (satoru)
static int tm_read_file(const char* path, char* buf, int max){
    int fd = open(path, O_RDONLY);
    if(fd < 0) return 0;
    int n = 0;
    while(n < max - 1){
        int r = (int)read(fd, buf + n, (size_t)(max - 1 - n));
        if(r <= 0) break;
        n += r;
    }
    close(fd);
    buf[n] = 0;
    return n;
}
// the value after "Key:" at the start of a line, or nullptr. (satoru)
static const char* tm_key(const char* text, const char* key){
    int kl = slen(key);
    for(const char* p = text; *p; ){
        if(strncmp(p, key, (size_t)kl) == 0 && p[kl] == ':'){
            p += kl + 1;
            while(*p == ' ' || *p == '\t') p++;
            return p;
        }
        while(*p && *p != '\n') p++;
        if(*p == '\n') p++;
    }
    return nullptr;
}
static long tm_num(const char* p){ return p ? strtol(p, nullptr, 10) : 0; }

static int tm_cpu_pct_from_ticks(uint64_t delta_ticks, uint32_t elapsed_ms){
    if(elapsed_ms == 0) return 0;
    uint64_t pct = (delta_ticks * 100ULL + (uint64_t)(elapsed_ms / 2)) / (uint64_t)elapsed_ms;
    return pct > 100ULL ? 100 : (int)pct;
}

static const char* tm_cpu_brand(){
    static char brand[64];
    if(!brand[0]){
        char buf[2048];
        if(tm_read_file("/proc/cpuinfo", buf, sizeof(buf))){
            const char* v = tm_key(buf, "model name");
            if(v){ int i = 0; while(v[i] && v[i] != '\n' && i < 63){ brand[i] = v[i]; i++; } brand[i] = 0; }
        }
        if(!brand[0]) scpy(brand, "CPU", 64);
    }
    return brand;
}
static int g_tm_installed_mb = 0;   // /proc/meminfo KuronoInstalledRam (satoru)
static int tm_installed_mb(){ return g_tm_installed_mb; }

struct TMCpuSample { uint32_t pid; uint64_t ticks; int pct; };
static TMCpuSample g_tm_cpu_samples[TM_MAX_PROCS];
static int g_tm_cpu_sample_count = 0;
static bool g_tm_cpu_samples_ready = false;
static int tm_find_cpu_sample(uint32_t pid){
    for(int i=0;i<g_tm_cpu_sample_count;i++) if(g_tm_cpu_samples[i].pid == pid) return i;
    return -1;
}

// per-thread cpu baselines are separate from the process table: a thread row is
// only built when its process is expanded. (satoru)
static TMCpuSample g_tm_thread_samples[192];
static int g_tm_thread_sample_count = 0;
static bool g_tm_thread_samples_ready = false;
static int tm_find_thread_sample(uint32_t tid){
    for(int i=0;i<g_tm_thread_sample_count;i++) if(g_tm_thread_samples[i].pid == tid) return i;
    return -1;
}

// utime + stime and the state char from one /proc/<pid>/stat line. (satoru)
static void tm_parse_stat(const char* buf, uint64_t* ticks, char* state){
    *ticks = 0; if(state) *state = 'S';
    if(!buf) return;
    const char* q = strrchr(buf, ')');
    if(!q) return;
    q += 2;
    if(state && *q) *state = *q;
    for(int f = 3; f < 14 && *q; f++){ while(*q && *q != ' ') q++; if(*q == ' ') q++; }
    *ticks = (uint64_t)strtoull(q, (char**)&q, 10);
    if(*q == ' ') *ticks += (uint64_t)strtoull(q + 1, nullptr, 10);
}

// sectors read/written for one disk from /proc/diskstats. (satoru)
static void tm_parse_diskstats(const char* buf, const char* name, uint64_t* sr, uint64_t* sw){
    *sr = 0; *sw = 0;
    if(!buf || !name) return;
    char pat[32]; int n = 0;
    pat[n++] = ' ';
    while(*name && n < 30) pat[n++] = *name++;
    pat[n++] = ' '; pat[n] = 0;
    const char* e = strstr(buf, pat);
    if(!e) return;
    e += slen(pat);
    uint64_t v[7] = {0};
    for(int k=0;k<7;k++){ while(*e == ' ') e++; v[k] = (uint64_t)strtoull(e, (char**)&e, 10); }
    *sr = v[2];
    *sw = v[6];
}

static bool tm_terminate_scheduler_process(const TMProcess* proc){
    if(!proc || proc->pid <= 0) return false;
    if(proc->is_kernel_proc){ tm_set_action_msg("Protected: ", proc->name, " is a kernel process"); return false; }
    if(kill((pid_t)proc->pid, SIGKILL) != 0){ tm_set_action_msg("Failed to end ", proc->name, nullptr); return false; }
    tm_set_action_msg("Ended: ", proc->name, nullptr);
    return true;
}

void TaskManagerApp::Init(){
    current_tab=TM_PROCESSES;
    proc_count=0; window_count=0; selected_proc=-1; selected_window=-1; scroll_offset=0;
    sort_col=TM_SORT_CPU; sort_asc=false;
    cpu_cores = 1;
    g_tm_last_render_ms = tm_now64();
    for(int i=0;i<60;i++){cpu_history[i]=0;mem_history[i]=0;}
    hist_idx=0; tick_counter=0;
    InitServices();
    RefreshProcesses();
}

// services mirror the kernel-task rows /proc exposes (non-user tasks). (satoru)
void TaskManagerApp::InitServices(){
    service_count = 0;
    for(int i=0;i<proc_count && service_count<32;i++){
        if(procs[i].user[0] == 'u') continue;           // user processes live on the Processes tab
        if(procs[i].source_kind == TM_PROC_SYNTH) continue;
        TMService* sv = &services[service_count++];
        scpy(sv->name, procs[i].name, 32);
        if(procs[i].state[0] == 'R')      scpy(sv->status, "Running", 12);
        else if(procs[i].state[0] == 'Z') scpy(sv->status, "Stopped", 12);
        else                              scpy(sv->status, "Sleeping", 12);
        scpy(sv->type, procs[i].is_kernel_proc ? "Kernel" : "System", 16);
        sv->pid = procs[i].pid;
    }
}

// no window list reaches a wayland client; the Windows tab stays empty. (satoru)
void TaskManagerApp::RefreshWindows(){ window_count = 0; selected_window = -1; }

void TaskManagerApp::RefreshProcesses(){
    // keep the selected row across the rebuild: the list is re-sorted every
    // refresh, so an index would drift to whatever process took that slot and
    // End Task would end the wrong one. match by pid (synthetic rows by name). (satoru)
    int keep_pid = -2; char keep_name[32] = {0};
    if(selected_proc >= 0 && selected_proc < proc_count){
        keep_pid = procs[selected_proc].pid;
        scpy(keep_name, procs[selected_proc].name, 32);
    }
    proc_count = 0;
    g_thread_count = 0;
    uint64_t now_ms = tm_now64();
    static uint64_t last_sample_ms = 0;
    uint32_t elapsed_ms = (last_sample_ms == 0 || now_ms <= last_sample_ms) ? 500u : (uint32_t)(now_ms - last_sample_ms);
    const bool fresh = (last_sample_ms == 0 || elapsed_ms >= 400u);
    if(fresh) last_sample_ms = now_ms;

    // pids: the numeric directories of /proc. kurono's /proc does not list them yet, so fall back to
    // probing the first few hundred. (satoru)
    int pids[TM_MAX_PROCS]; int np = 0;
    DIR* d = opendir("/proc");
    if(d){
        struct dirent* de;
        while((de = readdir(d)) && np < TM_MAX_PROCS - 1){
            if(de->d_name[0] >= '1' && de->d_name[0] <= '9') pids[np++] = atoi(de->d_name);
        }
        closedir(d);
    }
    if(np == 0){
        char path[40], probe[64];
        for(int pid = 1; pid <= 256 && np < TM_MAX_PROCS - 1; pid++){
            path[0] = 0; sapp(path, "/proc/", 40); char nb[12]; int_to_str(pid, nb, 12); sapp(path, nb, 40); sapp(path, "/status", 40);
            if(tm_read_file(path, probe, sizeof(probe)) > 0) pids[np++] = pid;
        }
    }

    static int cpu_pcts[TM_MAX_PROCS];
    int user_rss_kb = 0, total_busy = 0;
    for(int i=0;i<np && proc_count<TM_MAX_PROCS-1;i++){
        char path[64], nb[16], buf[2048];
        int_to_str(pids[i], nb, 12);
        path[0] = 0; sapp(path, "/proc/", 64); sapp(path, nb, 64); sapp(path, "/status", 64);
        if(!tm_read_file(path, buf, sizeof(buf))) continue;
        TMProcess* p = &procs[proc_count++];
        p->pid = pids[i];
        const char* v = tm_key(buf, "Name");
        { int k = 0; if(v) while(v[k] && v[k] != '\n' && k < 31){ p->name[k] = v[k]; k++; } p->name[k] = 0; if(!k) scpy(p->name, "process", 32); }
        {
            bool kern0 = tm_num(tm_key(buf, "KuronoKernelTask")) != 0;
            if(kern0 && tm_is_hidden_task(p->name)){ proc_count--; continue; }
            char disp[32]; int icon = TM_ICON_GENERIC, grp = -1;
            tm_name_map(p->name, disp, 32, &icon, &grp);
            scpy(p->name, disp, 32);
            p->icon = icon;
            p->group = (uint8_t)(grp >= 0 ? grp : (kern0 ? 1 : 0));
        }
        v = tm_key(buf, "State");
        const char sc = v ? *v : 'S';
        scpy(p->state, sc=='R' ? "RUNNING" : (sc=='Z' ? "ZOMBIE" : (sc=='T' ? "STOPPED" : "SLEEPING")), 12);
        // the exact scheduler state (READY vs RUNNING vs SLEEPING) is a kurono
        // line because linux's state char collapses ready and running to R. (satoru)
        const char* ks = tm_key(buf, "KuronoState");
        if(ks){
            int stv = atoi(ks);
            scpy(p->state, stv==1 ? "RUNNING" : (stv==0 ? "READY" : (stv==4 ? "EXITED" : (stv==2 ? "BLOCKED" : "SLEEPING"))), 12);
        }
        p->mem_kb = (int)tm_num(tm_key(buf, "VmRSS"));
        p->shared_kb = (int)tm_num(tm_key(buf, "RssShmem"));
        p->threads = (int)tm_num(tm_key(buf, "Threads")); if(p->threads < 1) p->threads = 1;
        // kernel tasks are the non-user rows /proc now exposes. their stack is in
        // VmRSS and the taskmem charge is a kurono-specific line. (satoru)
        bool kern = tm_num(tm_key(buf, "KuronoKernelTask")) != 0;
        scpy(p->user, kern ? "kernel" : "user", 16);
        p->is_kernel_proc = kern;
        p->priority = (int)tm_num(tm_key(buf, "KuronoPriority"));
        p->io_read_kb = 0; p->io_write_kb = 0; p->flags = 0;
        p->source_kind = TM_PROC_SCHED;
        p->stack_kb = (uint32_t)tm_num(tm_key(buf, "KuronoStack"));
        p->stack_cap_kb = (uint32_t)tm_num(tm_key(buf, "KuronoStackCap"));
        p->stack_grow_count = (uint32_t)tm_num(tm_key(buf, "KuronoStackGrows"));
        p->prio_tier = (uint8_t)tm_num(tm_key(buf, "KuronoPrioTier"));
        // in-kernel rows add the per-task kernel heap to the resident figure. (satoru)
        int kheap_kb = (int)tm_num(tm_key(buf, "KuronoTaskHeap"));
        kheap_kb += (int)tm_num(tm_key(buf, "KuronoTaskFrames"));
        if(!kern) user_rss_kb += p->mem_kb;
        p->mem_kb += kheap_kb;
        // cpu: utime + stime (clock ticks of 10 ms) from /proc/<pid>/stat. (satoru)
        uint64_t ticks = 0;
        path[0] = 0; sapp(path, "/proc/", 64); sapp(path, nb, 64); sapp(path, "/stat", 64);
        if(tm_read_file(path, buf, sizeof(buf))){
            char st = 'S'; tm_parse_stat(buf, &ticks, &st);
            if(!ks && (st != 'S' || p->state[0] == 'S'))
                scpy(p->state, st=='R' ? "RUNNING" : (st=='Z' ? "ZOMBIE" : (st=='T' ? "STOPPED" : "SLEEPING")), 12);
        }
        int si = tm_find_cpu_sample((uint32_t)p->pid);
        uint64_t prev = (g_tm_cpu_samples_ready && si >= 0) ? g_tm_cpu_samples[si].ticks : ticks;
        uint64_t delta = ticks >= prev ? (ticks - prev) * 10ULL : 0;   // ticks -> ms (satoru)
        int pct = fresh ? (g_tm_cpu_samples_ready ? tm_cpu_pct_from_ticks(delta, elapsed_ms) : 0) : (si >= 0 ? g_tm_cpu_samples[si].pct : 0);
        cpu_pcts[proc_count-1] = pct;
        p->cpu_pct = tm_clamp(pct, 0, 100);
        p->cpu_ms_total = (uint32_t)((ticks > 0xFFFFFFFFull / 10ull) ? 0xFFFFFFFFu : ticks * 10ULL);
        total_busy += p->cpu_pct;

        // thread rows: /proc/<pid>/task lists the thread group even though only
        // the leader is a top-level /proc entry. (satoru)
        if(p->threads > 1 && g_thread_count < 192){
            char tdir[64];
            tdir[0] = 0; sapp(tdir, "/proc/", 64); sapp(tdir, nb, 64); sapp(tdir, "/task", 64);
            DIR* td = opendir(tdir);
            if(td){
                struct dirent* te;
                while((te = readdir(td)) && g_thread_count < 192){
                    if(te->d_name[0] < '1' || te->d_name[0] > '9') continue;
                    int tid = atoi(te->d_name);
                    if(tid == pids[i]) continue;
                    char tn[16]; int_to_str(tid, tn, 16);
                    char tpath[96], tbuf[1024];
                    tpath[0] = 0; sapp(tpath, tdir, 96); sapp(tpath, "/", 96); sapp(tpath, tn, 96); sapp(tpath, "/stat", 96);
                    if(!tm_read_file(tpath, tbuf, sizeof(tbuf))) continue;
                    uint64_t tticks = 0; char tst = 'S';
                    tm_parse_stat(tbuf, &tticks, &tst);
                    int tsi = tm_find_thread_sample((uint32_t)tid);
                    uint64_t tprev = (g_tm_thread_samples_ready && tsi >= 0) ? g_tm_thread_samples[tsi].ticks : tticks;
                    uint64_t tdelta = tticks >= tprev ? (tticks - tprev) * 10ULL : 0;
                    int tpct = fresh ? (g_tm_thread_samples_ready ? tm_cpu_pct_from_ticks(tdelta, elapsed_ms) : 0)
                                     : (tsi >= 0 ? g_tm_thread_samples[tsi].pct : 0);
                    TMThread& th = g_threads[g_thread_count++];
                    th.leader = (uint32_t)pids[i]; th.pid = (uint32_t)tid;
                    th.cpu_pct = tm_clamp(tpct, 0, 100);
                    scpy(th.state, tst=='R' ? "RUNNING" : (tst=='Z' ? "ZOMBIE" : (tst=='T' ? "STOPPED" : "SLEEPING")), 12);
                    if(fresh){
                        if(tsi >= 0){
                            g_tm_thread_samples[tsi].ticks = tticks;
                            g_tm_thread_samples[tsi].pct = th.cpu_pct;
                        } else if(g_tm_thread_sample_count < 192){
                            g_tm_thread_samples[g_tm_thread_sample_count].pid = (uint32_t)tid;
                            g_tm_thread_samples[g_tm_thread_sample_count].ticks = tticks;
                            g_tm_thread_samples[g_tm_thread_sample_count].pct = th.cpu_pct;
                            g_tm_thread_sample_count++;
                        }
                    }
                }
                closedir(td);
            }
        }
    }
    if(fresh){
        g_tm_cpu_sample_count = proc_count;
        for(int i=0;i<proc_count;i++){ g_tm_cpu_samples[i].pid = (uint32_t)procs[i].pid; g_tm_cpu_samples[i].ticks = (uint64_t)procs[i].cpu_ms_total / 10ULL; g_tm_cpu_samples[i].pct = cpu_pcts[i]; }
        g_tm_cpu_samples_ready = true;
        g_tm_thread_samples_ready = true;
    }
    RefreshWindows();
    SortProcesses();

    // whole-system cpu and the per-core bars from /proc/stat deltas. (satoru)
    {
        static uint64_t pb[33], pt[33];
        static uint64_t pkern = 0;
        uint64_t kticks = 0;
        char buf[4096];
        int n = 0;
        if(tm_read_file("/proc/stat", buf, sizeof(buf))){
            for(const char* l = buf; *l; ){
                if(l[0]=='c' && l[1]=='p' && l[2]=='u'){
                    int idx = (l[3] == ' ') ? -1 : atoi(l + 3);
                    const char* q = l + 3;
                    while(*q >= '0' && *q <= '9') q++;   // the cpu index of "cpuN", none for the aggregate (satoru)
                    while(*q == ' ') q++;
                    uint64_t v[8] = {0}; const char* e = q;
                    for(int k=0;k<8;k++){ v[k] = (uint64_t)strtoull(e, (char**)&e, 10); }
                    uint64_t tot = 0; for(int k=0;k<8;k++) tot += v[k];
                    uint64_t idle = v[3] + v[4], busy = tot - idle;
                    int slot = idx < 0 ? 32 : (idx < 32 ? idx : -1);
                    if(slot >= 0){
                        int pct = 0;
                        if(fresh && tot > pt[slot]) pct = (int)(((busy - pb[slot]) * 100ULL) / (tot - pt[slot]));
                        if(fresh){ pb[slot] = busy; pt[slot] = tot; if(slot == 32) cpu_usage = tm_clamp(pct, 0, 100); else g_core_pct[slot] = tm_clamp(pct, 0, 100); }
                        if(slot < 32 && slot + 1 > n) n = slot + 1;
                    }
                } else if(tm_starts_with(l, "kurono_kernel ")){
                    kticks = (uint64_t)strtoull(l + 14, nullptr, 10);
                }
                while(*l && *l != '\n') l++;
                if(*l == '\n') l++;
            }
        }
        g_core_n = n > 0 ? n : 1;
        cpu_cores = g_core_n;
        if(cpu_usage == 0 && total_busy > 0 && !g_tm_cpu_samples_ready) cpu_usage = tm_clamp(total_busy, 0, 100);
        // kernel time no task row owns: its own System row. (satoru)
        {
            uint64_t d = kticks >= pkern ? kticks - pkern : 0;
            int kpct = 0;
            if(fresh && g_tm_cpu_samples_ready && elapsed_ms > 0 && g_core_n > 0)
                kpct = tm_clamp((int)((d * 10ull * 100ull) / ((uint64_t)elapsed_ms * (uint64_t)g_core_n)), 0, 100);
            if(fresh) pkern = kticks;
            if(proc_count < TM_MAX_PROCS-1){
                TMProcess* p = &procs[proc_count++];
                scpy(p->name, "Kernel (unowned)", 32);
                p->pid = 0; p->cpu_pct = kpct; p->mem_kb = 0; p->shared_kb = 0; p->threads = 0;
                scpy(p->state, kpct > 0 ? "RUNNING" : "IDLE", 12);
                scpy(p->user, "kernel", 16);
                p->priority = 0; p->io_read_kb = 0; p->io_write_kb = 0; p->flags = 0;
                p->source_kind = TM_PROC_SYNTH;
                p->stack_kb = 0; p->stack_cap_kb = 0;
                p->cpu_ms_total = (uint32_t)(kticks * 10ull > 0xFFFFFFFFull ? 0xFFFFFFFFu : kticks * 10ull);
                p->stack_grow_count = 0; p->prio_tier = 0; p->is_kernel_proc = true;
                p->icon = TM_ICON_GENERIC; p->group = 1;
            }
        }
        // per-cpu history for the performance graphs. (satoru)
        if(fresh){
            for(int c=0;c<g_core_n && c<32;c++) g_core_hist[c][g_core_hist_idx % 60] = g_core_pct[c];
            g_core_hist_idx++;
        }
    }
    // network and disk rates from /proc/net/dev and /proc/diskstats. (satoru)
    {
        static uint64_t pr_rx = 0, pr_tx = 0, pr_sr = 0, pr_sw = 0;
        char buf[2048]; uint64_t rx = 0, tx = 0;
        if(tm_read_file("/proc/net/dev", buf, sizeof(buf))){
            const char* e = strstr(buf, "eth0:");
            if(e){ e += 5; rx = (uint64_t)strtoull(e, (char**)&e, 10); for(int k=0;k<7;k++) strtoull(e, (char**)&e, 10); tx = (uint64_t)strtoull(e, nullptr, 10); }
        }
        uint64_t sr = 0, sw = 0;
        if(tm_read_file("/proc/diskstats", buf, sizeof(buf))) tm_parse_diskstats(buf, "nvme0n1", &sr, &sw);
        if(fresh && g_tm_cpu_samples_ready && elapsed_ms > 0){
            g_net_rx_rate = (int)(((rx - pr_rx) / 1024ULL) * 1000ULL / elapsed_ms);
            g_net_tx_rate = (int)(((tx - pr_tx) / 1024ULL) * 1000ULL / elapsed_ms);
            g_dsk_rd_rate = (int)((((sr - pr_sr) * 512ULL) / 1024ULL) * 1000ULL / elapsed_ms);
            g_dsk_wr_rate = (int)((((sw - pr_sw) * 512ULL) / 1024ULL) * 1000ULL / elapsed_ms);
        }
        if(fresh){ pr_rx = rx; pr_tx = tx; pr_sr = sr; pr_sw = sw; }
        g_dsk_rd_hist[g_io_idx % 60] = g_dsk_rd_rate; g_dsk_wr_hist[g_io_idx % 60] = g_dsk_wr_rate;
        g_net_rx_hist[g_io_idx % 60] = g_net_rx_rate; g_net_tx_hist[g_io_idx % 60] = g_net_tx_rate;
        g_io_idx++;
        net_rx_kb = (int)(rx / 1024); net_tx_kb = (int)(tx / 1024);
        disk_read_kb = (int)((sr * 512ULL) / 1024ULL); disk_write_kb = (int)((sw * 512ULL) / 1024ULL);
    }
    // system memory from /proc/meminfo; the split mirrors the in-kernel card. (satoru)
    {
        char buf[2048];
        if(tm_read_file("/proc/meminfo", buf, sizeof(buf))){
            mem_total_kb = (int)tm_num(tm_key(buf, "MemTotal"));
            mem_free_kb  = (int)tm_num(tm_key(buf, "MemFree"));
            mem_heap_kb  = (int)tm_num(tm_key(buf, "KuronoHeapArena"));
            g_tm_kmx_kb  = (int)tm_num(tm_key(buf, "KuronoKmemx"));
            g_tm_installed_mb = (int)(tm_num(tm_key(buf, "KuronoInstalledRam")) / 1024);
        }
        if(mem_total_kb < 1024) mem_total_kb = 1024;
        mem_used_kb = mem_total_kb - mem_free_kb; if(mem_used_kb < 0) mem_used_kb = 0;
        mem_cached_kb = mem_free_kb;
        g_tm_user_kb = user_rss_kb;
        int64_t other = (int64_t)mem_used_kb - (int64_t)g_tm_user_kb - (int64_t)mem_heap_kb - (int64_t)g_tm_kmx_kb;
        g_tm_other_kb = other > 0 ? (int)other : 0;
    }
    {
        char buf[64];
        if(tm_read_file("/proc/uptime", buf, sizeof(buf))) uptime_sec = atoi(buf);
        else uptime_sec = (int)(tm_now64() / 1000ULL);
    }
    cpu_history[hist_idx % 60] = tm_clamp(cpu_usage, 0, 100);
    mem_history[hist_idx % 60] = tm_clamp((int)(((int64_t)mem_used_kb * 100) / (mem_total_kb > 0 ? mem_total_kb : 1)), 0, 100);
    if(hist_idx == 0){ for(int i=1;i<60;i++) mem_history[i] = mem_history[0]; }
    hist_idx++;

    if(keep_pid != -2){
        selected_proc = -1;
        for(int i=0;i<proc_count;i++){
            bool match = (keep_pid == 0) ? (procs[i].pid == 0 && scmp(procs[i].name, keep_name) == 0)
                                         : (procs[i].pid == keep_pid);
            if(match){ selected_proc = i; break; }
        }
    }
    if(selected_proc >= proc_count) selected_proc = -1;
    if(selected_proc < -1) selected_proc = -1;
}

void TaskManagerApp::KillProcess(int idx){
    if(idx < 0 || idx >= proc_count) return;
    TMProcess proc = procs[idx];
    action_menu_open = false;
    if(proc.source_kind == TM_PROC_SCHED && proc.pid > 0 && tm_terminate_scheduler_process(&proc)){
        kui::Log("tasks-kill-pid", proc.name);
        RefreshProcesses();
        if(selected_proc >= proc_count) selected_proc = proc_count - 1;
        return;
    }
    kui::Log("tasks-kill-denied", proc.name);
}
void TaskManagerApp::RestartProcess(int idx){
    if(idx < 0 || idx >= proc_count) return;
    action_menu_open = false;
    kui::Log("tasks-restart-denied", procs[idx].name);   // no launcher in ring 3 (satoru)
}
void TaskManagerApp::KillWindow(int idx){ (void)idx; action_menu_open = false; }
void TaskManagerApp::RestartWindow(int idx){ (void)idx; action_menu_open = false; }

#endif
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
int         TaskManagerApp::mem_free_kb    = 0;
int         TaskManagerApp::mem_heap_kb    = 0;
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
int         TaskManagerApp::last_list_vis  = 0;
bool        TaskManagerApp::action_menu_open = false;
int         TaskManagerApp::action_menu_row  = -1;
TMTab       TaskManagerApp::action_menu_tab  = TM_PROCESSES;
int         TaskManagerApp::action_menu_x    = 0;
int         TaskManagerApp::action_menu_y    = 0;
int         TaskManagerApp::win_id           = -1;

#if !defined(KUI_BACKEND_WAYLAND)
void TaskManagerApp::Init(){
    current_tab=TM_PROCESSES;
    proc_count=0; window_count=0; selected_proc=-1; selected_window=-1; scroll_offset=0;
    sort_col=TM_SORT_CPU; sort_asc=false;
    cpu_cores=CPUDetect::GetCoreCount();
    if(cpu_cores < 1) cpu_cores = 1;
    g_tm_last_render_ms = tm_now64();   // the window is about to be drawn: allow the first walk (satoru)
    tm_query_memory_usage_kb(mem_total_kb, mem_used_kb, mem_free_kb, mem_heap_kb);
    if(mem_total_kb < 1024) mem_total_kb = 1024;   // divide-by-zero guard only; the pmm total is never below this once it is up (satoru)
    for(int i=0;i<60;i++){cpu_history[i]=0;mem_history[i]=0;}
    hist_idx=0; tick_counter=0;
    InitServices();
    RefreshProcesses();
}
#endif

// the toolkit handler of the Task Manager window. all state is static in TaskManagerApp, as before,
// so a second window shows the same lists. a 500 ms timer keeps the graphs and the 1 Hz refresh
// running while the window is open, the job the gui loop's IsOpen() check used to do. (satoru)
static int g_tm_windows = 0;
static const int TM_RCLICK = -4;   // Input() key value for a right click (ctrl-d is ascii 4) (satoru)

class TMApp : public kui::AppHandler {
public:
    ~TMApp() override { if(g_tm_windows > 0) g_tm_windows--; TaskManagerApp::win_id = g_tm_windows ? 1 : -1; }
    void OnCreate(kui::Window& w) override {
        g_tm_windows++; TaskManagerApp::win_id = 1;
        kui::StartTimer(&w, 500, true);
    }
    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t) override {
        TaskManagerApp::Render(w, c);
        g_cv = nullptr;
    }
    bool OnEvent(kui::Window& w, const kui::Event& e) override {
        using kui::EventType;
        switch(e.type){
        case EventType::Click:        // left press, the old wm event 1 (satoru)
            TaskManagerApp::Input(w, e.x, e.y, true, 0); return true;
        case EventType::PointerDown:  // right press, the old wm events 4 and 6 (satoru)
            if(e.button == kui::Button::Right){ TaskManagerApp::Input(w, e.x, e.y, false, TM_RCLICK); return true; }
            return false;
        case EventType::Scroll:       // the old event 3 passed notches, positive = wheel up, the same sign kui uses (satoru)
            TaskManagerApp::Scroll(e.scroll_steps); return true;   // whole notches, positive = wheel up like the old p1 (satoru)
        case EventType::KeyDown: {
            int k = e.key;
            if(k==kui::K_SHIFT || k==kui::K_CTRL || k==kui::K_ALT || k==kui::K_SUPER) return false;
            TaskManagerApp::Input(w, 0, 0, false, k);
            return true; }
        case EventType::Timer:
        case EventType::Resize:
        case EventType::ThemeChanged:
        case EventType::Focus:
        case EventType::Blur:         return true;
        default: return false;
        }
    }
};

// content size = the old 720 x 590 window minus the 1 px borders and the 36 px title bar (satoru)
static const kui::WindowSpec kTmSpec = { "Task Manager", "taskmgr", 718, 553, 560, 380, -1, -1, true, true, 0, -1 };

static kui::AppHandler* MakeTaskManager(){ TaskManagerApp::Init(); return new TMApp; }

#if !defined(KUI_BACKEND_WAYLAND)
int TaskManagerApp::Open(){
    TaskMem::AppScope _tms(TaskMem::APP_TASKMGR);   // kurono.taskmem=1: charge to the app (satoru)
    kui::AppHandler* a = MakeTaskManager();
    RuntimeLog::LogAppEvent("tasks", "open");
    // taller (590) so the Performance tab's four device panes fit fully under
    // the disk/network/system rows instead of overflowing the panel + the
    // content area (the old 420-tall window clipped them). (satoru)
    kui::Window* w = kui::OpenWindow(kTmSpec, a);
    if(!w){ delete a; return -1; }
    return 1;
}
#endif

// true while a Task Manager window is open. (satoru)
bool TaskManagerApp::IsOpen(){ return g_tm_windows > 0; }

#if !defined(KUI_BACKEND_WAYLAND)
void TaskManagerApp::InitServices(){
    // real kernel tasks from the live scheduler - name, real pid, real state -
    // instead of the old hardcoded placeholder table (fake pids, everything
    // pinned "Running"). refreshed every Tick alongside the process list. (satoru)
    service_count = 0;
    SchedulerProcessSnapshot snaps[TM_MAX_PROCS];
    int cnt = Scheduler::GetProcessSnapshot(snaps, TM_MAX_PROCS - 1);
    for(int i=0;i<cnt && service_count<32;i++){
        const SchedulerProcessSnapshot& s = snaps[i];
        if(s.pid == 0) continue;                                  // idle task (satoru)
        if((s.flags & PROCESS_FLAG_USER) != 0) continue;          // user procs live on Processes (satoru)
        TMService* sv = &services[service_count++];
        scpy(sv->name, s.name[0] ? s.name : "ktask", 32);
        if(s.state == Process_Running || s.state == Process_Ready)
            scpy(sv->status, "Running", 12);
        else if(s.state == Process_Terminated)
            scpy(sv->status, "Stopped", 12);
        else
            scpy(sv->status, "Sleeping", 12);
        scpy(sv->type, s.is_kernel_proc ? "Kernel" : "System", 16);
        sv->pid = (int)s.pid;
    }
}
#endif

void TaskManagerApp::Tick(){
#if !defined(KUI_BACKEND_WAYLAND)
    TaskMem::AppScope _tms(TaskMem::APP_TASKMGR);   // kurono.taskmem=1: charge to the app (satoru)
#endif
    // Time-based, not frame-based: refresh process list at ~1 Hz so we
    // don't burn CPU at high frame rates and stay smooth at low ones.
    static uint32_t last_refresh_ms = 0;
    uint32_t now = kui::NowMs();   // monotonic; the wall clock can step when the rtc syncs (satoru)
    if(last_refresh_ms != 0 && (now - last_refresh_ms) < 1000) return;
    last_refresh_ms = now;
    tick_counter++;
    RefreshProcesses();
    InitServices();   // services mirror the live kernel tasks (satoru)
}

// scroll the list on the active tab. positive delta = scroll toward the end.
// the offset is clamped against the row count of whichever list this tab shows
// and the last-rendered viewport height; the renderers re-clamp too. (satoru)
void TaskManagerApp::Scroll(int delta){
    if(delta == 0) return;
    int count = (current_tab == TM_WINDOWS) ? window_count : (current_tab == TM_PROCESSES ? g_row_count : proc_count);
    int vis = last_list_vis > 0 ? last_list_vis : 1;
    int max_off = count - vis; if(max_off < 0) max_off = 0;
    // wheel deltas come in as small signed steps; one notch == one row. (satoru)
    scroll_offset -= delta;
    if(scroll_offset > max_off) scroll_offset = max_off;
    if(scroll_offset < 0) scroll_offset = 0;
}

#if !defined(KUI_BACKEND_WAYLAND)
void TaskManagerApp::RefreshProcesses(){
    // keep the selected row across the rebuild (see the ring 3 twin). (satoru)
    int keep_pid = -2; char keep_name[32] = {0};
    if(selected_proc >= 0 && selected_proc < proc_count){
        keep_pid = procs[selected_proc].pid;
        scpy(keep_name, procs[selected_proc].name, 32);
    }
    proc_count=0;
    g_thread_count=0;
    // monotonic boot clock (pit-driven) for cpu% sampling + uptime. Time::GetTicks
    // is wall-clock-derived (utc ms) and produces a nonsense "uptime" of hundreds
    // of hours; Scheduler::NowMs counts from boot. (satoru)
    uint64_t now_ms = Timer::GetRealMs64();   // tsc clock, the same base the busy samplers use (satoru)
    static uint64_t last_sample_ms = 0;
    uint32_t elapsed_ms = (last_sample_ms == 0 || now_ms <= last_sample_ms)
                              ? 500u : (uint32_t)(now_ms - last_sample_ms);
    // a refresh triggered by a click lands a few ms after the last one; a rate over
    // that window is pure quantisation noise. only re-baseline on a full window and
    // keep showing the previous percentages otherwise. (satoru)
    const bool fresh = (last_sample_ms == 0 || elapsed_ms >= 400u);
    if(fresh) last_sample_ms = now_ms;

    NetworkInterface* eth = Network::GetInterface("eth0");
    int current_net_rx_kb = eth ? (int)(eth->rx_bytes / 1024) : 0;
    int current_net_tx_kb = eth ? (int)(eth->tx_bytes / 1024) : 0;
    net_rx_kb = current_net_rx_kb;
    net_tx_kb = current_net_tx_kb;

    SchedulerProcessSnapshot snapshots[TM_MAX_PROCS];
    int snapshot_count = Scheduler::GetProcessSnapshot(snapshots, TM_MAX_PROCS - 1);
    int total_busy_cpu = 0;

    // per-snapshot cpu% first (threads included), so thread activity can roll
    // up into its leader row and the overall figure counts every task. (satoru)
    static int cpu_pcts[TM_MAX_PROCS];
    for(int i=0;i<snapshot_count && i<TM_MAX_PROCS;i++){
        const SchedulerProcessSnapshot& snap = snapshots[i];
        int sample_idx = tm_find_cpu_sample(snap.pid);
        uint64_t prev_ticks = snap.cpu_ticks_total;
        if(g_tm_cpu_samples_ready && sample_idx >= 0){
            prev_ticks = g_tm_cpu_samples[sample_idx].cpu_ticks_total;
        }
        uint64_t delta_ticks = 0;
        if(snap.cpu_ticks_total >= prev_ticks) delta_ticks = snap.cpu_ticks_total - prev_ticks;
        if(fresh) cpu_pcts[i] = g_tm_cpu_samples_ready ? tm_cpu_pct_from_ticks(delta_ticks, elapsed_ms) : 0;
        else      cpu_pcts[i] = sample_idx >= 0 ? g_tm_cpu_samples[sample_idx].pct : 0;
        if(snap.pid != 0) total_busy_cpu += cpu_pcts[i];
    }

    // page-table walks are the only costly part of the memory column: run them only
    // while this window was drawn in the last 3 s (the desktop loop also refreshes
    // every 5 s with the window closed), and stop paying for new walks once one
    // refresh has spent 12 ms on them (rows then show their last walk). (satoru)
    const bool tm_visible = (now_ms - g_tm_last_render_ms) < 3000u;
    const uint64_t walk_t0 = Timer::GetRealMs64();
    const bool taskmem = TaskMem::On();
    g_tmrow_n = taskmem ? TaskMem::Snapshot(g_tmrows, 1024, &g_tmtot) : 0;
    int tm_user_kheap_kb = 0;   // kernel heap charged to user processes, for the shared row (satoru)

    for(int i=0;i<snapshot_count && proc_count<TM_MAX_PROCS-1;i++){
        const SchedulerProcessSnapshot& snap = snapshots[i];
        // skip the kernel idle task (pid 0): the scheduler charges it a tick
        // whenever the cpu would otherwise halt, so it reads as ~100% and is not
        // a process the user cares about. idle is reflected in the overall cpu
        // figure on the performance tab instead. (satoru)
        if(snap.pid == 0) continue;
        // clone threads fold into their leader's row (real thread counts +
        // summed cpu below) instead of appearing as duplicate processes. (satoru)
        if(snap.flags & PROCESS_FLAG_THREAD) continue;
        int cpu_pct = (i < TM_MAX_PROCS) ? cpu_pcts[i] : 0;

        // a scheduler task may also be a linux/user process - if so, take its
        // real command name and add its heap to the rss. user processes are
        // tagged via PROCESS_FLAG_USER; everything else is a kernel task. (satoru)
        bool is_user = (snap.flags & PROCESS_FLAG_USER) != 0;
        const LinuxProcess* lp = is_user ? tm_find_live_linux_proc((int)snap.pid) : nullptr;
        if(!is_user && tm_is_hidden_task(lp && lp->name[0] ? lp->name : snap.name)) continue;

        TMProcess* p = &procs[proc_count++];
        p->pid = (int)snap.pid;
        if(lp && lp->name[0]) scpy(p->name, lp->name, 32);
        else                  scpy(p->name, snap.name[0] ? snap.name : "process", 32);
        {
            char disp[32]; int icon = TM_ICON_GENERIC, grp = -1;
            tm_name_map(p->name, disp, 32, &icon, &grp);
            scpy(p->name, disp, 32);
            p->icon = icon;
            p->group = (uint8_t)(grp >= 0 ? grp : (is_user ? 0 : 1));
        }
        // per-process memory: the scheduler snapshot already accounts kernel
        // stack + user stack + mapped regions (rss-style). for user processes
        // fold in the heap (brk) so the figure is meaningful, not just the
        // stack. kernel tasks keep their committed kernel-stack cost. (satoru)
        // memory column = resident set. a user process is its address space's
        // present user pages, counted from the page tables (MemAcct, cached
        // ~1 s, only while this window is drawn); the old figure was the sum
        // of the mapped region SIZES (virtual) plus the brk window, which made
        // firefox read tens of gb. a kernel task is its committed kernel stack.
        // clone threads share the leader's space, so they add nothing here. (satoru)
        int mem_kb = 0, shr_kb = 0;
        if(is_user){
            MemAcct::SpaceMem sm;
            bool walk_ok = tm_visible && (Timer::GetRealMs64() - walk_t0) < 12u;
            if(snap.address_space && MemAcct::QuerySpace(snap.address_space, 900u, &sm, walk_ok)){
                mem_kb = tm_kb_i(sm.rss_kb);
                shr_kb = tm_kb_i(sm.shared_kb);
            }
        } else {
            mem_kb = snap.stack_kb ? (int)snap.stack_kb : (int)snap.memory_kb;
            if(taskmem) mem_kb = (int)snap.stack_kb + tm_task_kb(snap.pid);   // stack + own heap + own frames (satoru)
        }
        p->threads = 1;
        // real thread count for user processes: the address-space group size
        // (leader + clone threads); the siblings' cpu + memory roll up here. (satoru)
        if(is_user){
            // pids copied under the scheduler lock - reading grp[g]->pid after
            // the pointer variant returned raced a concurrent DestroyProcess
            // heap-free (uaf read of a recycled block). (satoru)
            uint32_t gpids[TM_KILL_GROUP_MAX];
            int gn = Scheduler::CollectAddressSpacePids(snap.pid, gpids, TM_KILL_GROUP_MAX);
            if(gn > 1){
                p->threads = gn;
                for(int g=0; g<gn; g++){
                    uint32_t gpid = gpids[g];
                    if(gpid == snap.pid) continue;
                    for(int s2=0; s2<snapshot_count && s2<TM_MAX_PROCS; s2++){
                        if(snapshots[s2].pid != gpid) continue;
                        if(g_thread_count < 192){
                            TMThread& th = g_threads[g_thread_count++];
                            th.leader = snap.pid; th.pid = gpid; th.cpu_pct = cpu_pcts[s2];
                            tm_format_process_state(snapshots[s2].state, th.state, 12);
                        }
                        cpu_pct += cpu_pcts[s2];
                        break;
                    }
                }
            }
        }
        if(taskmem && is_user){
            // kernel heap the process caused, summed over its threads (satoru)
            uint32_t kpids[TM_KILL_GROUP_MAX];
            int kn = Scheduler::CollectAddressSpacePids(snap.pid, kpids, TM_KILL_GROUP_MAX);
            int kh = 0;
            if(kn <= 0) kh = tm_task_kb(snap.pid);
            else for(int g=0; g<kn; g++) kh += tm_task_kb(kpids[g]);
            mem_kb += kh;
            tm_user_kheap_kb += kh;
        }
        p->cpu_pct = tm_clamp(cpu_pct, 0, 100);
        p->mem_kb = mem_kb;
        p->shared_kb = shr_kb;
        tm_format_process_state(snap.state, p->state, 12);
        // label kernel vs user clearly (shown in the Details "User" column). (satoru)
        scpy(p->user, is_user ? "user" : "kernel", 16);
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
        // is_kernel_proc drives the kernel-stack telemetry column; treat any
        // non-user task as kernel so the column populates correctly. (satoru)
        p->is_kernel_proc = snap.is_kernel_proc || !is_user;
    }

    if(fresh){
        tm_store_cpu_samples(snapshots, cpu_pcts, snapshot_count);
        g_tm_cpu_samples_ready = true;
    }

    // open windows are NOT processes - a window is a ui surface owned by some
    // process, and listing it here both inflated the count and double-counted
    // its framebuffer as "memory". windows have their own dedicated tab
    // (RenderWindows). just refresh that list; don't blend it into procs. (satoru)
    RefreshWindows();

    // in-kernel app windows have no task of their own, so the fallback build
    // (kurono.r3apps=0) would show no row for them. list each as an app row;
    // ring-3 windows are skipped because their process is already a row. (satoru)
    for(int wi=0; wi<window_count && proc_count<TM_MAX_PROCS-2; wi++){
        if(WaylandServer::IsWaylandWindow(window_rows[wi].id)) continue;
        const char* t = window_rows[wi].title;
        int icon = tm_icon_for_title(t);
        if(icon < 0) continue;
        bool dup = false;
        for(int j=0;j<proc_count;j++)
            if(procs[j].group == 0 && scmp(procs[j].name, t) == 0){ dup = true; break; }
        if(dup) continue;
        TMProcess* p = &procs[proc_count++];
        scpy(p->name, t, 32);
        p->pid = 0; p->cpu_pct = 0; p->mem_kb = 0; p->shared_kb = 0; p->threads = 0;
        scpy(p->state, "IN DESKTOP", 12);
        scpy(p->user, "app", 16);
        p->priority = 0; p->io_read_kb = 0; p->io_write_kb = 0; p->flags = 0;
        p->source_kind = TM_PROC_SYNTH;
        p->stack_kb = 0; p->stack_cap_kb = 0; p->cpu_ms_total = 0; p->stack_grow_count = 0;
        p->prio_tier = 0; p->is_kernel_proc = false;
        p->icon = icon; p->group = 0;
    }

    // built-in app rows (kurono.taskmem=1): the heap allocated while the app's
    // code ran inside its host task. synthetic: pid 0, never killable. (satoru)
    int tm_synth_kb = 0;
    if(taskmem){
        for(int i=0;i<g_tmrow_n && proc_count<TM_MAX_PROCS-2;i++){
            const TaskMem::Row& r = g_tmrows[i];
            if(!(r.key & TaskMem::APP_KEY)) continue;
            int kb = tm_kb_i((r.heap_bytes + r.frame_bytes) >> 10);
            if(kb <= 0) continue;
            // one row per app; several hosts (desktop, window_mgr) fold together (satoru)
            TMProcess* p = nullptr;
            for(int j=0;j<proc_count;j++)
                if(procs[j].source_kind == TM_PROC_SYNTH && procs[j].pid == 0 && tm_starts_with(procs[j].name, r.name) && procs[j].name[slen(r.name)] == 0) { p = &procs[j]; break; }
            if(!p){
                p = &procs[proc_count++];
                scpy(p->name, r.name, 32);
                p->pid = 0; p->cpu_pct = 0; p->mem_kb = 0; p->shared_kb = 0; p->threads = 0;
                bool fc = (r.key & 0xFF) == TaskMem::APP_FILECACHE;   // shared file data, not an app (satoru)
                scpy(p->state, fc ? "-" : "IN DESKTOP", 12);
                scpy(p->user, fc ? "kernel" : "app", 16);
                p->priority = 0; p->io_read_kb = 0; p->io_write_kb = 0; p->flags = 0;
                p->source_kind = TM_PROC_SYNTH;
                p->stack_kb = 0; p->stack_cap_kb = 0; p->cpu_ms_total = 0; p->stack_grow_count = 0;
                p->prio_tier = 0; p->is_kernel_proc = false;
                p->icon = TM_ICON_GENERIC; p->group = fc ? 1 : 0;
                { char disp[32]; int icon = TM_ICON_GENERIC, grp = -1;
                  if(tm_name_map(p->name, disp, 32, &icon, &grp)){ scpy(p->name, disp, 32); p->icon = icon; if(grp >= 0) p->group = (uint8_t)grp; } }
            }
            p->mem_kb += kb;
            tm_synth_kb += kb;
        }
    }

    SortProcesses();

    cpu_usage = tm_clamp(total_busy_cpu, 0, 100);

    // per-core load from the per-cpu busy-tick side table (one tick ~1ms); the
    // overall figure is the mean over cores so it matches the bars. (satoru)
    {
        static uint64_t prev_busy[32];
        int n = (int)SMP::CpuCount(); if(n < 1) n = 1; if(n > 32) n = 32;
        g_core_n = n;
        int sum = 0;
        for(int c=0;c<n;c++){
            uint64_t cur = g_cpu_busy_ticks[c];
            uint64_t d = cur - prev_busy[c];
            if(fresh){
                prev_busy[c] = cur;
                g_core_pct[c] = g_tm_cpu_samples_ready ? tm_cpu_pct_from_ticks(d, elapsed_ms) : 0;
            }
            sum += g_core_pct[c];
        }
        cpu_usage = tm_clamp(sum / n, 0, 100);
    }
    // kernel time no task row owns: its own System row, so the kernel reports
    // its own usage next to the service tasks. (satoru)
    {
        static uint64_t prev_kms = 0;
        uint64_t kms = hal_kernel_cpu_ms();
        uint64_t d = kms >= prev_kms ? kms - prev_kms : 0;
        int kpct = 0;
        if(fresh && g_tm_cpu_samples_ready && elapsed_ms > 0 && g_core_n > 0){
            uint64_t pct = (d * 100ull) / ((uint64_t)elapsed_ms * (uint64_t)g_core_n);
            kpct = tm_clamp((int)pct, 0, 100);
        }
        if(fresh) prev_kms = kms;
        if(proc_count < TM_MAX_PROCS-1){
            TMProcess* p = &procs[proc_count++];
            scpy(p->name, "Kernel (unowned)", 32);
            p->pid = 0; p->cpu_pct = kpct; p->mem_kb = 0; p->shared_kb = 0; p->threads = 0;
            scpy(p->state, kpct > 0 ? "RUNNING" : "IDLE", 12);
            scpy(p->user, "kernel", 16);
            p->priority = 0; p->io_read_kb = 0; p->io_write_kb = 0; p->flags = 0;
            p->source_kind = TM_PROC_SYNTH;
            p->stack_kb = 0; p->stack_cap_kb = 0;
            p->cpu_ms_total = (uint32_t)(kms > 0xFFFFFFFFull ? 0xFFFFFFFFu : kms);
            p->stack_grow_count = 0; p->prio_tier = 0; p->is_kernel_proc = true;
            p->icon = TM_ICON_GENERIC; p->group = 1;
        }
    }
    // per-cpu history for the performance graphs. (satoru)
    if(fresh){
        for(int c=0;c<g_core_n && c<32;c++) g_core_hist[c][g_core_hist_idx % 60] = g_core_pct[c];
        g_core_hist_idx++;
    }
    // disk + network rates (kb/s) from the real nvme / nic byte counters (satoru)
    {
        static uint64_t pr_rd = 0, pr_wr = 0, pr_rx = 0, pr_tx = 0;
        uint64_t rd = NVMe::GetBytesRead(), wr = NVMe::GetBytesWritten();
        uint64_t rx = eth ? eth->rx_bytes : 0, tx = eth ? eth->tx_bytes : 0;
        if(fresh && g_tm_cpu_samples_ready && elapsed_ms > 0){
            g_dsk_rd_rate = (int)(((rd - pr_rd) / 1024ULL) * 1000ULL / elapsed_ms);
            g_dsk_wr_rate = (int)(((wr - pr_wr) / 1024ULL) * 1000ULL / elapsed_ms);
            g_net_rx_rate = (int)(((rx - pr_rx) / 1024ULL) * 1000ULL / elapsed_ms);
            g_net_tx_rate = (int)(((tx - pr_tx) / 1024ULL) * 1000ULL / elapsed_ms);
        }
        if(fresh){ pr_rd = rd; pr_wr = wr; pr_rx = rx; pr_tx = tx; }
        g_dsk_rd_hist[g_io_idx % 60] = g_dsk_rd_rate; g_dsk_wr_hist[g_io_idx % 60] = g_dsk_wr_rate;
        g_net_rx_hist[g_io_idx % 60] = g_net_rx_rate; g_net_tx_hist[g_io_idx % 60] = g_net_tx_rate;
        g_io_idx++;
    }

    // honest system ram: real total/used/free from the pmm - never a sum of
    // per-process figures (that double-counts shared pages + framebuffers). (satoru)
    tm_query_memory_usage_kb(mem_total_kb, mem_used_kb, mem_free_kb, mem_heap_kb);
    mem_cached_kb = mem_free_kb;   // keep the legacy field coherent (satoru)

    // memory no task row claims, split into what the kernel can name, so the
    // rows sum to the used figure (user processes count once here, by their
    // proportional share). synthetic rows: pid 0, never killable. (satoru)
    //   kernel heap (shared)  heap blocks with no task owner: boot-time init,
    //                         interrupt work, table overflow
    //   kernel heap (exited)  heap still held by records of tasks that exited
    //   kernel heap (free)    the part of the heap arena nobody has allocated
    //   kernel (other)        the rest: kernel image, page tables, the buddy
    //                         and kmemx pools, framebuffers, dma buffers (satoru)
    if(taskmem && proc_count < TM_MAX_PROCS-4){
        int64_t exited_b = 0;
        for(int i=0;i<g_tmrow_n;i++){
            const TaskMem::Row& r = g_tmrows[i];
            if(r.key & TaskMem::APP_KEY) continue;   // app / file cache rows are shown on their own (satoru)
            bool alive = false;
            for(int j=0;j<snapshot_count && !alive;j++) if(snapshots[j].pid == r.pid) alive = true;
            if(!alive) exited_b += (int64_t)(r.heap_bytes + r.frame_bytes);
        }
        int64_t claimed = (int64_t)g_tm_user_kb + tm_user_kheap_kb + tm_synth_kb;
        for(int i=0;i<proc_count;i++)
            if(procs[i].source_kind == TM_PROC_SCHED && procs[i].user[0] == 'k') claimed += procs[i].mem_kb;
        int64_t shared_kb = (int64_t)((g_tmtot.kernel_bytes + g_tmtot.kernel_frames) >> 10);
        int64_t exited_kb = exited_b >> 10;
        int64_t free_kb = (int64_t)g_tm_sys.heap_arena_kb - (int64_t)(g_tmtot.heap_live >> 10);
        if(free_kb < 0) free_kb = 0;
        int64_t other_kb = (int64_t)mem_used_kb - claimed - shared_kb - exited_kb - free_kb;
        if(other_kb < 0) other_kb = 0;
        const char* names[4] = {"Kernel heap (shared)", "Kernel heap (exited)", "Kernel heap (free)", "Kernel (other)"};
        int64_t vals[4] = {shared_kb, exited_kb, free_kb, other_kb};
        for(int k=0;k<4;k++){
            if(k == 1 && vals[k] <= 0) continue;   // only while some exited task still holds heap (satoru)
            TMProcess* p = &procs[proc_count++];
            scpy(p->name, names[k], 32);
            p->pid = 0; p->cpu_pct = 0; p->mem_kb = tm_kb_i((uint64_t)vals[k]); p->shared_kb = 0; p->threads = 0;
            scpy(p->state, "-", 12);
            scpy(p->user, "kernel", 16);
            p->priority = 0; p->io_read_kb = 0; p->io_write_kb = 0; p->flags = 0;
            p->source_kind = TM_PROC_SYNTH;
            p->stack_kb = 0; p->stack_cap_kb = 0; p->cpu_ms_total = 0; p->stack_grow_count = 0;
            p->prio_tier = 0; p->is_kernel_proc = true;
            p->icon = TM_ICON_GENERIC; p->group = 1;
        }
        SortProcesses();
    }

    uptime_sec = (int)(Timer::GetRealMs64() / 1000ULL);   // tsc clock: the pit-irq count runs slow when irqs coalesce (satoru)
    disk_read_kb = (int)(KVFS::DiskUsage("/") / 1024);
    disk_write_kb = 0;   // no real disk-write counter exists; pane shows honest fields (satoru)

    // record history
    cpu_history[hist_idx % 60] = tm_clamp(cpu_usage, 0, 100);
    mem_history[hist_idx % 60] = tm_clamp((int)(((int64_t)mem_used_kb * 100) / (mem_total_kb > 0 ? mem_total_kb : 1)), 0, 100);   // 64-bit: kb*100 overflowed int above 20 gb (satoru)
    if(hist_idx == 0){ for(int i=1;i<60;i++) mem_history[i] = mem_history[0]; }   // no fake ramp from zero (satoru)
    hist_idx++;

    if(keep_pid != -2){
        selected_proc = -1;
        for(int i=0;i<proc_count;i++){
            bool match = (keep_pid == 0) ? (procs[i].pid == 0 && scmp(procs[i].name, keep_name) == 0)
                                         : (procs[i].pid == keep_pid);
            if(match){ selected_proc = i; break; }
        }
    }
    if(selected_proc >= proc_count) selected_proc = -1;
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
#endif

void TaskManagerApp::SortProcesses(){
    // simple bubble sort
    // apps first, then the system services; the chosen column orders each
    // group, so the two blocks stay readable while cpu still sorts hottest
    // first inside them. (satoru)
    for(int i=0;i<proc_count-1;i++){
        for(int j=0;j<proc_count-1-i;j++){
            bool swap=false;
            if(procs[j].group != procs[j+1].group){
                swap = procs[j].group > procs[j+1].group;
            } else {
                switch(sort_col){
                    case TM_SORT_NAME: swap = (scmp(procs[j].name,procs[j+1].name) > 0) != sort_asc; break;
                    case TM_SORT_PID:  swap = (procs[j].pid > procs[j+1].pid) != sort_asc; break;
                    case TM_SORT_CPU:  swap = (procs[j].cpu_pct < procs[j+1].cpu_pct) != sort_asc; break;
                    case TM_SORT_MEM:  swap = (procs[j].mem_kb < procs[j+1].mem_kb) != sort_asc; break;
                }
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
//  ---- modern redesign: tabs, smooth graphs, overview cards, grouped process list (satoru) ----

// new column geometry for the grouped process list (content-relative x) (satoru)
static const int NC_ICON = 8, NC_CHEV = 28, NC_PID = 46, NC_NAME = 100, NC_CPUBAR = 330, NC_CPUNUM = 402,
                 NC_MEM = 462, NC_THR = 560, NC_STATE = 606;
static const int SB_H = 30;     // bottom bar height (status text + End Task) (satoru)

#if !defined(KUI_BACKEND_WAYLAND)
static bool tm_anim_on(){ return UIConfig::Bool("compositor.window_animations", true); }
#else
static bool tm_anim_on(){ return !kui::CurrentTheme().reduced_motion; }
#endif
// eased display value, keyed per metric; snaps to the target when animations are off (satoru)
static int tm_ease_x100(uint32_t key, int target_pct){
    // integer ease-out cubic over 350ms (this file is built without sse, so no
    // floats); re-marks the ui dirty only while a value is still moving (satoru)
    if(!tm_anim_on()) return target_pct * 100;
    // 16 metric slots + 32 per-core slots: cores 16-19 had shared cores 0-3's (satoru)
    static int from[48], tgt[48], cur[48]; static uint32_t t0[48]; static bool seeded[48];
    int i = key < 0x100u ? (int)(key & 15u) : 16 + (int)(key & 31u);
    uint32_t now = kui::NowMs();
    if(!seeded[i]){ seeded[i] = true; from[i] = tgt[i] = cur[i] = target_pct * 100; t0[i] = now; }
    if(tgt[i] != target_pct * 100){ from[i] = cur[i]; tgt[i] = target_pct * 100; t0[i] = now; }
    uint32_t el = now - t0[i];
    if(el >= 350u){ cur[i] = tgt[i]; return cur[i]; }
    int q = (int)(el * 1000u / 350u);              // 0..1000
    int inv = 1000 - q;
    int e = 1000 - (inv * inv / 1000) * inv / 1000; // 1-(1-t)^3, 0..1000
    cur[i] = from[i] + (tgt[i] - from[i]) * e / 1000;
    if(g_tm_win) g_tm_win->RequestAnimationFrame();   // one more frame while the value moves (satoru)
    return cur[i];
}

static bool tm_is_expanded(uint32_t pid){
    for(int i=0;i<g_expanded_n;i++) if(g_expanded[i]==pid) return true;
    return false;
}
static void tm_toggle_expand(uint32_t pid){
    for(int i=0;i<g_expanded_n;i++){
        if(g_expanded[i]==pid){ g_expanded[i]=g_expanded[--g_expanded_n]; return; }
    }
    if(g_expanded_n < 8) g_expanded[g_expanded_n++] = pid;
}

// flat visible rows: group headers, each process, then its threads when
// expanded. kind 0 process, 1 thread, 2 group header (idx = group). (satoru)
static void tm_build_rows(const TMProcess* procs_, int proc_count_){
    g_row_count = 0;
    int last_group = -1;
    for(int i=0;i<proc_count_ && g_row_count < TM_MAX_PROCS+190;i++){
        if((int)procs_[i].group != last_group){
            last_group = (int)procs_[i].group;
            if(g_row_count < TM_MAX_PROCS+192){
                g_rows[g_row_count].kind = 2;
                g_rows[g_row_count].idx = (int16_t)last_group;
                g_rows[g_row_count].proc = -1;
                g_row_count++;
            }
        }
        g_rows[g_row_count].kind = 0; g_rows[g_row_count].idx = (int16_t)i; g_rows[g_row_count].proc = (int16_t)i; g_row_count++;
        if(procs_[i].threads > 1 && tm_is_expanded((uint32_t)procs_[i].pid)){
            for(int t=0;t<g_thread_count && g_row_count < TM_MAX_PROCS+190;t++){
                if(g_threads[t].leader != (uint32_t)procs_[i].pid) continue;
                g_rows[g_row_count].kind = 1; g_rows[g_row_count].idx = (int16_t)t; g_rows[g_row_count].proc = (int16_t)i; g_row_count++;
            }
        }
    }
}

static int tm_row_to_proc(int row){ return (row >= 0 && row < g_row_count) ? (int)g_rows[row].proc : -1; }

static void tm_card(int x,int y,int w,int h,const char* title,unsigned int accent){
    g_cv->RoundRect(x, y, w, h, 10, TM_PANEL, 255);
    g_cv->FillRoundedRect(x+12, y+13, 4, 12, 2, accent);
    tm_ds(x+22, y+11, title, TM_TEXT);
}

// smooth area graph: linear interpolation between samples, newest point eased,
// filled with a translucent wash under the line (satoru)
static void tm_graph(int gx,int gy,int gw,int gh,const int* data,int idx,int max_val,unsigned int col,int latest_x100){
    g_cv->FillRoundedRect(gx,gy,gw,gh,6,TM_GRAPH_BG);
    for(int i=1;i<4;i++){
        int yy = gy + i*gh/4;
        for(int p=gx+4;p<gx+gw-4;p+=8) g_cv->FillRect(p,yy,3,1,TM_GRAPH_GD);
    }
    if(max_val < 1) max_val = 1;
    const int span = gw - 6;
    for(int px=0; px<span; px+=2){
        int pos = (px * 59 * 1000) / (span > 1 ? span - 1 : 1);
        int k = pos / 1000, f = pos % 1000;
        int k1 = k < 59 ? k + 1 : 59;
        int v0 = data[(idx + k) % 60] * 100;
        int v1 = data[(idx + k1) % 60] * 100;
        if(k == 59) v0 = latest_x100;
        if(k1 == 59) v1 = latest_x100;
        int v = v0 + (v1 - v0) * f / 1000;
        if(v > max_val * 100) v = max_val * 100;
        if(v < 0) v = 0;
        int yy = gy + gh - 3 - (v * (gh - 8)) / (max_val * 100);
        int hh = gy + gh - 2 - yy;
        if(hh > 0) g_cv->FillRectAlpha(gx+3+px, yy, 2, hh, 56, col);
        g_cv->FillRect(gx+3+px, yy, 2, 2, col);
    }
}

static void tm_fmt_rate(char* out,int mx,int kbps){
    char t[12];
    out[0]=0;
    if(kbps >= 1024){ int_to_str(kbps/1024,t,12); sapp(out,t,mx); sapp(out,".",mx); int_to_str((kbps%1024)*10/1024,t,12); sapp(out,t,mx); sapp(out," MB/s",mx); }
    else { int_to_str(kbps,t,12); sapp(out,t,mx); sapp(out," KB/s",mx); }
}

void TaskManagerApp::RenderTabs(int x,int y,int w){
    g_cv->FillRect(x,y,w,TAB_H,TM_TAB_BG);
    g_cv->DrawLine(x,y+TAB_H-1,x+w,y+TAB_H-1,TM_BORDER);
    static const char* tabs[] = {"Processes", "Performance", "Details", "Services", "Windows"};
    int tx=x+6;
    for(int i=0;i<TM_TAB_COUNT;i++){
        int tw=slen(tabs[i])*8+20;
        bool active = (i==(int)current_tab);
        if(active){
            g_cv->RoundRect(tx, y+5, tw, TAB_H-10, 8, TM_TAB_SEL, 255);
            g_cv->FillRoundedRect(tx+tw/2-8, y+TAB_H-5, 16, 2, 1, TM_BLUE);
        }
        tm_ds(tx+10, y+11, tabs[i], active ? TM_TAB_ACT : TM_TAB_TXT);
        tx+=tw+4;
    }
}

void TaskManagerApp::RenderProcessList(int x,int y,int w,int h){
    tm_build_rows(procs, proc_count);
    // column header with sort indicator on the active column
    g_cv->FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    int  cols[]    = {NC_PID, NC_NAME, NC_CPUBAR, NC_MEM, NC_THR, NC_STATE};
    const char* headers[] = {"PID", "Name", "CPU", "Memory", "Thr", "Status"};
    TMSortCol scols[] = {TM_SORT_PID, TM_SORT_NAME, TM_SORT_CPU, TM_SORT_MEM, TM_SORT_PID, TM_SORT_NAME};
    for(int i=0;i<6;i++){
        bool act = (i < 4 && scols[i] == sort_col);
        tm_ds(x+cols[i], y+3, headers[i], act ? TM_BLUE : TM_DIM);
        if(act) tm_ds(x+cols[i]+slen(headers[i])*8+4, y+3, sort_asc ? "^" : "v", TM_BLUE);
    }
    y+=ROW_H;

    int vis=(h-ROW_H)/ROW_H;
    last_list_vis = vis;
    int max_off = g_row_count - vis; if(max_off < 0) max_off = 0;
    if(scroll_offset > max_off) scroll_offset = max_off;
    if(scroll_offset < 0) scroll_offset = 0;
    for(int i=0;i<vis && (i+scroll_offset)<g_row_count;i++){
        const TMRow& rw = g_rows[i+scroll_offset];
        int ry=y+i*ROW_H;
        char num[12];
        if(rw.kind == 2){
            // group header: Apps first, then the System services. quiet, one
            // accent bar and a count instead of a boxed table. (satoru)
            bool sys = rw.idx == 1;
            g_cv->FillRect(x, ry, w, ROW_H, TM_HEAD_BG);
            g_cv->FillRoundedRect(x+NC_ICON, ry+7, 3, 9, 1, sys ? TM_DIM : TM_BLUE);
            tm_ds(x+NC_ICON+10, ry+3, sys ? "System" : "Apps", sys ? TM_DIM : TM_BLUE);
            int cnt = 0;
            for(int k=0;k<proc_count;k++) if((int)procs[k].group == rw.idx) cnt++;
            int_to_str(cnt, num, 12);
            tm_ds(x+NC_ICON+10+slen(sys ? "System" : "Apps")*8+8, ry+3, num, TM_DIM);
            continue;
        }
        if(rw.kind == 1){
            // thread row: indented, dim, tree guide
            const TMThread& th = g_threads[rw.idx];
            g_cv->FillRect(x+NC_NAME+6, ry, 1, ROW_H, TM_BORDER);
            g_cv->FillRect(x+NC_NAME+6, ry+ROW_H/2, 8, 1, TM_BORDER);
            int_to_str((int)th.pid,num,12);
            tm_ds(x+NC_PID, ry+3, num, TM_DIM);
            char nm[24]="thread "; sapp(nm,num,24);
            tm_ds(x+NC_NAME+20, ry+3, nm, TM_DIM);
            int_to_str(th.cpu_pct,num,12); sapp(num,"%",12);
            tm_ds(x+NC_CPUNUM, ry+3, num, th.cpu_pct>0 ? TM_TEXT : TM_DIM);
            tm_ds(x+NC_STATE, ry+3, th.state, TM_DIM);
            continue;
        }
        int idx = rw.idx;
        TMProcess* p=&procs[idx];
        if(idx==selected_proc) g_cv->RoundRect(x+4,ry,w-8,ROW_H,6,TM_SEL_BG,255);
        else if(i%2) g_cv->FillRect(x,ry,w,ROW_H,TM_ROW_ALT);
        // the app tile makes a row readable at a glance; services get the same
        // generic tile so the two groups stay visually even. (satoru)
        g_cv->AppTile(p->icon, x+NC_ICON, ry+3, 16);
        if(p->threads > 1){
            // expand chevron
            bool ex = tm_is_expanded((uint32_t)p->pid);
            tm_ds(x+NC_CHEV, ry+3, ex ? "v" : ">", TM_DIM);
        }
        int_to_str(p->pid,num,12);
        if(p->source_kind == TM_PROC_SYNTH) scpy(num, "-", 12);   // no task of its own (satoru)
        tm_ds(x+NC_PID, ry+3, num, TM_DIM);
        char nm[30]; scpy(nm, p->name, 28);
        unsigned int nc = (p->state[0]=='R') ? TM_TEXT : TM_DIM;
        if(!p->is_kernel_proc) nc = TM_TEXT;
        tm_ds(x+NC_NAME, ry+3, nm, nc);
        // cpu bar (64 px) with the percentage to its right
        g_cv->FillRoundedRect(x+NC_CPUBAR, ry+7, 64, 8, 4, TM_GRAPH_BG);
        int bw = (p->cpu_pct * 64) / 100; if(p->cpu_pct > 0 && bw < 3) bw = 3;
        unsigned int bar_c = p->cpu_pct > 50 ? TM_RED : (p->cpu_pct > 20 ? TM_ORANGE : TM_GREEN);
        if(bw > 0) g_cv->FillRoundedRect(x+NC_CPUBAR, ry+7, bw, 8, 4, bar_c);
        int_to_str(p->cpu_pct, num, 12); sapp(num, "%", 12);
        tm_ds(x+NC_CPUNUM, ry+3, num, p->cpu_pct>0 ? bar_c : TM_DIM);
        char mem[16]={0};
        if (p->mem_kb <= 0) { sapp(mem, "-", 16); }   // nothing resident is known for this task: a dash, not an invented figure (satoru)
        else if (p->mem_kb >= 1024) { int_to_str(p->mem_kb/1024, mem, 16); sapp(mem, " MB", 16); }
        else { int_to_str(p->mem_kb, mem, 16); sapp(mem, " KB", 16); }
        tm_ds(x+NC_MEM, ry+3, mem, TM_TEXT);
        int_to_str(p->threads, num, 12);
        if(p->source_kind == TM_PROC_SYNTH) scpy(num, "-", 12);   // (satoru)
        tm_ds(x+NC_THR, ry+3, num, TM_DIM);
        unsigned int st_clr = TM_DIM;
        if(p->state[0]=='R') st_clr = TM_GREEN;
        else if(p->state[0]=='S') st_clr = TM_YELLOW;
        else if(p->state[0]=='Z') st_clr = TM_RED;
        g_cv->FillCircle(x+NC_STATE-8, ry+11, 3, st_clr);
        tm_ds(x+NC_STATE, ry+3, p->state, st_clr);
    }

    if(g_row_count > vis && vis > 0){
        int track_x = x + w - 5;
        int track_h = vis * ROW_H;
        int thumb_h = tm_max(16, track_h * vis / g_row_count);
        int mo = g_row_count - vis;
        int thumb_y = y + (mo > 0 ? (track_h - thumb_h) * scroll_offset / mo : 0);
        g_cv->FillRoundedRect(track_x, thumb_y, 3, thumb_h, 1, TM_BORDER);
    }
}

void TaskManagerApp::RenderPerformance(int x,int y,int w,int h){
    (void)h;
    const int pad = 12, gap = 10;
    const int half = (w - 2*pad - gap) / 2;
    int ly = y + pad;
    char t[16], line[64];

    // --- cpu + memory cards with large smooth graphs ---
    tm_card(x+pad, ly, half, 150, "CPU", TM_BLUE);
    int cpu_e = tm_ease_x100(1, cpu_usage);
    int_to_str(cpu_e/100, t, 16); sapp(t, "%", 16);
    tm_ds(x+pad+half-12-slen(t)*8, ly+11, t, TM_WHITE);
    tm_graph(x+pad+10, ly+34, half-20, 72, cpu_history, hist_idx, 100, TM_BLUE, cpu_e);
    {
        const char* brand = tm_cpu_brand();
        scpy(line, brand, (half-20)/8 < 60 ? (half-20)/8 : 60);
        tm_ds(x+pad+12, ly+128, line, TM_DIM);
    }

    int mem_pct = tm_clamp((int)(((int64_t)mem_used_kb*100)/(mem_total_kb>0?mem_total_kb:1)), 0, 100);   // 64-bit, see RefreshProcesses (satoru)
    tm_card(x+pad+half+gap, ly, half, 150, "Memory", TM_PURPLE);
    int mem_e = tm_ease_x100(2, mem_pct);
    int_to_str(mem_e/100, t, 16); sapp(t, "%", 16);
    tm_ds(x+pad+half+gap+half-12-slen(t)*8, ly+11, t, TM_WHITE);
    tm_graph(x+pad+half+gap+10, ly+34, half-20, 72, mem_history, hist_idx, 100, TM_PURPLE, mem_e);
    {
        // line 1: in use / total, free. line 2: where the used part went (mb). (satoru)
        char a[12];
        scpy(line, "In use ", 64);
        int_to_str(mem_used_kb/1024, a, 12); sapp(line, a, 64); sapp(line, " / ", 64);
        int_to_str(mem_total_kb/1024, a, 12); sapp(line, a, 64); sapp(line, " MB  free ", 64);
        int_to_str(mem_free_kb/1024, a, 12); sapp(line, a, 64);
        tm_ds(x+pad+half+gap+12, ly+110, line, TM_DIM);
        scpy(line, "heap ", 64);
        int_to_str(mem_heap_kb/1024, a, 12); sapp(line, a, 64); sapp(line, " user ", 64);
        int_to_str(g_tm_user_kb/1024, a, 12); sapp(line, a, 64); sapp(line, " kmx ", 64);
        int_to_str(g_tm_kmx_kb/1024, a, 12); sapp(line, a, 64); sapp(line, " other ", 64);
        int_to_str(g_tm_other_kb/1024, a, 12); sapp(line, a, 64);
        tm_ds(x+pad+half+gap+12, ly+128, line, TM_DIM);
    }
    ly += 150 + gap;

    // --- per-core load: real busy-tick counters per cpu, each with its own
    // usage graph so every online cpu is visible, not only the bars. (satoru)
    {
        int rows = (g_core_n + 3) / 4;
        int ch = 34 + rows * 28 + 6;
        tm_card(x+pad, ly, w-2*pad, ch, "Cores", TM_CYAN);
        int cw4 = (w - 2*pad - 24) / 4;
        for(int c=0;c<g_core_n;c++){
            int cx = x + pad + 12 + (c % 4) * cw4;
            int cy = ly + 34 + (c / 4) * 28;
            char lb[8]="C"; int_to_str(c, t, 16); sapp(lb, t, 8);
            tm_ds(cx, cy+2, lb, TM_DIM);
            int e = tm_ease_x100(0x100u + (uint32_t)c, g_core_pct[c]);
            int_to_str(e/100, t, 16); sapp(t, "%", 16);
            tm_ds(cx+22, cy+2, t, e > 5000 ? TM_RED : (e > 2000 ? TM_ORANGE : TM_TEXT));
            int gw = cw4 - 92; if(gw < 40) gw = 40;
            tm_graph(cx+70, cy+2, gw, 16, g_core_hist[c], g_core_hist_idx, 100, TM_CYAN, e);
        }
        ly += ch + gap;
    }

    // --- disk + network activity (real counters: nvme bytes, nic bytes) ---
    {
        int ch = 120;
        int mx_d = 64, mx_n = 64;
        for(int i=0;i<60;i++){
            int d = g_dsk_rd_hist[i] + g_dsk_wr_hist[i]; if(d > mx_d) mx_d = d;
            int n = g_net_rx_hist[i] + g_net_tx_hist[i]; if(n > mx_n) mx_n = n;
        }
        // graphs plot rx+tx combined: store sum in a scratch ring
        static int sum_d[60], sum_n[60];
        for(int i=0;i<60;i++){ sum_d[i] = g_dsk_rd_hist[i] + g_dsk_wr_hist[i]; sum_n[i] = g_net_rx_hist[i] + g_net_tx_hist[i]; }
        tm_card(x+pad, ly, half, ch, "Disk", TM_ORANGE);
        tm_graph(x+pad+10, ly+34, half-20, 52, sum_d, g_io_idx, mx_d, TM_ORANGE, sum_d[(g_io_idx+59)%60]*100);
        char r1[24], r2[24];
        tm_fmt_rate(r1, 24, g_dsk_rd_rate); tm_fmt_rate(r2, 24, g_dsk_wr_rate);
        scpy(line, "read ", 64); sapp(line, r1, 64); sapp(line, "  write ", 64); sapp(line, r2, 64);
        tm_ds(x+pad+12, ly+94, line, TM_DIM);
        tm_card(x+pad+half+gap, ly, half, ch, "Network", TM_GREEN);
        tm_graph(x+pad+half+gap+10, ly+34, half-20, 52, sum_n, g_io_idx, mx_n, TM_GREEN, sum_n[(g_io_idx+59)%60]*100);
        tm_fmt_rate(r1, 24, g_net_rx_rate); tm_fmt_rate(r2, 24, g_net_tx_rate);
        scpy(line, "down ", 64); sapp(line, r1, 64); sapp(line, "  up ", 64); sapp(line, r2, 64);
        tm_ds(x+pad+half+gap+12, ly+94, line, TM_DIM);
        ly += ch + gap;
    }

    // --- summary line ---
    {
        int hrs=uptime_sec/3600, mins=(uptime_sec%3600)/60;
        int total_threads=0; for(int i=0;i<proc_count;i++) total_threads+=procs[i].threads;
        char a[12];
        char line[96];   // wider than the shared 64: the installed-ram note below needs the room (satoru)
        scpy(line, "Uptime ", 96);
        int_to_str(hrs,a,12); sapp(line,a,96); sapp(line,"h ",96);
        int_to_str(mins,a,12); sapp(line,a,96); sapp(line,"m    Processes ",96);
        int_to_str(proc_count,a,12); sapp(line,a,96); sapp(line,"    Threads ",96);
        int_to_str(total_threads,a,12); sapp(line,a,96);
        // when the pmm manages less than is installed (kurono.memcap, or ram past the 512 gb identity map): say so instead of implying the machine has less. (satoru)
        uint64_t inst_mb = (uint64_t)tm_installed_mb();
        if(inst_mb > (uint64_t)(mem_total_kb / 1024) + 64){
            sapp(line,"    RAM installed ",96);
            int_to_str((int)inst_mb,a,12); sapp(line,a,96); sapp(line," MB",96);
        }
        tm_ds(x+pad+4, ly+2, line, TM_DIM);
    }
}

void TaskManagerApp::RenderDetails(int x,int y,int w,int h){
    // detailed view - header x and value x share one set of offsets so the
    // numbers line up under their labels (Thr/Pri used to be a few px off). (satoru)
    const int D_PID=4, D_USER=40, D_NAME=104, D_CPU=224, D_MEM=264, D_SHR=316, D_THR=364, D_PRI=396, D_IOR=430, D_IOW=486;
    g_cv->FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    tm_ds(x+D_PID, y+3,"PID",  TM_DIM);
    tm_ds(x+D_USER,y+3,"User", TM_DIM);
    tm_ds(x+D_NAME,y+3,"Name", TM_DIM);
    tm_ds(x+D_CPU, y+3,"CPU",  TM_DIM);
    tm_ds(x+D_MEM, y+3,"RSS",  TM_DIM);
    tm_ds(x+D_SHR, y+3,"Shr",  TM_DIM);
    tm_ds(x+D_THR, y+3,"Thr",  TM_DIM);
    tm_ds(x+D_PRI, y+3,"Pri",  TM_DIM);
    tm_ds(x+D_IOR, y+3,"I/O R",TM_DIM);
    tm_ds(x+D_IOW, y+3,"I/O W",TM_DIM);
    y+=ROW_H;

    int vis=(h-ROW_H)/ROW_H;
    last_list_vis = vis;
    int max_off = proc_count - vis; if(max_off < 0) max_off = 0;
    if(scroll_offset > max_off) scroll_offset = max_off;
    if(scroll_offset < 0) scroll_offset = 0;
    for(int i=0;i<vis && (i+scroll_offset)<proc_count;i++){
        int idx=i+scroll_offset;
        TMProcess* p=&procs[idx];
        int ry=y+i*ROW_H;
        if(idx==selected_proc) g_cv->FillRect(x,ry,w,ROW_H,TM_SEL_BG);
        else if(i%2) g_cv->FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        char num[12];
        int_to_str(p->pid,num,12);
        tm_ds(x+D_PID, ry+2,num,TM_DIM);
        // kernel vs user, colour-coded so the distinction reads at a glance. (satoru)
        bool is_user = p->user[0] == 'u';   // "user"; kernel and app rows say otherwise (satoru)
        tm_ds(x+D_USER,ry+2,p->user,is_user?TM_CYAN:TM_DIM);
        tm_ds(x+D_NAME,ry+2,p->name,TM_TEXT);
        int_to_str(p->cpu_pct,num,12); sapp(num,"%",12);
        tm_ds(x+D_CPU, ry+2,num,p->cpu_pct>20?TM_ORANGE:TM_DIM);
        // memory: MB rollup over 1 MB, else KB - consistent with Processes. (satoru)
        char mem[16]={0};
        if(p->mem_kb <= 0)   { sapp(mem,"-",16); }
        else if(p->mem_kb >= 1024){ int_to_str(p->mem_kb/1024,mem,16); sapp(mem,"M",16); }
        else                 { int_to_str(p->mem_kb,mem,16); sapp(mem,"K",16); }
        tm_ds(x+D_MEM, ry+2,mem,TM_DIM);
        char shr[16]={0};   // shared (memfd / MAP_SHARED) part of the rss; a dash for kernel tasks (satoru)
        if(!is_user)                  { sapp(shr,"-",16); }
        else if(p->shared_kb >= 1024) { int_to_str(p->shared_kb/1024,shr,16); sapp(shr,"M",16); }
        else                          { int_to_str(p->shared_kb,shr,16); sapp(shr,"K",16); }
        tm_ds(x+D_SHR, ry+2,shr,TM_DIM);
        int_to_str(p->threads,num,12);
        tm_ds(x+D_THR, ry+2,num,TM_DIM);
        int_to_str(p->priority,num,12);
        tm_ds(x+D_PRI, ry+2,num,TM_DIM);
        // no per-process io accounting exists in the kernel yet - show an
        // honest dash instead of a fake zero. (satoru)
        tm_ds(x+D_IOR, ry+2,"-",TM_DIM);
        tm_ds(x+D_IOW, ry+2,"-",TM_DIM);
    }
}

void TaskManagerApp::RenderWindows(int x,int y,int w,int h){
    g_cv->FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    tm_ds(x+8,  y+3, "ID",      TM_DIM);
    tm_ds(x+44, y+3, "Title",   TM_DIM);
    tm_ds(x+230,y+3, "State",   TM_DIM);
    tm_ds(x+292,y+3, "Session", TM_DIM);
    tm_ds(x+390,y+3, "Pos",     TM_DIM);
    tm_ds(x+470,y+3, "Size",    TM_DIM);
    y += ROW_H;

    int vis = (h - ROW_H) / ROW_H;
    last_list_vis = vis;
    int max_off = window_count - vis; if(max_off < 0) max_off = 0;
    if(scroll_offset > max_off) scroll_offset = max_off;
    if(scroll_offset < 0) scroll_offset = 0;
    for(int i=0;i<vis && (i+scroll_offset)<window_count;i++){
        int idx = i + scroll_offset;
        TMWindowRow* row = &window_rows[idx];
        int ry = y + i * ROW_H;
        if(idx == selected_window) g_cv->FillRect(x,ry,w,ROW_H,TM_SEL_BG);
        else if(i%2) g_cv->FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        char num[16] = {0};
        char pos[24] = {0};
        char size[24] = {0};
        int_to_str(row->id, num, 16);
        tm_ds(x+8, ry+2, num, TM_DIM);
        tm_ds(x+44, ry+2, row->title, TM_TEXT);

        unsigned int state_color = TM_DIM;
        if(row->state[0] == 'O' || row->state[0] == 'F') state_color = TM_GREEN;
        else if(row->state[0] == 'M') state_color = TM_YELLOW;
        tm_ds(x+230, ry+2, row->state, state_color);

        unsigned int session_color = scmp(row->session, "Focused") == 0 ? TM_BLUE : TM_DIM;
        tm_ds(x+292, ry+2, row->session, session_color);

        int_to_str(row->x, pos, 24); sapp(pos, ",", 24); int_to_str(row->y, num, 16); sapp(pos, num, 24);
        tm_ds(x+390, ry+2, pos, TM_DIM);

        size[0] = 0;
        int_to_str(row->w, size, 24); sapp(size, "x", 24); int_to_str(row->h, num, 16); sapp(size, num, 24);
        tm_ds(x+470, ry+2, size, TM_DIM);
    }
}

void TaskManagerApp::RenderServices(int x,int y,int w,int h){
    (void)h;
    g_cv->FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    tm_ds(x+8,  y+3,"Service",   TM_DIM);
    tm_ds(x+180,y+3,"Status",    TM_DIM);
    tm_ds(x+280,y+3,"Type",      TM_DIM);
    tm_ds(x+380,y+3,"PID",       TM_DIM);
    y+=ROW_H;

    for(int i=0;i<service_count;i++){
        int ry=y+i*ROW_H;
        if(i%2) g_cv->FillRect(x,ry,w,ROW_H,TM_ROW_ALT);

        tm_ds(x+8,  ry+2, services[i].name, TM_TEXT);
        // green = running, yellow = sleeping/blocked, red = stopped. (satoru)
        unsigned int sc = TM_RED;
        if(services[i].status[0]=='R') sc = TM_GREEN;
        else if(services[i].status[0]=='S' && services[i].status[1]=='l') sc = TM_YELLOW;
        g_cv->FillCircle(x+184, ry+10, 3, sc);
        tm_ds(x+192, ry+2, services[i].status, sc);
        tm_ds(x+280, ry+2, services[i].type, TM_DIM);
        if(services[i].pid > 0){
            char num[8]; int_to_str(services[i].pid, num, 8);
            tm_ds(x+380, ry+2, num, TM_DIM);
        } else {
            tm_ds(x+380, ry+2, "-", TM_DIM);
        }
    }
}

void TaskManagerApp::RenderStatusBar(int x,int y,int w){
    g_cv->FillRect(x,y,w,SB_H,TM_STATUS);
    g_cv->DrawLine(x,y,x+w,y,TM_BORDER);
    if(current_tab==TM_PROCESSES){
        bool can = selected_proc >= 0 && selected_proc < proc_count && !procs[selected_proc].is_kernel_proc;
        g_cv->RoundRect(x+w-112, y+4, 100, 22, 8, can ? TM_RED : TM_BORDER, 255);
        tm_ds(x+w-112+22, y+9, "End Task", can ? TM_WHITE : TM_DIM);
    }

    // action feedback owns the bar for a few seconds after a kill/restart so
    // the user sees the real outcome, not just a silently refreshed list. (satoru)
    if(g_tm_action_msg[0]){
        uint32_t now = kui::NowMs();
        if(now - g_tm_action_ms < 5000){
            tm_ds(x+12,y+8,g_tm_action_msg,TM_YELLOW);
            return;
        }
        g_tm_action_msg[0] = 0;
    }

    char info[128]={0};
    char n[8];
    sapp(info,"Processes: ",80); int_to_str(proc_count,n,8); sapp(info,n,80);
    sapp(info,"  |  CPU: ",80); int_to_str(cpu_usage,n,8); sapp(info,n,80); sapp(info,"%",80);
    sapp(info,"  |  Mem: ",80); int_to_str(mem_used_kb/1024,n,8); sapp(info,n,80); sapp(info,"/",80);
    int_to_str(mem_total_kb/1024,n,8); sapp(info,n,80); sapp(info," MB",80);
    sapp(info,"  |  Up: ",80); int_to_str(uptime_sec/60,n,8); sapp(info,n,80); sapp(info,"m",80);
    tm_ds(x+12,y+8,info,TM_DIM);
}

void TaskManagerApp::Render(kui::Window& win, kui::Canvas& c){
    g_cv = &c;
    g_tm_win = &win;
    const int cx=0, cy=0, cw=c.Width(), ch=c.Height();
    g_tm_last_render_ms = tm_now64();   // lets RefreshProcesses run its page-table walks (satoru)
    g_cv->FillRect(cx,cy,cw,ch,TM_BG);

    RenderTabs(cx,cy,cw);
    int content_y = cy + TAB_H + 1;
    int content_h = ch - TAB_H - 1 - SB_H;

    switch(current_tab){
        case TM_PROCESSES:   RenderProcessList(cx,content_y,cw,content_h); break;
        case TM_PERFORMANCE: RenderPerformance(cx,content_y,cw,content_h); break;
        case TM_DETAILS:     RenderDetails(cx,content_y,cw,content_h); break;
        case TM_SERVICES:    RenderServices(cx,content_y,cw,content_h); break;
        case TM_WINDOWS:     RenderWindows(cx,content_y,cw,content_h); break;
        default: break;
    }

    RenderStatusBar(cx, cy+ch-SB_H, cw);

    // action popup (kill / restart) drawn on top
    RenderActionMenu(cx, cy, cw, ch);

    // auto-refresh perf data
    Tick();
}

bool TaskManagerApp::Input(kui::Window& w,int mx,int my,bool clicked,int key){
    const kui::Size cs = w.ContentSize();
    // the window is the content here, so a point is already content-local (satoru)
    int abs_mx = mx;
    int abs_my = my;

    if(key=='r' || key=='R'){
        RefreshProcesses();
        return true;
    }

    if(key == TM_RCLICK){
        if(current_tab==TM_PROCESSES || current_tab==TM_DETAILS){
            int list_y = TAB_H + 1 + ROW_H;
            if(my >= list_y){
                int row = (my - list_y) / ROW_H + scroll_offset;
                if(current_tab==TM_PROCESSES) row = tm_row_to_proc(row);
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
        // hit-test against the ACTUAL drawn position: RenderActionMenu clamps the
        // popup to the content area, so a right-click near the right/bottom edge
        // draws it shifted. mirror that exact clamp here or the click misses the
        // visible menu - this was the broken hit test. (satoru)
        int am_x = action_menu_x;
        int am_y = action_menu_y;
        if(am_x + am_w > cs.w) am_x = cs.w - am_w;
        if(am_y + am_h > cs.h) am_y = cs.h - am_h;
        if(abs_mx >= am_x && abs_mx < am_x + am_w &&
           abs_my >= am_y && abs_my < am_y + am_h){
            int row = (abs_my - am_y) / 26;
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
                if(current_tab != (TMTab)i) scroll_offset = 0;  // fresh scroll per tab (satoru)
                current_tab=(TMTab)i;
                return true;
            }
            tx+=tw+4;
        }
    }

    // column header click for sorting (processes tab) - ranges mirror the
    // PL_* column geometry so a header click hits the column it's under. (satoru)
    if(current_tab==TM_PROCESSES && my >= TAB_H+1 && my < TAB_H+1+ROW_H){
        if(mx>=NC_PID && mx<NC_NAME){ sort_col=TM_SORT_PID; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=NC_NAME && mx<NC_CPUBAR){ sort_col=TM_SORT_NAME; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=NC_CPUBAR && mx<NC_MEM){ sort_col=TM_SORT_CPU; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=NC_MEM && mx<NC_THR){ sort_col=TM_SORT_MEM; sort_asc=!sort_asc; SortProcesses(); return true; }
    }

    // End Task button in the bottom bar (processes tab)
    if(current_tab==TM_PROCESSES && my >= cs.h - SB_H && mx >= cs.w - 112 && mx < cs.w - 12){
        if(selected_proc >= 0 && selected_proc < proc_count && !procs[selected_proc].is_kernel_proc){
            KillProcess(selected_proc);
        } else if(selected_proc >= 0 && selected_proc < proc_count){
            tm_set_action_msg("Protected: ", procs[selected_proc].name, " is a kernel process");
        }
        return true;
    }

    // process row click - if already selected, open action menu
    if(current_tab==TM_PROCESSES || current_tab==TM_DETAILS){
        int list_y = TAB_H + 1 + ROW_H;
        if(my >= list_y){
            int row = (my - list_y) / ROW_H + scroll_offset;
            if(current_tab==TM_PROCESSES){
                // chevron: expand / collapse the thread group of this process
                if(mx < NC_PID && row >= 0 && row < g_row_count && g_rows[row].kind == 0){
                    const TMProcess& pp = procs[g_rows[row].idx];
                    if(pp.threads > 1){ tm_toggle_expand((uint32_t)pp.pid); return true; }
                }
                row = tm_row_to_proc(row);
            }
            if(row>=0 && row<proc_count){
                if(row == selected_proc){
                    // second click on same row - open action menu
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
    g_cv->FillRoundedRect(am_x+3, am_y+3, am_w, am_h, 6, 0xFF060610);
    g_cv->FillRoundedRect(am_x, am_y, am_w, am_h, 6, 0xFF161626);
    g_cv->DrawRect(am_x, am_y, am_w, am_h, TM_BORDER);

    // kill row
    tm_ds(am_x+10, am_y+6,  windows_menu ? "Kill" : "Terminate", TM_RED);
    // divider
    g_cv->FillRect(am_x+8, am_y+25, am_w-16, 1, 0xFF252540);
    // restart row
    tm_ds(am_x+10, am_y+30, allow_restart ? "Restart" : "Restart N/A", allow_restart ? TM_GREEN : TM_DIM);
}

#if !defined(KUI_BACKEND_WAYLAND)
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
#endif

KUI_APP(taskmgr, kTmSpec, MakeTaskManager)
