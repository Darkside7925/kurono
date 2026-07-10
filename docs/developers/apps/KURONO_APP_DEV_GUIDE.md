# Kurono App Development Guide

## Overview

Kurono OS apps are built using **KCL** (Kurono Command Language), a lightweight scripting language embedded in the kernel. Apps are packaged as **.kro** files (Kurono Runtime Objects).

This guide covers:
1. KCL language reference
2. Creating a .kro app
3. Packaging and distribution
4. Available builtins and system calls

---

## 1. KCL Language Reference

KCL is a tree-walking interpreted language with typed values (int, float, string,
bool, list, none). See **[../kcl/KCL.md](../kcl/KCL.md)** and
**[../kcl/KCL_REFERENCE.md](../kcl/KCL_REFERENCE.md)** for the authoritative
grammar - the summary below matches the real `src/kcl/kcl.cpp` parser.

### Variables

Assignment **requires `=`** (`set x value` without `=` is a parse error):

```
set x = 10          # declare or update a variable
let name = "Kurono" # let is accepted too
```

### Control Flow

```
# If / elif / else  (blocks end with `end`)
if x > 5
  print("big")
elif x > 0
  print("small")
else
  print("non-positive")
end

# While loop
set i = 0
while i < 10
  print(i)
  set i = i + 1
end

# For loop over a numeric range a..b  (NOT `1 10`)
for i in 1..10
  print(i)
end

# For over a list or string
for item in [1, 2, 3]
  print(item)
end

# break / continue work inside loops
```

### Built-in Functions

The real stdlib (`call_builtin` in `kcl.cpp`):

`print` `input` `len` `str` `int` `float` `sqrt` `rand` `abs` `min` `max` `type`
`read` `write` `exists` `exec` `sleep` `upper` `lower` (plus the list ops
`append` / `remove`).

> Note: `pow`, `clamp`, `time`, and constants like `PI` / `VERSION` / `OS` /
> `TRUE` / `FALSE` are **not** part of KCL - earlier versions of this guide listed
> them but they do not exist in the interpreter.

### Shell Execution

```
exec("ls -la")                    # execute a shell command
exec("curl http://example.com")   # HTTP GET
exec("echo hello > /home/user/test.txt")
```

### Import

```
import lib.kcl      # include another KCL script (no quotes)
```

### Comments

```
# This is a comment
# Comments start with # and go to end of line
```

### Operators

Arithmetic: `+`, `-`, `*`, `/`, `%`, `-` (unary negate)
Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
Logical: `and`, `or`, `not`
String: strings are enclosed in `"double"` or `'single'` quotes

---

## 2. Creating a .kro App

### Directory Structure

```
my_app/
├── manifest.kcl     # App metadata (REQUIRED)
├── main.kcl         # Entry point (REQUIRED)
├── utils.kcl        # Optional modules
└── assets/          # Optional assets directory
    └── icon.bmp
```

### manifest.kcl

```
set app_name = "MyApp"
set app_version = "1.0.0"
set app_author = "Your Name"
set app_type = "app"
set app_entry = "main.kcl"
```

(The package manager reads `app_entry` from `manifest.kcl`; it defaults to
`main.kcl` - see `pkgmgr.cpp`.)

### main.kcl (entry point)

```
# Your app logic here
print("Hello from MyApp!")
set result = 0
for i in 1..100
  set result = result + i
end
print("Sum: " + str(result))
```

### Packaging

On Windows/WSL host:
```powershell
# Package with the kropack tool
.\tools\kropack.ps1 .\tools\sample_app .\sample_app.kro
```

On Kurono OS (once installed):
```
kpkg install /home/user/my_app.kro
kcl /apps/my_app/main.kcl
```

Direct run without a separate install step:
```
kcl /home/user/my_app.kro
```

This installs the archive into `/apps/<app_name>/` and then runs the manifest entrypoint.

---

## 3. .kro File Format

A `.kro` file is a simple binary archive:

| Offset | Size  | Field              |
|--------|-------|--------------------|
| 0      | 4     | Magic "KRO1"       |
| 4      | 4     | Entry count (u32)  |
| 8      | ...   | Entries (see below)|
| ...    | 4     | Footer "ENDK"      |

Entry format:
| Field        | Type   | Description          |
|-------------|--------|----------------------|
| name_len    | u32    | Length of filename   |
| name        | bytes  | UTF-8 filename       |
| data_len    | u32    | Length of file data  |
| data        | bytes  | File contents        |

---

## 4. App Lifecycle

1. **Development**: Write KCL scripts in a directory structure
2. **Package**: Run `kropack.ps1` to create the .kro file
3. **Deploy**: Copy .kro to the VM filesystem
4. **Install**: `kpkg install app.kro` extracts to `/apps/{app_name}/`
5. **Run**: `kcl /apps/{app_name}/main.kcl`, `kcl /home/user/app.kro`, or click a desktop shortcut

---

## 5. System Integration

### File I/O (via shell)
```
exec "echo content > /home/user/file.txt"     # Write file
exec "cat /home/user/file.txt"                # Read file
exec "rm /home/user/file.txt"                 # Delete file
exec "mkdir -p /home/user/newdir"             # Create directory
```

### Networking (via shell)
```
exec "curl http://example.com"                # HTTP GET
exec "wget http://example.com/file.txt"       # Download
exec "ping 10.0.2.2"                          # Ping
```

### Process Management (via shell)
```
exec "ps"                                      # List processes
exec "kill 123"                                # Kill process by PID
exec "taskmgr"                                 # Open task manager
```

---

## 6. Advanced: Functions

```
func greet(name)
  print("Hello, " + name + "!")
end

greet("World")    # call the function directly (no `exec`)
```

Functions are declared with `func name(args) ... end`, support parameters,
`return` values, and recursion, and are called directly by name (not via `exec`,
which runs *shell* commands).

---

## 7. Debugging

```
# Print variable values
print("x = " + str(x))
```

KCL reports parse/runtime errors with line numbers and never crashes the OS, so
the fastest debugging loop is `kcl -c "<snippet>"` in the terminal. (There is no
built-in `time()` / `DEBUG` facility - use serial logs or `print` checkpoints.)

---

## 8. Best Practices

1. **Mind the limits**: a script source is capped at 65536 bytes and 8192 tokens
   (`KCL_MAX_SCRIPT` / `KCL_MAX_TOKENS`); recursion depth is 20, params per func 8,
   imports 16
2. **Use imports**: Split large apps into multiple `.kcl` files (`import lib.kcl`)
3. **Use exec for heavy tasks**: Shell commands are more efficient for file I/O
4. **Test incrementally**: Run snippets with `kcl -c "..."` in the terminal first
5. **Handle errors**: Check return values from `exec` and file operations

---

## Quick Reference

```kcl
# ── Hello World App ──
set app_name = "HelloApp"
print("Hello from " + app_name)

# ── Counter App ──
set count = 0
while count < 5
  print(count)
  set count = count + 1
end

# ── Calculator App ──
set a = 10
set b = 20
set result = a + b * 2
print(result)

# ── File Writer App ──
exec("mkdir -p /home/user/MyApp")
exec("echo 'App data' > /home/user/MyApp/data.txt")

# ── Network App ──
exec("curl http://example.com")
```
