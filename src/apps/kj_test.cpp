//  kurono os  -  kj self-test harness
//
//  scripts exercising every major kj feature + the kss host bindings. each test
//  runs the source through KJ::Execute and compares the captured output to an
//  expected string (exact, or substring where noted). results are logged to
//  serial so a headless qemu run can be polled for PASS/FAIL. (satoru)

#include "kj_test.h"
#include "kj.h"
#include "../ui/kss.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

namespace {

static bool tstreq(const char* a, const char* b){
    if (!a||!b) return false;
    while (*a&&*b&&*a==*b){ a++; b++; }
    return *a==0&&*b==0;
}
static bool tcontains(const char* hay, const char* needle){
    if (!hay||!needle) return false;
    for (int i=0; hay[i]; i++){ int j=0; while (needle[j]&&hay[i+j]==needle[j]) j++; if (!needle[j]) return true; }
    return false;
}

struct Test { const char* name; const char* src; const char* expect; bool substring; };

// ── hello / console.log ──────────────────────────────────────────────────────
static constexpr const char* SRC_HELLO =
    "console.log('Hello, KJ!');\n";

// ── arithmetic + precedence + number formatting ──────────────────────────────
static constexpr const char* SRC_ARITH =
    "console.log(2 + 3 * 4);\n"          // 14
    "console.log((2 + 3) * 4);\n"        // 20
    "console.log(17 % 5);\n"             // 2
    "console.log(10 / 4);\n"             // 2.5
    "console.log(7 - 9);\n";             // -2

// ── var/let/const + reassignment + strings ───────────────────────────────────
static constexpr const char* SRC_VARS =
    "let name = 'kurono';\n"
    "const v = 2;\n"
    "var msg = 'hi ' + name + ' v' + v;\n"
    "console.log(msg);\n"                // hi kurono v2
    "console.log(name.length);\n"        // 6
    "console.log(name.toUpperCase());\n";// KURONO

// ── if/else + comparisons + logical + ternary ────────────────────────────────
static constexpr const char* SRC_FLOW =
    "let x = 7;\n"
    "if (x > 10) { console.log('big'); }\n"
    "else if (x > 5 && x < 10) { console.log('medium'); }\n"
    "else { console.log('small'); }\n"
    "console.log(!false);\n"             // true
    "console.log(3 === 3 || 1 > 2);\n"   // true
    "console.log(x > 5 ? 'yes' : 'no');\n"; // yes

// ── while + break, for(;;) + continue ────────────────────────────────────────
static constexpr const char* SRC_LOOPS =
    "let i = 0; let total = 0;\n"
    "while (i < 100) { i = i + 1; if (i > 5) { break; } total = total + i; }\n"
    "console.log(total);\n"              // 1+2+3+4+5 = 15
    "let sum = 0;\n"
    "for (let k = 0; k < 5; k++) { sum += k; }\n"
    "console.log(sum);\n";               // 0+1+2+3+4 = 10

// ── functions + recursion + closures ─────────────────────────────────────────
static constexpr const char* SRC_FUNC =
    "function fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }\n"
    "console.log(fib(10));\n"            // 55
    "function counter() { let c = 0; return function() { c = c + 1; return c; }; }\n"
    "let next = counter();\n"
    "console.log(next());\n"             // 1
    "console.log(next());\n"             // 2
    "console.log(next());\n";            // 3

// ── arrays: literal, index, push, length, for-of ─────────────────────────────
static constexpr const char* SRC_ARRAYS =
    "let xs = [10, 20, 30];\n"
    "console.log(xs[0]);\n"              // 10
    "xs.push(40);\n"
    "console.log(xs.length);\n"          // 4
    "let acc = 0;\n"
    "for (let v of xs) { acc += v; }\n"
    "console.log(acc);\n";               // 100

// ── objects: literal, dot + bracket access, nested, methods ──────────────────
static constexpr const char* SRC_OBJECTS =
    "let p = { name: 'win', size: 5, nested: { z: 9 } };\n"
    "console.log(p.name);\n"             // win
    "console.log(p['size']);\n"          // 5
    "console.log(p.nested.z);\n"         // 9
    "p.size = 12;\n"
    "console.log(p.size);\n"             // 12
    "let o = { greet: function() { return 'hey'; } };\n"
    "console.log(o.greet());\n";         // hey

// ── Math + typeof ────────────────────────────────────────────────────────────
static constexpr const char* SRC_MATH =
    "console.log(Math.floor(3.7));\n"    // 3
    "console.log(Math.max(1, 9, 4));\n"  // 9
    "console.log(Math.abs(-5));\n"       // 5
    "console.log(Math.sqrt(16));\n"      // 4
    "console.log(typeof 5);\n"           // number
    "console.log(typeof 'a');\n"         // string
    "console.log(typeof [1]);\n";        // object

static constexpr Test TESTS[] = {
    { "hello",      SRC_HELLO,   "Hello, KJ!\n", false },
    { "arithmetic", SRC_ARITH,   "14\n20\n2\n2.5\n-2\n", false },
    { "vars",       SRC_VARS,    "hi kurono v2\n6\nKURONO\n", false },
    { "flow",       SRC_FLOW,    "medium\ntrue\ntrue\nyes\n", false },
    { "loops",      SRC_LOOPS,   "15\n10\n", false },
    { "functions",  SRC_FUNC,    "55\n1\n2\n3\n", false },
    { "arrays",     SRC_ARRAYS,  "10\n4\n100\n", false },
    { "objects",    SRC_OBJECTS, "win\n5\n9\n12\nhey\n", false },
    { "math_typeof",SRC_MATH,    "3\n9\n5\n4\nnumber\nstring\nobject\n", false },
};
static const int TEST_COUNT = (int)(sizeof(TESTS)/sizeof(TESTS[0]));

} // namespace

int KJTest::RunAll(){
    SerialLogger::Log("KJ-TEST: BEGIN\r\n");
    char* out=(char*)KernelHeap::Alloc(8192);
    if (!out){ SerialLogger::Log("KJ-TEST: alloc failed\r\n"); return 0; }

    int pass=0;
    for (int i=0;i<TEST_COUNT;i++){
        out[0]=0;
        KJ::Execute(TESTS[i].src, out, 8192);
        bool ok = TESTS[i].substring ? tcontains(out,TESTS[i].expect) : tstreq(out,TESTS[i].expect);
        SerialLogger::Log("KJ-TEST: ");
        SerialLogger::Log(TESTS[i].name);
        SerialLogger::Log(ok? " PASS\r\n" : " FAIL\r\n");
        if (!ok){
            SerialLogger::Log("KJ-TEST:   got=["); SerialLogger::Log(out); SerialLogger::Log("]\r\n");
            SerialLogger::Log("KJ-TEST:   exp=["); SerialLogger::Log(TESTS[i].expect); SerialLogger::Log("]\r\n");
        }
        if (ok) pass++;
    }

    // ── host-binding test: a script drives the kss stylesheet, then we read the
    // value back through the C++ api to prove the binding actually mutated the
    // live sheet (not just printed). (satoru)
    int total = TEST_COUNT + 2;
    {
        out[0]=0;
        const char* src =
            "kss.set('kjtest', 'background', 4286945791);\n"   // == 0xFF8599FF
            "kss.set('kjtest', 'radius', 18);\n"
            "console.log('styled ' + kss.get('kjtest','radius'));\n";
        KJ::Execute(src, out, 8192);
        int rule = KSS::Sheet::FindRule("kjtest");
        uint32_t bg = rule>=0? KSS::Sheet::GetColor(rule, KSS::Sheet::P_BG) : 0;
        float    rad= rule>=0? KSS::Sheet::GetScalar(rule, KSS::Sheet::P_RADIUS) : 0;
        bool ok = (rule>=0) && (bg==0xFF8599FFu) && (rad>17.5f && rad<18.5f) && tcontains(out,"styled 18");
        SerialLogger::Log("KJ-TEST: kss_binding");
        SerialLogger::Log(ok? " PASS\r\n" : " FAIL\r\n");
        if (!ok){
            SerialLogger::Log("KJ-TEST:   got=["); SerialLogger::Log(out); SerialLogger::Log("]\r\n");
            SerialLogger::Log("KJ-TEST:   bg="); SerialLogger::LogHex(bg);
            SerialLogger::Log(" rad="); SerialLogger::LogDec((int)(rad*100)); SerialLogger::Log("\r\n");
        }
        if (ok) pass++;
    }

    // ── keyframe binding test: a script defines + plays a keyframe track; we
    // confirm the sheet reports an active animation. (satoru)
    {
        out[0]=0;
        const char* src =
            "kss.keyframes('pulse', 'opacity', [0, 0.5, 1], [255, 128, 255]);\n"
            "let ok = kss.play('kjtest', 'pulse', 1000, true, 'linear');\n"
            "console.log('play ' + ok);\n";
        KJ::Execute(src, out, 8192);
        // advance the sheet clock so the track is mid-flight, then check Active(). (satoru)
        KSS::Anim::Tick(KSS::Anim::Now() + 100);
        bool active = KSS::Sheet::Active();
        bool ok = active && tcontains(out,"play true");
        SerialLogger::Log("KJ-TEST: kss_keyframes");
        SerialLogger::Log(ok? " PASS\r\n" : " FAIL\r\n");
        if (!ok){ SerialLogger::Log("KJ-TEST:   got=["); SerialLogger::Log(out); SerialLogger::Log("]\r\n"); }
        if (ok) pass++;
        // stop the loop so it doesn't pin the render gate after the test. (satoru)
        int r=KSS::Sheet::FindRule("kjtest"); if (r>=0) KSS::Sheet::StopKeyframes(r);
    }

    // ── shell-command wrapper test: drive KJ::cmd_kj exactly as the `kj -c`
    // shell command does, proving the user-facing entry point produces the same
    // result as KJ::Execute (the gui/cli autorun harness is finicky headless, so
    // this exercises the wrapper directly + deterministically). (satoru)
    total += 1;
    {
        out[0]=0;
        const char* argv[] = { "kj", "-c", "console.log(6*7)" };
        KJ::cmd_kj(nullptr, 3, argv, out, 8192);
        bool ok = tstreq(out, "42\n");
        SerialLogger::Log("KJ-TEST: shell_cmd_kj");
        SerialLogger::Log(ok? " PASS\r\n" : " FAIL\r\n");
        if (!ok){ SerialLogger::Log("KJ-TEST:   got=["); SerialLogger::Log(out); SerialLogger::Log("]\r\n"); }
        if (ok) pass++;
    }

    SerialLogger::Log("KJ-TEST: SUMMARY ");
    SerialLogger::LogDec(pass);
    SerialLogger::Log("/");
    SerialLogger::LogDec(total);
    SerialLogger::Log("\r\n");
    SerialLogger::Log(pass==total? "KJ-TEST: ALL PASS\r\n" : "KJ-TEST: SOME FAILED\r\n");

    KernelHeap::Free(out);
    return pass;
}
// end (satoru)
