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

KCL is a simple imperative scripting language. All variables are global and dynamically typed (integer or string).

### Variables

```
set name value      # declare or update a variable
let x 42            # alias for set
name = "Kurono"     # inline assignment
```

Built-in constants:
- `PI` = 3
- `VERSION` = "1.0.0"
- `OS` = "Kurono"
- `TRUE` = 1
- `FALSE` = 0

### Control Flow

```
# If/Else
if condition then
  print "yes"
else
  print "no"
end

# While loop
set i 0
while i < 10 do
  print i
  set i i + 1
end

# For loop (numeric range)
for i in 1 10 do
  print i
end

# Break / Continue
while true do
  if condition then
    break
  end
end
```

### Built-in Functions

| Function       | Description                    | Example              |
|---------------|--------------------------------|----------------------|
| `abs(n)`      | Absolute value                 | `abs(-5)` → 5        |
| `min(a,b)`    | Minimum of two numbers         | `min(3,7)` → 3       |
| `max(a,b)`    | Maximum of two numbers         | `max(3,7)` → 7       |
| `len(s)`      | Length of string               | `len("hello")` → 5   |
| `rand()`      | Random integer (0-32767)       | `rand()`              |
| `time()`      | Current time in milliseconds    | `time()`              |
| `sqrt(n)`     | Integer square root            | `sqrt(16)` → 4       |
| `pow(b,e)`    | Power                          | `pow(2,10)` → 1024   |
| `clamp(v,l,h)`| Clamp value to range           | `clamp(15,0,10)` → 10|

### Shell Execution

```
exec "ls -la"       # execute a shell command
exec "curl http://example.com"  # HTTP GET
exec "echo hello > /home/user/test.txt"  # file output
```

### Import

```
import "math.kcl"   # include another KCL script
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
set app_name "MyApp"
set app_version "1.0.0"
set app_author "Your Name"
set app_type "app"
set app_entry "main.kcl"
```

### main.kcl (entry point)

```
# Your app logic here
print "Hello from MyApp!"
set result 0
for i in 1 100 do
  set result result + i
end
print "Sum: "
print result
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
  print "Hello, "
  print name
  print "!"
end

exec greet("World")    # call the function
```

Functions are defined at the top level and can be called from anywhere in the script. Function arguments are numeric.

---

## 7. Debugging

```
# Enable verbose logging
set DEBUG 1

# Print variable values
print "x = "
print x

# Time execution
set start time()
# ... your code ...
set elapsed time() - start
print "Elapsed ms: "
print elapsed
```

---

## 8. Best Practices

1. **Keep scripts small**: KCL token limit is 256 tokens per execution
2. **Use imports**: Split large apps into multiple .kcl files
3. **Use exec for heavy tasks**: Shell commands are more efficient for file I/O
4. **Test incrementally**: Run scripts line-by-line in the terminal first
5. **Handle errors**: Check return values from exec and file operations

---

## Quick Reference

```kcl
# ── Hello World App ──
set app_name "HelloApp"
print "Hello from " + app_name
print "Running on " + OS + " v" + VERSION

# ── Counter App ──  
set count 0
while count < 5 do
  print count
  set count count + 1
end

# ── Calculator App ──
set a 10
set b 20
set result a + b * 2
print result

# ── File Writer App ──
exec "mkdir -p /home/user/MyApp"
exec "echo 'App data' > /home/user/MyApp/data.txt"

# ── Network App ──
exec "curl http://example.com"
```
