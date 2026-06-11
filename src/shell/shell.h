#pragma once
#include "../kernel/types.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Kurono Shell — Bare-metal terminal command processor
// ═══════════════════════════════════════════════════════════════════════════

#define SHELL_MAX_CMD    512
#define SHELL_MAX_ARGS   32
#define SHELL_MAX_HIST   128
#define SHELL_MAX_ALIAS  64
#define SHELL_OUTPUT_BUF 8192

enum CmdEnvironment : uint8_t {
    ENV_KURONO  = 0,
    ENV_LINUX   = 1,
    ENV_WINDOWS = 2,
    ENV_AUTO    = 3,
};

struct ShellAlias {
    char name[32];
    char value[128];
};

struct ShellVar {
    char key[32];
    char value[128];
};

#define SHELL_MAX_VARS 128

// Forward
class KuronoShell;

// Command handler: takes (shell*, argc, argv, output_buf, max_output) → bytes written
typedef int (*ShellCmdHandler)(KuronoShell* sh, int argc, const char** argv,
                                char* output, int max_output);

struct ShellCommand {
    char name[32];
    char description[64];
    CmdEnvironment env;
    char category[16];
    ShellCmdHandler handler;
};

#define SHELL_MAX_COMMANDS 256

class KuronoShell {
public:
    static void Init();
    static void ProcessLine(const char* line, char* output, int max_output);
    static int Execute(const char* cmdline, char* output, int max_output);

    // Command registration
    static void RegisterCommand(const char* name, const char* desc,
                                CmdEnvironment env, const char* category,
                                ShellCmdHandler handler);
    // Convenience overload for subsystem modules (void* context handler)
    static void RegisterCommand(const char* name,
        int (*handler)(void*, int, const char**, char*, int),
        const char* desc) {
        RegisterCommand(name, desc, ENV_AUTO, "system",
            reinterpret_cast<ShellCmdHandler>(handler));
    }
    static ShellCommand* FindCommand(const char* name);
    static int  FindAllCommands(const char* name, ShellCommand** results, int max_results);
    static ShellCommand* GetCommands();
    static int GetCommandCount();

    // Conflict resolution
    static bool conflict_pending;
    static ShellCommand* conflict_choices[4];
    static int conflict_count;
    static char conflict_cmdline[SHELL_MAX_CMD];
    static int  ResolveConflict(int choice, char* output, int max_output);

    // Environment
    static CmdEnvironment GetEnv();
    static void SetEnv(CmdEnvironment e);
    static const char* EnvName(CmdEnvironment e);

    // Variables
    static void SetVar(const char* key, const char* value);
    static const char* GetVar(const char* key);
    static void ExpandVars(const char* input, char* output, int max_len);

    // Aliases
    static void SetAlias(const char* name, const char* value);
    static const char* GetAlias(const char* name);

    // History
    static void AddHistory(const char* line);
    static const char* GetHistory(int index);
    static int GetHistoryCount();

    // Piping: "cmd1 | cmd2"
    static int ExecutePiped(const char* cmdline, char* output, int max_output);
    // Cross-env: "linux:ls | windows:findstr"
    static int ExecuteCrossEnv(const char* cmdline, char* output, int max_output);

    // Prompt
    static void GetPrompt(char* buf, int max_len);

    // Data accessed by command handlers
    static CmdEnvironment current_env;
    static ShellVar vars[SHELL_MAX_VARS];
    static int var_count;

    // Data used by built-in command handlers
    static ShellCommand commands[SHELL_MAX_COMMANDS];
    static int command_count;
    static ShellAlias aliases[SHELL_MAX_ALIAS];
    static int alias_count;
    static char history[SHELL_MAX_HIST][SHELL_MAX_CMD];
    static int history_count;
    static int history_index;

private:
    static int ParseArgs(const char* line, const char** argv, int max_args);
    static void RegisterBuiltins();
};

// ── Built-in command handlers ────────────────────────────────────────────
namespace ShellBuiltins {
    int cmd_help(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_version(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_env(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_switch(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_clear(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_echo(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_set(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_alias(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_history(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_exit(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_reboot(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_shutdown(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_restart(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_sysinfo(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_bash(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_cmd(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_whoami(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_uname(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_hostname(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_date(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
}

// Alias for subsystem module compatibility
using Shell = KuronoShell;
