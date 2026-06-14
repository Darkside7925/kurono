# KCL  -  Kurono Command Language

`src/kcl/kcl.cpp` and `kcl.h` implement a complete tree-walking scripting
language built into Kurono. See [KCL_REFERENCE.md](KCL_REFERENCE.md) for the
full grammar and stdlib.

## 1. What KCL is

KCL is a real interpreted language (lexer + recursive-descent parser +
evaluator) for automating Kurono shell operations, writing scripts, and
building lightweight tools inside the OS. It runs via the `kcl` shell command,
from `.kcl` files, or by double-clicking a `.kcl` file in the File Manager.

## 2. Language features

- Typed values: int, float, string, bool, list, none
- Variables: `set x = 10`, `let name = "Kurono"`
- Arithmetic, string concatenation/repeat, comparisons, boolean logic (`and`/`or`/`not`)
- Conditionals: `if` / `elif` / `else` / `end`
- Loops: `while`, `for x in a..b`, `for x in <list/string>`, with `break`/`continue`
- Functions with parameters, return values, and recursion: `func name(args) ... end`
- Lists: literals `[1, 2, 3]`, indexing, `append` / `remove` / `len`
- `import` to load another `.kcl` file; `#` comments and `#!/kcl` shebang
- Stdlib: `print` `input` `len` `str` `int` `float` `sqrt` `rand` `abs` `min` `max`
  `type` `read` `write` `exists` `exec` `sleep` `upper` `lower`

## 3. Running KCL

From the shell:
```
kcl /path/to/script.kcl
kcl -c "print('Hello, World!')"
```

A script can start with a shebang:
```
#!/kcl
print("Hello, World!")
```

Example session:
```
> kcl -c "set x = 10 + 5  print(x)"
15
```

## 4. Registration

The `kcl` command is registered by KCL itself, in `src/kcl/kcl.cpp`
(`RegisterCommand("kcl", "Run KCL script", ENV_KURONO, "scripting", cmd_kcl)`)  - 
not in `shell.cpp`'s `RegisterBuiltins()`. Its category is `"scripting"`.
Separately, `shell.cpp` recognizes a `.kcl` path on the command line and runs it
through `KCL::ExecFile()`.

## 5. Use cases

- Automating customization: write a KCL script to apply a color theme by writing to the config file and running `kurono reload`.
- Boot scripts: run a `.kcl` file at startup via a KVFS path referenced in the kernel init sequence.
- Batch file operations: copy, rename, and process files in KVFS.

## 6. Related files

- `src/kcl/kcl.cpp`, `src/kcl/kcl.h`  -  the interpreter (lexer/parser/evaluator)
- `src/kcl/kcl_test.cpp`  -  the 11-script self-test suite (boot token `kurono.kcltest`)
- `src/shell/shell.cpp`  -  KCL command registration + `.kcl` shell dispatch
- `src/apps/file_manager.cpp`  -  `.kcl` double-click → run in Terminal
- `src/fs/kvfs.cpp`  -  file operations called from KCL scripts (`read`/`write`/`exists`)
