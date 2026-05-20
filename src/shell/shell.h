#pragma once
#include "../kernel/types.h"

//  kurono shell  -  bare-metal terminal command processor

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
    ENV_DEBIAN  = 4,
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

// forward
class KuronoShell;

// command handler: takes (shell*, argc, argv, output_buf, max_output) → bytes written
typedef int (*ShellCmdHandler)(KuronoShell* sh, int argc, const char** argv,
                                char* output, int max_output);

// Optional GUI / incremental sink: emits command output chunks while Execute runs.
typedef void (*ShellOutputChunkCallback)(void* udata, const char* data, int len);

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
    static void PumpUI();

    // Incremental output (GUI responsiveness): sinks copy bytes from handlers into the terminal.
    static void SetOutputChunkCallback(ShellOutputChunkCallback fn, void* udata);
    static void ClearOutputChunkCallback();
    static void EmitIncrementalRange(const char* buf, int from, int to_exclusive);
    /** Returns whether any incremental bytes were sunk this command (then clears the latch). */
    static bool TakeIncrementalOutputUsed();

    // Cooperative Ctrl+C cancellation during command execution.
    static void ClearCommandCancel();
    static void RequestCommandCancel();
    static bool IsCommandCancelRequested();

    // command registration
    static void RegisterCommand(const char* name, const char* desc,
                                CmdEnvironment env, const char* category,
                                ShellCmdHandler handler);
    // convenience overload for subsystem modules (void* context handler)
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

    // conflict resolution (extended: registered + alpine vm + powershell)
    enum ConflictBackend : uint8_t { CB_REGISTERED=0, CB_ALPINE=1, CB_PWSH=2 };
    static bool conflict_pending;
    static ShellCommand* conflict_choices[8];
    static uint8_t       conflict_backend[8]; // conflictbackend per slot
    static int conflict_count;
    static char conflict_cmdline[SHELL_MAX_CMD];
    static int  ResolveConflict(int choice, char* output, int max_output);

    // dynamic os probing  -  pings all backends at kernel level
    static bool pwsh_available;
    static char alpine_cmd_cache[4096];
    static bool alpine_cmd_cached;
    static void CacheAlpineCommands();
    static bool ProbeAlpine(const char* cmd_name);
    static bool ProbePwsh(const char* cmd_name);
    static int  RunViaAlpine(const char* cmdline, char* output, int max_output);
    static int  RunViaPwsh(const char* cmdline, char* output, int max_output);

    // environment
    static CmdEnvironment GetEnv();
    static void SetEnv(CmdEnvironment e);
    static const char* EnvName(CmdEnvironment e);

    // variables
    static void SetVar(const char* key, const char* value);
    static const char* GetVar(const char* key);
    static void ExpandVars(const char* input, char* output, int max_len);

    // aliases
    static void SetAlias(const char* name, const char* value);
    static const char* GetAlias(const char* name);

    // history
    static void AddHistory(const char* line);
    static const char* GetHistory(int index);
    static int GetHistoryCount();

    // piping: "cmd1 | cmd2"
    static int ExecutePiped(const char* cmdline, char* output, int max_output);
    // cross-env: "linux:ls | windows:findstr"
    static int ExecuteCrossEnv(const char* cmdline, char* output, int max_output);

    // prompt
    static void GetPrompt(char* buf, int max_len);

    // data accessed by command handlers
    static CmdEnvironment current_env;
    static ShellVar vars[SHELL_MAX_VARS];
    static int var_count;

    // data used by built-in command handlers
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

namespace ShellBuiltins {
    int cmd_help(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_denji(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_vgpu(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
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
    int cmd_pwsh_setup(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_pwsh(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_whoami(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_uname(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_hostname(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_date(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_usermode(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);

    // alpine vm bridge commands
    int cmd_alpine(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_ffmpeg(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_ffprobe(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_apk(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
    int cmd_codecs(KuronoShell* sh, int argc, const char** argv, char* out, int maxo);
}

// alias for subsystem module compatibility
using Shell = KuronoShell;
