#pragma once
//  kurono os - windows bridge commands
//  nt-style commands operating on kvfs for bare-metal kernel

#include "../shell/shell.h"

namespace WindowsCmds {
    void RegisterAll(KuronoShell* sh);

    int cmd_dir(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_copy(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_move(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_del(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_type(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_md(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_rd(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ren(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_cls(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_findstr(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_tasklist(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_taskkill(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_systeminfo(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ipconfig(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_ver(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_tree(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_attrib(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
    int cmd_chkdsk(KuronoShell* sh, int argc, const char** argv, char* out, int mx);
}
