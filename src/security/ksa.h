#pragma once
//  kurono os  -  ksa (kurono secure authorization)
//  hypervisor-backed privilege prompts. kurono's equivalent of windows uac,
//  but the prompt is rendered + arbitrated inside an ept-isolated guest
//  context that the main os has no page-table mapping into.  the result
//  (approve/deny + credential hash) returns through a single restricted
//  vmcall channel  -  ring-0 malware in the main os cannot intercept it or
//  inject a forged approval.
//
//  isolation model (see docs/security/ksa.md):
//    - ksa asks the hypervisor to spawn a minimal isolated context
//      (KSA::Spawn).  the hypervisor allocates a dedicated guest-physical
//      region (framebuffer + state + result) backed by host memory that is
//      mapped ONLY into the ksa context's ept root, never into KernelVMM.
//    - the prompt renders into that isolated framebuffer.  the main os
//      compositor never receives a pointer to it; only the hypervisor blits
//      the final frame to the screen.
//    - the verdict crosses back via VMCALL 0x4B (read-only result channel).
//    - on hosts where true nested vmx for the inner vm is unavailable
//      (e.g. nested kvm under qemu), ksa runs as an ept-isolated guest
//      *context* instead of a fully separate vmlaunch'd vm.  the security
//      invariants (no main-os mapping; result only via the gated channel)
//      hold either way; the difference is documented, not hidden.
#include "../kernel/types.h"

// vmcall number for the ksa result channel  -  'K' = 0x4B. read-only verdict.
#define KSA_VMCALL_CHANNEL  0x4B

// ksa result channel sub-functions (ecx). (satoru)
#define KSA_SUB_GET_VERDICT 0   // host reads verdict out of the isolated region
#define KSA_SUB_GET_INFO    1   // host queries channel revision

// a single ksa prompt request. the credential the user types never leaves the
// isolated region in cleartext  -  only its salted hash is exposed to the host
// verdict reader. (satoru)
struct KSARequest {
    const char* title;       // e.g. "Privilege Escalation"
    const char* detail;      // what is being authorized
    const char* username;    // account whose credential is required
    bool        want_cred;   // true => prompt for a password/credential
};

// verdict produced inside the isolated context, read back over the channel.
struct KSAVerdict {
    bool          approved;          // user pressed approve
    bool          completed;         // prompt actually ran to a decision
    unsigned char cred_hash[32];     // salted hash of the typed credential
    bool          have_cred_hash;    // true if cred_hash is meaningful
};

class KSA {
public:
    static void Init();

    // true once the hypervisor-backed isolated context is usable. on a host
    // with no virtualization at all this returns false and callers must treat
    // ksa as unavailable (the password factor then carries the policy). (satoru)
    static bool IsAvailable();

    // true if the active prompt path is a real nested vm (vmlaunch'd) vs the
    // ept-isolated context fallback. purely informational / for the docs and
    // the `supr policy` status line. (satoru)
    static bool IsRealNestedVM();

    // spawn the isolated context, render the prompt, drive input, tear the
    // context down, and return the verdict via the restricted channel.
    // blocking (cooperative). returns false only if ksa could not run at all
    // (then callers fall back per policy). (satoru)
    static bool Prompt(const KSARequest& req, KSAVerdict& out);

    // diagnostics  -  used by the boot self-test (kurono.ksa.test). renders a
    // synthetic prompt, auto-answers it inside the isolated context, and
    // verifies: (a) the isolated region is NOT reachable through the main-os
    // page tables, and (b) the verdict only crosses via the channel. logs
    // every check to serial. returns true if all invariants held. (satoru)
    static bool SelfTest();

    // interactive render-path verification  -  used by the boot gate
    // kurono.ksa.prompt. runs the REAL KSA::Prompt() with a sample request so
    // the modal is drawn on the actual framebuffer; a headless screendump can
    // then capture it, and synthetic input (Enter/Esc or an Approve/Deny click)
    // drives the verdict. logs the verdict to serial. unlike SelfTest() this
    // exercises the on-screen renderer + input loop, not just the isolation
    // invariants. returns the approve/deny decision. (satoru)
    static bool PromptDemo(bool want_cred);

    // host-side accessors used by the vmcall handler ONLY. these are the sole
    // bridge out of the isolated region and expose a copy of the verdict, not
    // a pointer into ksa memory. (satoru)
    static bool ReadVerdictForChannel(KSAVerdict& out);
    static uint32_t ChannelRevision();

private:
    static bool available;
    static bool real_nested;
    static bool initialized;

    // guest-physical layout of the isolated region (offsets within the region).
    static bool   SpawnContext();    // alloc isolated region + dedicated ept
    static void   TeardownContext(); // wipe + free the isolated region
    static bool   MainOSCanReach(uint64_t host_phys); // page-table reachability probe
};

// end (satoru)
