#pragma once
//  kurono os  -  built-in Python 3.x mini interpreter
//  Subset: int/float/string/list/bool/None, arithmetic, comparisons,
//  logical ops, indexing, slicing (start:stop), function calls,
//  print/len/range/str/int/abs/min/max/input/type, assignment,
//  if/elif/else, while, for x in iterable, def/return, comments.

class PythonInterp {
public:
    static void Init();
    static int  RunSource(const char* source, char* out, int max_out);
    static int  RunFile(const char* vfs_path, char* out, int max_out);
    static void RegisterShellCommands(void* shell);

    static int cmd_python(void* sh, int argc, const char** argv, char* out, int mx);
};
