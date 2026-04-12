# KCL  -  Kurono Configuration Language

`src/kcl/kcl.cpp` and `kcl.h` implement a small scripting/configuration language built into Kurono.

## 1. What KCL is

KCL is a simple interpreted language for automating Kurono shell operations, writing configuration scripts, and building lightweight tools inside the OS. It runs in the `kcl` shell command or from `.kcl` files.

## 2. Language features

- Variables: `let name = value`
- Arithmetic and string expressions
- Conditionals: `if ... else`
- Loops: `for`, `while`
- Shell command invocation: `run("command")`
- File operations: `read(path)`, `write(path, content)`
- Print output: `print(value)`

## 3. Running KCL

From the shell:
```
kcl /path/to/script.kcl
```

Or interactively:
```
kcl
> let x = 10 + 5
> print(x)
15
```

## 4. Registration

KCL commands are registered with `RegisterBuiltins()` in the shell startup sequence. The `kcl` command category is `"kcl"`.

## 5. Use cases

- Automating customization: write a KCL script to apply a color theme by writing to the config file and running `kurono reload`.
- Boot scripts: run a `.kcl` file at startup via a KVFS path referenced in the kernel init sequence.
- Batch file operations: copy, rename, and process files in KVFS.

## 6. Related files

- `src/shell/shell.cpp`  -  KCL command registration
- `src/fs/kvfs.cpp`  -  file operations called from KCL scripts
- `src/system/ui_config.cpp`  -  frequently the target of KCL customization scripts
