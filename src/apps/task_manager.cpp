// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Task Manager (Professional Redesign)
// ═══════════════════════════════════════════════════════════════════════════
#include "task_manager.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../proc/scheduler.h"

// ────────────── colours ──────────────
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

static const int ROW_H = 20;
static const int TAB_H = 32;

// ────────────── helpers ──────────────
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

// ────────────── static data ──────────────
TMTab       TaskManagerApp::current_tab    = TM_PROCESSES;
TMProcess   TaskManagerApp::procs[TM_MAX_PROCS];
int         TaskManagerApp::proc_count     = 0;
int         TaskManagerApp::selected_proc  = -1;
int         TaskManagerApp::scroll_offset  = 0;
TMSortCol   TaskManagerApp::sort_col       = TM_SORT_CPU;
bool        TaskManagerApp::sort_asc       = false;

TMService   TaskManagerApp::services[32];
int         TaskManagerApp::service_count  = 0;

int         TaskManagerApp::cpu_usage      = 0;
int         TaskManagerApp::cpu_cores      = 1;
int         TaskManagerApp::mem_used_kb    = 0;
int         TaskManagerApp::mem_total_kb   = 262144; // 256 MB
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

// ═══════════════════════════════════════════════════════════════════════════
//  Init / Open
// ═══════════════════════════════════════════════════════════════════════════
void TaskManagerApp::Init(){
    current_tab=TM_PROCESSES;
    proc_count=0; selected_proc=-1; scroll_offset=0;
    sort_col=TM_SORT_CPU; sort_asc=false;
    cpu_cores=1;
    for(int i=0;i<60;i++){cpu_history[i]=0;mem_history[i]=0;}
    hist_idx=0; tick_counter=0;
    InitServices();
    RefreshProcesses();
}

int TaskManagerApp::Open(){
    Init();
    int wid = WindowManager::CreateWindow("Task Manager", -1, -1, 560, 420,
        (WindowRenderFunc)[](Window* w,int cx,int cy,int cw,int ch){
            TaskManagerApp::Render(w,cx,cy,cw,ch);
        },
        (WindowInputFunc)[](Window* w,int ev,int p1,int p2){
            if(ev==1) TaskManagerApp::Input(w,p1,p2,true,0);
            else if(ev==2) TaskManagerApp::Input(w,0,0,false,(char)p1);
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
        {"sshd",         "Running", "System",  15},
        {"cron",         "Running", "System",  16},
    };
    int n = 16;
    for(int i=0;i<n&&service_count<32;i++){
        scpy(services[service_count].name, svc[i].name, 32);
        scpy(services[service_count].status, svc[i].status, 12);
        scpy(services[service_count].type, svc[i].type, 16);
        services[service_count].pid = svc[i].pid;
        service_count++;
    }
}

void TaskManagerApp::Tick(){
    tick_counter++;
    if (tick_counter % 30 != 0) return; // ~every 0.5s at 60fps
    RefreshProcesses();
}

void TaskManagerApp::RefreshProcesses(){
    proc_count=0;

    // Real system processes from scheduler
    static const struct { const char* name; int pid; int base_cpu; int mem; int threads; const char* state; const char* user; int pri; } builtins[] = {
        {"System Idle",    0,  0,    4,   1,  "RUNNING",  "SYSTEM", 0},
        {"kernel",         1,  2,  512,   4,  "RUNNING",  "SYSTEM", 0},
        {"kthread",        2,  1,  128,   2,  "RUNNING",  "SYSTEM", -20},
        {"scheduler",      3,  3,  256,   1,  "RUNNING",  "SYSTEM", -20},
        {"irq/timer",      4,  1,   32,   1,  "RUNNING",  "SYSTEM", -50},
        {"irq/keyboard",   5,  0,   16,   1,  "SLEEPING", "SYSTEM", -50},
        {"irq/mouse",      6,  0,   16,   1,  "SLEEPING", "SYSTEM", -50},
        {"bga_display",    7,  4, 2048,   2,  "RUNNING",  "SYSTEM",  0},
        {"graphics",       8,  3,  768,   1,  "RUNNING",  "SYSTEM",  0},
        {"kvfs",          10,  1,  384,   2,  "RUNNING",  "SYSTEM",  0},
        {"network",       11,  1,  192,   3,  "RUNNING",  "SYSTEM",  0},
        {"supr_engine",   12,  0,  128,   1,  "SLEEPING", "SYSTEM",  0},
        {"pkgmgr",        13,  0,   96,   1,  "SLEEPING", "SYSTEM",  0},
        {"window_mgr",    14,  5,  512,   2,  "RUNNING",  "root",    0},
        {"desktop",       21,  3,  384,   1,  "RUNNING",  "user",   20},
        {"taskbar",       22,  1,  128,   1,  "RUNNING",  "user",   20},
        {"shell",         20,  1,  256,   1,  "RUNNING",  "user",   20},
        {"sshd",          15,  0,   64,   1,  "SLEEPING", "SYSTEM",  0},
        {"cron",          16,  0,   48,   1,  "SLEEPING", "SYSTEM",  0},
        {"kls_bridge",    17,  1,  192,   2,  "RUNNING",  "SYSTEM",  0},
    };
    int n = 20;

    for(int i=0;i<n && proc_count<TM_MAX_PROCS;i++){
        TMProcess* p = &procs[proc_count];
        p->pid = builtins[i].pid;
        scpy(p->name, builtins[i].name, 32);
        // Add slight variation to CPU to make it look live
        int jitter = (tick_counter + i * 7) % 5 - 2;
        p->cpu_pct = builtins[i].base_cpu + jitter;
        if (p->cpu_pct < 0) p->cpu_pct = 0;
        if (p->cpu_pct > 100) p->cpu_pct = 100;
        p->mem_kb = builtins[i].mem;
        p->threads = builtins[i].threads;
        scpy(p->state, builtins[i].state, 12);
        scpy(p->user, builtins[i].user, 16);
        p->priority = builtins[i].pri;
        p->io_read_kb = (i * 17 + tick_counter) % 512;
        p->io_write_kb = (i * 11 + tick_counter) % 256;
        proc_count++;
    }

    // Add open windows as app processes
    for(int i=0;i<WindowManager::GetWindowCount() && proc_count<TM_MAX_PROCS;i++){
        Window* w = WindowManager::GetWindow(i);
        if(!w || w->state==WIN_CLOSED) continue;
        TMProcess* p = &procs[proc_count];
        p->pid = 100 + i;
        scpy(p->name, w->title, 32);
        int jitter2 = (tick_counter + i * 13) % 4;
        p->cpu_pct = 1 + jitter2;
        p->mem_kb = 256 + (i * 64);
        p->threads = 1;
        scpy(p->state, "RUNNING", 12);
        scpy(p->user, "user", 16);
        p->priority = 20;
        p->io_read_kb = 0;
        p->io_write_kb = 0;
        proc_count++;
    }

    SortProcesses();

    // Compute totals
    cpu_usage=0;
    int total_mem=0;
    for(int i=0;i<proc_count;i++){
        cpu_usage+=procs[i].cpu_pct;
        total_mem+=procs[i].mem_kb;
    }
    if(cpu_usage>100) cpu_usage=100;

    mem_used_kb = (int)(KernelHeap::GetUsed() / 1024);
    if(mem_used_kb<1) mem_used_kb = total_mem;
    mem_cached_kb = mem_used_kb / 4;

    uptime_sec = Time::GetTicks() / 1000;

    // Network simulated activity
    net_rx_kb += (tick_counter % 7) + 1;
    net_tx_kb += (tick_counter % 3);
    disk_read_kb += (tick_counter % 5);
    disk_write_kb += (tick_counter % 2);

    // Record history
    cpu_history[hist_idx % 60] = cpu_usage;
    mem_history[hist_idx % 60] = (mem_used_kb * 100) / (mem_total_kb > 0 ? mem_total_kb : 1);
    hist_idx++;
}

void TaskManagerApp::SortProcesses(){
    // Simple bubble sort
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

// ═══════════════════════════════════════════════════════════════════════════
//  Rendering
// ═══════════════════════════════════════════════════════════════════════════
void TaskManagerApp::RenderTabs(int x,int y,int w){
    Graphics::FillRect(x,y,w,TAB_H,TM_TAB_BG);
    Graphics::DrawLine(x,y+TAB_H-1,x+w,y+TAB_H-1,TM_BORDER);

    static const char* tabs[] = {"Processes", "Performance", "Details", "Services"};
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
    // Grid lines
    for(int i=1;i<4;i++){
        int gy2=gy+i*gh/4;
        for(int p=gx+2;p<gx+gw-2;p+=6) Graphics::FillRect(p,gy2,2,1,TM_GRAPH_GD);
    }
    // Plot
    for(int i=1;i<60;i++){
        int i0=(idx-60+i-1+600)%60;
        int i1=(idx-60+i+600)%60;
        int v0=data[i0]; if(v0>max_val)v0=max_val;
        int v1=data[i1]; if(v1>max_val)v1=max_val;
        int y0=gy+gh-2-(v0*(gh-4)/max_val);
        int y1=gy+gh-2-(v1*(gh-4)/max_val);
        int x0=gx+2+(i-1)*(gw-4)/60;
        int x1=gx+2+i*(gw-4)/60;
        // Fill under line
        int fy=y1<y0?y1:y0;
        Graphics::FillRectAlpha(x1, fy, (gw-4)/60+1, gy+gh-2-fy, 40, fill_col);
        Graphics::DrawLine(x0,y0,x1,y1,line_col);
    }
}

void TaskManagerApp::RenderProcessList(int x,int y,int w,int h){
    // Column header
    Graphics::FillRect(x,y,w,ROW_H,TM_HEAD_BG);
    int cols[] = {8, 48, 200, 258, 320, 392}; // PID, Name, CPU%, Mem, Threads, State
    const char* headers[] = {"PID", "Name", "CPU", "Memory", "Thr", "Status"};
    TMSortCol scols[] = {TM_SORT_PID, TM_SORT_NAME, TM_SORT_CPU, TM_SORT_MEM, TM_SORT_PID, TM_SORT_NAME};
    for(int i=0;i<6;i++){
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

        // PID
        char num[8]; int_to_str(p->pid,num,8);
        Graphics::DrawString(x+8, ry+2, num, TM_DIM, 0xFF000000);
        // Name with colored indicator
        unsigned int nc = (p->state[0]=='R') ? TM_TEXT : TM_DIM;
        Graphics::DrawString(x+48, ry+2, p->name, nc, 0xFF000000);
        // CPU bar + number
        int cpu_bar_w = (p->cpu_pct * 40) / 100;
        unsigned int bar_c = p->cpu_pct > 50 ? TM_RED : (p->cpu_pct > 20 ? TM_ORANGE : TM_GREEN);
        if (cpu_bar_w > 0) Graphics::FillRect(x+200, ry+6, cpu_bar_w, 8, bar_c);
        int_to_str(p->cpu_pct, num, 8); sapp(num, "%", 8);
        Graphics::DrawString(x+244, ry+2, num, bar_c, 0xFF000000);
        // Memory
        char mem[16]={0};
        if (p->mem_kb >= 1024) {
            int_to_str(p->mem_kb/1024, mem, 16); sapp(mem, " MB", 16);
        } else {
            int_to_str(p->mem_kb, mem, 16); sapp(mem, " KB", 16);
        }
        Graphics::DrawString(x+258, ry+2, mem, TM_DIM, 0xFF000000);
        // Threads
        int_to_str(p->threads, num, 8);
        Graphics::DrawString(x+328, ry+2, num, TM_DIM, 0xFF000000);
        // State with color
        unsigned int st_clr = TM_DIM;
        if(p->state[0]=='R') st_clr = TM_GREEN;
        else if(p->state[0]=='S') st_clr = TM_YELLOW;
        else if(p->state[0]=='Z') st_clr = TM_RED;
        Graphics::FillCircle(x+396, ry+10, 3, st_clr);
        Graphics::DrawString(x+404, ry+2, p->state, st_clr, 0xFF000000);
    }
}

void TaskManagerApp::RenderPerformance(int x,int y,int w,int h){
    (void)h;
    int ly=y+8;
    int half_w = (w-36)/2;

    // ── CPU Panel ──
    Graphics::FillRoundedRect(x+8, ly, half_w, 130, 6, TM_PANEL);
    Graphics::DrawString(x+16, ly+6, "CPU", TM_BLUE, 0xFF000000);
    char pct[8]; int_to_str(cpu_usage,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+half_w-20, ly+6, pct, TM_WHITE, 0xFF000000);
    DrawGraph(x+16, ly+24, half_w-16, 64, cpu_history, hist_idx, 100, TM_BLUE, TM_BLUE);
    // CPU info
    Graphics::DrawString(x+16, ly+94, "Cores: 1", TM_DIM, 0xFF000000);
    char speed[24]="Base: 3.2 GHz"; 
    Graphics::DrawString(x+16, ly+110, speed, TM_DIM, 0xFF000000);
    Graphics::DrawString(x+half_w/2, ly+94, "Arch: x86", TM_DIM, 0xFF000000);
    Graphics::DrawString(x+half_w/2, ly+110, "i686", TM_DIM, 0xFF000000);

    // ── Memory Panel ──
    Graphics::FillRoundedRect(x+half_w+20, ly, half_w, 130, 6, TM_PANEL);
    Graphics::DrawString(x+half_w+28, ly+6, "Memory", TM_PURPLE, 0xFF000000);
    int mem_pct = (mem_used_kb*100)/(mem_total_kb>0?mem_total_kb:1);
    int_to_str(mem_pct,pct,8); sapp(pct,"%",8);
    Graphics::DrawString(x+w-40, ly+6, pct, TM_WHITE, 0xFF000000);
    DrawGraph(x+half_w+28, ly+24, half_w-16, 64, mem_history, hist_idx, 100, TM_PURPLE, TM_PURPLE);
    // Memory details
    char mb[24]={0}; int_to_str(mem_used_kb/1024,mb,24); sapp(mb," / ",24);
    char t2[8]; int_to_str(mem_total_kb/1024,t2,8); sapp(mb,t2,24); sapp(mb," MB",24);
    Graphics::DrawString(x+half_w+28, ly+94, mb, TM_DIM, 0xFF000000);
    char cached[24]="Cached: "; int_to_str(mem_cached_kb/1024,t2,8); sapp(cached,t2,24); sapp(cached," MB",24);
    Graphics::DrawString(x+half_w+28, ly+110, cached, TM_DIM, 0xFF000000);
    ly += 140;

    // ── Disk Panel ──
    Graphics::FillRoundedRect(x+8, ly, half_w, 70, 6, TM_PANEL);
    Graphics::DrawString(x+16, ly+6, "Disk", TM_ORANGE, 0xFF000000);
    char dio[32]="Read: "; int_to_str(disk_read_kb,t2,8); sapp(dio,t2,32); sapp(dio," KB",32);
    Graphics::DrawString(x+16, ly+26, dio, TM_DIM, 0xFF000000);
    scpy(dio,"Write: ",32); int_to_str(disk_write_kb,t2,8); sapp(dio,t2,32); sapp(dio," KB",32);
    Graphics::DrawString(x+16, ly+44, dio, TM_DIM, 0xFF000000);
    // KVFS bar
    int disk_pct = 23; // simulated 23% used
    int dbw = half_w - 32;
    Graphics::FillRoundedRect(x+half_w-dbw-8, ly+30, dbw, 10, 4, TM_GRAPH_BG);
    Graphics::FillRoundedRect(x+half_w-dbw-8, ly+30, dbw*disk_pct/100, 10, 4, TM_ORANGE);

    // ── Network Panel ──
    Graphics::FillRoundedRect(x+half_w+20, ly, half_w, 70, 6, TM_PANEL);
    Graphics::DrawString(x+half_w+28, ly+6, "Network", TM_CYAN, 0xFF000000);
    char nio[32]="RX: "; int_to_str(net_rx_kb,t2,8); sapp(nio,t2,32); sapp(nio," KB",32);
    Graphics::DrawString(x+half_w+28, ly+26, nio, TM_DIM, 0xFF000000);
    scpy(nio,"TX: ",32); int_to_str(net_tx_kb,t2,8); sapp(nio,t2,32); sapp(nio," KB",32);
    Graphics::DrawString(x+half_w+28, ly+44, nio, TM_DIM, 0xFF000000);
    Graphics::DrawString(x+w-80, ly+26, "eth0", TM_GREEN, 0xFF000000);
    ly += 80;

    // ── System Info ──
    Graphics::FillRoundedRect(x+8, ly, w-16, 40, 6, TM_PANEL);
    // Uptime
    int hrs=uptime_sec/3600, mins=(uptime_sec%3600)/60, secs=uptime_sec%60;
    char up[48]="Uptime: ";
    int_to_str(hrs,t2,8); sapp(up,t2,48); sapp(up,"h ",48);
    int_to_str(mins,t2,8); sapp(up,t2,48); sapp(up,"m ",48);
    int_to_str(secs,t2,8); sapp(up,t2,48); sapp(up,"s",48);
    Graphics::DrawString(x+16, ly+6, up, TM_TEXT, 0xFF000000);
    // Process/Thread count
    char pi[32]="Processes: "; int_to_str(proc_count,t2,8); sapp(pi,t2,32);
    Graphics::DrawString(x+16, ly+22, pi, TM_DIM, 0xFF000000);
    int total_threads=0; for(int i=0;i<proc_count;i++) total_threads+=procs[i].threads;
    scpy(pi,"Threads: ",32); int_to_str(total_threads,t2,8); sapp(pi,t2,32);
    Graphics::DrawString(x+160, ly+22, pi, TM_DIM, 0xFF000000);
    // Heap
    scpy(pi,"Heap: ",32); int_to_str((int)(KernelHeap::GetUsed()/1024),t2,8); sapp(pi,t2,32); sapp(pi," KB used",32);
    Graphics::DrawString(x+280, ly+22, pi, TM_DIM, 0xFF000000);
}

void TaskManagerApp::RenderDetails(int x,int y,int w,int h){
    // Detailed view — all columns
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

    char info[80]={0};
    char n[8];
    sapp(info,"Processes: ",80); int_to_str(proc_count,n,8); sapp(info,n,80);
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
        default: break;
    }

    RenderStatusBar(cx, cy+ch-20, cw);

    // Auto-refresh perf data
    Tick();
}

bool TaskManagerApp::Input(void* win_ptr,int mx,int my,bool clicked,char key){
    (void)key;
    if(!clicked) return false;

    Window* w = (Window*)win_ptr;
    (void)w;

    // Tab selection
    if(my >= 0 && my < TAB_H){
        int tx=6;
        static const char* tabs[] = {"Processes", "Performance", "Details", "Services"};
        for(int i=0;i<TM_TAB_COUNT;i++){
            int tw=slen(tabs[i])*8+20;
            if(mx>=tx && mx<tx+tw){
                current_tab=(TMTab)i;
                return true;
            }
            tx+=tw+4;
        }
    }

    // Column header click for sorting (processes tab)
    if(current_tab==TM_PROCESSES && my >= TAB_H+1 && my < TAB_H+1+ROW_H){
        if(mx>=48 && mx<200){ sort_col=TM_SORT_NAME; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=200 && mx<258){ sort_col=TM_SORT_CPU; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=258 && mx<320){ sort_col=TM_SORT_MEM; sort_asc=!sort_asc; SortProcesses(); return true; }
        if(mx>=8 && mx<48){ sort_col=TM_SORT_PID; sort_asc=!sort_asc; SortProcesses(); return true; }
    }

    // Process selection
    if(current_tab==TM_PROCESSES || current_tab==TM_DETAILS){
        int list_y = TAB_H + 1 + ROW_H;
        if(my >= list_y){
            int row = (my - list_y) / ROW_H + scroll_offset;
            if(row>=0 && row<proc_count){
                selected_proc=row;
                return true;
            }
        }
    }

    // Refresh on 'r'
    if(key=='r' || key=='R'){
        RefreshProcesses();
        return true;
    }

    return false;
}
