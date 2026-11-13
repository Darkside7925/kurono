//  kurono os - single definition point for the embedded logo bitmap
//
// logo.h used to define the 300x300 array with internal linkage, so the
// kernel, the panic screen and the desktop each carried their own 351 kb
// copy. the header declares it now and this file owns the one definition.
// (satoru)
#define KURONO_LOGO_IMPL
#include "../../logo.h"

// end (satoru)
