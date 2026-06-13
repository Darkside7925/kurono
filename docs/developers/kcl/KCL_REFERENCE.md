# KCL  -  Kurono Command Language Reference

KCL is the built-in scripting language of Kurono OS. It is a complete
tree-walking interpreter (lexer + recursive-descent parser + evaluator) that
runs inside the kernel  -  no libc, no external runtime. Implemented in
`src/kcl/kcl.cpp` / `kcl.h`.

## Value types

KCL is dynamically typed. Every value is one of:

| Type   | Examples              | Notes                                  |
|--------|-----------------------|----------------------------------------|
| int    | `42`, `-5`, `0`       | 64-bit signed                          |
| float  | `3.14`, `2.0`, `.5`   | real doubles, printed to 6 decimals    |
| string | `"hello"`, `'world'`  | escapes: `\n \t \r \\ \" \'`           |
| bool   | `true`, `false`       |                                        |
| list   | `[1, 2, 3]`, `[]`     | heterogeneous, growable                |
| none   | `none` / `null`       | the empty value                        |

## Language grammar

```
program    := statement*
statement  := set | assign | print | if | while | for | func
            | return | import | break | continue | expr
set        := ("set" | "let") IDENT "=" expr
assign     := IDENT "=" expr  |  IDENT "[" expr "]" "=" expr
print      := "print" ["("] expr ("," expr)* [")"]
if         := "if" expr ["then"] block ("elif" expr ["then"] block)*
              ["else" block] "end"
while      := "while" expr ["do"] block "end"
for        := "for" IDENT "in" (expr ".." expr | expr) ["do"] block "end"
func       := "func" IDENT "(" params? ")" block "end"
return     := "return" [expr]
import     := "import" (STRING | IDENT ["." IDENT])
block      := statement*

expr       := or
or         := and  ("or"  and)*
and        := cmp  ("and" cmp)*
cmp        := add  (("==" | "!=" | "<" | ">" | "<=" | ">=") add)*
add        := mul  (("+" | "-") mul)*
mul        := unary (("*" | "/" | "%") unary)*
unary      := ("-" | "not" | "+")* postfix
postfix    := atom ("[" expr "]")*
atom       := NUMBER | FLOAT | STRING | "true" | "false" | "none"
            | "[" (expr ("," expr)*)? "]"
            | IDENT | IDENT "(" args? ")" | "(" expr ")"
```

Statements are separated by newlines or `;`. `then`/`do` are optional sugar.
`#` starts a comment to end of line; a `#!/kcl` shebang on line 1 is ignored.

## Operators

- arithmetic: `+ - * / %` (int stays int; any float operand promotes to float)
- `+` also concatenates when either operand is a string, and joins two lists
- `*` repeats a string (`"ab" * 3` → `"ababab"`)
- comparison: `== != < > <= >=` (numbers and strings)
- boolean: `and`, `or`, `not`
- indexing: `x[i]` on lists/strings; negative `i` counts from the end

## Built-in functions (stdlib)

| Name     | Args       | Returns / effect                               |
|----------|------------|------------------------------------------------|
| print    | 0+         | print values (space-separated) + newline       |
| input    | 0-1        | echo prompt; returns "" (headless kernel)       |
| len      | 1          | length of a string or list                      |
| str      | 1          | convert any value to its string form            |
| int      | 1          | convert string/float/bool to int                |
| float    | 1          | convert string/int/bool to float                |
| abs      | 1          | absolute value (preserves int/float)            |
| sqrt     | 1          | square root (Newton's method, returns float)    |
| rand     | 0-2        | `rand()`→[0,32767], `rand(n)`→[0,n), `rand(a,b)`→[a,b] |
| min/max  | 1+         | minimum / maximum of the arguments              |
| type     | 1          | type name as a string                           |
| append   | 2          | `append(list, value)` → grown list              |
| remove   | 2          | `remove(list, index)` → list without that index |
| upper    | 1          | uppercase a string                              |
| lower    | 1          | lowercase a string                              |
| read     | 1          | read a file from KVFS → string                  |
| write    | 2          | `write(path, content)` → bool (success)         |
| exists   | 1          | does a KVFS path exist → bool                   |
| exec     | 1          | run a shell command → its output string         |
| sleep    | 1          | sleep N milliseconds                            |

Lists are value types: `append`/`remove` return a new list, so the idiom is
`set xs = append(xs, v)`.

## Statements (examples)

### set / assign
```
set x = 42
let name = "Kurono"
count = count + 1
items[0] = "first"
```

### print
```
print("Hello, World!")
print("x =", x, "y =", y)
print("sum = " + str(a + b))
```

### if / elif / else
```
if x > 10 then
  print("big")
elif x > 5 then
  print("medium")
else
  print("small")
end
```

### while
```
set i = 0
while i < 5 do
  print(i)
  set i = i + 1
end
```

### for (range, list, or string)
```
for i in 1..10 do
  print(i)
end

for item in ["a", "b", "c"] do
  print(item)
end
```

### func (with recursion)
```
func fib(n)
  if n < 2 then
    return n
  end
  return fib(n - 1) + fib(n - 2)
end
print(fib(10))     # 55
```

### import
```
import mathlib          # resolves ./mathlib.kcl then /kurono/lib/mathlib.kcl
import "/home/user/util.kcl"
```
An imported file's top-level statements run into the current global scope, so
its functions and variables become visible to the importing script.

### break / continue
```
while true do
  if done then break end
  if skip then continue end
end
```

## Running KCL

```
kcl script.kcl          # run a script file
kcl -c "print('hi')"    # run inline code  (-e is an alias)
```
- A file with a `#!/kcl` first line is treated as a KCL script.
- Double-clicking a `.kcl` file in the File Manager opens the Terminal and runs it.

## Execution model

1. The whole source is tokenized into a heap-allocated token stream (with line numbers).
2. A recursive-descent parser/evaluator walks the tokens directly.
3. Variables and functions live in lexically-scoped environments; functions are
   registered in the global scope. Each call scope is heap-allocated (the kernel
   stack is only 64 KB), so recursion stays cheap.
4. `exec` runs shell commands via the integrated `KuronoShell`.
5. `import` reads and runs another `.kcl` file via KVFS; its token stream is
   retained for the run so imported function bodies remain valid.
6. Passing a `.kro` file to `kcl` installs it and runs its manifest entrypoint.

## Errors

Errors are reported with line numbers (e.g. `kcl: line 3: division by zero`)
and captured into the command output. A script error  -  including infinite
recursion (bounded by a call-depth guard) and runaway loops (bounded by an
iteration guard)  -  is contained and reported; it never crashes the OS.

## Limits

- Up to 8192 tokens per source file; scripts up to 64 KB
- 96 variable/function binding slots per scope
- 8 parameters per function
- Call-depth guard (recursion) and a per-loop iteration guard

## Test suite

A self-test of 11 scripts (hello world, arithmetic, strings, booleans/if,
while, for, recursive fibonacci, fizzbuzz, lists, file I/O, and
import+ExecFile) is gated behind the `kurono.kcltest` boot token. It runs
headless at boot and logs `KCL-TEST: <name> PASS|FAIL` plus a
`KCL-TEST: SUMMARY <pass>/<total>` line to the serial console. Source:
`src/kcl/kcl_test.cpp`.
