#pragma once
//  kurono os - kj (kurono javascript)
//
//  a freestanding javascript-subset interpreter for scripting the desktop: a
//  hand-written lexer + recursive-descent parser to an ast, then a tree-walking
//  evaluator over a tagged value type (undefined/null/bool/number/string/array/
//  object/function-closure). no libc, no stl. modeled on the proven kcl/python
//  in-kernel interpreters. compiled with the generic sse-enabled rule so the
//  double-based number type works (see Makefile - kj.cpp is NOT in the no-sse
//  list, same as kcl.cpp).
//
//  language subset:
//    - var / let / const (let and const are block-ish; treated as function-scope
//      bindings with const reassignment guarded)
//    - functions: declarations, expressions, arrow-free `function`, closures that
//      capture their defining scope, return
//    - objects { k: v, ... } with dot + bracket member access and assignment
//    - arrays [ ... ] with index access/assignment and .length / .push
//    - numbers (double), strings ('..' or ".."), string concat with +
//    - arithmetic + - * / %, comparisons == != === !== < > <= >=, && || !,
//      unary -, prefix/postfix ++ --, ternary ?:
//    - if/else, while, for(init;cond;step), for-of, break/continue
//    - // and /* */ comments
//
//  host bindings (the point - scripts drive the kss styling/animation layer):
//    console.log(...)                     -> program output
//    kss.set(selector, prop, value)       -> KSS::Sheet::SetColor/SetScalar
//    kss.get(selector, prop)              -> read a resolved property
//    kss.transition(sel, prop, ms, ease)  -> KSS::Sheet::SetTransition
//    kss.keyframes(name, prop, [o..],[v..]) + kss.play(sel, name, ms, loop, ease)
//    ui.notify(title, body)               -> NotificationManager::Post
//    Math.{floor,ceil,round,abs,min,max,sqrt,sin,cos,random}, Math.PI
//  (satoru)

class KJ {
public:
    static void Init();

    // run kj source; program output (console.log etc.) is written to `out` (capped
    // at max_out, always nul-terminated). errors are captured into `out` too - kj
    // never crashes the os. returns bytes written. (satoru)
    static int Execute(const char* source, char* out, int max_out);
    // run a script stored in the kvfs. (satoru)
    static int ExecFile(const char* path, char* out, int max_out);

    // register the `kj` / `node` shell commands. shell_ptr is a KuronoShell*. (satoru)
    static void RegisterShellCommands(void* shell_ptr);
    // shell entry: `kj <file.js>` | `kj -c "<code>"`. (satoru)
    static int  cmd_kj(void* sh, int argc, const char** argv, char* out, int mx);
};
// end (satoru)
