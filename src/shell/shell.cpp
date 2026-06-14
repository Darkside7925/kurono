#include "shell.h"
#include "linux_cmds.h"
#include "windows_cmds.h"
#include "../system/conduit.h"
#include "../system/ui_config.h"
#include "../ui/desktop.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../linux/ext4.h"
#include "../kernel/heap.h"
#include "../kernel/userspace.h"
#include "../kernel/elf_loader.h"
#include "../linux/linux_syscall.h"
#include "../apps/denji_app.h"
#include "../apps/firefox_launcher.h"
#include "../packages/pkgmgr.h"
#include "../linux/ld_kurono.h"
#include "../proc/scheduler.h"
#include "../media/embedded_media.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/display_mgr.h"
#include "../hal/hal.h"
#include "../hal/cpufreq.h"
#include "../system/screenshot.h"
#include "../ui/notification.h"
#include "../kernel/time.h"
#include "../drivers/timer.h"
// Note: PumpUI must advance TimeManager so wallclock-based loops
// (Time::GetTicks via TimeManager::monotonic_ms) don't freeze while a
// shell command blocks the kernel main loop.
#include "../system/user_mgmt.h"
#include "../virt/hypervisor.h"
#include "../media/codec.h"
#include "../apps/denji_app.h"
#include "../virt/virtio_gpu_host.h"
#include "../virt/vpci.h"
#include "../virt/pci_passthrough.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../system/input_manager.h"
#include "../security/supr.h"   // supr policy / passwd commands (satoru)
#include "../security/ksa.h"    // ksa status for `supr policy` (satoru)
#include "../kcl/kcl.h"

//  kurono shell implementation

CmdEnvironment KuronoShell::current_env = ENV_KURONO;
ShellCommand KuronoShell::commands[SHELL_MAX_COMMANDS];
int KuronoShell::command_count = 0;
ShellAlias KuronoShell::aliases[SHELL_MAX_ALIAS];
int KuronoShell::alias_count = 0;
char KuronoShell::history[SHELL_MAX_HIST][SHELL_MAX_CMD];
int KuronoShell::history_count = 0;
int KuronoShell::history_index = 0;
ShellVar KuronoShell::vars[SHELL_MAX_VARS];
int KuronoShell::var_count = 0;
bool KuronoShell::conflict_pending = false;
ShellCommand* KuronoShell::conflict_choices[8] = {nullptr};
uint8_t KuronoShell::conflict_backend[8] = {0};
int KuronoShell::conflict_count = 0;
char KuronoShell::conflict_cmdline[SHELL_MAX_CMD] = {0};
bool KuronoShell::pwsh_available = false;
char KuronoShell::alpine_cmd_cache[4096] = {0};
bool KuronoShell::alpine_cmd_cached = false;

static ShellOutputChunkCallback g_shell_chunk_cb = nullptr;
static void* g_shell_chunk_udata = nullptr;
static bool g_shell_incremental_used = false;
static volatile bool g_shell_cancel_requested = false;

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void scpy(char* d, const char* s, int max) {
    int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

static void scat(char* d, const char* s, int max) {
    int dl = slen(d); int i = 0;
    while (s[i] && dl + i < max - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
}

static bool seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool sstart(const char* s, const char* prefix) {
    while (*prefix) { if (*s != *prefix) return false; s++; prefix++; }
    return true;
}

static bool should_show_guest_command(const char* name) {
    bool enabled = Hypervisor::IsLinuxGuestEnabled();
    LinuxGuestProfile profile = Hypervisor::GetLinuxGuestProfile();

    if (seq(name, "alpine") || seq(name, "apk") || seq(name, "ffmpeg") ||
        seq(name, "ffprobe") || seq(name, "pwsh-setup")) {
        return enabled && profile == LINUX_GUEST_ALPINE;
    }
    if (seq(name, "debian") || seq(name, "apt")) {
        return enabled && profile == LINUX_GUEST_DEBIAN;
    }
    return true;
}

static int sappend(char* buf, int pos, int max, const char* str) {
    while (*str && pos < max - 1) buf[pos++] = *str++;
    buf[pos] = 0;
    return pos;
}

static int sappend_char(char* buf, int pos, int max, char c) {
    if (pos < max - 1) { buf[pos++] = c; buf[pos] = 0; }
    return pos;
}

static int sappend_int(char* buf, int pos, int max, int val) {
    if (val < 0) { pos = sappend_char(buf, pos, max, '-'); val = -val; }
    if (val == 0) return sappend_char(buf, pos, max, '0');
    char tmp[12]; int ti = 0;
    while (val > 0) { tmp[ti++] = '0' + (val % 10); val /= 10; }
    while (ti > 0) pos = sappend_char(buf, pos, max, tmp[--ti]);
    return pos;
}

static bool is_space(char c) { return c == ' ' || c == '\t'; }

void KuronoShell::Init() {
    command_count = 0;
    alias_count = 0;
    history_count = 0;
    var_count = 0;
    current_env = ENV_KURONO;

    // session variables
    const char* current_user = UserManager::GetCurrentUsername();
    char home_path[96];
    home_path[0] = 0;
    scpy(home_path, "/home/", 96);
    scat(home_path, current_user, 96);

    SetVar("USER", current_user);
    SetVar("HOME", home_path);
    SetVar("SHELL", "/bin/ksh");
    SetVar("PATH", "/bin:/usr/bin:/kurono/bin");
    SetVar("HOSTNAME", "kurono-machine");
    SetVar("TERM", "kurono-256color");
    SetVar("PWD", home_path);
    SetVar("PS1", "\\u@\\h:\\w$ ");

    RegisterBuiltins();
    SerialLogger::Log("Shell: Initialized\r\n");
}

void KuronoShell::RegisterCommand(const char* name, const char* desc,
                                   CmdEnvironment env, const char* category,
                                   ShellCmdHandler handler) {
    if (command_count >= SHELL_MAX_COMMANDS) return;
    ShellCommand& c = commands[command_count++];
    scpy(c.name, name, 32);
    scpy(c.description, desc, 64);
    c.env = env;
    scpy(c.category, category, 16);
    c.handler = handler;
}

ShellCommand* KuronoShell::FindCommand(const char* name) {
    // check aliases first
    const char* real = GetAlias(name);
    if (real) name = real;

    // exact match in current env first
    for (int i = 0; i < command_count; i++) {
        if (seq(commands[i].name, name) && commands[i].env == current_env)
            return &commands[i];
    }
    // then any env
    for (int i = 0; i < command_count; i++) {
        if (seq(commands[i].name, name))
            return &commands[i];
    }
    return nullptr;
}

ShellCommand* KuronoShell::GetCommands() { return commands; }
int KuronoShell::GetCommandCount() { return command_count; }

// find all commands matching a name (for conflict detection)
int KuronoShell::FindAllCommands(const char* name, ShellCommand** results, int max_results) {
    const char* real = GetAlias(name);
    if (real) name = real;
    int count = 0;
    // track which environments we've already found
    bool env_seen[4] = {false, false, false, false};
    for (int i = 0; i < command_count && count < max_results; i++) {
        if (seq(commands[i].name, name)) {
            int e = (int)commands[i].env;
            if (e < 4 && !env_seen[e]) {
                results[count++] = &commands[i];
                env_seen[e] = true;
            }
        }
    }
    return count;
}

// resolve a pending conflict by choosing option 1-n
int KuronoShell::ResolveConflict(int choice, char* output, int max_output) {
    if (!conflict_pending || choice < 1 || choice > conflict_count) {
        return sappend(output, 0, max_output, "Invalid selection.\n");
    }
    conflict_pending = false;

    uint8_t backend = conflict_backend[choice - 1];

    if (backend == CB_ALPINE) {
        int r = RunViaAlpine(conflict_cmdline, output, max_output);
        ConduitBridge::RecordCommand(conflict_cmdline);
        return r;
    }
    if (backend == CB_PWSH) {
        int r = RunViaPwsh(conflict_cmdline, output, max_output);
        ConduitBridge::RecordCommand(conflict_cmdline);
        return r;
    }

    // cb_registered  -  use the stored shellcommand handler
    ShellCommand* cmd = conflict_choices[choice - 1];
    if (!cmd || !cmd->handler) return 0;

    // parse the original command line for args
    const char* argv[SHELL_MAX_ARGS];
    int argc = ParseArgs(conflict_cmdline, argv, SHELL_MAX_ARGS);
    if (argc == 0) return 0;

    static KuronoShell shell_singleton;
    int r = cmd->handler(&shell_singleton, argc, argv, output, max_output);
    ConduitBridge::RecordCommand(conflict_cmdline);
    return r;
}

CmdEnvironment KuronoShell::GetEnv() { return current_env; }
void KuronoShell::SetEnv(CmdEnvironment e) { current_env = e; }
const char* KuronoShell::EnvName(CmdEnvironment e) {
    switch (e) {
        case ENV_KURONO: return "kurono";
        case ENV_LINUX: return "linux";
        case ENV_WINDOWS: return "windows";
        case ENV_DEBIAN: return "debian";
        default: return "auto";
    }
}

void KuronoShell::SetVar(const char* key, const char* value) {
    for (int i = 0; i < var_count; i++) {
        if (seq(vars[i].key, key)) {
            scpy(vars[i].value, value, 128);
            return;
        }
    }
    if (var_count < SHELL_MAX_VARS) {
        scpy(vars[var_count].key, key, 32);
        scpy(vars[var_count].value, value, 128);
        var_count++;
    }
}

const char* KuronoShell::GetVar(const char* key) {
    for (int i = 0; i < var_count; i++) {
        if (seq(vars[i].key, key)) return vars[i].value;
    }
    return nullptr;
}

void KuronoShell::ExpandVars(const char* input, char* output, int max_len) {
    int o = 0;
    for (int i = 0; input[i] && o < max_len - 1; ) {
        if (input[i] == '$') {
            i++;
            char varname[32];
            int vi = 0;
            bool braced = (input[i] == '{');
            if (braced) i++;
            while (input[i] && ((braced && input[i] != '}') || (!braced && ((input[i] >= 'A' && input[i] <= 'Z') || (input[i] >= 'a' && input[i] <= 'z') || input[i] == '_' || (input[i] >= '0' && input[i] <= '9'))))) {
                if (vi < 31) varname[vi++] = input[i];
                i++;
            }
            varname[vi] = 0;
            if (braced && input[i] == '}') i++;
            const char* val = GetVar(varname);
            if (val) o = sappend(output, o, max_len, val);
        } else {
            output[o++] = input[i++];
        }
    }
    output[o] = 0;
}

void KuronoShell::SetAlias(const char* name, const char* value) {
    for (int i = 0; i < alias_count; i++) {
        if (seq(aliases[i].name, name)) {
            scpy(aliases[i].value, value, 128);
            return;
        }
    }
    if (alias_count < SHELL_MAX_ALIAS) {
        scpy(aliases[alias_count].name, name, 32);
        scpy(aliases[alias_count].value, value, 128);
        alias_count++;
    }
}

const char* KuronoShell::GetAlias(const char* name) {
    for (int i = 0; i < alias_count; i++) {
        if (seq(aliases[i].name, name)) return aliases[i].value;
    }
    return nullptr;
}

void KuronoShell::AddHistory(const char* line) {
    if (!line[0]) return;
    int idx = history_count % SHELL_MAX_HIST;
    scpy(history[idx], line, SHELL_MAX_CMD);
    history_count++;
}

const char* KuronoShell::GetHistory(int index) {
    if (index < 0 || index >= history_count) return "";
    int total = history_count < SHELL_MAX_HIST ? history_count : SHELL_MAX_HIST;
    int start = history_count > SHELL_MAX_HIST ? history_count - SHELL_MAX_HIST : 0;
    if (index < start || index >= history_count) return "";
    return history[index % SHELL_MAX_HIST];
}

int KuronoShell::GetHistoryCount() {
    return history_count < SHELL_MAX_HIST ? history_count : SHELL_MAX_HIST;
}

int KuronoShell::ParseArgs(const char* line, const char** argv, int max_args) {
    static char arg_buf[SHELL_MAX_CMD];
    scpy(arg_buf, line, SHELL_MAX_CMD);

    int argc = 0;
    int i = 0;
    while (arg_buf[i] && argc < max_args) {
        while (is_space(arg_buf[i])) i++;
        if (!arg_buf[i]) break;

        if (arg_buf[i] == '"') {
            i++;
            argv[argc++] = &arg_buf[i];
            while (arg_buf[i] && arg_buf[i] != '"') i++;
            if (arg_buf[i]) arg_buf[i++] = 0;
        } else if (arg_buf[i] == '\'') {
            i++;
            argv[argc++] = &arg_buf[i];
            while (arg_buf[i] && arg_buf[i] != '\'') i++;
            if (arg_buf[i]) arg_buf[i++] = 0;
        } else {
            argv[argc++] = &arg_buf[i];
            while (arg_buf[i] && !is_space(arg_buf[i])) i++;
            if (arg_buf[i]) arg_buf[i++] = 0;
        }
    }
    return argc;
}

void KuronoShell::ProcessLine(const char* line, char* output, int max_output) {
    Execute(line, output, max_output);
}

void KuronoShell::SetOutputChunkCallback(ShellOutputChunkCallback fn, void* udata) {
    g_shell_chunk_cb = fn;
    g_shell_chunk_udata = udata;
}

void KuronoShell::ClearOutputChunkCallback() {
    g_shell_chunk_cb = nullptr;
    g_shell_chunk_udata = nullptr;
}

void KuronoShell::EmitIncrementalRange(const char* buf, int from, int to_exclusive) {
    if (!g_shell_chunk_cb || !buf || from >= to_exclusive || to_exclusive < from)
        return;
    g_shell_incremental_used = true;
    constexpr int kChunk = 256;
    while (from < to_exclusive) {
        int n = to_exclusive - from;
        if (n > kChunk) n = kChunk;
        g_shell_chunk_cb(g_shell_chunk_udata, buf + from, n);
        from += n;
    }
}

bool KuronoShell::TakeIncrementalOutputUsed() {
    bool used = g_shell_incremental_used;
    g_shell_incremental_used = false;
    return used;
}

void KuronoShell::ClearCommandCancel() { g_shell_cancel_requested = false; }
void KuronoShell::RequestCommandCancel() { g_shell_cancel_requested = true; }
bool KuronoShell::IsCommandCancelRequested() {
    return g_shell_cancel_requested;
}

void KuronoShell::PumpUI() {
    static bool reentrant = false;
    static uint32_t last_pump_ms = 0;
    if (reentrant) return;

    /* Always advance the wallclock so Time::GetTicks() (consumed by ping/DNS/etc.)
       keeps moving even while a synchronous shell command blocks the kernel
       main loop. Throttling only the heavy UI work below. */
    uint32_t real_elapsed = Timer::ElapsedSinceLast();
    if (real_elapsed > 0)
        TimeManager::AdvanceByMs(real_elapsed);

    uint32_t now_ms = Timer::GetRealMs();
    if ((uint32_t)(now_ms - last_pump_ms) < 16u) return;
    last_pump_ms = now_ms;
    reentrant = true;

    InputManager::Poll();

    int scroll_delta = 0;
    while (Mouse::HasEvent()) {
        Mouse::Event mevt = Mouse::GetEvent();
        if (mevt.type == 3) {
            scroll_delta += mevt.dz;
            continue;
        }
        if (mevt.type == 1 || mevt.type == 2) {
            WindowManager::HandlePointerButton(mevt.x, mevt.y,
                                               (int)mevt.button,
                                               mevt.type == 1);
        }
    }

    WindowManager::HandlePointerMove(Mouse::mx, Mouse::my);

    // Drain keyboard chars through the desktop environment  -  mirrors the
    // kernel main loop's input pattern so keystrokes reach the focused
    // window even while a shell command blocks the main loop.
    {
        char kb = 0;
        if (Keyboard::HasChar()) kb = Keyboard::GetChar();
        DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my,
                                        Mouse::IsLeftDown(),
                                        Mouse::LeftClicked(), kb);
        while (Keyboard::HasChar()) {
            kb = Keyboard::GetChar();
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, false, false, kb);
        }
    }

    if (scroll_delta != 0) {
        Window* fw = WindowManager::GetFocusedWindow();
        if (fw && fw->input) fw->input(fw, 3, scroll_delta, 0);
    }

    DesktopEnvironment::Update();
    if (Graphics::ShouldRender()) {
        DesktopEnvironment::Render();
        Mouse::DrawAt(Mouse::mx, Mouse::my);
        Graphics::SwapBuffers();
    }

    reentrant = false;
}

int KuronoShell::Execute(const char* cmdline, char* output, int max_output) {
    if (!cmdline || !cmdline[0]) return 0;
    output[0] = 0;

    ClearCommandCancel();
    g_shell_incremental_used = false;

    // add to history
    AddHistory(cmdline);

    // if there's a pending conflict, check if the user typed a number to resolve it
    if (conflict_pending) {
        int choice = 0;
        if (cmdline[0] >= '1' && cmdline[0] <= '9' && (cmdline[1] == 0 || cmdline[1] == ' ')) {
            choice = cmdline[0] - '0';
        }
        if (choice >= 1 && choice <= conflict_count) {
            return ResolveConflict(choice, output, max_output);
        }
        // not a valid selection -- cancel conflict and process as normal command
        conflict_pending = false;
    }

    // variable expansion
    char expanded[SHELL_MAX_CMD];
    ExpandVars(cmdline, expanded, SHELL_MAX_CMD);

    // check for pipes
    bool has_pipe = false;
    for (int i = 0; expanded[i]; i++) {
        if (expanded[i] == '|') { has_pipe = true; break; }
    }
    if (has_pipe) return ExecutePiped(expanded, output, max_output);

    // check for cross-env prefix: "linux:cmd" or "windows:cmd"
    const char* actual_cmd = expanded;
    CmdEnvironment saved_env = current_env;
    bool temp_env = false;

    if (sstart(expanded, "linux:")) {
        current_env = ENV_LINUX; actual_cmd = expanded + 6; temp_env = true;
    } else if (sstart(expanded, "windows:")) {
        current_env = ENV_WINDOWS; actual_cmd = expanded + 8; temp_env = true;
    } else if (sstart(expanded, "kurono:")) {
        current_env = ENV_KURONO; actual_cmd = expanded + 7; temp_env = true;
    } else if (sstart(expanded, "debian:")) {
        current_env = ENV_DEBIAN; actual_cmd = expanded + 7; temp_env = true;
    }

    // parse args
    const char* argv[SHELL_MAX_ARGS];
    int argc = ParseArgs(actual_cmd, argv, SHELL_MAX_ARGS);
    if (argc == 0) {    
        if (temp_env) current_env = saved_env;
        return 0;
    }

    // pings all execution backends: registered commands, alpine vm, powershell
    if (!temp_env) {
        ShellCommand* matches[4];
        int match_count = FindAllCommands(argv[0], matches, 4);

        // separate registered non-auto matches
        int reg_count = 0;
        ShellCommand* reg_matches[4];
        for (int i = 0; i < match_count; i++) {
            if (matches[i]->env != ENV_AUTO)
                reg_matches[reg_count++] = matches[i];
        }

        // build combined option list from all os backends
        int total = 0;
        for (int i = 0; i < reg_count && total < 8; i++) {
            conflict_choices[total] = reg_matches[i];
            conflict_backend[total] = CB_REGISTERED;
            total++;
        }

        // dynamic probe: ping alpine vm + powershell at kernel level
        // probe when: 2+ registered conflicts (extend menu) or 0 registered & no auto (fallback)
        bool has_auto = (match_count > reg_count);
        bool do_probe = (reg_count >= 2) || (reg_count == 0 && !has_auto);

        if (do_probe) {
            if (ProbeAlpine(argv[0]) && total < 8) {
                conflict_choices[total] = nullptr;
                conflict_backend[total] = CB_ALPINE;
                total++;
            }
            if (ProbePwsh(argv[0]) && total < 8) {
                conflict_choices[total] = nullptr;
                conflict_backend[total] = CB_PWSH;
                total++;
            }
        }

        if (total > 1) {
            // multiple os backends have this command → present selector
            conflict_pending = true;
            conflict_count = total;
            scpy(conflict_cmdline, actual_cmd, SHELL_MAX_CMD);

            int p = 0;
            p = sappend(output, p, max_output, "\033[33m[Conflict] '");
            p = sappend(output, p, max_output, argv[0]);
            p = sappend(output, p, max_output, "' found across multiple OS backends:\033[0m\n");
            for (int i = 0; i < total; i++) {
                p = sappend(output, p, max_output, "  ");
                p = sappend_char(output, p, max_output, '1' + i);
                p = sappend(output, p, max_output, ")  ");
                if (conflict_backend[i] == CB_REGISTERED) {
                    p = sappend(output, p, max_output, EnvName(conflict_choices[i]->env));
                    p = sappend(output, p, max_output, ":");
                    p = sappend(output, p, max_output, conflict_choices[i]->name);
                    p = sappend(output, p, max_output, "  -- ");
                    p = sappend(output, p, max_output, conflict_choices[i]->description);
                } else if (conflict_backend[i] == CB_ALPINE) {
                    p = sappend(output, p, max_output, "\033[32mAlpine VM\033[0m");
                    p = sappend(output, p, max_output, "  -- Run in Alpine Linux guest");
                } else if (conflict_backend[i] == CB_PWSH) {
                    p = sappend(output, p, max_output, "\033[36mPowerShell\033[0m");
                    p = sappend(output, p, max_output, "  -- Run via pwsh in Alpine");
                }
                p = sappend_char(output, p, max_output, '\n');
            }
            p = sappend(output, p, max_output, "Select (1-");
            p = sappend_char(output, p, max_output, '0' + total);
            p = sappend(output, p, max_output, "): ");
            if (temp_env) current_env = saved_env;
            return p;
        }

        // single dynamic-only match (no registered cmd) → execute directly
        if (total == 1 && reg_count == 0) {
            int r = 0;
            if (conflict_backend[0] == CB_ALPINE)
                r = RunViaAlpine(actual_cmd, output, max_output);
            else if (conflict_backend[0] == CB_PWSH)
                r = RunViaPwsh(actual_cmd, output, max_output);
            ConduitBridge::RecordCommand(actual_cmd);
            if (temp_env) current_env = saved_env;
            return r;
        }
    }

    // find and execute command -- silently uses current env match first
    ShellCommand* cmd = FindCommand(argv[0]);

    int result = 0;
    if (cmd && cmd->handler) {
        static KuronoShell shell_singleton;
        result = cmd->handler(&shell_singleton, argc, argv, output, max_output);
    } else {
        // check if it's a kcl script  -  run it through the kcl interpreter.
        // resolve a bare name against the cwd if it isn't already a path. (satoru)
        int nlen = slen(argv[0]);
        if (nlen > 4 && argv[0][nlen-4] == '.' && argv[0][nlen-3] == 'k' &&
            argv[0][nlen-2] == 'c' && argv[0][nlen-1] == 'l') {
            if (KVFS::Exists(argv[0])) {
                result = KCL::ExecFile(argv[0], output, max_output);
            } else {
                char path[KVFS_MAX_PATH];
                const char* cwd = KVFS::GetCwd();
                int pi = 0;
                for (int i = 0; cwd && cwd[i] && pi < KVFS_MAX_PATH - 1; i++) path[pi++] = cwd[i];
                if (pi == 0 || path[pi-1] != '/') { if (pi < KVFS_MAX_PATH - 1) path[pi++] = '/'; }
                for (int i = 0; argv[0][i] && pi < KVFS_MAX_PATH - 1; i++) path[pi++] = argv[0][i];
                path[pi] = 0;
                result = KCL::ExecFile(path, output, max_output);
            }
        } else {
            result = sappend(output, 0, max_output, "ksh: command not found: ");
            result = sappend(output, result, max_output, argv[0]);
            result = sappend_char(output, result, max_output, '\n');
        }
    }

    ConduitBridge::RecordCommand(actual_cmd);

    if (temp_env) current_env = saved_env;
    return result;
}

int KuronoShell::ExecutePiped(const char* cmdline, char* output, int max_output) {
    // split by |
    char segments[8][SHELL_MAX_CMD];
    int seg_count = 0;
    int si = 0;

    for (int i = 0; cmdline[i]; i++) {
        if (cmdline[i] == '|') {
            segments[seg_count][si] = 0;
            seg_count++;
            si = 0;
            if (seg_count >= 8) break;
        } else {
            if (si < SHELL_MAX_CMD - 1) segments[seg_count][si++] = cmdline[i];
        }
    }
    segments[seg_count][si] = 0;
    seg_count++;

    // execute in sequence, piping output → input
    char buf_a[SHELL_OUTPUT_BUF];
    char buf_b[SHELL_OUTPUT_BUF];
    buf_a[0] = 0;
    buf_b[0] = 0;

    for (int i = 0; i < seg_count; i++) {
        // trim leading spaces
        const char* seg = segments[i];
        while (*seg == ' ') seg++;

        char* in_buf = (i % 2 == 0) ? buf_a : buf_b;
        char* out_buf = (i % 2 == 0) ? buf_b : buf_a;
        out_buf[0] = 0;

        // for piped commands, we'd need stdin passing (simplified: just execute)
        Execute(seg, out_buf, SHELL_OUTPUT_BUF);

        // if first command, we've set up output. subsequent would need stdin.
    }

    // copy final output
    char* final_buf = (seg_count % 2 == 0) ? buf_a : buf_b;
    scpy(output, final_buf, max_output);
    return slen(output);
}

int KuronoShell::ExecuteCrossEnv(const char* cmdline, char* output, int max_output) {
    return Execute(cmdline, output, max_output);
}

void KuronoShell::GetPrompt(char* buf, int max_len) {
    int p = 0;
    const char* user = GetVar("USER");
    const char* host = GetVar("HOSTNAME");
    const char* cwd = KVFS::GetCwd();
    const char* ps1 = GetVar("PS1");

    if (!user) user = "user";
    if (!host) host = "kurono";
    if (!cwd) cwd = "/";

    // windows environment uses its own ps1 style
    if (current_env == ENV_WINDOWS) {
        if (ps1 && ps1[0]) {
            p = sappend(buf, p, max_len, ps1);
        } else {
            p = sappend(buf, p, max_len, "C:\\> ");
        }
        return;
    }

    // linux & kurono environments: interpret ps1 escape sequences
    // supported: \u (user), \h (host), \w (cwd with ~ abbrev), \$ ($ or #)
    if (ps1 && ps1[0]) {
        const char* home = GetVar("HOME");
        for (int i = 0; ps1[i] && p < max_len - 1; i++) {
            if (ps1[i] == '\\' && ps1[i+1]) {
                i++;
                switch (ps1[i]) {
                    case 'u': p = sappend(buf, p, max_len, user); break;
                    case 'h': p = sappend(buf, p, max_len, host); break;
                    case 'w':
                        if (home && sstart(cwd, home)) {
                            p = sappend_char(buf, p, max_len, '~');
                            p = sappend(buf, p, max_len, cwd + slen(home));
                        } else {
                            p = sappend(buf, p, max_len, cwd);
                        }
                        break;
                    case '$': p = sappend_char(buf, p, max_len, '$'); break;
                    case 'n': p = sappend_char(buf, p, max_len, '\n'); break;
                    default:  p = sappend_char(buf, p, max_len, ps1[i]); break;
                }
            } else {
                p = sappend_char(buf, p, max_len, ps1[i]);
            }
        }
    } else {
        // default fallback: user@host:cwd$
        p = sappend(buf, p, max_len, user);
        p = sappend_char(buf, p, max_len, '@');
        p = sappend(buf, p, max_len, host);
        p = sappend_char(buf, p, max_len, ':');
        const char* home = GetVar("HOME");
        if (home && sstart(cwd, home)) {
            p = sappend_char(buf, p, max_len, '~');
            p = sappend(buf, p, max_len, cwd + slen(home));
        } else {
            p = sappend(buf, p, max_len, cwd);
        }
        p = sappend(buf, p, max_len, "$ ");
    }
}

//  dynamic os probing  -  kernel-level backend detection

// known powershell commands/aliases for fast probe (no vm call needed)
static const char* pwsh_known[] = {
    "whoami", "hostname", "ls", "cat", "pwd", "date", "echo", "mkdir", "rm",
    "cp", "mv", "ps", "kill", "clear", "sort", "man", "cd", "dir", "type",
    "del", "copy", "move", "ren", "find", "select", "where", "foreach",
    "write", "read", "set", "get", "new", "remove", "start", "stop",
    "test", "measure", "compare", "format", "out", "export", "import",
    "invoke", "ping", "curl", "wget", "head", "tail", "wc", "grep",
    "uname", "uptime", "id", "chmod", "chown", "df", "du", "mount",
    nullptr
};

// cache alpine's available commands (fetched from vm once per boot)
void KuronoShell::CacheAlpineCommands() {
    if (alpine_cmd_cached) return;
    if (!Hypervisor::IsLinuxGuestEnabled() || Hypervisor::GetLinuxGuestProfile() != LINUX_GUEST_ALPINE) return;
    if (!Hypervisor::IsAlpineBooted()) return;
    if (Hypervisor::GetStats().total_exits == 0) return;

    char result[4096];
    result[0] = 0;
    // busybox provides most commands; also scan /usr/bin
    int n = Hypervisor::AlpineExec(
        "busybox --list 2>/dev/null; ls /usr/bin /usr/sbin 2>/dev/null",
        result, (int)sizeof(result) - 1);
    if (n > 0) {
        result[n] = 0;
        // convert newlines to spaces for easy word search
        for (int i = 0; i < n; i++) {
            if (result[i] == '\n' || result[i] == '\r') result[i] = ' ';
        }
        scpy(alpine_cmd_cache, result, 4096);
        alpine_cmd_cached = true;
    }
}

// check if a command exists in alpine vm (cached word search)
bool KuronoShell::ProbeAlpine(const char* cmd_name) {
    if (!Hypervisor::IsLinuxGuestEnabled() || Hypervisor::GetLinuxGuestProfile() != LINUX_GUEST_ALPINE)
        return false;
    if (!Hypervisor::IsAlpineBooted()) return false;
    if (!alpine_cmd_cached) CacheAlpineCommands();
    if (!alpine_cmd_cached) return false;

    int cmd_len = 0;
    while (cmd_name[cmd_len]) cmd_len++;
    if (cmd_len == 0 || cmd_len > 63) return false;

    // word-boundary search in the cached space-separated list
    const char* p = alpine_cmd_cache;
    while (*p) {
        // skip spaces
        while (*p == ' ') p++;
        if (!*p) break;
        // compare word
        const char* start = p;
        int wlen = 0;
        while (*p && *p != ' ') { p++; wlen++; }
        if (wlen == cmd_len) {
            bool match = true;
            for (int i = 0; i < cmd_len; i++) {
                if (start[i] != cmd_name[i]) { match = false; break; }
            }
            if (match) return true;
        }
    }
    return false;
}

// check if a command is a known powershell cmdlet/alias
bool KuronoShell::ProbePwsh(const char* cmd_name) {
    if (!pwsh_available) return false;
    for (int i = 0; pwsh_known[i]; i++) {
        if (seq(cmd_name, pwsh_known[i])) return true;
    }
    // also check cmdlet pattern: verb-noun (contains '-')
    for (const char* c = cmd_name; *c; c++) {
        if (*c == '-') return true;
    }
    return false;
}

// execute a command in the alpine vm guest
int KuronoShell::RunViaAlpine(const char* cmdline, char* output, int max_output) {
    if (!Hypervisor::IsLinuxGuestEnabled()) {
        return sappend(output, 0, max_output,
            "\033[31m[Alpine] Linux guest integration is disabled in Settings > System > Linux.\033[0m\n");
    }
    if (Hypervisor::GetLinuxGuestProfile() != LINUX_GUEST_ALPINE) {
        return sappend(output, 0, max_output,
            "\033[31m[Alpine] Alpine is not the selected guest profile. Switch it in Settings > System > Linux.\033[0m\n");
    }
    if (!Hypervisor::IsAlpineBooted()) {
        return sappend(output, 0, max_output,
            "\033[31m[Alpine] VM not booted. Run 'alpine boot' first.\033[0m\n");
    }
    char result[4096];
    int n = Hypervisor::AlpineExec(cmdline, result, (int)sizeof(result) - 1);
    if (n <= 0) {
        int p = 0;
        p = sappend(output, p, max_output, "\033[32m[Alpine]\033[0m ");
        p = sappend(output, p, max_output, "(no output)\n");
        return p;
    }
    result[n] = 0;
    int p = 0;
    p = sappend(output, p, max_output, "\033[32m[Alpine]\033[0m ");
    p = sappend(output, p, max_output, result);
    if (n > 0 && result[n-1] != '\n')
        p = sappend_char(output, p, max_output, '\n');
    return p;
}

// execute a command via powershell (pwsh in alpine vm)
int KuronoShell::RunViaPwsh(const char* cmdline, char* output, int max_output) {
    if (!Hypervisor::IsLinuxGuestEnabled() || Hypervisor::GetLinuxGuestProfile() != LINUX_GUEST_ALPINE) {
        return sappend(output, 0, max_output,
            "\033[31m[PowerShell] PowerShell bridge currently requires the Alpine guest profile.\033[0m\n");
    }
    if (!pwsh_available) {
        return sappend(output, 0, max_output,
            "\033[31m[PowerShell] Not installed. Run 'pwsh-setup' first.\033[0m\n");
    }
    // build: pwsh -noprofile -c "cmdline"
    char pwsh_cmd[512];
    scpy(pwsh_cmd, "pwsh -NoProfile -c \"", 512);
    scat(pwsh_cmd, cmdline, 500);
    scat(pwsh_cmd, "\"", 512);

    char result[4096];
    int n = Hypervisor::AlpineExec(pwsh_cmd, result, (int)sizeof(result) - 1);
    if (n <= 0) {
        int p = 0;
        p = sappend(output, p, max_output, "\033[36m[PowerShell]\033[0m ");
        p = sappend(output, p, max_output, "(no output)\n");
        return p;
    }
    result[n] = 0;
    int p = 0;
    p = sappend(output, p, max_output, "\033[36m[PowerShell]\033[0m ");
    p = sappend(output, p, max_output, result);
    if (n > 0 && result[n-1] != '\n')
        p = sappend_char(output, p, max_output, '\n');
    return p;
}

namespace ShellBuiltins {
    int cmd_crash(KuronoShell*, int, const char**, char*, int);
    int cmd_gpu(KuronoShell*, int, const char**, char*, int);
    int cmd_kurono(KuronoShell*, int, const char**, char*, int);
    int cmd_screenshot(KuronoShell*, int, const char**, char*, int);
    int cmd_cpufreq(KuronoShell*, int, const char**, char*, int);
    int cmd_poweroff(KuronoShell*, int, const char**, char*, int);
    int cmd_suspend(KuronoShell*, int, const char**, char*, int);
    int cmd_ffmpeg(KuronoShell*, int, const char**, char*, int);
    int cmd_wltest(KuronoShell*, int, const char**, char*, int);
    int cmd_pthtest(KuronoShell*, int, const char**, char*, int);
    int cmd_playvideo(KuronoShell*, int, const char**, char*, int);
    int cmd_persisttest(KuronoShell*, int, const char**, char*, int);
    int cmd_supr(KuronoShell*, int, const char**, char*, int);
    int cmd_firefox(KuronoShell*, int, const char**, char*, int);
}

void KuronoShell::RegisterBuiltins() {
    using namespace ShellBuiltins;
    RegisterCommand("help",     "Show available commands",     ENV_KURONO, "builtin", cmd_help);
    RegisterCommand("denji",    "Open Denji video player",     ENV_KURONO, "media",   cmd_denji);
    RegisterCommand("ffmpeg",   "Run embedded ffmpeg transcoder", ENV_AUTO, "media",   cmd_ffmpeg);
    RegisterCommand("wltest",   "Run the wl_shm wayland render test", ENV_AUTO, "media", cmd_wltest);
    RegisterCommand("pthtest",  "Run the pthreads (clone+futex) smoke test", ENV_AUTO, "media", cmd_pthtest);
    RegisterCommand("firefox",  "Launch Firefox (installs it first if needed)", ENV_AUTO, "apps", cmd_firefox);
    RegisterCommand("persisttest","Test kvfs persistence across reboot", ENV_AUTO, "system", cmd_persisttest);
    RegisterCommand("playvideo","Play the imported video (ssstik)", ENV_AUTO, "media",  cmd_playvideo);
    RegisterCommand("vgpu",     "VirtIO-GPU host status",      ENV_KURONO, "virt",    cmd_vgpu);
    RegisterCommand("version",  "Show OS version",             ENV_KURONO, "builtin", cmd_version);
    RegisterCommand("env",      "Show current environment",    ENV_KURONO, "builtin", cmd_env);
    RegisterCommand("switch",   "Switch environment",          ENV_KURONO, "builtin", cmd_switch);
    RegisterCommand("clear",    "Clear terminal",              ENV_KURONO, "builtin", cmd_clear);
    RegisterCommand("echo",     "Print text",                  ENV_KURONO, "builtin", cmd_echo);
    RegisterCommand("set",      "Set variable",                ENV_KURONO, "builtin", cmd_set);
    RegisterCommand("alias",    "Set command alias",           ENV_KURONO, "builtin", cmd_alias);
    RegisterCommand("history",  "Show command history",        ENV_KURONO, "builtin", cmd_history);
    RegisterCommand("exit",     "Exit shell",                  ENV_KURONO, "builtin", cmd_exit);
    RegisterCommand("reboot",   "Reboot system",               ENV_KURONO, "builtin", cmd_reboot);
    RegisterCommand("shutdown", "Shutdown system",              ENV_KURONO, "builtin", cmd_shutdown);
    RegisterCommand("restart",  "Restart shell",                ENV_KURONO, "builtin", cmd_restart);
    RegisterCommand("crash",    "Trigger kernel panic test",  ENV_KURONO, "system",  cmd_crash);
    RegisterCommand("gpu",      "Show GPU information",        ENV_KURONO, "system",  cmd_gpu);
    RegisterCommand("sysinfo",  "System information",           ENV_KURONO, "builtin", cmd_sysinfo);

    // shell switch commands
    RegisterCommand("bash",      "Switch to Linux shell",        ENV_KURONO, "builtin", cmd_bash);
    RegisterCommand("linux",     "Switch to Linux environment",  ENV_KURONO, "builtin", cmd_bash);
    RegisterCommand("cmd",       "Switch to Windows shell",      ENV_KURONO, "builtin", cmd_cmd);

    // powershell  -  real pwsh via alpine vm
    RegisterCommand("pwsh-setup","Install PowerShell in Alpine", ENV_KURONO, "system",  cmd_pwsh_setup);
    RegisterCommand("pwsh",      "Run PowerShell command",       ENV_KURONO, "system",  cmd_pwsh);
    RegisterCommand("powershell","Run PowerShell command",       ENV_KURONO, "system",  cmd_pwsh);

    // common unix commands
    RegisterCommand("whoami",    "Print current user",           ENV_KURONO, "system",  cmd_whoami);
    RegisterCommand("uname",     "Print system information",     ENV_KURONO, "system",  cmd_uname);
    RegisterCommand("hostname",  "Print hostname",               ENV_KURONO, "system",  cmd_hostname);
    RegisterCommand("date",      "Print current date/time",      ENV_KURONO, "system",  cmd_date);
    RegisterCommand("uptime",    "Print system uptime",          ENV_KURONO, "system",  cmd_uptime);
    RegisterCommand("pwd",       "Print working directory",      ENV_KURONO, "filesystem",cmd_pwd);
    RegisterCommand("usermode",  "Run ring-3 demo process",      ENV_KURONO, "system",  cmd_usermode);

    // ksa / supr  -  hypervisor-backed privilege prompts + auth policy. (satoru)
    RegisterCommand("supr",      "Privilege & auth policy (KSA)", ENV_KURONO, "security", cmd_supr);

    // alpine vm bridge commands  -  embed alpine into shell
    RegisterCommand("alpine",   "Run command in Alpine VM",     ENV_KURONO, "system",  cmd_alpine);
    // ffmpeg registered above as the real embedded musl-static transcoder (satoru)
    RegisterCommand("ffprobe",  "Media info (Alpine ffprobe)",  ENV_KURONO, "system",  cmd_ffprobe);
    RegisterCommand("apk",      "Alpine package manager",       ENV_KURONO, "package", cmd_apk);
    RegisterCommand("codecs",   "List registered codecs",       ENV_KURONO, "system",  cmd_codecs);

    // kurono system command  -  reload config, show info
    RegisterCommand("kurono",  "Kurono system control",        ENV_KURONO, "system",  cmd_kurono);

    // phase 8: screenshots, cpu frequency, power management. (satoru)
    RegisterCommand("screenshot","Capture screen to a BMP",      ENV_KURONO, "system",  cmd_screenshot);
    RegisterCommand("cpufreq",  "Show per-core CPU frequency",   ENV_KURONO, "system",  cmd_cpufreq);
    RegisterCommand("poweroff", "Power off the machine",         ENV_KURONO, "system",  cmd_poweroff);
    RegisterCommand("suspend",  "Suspend to RAM (ACPI S3)",      ENV_KURONO, "system",  cmd_suspend);
}

//  built-in commands

namespace ShellBuiltins {

int cmd_help(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    int p = 0;
    p = sappend(out, p, maxo, "╔══════════════════════════════════════════╗\n");
    p = sappend(out, p, maxo, "║          Kurono OS Shell v1.0           ║\n");
    p = sappend(out, p, maxo, "╚══════════════════════════════════════════╝\n\n");

    // group by category
    const char* cats[] = {"builtin", "filesystem", "text", "system", "network", "security", "package", nullptr};
    const char* cat_names[] = {"Built-in", "Filesystem", "Text", "System", "Network", "Security", "Package"};

    for (int ci = 0; cats[ci]; ci++) {
        bool has_any = false;
        for (int i = 0; i < sh->GetCommandCount(); i++) {
            ShellCommand* c = &sh->GetCommands()[i];
            if (seq(c->category, cats[ci]) && should_show_guest_command(c->name)) {
                if (!has_any) {
                    p = sappend(out, p, maxo, " ");
                    p = sappend(out, p, maxo, cat_names[ci]);
                    p = sappend(out, p, maxo, ":\n");
                    has_any = true;
                }
                p = sappend(out, p, maxo, "   ");
                p = sappend(out, p, maxo, c->name);
                // pad to 14 chars
                int nl = slen(c->name);
                for (int j = nl; j < 14; j++) p = sappend_char(out, p, maxo, ' ');
                p = sappend(out, p, maxo, c->description);
                p = sappend_char(out, p, maxo, '\n');
            }
        }
        if (has_any) p = sappend_char(out, p, maxo, '\n');
    }

    p = sappend(out, p, maxo, " Cross-env piping: linux:ls /home | windows:findstr user\n");
    p = sappend(out, p, maxo, " Switch env: switch linux | switch windows | switch kurono\n");
    return p;
}

int cmd_version(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    return sappend(out, 0, maxo, "Kurono OS 1.0.0 \"Aurora\"\nHybrid Kernel  -  Linux · Windows · Kurono\n");
}

int cmd_denji(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    DenjiApp::Open();
    if (DenjiApp::IsOpen()) {
        return sappend(out, 0, maxo, "Opened Denji video player.\n");
    }
    return sappend(out, 0, maxo,
        "Denji video asset not embedded in this build.\n");
}

int cmd_vgpu(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    int p = 0;

    // sub-commands: passthrough <nvidia|amd>, reclaim, status (default)
    if (argc >= 2) {
        const char* sub = argv[1];
        if (sub && (sub[0]=='p' && sub[1]=='a')) { // passthrough
            const char* which = (argc >= 3) ? argv[2] : "";
            bool ok = false;
            if (which[0]=='n') ok = PCIPassthrough::HandoffNvidiaGPU();
            else if (which[0]=='a') ok = PCIPassthrough::HandoffAmdGPU();
            else return sappend(out, 0, maxo,
                                 "usage: vgpu passthrough <nvidia|amd>\n");
            return sappend(out, 0, maxo,
                            ok ? "passthrough engaged.\n"
                               : "passthrough failed (see serial log).\n");
        }
        if (sub && sub[0]=='r') { // reclaim
            PCIPassthrough::ReclaimAll();
            return sappend(out, 0, maxo, "all passthrough devices reclaimed.\n");
        }
    }

    // status (default)
    p = sappend(out, p, maxo, "VirtIO-GPU host (path A: virtio scanout)\n");
    p = sappend(out, p, maxo, "  registered : ");
    p = sappend(out, p, maxo, VirtIOGPUHost::IsRegistered() ? "yes\n" : "no\n");
    p = sappend(out, p, maxo, "  vpci slots : ");
    p = sappend_int(out, p, maxo, VPCI::DeviceCount());
    p = sappend_char(out, p, maxo, '\n');
    p = sappend(out, p, maxo, "  resolution : ");
    p = sappend_int(out, p, maxo, VirtIOGPUHost::GetWidth());
    p = sappend(out, p, maxo, "x");
    p = sappend_int(out, p, maxo, VirtIOGPUHost::GetHeight());
    p = sappend_char(out, p, maxo, '\n');
    p = sappend(out, p, maxo, "  resources  : ");
    p = sappend_int(out, p, maxo, VirtIOGPUHost::ResourceCount());
    p = sappend_char(out, p, maxo, '\n');
    p = sappend(out, p, maxo, "  frames     : ");
    p = sappend_int(out, p, maxo, (int)VirtIOGPUHost::FrameCount());
    p = sappend_char(out, p, maxo, '\n');

    char buf[1024];
    PCIPassthrough::DumpStatus(buf, sizeof(buf));
    p = sappend(out, p, maxo, buf);
    return p;
}

int cmd_env(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    int p = 0;
    p = sappend(out, p, maxo, "Current environment: ");
    p = sappend(out, p, maxo, sh->EnvName(sh->GetEnv()));
    p = sappend_char(out, p, maxo, '\n');
    return p;
}

int cmd_switch(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    if (argc < 2) return sappend(out, 0, maxo, "Usage: switch linux|windows|kurono|debian\n");
    int p = 0;
    if (seq(argv[1], "linux")) {
        sh->SetEnv(ENV_LINUX);
        p = sappend(out, p, maxo, "Switched to Linux environment.\n");
    } else if (seq(argv[1], "windows")) {
        sh->SetEnv(ENV_WINDOWS);
        p = sappend(out, p, maxo, "Switched to Windows environment.\n");
    } else if (seq(argv[1], "kurono")) {
        sh->SetEnv(ENV_KURONO);
        p = sappend(out, p, maxo, "Switched to Kurono environment.\n");
    } else if (seq(argv[1], "debian")) {
        // Debian environment requires the rootfs to be installed via
        // `kpkg install debian` first.  If /debian/etc/os-release is
        // present we know the install completed.
        if (KVFS::Resolve("/debian/etc/os-release")) {
            sh->SetEnv(ENV_DEBIAN);
            p = sappend(out, p, maxo, "Switched to Debian environment.\n");
        } else {
            p = sappend(out, p, maxo, "Debian rootfs not installed. Run: kpkg install debian\n");
        }
    } else {
        p = sappend(out, p, maxo, "Unknown environment: ");
        p = sappend(out, p, maxo, argv[1]);
        p = sappend_char(out, p, maxo, '\n');
    }
    return p;
}

int cmd_clear(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    out[0] = '\x1b'; out[1] = '['; out[2] = 'C'; out[3] = 'L'; out[4] = 'R'; out[5] = 0;
    return 5;
}

int cmd_echo(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    int p = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) p = sappend_char(out, p, maxo, ' ');
        // expand vars
        char expanded[256];
        sh->ExpandVars(argv[i], expanded, 256);
        p = sappend(out, p, maxo, expanded);
    }
    p = sappend_char(out, p, maxo, '\n');
    return p;
}

int cmd_set(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    if (argc < 2) {
        int p = 0;
        for (int i = 0; i < sh->var_count; i++) {
            p = sappend(out, p, maxo, sh->vars[i].key);
            p = sappend_char(out, p, maxo, '=');
            p = sappend(out, p, maxo, sh->vars[i].value);
            p = sappend_char(out, p, maxo, '\n');
        }
        return p;
    }
    // set key = value or set key=value
    if (argc >= 4 && seq(argv[2], "=")) {
        sh->SetVar(argv[1], argv[3]);
    } else if (argc >= 2) {
        // check for = in argv[1]
        char key[32]; int ki = 0;
        char val[128]; int vi = 0;
        bool found_eq = false;
        for (int i = 0; argv[1][i]; i++) {
            if (!found_eq && argv[1][i] == '=') { found_eq = true; continue; }
            if (!found_eq) { if (ki < 31) key[ki++] = argv[1][i]; }
            else { if (vi < 127) val[vi++] = argv[1][i]; }
        }
        key[ki] = 0; val[vi] = 0;
        if (found_eq) sh->SetVar(key, val);
    }
    return 0;
}

int cmd_alias(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    if (argc < 2) {
        int p = 0;
        for (int i = 0; i < sh->alias_count; i++) {
            p = sappend(out, p, maxo, "alias ");
            p = sappend(out, p, maxo, sh->aliases[i].name);
            p = sappend(out, p, maxo, "='");
            p = sappend(out, p, maxo, sh->aliases[i].value);
            p = sappend(out, p, maxo, "'\n");
        }
        return p;
    }
    if (argc >= 3) sh->SetAlias(argv[1], argv[2]);
    return 0;
}

int cmd_history(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    int p = 0;
    int count = sh->GetHistoryCount();
    int start = sh->history_count > SHELL_MAX_HIST ? sh->history_count - SHELL_MAX_HIST : 0;
    for (int i = start; i < sh->history_count && i < start + 50; i++) {
        p = sappend(out, p, maxo, "  ");
        p = sappend_int(out, p, maxo, i + 1);
        p = sappend(out, p, maxo, "  ");
        p = sappend(out, p, maxo, sh->GetHistory(i));
        p = sappend_char(out, p, maxo, '\n');
    }
    (void)count;
    return p;
}

int cmd_exit(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    return sappend(out, 0, maxo, "Goodbye.\n");
}

int cmd_crash(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    // determine crash method from argument (default: div0)
    const char* method = (argc >= 2) ? argv[1] : "div0";

    int p = 0;

    // helper lambdas would be nice but we're freestanding, so just branch:
    if (seq(method, "div0")) {
        p = sappend(out, p, maxo, "Crash: CPU divide-by-zero (#DE, vector 0)\n");
        // inline asm  -  compiler cannot optimise this away
        __asm__ __volatile__(
            "xorl %%ecx, %%ecx\n\t"   // ecx = 0
            "movl $1, %%eax\n\t"       // eax = 1
            "xorl %%edx, %%edx\n\t"    // edx = 0  (edx:eax = dividend)
            "divl %%ecx\n\t"           // divide by zero → #de
            ::: "eax", "ecx", "edx", "memory"
        );
    } else if (seq(method, "null")) {
        p = sappend(out, p, maxo, "Crash: NULL pointer write (#PF, vector 14)\n");
        // write to address 0  -  guaranteed page fault
        __asm__ __volatile__(
            "xorl %%eax, %%eax\n\t"
            "movl $0xDEAD, (%%rax)\n\t"
            ::: "eax", "memory"
        );
    } else if (seq(method, "ud2")) {
        p = sappend(out, p, maxo, "Crash: Invalid opcode (#UD, vector 6)\n");
        // ud2  -  guaranteed invalid-opcode exception
        __asm__ __volatile__("ud2" ::: "memory");
    } else if (seq(method, "int3")) {
        p = sappend(out, p, maxo, "Crash: Breakpoint trap (#BP, vector 3)\n");
        __asm__ __volatile__("int $3" ::: "memory");
    } else if (seq(method, "overflow")) {
        p = sappend(out, p, maxo, "Crash: Stack overflow (recursive)\n");
        // infinite recursion  -  will blow the stack and page-fault
        cmd_crash(sh, argc, argv, out, maxo);
    } else if (seq(method, "gpf")) {
        p = sappend(out, p, maxo, "Crash: General protection fault (#GP, vector 13)\n");
        // load a bogus value into a segment register → #gp
        __asm__ __volatile__(
            "movw $0xFFFF, %%ax\n\t"
            "movw %%ax, %%ds\n\t"
            ::: "eax", "memory"
        );
    } else {
        p = sappend(out, p, maxo, "Usage: crash [div0|null|ud2|int3|gpf|overflow]\n");
        p = sappend(out, p, maxo, "  div0     - Divide-by-zero (#DE)\n");
        p = sappend(out, p, maxo, "  null     - NULL ptr write (#PF)\n");
        p = sappend(out, p, maxo, "  ud2      - Invalid opcode (#UD)\n");
        p = sappend(out, p, maxo, "  int3     - Breakpoint (#BP)\n");
        p = sappend(out, p, maxo, "  gpf      - General protection (#GP)\n");
        p = sappend(out, p, maxo, "  overflow - Stack overflow (#PF)\n");
    }

    return p; // unreachable for real crashes
}

int cmd_gpu(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;

    // helper: append hex32
    auto app_hex = [&](uint32_t v) {
        const char* hx = "0123456789ABCDEF";
        char b[9]; int i = 0;
        for (int s = 28; s >= 0; s -= 4) b[i++] = hx[(v >> s) & 0xF];
        b[i] = 0;
        p = sappend(out, p, maxo, b);
    };
    auto app_num = [&](int v) {
        char b[16]; int i = 0;
        if (v < 0) { b[i++] = '-'; v = -v; }
        if (v == 0) { b[i++] = '0'; }
        else {
            char r[12]; int ri = 0;
            while (v) { r[ri++] = '0' + (v % 10); v /= 10; }
            while (ri--) b[i++] = r[ri];
        }
        b[i] = 0;
        p = sappend(out, p, maxo, b);
    };

    p = sappend(out, p, maxo, "\033[36m╔══════════════════════════════════════════╗\033[0m\n");
    p = sappend(out, p, maxo, "\033[36m║        Kurono OS  -  GPU Information       ║\033[0m\n");
    p = sappend(out, p, maxo, "\033[36m╚══════════════════════════════════════════╝\033[0m\n\n");

    const GpuProbeResult& gpr = GpuProbe::GetResult();
    p = sappend(out, p, maxo, " \033[33mDetected GPUs:\033[0m ");
    app_num(gpr.count);
    p = sappend(out, p, maxo, "\n");

    p = sappend(out, p, maxo, " \033[33mTopology:\033[0m     ");
    switch (gpr.topology) {
        case GPU_TOPO_SINGLE:           p = sappend(out, p, maxo, "Single GPU"); break;
        case GPU_TOPO_OPTIMUS_MUXLESS:  p = sappend(out, p, maxo, "NVIDIA Optimus (Muxless)"); break;
        case GPU_TOPO_OPTIMUS_MUX:      p = sappend(out, p, maxo, "NVIDIA Optimus (MUX)"); break;
        case GPU_TOPO_POWERXPRESS:      p = sappend(out, p, maxo, "AMD PowerXpress"); break;
        case GPU_TOPO_DUAL_DISCRETE:    p = sappend(out, p, maxo, "Dual Discrete"); break;
        case GPU_TOPO_VIRTUAL:          p = sappend(out, p, maxo, "Virtual GPU"); break;
        default:                        p = sappend(out, p, maxo, "Unknown"); break;
    }
    p = sappend(out, p, maxo, "\n\n");

    // list all probed gpus
    for (int i = 0; i < gpr.count && i < GPU_PROBE_MAX; i++) {
        const GpuInfo& g = gpr.gpus[i];
        p = sappend(out, p, maxo, " \033[32m[");
        app_num(i);
        p = sappend(out, p, maxo, "]\033[0m ");
        p = sappend(out, p, maxo, g.desc);
        p = sappend(out, p, maxo, "\n     PCI ");
        app_num(g.bus); p = sappend(out, p, maxo, ":");
        app_num(g.device); p = sappend(out, p, maxo, ".");
        app_num(g.function);
        p = sappend(out, p, maxo, "  ID: 0x");
        app_hex(g.vendor_id);
        p = sappend(out, p, maxo, ":0x");
        app_hex(g.device_id);
        p = sappend(out, p, maxo, "  Role: ");
        switch (g.role) {
            case GPU_ROLE_PRIMARY:   p = sappend(out, p, maxo, "\033[32mPRIMARY\033[0m"); break;
            case GPU_ROLE_SECONDARY: p = sappend(out, p, maxo, "\033[33mSECONDARY\033[0m"); break;
            case GPU_ROLE_VIRTUAL:   p = sappend(out, p, maxo, "\033[36mVIRTUAL\033[0m"); break;
            default:                 p = sappend(out, p, maxo, "UNKNOWN"); break;
        }
        p = sappend(out, p, maxo, "\n");
    }
    p = sappend(out, p, maxo, "\n");

    if (NvidiaGPU::IsDetected()) {
        const NvidiaGPUInfo& nv = NvidiaGPU::GetInfo();
        p = sappend(out, p, maxo, " \033[32m[NVIDIA]\033[0m ");
        p = sappend(out, p, maxo, nv.name);
        p = sappend(out, p, maxo, "\n   Arch: ");
        p = sappend(out, p, maxo, NvidiaGPU::GetArchName());
        p = sappend(out, p, maxo, "  VRAM: ");
        app_num(nv.vram_mb);
        p = sappend(out, p, maxo, " MB  Mem: ");
        p = sappend(out, p, maxo, NvidiaGPU::GetMemTypeName());
        p = sappend(out, p, maxo, "\n   BAR0: 0x");
        app_hex((uint32_t)(nv.bar0 >> 32)); app_hex((uint32_t)(nv.bar0 & 0xFFFFFFFF));
        p = sappend(out, p, maxo, "  BAR1: 0x");
        app_hex((uint32_t)(nv.bar1 >> 32)); app_hex((uint32_t)(nv.bar1 & 0xFFFFFFFF));
        p = sappend(out, p, maxo, "\n   State: ");
        switch (NvidiaGPU::GetState()) {
            case GPU_STATE_INITIALIZED: p = sappend(out, p, maxo, "\033[32mReady\033[0m"); break;
            case GPU_STATE_DETECTED:    p = sappend(out, p, maxo, "\033[33mDetected\033[0m"); break;
            case GPU_STATE_PASSTHROUGH: p = sappend(out, p, maxo, "\033[36mPassthrough\033[0m"); break;
            default:                    p = sappend(out, p, maxo, "Unknown"); break;
        }
        p = sappend(out, p, maxo, "\n\n");
    }

    if (AmdGPU::IsAvailable()) {
        const AmdGPUInfo& amd = AmdGPU::GetInfo();
        p = sappend(out, p, maxo, " \033[31m[AMD]\033[0m ");
        p = sappend(out, p, maxo, amd.name);
        p = sappend(out, p, maxo, "\n   Arch: ");
        p = sappend(out, p, maxo, AmdGPU::GetArchName());
        p = sappend(out, p, maxo, "  DCE: ");
        p = sappend(out, p, maxo, AmdGPU::GetDisplayEngineName());
        p = sappend(out, p, maxo, "\n   CUs: ");
        app_num(amd.compute_units);
        p = sappend(out, p, maxo, "  SPs: ");
        app_num(amd.stream_processors);
        p = sappend(out, p, maxo, "  VRAM: ");
        app_num((int)(amd.vram_size / (1024*1024)));
        p = sappend(out, p, maxo, " MB");
        if (amd.resizable_bar) p = sappend(out, p, maxo, "  \033[32mReBAR\033[0m");
        if (amd.hardware_raytracing) p = sappend(out, p, maxo, "  \033[33mRT\033[0m");
        p = sappend(out, p, maxo, "\n   BAR0: 0x");
        app_hex((uint32_t)(amd.bar0 >> 32)); app_hex((uint32_t)(amd.bar0 & 0xFFFFFFFF));
        p = sappend(out, p, maxo, "\n\n");
    }

    if (IntelGPU::IsDetected()) {
        const IntelGPUInfo& ig = IntelGPU::GetInfo();
        p = sappend(out, p, maxo, " \033[34m[Intel]\033[0m ");
        p = sappend(out, p, maxo, ig.name);
        p = sappend(out, p, maxo, "\n   Gen: ");
        p = sappend(out, p, maxo, IntelGPU::GetGenName());
        p = sappend(out, p, maxo, "\n   BAR0: 0x");
        app_hex((uint32_t)(ig.bar0 >> 32)); app_hex((uint32_t)(ig.bar0 & 0xFFFFFFFF));
        p = sappend(out, p, maxo, " (MMIO)  BAR2: 0x");
        app_hex((uint32_t)(ig.bar2 >> 32)); app_hex((uint32_t)(ig.bar2 & 0xFFFFFFFF));
        p = sappend(out, p, maxo, " (GMADR)");
        if (ig.pipe_a.enabled) {
            p = sappend(out, p, maxo, "\n   Pipe A: ");
            app_num(ig.pipe_a.width); p = sappend(out, p, maxo, "x");
            app_num(ig.pipe_a.height);
            p = sappend(out, p, maxo, " @ 0x");
            app_hex((uint32_t)ig.pipe_a.surface_addr);
        }
        if (ig.pipe_b.enabled) {
            p = sappend(out, p, maxo, "\n   Pipe B: ");
            app_num(ig.pipe_b.width); p = sappend(out, p, maxo, "x");
            app_num(ig.pipe_b.height);
        }
        p = sappend(out, p, maxo, "\n\n");
    }

    p = sappend(out, p, maxo, " \033[33mDisplay Backend:\033[0m ");
    p = sappend(out, p, maxo, DisplayManager::GetBackendName());
    p = sappend(out, p, maxo, "\n");
    p = sappend(out, p, maxo, " \033[33mResolution:\033[0m     ");
    app_num(Graphics::GetWidth()); p = sappend(out, p, maxo, "x");
    app_num(Graphics::GetHeight()); p = sappend(out, p, maxo, "x");
    app_num(Graphics::GetBpp()); p = sappend(out, p, maxo, "bpp");
    p = sappend(out, p, maxo, "  Pitch: ");
    app_num(Graphics::GetPitch());
    p = sappend(out, p, maxo, "\n");
    p = sappend(out, p, maxo, " \033[33mFramebuffer:\033[0m    0x");
    uintptr_t fb_a = (uintptr_t)Graphics::GetBuffer();
    app_hex((uint32_t)(fb_a >> 32)); app_hex((uint32_t)(fb_a & 0xFFFFFFFF));
    p = sappend(out, p, maxo, "  WC: ");
    p = sappend(out, p, maxo, Graphics::IsFramebufferWC() ? "\033[32mYes\033[0m" : "\033[31mNo\033[0m");
    p = sappend(out, p, maxo, "\n");

    return p;
}

// serialize the in-memory kvfs tree to the persistent ext4 image so the desktop
// filesystem survives a reboot/shutdown. no-op if no ext4 target is mounted. (satoru)
static void persist_kvfs() {
    if (!PersistStore::Available()) return;
    // mirror the kvfs user-data subtrees (/home, /etc, /root) to a real on-disk
    // KFS filesystem on the nvme data disk  -  re-seeded /usr binaries + the big
    // sample media are skipped (size-capped) and re-filled by the boot seeding.
    // KFS::WriteFile uses contiguous runs + multi-page dma, so this completes in
    // a handful of commands. (satoru)
    PersistStore::SaveTree();
}

int cmd_reboot(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = sappend(out, 0, maxo, "Rebooting...\n");
    persist_kvfs();   // save desktop fs before reboot (satoru)
    HAL::Reboot();
    return p;
}

int cmd_shutdown(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = sappend(out, 0, maxo, "Shutting down...\n");
    persist_kvfs();   // save desktop fs before power-off (satoru)
    HAL::PowerOff();          // acpi/emulator soft power-off; does not return (satoru)
    return p;
}

// persisttest  -  prove the kvfs persistence loop end to end. first run writes a
// marker and flushes it to the ext4 data disk; after a reboot the boot-mount +
// restore brings it back, so a second run reports it survived. (satoru)
int cmd_persisttest(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    const char* path = "/home/user/.persisttest";
    if (KVFS::Exists(path)) {
        char val[64]; val[0] = 0;
        KVFS::ReadString(path, val, (int)sizeof(val));
        int p = sappend(out, 0, maxo, "PERSIST OK: marker survived the reboot -> \"");
        p = sappend(out, p, maxo, val);
        p = sappend(out, p, maxo, "\"\n");
        return p;
    }
    KVFS::WriteString(path, "kurono-persist-v1");
    int p = sappend(out, 0, maxo, "PERSIST CREATED: wrote marker to ");
    p = sappend(out, p, maxo, path);
    p = sappend(out, p, maxo, "\n");
    if (PersistStore::Available()) {
        persist_kvfs();
        p = sappend(out, p, maxo, "PERSIST SAVED: flushed kvfs to the raw nvme store; reboot + rerun to verify\n");
    } else {
        p = sappend(out, p, maxo, "PERSIST WARN: no nvme data disk present -> will NOT survive a reboot\n");
    }
    return p;
}

// capture the framebuffer to a bmp; explicit path or auto-timestamped. (satoru)
int cmd_screenshot(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    int p = 0;
    if (argc > 1) {
        if (Screenshot::CaptureToBMP(argv[1])) {
            p = sappend(out, p, maxo, "screenshot saved to ");
            p = sappend(out, p, maxo, argv[1]);
            p = sappend(out, p, maxo, "\n");
            NotificationManager::Post("Screenshot", argv[1],
                                      NotificationManager::ICON_SUCCESS, 3000);
        } else {
            p = sappend(out, p, maxo, "screenshot failed (could not encode/write ");
            p = sappend(out, p, maxo, argv[1]);
            p = sappend(out, p, maxo, ")\n");
        }
    } else {
        if (Screenshot::CaptureTimestamped()) {
            p = sappend(out, p, maxo, "screenshot saved to /home/user\n");
            NotificationManager::Post("Screenshot", "saved to /home/user",
                                      NotificationManager::ICON_SUCCESS, 3000);
        } else {
            p = sappend(out, p, maxo, "screenshot failed\n");
        }
    }
    return p;
}

// per-core current/base/turbo frequency + active governor. (satoru)
int cmd_cpufreq(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    int n = CPUFreq::CPUCount();
    if (n <= 0) {
        return sappend(out, p, maxo, "cpufreq: no cpus reported\n");
    }
    for (int i = 0; i < n; i++) {
        const CPUFreq::CPUInfo* ci = CPUFreq::GetCPU((uint32_t)i);
        if (!ci || !ci->present) continue;
        p = sappend(out, p, maxo, "cpu");
        p = sappend_int(out, p, maxo, i);
        p = sappend(out, p, maxo, ": cur ");
        p = sappend_int(out, p, maxo, (int)ci->cur_mhz);
        p = sappend(out, p, maxo, " MHz  base ");
        p = sappend_int(out, p, maxo, (int)ci->base_mhz);
        p = sappend(out, p, maxo, " MHz  turbo ");
        p = sappend_int(out, p, maxo, (int)ci->turbo_mhz);
        p = sappend(out, p, maxo, " MHz  gov ");
        const char* gov = "unknown";
        switch (ci->governor) {
            case CPUFreq::GOV_PERFORMANCE: gov = "performance"; break;
            case CPUFreq::GOV_POWERSAVE:   gov = "powersave";   break;
            case CPUFreq::GOV_ONDEMAND:    gov = "ondemand";    break;
            case CPUFreq::GOV_SCHEDUTIL:   gov = "schedutil";   break;
            case CPUFreq::GOV_USERSPACE:   gov = "userspace";   break;
        }
        p = sappend(out, p, maxo, gov);
        p = sappend(out, p, maxo, "\n");
    }
    return p;
}

// soft power-off after persisting the desktop fs. does not return. (satoru)
int cmd_poweroff(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = sappend(out, 0, maxo, "Powering off...\n");
    persist_kvfs();
    HAL::PowerOff();
    return p;
}

// acpi s3 suspend-to-ram; reports if the platform cannot enter s3. (satoru)
int cmd_suspend(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    if (!HAL::Suspend()) {
        p = sappend(out, p, maxo, "suspend unsupported (acpi s3 unavailable)\n");
    } else {
        p = sappend(out, p, maxo, "resumed from suspend\n");
    }
    return p;
}

int cmd_restart(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    // re-initialize shell state and re-register all commands
    sh->Init();
    LinuxCmds::RegisterAll(sh);
    WindowsCmds::RegisterAll(sh);
    int p = 0;
    p = sappend(out, p, maxo, "\033[32mShell restarted.\033[0m\n");
    return p;
}

int cmd_sysinfo(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    int p = 0;
    p = sappend(out, p, maxo, "╔══════════════════════════════════════════╗\n");
    p = sappend(out, p, maxo, "║          Kurono OS System Info          ║\n");
    p = sappend(out, p, maxo, "╠══════════════════════════════════════════╣\n");
    p = sappend(out, p, maxo, "║ OS:       Kurono OS 1.0.0 \"Aurora\"      ║\n");
    p = sappend(out, p, maxo, "║ Kernel:   kurono-kernel 1.0.0           ║\n");
    p = sappend(out, p, maxo, "║ Arch:     x86 (i386)                    ║\n");
    p = sappend(out, p, maxo, "║ User:     ");
    const char* u = sh->GetVar("USER");
    p = sappend(out, p, maxo, u ? u : "user");
    for (int i = slen(u ? u : "user"); i < 30; i++) p = sappend_char(out, p, maxo, ' ');
    p = sappend(out, p, maxo, "║\n");
    p = sappend(out, p, maxo, "║ Env:      ");
    p = sappend(out, p, maxo, sh->EnvName(sh->GetEnv()));
    for (int i = slen(sh->EnvName(sh->GetEnv())); i < 30; i++) p = sappend_char(out, p, maxo, ' ');
    p = sappend(out, p, maxo, "║\n");
    p = sappend(out, p, maxo, "╚══════════════════════════════════════════╝\n");
    return p;
}

int cmd_bash(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    sh->SetEnv(ENV_LINUX);
    sh->SetVar("SHELL", "/bin/bash");
    sh->SetVar("PS1", "\\u@\\h:\\w$ ");
    int p = 0;
    p = sappend(out, p, maxo, "\033[32mSwitched to Linux (bash) environment.\033[0m\n");
    p = sappend(out, p, maxo, "Type 'switch kurono' or 'exit' to return.\n");
    return p;
}

int cmd_cmd(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    sh->SetEnv(ENV_WINDOWS);
    sh->SetVar("SHELL", "C:\\Windows\\System32\\cmd.exe");
    sh->SetVar("PS1", "C:\\> ");
    int p = 0;
    p = sappend(out, p, maxo, "\033[36mSwitched to Windows (cmd) environment.\033[0m\n");
    p = sappend(out, p, maxo, "Type 'switch kurono' or 'exit' to return.\n");
    return p;
}

int cmd_whoami(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    const char* u = sh->GetVar("USER");
    int p = sappend(out, 0, maxo, u ? u : "user");
    return sappend_char(out, p, maxo, '\n');
}

int cmd_uname(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc >= 2 && seq(argv[1], "-a"))
        return sappend(out, 0, maxo, "KuronoOS kurono-machine 1.0.0 i686 Kurono/x86\n");
    return sappend(out, 0, maxo, "KuronoOS\n");
}

int cmd_hostname(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    const char* h = sh->GetVar("HOSTNAME");
    int p = sappend(out, 0, maxo, h ? h : "kurono");
    return sappend_char(out, p, maxo, '\n');
}

int cmd_date(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    DateTime dt = TimeManager::NowUTCDateTime();
    static const char* dow_names[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    static const char* mon_names[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
    int p = 0;
    if (dt.dow >= 1 && dt.dow <= 7)
        p = sappend(out, p, maxo, dow_names[dt.dow - 1]);
    p = sappend_char(out, p, maxo, ' ');
    if (dt.mon >= 1 && dt.mon <= 12)
        p = sappend(out, p, maxo, mon_names[dt.mon]);
    p = sappend_char(out, p, maxo, ' ');
    p = sappend_int(out, p, maxo, dt.dom);
    p = sappend_char(out, p, maxo, ' ');
    if (dt.h < 10) p = sappend_char(out, p, maxo, '0');
    p = sappend_int(out, p, maxo, dt.h);
    p = sappend_char(out, p, maxo, ':');
    if (dt.m < 10) p = sappend_char(out, p, maxo, '0');
    p = sappend_int(out, p, maxo, dt.m);
    p = sappend_char(out, p, maxo, ':');
    if (dt.s < 10) p = sappend_char(out, p, maxo, '0');
    p = sappend_int(out, p, maxo, dt.s);
    p = sappend(out, p, maxo, " UTC ");
    p = sappend_int(out, p, maxo, dt.year);
    p = sappend_char(out, p, maxo, '\n');
    return p;
}

int cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    uint32_t ticks = Time::GetTicks();
    uint32_t seconds = ticks / 1000;
    uint32_t days = seconds / 86400;
    uint32_t hours = (seconds % 86400) / 3600;
    uint32_t minutes = (seconds % 3600) / 60;
    int p = 0;
    p = sappend(out, p, maxo, "up ");
    p = sappend_int(out, p, maxo, days);
    p = sappend(out, p, maxo, " days, ");
    p = sappend_int(out, p, maxo, hours);
    p = sappend_char(out, p, maxo, ':');
    if (minutes < 10) p = sappend_char(out, p, maxo, '0');
    p = sappend_int(out, p, maxo, minutes);
    p = sappend_char(out, p, maxo, '\n');
    return p;
}

int cmd_usermode(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    if (!Userspace::IsReady()) {
        Userspace::Init();
    }

    Process* proc = Userspace::CreateDemoProcess();
    if (!proc) {
        return sappend(out, 0, maxo, "usermode: failed to create demo process\n");
    }

    int exit_code = Userspace::RunProcess(proc);
    int waited_exit = exit_code;
    if (Scheduler::WaitForProcess(proc, &waited_exit)) {
        exit_code = waited_exit;
        Scheduler::ReapProcess(proc);
    } else {
        Scheduler::DestroyProcess(proc);
    }

    int p = 0;
    p = sappend(out, p, maxo, "usermode: ring-3 demo exited with code ");
    p = sappend_int(out, p, maxo, exit_code);
    p = sappend_char(out, p, maxo, '\n');
    return p;
}

// run the embedded static ffmpeg as a real ring-3 linux process and capture
// its stdout/stderr back into the terminal. e.g. "ffmpeg -version" or
// "ffmpeg -i /tmp/in.wav /tmp/out.wav". paths like /tmp resolve into kvfs
// via LinuxSyscall::ResolvePath. single-threaded (the binary is built so;
// pass -threads 1 if a filter ever tries to spawn workers). (satoru)
int cmd_ffmpeg(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (!Userspace::IsReady()) Userspace::Init();
    if (!KVFS::Exists("/usr/bin/ffmpeg")) {
        return sappend(out, 0, mx, "ffmpeg: /usr/bin/ffmpeg not present in this build\n");
    }
    Process* proc = ElfLoader::LoadELF64FromVFS("/usr/bin/ffmpeg", "ffmpeg");
    if (!proc) {
        return sappend(out, 0, mx, "ffmpeg: failed to load binary (out of memory?)\n");
    }
    // build a null-terminated argv copy (the shell's argv is not guaranteed
    // null-terminated, and build_initial_stack scans for the null). (satoru)
    const char* av[40];
    int n = 0;
    for (int i = 0; i < argc && n < 39; i++) av[n++] = argv[i];
    av[n] = nullptr;
    const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user", "TMPDIR=/tmp", nullptr };

    LinuxSyscall::ClearConsoleOutput();
    int rc = Userspace::RunProcessWithArgs(proc, av, envp);
    int p = LinuxSyscall::ReadConsoleOutput(out, mx - 1);
    if (p < 0) p = 0;
    out[p] = 0;
    if (p == 0) {
        p = sappend(out, 0, mx, "ffmpeg: exited (code ");
        p = sappend_int(out, p, mx, rc);
        p = sappend(out, p, mx, ")\n");
    }
    // free the process (stack + address space). DestroyProcess has a
    // double-free guard so this is safe on an already-exited task. (satoru)
    Scheduler::DestroyProcess(proc);
    return p;
}

// run the embedded wl_shm wayland test client as a real ring-3 linux process.
// proves the compositor's shared-memory render path (memfd -> mmap -> SCM_RIGHTS
// -> wl_shm pool -> buffer -> commit). run it from a desktop terminal to see a
// blue toplevel window appear. (satoru)
int cmd_wltest(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    if (!Userspace::IsReady()) Userspace::Init();
    if (!KVFS::Exists("/usr/bin/wl_shm_test")) {
        return sappend(out, 0, mx, "wltest: /usr/bin/wl_shm_test not present in this build\n");
    }
    Process* proc = ElfLoader::LoadELF64FromVFS("/usr/bin/wl_shm_test", "wl_shm_test");
    if (!proc) {
        return sappend(out, 0, mx, "wltest: failed to load binary (out of memory?)\n");
    }
    const char* av[] = { "wl_shm_test", nullptr };
    const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user",
                           "XDG_RUNTIME_DIR=/system/run/user/1000",
                           "WAYLAND_DISPLAY=wayland-0", nullptr };
    LinuxSyscall::ClearConsoleOutput();
    int rc = Userspace::RunProcessWithArgs(proc, av, envp);
    int p = LinuxSyscall::ReadConsoleOutput(out, mx - 1);
    if (p < 0) p = 0;
    out[p] = 0;
    p = sappend(out, p, mx, "\nwltest: exited (code ");
    p = sappend_int(out, p, mx, rc);
    p = sappend(out, p, mx, ")\n");
    Scheduler::DestroyProcess(proc);
    return p;
}

// run the embedded pthreads smoke test  -  proves CLONE_THREAD + the futex-backed
// mutex (serialisation) + pthread_join. a correct run prints "counter=200000
// PASS". (satoru)
int cmd_pthtest(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    if (!Userspace::IsReady()) Userspace::Init();
    if (!KVFS::Exists("/usr/bin/pthread_test")) {
        return sappend(out, 0, mx, "pthtest: /usr/bin/pthread_test not present in this build\n");
    }
    Process* proc = ElfLoader::LoadELF64FromVFS("/usr/bin/pthread_test", "pthread_test");
    if (!proc) {
        return sappend(out, 0, mx, "pthtest: failed to load binary (out of memory?)\n");
    }
    const char* av[] = { "pthread_test", nullptr };
    const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user", nullptr };
    LinuxSyscall::ClearConsoleOutput();
    int rc = Userspace::RunProcessWithArgs(proc, av, envp);
    int p = LinuxSyscall::ReadConsoleOutput(out, mx - 1);
    if (p < 0) p = 0;
    out[p] = 0;
    p = sappend(out, p, mx, "\npthtest: exited (code ");
    p = sappend_int(out, p, mx, rc);
    p = sappend(out, p, mx, ")\n");
    Scheduler::DestroyProcess(proc);
    return p;
}

// launch firefox. if it isn't installed yet, run the kpkg streaming installer
// first (download+extract into /apps/firefox), then exec it through the
// FirefoxLauncher (which seeds the env + execve /apps/firefox/firefox via
// ld-kurono -> musl). this is the single entrypoint used by the terminal and
// by the gui autorun. (satoru)
int cmd_firefox(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (!Userspace::IsReady()) Userspace::Init();

    const char* url = (argc >= 2) ? argv[1] : nullptr;

    int p = 0;
    if (!FirefoxLauncher::IsInstalled()) {
        p = sappend(out, p, mx, "firefox: not installed yet  -  installing via kpkg...\n");
        const char* iav[] = { "install", "firefox", nullptr };
        // reuse the package manager's firefox branch (streaming install). (satoru)
        char ibuf[1024];
        int n = PackageManager::cmd_install(nullptr, 2, iav, ibuf, (int)sizeof(ibuf));
        if (n > 0) { ibuf[n < (int)sizeof(ibuf) ? n : (int)sizeof(ibuf) - 1] = 0; p = sappend(out, p, mx, ibuf); }
        if (!FirefoxLauncher::IsInstalled()) {
            p = sappend(out, p, mx, "firefox: install did not complete; aborting launch.\n");
            return p;
        }
    }

    p = sappend(out, p, mx, "firefox: launching /apps/firefox/firefox via ld-kurono...\n");

    // run firefox through ld-kurono ExecPIE directly (the proven dynamic-pie
    // path used by dyntest), so we control the env passed on the user stack.
    // the old FirefoxLauncher::Launch() used LSYS_EXECVE dispatched from kernel
    // context, which returns -ENOSYS (sys_execve requires a live user task).
    // we DON'T use ElfLoader::LoadELF64FromVFS because it hardcodes an envp
    // without the wayland/egl vars gecko needs. (satoru)
    int fsz = KVFS::GetFileSize("/apps/firefox/firefox");
    if (fsz <= 0) {
        p = sappend(out, p, mx, "firefox: /apps/firefox/firefox missing or empty.\n");
        return p;
    }
    uint8_t* image = (uint8_t*)KernelHeap::Alloc((uint32_t)fsz);
    if (!image) { p = sappend(out, p, mx, "firefox: OOM reading exe.\n"); return p; }
    if (KVFS::ReadFile("/apps/firefox/firefox", image, (uint32_t)fsz) != fsz) {
        KernelHeap::Free(image);
        p = sappend(out, p, mx, "firefox: read of exe failed.\n");
        return p;
    }

    Process* proc = Scheduler::CreateUserProcess("firefox", 0, 1);
    if (!proc) { KernelHeap::Free(image); p = sappend(out, p, mx, "firefox: CreateUserProcess failed.\n"); return p; }

    const char* av[] = { "/apps/firefox/firefox", url, nullptr };
    if (!url) av[1] = nullptr;
    const char* envp[] = {
        "HOME=/home/user", "USER=user",
        // /apps/firefox first: gecko's dependent libs (libxul.so etc.) live next
        // to the binary at the gre dir, system lib closure under lib/. (satoru)
        "LD_LIBRARY_PATH=/apps/firefox:/apps/firefox/lib:/system/lib:/system/lib/kurono:/apps/lib",
        "XDG_RUNTIME_DIR=/system/run/user/1000",
        "WAYLAND_DISPLAY=wayland-0",
        "MOZ_ENABLE_WAYLAND=1", "GDK_BACKEND=wayland",
        "LIBGL_ALWAYS_SOFTWARE=1", "DISPLAY=:0",
        "FONTCONFIG_PATH=/system/fonts",
        "MOZ_DISABLE_RDD_SANDBOX=1", "MOZ_DISABLE_CONTENT_SANDBOX=1",
        "MOZ_CRASHREPORTER_DISABLE=1",
        // kurono runs the browser as the system uid (0) while $HOME is owned by
        // uid 1000; gecko's nsAppRunner refuses that mismatch ("Running ... as
        // root in a regular user's session is not supported") and exits 1. this
        // is gecko's official override for exactly that case. (satoru)
        "MOZ_ALLOW_ROOT=1",
        // kurono's compositor only blits wl_shm (no dmabuf/egl); force gecko onto
        // the software/basic path so it commits shm buffers, never egl/webrender.
        // (the firefox.env file carries the same set but this inline envp is the
        // one actually handed to the user stack here.) (satoru)
        "MOZ_WEBRENDER=0", "WEBRENDER_SOFTWARE=1", "MOZ_ACCELERATED=0",
        "MOZ_X11_EGL=0", "MOZ_DISABLE_GPU_SANDBOX=1",
        "GALLIUM_DRIVER=llvmpipe",
        nullptr
    };

    uint64_t entry = 0, rsp = 0;
    bool ok = LdKurono::ExecPIE(proc, image, (uint64_t)fsz, "/apps/firefox/firefox",
                                av, envp, 1000, 1000, &entry, &rsp);
    KernelHeap::Free(image);
    if (!ok) {
        p = sappend(out, p, mx, "firefox: ld-kurono ExecPIE failed (see serial for which lib/reloc).\n");
        Scheduler::DestroyProcess(proc);
        return p;
    }
    proc->rip = entry; proc->rsp = rsp;
    proc->user_frame.rip = entry; proc->user_frame.rsp = rsp;
    proc->flags |= PROCESS_FLAG_STACK_READY;

    LinuxSyscall::ClearConsoleOutput();
    int rc = Userspace::RunProcessWithArgs(proc, av, envp);
    int q = LinuxSyscall::ReadConsoleOutput(out + p, mx - 1 - p);
    if (q > 0) p += q;
    out[p] = 0;
    p = sappend(out, p, mx, "\nfirefox: process returned (code ");
    p = sappend_int(out, p, mx, rc);
    p = sappend(out, p, mx, ")  -  see serial for ld-kurono module bases + any fault.\n");
    Scheduler::DestroyProcess(proc);
    return p;
}

// open the user-imported video in the native player window. plays the
// embedded ssstik.kvid (transcoded from the imported mp4). (satoru)
int cmd_playvideo(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    if (!EmbeddedMedia::HasSsstikKVID()) {
        return sappend(out, 0, mx, "playvideo: no imported video in this build\n");
    }
    DenjiApp::OpenBuffer(EmbeddedMedia::SsstikKVIDData(),
                         EmbeddedMedia::SsstikKVIDSize(), "ssstik.mp4");
    return sappend(out, 0, mx, "playvideo: opening imported video (ssstik.mp4)...\n");
}

int cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    const char* cwd = sh->GetVar("PWD");
    int p = sappend(out, 0, maxo, cwd ? cwd : "/");
    return sappend_char(out, p, maxo, '\n');
}

//  pwsh-setup  -  install powershell in alpine vm guest

int cmd_pwsh_setup(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;

    if (!Hypervisor::IsLinuxGuestEnabled() || Hypervisor::GetLinuxGuestProfile() != LINUX_GUEST_ALPINE) {
        return sappend(out, 0, maxo, "\033[31m[pwsh-setup]\033[0m PowerShell setup is only available while Alpine is the selected Linux guest.\n");
    }

    if (KuronoShell::pwsh_available) {
        p = sappend(out, p, maxo, "\033[36m[PowerShell]\033[0m Already installed.\n");
        char ver[256];
        int n = Hypervisor::AlpineExec("pwsh --version 2>/dev/null", ver, 255);
        if (n > 0) { ver[n] = 0; p = sappend(out, p, maxo, "  "); p = sappend(out, p, maxo, ver); }
        if (n > 0 && ver[n-1] != '\n') p = sappend_char(out, p, maxo, '\n');
        return p;
    }

    // step 1: check alpine vm
    if (!Hypervisor::IsAlpineBooted()) {
        p = sappend(out, p, maxo, "\033[33m[pwsh-setup]\033[0m Booting Alpine VM...\n");
        Hypervisor::BootAlpineWithExtraction(50000);
        if (!Hypervisor::IsAlpineBooted()) {
            p = sappend(out, p, maxo, "\033[31m[pwsh-setup]\033[0m Failed to boot Alpine VM.\n");
            return p;
        }
        p = sappend(out, p, maxo, "\033[32m[pwsh-setup]\033[0m Alpine VM booted.\n");
    } else {
        p = sappend(out, p, maxo, "\033[32m[pwsh-setup]\033[0m Alpine VM already running.\n");
    }

    // step 2: update repos and install powershell
    p = sappend(out, p, maxo, "\033[33m[pwsh-setup]\033[0m Installing PowerShell via apk...\n");
    p = sappend(out, p, maxo, "  Adding edge/community repository...\n");

    char result[2048];
    // add edge community repo
    Hypervisor::AlpineExec(
        "echo 'http://dl-cdn.alpinelinux.org/alpine/edge/community' >> /etc/apk/repositories",
        result, (int)sizeof(result) - 1);
    Hypervisor::AlpineExec(
        "echo 'http://dl-cdn.alpinelinux.org/alpine/edge/testing' >> /etc/apk/repositories",
        result, (int)sizeof(result) - 1);

    // update and install
    p = sappend(out, p, maxo, "  Running apk update...\n");
    int n = Hypervisor::AlpineExec("apk update 2>&1", result, (int)sizeof(result) - 1);
    if (n > 0) { result[n] = 0; p = sappend(out, p, maxo, "  "); p = sappend(out, p, maxo, result); }

    p = sappend(out, p, maxo, "  Installing powershell...\n");
    n = Hypervisor::AlpineExec(
        "apk add --no-cache powershell 2>&1 || apk add --no-cache pwsh 2>&1",
        result, (int)sizeof(result) - 1);
    if (n > 0) { result[n] = 0; p = sappend(out, p, maxo, "  "); p = sappend(out, p, maxo, result); }

    // step 3: verify
    p = sappend(out, p, maxo, "  Verifying installation...\n");
    n = Hypervisor::AlpineExec("pwsh --version 2>/dev/null", result, (int)sizeof(result) - 1);
    if (n > 0 && result[0] == 'P') {
        result[n] = 0;
        KuronoShell::pwsh_available = true;
        p = sappend(out, p, maxo, "\033[32m[pwsh-setup]\033[0m Success! ");
        p = sappend(out, p, maxo, result);
        if (result[n-1] != '\n') p = sappend_char(out, p, maxo, '\n');
        p = sappend(out, p, maxo, "  PowerShell is now available as an OS backend.\n");
        p = sappend(out, p, maxo, "  Commands found in pwsh will appear in conflict menus.\n");
        // invalidate alpine cache so new commands (pwsh) are picked up
        KuronoShell::alpine_cmd_cached = false;
    } else {
        p = sappend(out, p, maxo, "\033[33m[pwsh-setup]\033[0m Could not verify pwsh.\n");
        p = sappend(out, p, maxo, "  Alpine VM may need network access for package download.\n");
        p = sappend(out, p, maxo, "  You can try: alpine exec apk add powershell\n");
    }

    return p;
}

//  pwsh  -  run a powershell command directly

int cmd_pwsh(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = sappend(out, p, maxo, "\033[36m[PowerShell]\033[0m ");
        if (!KuronoShell::pwsh_available) {
            p = sappend(out, p, maxo, "Not installed. Run 'pwsh-setup' first.\n");
        } else {
            p = sappend(out, p, maxo, "Usage: pwsh <command>\n");
            p = sappend(out, p, maxo, "  Examples:\n");
            p = sappend(out, p, maxo, "    pwsh Get-Process\n");
            p = sappend(out, p, maxo, "    pwsh Get-ChildItem /\n");
            p = sappend(out, p, maxo, "    pwsh '$PSVersionTable'\n");
            p = sappend(out, p, maxo, "    pwsh Get-Command\n");
        }
        return p;
    }

    // reconstruct command from argv[1..]
    char cmd_buf[512];
    cmd_buf[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) scat(cmd_buf, " ", 512);
        scat(cmd_buf, argv[i], 512);
    }

    return KuronoShell::RunViaPwsh(cmd_buf, out, maxo);
}

//  alpine  -  run any command in alpine vm guest
int cmd_alpine(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = sappend(out, p, maxo, "\033[32m[Alpine]\033[0m Usage: alpine <command>\n");
        p = sappend(out, p, maxo, "  Run any command inside the Alpine Linux VM guest.\n");
        p = sappend(out, p, maxo, "  Examples:\n");
        p = sappend(out, p, maxo, "    alpine uname -a\n");
        p = sappend(out, p, maxo, "    alpine cat /etc/os-release\n");
        p = sappend(out, p, maxo, "    alpine ls /usr/bin\n");
        p = sappend(out, p, maxo, "    alpine python3 -c \"print('hello')\"\n");
        return p;
    }

    // reconstruct command from argv[1..]
    char cmd_buf[512];
    cmd_buf[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) scat(cmd_buf, " ", 512);
        scat(cmd_buf, argv[i], 512);
    }

    return KuronoShell::RunViaAlpine(cmd_buf, out, maxo);
}

//  ffmpeg  -  video encode/decode via alpine's ffmpeg
// (legacy alpine-vm ffmpeg stub removed  -  replaced by the real embedded
//  musl-static ffmpeg in cmd_ffmpeg above. (satoru))

//  ffprobe  -  media file info via alpine's ffprobe
int cmd_ffprobe(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = sappend(out, p, maxo, "\033[33m[ffprobe]\033[0m Media info via Alpine VM\n");
        p = sappend(out, p, maxo, "  Usage: ffprobe <file>\n");
        p = sappend(out, p, maxo, "  Shows codec, resolution, bitrate, duration etc.\n");
        return p;
    }

    char cmd_buf[512];
    scpy(cmd_buf, "ffprobe -hide_banner", 512);
    for (int i = 1; i < argc; i++) {
        scat(cmd_buf, " ", 512);
        scat(cmd_buf, argv[i], 512);
    }

    return KuronoShell::RunViaAlpine(cmd_buf, out, maxo);
}

//  apk  -  alpine package manager
int cmd_apk(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = sappend(out, p, maxo, "\033[32m[apk]\033[0m Alpine Package Manager\n");
        p = sappend(out, p, maxo, "  Usage: apk <subcommand> [args...]\n");
        p = sappend(out, p, maxo, "  Subcommands:\n");
        p = sappend(out, p, maxo, "    apk add <pkg>    Install package\n");
        p = sappend(out, p, maxo, "    apk del <pkg>    Remove package\n");
        p = sappend(out, p, maxo, "    apk update       Update package index\n");
        p = sappend(out, p, maxo, "    apk upgrade      Upgrade all packages\n");
        p = sappend(out, p, maxo, "    apk search <q>   Search packages\n");
        p = sappend(out, p, maxo, "    apk info         List installed packages\n");
        return p;
    }

    char cmd_buf[512];
    scpy(cmd_buf, "apk", 512);
    for (int i = 1; i < argc; i++) {
        scat(cmd_buf, " ", 512);
        scat(cmd_buf, argv[i], 512);
    }

    int p = KuronoShell::RunViaAlpine(cmd_buf, out, maxo);
    // invalidate alpine command cache after package install/remove
    if (argc >= 2 && (seq(argv[1], "add") || seq(argv[1], "del"))) {
        KuronoShell::alpine_cmd_cached = false;
    }
    return p;
}

//  codecs  -  list registered codecs and their status
int cmd_codecs(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = sappend(out, p, maxo, "╔═══════════════════════════════════════════════════╗\n");
    p = sappend(out, p, maxo, "║          Kurono OS Codec Registry                ║\n");
    p = sappend(out, p, maxo, "╠═══════════════════════════════════════════════════╣\n");

    int total = CodecRegistry::GetRegisteredCount();
    for (int i = 0; i < total; i++) {
        const CodecInfo* ci = CodecRegistry::GetCodecByIndex(i);
        if (!ci) continue;

        p = sappend(out, p, maxo, "║ ");
        // status indicator
        if (ci->available) {
            p = sappend(out, p, maxo, "\033[32m●\033[0m ");
        } else {
            p = sappend(out, p, maxo, "\033[31m○\033[0m ");
        }

        // name (padded to 16 chars)
        p = sappend(out, p, maxo, ci->name);
        int nl = 0; const char* s = ci->name; while (*s++) nl++;
        for (int j = nl; j < 16; j++) p = sappend_char(out, p, maxo, ' ');

        // capabilities
        if (ci->caps & CAP_DECODE_AUDIO)  p = sappend(out, p, maxo, "DA ");
        if (ci->caps & CAP_DECODE_VIDEO)  p = sappend(out, p, maxo, "DV ");
        if (ci->caps & CAP_ENCODE_AUDIO)  p = sappend(out, p, maxo, "EA ");
        if (ci->caps & CAP_ENCODE_VIDEO)  p = sappend(out, p, maxo, "EV ");
        if (ci->caps & CAP_CONTAINER)     p = sappend(out, p, maxo, "CT ");
        if (ci->caps & CAP_HARDWARE_ACCEL)p = sappend(out, p, maxo, "HW ");

        // extensions
        p = sappend(out, p, maxo, " [");
        p = sappend(out, p, maxo, ci->file_extensions);
        p = sappend(out, p, maxo, "]\n");
    }

    p = sappend(out, p, maxo, "╚═══════════════════════════════════════════════════╝\n");
    p = sappend(out, p, maxo, " DA=Decode Audio DV=Decode Video EA=Encode Audio\n");
    p = sappend(out, p, maxo, " EV=Encode Video CT=Container   HW=Hardware Accel\n");
    p = sappend(out, p, maxo, " \033[32m●\033[0m=Available  \033[31m○\033[0m=Not loaded\n");
    return p;
}

//
//  kurono reload    reload /etc/kurono/ui.conf and refresh all ui
//  kurono info      print os and config summary
//  kurono config    dump current config key=value pairs
//
int cmd_kurono(KuronoShell*, int argc, const char** argv, char* out, int maxo) {
    int p = 0;
    if (argc < 2) {
        p = sappend(out, p, maxo,
            "kurono  -  system control\n"
            "\n"
            "  kurono reload    Reload UI config and refresh desktop\n"
            "  kurono info      Show system information\n"
            "  kurono config    Print current UI config\n");
        return p;
    }

    const char* sub = argv[1];

    if (seq(sub, "reload")) {
        DesktopEnvironment::ReloadFromConfig();
        p = sappend(out, p, maxo, "UI configuration reloaded from ");
        p = sappend(out, p, maxo, UIConfig::Path());
        p = sappend(out, p, maxo, "\n");
        return p;
    }

    if (seq(sub, "info")) {
        p = sappend(out, p, maxo, "Kurono OS v1.0.0  (kurono-kernel 1.0)\n");
        p = sappend(out, p, maxo, "Architecture: x86_64\n");
        p = sappend(out, p, maxo, "Filesystem:   KVFS\n");
        p = sappend(out, p, maxo, "Shell:        KuronoShell + KCL\n");
        p = sappend(out, p, maxo, "Config file:  ");
        p = sappend(out, p, maxo, UIConfig::Path());
        p = sappend(out, p, maxo, "\n");
        p = sappend(out, p, maxo, "Config ver:   ");
        p = sappend_int(out, p, maxo, (int)UIConfig::Version());
        p = sappend(out, p, maxo, "\n");
        return p;
    }

    if (seq(sub, "config")) {
        char buf[4096];
        int n = KVFS::ReadString(UIConfig::Path(), buf, sizeof(buf));
        if (n <= 0) {
            p = sappend(out, p, maxo, "(no config file)\n");
        } else {
            p = sappend(out, p, maxo, buf);
        }
        return p;
    }

    p = sappend(out, p, maxo, "kurono: unknown subcommand '");
    p = sappend(out, p, maxo, sub);
    p = sappend(out, p, maxo, "'\n");
    return p;
}

// argv flag helpers for `supr`. (satoru)
static bool supr_has_flag(int argc, const char** argv, const char* flag) {
    for (int i = 1; i < argc; i++) if (seq(argv[i], flag)) return true;
    return false;
}
// match "--auth=VALUE" → returns VALUE or nullptr. (satoru)
static const char* supr_auth_value(int argc, const char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* pre = "--auth=";
        int k = 0; while (pre[k] && a[k] == pre[k]) k++;
        if (!pre[k]) return a + k;
    }
    return nullptr;
}

//  supr  -  kurono privilege / auth policy control.
//    supr policy                     show current policy + ksa status
//    supr policy --auth=passwd|kvault|both [--sovereign-override]
//    supr policy kvault disable --force --acknowledge-risk [--sovereign-override]
//    supr policy kvault enable
//    supr policy --auth=kvault --disable-passwd --acknowledge-risk
//    supr policy passwd enable
//    supr passwd                     change password (no ksa; normal flow)
//    supr selftest                   run the ksa isolation self-test (logs to serial)
//  every policy mutation is audited (even when ksa is disabled). (satoru)
int cmd_supr(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    int p = 0;
    int sid = SUPR::GetCurrentSession();

    if (argc < 2) {
        p = sappend(out, p, maxo, "usage: supr policy | supr passwd | supr selftest\n");
        return p;
    }

    const char* sub = argv[1];

    if (seq(sub, "selftest")) {
        bool ok = KSA::SelfTest();
        p = sappend(out, p, maxo, "ksa selftest: ");
        p = sappend(out, p, maxo, ok ? "PASS" : "FAIL");
        p = sappend(out, p, maxo, " (see serial log for details)\n");
        return p;
    }

    if (seq(sub, "passwd")) {
        // normal password-change flow  -  NO ksa involvement, per spec. (satoru)
        p = sappend(out, p, maxo, "supr passwd: interactive password change is GUI/login-driven.\n");
        p = sappend(out, p, maxo, "(no ksa involved; uses the standard password flow)\n");
        return p;
    }

    if (seq(sub, "policy")) {
        bool sov   = supr_has_flag(argc, argv, "--sovereign-override");
        bool force = supr_has_flag(argc, argv, "--force");
        bool ack   = supr_has_flag(argc, argv, "--acknowledge-risk");
        char err[160]; err[0] = 0;

        // kvault enable/disable subforms. (satoru)
        if (argc >= 3 && seq(argv[2], "kvault")) {
            if (argc >= 4 && seq(argv[3], "disable")) {
                bool ok = SUPR::DisableKvault(sid, force, ack, sov, err, sizeof(err));
                p = sappend(out, p, maxo, ok ? "ksa (kvault) disabled.\n" : "refused: ");
                if (!ok) { p = sappend(out, p, maxo, err); p = sappend_char(out, p, maxo, '\n'); }
                if (ok && SUPR::BothAuthDisabled())
                    p = sappend(out, p, maxo, "WARNING: both auth factors are now disabled.\n");
                return p;
            }
            if (argc >= 4 && seq(argv[3], "enable")) {
                bool ok = SUPR::EnableKvault(sid, err, sizeof(err));
                p = sappend(out, p, maxo, ok ? "ksa (kvault) enabled.\n" : "refused: ");
                if (!ok) { p = sappend(out, p, maxo, err); p = sappend_char(out, p, maxo, '\n'); }
                return p;
            }
        }
        if (argc >= 3 && seq(argv[2], "passwd")) {
            if (argc >= 4 && seq(argv[3], "enable")) {
                bool ok = SUPR::EnablePasswd(sid, err, sizeof(err));
                p = sappend(out, p, maxo, ok ? "password auth enabled.\n" : "refused: ");
                if (!ok) { p = sappend(out, p, maxo, err); p = sappend_char(out, p, maxo, '\n'); }
                return p;
            }
        }

        // --disable-passwd (requires ksa active). (satoru)
        if (supr_has_flag(argc, argv, "--disable-passwd")) {
            bool ok = SUPR::DisablePasswd(sid, ack, sov, err, sizeof(err));
            // also apply any --auth= mode given alongside (e.g. --auth=kvault).
            const char* mode = supr_auth_value(argc, argv);
            if (ok && mode && seq(mode, "kvault")) {
                char e2[160]; e2[0]=0; SUPR::EnableKvault(sid, e2, sizeof(e2));
            }
            p = sappend(out, p, maxo, ok ? "password auth disabled.\n" : "refused: ");
            if (!ok) { p = sappend(out, p, maxo, err); p = sappend_char(out, p, maxo, '\n'); }
            return p;
        }

        // --auth=mode. (satoru)
        const char* mode = supr_auth_value(argc, argv);
        if (mode) {
            SUPRAuthMode m;
            if (seq(mode, "passwd")) m = AUTH_PASSWD;
            else if (seq(mode, "kvault")) m = AUTH_KVAULT;
            else if (seq(mode, "both")) m = AUTH_BOTH;
            else { p = sappend(out, p, maxo, "supr: --auth must be passwd|kvault|both\n"); return p; }
            bool ok = SUPR::SetAuthMode(sid, m, sov, err, sizeof(err));
            p = sappend(out, p, maxo, ok ? "auth policy set.\n" : "refused: ");
            if (!ok) { p = sappend(out, p, maxo, err); p = sappend_char(out, p, maxo, '\n'); }
            if (ok && SUPR::BothAuthDisabled())
                p = sappend(out, p, maxo, "WARNING: both auth factors are now disabled.\n");
            return p;
        }

        // no mutation → show status. (satoru)
        p = sappend(out, p, maxo, "KSA / SUPR auth policy\n");
        p = sappend(out, p, maxo, "  password factor: ");
        p = sappend(out, p, maxo, SUPR::IsPasswdEnabled() ? "ENABLED\n" : "disabled\n");
        p = sappend(out, p, maxo, "  kvault (ksa)   : ");
        p = sappend(out, p, maxo, SUPR::IsKvaultEnabled() ? "ENABLED\n" : "disabled\n");
        p = sappend(out, p, maxo, "  effective mode : ");
        SUPRAuthMode em = SUPR::GetAuthMode();
        p = sappend(out, p, maxo, em == AUTH_BOTH ? "both\n" : (em == AUTH_KVAULT ? "kvault\n" : "passwd\n"));
        p = sappend(out, p, maxo, "  ksa available  : ");
        p = sappend(out, p, maxo, KSA::IsAvailable() ? "yes" : "no (hypervisor not present)");
        p = sappend_char(out, p, maxo, '\n');
        p = sappend(out, p, maxo, "  ksa prompt path: ");
        p = sappend(out, p, maxo, KSA::IsAvailable()
            ? (KSA::IsRealNestedVM() ? "nested VM\n" : "EPT-isolated context (nested-virt fallback)\n")
            : "n/a\n");
        if (SUPR::BothAuthDisabled())
            p = sappend(out, p, maxo, "  WARNING: both auth factors disabled  -  escalations show a risk warning.\n");
        return p;
    }

    // ── sudo-style reroute: `supr <cmd> [args]` runs <cmd> elevated. (satoru)
    // shell builtins can't meaningfully run elevated (they mutate shell state),
    // and whoami is informational; everything else goes through the auth gate.
    if (seq(sub, "cd") || seq(sub, "clear") || seq(sub, "history") ||
        seq(sub, "linux") || seq(sub, "cmd") || seq(sub, "exit")) {
        p = sappend(out, p, maxo, "supr: '");
        p = sappend(out, p, maxo, sub);
        p = sappend(out, p, maxo, "' is a shell builtin  -  run it directly (elevation doesn't apply).\n");
        return p;
    }
    if (seq(sub, "whoami")) {
        // supr whoami reports the elevated identity. (satoru)
        p = sappend(out, p, maxo, "root\n");
        return p;
    }
    // a guest may not escalate. (satoru)
    if (SUPR::GetLevel(sid) <= SUPR_GUEST) {
        p = sappend(out, p, maxo, "supr: permission denied  -  your role may not escalate.\n");
        return p;
    }
    // the target must exist before we bother prompting. (satoru)
    ShellCommand* target = KuronoShell::FindCommand(sub);
    if (!target || !target->handler) {
        p = sappend(out, p, maxo, "supr: command not found: ");
        p = sappend(out, p, maxo, sub);
        p = sappend_char(out, p, maxo, '\n');
        return p;
    }
    // authenticate (the interactive modal collects the credential/approval per the
    // active policy), run the command as root, then drop back. (satoru)
    char reason[80]; int rp = 0;
    rp = sappend(reason, rp, (int)sizeof(reason), "run '");
    rp = sappend(reason, rp, (int)sizeof(reason), sub);
    rp = sappend(reason, rp, (int)sizeof(reason), "' as root");
    int saved_user = -1;
    if (!SUPR::SudoBegin(sid, reason, &saved_user)) {
        p = sappend(out, p, maxo, "supr: authentication failed  -  not running '");
        p = sappend(out, p, maxo, sub);
        p = sappend(out, p, maxo, "'.\n");
        return p;
    }
    int wrote = target->handler(sh, argc - 1, argv + 1, out + p, maxo - p);
    SUPR::SudoEnd(sid, saved_user);
    if (wrote > 0) p += wrote;
    return p;
}

}  // namespace shellbuiltins
