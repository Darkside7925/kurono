# KCL

`src/kcl/kcl.cpp` and `kcl.h`. A complete tree-walking scripting language built into
Kurono. Grammar and stdlib are in [KCL_REFERENCE.md](KCL_REFERENCE.md).

Real interpreter: lexer, recursive-descent parser, evaluator. For automating shell work,
writing scripts, and small tools inside the OS. Run it with the `kcl` command, from a
`.kcl` file, or by double-clicking a `.kcl` in the File Manager.

## What it has

- typed values: int, float, string, bool, list, none
- variables: `set x = 10`, `let name = "Kurono"`
- arithmetic, string concat and repeat, comparisons, boolean logic (`and`/`or`/`not`)
- `if` / `elif` / `else` / `end`
- loops: `while`, `for x in a..b`, `for x in <list/string>`, with `break` and `continue`
- functions with parameters, return values and recursion: `func name(args) ... end`
- lists: `[1, 2, 3]`, indexing, `append`, `remove`, `len`
- `import` to load another `.kcl`, `#` comments, `#!/kcl` shebang
- stdlib: `print` `input` `len` `str` `int` `float` `sqrt` `rand` `abs` `min` `max` `type`
  `read` `write` `exists` `exec` `sleep` `upper` `lower`

## Running it

```
kcl /path/to/script.kcl
kcl -c "print('Hello, World!')"
```

Shebang works:

```
#!/kcl
print("Hello, World!")
```

```
> kcl -c "set x = 10 + 5  print(x)"
15
```

## Registration

KCL registers its own command, in `src/kcl/kcl.cpp`:
`RegisterCommand("kcl", "Run KCL script", ENV_KURONO, "scripting", cmd_kcl)`. Not in
`shell.cpp`'s `RegisterBuiltins()`. Separately, `shell.cpp` notices a `.kcl` path on the
command line and runs it through `KCL::ExecFile()`.

## What people use it for

- customisation: a script that writes a colour theme into the config and runs
  `kurono reload`
- boot scripts: a `.kcl` file at a KVFS path referenced from the kernel init sequence
- batch file work in KVFS: copy, rename, process

## Files

- `src/kcl/kcl.cpp`, `src/kcl/kcl.h` the interpreter
- `src/kcl/kcl_test.cpp` the 11-script self-test, boot token `kurono.kcltest`
- `src/shell/shell.cpp` command registration and `.kcl` dispatch
- `src/apps/file_manager.cpp` double-click runs it in the terminal
- `src/fs/kvfs.cpp` file ops behind `read`, `write` and `exists`
