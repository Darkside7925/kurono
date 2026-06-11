#include "shell.h"
#include "linux_cmds.h"
#include "windows_cmds.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Kurono Shell Implementation
// ═══════════════════════════════════════════════════════════════════════════

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
ShellCommand* KuronoShell::conflict_choices[4] = {nullptr};
int KuronoShell::conflict_count = 0;
char KuronoShell::conflict_cmdline[SHELL_MAX_CMD] = {0};

// ── String helpers (bare-metal, no libc) ─────────────────────────────────

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

// ── Init ─────────────────────────────────────────────────────────────────

void KuronoShell::Init() {
    command_count = 0;
    alias_count = 0;
    history_count = 0;
    var_count = 0;
    current_env = ENV_KURONO;

    // Default variables
    SetVar("USER", "user");
    SetVar("HOME", "/home/user");
    SetVar("SHELL", "/bin/ksh");
    SetVar("PATH", "/bin:/usr/bin:/kurono/bin");
    SetVar("HOSTNAME", "kurono-machine");
    SetVar("TERM", "kurono-256color");
    SetVar("PWD", "/home/user");
    SetVar("PS1", "\\u@\\h:\\w$ ");

    RegisterBuiltins();
    SerialLogger::Log("Shell: Initialized\r\n");
}

// ── Command registration ─────────────────────────────────────────────────

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
    // Check aliases first
    const char* real = GetAlias(name);
    if (real) name = real;

    // Exact match in current env first
    for (int i = 0; i < command_count; i++) {
        if (seq(commands[i].name, name) && commands[i].env == current_env)
            return &commands[i];
    }
    // Then any env
    for (int i = 0; i < command_count; i++) {
        if (seq(commands[i].name, name))
            return &commands[i];
    }
    return nullptr;
}

ShellCommand* KuronoShell::GetCommands() { return commands; }
int KuronoShell::GetCommandCount() { return command_count; }

// Find ALL commands matching a name (for conflict detection)
int KuronoShell::FindAllCommands(const char* name, ShellCommand** results, int max_results) {
    const char* real = GetAlias(name);
    if (real) name = real;
    int count = 0;
    // Track which environments we've already found
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

// Resolve a pending conflict by choosing option 1-N
int KuronoShell::ResolveConflict(int choice, char* output, int max_output) {
    if (!conflict_pending || choice < 1 || choice > conflict_count) {
        return sappend(output, 0, max_output, "Invalid selection.\n");
    }
    conflict_pending = false;
    ShellCommand* cmd = conflict_choices[choice - 1];
    if (!cmd || !cmd->handler) return 0;

    // Parse the original command line for args
    const char* argv[SHELL_MAX_ARGS];
    int argc = ParseArgs(conflict_cmdline, argv, SHELL_MAX_ARGS);
    if (argc == 0) return 0;

    static KuronoShell shell_singleton;
    return cmd->handler(&shell_singleton, argc, argv, output, max_output);
}

// ── Environment ──────────────────────────────────────────────────────────

CmdEnvironment KuronoShell::GetEnv() { return current_env; }
void KuronoShell::SetEnv(CmdEnvironment e) { current_env = e; }
const char* KuronoShell::EnvName(CmdEnvironment e) {
    switch (e) {
        case ENV_KURONO: return "kurono";
        case ENV_LINUX: return "linux";
        case ENV_WINDOWS: return "windows";
        default: return "auto";
    }
}

// ── Variables ────────────────────────────────────────────────────────────

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

// ── Aliases ──────────────────────────────────────────────────────────────

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

// ── History ──────────────────────────────────────────────────────────────

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

// ── Arg parsing ──────────────────────────────────────────────────────────

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

// ── Execute ──────────────────────────────────────────────────────────────

void KuronoShell::ProcessLine(const char* line, char* output, int max_output) {
    Execute(line, output, max_output);
}

int KuronoShell::Execute(const char* cmdline, char* output, int max_output) {
    if (!cmdline || !cmdline[0]) return 0;
    output[0] = 0;

    // Add to history
    AddHistory(cmdline);

    // If there's a pending conflict, check if the user typed a number to resolve it
    if (conflict_pending) {
        int choice = 0;
        if (cmdline[0] >= '1' && cmdline[0] <= '9' && (cmdline[1] == 0 || cmdline[1] == ' ')) {
            choice = cmdline[0] - '0';
        }
        if (choice >= 1 && choice <= conflict_count) {
            return ResolveConflict(choice, output, max_output);
        }
        // Not a valid selection -- cancel conflict and process as normal command
        conflict_pending = false;
    }

    // Variable expansion
    char expanded[SHELL_MAX_CMD];
    ExpandVars(cmdline, expanded, SHELL_MAX_CMD);

    // Check for pipes
    bool has_pipe = false;
    for (int i = 0; expanded[i]; i++) {
        if (expanded[i] == '|') { has_pipe = true; break; }
    }
    if (has_pipe) return ExecutePiped(expanded, output, max_output);

    // Check for cross-env prefix: "linux:cmd" or "windows:cmd"
    const char* actual_cmd = expanded;
    CmdEnvironment saved_env = current_env;
    bool temp_env = false;

    if (sstart(expanded, "linux:")) {
        current_env = ENV_LINUX; actual_cmd = expanded + 6; temp_env = true;
    } else if (sstart(expanded, "windows:")) {
        current_env = ENV_WINDOWS; actual_cmd = expanded + 8; temp_env = true;
    } else if (sstart(expanded, "kurono:")) {
        current_env = ENV_KURONO; actual_cmd = expanded + 7; temp_env = true;
    }

    // Parse args
    const char* argv[SHELL_MAX_ARGS];
    int argc = ParseArgs(actual_cmd, argv, SHELL_MAX_ARGS);
    if (argc == 0) {
        if (temp_env) current_env = saved_env;
        return 0;
    }

    // --- Command Conflict Detection ---
    // If not using an explicit env prefix, check if the command exists in multiple envs
    if (!temp_env) {
        ShellCommand* matches[4];
        int match_count = FindAllCommands(argv[0], matches, 4);

        // Filter: if one match is in current_env and others are different, that's a conflict
        // ENV_AUTO matches don't conflict -- they work everywhere
        int real_count = 0;
        ShellCommand* real_matches[4];
        for (int i = 0; i < match_count; i++) {
            if (matches[i]->env != ENV_AUTO) {
                real_matches[real_count++] = matches[i];
            }
        }

        if (real_count > 1) {
            // Multiple environment-specific versions exist -- present selector
            conflict_pending = true;
            conflict_count = real_count;
            scpy(conflict_cmdline, actual_cmd, SHELL_MAX_CMD);
            for (int i = 0; i < real_count && i < 4; i++)
                conflict_choices[i] = real_matches[i];

            int p = 0;
            p = sappend(output, p, max_output, "\033[33m[Conflict] '");
            p = sappend(output, p, max_output, argv[0]);
            p = sappend(output, p, max_output, "' exists in multiple environments:\033[0m\n");
            for (int i = 0; i < real_count; i++) {
                p = sappend(output, p, max_output, "  ");
                p = sappend_char(output, p, max_output, '1' + i);
                p = sappend(output, p, max_output, ")  ");
                p = sappend(output, p, max_output, EnvName(real_matches[i]->env));
                p = sappend(output, p, max_output, ":");
                p = sappend(output, p, max_output, real_matches[i]->name);
                p = sappend(output, p, max_output, "  -- ");
                p = sappend(output, p, max_output, real_matches[i]->description);
                p = sappend_char(output, p, max_output, '\n');
            }
            p = sappend(output, p, max_output, "Enter selection (1-");
            p = sappend_char(output, p, max_output, '0' + real_count);
            p = sappend(output, p, max_output, "): ");
            if (temp_env) current_env = saved_env;
            return p;
        }
    }

    // Find and execute command -- silently uses current env match first
    ShellCommand* cmd = FindCommand(argv[0]);

    int result = 0;
    if (cmd && cmd->handler) {
        static KuronoShell shell_singleton;
        result = cmd->handler(&shell_singleton, argc, argv, output, max_output);
    } else {
        // Check if it's a KCL script
        int nlen = slen(argv[0]);
        if (nlen > 4 && argv[0][nlen-4] == '.' && argv[0][nlen-3] == 'k' &&
            argv[0][nlen-2] == 'c' && argv[0][nlen-1] == 'l') {
            result = sappend(output, 0, max_output, "kcl: would execute ");
            result = sappend(output, result, max_output, argv[0]);
            result = sappend_char(output, result, max_output, '\n');
        } else {
            result = sappend(output, 0, max_output, "ksh: command not found: ");
            result = sappend(output, result, max_output, argv[0]);
            result = sappend_char(output, result, max_output, '\n');
        }
    }

    if (temp_env) current_env = saved_env;
    return result;
}

// ── Pipe execution ───────────────────────────────────────────────────────

int KuronoShell::ExecutePiped(const char* cmdline, char* output, int max_output) {
    // Split by |
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

    // Execute in sequence, piping output → input
    char buf_a[SHELL_OUTPUT_BUF];
    char buf_b[SHELL_OUTPUT_BUF];
    buf_a[0] = 0;
    buf_b[0] = 0;

    for (int i = 0; i < seg_count; i++) {
        // Trim leading spaces
        const char* seg = segments[i];
        while (*seg == ' ') seg++;

        char* in_buf = (i % 2 == 0) ? buf_a : buf_b;
        char* out_buf = (i % 2 == 0) ? buf_b : buf_a;
        out_buf[0] = 0;

        // For piped commands, we'd need stdin passing (simplified: just execute)
        Execute(seg, out_buf, SHELL_OUTPUT_BUF);

        // If first command, we've set up output. Subsequent would need stdin.
    }

    // Copy final output
    char* final_buf = (seg_count % 2 == 0) ? buf_a : buf_b;
    scpy(output, final_buf, max_output);
    return slen(output);
}

int KuronoShell::ExecuteCrossEnv(const char* cmdline, char* output, int max_output) {
    return Execute(cmdline, output, max_output);
}

// ── Prompt ───────────────────────────────────────────────────────────────

void KuronoShell::GetPrompt(char* buf, int max_len) {
    int p = 0;
    const char* user = GetVar("USER");
    const char* host = GetVar("HOSTNAME");
    const char* cwd = KVFS::GetCwd();
    const char* ps1 = GetVar("PS1");

    if (!user) user = "user";
    if (!host) host = "kurono";
    if (!cwd) cwd = "/";

    // Windows environment uses its own PS1 style
    if (current_env == ENV_WINDOWS) {
        if (ps1 && ps1[0]) {
            p = sappend(buf, p, max_len, ps1);
        } else {
            p = sappend(buf, p, max_len, "C:\\> ");
        }
        return;
    }

    // Linux & Kurono environments: interpret PS1 escape sequences
    // Supported: \u (user), \h (host), \w (cwd with ~ abbrev), \$ ($ or #)
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
        // Default fallback: user@host:cwd$
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

// ── Built-in registration ────────────────────────────────────────────────

void KuronoShell::RegisterBuiltins() {
    using namespace ShellBuiltins;
    RegisterCommand("help",     "Show available commands",     ENV_KURONO, "builtin", cmd_help);
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
    RegisterCommand("sysinfo",  "System information",           ENV_KURONO, "builtin", cmd_sysinfo);

    // Shell switch commands
    RegisterCommand("bash",      "Switch to Linux shell",        ENV_KURONO, "builtin", cmd_bash);
    RegisterCommand("linux",     "Switch to Linux environment",  ENV_KURONO, "builtin", cmd_bash);
    RegisterCommand("cmd",       "Switch to Windows shell",      ENV_KURONO, "builtin", cmd_cmd);
    RegisterCommand("powershell","Switch to Windows environment",ENV_KURONO, "builtin", cmd_cmd);

    // Common UNIX commands
    RegisterCommand("whoami",    "Print current user",           ENV_AUTO,   "system",  cmd_whoami);
    RegisterCommand("uname",     "Print system information",     ENV_AUTO,   "system",  cmd_uname);
    RegisterCommand("hostname",  "Print hostname",               ENV_AUTO,   "system",  cmd_hostname);
    RegisterCommand("date",      "Print current date/time",      ENV_AUTO,   "system",  cmd_date);
    RegisterCommand("uptime",    "Print system uptime",          ENV_AUTO,   "system",  cmd_uptime);
    RegisterCommand("pwd",       "Print working directory",      ENV_AUTO, "filesystem",cmd_pwd);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Built-in commands
// ═══════════════════════════════════════════════════════════════════════════

namespace ShellBuiltins {

int cmd_help(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    int p = 0;
    p = sappend(out, p, maxo, "╔══════════════════════════════════════════╗\n");
    p = sappend(out, p, maxo, "║          Kurono OS Shell v1.0           ║\n");
    p = sappend(out, p, maxo, "╚══════════════════════════════════════════╝\n\n");

    // Group by category
    const char* cats[] = {"builtin", "filesystem", "text", "system", "network", "security", "package", nullptr};
    const char* cat_names[] = {"Built-in", "Filesystem", "Text", "System", "Network", "Security", "Package"};

    for (int ci = 0; cats[ci]; ci++) {
        bool has_any = false;
        for (int i = 0; i < sh->GetCommandCount(); i++) {
            ShellCommand* c = &sh->GetCommands()[i];
            if (seq(c->category, cats[ci])) {
                if (!has_any) {
                    p = sappend(out, p, maxo, " ");
                    p = sappend(out, p, maxo, cat_names[ci]);
                    p = sappend(out, p, maxo, ":\n");
                    has_any = true;
                }
                p = sappend(out, p, maxo, "   ");
                p = sappend(out, p, maxo, c->name);
                // Pad to 14 chars
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
    return sappend(out, 0, maxo, "Kurono OS 1.0.0 \"Aurora\"\nHybrid Kernel — Linux · Windows · Kurono\n");
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
    if (argc < 2) return sappend(out, 0, maxo, "Usage: switch linux|windows|kurono\n");
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
        // Expand vars
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
    // set KEY = VALUE or set KEY=VALUE
    if (argc >= 4 && seq(argv[2], "=")) {
        sh->SetVar(argv[1], argv[3]);
    } else if (argc >= 2) {
        // Check for = in argv[1]
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

int cmd_reboot(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = sappend(out, 0, maxo, "Rebooting...\n");
    HAL::Reboot();
    return p;
}

int cmd_shutdown(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    int p = sappend(out, 0, maxo, "System halted.\n");
    HAL::DisableInterrupts();
    HAL::Halt();
    return p;
}

int cmd_restart(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    // Re-initialize shell state and re-register all commands
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
    return sappend(out, 0, maxo, "2025-01-01 00:00:00 UTC\n");
}

int cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh; (void)argc; (void)argv;
    return sappend(out, 0, maxo, "up 0 days, 0:00\n");
}

int cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)argc; (void)argv;
    const char* cwd = sh->GetVar("PWD");
    int p = sappend(out, 0, maxo, cwd ? cwd : "/");
    return sappend_char(out, p, maxo, '\n');
}

}  // namespace ShellBuiltins
