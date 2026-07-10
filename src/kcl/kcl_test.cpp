//  kurono os - kcl self-test harness
//
//  ten scripts exercising every major language feature + the stdlib. each test
//  runs the source through KCL::Execute and compares the captured output to an
//  expected string (exact, or substring for the file-i/o test). every result is
//  logged to serial so a headless qemu run can be polled for PASS/FAIL. (satoru)

#include "kcl_test.h"
#include "kcl.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

namespace {

static bool tstreq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static bool tcontains(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0; while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

struct Test {
    const char* name;
    const char* src;
    const char* expect;
    bool substring;   // expect is a substring (file-i/o), not an exact match (satoru)
};

// ── the ten tests ────────────────────────────────────────────────────────
static constexpr const char* SRC_HELLO =
    "# the classic\n"
    "print(\"Hello, World!\")\n";

static constexpr const char* SRC_ARITH =
    "# arithmetic + operator precedence, int vs float, modulo\n"
    "set a = 2 + 3 * 4\n"          // 14
    "print(a)\n"
    "set b = (2 + 3) * 4\n"        // 20
    "print(b)\n"
    "print(17 % 5)\n"             // 2
    "print(10 / 4)\n"            // 2 (int division)
    "print(10.0 / 4)\n";        // 2.500000 (float)

static constexpr const char* SRC_STRINGS =
    "# string concat, len, str(), upper\n"
    "set name = \"kurono\"\n"
    "print(\"hi \" + name)\n"     // hi kurono
    "print(len(name))\n"          // 6
    "print(\"v\" + str(2))\n"     // v2
    "print(upper(name))\n";       // KURONO

static constexpr const char* SRC_BOOL =
    "# comparisons, boolean logic, if/elif/else\n"
    "set x = 7\n"
    "if x > 10 then\n"
    "  print(\"big\")\n"
    "elif x > 5 and x < 10 then\n"
    "  print(\"medium\")\n"
    "else\n"
    "  print(\"small\")\n"
    "end\n"
    "print(not false)\n"          // true
    "print(3 == 3 or 1 > 2)\n";   // true

static constexpr const char* SRC_WHILE =
    "# while loop with break\n"
    "set i = 0\n"
    "set total = 0\n"
    "while i < 100 do\n"
    "  set i = i + 1\n"
    "  if i > 5 then\n"
    "    break\n"
    "  end\n"
    "  set total = total + i\n"
    "end\n"
    "print(total)\n";             // 1+2+3+4+5 = 15

static constexpr const char* SRC_FOR =
    "# for range loop + continue\n"
    "set sum = 0\n"
    "for n in 1..10 do\n"
    "  if n % 2 == 0 then\n"
    "    continue\n"
    "  end\n"
    "  set sum = sum + n\n"
    "end\n"
    "print(sum)\n";               // 1+3+5+7+9 = 25

static constexpr const char* SRC_FIB =
    "# recursive fibonacci\n"
    "func fib(n)\n"
    "  if n < 2 then\n"
    "    return n\n"
    "  end\n"
    "  return fib(n - 1) + fib(n - 2)\n"
    "end\n"
    "for i in 0..10 do\n"
    "  print(fib(i))\n"
    "end\n";                      // 0 1 1 2 3 5 8 13 21 34 55

static constexpr const char* SRC_FIZZBUZZ =
    "# fizzbuzz 1..15\n"
    "for n in 1..15 do\n"
    "  if n % 15 == 0 then\n"
    "    print(\"FizzBuzz\")\n"
    "  elif n % 3 == 0 then\n"
    "    print(\"Fizz\")\n"
    "  elif n % 5 == 0 then\n"
    "    print(\"Buzz\")\n"
    "  else\n"
    "    print(n)\n"
    "  end\n"
    "end\n";

static constexpr const char* SRC_LISTS =
    "# list literal, index, append, iterate, len\n"
    "set xs = [10, 20, 30]\n"
    "print(xs[0])\n"              // 10
    "print(xs[-1])\n"            // 30
    "set xs = append(xs, 40)\n"
    "print(len(xs))\n"           // 4
    "set acc = 0\n"
    "for v in xs do\n"
    "  set acc = acc + v\n"
    "end\n"
    "print(acc)\n";              // 100

static constexpr const char* SRC_FILEIO =
    "# file i/o through kvfs\n"
    "set path = \"/tmp/kcl_io_test.txt\"\n"
    "write(path, \"persisted-by-kcl\")\n"
    "if exists(path) then\n"
    "  print(read(path))\n"
    "else\n"
    "  print(\"MISSING\")\n"
    "end\n";

static constexpr Test TESTS[] = {
    { "hello_world", SRC_HELLO,    "Hello, World!\n", false },
    { "arithmetic",  SRC_ARITH,    "14\n20\n2\n2\n2.500000\n", false },
    { "strings",     SRC_STRINGS,  "hi kurono\n6\nv2\nKURONO\n", false },
    { "boolean_if",  SRC_BOOL,     "medium\ntrue\ntrue\n", false },
    { "while_loop",  SRC_WHILE,    "15\n", false },
    { "for_loop",    SRC_FOR,      "25\n", false },
    { "fibonacci",   SRC_FIB,      "0\n1\n1\n2\n3\n5\n8\n13\n21\n34\n55\n", false },
    { "fizzbuzz",    SRC_FIZZBUZZ, "1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n", false },
    { "lists",       SRC_LISTS,    "10\n30\n4\n100\n", false },
    { "file_io",     SRC_FILEIO,   "persisted-by-kcl\n", true },
};
static const int TEST_COUNT = (int)(sizeof(TESTS) / sizeof(TESTS[0]));

} // namespace

int KCLTest::RunAll() {
    SerialLogger::Log("KCL-TEST: BEGIN\r\n");
    char* out = (char*)KernelHeap::Alloc(8192);
    if (!out) { SerialLogger::Log("KCL-TEST: alloc failed\r\n"); return 0; }

    int pass = 0;
    for (int i = 0; i < TEST_COUNT; i++) {
        out[0] = 0;
        KCL::Execute(TESTS[i].src, out, 8192);
        bool ok = TESTS[i].substring ? tcontains(out, TESTS[i].expect)
                                     : tstreq(out, TESTS[i].expect);
        SerialLogger::Log("KCL-TEST: ");
        SerialLogger::Log(TESTS[i].name);
        SerialLogger::Log(ok ? " PASS\r\n" : " FAIL\r\n");
        if (!ok) {
            // dump actual output so a failure is diagnosable from the log. (satoru)
            SerialLogger::Log("KCL-TEST:   got=[");
            SerialLogger::Log(out);
            SerialLogger::Log("]\r\n");
            SerialLogger::Log("KCL-TEST:   exp=[");
            SerialLogger::Log(TESTS[i].expect);
            SerialLogger::Log("]\r\n");
        }
        if (ok) pass++;
    }

    // extra: exercise the shell/file-manager entry point (KCL::ExecFile) and the
    // import statement together - write a library + a main script to kvfs, then
    // run the main via ExecFile exactly as `kcl <file>` does. (satoru)
    int total = TEST_COUNT + 1;
    {
        KVFS::Mkdirs("/kurono/lib");
        KVFS::WriteString("/kurono/lib/mathlib.kcl",
            "func square(n)\n  return n * n\nend\n");
        KVFS::WriteString("/tmp/kcl_import_test.kcl",
            "import mathlib\nprint(square(9))\n");
        out[0] = 0;
        KCL::ExecFile("/tmp/kcl_import_test.kcl", out, 8192);
        bool ok = tstreq(out, "81\n");
        SerialLogger::Log("KCL-TEST: import_execfile");
        SerialLogger::Log(ok ? " PASS\r\n" : " FAIL\r\n");
        if (!ok) {
            SerialLogger::Log("KCL-TEST:   got=["); SerialLogger::Log(out); SerialLogger::Log("]\r\n");
            SerialLogger::Log("KCL-TEST:   exp=[81\n]\r\n");
        }
        if (ok) pass++;
    }

    SerialLogger::Log("KCL-TEST: SUMMARY ");
    SerialLogger::LogDec(pass);
    SerialLogger::Log("/");
    SerialLogger::LogDec(total);
    SerialLogger::Log("\r\n");
    SerialLogger::Log(pass == total ? "KCL-TEST: ALL PASS\r\n" : "KCL-TEST: SOME FAILED\r\n");

    KernelHeap::Free(out);
    return pass;
}
// end (satoru)
