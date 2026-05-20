# KCL  -  Kurono Command Language Reference

KCL is the built-in scripting language of Kurono OS. It is interpreted at runtime by the kernel.

## Language Grammar

```
program    := statement*
statement  := set | print | if | while | for | func | exec | import | break | continue | return | expr
set        := "set" | "let" IDENT ["="] (expr | STRING)
print      := "print" expr*
if         := "if" expr "then" statement* ("else" statement*)? "end"
while      := "while" expr "do" statement* "end"
for        := "for" IDENT "in" expr [".." expr] "do" statement* "end"
func       := "func" IDENT "(" params? ")" statement* "end"
exec       := "exec" (STRING | IDENT)+
import     := "import" STRING
expr       := logical ("and" | "or" logical)*
logical    := compare (("==" | "!=" | "<" | ">" | "<=" | ">=") compare)*
compare    := term (("+" | "-") term)*
term       := factor (("*" | "/" | "%") factor)*
factor     := NUMBER | "-" factor | "not" factor | IDENT ["(" args? ")"] | "(" expr ")"
```

## Token Types

| Token        | Examples           |
|-------------|--------------------|
| NUMBER      | 42, -5, 0         |
| STRING      | "hello", 'world'  |
| IDENT       | x, my_var         |
| KEYWORD     | set, print, if, while, for, func, exec, import, break, continue, return, end, do, then, else, in |
| OPERATOR    | + - * / % == != < > <= >= = |
| PAREN       | ( ) { } [ ]        |
| PUNCTUATION | , ; .              |

## Built-in Functions

All built-in functions return integer values.

| Name    | Args  | Description                              |
|---------|-------|------------------------------------------|
| abs     | 1     | Absolute value                           |
| min     | 2+    | Minimum value                            |
| max     | 2+    | Maximum value                            |
| len     | 1     | String length (in numeric context)       |
| rand    | 0     | Random number 0..32767                   |
| time    | 0     | Current monotonic milliseconds           |
| sqrt    | 1     | Integer square root (Newton's method)    |
| pow     | 2     | base^exponent (up to 30 iterations)      |
| clamp   | 3     | clamp(value, lo, hi)                     |
| sign    | 1     | sign(x): -1, 0, or 1                     |

## Statements

### set / let
```
set x 42
let name "Kurono"
count = 0
```

### print
```
print "Hello"
print "Value: " + x
```

### if / else
```
if x > 10 then
  print "big"
else
  print "small" 
end
```

### while
```
set i 0
while i < 5 do
  print i
  set i i + 1
end
```

### for
```
for i in 1 10 do
  print i
end
```

### func
```
func add(a, b)
  set result a + b
  return result
end
```

### exec
```
exec "ls -la"
exec "curl http://example.com"
```

### import
```
import "/home/user/math.kcl"
```

### break / continue
```
while true do
  if done then break end
  if skip then continue end
end
```

## Execution Model

1. KCL scripts are parsed into tokens (max 256 tokens)
2. Tokens are executed sequentially
3. Variables are stored in a global table (max 128)
4. Functions are stored in a global table (max 32)
5. `exec` runs shell commands via the integrated shell
6. `import` reads and executes another .kcl file via KVFS
7. Passing a `.kro` file to `kcl` installs it into `/apps/<app_name>/` and runs its manifest entrypoint

## Limitations

- Max 128 variables
- Max 32 functions
- Max 256 tokens per script
- Max 32 nested loops
- Functions cannot be nested
- String operations are limited (no concatenation operator in expressions  -  use `print` for output)
- No floating-point arithmetic (integers only)
- No arrays or lists
