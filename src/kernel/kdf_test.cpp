//  kurono os: kdf crash-recovery self-test (the kurono.kdf.test boot gate).
//
//  this is the load-bearing proof of the hybrid-kernel thesis: a kdf-sandboxed
//  driver can crash (here, by deliberately running off the end of its own
//  guard-fenced dma buffer) and the kernel SURVIVES, the offending region is
//  quarantined, the crash is reported to kinit, and kinit RESTARTS the driver.
//
//  it runs three scenarios headless and logs PASS/FAIL to serial:
//
//    A) sandbox unwind: an op inside KDF::RunGuarded writes one byte past its
//       AllocDMA buffer (into the trailing guard page). the #pf is caught by
//       HandleGuardFault, which quarantines the region and longjmps back to
//       RunGuarded -> returns false. PASS if RunGuarded returned false AND the
//       kernel is still executing (we reach the next line). this is the core
//       "driver crash != kernel crash" guarantee.
//
//    B) in-bounds still works: a fresh guarded op writes/reads WITHIN a new
//       AllocDMA buffer and must complete (RunGuarded returns true). proves the
//       sandbox does not break normal operation.
//
//    C) kinit restart: the fault in (A) was reported to kinit via the crash
//       bridge; this scenario asks kinit to drive the restart (Tick over the
//       backoff window) and verifies the driver's kinit unit returns to RUNNING
//       and the driver re-inits (its init counter increments). proves end-to-end
//       crash -> quarantine -> notify -> restart recovery.
//
//  gated by kurono.kdf.test; kurono.kdf.poweroff=1 powers off after, for a
//  bounded headless CI run. (satoru)

#include "kdf.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"
#include "../hal/hal.h"
#include "../system/kinit.h"

namespace KDF {

namespace {

// ── the test driver's state (satoru) ─────────────────────────────────────────
int      g_test_kdf_id   = -1;
int      g_test_svc      = -1;
volatile int g_test_init_count = 0;     // bumped each (re-)init (satoru)
volatile uint8_t* g_test_buf = nullptr; // last guard-fenced buffer (satoru)
uint64_t g_test_buf_bytes = 0;

// the test driver's init: allocate a guard-fenced dma buffer + touch it in
// bounds. runs inside the kdf sandbox (KDF::Start / kinit restart). (satoru)
bool test_driver_init() {
    g_test_init_count++;
    void* b = AllocDMA(4096);            // one page, guard-fenced (satoru)
    if (!b) { LogDriver("kdf-fault-test", "init: AllocDMA failed"); return false; }
    g_test_buf = (volatile uint8_t*)b;
    g_test_buf_bytes = 4096;
    // in-bounds write to prove the buffer is live. (satoru)
    g_test_buf[0] = 0xAB;
    g_test_buf[4095] = 0xCD;
    LogDriver("kdf-fault-test", "init OK (4KiB guard-fenced dma)");
    return true;
}

// ── guarded ops (run via RunGuarded) (satoru) ─────────────────────────────────
struct OpResult { volatile bool reached_after; };

// scenario A: write ONE BYTE PAST the buffer end -> into the trailing guard page.
// this is the deliberate fault. the line after the store should NEVER execute
// (HandleGuardFault longjmps out), so reached_after stays false. (satoru)
void op_overrun(void* arg) {
    OpResult* r = (OpResult*)arg;
    volatile uint8_t* p = g_test_buf;
    if (!p) return;
    // touch the first byte of the trailing guard page (buf + bytes). the trailing
    // guard sits exactly one page after the last payload page. (satoru)
    p[g_test_buf_bytes] = 0xFF;          // <-- guard-page fault here (satoru)
    r->reached_after = true;             // must NOT be reached (satoru)
}

// scenario B: a normal in-bounds access that must complete cleanly. (satoru)
void op_inbounds(void* arg) {
    OpResult* r = (OpResult*)arg;
    volatile uint8_t* p = g_test_buf;
    if (!p) return;
    uint8_t acc = 0;
    for (uint64_t i = 0; i < g_test_buf_bytes; i++) { p[i] = (uint8_t)i; acc = (uint8_t)(acc + p[i]); }
    (void)acc;
    r->reached_after = true;             // SHOULD be reached (satoru)
}

void log_result(const char* scenario, bool pass, const char* detail) {
    char b[160]; int n = 0;
    auto cat = [&](const char* s) { while (s && *s && n < (int)sizeof(b) - 1) b[n++] = *s++; b[n] = 0; };
    cat("[kdf-test] ");
    cat(scenario);
    cat(pass ? " PASS" : " FAIL");
    if (detail && *detail) { cat(" - "); cat(detail); }
    cat("\r\n");
    SerialLogger::Log(b);
    KDF::LogDriver("kdf-fault-test", b + 10);   // skip the [kdf-test] prefix (satoru)
}

}  // namespace

// register the test driver as a supervised kinit kdf unit. called from
// kernel_main when kurono.kdf.test is set, BEFORE KInit::Boot, so it is a known
// unit. it is NOT marked already-running: kinit brings it up at the user target
// (we then crash it). (satoru)
void RegisterCrashTestDriver() {
    g_test_svc = KInit::RegisterKdfDriver("kdf-fault-test", &test_driver_init,
                                          KInit::KTGT_USER, /*critical=*/false,
                                          /*already_running=*/false);
    g_test_kdf_id = FindDriver("kdf-fault-test");
    SerialLogger::Log("[kdf-test] crash-recovery test driver registered\r\n");
}

bool g_poweroff_after = false;
void SetCrashTestPoweroff(bool v) { g_poweroff_after = v; }

// run the three scenarios. called from a kernel-process after the desktop is up
// (so kinit's monitor is ticking). (satoru)
void RunCrashRecoveryTest() {
    SerialLogger::Log("\r\n========== KDF CRASH-RECOVERY TEST ==========\r\n");

    if (g_test_kdf_id < 0) {
        SerialLogger::Log("[kdf-test] FAIL - test driver not registered\r\n");
        if (g_poweroff_after) HAL::PowerOff();
        return;
    }

    // make sure the driver is up first (kinit should have started it at the user
    // target; if not, start it now via kdf). (satoru)
    if (GetState(g_test_kdf_id) != KDF_RUNNING) {
        Start(g_test_kdf_id);
    }
    int init_before = g_test_init_count;
    bool driver_up = (GetState(g_test_kdf_id) == KDF_RUNNING) && g_test_buf;
    log_result("setup", driver_up, driver_up ? "driver running, buffer live" : "driver not up");

    int passes = 0, total = 0;

    // ── scenario A: deliberate guard-page overrun, sandbox must unwind ────────
    total++;
    OpResult ra{ false };
    bool ran = RunGuarded(g_test_kdf_id, op_overrun, &ra);
    // we are still executing here -> the kernel SURVIVED the driver crash.
    // RunGuarded must have returned false (faulted), and the line after the
    // out-of-bounds store must NOT have executed. (satoru)
    bool a_pass = (!ran) && (!ra.reached_after) && (GetState(g_test_kdf_id) == KDF_CRASHED);
    {
        char d[96]; int n = 0;
        auto cat = [&](const char* s){ while(*s && n<(int)sizeof(d)-1) d[n++]=*s++; d[n]=0; };
        auto catu = [&](uint64_t v){ char t[20]; int m=0; if(!v)t[m++]='0'; while(v){t[m++]=(char)('0'+v%10);v/=10;} while(m) if(n<(int)sizeof(d)-1) d[n++]=t[--m]; d[n]=0; };
        cat("ran="); catu(ran?1:0); cat(" after="); catu(ra.reached_after?1:0);
        cat(" state="); cat(StateName(GetState(g_test_kdf_id)));
        log_result("A.sandbox-unwind", a_pass, d);
    }
    if (a_pass) passes++;

    // the kernel survived the fault: prove it by doing real work right here. (satoru)
    SerialLogger::Log("[kdf-test] kernel alive after driver crash (executing post-fault)\r\n");

    // ── scenario C: kinit restart ────────────────────────────────────────────
    // the fault above reported the crash to kinit (via the crash bridge), which
    // scheduled a backoff restart. drive kinit's monitor over the backoff window
    // and verify the driver re-inits + its kinit unit returns to RUNNING. the
    // base backoff is 1s; poll up to ~6s. (satoru)
    total++;
    bool restarted = false;
    for (int i = 0; i < 60; i++) {                 // ~6s @ 100ms (satoru)
        KInit::Tick();                              // advance the restart machinery (satoru)
        Scheduler::SleepMs(100);
        if (g_test_init_count > init_before && GetState(g_test_kdf_id) == KDF_RUNNING) {
            restarted = true;
            break;
        }
    }
    {
        char d[96]; int n = 0;
        auto cat = [&](const char* s){ while(*s && n<(int)sizeof(d)-1) d[n++]=*s++; d[n]=0; };
        auto catu = [&](uint64_t v){ char t[20]; int m=0; if(!v)t[m++]='0'; while(v){t[m++]=(char)('0'+v%10);v/=10;} while(m) if(n<(int)sizeof(d)-1) d[n++]=t[--m]; d[n]=0; };
        cat("init_count "); catu((uint64_t)init_before); cat("->"); catu((uint64_t)g_test_init_count);
        cat(" state="); cat(StateName(GetState(g_test_kdf_id)));
        log_result("C.kinit-restart", restarted, d);
    }
    if (restarted) passes++;

    // ── scenario B: in-bounds op still works after recovery ───────────────────
    total++;
    OpResult rb{ false };
    bool ran_b = RunGuarded(g_test_kdf_id, op_inbounds, &rb);
    bool b_pass = ran_b && rb.reached_after;
    log_result("B.inbounds-ok", b_pass, b_pass ? "completed within buffer" : "did not complete");
    if (b_pass) passes++;

    // ── summary ───────────────────────────────────────────────────────────────
    {
        char d[64]; int n = 0;
        auto cat = [&](const char* s){ while(*s && n<(int)sizeof(d)-1) d[n++]=*s++; d[n]=0; };
        auto catu = [&](uint64_t v){ char t[20]; int m=0; if(!v)t[m++]='0'; while(v){t[m++]=(char)('0'+v%10);v/=10;} while(m) if(n<(int)sizeof(d)-1) d[n++]=t[--m]; d[n]=0; };
        cat("KDF-SELFTEST: "); catu((uint64_t)passes); cat("/"); catu((uint64_t)total);
        cat(passes == total ? " OVERALL PASS" : " OVERALL FAIL");
        SerialLogger::Log("[kdf-test] ");
        SerialLogger::Log(d);
        SerialLogger::Log("\r\n");
    }
    SerialLogger::Log("========== KDF CRASH-RECOVERY TEST DONE ==========\r\n\r\n");

    if (g_poweroff_after) {
        SerialLogger::Log("[kdf-test] powering off (kurono.kdf.poweroff)\r\n");
        for (volatile int d = 0; d < 2000000; d++) asm volatile("pause");
        HAL::PowerOff();
    }
}

}  // namespace KDF

// end (satoru)
