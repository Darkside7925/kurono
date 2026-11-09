#pragma once
#include "../kernel/types.h"

// intel display stage 0: a read-only probe of what the firmware (gop)
// left programmed in the intel display engine. it never writes an mmio
// register, never touches the gtt, forcewake or power wells. it only
// reads, decodes and logs. register offsets are for display version 14
// (meteor lake / arrow lake, linux "xe_lpdp"). (satoru)
//
// every line starts with "[igpu]". the boot run writes them to the serial
// log (KURONO.LOG on bare metal); the `gpuprobe` shell command runs the
// same probe again and also copies the lines into the terminal. (satoru)
namespace IntelDisplayProbe {

// run the probe. lines go to the serial log, and are also appended to
// out (when out is non-null). returns the number of bytes written to out.
// harmless without an intel gpu: logs one line and returns. (satoru)
int Run(char* out, int maxo);

// refresh in millihertz that the last Run computed from the pipe timings for
// the live pipe showing a w x h mode (else the first live pipe), 0 if none.
// returns stored values only; no register access. (satoru)
uint32_t RefreshMilliHz(uint32_t w, uint32_t h);

// the identity-mapped bar0 register window when the last Run found a
// display version 14 engine in d0 with pw_1 up, else nullptr. used by the
// stage 1 aux probe (intel_dp_aux.cpp) so it never maps bar0 itself. (satoru)
volatile uint8_t* Mmio();

}

// end (satoru)
